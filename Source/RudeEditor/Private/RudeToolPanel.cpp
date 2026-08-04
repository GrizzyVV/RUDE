// RUDE - RAGE <-> Unreal Development Environment
#include "RudeToolPanel.h"

#include "RudeInvoke.h"
#include "RudeToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SExpandableArea.h"
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

// `RUDE.Run <Tool> [arg]...` - the 4th surface over the same FRudeInvoke core (panel/CLI/MCP are
// the other three). Exists so tools can be driven from `-ExecCmds` at editor launch: a scripted or
// agent-run import needs no MCP session and no human at the panel. Result goes to the log, which is
// what a headless driver reads anyway.
static FAutoConsoleCommand GRudeRun(
	TEXT("RUDE.Run"),
	TEXT("Run a RUDE tool by name with positional args: RUDE.Run <Tool> [arg]..."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogRudePanel, Display, TEXT("[RUDE.Run] usage: RUDE.Run <Tool> [arg]..."));
			return;
		}
		UFunction* Fn = FRudeInvoke::FindTool(Args[0]);
		TArray<FString> Values(Args.GetData() + 1, Args.Num() - 1);
		FString Result;
		FString Error;
		// ⛔ A CONSOLE COMMAND CANNOT SET A PROCESS EXIT CODE, so a headless harness keying on
		// `-ExecCmds` exit status sees success no matter what happened (measured 2026-07-31).
		// The contract instead is a STABLE, GREPPABLE MARKER: any failure - a dead tool, a bad
		// call, or a tool reporting ok:false - prints `RUDE_RUN_FAILED`, and nothing else in the
		// plugin ever prints that token. Harness rule: grep the log for RUDE_RUN_FAILED and treat
		// a hit as a failed run. (Control flow is deliberately unchanged: aborting the chain here
		// would skip a later cleanup step, which is worse than reporting honestly and continuing.)
		if (!Fn || !FRudeInvoke::Call(Fn, Values, Result, Error))
		{
			UE_LOG(LogRudePanel, Error, TEXT("[RUDE.Run] RUDE_RUN_FAILED %s: %s"), *Args[0],
				Fn ? *Error : TEXT("no such tool"));
			return;
		}
		if (FRudeInvoke::ReportedFailure(Result) || Result.IsEmpty())
		{
			// The call SUCCEEDED but the tool reported ok:false - that is still a failed run, and
			// it used to be logged at Display like any success.
			// ⛔ AN EMPTY RESULT COUNTS TOO. WHAT WAS WRONG: ReportedFailure only finds the literal
			// "ok":false, so a tool that returned NOTHING printed the success line and no harness
			// grepping RUDE_RUN_FAILED could see it - while the Slate path 240 lines below calls
			// the same case out explicitly (:318-323). Same core, two different answers. WHAT IT
			// COST: latent - measured, none of the 30 AICallable tools has an empty-return path
			// today - but this is the surface an agent drives, and it is the one that was silent.
			UE_LOG(LogRudePanel, Error, TEXT("[RUDE.Run] RUDE_RUN_FAILED %s -> %s"), *Args[0],
				Result.IsEmpty() ? TEXT("(no output)") : *Result);
			return;
		}
		UE_LOG(LogRudePanel, Display, TEXT("[RUDE.Run] %s -> %s"), *Args[0], *Result);
	}));

void SRudeToolPanel::Construct(const FArguments& InArgs)
{
	// HUMAN list: agent-only tools are dropped by their own RudeAudience tag, never by a name list
	// here - see FRudeInvoke::CollectTools.
	Tools = FRudeInvoke::CollectTools(/*bHumanOnly*/ true);
	StatusColour = FSlateColor::UseSubduedForeground();
	StatusText = LOCTEXT("Ready", "Pick a tool to begin.");

	// Log the split. It tells a maintainer at a glance that the audience filter is doing something,
	// and it is the only way to confirm the panel built when no UI automation is available.
	const int32 NumAll = FRudeInvoke::CollectTools(false).Num();
	UE_LOG(LogRudePanel, Display, TEXT("panel built: %d tools shown to humans, %d hidden as "
		"agent-only, %d total"), Tools.Num(), NumAll - Tools.Num(), NumAll);

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

		// ---- PLAIN-LANGUAGE help: what this does, for a person ----
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 2)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([this]()
			{
				return Selected ? FText::FromString(FRudeInvoke::PlainHelp(Selected))
				                : FText::GetEmpty();
			})
		]

		// ---- the technical detail, DEMOTED but not deleted: page plans and struct offsets are
		// load-bearing for agents and for whoever maintains this next, so it stays available.
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(true)
			.AreaTitle(LOCTEXT("TechDetail", "Technical detail"))
			.BodyContent()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]()
				{
					return Selected ? FText::FromString(FRudeInvoke::ToolTip(Selected))
					                : FText::GetEmpty();
				})
			]
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
		+ SVerticalBox::Slot().FillHeight(0.55f).Padding(8, 4, 8, 4)
		[
			SAssignNew(ResultBox, SMultiLineEditableTextBox)
			.IsReadOnly(true)
			.AllowMultiLine(true)
			.Text(FText::GetEmpty())
		]

		// ---- FOOTER: version + whether the agent surface came up. This replaces Ping as a menu
		// row - a person wants to SEE that the plugin is alive, not run a tool to ask.
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2, 8, 8)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::Format(
					LOCTEXT("Footer", "RUDE {0}  -  {1} tools  -  agent surface: {2}"),
					FText::FromString(GetDefault<URudeToolset>()->GetToolsetVersion()),
					FText::AsNumber(Tools.Num()),
					UToolsetRegistry::IsAvailable()
						&& UToolsetRegistry::IsToolsetClassRegistered(URudeToolset::StaticClass())
						? LOCTEXT("Reg", "registered") : LOCTEXT("Unreg", "NOT registered")))
			]
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
