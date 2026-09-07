// RUDE - RAGE <-> Unreal Development Environment
//
// THE WEAPON BENCH (v1). A GTA V weapon is not one drawable either, but it fails differently from a
// vehicle: the weapon's ydr is the gun with EMPTY sockets, and the magazine, scope, suppressor,
// flashlight and grip are separate ydrs the weapon's meta row names. Import a weapon through the
// ordinary drawable lane and you get a rifle with no magazine - and nothing tells you a magazine was
// ever supposed to be there, because the drawable does not mention one. The join lives in the metas.
//
// THE JOIN, MEASURED 2026-09-06 over the whole weapon set of a corpus cut from a legally owned copy of
// the game (875 w_* drawables, 184 CWeaponInfo rows, 474 component rows; maintainer lane `weapons`
// (`LAWS.md`) carries every number with its denominator):
//   * The WEAPON's skeleton carries the sockets, named WAP*: WAPClip on 185 drawables, WAPFlshLasr 163,
//     WAPSupp 151, WAPScop 115, WAPGrip 76, WAPScop_2 69, WAPSupp_2 44, WAPCover/WAPBarrel 17 each.
//   * The COMPONENT's drawable carries ONE bone named AAP*, and it is that drawable's FIRST bone with
//     parent -1 in 516/516 - identity rotation and zero translation in 516/516. The component's
//     geometry is authored AT its attach frame's origin, so the mesh needs NO correction: it drops
//     straight onto the weapon's socket frame. (⛔ do NOT copy the ped-prop rule here, which inverts
//     the anchor bone's bind rotation - a prop is modelled in ped axes, a weapon component is not.)
//   * 198 of the 875 drawables carry WAP bones and 516 carry AAP bones; NOT ONE carries both. The two
//     prefixes are the two halves of one join.
//   * The meta says which AAP goes on which WAP: 402 component references sit at a WAP bone and the
//     component's own <AttachBone> stem prefixes the socket's stem in 377/402 (WAPClip<-AAPClip,
//     WAPSupp/WAPSupp_2<-AAPSupp, WAPFlshLasr<-AAPFlsh, WAPScop/WAPScop_2<-AAPScop, WAPGrip<-AAPGrip,
//     WAPBarrel<-AAPBarrel). The 25 that do not are real data, not noise: WAPScop_2<-AAPCamo2 (22),
//     WAPScop<-AAPFlsh (2), WAPFlshLasr<-AAPCover (1). ⛔ So the socket is taken FROM THE META, never
//     derived from the component's bone name - the name rule would place 25 references wrong.
//   * A further 133 references sit at `gun_root` (AAPCamo skins) and 12 at `gun_gripr`: a socket does
//     not have to be a WAP bone, and both of those ARE on the weapon skeleton.
//   * 16 of 272 attach points name a bone the weapon's own skeleton does not have (every one WAPClip,
//     on shotguns, launchers and the musket). Those components are placed at the weapon's origin and
//     COUNTED (componentsUnmapped) - never silently dropped, never silently placed.
//   * Bone scale is unit in 4,171/4,171 bones of the whole set, so no bone frame is degenerate; the WAP
//     sockets hang mid-chain (249 of 317 on bone index 2, Gun_Main_Bone), so the frame MUST be composed
//     up the parent chain exactly as a vehicle's wheel bone is. Reading the local translation alone is
//     wrong on nearly every weapon.
//   * Weapons ship NO lod groups INSIDE a file: 875/875 drawables carry <DrawableModelsHigh> and
//     NOTHING else, and 0/875 carry lights. The detail toggle is a SECOND FILE - 204 of the 875 have
//     a <name>_hi twin with the same skeleton and more vertices in 194/204 (w_ar_carbinerifle 3,961 ->
//     20,251; w_pi_pistol 1,071 -> 3,748) - so ImportWithHiLod shows the _hi and hangs the base
//     drawable off it as LOD1, which is the vehicle lane's rule applied to a file pair rather than to
//     lod groups. ⛔ Do NOT read the base file's counts off the placed mesh: the verdict reports the
//     BASE numbers, which is what the offline comparator recomputes from <model>.ydr.xml.
//   * Textures: 5,274 sampler references over the set resolve 2,806 in the drawable's OWN ytd (the
//     dictionary of the same name), 234 embedded in the ydr itself, 1,859 in ANOTHER weapon dictionary,
//     and 375 nowhere in the weapon set (339 env_smooth_concrete2 + 2 env_noise_heavy, which live in map
//     dictionaries, and 34 givemechecker, which exists in no dictionary anywhere - the engine's
//     placeholder). gtxd.ymt has ZERO w_ rows, so a weapon does NOT ride the map's texture-parent chain:
//     the scope below is the drawable's own dictionary plus the other dictionaries OF THIS COMPOSITE.
//     ⚠ Measured yield of that second tier on the composite set: 0 of 152 unresolved references, whose
//     remainder is 137 env_smooth_concrete2 + 15 givemechecker. It is carried because the weapon
//     dictionaries are one flat namespace, not because it rescued anything here.
// ⚠ This corpus (2026-09-04 export) carries NO pixel sidecars for weapon ytds (0 of 804 dictionaries,
//   any copy), so textures cannot bind until ROUT exports them. texturesMissing is therefore expected to
//   be non-zero and NEVER gates ok - the same state the vehicle lane is in.
#include "RudeToolset.h"
#include "RudeCorpus.h"
#include "RudeToolsetInternal.h"
#include "RudeWeaponAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "StaticMeshAttributes.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/Function.h"
#include "UObject/Package.h"
#include "XmlFile.h"

namespace RudeWeaponLane
{
	// GTA metres -> UE centimetres with the pinned Y mirror, and the plain quaternion mirror for a bone
	// frame. Identical to the vehicle lane's map and for the identical reason: a bone <Rotation> is a
	// FORWARD orientation (the ymap inverse-stored rule is a CEntityDef property and does not apply
	// here). See RudeVehicle.cpp's GtaToUe note for the measurement that settled it. Duplicated rather
	// than shared because that one is file-local to the vehicle lane; if a third lane needs it, lift it
	// into RudeToolsetInternal.h then - two copies is not yet a header.
	static FTransform GtaToUe(const FTransform& G)
	{
		const FQuat Q = G.GetRotation();
		const FVector T = G.GetTranslation();
		return FTransform(FQuat(-Q.X, Q.Y, -Q.Z, Q.W).GetNormalized(),
			FVector(T.X * 100.0, -T.Y * 100.0, T.Z * 100.0),
			G.GetScale3D());
	}

	struct FBone
	{
		FString Name;
		int32 Tag = -1;
		int32 Parent = -1;
		FTransform LocalGta = FTransform::Identity;
	};

	static FString Attr(const FXmlNode* N, const TCHAR* Key, const TCHAR* Def)
	{
		if (!N) { return Def; }
		const FString V = N->GetAttribute(Key);
		return V.IsEmpty() ? FString(Def) : V;
	}

	static FVector Vec3(const FXmlNode* N, const FVector& Def)
	{
		if (!N) { return Def; }
		return FVector(FCString::Atod(*Attr(N, TEXT("x"), TEXT("0"))),
			FCString::Atod(*Attr(N, TEXT("y"), TEXT("0"))),
			FCString::Atod(*Attr(N, TEXT("z"), TEXT("0"))));
	}

	// ⛔ A meta field is spelled TWO ways in the same file and reading only one is a silent lie: the
	// game writes <AttachBone>WAPClip</AttachBone> as TEXT but <Default value="true" /> as an ATTRIBUTE.
	// Reading <Default> with GetContent() alone returns "" for every component in the game, so every
	// component would import HIDDEN and the weapon would come in bare with nothing reporting a problem.
	// (Caught by the comparator before this ever ran: it read the same node the same wrong way and
	// counted 0 defaults on a pistol whose meta declares one.) Value attribute first, text second.
	static FString NodeValue(const FXmlNode* N)
	{
		if (!N) { return FString(); }
		const FString V = N->GetAttribute(TEXT("value"));
		return V.IsEmpty() ? N->GetContent().TrimStartAndEnd() : V.TrimStartAndEnd();
	}

	static FString JsonEscape(const FString& In)
	{
		return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	}

	static FString JsonStrings(const TArray<FString>& In, int32 Cap = 0)
	{
		FString O;
		for (int32 i = 0; i < In.Num() && (Cap == 0 || i < Cap); ++i)
		{
			O += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *JsonEscape(In[i]));
		}
		return O;
	}

	static TSharedPtr<FJsonObject> ParseVerdict(const FString& Verdict)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Verdict);
		if (!FJsonSerializer::Deserialize(Reader, Obj)) { return nullptr; }
		return Obj;
	}

	static int32 JsonInt(const FString& Verdict, const TCHAR* Field, int32 Fallback)
	{
		const TSharedPtr<FJsonObject> Obj = ParseVerdict(Verdict);
		double D = 0;
		return (Obj.IsValid() && Obj->TryGetNumberField(Field, D)) ? (int32)D : Fallback;
	}

	// A drawable's <Skeleton><Bones>: every bone with its LOCAL frame, tag and parent. A degenerate
	// <Scale> refuses (unit in 4,171/4,171 measured bones - a zero component is a lost attribute, and a
	// collapsed frame would place a component as an invisible sliver). No <Skeleton> is not an error:
	// 7 of 875 weapon-set drawables have none and 464 have exactly one bone.
	static bool ParseSkeleton(const FXmlNode* DrawableNode, TArray<FBone>& Bones, FString& Why)
	{
		Bones.Reset();
		const FXmlNode* Skel = DrawableNode ? DrawableNode->FindChildNode(TEXT("Skeleton")) : nullptr;
		const FXmlNode* BoneList = Skel ? Skel->FindChildNode(TEXT("Bones")) : nullptr;
		if (!BoneList) { return true; }
		for (const FXmlNode* It : BoneList->GetChildrenNodes())
		{
			FBone B;
			if (const FXmlNode* N = It->FindChildNode(TEXT("Name")))
			{
				B.Name = N->GetContent().TrimStartAndEnd();
			}
			B.Tag = FCString::Atoi(*Attr(It->FindChildNode(TEXT("Tag")), TEXT("value"), TEXT("-1")));
			B.Parent = FCString::Atoi(*Attr(It->FindChildNode(TEXT("ParentIndex")), TEXT("value"), TEXT("-1")));
			const FXmlNode* R = It->FindChildNode(TEXT("Rotation"));
			const FQuat Q(FCString::Atod(*Attr(R, TEXT("x"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("y"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("z"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("w"), TEXT("1"))));
			const FVector S = Vec3(It->FindChildNode(TEXT("Scale")), FVector::OneVector);
			if (S.GetAbsMin() < UE_KINDA_SMALL_NUMBER)
			{
				Why = FString::Printf(TEXT("bone '%s' has a degenerate <Scale> (%s) - refusing rather "
					"than placing components on a collapsed frame"), *B.Name, *S.ToString());
				return false;
			}
			B.LocalGta = FTransform(Q.GetNormalized(),
				Vec3(It->FindChildNode(TEXT("Translation")), FVector::ZeroVector), S);
			Bones.Add(MoveTemp(B));
		}
		return true;
	}

	// Compose each bone's MODEL-space frame up its parent chain (memoised, refusing on a cycle). 249 of
	// 317 sockets hang off bone index 2, so the composition is the whole point.
	static bool ResolveBoneWorld(const TArray<FBone>& Bones, TArray<FTransform>& WorldGta, FString& Why)
	{
		TArray<uint8> State;
		WorldGta.SetNum(Bones.Num());
		State.SetNumZeroed(Bones.Num());
		TFunction<bool(int32)> Resolve = [&](int32 i) -> bool
		{
			if (!Bones.IsValidIndex(i)) { return false; }
			if (State[i] == 2) { return true; }
			if (State[i] == 1) { return false; }
			State[i] = 1;
			const int32 P = Bones[i].Parent;
			if (P < 0 || !Bones.IsValidIndex(P)) { WorldGta[i] = Bones[i].LocalGta; }
			else
			{
				if (!Resolve(P)) { return false; }
				WorldGta[i] = Bones[i].LocalGta * WorldGta[P];
			}
			State[i] = 2;
			return true;
		};
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			if (!Resolve(i))
			{
				Why = FString::Printf(TEXT("bone %d (%s) sits in a parent CYCLE - refusing rather than "
					"placing components off a half-composed frame"), i, *Bones[i].Name);
				return false;
			}
		}
		return true;
	}

	// Every field of a meta item BY NAME as spelled: the "value" attribute, else the attributes joined,
	// else the text. Subtrees flatten with '/', repeated tags and list items index [n]. Same shape as the
	// vehicle lane's, minus its PSO hash-tag resolution: the weapon metas are plain XML (89 distinct file
	// names, 91 of 184 weapon rows in weapons.meta itself, all 474 component rows in weaponcomponents.meta).
	static void FlattenItem(const FXmlNode* N, const FString& Prefix, TMap<FString, FString>& Out, int32 Depth)
	{
		if (!N || Depth > 6) { return; }
		TMap<FString, int32> Total, Seen;
		for (const FXmlNode* C : N->GetChildrenNodes()) { ++Total.FindOrAdd(C->GetTag()); }
		for (const FXmlNode* C : N->GetChildrenNodes())
		{
			const FString Tag = C->GetTag();
			int32& Nth = Seen.FindOrAdd(Tag);
			const bool bIndexed = Tag == TEXT("Item") || Total[Tag] > 1;
			const FString Key = Prefix + (bIndexed ? FString::Printf(TEXT("%s[%d]"), *Tag, Nth) : Tag);
			++Nth;
			if (C->GetChildrenNodes().Num() == 0)
			{
				FString V = C->GetAttribute(TEXT("value"));
				if (V.IsEmpty() && C->GetAttributes().Num() > 0)
				{
					for (const FXmlAttribute& A : C->GetAttributes())
					{
						V += (V.IsEmpty() ? TEXT("") : TEXT(" ")) + A.GetTag() + TEXT("=") + A.GetValue();
					}
				}
				if (V.IsEmpty()) { V = C->GetContent().TrimStartAndEnd(); }
				Out.Add(Key, V);
			}
			else
			{
				const FString ItemType = C->GetAttribute(TEXT("type"));
				if (!ItemType.IsEmpty()) { Out.Add(Key + TEXT("@type"), ItemType); }
				FlattenItem(C, Key + TEXT("/"), Out, Depth + 1);
			}
		}
	}

	struct FMetaHit
	{
		bool bFound = false;
		int32 SlotRank = -1;
		FString Path, Slot, Type, Xml;
		TMap<FString, FString> Fields;
	};

	// Every ledger name of a weapon metadata file. The weapon rows are NOT all in weapons.meta: 89
	// distinct file names carry CWeaponInfo items (weapons.meta 91 rows, then one file per DLC weapon -
	// weaponrevolver.meta, weaponspecialcarbine.meta, weapons_arena.meta, ...). Prefix "weapon" catches
	// every one of them plus weaponcomponents.meta and weaponarchetypes.meta.
	static void WeaponMetaNames(const FRudeCorpus& Corpus, TArray<FString>& Out)
	{
		TArray<const FRudeCorpusEntry*> Rows;
		Corpus.ByPrefix(TEXT("meta"), TEXT("weapon"), Rows);
		for (const FRudeCorpusEntry* E : Rows) { Out.AddUnique(E->Name); }
	}

	// Walk an item tree calling Visit on every <Item> that carries a type attribute.
	static void ForEachItem(const FXmlNode* N, TFunctionRef<void(const FXmlNode*, const FString&)> Visit)
	{
		if (!N) { return; }
		if (N->GetTag() == TEXT("Item"))
		{
			const FString T = N->GetAttribute(TEXT("type"));
			if (!T.IsEmpty()) { Visit(N, T); }
		}
		for (const FXmlNode* C : N->GetChildrenNodes()) { ForEachItem(C, Visit); }
	}

	// The CWeaponInfo item whose <Name> or <Model> equals Want (case-insensitive), searched over EVERY
	// copy of every weapon meta lowest slot first, so the copy the game loads last wins. A file is parsed
	// only when its text contains Want at all - that is what keeps 130-odd files cheap.
	static FMetaHit FindWeaponInfo(const FRudeCorpus& Corpus, const FString& Want, int32& Searched)
	{
		FMetaHit Hit;
		if (Want.IsEmpty()) { return Hit; }
		TArray<FString> Names;
		WeaponMetaNames(Corpus, Names);
		for (const FString& MetaName : Names)
		{
			for (const FRudeCorpusEntry* E : Corpus.History(TEXT("meta"), MetaName))
			{
				const FString Path = Corpus.PathOf(*E);
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *Path)) { continue; }
				++Searched;
				if (!Text.Contains(Want, ESearchCase::IgnoreCase)) { continue; }
				FXmlFile Doc(Text, EConstructMethod::ConstructFromBuffer);
				if (!Doc.IsValid()) { continue; }
				ForEachItem(Doc.GetRootNode(), [&](const FXmlNode* Item, const FString& ItemType)
				{
					if (ItemType != TEXT("CWeaponInfo")) { return; }
					const bool bMatch =
						NodeValue(Item->FindChildNode(TEXT("Name"))).Equals(Want, ESearchCase::IgnoreCase)
						|| NodeValue(Item->FindChildNode(TEXT("Model"))).Equals(Want, ESearchCase::IgnoreCase);
					if (!bMatch) { return; }
					Hit.bFound = true;
					Hit.SlotRank = E->SlotRank;
					Hit.Path = Path;
					Hit.Slot = E->Slot;
					Hit.Type = ItemType;
					Hit.Xml.Reset();
					RudeXmlNodeToString(Item, Hit.Xml, 0);
					Hit.Fields.Reset();
					FlattenItem(Item, TEXT(""), Hit.Fields, 0);
				});
			}
		}
		return Hit;
	}

	// Every CWeaponComponent*Info item named in Wanted, over the same file set and the same
	// lowest-slot-first rule (a later copy overwrites an earlier one). One pass, so a rifle with 11
	// components costs the same file reads as one with 1.
	static void FindComponentInfos(const FRudeCorpus& Corpus, const TSet<FString>& Wanted,
		TMap<FString, FMetaHit>& Out, int32& Searched)
	{
		if (Wanted.Num() == 0) { return; }
		TArray<FString> Names;
		WeaponMetaNames(Corpus, Names);
		for (const FString& MetaName : Names)
		{
			for (const FRudeCorpusEntry* E : Corpus.History(TEXT("meta"), MetaName))
			{
				const FString Path = Corpus.PathOf(*E);
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *Path)) { continue; }
				bool bAny = false;
				for (const FString& W : Wanted)
				{
					if (Text.Contains(W, ESearchCase::IgnoreCase)) { bAny = true; break; }
				}
				if (!bAny) { continue; }
				++Searched;
				FXmlFile Doc(Text, EConstructMethod::ConstructFromBuffer);
				if (!Doc.IsValid()) { continue; }
				ForEachItem(Doc.GetRootNode(), [&](const FXmlNode* Item, const FString& ItemType)
				{
					if (!ItemType.StartsWith(TEXT("CWeaponComponent"))) { return; }
					const FString CompName = NodeValue(Item->FindChildNode(TEXT("Name")));
					if (CompName.IsEmpty() || !Wanted.Contains(CompName.ToUpper())) { return; }
					FMetaHit& H = Out.FindOrAdd(CompName.ToUpper());
					H.bFound = true;
					H.SlotRank = E->SlotRank;
					H.Path = Path;
					H.Slot = E->Slot;
					H.Type = ItemType;
					H.Xml.Reset();
					RudeXmlNodeToString(Item, H.Xml, 0);
					H.Fields.Reset();
					FlattenItem(Item, TEXT(""), H.Fields, 0);
				});
			}
		}
	}

	// A re-spelled item back into a walkable tree. RudeXmlNodeToString emits the item alone, which is a
	// well-formed document with one root once a declaration is prepended.
	static bool ReparseItem(const FString& Xml, TUniquePtr<FXmlFile>& Doc, const FXmlNode*& OutRoot)
	{
		if (Xml.IsEmpty()) { return false; }
		const FString Buffer = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") + Xml;
		Doc = MakeUnique<FXmlFile>(Buffer, EConstructMethod::ConstructFromBuffer);
		OutRoot = Doc->IsValid() ? Doc->GetRootNode() : nullptr;
		return OutRoot != nullptr;
	}

	// Import <name>.ydr.xml as <DestFolder>/<name> unless the package is already there, then hand back the
	// mesh - the same skip-if-exists idempotence the vehicle lane uses. Scoped so the drawable importer
	// binds textures by the measured weapon rule.
	static UStaticMesh* ImportOrLoadScoped(const FString& XmlPath, const FString& MeshName,
		const FString& DestFolder, const FRudeTextureScope* Scope, FString& OutAssetPath, FString& OutVerdict)
	{
		OutAssetPath = DestFolder / MeshName;
		if (!FPackageName::DoesPackageExist(OutAssetPath))
		{
			OutVerdict = RudeImportYdrScoped(XmlPath, DestFolder, Scope);
			const TSharedPtr<FJsonObject> Obj = ParseVerdict(OutVerdict);
			FString Reported;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("assetPath"), Reported) && !Reported.IsEmpty())
			{
				OutAssetPath = Reported;
			}
			else if (!OutVerdict.Contains(TEXT("\"ok\":true"))) { return nullptr; }
		}
		return LoadObject<UStaticMesh>(nullptr, *OutAssetPath);
	}

	// Copy Src's LOD0 geometry in as Dst's LOD<LodIndex>: its material slots are appended as lod<N>__<slot>
	// (a fresh name per LOD - the two files' shader lists differ), the polygon groups are re-pointed at
	// those names, and the section map pins each section to its material index so the build cannot fall
	// back to "polygon group ordinal = material index". Lifted from the vehicle lane's AppendLod
	// (RudeVehicle.cpp, 2026-09-06) unchanged in behaviour - the two lanes solve the same problem.
	static bool AppendLod(UStaticMesh* Dst, UStaticMesh* Src, int32 LodIndex, FString& Why)
	{
		const FMeshDescription* SrcDesc = Src ? Src->GetMeshDescription(0) : nullptr;
		if (!SrcDesc) { Why = TEXT("LOD source has no mesh description"); return false; }
		TArray<FStaticMaterial> Mats = Dst->GetStaticMaterials();
		TMap<FName, FName> Rename;
		for (const FStaticMaterial& M : Src->GetStaticMaterials())
		{
			const FName NewSlot(*FString::Printf(TEXT("lod%d__%s"), LodIndex, *M.MaterialSlotName.ToString()));
			Rename.Add(M.MaterialSlotName, NewSlot);
			Mats.Add(FStaticMaterial(M.MaterialInterface, NewSlot, NewSlot));
		}
		Dst->SetStaticMaterials(Mats);
		FMeshDescription Copy = *SrcDesc;
		{
			FStaticMeshAttributes A(Copy);
			TPolygonGroupAttributesRef<FName> Slots = A.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID G : Copy.PolygonGroups().GetElementIDs())
			{
				if (const FName* N = Rename.Find(Slots[G])) { Slots[G] = *N; }
			}
		}
		while (Dst->GetNumSourceModels() <= LodIndex) { Dst->AddSourceModel(); }
		FMeshDescription* Written = Dst->CreateMeshDescription(LodIndex, MoveTemp(Copy));
		if (!Written) { Why = TEXT("CreateMeshDescription failed"); return false; }
		Dst->CommitMeshDescription(LodIndex);
		FStaticMeshSourceModel& SM = Dst->GetSourceModel(LodIndex);
		SM.ReductionSettings.PercentTriangles = 1.f;
		SM.ReductionSettings.PercentVertices = 1.f;
		SM.BuildSettings.bRecomputeNormals = false;
		SM.BuildSettings.bRecomputeTangents = false;
		{
			FStaticMeshConstAttributes A(*Written);
			TPolygonGroupAttributesConstRef<FName> Slots = A.GetPolygonGroupMaterialSlotNames();
			int32 Section = 0;
			for (const FPolygonGroupID G : Written->PolygonGroups().GetElementIDs())
			{
				int32 MatIdx = INDEX_NONE;
				for (int32 i = 0; i < Mats.Num(); ++i) { if (Mats[i].MaterialSlotName == Slots[G]) { MatIdx = i; break; } }
				if (MatIdx != INDEX_NONE) { Dst->GetSectionInfoMap().Set(LodIndex, Section, FMeshSectionInfo(MatIdx)); }
				++Section;
			}
		}
		return true;
	}

	// Import <Name>.ydr and, when the corpus also has <Name>_hi, make the _hi mesh the one the actor shows
	// with the base drawable as its LOD1 - the vehicle lane's "detail toggle = LOD", applied to the weapon
	// lane's own file pair rather than to lod groups inside one file (weapons have none: 875/875 High only).
	// MEASURED 2026-09-06: 204 of the 875 weapon-set drawables have a _hi twin, the _hi carries MORE
	// vertices in 194/204 (w_ar_carbinerifle 3,961 -> 20,251, w_pi_pistol 1,071 -> 3,748), and the twin
	// carries the SAME skeleton (w_pi_pistol / w_pi_pistol_hi: the same 12 bones in the same order), so the
	// socket frames read off the base file place the components correctly on either mesh.
	// The BASE numbers are what the verdict reports and what the offline comparator recomputes; the _hi is
	// LOD0 for the eye. A failure here is counted and named, never fatal: the base mesh still stands.
	static UStaticMesh* ImportWithHiLod(const FRudeCorpus& Corpus, const FString& Name, const FString& DestFolder,
		const FRudeTextureScope* Scope, FString& OutBaseAssetPath, FString& OutBaseVerdict,
		bool& bOutHi, int32& OutLodFailed, TArray<FString>& Missing)
	{
		const FRudeCorpusEntry* BaseRow = Corpus.Effective(TEXT("ydr"), Name);
		if (!BaseRow) { return nullptr; }
		UStaticMesh* Base = ImportOrLoadScoped(Corpus.PathOf(*BaseRow), Name, DestFolder, Scope,
			OutBaseAssetPath, OutBaseVerdict);
		if (!Base) { return nullptr; }
		const FRudeCorpusEntry* HiRow = Corpus.Effective(TEXT("ydr"), Name + TEXT("_hi"));
		if (!HiRow) { return Base; }
		FString HiPath, HiVerdict;
		UStaticMesh* Hi = ImportOrLoadScoped(Corpus.PathOf(*HiRow), Name + TEXT("_hi"), DestFolder, Scope,
			HiPath, HiVerdict);
		if (!Hi)
		{
			++OutLodFailed;
			Missing.Add(FString::Printf(TEXT("%s_hi exists in the corpus but did not import (%s) - showing "
				"the base drawable"), *Name, *(HiVerdict.IsEmpty() ? FString(TEXT("asset did not load")) : HiVerdict)));
			return Base;
		}
		// Rebuild the LOD stack every run so a re-run is idempotent rather than stacking lod slots.
		Hi->SetNumSourceModels(1);
		{
			TArray<FStaticMaterial> Mats = Hi->GetStaticMaterials();
			Mats.RemoveAll([](const FStaticMaterial& M) { return M.MaterialSlotName.ToString().StartsWith(TEXT("lod")); });
			Hi->SetStaticMaterials(Mats);
		}
		FString LodWhy;
		if (!AppendLod(Hi, Base, 1, LodWhy))
		{
			++OutLodFailed;
			Missing.Add(FString::Printf(TEXT("%s: the base drawable did not attach as LOD1 of %s_hi (%s)"),
				*Name, *Name, *LodWhy));
		}
		Hi->Build(true);
		Hi->PostEditChange();
		Hi->MarkPackageDirty();
		bOutHi = true;
		return Hi;
	}

	static AActor* FindWeaponActor(UWorld* World, const FString& Label)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(FName(TEXT("RUDE_WEAPON_ROOT")))
				&& It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

FString URudeToolset::ImportWeapon(const FString& CorpusRoot, const FString& WeaponName,
                                   const FString& DestFolder)
{
	using namespace RudeWeaponLane;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Why));
	};

	// ---- 0) the corpus, then the weapon's meta row ------------------------------------------------
	FString Want = WeaponName.TrimStartAndEnd();
	Want.RemoveFromEnd(TEXT(".xml"));
	Want.RemoveFromEnd(TEXT(".ydr"));
	if (Want.IsEmpty()) { return Fail(TEXT("give a weapon name - the model (w_pi_pistol) or the meta name (WEAPON_PISTOL)")); }
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::LooksLikeCorpus(CorpusRoot)
		? FRudeCorpus::Open(CorpusRoot, CorpusErr) : nullptr;
	if (!Corpus.IsValid())
	{
		return Fail(FString::Printf(TEXT("CorpusRoot must be a filebase (the folder holding _FILEBASE.json "
			"and _PROVENANCE.jsonl) - the component join needs the ledger: %s"), *CorpusErr));
	}

	int32 MetaFilesSearched = 0;
	const FMetaHit WeaponHit = FindWeaponInfo(*Corpus, Want, MetaFilesSearched);
	if (!WeaponHit.bFound)
	{
		return Fail(FString::Printf(TEXT("no CWeaponInfo row whose <Name> or <Model> is '%s' in any weapon "
			"meta (%d files read). Name it as the meta spells it - WEAPON_PISTOL - or as the model - "
			"w_pi_pistol"), *Want, MetaFilesSearched));
	}
	const FString MetaName = WeaponHit.Fields.FindRef(TEXT("Name"));
	const FString ModelName = WeaponHit.Fields.FindRef(TEXT("Model")).TrimStartAndEnd().ToLower();
	if (ModelName.IsEmpty())
	{
		return Fail(FString::Printf(TEXT("%s declares no <Model>, so it has no drawable to import (60 of "
			"184 weapon rows are model-less: unarmed, the vehicle weapons and the ammo-only rows)"), *MetaName));
	}

	// ---- 1) the weapon drawable through the ledger -------------------------------------------------
	const FRudeCorpusEntry* YdrRow = Corpus->Effective(TEXT("ydr"), ModelName);
	if (!YdrRow)
	{
		return Fail(FString::Printf(TEXT("%s names model %s but the corpus has no ydr of that name - "
			"export the weapons archives and re-run"), *MetaName, *ModelName));
	}
	const FString YdrPath = Corpus->PathOf(*YdrRow);
	const FString WeaponFolder = DestFolder / ModelName;
	if (!FPackageName::IsValidLongPackageName(WeaponFolder / ModelName))
	{
		return Fail(FString::Printf(TEXT("bad content path: %s"), *WeaponFolder));
	}
	FXmlFile Xml(YdrPath);
	if (!Xml.IsValid()) { return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError())); }
	const FXmlNode* DrawableRoot = Xml.GetRootNode();
	if (!DrawableRoot || DrawableRoot->GetTag() != TEXT("Drawable"))
	{
		return Fail(FString::Printf(TEXT("root of %s is not <Drawable> - 875 of 875 weapon-set drawables are"), *YdrPath));
	}

	TArray<FBone> Bones;
	TArray<FTransform> WorldGta;
	{
		FString SkelWhy;
		if (!ParseSkeleton(DrawableRoot, Bones, SkelWhy)) { return Fail(SkelWhy); }
		if (!ResolveBoneWorld(Bones, WorldGta, SkelWhy)) { return Fail(SkelWhy); }
	}
	TMap<FString, int32> BoneByName;
	int32 WapBones = 0;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const FString Upper = Bones[i].Name.ToUpper();
		if (!BoneByName.Contains(Upper)) { BoneByName.Add(Upper, i); }
		if (Upper.StartsWith(TEXT("WAP"))) { ++WapBones; }
	}

	// ---- 2) the attach points, straight off the weapon's own item ----------------------------------
	// <AttachPoints><Item><AttachBone> + <Components><Item><Name>/<Default>. 415/415 measured attach-point
	// items carry both children; 273 attach points over 184 weapons, 93 of them with exactly one default
	// and NOT ONE with two.
	struct FCompRef
	{
		FString Name;
		bool bDefault = false;
		int32 AttachPointIndex = -1;
	};
	struct FPointRow
	{
		FString Bone;
		int32 BoneIndex = -1;
		TArray<int32> Comps;
		FString DefaultComp;
	};
	TArray<FPointRow> Points;
	TArray<FCompRef> Refs;
	TArray<FString> Missing;
	{
		TUniquePtr<FXmlFile> ItemDoc;
		const FXmlNode* ItemRoot = nullptr;
		if (!ReparseItem(WeaponHit.Xml, ItemDoc, ItemRoot))
		{
			return Fail(TEXT("the weapon's own meta item did not re-parse - refusing rather than importing a gun with no sockets"));
		}
		if (const FXmlNode* Aps = ItemRoot->FindChildNode(TEXT("AttachPoints")))
		{
			for (const FXmlNode* Ap : Aps->GetChildrenNodes())
			{
				FPointRow P;
				P.Bone = NodeValue(Ap->FindChildNode(TEXT("AttachBone")));
				if (const int32* Bi = BoneByName.Find(P.Bone.ToUpper())) { P.BoneIndex = *Bi; }
				const int32 PointIndex = Points.Num();
				if (const FXmlNode* Comps = Ap->FindChildNode(TEXT("Components")))
				{
					for (const FXmlNode* CItem : Comps->GetChildrenNodes())
					{
						FCompRef R;
						R.Name = NodeValue(CItem->FindChildNode(TEXT("Name")));
						if (R.Name.IsEmpty()) { continue; }
						// <Default value="true" /> - an ATTRIBUTE in every measured copy. See NodeValue.
						R.bDefault = NodeValue(CItem->FindChildNode(TEXT("Default")))
							.Equals(TEXT("true"), ESearchCase::IgnoreCase);
						R.AttachPointIndex = PointIndex;
						P.Comps.Add(Refs.Num());
						if (R.bDefault && P.DefaultComp.IsEmpty()) { P.DefaultComp = R.Name; }
						Refs.Add(MoveTemp(R));
					}
				}
				if (P.BoneIndex < 0 && P.Comps.Num() > 0)
				{
					Missing.Add(FString::Printf(TEXT("attach point %d names bone '%s', which %s's own "
						"skeleton does not carry (16 of 272 measured attach points are like this, every "
						"one a WAPClip on a shotgun or launcher) - its %d component(s) ride the weapon's origin"),
						PointIndex, *P.Bone, *ModelName, P.Comps.Num()));
				}
				Points.Add(MoveTemp(P));
			}
		}
	}

	// ---- 3) the component rows, one pass over the weapon metas --------------------------------------
	TSet<FString> WantedComps;
	for (const FCompRef& R : Refs) { WantedComps.Add(R.Name.ToUpper()); }
	TMap<FString, FMetaHit> CompHits;
	FindComponentInfos(*Corpus, WantedComps, CompHits, MetaFilesSearched);

	// ---- 4) textures: the drawable's own dictionary, then the other dictionaries of THIS composite ---
	// Measured rule, not a guess: 2,806 of 5,274 sampler references live in the dictionary of the same
	// name and 234 are embedded in the ydr; the weapons' dictionaries are one flat namespace (1,859 more
	// resolve in another weapon dictionary); gtxd.ymt names NO w_ dictionary, so there is no map-style
	// parent chain to walk. EffectiveWithSidecar prefers a copy that HAS its pixels.
	TArray<FString> DictNames;
	DictNames.Add(ModelName);
	for (const FCompRef& R : Refs)
	{
		const FMetaHit* H = CompHits.Find(R.Name.ToUpper());
		if (!H) { continue; }
		const FString CompModel = H->Fields.FindRef(TEXT("Model")).TrimStartAndEnd().ToLower();
		if (!CompModel.IsEmpty()) { DictNames.AddUnique(CompModel); }
	}
	int32 TxdsImported = 0;
	FString WeaponYtdPath;
	TArray<FString> DictsWithPixels;
	for (const FString& D : DictNames)
	{
		const FRudeCorpusEntry* TxdRow = Corpus->EffectiveWithSidecar(TEXT("ytd"), D);
		if (!TxdRow) { continue; }
		const FString TxdPath = Corpus->PathOf(*TxdRow);
		if (D == ModelName) { WeaponYtdPath = TxdPath; }
		const FString V = URudeToolset::ImportYtd(TxdPath, TEXT(""), TEXT("/Game/RUDE/Textures"));
		if (V.Contains(TEXT("\"ok\":true"))) { ++TxdsImported; DictsWithPixels.Add(D); }
	}

	// ---- 5) the weapon mesh ------------------------------------------------------------------------
	FRudeTextureScope WeaponScope;
	WeaponScope.ArchetypeTxd = ModelName;
	for (const FString& D : DictNames) { if (D != ModelName) { WeaponScope.ParentTxdChain.Add(D); } }
	FString WeaponAssetPath, WeaponVerdict;
	bool bHiDrawable = false;
	int32 LodFailed = 0;
	UStaticMesh* WeaponMesh = ImportWithHiLod(*Corpus, ModelName, WeaponFolder, &WeaponScope,
		WeaponAssetPath, WeaponVerdict, bHiDrawable, LodFailed, Missing);
	if (!WeaponMesh)
	{
		return Fail(FString::Printf(TEXT("the weapon drawable failed to import: %s"),
			*(WeaponVerdict.IsEmpty() ? FString(TEXT("asset did not load")) : WeaponVerdict)));
	}
	int32 TexturesMissing = FMath::Max(0, JsonInt(WeaponVerdict, TEXT("missingTextures"), 0));
	const int32 WeaponGeos = JsonInt(WeaponVerdict, TEXT("geometries"), -1);
	const int32 WeaponVerts = JsonInt(WeaponVerdict, TEXT("vertices"), -1);
	const int32 WeaponTris = JsonInt(WeaponVerdict, TEXT("triangles"), -1);

	// ---- 6) each component's mesh, and the AAP bone it declares ------------------------------------
	struct FCompRow
	{
		FString Name, Type, Model, AttachPoint, AttachBone, ResolvedBone, AssetPath, ComponentName, SourceFile, Xml;
		bool bDefault = false;
		int32 AttachPointIndex = -1;
		int32 BoneIndex = -1;
		int32 Vertices = 0, Triangles = 0;
		UStaticMesh* Mesh = nullptr;
		TMap<FString, FString> Fields;
	};
	TArray<FCompRow> Rows;
	int32 ComponentsImported = 0, ComponentsMissingMesh = 0, ComponentsUnmapped = 0;
	int32 ComponentsWithoutModel = 0, ComponentsWithoutInfo = 0, ComponentBonesMatched = 0, ComponentHiLods = 0;
	int32 ComponentMetaFields = 0;
	for (const FCompRef& R : Refs)
	{
		FCompRow Row;
		Row.Name = R.Name;
		Row.bDefault = R.bDefault;
		Row.AttachPointIndex = R.AttachPointIndex;
		Row.AttachPoint = Points.IsValidIndex(R.AttachPointIndex) ? Points[R.AttachPointIndex].Bone : FString();
		Row.BoneIndex = Points.IsValidIndex(R.AttachPointIndex) ? Points[R.AttachPointIndex].BoneIndex : -1;
		const FMetaHit* Hit = CompHits.Find(R.Name.ToUpper());
		if (!Hit)
		{
			++ComponentsWithoutInfo;
			Missing.Add(FString::Printf(TEXT("component %s is listed by %s but no CWeaponComponent*Info row "
				"of that name exists in any weapon meta (627 of 627 measured references resolved, so this "
				"is a corpus gap, not a shape the game ships)"), *R.Name, *MetaName));
			Rows.Add(MoveTemp(Row));
			continue;
		}
		Row.Type = Hit->Type;
		Row.Fields = Hit->Fields;
		Row.SourceFile = Hit->Path;
		Row.Xml = Hit->Xml;
		Row.AttachBone = Hit->Fields.FindRef(TEXT("AttachBone")).TrimStartAndEnd();
		Row.Model = Hit->Fields.FindRef(TEXT("Model")).TrimStartAndEnd().ToLower();
		ComponentMetaFields += Hit->Fields.Num();
		if (Row.BoneIndex < 0) { ++ComponentsUnmapped; }
		if (Row.Model.IsEmpty())
		{
			++ComponentsWithoutModel;   // 24 of 627 measured references name no model (reload-data rows)
			Rows.Add(MoveTemp(Row));
			continue;
		}
		const FRudeCorpusEntry* CompYdr = Corpus->Effective(TEXT("ydr"), Row.Model);
		if (!CompYdr)
		{
			++ComponentsMissingMesh;
			Missing.Add(FString::Printf(TEXT("component %s names model %s but the corpus has no ydr of that "
				"name (1 of 603 measured, the musket's p_w_ar_musket_chrg, is a p_ prop not in the weapon set)"),
				*R.Name, *Row.Model));
			Rows.Add(MoveTemp(Row));
			continue;
		}
		FRudeTextureScope CompScope;
		CompScope.ArchetypeTxd = Row.Model;
		for (const FString& D : DictNames) { if (D != Row.Model) { CompScope.ParentTxdChain.Add(D); } }
		FString CompVerdict;
		bool bCompHi = false;
		Row.Mesh = ImportWithHiLod(*Corpus, Row.Model, WeaponFolder, &CompScope, Row.AssetPath, CompVerdict,
			bCompHi, LodFailed, Missing);
		if (bCompHi) { ++ComponentHiLods; }
		if (!Row.Mesh)
		{
			++ComponentsMissingMesh;
			Missing.Add(FString::Printf(TEXT("component %s (%s) failed to import: %s"), *R.Name, *Row.Model,
				*(CompVerdict.IsEmpty() ? FString(TEXT("asset did not load")) : CompVerdict)));
			Rows.Add(MoveTemp(Row));
			continue;
		}
		++ComponentsImported;
		Row.Vertices = FMath::Max(0, JsonInt(CompVerdict, TEXT("vertices"), 0));
		Row.Triangles = FMath::Max(0, JsonInt(CompVerdict, TEXT("triangles"), 0));
		TexturesMissing += FMath::Max(0, JsonInt(CompVerdict, TEXT("missingTextures"), 0));
		// The component drawable's own AAP bone, read from the file rather than assumed: it is the first
		// bone in 516/516 measured AAP-bearing drawables, and the declared name is on the drawable in
		// 509/602 model-bearing references. A mismatch is REPORTED, never corrected by guessing.
		{
			FXmlFile CompXml(Corpus->PathOf(*CompYdr));
			const FXmlNode* CompRoot = CompXml.IsValid() ? CompXml.GetRootNode() : nullptr;
			TArray<FBone> CompBones;
			FString CompWhy;
			if (CompRoot && ParseSkeleton(CompRoot, CompBones, CompWhy))
			{
				for (const FBone& B : CompBones)
				{
					if (B.Name.ToUpper().StartsWith(TEXT("AAP"))) { Row.ResolvedBone = B.Name; break; }
				}
			}
			if (!Row.AttachBone.IsEmpty() && !Row.ResolvedBone.Equals(Row.AttachBone, ESearchCase::IgnoreCase))
			{
				Missing.Add(FString::Printf(TEXT("component %s declares <AttachBone> %s but its drawable %s "
					"carries %s - placed on the weapon's %s socket as the meta says (the meta is the join; "
					"377 of 402 socket pairs agree by name and 25 do not)"), *R.Name, *Row.AttachBone,
					*Row.Model, Row.ResolvedBone.IsEmpty() ? TEXT("no AAP bone") : *Row.ResolvedBone, *Row.AttachPoint));
			}
			else if (!Row.AttachBone.IsEmpty()) { ++ComponentBonesMatched; }
		}
		Rows.Add(MoveTemp(Row));
	}

	// ---- 7) the ammo row <AmmoInfo> points at ------------------------------------------------------
	TMap<FString, FString> AmmoFields;
	{
		const FString AmmoName = WeaponHit.Fields.FindRef(TEXT("AmmoInfo")).TrimStartAndEnd();
		if (!AmmoName.IsEmpty() && !AmmoName.Equals(TEXT("NULL"), ESearchCase::IgnoreCase))
		{
			// CAmmoInfo items are not CWeaponComponent*, so this pass looks for them by name directly.
			// 70 distinct ammo rows over 107 copies, 94 distinct field names (CAmmoInfo / CAmmoThrownInfo /
			// CAmmoRocketInfo / CAmmoProjectileInfo) - all matched by the "CAmmo" prefix.
			TArray<FString> AmmoMetaNames;
			WeaponMetaNames(*Corpus, AmmoMetaNames);
			for (const FString& AmmoMetaName : AmmoMetaNames)
			{
				for (const FRudeCorpusEntry* E : Corpus->History(TEXT("meta"), AmmoMetaName))
				{
					FString Text;
					if (!FFileHelper::LoadFileToString(Text, *Corpus->PathOf(*E))) { continue; }
					if (!Text.Contains(AmmoName, ESearchCase::IgnoreCase)) { continue; }
					++MetaFilesSearched;
					FXmlFile AmmoDoc(Text, EConstructMethod::ConstructFromBuffer);
					if (!AmmoDoc.IsValid()) { continue; }
					ForEachItem(AmmoDoc.GetRootNode(), [&](const FXmlNode* Item, const FString& ItemType)
					{
						if (!ItemType.StartsWith(TEXT("CAmmo"))) { return; }
						if (!NodeValue(Item->FindChildNode(TEXT("Name"))).Equals(AmmoName, ESearchCase::IgnoreCase)) { return; }
						AmmoFields.Reset();
						FlattenItem(Item, TEXT(""), AmmoFields, 0);
					});
				}
			}
		}
	}

	// ---- 8) spawn: the weapon at identity, every component at its socket frame ----------------------
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FName IdTag(*(TEXT("RUDE_WEAPON:") + ModelName));
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It) { if (It->Tags.Contains(IdTag)) { Stale.Add(*It); } }
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}
	AActor* Actor = World->SpawnActor<AActor>();
	if (!Actor) { return Fail(TEXT("actor spawn failed")); }
	USceneComponent* RootComp = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Actor->SetRootComponent(RootComp);
	RootComp->SetMobility(EComponentMobility::Static);
	RootComp->RegisterComponent();
	Actor->AddInstanceComponent(RootComp);
	Actor->SetActorLabel(TEXT("Weapon_") + ModelName);
	Actor->SetFolderPath(FName(TEXT("RUDE_WEAPONS")));
	Actor->Tags.Add(IdTag);
	Actor->Tags.Add(FName(TEXT("RUDE_WEAPON_ROOT")));
	const FString AssetName = ModelName + TEXT("_weapon");
	const FString AssetPkgName = WeaponFolder / AssetName;
	Actor->Tags.Add(FName(*(TEXT("RUDE_WEAPON_ASSET:") + AssetPkgName)));

	auto AddMesh = [&](UStaticMesh* M, const FName& CompName, const FTransform& Xf) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(Actor, CompName);
		SMC->SetStaticMesh(M);
		SMC->SetMobility(EComponentMobility::Static);
		SMC->SetupAttachment(RootComp);
		SMC->SetRelativeTransform(Xf);
		SMC->RegisterComponent();
		Actor->AddInstanceComponent(SMC);
		return SMC;
	};
	// The weapon drawable's vertices are in weapon-local space, so the gun sits at the actor's origin -
	// which is what makes the socket frames (also weapon-local) drop in as relative transforms.
	AddMesh(WeaponMesh, TEXT("Weapon"), FTransform::Identity);

	int32 Placed = 0, Shown = 0;
	for (int32 i = 0; i < Rows.Num(); ++i)
	{
		FCompRow& Row = Rows[i];
		if (!Row.Mesh) { continue; }
		const FTransform Xf = (Row.BoneIndex >= 0 && WorldGta.IsValidIndex(Row.BoneIndex))
			? GtaToUe(WorldGta[Row.BoneIndex]) : FTransform::Identity;
		// Index + name: NewObject with a name already used under the same outer is a hard error, and one
		// component model can sit on two sockets of the same weapon (the _2 sockets).
		const FName CompName(*FString::Printf(TEXT("Comp_%02d_%s"), i, *Row.Model));
		UStaticMeshComponent* SMC = AddMesh(Row.Mesh, CompName, Xf);
		if (!SMC) { continue; }
		++Placed;
		Row.ComponentName = CompName.ToString();
		// The tag is how SetWeaponComponent finds it again (the shape SetPedProp uses for prop anchors):
		// RUDE_WEAPONCOMP:<attach point ordinal>:<component meta name>.
		SMC->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_WEAPONCOMP:%d:%s"),
			Row.AttachPointIndex, *Row.Name)));
		// Defaults visible, alternates imported and hidden - the game's own <Default> flag, which is
		// true on 93 of 627 references and never twice on one socket.
		SMC->SetVisibility(Row.bDefault);
		if (Row.bDefault) { ++Shown; }
	}

	// ---- 9) the DataAsset --------------------------------------------------------------------------
	UPackage* APkg = CreatePackage(*AssetPkgName);
	if (!APkg) { return Fail(TEXT("CreatePackage failed for the weapon asset")); }
	APkg->FullyLoad();
	URudeWeapon* WA = FindObject<URudeWeapon>(APkg, *AssetName);
	const bool bNewAsset = WA == nullptr;
	if (!WA) { WA = NewObject<URudeWeapon>(APkg, FName(*AssetName), RF_Public | RF_Standalone); }
	if (!WA) { return Fail(TEXT("NewObject<URudeWeapon> failed")); }
	WA->WeaponName = MetaName;
	WA->ModelName = ModelName;
	WA->WeaponMesh = WeaponMesh;
	WA->Audio = WeaponHit.Fields.FindRef(TEXT("Audio"));
	WA->Slot = WeaponHit.Fields.FindRef(TEXT("Slot"));
	WA->DamageType = WeaponHit.Fields.FindRef(TEXT("DamageType"));
	WA->FireType = WeaponHit.Fields.FindRef(TEXT("FireType"));
	WA->WheelSlot = WeaponHit.Fields.FindRef(TEXT("WheelSlot"));
	WA->Group = WeaponHit.Fields.FindRef(TEXT("Group"));
	WA->AmmoInfoName = WeaponHit.Fields.FindRef(TEXT("AmmoInfo"));
	WA->AimingInfo = WeaponHit.Fields.FindRef(TEXT("AimingInfo"));
	WA->ClipSize = WeaponHit.Fields.FindRef(TEXT("ClipSize"));
	WA->Damage = WeaponHit.Fields.FindRef(TEXT("Damage"));
	WA->AccuracySpread = WeaponHit.Fields.FindRef(TEXT("AccuracySpread"));
	WA->TimeBetweenShots = WeaponHit.Fields.FindRef(TEXT("TimeBetweenShots"));
	WA->ReloadTimeMP = WeaponHit.Fields.FindRef(TEXT("ReloadTimeMP"));
	WA->ReloadTimeSP = WeaponHit.Fields.FindRef(TEXT("ReloadTimeSP"));
	WA->Speed = WeaponHit.Fields.FindRef(TEXT("Speed"));
	WA->Penetration = WeaponHit.Fields.FindRef(TEXT("Penetration"));
	WA->WeaponMeta = WeaponHit.Fields;
	WA->AmmoMeta = AmmoFields;
	WA->BoneTags.Reset();
	for (const FBone& B : Bones) { WA->BoneTags.Add(FName(*B.Name), B.Tag); }
	WA->BoneCount = Bones.Num();
	WA->WapBoneCount = WapBones;
	WA->bHiDrawable = bHiDrawable;
	WA->HiLodMeshes = ComponentHiLods + (bHiDrawable ? 1 : 0);
	WA->TextureDictionaries = DictsWithPixels;
	WA->TexturesMissing = TexturesMissing;
	WA->SourceYdr = YdrPath;
	WA->SourceYtd = WeaponYtdPath;
	WA->SourceWeaponsMeta = WeaponHit.Path;
	WA->WeaponsMetaXml = WeaponHit.Xml;
	WA->Components.Reset();
	for (const FCompRow& Row : Rows)
	{
		FRudeWeaponComponent C;
		C.Name = Row.Name; C.Type = Row.Type; C.Model = Row.Model;
		C.AttachPoint = Row.AttachPoint; C.AttachBone = Row.AttachBone; C.ResolvedBone = Row.ResolvedBone;
		C.bDefault = Row.bDefault; C.AttachPointIndex = Row.AttachPointIndex; C.BoneIndex = Row.BoneIndex;
		C.Mesh = Row.Mesh; C.ComponentName = Row.ComponentName; C.bVisible = Row.bDefault;
		C.Vertices = Row.Vertices; C.Triangles = Row.Triangles;
		C.LocName = Row.Fields.FindRef(TEXT("LocName"));
		C.LocDesc = Row.Fields.FindRef(TEXT("LocDesc"));
		C.AccuracyModifier = Row.Fields.FindRef(TEXT("AccuracyModifier"));
		C.DamageModifier = Row.Fields.FindRef(TEXT("DamageModifier"));
		C.CreateObject = Row.Fields.FindRef(TEXT("CreateObject"));
		C.ClipSize = Row.Fields.FindRef(TEXT("ClipSize"));
		C.MuzzleBone = Row.Fields.FindRef(TEXT("MuzzleBone"));
		C.CameraHash = Row.Fields.FindRef(TEXT("CameraHash"));
		C.Fields = Row.Fields; C.SourceFile = Row.SourceFile; C.Xml = Row.Xml;
		WA->Components.Add(MoveTemp(C));
	}
	WA->AttachPoints.Reset();
	for (const FPointRow& P : Points)
	{
		FRudeWeaponAttachPoint A;
		A.Bone = P.Bone; A.BoneIndex = P.BoneIndex; A.Components = P.Comps;
		A.DefaultComponent = P.DefaultComp; A.CurrentComponent = P.DefaultComp;
		WA->AttachPoints.Add(MoveTemp(A));
	}
	WA->MarkPackageDirty();
	if (bNewAsset)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(WA);
	}

	// ---- 10) verdict: ok is COMPUTED, and refuses only on the total-loss shape ----------------------
	// ⛔ The failure this file exists to prevent is a rifle imported with NO magazine. So: the weapon's
	// own mesh must be there, and when its meta lists components at least one must have got a mesh. A
	// PARTIAL loss stays ok:true with the counts beside it - refusing there would also throw away the
	// components that DID import, and 23 of 627 measured references legitimately have no model at all.
	// Textures NEVER gate: this corpus has pixels for 0 of 804 weapon dictionaries, so a texture-gated
	// ok would be false on every weapon in the game.
	const bool bOk = (WeaponMesh != nullptr) && (Refs.Num() == 0 || ComponentsImported > 0);
	const int32 MissingListCap = 20;
	// The two arrays the offline comparator checks against the corpus XML: the skeleton's bone NAMES (not
	// just a count - a count cannot catch a socket read off the wrong file) and one row per component with
	// the numbers a mesh can be identified by. Bounded: the widest weapon in the set lists 32 components.
	TArray<FString> BoneNames;
	for (const FBone& B : Bones) { BoneNames.Add(B.Name); }
	FString CompJson;
	for (const FCompRow& Row : Rows)
	{
		CompJson += FString::Printf(
			TEXT("%s{\"name\":\"%s\",\"model\":\"%s\",\"attachPoint\":\"%s\",\"componentBone\":\"%s\","
				"\"vertices\":%d,\"triangles\":%d,\"default\":%s,\"mesh\":%s}"),
			CompJson.IsEmpty() ? TEXT("") : TEXT(","), *JsonEscape(Row.Name), *JsonEscape(Row.Model),
			*JsonEscape(Row.AttachPoint), *JsonEscape(Row.ResolvedBone), Row.Vertices, Row.Triangles,
			Row.bDefault ? TEXT("true") : TEXT("false"), Row.Mesh ? TEXT("true") : TEXT("false"));
	}
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"weapon\":\"%s\",\"model\":\"%s\",\"actor\":\"%s\",\"asset\":\"%s\",\"weaponMesh\":\"%s\","
		"\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"bones\":%d,\"wapBones\":%d,\"attachPoints\":%d,"
		"\"components\":%d,\"componentsImported\":%d,\"componentsMissingMesh\":%d,\"componentsUnmapped\":%d,"
		"\"componentsWithoutModel\":%d,\"componentsWithoutInfo\":%d,\"componentBonesMatched\":%d,"
		"\"componentsPlaced\":%d,\"componentsShown\":%d,\"hiDrawable\":%s,\"componentHiLods\":%d,"
		"\"lodFailed\":%d,\"texturesMissing\":%d,\"textureDictionaries\":[%s],"
		"\"txdsImported\":%d,\"metaFilesSearched\":%d,\"weaponMetaFields\":%d,\"ammoMetaFields\":%d,"
		"\"componentMetaFields\":%d,\"sourceYdr\":\"%s\",\"sourceWeaponsMeta\":\"%s\","
		"\"boneNames\":[%s],\"componentMeshes\":[%s],"
		"\"missingCount\":%d,\"missingTruncated\":%d,\"missing\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		*JsonEscape(MetaName), *ModelName, *JsonEscape(Actor->GetActorLabel()), *JsonEscape(AssetPkgName),
		*JsonEscape(WeaponAssetPath), WeaponGeos, WeaponVerts, WeaponTris, Bones.Num(), WapBones, Points.Num(),
		Refs.Num(), ComponentsImported, ComponentsMissingMesh, ComponentsUnmapped,
		ComponentsWithoutModel, ComponentsWithoutInfo, ComponentBonesMatched,
		Placed, Shown, bHiDrawable ? TEXT("true") : TEXT("false"), ComponentHiLods, LodFailed,
		TexturesMissing, *JsonStrings(DictsWithPixels), TxdsImported, MetaFilesSearched,
		WeaponHit.Fields.Num(), AmmoFields.Num(), ComponentMetaFields,
		*JsonEscape(YdrPath), *JsonEscape(WeaponHit.Path),
		*JsonStrings(BoneNames), *CompJson,
		Missing.Num(), FMath::Max(0, Missing.Num() - MissingListCap), *JsonStrings(Missing, MissingListCap));
}

FString URudeToolset::SetWeaponComponent(const FString& ActorLabel, const FString& AttachPoint,
                                         const FString& ComponentName)
{
	using namespace RudeWeaponLane;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* Actor = FindWeaponActor(World, ActorLabel.TrimStartAndEnd());
	if (!Actor)
	{
		return Fail(FString::Printf(TEXT("no weapon actor labelled '%s' (ImportWeapon labels them Weapon_<model>)"),
			*ActorLabel));
	}
	FString AssetPath;
	for (const FName& T : Actor->Tags)
	{
		const FString S = T.ToString();
		if (S.StartsWith(TEXT("RUDE_WEAPON_ASSET:"))) { AssetPath = S.Mid(18); }
	}
	URudeWeapon* WA = AssetPath.IsEmpty() ? nullptr : LoadObject<URudeWeapon>(nullptr, *AssetPath);
	if (!WA) { return Fail(TEXT("that actor carries no RUDE weapon asset (was it built by ImportWeapon?)")); }

	// ---- the attach point: the socket bone as the meta spells it, or its ordinal -------------------
	const FString WantPoint = AttachPoint.TrimStartAndEnd();
	int32 PointIndex = INDEX_NONE;
	for (int32 i = 0; i < WA->AttachPoints.Num(); ++i)
	{
		if (WA->AttachPoints[i].Bone.Equals(WantPoint, ESearchCase::IgnoreCase)) { PointIndex = i; break; }
	}
	if (PointIndex == INDEX_NONE && WantPoint.IsNumeric())
	{
		const int32 Ordinal = FCString::Atoi(*WantPoint);
		if (WA->AttachPoints.IsValidIndex(Ordinal)) { PointIndex = Ordinal; }
	}
	if (PointIndex == INDEX_NONE)
	{
		TArray<FString> Known;
		for (const FRudeWeaponAttachPoint& A : WA->AttachPoints) { Known.Add(A.Bone); }
		return Fail(FString::Printf(TEXT("%s has no attach point '%s' - it has %d: %s"), *WA->ModelName,
			*WantPoint, WA->AttachPoints.Num(), *FString::Join(Known, TEXT(", "))));
	}
	FRudeWeaponAttachPoint& Point = WA->AttachPoints[PointIndex];

	// ---- the component: its meta name or its model name, empty = nothing on this socket ------------
	const FString WantComp = ComponentName.TrimStartAndEnd();
	int32 RowIndex = INDEX_NONE;
	if (!WantComp.IsEmpty())
	{
		for (const int32 Ci : Point.Components)
		{
			if (!WA->Components.IsValidIndex(Ci)) { continue; }
			const FRudeWeaponComponent& C = WA->Components[Ci];
			if (C.Name.Equals(WantComp, ESearchCase::IgnoreCase) || C.Model.Equals(WantComp, ESearchCase::IgnoreCase))
			{
				RowIndex = Ci;
				break;
			}
		}
		if (RowIndex == INDEX_NONE)
		{
			TArray<FString> Known;
			for (const int32 Ci : Point.Components)
			{
				if (WA->Components.IsValidIndex(Ci)) { Known.Add(WA->Components[Ci].Name); }
			}
			return Fail(FString::Printf(TEXT("'%s' is not one of the %d components %s lists at %s: %s"),
				*WantComp, Point.Components.Num(), *WA->ModelName, *Point.Bone, *FString::Join(Known, TEXT(", "))));
		}
		if (WA->Components[RowIndex].Mesh.IsNull())
		{
			return Fail(FString::Printf(TEXT("component %s has no imported mesh (its row names model '%s'); "
				"ImportWeapon's verdict says why - componentsMissingMesh / componentsWithoutModel"),
				*WA->Components[RowIndex].Name, *WA->Components[RowIndex].Model));
		}
	}

	// ---- one component per socket, exactly as the game's <Default> data is shaped ------------------
	const FString TagPrefix = FString::Printf(TEXT("RUDE_WEAPONCOMP:%d:"), PointIndex);
	TArray<UStaticMeshComponent*> Comps;
	Actor->GetComponents<UStaticMeshComponent>(Comps);
	int32 Hidden = 0;
	UStaticMeshComponent* ShownComp = nullptr;
	const FString WantTag = (RowIndex == INDEX_NONE) ? FString() : TagPrefix + WA->Components[RowIndex].Name;
	for (UStaticMeshComponent* C : Comps)
	{
		bool bMine = false, bWant = false;
		for (const FName& T : C->ComponentTags)
		{
			const FString S = T.ToString();
			if (!S.StartsWith(TagPrefix)) { continue; }
			bMine = true;
			bWant = !WantTag.IsEmpty() && S.Equals(WantTag, ESearchCase::IgnoreCase);
		}
		if (!bMine) { continue; }
		if (bWant) { ShownComp = C; }
		else if (C->IsVisible()) { C->SetVisibility(false); ++Hidden; }
	}
	if (RowIndex != INDEX_NONE && !ShownComp)
	{
		return Fail(FString::Printf(TEXT("component %s has an imported mesh but no component on the actor - "
			"re-run ImportWeapon (the actor is rebuilt by tag every run)"), *WA->Components[RowIndex].Name));
	}
	if (ShownComp)
	{
		ShownComp->SetVisibility(true);
		ShownComp->MarkRenderStateDirty();
	}
	for (int32 i = 0; i < WA->Components.Num(); ++i)
	{
		if (WA->Components[i].AttachPointIndex == PointIndex) { WA->Components[i].bVisible = (i == RowIndex); }
	}
	Point.CurrentComponent = (RowIndex == INDEX_NONE) ? FString() : WA->Components[RowIndex].Name;
	Actor->Modify();
	Actor->MarkPackageDirty();
	WA->MarkPackageDirty();

	// ok is COMPUTED: the socket now shows exactly what was asked for - the named component, or nothing.
	const bool bOk = (RowIndex == INDEX_NONE) ? (ShownComp == nullptr) : (ShownComp != nullptr && ShownComp->IsVisible());
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"weapon\":\"%s\",\"attachPoint\":\"%s\",\"attachPointIndex\":%d,\"boneIndex\":%d,"
		"\"component\":\"%s\",\"model\":\"%s\",\"componentBone\":\"%s\",\"mesh\":\"%s\",\"vertices\":%d,"
		"\"triangles\":%d,\"shown\":%s,\"hidden\":%d,\"sceneComponent\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *JsonEscape(WA->ModelName), *JsonEscape(Point.Bone), PointIndex,
		Point.BoneIndex,
		RowIndex == INDEX_NONE ? TEXT("") : *JsonEscape(WA->Components[RowIndex].Name),
		RowIndex == INDEX_NONE ? TEXT("") : *JsonEscape(WA->Components[RowIndex].Model),
		RowIndex == INDEX_NONE ? TEXT("") : *JsonEscape(WA->Components[RowIndex].ResolvedBone),
		RowIndex == INDEX_NONE ? TEXT("") : *JsonEscape(WA->Components[RowIndex].Mesh.ToString()),
		RowIndex == INDEX_NONE ? 0 : WA->Components[RowIndex].Vertices,
		RowIndex == INDEX_NONE ? 0 : WA->Components[RowIndex].Triangles,
		ShownComp ? TEXT("true") : TEXT("false"), Hidden,
		ShownComp ? *JsonEscape(ShownComp->GetName()) : TEXT(""));
}

// ---- SetWeaponTint (WP12 weapon_tint lane, 2026-09-07) -----------------------------------------
// Pick which of the game's own tints a weapon is painted in. Everything this needs already sits on
// the material instances ImportWeapon built - the palette texture in TintPalette and the row scale
// beside it - so the tool only has to write the SELECTOR and report what it found.
//
// WHY IT DOES NOT TOUCH TintAmount. That switch is the IMPORT's decision, made once, from evidence:
// a palette actually bound, on a RenderBucket-0 shader, over a diffuse whose alpha varies, on a
// preset the lookup is enabled for. Letting this tool force it to 1 would let an author switch on a
// lookup the data does not support - exactly the "silent default" shape the conventions forbid. So
// the index always lands, and the verdict reports slotsTinting: how many slots will actually LOOK
// different. ok is COMPUTED from that, not from "the write ran".
//
// ⛔ WHAT THIS DOES NOT DO: the index is EDITOR STATE. ExportYdr emits a fixed census-standard
// parameter block and the diffuse/bump/spec samplers - it reads no scalar off the instance and emits
// no palette sampler - so the tint does not survive an export back to the game's format. Round-
// tripping it is unbuilt work named in the lane's NOTES.md, not a property this tool has.
//
// The RANGE is the palette's own row count, read off the bound texture, never a constant: 94 of the
// 98 palettes in this corpus's weapon dictionaries are 128x32 and 4 are 4x4, and the number of
// DISTINCT rows varies too - 27 palettes carry 8 distinct leading rows (the 8 tints weapons.meta's
// TINT_DEFAULT declares, referenced by 91/91 CWeaponInfo rows in the copy the game loads), 34 carry
// 9, and 29 are 32 distinct.
// So the verdict also reports distinctRows and rowDuplicateOf: asking for tint 12 on a palette whose
// rows 8..29 all repeat row 7 is legal, and says so, instead of looking broken.
FString URudeToolset::SetWeaponTint(const FString& ActorLabel, const FString& TintIndex)
{
	using namespace RudeWeaponLane;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Want = ActorLabel.TrimStartAndEnd();
	AActor* Actor = FindWeaponActor(World, Want);
	if (!Actor)
	{
		TArray<FString> Known;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(FName(TEXT("RUDE_WEAPON_ROOT")))) { Known.Add(It->GetActorLabel()); }
		}
		Known.Sort();
		return Fail(FString::Printf(TEXT("no weapon actor labelled '%s' (ImportWeapon labels them "
			"Weapon_<model>); this level holds %d: %s"), *Want, Known.Num(),
			Known.Num() > 0 ? *FString::Join(Known, TEXT(", ")) : TEXT("none")));
	}
	FString AssetPath;
	for (const FName& ActorTag : Actor->Tags)
	{
		const FString S = ActorTag.ToString();
		if (S.StartsWith(TEXT("RUDE_WEAPON_ASSET:"))) { AssetPath = S.Mid(18); }
	}
	URudeWeapon* WA = AssetPath.IsEmpty() ? nullptr : LoadObject<URudeWeapon>(nullptr, *AssetPath);
	if (!WA) { return Fail(TEXT("that actor carries no RUDE weapon asset (was it built by ImportWeapon?)")); }

	const FString IdxText = TintIndex.TrimStartAndEnd();
	if (IdxText.IsEmpty() || !IdxText.IsNumeric())
	{
		return Fail(FString::Printf(TEXT("tint '%s' is not a number - it is the game's tint index, "
			"counting from 0"), *IdxText));
	}
	const int32 Idx = FCString::Atoi(*IdxText);
	if (Idx < 0) { return Fail(FString::Printf(TEXT("tint %d is negative; tints count from 0"), Idx)); }

	// ---- every material slot on the gun and its components that carries a REAL palette -------------
	const FMaterialParameterInfo PalInfo(TEXT("TintPalette"));
	const FMaterialParameterInfo AmtInfo(TEXT("TintAmount"));
	const FMaterialParameterInfo RowInfo(TEXT("TintRowScale"));
	const FMaterialParameterInfo SelInfo(TEXT("paletteSelector"));
	const FMaterialParameterInfo TntInfo(TEXT("tintPaletteSelector"));
	TArray<UStaticMeshComponent*> Comps;
	Actor->GetComponents<UStaticMeshComponent>(Comps);
	TArray<UMaterialInstanceConstant*> Targets;
	TSet<UMaterialInstanceConstant*> SeenMics;
	TArray<FString> PaletteNames;
	int32 SlotsTotal = 0, SlotsWithoutInstance = 0, SlotsWithoutParameter = 0, SlotsPaletteUnbound = 0;
	int32 RowsMin = MAX_int32, RowsMax = 0;
	for (UStaticMeshComponent* C : Comps)
	{
		UStaticMesh* Mesh = C ? C->GetStaticMesh() : nullptr;
		if (!Mesh) { continue; }
		for (const FStaticMaterial& SM : Mesh->GetStaticMaterials())
		{
			++SlotsTotal;
			UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(SM.MaterialInterface);
			if (!MIC) { ++SlotsWithoutInstance; continue; }
			UTexture* PalTex = nullptr;
			if (!MIC->GetTextureParameterValue(PalInfo, PalTex)) { ++SlotsWithoutParameter; continue; }
			UTexture2D* Pal2D = Cast<UTexture2D>(PalTex);
			// The master's own white default sitting in the parameter is NOT a palette. Counting that
			// case separately is what turns "nothing happened" into a diagnosable number.
			// int32(): GetSizeX/Y are int64 in 5.8; the cast is explicit so the narrowing is intended, not
			// a warning this project's targets are one settings change away from making an error.
			const int32 Rows = (Pal2D && Pal2D->Source.IsValid()) ? int32(Pal2D->Source.GetSizeY()) : 0;
			if (!Pal2D || Rows <= 0 || Pal2D->GetPathName().StartsWith(TEXT("/Engine/")))
			{
				++SlotsPaletteUnbound;
				continue;
			}
			RowsMin = FMath::Min(RowsMin, Rows);
			RowsMax = FMath::Max(RowsMax, Rows);
			PaletteNames.AddUnique(Pal2D->GetName());
			if (!SeenMics.Contains(MIC)) { SeenMics.Add(MIC); Targets.Add(MIC); }
		}
	}
	if (Targets.Num() == 0)
	{
		return Fail(FString::Printf(TEXT("%s has no material carrying a tint palette: %d slots, %d without "
			"a material instance, %d whose master has no TintPalette parameter, %d with the parameter and "
			"nothing in it. A weapon only tints when its dictionary carried the <model>_Dpal entry AND its "
			"drawable's shader bound it - run RegenerateMasters and re-import if this weapon predates the "
			"palette lane"),
			*WA->ModelName, SlotsTotal, SlotsWithoutInstance, SlotsWithoutParameter, SlotsPaletteUnbound));
	}
	if (Idx >= RowsMin)
	{
		return Fail(FString::Printf(TEXT("tint %d is out of range for %s: its palette holds %d rows, so the "
			"tints are 0..%d (the game declares 8 for every weapon naming TINT_DEFAULT)"),
			Idx, *WA->ModelName, RowsMin, RowsMin - 1));
	}

	// ---- write the selector; TintRowScale is RE-DERIVED from the texture, so it cannot drift --------
	int32 SlotsUpdated = 0, SlotsTinting = 0;
	for (UMaterialInstanceConstant* MIC : Targets)
	{
		UTexture* PalTex = nullptr;
		MIC->GetTextureParameterValue(PalInfo, PalTex);
		UTexture2D* Pal2D = Cast<UTexture2D>(PalTex);
		const int32 Rows = (Pal2D && Pal2D->Source.IsValid()) ? int32(Pal2D->Source.GetSizeY()) : 0;
		if (Rows <= 0) { continue; }
		MIC->SetScalarParameterValueEditorOnly(SelInfo, float(Idx));
		// The two spellings SUM in the master and no shader item carries both (0/583), so exactly one
		// of them may hold the index; this one does, and the other is pinned to 0.
		MIC->SetScalarParameterValueEditorOnly(TntInfo, 0.f);
		MIC->SetScalarParameterValueEditorOnly(RowInfo, 1.f / float(Rows));
		MIC->PostEditChange();
		MIC->MarkPackageDirty();
		++SlotsUpdated;
		float Amount = 0.f;
		if (MIC->GetScalarParameterValue(AmtInfo, Amount) && Amount > 0.f) { ++SlotsTinting; }
	}

	// ---- how many of the palette's rows are actually different, and is THIS one a repeat? ----------
	// Capped at 64 rows: the duplicate scan is O(rows^2) memcmp and every weapon palette measured in
	// this corpus is 32 rows or fewer. Beyond the cap it reports -1 (unknown) rather than a number it
	// did not compute.
	int32 DistinctRows = -1, RowDuplicateOf = -1;
	{
		UTexture* PalTex = nullptr;
		Targets[0]->GetTextureParameterValue(PalInfo, PalTex);
		UTexture2D* Pal2D = Cast<UTexture2D>(PalTex);
		TArray64<uint8> Mip;
		if (Pal2D && Pal2D->Source.IsValid() && Pal2D->Source.GetFormat() == TSF_BGRA8
			&& Pal2D->Source.GetSizeY() <= 64 && Pal2D->Source.GetMipData(Mip, 0))
		{
			const int32 PalW = int32(Pal2D->Source.GetSizeX());
			const int32 PalH = int32(Pal2D->Source.GetSizeY());
			const int64 Pitch = int64(PalW) * 4;
			if (Mip.Num() >= Pitch * PalH)
			{
				DistinctRows = 0;
				for (int32 Row = 0; Row < PalH; ++Row)
				{
					int32 SameAs = -1;
					for (int32 Prev = 0; Prev < Row; ++Prev)
					{
						if (FMemory::Memcmp(Mip.GetData() + Prev * Pitch, Mip.GetData() + Row * Pitch, Pitch) == 0)
						{
							SameAs = Prev;
							break;
						}
					}
					if (SameAs < 0) { ++DistinctRows; }
					if (Row == Idx) { RowDuplicateOf = SameAs; }
				}
			}
		}
	}

	// ok is COMPUTED: the index landed AND at least one slot will actually look different for it.
	const bool bOk = SlotsUpdated > 0 && SlotsTinting > 0;
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"weapon\":\"%s\",\"actor\":\"%s\",\"tint\":%d,\"paletteRows\":%d,"
		"\"paletteRowsMax\":%d,\"distinctRows\":%d,\"rowDuplicateOf\":%d,\"palettes\":[%s],"
		"\"slots\":%d,\"slotsUpdated\":%d,\"slotsTinting\":%d,\"slotsWithoutInstance\":%d,"
		"\"slotsWithoutParameter\":%d,\"slotsPaletteUnbound\":%d,\"tintSpecValues\":\"%s\","
		"\"note\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *JsonEscape(WA->ModelName), *JsonEscape(Actor->GetActorLabel()),
		Idx, RowsMin, RowsMax, DistinctRows, RowDuplicateOf, *JsonStrings(PaletteNames),
		SlotsTotal, SlotsUpdated, SlotsTinting, SlotsWithoutInstance, SlotsWithoutParameter, SlotsPaletteUnbound,
		*JsonEscape(WA->WeaponMeta.FindRef(TEXT("TintSpecValues"))),
		SlotsTinting > 0
			? TEXT("material instances are per mesh, so every placed copy of this weapon shows the tint")
			: TEXT("the index landed but every slot has TintAmount 0 - the import left the lookup off (a non-opaque shader, or a diffuse with no alpha to index with)"));
}
