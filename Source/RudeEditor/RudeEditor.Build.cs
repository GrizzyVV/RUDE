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
			"Projects",   // IPluginManager: the plugin locates its own bundled catalog
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
			// WP13 ycd_pack: RudeYcdPack.cpp joins the module. Named here on purpose - a NEW .cpp is
			// not compiled until the module's file list is invalidated, and the only symptom is a
			// link error against a symbol whose source is sitting right there. Editing this file is
			// what invalidates it; no new dependency is needed (FCompression is in Core).
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
			// WP13 vfx_move lane (RudeVfxMove.cpp): ypt effect dictionaries and mrf MoVE networks,
			// both READ-ONLY. It needs no module the list above does not already carry - this note is
			// here because touching Build.cs is what invalidates the module file list, and a NEW .cpp
			// that the list has not been rebuilt for fails at LINK with no other symptom (AGENTS 5).
		});
	}
}
// WP13 cutscene_export: RudeCutsceneExport.cpp (ExportCutscene / SetCutsceneEventTime / ProbeCutsceneSource).
