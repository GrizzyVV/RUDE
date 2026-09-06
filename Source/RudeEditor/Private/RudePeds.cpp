// RUDE - RAGE <-> Unreal Development Environment
//
// THE PED LANE (WP10 draft, 2026-09-06). A GTA V ped is not one drawable. It is a SKELETON in a fragment
// (<ped>.yft: <Fragment><Drawable><Skeleton><Bones>, 106 bones on a_m_m_business_01), a DICTIONARY of skinned
// component drawables (<ped>.ydd, entries named <comp>_<ddd>_<class>), a texture dictionary of variations
// (<ped>.ytd, <comp>_diff_<ddd>_<letter>_<race>) and a variation matrix (<ped>.ymt, CPedVariationInfo) that
// says which component slots exist, how many drawables each has and how many texture letters each drawable
// has. This lane builds one USkeleton, one skinned USkeletalMesh per dictionary entry, one URudePedOutfit
// holding the matrix, and a preview actor wearing drawable 0 / letter a of every component.
//
// MEASURED FACTS THIS LANE RESTS ON (scratchpad/wp10/peds/LAWS.md, a_m_m_business_01, 2026-09-06):
//   * BlendIndices are positions in the geometry's own <BoneIDs> table (identity 0..105 in 28/28
//     geometries), NOT bone tags: the max index carrying weight is 104 < 106 bones, while 105/106 tags
//     exceed 105. Hair binds 100% to index 80 = SKEL_Head; trousers reach 28 = SKEL_Spine1.
//   * BlendWeights are 0..255 bytes; the sum is 255 on 27,808/27,808 vertices; 0 vertices are unweighted.
//   * Every model has HasSkin=1 and BoneIndex=0 (24/24); every entry carries High/Medium/Low groups (8/8).
//     Like every other RUDE lane this one imports DrawableModelsHigh only.
//   * Bone <Scale> is unit 106/106; exactly one root (SKEL_ROOT); parents precede children 106/106.
//   * The ydd's shader binds texture letter a (8/8 DiffuseSampler names end _a_<race>); letters b/c swap
//     the diffuse only, normal/spec are per drawable.
// The GTA->UE bone frame map is the vehicle lane's proven one (RudeVehicle.cpp: FORWARD local TRS, plain
// mirror x,-y,z / -qx,qy,-qz,qw). The mesh vertices go through ImportDrawableNode's (x*100, -y*100, z*100),
// so bind pose and skin land in one space.
//
// MATERIALS ARE NOT REBUILT HERE. Per entry the shared ImportDrawableNode (RudeToolsetInternal.h) first
// produces a static twin under <ped>/_static/ - its material instances go through the existing preset /
// RenderBucket / texture-scope path - and the skeletal mesh borrows the twin's material per slot name.
//
// NOT in v1: Medium/Low LOD groups, cloth, the heads' own <Skeleton>, pedprops (<ped>_p.ydd), expressions,
// the peds.ymt row (movement sets, audio). Every one of those is a counted absence, not a silent one.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudePedOutfit.h"

#include "Animation/Skeleton.h"
#include "Animation/SkeletalMeshActor.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SkeletalMeshAttributes.h"
#include "SkinWeightsAttributesRef.h"
#include "UObject/Package.h"
#include "XmlFile.h"

namespace RudePeds
{
	// availComp slot order. 0 head, 2 hair, 3 uppr, 4 lowr, 8 accs, 9 task MEASURED on two peds (drawable
	// counts + compInfos audio ids); 1 berd, 5 hand, 6 feet, 7 teef, 10 decl, 11 jbib are the conventional
	// order - no ped read so far fills those slots (LAWS.md law 6).
	static const TCHAR* kSlots[12] = { TEXT("head"), TEXT("berd"), TEXT("hair"), TEXT("uppr"), TEXT("lowr"), TEXT("hand"),
	                                    TEXT("feet"), TEXT("teef"), TEXT("accs"), TEXT("task"), TEXT("decl"), TEXT("jbib") };

	static int32 SlotIndex(const FString& Comp)
	{
		for (int32 i = 0; i < 12; ++i) { if (Comp == kSlots[i]) { return i; } }
		return -1;
	}

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

	static FString NodeText(const FXmlNode* Parent, const TCHAR* Child)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Child) : nullptr;
		return N ? N->GetContent().TrimStartAndEnd() : FString();
	}

	static int32 ValueInt(const FXmlNode* Parent, const TCHAR* Child, int32 Def)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Child) : nullptr;
		return N ? FCString::Atoi(*Attr(N, TEXT("value"), *FString::FromInt(Def))) : Def;
	}

	// GTA metres -> UE centimetres with the pinned Y mirror; the vehicle lane's proven bone map
	// (RudeVehicle.cpp GtaToUe: forward orientation, plain mirror - NOT the ymap conjugate).
	static FTransform GtaToUe(const FTransform& G)
	{
		const FQuat Q = G.GetRotation();
		const FVector T = G.GetTranslation();
		return FTransform(FQuat(-Q.X, Q.Y, -Q.Z, Q.W).GetNormalized(),
			FVector(T.X * 100.0, -T.Y * 100.0, T.Z * 100.0), G.GetScale3D());
	}

	static FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	}

	static int32 JsonInt(const FString& Verdict, const TCHAR* Field, int32 Fallback)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Verdict);
		double D = 0;
		return (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->TryGetNumberField(Field, D)) ? (int32)D : Fallback;
	}

	struct FBone
	{
		FString Name;
		int32 Tag = -1;
		int32 Parent = -1;
		FTransform LocalGta = FTransform::Identity;
	};

	// <Drawable><Skeleton><Bones>: name / tag / parent / local TRS. Refuses a degenerate scale and a bone
	// whose parent does not precede it (both laws measured 106/106; a violation is a broken file).
	static bool ReadSkeleton(const FXmlNode* DrawableNode, TArray<FBone>& Out, FString& Err)
	{
		const FXmlNode* Skel = DrawableNode ? DrawableNode->FindChildNode(TEXT("Skeleton")) : nullptr;
		const FXmlNode* BoneList = Skel ? Skel->FindChildNode(TEXT("Bones")) : nullptr;
		if (!BoneList) { Err = TEXT("no <Drawable><Skeleton><Bones> in the fragment"); return false; }
		for (const FXmlNode* It : BoneList->GetChildrenNodes())
		{
			FBone B;
			B.Name = NodeText(It, TEXT("Name"));
			B.Tag = ValueInt(It, TEXT("Tag"), -1);
			B.Parent = ValueInt(It, TEXT("ParentIndex"), -1);
			const FXmlNode* R = It->FindChildNode(TEXT("Rotation"));
			const FQuat Q(FCString::Atod(*Attr(R, TEXT("x"), TEXT("0"))), FCString::Atod(*Attr(R, TEXT("y"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("z"), TEXT("0"))), FCString::Atod(*Attr(R, TEXT("w"), TEXT("1"))));
			const FVector S = Vec3(It->FindChildNode(TEXT("Scale")), FVector::OneVector);
			if (S.GetAbsMin() < UE_KINDA_SMALL_NUMBER)
			{
				Err = FString::Printf(TEXT("bone '%s' has a degenerate <Scale> (%s)"), *B.Name, *S.ToString());
				return false;
			}
			if (B.Name.IsEmpty()) { B.Name = FString::Printf(TEXT("bone_%d"), Out.Num()); }
			B.LocalGta = FTransform(Q.GetNormalized(), Vec3(It->FindChildNode(TEXT("Translation")), FVector::ZeroVector), S);
			Out.Add(MoveTemp(B));
		}
		for (int32 i = 0; i < Out.Num(); ++i)
		{
			if (Out[i].Parent >= i)
			{
				Err = FString::Printf(TEXT("bone %d (%s) names parent %d at or after itself - a forward pass cannot compose this"), i, *Out[i].Name, Out[i].Parent);
				return false;
			}
		}
		if (Out.Num() == 0) { Err = TEXT("<Bones> is empty"); return false; }
		return true;
	}

	// Vertex layout semantic -> token width. Same table as RudeYdr::SemanticWidth (RudeToolset.cpp), with the
	// two skin semantics READ here instead of skipped. Width 0 = unknown = refuse (a guess misaligns the stream).
	static int32 SemanticWidth(const FString& Tag)
	{
		if (Tag == TEXT("Position") || Tag == TEXT("Normal")) return 3;
		if (Tag == TEXT("Colour0") || Tag == TEXT("Colour1") || Tag == TEXT("Tangent")) return 4;
		if (Tag == TEXT("BlendWeights") || Tag == TEXT("BlendIndices")) return 4;
		if (Tag.StartsWith(TEXT("TexCoord"))) return 2;
		return 0;
	}

	struct FSkinVert
	{
		FVector3f P = FVector3f::ZeroVector;
		FVector3f N = FVector3f(0, 0, 1);
		FVector2f UV0 = FVector2f::ZeroVector;
		FVector2f UV1 = FVector2f::ZeroVector;
		FVector4f Col = FVector4f(1, 1, 1, 1);
		uint16 Bone[4] = { 0, 0, 0, 0 };   // skeleton indices (after the <BoneIDs> remap)
		float W[4] = { 0, 0, 0, 0 };
		int32 NumW = 0;
	};

	struct FSkinGeo
	{
		int32 ShaderIndex = 0;
		TArray<FSkinVert> Verts;
		TArray<int32> Indices;
		TArray<int32> BoneIDs;
		bool bHasUV0 = false;
		bool bHasSkin = false;
		int32 Unweighted = 0;             // vertices whose four weights are all zero
		int32 InfluencesOutOfRange = 0;   // weight > 0 on an index outside <BoneIDs> or the skeleton (dropped, counted)
	};

	// The ydr parse rules of RudeYdr::ParseGeometry re-spelled with BlendWeights/BlendIndices kept:
	// ordered semantics, the whole <Data> token stream (FXmlFile flattens lines), the Normal-width rule
	// (3 or 4 tokens, chosen by divisibility + a Colour0 sanity check, GTAV1 breaking a tie toward 3),
	// GTA metres -> UE cm with the Y mirror, normals mirrored, indices verbatim.
	static bool ParseSkinnedGeometry(const FXmlNode* GeoNode, int32 NumBones, FSkinGeo& Out, FString& Error)
	{
		Out.ShaderIndex = ValueInt(GeoNode, TEXT("ShaderIndex"), 0);
		if (const FXmlNode* BI = GeoNode->FindChildNode(TEXT("BoneIDs")))
		{
			TArray<FString> Parts;
			BI->GetContent().ParseIntoArray(Parts, TEXT(","), true);
			for (const FString& P : Parts) { Out.BoneIDs.Add(FCString::Atoi(*P.TrimStartAndEnd())); }
		}
		const FXmlNode* VB = GeoNode->FindChildNode(TEXT("VertexBuffer"));
		const FXmlNode* IB = GeoNode->FindChildNode(TEXT("IndexBuffer"));
		if (!VB || !IB) { Error = TEXT("geometry missing VertexBuffer/IndexBuffer"); return false; }
		TArray<FString> Semantics;
		int32 LineWidth = 0;
		FString LayoutType;
		if (const FXmlNode* Layout = VB->FindChildNode(TEXT("Layout")))
		{
			LayoutType = Layout->GetAttribute(TEXT("type"));
			for (const FXmlNode* Child : Layout->GetChildrenNodes())
			{
				const int32 W = SemanticWidth(Child->GetTag());
				if (W == 0) { Error = FString::Printf(TEXT("unknown vertex semantic '%s' - refusing to misalign"), *Child->GetTag()); return false; }
				if (Child->GetTag() == TEXT("TexCoord0")) { Out.bHasUV0 = true; }
				if (Child->GetTag() == TEXT("BlendWeights")) { Out.bHasSkin = true; }
				Semantics.Add(Child->GetTag());
				LineWidth += W;
			}
		}
		if (LineWidth == 0) { Error = TEXT("empty vertex layout"); return false; }
		const FXmlNode* VData = VB->FindChildNode(TEXT("Data"));
		const FXmlNode* IData = IB->FindChildNode(TEXT("Data"));
		if (!VData || !IData) { Error = TEXT("missing Data payloads"); return false; }
		TArray<FString> Toks;
		VData->GetContent().ParseIntoArrayWS(Toks);
		int32 NormalWidth = 3;
		{
			auto ColourSane = [&](int32 NW) -> bool
			{
				int32 Off = 0;
				for (const FString& Sem : Semantics)
				{
					const int32 W = (Sem == TEXT("Normal")) ? NW : SemanticWidth(Sem);
					if (Sem == TEXT("Colour0") || Sem == TEXT("Colour1"))
					{
						for (int32 k = 0; k < 4; ++k)
						{
							if (!Toks.IsValidIndex(Off + k)) { return false; }
							const FString& T = Toks[Off + k];
							if (T.Contains(TEXT("."))) { return false; }
							const int32 Val = FCString::Atoi(*T);
							if (Val < 0 || Val > 255) { return false; }
						}
						return true;
					}
					Off += W;
				}
				return true;
			};
			const bool bHasNormal = Semantics.Contains(TEXT("Normal"));
			const bool bOk3 = (Toks.Num() % LineWidth == 0) && ColourSane(3);
			const bool bOk4 = bHasNormal && (Toks.Num() % (LineWidth + 1) == 0) && ColourSane(4);
			if (bOk3 && bOk4) { NormalWidth = LayoutType.Equals(TEXT("GTAV1"), ESearchCase::IgnoreCase) ? 3 : 4; }
			else if (bOk4) { NormalWidth = 4; }
			else if (bOk3) { NormalWidth = 3; }
			else
			{
				Error = FString::Printf(TEXT("vertex stream misaligned: %d tokens, layout '%s' sums to %d (+1 tried)"), Toks.Num(), *LayoutType, LineWidth);
				return false;
			}
			if (NormalWidth == 4) { LineWidth += 1; }
		}
		const int32 NumVerts = Toks.Num() / LineWidth;
		Out.Verts.Reserve(NumVerts);
		for (int32 V = 0; V < NumVerts; ++V)
		{
			int32 Off = V * LineWidth;
			FSkinVert SV;
			int32 RawW[4] = { 0, 0, 0, 0 };
			int32 RawI[4] = { 0, 0, 0, 0 };
			FVector3f Pos = FVector3f::ZeroVector, Nrm(0, 0, 1);
			for (const FString& Sem : Semantics)
			{
				const int32 W = (Sem == TEXT("Normal")) ? NormalWidth : SemanticWidth(Sem);
				if (Sem == TEXT("Position")) { Pos = FVector3f(FCString::Atof(*Toks[Off]), FCString::Atof(*Toks[Off + 1]), FCString::Atof(*Toks[Off + 2])); }
				else if (Sem == TEXT("Normal")) { Nrm = FVector3f(FCString::Atof(*Toks[Off]), FCString::Atof(*Toks[Off + 1]), FCString::Atof(*Toks[Off + 2])); }
				else if (Sem == TEXT("TexCoord0")) { SV.UV0 = FVector2f(FCString::Atof(*Toks[Off]), FCString::Atof(*Toks[Off + 1])); }
				else if (Sem == TEXT("TexCoord1")) { SV.UV1 = FVector2f(FCString::Atof(*Toks[Off]), FCString::Atof(*Toks[Off + 1])); }
				else if (Sem == TEXT("Colour0"))
				{
					SV.Col = FVector4f(FCString::Atof(*Toks[Off]) / 255.f, FCString::Atof(*Toks[Off + 1]) / 255.f,
						FCString::Atof(*Toks[Off + 2]) / 255.f, FCString::Atof(*Toks[Off + 3]) / 255.f);
				}
				else if (Sem == TEXT("BlendWeights")) { for (int32 k = 0; k < 4; ++k) { RawW[k] = FCString::Atoi(*Toks[Off + k]); } }
				else if (Sem == TEXT("BlendIndices")) { for (int32 k = 0; k < 4; ++k) { RawI[k] = FCString::Atoi(*Toks[Off + k]); } }
				Off += W;
			}
			SV.P = FVector3f(Pos.X * 100.f, -Pos.Y * 100.f, Pos.Z * 100.f);
			SV.N = FVector3f(Nrm.X, -Nrm.Y, Nrm.Z);
			// Law 4/5: weight bytes -> 0..1; index -> skeleton position through <BoneIDs> (raw when absent);
			// an index outside the table or the skeleton is DROPPED AND COUNTED, never clamped.
			for (int32 k = 0; k < 4; ++k)
			{
				if (RawW[k] <= 0) { continue; }
				int32 Skel = RawI[k];
				if (Out.BoneIDs.Num() > 0)
				{
					if (!Out.BoneIDs.IsValidIndex(RawI[k])) { ++Out.InfluencesOutOfRange; continue; }
					Skel = Out.BoneIDs[RawI[k]];
				}
				if (Skel < 0 || Skel >= NumBones) { ++Out.InfluencesOutOfRange; continue; }
				SV.Bone[SV.NumW] = (uint16)Skel;
				SV.W[SV.NumW] = (float)RawW[k] / 255.f;
				++SV.NumW;
			}
			if (SV.NumW == 0) { ++Out.Unweighted; }
			Out.Verts.Add(MoveTemp(SV));
		}
		TArray<FString> IdxToks;
		IData->GetContent().ParseIntoArrayWS(IdxToks);
		Out.Indices.Reserve(IdxToks.Num());
		for (const FString& T : IdxToks) { Out.Indices.Add(FCString::Atoi(*T)); }
		if (Out.Verts.Num() == 0 || Out.Indices.Num() < 3) { Error = TEXT("no usable vertex/index data"); return false; }
		return true;
	}

	// <comp>_<ddd>_<class> -> parts (false when the name is not in the grammar, e.g. an unresolved hash_).
	static bool SplitEntryName(const FString& Name, FString& OutComp, int32& OutIndex, FString& OutClass)
	{
		TArray<FString> Parts;
		Name.ToLower().ParseIntoArray(Parts, TEXT("_"), true);
		if (Parts.Num() != 3 || SlotIndex(Parts[0]) < 0 || !Parts[1].IsNumeric()) { return false; }
		OutComp = Parts[0]; OutIndex = FCString::Atoi(*Parts[1]); OutClass = Parts[2];
		return true;
	}

	// hash_XXXXXXXX -> the <comp>_<ddd>_<class> whose joaat matches (12 slots x 000..099 x r/u/m = 3,600 hashes).
	// This is how uppr_000_m / lowr_000_m came back on a_m_m_business_01 (ROUT's own name table lacks _m).
	static FString ResolveHashName(const FString& HashName)
	{
		if (HashName.Len() != 13 || !HashName.StartsWith(TEXT("hash_"), ESearchCase::IgnoreCase)) { return FString(); }
		const uint32 Want = FParse::HexNumber(*HashName.Mid(5));
		static const TCHAR* Classes[3] = { TEXT("r"), TEXT("u"), TEXT("m") };
		for (int32 s = 0; s < 12; ++s)
		{
			for (int32 d = 0; d < 100; ++d)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					const FString Cand = FString::Printf(TEXT("%s_%03d_%s"), kSlots[s], d, Classes[c]);
					if (RudeJoaat(Cand) == Want) { return Cand; }
				}
			}
		}
		return FString();
	}

	// The static twin's material for a slot: by NAME first (<preset>__<geoIdx>, ImportDrawableNode's spelling),
	// by index as the fallback, null when the twin failed (the mesh still builds; the slot renders default).
	static UMaterialInterface* SlotMaterial(UStaticMesh* Twin, const FString& SlotName, int32 Index)
	{
		if (!Twin) { return nullptr; }
		const TArray<FStaticMaterial>& Mats = Twin->GetStaticMaterials();
		for (const FStaticMaterial& M : Mats)
		{
			if (M.MaterialSlotName == FName(*SlotName) && M.MaterialInterface) { return M.MaterialInterface; }
		}
		return Mats.IsValidIndex(Index) ? Mats[Index].MaterialInterface : nullptr;
	}

	// Every <Item><Name> anywhere under a node (the ytd's texture names).
	static void CollectItemNames(const FXmlNode* N, TArray<FString>& Out)
	{
		if (!N) { return; }
		if (N->GetTag() == TEXT("Item"))
		{
			const FString Nm = NodeText(N, TEXT("Name"));
			if (!Nm.IsEmpty()) { Out.Add(Nm); }
		}
		for (const FXmlNode* C : N->GetChildrenNodes()) { CollectItemNames(C, Out); }
	}

	struct FVarTex { int32 TexId = 0; int32 Dist = 255; };
	struct FVarDrawable { int32 PropMask = 0; int32 NumAlt = 0; TArray<FVarTex> Tex; };
	struct FVarComp { int32 NumAvailTex = 0; TArray<FVarDrawable> Drawables; };

	struct FImported
	{
		FString EntryName, ResolvedName, Comp, Class;
		int32 Index = -1;
		USkeletalMesh* Mesh = nullptr;
		FString AssetPath;
		int32 Verts = 0, Tris = 0, Unweighted = 0, OutOfRange = 0, TrisOutOfRange = 0, Geos = 0, GeosDropped = 0;
		bool bOwnSkeleton = false;
	};
}

// ---- ImportPed --------------------------------------------------------------------------------
FString URudeToolset::ImportPed(const FString& CorpusRoot, const FString& PedName, const FString& DestFolder)
{
	using namespace RudePeds;

	// ---- 0) the four files, through the ledger when CorpusRoot is a corpus ------------------------
	FString Name = PedName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml")); Name.RemoveFromEnd(TEXT(".yft")); Name.RemoveFromEnd(TEXT(".ydd"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a ped name, e.g. a_m_m_business_01")); }
	FString YftPath = CorpusRoot / (Name + TEXT(".yft.xml"));
	FString YddPath = CorpusRoot / (Name + TEXT(".ydd.xml"));
	FString YtdPath = CorpusRoot / (Name + TEXT(".ytd.xml"));
	FString YmtPath = CorpusRoot / (Name + TEXT(".ymt.xml"));
	if (FRudeCorpus::LooksLikeCorpus(CorpusRoot))
	{
		FString CorpusErr;
		const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
		if (!Corpus.IsValid()) { return Fail(CorpusErr); }
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("yft"), Name)) { YftPath = Corpus->PathOf(*R); }
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("ydd"), Name)) { YddPath = Corpus->PathOf(*R); }
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("ytd"), Name)) { YtdPath = Corpus->PathOf(*R); }
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("ymt"), Name)) { YmtPath = Corpus->PathOf(*R); }
	}
	if (!FPaths::FileExists(YftPath)) { return Fail(FString::Printf(TEXT("no fragment XML at %s - is the name right, and is this a component ped?"), *YftPath)); }
	if (!FPaths::FileExists(YddPath)) { return Fail(FString::Printf(TEXT("no drawable dictionary at %s - a ped without components cannot be dressed"), *YddPath)); }
	const bool bHasYtd = FPaths::FileExists(YtdPath);
	const bool bHasYmt = FPaths::FileExists(YmtPath);
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game/RUDE/Peds")) : DestFolder.TrimStartAndEnd();
	const FString PedFolder = Dest / Name;
	if (!FPackageName::IsValidLongPackageName(PedFolder / Name)) { return Fail(FString::Printf(TEXT("bad content path: %s"), *PedFolder)); }
	TArray<FString> Problems;

	// ---- 1) the skeleton (law 1) --------------------------------------------------------------------
	TArray<FBone> Bones;
	{
		FXmlFile Yft(YftPath);
		if (!Yft.IsValid()) { return Fail(FString::Printf(TEXT("yft XML load failed: %s"), *Yft.GetLastError())); }
		const FXmlNode* Root = Yft.GetRootNode();
		if (!Root || Root->GetTag() != TEXT("Fragment")) { return Fail(TEXT("yft root is not <Fragment>")); }
		FString Err;
		if (!ReadSkeleton(Root->FindChildNode(TEXT("Drawable")), Bones, Err)) { return Fail(Err); }
	}
	FReferenceSkeleton RefSkel;
	{
		FReferenceSkeletonModifier Mod(RefSkel, nullptr);
		TSet<FName> Seen;
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			const FName BN(*Bones[i].Name);
			if (Seen.Contains(BN)) { return Fail(FString::Printf(TEXT("duplicate bone name '%s' (index %d) - UE bones are keyed by name"), *Bones[i].Name, i)); }
			Seen.Add(BN);
			Mod.Add(FMeshBoneInfo(BN, Bones[i].Name, Bones[i].Parent), GtaToUe(Bones[i].LocalGta));
		}
	}

	// ---- 2) the variation matrix (law 6) -----------------------------------------------------------
	TArray<int32> AvailComp;
	TArray<FVarComp> CompData;
	bool bHasTexVar = false, bHasDrawVar = false, bYmtRead = false;
	if (bHasYmt)
	{
		FXmlFile Ymt(YmtPath);
		const FXmlNode* Root = Ymt.IsValid() ? Ymt.GetRootNode() : nullptr;
		if (Root && Root->GetTag() == TEXT("CPedVariationInfo"))
		{
			bYmtRead = true;
			bHasTexVar = Attr(Root->FindChildNode(TEXT("bHasTexVariations")), TEXT("value"), TEXT("false")) == TEXT("true");
			bHasDrawVar = Attr(Root->FindChildNode(TEXT("bHasDrawblVariations")), TEXT("value"), TEXT("false")) == TEXT("true");
			TArray<FString> AC;
			NodeText(Root, TEXT("availComp")).ParseIntoArrayWS(AC);
			for (const FString& A : AC) { AvailComp.Add(FCString::Atoi(*A)); }
			if (const FXmlNode* Comps = Root->FindChildNode(TEXT("aComponentData3")))
			{
				for (const FXmlNode* CI : Comps->GetChildrenNodes())
				{
					if (CI->GetTag() != TEXT("Item")) { continue; }
					FVarComp C;
					C.NumAvailTex = ValueInt(CI, TEXT("numAvailTex"), 0);
					if (const FXmlNode* DL = CI->FindChildNode(TEXT("aDrawblData3")))
					{
						for (const FXmlNode* DI : DL->GetChildrenNodes())
						{
							if (DI->GetTag() != TEXT("Item")) { continue; }
							FVarDrawable D;
							D.PropMask = ValueInt(DI, TEXT("propMask"), 0);
							D.NumAlt = ValueInt(DI, TEXT("numAlternatives"), 0);
							if (const FXmlNode* TL = DI->FindChildNode(TEXT("aTexData")))
							{
								for (const FXmlNode* TI : TL->GetChildrenNodes())
								{
									if (TI->GetTag() != TEXT("Item")) { continue; }
									FVarTex T;
									T.TexId = ValueInt(TI, TEXT("texId"), 0);
									T.Dist = ValueInt(TI, TEXT("distribution"), 255);
									D.Tex.Add(T);
								}
							}
							C.Drawables.Add(MoveTemp(D));
						}
					}
					CompData.Add(MoveTemp(C));
				}
			}
		}
		else { Problems.Add(FString::Printf(TEXT("ymt at %s is not a CPedVariationInfo - the outfit will be built from the dictionary alone"), *YmtPath)); }
	}
	else { Problems.Add(TEXT("no <ped>.ymt - the outfit is built from the dictionary alone (no texture letters)")); }

	// ---- 3) the textures: the ped's ytd into /Game/RUDE/Textures/<ped>/ once (ImportYtd's own landing rule) ----
	const FString TexRoot = TEXT("/Game/RUDE/Textures");
	TArray<FString> YtdNames;
	int32 TexImported = 0;
	if (bHasYtd)
	{
		FXmlFile Ytd(YtdPath);
		if (Ytd.IsValid()) { CollectItemNames(Ytd.GetRootNode(), YtdNames); }
		const bool bAlready = YtdNames.Num() > 0 && FPackageName::DoesPackageExist(TexRoot / Name / YtdNames[0]);
		if (!bAlready)
		{
			const FString V = URudeToolset::ImportYtd(YtdPath, TEXT(""), TexRoot);
			TexImported = JsonInt(V, TEXT("imported"), -1);
			if (!V.Contains(TEXT("\"ok\":true"))) { Problems.Add(FString::Printf(TEXT("ImportYtd: %s"), *V.Left(200))); }
		}
		else { TexImported = -2; }   // -2 = already present, skipped
	}
	else { Problems.Add(TEXT("no <ped>.ytd - materials bind no textures")); }
	FRudeTextureScope Scope;
	Scope.ArchetypeTxd = Name;   // tier 2 of the resolver = /Game/RUDE/Textures/<ped>/

	// ---- 4) the dictionary: one skinned mesh per entry (laws 2, 4, 5) -------------------------------------
	FXmlFile Ydd(YddPath);
	if (!Ydd.IsValid()) { return Fail(FString::Printf(TEXT("ydd XML load failed: %s"), *Ydd.GetLastError())); }
	const FXmlNode* DictRoot = Ydd.GetRootNode();
	if (!DictRoot || DictRoot->GetTag() != TEXT("DrawableDictionary")) { return Fail(TEXT("ydd root is not <DrawableDictionary>")); }

	const FString SkelName = TEXT("SKEL_") + Name;
	UPackage* SkelPkg = CreatePackage(*(PedFolder / SkelName));
	SkelPkg->FullyLoad();
	USkeleton* Skeleton = FindObject<USkeleton>(SkelPkg, *SkelName);
	const bool bNewSkeleton = Skeleton == nullptr;
	if (!Skeleton) { Skeleton = NewObject<USkeleton>(SkelPkg, FName(*SkelName), RF_Public | RF_Standalone); }
	if (!Skeleton) { return Fail(TEXT("NewObject<USkeleton> failed")); }
	bool bPreviewSet = false;

	TArray<FImported> Entries;
	int32 TotalVerts = 0, TotalTris = 0, TotalUnweighted = 0, TotalOutOfRange = 0, TotalTrisOut = 0, Imported = 0, OwnSkeletons = 0;
	for (const FXmlNode* Item : DictRoot->GetChildrenNodes())
	{
		if (Item->GetTag() != TEXT("Item")) { continue; }
		FImported E;
		E.EntryName = NodeText(Item, TEXT("Name"));
		if (E.EntryName.IsEmpty()) { Problems.Add(TEXT("a dictionary entry has no <Name> - skipped")); continue; }
		E.ResolvedName = E.EntryName.ToLower();
		if (E.ResolvedName.StartsWith(TEXT("hash_")))
		{
			const FString R = ResolveHashName(E.ResolvedName);
			if (!R.IsEmpty()) { E.ResolvedName = R; }
			else { Problems.Add(FString::Printf(TEXT("%s: no <comp>_<ddd>_<r|u|m> name hashes to it - imported under the hash name, outside the matrix"), *E.EntryName)); }
		}
		SplitEntryName(E.ResolvedName, E.Comp, E.Index, E.Class);
		E.bOwnSkeleton = Item->FindChildNode(TEXT("Skeleton")) != nullptr;
		if (E.bOwnSkeleton) { ++OwnSkeletons; }
		const FString MeshName = E.ResolvedName;

		// a) materials through the shared drawable lane: a static twin under _static/ carries the instances
		const FString StaticFolder = PedFolder / TEXT("_static");
		const FString TwinVerdict = ImportDrawableNode(Item, MeshName, StaticFolder, &Scope);
		UStaticMesh* Twin = TwinVerdict.Contains(TEXT("\"ok\":true"))
			? LoadObject<UStaticMesh>(nullptr, *(StaticFolder / MeshName + TEXT(".") + MeshName)) : nullptr;
		if (!Twin) { Problems.Add(FString::Printf(TEXT("%s: static twin (materials) failed: %s"), *MeshName, *TwinVerdict.Left(160))); }

		// b) the skinned geometry (High group only, like every RUDE lane)
		TArray<FSkinGeo> Geos;
		if (const FXmlNode* High = Item->FindChildNode(TEXT("DrawableModelsHigh")))
		{
			for (const FXmlNode* ModelItem : High->GetChildrenNodes())
			{
				const FXmlNode* Geometries = ModelItem->FindChildNode(TEXT("Geometries"));
				if (!Geometries) { continue; }
				for (const FXmlNode* GeoItem : Geometries->GetChildrenNodes())
				{
					FSkinGeo G;
					FString Err;
					if (ParseSkinnedGeometry(GeoItem, Bones.Num(), G, Err)) { Geos.Add(MoveTemp(G)); }
					else { ++E.GeosDropped; Problems.Add(FString::Printf(TEXT("%s: geometry dropped - %s"), *MeshName, *Err)); }
				}
			}
		}
		if (Geos.Num() == 0)
		{
			Problems.Add(FString::Printf(TEXT("%s: no geometry survived in DrawableModelsHigh (%d dropped)"), *MeshName, E.GeosDropped));
			Entries.Add(E);
			continue;
		}
		TArray<FString> Presets;
		if (const FXmlNode* SG = Item->FindChildNode(TEXT("ShaderGroup")))
		{
			if (const FXmlNode* Sh = SG->FindChildNode(TEXT("Shaders")))
			{
				for (const FXmlNode* S : Sh->GetChildrenNodes()) { Presets.Add(NodeText(S, TEXT("Name"))); }
			}
		}

		// c) the skeletal mesh asset: reference skeleton, one LOD, mesh description with bones + skin weights
		const FString PkgName = PedFolder / MeshName;
		UPackage* Pkg = CreatePackage(*PkgName);
		Pkg->FullyLoad();
		USkeletalMesh* SK = FindObject<USkeletalMesh>(Pkg, *MeshName);
		if (!SK) { SK = NewObject<USkeletalMesh>(Pkg, FName(*MeshName), RF_Public | RF_Standalone); }
		if (!SK) { Problems.Add(FString::Printf(TEXT("%s: NewObject<USkeletalMesh> failed"), *MeshName)); Entries.Add(E); continue; }
		SK->PreEditChange(nullptr);
		// Reimport-over-existing RESETS prior state (the static lane's rule: stale slot 0 wins otherwise).
		SK->SetNumSourceModels(0);
		SK->GetMaterials().Empty();
		SK->SetRefSkeleton(RefSkel);
		FSkeletalMeshLODInfo& Lod = SK->AddLODInfo();
		// the LOD model slot must exist before the description is committed (measured 2026-09-06: an ensure
		// "LODModels.IsValidIndex(InLODIndex)" from CommitMeshDescription without it - the Interchange factory adds it here)
		SK->GetImportedModel()->LODModels.Empty();
		SK->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
		Lod.BuildSettings.bRecomputeNormals = false;
		Lod.BuildSettings.bRecomputeTangents = true;   // no tangents consumed in v1 (5/28 geometries carry them)
		Lod.BuildSettings.bUseMikkTSpace = true;
		Lod.LODHysteresis = 0.02f;
		FMeshDescription* MD = SK->CreateMeshDescription(0);
		if (!MD) { Problems.Add(FString::Printf(TEXT("%s: CreateMeshDescription failed"), *MeshName)); Entries.Add(E); continue; }
		FSkeletalMeshAttributes A(*MD);
		A.Register();
		A.GetVertexInstanceUVs().SetNumChannels(2);
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			const FBoneID BID = A.CreateBone();
			A.GetBoneNames()[BID] = FName(*Bones[i].Name);
			A.GetBoneParentIndices()[BID] = Bones[i].Parent;
			A.GetBonePoses()[BID] = GtaToUe(Bones[i].LocalGta);   // local, the same value the ref skeleton holds
		}
		TVertexAttributesRef<FVector3f> VertexPositions = A.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> InstNormals = A.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> InstUVs = A.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> InstColors = A.GetVertexInstanceColors();
		TPolygonGroupAttributesRef<FName> GroupSlotNames = A.GetPolygonGroupMaterialSlotNames();
		FSkinWeightsVertexAttributesRef SkinWeights = A.GetVertexSkinWeights();
		TArray<FSkeletalMaterial> Mats;
		for (int32 g = 0; g < Geos.Num(); ++g)
		{
			const FSkinGeo& G = Geos[g];
			const FString Preset = Presets.IsValidIndex(G.ShaderIndex) && !Presets[G.ShaderIndex].IsEmpty() ? Presets[G.ShaderIndex] : FString(TEXT("default"));
			const FString SlotName = FString::Printf(TEXT("%s__%d"), *Preset, g);
			Mats.Add(FSkeletalMaterial(SlotMaterial(Twin, SlotName, g), true, false, FName(*SlotName), FName(*SlotName)));
			const FPolygonGroupID GID = MD->CreatePolygonGroup();
			GroupSlotNames[GID] = FName(*SlotName);
			TArray<FVertexID> VIDs;
			VIDs.Reserve(G.Verts.Num());
			for (const FSkinVert& V : G.Verts)
			{
				const FVertexID VID = MD->CreateVertex();
				VertexPositions[VID] = V.P;
				if (V.NumW > 0)
				{
					SkinWeights.Set(VID, UE::AnimationCore::FBoneWeights::Create(V.Bone, V.W, V.NumW));
				}
				else
				{
					// counted in verticesWithoutWeights; bound to the root so the vertex still moves with the ped
					const uint16 RootBone = 0; const float One = 1.f;
					SkinWeights.Set(VID, UE::AnimationCore::FBoneWeights::Create(&RootBone, &One, 1));
				}
				VIDs.Add(VID);
			}
			for (int32 t = 0; t + 2 < G.Indices.Num(); t += 3)
			{
				const int32 I0 = G.Indices[t], I1 = G.Indices[t + 1], I2 = G.Indices[t + 2];
				if (!VIDs.IsValidIndex(I0) || !VIDs.IsValidIndex(I1) || !VIDs.IsValidIndex(I2)) { ++E.TrisOutOfRange; continue; }
				if (I0 == I1 || I1 == I2 || I0 == I2) { continue; }
				TArray<FVertexInstanceID> Inst;
				const int32 Corner[3] = { I0, I1, I2 };
				for (int32 c = 0; c < 3; ++c)
				{
					const FSkinVert& V = G.Verts[Corner[c]];
					const FVertexInstanceID IID = MD->CreateVertexInstance(VIDs[Corner[c]]);
					InstNormals[IID] = V.N;
					InstUVs.Set(IID, 0, V.UV0);
					InstUVs.Set(IID, 1, V.UV1);
					InstColors[IID] = V.Col;
					Inst.Add(IID);
				}
				MD->CreateTriangle(GID, Inst);
				++E.Tris;
			}
			E.Verts += G.Verts.Num();
			E.Unweighted += G.Unweighted;
			E.OutOfRange += G.InfluencesOutOfRange;
		}
		E.Geos = Geos.Num();
		SK->SetMaterials(Mats);
		SK->CommitMeshDescription(0);
		SK->CalculateInvRefMatrices();
		SK->SetSkeleton(Skeleton);
		if (!Skeleton->MergeAllBonesToBoneTree(SK, false)) { Problems.Add(FString::Printf(TEXT("%s: the skeleton refused to merge this mesh's bones"), *MeshName)); }
		SK->InvalidateDeriveDataCacheGUID();
		SK->Build();
		SK->PostEditChange();
		SK->MarkPackageDirty();
		if (!bPreviewSet) { Skeleton->SetPreviewMesh(SK, true); bPreviewSet = true; }
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			ARM.Get().AssetCreated(SK);
		}
		E.Mesh = SK;
		E.AssetPath = PkgName;
		++Imported;
		TotalVerts += E.Verts; TotalTris += E.Tris; TotalUnweighted += E.Unweighted; TotalOutOfRange += E.OutOfRange; TotalTrisOut += E.TrisOutOfRange;
		Entries.Add(E);
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	Skeleton->MarkPackageDirty();
	if (bNewSkeleton)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(Skeleton);
	}
	int32 BuiltSections = 0;
	for (const FImported& E : Entries)
	{
		if (E.Mesh && E.Mesh->GetImportedModel() && E.Mesh->GetImportedModel()->LODModels.Num() > 0)
		{
			BuiltSections += E.Mesh->GetImportedModel()->LODModels[0].Sections.Num();
		}
	}

	// ---- 5) the outfit: the matrix joined against the dictionary and the ytd (law 3 + 6) ----------------
	const FString OutfitName = Name + TEXT("_outfit");
	UPackage* OPkg = CreatePackage(*(PedFolder / OutfitName));
	OPkg->FullyLoad();
	URudePedOutfit* Outfit = FindObject<URudePedOutfit>(OPkg, *OutfitName);
	const bool bNewOutfit = Outfit == nullptr;
	if (!Outfit) { Outfit = NewObject<URudePedOutfit>(OPkg, FName(*OutfitName), RF_Public | RF_Standalone); }
	if (!Outfit) { return Fail(TEXT("NewObject<URudePedOutfit> failed")); }
	Outfit->PedName = Name;
	Outfit->Skeleton = Skeleton;
	Outfit->BoneCount = Bones.Num();
	Outfit->BoneTags.Reset();
	for (const auto& B : Bones) { Outfit->BoneTags.Add(FName(*B.Name), B.Tag); }
	Outfit->AvailComp = AvailComp;
	Outfit->bHasTexVariations = bHasTexVar;
	Outfit->bHasDrawblVariations = bHasDrawVar;
	Outfit->SourceYft = YftPath; Outfit->SourceYdd = YddPath; Outfit->SourceYtd = bHasYtd ? YtdPath : FString(); Outfit->SourceYmt = bYmtRead ? YmtPath : FString();
	Outfit->Components.Reset();
	auto FindEntry = [&](const FString& Comp, int32 Index) -> FImported*
	{
		for (FImported& E : Entries) { if (E.Comp == Comp && E.Index == Index) { return &E; } }
		return nullptr;
	};
	auto FillDrawable = [&](FRudePedDrawable& D, const FImported& E)
	{
		D.Name = E.ResolvedName; D.Class = E.Class; D.Mesh = E.Mesh;
		D.Vertices = E.Verts; D.Triangles = E.Tris; D.VerticesWithoutWeights = E.Unweighted;
	};
	int32 DrawablesInMatrix = 0, DrawablesResolved = 0, TexturesInMatrix = 0, TexturesResolved = 0;
	if (bYmtRead)
	{
		for (int32 s = 0; s < AvailComp.Num() && s < 12; ++s)
		{
			const int32 ci = AvailComp[s];
			if (ci == 255 || !CompData.IsValidIndex(ci)) { continue; }
			FRudePedComponent C;
			C.ComponentIndex = s;
			C.Slot = kSlots[s];
			C.NumAvailTex = CompData[ci].NumAvailTex;
			int32 TexRows = 0;
			for (int32 d = 0; d < CompData[ci].Drawables.Num(); ++d)
			{
				const FVarDrawable& VD = CompData[ci].Drawables[d];
				FRudePedDrawable D;
				D.DrawableIndex = d;
				D.PropMask = VD.PropMask;
				D.NumAlternatives = VD.NumAlt;
				++DrawablesInMatrix;
				if (const FImported* E = FindEntry(C.Slot, d)) { FillDrawable(D, *E); ++DrawablesResolved; }
				else
				{
					D.Name = FString::Printf(TEXT("%s_%03d_?"), *C.Slot, d);
					Problems.Add(FString::Printf(TEXT("matrix names %s drawable %d but the dictionary has no such entry"), *C.Slot, d));
				}
				for (int32 t = 0; t < VD.Tex.Num(); ++t)
				{
					FRudePedTexture T;
					T.Letter = FString::Chr((TCHAR)(TEXT('a') + t));
					T.TexId = VD.Tex[t].TexId;
					T.Distribution = VD.Tex[t].Dist;
					++TexturesInMatrix; ++TexRows;
					const FString Prefix = FString::Printf(TEXT("%s_diff_%03d_%s_"), *C.Slot, d, *T.Letter);
					for (const FString& N : YtdNames) { if (N.StartsWith(Prefix, ESearchCase::IgnoreCase)) { T.TextureName = N; break; } }
					if (!T.TextureName.IsEmpty())
					{
						++TexturesResolved;
						T.Texture = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TexRoot / Name / T.TextureName + TEXT(".") + T.TextureName));
					}
					D.Textures.Add(T);
				}
				C.Drawables.Add(MoveTemp(D));
			}
			if (TexRows != C.NumAvailTex) { Problems.Add(FString::Printf(TEXT("%s: numAvailTex %d != %d texture rows"), *C.Slot, C.NumAvailTex, TexRows)); }
			Outfit->Components.Add(MoveTemp(C));
		}
	}
	else
	{
		// no matrix: one component per slot the dictionary spells, drawables in index order, no letters
		for (int32 s = 0; s < 12; ++s)
		{
			FRudePedComponent C;
			C.ComponentIndex = s;
			C.Slot = kSlots[s];
			for (int32 d = 0; d < 100; ++d)
			{
				if (const FImported* E = FindEntry(C.Slot, d)) { FRudePedDrawable D; D.DrawableIndex = d; FillDrawable(D, *E); C.Drawables.Add(MoveTemp(D)); }
			}
			if (C.Drawables.Num() > 0) { Outfit->Components.Add(MoveTemp(C)); }
		}
	}
	Outfit->MarkPackageDirty();
	if (bNewOutfit)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(Outfit);
	}

	// ---- 6) the preview actor: drawable 0 of every component, letter a (what the ydd's shader binds) -------
	FString ActorLabel;
	int32 PartsWorn = 0;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		const FName PedTag(*(TEXT("RUDE_PED:") + Name));
		TArray<AActor*> Old;
		for (TActorIterator<AActor> It(World); It; ++It) { if (It->Tags.Contains(PedTag)) { Old.Add(*It); } }
		for (AActor* O : Old) { World->DestroyActor(O); }
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, SP))
		{
			Actor->SetActorLabel(TEXT("PED_") + Name);
			Actor->Tags.Add(FName(TEXT("RUDE_PED")));
			Actor->Tags.Add(PedTag);
			Actor->SetFolderPath(FName(TEXT("RUDE_PEDS")));
			USkeletalMeshComponent* Lead = Actor->GetSkeletalMeshComponent();
			for (const FRudePedComponent& C : Outfit->Components)
			{
				if (C.Drawables.Num() == 0) { continue; }
				USkeletalMesh* M = C.Drawables[0].Mesh.Get();
				if (!M) { continue; }
				if (PartsWorn == 0 && Lead) { Lead->SetSkeletalMeshAsset(M); }
				else
				{
					USkeletalMeshComponent* Part = NewObject<USkeletalMeshComponent>(Actor, FName(*FString::Printf(TEXT("Part_%s"), *C.Slot)));
					Part->SetSkeletalMeshAsset(M);
					Part->SetupAttachment(Lead ? static_cast<USceneComponent*>(Lead) : Actor->GetRootComponent());
					if (Lead) { Part->SetLeaderPoseComponent(Lead); }
					Part->RegisterComponent();
					Actor->AddInstanceComponent(Part);
				}
				++PartsWorn;
			}
			Actor->MarkPackageDirty();
			ActorLabel = Actor->GetActorLabel();
		}
		else { Problems.Add(TEXT("SpawnActor<ASkeletalMeshActor> failed")); }
	}
	else { Problems.Add(TEXT("no editor world - assets built, no preview actor")); }

	// ---- 7) the verdict: every drop has a counter ---------------------------------------------------
	FString MeshesJson, ProblemsJson;
	for (const FImported& E : Entries)
	{
		MeshesJson += FString::Printf(TEXT("%s{\"entry\":\"%s\",\"name\":\"%s\",\"comp\":\"%s\",\"index\":%d,\"class\":\"%s\",\"asset\":\"%s\",\"geometries\":%d,\"geometriesDropped\":%d,\"vertices\":%d,\"triangles\":%d,\"unweighted\":%d,\"influencesOutOfRange\":%d,\"ownSkeleton\":%s}"),
			MeshesJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(E.EntryName), *RudeJsonEscape(E.ResolvedName), *E.Comp, E.Index, *E.Class,
			*RudeJsonEscape(E.AssetPath), E.Geos, E.GeosDropped, E.Verts, E.Tris, E.Unweighted, E.OutOfRange, E.bOwnSkeleton ? TEXT("true") : TEXT("false"));
	}
	for (const FString& P : Problems) { ProblemsJson += FString::Printf(TEXT("%s\"%s\""), ProblemsJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(P)); }
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ped\":\"%s\",\"bones\":%d,\"boneRoot\":\"%s\",\"components\":%d,\"drawables\":%d,\"drawablesImported\":%d,")
		TEXT("\"drawablesInMatrix\":%d,\"drawablesResolved\":%d,\"entriesWithOwnSkeleton\":%d,\"textures\":%d,\"texturesImported\":%d,")
		TEXT("\"texturesInMatrix\":%d,\"texturesResolved\":%d,\"skinnedVertices\":%d,\"verticesWithoutWeights\":%d,\"influencesOutOfRange\":%d,")
		TEXT("\"trianglesOutOfRange\":%d,\"triangles\":%d,\"builtSections\":%d,\"skeleton\":\"%s\",\"outfit\":\"%s\",\"actor\":\"%s\",\"partsWorn\":%d,")
		TEXT("\"meshes\":[%s],\"problems\":[%s]}"),
		Imported > 0 ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), Bones.Num(), *RudeJsonEscape(Bones[0].Name), Outfit->Components.Num(), Entries.Num(), Imported,
		DrawablesInMatrix, DrawablesResolved, OwnSkeletons, YtdNames.Num(), TexImported,
		TexturesInMatrix, TexturesResolved, TotalVerts, TotalUnweighted, TotalOutOfRange,
		TotalTrisOut, TotalTris, BuiltSections, *RudeJsonEscape(PedFolder / SkelName), *RudeJsonEscape(PedFolder / OutfitName), *RudeJsonEscape(ActorLabel), PartsWorn,
		*MeshesJson, *ProblemsJson);
}
