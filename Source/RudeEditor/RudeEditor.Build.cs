// RUDE - RAGE <-> Unreal Development Environment
using UnrealBuildTool;

public class RudeEditor : ModuleRules
{
	public RudeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			// The human surface (SRudeToolPanel): a dockable editor tab under Window > Tools.
			// InputCore is required, not optional: SComboBox/SListView reference EKeys, so
			// omitting it links with 10 unresolved EKeys symbols rather than a clear error.
			"Slate",
			"SlateCore",
			"InputCore",
			"WorkspaceMenuStructure",
			"XmlParser",
			"MeshDescription",
			"StaticMeshDescription",
			"AssetRegistry",
			"ToolsetRegistry",
			"ImageWrapper",
			"Json",
		});
	}
}
