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
			"MeshReductionInterface",
			"MeshMergeUtilities",
			"MeshUtilitiesCommon",
			// WP10 peds: FSkeletalMeshAttributes / skin-weight attributes, and UE::AnimationCore::FBoneWeights.
			"SkeletalMeshDescription",
			"AnimationCore",
			"AssetRegistry",
			"ToolsetRegistry",
			"ImageWrapper",
			"Json",
			// The corpus index: every "where is this game file" question goes through it.
			"RudeIntake",
			// WP4: Data Layers from code (the ymap-as-layer projection).
			"DataLayerEditor",
			// WP10 audio lane: USoundFactory is the engine's own WAV importer (ImportAwc).
			"AudioEditor",
			"RudeCore",
			// WP11 BuildDriveable: FChaosWheelSetup / the wheel classes (ChaosVehicles), UBodySetupCore fields (PhysicsCore).
			"ChaosVehicles",
			"ChaosVehiclesCore",
			"PhysicsCore",
			// WP10 ANIMS: ycd -> UAnimSequence (IAnimationDataController lives in Engine; the controller
			// implementation module is named so the headless CLI has it loaded), cut -> Level Sequence.
			"AnimationDataController",
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
		});
	}
}
