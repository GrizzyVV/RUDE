// RUDE - RAGE <-> Unreal Development Environment
// The human surface: a dockable editor panel driving every RUDE tool.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UFunction;
class SEditableTextBox;
class SMultiLineEditableTextBox;

// WHY THIS EXISTS: RUDE had no human path at all. Every tool was reachable only from MCP or the
// Python console, so a person sitting at the editor could not use the plugin - foundation step 5.
//
// It is built on the SAME reflective core as the CLI (FRudeInvoke), so the panel lists exactly the
// tools that exist, with exactly their real parameters, and cannot fall out of date as tools are
// added. Nothing about the tool set is written down twice.
class SRudeToolPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRudeToolPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Tab id + registration, called from the module so the panel appears under Window.
	static const FName TabId;
	static void RegisterTabSpawner();
	static void UnregisterTabSpawner();

private:
	void SelectTool(UFunction* Fn);
	void RebuildArgumentRows();
	FReply OnRun();

	TArray<UFunction*> Tools;
	UFunction* Selected = nullptr;

	TArray<FString> ParamLabels;
	TArray<TSharedPtr<SEditableTextBox>> ParamBoxes;
	TSharedPtr<class SVerticalBox> ArgumentBox;
	TSharedPtr<SMultiLineEditableTextBox> ResultBox;
	FText StatusText;
	FSlateColor StatusColour;
};
