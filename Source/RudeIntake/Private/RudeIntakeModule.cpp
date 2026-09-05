// RUDE - RAGE <-> Unreal Development Environment
#include "Modules/ModuleManager.h"

// RudeIntake has no startup work: the corpus index opens lazily on the first tool call that
// names a corpus root and stays cached until the ledger's timestamp changes.
class FRudeIntakeModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FRudeIntakeModule, RudeIntake)
