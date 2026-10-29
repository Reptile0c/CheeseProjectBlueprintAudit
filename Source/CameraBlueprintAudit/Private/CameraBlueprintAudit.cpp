#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "K2Node_CallFunction.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraBlueprintAudit, Log, All);

namespace CameraBlueprintAudit
{
    static void AppendCameraProperties(const UObject* Object, FString& Output)
    {
        if (!Object) return;
        Output += FString::Printf(TEXT("%s [%s]\n"), *Object->GetName(), *Object->GetClass()->GetPathName());
        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FProperty* Property = *It;
            const FString Name = Property->GetName();
            if (!Name.Contains(TEXT("Camera")) && !Name.Contains(TEXT("Rotation")) &&
                !Name.Contains(TEXT("Yaw")) && !Name.Contains(TEXT("Pitch")) &&
                !Name.Contains(TEXT("Input")) && !Name.Contains(TEXT("Cursor")) &&
                !Name.Contains(TEXT("Lag")) && !Name.Contains(TEXT("Control")) &&
                !Name.Contains(TEXT("FOV")) && !Name.Contains(TEXT("FieldOfView")) &&
                !Name.Contains(TEXT("ArmLength")) && !Name.Contains(TEXT("Aperture")) &&
                !Name.Contains(TEXT("Focus")) && !Name.Contains(TEXT("Capture")) && !Name.Contains(TEXT("PostProcess")) &&
                !Name.Contains(TEXT("Exposure"))) continue;
            FString Value;
            Property->ExportText_InContainer(0, Value, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
            if (Value.Len() > 20000) Value = Value.Left(20000) + TEXT(" [truncated]");
            Output += TEXT("  ") + Name + TEXT(" = ") + Value + TEXT("\n");
        }
        Output += TEXT("\n");
    }


    static FString SafeFilename(FString Value)
    {
        for (TCHAR& Character : Value)
        {
            if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
            {
                Character = TEXT('_');
            }
        }
        return Value;
    }

    static bool Save(const FString& Path, const FString& Contents)
    {
        IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
        return Files.CreateDirectoryTree(*FPaths::GetPath(Path)) &&
            FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    static FString RelativePackagePath(FString Package)
    {
        Package.RemoveFromStart(TEXT("/Game/"));
        TArray<FString> Segments;
        Package.ParseIntoArray(Segments, TEXT("/"), true);
        for (FString& Segment : Segments) Segment = SafeFilename(Segment);
        return FString::Join(Segments, TEXT("/"));
    }

    static void Export(const TArray<FString>& Args)
    {
        FString Scope = Args.IsEmpty() ? TEXT("/Game") : Args[0];
        Scope.RemoveFromEnd(TEXT("/"));
        if (Scope.IsEmpty()) Scope = TEXT("/Game");
        if (Args.Num() > 1 || !(Scope == TEXT("/Game") || Scope.StartsWith(TEXT("/Game/"))) ||
            Scope.Contains(TEXT("..")))
        {
            UE_LOG(LogCameraBlueprintAudit, Error, TEXT("Usage: SayCheese.ExportCameraBlueprints [/Game/OptionalFolder]"));
            return;
        }

        const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CameraBlueprintAudit"));
        const FString RunName = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")) + TEXT("_") +
            FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
        const FString RunDir = FPaths::Combine(Root, TEXT("Runs"), RunName);
        IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Scope));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        TArray<FAssetData> Assets;
        Registry.GetAssets(Filter, Assets);
        Assets.Sort([](const FAssetData& A, const FAssetData& B) {
            return A.PackageName.LexicalLess(B.PackageName);
        });

        // Include all /Game Blueprint package names so a scoped export can still name outside targets.
        FARFilter UniverseFilter = Filter;
        UniverseFilter.PackagePaths.Reset();
        UniverseFilter.PackagePaths.Add(FName(TEXT("/Game")));
        TArray<FAssetData> UniverseAssets;
        Registry.GetAssets(UniverseFilter, UniverseAssets);
        TSet<FName> BlueprintPackages;
        for (const FAssetData& BlueprintAsset : UniverseAssets)
            BlueprintPackages.Add(BlueprintAsset.PackageName);

        TSet<FString> RelationRows;
        TMap<FString, TArray<FString>> Outgoing;
        TMap<FString, TArray<FString>> Incoming;
        auto AddRelation = [&](const FString& Source, const FString& Kind,
            const FString& Target, const FString& GraphFile, const FString& Member)
        {
            if (Source == Target) return;
            const FString Row = Source + TEXT("\t") + Kind + TEXT("\t") + Target +
                TEXT("\t") + GraphFile + TEXT("\t") + Member;
            if (RelationRows.Contains(Row)) return;
            RelationRows.Add(Row);
            const FString Detail = Kind + TEXT("  ") + Target +
                (Member.IsEmpty() ? TEXT("") : TEXT("::") + Member) +
                (GraphFile.IsEmpty() ? TEXT("") : TEXT("  [") + GraphFile + TEXT("]"));
            Outgoing.FindOrAdd(Source).Add(Detail);
            Incoming.FindOrAdd(Target).Add(Kind + TEXT("  ") + Source +
                (Member.IsEmpty() ? TEXT("") : TEXT("::") + Member) +
                (GraphFile.IsEmpty() ? TEXT("") : TEXT("  [") + GraphFile + TEXT("]")));
        };

        FString AssetPaths;
        TSet<FString> CurrentPaths;
        for (const FAssetData& Asset : Assets)
        {
            const FString Path = Asset.PackageName.ToString();
            AssetPaths += Path + TEXT("\n");
            CurrentPaths.Add(Path);
        }
        FString PreviousRun;
        FString PreviousPaths;
        FString Changes;
        bool bHasPrevious = false;
        TArray<FString> RunNames;
        IFileManager::Get().FindFiles(RunNames, *FPaths::Combine(Root, TEXT("Runs"), TEXT("*")),
            false, true);
        RunNames.Sort([](const FString& A, const FString& B) { return A > B; });
        for (const FString& Name : RunNames)
        {
            const FString Candidate = FPaths::Combine(Root, TEXT("Runs"), Name);
            FString PriorIndex;
            if (!FFileHelper::LoadFileToString(PriorIndex, *FPaths::Combine(Candidate, TEXT("INDEX.txt"))))
                continue;
            if (!PriorIndex.Contains(TEXT("\nScope: ") + Scope + TEXT("\n")))
                continue;
            if (FFileHelper::LoadFileToString(PreviousPaths,
                *FPaths::Combine(Candidate, TEXT("AssetPaths.txt"))))
            {
                PreviousRun = Candidate;
                bHasPrevious = true;
                break;
            }
        }
        int32 Added = 0;
        int32 Removed = 0;
        if (bHasPrevious)
        {
            TArray<FString> PriorLines;
            PreviousPaths.ParseIntoArrayLines(PriorLines, true);
            TSet<FString> PriorPaths;
            for (const FString& Path : PriorLines) PriorPaths.Add(Path);
            for (const FString& Path : CurrentPaths)
                if (!PriorPaths.Contains(Path))
                {
                    Changes += TEXT("ADDED: ") + Path + TEXT("\n");
                    ++Added;
                }
            for (const FString& Path : PriorPaths)
                if (!CurrentPaths.Contains(Path))
                {
                    Changes += TEXT("REMOVED: ") + Path + TEXT("\n");
                    ++Removed;
                }
            if (Changes.IsEmpty()) Changes = TEXT("No Blueprint assets added or removed.\n");
        }
        else Changes = TEXT("First run with an asset manifest; no previous run to compare.\n");
        if (!Save(FPaths::Combine(RunDir, TEXT("AssetPaths.txt")), AssetPaths))
            UE_LOG(LogCameraBlueprintAudit, Warning, TEXT("Could not write AssetPaths.txt"));
        if (!Save(FPaths::Combine(RunDir, TEXT("ChangesSincePrevious.txt")), Changes))
            UE_LOG(LogCameraBlueprintAudit, Warning, TEXT("Could not write ChangesSincePrevious.txt"));
        FString Index = TEXT("Camera Blueprint Audit (read-only)\nProject: ") +
            FPaths::GetProjectFilePath() + TEXT("\nScope: ") + Scope + TEXT("\n\n");
        int32 Exported = 0;
        int32 Failures = 0;
        for (const FAssetData& Asset : Assets)
        {
            UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
            if (!Blueprint)
            {
                Index += FString::Printf(TEXT("LOAD FAILED: %s\n"), *Asset.PackageName.ToString());
                ++Failures;
                continue;
            }
            ++Exported;
            const FString AssetDir = FPaths::Combine(RunDir, TEXT("Blueprints"),
                RelativePackagePath(Asset.PackageName.ToString()));
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            Index += FString::Printf(TEXT("%s [%s] (%d graphs)\n"), *Asset.PackageName.ToString(),
                *Blueprint->GetClass()->GetName(), Graphs.Num());

            FString Variables;
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
                Variables += FString::Printf(TEXT("%s : %s\n"), *Variable.VarName.ToString(),
                    *Variable.VarType.PinCategory.ToString());
            if (!Save(FPaths::Combine(AssetDir, TEXT("variables.txt")), Variables)) ++Failures;

            FString Components;
            AppendCameraProperties(Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr, Components);
            if (Blueprint->SimpleConstructionScript)
            {
                const TArray<USCS_Node*> Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
                TMap<const USCS_Node*, FString> Parents;
                for (const USCS_Node* Parent : Nodes)
                    if (Parent)
                        for (const USCS_Node* Child : Parent->GetChildNodes())
                            if (Child) Parents.Add(Child, Parent->GetVariableName().ToString());
                for (const USCS_Node* Node : Nodes)
                {
                    if (!Node) continue;
                    const FString* SCSParent = Parents.Find(Node);
                    const FString Parent = SCSParent ? *SCSParent :
                        (Node->ParentComponentOrVariableName.IsNone() ? TEXT("<root or inherited>") :
                            Node->ParentComponentOrVariableName.ToString());
                    Components += FString::Printf(TEXT("SCS component: %s; parent: %s\n"),
                        *Node->GetVariableName().ToString(), *Parent);
                    AppendCameraProperties(Node->ComponentTemplate, Components);
                }
            }
            if (!Save(FPaths::Combine(AssetDir, TEXT("components.txt")), Components)) ++Failures;

            for (int32 GraphIndex = 0; GraphIndex < Graphs.Num(); ++GraphIndex)
            {
                UEdGraph* Graph = Graphs[GraphIndex];
                if (!Graph) continue;
                TSet<UObject*> Nodes;
                for (UEdGraphNode* Node : Graph->Nodes) if (Node) Nodes.Add(Node);
                FString GraphText;
                FEdGraphUtilities::ExportNodesToText(Nodes, GraphText);
                const FString Filename = FString::Printf(TEXT("%03d__%s.txt"), GraphIndex,
                    *SafeFilename(Graph->GetName()));
                const bool bSaved = Save(FPaths::Combine(AssetDir, TEXT("graphs"), Filename), GraphText);
                if (!bSaved) ++Failures;
                Index += FString::Printf(TEXT("  %s: %d nodes%s\n"), *Filename, Nodes.Num(),
                    bSaved ? TEXT("") : TEXT(" [SAVE FAILED]"));

                // Compact graph view: linked pins and meaningful unlinked input defaults.
                FString Summary = FString::Printf(TEXT("%s :: %s\n"), *Asset.PackageName.ToString(),
                    *Graph->GetName());
                TMap<const UEdGraphNode*, int32> NodeNumbers;
                for (int32 NodeIndex = 0; NodeIndex < Graph->Nodes.Num(); ++NodeIndex)
                    if (Graph->Nodes[NodeIndex]) NodeNumbers.Add(Graph->Nodes[NodeIndex], NodeIndex);
                for (int32 NodeIndex = 0; NodeIndex < Graph->Nodes.Num(); ++NodeIndex)
                {
                    const UEdGraphNode* Node = Graph->Nodes[NodeIndex];
                    if (!Node) continue;
                    FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
                    Title.ReplaceInline(TEXT("\r"), TEXT(" "));
                    Title.ReplaceInline(TEXT("\n"), TEXT(" "));
                    Summary += FString::Printf(TEXT("#%d %s | %s\n"), NodeIndex,
                        *Node->GetClass()->GetName(), *Title);
                    for (const UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin || Pin->bHidden) continue;
                        const FString PinName = Pin->PinName.IsNone() ? TEXT("<unnamed>") : Pin->PinName.ToString();
                        const TCHAR* Direction = Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out");
                        FString Destinations;
                        for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        {
                            if (!Linked || !Linked->GetOwningNode()) continue;
                            if (const int32* TargetNumber = NodeNumbers.Find(Linked->GetOwningNode()))
                            {
                                if (!Destinations.IsEmpty()) Destinations += TEXT(", ");
                                Destinations += FString::Printf(TEXT("#%d.%s"), *TargetNumber,
                                    *Linked->PinName.ToString());
                            }
                        }
                        FString Default;
                        if (Pin->Direction == EGPD_Input && Destinations.IsEmpty())
                        {
                            Default = Pin->DefaultValue;
                            if (Pin->DefaultObject) Default = Pin->DefaultObject->GetPathName();
                            if (Default.Len() > 160) Default = Default.Left(160) + TEXT("...");
                        }
                        if (!Destinations.IsEmpty() || !Default.IsEmpty())
                            Summary += FString::Printf(TEXT("  %s %s:%s%s%s\n"), Direction,
                                *Pin->PinType.PinCategory.ToString(), *PinName,
                                Destinations.IsEmpty() ? TEXT(" = ") : TEXT(" -> "),
                                Destinations.IsEmpty() ? *Default : *Destinations);
                    }
                }
                const FString SummaryFile = Filename.LeftChop(4) + TEXT(".summary.txt");
                if (!Save(FPaths::Combine(AssetDir, TEXT("graphs"), SummaryFile), Summary))
                    ++Failures;
                for (const UEdGraphNode* Node : Graph->Nodes)
                {
                    const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                    if (!Call) continue;
                    const UClass* Owner = Call->FunctionReference.GetMemberParentClass(Blueprint->GeneratedClass);
                    if (!Owner) continue;
                    const FName TargetPackage = Owner->GetOutermost()->GetFName();
                    if (BlueprintPackages.Contains(TargetPackage))
                        AddRelation(Asset.PackageName.ToString(), TEXT("CALL"),
                            TargetPackage.ToString(), Filename, Call->GetFunctionName().ToString());
                }
            }
            TArray<FName> Dependencies;
            Registry.GetDependencies(Asset.PackageName, Dependencies,
                UE::AssetRegistry::EDependencyCategory::Package);
            for (const FName& Dependency : Dependencies)
                if (BlueprintPackages.Contains(Dependency))
                    AddRelation(Asset.PackageName.ToString(), TEXT("ASSET_REF"),
                        Dependency.ToString(), TEXT(""), TEXT(""));
        }

        FString Relationships = TEXT("source\tkind\ttarget\tgraph_file\tfunction\n");
        TArray<FString> SortedRelations;
        for (const FString& Row : RelationRows) SortedRelations.Add(Row);
        SortedRelations.Sort();
        for (const FString& Row : SortedRelations) Relationships += Row + TEXT("\n");
        if (!Save(FPaths::Combine(RunDir, TEXT("BlueprintReferences.tsv")), Relationships))
            ++Failures;

        FString ReferenceIndex = TEXT("Blueprint reference index (exported scope: ") + Scope +
            TEXT(")\nCALL = function declared on the named Blueprint; runtime target may differ.\n") +
            TEXT("ASSET_REF = package dependency; it does not prove a function call.\n") +
            TEXT("Incoming links include only Blueprints exported in this run.\n\n");
        for (const FAssetData& Asset : Assets)
        {
            const FString Path = Asset.PackageName.ToString();
            ReferenceIndex += Path + TEXT("\n");
            if (TArray<FString>* Entries = Outgoing.Find(Path))
            {
                Entries->Sort();
                ReferenceIndex += TEXT("  Outgoing:\n");
                for (const FString& Entry : *Entries) ReferenceIndex += TEXT("    ") + Entry + TEXT("\n");
            }
            if (TArray<FString>* Entries = Incoming.Find(Path))
            {
                Entries->Sort();
                ReferenceIndex += TEXT("  Referenced by:\n");
                for (const FString& Entry : *Entries) ReferenceIndex += TEXT("    ") + Entry + TEXT("\n");
            }
            ReferenceIndex += TEXT("\n");
        }
        if (!Save(FPaths::Combine(RunDir, TEXT("ReferenceIndex.txt")), ReferenceIndex))
            ++Failures;

        FARFilter InputFilter;
        InputFilter.PackagePaths.Add(FName(*Scope));
        InputFilter.bRecursivePaths = true;
        InputFilter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
        InputFilter.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
        TArray<FAssetData> Inputs;
        Registry.GetAssets(InputFilter, Inputs);
        Inputs.Sort([](const FAssetData& A, const FAssetData& B) {
            return A.PackageName.LexicalLess(B.PackageName);
        });
        FString Mappings;
        for (const FAssetData& Asset : Inputs)
        {
            UObject* Object = Asset.GetAsset();
            if (const UInputMappingContext* Context = Cast<UInputMappingContext>(Object))
            {
                Mappings += FString::Printf(TEXT("\n%s\n"), *Asset.PackageName.ToString());
                for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
                {
                    Mappings += FString::Printf(TEXT("  %s <- %s\n"),
                        Mapping.Action ? *Mapping.Action->GetPathName() : TEXT("None"), *Mapping.Key.ToString());
                    for (const auto& Modifier : Mapping.Modifiers)
                        if (Modifier) Mappings += TEXT("    modifier: ") + Modifier->GetClass()->GetName() + TEXT("\n");
                    for (const auto& Trigger : Mapping.Triggers)
                        if (Trigger) Mappings += TEXT("    trigger: ") + Trigger->GetClass()->GetName() + TEXT("\n");
                }
            }
            else if (const UInputAction* Action = Cast<UInputAction>(Object))
                Mappings += FString::Printf(TEXT("Action %s: %s\n"), *Asset.PackageName.ToString(),
                    *StaticEnum<EInputActionValueType>()->GetNameStringByValue(static_cast<int64>(Action->ValueType)));
            else
            {
                Index += FString::Printf(TEXT("LOAD FAILED (input): %s\n"), *Asset.PackageName.ToString());
                ++Failures;
            }
        }
        if (!Save(FPaths::Combine(RunDir, TEXT("InputMappings.txt")), Mappings)) ++Failures;
        Index += FString::Printf(TEXT("\nBlueprints exported: %d / %d\nInput assets: %d\nReference links: %d\nFailures: %d\nAdded since prior run: %d\nRemoved since prior run: %d\n"),
            Exported, Assets.Num(), Inputs.Num(), RelationRows.Num(), Failures, Added, Removed);
        if (Save(FPaths::Combine(RunDir, TEXT("INDEX.txt")), Index) &&
            Save(FPaths::Combine(Root, TEXT("LATEST.txt")), RunDir + TEXT("\n")))
        {
            UE_LOG(LogCameraBlueprintAudit, Display, TEXT("Exported %d Blueprints (%d failures) to %s"),
                Exported, Failures, *RunDir);
        }
        else
        {
            UE_LOG(LogCameraBlueprintAudit, Error, TEXT("Could not write export index under %s"), *Root);
        }
    }

}

class FCameraBlueprintAuditModule : public IModuleInterface
{
    IConsoleObject* Command = nullptr;
    IConsoleObject* LegacyCommand = nullptr;
public:
    virtual void StartupModule() override
    {
        Command = IConsoleManager::Get().RegisterConsoleCommand(
            TEXT("BlueprintAudit.Export"),
            TEXT("Export all /Game Blueprints and Enhanced Input assets. Optional /Game/folder scope."),
            FConsoleCommandWithArgsDelegate::CreateStatic(&CameraBlueprintAudit::Export), ECVF_Default);
        LegacyCommand = IConsoleManager::Get().RegisterConsoleCommand(
            TEXT("SayCheese.ExportCameraBlueprints"),
            TEXT("Compatibility alias for BlueprintAudit.Export."),
            FConsoleCommandWithArgsDelegate::CreateStatic(&CameraBlueprintAudit::Export), ECVF_Default);
    }
    virtual void ShutdownModule() override
    {
        if (Command) IConsoleManager::Get().UnregisterConsoleObject(Command);
        if (LegacyCommand) IConsoleManager::Get().UnregisterConsoleObject(LegacyCommand);
        Command = nullptr;
        LegacyCommand = nullptr;
    }
};

IMPLEMENT_MODULE(FCameraBlueprintAuditModule, CameraBlueprintAudit)

