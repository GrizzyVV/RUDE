// RUDE - RAGE <-> Unreal Development Environment
//
// WP10 ANIMS lane: clip dictionaries (.ycd) -> UAnimSequence, cutscenes (.cut) -> Level Sequence.
// Every structural claim below was MEASURED on the corpus (maintainer lane `anims` (`LAWS.md`), 2026-09-06):
// 5 dictionaries (2 ambient, 1 camera, 2 cutscene parts), 30 animations, 44 sequences, 4,109 channels.
// What is NOT measured is named: the sign of a cached quaternion's omitted component, the camera's look
// axis, the cutscene part boundary rule. Those are reported in the verdict, never guessed silently.
#include "RudeAnims.h"
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Camera/CameraActor.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSpawnSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "XmlFile.h"

namespace RudeAnims
{
	static FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	}
	static FString JStr(const FString& S) { return TEXT("\"") + RudeJsonEscape(S) + TEXT("\""); }

	static FString Attr(const FXmlNode* N, const TCHAR* Key, const TCHAR* Def)
	{
		if (!N) { return Def; }
		const FString V = N->GetAttribute(Key);
		return V.IsEmpty() ? FString(Def) : V;
	}
	// <Tag value="x"/> or <Tag>x</Tag> under Parent; Def when absent.
	static FString ValueOf(const FXmlNode* Parent, const TCHAR* Tag, const TCHAR* Def = TEXT(""))
	{
		const FXmlNode* C = Parent ? Parent->FindChildNode(Tag) : nullptr;
		if (!C) { return Def; }
		const FString V = C->GetAttribute(TEXT("value"));
		return V.IsEmpty() ? C->GetContent().TrimStartAndEnd() : V;
	}
	static double NumOf(const FXmlNode* P, const TCHAR* Tag, double Def)
	{
		const FString V = ValueOf(P, Tag);
		return V.IsEmpty() ? Def : FCString::Atod(*V);
	}
	static int32 IntOf(const FXmlNode* P, const TCHAR* Tag, int32 Def)
	{
		const FString V = ValueOf(P, Tag);
		return V.IsEmpty() ? Def : FCString::Atoi(*V);
	}
	static void SplitNumbers(const FString& Text, TArray<float>& Out)
	{
		TArray<FString> Parts;
		Text.ParseIntoArrayWS(Parts);
		Out.Reserve(Out.Num() + Parts.Num());
		for (const FString& P : Parts) { Out.Add((float)FCString::Atod(*P)); }
	}
	// Asset-safe spelling of a game name (amb@x@y -> amb_x_y). The verdict carries the original.
	static FString AssetNameOf(const FString& In)
	{
		FString O;
		for (const TCHAR C : In) { O.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_')); }
		return O.IsEmpty() ? FString(TEXT("_")) : O;
	}
	// GTA metres -> UE centimetres with the pinned Y mirror; bone/camera rotations take the plain mirror
	// (x, -y, z, w) - a skeleton bone is not a ymap entity (RudeVehicle.cpp bone note).
	static FVector ToUePos(const FVector& G) { return FVector(G.X * 100.0, -G.Y * 100.0, G.Z * 100.0); }
	static FQuat ToUeQuat(const FQuat& G) { return FQuat(G.X, -G.Y, G.Z, G.W).GetNormalized(); }

	// ---- the decoded XML model --------------------------------------------------------------------------
	struct FChan
	{
		FString Type;
		bool bPerFrame = false;
		TArray<float> Frames;                     // per-frame values (QuantizeFloat, expanded IndirectQuantizeFloat)
		float Static = 0.f;                       // StaticFloat
		FVector StaticVec = FVector::ZeroVector;  // StaticVector3
		FQuat StaticQuat = FQuat::Identity;       // StaticQuaternion
		int32 QuatIndex = -1;                     // CachedQuaternion1/2 marker: the OMITTED component
		bool IsScalar() const { return bPerFrame || Type == TEXT("StaticFloat"); }
		float At(int32 F) const { return bPerFrame ? (Frames.IsValidIndex(F) ? Frames[F] : 0.f) : Static; }
	};
	struct FBoneRef { int32 Tag = -1; int32 Track = -1; int32 Kind = -1; };
	struct FSeq { int32 Frames = 0; TArray<TArray<FChan>> Items; };
	struct FAnim
	{
		FString Hash;
		int32 FrameCount = 0, Limit = 0;
		double Duration = 0.0;
		TArray<FBoneRef> Bones;
		TArray<FSeq> Seqs;
		FString Refusal;     // non-empty = the reader will not import this animation, and says why
	};
	struct FTag { FString Name; double StartPhase = 0.0, EndPhase = 0.0; int32 Attributes = 0; };
	struct FAnimRef { FString Hash; double Start = 0.0, End = 0.0, Rate = 1.0; };
	struct FClip { FString Hash; FString Type; TArray<FAnimRef> Refs; TArray<FTag> Tags; int32 Properties = 0; };

	static bool DecodeChannels(const FXmlNode* ChannelsNode, int32 N, TArray<FChan>& Out, FString& Why)
	{
		if (!ChannelsNode) { return true; }
		for (const FXmlNode* C : ChannelsNode->GetChildrenNodes())
		{
			FChan Ch;
			Ch.Type = ValueOf(C, TEXT("Type"));
			if (Ch.Type == TEXT("QuantizeFloat"))
			{
				const FXmlNode* V = C->FindChildNode(TEXT("Values"));
				SplitNumbers(V ? V->GetContent() : FString(), Ch.Frames);
				if (Ch.Frames.Num() != N)
				{
					Why = FString::Printf(TEXT("QuantizeFloat carries %d values for a %d-frame sequence (measured 2,595/2,595 equal)"), Ch.Frames.Num(), N);
					return false;
				}
				Ch.bPerFrame = true;
			}
			else if (Ch.Type == TEXT("IndirectQuantizeFloat"))
			{
				// <Values> = the palette, <Frames> = one palette index per frame (144/144 measured channels)
				TArray<float> Palette, Idx;
				const FXmlNode* V = C->FindChildNode(TEXT("Values"));
				const FXmlNode* Fr = C->FindChildNode(TEXT("Frames"));
				SplitNumbers(V ? V->GetContent() : FString(), Palette);
				SplitNumbers(Fr ? Fr->GetContent() : FString(), Idx);
				if (Idx.Num() != N)
				{
					Why = FString::Printf(TEXT("IndirectQuantizeFloat carries %d frame indices for a %d-frame sequence"), Idx.Num(), N);
					return false;
				}
				Ch.Frames.Reserve(N);
				for (const float I : Idx)
				{
					const int32 K = FMath::RoundToInt(I);
					if (!Palette.IsValidIndex(K))
					{
						Why = FString::Printf(TEXT("IndirectQuantizeFloat frame index %d outside its %d-entry palette"), K, Palette.Num());
						return false;
					}
					Ch.Frames.Add(Palette[K]);
				}
				Ch.bPerFrame = true;
			}
			else if (Ch.Type == TEXT("StaticFloat"))
			{
				Ch.Static = (float)FCString::Atod(*Attr(C->FindChildNode(TEXT("Value")), TEXT("value"), TEXT("0")));
			}
			else if (Ch.Type == TEXT("StaticVector3"))
			{
				const FXmlNode* V = C->FindChildNode(TEXT("Value"));
				Ch.StaticVec = FVector(FCString::Atod(*Attr(V, TEXT("x"), TEXT("0"))), FCString::Atod(*Attr(V, TEXT("y"), TEXT("0"))), FCString::Atod(*Attr(V, TEXT("z"), TEXT("0"))));
			}
			else if (Ch.Type == TEXT("StaticQuaternion"))
			{
				const FXmlNode* V = C->FindChildNode(TEXT("Value"));
				Ch.StaticQuat = FQuat(FCString::Atod(*Attr(V, TEXT("x"), TEXT("0"))), FCString::Atod(*Attr(V, TEXT("y"), TEXT("0"))),
					FCString::Atod(*Attr(V, TEXT("z"), TEXT("0"))), FCString::Atod(*Attr(V, TEXT("w"), TEXT("1"))));
			}
			else if (Ch.Type.StartsWith(TEXT("CachedQuaternion")))
			{
				Ch.QuatIndex = IntOf(C, TEXT("QuatIndex"), -1);
			}
			else
			{
				Why = FString::Printf(TEXT("channel type '%s' is not one this reader decodes (measured set: QuantizeFloat, IndirectQuantizeFloat, StaticFloat, StaticVector3, StaticQuaternion, CachedQuaternion1/2)"), *Ch.Type);
				return false;
			}
			Out.Add(MoveTemp(Ch));
		}
		return true;
	}
	// vec3 item shapes measured: [StaticVector3] | [scalar, scalar, scalar]  (479/479)
	static bool Vec3At(const TArray<FChan>& Ch, int32 F, FVector& Out)
	{
		if (Ch.Num() == 1 && Ch[0].Type == TEXT("StaticVector3")) { Out = Ch[0].StaticVec; return true; }
		if (Ch.Num() == 3 && Ch[0].IsScalar() && Ch[1].IsScalar() && Ch[2].IsScalar())
		{
			Out = FVector(Ch[0].At(F), Ch[1].At(F), Ch[2].At(F));
			return true;
		}
		return false;
	}
	// quat item shapes measured: [StaticQuaternion] | [l0, l1, l2, CachedQuaternion1/2] (1,136/1,141; the 5 others
	// are the 5-channel CachedQuaternion2 form, refused here and counted). QuatIndex names the OMITTED component,
	// the labels fill the remaining indices in order (ROUT ycd2xml.py:983, YCD_LAYER_B scar 13); the omitted
	// component is reconstructed non-negative (the convention stage 2b wrote and the game played - not proven).
	static bool QuatAt(const TArray<FChan>& Ch, int32 F, FQuat& Out)
	{
		if (Ch.Num() == 1 && Ch[0].Type == TEXT("StaticQuaternion")) { Out = Ch[0].StaticQuat; return true; }
		if (Ch.Num() == 4 && Ch[3].QuatIndex >= 0 && Ch[3].QuatIndex <= 3 && Ch[0].IsScalar() && Ch[1].IsScalar() && Ch[2].IsScalar())
		{
			const double L[3] = { Ch[0].At(F), Ch[1].At(F), Ch[2].At(F) };
			double Q[4] = { 0.0, 0.0, 0.0, 0.0 };
			int32 Li = 0;
			for (int32 i = 0; i < 4; ++i) { if (i == Ch[3].QuatIndex) { continue; } Q[i] = L[Li++]; }
			const double S = 1.0 - L[0] * L[0] - L[1] * L[1] - L[2] * L[2];
			Q[Ch[3].QuatIndex] = S > 0.0 ? FMath::Sqrt(S) : 0.0;
			Out = FQuat(Q[0], Q[1], Q[2], Q[3]).GetNormalized();
			return true;
		}
		return false;
	}
	// Global frame -> (sequence, local frame). Consecutive sequences share one frame (6/6 multi-sequence
	// animations measured), so sequence s starts at global s * SequenceFrameLimit.
	static bool Locate(const FAnim& A, int32 F, int32& S, int32& L)
	{
		if (A.Seqs.Num() <= 1) { S = 0; L = F; }
		else
		{
			const int32 Lim = FMath::Max(1, A.Limit);
			S = FMath::Min(F / Lim, A.Seqs.Num() - 1);
			L = F - S * Lim;
		}
		return A.Seqs.IsValidIndex(S) && L >= 0 && L < A.Seqs[S].Frames;
	}
	static int32 FpsOf(const FAnim& A)
	{
		return (A.Duration > 0.0 && A.FrameCount > 1) ? FMath::Max(1, FMath::RoundToInt((A.FrameCount - 1) / A.Duration)) : 30;
	}

	// Parse <ClipDictionary>: clips (Animation | AnimationList) and animations with every sequence decoded.
	static bool ParseDictionary(const FXmlNode* Root, TArray<FClip>& Clips, TArray<FAnim>& Anims, FString& Why)
	{
		if (!Root || Root->GetTag() != TEXT("ClipDictionary")) { Why = TEXT("root is not <ClipDictionary>"); return false; }
		if (const FXmlNode* CL = Root->FindChildNode(TEXT("Clips")))
		{
			for (const FXmlNode* It : CL->GetChildrenNodes())
			{
				FClip C;
				C.Hash = ValueOf(It, TEXT("Hash"));
				C.Type = ValueOf(It, TEXT("Type"));
				if (C.Type == TEXT("Animation"))
				{
					FAnimRef R; R.Hash = ValueOf(It, TEXT("AnimationHash")); R.Start = NumOf(It, TEXT("StartTime"), 0.0); R.End = NumOf(It, TEXT("EndTime"), 0.0); R.Rate = NumOf(It, TEXT("Rate"), 1.0);
					C.Refs.Add(R);
				}
				else if (const FXmlNode* AL = It->FindChildNode(TEXT("Animations")))
				{
					for (const FXmlNode* A : AL->GetChildrenNodes())
					{
						FAnimRef R; R.Hash = ValueOf(A, TEXT("AnimationHash")); R.Start = NumOf(A, TEXT("StartTime"), 0.0); R.End = NumOf(A, TEXT("EndTime"), 0.0); R.Rate = NumOf(A, TEXT("Rate"), 1.0);
						C.Refs.Add(R);
					}
				}
				if (const FXmlNode* TL = It->FindChildNode(TEXT("Tags")))
				{
					for (const FXmlNode* T : TL->GetChildrenNodes())
					{
						FTag Tg; Tg.Name = ValueOf(T, TEXT("NameHash")); Tg.StartPhase = NumOf(T, TEXT("StartPhase"), 0.0); Tg.EndPhase = NumOf(T, TEXT("EndPhase"), 0.0);
						if (const FXmlNode* AtL = T->FindChildNode(TEXT("Attributes"))) { Tg.Attributes = AtL->GetChildrenNodes().Num(); }
						C.Tags.Add(Tg);
					}
				}
				if (const FXmlNode* PL = It->FindChildNode(TEXT("Properties"))) { C.Properties = PL->GetChildrenNodes().Num(); }
				Clips.Add(MoveTemp(C));
			}
		}
		if (const FXmlNode* AL = Root->FindChildNode(TEXT("Animations")))
		{
			for (const FXmlNode* It : AL->GetChildrenNodes())
			{
				FAnim A;
				A.Hash = ValueOf(It, TEXT("Hash"));
				A.FrameCount = IntOf(It, TEXT("FrameCount"), 0);
				A.Limit = IntOf(It, TEXT("SequenceFrameLimit"), 0);
				A.Duration = NumOf(It, TEXT("Duration"), 0.0);
				if (const FXmlNode* BL = It->FindChildNode(TEXT("BoneIds")))
				{
					for (const FXmlNode* B : BL->GetChildrenNodes())
					{
						FBoneRef R; R.Tag = IntOf(B, TEXT("BoneId"), -1); R.Track = IntOf(B, TEXT("Track"), -1); R.Kind = IntOf(B, TEXT("Unk0"), -1);
						A.Bones.Add(R);
					}
				}
				int32 SumFrames = 0;
				if (const FXmlNode* SL = It->FindChildNode(TEXT("Sequences")))
				{
					for (const FXmlNode* S : SL->GetChildrenNodes())
					{
						FSeq Q;
						Q.Frames = IntOf(S, TEXT("FrameCount"), 0);
						SumFrames += Q.Frames;
						if (const FXmlNode* SD = S->FindChildNode(TEXT("SequenceData")))
						{
							for (const FXmlNode* Item : SD->GetChildrenNodes())
							{
								TArray<FChan> Chans;
								FString ChWhy;
								if (!DecodeChannels(Item->FindChildNode(TEXT("Channels")), Q.Frames, Chans, ChWhy))
								{
									if (A.Refusal.IsEmpty()) { A.Refusal = FString::Printf(TEXT("sequence %d item %d: %s"), A.Seqs.Num(), Q.Items.Num(), *ChWhy); }
								}
								Q.Items.Add(MoveTemp(Chans));
							}
						}
						if (Q.Items.Num() != A.Bones.Num() && A.Refusal.IsEmpty())
						{
							A.Refusal = FString::Printf(TEXT("sequence %d carries %d items for %d BoneIds (measured 44/44 equal)"), A.Seqs.Num(), Q.Items.Num(), A.Bones.Num());
						}
						A.Seqs.Add(MoveTemp(Q));
					}
				}
				if (A.Seqs.Num() == 0 && A.Refusal.IsEmpty()) { A.Refusal = TEXT("no <Sequences>"); }
				if (A.Seqs.Num() > 0 && SumFrames - (A.Seqs.Num() - 1) != A.FrameCount && A.Refusal.IsEmpty())
				{
					A.Refusal = FString::Printf(TEXT("sum(sequence frames) %d - (%d - 1) != FrameCount %d (the shared-boundary-frame law, 6/6 measured)"), SumFrames, A.Seqs.Num(), A.FrameCount);
				}
				Anims.Add(MoveTemp(A));
			}
		}
		return true;
	}

	// ---- bone map: tag -> name --------------------------------------------------------------------------------
	struct FBoneMap
	{
		TMap<int32, FName> TagToName;
		FString Source;      // "outfit:<asset>" | "none"
		FString Note;
	};
	// Read a TMap<FName,int32> named BoneTags (or the only FName->int32 map) off any object by reflection:
	// the ped lane's URudePedOutfit is a sibling under construction, so this lane names no type of it.
	static bool ReadTagMap(UObject* Obj, TMap<int32, FName>& Out, FString& Why)
	{
		FMapProperty* Chosen = nullptr;
		TArray<FString> Candidates;
		for (TFieldIterator<FMapProperty> It(Obj->GetClass()); It; ++It)
		{
			FMapProperty* MP = *It;
			if (!MP->KeyProp || !MP->ValueProp || !MP->KeyProp->IsA<FNameProperty>() || !MP->ValueProp->IsA<FIntProperty>()) { continue; }
			Candidates.Add(MP->GetName());
			if (MP->GetName() == TEXT("BoneTags")) { Chosen = MP; }
		}
		if (!Chosen && Candidates.Num() == 1) { Chosen = FindFProperty<FMapProperty>(Obj->GetClass(), *Candidates[0]); }
		if (!Chosen)
		{
			Why = FString::Printf(TEXT("%s has no TMap<FName,int32> named BoneTags (FName->int32 maps found: %s)"), *Obj->GetPathName(), *FString::Join(Candidates, TEXT(",")));
			return false;
		}
		FScriptMapHelper H(Chosen, Chosen->ContainerPtrToValuePtr<void>(Obj));
		for (int32 i = 0; i < H.GetMaxIndex(); ++i)
		{
			if (!H.IsValidIndex(i)) { continue; }
			const FName K = *reinterpret_cast<const FName*>(H.GetKeyPtr(i));
			const int32 V = *reinterpret_cast<const int32*>(H.GetValuePtr(i));
			Out.Add(V, K);
		}
		return true;
	}
	static FString NormalizeAssetPath(const FString& In)
	{
		FString P = In.TrimStartAndEnd();
		if (!P.IsEmpty() && !P.Contains(TEXT("."))) { P += TEXT(".") + FPackageName::GetShortName(P); }
		return P;
	}
	static bool BuildBoneMap(USkeleton* Skel, const FString& SkeletonPath, const FString& OutfitPath, FBoneMap& Map, FString& Why)
	{
		UObject* Outfit = nullptr;
		if (!OutfitPath.IsEmpty())
		{
			Outfit = LoadObject<UObject>(nullptr, *NormalizeAssetPath(OutfitPath));
			if (!Outfit) { Why = FString::Printf(TEXT("no outfit asset at %s"), *OutfitPath); return false; }
		}
		else
		{
			// the single URudePedOutfit beside the skeleton; anything else is stated, never picked at random
			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
			TArray<FAssetData> Found;
			AR.GetAssetsByPath(FName(*FPackageName::GetLongPackagePath(SkeletonPath)), Found, false);
			TArray<FAssetData> Outfits;
			for (const FAssetData& D : Found) { if (D.AssetClassPath.GetAssetName() == FName(TEXT("RudePedOutfit"))) { Outfits.Add(D); } }
			if (Outfits.Num() == 1) { Outfit = Outfits[0].GetAsset(); Map.Note = TEXT("outfit found beside the skeleton"); }
			else if (Outfits.Num() > 1)
			{
				FString Names; for (const FAssetData& D : Outfits) { Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + D.AssetName.ToString(); }
				Why = FString::Printf(TEXT("%d RudePedOutfit assets beside the skeleton (%s) - name one: SkeletonAssetPath=\"<skeleton>;<outfit>\""), Outfits.Num(), *Names);
				return false;
			}
		}
		if (Outfit)
		{
			if (!ReadTagMap(Outfit, Map.TagToName, Why)) { return false; }
			Map.Source = TEXT("outfit:") + Outfit->GetPathName();
			return true;
		}
		Map.Source = TEXT("none");
		Map.Note = TEXT("no outfit asset: only bones whose skeleton name spells the tag's decimal can map");
		return true;
	}
	static FName ResolveBone(const FBoneMap& Map, const FReferenceSkeleton& Ref, int32 Tag, FString& OutSource)
	{
		if (const FName* N = Map.TagToName.Find(Tag))
		{
			if (Ref.FindBoneIndex(*N) != INDEX_NONE) { OutSource = TEXT("name"); return *N; }
			OutSource = TEXT("name-not-on-skeleton"); return NAME_None;
		}
		const FName ByTag(*FString::FromInt(Tag));
		if (Ref.FindBoneIndex(ByTag) != INDEX_NONE) { OutSource = TEXT("tag"); return ByTag; }
		OutSource = TEXT("unmapped"); return NAME_None;
	}

	// ---- corpus resolution --------------------------------------------------------------------------------------
	static bool ResolveXml(const FString& CorpusRoot, const TCHAR* Type, const FString& Name, const TCHAR* AdHocSuffix, FString& OutPath, TSharedPtr<FRudeCorpus>& OutCorpus, FString& Why)
	{
		OutPath = CorpusRoot / (Name + AdHocSuffix);
		if (FRudeCorpus::LooksLikeCorpus(CorpusRoot))
		{
			FString CorpusErr;
			OutCorpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
			if (!OutCorpus.IsValid()) { Why = CorpusErr; return false; }
			if (const FRudeCorpusEntry* Row = OutCorpus->Effective(Type, Name)) { OutPath = OutCorpus->PathOf(*Row); }
			else { Why = FString::Printf(TEXT("the corpus has no %s named '%s'"), Type, *Name); return false; }
		}
		if (!FPaths::FileExists(OutPath)) { Why = FString::Printf(TEXT("no file at %s"), *OutPath); return false; }
		return true;
	}
}

// ---- ImportClipDictionary ---------------------------------------------------------------------------------------
FString URudeToolset::ImportClipDictionary(const FString& CorpusRoot, const FString& YcdName,
                                           const FString& SkeletonAssetPath, const FString& DestFolder)
{
	using namespace RudeAnims;
	FString Name = YcdName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml")); Name.RemoveFromEnd(TEXT(".ycd"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a clip dictionary name, e.g. amb@bagels@male@walking@")); }
	FString SkelPath, OutfitPath;
	if (!SkeletonAssetPath.Split(TEXT(";"), &SkelPath, &OutfitPath)) { SkelPath = SkeletonAssetPath; }
	SkelPath = NormalizeAssetPath(SkelPath); OutfitPath = OutfitPath.TrimStartAndEnd();
	if (SkelPath.IsEmpty()) { return Fail(TEXT("give the skeleton asset path (optionally \"<skeleton>;<outfit DataAsset>\")")); }
	USkeleton* Skel = LoadObject<USkeleton>(nullptr, *SkelPath);
	if (!Skel) { return Fail(FString::Printf(TEXT("no USkeleton at %s"), *SkelPath)); }
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game/RUDE/Anims")) : DestFolder.TrimStartAndEnd();
	const FString DictFolder = Dest / AssetNameOf(Name);
	if (!FPackageName::IsValidLongPackageName(DictFolder / TEXT("a"))) { return Fail(FString::Printf(TEXT("bad content path: %s"), *DictFolder)); }

	FString XmlPath, Why; TSharedPtr<FRudeCorpus> Corpus;
	if (!ResolveXml(CorpusRoot, TEXT("ycd"), Name, TEXT(".ycd.xml"), XmlPath, Corpus, Why)) { return Fail(Why); }
	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid()) { return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError())); }
	TArray<FClip> Clips; TArray<FAnim> Anims;
	if (!ParseDictionary(Xml.GetRootNode(), Clips, Anims, Why)) { return Fail(Why); }
	if (Anims.Num() == 0) { return Fail(TEXT("the dictionary has no <Animations>")); }

	FBoneMap Map;
	if (!BuildBoneMap(Skel, SkelPath, OutfitPath, Map, Why)) { return Fail(Why); }
	const FReferenceSkeleton& Ref = Skel->GetReferenceSkeleton();
	const TArray<FTransform>& RefPose = Ref.GetRefBonePose();

	FString AnimJson, FailJson;
	int32 Built = 0, Refused = 0, MappedTotal = 0, UnmappedTotal = 0, NotifiesTotal = 0;
	for (const FAnim& A : Anims)
	{
		if (!A.Refusal.IsEmpty())
		{
			++Refused;
			FailJson += (FailJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("{\"hash\":%s,\"why\":%s}"), *JStr(A.Hash), *JStr(A.Refusal));
			continue;
		}
		const int32 N = FMath::Max(1, A.FrameCount);
		const int32 Fps = FpsOf(A);
		const FString AssetName = TEXT("A_") + AssetNameOf(A.Hash);
		const FString PkgName = DictFolder / AssetName;
		UPackage* Pkg = CreatePackage(*PkgName);
		Pkg->FullyLoad();
		UAnimSequence* Seq = FindObject<UAnimSequence>(Pkg, *AssetName);
		const bool bNew = Seq == nullptr;
		if (!Seq) { Seq = NewObject<UAnimSequence>(Pkg, FName(*AssetName), RF_Public | RF_Standalone); }
		Seq->SetSkeleton(Skel);
		Seq->Notifies.Empty();

		// one item index per (tag, track) - the SequenceData items are parallel to BoneIds in every sequence
		TMap<int32, int32> ItemT, ItemR;
		TMap<int32, int32> Skipped;
		for (int32 j = 0; j < A.Bones.Num(); ++j)
		{
			const FBoneRef& B = A.Bones[j];
			if (B.Track == 0) { ItemT.Add(B.Tag, j); }
			else if (B.Track == 1) { ItemR.Add(B.Tag, j); }
			else { Skipped.FindOrAdd(B.Track)++; }
		}
		TSet<int32> Tags; for (const auto& P : ItemT) { Tags.Add(P.Key); } for (const auto& P : ItemR) { Tags.Add(P.Key); }
		TArray<int32> TagList = Tags.Array(); TagList.Sort();

		IAnimationDataController& C = Seq->GetController();
		C.OpenBracket(FText::FromString(TEXT("RUDE ImportClipDictionary")), false);
		if (bNew) { C.InitializeModel(); } else { C.ResetModel(false); }
		C.SetFrameRate(FFrameRate(Fps, 1), false);
		C.SetNumberOfFrames(FFrameNumber(FMath::Max(1, N - 1)), false);
		const int32 Keys = FMath::Max(2, N);   // a 1-frame animation still needs two keys (one interval)
		int32 Mapped = 0, Unmapped = 0, PosUnreadable = 0, RotUnreadable = 0, FrameMisses = 0;
		FString UnmappedList, KeysJson;
		for (const int32 Tag : TagList)
		{
			FString Src;
			const FName Bone = ResolveBone(Map, Ref, Tag, Src);
			if (Bone.IsNone())
			{
				++Unmapped;
				if (Unmapped <= 12) { UnmappedList += (UnmappedList.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("{\"tag\":%d,\"why\":\"%s\"}"), Tag, *Src); }
				continue;
			}
			const int32 Bi = Ref.FindBoneIndex(Bone);
			const FTransform& RefXf = RefPose[Bi];
			const int32* JT = ItemT.Find(Tag);
			const int32* JR = ItemR.Find(Tag);
			TArray<FVector> P; TArray<FQuat> R; TArray<FVector> Sc;
			P.Reserve(Keys); R.Reserve(Keys); Sc.Reserve(Keys);
			bool bPosBad = false, bRotBad = false;
			for (int32 K = 0; K < Keys; ++K)
			{
				const int32 F = FMath::Min(K, N - 1);
				int32 S = 0, L = 0;
				const bool bLoc = Locate(A, F, S, L);
				if (!bLoc) { ++FrameMisses; }
				FVector G; FQuat Q;
				if (bLoc && JT && A.Seqs[S].Items.IsValidIndex(*JT) && Vec3At(A.Seqs[S].Items[*JT], L, G)) { P.Add(ToUePos(G)); }
				else { if (JT) { bPosBad = true; } P.Add(RefXf.GetTranslation()); }
				if (bLoc && JR && A.Seqs[S].Items.IsValidIndex(*JR) && QuatAt(A.Seqs[S].Items[*JR], L, Q)) { R.Add(ToUeQuat(Q)); }
				else { if (JR) { bRotBad = true; } R.Add(RefXf.GetRotation()); }
				Sc.Add(RefXf.GetScale3D());
			}
			PosUnreadable += bPosBad ? 1 : 0; RotUnreadable += bRotBad ? 1 : 0;
			C.AddBoneCurve(Bone, false);
			C.SetBoneTrackKeys(Bone, P, R, Sc, false);
			++Mapped;
			if (Mapped <= 6)
			{
				const FVector& T0 = P[0]; const FVector& TL = P.Last(); const FQuat& R0 = R[0]; const FQuat& RL = R.Last();
				KeysJson += (KeysJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(
					TEXT("{\"tag\":%d,\"bone\":%s,\"by\":\"%s\",\"hasT\":%s,\"hasR\":%s,\"t0\":[%.4f,%.4f,%.4f],\"tLast\":[%.4f,%.4f,%.4f],\"r0\":[%.6f,%.6f,%.6f,%.6f],\"rLast\":[%.6f,%.6f,%.6f,%.6f]}"),
					Tag, *JStr(Bone.ToString()), *Src, JT ? TEXT("true") : TEXT("false"), JR ? TEXT("true") : TEXT("false"),
					T0.X, T0.Y, T0.Z, TL.X, TL.Y, TL.Z, R0.X, R0.Y, R0.Z, R0.W, RL.X, RL.Y, RL.Z, RL.W);
			}
		}
		C.NotifyPopulated();
		C.CloseBracket(false);
		Seq->bEnableRootMotion = false;

		// tags -> plain notifies (name = NameHash) at the clip-relative time of the reference that names this animation
		int32 Notifies = 0;
		for (const FClip& Cl : Clips)
		{
			for (const FAnimRef& Rf : Cl.Refs)
			{
				if (Rf.Hash != A.Hash) { continue; }
				for (const FTag& T : Cl.Tags)
				{
					const double Time = FMath::Clamp(Rf.Start + T.StartPhase * (Rf.End - Rf.Start), 0.0, A.Duration);
					FAnimNotifyEvent& E = Seq->Notifies.AddDefaulted_GetRef();
					E.NotifyName = FName(*T.Name);
					E.Link(Seq, (float)Time);
					E.TriggerTimeOffset = GetTriggerTimeOffsetForType(Seq->CalculateOffsetForNotify((float)Time));
					E.Guid = FGuid::NewGuid();
					Skel->AddNewAnimationNotify(E.NotifyName);
					++Notifies;
				}
			}
		}
		Seq->RefreshCacheData();
		Seq->MarkPackageDirty();
		if (bNew) { FAssetRegistryModule::AssetCreated(Seq); }
		++Built; MappedTotal += Mapped; UnmappedTotal += Unmapped; NotifiesTotal += Notifies;

		FString SkipJson; for (const auto& Pr : Skipped) { SkipJson += (SkipJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("\"%d\":%d"), Pr.Key, Pr.Value); }
		FString SeqJson; for (const FSeq& Sq : A.Seqs) { SeqJson += (SeqJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::FromInt(Sq.Frames); }
		AnimJson += (AnimJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(
			TEXT("{\"hash\":%s,\"asset\":%s,\"frames\":%d,\"frameRate\":%d,\"duration\":%g,\"sequences\":[%s],\"bonesMapped\":%d,\"bonesUnmapped\":%d,\"unmappedTags\":[%s],")
			TEXT("\"tracksSkipped\":{%s},\"posUnreadable\":%d,\"rotUnreadable\":%d,\"frameMisses\":%d,\"notifies\":%d,\"keys\":[%s]}"),
			*JStr(A.Hash), *JStr(PkgName), A.FrameCount, Fps, A.Duration, *SeqJson, Mapped, Unmapped, *UnmappedList, *SkipJson, PosUnreadable, RotUnreadable, FrameMisses, Notifies, *KeysJson);
	}
	if (Built == 0)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"no animation could be built\",\"ycd\":%s,\"failures\":[%s]}"), *JStr(Name), *FailJson);
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"ycd\":%s,\"file\":%s,\"skeleton\":%s,\"boneMap\":{\"source\":%s,\"entries\":%d,\"note\":%s},\"clips\":%d,\"animations\":%d,\"built\":%d,\"refused\":%d,")
		TEXT("\"bonesMappedTotal\":%d,\"bonesUnmappedTotal\":%d,\"notifiesTotal\":%d,\"folder\":%s,\"anims\":[%s],\"failures\":[%s],")
		TEXT("\"note\":\"rotation reconstruction sign and the skeleton-space match are for Matt's eyes; run SaveAssets or let the CLI save at exit\"}"),
		*JStr(Name), *JStr(XmlPath), *JStr(SkelPath), *JStr(Map.Source), Map.TagToName.Num(), *JStr(Map.Note), Clips.Num(), Anims.Num(), Built, Refused,
		MappedTotal, UnmappedTotal, NotifiesTotal, *JStr(DictFolder), *AnimJson, *FailJson);
}

// ---- ImportCutscene ---------------------------------------------------------------------------------------------
namespace RudeAnims
{
	struct FCamKey { FVector PosUE; FRotator RotUE; float Fov = 0.f; };
	// The camera clip of one part: bone 0 track 7 (vec3) / 8 (quat) / 27 (float) over every sequence.
	static bool CameraKeysOfPart(const FAnim& A, TArray<FCamKey>& Out, FString& Why)
	{
		int32 JT = -1, JR = -1, JF = -1;
		for (int32 j = 0; j < A.Bones.Num(); ++j)
		{
			if (A.Bones[j].Tag != 0) { continue; }
			if (A.Bones[j].Track == 7) { JT = j; } else if (A.Bones[j].Track == 8) { JR = j; } else if (A.Bones[j].Track == 27) { JF = j; }
		}
		if (JT < 0) { Why = TEXT("the camera clip has no bone-0 track 7 (position)"); return false; }
		for (int32 F = 0; F < A.FrameCount; ++F)
		{
			int32 S = 0, L = 0;
			if (!Locate(A, F, S, L)) { Why = FString::Printf(TEXT("frame %d has no sequence"), F); return false; }
			FCamKey K;
			FVector G;
			if (!A.Seqs[S].Items.IsValidIndex(JT) || !Vec3At(A.Seqs[S].Items[JT], L, G)) { Why = FString::Printf(TEXT("track 7 unreadable at frame %d"), F); return false; }
			K.PosUE = ToUePos(G);
			FQuat Q = FQuat::Identity;
			if (JR >= 0 && A.Seqs[S].Items.IsValidIndex(JR)) { QuatAt(A.Seqs[S].Items[JR], L, Q); }
			K.RotUE = ToUeQuat(Q).Rotator();
			if (JF >= 0 && A.Seqs[S].Items.IsValidIndex(JF) && A.Seqs[S].Items[JF].Num() == 1) { K.Fov = A.Seqs[S].Items[JF][0].At(L); }
			Out.Add(K);
		}
		return true;
	}
	static void NodeXml(const FXmlNode* N, FString& Out) { Out.Reset(); if (N) { RudeXmlNodeToString(N, Out, 0); } }
}

FString URudeToolset::ImportCutscene(const FString& CorpusRoot, const FString& CutName, const FString& DestFolder)
{
	using namespace RudeAnims;
	FString Name = CutName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml")); Name.RemoveFromEnd(TEXT(".pso")); Name.RemoveFromEnd(TEXT(".cut"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a cutscene name, e.g. ah_1_int")); }
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game/RUDE/Cutscenes")) : DestFolder.TrimStartAndEnd();
	const FString Folder = Dest / AssetNameOf(Name);
	if (!FPackageName::IsValidLongPackageName(Folder / TEXT("a"))) { return Fail(FString::Printf(TEXT("bad content path: %s"), *Folder)); }

	FString XmlPath, Why; TSharedPtr<FRudeCorpus> Corpus;
	if (!ResolveXml(CorpusRoot, TEXT("cut"), Name, TEXT(".cut.pso.xml"), XmlPath, Corpus, Why)) { return Fail(Why); }
	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid()) { return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError())); }
	const FXmlNode* Root = Xml.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("rage__cutfCutsceneFile2")) { return Fail(TEXT("root is not <rage__cutfCutsceneFile2>")); }

	// ---- 1) the sidecar: everything verbatim -----------------------------------------------------------------
	const FString DaName = TEXT("DA_") + AssetNameOf(Name) + TEXT("_events");
	UPackage* DaPkg = CreatePackage(*(Folder / DaName));
	DaPkg->FullyLoad();
	URudeCutsceneEvents* DA = FindObject<URudeCutsceneEvents>(DaPkg, *DaName);
	const bool bDaNew = DA == nullptr;
	if (!DA) { DA = NewObject<URudeCutsceneEvents>(DaPkg, FName(*DaName), RF_Public | RF_Standalone); }
	DA->CutName = Name; DA->SourceFile = XmlPath;
	DA->TotalDuration = (float)NumOf(Root, TEXT("fTotalDuration"), 0.0);
	{
		const FXmlNode* O = Root->FindChildNode(TEXT("vOffset"));
		DA->SceneOffsetRage = FVector(FCString::Atod(*Attr(O, TEXT("x"), TEXT("0"))), FCString::Atod(*Attr(O, TEXT("y"), TEXT("0"))), FCString::Atod(*Attr(O, TEXT("z"), TEXT("0"))));
		DA->SceneRotationRage = (float)NumOf(Root, TEXT("fRotation"), 0.0);
		DA->CutsceneFlags = ValueOf(Root, TEXT("iCutsceneFlags"));
	}
	DA->Objects.Reset(); DA->Events.Reset(); DA->EventArgsXml.Reset(); DA->ConcatXml.Reset(); DA->Parts.Reset();
	FString CamName;
	if (const FXmlNode* OL = Root->FindChildNode(TEXT("pCutsceneObjects")))
	{
		for (const FXmlNode* It : OL->GetChildrenNodes())
		{
			FRudeCutObject O;
			O.ObjectId = IntOf(It, TEXT("iObjectId"), -1); O.Type = It->GetAttribute(TEXT("type"));
			O.Name = ValueOf(It, TEXT("cName")); O.StreamingName = ValueOf(It, TEXT("StreamingName"));
			NodeXml(It, O.Xml);
			if (O.Type == TEXT("rage__cutfCameraObject") && CamName.IsEmpty()) { CamName = O.Name; }
			DA->Objects.Add(MoveTemp(O));
		}
	}
	TArray<const FXmlNode*> ArgNodes;
	if (const FXmlNode* AL = Root->FindChildNode(TEXT("pCutsceneEventArgsList")))
	{
		for (const FXmlNode* It : AL->GetChildrenNodes()) { ArgNodes.Add(It); FString X; NodeXml(It, X); DA->EventArgsXml.Add(MoveTemp(X)); }
	}
	int32 CameraCuts = 0;
	TArray<double> CutTimes;
	for (const TCHAR* ListName : { TEXT("pCutsceneLoadEventList"), TEXT("pCutsceneEventList") })
	{
		const FXmlNode* EL = Root->FindChildNode(ListName);
		if (!EL) { continue; }
		for (const FXmlNode* It : EL->GetChildrenNodes())
		{
			FRudeCutEvent E;
			E.List = ListName; E.EventType = It->GetAttribute(TEXT("type"));
			E.Time = (float)NumOf(It, TEXT("fTime"), 0.0); E.EventId = IntOf(It, TEXT("iEventId"), -1);
			E.ObjectId = IntOf(It, TEXT("iObjectId"), -1); E.ArgsIndex = IntOf(It, TEXT("iEventArgsIndex"), -1);
			if (ArgNodes.IsValidIndex(E.ArgsIndex))
			{
				E.ArgsType = ArgNodes[E.ArgsIndex]->GetAttribute(TEXT("type"));
				E.ArgsName = ValueOf(ArgNodes[E.ArgsIndex], TEXT("cName"));
				E.ArgsXml = DA->EventArgsXml[E.ArgsIndex];
			}
			NodeXml(It, E.EventXml);
			if (E.ArgsType == TEXT("rage__cutfCameraCutEventArgs")) { ++CameraCuts; CutTimes.Add(E.Time); }
			DA->Events.Add(MoveTemp(E));
		}
	}
	int32 ConcatValid = 0;
	if (const FXmlNode* CL = Root->FindChildNode(TEXT("concatDataList")))
	{
		for (const FXmlNode* It : CL->GetChildrenNodes())
		{
			FString X; NodeXml(It, X); DA->ConcatXml.Add(MoveTemp(X));
			if (ValueOf(It, TEXT("bValidForPlayBack")) == TEXT("true")) { ++ConcatValid; }
		}
	}
	if (CamName.IsEmpty()) { return Fail(TEXT("the cutscene declares no rage__cutfCameraObject - nothing to key a camera from")); }

	// ---- 2) the camera: every part <cut>-<k>, clip "<camera>-<k>" ---------------------------------------------
	TArray<TPair<int32, FString>> Parts;   // k -> xml path
	if (Corpus.IsValid())
	{
		TArray<const FRudeCorpusEntry*> Rows;
		Corpus->ByPrefix(TEXT("ycd"), Name + TEXT("-"), Rows);
		for (const FRudeCorpusEntry* R : Rows)
		{
			const FString Suffix = R->Name.Mid(Name.Len() + 1);
			if (!Suffix.IsEmpty() && Suffix.IsNumeric()) { Parts.Emplace(FCString::Atoi(*Suffix), Corpus->PathOf(*R)); }
		}
	}
	else
	{
		for (int32 K = 0; K < 256; ++K)
		{
			const FString P = CorpusRoot / FString::Printf(TEXT("%s-%d.ycd.xml"), *Name, K);
			if (!FPaths::FileExists(P)) { break; }
			Parts.Emplace(K, P);
		}
	}
	Parts.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
	if (Parts.Num() == 0) { return Fail(FString::Printf(TEXT("no animation parts '%s-<k>' in the corpus - the camera lives there, not in the .cut"), *Name)); }

	// Part placement law (measured on ah_1_int + ah_1_ext_t6): the .cut's <cameraCutList> holds the split times;
	// part k spans [split[k-1], split[k]) (split[-1] = 0, the last part ends at fTotalDuration) and the part
	// durations tile the cutscene exactly (9 parts: 15+15+17.33+11.13+14.97+15+15+15+15.77 = 134.2). A part's
	// last frame coincides in time with the next part's first frame; the earlier part's key is kept.
	TArray<float> Splits;
	SplitNumbers(ValueOf(Root, TEXT("cameraCutList")), Splits);
	const bool bBySplits = Parts.Num() > 0 && Splits.Num() >= Parts.Num() - 1;
	TArray<TPair<int32, FCamKey>> Keyed;   // absolute frame -> key
	TSet<int32> UsedFrames;
	FString PartJson, PartFail, StartsJson;
	double SumPartsSeconds = 0.0, BackToBack = 0.0;
	int32 Fps = 30, SpanMismatches = 0;
	for (const TPair<int32, FString>& Pt : Parts)
	{
		FXmlFile PX(Pt.Value);
		if (!PX.IsValid()) { PartFail += FString::Printf(TEXT("part %d: XML load failed; "), Pt.Key); continue; }
		TArray<FClip> PClips; TArray<FAnim> PAnims; FString PWhy;
		if (!ParseDictionary(PX.GetRootNode(), PClips, PAnims, PWhy)) { PartFail += FString::Printf(TEXT("part %d: %s; "), Pt.Key, *PWhy); continue; }
		const FString ClipName = FString::Printf(TEXT("%s-%d"), *CamName, Pt.Key);
		const FClip* Clip = PClips.FindByPredicate([&](const FClip& C) { return C.Hash == ClipName; });
		const FAnim* Anim = Clip && Clip->Refs.Num() > 0 ? PAnims.FindByPredicate([&](const FAnim& A) { return A.Hash == Clip->Refs[0].Hash; }) : nullptr;
		if (!Anim) { PartFail += FString::Printf(TEXT("part %d: no clip '%s' with an animation; "), Pt.Key, *ClipName); continue; }
		if (!Anim->Refusal.IsEmpty()) { PartFail += FString::Printf(TEXT("part %d: %s; "), Pt.Key, *Anim->Refusal); continue; }
		TArray<FCamKey> PK;
		if (!CameraKeysOfPart(*Anim, PK, PWhy)) { PartFail += FString::Printf(TEXT("part %d: %s; "), Pt.Key, *PWhy); continue; }
		Fps = FpsOf(*Anim);
		const double PartSeconds = (Anim->FrameCount - 1) / (double)Fps;
		double Start = BackToBack;
		if (bBySplits)
		{
			Start = Pt.Key <= 0 ? 0.0 : (Splits.IsValidIndex(Pt.Key - 1) ? Splits[Pt.Key - 1] : BackToBack);
			const double ExpectedEnd = Splits.IsValidIndex(Pt.Key) ? Splits[Pt.Key] : DA->TotalDuration;
			if (FMath::Abs(Start + PartSeconds - ExpectedEnd) > 0.5 / Fps) { ++SpanMismatches; }
		}
		const int32 Base = FMath::RoundToInt(Start * Fps);
		for (int32 i = 0; i < PK.Num(); ++i)
		{
			const int32 Frame = Base + i;
			if (UsedFrames.Contains(Frame)) { continue; }
			UsedFrames.Add(Frame);
			Keyed.Emplace(Frame, PK[i]);
		}
		BackToBack = Start + PartSeconds;
		SumPartsSeconds += PartSeconds;
		DA->Parts.Add(FPaths::GetBaseFilename(Pt.Value));
		StartsJson += (StartsJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("%.4f"), Start);
		PartJson += (PartJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("{\"k\":%d,\"start\":%.4f,\"frames\":%d,\"fps\":%d,\"seqs\":%d,\"fov0\":%g}"), Pt.Key, Start, Anim->FrameCount, Fps, Anim->Seqs.Num(), PK.Num() ? PK[0].Fov : 0.f);
	}
	if (Keyed.Num() == 0) { return Fail(TEXT("no camera keys could be read from any part: ") + PartFail); }
	Keyed.Sort([](const TPair<int32, FCamKey>& A, const TPair<int32, FCamKey>& B) { return A.Key < B.Key; });
	TArray<FCamKey> Keys; Keys.Reserve(Keyed.Num());
	TArray<int32> KeyFrames; KeyFrames.Reserve(Keyed.Num());
	for (const TPair<int32, FCamKey>& KF : Keyed) { KeyFrames.Add(KF.Key); Keys.Add(KF.Value); }

	// ---- 3) the Level Sequence ---------------------------------------------------------------------------------
	const FString LsName = TEXT("LS_") + AssetNameOf(Name);
	UPackage* LsPkg = CreatePackage(*(Folder / LsName));
	LsPkg->FullyLoad();
	if (UObject* Stale = StaticFindObject(ULevelSequence::StaticClass(), LsPkg, *LsName))
	{
		Stale->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}
	ULevelSequence* LS = NewObject<ULevelSequence>(LsPkg, FName(*LsName), RF_Public | RF_Standalone | RF_Transactional);
	LS->Initialize();
	UMovieScene* MS = LS->GetMovieScene();
	const FFrameRate Display(Fps, 1);
	MS->SetDisplayRate(Display);
	const FFrameRate Tick = MS->GetTickResolution();
	auto ToTick = [&](double Frame) { return FFrameRate::TransformTime(FFrameTime::FromDecimal(Frame), Display, Tick).RoundToFrame(); };
	MS->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), ToTick(KeyFrames.Last())));

	ACameraActor* Template = NewObject<ACameraActor>(MS, ACameraActor::StaticClass(), FName(*AssetNameOf(CamName)), RF_Transactional);
	const FGuid CamGuid = MS->AddSpawnable(CamName, *Template);
	{
		UMovieSceneSpawnTrack* ST = MS->AddTrack<UMovieSceneSpawnTrack>(CamGuid);
		ST->SetObjectId(CamGuid);
		UMovieSceneSpawnSection* SS = Cast<UMovieSceneSpawnSection>(ST->CreateNewSection());
		SS->GetChannel().SetDefault(true);
		SS->SetRange(TRange<FFrameNumber>::All());
		ST->AddSection(*SS);
	}
	{
		UMovieScene3DTransformTrack* TT = MS->AddTrack<UMovieScene3DTransformTrack>(CamGuid);
		UMovieScene3DTransformSection* TS = Cast<UMovieScene3DTransformSection>(TT->CreateNewSection());
		TS->SetRange(TRange<FFrameNumber>::All());
		TArrayView<FMovieSceneDoubleChannel*> Ch = TS->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Ch.Num() < 9) { return Fail(FString::Printf(TEXT("the transform section exposes %d double channels, expected 9 (T xyz, R xyz, S xyz)"), Ch.Num())); }
		TArray<FFrameNumber> Times; Times.Reserve(Keys.Num());
		TArray<FMovieSceneDoubleValue> V[6];
		for (int32 i = 0; i < 6; ++i) { V[i].Reserve(Keys.Num()); }
		FRotator Prev = Keys[0].RotUE;
		for (int32 K = 0; K < Keys.Num(); ++K)
		{
			Times.Add(ToTick(KeyFrames[K]));
			FRotator R = Keys[K].RotUE;
			// keep Euler continuity between frames (a linear key across the +-180 seam would spin the camera)
			R.Roll = Prev.Roll + FMath::FindDeltaAngleDegrees(Prev.Roll, R.Roll);
			R.Pitch = Prev.Pitch + FMath::FindDeltaAngleDegrees(Prev.Pitch, R.Pitch);
			R.Yaw = Prev.Yaw + FMath::FindDeltaAngleDegrees(Prev.Yaw, R.Yaw);
			Prev = R;
			const double Vals[6] = { Keys[K].PosUE.X, Keys[K].PosUE.Y, Keys[K].PosUE.Z, R.Roll, R.Pitch, R.Yaw };
			for (int32 i = 0; i < 6; ++i) { FMovieSceneDoubleValue D(Vals[i]); D.InterpMode = RCIM_Linear; V[i].Add(D); }
		}
		for (int32 i = 0; i < 6; ++i) { Ch[i]->AddKeys(Times, V[i]); }
		for (int32 i = 6; i < 9; ++i) { Ch[i]->SetDefault(1.0); }
		TT->AddSection(*TS);
	}
	int32 CutsKeyed = 0;
	{
		UMovieSceneCameraCutTrack* CC = Cast<UMovieSceneCameraCutTrack>(MS->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
		if (!CC) { return Fail(TEXT("could not add a camera cut track")); }
		CutTimes.Sort();
		if (CutTimes.Num() == 0 || CutTimes[0] > 0.0) { CutTimes.Insert(0.0, 0); }
		for (const double T : CutTimes)
		{
			CC->AddNewCameraCut(FMovieSceneObjectBindingID(UE::MovieScene::FRelativeObjectBindingID(CamGuid)), ToTick(T * Fps));
			++CutsKeyed;
		}
	}
	LS->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(LS);
	DA->MarkPackageDirty();
	if (bDaNew) { FAssetRegistryModule::AssetCreated(DA); }

	const FCamKey& K0 = Keys[0];
	return FString::Printf(
		TEXT("{\"ok\":true,\"cut\":%s,\"file\":%s,\"totalDuration\":%g,\"objects\":%d,\"events\":%d,\"eventArgs\":%d,\"cameraCutEvents\":%d,\"cameraCutsKeyed\":%d,\"cameraObject\":%s,")
		TEXT("\"concatRows\":%d,\"concatValid\":%d,\"partsFound\":%d,\"partPlacement\":\"%s\",\"partSplits\":%d,\"partStarts\":[%s],\"partSpanMismatches\":%d,\"parts\":[%s],\"partFailures\":%s,")
		TEXT("\"cameraKeys\":%d,\"lastKeyFrame\":%d,\"fps\":%d,\"sumPartsSeconds\":%.4f,")
		TEXT("\"firstCamPosUE\":[%.2f,%.2f,%.2f],\"firstCamRotUE\":[%.3f,%.3f,%.3f],\"fovFirst\":%g,\"levelSequence\":%s,\"eventsAsset\":%s,")
		TEXT("\"note\":\"camera look-axis convention is unverified (NOTES.md); parts placed at the .cut's cameraCutList split times; the sidecar carries every event verbatim\"}"),
		*JStr(Name), *JStr(XmlPath), DA->TotalDuration, DA->Objects.Num(), DA->Events.Num(), DA->EventArgsXml.Num(), CameraCuts, CutsKeyed, *JStr(CamName),
		DA->ConcatXml.Num(), ConcatValid, Parts.Num(), bBySplits ? TEXT("cameraCutList") : TEXT("back-to-back"), Splits.Num(), *StartsJson, SpanMismatches, *PartJson, *JStr(PartFail),
		Keys.Num(), KeyFrames.Last(), Fps, SumPartsSeconds,
		K0.PosUE.X, K0.PosUE.Y, K0.PosUE.Z, K0.RotUE.Roll, K0.RotUE.Pitch, K0.RotUE.Yaw, K0.Fov, *JStr(Folder / LsName), *JStr(Folder / DaName));
}
