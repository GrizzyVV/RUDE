// RUDE - RAGE <-> Unreal Development Environment
#include "RudeToolPanel.h"

#include "RudeInvoke.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "RudeToolPanel"

DEFINE_LOG_CATEGORY_STATIC(LogRudePanel, Log, All);

const FName SRudeToolPanel::TabId(TEXT("RudeToolPanel"));

// `RUDE.Panel` opens the tab from the console - which also means from `-ExecCmds`, a startup script
// or CI. Registering the tab under Window is not enough on its own: without a command there is no
// way to open the panel except a human clicking menus, so it could not be driven or tested at all.
static FAutoConsoleCommand GRudeOpenPanel(
	TEXT("RUDE.Panel"),
	TEXT("Open the RUDE tool panel (the human surface for every RUDE import/export tool)."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->TryInvokeTab(SRudeToolPanel::TabId);
		}
	}));

void SRudeToolPanel::Construct(const FArguments& InArgs)
{
	Tools = FRudeInvoke::CollectTools();
	StatusColour = FSlateColor::UseSubduedForeground();
	StatusText = FText::Format(
		LOCTEXT("Ready", "{0} tools, listed by reflection - this panel cannot go stale."),
		FText::AsNumber(Tools.Num()));

	ChildSlot
	[
		SNew(SVerticalBox)

		// ---- tool picker ----
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("ToolLabel", "Tool"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SComboBox<UFunction*>)
				.OptionsSource(&Tools)
				.OnGenerateWidget_Lambda([](UFunction* Fn)
				{
					return SNew(STextBlock).Text(FText::FromString(Fn->GetName()));
				})
				.OnSelectionChanged_Lambda([this](UFunction* Fn, ESelectInfo::Type)
				{
					if (Fn)
					{
						SelectTool(Fn);
					}
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return Selected ? FText::FromString(Selected->GetName())
						                : LOCTEXT("PickTool", "choose a tool...");
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Run", "Run"))
				.IsEnabled_Lambda([this]() { return Selected != nullptr; })
				.OnClicked(this, &SRudeToolPanel::OnRun)
			]
		]

		// ---- the tool's own documentation, straight from its UFUNCTION comment ----
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([this]()
			{
				return Selected ? FText::FromString(FRudeInvoke::ToolTip(Selected)) : FText::GetEmpty();
			})
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2) [ SNew(SSeparator) ]

		// ---- one row per real parameter ----
		+ SVerticalBox::Slot().FillHeight(0.45f).Padding(8, 4)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(ArgumentBox, SVerticalBox)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return StatusText; })
			.ColorAndOpacity_Lambda([this]() { return StatusColour; })
			.AutoWrapText(true)
		]

		// ---- the tool's raw JSON, unedited: the same string agents and the CLI see ----
		+ SVerticalBox::Slot().FillHeight(0.55f).Padding(8, 4, 8, 8)
		[
			SAssignNew(ResultBox, SMultiLineEditableTextBox)
			.IsReadOnly(true)
			.AllowMultiLine(true)
			.Text(FText::GetEmpty())
		]
	];
}

void SRudeToolPanel::SelectTool(UFunction* Fn)
{
	Selected = Fn;
	RebuildArgumentRows();
	StatusColour = FSlateColor::UseSubduedForeground();

	FString Why;
	if (!FRudeInvoke::IsAllStrings(Fn, Why))
	{
		// Say so rather than presenting a Run button that would invoke a zeroed frame.
		StatusColour = FSlateColor(FLinearColor(1.f, 0.55f, 0.1f));
		StatusText = FText::FromString(FString::Printf(
			TEXT("'%s' cannot be driven from this panel: %s"), *Fn->GetName(), *Why));
		return;
	}
	StatusText = FText::Format(LOCTEXT("Selected", "{0} - {1} parameter(s)."),
		FText::FromString(Fn->GetName()), FText::AsNumber(ParamLabels.Num()));
}

void SRudeToolPanel::RebuildArgumentRows()
{
	ParamLabels.Reset();
	ParamBoxes.Reset();
	if (!ArgumentBox.IsValid())
	{
		return;
	}
	ArgumentBox->ClearChildren();
	if (!Selected)
	{
		return;
	}

	ParamLabels = FRudeInvoke::ParamNames(Selected);
	for (const FString& Label : ParamLabels)
	{
		TSharedPtr<SEditableTextBox> Box;
		ArgumentBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.32f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(FText::FromString(Label))
			]
			+ SHorizontalBox::Slot().FillWidth(0.68f)
			[
				SAssignNew(Box, SEditableTextBox)
				.HintText(LOCTEXT("EmptyHint", "(empty)"))
			]
		];
		ParamBoxes.Add(Box);
	}
}

FReply SRudeToolPanel::OnRun()
{
	if (!Selected)
	{
		return FReply::Handled();
	}

	TArray<FString> Values;
	Values.Reserve(ParamBoxes.Num());
	for (const TSharedPtr<SEditableTextBox>& Box : ParamBoxes)
	{
		Values.Add(Box.IsValid() ? Box->GetText().ToString() : FString());
	}

	FString Result;
	FString Error;
	const bool bCalled = FRudeInvoke::Call(Selected, Values, Result, Error);

	if (!bCalled)
	{
		StatusColour = FSlateColor(FLinearColor(1.f, 0.3f, 0.3f));
		StatusText = FText::FromString(Error);
		if (ResultBox.IsValid())
		{
			ResultBox->SetText(FText::GetEmpty());
		}
		return FReply::Handled();
	}

	// Report the TOOL's verdict, not merely that the call returned - the same polarity the CLI's
	// exit code uses, and for the same reason: not every tool returns a JSON envelope.
	// ⚠ An EMPTY result is called out separately: it is neither success nor a reported failure, and
	// folding it into "completed" would let a tool that produced nothing look like it worked.
	const bool bFailed = FRudeInvoke::ReportedFailure(Result);
	if (Result.IsEmpty())
	{
		StatusColour = FSlateColor(FLinearColor(1.f, 0.55f, 0.1f));
		StatusText = FText::Format(LOCTEXT("Empty", "{0} returned no output."),
			FText::FromString(Selected->GetName()));
	}
	else
	{
		StatusColour = bFailed ? FSlateColor(FLinearColor(1.f, 0.3f, 0.3f))
		                       : FSlateColor(FLinearColor(0.35f, 0.85f, 0.4f));
		StatusText = bFailed
			? FText::Format(LOCTEXT("Failed", "{0} reported a failure."),
				FText::FromString(Selected->GetName()))
			: FText::Format(LOCTEXT("Ok", "{0} completed."), FText::FromString(Selected->GetName()));
	}

	if (ResultBox.IsValid())
	{
		ResultBox->SetText(FText::FromString(Result));
	}

	// Also log it. A read-only multiline box does not expose its contents to Slate automation, so
	// without this a panel run cannot be verified by anything except a human reading the widget -
	// and users get a scrollback of what they ran either way.
	UE_LOG(LogRudePanel, Display, TEXT("%s -> %s"), *Selected->GetName(), *Result);
	return FReply::Handled();
}

void SRudeToolPanel::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SRudeToolPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "RUDE"))
		.SetTooltipText(LOCTEXT("TabTooltip",
			"RUDE - RAGE <-> Unreal Development Environment. Every import/export tool, "
			"listed by reflection."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
}

void SRudeToolPanel::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

#undef LOCTEXT_NAMESPACE
