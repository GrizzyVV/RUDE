// RUDE - RAGE <-> Unreal Development Environment
// RudeCore: the RAGE-side data that rides on UE things - the entity component a placed actor
// carries, the carried-field bag, the document types. Runtime so PIE (the Sandbox) can load it.
using UnrealBuildTool;

public class RudeCore : ModuleRules
{
	public RudeCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// THE SANDBOX (GDD 1b): raw-key input bindings (EKeys) and cutscene playback (ULevelSequencePlayer).
			"InputCore",
			"LevelSequence",
			"MovieScene",
			// WP11 THE CHAOS TEST-DRIVE: AWheeledVehiclePawn / UChaosWheeledVehicleMovementComponent / UChaosVehicleWheel
			// (plugin module ChaosVehicles), the sim configs the drive component owns (ChaosVehiclesCore), and the
			// physics-asset body setup base (PhysicsCore). Chaos/ChaosCore are Engine public deps already; named
			// so the include chain (SimpleVehicle.h -> Chaos headers) never depends on that staying true.
			"ChaosVehicles",
			"ChaosVehiclesCore",
			"PhysicsCore",
			"Chaos",
			"ChaosCore",
		});
	}
}
