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
// MEASURED FACTS THIS LANE RESTS ON (maintainer lane `peds` (`LAWS.md`), a_m_m_business_01, 2026-09-06):
//   * BlendIndices are positions in the geometry's own <BoneIDs> table (identity 0..105 in 28/28
//     geometries), NOT bone tags: the max index carrying weight is 104 < 106 bones, while 105/106 tags
//     exceed 105. Hair binds 100% to index 80 = SKEL_Head; trousers reach 28 = SKEL_Spine1.
//   * BlendWeights are 0..255 bytes; the sum is 255 on 27,808/27,808 vertices; 0 vertices are unweighted.
//   * Every model has HasSkin=1 and BoneIndex=0 (24/24); every entry carries High/Medium/Low groups (8/8).
//     Since WP12 (RUDE_PEDLOD) ALL of them are imported: High -> LOD0, Medium -> LOD1, Low -> LOD2
//     (823/1,152 corpus entries carry three groups, 182 two, 147 one; 0 carry a VeryLow group).
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
// NOT in v1: cloth, the heads' own <Skeleton>, expressions,
// (Medium/Low LOD groups ARE in since WP12 - RUDE_PEDLOD regions below; laws in maintainer lane `ped_lods` (`LAWS.md`))
// (pedprops <ped>_p.ydd ARE in since WP11 - RUDE_PEDPROPS regions below; laws in maintainer lane `pedprops` (`LAWS.md`))
// the peds.ymt row (movement sets, audio). Every one of those is a counted absence, not a silent one.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudePedOutfit.h"

#include "Animation/Skeleton.h"
#include "Animation/SkeletalMeshActor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"   // RUDE_PEDPROPS: props ride the bones as static-mesh components
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

	// RUDE_PEDLOD_BEGIN helpers
	// ---- the LOD groups (WP12, maintainer lane `ped_lods` (`LAWS.md`)) ----
	// A component ped ships up to THREE groups per entry: High + Medium + Low on 823/1,152 corpus entries and
	// 1,570/2,125 game binary entries, High + Medium on 182 / 383, High alone on 147 / 172. NO measured entry
	// carries a VeryLow group (0/1,152 and 0/2,125), so RUDE reads three and writes three.
	static const TCHAR* kPedLodGroupTag[3] = { TEXT("DrawableModelsHigh"), TEXT("DrawableModelsMedium"), TEXT("DrawableModelsLow") };

	// UE screen sizes for LOD0/1/2. INFERRED, and tagged as such wherever it is quoted: the ydd's four lodDist
	// floats are a CONSTANT 9998 on 2,119/2,125 game entries and 1,148/1,152 corpus entries (law 2), so the file
	// stores no switch distance to convert. These are fitted to the measured vertex ratios (Medium/High mean
	// 0.358, Low/High mean 0.083 over 1,953 / 1,570 entries) and are an EDITOR PREVIEW setting only - nothing in
	// the exported .ydd depends on them.
	static const float kPedLodScreenSize[3] = { 1.0f, 0.4f, 0.15f };

	// One LOD group's geometries out of a dictionary entry. Returns how many were DROPPED (each with a reason);
	// a missing group is simply an absent group, not a failure.
	static int32 ParseLodGroup(const FXmlNode* Item, int32 GroupIdx, int32 NumBones, TArray<FSkinGeo>& Out,
	                           TArray<FString>& Problems, const FString& MeshName)
	{
		int32 Dropped = 0;
		const FXmlNode* Grp = Item->FindChildNode(kPedLodGroupTag[GroupIdx]);
		if (!Grp) { return 0; }
		for (const FXmlNode* ModelItem : Grp->GetChildrenNodes())
		{
			const FXmlNode* Geometries = ModelItem->FindChildNode(TEXT("Geometries"));
			if (!Geometries) { continue; }
			for (const FXmlNode* GeoItem : Geometries->GetChildrenNodes())
			{
				FSkinGeo G;
				FString Err;
				if (ParseSkinnedGeometry(GeoItem, NumBones, G, Err)) { Out.Add(MoveTemp(G)); }
				else { ++Dropped; Problems.Add(FString::Printf(TEXT("%s: %s geometry dropped - %s"), *MeshName, kPedLodGroupTag[GroupIdx], *Err)); }
			}
		}
		return Dropped;
	}

	// The four <LodDist*> floats AS SPELLED (law 2.3: carried, never computed; 9998 when the entry omits one).
	static void ReadLodDist(const FXmlNode* Item, float Out[4])
	{
		static const TCHAR* Tags[4] = { TEXT("LodDistHigh"), TEXT("LodDistMed"), TEXT("LodDistLow"), TEXT("LodDistVlow") };
		for (int32 k = 0; k < 4; ++k)
		{
			Out[k] = 9998.f;
			if (const FXmlNode* N = Item->FindChildNode(Tags[k]))
			{
				const FString V = N->GetAttribute(TEXT("value"));
				if (!V.IsEmpty()) { Out[k] = FCString::Atof(*V); }
			}
		}
	}
	// RUDE_PEDLOD_END helpers

	struct FImported
	{
		FString EntryName, ResolvedName, Comp, Class;
		int32 Index = -1;
		USkeletalMesh* Mesh = nullptr;
		FString AssetPath;
		int32 Verts = 0, Tris = 0, Unweighted = 0, OutOfRange = 0, TrisOutOfRange = 0, Geos = 0, GeosDropped = 0;
		int32 TexBound = -1, TexMissing = -1;   // the twin's material binding, surfaced per mesh (2026-09-06)
		// RUDE_PEDLOD: the LOD groups the entry shipped, index 0 = High. EVERY counter that existed before this
		// change stays HIGH-only - Verts, Tris, Geos, GeosDropped, Unweighted, OutOfRange and TrisOutOfRange all
		// still measure the High group alone, so a caller reading them (and the file-level totals they feed) gets
		// the same quantity it got before. What the Medium / Low groups add is counted in the Lod* twins below.
		int32 LodGroups = 1, LodSlotsClamped = 0, LodGroupRenumbered = 0;
		int32 LodVerts[3] = { 0, 0, 0 }, LodTris[3] = { 0, 0, 0 }, LodGeoCount[3] = { 0, 0, 0 };
		int32 LodUeIndex[3] = { 0, -1, -1 };   // the UE LOD each SOURCE group landed on (-1 = the group was absent)
		int32 LodGeosDropped = 0, LodUnweighted = 0, LodOutOfRange = 0, LodTrisOutOfRange = 0;
		float LodDist[4] = { 9998.f, 9998.f, 9998.f, 9998.f };
		bool bOwnSkeleton = false;
	};
	// RUDE_PEDPROPS_BEGIN helpers
	// ---- ped props (WP11 lane, maintainer lane `pedprops` (`LAWS.md`)) ----
	// The only anchor ids the game's data spells (1,169/1,169 aAnchors rows over 709 peds) and the yft bone each rides.
	// id -> enumerant -> entry word are MEASURED (aAnchors in ascending anchorId order on every ped; propId sets equal
	// the entry ddd sets 1,169/1,169); the BONE per anchor is RUDE's table (the game's own is code, not data) - a
	// skeleton without it is counted (anchorsUnmapped), never guessed.
	struct FPropAnchor { int32 Id; const TCHAR* Word; const TCHAR* Enum; const TCHAR* Bone; };
	static const FPropAnchor kPropAnchors[5] =
	{
		{ 0, TEXT("head"),   TEXT("ANCHOR_HEAD"),        TEXT("SKEL_Head") },
		{ 1, TEXT("eyes"),   TEXT("ANCHOR_EYES"),        TEXT("SKEL_Head") },
		{ 2, TEXT("ears"),   TEXT("ANCHOR_EARS"),        TEXT("SKEL_Head") },
		{ 6, TEXT("lwrist"), TEXT("ANCHOR_LEFT_WRIST"),  TEXT("SKEL_L_Hand") },
		{ 7, TEXT("rwrist"), TEXT("ANCHOR_RIGHT_WRIST"), TEXT("SKEL_R_Hand") },
	};
	static const FPropAnchor* AnchorById(int32 Id)
	{
		for (const FPropAnchor& A : kPropAnchors) { if (A.Id == Id) { return &A; } }
		return nullptr;
	}
	static const FPropAnchor* AnchorByWord(const FString& Word)
	{
		for (const FPropAnchor& A : kPropAnchors) { if (Word.Equals(A.Word, ESearchCase::IgnoreCase)) { return &A; } }
		return nullptr;
	}
	// "head" / "p_head" / "ANCHOR_HEAD" / "0" -> the row (nullptr when none of the five)
	static const FPropAnchor* AnchorByText(const FString& Text)
	{
		const FString T = Text.TrimStartAndEnd().ToLower();
		if (T.IsEmpty()) { return nullptr; }
		if (T.IsNumeric()) { return AnchorById(FCString::Atoi(*T)); }
		for (const FPropAnchor& A : kPropAnchors)
		{
			if (T == FString(A.Word).ToLower() || T == FString(A.Enum).ToLower() || T == FString(TEXT("p_")) + A.Word) { return &A; }
		}
		return nullptr;
	}
	// p_<anchor>_<ddd> -> parts (1,763/1,763 entries measured spell this)
	static bool SplitPropName(const FString& Name, FString& OutAnchorWord, int32& OutIndex)
	{
		TArray<FString> Parts;
		Name.ToLower().ParseIntoArray(Parts, TEXT("_"), true);
		if (Parts.Num() != 3 || Parts[0] != TEXT("p") || !Parts[2].IsNumeric()) { return false; }
		OutAnchorWord = Parts[1]; OutIndex = FCString::Atoi(*Parts[2]);
		return true;
	}
	struct FPropMetaTex { int32 TexId = 0; int32 Dist = 255; };
	struct FPropMeta { int32 AnchorId = -1; int32 PropId = -1; int32 PropFlags = 0; int32 Flags = 0; FString AudioId; TArray<FPropMetaTex> Tex; };
	struct FPropAnchorRow { FString Enum; TArray<int32> Props; };
	// <propInfo>: numAvailProps, aPropMetaData rows (anchorId / propId / texData / propFlags / flags / audioId), aAnchors
	// rows (enumerant + per-prop texture counts). Re-opens the ymt (small). False when it is not a CPedVariationInfo.
	static bool ReadPropInfo(const FString& YmtPath, int32& OutNumAvail, TArray<FPropMeta>& OutRows, TArray<FPropAnchorRow>& OutAnchors)
	{
		FXmlFile Ymt(YmtPath);
		const FXmlNode* Root = Ymt.IsValid() ? Ymt.GetRootNode() : nullptr;
		if (!Root || Root->GetTag() != TEXT("CPedVariationInfo")) { return false; }
		const FXmlNode* PropIx = Root->FindChildNode(TEXT("propInfo"));
		if (!PropIx) { return true; }
		OutNumAvail = ValueInt(PropIx, TEXT("numAvailProps"), 0);
		if (const FXmlNode* MD = PropIx->FindChildNode(TEXT("aPropMetaData")))
		{
			for (const FXmlNode* It : MD->GetChildrenNodes())
			{
				if (It->GetTag() != TEXT("Item")) { continue; }
				FPropMeta M;
				M.AnchorId = ValueInt(It, TEXT("anchorId"), -1); M.PropId = ValueInt(It, TEXT("propId"), -1);
				M.PropFlags = ValueInt(It, TEXT("propFlags"), 0); M.Flags = ValueInt(It, TEXT("flags"), 0);
				M.AudioId = NodeText(It, TEXT("audioId"));
				if (const FXmlNode* TD = It->FindChildNode(TEXT("texData")))
				{
					for (const FXmlNode* T : TD->GetChildrenNodes())
					{
						if (T->GetTag() != TEXT("Item")) { continue; }
						FPropMetaTex X; X.TexId = ValueInt(T, TEXT("texId"), 0); X.Dist = ValueInt(T, TEXT("distribution"), 255);
						M.Tex.Add(X);
					}
				}
				OutRows.Add(MoveTemp(M));
			}
		}
		if (const FXmlNode* AN = PropIx->FindChildNode(TEXT("aAnchors")))
		{
			for (const FXmlNode* It : AN->GetChildrenNodes())
			{
				if (It->GetTag() != TEXT("Item")) { continue; }
				FPropAnchorRow R; R.Enum = NodeText(It, TEXT("anchor"));
				TArray<FString> P; NodeText(It, TEXT("props")).ParseIntoArrayWS(P);
				for (const FString& S : P) { R.Props.Add(FCString::Atoi(*S)); }
				OutAnchors.Add(MoveTemp(R));
			}
		}
		return true;
	}
	struct FPropImported
	{
		FString EntryName, AnchorWord; int32 Index = -1;
		UStaticMesh* Mesh = nullptr; FString AssetPath;
		int32 Verts = 0, Tris = 0, Geos = 0, SkinnedGeos = 0, TexBound = -1, TexMissing = -1;
		TArray<FString> Presets;
		bool bNamed = false, bOwnSkeleton = false;
	};
	// Geometry count of the High group and how many carry BlendWeights (= a skinned prop; none measured, 977/977 rigid).
	static void CountPropGeometry(const FXmlNode* Item, FPropImported& P)
	{
		const FXmlNode* High = Item->FindChildNode(TEXT("DrawableModelsHigh"));
		if (!High) { return; }
		for (const FXmlNode* M : High->GetChildrenNodes())
		{
			const FXmlNode* Gs = M->FindChildNode(TEXT("Geometries"));
			if (!Gs) { continue; }
			for (const FXmlNode* G : Gs->GetChildrenNodes())
			{
				++P.Geos;
				const FXmlNode* VB = G->FindChildNode(TEXT("VertexBuffer"));
				const FXmlNode* L = VB ? VB->FindChildNode(TEXT("Layout")) : nullptr;
				if (L && L->FindChildNode(TEXT("BlendWeights"))) { ++P.SkinnedGeos; }
			}
		}
	}
	// One HIDDEN static-mesh component on the prop's anchor bone. Props are modeled in ped axes with the origin at the
	// bone (LAWS.md law 7: hats extend +Z while the head bone's local X is world-up), so the relative rotation is the
	// bone's component-space bind rotation INVERTED and the translation zero. Null when the bone is not on the rig.
	static UStaticMeshComponent* AttachPropComponent(AActor* Actor, USkeletalMeshComponent* Lead, const FReferenceSkeleton& RS, const FRudePedProp& P, UStaticMesh* PM)
	{
		if (!Actor || !Lead || !PM || P.AnchorBone.IsNone()) { return nullptr; }
		const int32 BI = RS.FindBoneIndex(P.AnchorBone);
		if (BI < 0) { return nullptr; }
		FTransform CS = FTransform::Identity;
		for (int32 b = BI; b >= 0; b = RS.GetParentIndex(b)) { CS = CS * RS.GetRefBonePose()[b]; }   // child local first, then each parent
		UStaticMeshComponent* PC = NewObject<UStaticMeshComponent>(Actor, FName(*FString::Printf(TEXT("Prop_%s_%03d"), *P.Anchor, P.PropIndex)));
		PC->SetStaticMesh(PM);
		PC->SetupAttachment(Lead, P.AnchorBone);
		PC->SetRelativeTransform(FTransform(CS.GetRotation().Inverse()));
		PC->SetVisibility(false);
		PC->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_PEDPROP:%d:%d"), P.AnchorId, P.PropIndex)));
		PC->RegisterComponent();
		Actor->AddInstanceComponent(PC);
		return PC;
	}
	// RUDE_PEDPROPS_END helpers
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
	// RUDE_PEDPROPS_BEGIN paths
	FString PropYddPath = CorpusRoot / (Name + TEXT("_p.ydd.xml"));   // the prop dictionary (pedprops.rpf), when the ped has one
	FString PropYtdPath = CorpusRoot / (Name + TEXT("_p.ytd.xml"));
	// RUDE_PEDPROPS_END paths
	if (FRudeCorpus::LooksLikeCorpus(CorpusRoot))
	{
		FString CorpusErr;
		const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
		if (!Corpus.IsValid()) { return Fail(CorpusErr); }
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("yft"), Name)) { YftPath = Corpus->PathOf(*R); }
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("ydd"), Name)) { YddPath = Corpus->PathOf(*R); }
		if (const FRudeCorpusEntry* R = Corpus->EffectiveWithSidecar(TEXT("ytd"), Name)) { YtdPath = Corpus->PathOf(*R); }   // the copy WITH pixels
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("ymt"), Name)) { YmtPath = Corpus->PathOf(*R); }
		// RUDE_PEDPROPS_BEGIN corpus
		if (const FRudeCorpusEntry* R = Corpus->Effective(TEXT("ydd"), Name + TEXT("_p"))) { PropYddPath = Corpus->PathOf(*R); }
		if (const FRudeCorpusEntry* R = Corpus->EffectiveWithSidecar(TEXT("ytd"), Name + TEXT("_p"))) { PropYtdPath = Corpus->PathOf(*R); }
		// RUDE_PEDPROPS_END corpus
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
		// The twin is PED-QUALIFIED (2026-09-06): the shared lane keys its material instances by mesh name
		// (/Game/RUDE/Materials/Instances/<mesh>/MI_<mesh>_<geo>), and every ped spells lowr_000_m - so two
		// peds overwrote one MI, and a stale on-disk MI_lowr_000_m_0 (dangling textures) loaded over the fresh
		// one: lowr exported with no diffuse while the import had bound 3/3. Unique per ped now.
		const FString TwinName = Name + TEXT("__") + E.ResolvedName;

		// a) materials through the shared drawable lane: a static twin under _static/ carries the instances
		const FString StaticFolder = PedFolder / TEXT("_static");
		const FString TwinVerdict = ImportDrawableNode(Item, TwinName, StaticFolder, &Scope);
		E.TexBound = JsonInt(TwinVerdict, TEXT("boundTextures"), -1); E.TexMissing = JsonInt(TwinVerdict, TEXT("missingTextures"), -1);
		UStaticMesh* Twin = TwinVerdict.Contains(TEXT("\"ok\":true"))
			? LoadObject<UStaticMesh>(nullptr, *(StaticFolder / TwinName + TEXT(".") + TwinName)) : nullptr;
		if (!Twin) { Problems.Add(FString::Printf(TEXT("%s: static twin (materials) failed: %s"), *MeshName, *TwinVerdict.Left(160))); }

		// b) the skinned geometry, ONE LOD PER GROUP (RUDE_PEDLOD): DrawableModelsHigh -> LOD0,
		// DrawableModelsMedium -> LOD1, DrawableModelsLow -> LOD2 (maintainer lane `ped_lods` (`LAWS.md`) law 9.3).
		// An absent group is skipped WITHOUT renumbering, which is a shape the game itself ships (High+Medium on
		// 383/2,125 game entries). VeryLow is never read: 0/1,152 corpus and 0/2,125 game entries carry one.
		TArray<FSkinGeo> LodGeos[3];
		E.GeosDropped += ParseLodGroup(Item, 0, Bones.Num(), LodGeos[0], Problems, MeshName);   // HIGH-only, as before
		for (int32 lg = 1; lg < 3; ++lg) { E.LodGeosDropped += ParseLodGroup(Item, lg, Bones.Num(), LodGeos[lg], Problems, MeshName); }
		ReadLodDist(Item, E.LodDist);
		TArray<FSkinGeo>& Geos = LodGeos[0];
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
		// RUDE_PEDLOD: LOD1 / LOD2 from the Medium / Low groups. The skinning, the layout and the bone semantics
		// are IDENTICAL to High in the game's own files (maintainer lane `ped_lods` (`LAWS.md`) laws 5.1-5.5), so
		// this is the same fill against a different geometry list. A LOD geometry re-uses the High group's
		// material slot BY ORDINAL (law 9.4: the counts match on 1,665/1,953 measured Medium groups); a surplus
		// LOD geometry clamps onto the last slot and is COUNTED, never silently dropped.
		E.LodVerts[0] = E.Verts; E.LodTris[0] = E.Tris; E.LodGeoCount[0] = Geos.Num();
		if (FSkeletalMeshLODInfo* Lod0 = SK->GetLODInfo(0)) { Lod0->ScreenSize.Default = kPedLodScreenSize[0]; }
		for (int32 lg = 1; lg < 3; ++lg)
		{
			if (LodGeos[lg].Num() == 0) { continue; }
			// The UE LOD index is the next free slot, so an ABSENT lower group does not leave a hole. The counts
			// below stay indexed by SOURCE group (that is what the corpus comparison reads), and the UE slot each
			// group landed on is reported beside them - a divergence (only possible for a High+Low entry, a shape
			// the corpus never ships: 0/1,152 and 0/2,125) is COUNTED and named, never silent, because on export
			// UE LOD1 becomes DrawableModelsMedium (law 9.3).
			const int32 LodIdx = SK->GetLODNum();
			E.LodUeIndex[lg] = LodIdx;
			if (LodIdx != lg)
			{
				++E.LodGroupRenumbered;
				Problems.Add(FString::Printf(TEXT("%s: %s has no lower group before it, so it becomes UE LOD%d and would export as %s"),
					*MeshName, kPedLodGroupTag[lg], LodIdx, kPedLodGroupTag[LodIdx]));
			}
			FSkeletalMeshLODInfo& LodInfo = SK->AddLODInfo();
			// the LOD model slot must exist before the description is committed (the same law LOD0 rests on)
			SK->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
			LodInfo.BuildSettings.bRecomputeNormals = false;
			LodInfo.BuildSettings.bRecomputeTangents = true;
			LodInfo.BuildSettings.bUseMikkTSpace = true;
			LodInfo.LODHysteresis = 0.02f;
			LodInfo.ScreenSize.Default = kPedLodScreenSize[lg];   // INFERRED (law 9.2), editor preview only
			FMeshDescription* LodMD = SK->CreateMeshDescription(LodIdx);
			if (!LodMD)
			{
				Problems.Add(FString::Printf(TEXT("%s: CreateMeshDescription(%d) failed - %s not built"), *MeshName, LodIdx, kPedLodGroupTag[lg]));
				break;
			}
			FSkeletalMeshAttributes LodA(*LodMD);
			LodA.Register();
			LodA.GetVertexInstanceUVs().SetNumChannels(2);
			for (int32 b = 0; b < Bones.Num(); ++b)
			{
				const FBoneID LodBID = LodA.CreateBone();
				LodA.GetBoneNames()[LodBID] = FName(*Bones[b].Name);
				LodA.GetBoneParentIndices()[LodBID] = Bones[b].Parent;
				LodA.GetBonePoses()[LodBID] = GtaToUe(Bones[b].LocalGta);
			}
			TVertexAttributesRef<FVector3f> LodPositions = LodA.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> LodNormals = LodA.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> LodUVs = LodA.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> LodColors = LodA.GetVertexInstanceColors();
			TPolygonGroupAttributesRef<FName> LodSlotNames = LodA.GetPolygonGroupMaterialSlotNames();
			FSkinWeightsVertexAttributesRef LodSkin = LodA.GetVertexSkinWeights();
			int32 LodV = 0, LodT = 0;
			for (int32 lgi = 0; lgi < LodGeos[lg].Num(); ++lgi)
			{
				const FSkinGeo& LG = LodGeos[lg][lgi];
				if (lgi >= Mats.Num()) { ++E.LodSlotsClamped; }
				const int32 LodSlotIdx = FMath::Clamp(lgi, 0, FMath::Max(0, Mats.Num() - 1));
				const FName LodSlotName = Mats.IsValidIndex(LodSlotIdx) ? Mats[LodSlotIdx].MaterialSlotName : FName(TEXT("default__0"));
				const FPolygonGroupID LodGID = LodMD->CreatePolygonGroup();
				LodSlotNames[LodGID] = LodSlotName;
				TArray<FVertexID> LodVIDs;
				LodVIDs.Reserve(LG.Verts.Num());
				for (const FSkinVert& LV : LG.Verts)
				{
					const FVertexID LodVID = LodMD->CreateVertex();
					LodPositions[LodVID] = LV.P;
					if (LV.NumW > 0)
					{
						LodSkin.Set(LodVID, UE::AnimationCore::FBoneWeights::Create(LV.Bone, LV.W, LV.NumW));
					}
					else
					{
						const uint16 LodRootBone = 0; const float LodOne = 1.f;
						LodSkin.Set(LodVID, UE::AnimationCore::FBoneWeights::Create(&LodRootBone, &LodOne, 1));
					}
					LodVIDs.Add(LodVID);
				}
				for (int32 lt = 0; lt + 2 < LG.Indices.Num(); lt += 3)
				{
					const int32 J0 = LG.Indices[lt], J1 = LG.Indices[lt + 1], J2 = LG.Indices[lt + 2];
					if (!LodVIDs.IsValidIndex(J0) || !LodVIDs.IsValidIndex(J1) || !LodVIDs.IsValidIndex(J2)) { ++E.LodTrisOutOfRange; continue; }
					if (J0 == J1 || J1 == J2 || J0 == J2) { continue; }
					TArray<FVertexInstanceID> LodInst;
					const int32 LodCorner[3] = { J0, J1, J2 };
					for (int32 c = 0; c < 3; ++c)
					{
						const FSkinVert& LV = LG.Verts[LodCorner[c]];
						const FVertexInstanceID LodIID = LodMD->CreateVertexInstance(LodVIDs[LodCorner[c]]);
						LodNormals[LodIID] = LV.N;
						LodUVs.Set(LodIID, 0, LV.UV0);
						LodUVs.Set(LodIID, 1, LV.UV1);
						LodColors[LodIID] = LV.Col;
						LodInst.Add(LodIID);
					}
					LodMD->CreateTriangle(LodGID, LodInst);
					++LodT;
				}
				LodV += LG.Verts.Num();
				E.LodUnweighted += LG.Unweighted;             // RUDE_PEDLOD: NOT E.Unweighted - that stays HIGH-only
				E.LodOutOfRange += LG.InfluencesOutOfRange;   // (the outfit's VerticesWithoutWeights and the file totals read it)
			}
			SK->CommitMeshDescription(LodIdx);
			E.LodVerts[lg] = LodV; E.LodTris[lg] = LodT; E.LodGeoCount[lg] = LodGeos[lg].Num();
			++E.LodGroups;
		}
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

	// RUDE_PEDPROPS_BEGIN import
	// ---- 4b) the props: <ped>_p.ydd entries (RIGID, 977/977 measured) as static meshes under <ped>/props/, <ped>_p.ytd once ----
	// LAWS: maintainer lane `pedprops` (`LAWS.md.`) A streamed ped (mp_m_freemode_01, cs_*) keeps one ydd per prop in a
	// <ped>_p/ folder the corpus cannot address by name - counted here as an absence, not read.
	const bool bHasPropYdd = FPaths::FileExists(PropYddPath);
	const bool bHasPropYtd = FPaths::FileExists(PropYtdPath);
	int32 NumAvailProps = 0; TArray<FPropMeta> PropRows; TArray<FPropAnchorRow> PropAnchorRows;
	if (bYmtRead) { ReadPropInfo(YmtPath, NumAvailProps, PropRows, PropAnchorRows); }
	if (!bHasPropYdd && PropRows.Num() > 0) { Problems.Add(FString::Printf(TEXT("the ymt lists %d props but no %s_p.ydd resolves (a streamed ped keeps one ydd per prop in a %s_p/ folder - not read in v1)"), PropRows.Num(), *Name, *Name)); }
	TArray<FString> PropYtdNames;
	int32 PropTexImported = 0;
	if (bHasPropYtd)
	{
		FXmlFile PYtd(PropYtdPath);
		if (PYtd.IsValid()) { CollectItemNames(PYtd.GetRootNode(), PropYtdNames); }
		const bool bAlready = PropYtdNames.Num() > 0 && FPackageName::DoesPackageExist(TexRoot / (Name + TEXT("_p")) / PropYtdNames[0]);
		if (!bAlready)
		{
			const FString V = URudeToolset::ImportYtd(PropYtdPath, TEXT(""), TexRoot);
			PropTexImported = JsonInt(V, TEXT("imported"), -1);
			if (!V.Contains(TEXT("\"ok\":true"))) { Problems.Add(FString::Printf(TEXT("ImportYtd (%s_p): %s"), *Name, *V.Left(200))); }
		}
		else { PropTexImported = -2; }   // -2 = already present, skipped
	}
	else if (bHasPropYdd) { Problems.Add(FString::Printf(TEXT("no %s_p.ytd - prop materials bind no textures"), *Name)); }
	TArray<FPropImported> PropEntries;
	int32 PropsImported = 0, PropsSkinned = 0, PropsUnnamed = 0, PropVerts = 0, PropTris = 0;
	if (bHasPropYdd)
	{
		FXmlFile PYdd(PropYddPath);
		const FXmlNode* PRoot = PYdd.IsValid() ? PYdd.GetRootNode() : nullptr;
		if (!PRoot || PRoot->GetTag() != TEXT("DrawableDictionary")) { Problems.Add(FString::Printf(TEXT("%s: not a <DrawableDictionary> - props skipped"), *PropYddPath)); }
		else
		{
			FRudeTextureScope PropScope;
			PropScope.ArchetypeTxd = Name + TEXT("_p");   // tier 2 = /Game/RUDE/Textures/<ped>_p/ (ImportYtd's landing for the prop dictionary)
			const FString PropFolder = PedFolder / TEXT("props");
			for (const FXmlNode* Item : PRoot->GetChildrenNodes())
			{
				if (Item->GetTag() != TEXT("Item")) { continue; }
				FPropImported P;
				P.EntryName = NodeText(Item, TEXT("Name")).ToLower();
				if (P.EntryName.IsEmpty()) { Problems.Add(TEXT("a prop entry has no <Name> - skipped")); continue; }
				P.bNamed = SplitPropName(P.EntryName, P.AnchorWord, P.Index);
				if (!P.bNamed) { ++PropsUnnamed; Problems.Add(FString::Printf(TEXT("%s: not p_<anchor>_<ddd> - imported outside the prop matrix"), *P.EntryName)); }
				P.bOwnSkeleton = Item->FindChildNode(TEXT("Skeleton")) != nullptr;
				CountPropGeometry(Item, P);
				if (P.SkinnedGeos > 0)
				{
					// no skinned prop exists in the measured game data (977/977 rigid): a counted refusal, never a silent static import
					++PropsSkinned;
					Problems.Add(FString::Printf(TEXT("%s: %d/%d geometries carry BlendWeights - skinned props are not imported in v1"), *P.EntryName, P.SkinnedGeos, P.Geos));
					PropEntries.Add(P);
					continue;
				}
				if (const FXmlNode* SG = Item->FindChildNode(TEXT("ShaderGroup")))
				{
					if (const FXmlNode* Sh = SG->FindChildNode(TEXT("Shaders")))
					{
						for (const FXmlNode* S : Sh->GetChildrenNodes()) { P.Presets.Add(NodeText(S, TEXT("Name"))); }
					}
				}
				const FString MeshName = Name + TEXT("__") + P.EntryName;   // ped-qualified like the component twins (MI keys are per mesh name)
				const FString V = ImportDrawableNode(Item, MeshName, PropFolder, &PropScope);
				P.TexBound = JsonInt(V, TEXT("boundTextures"), -1); P.TexMissing = JsonInt(V, TEXT("missingTextures"), -1);
				if (V.Contains(TEXT("\"ok\":true"))) { P.Mesh = LoadObject<UStaticMesh>(nullptr, *(PropFolder / MeshName + TEXT(".") + MeshName)); }
				if (!P.Mesh) { Problems.Add(FString::Printf(TEXT("%s: prop import failed: %s"), *P.EntryName, *V.Left(160))); PropEntries.Add(P); continue; }
				P.AssetPath = PropFolder / MeshName;
				if (const FMeshDescription* PMD = P.Mesh->GetMeshDescription(0)) { P.Verts = PMD->Vertices().Num(); P.Tris = PMD->Triangles().Num(); }
				PropVerts += P.Verts; PropTris += P.Tris;
				++PropsImported;
				PropEntries.Add(P);
			}
		}
	}
	// RUDE_PEDPROPS_END import

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
		// RUDE_PEDLOD: the entry's LOD groups and its four lodDist floats, so ExportYddBinary re-emits the
		// entry's OWN values instead of the modal 9998 (maintainer lane `ped_lods` (`LAWS.md`) law 2.3).
		D.LodGroups = E.LodGroups;
		D.LodDist.Empty(); D.LodVertices.Empty(); D.LodTriangles.Empty();
		for (int32 k = 0; k < 4; ++k) { D.LodDist.Add(E.LodDist[k]); }
		for (int32 k = 0; k < 3; ++k) { D.LodVertices.Add(E.LodVerts[k]); D.LodTriangles.Add(E.LodTris[k]); }
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
	// RUDE_PEDPROPS_BEGIN outfit
	// ---- 5b) the prop matrix: propInfo rows joined against the prop dictionary and <ped>_p.ytd (LAWS.md laws 6, 8) ----
	Outfit->Props.Reset();
	Outfit->NumAvailProps = NumAvailProps;
	Outfit->SourcePropYdd = bHasPropYdd ? PropYddPath : FString();
	Outfit->SourcePropYtd = bHasPropYtd ? PropYtdPath : FString();
	int32 PropsInMatrix = 0, PropsResolved = 0, PropTexInMatrix = 0, PropTexResolved = 0, AnchorsUnmapped = 0, PropsOutsideMatrix = 0;
	{
		TSet<FString> Placed;
		auto AddProp = [&](const FPropAnchor* A, int32 AnchorId, int32 PropId, const FPropMeta* Meta, const FPropImported* E)
		{
			FRudePedProp P;
			P.AnchorId = AnchorId; P.PropIndex = PropId;
			P.Anchor = A ? FString(A->Word) : (E ? E->AnchorWord : FString::Printf(TEXT("anchor%d"), AnchorId));
			P.AnchorBone = A ? FName(A->Bone) : NAME_None;
			if (!A || !Outfit->BoneTags.Contains(P.AnchorBone))
			{
				++AnchorsUnmapped;
				Problems.Add(FString::Printf(TEXT("prop anchor %d (%s): no bone to ride (%s)"), AnchorId, *P.Anchor, A ? A->Bone : TEXT("not one of the five anchor ids the game's data spells")));
				P.AnchorBone = NAME_None;
			}
			if (Meta)
			{
				P.PropFlags = Meta->PropFlags; P.Flags = Meta->Flags; P.AudioId = Meta->AudioId;
				for (int32 t = 0; t < Meta->Tex.Num(); ++t)
				{
					FRudePedTexture T;
					T.Letter = FString::Chr((TCHAR)(TEXT('a') + t));
					T.TexId = Meta->Tex[t].TexId; T.Distribution = Meta->Tex[t].Dist;
					++PropTexInMatrix;
					// p_<anchor>_diff_<ddd>_<letter> (3,230/6,670 names; no race suffix; MIXED case inside one file)
					const FString Want = FString::Printf(TEXT("p_%s_diff_%03d_%s"), *P.Anchor, PropId, *T.Letter);
					for (const FString& N : PropYtdNames) { if (N.Equals(Want, ESearchCase::IgnoreCase) || N.StartsWith(Want + TEXT("_"), ESearchCase::IgnoreCase)) { T.TextureName = N; break; } }
					if (!T.TextureName.IsEmpty())
					{
						++PropTexResolved;
						T.Texture = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TexRoot / (Name + TEXT("_p")) / T.TextureName + TEXT(".") + T.TextureName));
					}
					P.Textures.Add(T);
				}
			}
			if (E) { P.Name = E->EntryName; P.Mesh = E->Mesh; P.Vertices = E->Verts; P.Triangles = E->Tris; P.ShaderPresets = E->Presets; }
			else { P.Name = FString::Printf(TEXT("p_%s_%03d_?"), *P.Anchor, PropId); }
			Outfit->Props.Add(MoveTemp(P));
		};
		auto FindProp = [&](const FString& Word, int32 Index) -> const FPropImported*
		{
			for (const FPropImported& E : PropEntries) { if (E.bNamed && E.AnchorWord == Word && E.Index == Index) { return &E; } }
			return nullptr;
		};
		for (const FPropMeta& M : PropRows)
		{
			++PropsInMatrix;
			const FPropAnchor* A = AnchorById(M.AnchorId);
			const FPropImported* E = A ? FindProp(A->Word, M.PropId) : nullptr;
			if (E && E->Mesh) { ++PropsResolved; }
			else if (bHasPropYdd) { Problems.Add(FString::Printf(TEXT("prop matrix names anchor %d prop %d but the dictionary has no imported p_%s_%03d"), M.AnchorId, M.PropId, A ? A->Word : TEXT("?"), M.PropId)); }
			AddProp(A, M.AnchorId, M.PropId, &M, E);
			Placed.Add(FString::Printf(TEXT("%d:%d"), M.AnchorId, M.PropId));
		}
		// dictionary entries the matrix does not list (or no matrix at all): carried, no letters
		for (const FPropImported& E : PropEntries)
		{
			if (!E.Mesh) { continue; }
			const FPropAnchor* A = AnchorByWord(E.AnchorWord);
			const int32 AnchorId = A ? A->Id : -1;
			if (Placed.Contains(FString::Printf(TEXT("%d:%d"), AnchorId, E.Index))) { continue; }
			++PropsOutsideMatrix;
			AddProp(A, AnchorId, E.Index, nullptr, &E);
		}
		for (const FPropAnchorRow& R : PropAnchorRows)
		{
			// aAnchors.props = per-prop texture counts in propId order (1,169/1,169 measured) - a disagreement is a broken file
			const FPropAnchor* A = nullptr;
			for (const FPropAnchor& X : kPropAnchors) { if (R.Enum == X.Enum) { A = &X; } }
			if (!A) { Problems.Add(FString::Printf(TEXT("aAnchors names %s - not one of the five anchors the game's data spells"), *R.Enum)); continue; }
			int32 Rows = 0;
			for (const FPropMeta& M : PropRows) { if (M.AnchorId == A->Id) { ++Rows; } }
			if (Rows != R.Props.Num()) { Problems.Add(FString::Printf(TEXT("%s: aAnchors lists %d props, aPropMetaData has %d rows"), *R.Enum, R.Props.Num(), Rows)); }
		}
	}
	// RUDE_PEDPROPS_END outfit
	Outfit->MarkPackageDirty();
	if (bNewOutfit)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(Outfit);
	}

	// ---- 6) the preview actor: drawable 0 of every component, letter a (what the ydd's shader binds) -------
	FString ActorLabel;
	int32 PartsWorn = 0;
	int32 PropsAttached = 0;   // RUDE_PEDPROPS
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
			// RUDE_PEDPROPS_BEGIN preview
			// ---- 6b) every prop as a HIDDEN static-mesh component on its anchor bone (SetPedProp shows one per anchor) ----
			if (Lead)
			{
				for (const FRudePedProp& P : Outfit->Props)
				{
					UStaticMesh* PM = P.Mesh.Get();
					if (!PM) { continue; }
					if (AttachPropComponent(Actor, Lead, RefSkel, P, PM)) { ++PropsAttached; }
				}
			}
			// RUDE_PEDPROPS_END preview
			Actor->MarkPackageDirty();
			ActorLabel = Actor->GetActorLabel();
		}
		else { Problems.Add(TEXT("SpawnActor<ASkeletalMeshActor> failed")); }
	}
	else { Problems.Add(TEXT("no editor world - assets built, no preview actor")); }

	// ---- 7) the verdict: every drop has a counter ---------------------------------------------------
	FString MeshesJson, ProblemsJson;
	FString PropsJson;   // RUDE_PEDPROPS
	for (const FPropImported& P : PropEntries)
	{
		PropsJson += FString::Printf(TEXT("%s{\"entry\":\"%s\",\"anchor\":\"%s\",\"index\":%d,\"asset\":\"%s\",\"geometries\":%d,\"skinnedGeometries\":%d,\"vertices\":%d,\"triangles\":%d,\"texturesBound\":%d,\"texturesMissing\":%d,\"ownSkeleton\":%s}"),
			PropsJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(P.EntryName), *RudeJsonEscape(P.AnchorWord), P.Index, *RudeJsonEscape(P.AssetPath), P.Geos, P.SkinnedGeos, P.Verts, P.Tris, P.TexBound, P.TexMissing, P.bOwnSkeleton ? TEXT("true") : TEXT("false"));
	}
	for (const FImported& E : Entries)
	{
		MeshesJson += FString::Printf(
			TEXT("%s{\"entry\":\"%s\",\"name\":\"%s\",\"comp\":\"%s\",\"index\":%d,\"class\":\"%s\",\"asset\":\"%s\",\"geometries\":%d,\"geometriesDropped\":%d,\"vertices\":%d,\"triangles\":%d,\"unweighted\":%d,\"influencesOutOfRange\":%d,\"texturesBound\":%d,\"texturesMissing\":%d,\"ownSkeleton\":%s,")
			// RUDE_PEDLOD: `vertices` / `triangles` / `geometries` above stay the HIGH group's; the LOD groups
			// are reported beside them, index 0 = High (maintainer lane `ped_lods` (`LAWS.md`)).
			TEXT("\"lodGroups\":%d,\"lodVertices\":[%d,%d,%d],\"lodTriangles\":[%d,%d,%d],\"lodGeometries\":[%d,%d,%d],\"lodSlotsClamped\":%d,\"lodDist\":[%g,%g,%g,%g],")
			// the LOD groups' OWN problem counters - the four above them (geometriesDropped / unweighted /
			// influencesOutOfRange, and the file-level totals) keep counting the HIGH group alone
			TEXT("\"lodGeometriesDropped\":%d,\"lodUnweighted\":%d,\"lodInfluencesOutOfRange\":%d,\"lodTrianglesOutOfRange\":%d,\"lodUeIndex\":[%d,%d,%d],\"lodGroupRenumbered\":%d}"),
			MeshesJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(E.EntryName), *RudeJsonEscape(E.ResolvedName), *E.Comp, E.Index, *E.Class,
			*RudeJsonEscape(E.AssetPath), E.Geos, E.GeosDropped, E.Verts, E.Tris, E.Unweighted, E.OutOfRange, E.TexBound, E.TexMissing, E.bOwnSkeleton ? TEXT("true") : TEXT("false"),
			E.LodGroups, E.LodVerts[0], E.LodVerts[1], E.LodVerts[2], E.LodTris[0], E.LodTris[1], E.LodTris[2],
			E.LodGeoCount[0], E.LodGeoCount[1], E.LodGeoCount[2], E.LodSlotsClamped, E.LodDist[0], E.LodDist[1], E.LodDist[2], E.LodDist[3],
			E.LodGeosDropped, E.LodUnweighted, E.LodOutOfRange, E.LodTrisOutOfRange, E.LodUeIndex[0], E.LodUeIndex[1], E.LodUeIndex[2], E.LodGroupRenumbered);
	}
	for (const FString& P : Problems) { ProblemsJson += FString::Printf(TEXT("%s\"%s\""), ProblemsJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(P)); }
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ped\":\"%s\",\"bones\":%d,\"boneRoot\":\"%s\",\"components\":%d,\"drawables\":%d,\"drawablesImported\":%d,")
		TEXT("\"drawablesInMatrix\":%d,\"drawablesResolved\":%d,\"entriesWithOwnSkeleton\":%d,\"textures\":%d,\"texturesImported\":%d,")
		TEXT("\"texturesInMatrix\":%d,\"texturesResolved\":%d,\"skinnedVertices\":%d,\"verticesWithoutWeights\":%d,\"influencesOutOfRange\":%d,")
		TEXT("\"trianglesOutOfRange\":%d,\"triangles\":%d,\"builtSections\":%d,\"skeleton\":\"%s\",\"outfit\":\"%s\",\"actor\":\"%s\",\"partsWorn\":%d,")
		TEXT("\"props\":%d,\"propsImported\":%d,\"propsInMatrix\":%d,\"propsResolved\":%d,\"propsOutsideMatrix\":%d,\"propsSkinnedRefused\":%d,\"propsUnnamed\":%d,")
		TEXT("\"propTextures\":%d,\"propTexturesImported\":%d,\"propTexturesInMatrix\":%d,\"propTexturesResolved\":%d,\"anchorsUnmapped\":%d,\"propsAttached\":%d,")
		TEXT("\"propVertices\":%d,\"propTriangles\":%d,\"numAvailProps\":%d,\"propYdd\":\"%s\",\"propEntries\":[%s],")
		TEXT("\"meshes\":[%s],\"problems\":[%s]}"),
		Imported > 0 ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), Bones.Num(), *RudeJsonEscape(Bones[0].Name), Outfit->Components.Num(), Entries.Num(), Imported,
		DrawablesInMatrix, DrawablesResolved, OwnSkeletons, YtdNames.Num(), TexImported,
		TexturesInMatrix, TexturesResolved, TotalVerts, TotalUnweighted, TotalOutOfRange,
		TotalTrisOut, TotalTris, BuiltSections, *RudeJsonEscape(PedFolder / SkelName), *RudeJsonEscape(PedFolder / OutfitName), *RudeJsonEscape(ActorLabel), PartsWorn,
		PropEntries.Num(), PropsImported, PropsInMatrix, PropsResolved, PropsOutsideMatrix, PropsSkinned, PropsUnnamed,
		PropYtdNames.Num(), PropTexImported, PropTexInMatrix, PropTexResolved, AnchorsUnmapped, PropsAttached,
		PropVerts, PropTris, NumAvailProps, *RudeJsonEscape(bHasPropYdd ? PropYddPath : FString()), *PropsJson,
		*MeshesJson, *ProblemsJson);
}

// ---- SetPedOutfit (agent + Matt) ------------------------------------------------------------
// The variation matrix as a surface: put drawable D of component slot S on the ped, wearing texture
// letter L (a..). Drawables are the outfit asset's; the part is the actor's leader component (the first
// worn slot) or its Part_<slot> component; the letter lands on the part's materials as a Diffuse
// override (a dynamic instance, editor preview). Rough by design: the game's own rule is the ymt's
// per-drawable texture list, which the asset carries.
FString URudeToolset::SetPedOutfit(const FString& ActorLabel, const FString& Slot, const FString& DrawableIndex, const FString& TextureLetter)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* A = nullptr;
	FString PedName;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() != ActorLabel.TrimStartAndEnd()) { continue; }
		for (const FName& T : It->Tags) { const FString S = T.ToString(); if (S.StartsWith(TEXT("RUDE_PED:"))) { PedName = S.Mid(9); } }
		if (!PedName.IsEmpty()) { A = *It; break; }
	}
	if (!A) { return Fail(FString::Printf(TEXT("no RUDE ped actor labelled '%s' (ImportPed labels them PED_<name>)"), *ActorLabel)); }
	// the outfit asset: the one whose PedName matches, wherever it was imported
	URudePedOutfit* Outfit = nullptr;
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Assets;
		ARM.Get().GetAssetsByClass(URudePedOutfit::StaticClass()->GetClassPathName(), Assets, true);
		for (const FAssetData& AD : Assets)
		{
			URudePedOutfit* O = Cast<URudePedOutfit>(AD.GetAsset());
			if (O && O->PedName.Equals(PedName, ESearchCase::IgnoreCase)) { Outfit = O; break; }
		}
	}
	if (!Outfit) { return Fail(FString::Printf(TEXT("no URudePedOutfit asset for '%s'"), *PedName)); }
	const FString S = Slot.TrimStartAndEnd().ToLower();
	const FRudePedComponent* Comp = Outfit->Components.FindByPredicate([&](const FRudePedComponent& C) { return C.Slot.ToLower() == S; });
	if (!Comp) { return Fail(FString::Printf(TEXT("the outfit has no component slot '%s'"), *S)); }
	const int32 DI = FCString::Atoi(*DrawableIndex);
	const FRudePedDrawable* Dr = Comp->Drawables.FindByPredicate([&](const FRudePedDrawable& D) { return D.DrawableIndex == DI; });
	if (!Dr) { Dr = Comp->Drawables.IsValidIndex(DI) ? &Comp->Drawables[DI] : nullptr; }
	if (!Dr) { return Fail(FString::Printf(TEXT("slot %s has no drawable %d (%d drawables)"), *S, DI, Comp->Drawables.Num())); }
	USkeletalMesh* M = Dr->Mesh.LoadSynchronous();
	if (!M) { return Fail(FString::Printf(TEXT("drawable %s has no imported mesh"), *Dr->Name)); }
	// the part: the leader wears the FIRST worn slot; the others are Part_<slot>
	ASkeletalMeshActor* SA = Cast<ASkeletalMeshActor>(A);
	USkeletalMeshComponent* Lead = SA ? SA->GetSkeletalMeshComponent() : A->FindComponentByClass<USkeletalMeshComponent>();
	USkeletalMeshComponent* Part = nullptr;
	{
		TArray<USkeletalMeshComponent*> Comps;
		A->GetComponents<USkeletalMeshComponent>(Comps);
		const FString Want = TEXT("Part_") + Comp->Slot;
		for (USkeletalMeshComponent* C : Comps) { if (C->GetName().Equals(Want, ESearchCase::IgnoreCase)) { Part = C; break; } }
		if (!Part)
		{
			// the first worn slot lives on the leader
			for (const FRudePedComponent& C : Outfit->Components)
			{
				if (C.Drawables.Num() == 0 || !C.Drawables[0].Mesh.LoadSynchronous()) { continue; }
				if (C.Slot.ToLower() == S) { Part = Lead; }
				break;
			}
		}
		if (!Part)
		{
			Part = NewObject<USkeletalMeshComponent>(A, FName(*Want));
			Part->SetupAttachment(Lead ? static_cast<USceneComponent*>(Lead) : A->GetRootComponent());
			if (Lead) { Part->SetLeaderPoseComponent(Lead); }
			Part->RegisterComponent();
			A->AddInstanceComponent(Part);
		}
	}
	A->Modify();
	Part->SetSkeletalMeshAsset(M);
	// the texture letter as a Diffuse override on every material slot that has one
	FString Letter = TextureLetter.TrimStartAndEnd().ToLower();
	int32 SlotsOverridden = 0;
	FString TexName;
	if (!Letter.IsEmpty())
	{
		const FRudePedTexture* Tx = Dr->Textures.FindByPredicate([&](const FRudePedTexture& T) { return T.Letter.ToLower() == Letter; });
		if (!Tx) { return Fail(FString::Printf(TEXT("drawable %s has no texture letter '%s' (%d letters)"), *Dr->Name, *Letter, Dr->Textures.Num())); }
		UTexture2D* T = Tx->Texture.LoadSynchronous();
		if (!T) { return Fail(FString::Printf(TEXT("texture %s is not imported (run ImportPed after the corpus has pixels)"), *Tx->TextureName)); }
		TexName = Tx->TextureName;
		for (int32 i = 0; i < Part->GetNumMaterials(); ++i)
		{
			UMaterialInterface* Base = Part->GetMaterial(i);
			if (!Base) { continue; }
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Base);
			if (!MID) { MID = UMaterialInstanceDynamic::Create(Base, A); Part->SetMaterial(i, MID); }
			MID->SetTextureParameterValue(FName(TEXT("Diffuse")), T);
			++SlotsOverridden;
		}
	}
	Part->MarkRenderStateDirty();
	A->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"ped\":\"%s\",\"slot\":\"%s\",\"drawable\":\"%s\",\"mesh\":\"%s\",\"letter\":\"%s\",\"texture\":\"%s\",\"materialSlotsOverridden\":%d,\"part\":\"%s\"}"),
		*RudeJsonEscape(PedName), *RudeJsonEscape(Comp->Slot), *RudeJsonEscape(Dr->Name), *RudeJsonEscape(M->GetPathName()), *RudeJsonEscape(Letter), *RudeJsonEscape(TexName), SlotsOverridden, *RudeJsonEscape(Part->GetName()));
}

// RUDE_PEDPROPS_BEGIN setpedprop
// ---- SetPedProp (agent + Matt) ---------------------------------------------------------------
// The prop matrix as a surface, mirroring SetPedOutfit: prop P of anchor A (head / eyes / ears / lwrist / rwrist,
// an ANCHOR_* enumerant, or the id 0/1/2/6/7) on the imported ped, wearing texture index T (0 = letter a). PropIndex
// -1 = nothing on that anchor (the game's own "no prop"). One prop per anchor, like the game: every other prop
// component of the anchor goes dark. A prop the import did not attach (bone missing) is refused by name.
FString URudeToolset::SetPedProp(const FString& ActorLabel, const FString& Anchor, const FString& PropIndex, const FString& TextureIndex)
{
	using namespace RudePeds;
	auto Bad = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Bad(TEXT("no editor world")); }
	AActor* A = nullptr;
	FString PedName;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() != ActorLabel.TrimStartAndEnd()) { continue; }
		for (const FName& T : It->Tags) { const FString S = T.ToString(); if (S.StartsWith(TEXT("RUDE_PED:"))) { PedName = S.Mid(9); } }
		if (!PedName.IsEmpty()) { A = *It; break; }
	}
	if (!A) { return Bad(FString::Printf(TEXT("no RUDE ped actor labelled '%s' (ImportPed labels them PED_<name>)"), *ActorLabel)); }
	const FPropAnchor* An = AnchorByText(Anchor);
	if (!An) { return Bad(FString::Printf(TEXT("unknown anchor '%s' - head, eyes, ears, lwrist, rwrist (or ANCHOR_HEAD.. / 0,1,2,6,7)"), *Anchor)); }
	URudePedOutfit* Outfit = nullptr;
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Assets;
		ARM.Get().GetAssetsByClass(URudePedOutfit::StaticClass()->GetClassPathName(), Assets, true);
		for (const FAssetData& AD : Assets)
		{
			URudePedOutfit* O = Cast<URudePedOutfit>(AD.GetAsset());
			if (O && O->PedName.Equals(PedName, ESearchCase::IgnoreCase)) { Outfit = O; break; }
		}
	}
	if (!Outfit) { return Bad(FString::Printf(TEXT("no URudePedOutfit asset for '%s'"), *PedName)); }
	const int32 PropIx = PropIndex.TrimStartAndEnd().IsEmpty() ? -1 : FCString::Atoi(*PropIndex);
	ASkeletalMeshActor* SA = Cast<ASkeletalMeshActor>(A);
	USkeletalMeshComponent* Lead = SA ? SA->GetSkeletalMeshComponent() : A->FindComponentByClass<USkeletalMeshComponent>();
	// every prop component of this anchor goes dark; the chosen one shows (created here when the import did not attach it)
	TArray<UStaticMeshComponent*> Comps;
	A->GetComponents<UStaticMeshComponent>(Comps);
	const FString TagPrefix = FString::Printf(TEXT("RUDE_PEDPROP:%d:"), An->Id);
	UStaticMeshComponent* Shown = nullptr;
	int32 Hidden = 0;
	for (UStaticMeshComponent* C : Comps)
	{
		bool bMine = false, bWant = false;
		for (const FName& T : C->ComponentTags)
		{
			const FString S = T.ToString();
			if (!S.StartsWith(TagPrefix)) { continue; }
			bMine = true;
			bWant = FCString::Atoi(*S.Mid(TagPrefix.Len())) == PropIx;
		}
		if (!bMine) { continue; }
		if (bWant) { Shown = C; }
		else if (C->IsVisible()) { C->SetVisibility(false); ++Hidden; }
	}
	A->Modify();
	if (PropIx < 0)
	{
		A->MarkPackageDirty();
		return FString::Printf(TEXT("{\"ok\":true,\"ped\":\"%s\",\"anchor\":\"%s\",\"anchorId\":%d,\"prop\":-1,\"hidden\":%d,\"component\":\"\"}"),
			*RudeJsonEscape(PedName), An->Word, An->Id, Hidden);
	}
	const FRudePedProp* Pr = Outfit->Props.FindByPredicate([&](const FRudePedProp& P) { return P.AnchorId == An->Id && P.PropIndex == PropIx; });
	if (!Pr) { return Bad(FString::Printf(TEXT("anchor %s has no prop %d in the outfit (%d props total)"), An->Word, PropIx, Outfit->Props.Num())); }
	UStaticMesh* PM = Pr->Mesh.LoadSynchronous();
	if (!PM) { return Bad(FString::Printf(TEXT("prop %s has no imported mesh"), *Pr->Name)); }
	if (!Shown)
	{
		if (!Lead || !Lead->GetSkeletalMeshAsset()) { return Bad(TEXT("the ped actor has no skeletal mesh to hang a prop on")); }
		Shown = AttachPropComponent(A, Lead, Lead->GetSkeletalMeshAsset()->GetRefSkeleton(), *Pr, PM);
		if (!Shown) { return Bad(FString::Printf(TEXT("prop %s: anchor bone '%s' is not on this skeleton (anchorsUnmapped at import)"), *Pr->Name, *Pr->AnchorBone.ToString())); }
	}
	Shown->SetStaticMesh(PM);
	Shown->SetVisibility(true);
	// the texture index as a Diffuse override on every material slot (dynamic instances, editor preview)
	const int32 TI = TextureIndex.TrimStartAndEnd().IsEmpty() ? -1 : FCString::Atoi(*TextureIndex);
	FString TexName;
	int32 SlotsOverridden = 0;
	if (TI >= 0)
	{
		if (!Pr->Textures.IsValidIndex(TI)) { return Bad(FString::Printf(TEXT("prop %s has no texture %d (%d letters)"), *Pr->Name, TI, Pr->Textures.Num())); }
		UTexture2D* T = Pr->Textures[TI].Texture.LoadSynchronous();
		if (!T) { return Bad(FString::Printf(TEXT("texture %s is not imported (the corpus has no pixels for %s_p.ytd yet)"), *Pr->Textures[TI].TextureName, *PedName)); }
		TexName = Pr->Textures[TI].TextureName;
		for (int32 i = 0; i < Shown->GetNumMaterials(); ++i)
		{
			UMaterialInterface* Base = Shown->GetMaterial(i);
			if (!Base) { continue; }
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Base);
			if (!MID) { MID = UMaterialInstanceDynamic::Create(Base, A); Shown->SetMaterial(i, MID); }
			MID->SetTextureParameterValue(FName(TEXT("Diffuse")), T);
			++SlotsOverridden;
		}
	}
	Shown->MarkRenderStateDirty();
	A->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"ped\":\"%s\",\"anchor\":\"%s\",\"anchorId\":%d,\"bone\":\"%s\",\"prop\":%d,\"entry\":\"%s\",\"mesh\":\"%s\",\"vertices\":%d,\"triangles\":%d,\"texture\":%d,\"textureName\":\"%s\",\"materialSlotsOverridden\":%d,\"hidden\":%d,\"component\":\"%s\"}"),
		*RudeJsonEscape(PedName), An->Word, An->Id, *Pr->AnchorBone.ToString(), PropIx, *RudeJsonEscape(Pr->Name), *RudeJsonEscape(PM->GetPathName()), Pr->Vertices, Pr->Triangles,
		TI, *RudeJsonEscape(TexName), SlotsOverridden, Hidden, *RudeJsonEscape(Shown->GetName()));
}
// RUDE_PEDPROPS_END setpedprop

// ---- ExportPedReplace (agent + Matt) --------------------------------------------------------
// GDD "custom clothing", the path that needs NO variation-table writer: a REPLACE resource for one ped.
// Every drawable the outfit knows (all slots, all indices, the High mesh) goes into stream/<ped>.ydd under
// the game's own entry names (joaat(<comp>_<ddd>_<class>) = the game's hash), every texture imported from
// the ped's dictionary goes into stream/<ped>.ytd under its own name, plus fxmanifest.lua. Streamed, the
// pair shadows the game's files by name - edit one part in UE, export, the ped wears it. Since WP12
// (RUDE_PEDLOD) every LOD group the meshes carry is written, and each drawable's four lodDist floats ride
// through to the writer as LODDIST= (maintainer lane `ped_lods` (`LAWS.md`) law 2.3).
FString URudeToolset::ExportPedReplace(const FString& OutfitAssetPath, const FString& OutDir, const FString& Options)
{
	using namespace RudePeds;
	auto Bad = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	FString Path = OutfitAssetPath.TrimStartAndEnd();
	if (Path.IsEmpty()) { return Bad(TEXT("give the outfit asset (/Game/RUDE/Peds/<ped>/<ped>_outfit) or the ped name")); }
	if (!Path.StartsWith(TEXT("/"))) { Path = FString::Printf(TEXT("/Game/RUDE/Peds/%s/%s_outfit"), *Path, *Path); }
	if (!Path.Contains(TEXT("."))) { Path += TEXT(".") + FPackageName::GetShortName(Path); }
	URudePedOutfit* Outfit = LoadObject<URudePedOutfit>(nullptr, *Path);
	if (!Outfit) { return Bad(FString::Printf(TEXT("outfit asset not found: %s (ImportPed builds it)"), *Path)); }
	const FString Ped = Outfit->PedName.IsEmpty() ? FPackageName::GetShortName(Path).Replace(TEXT("_outfit"), TEXT("")) : Outfit->PedName;
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Bad(TEXT("OutDir is empty")); }
	const FString StreamDir = OutDir / TEXT("stream");
	IFileManager::Get().MakeDirectory(*StreamDir, true);

	// 1) the dictionary: every drawable with a mesh, under the game's own entry name
	TArray<FString> Problems;
	FString Paths, Names;
	// RUDE_PEDLOD: the four <LodDist*> floats each drawable shipped, in the SAME order as Paths / Names, handed to
	// ExportYddBinary as `LODDIST=a/b/c/d,...`. ImportPed read them off the entry (maintainer lane `ped_lods`
	// (`LAWS.md`) law 2.3) and the outfit carried them; this is the last leg, so the exported entry re-emits its
	// OWN values. A drawable the outfit has no floats for contributes an EMPTY group and the writer falls back to
	// the measured modal 9998 x4 - which is what 1,148/1,152 corpus entries spell anyway.
	FString LodSpec;
	int32 Drawables = 0, DrawablesWithoutMesh = 0, DrawablesWithLodDist = 0;
	for (const FRudePedComponent& C : Outfit->Components)
	{
		for (const FRudePedDrawable& D : C.Drawables)
		{
			const FString MeshPath = D.Mesh.ToSoftObjectPath().ToString();
			if (MeshPath.IsEmpty() || D.Name.IsEmpty() || D.Name.EndsWith(TEXT("_?"))) { ++DrawablesWithoutMesh; continue; }
			Paths += (Paths.IsEmpty() ? TEXT("") : TEXT(",")) + MeshPath;
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(",")) + D.Name;
			LodSpec += Drawables ? TEXT(",") : TEXT("");
			if (D.LodDist.Num() == 4)
			{
				LodSpec += FString::Printf(TEXT("%g/%g/%g/%g"), (double)D.LodDist[0], (double)D.LodDist[1], (double)D.LodDist[2], (double)D.LodDist[3]);
				++DrawablesWithLodDist;
			}
			++Drawables;
		}
	}
	if (Drawables == 0) { return Bad(TEXT("the outfit names no drawable with a mesh")); }
	const FString YddPath = StreamDir / (Ped + TEXT(".ydd"));
	// RUDE_PEDLOD: the caller's Options are kept verbatim and the lodDist token is APPENDED for the component
	// dictionary only - the prop dictionary below is rigid, always one group, and passes the untouched Options,
	// so its bytes are unchanged.
	FString YddOptions = Options.TrimStartAndEnd();
	if (DrawablesWithLodDist > 0) { YddOptions += (YddOptions.IsEmpty() ? TEXT("") : TEXT(";")) + FString(TEXT("LODDIST=")) + LodSpec; }
	const FString YddVerdict = ExportYddBinary(Paths, Names, YddPath, YddOptions);
	const bool bYddOk = YddVerdict.Contains(TEXT("\"ok\":true"));
	if (!bYddOk) { Problems.Add(TEXT("ydd: ") + YddVerdict.Left(300)); }
	// RUDE_PEDPROPS_BEGIN export
	// 1b) the prop dictionary: every prop with a mesh -> stream/<ped>_p.ydd (RIGID entries, the same writer), every
	// texture under /Game/RUDE/Textures/<ped>_p/ -> stream/<ped>_p.ytd. Nothing is written when the ped has no props.
	FString PropPaths, PropNames;
	int32 Props = 0, PropsWithoutMesh = 0;
	for (const FRudePedProp& P : Outfit->Props)
	{
		const FString MeshPath = P.Mesh.ToSoftObjectPath().ToString();
		if (MeshPath.IsEmpty() || P.Name.IsEmpty() || P.Name.EndsWith(TEXT("_?"))) { ++PropsWithoutMesh; continue; }
		PropPaths += (PropPaths.IsEmpty() ? TEXT("") : TEXT(",")) + MeshPath;
		PropNames += (PropNames.IsEmpty() ? TEXT("") : TEXT(",")) + P.Name;
		++Props;
	}
	FString PropYddVerdict, PropYtdVerdict;
	bool bPropYddOk = true, bPropYtdOk = true;
	int32 PropTextures = 0;
	const FString PropYddPath = StreamDir / (Ped + TEXT("_p.ydd"));
	const FString PropYtdPath = StreamDir / (Ped + TEXT("_p.ytd"));
	if (Props > 0)
	{
		PropYddVerdict = ExportYddBinary(PropPaths, PropNames, PropYddPath, Options);
		bPropYddOk = PropYddVerdict.Contains(TEXT("\"ok\":true"));
		if (!bPropYddOk) { Problems.Add(TEXT("props ydd: ") + PropYddVerdict.Left(300)); }
		TArray<FAssetData> PropTexAssets;
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			ARM.Get().WaitForCompletion();
			ARM.Get().GetAssetsByPath(FName(*(TEXT("/Game/RUDE/Textures/") + Ped + TEXT("_p"))), PropTexAssets, false);
		}
		FString PropSpecs;
		for (const FAssetData& AD : PropTexAssets)
		{
			UTexture2D* T = Cast<UTexture2D>(AD.GetAsset());
			if (!T) { continue; }
			const TCHAR* Usage = (T->CompressionSettings == TC_Normalmap) ? TEXT("NORMAL") : (!T->SRGB ? TEXT("SPECULAR") : TEXT("DIFFUSE"));
			PropSpecs += FString::Printf(TEXT("%s%s;%s;%s"), PropSpecs.IsEmpty() ? TEXT("") : TEXT(","), *T->GetPathName(), *T->GetName(), Usage);
			++PropTextures;
		}
		if (PropTextures > 0)
		{
			PropYtdVerdict = ExportYtdBinary(PropSpecs, PropYtdPath, TEXT("0"));
			bPropYtdOk = PropYtdVerdict.Contains(TEXT("\"ok\":true"));
			if (!bPropYtdOk) { Problems.Add(TEXT("props ytd: ") + PropYtdVerdict.Left(300)); }
		}
		else { Problems.Add(FString::Printf(TEXT("no textures under /Game/RUDE/Textures/%s_p - the props stream untextured (the corpus has no pixels for pedprops ytds yet)"), *Ped)); }
	}
	// RUDE_PEDPROPS_END export

	// 2) the texture dictionary: everything ImportPed brought in from <ped>.ytd, usage read off the asset
	TArray<FAssetData> TexAssets;
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		ARM.Get().WaitForCompletion();
		ARM.Get().GetAssetsByPath(FName(*(TEXT("/Game/RUDE/Textures/") + Ped)), TexAssets, false);
	}
	FString Specs;
	int32 Textures = 0;
	for (const FAssetData& AD : TexAssets)
	{
		UTexture2D* T = Cast<UTexture2D>(AD.GetAsset());
		if (!T) { continue; }
		const TCHAR* Usage = (T->CompressionSettings == TC_Normalmap) ? TEXT("NORMAL") : (!T->SRGB ? TEXT("SPECULAR") : TEXT("DIFFUSE"));
		Specs += FString::Printf(TEXT("%s%s;%s;%s"), Specs.IsEmpty() ? TEXT("") : TEXT(","), *T->GetPathName(), *T->GetName(), Usage);
		++Textures;
	}
	FString YtdVerdict; bool bYtdOk = false;
	const FString YtdPath = StreamDir / (Ped + TEXT(".ytd"));
	if (Textures > 0)
	{
		YtdVerdict = ExportYtdBinary(Specs, YtdPath, TEXT("0"));
		bYtdOk = YtdVerdict.Contains(TEXT("\"ok\":true"));
		if (!bYtdOk) { Problems.Add(TEXT("ytd: ") + YtdVerdict.Left(300)); }
	}
	else { Problems.Add(FString::Printf(TEXT("no textures under /Game/RUDE/Textures/%s - the ped streams untextured (ImportPed imports them when the corpus has pixels)"), *Ped)); }

	// 3) the manifest, written once and kept
	const FString ManifestPath = OutDir / TEXT("fxmanifest.lua");
	FString ManifestState = TEXT("kept");
	if (!FPaths::FileExists(ManifestPath))
	{
		const FString Manifest = FString::Printf(TEXT(
			"fx_version 'cerulean'\ngame 'gta5'\n\n"
			"-- Ped REPLACE resource for '%s': stream/%s.ydd and stream/%s.ytd carry the game's own file names,\n"
			"-- so they shadow the vanilla files - no variation table (ymt) is needed. Every drawable the ped's\n"
			"-- table lists is inside the dictionary under its vanilla entry name (joaat of <comp>_<ddd>_<class>).\n"
			"-- Every LOD group the meshes carry is written (High / Medium / Low). The game's own dictionaries\n"
			"-- carry up to three: 823 of 1,152 measured entries carry all three, 182 two, 147 one. A part\n"
			"-- exported from a mesh with a single LOD ships one group, which is a shape the game itself ships.\n"
			"-- Textures are every name the vanilla dictionary held, re-encoded.\n"
			"-- Props (hats / glasses / earpieces / watches): stream/%s_p.ydd + stream/%s_p.ytd when the ped has any.\n"),
			*Ped, *Ped, *Ped, *Ped, *Ped);
		ManifestState = FFileHelper::SaveStringToFile(Manifest, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? TEXT("written") : TEXT("writeFailed");
	}
	FString ProblemsJson;
	for (const FString& P : Problems) { ProblemsJson += (ProblemsJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("\"%s\""), *RudeJsonEscape(P)); }
	return FString::Printf(TEXT("{\"ok\":%s,\"ped\":\"%s\",\"outDir\":\"%s\",\"yddPath\":\"%s\",\"drawables\":%d,\"drawablesWithoutMesh\":%d,\"drawablesWithLodDist\":%d,\"yddEntries\":%d,\"yddBytes\":%d,")   // RUDE_PEDLOD
		TEXT("\"ytdPath\":\"%s\",\"textures\":%d,\"ytdBytes\":%d,")
		TEXT("\"props\":%d,\"propsExported\":%d,\"propsWithoutMesh\":%d,\"propYddPath\":\"%s\",\"propYddEntries\":%d,\"propYddBytes\":%d,\"propTextures\":%d,\"propYtdPath\":\"%s\",\"propYtdBytes\":%d,")
		TEXT("\"manifest\":\"%s\",\"problems\":[%s]}"),
		(bYddOk && (Textures == 0 || bYtdOk) && bPropYddOk && bPropYtdOk) ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Ped), *RudeJsonEscape(OutDir), *RudeJsonEscape(YddPath),   // RUDE_PEDPROPS: ok folds the prop verdicts in
		Drawables, DrawablesWithoutMesh, DrawablesWithLodDist, JsonInt(YddVerdict, TEXT("entries"), -1), JsonInt(YddVerdict, TEXT("bytes"), -1),   // RUDE_PEDLOD
		*RudeJsonEscape(YtdPath), Textures, JsonInt(YtdVerdict, TEXT("bytes"), -1),
		Outfit->Props.Num(), Props, PropsWithoutMesh, *RudeJsonEscape(PropYddPath), JsonInt(PropYddVerdict, TEXT("entries"), -1), JsonInt(PropYddVerdict, TEXT("bytes"), -1), PropTextures, *RudeJsonEscape(PropYtdPath), JsonInt(PropYtdVerdict, TEXT("bytes"), -1),
		*ManifestState, *ProblemsJson);
}

// ---- ExportTxdReplace (agent + Matt) --------------------------------------------------------
// One imported texture dictionary back to the game as a REPLACE resource: every UTexture2D under
// /Game/RUDE/Textures/<dict>/ (edited or not) into stream/<dict>.ytd under its own name, usage read off
// the asset (normal map / linear = specular / sRGB = diffuse), + fxmanifest.lua. Streamed, it shadows the
// game's <dict>.ytd - the livery / paint / prop-texture edit path with no model writer involved.
FString URudeToolset::ExportTxdReplace(const FString& DictName, const FString& OutDir, const FString& MaxDim)
{
	using namespace RudePeds;
	auto Bad = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	const FString Dict = DictName.TrimStartAndEnd().ToLower();
	if (Dict.IsEmpty()) { return Bad(TEXT("give the dictionary name (the folder under /Game/RUDE/Textures/, e.g. blista)")); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Bad(TEXT("OutDir is empty")); }
	TArray<FAssetData> TexAssets;
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		ARM.Get().WaitForCompletion();
		ARM.Get().GetAssetsByPath(FName(*(TEXT("/Game/RUDE/Textures/") + Dict)), TexAssets, false);
	}
	FString Specs;
	int32 Textures = 0, NotTextures = 0;
	for (const FAssetData& AD : TexAssets)
	{
		UTexture2D* T = Cast<UTexture2D>(AD.GetAsset());
		if (!T) { ++NotTextures; continue; }
		const TCHAR* Usage = (T->CompressionSettings == TC_Normalmap) ? TEXT("NORMAL") : (!T->SRGB ? TEXT("SPECULAR") : TEXT("DIFFUSE"));
		Specs += FString::Printf(TEXT("%s%s;%s;%s"), Specs.IsEmpty() ? TEXT("") : TEXT(","), *T->GetPathName(), *T->GetName(), Usage);
		++Textures;
	}
	if (Textures == 0) { return Bad(FString::Printf(TEXT("no textures under /Game/RUDE/Textures/%s (ImportYtd / ImportVehicleComposite / ImportPed fill it)"), *Dict)); }
	const FString StreamDir = OutDir / TEXT("stream");
	IFileManager::Get().MakeDirectory(*StreamDir, true);
	const FString YtdPath = StreamDir / (Dict + TEXT(".ytd"));
	const FString YtdVerdict = ExportYtdBinary(Specs, YtdPath, MaxDim.TrimStartAndEnd().IsEmpty() ? TEXT("0") : MaxDim);
	const bool bOk = YtdVerdict.Contains(TEXT("\"ok\":true"));
	const FString ManifestPath = OutDir / TEXT("fxmanifest.lua");
	FString ManifestState = TEXT("kept");
	if (bOk && !FPaths::FileExists(ManifestPath))
	{
		const FString Manifest = FString::Printf(TEXT(
			"fx_version 'cerulean'\ngame 'gta5'\n\n"
			"-- Texture-dictionary REPLACE resource: stream/%s.ytd carries the game's own dictionary name, so it\n"
			"-- shadows the vanilla file. Every texture the imported dictionary held is inside under its own name\n"
			"-- (%d), re-encoded (DXT1/DXT5 for colour, ATI2 for normal maps, full mip chains).\n"),
			*Dict, Textures);
		ManifestState = FFileHelper::SaveStringToFile(Manifest, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? TEXT("written") : TEXT("writeFailed");
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"dict\":\"%s\",\"outDir\":\"%s\",\"ytdPath\":\"%s\",\"textures\":%d,\"notTextures\":%d,\"ytdBytes\":%d,\"manifest\":\"%s\",\"ytd\":%s}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Dict), *RudeJsonEscape(OutDir), *RudeJsonEscape(YtdPath), Textures, NotTextures,
		JsonInt(YtdVerdict, TEXT("bytes"), -1), *ManifestState, bOk ? *YtdVerdict : *Bad(YtdVerdict.Left(300)));
}
