using UnrealBuildTool;

public class CameraBlueprintAudit : ModuleRules
{
    public CameraBlueprintAudit(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "EnhancedInput", "InputCore", "AssetRegistry", "BlueprintGraph"
        });
    }
}


