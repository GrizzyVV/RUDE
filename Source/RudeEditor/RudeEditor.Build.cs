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
