// RUDE - RAGE <-> Unreal Development Environment
// The ONE reflective call path into URudeToolset, shared by every human/agent surface.
#pragma once

#include "CoreMinimal.h"

class UFunction;

// Every RUDE tool is a `static FString Fn(const FString&...)` UFUNCTION on URudeToolset. That
// uniformity is what lets one generic marshaller serve all of them - so the CLI, the Slate panel
// and the MCP layer expose the SAME tool set with the SAME semantics, and none of them carries a
// hand-written list that can fall out of date. (AGENTS.md drifted to 14-of-18 doing exactly that.)
//
// This is the Northstar made literal: "UI + MCP call the same URudeToolset core."
struct FRudeInvoke
{
	// Agent-callable tools, sorted by name. Reflection over URudeToolset, nothing hard-coded.
	// bHumanOnly drops tools marked RudeAudience="agent" - real capability that a person does not
	// want on a menu (they have better screenshot tools; version belongs in a status strip).
	// ⭐ THE TOOL DECLARES ITS OWN AUDIENCE. Do NOT filter by a name list in the UI instead: a
	// hand-maintained list is precisely the thing reflection removed, and the next agent-only tool
	// someone adds would silently appear on a human's menu.
	static TArray<UFunction*> CollectTools(bool bHumanOnly = false);

	// Plain-language, task-first help for a person. Falls back to the technical tooltip when a tool
	// has no RudeHelp yet, so a newly added tool is under-explained rather than blank.
	// ⚠ These are DELIBERATELY two different texts: the UFUNCTION comment carries page plans and
	// struct offsets that agents and maintainers need, and simplifying it would cost them.
	static FString PlainHelp(const UFunction* Fn);

	// Case-insensitive lookup among CollectTools(); nullptr when absent.
	static UFunction* FindTool(const FString& Name);

	// Parameter names in declaration order, excluding the return value.
	static TArray<FString> ParamNames(const UFunction* Fn);

	// The tooltip/doc comment UHT captured, so a UI can explain a tool without duplicating prose.
	static FString ToolTip(const UFunction* Fn);

	// True when the signature is FString-only (params AND return). A tool that ever stops being so
	// must be reported, never invoked with a zeroed frame - which would look like a real run.
	static bool IsAllStrings(const UFunction* Fn, FString& OutWhy);

	// Marshal `Values` positionally, invoke, and return the tool's own JSON string.
	// Missing values pass as empty strings, matching MCP behaviour. Returns false with OutError
	// set when the tool cannot be driven this way at all.
	static bool Call(UFunction* Fn, const TArray<FString>& Values, FString& OutResult,
	                 FString& OutError);

	// A tool reported failure. Note the polarity: only an explicit "ok":false is a failure, because
	// not every tool returns a JSON envelope (Ping returns a plain sentence).
	static bool ReportedFailure(const FString& Result);
};
