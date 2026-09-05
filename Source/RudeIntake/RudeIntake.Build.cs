// RUDE - RAGE <-> Unreal Development Environment
// RudeIntake: how RUDE finds a game file inside a prepared corpus. Reads the corpus LEDGERS
// (_FILEBASE.json + _PROVENANCE.jsonl), never walks the folder tree by convention.
using UnrealBuildTool;

public class RudeIntake : ModuleRules
{
	public RudeIntake(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
		});
	}
}
