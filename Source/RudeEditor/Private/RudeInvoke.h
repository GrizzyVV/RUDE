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
	static TArray<UFunction*> CollectTools();

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
