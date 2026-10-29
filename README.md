# Camera Blueprint Audit

A UE 5.7 editor plugin that exports Blueprint graphs as readable text. It discovers assets dynamically; no Blueprint paths are hardcoded. Use it to inspect pin wiring, review changes, or share a focused report with a teammate. It does not modify, compile, or save Blueprint assets.

## What it exports

- Every Blueprint asset under /Game by default, including Widget Blueprints and Blueprint components.
- Each graph's nodes, pins, and links, plus variable names and selected component settings (camera, rotation, focus, exposure, post-process).
- Enhanced Input actions and mapping contexts in the chosen folder.
- AssetPaths.txt and ChangesSincePrevious.txt to identify Blueprints added or removed since the previous run in this project.

A newly created Blueprint is discovered on the next export. The comparison tracks **asset paths**, not changes inside existing graphs. Renaming or moving a Blueprint appears as one removed and one added path.

## Requirements and installation

Unreal Engine 5.7 with Enhanced Input enabled. Copy the CameraBlueprintAudit folder into <YourProject>/Plugins/. The descriptor should be <YourProject>/Plugins/CameraBlueprintAudit/CameraBlueprintAudit.uplugin. Restart the editor and allow the module to compile if prompted. No SayCheese project assets are required.

For GitHub, commit the descriptor, Source folder, README.md, and .gitignore. You can publish a ZIP with Binaries for the same Windows UE 5.7 engine build, but source is the portable part. Do not commit project Saved reports by default.

## Export

In Unreal Editor's Output Log, run:

    BlueprintAudit.Export

To export just one folder and its descendants:

    BlueprintAudit.Export /Game/ThirdPerson

The original command remains as a compatibility alias:

    SayCheese.ExportCameraBlueprints

Each run writes to <YourProject>/Saved/CameraBlueprintAudit/Runs/<UTC timestamp>_<id>/. Open <YourProject>/Saved/CameraBlueprintAudit/LATEST.txt to find the newest run. That run's INDEX.txt lists discovered Blueprints, graph counts, load/save failures, and added/removed counts. Blueprints/<original Game folder>/<asset>/graphs/*.txt contains pin links. InputMappings.txt lists input bindings. AssetPaths.txt is the current inventory; ChangesSincePrevious.txt compares with the preceding run.

The first run has no comparison baseline. A folder-scoped run compares with the previous run's inventory, so use the same scope for a meaningful added/removed count. Only /Game content is scanned; engine and plugin assets are excluded. Exports can take time on a large project because they load each matching Blueprint.

## Sharing and publishing

Reports contain project logic, asset paths, and some literal Blueprint values. Review an export before sharing it publicly. This repository can contain plugin source without publishing your game Blueprints. Choose a license with your team's permission if you want to grant public reuse; none is assumed here.

## Build

The source is in Source/CameraBlueprintAudit. The plugin has an Editor module and dependencies on UnrealEd, AssetRegistry, EnhancedInput, Engine, CoreUObject, Core, and InputCore. UE 5.7 can compile it when the project opens. The provided Windows DLL is optional and may need rebuilding for a different UE 5.7 build or platform.

