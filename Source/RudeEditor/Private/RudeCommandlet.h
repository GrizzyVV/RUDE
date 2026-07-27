// RUDE - RAGE <-> Unreal Development Environment
// The RUDE CLI: every toolset tool, callable headless from a terminal.
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "RudeCommandlet.generated.h"

// RUDE's command-line surface.
//
// WHY A COMMANDLET: the toolset was agent-only - MCP or the Python console - so a human with a
// terminal, a build script, or CI had no way in. A commandlet is the headless vehicle:
//
//   UnrealEditor-Cmd.exe <project>.uproject -run=RudeCommandlet -list
//   UnrealEditor-Cmd.exe <project>.uproject -run=RudeCommandlet -tool=ImportYdr <xml> <destFolder>
//
// ⭐ IT DISPATCHES BY REFLECTION, ON PURPOSE. Every tool is a `static FString Fn(const FString&...)`
// UFUNCTION on URudeToolset, so this walks the class's UFunctions and marshals string arguments
// generically. Nothing here enumerates tools by hand, which means the CLI CANNOT drift out of sync
// as tools are added or renamed - the failure that already left `AGENTS.md` describing 14 of 18
// tools. `-list` is therefore generated from the same authoritative source the MCP layer uses.
UCLASS()
class URudeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	URudeCommandlet();

	virtual int32 Main(const FString& Params) override;
};
