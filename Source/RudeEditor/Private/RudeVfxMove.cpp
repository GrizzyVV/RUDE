// RUDE - RAGE <-> Unreal Development Environment
// WP13 vfx_move lane: the two GDD Tier 3 formats that had no surface at all.
//   * .ypt  (particle effect dictionaries)  -> URudeParticleEffect, PREVIEW tier, READ-ONLY
//   * .mrf  (MoVE animation networks)       -> URudeMoveNetwork,   READ-ONLY graph view
// Neither lane has a writer and neither is a conversion. Measured over the corpus (maintainer lane
// `vfx_move`, `LAWS.md`): 1,240 ypt files / 10,268 effect rules / 23,840 emitter rules / 21,837
// particle rules, and 162 mrf files / 33,636 graph nodes / 8,000 transitions / 9,136 conditions.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudeParticleEffect.h"
#include "RudeMoveNetwork.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "XmlFile.h"

namespace RudeVfx
{
	// A package-legal spelling of a game name (effect names carry no exotic characters in this corpus,
	// but a name is never trusted to be legal).
	static FString SafeName(const FString& In)
	{
		FString O;
		O.Reserve(In.Len());
		for (TCHAR C : In) { O.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_')); }
		return O.IsEmpty() ? FString(TEXT("_")) : O;
	}

	static const FXmlNode* Kid(const FXmlNode* N, const TCHAR* Tag)
	{
		if (!N) { return nullptr; }
		for (const FXmlNode* C : N->GetChildrenNodes()) { if (C && C->GetTag() == Tag) { return C; } }
		return nullptr;
	}

	// The text a leaf element carries: its value= attribute, else an x/y/z(/w) tuple, else its content.
	static FString LeafText(const FXmlNode* N)
	{
		if (!N) { return FString(); }
		const FString V = N->GetAttribute(TEXT("value"));
		if (!V.IsEmpty()) { return V; }
		const FString R = N->GetAttribute(TEXT("ref"));
		if (!R.IsEmpty()) { return R; }
		const FString X = N->GetAttribute(TEXT("x"));
		if (!X.IsEmpty())
		{
			FString O = X + TEXT(",") + N->GetAttribute(TEXT("y")) + TEXT(",") + N->GetAttribute(TEXT("z"));
			const FString W = N->GetAttribute(TEXT("w"));
			if (!W.IsEmpty()) { O += TEXT(",") + W; }
			return O;
		}
		return N->GetContent().TrimStartAndEnd();
	}

	static FString Str(const FXmlNode* N, const TCHAR* Tag) { return LeafText(Kid(N, Tag)); }
	static float   Flt(const FXmlNode* N, const TCHAR* Tag) { return FCString::Atof(*Str(N, Tag)); }
	static int32   Num(const FXmlNode* N, const TCHAR* Tag) { return FCString::Atoi(*Str(N, Tag)); }
	static bool    Bl (const FXmlNode* N, const TCHAR* Tag)
	{
		const FString S = Str(N, Tag);
		return S == TEXT("1") || S.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	// RAGE metres -> UE centimetres with the house Y mirror (AGENTS section 6 rule 1).
	static FVector XyzCm(const FXmlNode* N, const TCHAR* Tag)
	{
		const FXmlNode* C = Kid(N, Tag);
		if (!C) { return FVector::ZeroVector; }
		return FVector(FCString::Atod(*C->GetAttribute(TEXT("x"))) * 100.0,
		               -FCString::Atod(*C->GetAttribute(TEXT("y"))) * 100.0,
		               FCString::Atod(*C->GetAttribute(TEXT("z"))) * 100.0);
	}

	// Every LEAF child of Item into Out (tag -> text). A child that has children of its own is a
	// container and is skipped here - the reader handles those explicitly or not at all, and the
	// verdict says which. Nothing is silently converted.
	static void RawMap(const FXmlNode* Item, TMap<FString, FString>& Out)
	{
		if (!Item) { return; }
		for (const FXmlNode* C : Item->GetChildrenNodes())
		{
			if (!C || C->GetChildrenNodes().Num() > 0) { continue; }
			Out.Add(C->GetTag(), LeafText(C));
		}
	}

	static bool IsHashName(const FString& S)
	{
		if (S.Len() != 13 || !S.StartsWith(TEXT("hash_"), ESearchCase::CaseSensitive)) { return false; }
		for (int32 i = 5; i < 13; ++i) { if (!FChar::IsHexDigit(S[i])) { return false; } }
		return true;
	}

	static FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	}

	// Open the corpus and find the effective row of (Type, Name). Loads the XML. Empty return = ok.
	static FString OpenRow(const FString& CorpusRoot, const TCHAR* Type, const FString& Name,
	                       TSharedPtr<FRudeCorpus>& OutCorpus, const FRudeCorpusEntry*& OutRow, FString& OutXmlPath)
	{
		FString Err;
		OutCorpus = FRudeCorpus::Open(CorpusRoot, Err);
		if (!OutCorpus.IsValid()) { return Err; }
		const FString Wanted = Name.TrimStartAndEnd().ToLower();
		if (Wanted.IsEmpty()) { return FString::Printf(TEXT("no %s name given"), Type); }
		OutRow = OutCorpus->Effective(Type, Wanted);
		if (!OutRow)
		{
			TArray<const FRudeCorpusEntry*> Rows;
			OutCorpus->ByPrefix(Type, TEXT(""), Rows);
			FString Near;
			int32 Listed = 0;
			for (const FRudeCorpusEntry* E : Rows)
			{
				if (!E->Name.Contains(Wanted)) { continue; }
				if (Listed++ >= 10) { break; }
				Near += (Near.IsEmpty() ? TEXT("") : TEXT(" ")) + E->Name;
			}
			return FString::Printf(TEXT("the corpus has no %s named '%s' (%d %s files; near matches: %s)"),
			                       Type, *Wanted, Rows.Num(), Type, Near.IsEmpty() ? TEXT("none") : *Near);
		}
		if (!OutRow->bConverted) { return FString::Printf(TEXT("'%s' is a KEPT BINARY row in this corpus - no interchange XML to read"), *Wanted); }
		OutXmlPath = OutCorpus->PathOf(*OutRow);
		return FString();
	}
}

// ---- ImportParticleEffects -----------------------------------------------------------------
// One .ypt's effect rules as URudeParticleEffect DataAssets, one asset per EFFECT RULE, under
// <DestFolder>/<ypt>/. EffectName is "<ypt>" for every rule in that dictionary or "<ypt>/<rule>" for
// one. Both halves are needed to name an effect: 2,549 distinct effect-rule names occur 10,268 times
// across the corpus's 1,240 ypt files, so a rule name alone is ambiguous.
//
// What is READ (measured over 10,268 effect rules): the 43 fields every effect rule carries plus
// EvolutionList on 8,441 of them; each rule's EventEmitters (27,676 across the corpus) with the emitter
// and particle rule each names - 27,676/27,676 of those names resolve inside the SAME file's own
// dictionaries, so the lane never needs a cross-file lookup; and a shallow read of each referenced
// emitter rule (creation/target domain shape) and particle rule (shader file, technique, draw type,
// behaviour list). Positions and distances become UE centimetres with the house Y mirror; everything
// else is carried as the file spells it, and EVERY leaf field of every record is also kept verbatim in
// a raw field map, so a field this lane does not understand is visible rather than dropped.
//
// ⛔ PREVIEW TIER, READ-ONLY, and it does NOT convert an effect. No Niagara system is generated, no
// keyframe curve is evaluated (40,055 of the corpus's 51,340 effect-rule keyframe props carry zero
// keys anyway), no particle material is built, no drawable or texture in the ypt is imported, and
// there is NO WRITER - nothing in RUDE emits a .ypt. Full authoring is a later epic.
// Returns JSON: {ok, ypt, slot, file, sha1, effectRules, matched, created, refilled, eventEmitters,
// rulesRead, rulesUnresolved, keyframeProps, keyframePropsWithKeys, drawablesNotImported,
// texturesNotImported, invalidNames, destFolder, tier, note, sample[]}.
// Every per-rule number counts only the rules the filter MATCHED; effectRules, drawablesNotImported
// and texturesNotImported are whole-dictionary counts.
FString URudeToolset::ImportParticleEffects(const FString& CorpusRoot, const FString& EffectName, const FString& DestFolder)
{
	FString Wanted = EffectName.TrimStartAndEnd();
	FString RuleFilter;
	int32 Slash = INDEX_NONE;
	if (Wanted.FindChar(TEXT('/'), Slash)) { RuleFilter = Wanted.Mid(Slash + 1).TrimStartAndEnd().ToLower(); Wanted = Wanted.Left(Slash); }

	TSharedPtr<FRudeCorpus> Corpus;
	const FRudeCorpusEntry* Row = nullptr;
	FString XmlPath;
	const FString Why = RudeVfx::OpenRow(CorpusRoot, TEXT("ypt"), Wanted, Corpus, Row, XmlPath);
	if (!Why.IsEmpty()) { return RudeVfx::Fail(Why); }

	FXmlFile Doc(XmlPath, EConstructMethod::ConstructFromFile);
	if (!Doc.IsValid()) { return RudeVfx::Fail(FString::Printf(TEXT("could not parse %s: %s"), *XmlPath, *Doc.GetLastError())); }
	const FXmlNode* Root = Doc.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("ParticleEffectsList"))
	{
		return RudeVfx::Fail(FString::Printf(TEXT("%s root is <%s>, not <ParticleEffectsList>"), *XmlPath, Root ? *Root->GetTag() : TEXT("none")));
	}

	const FXmlNode* EffDict = RudeVfx::Kid(Root, TEXT("EffectRuleDictionary"));
	const FXmlNode* EmiDict = RudeVfx::Kid(Root, TEXT("EmitterRuleDictionary"));
	const FXmlNode* ParDict = RudeVfx::Kid(Root, TEXT("ParticleRuleDictionary"));
	const FXmlNode* DrwDict = RudeVfx::Kid(Root, TEXT("DrawableDictionary"));
	const FXmlNode* TexDict = RudeVfx::Kid(Root, TEXT("TextureDictionary"));

	// name -> the rule item, for the in-file resolution the format guarantees.
	TMap<FString, const FXmlNode*> EmiByName, ParByName;
	if (EmiDict) { for (const FXmlNode* It : EmiDict->GetChildrenNodes()) { EmiByName.Add(RudeVfx::Str(It, TEXT("Name")).ToLower(), It); } }
	if (ParDict) { for (const FXmlNode* It : ParDict->GetChildrenNodes()) { ParByName.Add(RudeVfx::Str(It, TEXT("Name")).ToLower(), It); } }

	const FString YptName = Wanted.ToLower();
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty()
		? (TEXT("/Game/RUDE/VFX/") + RudeVfx::SafeName(YptName))
		: (DestFolder.TrimStartAndEnd() / RudeVfx::SafeName(YptName));

	const TCHAR* const TierText = TEXT("PREVIEW TIER, READ-ONLY: the ypt record as measured, not a conversion. No Niagara system, no curves evaluated, no materials, no drawables or textures imported, and no .ypt writer exists.");

	int32 Total = 0, Matched = 0, Created = 0, Refilled = 0, Events = 0, RulesRead = 0, RulesUnresolved = 0;
	int32 KfProps = 0, KfWithKeys = 0, Invalid = 0;
	FString Sample;
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	if (EffDict)
	{
		for (const FXmlNode* It : EffDict->GetChildrenNodes())
		{
			++Total;
			const FString RuleName = RudeVfx::Str(It, TEXT("Name"));
			if (!RuleFilter.IsEmpty() && RuleName.ToLower() != RuleFilter) { continue; }
			++Matched;

			const FString AssetName = RudeVfx::SafeName(RuleName);
			const FString PkgName = Dest / AssetName;
			if (!FPackageName::IsValidLongPackageName(PkgName)) { ++Invalid; continue; }

			URudeParticleEffect* A = LoadObject<URudeParticleEffect>(nullptr, *(PkgName + TEXT(".") + AssetName));
			UPackage* Pkg = nullptr;
			if (A) { Pkg = A->GetOutermost(); }
			else { Pkg = CreatePackage(*PkgName); }
			bool bNew = false;
			if (!A) { A = NewObject<URudeParticleEffect>(Pkg, FName(*AssetName), RF_Public | RF_Standalone); bNew = true; }

			A->EffectName = RuleName;
			A->YptName = YptName;
			A->Tier = TEXT("preview");
			A->TierNote = TierText;
			A->CorpusRoot = Corpus->GetRoot();
			A->SourceSlot = Row->Slot;
			A->SourceFile = Row->File;
			A->SourceSha1 = Row->Sha1;

			A->RefCount = RudeVfx::Num(It, TEXT("RefCount"));
			A->FileVersion = RudeVfx::Str(It, TEXT("FileVersion"));
			A->NumLoops = RudeVfx::Str(It, TEXT("NumLoops"));
			A->DurationMin = RudeVfx::Flt(It, TEXT("DurationMin"));
			A->DurationMax = RudeVfx::Flt(It, TEXT("DurationMax"));
			A->PreUpdateTime = RudeVfx::Flt(It, TEXT("PreUpdateTime"));
			A->PreUpdateTimeInterval = RudeVfx::Flt(It, TEXT("PreUpdateTimeInterval"));
			A->PlaybackRateScalarMin = RudeVfx::Flt(It, TEXT("PlaybackRateScalarMin"));
			A->PlaybackRateScalarMax = RudeVfx::Flt(It, TEXT("PlaybackRateScalarMax"));
			A->bIsShortLived = RudeVfx::Bl(It, TEXT("IsShortLived"));
			A->bHasNoShadows = RudeVfx::Bl(It, TEXT("HasNoShadows"));
			A->bSortEventsByDistance = RudeVfx::Bl(It, TEXT("SortEventsByDistance"));
			A->DrawListID = RudeVfx::Num(It, TEXT("DrawListID"));
			A->RandomOffsetPosCm = RudeVfx::XyzCm(It, TEXT("VRandomOffsetPos"));
			A->ViewportCullingSphereOffsetCm = RudeVfx::XyzCm(It, TEXT("ViewportCullingSphereOffset"));
			A->ViewportCullingSphereRadiusCm = RudeVfx::Flt(It, TEXT("ViewportCullingSphereRadius")) * 100.f;
			A->DistanceCullingFadeDistCm = RudeVfx::Flt(It, TEXT("DistanceCullingFadeDist")) * 100.f;
			A->DistanceCullingCullDistCm = RudeVfx::Flt(It, TEXT("DistanceCullingCullDist")) * 100.f;
			A->LodEvoDistanceMinCm = RudeVfx::Flt(It, TEXT("LodEvoDistanceMin")) * 100.f;
			A->LodEvoDistanceMaxCm = RudeVfx::Flt(It, TEXT("LodEvoDistanceMax")) * 100.f;
			A->CollisionRangeCm = RudeVfx::Flt(It, TEXT("CollisionRange")) * 100.f;
			A->CollisionProbeDistanceCm = RudeVfx::Flt(It, TEXT("CollisionProbeDistance")) * 100.f;
			A->CollisionType = RudeVfx::Num(It, TEXT("CollisionType"));
			A->ViewportCullingMode = RudeVfx::Num(It, TEXT("ViewportCullingMode"));
			A->DistanceCullingMode = RudeVfx::Num(It, TEXT("DistanceCullingMode"));
			A->ZoomLevel = RudeVfx::Num(It, TEXT("ZoomLevel"));
			A->GameFlags = RudeVfx::Str(It, TEXT("GameFlags"));
			A->RawFields.Empty();
			RudeVfx::RawMap(It, A->RawFields);

			A->Evolutions.Empty();
			if (const FXmlNode* Evo = RudeVfx::Kid(RudeVfx::Kid(It, TEXT("EvolutionList")), TEXT("Evolutions")))
			{
				for (const FXmlNode* E : Evo->GetChildrenNodes()) { A->Evolutions.Add(E->GetContent().TrimStartAndEnd()); }
			}

			A->KeyframePropKeys.Empty();
			if (const FXmlNode* Kfs = RudeVfx::Kid(It, TEXT("KeyframeProps")))
			{
				for (const FXmlNode* K : Kfs->GetChildrenNodes())
				{
					++KfProps;
					const FXmlNode* List = RudeVfx::Kid(K, TEXT("Keyframes"));
					const int32 Keys = List ? List->GetChildrenNodes().Num() : 0;
					if (Keys > 0) { ++KfWithKeys; }
					A->KeyframePropKeys.Add(RudeVfx::Str(K, TEXT("Name")), Keys);
				}
			}

			A->EventEmitters.Empty();
			A->Rules.Empty();
			TSet<FString> RulesSeen;
			if (const FXmlNode* Evs = RudeVfx::Kid(It, TEXT("EventEmitters")))
			{
				for (const FXmlNode* Ev : Evs->GetChildrenNodes())
				{
					++Events;
					FRudeYptEventEmitter Out;
					Out.EmitterRule = RudeVfx::Str(Ev, TEXT("EmitterRule"));
					Out.ParticleRule = RudeVfx::Str(Ev, TEXT("ParticleRule"));
					Out.EventType = RudeVfx::Num(Ev, TEXT("EventType"));
					Out.StartRatio = RudeVfx::Flt(Ev, TEXT("StartRatio"));
					Out.EndRatio = RudeVfx::Flt(Ev, TEXT("EndRatio"));
					Out.PlaybackRateScalarMin = RudeVfx::Flt(Ev, TEXT("PlaybackRateScalarMin"));
					Out.PlaybackRateScalarMax = RudeVfx::Flt(Ev, TEXT("PlaybackRateScalarMax"));
					Out.ZoomScalarMin = RudeVfx::Flt(Ev, TEXT("ZoomScalarMin"));
					Out.ZoomScalarMax = RudeVfx::Flt(Ev, TEXT("ZoomScalarMax"));
					Out.ColourTintMin = RudeVfx::Str(Ev, TEXT("ColourTintMin"));
					Out.ColourTintMax = RudeVfx::Str(Ev, TEXT("ColourTintMax"));
					if (const FXmlNode* EvList = RudeVfx::Kid(Ev, TEXT("EvolutionList")))
					{
						if (const FXmlNode* Names = RudeVfx::Kid(EvList, TEXT("Evolutions")))
						{
							for (const FXmlNode* E : Names->GetChildrenNodes()) { Out.Evolutions.Add(E->GetContent().TrimStartAndEnd()); }
						}
						if (const FXmlNode* Ekp = RudeVfx::Kid(EvList, TEXT("EvolvedKeyframeProps"))) { Out.EvolvedKeyframeProps = Ekp->GetChildrenNodes().Num(); }
					}
					A->EventEmitters.Add(Out);

					// The two rules the event names, read shallow, first reference only.
					for (int32 Which = 0; Which < 2; ++Which)
					{
						const bool bEmitter = (Which == 0);
						const FString RefName = bEmitter ? Out.EmitterRule : Out.ParticleRule;
						if (RefName.IsEmpty()) { continue; }
						const FString Key = (bEmitter ? TEXT("e:") : TEXT("p:")) + RefName.ToLower();
						if (RulesSeen.Contains(Key)) { continue; }
						RulesSeen.Add(Key);
						const FXmlNode* const* Found = bEmitter ? EmiByName.Find(RefName.ToLower()) : ParByName.Find(RefName.ToLower());
						if (!Found || !*Found) { ++RulesUnresolved; continue; }
						++RulesRead;
						const FXmlNode* RuleIt = *Found;
						FRudeYptRuleRef Ref;
						Ref.Name = RefName;
						Ref.Kind = bEmitter ? TEXT("emitter") : TEXT("particle");
						if (bEmitter)
						{
							Ref.CreationDomainType = RudeVfx::Str(RudeVfx::Kid(RuleIt, TEXT("CreationDomainObj")), TEXT("DomainType"));
							Ref.TargetDomainType = RudeVfx::Str(RudeVfx::Kid(RuleIt, TEXT("TargetDomainObj")), TEXT("DomainType"));
						}
						else
						{
							Ref.ShaderFile = RudeVfx::Str(RuleIt, TEXT("ShaderFile"));
							Ref.ShaderTechnique = RudeVfx::Str(RuleIt, TEXT("ShaderTechnique"));
							Ref.DrawType = RudeVfx::Num(RuleIt, TEXT("DrawType"));
							if (const FXmlNode* Behs = RudeVfx::Kid(RuleIt, TEXT("AllBehaviours")))
							{
								for (const FXmlNode* B : Behs->GetChildrenNodes()) { Ref.Behaviours.Add(RudeVfx::Str(B, TEXT("Type"))); }
							}
						}
						RudeVfx::RawMap(RuleIt, Ref.RawFields);
						A->Rules.Add(Ref);
					}
				}
			}

			Pkg->MarkPackageDirty();
			if (bNew) { ARM.Get().AssetCreated(A); ++Created; } else { ++Refilled; }
			if (Sample.Len() < 1200)
			{
				Sample += FString::Printf(TEXT("%s{\"effect\":\"%s\",\"events\":%d,\"rules\":%d,\"durationMax\":%.4f}"),
					Sample.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(RuleName), A->EventEmitters.Num(), A->Rules.Num(), A->DurationMax);
			}
		}
	}

	const bool bOk = Matched > 0 && (Created + Refilled) == (Matched - Invalid) && Invalid == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ypt\":\"%s\",\"slot\":\"%s\",\"file\":\"%s\",\"sha1\":\"%s\",\"effectRules\":%d,\"matched\":%d,\"created\":%d,\"refilled\":%d,")
		TEXT("\"eventEmitters\":%d,\"rulesRead\":%d,\"rulesUnresolved\":%d,\"keyframeProps\":%d,\"keyframePropsWithKeys\":%d,\"drawablesNotImported\":%d,")
		TEXT("\"texturesNotImported\":%d,\"invalidNames\":%d,\"destFolder\":\"%s\",\"tier\":\"preview\",\"note\":\"%s\",\"sample\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(YptName), *RudeJsonEscape(Row->Slot), *RudeJsonEscape(Row->File), *RudeJsonEscape(Row->Sha1),
		Total, Matched, Created, Refilled, Events, RulesRead, RulesUnresolved, KfProps, KfWithKeys,
		DrwDict ? DrwDict->GetChildrenNodes().Num() : 0, TexDict ? TexDict->GetChildrenNodes().Num() : 0,
		Invalid, *RudeJsonEscape(Dest), *RudeJsonEscape(TierText), *Sample);
}

// ---- PlaceParticlePreview ------------------------------------------------------------------
// A placement stand-in for one imported effect: an ARudeParticlePreview with a wireframe sphere at
// the effect's own viewport-culling radius and a text label that says what it is.
// WHERE IT LANDS: LocationCm PLUS the effect's own ViewportCullingSphereOffset, so the marker is
// centred on the culling volume rather than on the point passed in. A caller passing "0,0,0" for an
// effect with a non-zero offset gets an actor away from the origin; the verdict's locationCm always
// reports where it actually went.
// ⛔ It is an APPROXIMATION of WHERE the effect sits and roughly how far it reaches. It does not
// simulate, emit or render the effect and it is not a conversion of one. The marker is a SPHERE
// whatever the emitter's creation domain says (Cylinder 1,619 / Sphere 1,413 / Box 1,114 over the base
// slot's 4,146 emitter rules) - the domain is recorded on the actor, not drawn.
// The radius is neutral by default: the effect's ViewportCullingSphereRadius when it declares one,
// else its DistanceCullingCullDist, else 100 cm, and radiusSource says which was used.
// Returns JSON: {ok, effect, ypt, actor, locationCm, radiusCm, radiusSource, creationDomain, approximation}.
FString URudeToolset::PlaceParticlePreview(const FString& EffectAssetPath, const FString& LocationCm)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return RudeVfx::Fail(TEXT("no editor world")); }
	const FString Path = EffectAssetPath.TrimStartAndEnd();
	if (Path.IsEmpty()) { return RudeVfx::Fail(TEXT("EffectAssetPath is empty (a URudeParticleEffect built by ImportParticleEffects)")); }
	URudeParticleEffect* Fx = LoadObject<URudeParticleEffect>(nullptr, *Path);
	if (!Fx) { return RudeVfx::Fail(FString::Printf(TEXT("no URudeParticleEffect at '%s'"), *Path)); }

	FVector Where = FVector::ZeroVector;
	const FString Loc = LocationCm.TrimStartAndEnd();
	if (!Loc.IsEmpty())
	{
		TArray<FString> Parts;
		Loc.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 3) { return RudeVfx::Fail(FString::Printf(TEXT("LocationCm '%s' is not \"x,y,z\" in centimetres"), *Loc)); }
		Where = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
	}

	float Radius = 100.f;
	FString RadiusSource = TEXT("default 100 cm (the effect declares neither a culling sphere nor a cull distance)");
	if (Fx->ViewportCullingSphereRadiusCm > 0.f) { Radius = Fx->ViewportCullingSphereRadiusCm; RadiusSource = TEXT("ViewportCullingSphereRadius"); }
	else if (Fx->DistanceCullingCullDistCm > 0.f) { Radius = Fx->DistanceCullingCullDistCm; RadiusSource = TEXT("DistanceCullingCullDist"); }

	FString Domain;
	for (const FRudeYptRuleRef& R : Fx->Rules)
	{
		if (R.Kind == TEXT("emitter") && !R.CreationDomainType.IsEmpty()) { Domain = R.CreationDomainType; break; }
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transactional;
	ARudeParticlePreview* A = World->SpawnActor<ARudeParticlePreview>(ARudeParticlePreview::StaticClass(), FTransform(Where + Fx->ViewportCullingSphereOffsetCm), Params);
	if (!A) { return RudeVfx::Fail(TEXT("SpawnActor returned null")); }

	A->Effect = Fx;
	A->EffectName = Fx->EffectName;
	A->YptName = Fx->YptName;
	A->CreationDomain = Domain;
	A->RadiusCm = Radius;
	A->RadiusSource = RadiusSource;
	if (A->Extent) { A->Extent->SetSphereRadius(Radius); }
	if (A->Label)
	{
		A->Label->SetHorizontalAlignment(EHTA_Center);
		A->Label->SetWorldSize(FMath::Clamp(Radius * 0.2f, 20.f, 200.f));
		A->Label->SetTextRenderColor(FColor(255, 160, 0));
		A->Label->SetText(FText::FromString(FString::Printf(TEXT("%s\n(RUDE VFX PREVIEW - approximation)"), *Fx->EffectName)));
	}
	A->Tags.Add(FName(TEXT("RUDE_VFX_PREVIEW")));
	A->SetActorLabel(FString::Printf(TEXT("VFXPREVIEW_%s"), *RudeVfx::SafeName(Fx->EffectName)));

	// ok must be able to be FALSE. Extent and Label are constructor subobjects, so testing only that
	// they exist can never fail on a spawned actor; these can: the sphere has to carry the radius the
	// tool computed, a radiusSource has to have been recorded, and the actor has to be where the spawn
	// transform asked it to be.
	const bool bRadius = A->Extent != nullptr && FMath::IsNearlyEqual(A->Extent->GetUnscaledSphereRadius(), Radius, 0.01f);
	const bool bPlaced = A->GetActorLocation().Equals(Where + Fx->ViewportCullingSphereOffsetCm, 0.5f);
	const bool bOk = bRadius && bPlaced && A->Label != nullptr && !RadiusSource.IsEmpty();
	return FString::Printf(
		TEXT("{\"ok\":%s,\"effect\":\"%s\",\"ypt\":\"%s\",\"actor\":\"%s\",\"locationCm\":\"%.1f,%.1f,%.1f\",\"radiusCm\":%.2f,\"radiusSource\":\"%s\",")
		TEXT("\"creationDomain\":\"%s\",\"approximation\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Fx->EffectName), *RudeJsonEscape(Fx->YptName), *RudeJsonEscape(A->GetActorLabel()),
		A->GetActorLocation().X, A->GetActorLocation().Y, A->GetActorLocation().Z, Radius, *RudeJsonEscape(RadiusSource),
		*RudeJsonEscape(Domain), *RudeJsonEscape(A->Approximation));
}

// ---- ProbeYptXml ---------------------------------------------------------------------------
// Re-read one ypt out of the corpus and count the lane's own structural laws on it, without creating
// a single asset: the four dictionaries' sizes, how many event emitters there are and how many of
// their emitter/particle references resolve inside the same file, how many keyframe props carry keys,
// the shader-file and domain-type census, and the field-tag count of the first effect rule. This is
// the instrument a gate runs when it wants numbers rather than assets.
// ok is COMPUTED: it is true only when the file parses, its root is <ParticleEffectsList>, and every
// event-emitter reference resolves in-file (27,676/27,676 corpus-wide, so an unresolved one is news).
// Returns JSON: {ok, ypt, slot, file, bytes, effectRules, emitterRules, particleRules, drawables,
// textures, unreferencedStrings, eventEmitters, refsResolved, refsUnresolved, keyframeProps,
// keyframePropsWithKeys, firstEffectFieldTags, shaderFiles{}, creationDomains{}}.
FString URudeToolset::ProbeYptXml(const FString& CorpusRoot, const FString& YptName)
{
	TSharedPtr<FRudeCorpus> Corpus;
	const FRudeCorpusEntry* Row = nullptr;
	FString XmlPath;
	const FString Why = RudeVfx::OpenRow(CorpusRoot, TEXT("ypt"), YptName, Corpus, Row, XmlPath);
	if (!Why.IsEmpty()) { return RudeVfx::Fail(Why); }

	const int64 Bytes = IFileManager::Get().FileSize(*XmlPath);
	FXmlFile Doc(XmlPath, EConstructMethod::ConstructFromFile);
	if (!Doc.IsValid()) { return RudeVfx::Fail(FString::Printf(TEXT("could not parse %s: %s"), *XmlPath, *Doc.GetLastError())); }
	const FXmlNode* Root = Doc.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("ParticleEffectsList"))
	{
		return RudeVfx::Fail(FString::Printf(TEXT("%s root is <%s>, not <ParticleEffectsList>"), *XmlPath, Root ? *Root->GetTag() : TEXT("none")));
	}

	const FXmlNode* EffDict = RudeVfx::Kid(Root, TEXT("EffectRuleDictionary"));
	const FXmlNode* EmiDict = RudeVfx::Kid(Root, TEXT("EmitterRuleDictionary"));
	const FXmlNode* ParDict = RudeVfx::Kid(Root, TEXT("ParticleRuleDictionary"));
	const FXmlNode* DrwDict = RudeVfx::Kid(Root, TEXT("DrawableDictionary"));
	const FXmlNode* TexDict = RudeVfx::Kid(Root, TEXT("TextureDictionary"));
	const FXmlNode* UnrList = RudeVfx::Kid(Root, TEXT("UnreferencedStrings"));

	TSet<FString> EmiNames, ParNames;
	TMap<FString, int32> Shaders, Domains;
	if (EmiDict)
	{
		for (const FXmlNode* It : EmiDict->GetChildrenNodes())
		{
			EmiNames.Add(RudeVfx::Str(It, TEXT("Name")).ToLower());
			Domains.FindOrAdd(RudeVfx::Str(RudeVfx::Kid(It, TEXT("CreationDomainObj")), TEXT("DomainType")))++;
		}
	}
	if (ParDict)
	{
		for (const FXmlNode* It : ParDict->GetChildrenNodes())
		{
			ParNames.Add(RudeVfx::Str(It, TEXT("Name")).ToLower());
			Shaders.FindOrAdd(RudeVfx::Str(It, TEXT("ShaderFile")))++;
		}
	}

	int32 Events = 0, Resolved = 0, Unresolved = 0, KfProps = 0, KfWithKeys = 0, FirstTags = 0;
	if (EffDict)
	{
		bool bFirst = true;
		for (const FXmlNode* It : EffDict->GetChildrenNodes())
		{
			if (bFirst) { FirstTags = It->GetChildrenNodes().Num(); bFirst = false; }
			if (const FXmlNode* Evs = RudeVfx::Kid(It, TEXT("EventEmitters")))
			{
				for (const FXmlNode* Ev : Evs->GetChildrenNodes())
				{
					++Events;
					const FString ER = RudeVfx::Str(Ev, TEXT("EmitterRule")).ToLower();
					const FString PR = RudeVfx::Str(Ev, TEXT("ParticleRule")).ToLower();
					if (EmiNames.Contains(ER)) { ++Resolved; } else { ++Unresolved; }
					if (ParNames.Contains(PR)) { ++Resolved; } else { ++Unresolved; }
				}
			}
			if (const FXmlNode* Kfs = RudeVfx::Kid(It, TEXT("KeyframeProps")))
			{
				for (const FXmlNode* K : Kfs->GetChildrenNodes())
				{
					++KfProps;
					const FXmlNode* List = RudeVfx::Kid(K, TEXT("Keyframes"));
					if (List && List->GetChildrenNodes().Num() > 0) { ++KfWithKeys; }
				}
			}
		}
	}

	FString ShaderJson, DomainJson;
	for (const TPair<FString, int32>& KV : Shaders)
	{
		ShaderJson += FString::Printf(TEXT("%s\"%s\":%d"), ShaderJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(KV.Key), KV.Value);
	}
	for (const TPair<FString, int32>& KV : Domains)
	{
		DomainJson += FString::Printf(TEXT("%s\"%s\":%d"), DomainJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(KV.Key), KV.Value);
	}

	const bool bOk = Unresolved == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ypt\":\"%s\",\"slot\":\"%s\",\"file\":\"%s\",\"bytes\":%lld,\"effectRules\":%d,\"emitterRules\":%d,\"particleRules\":%d,")
		TEXT("\"drawables\":%d,\"textures\":%d,\"unreferencedStrings\":%d,\"eventEmitters\":%d,\"refsResolved\":%d,\"refsUnresolved\":%d,")
		TEXT("\"keyframeProps\":%d,\"keyframePropsWithKeys\":%d,\"firstEffectFieldTags\":%d,\"shaderFiles\":{%s},\"creationDomains\":{%s}}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(YptName.TrimStartAndEnd().ToLower()), *RudeJsonEscape(Row->Slot), *RudeJsonEscape(Row->File), Bytes,
		EffDict ? EffDict->GetChildrenNodes().Num() : 0, EmiDict ? EmiDict->GetChildrenNodes().Num() : 0,
		ParDict ? ParDict->GetChildrenNodes().Num() : 0, DrwDict ? DrwDict->GetChildrenNodes().Num() : 0,
		TexDict ? TexDict->GetChildrenNodes().Num() : 0, UnrList ? UnrList->GetChildrenNodes().Num() : 0,
		Events, Resolved, Unresolved, KfProps, KfWithKeys, FirstTags, *ShaderJson, *DomainJson);
}

namespace RudeVfx
{
	// Every TOPMOST typed element beneath Container (never Container itself): descend only while
	// nothing on the path carries a type= attribute, and stop at the first that does.
	static void TopmostTyped(const FXmlNode* Container, TArray<const FXmlNode*>& Out)
	{
		if (!Container) { return; }
		for (const FXmlNode* C : Container->GetChildrenNodes())
		{
			if (!C) { continue; }
			if (!C->GetAttribute(TEXT("type")).IsEmpty()) { Out.Add(C); }
			else { TopmostTyped(C, Out); }
		}
	}

	// One pass of the MoVE tree into the flat node list. Depth-first, parent kept, so the printer can
	// re-indent it and a reader can walk it without the XML.
	static void WalkMove(const FXmlNode* Xml, URudeMoveNetwork* Net, int32 Parent, int32 Depth, const FString& Role)
	{
		if (!Xml) { return; }
		const FString Type = Xml->GetAttribute(TEXT("type"));
		const int32 Self = Net->Nodes.Num();
		FRudeMoveNode N;
		N.Index = Self;
		N.Parent = Parent;
		N.Depth = Depth;
		N.Type = Type;
		N.Role = Role;
		N.Name = Str(Xml, TEXT("Name"));
		N.bHashedName = IsHashName(N.Name);
		N.NodeIndex = Num(Xml, TEXT("NodeIndex"));
		N.InitialState = Str(Xml, TEXT("InitialState"));
		N.EntryParameterName = Str(Xml, TEXT("EntryParameterName"));
		N.ExitParameterName = Str(Xml, TEXT("ExitParameterName"));
		if (const FXmlNode* Clip = Kid(Xml, TEXT("Clip")))
		{
			N.ClipContainerType = Str(Clip, TEXT("ContainerType"));
			N.ClipContainerName = Str(Clip, TEXT("ContainerName"));
			N.ClipName = Str(Clip, TEXT("Name"));
		}
		RawMap(Xml, N.RawFields);
		Net->Nodes.Add(N);
		Net->NodeTypeCounts.FindOrAdd(Type.IsEmpty() ? FString(TEXT("(untyped)")) : Type)++;
		if (N.bHashedName) { ++Net->HashedNames; }
		else if (!N.Name.IsEmpty()) { ++Net->PlainNames; }
		if (Type == TEXT("State")) { ++Net->NumStates; }
		else if (Type == TEXT("StateMachine") || Type == TEXT("InlinedStateMachine")) { ++Net->NumStateMachines; }
		else if (Type == TEXT("Clip")) { ++Net->NumClips; }

		// The edges this node owns.
		if (const FXmlNode* Trs = Kid(Xml, TEXT("Transitions")))
		{
			for (const FXmlNode* T : Trs->GetChildrenNodes())
			{
				FRudeMoveTransition Tr;
				Tr.FromNode = Self;
				Tr.TargetState = Str(T, TEXT("TargetState"));
				Tr.Duration = Flt(T, TEXT("Duration"));
				Tr.BlendModifier = Str(T, TEXT("BlendModifier"));
				Tr.SynchronizerType = Str(T, TEXT("SynchronizerType"));
				Tr.FrameFilter = Str(T, TEXT("FrameFilter"));
				Tr.DurationParameterName = Str(T, TEXT("DurationParameterName"));
				Tr.ProgressParameterName = Str(T, TEXT("ProgressParameterName"));
				RawMap(T, Tr.RawFields);
				if (const FXmlNode* Conds = Kid(T, TEXT("Conditions")))
				{
					for (const FXmlNode* C : Conds->GetChildrenNodes())
					{
						FRudeMoveCondition Cond;
						Cond.Type = C->GetAttribute(TEXT("type"));
						const FString Bit = Str(C, TEXT("BitPosition"));
						Cond.BitPosition = Bit.IsEmpty() ? -1 : FCString::Atoi(*Bit);
						Cond.bInvert = Bl(C, TEXT("Invert"));
						RawMap(C, Cond.RawFields);
						Cond.Summary = Cond.BitPosition >= 0
							? FString::Printf(TEXT("%s bit %d%s"), *Cond.Type, Cond.BitPosition, Cond.bInvert ? TEXT(" (inverted)") : TEXT(""))
							: FString::Printf(TEXT("%s%s"), *Cond.Type, Cond.bInvert ? TEXT(" (inverted)") : TEXT(""));
						Tr.Conditions.Add(Cond);
						++Net->NumConditions;
					}
				}
				Net->Nodes[Self].Transitions.Add(Net->Transitions.Num());
				Net->Transitions.Add(Tr);
			}
		}

		// Children: the TOPMOST typed elements beneath each child element, skipping <Transitions>
		// (edges, not nodes) and <Clip> (this node's own clip reference). The descent is needed and
		// measured over the base slot's 99 .mrf files: a <States> list carries its typed <Item>s one
		// level down (3,243 of them - 2,932 State, 311 StateMachine), while <Operations> and <Children>
		// wrap each typed node inside an UNTYPED <Item> two levels down (1,810 and 1,629 such items).
		// Those are the only two shapes that HIDE a node - a child element that is itself typed
		// (InitialNode, Child0, Child1, FallbackNode) needs no descent. Stopping at the first typed
		// element is what keeps a node from being counted twice.
		for (const FXmlNode* C : Xml->GetChildrenNodes())
		{
			if (!C) { continue; }
			const FString Tag = C->GetTag();
			if (Tag == TEXT("Transitions") || Tag == TEXT("Clip")) { continue; }
			TArray<const FXmlNode*> Kids;
			if (!C->GetAttribute(TEXT("type")).IsEmpty()) { Kids.Add(C); }
			else { TopmostTyped(C, Kids); }
			for (const FXmlNode* K : Kids)
			{
				const int32 Child = Net->Nodes.Num();
				WalkMove(K, Net, Self, Depth + 1, Tag);
				Net->Nodes[Self].Children.Add(Child);
			}
		}
	}

	// Resolve every transition's TargetState against the states of the owning node's state machine.
	static void ResolveMoveTargets(URudeMoveNetwork* Net)
	{
		for (FRudeMoveTransition& T : Net->Transitions)
		{
			T.TargetNode = -1;
			if (T.TargetState.IsEmpty() || !Net->Nodes.IsValidIndex(T.FromNode)) { ++Net->TransitionsUnresolved; continue; }
			const int32 Owner = Net->Nodes[T.FromNode].Parent;
			const int32 Scope = Net->Nodes.IsValidIndex(Owner) ? Owner : T.FromNode;
			for (int32 Sib : Net->Nodes[Scope].Children)
			{
				if (Net->Nodes.IsValidIndex(Sib) && Net->Nodes[Sib].Name == T.TargetState) { T.TargetNode = Sib; break; }
			}
			if (T.TargetNode < 0)
			{
				// Fall back to the whole network, and SAY it fell back by leaving the count visible.
				for (const FRudeMoveNode& N : Net->Nodes)
				{
					if (N.Name == T.TargetState) { T.TargetNode = N.Index; break; }
				}
			}
			if (T.TargetNode >= 0) { ++Net->TransitionsResolved; } else { ++Net->TransitionsUnresolved; }
		}
	}
}

// ---- ImportMoveNetwork ---------------------------------------------------------------------
// One .mrf as a URudeMoveNetwork DataAsset: the whole node tree flattened (index, parent, depth, type,
// name, clip reference and every leaf field verbatim), every transition with its conditions, and the
// network's trigger and flag bit tables.
//
// Measured over the corpus's 162 .mrf files (maintainer lane `vfx_move`, `LAWS.md`): 42,772 elements
// carry a type= attribute in 35 kinds, of which 9,136 are transition CONDITIONS - leaving 33,636 graph
// nodes in 23 kinds, the two kind-sets disjoint. 8,000 transitions all carry the same 18 fields; 9,136
// conditions in 12 kinds; 1,050 triggers and 945 flags; 124 networks root in a StateMachine, 38 in a
// bare State. A walker that follows only directly-typed children plus <States> finds 15,415 of 33,636.
//
// ⛔ READ-ONLY TIER. No AnimBlueprint, no AnimGraph, no UE state machine is generated, and there is NO
// WRITER - nothing in RUDE emits a .mrf. A full AnimBlueprint projection is a later epic. Two measured
// facts say why the graph alone is the honest deliverable: 25,908 of the 26,565 non-empty node names in
// the corpus are hash_XXXXXXXX (the file does not spell its own identifiers), and 5,046 of the 5,264
// clip records name a clip SET rather than a clip, whose contents live outside the .mrf.
// Returns JSON: {ok, network, slot, file, sha1, nodes, states, stateMachines, clips, transitions,
// transitionsResolved, transitionsUnresolved, conditions, triggers, flags, hashedNames, plainNames,
// rootType, created, refilled, asset, tier, note, nodeTypes{}}.
FString URudeToolset::ImportMoveNetwork(const FString& CorpusRoot, const FString& NetworkName, const FString& DestFolder)
{
	TSharedPtr<FRudeCorpus> Corpus;
	const FRudeCorpusEntry* Row = nullptr;
	FString XmlPath;
	const FString Why = RudeVfx::OpenRow(CorpusRoot, TEXT("mrf"), NetworkName, Corpus, Row, XmlPath);
	if (!Why.IsEmpty()) { return RudeVfx::Fail(Why); }

	FXmlFile Doc(XmlPath, EConstructMethod::ConstructFromFile);
	if (!Doc.IsValid()) { return RudeVfx::Fail(FString::Printf(TEXT("could not parse %s: %s"), *XmlPath, *Doc.GetLastError())); }
	const FXmlNode* Root = Doc.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("MoveNetwork"))
	{
		return RudeVfx::Fail(FString::Printf(TEXT("%s root is <%s>, not <MoveNetwork>"), *XmlPath, Root ? *Root->GetTag() : TEXT("none")));
	}
	const FXmlNode* RootState = RudeVfx::Kid(Root, TEXT("RootState"));
	if (!RootState) { return RudeVfx::Fail(FString::Printf(TEXT("%s has no <RootState>"), *XmlPath)); }

	const FString Name = NetworkName.TrimStartAndEnd().ToLower();
	const FString AssetName = RudeVfx::SafeName(Name);
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game/RUDE/MoVE")) : DestFolder.TrimStartAndEnd();
	const FString PkgName = Dest / AssetName;
	if (!FPackageName::IsValidLongPackageName(PkgName)) { return RudeVfx::Fail(FString::Printf(TEXT("'%s' is not a valid package path"), *PkgName)); }

	URudeMoveNetwork* Net = LoadObject<URudeMoveNetwork>(nullptr, *(PkgName + TEXT(".") + AssetName));
	UPackage* Pkg = nullptr;
	if (Net) { Pkg = Net->GetOutermost(); }
	else { Pkg = CreatePackage(*PkgName); }
	bool bNew = false;
	if (!Net) { Net = NewObject<URudeMoveNetwork>(Pkg, FName(*AssetName), RF_Public | RF_Standalone); bNew = true; }

	Net->NetworkName = Name;
	Net->Tier = TEXT("read-only");
	Net->TierNote = TEXT("READ-ONLY TIER: the network as measured, not a projection. No AnimBlueprint or AnimGraph is generated and no .mrf writer exists.");
	Net->CorpusRoot = Corpus->GetRoot();
	Net->SourceSlot = Row->Slot;
	Net->SourceFile = Row->File;
	Net->SourceSha1 = Row->Sha1;
	Net->Nodes.Empty();
	Net->Transitions.Empty();
	Net->Triggers.Empty();
	Net->Flags.Empty();
	Net->NodeTypeCounts.Empty();
	Net->NumStates = 0;
	Net->NumStateMachines = 0;
	Net->NumClips = 0;
	Net->NumConditions = 0;
	Net->HashedNames = 0;
	Net->PlainNames = 0;
	Net->TransitionsResolved = 0;
	Net->TransitionsUnresolved = 0;
	Net->RootType = RootState->GetAttribute(TEXT("type"));

	for (int32 Which = 0; Which < 2; ++Which)
	{
		const FXmlNode* List = RudeVfx::Kid(Root, Which == 0 ? TEXT("MoveNetworkTriggers") : TEXT("MoveNetworkFlags"));
		if (!List) { continue; }
		for (const FXmlNode* It : List->GetChildrenNodes())
		{
			FRudeMoveNamedBit B;
			B.Name = RudeVfx::Str(It, TEXT("Name"));
			B.BitPosition = RudeVfx::Num(It, TEXT("BitPosition"));
			B.bHashedName = RudeVfx::IsHashName(B.Name);
			if (Which == 0) { Net->Triggers.Add(B); } else { Net->Flags.Add(B); }
		}
	}

	RudeVfx::WalkMove(RootState, Net, -1, 0, TEXT("RootState"));
	RudeVfx::ResolveMoveTargets(Net);

	FString TypeJson;
	for (const TPair<FString, int32>& KV : Net->NodeTypeCounts)
	{
		TypeJson += FString::Printf(TEXT("%s\"%s\":%d"), TypeJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(KV.Key), KV.Value);
	}

	Pkg->MarkPackageDirty();
	if (bNew) { FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().AssetCreated(Net); }

	const bool bOk = Net->Nodes.Num() > 0 && Net->TransitionsUnresolved == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"network\":\"%s\",\"slot\":\"%s\",\"file\":\"%s\",\"sha1\":\"%s\",\"nodes\":%d,\"states\":%d,\"stateMachines\":%d,\"clips\":%d,")
		TEXT("\"transitions\":%d,\"transitionsResolved\":%d,\"transitionsUnresolved\":%d,\"conditions\":%d,\"triggers\":%d,\"flags\":%d,\"hashedNames\":%d,")
		TEXT("\"plainNames\":%d,\"rootType\":\"%s\",\"created\":%d,\"refilled\":%d,\"asset\":\"%s\",\"tier\":\"read-only\",\"note\":\"%s\",\"nodeTypes\":{%s}}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), *RudeJsonEscape(Row->Slot), *RudeJsonEscape(Row->File), *RudeJsonEscape(Row->Sha1),
		Net->Nodes.Num(), Net->NumStates, Net->NumStateMachines, Net->NumClips, Net->Transitions.Num(),
		Net->TransitionsResolved, Net->TransitionsUnresolved, Net->NumConditions, Net->Triggers.Num(), Net->Flags.Num(),
		Net->HashedNames, Net->PlainNames, *RudeJsonEscape(Net->RootType), bNew ? 1 : 0, bNew ? 0 : 1,
		*RudeJsonEscape(PkgName + TEXT(".") + AssetName), *RudeJsonEscape(Net->TierNote), *TypeJson);
}

// ---- PrintMoveNetwork ----------------------------------------------------------------------
// The graph of an imported URudeMoveNetwork as indented text, so it can be READ headlessly - the whole
// point of the read-only tier. Each node prints as "<depth indent><type> <name> [role]" with its clip
// reference when it has one, and each transition as "-> <target> <duration>s <blend> <sync> if
// <conditions>". MaxLines caps the output (default 200; "0" means every line) and the verdict says
// whether it truncated. The text also goes to the log, so a -script= run shows it without a viewer.
// ⛔ Read-only: this prints, it does not build an AnimBlueprint and there is no .mrf writer.
// Returns JSON: {ok, network, nodes, transitions, lines, printed, truncated, text}.
FString URudeToolset::PrintMoveNetwork(const FString& NetworkAssetPath, const FString& MaxLines)
{
	const FString Path = NetworkAssetPath.TrimStartAndEnd();
	if (Path.IsEmpty()) { return RudeVfx::Fail(TEXT("NetworkAssetPath is empty (a URudeMoveNetwork built by ImportMoveNetwork)")); }
	URudeMoveNetwork* Net = LoadObject<URudeMoveNetwork>(nullptr, *Path);
	if (!Net) { return RudeVfx::Fail(FString::Printf(TEXT("no URudeMoveNetwork at '%s'"), *Path)); }

	const FString CapText = MaxLines.TrimStartAndEnd();
	const int32 Cap = CapText.IsEmpty() ? 200 : FCString::Atoi(*CapText);
	const bool bAll = (Cap <= 0);

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("MoVE network '%s' (%s) - READ-ONLY view, no AnimBlueprint is generated"), *Net->NetworkName, *Net->RootType));
	Lines.Add(FString::Printf(TEXT("  %d nodes, %d transitions, %d conditions, %d triggers, %d flags, %d/%d names hashed"),
		Net->Nodes.Num(), Net->Transitions.Num(), Net->NumConditions, Net->Triggers.Num(), Net->Flags.Num(),
		Net->HashedNames, Net->HashedNames + Net->PlainNames));

	for (const FRudeMoveNode& N : Net->Nodes)
	{
		FString Pad;
		for (int32 i = 0; i < N.Depth; ++i) { Pad += TEXT("  "); }
		FString Line = FString::Printf(TEXT("%s[%d] %s %s"), *Pad, N.Index, *N.Type, N.Name.IsEmpty() ? TEXT("(unnamed)") : *N.Name);
		if (!N.Role.IsEmpty()) { Line += FString::Printf(TEXT("  <%s>"), *N.Role); }
		if (!N.ClipContainerType.IsEmpty())
		{
			Line += FString::Printf(TEXT("  clip=%s/%s:%s"), *N.ClipContainerType, *N.ClipContainerName, *N.ClipName);
		}
		if (!N.InitialState.IsEmpty()) { Line += FString::Printf(TEXT("  initial=%s"), *N.InitialState); }
		Lines.Add(Line);
		for (int32 Ti : N.Transitions)
		{
			if (!Net->Transitions.IsValidIndex(Ti)) { continue; }
			const FRudeMoveTransition& T = Net->Transitions[Ti];
			FString Conds;
			for (const FRudeMoveCondition& C : T.Conditions)
			{
				Conds += (Conds.IsEmpty() ? TEXT("") : TEXT(" AND ")) + C.Summary;
			}
			FString TargetTag;
			if (T.TargetNode >= 0) { TargetTag = FString::Printf(TEXT(" [%d]"), T.TargetNode); }
			else { TargetTag = TEXT(" [UNRESOLVED]"); }
			Lines.Add(FString::Printf(TEXT("%s   -> %s%s  %.3fs %s %s%s%s"),
				*Pad, *T.TargetState, *TargetTag,
				T.Duration, *T.BlendModifier, *T.SynchronizerType,
				Conds.IsEmpty() ? TEXT("") : TEXT("  if "), *Conds));
		}
	}

	const int32 Printed = bAll ? Lines.Num() : FMath::Min(Cap, Lines.Num());
	FString Text;
	for (int32 i = 0; i < Printed; ++i) { Text += Lines[i] + TEXT("\n"); }
	if (Printed < Lines.Num()) { Text += FString::Printf(TEXT("... %d more lines (raise MaxLines, or pass 0 for all)\n"), Lines.Num() - Printed); }
	UE_LOG(LogTemp, Display, TEXT("%s"), *Text);

	const bool bOk = Net->Nodes.Num() > 0 && Printed > 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"network\":\"%s\",\"nodes\":%d,\"transitions\":%d,\"lines\":%d,\"printed\":%d,\"truncated\":%s,\"text\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Net->NetworkName), Net->Nodes.Num(), Net->Transitions.Num(),
		Lines.Num(), Printed, Printed < Lines.Num() ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Text));
}
