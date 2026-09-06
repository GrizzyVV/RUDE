// RUDE - RAGE <-> Unreal Development Environment
// Wave-1 level tools: the district as a World Partition level, the archetype palette, interiors as
// Level Instances, the level readback to ymap/ytyp, LOD/ymap visibility, and the instruments (PickAt,
// InspectMesh, XmlShapeRoundTrip). Split out of RudeToolset.cpp 2026-09-06.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetCompilingManager.h"
#include "FileHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "MeshDescription.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Framework/Application/SlateApplication.h"
#include "XmlFile.h"
#include "RudeCorpus.h"
#include "RudeDds.h"
#include "RudeEntityComponent.h"
#include "RudeArchetype.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "RudeCarGenComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "ContentStreaming.h"
#include "Containers/Ticker.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/LevelStreaming.h"
#include "ShaderCompiler.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RudeToolsetInternal.h"
#include "MeshReductionSettings.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "IMeshReductionManagerModule.h"
#include "IMeshMergeUtilities.h"
#include "MeshMergeModule.h"
#include "MeshMerge/MeshMergingSettings.h"
#include "Engine/MaterialMerging.h"
#include "Materials/MaterialInstanceConstant.h"
#include "IMeshReductionInterfaces.h"
#include "OverlappingCorners.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "UObject/SavePackage.h"

// =========================== LOD lineage (ENGINEERING_LOG laws 24-28) ===========================
// Measured on downtown 2026-09-06 (158 ymaps, 14,248 entities; maintainer lane `lod_rules`*.py):
//   24. parentIndex points into the ymap named by CMapData/parent OR into the entity's own ymap -
//       whichever holds the entity exactly one LOD level coarser (2,813/2,813 unique, 0 ambiguous).
//   25. numChildren is a pure count (14,236/14,248; the 12 misses have children outside the district).
//   26. lodDist and childLodDist are AUTHORED (entity == archetype only 2,722/14,248): never derived.
//   27. HD vs ORPHANHD is exactly "has a parent" (1,758/1,758 and 11,394/11,394).
//   28. the extents boxes follow no reproducible formula: containing boxes, grow-only.
static int32 RudeLodRank(const FString& Level)
{
	static const TCHAR* Levels[] = { TEXT("LODTYPES_DEPTH_HD"), TEXT("LODTYPES_DEPTH_LOD"), TEXT("LODTYPES_DEPTH_SLOD1"),
		TEXT("LODTYPES_DEPTH_SLOD2"), TEXT("LODTYPES_DEPTH_SLOD3"), TEXT("LODTYPES_DEPTH_SLOD4") };
	for (int32 i = 0; i < UE_ARRAY_COUNT(Levels); ++i) { if (Level == Levels[i]) { return i; } }
	if (Level == TEXT("LODTYPES_DEPTH_ORPHANHD")) { return 0; }
	return -1;
}
static bool RudeIsHdLevel(const FString& Level)
{
	return Level == TEXT("LODTYPES_DEPTH_HD") || Level == TEXT("LODTYPES_DEPTH_ORPHANHD");
}

void RudeResolveLodLineage(UWorld* World, const TMap<FString, FString>& YmapParent,
                           int32& OutLinks, int32& OutUnresolved, int32& OutPartial)
{
	OutLinks = OutUnresolved = OutPartial = 0;
	TMap<FString, TMap<int32, URudeEntityComponent*>> ByYmap;   // ymap (lower) -> source ordinal -> component
	TArray<URudeEntityComponent*> All;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
		if (!R || R->SourceIndex < 0 || R->SourceYmap.IsEmpty()) { continue; }
		const FString Y = R->SourceYmap.ToLower();
		ByYmap.FindOrAdd(Y).Add(R->SourceIndex, R);
		if (const FString* P = YmapParent.Find(Y)) { R->SourceYmapParent = *P; }
		R->LodParent.Reset();
		R->LodChildren.Reset();
		R->bLodPartial = false;
		All.Add(R);
	}
	for (URudeEntityComponent* R : All)
	{
		if (R->ParentIndex < 0) { continue; }
		const int32 Want = RudeLodRank(R->LodLevel) + 1;
		URudeEntityComponent* Parent = nullptr;
		const TArray<FString> Tables = { R->SourceYmapParent.ToLower(), R->SourceYmap.ToLower() };
		for (const FString& Y : Tables)
		{
			if (Y.IsEmpty()) { continue; }
			if (TMap<int32, URudeEntityComponent*>* M = ByYmap.Find(Y))
			{
				if (URudeEntityComponent** C = M->Find(R->ParentIndex))
				{
					if (RudeLodRank((*C)->LodLevel) == Want) { Parent = *C; break; }
				}
			}
		}
		if (!Parent)
		{
			++OutUnresolved;
			R->GetOwner()->Tags.AddUnique(FName(TEXT("RUDE_LOD_UNRESOLVED")));
			continue;
		}
		R->LodParent = Parent->GetOwner();
		Parent->LodChildren.Add(R->GetOwner());
		++OutLinks;
	}
	for (URudeEntityComponent* R : All)
	{
		if (R->NumChildren != R->LodChildren.Num())
		{
			R->bLodPartial = true;
			R->GetOwner()->Tags.AddUnique(FName(TEXT("RUDE_LOD_PARTIAL")));
			++OutPartial;
		}
	}
}


// =========================== entity lights (Tier 1: lights on the owning actor) ===========================
static FString RudeNum(double V);   // defined with the entity writer below
static bool RudeRawItems(const FString& Text, const TCHAR* BlockTag, TArray<FString>& Out);

// =========================== car generators (ymap <carGenerators>) ===========================
namespace RudeCarGen
{
	// the item as the file spells it (2-space Item, 3-space fields; measured shape 723/723 downtown)
	static FString Spell(const URudeCarGenComponent* C, const FTransform& Xf)
	{
		const FVector P = Xf.GetLocation();
		const FVector Fwd = Xf.GetRotation().GetForwardVector();
		const double Ox = Fwd.X * C->Length, Oy = -Fwd.Y * C->Length;   // UE forward -> RAGE (x, -y)
		// an unchanged heading keeps the file's own digits (a recomputed vector re-spells 5.603781 as 5.60378075)
		const bool bSameHeading = !C->SourceOrientXText.IsEmpty() && Xf.GetRotation().Equals(C->SourceTransform.GetRotation(), 1e-4f);
		FString O;
		O += TEXT("  <Item>\n");
		O += FString::Printf(TEXT("   <position x=\"%s\" y=\"%s\" z=\"%s\" />\n"), *RudeNum(P.X / 100.0), *RudeNum(-P.Y / 100.0), *RudeNum(P.Z / 100.0));
		O += FString::Printf(TEXT("   <orientX value=\"%s\" />\n   <orientY value=\"%s\" />\n"), bSameHeading ? *C->SourceOrientXText : *RudeNum(Ox), bSameHeading ? *C->SourceOrientYText : *RudeNum(Oy));
		O += FString::Printf(TEXT("   <perpendicularLength value=\"%s\" />\n"), *RudeNum(C->PerpendicularLength));
		if (C->CarModel.IsEmpty()) { O += TEXT("   <carModel />\n"); } else { O += TEXT("   <carModel>"); RudeXmlEscapeInto(O, C->CarModel); O += TEXT("</carModel>\n"); }
		O += FString::Printf(TEXT("   <flags value=\"%u\" />\n"), C->Flags);
		O += FString::Printf(TEXT("   <bodyColorRemap1 value=\"%d\" />\n   <bodyColorRemap2 value=\"%d\" />\n   <bodyColorRemap3 value=\"%d\" />\n   <bodyColorRemap4 value=\"%d\" />\n"), C->BodyColorRemap1, C->BodyColorRemap2, C->BodyColorRemap3, C->BodyColorRemap4);
		if (C->PopGroup.IsEmpty()) { O += TEXT("   <popGroup />\n"); } else { O += TEXT("   <popGroup>"); RudeXmlEscapeInto(O, C->PopGroup); O += TEXT("</popGroup>\n"); }
		O += FString::Printf(TEXT("   <livery value=\"%d\" />\n"), C->Livery);
		O += TEXT("  </Item>\n");
		return O;
	}
	static bool Untouched(const URudeCarGenComponent* C, const FTransform& Xf)
	{
		// position + heading only: the marker's scale is its slab size, not a field
		return !C->SourceXml.IsEmpty() && C->SourceIndex >= 0 && Xf.GetLocation().Equals(C->SourceTransform.GetLocation(), 1e-3f)
			&& Xf.GetRotation().Equals(C->SourceTransform.GetRotation(), 1e-4f) && C->FieldsKey() == C->SourceFieldsKey;
	}
	// every marker of a ymap: ordinal -> (component, transform); added ones (no ordinal) after
	struct FMark { URudeCarGenComponent* C; FTransform Xf; };
	static void Collect(UWorld* World, const FString& YmapLower, TMap<int32, FMark>& ByOrdinal, TArray<FMark>& Added)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			URudeCarGenComponent* C = It->FindComponentByClass<URudeCarGenComponent>();
			if (!C || C->SourceYmap.ToLower() != YmapLower) { continue; }
			FMark M{ C, It->GetActorTransform() };
			if (C->SourceIndex >= 0) { ByOrdinal.Add(C->SourceIndex, M); } else { Added.Add(M); }
		}
	}
}

namespace RudeLights
{
	struct FInst
	{
		FVector Posn = FVector::ZeroVector, Colour = FVector(255, 255, 255), Dir = FVector(0, 0, -1), Off = FVector::ZeroVector;
		double Intensity = 0, Falloff = 0, FalloffExp = 0, ConeIn = 0, ConeOut = 0;
		int32 Type = 2;
		FXmlNode* Node = nullptr;
	};
	static bool Vec(const FXmlNode* N, const TCHAR* Tag, FVector& Out)
	{
		const FXmlNode* C = N ? N->FindChildNode(Tag) : nullptr;
		if (!C) { return false; }
		TArray<FString> T; C->GetContent().ParseIntoArrayWS(T);
		if (T.Num() < 3) { return false; }
		Out = FVector(FCString::Atod(*T[0]), FCString::Atod(*T[1]), FCString::Atod(*T[2]));
		return true;
	}
	static double Num(const FXmlNode* N, const TCHAR* Tag, double Def)
	{
		const FXmlNode* C = N ? N->FindChildNode(Tag) : nullptr;
		return C ? FCString::Atod(*C->GetAttribute(TEXT("value"))) : Def;
	}
	// every instance of every CExtensionDefLightEffect, in document order (index = light index)
	static void Parse(FXmlNode* Root, TArray<FInst>& Out)
	{
		FXmlNode* Ext = Root->FindChildNode(TEXT("extensions"));
		if (!Ext) { Ext = Root; }
		for (FXmlNode* Item : Ext->GetChildrenNodes())
		{
			if (Item->GetAttribute(TEXT("type")) != TEXT("CExtensionDefLightEffect")) { continue; }
			FVector Off = FVector::ZeroVector;
			if (const FXmlNode* O = Item->FindChildNode(TEXT("offsetPosition")))
			{
				Off = FVector(FCString::Atod(*O->GetAttribute(TEXT("x"))), FCString::Atod(*O->GetAttribute(TEXT("y"))), FCString::Atod(*O->GetAttribute(TEXT("z"))));
			}
			FXmlNode* Inst = Item->FindChildNode(TEXT("instances"));
			if (!Inst) { continue; }
			for (FXmlNode* L : Inst->GetChildrenNodes())
			{
				FInst I;
				I.Node = L; I.Off = Off;
				Vec(L, TEXT("posn"), I.Posn); Vec(L, TEXT("colour"), I.Colour); Vec(L, TEXT("direction"), I.Dir);
				I.Intensity = Num(L, TEXT("intensity"), 0); I.Falloff = Num(L, TEXT("falloff"), 0); I.FalloffExp = Num(L, TEXT("falloffExponent"), 0);
				I.ConeIn = Num(L, TEXT("coneInnerAngle"), 0); I.ConeOut = Num(L, TEXT("coneOuterAngle"), 0); I.Type = (int32)Num(L, TEXT("lightType"), 2);
				Out.Add(I);
			}
		}
	}
	// the fields a component mirrors, hashed the same way from either side
	static uint32 Key(const FVector& LocalCm, const FLinearColor& C, double IntensityCd, double RadiusCm, double Exp, double In, double Out, const FRotator& Rot)
	{
		const FString S = FString::Printf(TEXT("%.1f,%.1f,%.1f|%.3f,%.3f,%.3f|%.2f|%.1f|%.2f|%.2f|%.2f|%.2f,%.2f,%.2f"),
			LocalCm.X, LocalCm.Y, LocalCm.Z, C.R, C.G, C.B, IntensityCd, RadiusCm, Exp, In, Out, Rot.Pitch, Rot.Yaw, Rot.Roll);
		return FCrc::StrCrc32(*S);
	}
	static FString Tag(const FName& N, const TCHAR* Prefix) { const FString S = N.ToString(); return S.StartsWith(Prefix) ? S.Mid(FCString::Strlen(Prefix)) : FString(); }
	static uint32 KeyOf(const ULightComponent* LC)
	{
		double Exp = 0, In = 0, Out = 0;
		if (const UPointLightComponent* P = Cast<UPointLightComponent>(LC)) { Exp = P->LightFalloffExponent; }
		if (const USpotLightComponent* Sp = Cast<USpotLightComponent>(LC)) { In = Sp->InnerConeAngle; Out = Sp->OuterConeAngle; }
		const UPointLightComponent* PL = Cast<UPointLightComponent>(LC);
		return Key(LC->GetRelativeLocation(), LC->GetLightColor(), LC->Intensity, PL ? PL->AttenuationRadius : 0.0, Exp, In, Out, LC->GetRelativeRotation());
	}
}

int32 RudeAttachEntityLights(AActor* Actor, URudeEntityComponent* R)
{
	if (!Actor || !R || !R->ExtensionsXml.Contains(TEXT("CExtensionDefLightEffect"))) { return 0; }
	FXmlFile Doc(TEXT("<root>") + R->ExtensionsXml + TEXT("</root>"), EConstructMethod::ConstructFromBuffer);
	if (!Doc.IsValid() || !Doc.GetRootNode()) { return 0; }
	TArray<RudeLights::FInst> Insts;
	RudeLights::Parse(Doc.GetRootNode(), Insts);
	USceneComponent* RootC = Actor->GetRootComponent();
	int32 N = 0;
	for (int32 i = 0; i < Insts.Num(); ++i)
	{
		const RudeLights::FInst& I = Insts[i];
		const FVector L = I.Posn + I.Off;
		const FVector LocalCm(L.X * 100.0, -L.Y * 100.0, L.Z * 100.0);
		const FVector DirUe = FVector(I.Dir.X, -I.Dir.Y, I.Dir.Z).GetSafeNormal();
		UPointLightComponent* LC = nullptr;
		if (I.Type == 2)
		{
			USpotLightComponent* Sp = NewObject<USpotLightComponent>(Actor, *FString::Printf(TEXT("RudeLight%d"), i));
			Sp->InnerConeAngle = (float)FMath::Clamp(I.ConeIn, 0.0, 89.0);
			Sp->OuterConeAngle = (float)FMath::Clamp(I.ConeOut, 1.0, 89.0);
			LC = Sp;
		}
		else
		{
			LC = NewObject<UPointLightComponent>(Actor, *FString::Printf(TEXT("RudeLight%d"), i));
			if (I.Type == 4) { LC->SourceLength = 50.f; }   // capsule: a rough length
		}
		LC->SetMobility(EComponentMobility::Static);
		LC->SetIntensityUnits(ELightUnits::Candelas);
		LC->Intensity = (float)(I.Intensity * 100.0);   // RAGE intensity -> candelas, a rough factor
		LC->SetLightColor(FLinearColor(I.Colour.X / 255.f, I.Colour.Y / 255.f, I.Colour.Z / 255.f));
		LC->AttenuationRadius = (float)FMath::Max(I.Falloff * 100.0, 10.0);
		LC->bUseInverseSquaredFalloff = false;
		LC->LightFalloffExponent = (float)FMath::Clamp(I.FalloffExp, 2.0, 16.0);
		LC->CastShadows = false;
		LC->SetupAttachment(RootC);
		LC->SetRelativeLocation(LocalCm);
		LC->SetRelativeRotation(FRotationMatrix::MakeFromX(DirUe.IsNearlyZero() ? FVector(0, 0, -1) : DirUe).Rotator());
		LC->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_LIGHT:%d"), i)));
		LC->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_LIGHT_KEY:%u"), RudeLights::KeyOf(LC))));
		LC->RegisterComponent();
		Actor->AddInstanceComponent(LC);
		++N;
	}
	return N;
}

int32 RudeSyncEntityLights(AActor* Actor, URudeEntityComponent* R)
{
	if (!Actor || !R || !R->ExtensionsXml.Contains(TEXT("CExtensionDefLightEffect"))) { return 0; }
	TArray<UPointLightComponent*> Comps;
	Actor->GetComponents<UPointLightComponent>(Comps);
	TMap<int32, UPointLightComponent*> ByIndex;
	bool bAnyChanged = false;
	for (UPointLightComponent* LC : Comps)
	{
		int32 Idx = -1; uint32 Stored = 0; bool bHas = false;
		for (const FName& T : LC->ComponentTags)
		{
			const FString A = RudeLights::Tag(T, TEXT("RUDE_LIGHT:")); if (!A.IsEmpty()) { Idx = FCString::Atoi(*A); }
			const FString K = RudeLights::Tag(T, TEXT("RUDE_LIGHT_KEY:")); if (!K.IsEmpty()) { Stored = (uint32)FCString::Strtoui64(*K, nullptr, 10); bHas = true; }
		}
		if (Idx < 0) { continue; }
		ByIndex.Add(Idx, LC);
		if (!bHas || RudeLights::KeyOf(LC) != Stored) { bAnyChanged = true; }
	}
	if (!bAnyChanged) { return 0; }
	FXmlFile Doc(TEXT("<root>") + R->ExtensionsXml + TEXT("</root>"), EConstructMethod::ConstructFromBuffer);
	if (!Doc.IsValid() || !Doc.GetRootNode()) { return 0; }
	TArray<RudeLights::FInst> Insts;
	RudeLights::Parse(Doc.GetRootNode(), Insts);
	int32 Rewritten = 0;
	auto SetVal = [](FXmlNode* N, const TCHAR* Tag, const FString& V)
	{
		FXmlNode* C = N->FindChildNode(Tag);
		if (!C) { return; }
		TArray<FXmlAttribute> A = C->GetAttributes();
		bool bSet = false;
		for (FXmlAttribute& At : A) { if (At.GetTag() == TEXT("value")) { At = FXmlAttribute(TEXT("value"), V); bSet = true; } }
		if (!bSet) { A.Add(FXmlAttribute(TEXT("value"), V)); }
		C->SetAttributes(A);
	};
	for (int32 i = 0; i < Insts.Num(); ++i)
	{
		UPointLightComponent** Found = ByIndex.Find(i);
		if (!Found) { continue; }
		UPointLightComponent* LC = *Found;
		uint32 Stored = 0; bool bHas = false;
		for (const FName& T : LC->ComponentTags) { const FString K = RudeLights::Tag(T, TEXT("RUDE_LIGHT_KEY:")); if (!K.IsEmpty()) { Stored = (uint32)FCString::Strtoui64(*K, nullptr, 10); bHas = true; } }
		if (bHas && RudeLights::KeyOf(LC) == Stored) { continue; }
		RudeLights::FInst& I = Insts[i];
		// Only a field whose component value differs from the SOURCE's own mapping is rewritten: UE holds
		// some values approximately (a 90 deg cone clamps to 89, exponents clamp to 2..16), and an untouched
		// field must keep the game's spelling (measured 2026-09-06: coneOuterAngle 90 -> 89 on an intensity edit).
		const FVector SrcL = I.Posn + I.Off;
		const FVector SrcLocalCm(SrcL.X * 100.0, -SrcL.Y * 100.0, SrcL.Z * 100.0);
		const FVector Rel = LC->GetRelativeLocation();
		if (!Rel.Equals(SrcLocalCm, 0.05))
		{
			const FVector LocalRage(Rel.X / 100.0, -Rel.Y / 100.0, Rel.Z / 100.0);
			const FVector Posn = LocalRage - I.Off;
			I.Node->FindChildNode(TEXT("posn"))->SetContent(FString::Printf(TEXT("%s %s %s"), *RudeNum(Posn.X), *RudeNum(Posn.Y), *RudeNum(Posn.Z)));
		}
		const FLinearColor C = LC->GetLightColor();
		const int32 Cr = FMath::RoundToInt(C.R * 255.f), Cg = FMath::RoundToInt(C.G * 255.f), Cb = FMath::RoundToInt(C.B * 255.f);
		if (Cr != (int32)I.Colour.X || Cg != (int32)I.Colour.Y || Cb != (int32)I.Colour.Z)
		{
			if (FXmlNode* Cn = I.Node->FindChildNode(TEXT("colour"))) { Cn->SetContent(FString::Printf(TEXT("%d %d %d"), Cr, Cg, Cb)); }
		}
		if (!FMath::IsNearlyEqual(LC->Intensity / 100.0, I.Intensity, 1e-3)) { SetVal(I.Node, TEXT("intensity"), RudeNum(LC->Intensity / 100.0)); }
		if (!FMath::IsNearlyEqual(LC->AttenuationRadius / 100.0, FMath::Max(I.Falloff, 0.1), 1e-3)) { SetVal(I.Node, TEXT("falloff"), RudeNum(LC->AttenuationRadius / 100.0)); }
		if (!FMath::IsNearlyEqual((double)LC->LightFalloffExponent, FMath::Clamp(I.FalloffExp, 2.0, 16.0), 1e-3)) { SetVal(I.Node, TEXT("falloffExponent"), RudeNum(LC->LightFalloffExponent)); }
		if (const USpotLightComponent* Sp = Cast<USpotLightComponent>(LC))
		{
			if (!FMath::IsNearlyEqual((double)Sp->InnerConeAngle, FMath::Clamp(I.ConeIn, 0.0, 89.0), 1e-3)) { SetVal(I.Node, TEXT("coneInnerAngle"), RudeNum(Sp->InnerConeAngle)); }
			if (!FMath::IsNearlyEqual((double)Sp->OuterConeAngle, FMath::Clamp(I.ConeOut, 1.0, 89.0), 1e-3)) { SetVal(I.Node, TEXT("coneOuterAngle"), RudeNum(Sp->OuterConeAngle)); }
		}
		const FVector SrcDirUe = FVector(I.Dir.X, -I.Dir.Y, I.Dir.Z).GetSafeNormal();
		const FVector Fwd = LC->GetRelativeRotation().Vector();
		if (!Fwd.Equals(SrcDirUe.IsNearlyZero() ? FVector(0, 0, -1) : SrcDirUe, 1e-3))
		{
			if (FXmlNode* Dn = I.Node->FindChildNode(TEXT("direction"))) { Dn->SetContent(FString::Printf(TEXT("%s %s %s"), *RudeNum(Fwd.X), *RudeNum(-Fwd.Y), *RudeNum(Fwd.Z))); }
		}
		// the key now matches the written values
		TArray<FName> Tags;
		for (const FName& T : LC->ComponentTags) { if (!T.ToString().StartsWith(TEXT("RUDE_LIGHT_KEY:"))) { Tags.Add(T); } }
		Tags.Add(FName(*FString::Printf(TEXT("RUDE_LIGHT_KEY:%u"), RudeLights::KeyOf(LC))));
		LC->ComponentTags = Tags;
		++Rewritten;
	}
	if (Rewritten > 0)
	{
		FString O;
		FXmlNode* Ext = Doc.GetRootNode()->FindChildNode(TEXT("extensions"));
		RudeXmlNodeToString(Ext ? Ext : Doc.GetRootNode(), O, 0);
		R->ExtensionsXml = O;
	}
	return Rewritten;
}

// What the links say the derived fields should be. Ordinal: component -> its ordinal in the file the
// export is about to write (empty map = use SourceIndex, the audit's view). Returns false with a named
// reason when the link is one the ymap format cannot express.
struct FRudeLodDerived
{
	int32 ParentIndex = -1;
	int32 NumChildren = 0;
	FString LodLevel;
	bool bChanged = false;
};
static bool RudeDeriveLodFields(const URudeEntityComponent* R, const TMap<const AActor*, URudeEntityComponent*>& Comp,
                                const TMap<const URudeEntityComponent*, int32>& Kids, const TMap<const URudeEntityComponent*, int32>& Ordinal,
                                const FString& NewYmap, FRudeLodDerived& Out, FString& Why)
{
	Out.ParentIndex = -1;
	Out.NumChildren = R->bLodPartial ? R->NumChildren : Kids.FindRef(R);
	Out.LodLevel = R->LodLevel;
	if (!R->LodParent.IsNull())
	{
		const URudeEntityComponent* P = Comp.FindRef(R->LodParent.Get());
		if (!P) { Why = TEXT("LodParent actor carries no RUDE entity"); return false; }
		const FString YmapR = R->SourceYmap.IsEmpty() ? NewYmap : R->SourceYmap.ToLower();
		const FString YmapP = P->SourceYmap.IsEmpty() ? NewYmap : P->SourceYmap.ToLower();
		if (YmapP != YmapR && YmapP != R->SourceYmapParent.ToLower())
		{
			Why = FString::Printf(TEXT("LodParent lives in '%s', which is neither this entity's ymap '%s' nor its parent ymap '%s' - the format cannot point there"),
				*YmapP, *YmapR, *R->SourceYmapParent.ToLower());
			return false;
		}
		if (RudeLodRank(P->LodLevel) != RudeLodRank(R->LodLevel) + 1)
		{
			Why = FString::Printf(TEXT("LodParent is %s, not the level one step coarser than %s"), *P->LodLevel, *R->LodLevel);
			return false;
		}
		Out.ParentIndex = Ordinal.Contains(P) ? Ordinal[P] : P->SourceIndex;
	}
	if (RudeIsHdLevel(R->LodLevel)) { Out.LodLevel = !R->LodParent.IsNull() ? TEXT("LODTYPES_DEPTH_HD") : TEXT("LODTYPES_DEPTH_ORPHANHD"); }
	Out.bChanged = Out.ParentIndex != R->ParentIndex || Out.NumChildren != R->NumChildren || Out.LodLevel != R->LodLevel;
	return true;
}
// Children per parent, from the links as they stand now.
static void RudeCountLodKids(UWorld* World, TMap<const AActor*, URudeEntityComponent*>& Comp, TMap<const URudeEntityComponent*, int32>& Kids, TArray<URudeEntityComponent*>& All)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>()) { Comp.Add(*It, R); All.Add(R); }
	}
	for (const URudeEntityComponent* R : All)
	{
		if (!R->LodParent.IsNull()) { if (const URudeEntityComponent* P = Comp.FindRef(R->LodParent.Get())) { Kids.FindOrAdd(P)++; } }
	}
}

// ---- ExportLevelYmaps --------------------------------------------------------------------
// A number as the game's files spell it: shortest fixed form, no trailing zeros ("1", "15000",
// "-153.610275"). Only EDITED entities are spelled this way; untouched ones go out verbatim.
// The game's values are float32 and the corpus spells them the way .NET's "R" format does (the
// reference-parity oracle ROUT reproduces byte-for-byte): SEVEN significant digits when that reads
// back to the same float, otherwise NINE - never eight (178.533508, 37.2113342, 0.9848078, and
// 0.17364794 is %.9g with its trailing zero dropped). Measured 2026-09-06 on the re-parent gate:
// "%.6f" re-spelt 37.2113342 as 37.211334, and a shortest-round-trip search gave 178.53351 (8).
static FString RudeNum(double V)
{
	const float F = (float)V;
	FString S = FString::Printf(TEXT("%.7g"), (double)F);
	if ((float)FCString::Atod(*S) != F) { S = FString::Printf(TEXT("%.9g"), (double)F); }
	if (S.Contains(TEXT("e")))
	{
		// tiny/huge magnitudes: fall back to a plain decimal spelling
		S = FString::Printf(TEXT("%.9f"), (double)F);
		while (S.EndsWith(TEXT("0"))) { S.LeftChopInline(1); }
		if (S.EndsWith(TEXT("."))) { S.LeftChopInline(1); }
	}
	if (S == TEXT("-0")) { S = TEXT("0"); }
	return S;
}

// RudeNum for other translation units (RudeMloExport.cpp); declared in RudeToolsetInternal.h.
FString RudeNumText(double V) { return RudeNum(V); }

// A CEntityDef from the component + the actor's transform, in the game's field order (verified
// against dt1_02.ymap.xml 2026-09-05). UE -> RAGE: position /100 with Y mirrored; the ymap stores
// the INVERSE orientation, so the actor quaternion goes out as (x, -y, z, w); scale XY from X.
static FString RudeEntityDefXml(const URudeEntityComponent* R, const FTransform& Xf)
{
	const FVector P = Xf.GetLocation();
	const FQuat Q = Xf.GetRotation().GetNormalized();
	const FVector S = Xf.GetScale3D();
	FString O;
	O += TEXT("  <Item type=\"CEntityDef\">\n");
	O += TEXT("   <archetypeName>"); RudeXmlEscapeInto(O, R->ArchetypeName); O += TEXT("</archetypeName>\n");
	O += FString::Printf(TEXT("   <flags value=\"%u\" />\n"), R->Flags);
	O += FString::Printf(TEXT("   <guid value=\"%u\" />\n"), R->Guid);
	// An UNMOVED entity keeps the game's own position/rotation lines verbatim: the rotation is stored
	// inverted, and inverting a float32 quaternion in double and back does not land on the same float
	// (measured 2026-09-06: w 0.9961947 -> 0.996194661 on a light-only edit).
	auto SourceLine = [&](const TCHAR* Tag) -> FString
	{
		if (R->SourceXml.IsEmpty() || !Xf.Equals(R->SourceTransform, 1e-3f)) { return FString(); }
		const FString Open = FString::Printf(TEXT("<%s "), Tag);
		const int32 A = R->SourceXml.Find(Open);
		if (A == INDEX_NONE) { return FString(); }
		const int32 B = R->SourceXml.Find(TEXT("/>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, A);
		return B == INDEX_NONE ? FString() : R->SourceXml.Mid(A, B + 2 - A);
	};
	const FString SrcPos = SourceLine(TEXT("position")), SrcRot = SourceLine(TEXT("rotation"));
	if (!SrcPos.IsEmpty()) { O += TEXT("   ") + SrcPos + TEXT("\n"); }
	else { O += FString::Printf(TEXT("   <position x=\"%s\" y=\"%s\" z=\"%s\" />\n"), *RudeNum(P.X / 100.0), *RudeNum(-P.Y / 100.0), *RudeNum(P.Z / 100.0)); }
	if (!SrcRot.IsEmpty()) { O += TEXT("   ") + SrcRot + TEXT("\n"); }
	else { O += FString::Printf(TEXT("   <rotation x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />\n"), *RudeNum(Q.X), *RudeNum(-Q.Y), *RudeNum(Q.Z), *RudeNum(Q.W)); }
	O += FString::Printf(TEXT("   <scaleXY value=\"%s\" />\n"), *RudeNum(S.X));
	O += FString::Printf(TEXT("   <scaleZ value=\"%s\" />\n"), *RudeNum(S.Z));
	O += FString::Printf(TEXT("   <parentIndex value=\"%d\" />\n"), R->ParentIndex);
	O += FString::Printf(TEXT("   <lodDist value=\"%s\" />\n"), *RudeNum(R->LodDist));
	O += FString::Printf(TEXT("   <childLodDist value=\"%s\" />\n"), *RudeNum(R->ChildLodDist));
	O += TEXT("   <lodLevel>"); RudeXmlEscapeInto(O, R->LodLevel); O += TEXT("</lodLevel>\n");
	O += FString::Printf(TEXT("   <numChildren value=\"%d\" />\n"), R->NumChildren);
	O += TEXT("   <priorityLevel>"); RudeXmlEscapeInto(O, R->PriorityLevel); O += TEXT("</priorityLevel>\n");
	if (R->ExtensionsXml.TrimStartAndEnd().IsEmpty()) { O += TEXT("   <extensions />\n"); }
	else
	{
		// carried verbatim (RudeXmlNodeToString spelled it at depth 0; re-indent by 3)
		TArray<FString> Lines;
		R->ExtensionsXml.ParseIntoArrayLines(Lines);
		for (const FString& L : Lines) { O += TEXT("   "); O += L; O += TEXT("\n"); }
	}
	O += FString::Printf(TEXT("   <ambientOcclusionMultiplier value=\"%s\" />\n"), *RudeNum(R->AmbientOcclusionMultiplier));
	O += FString::Printf(TEXT("   <artificialAmbientOcclusion value=\"%s\" />\n"), *RudeNum(R->ArtificialAmbientOcclusion));
	O += FString::Printf(TEXT("   <tintValue value=\"%u\" />\n"), R->TintValue);
	O += TEXT("  </Item>\n");
	return O;
}

struct FRudeExportEntity
{
	URudeEntityComponent* R = nullptr;
	FTransform Xf;
	bool bUntouched = false;
};

FString URudeToolset::ExportLevelYmaps(const FString& OutDir, const FString& YmapFilter,
                                       const FString& CorpusRoot, const FString& NewEntitiesYmap)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an OutDir for the FiveM resource")); }
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }

	TSet<FString> Wanted;
	{
		TArray<FString> Parts;
		YmapFilter.ParseIntoArray(Parts, TEXT(","), true);
		for (FString P : Parts) { P.TrimStartAndEndInline(); if (!P.IsEmpty()) { Wanted.Add(P.ToLower()); } }
	}
	const FString NewYmap = NewEntitiesYmap.TrimStartAndEnd().ToLower();

	// ---- 1) read the level back: every entity component, grouped by its source ymap
	TMap<FString, TArray<FRudeExportEntity>> Groups;
	int32 Seen = 0, Unsourced = 0, UnsourcedDropped = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
		if (!R) { continue; }
		++Seen;
		FString Ymap = R->SourceYmap.ToLower();
		if (Ymap.IsEmpty())
		{
			++Unsourced;
			if (NewYmap.IsEmpty()) { ++UnsourcedDropped; continue; }
			Ymap = NewYmap;
		}
		if (Wanted.Num() > 0 && !Wanted.Contains(Ymap)) { continue; }
		FRudeExportEntity E;
		E.R = R;
		E.Xf = It->GetActorTransform();
		// An UNMOVED entity spells its transform from the transform it was spawned with: the actor's
		// rotation lives in UE as Euler angles and comes back with its last digit moved (measured
		// 2026-09-06: w 0.9961947 -> 0.996194661 on a light-only edit). Moved = the actor's transform.
		if (R->SourceIndex >= 0 && E.Xf.Equals(R->SourceTransform, 1e-3f)) { E.Xf = R->SourceTransform; }
		Groups.FindOrAdd(Ymap).Add(E);
	}
	if (Groups.Num() == 0) { return Fail(FString::Printf(TEXT("no RUDE entities to export (%d components seen)"), Seen)); }

	// ---- 1b) LOD lineage -> fields (laws 24-27). The links are what gets edited; parentIndex,
	// numChildren and HD/ORPHANHD are derived from them HERE, before keying, so an unchanged lineage
	// keys identical (verbatim bytes) and a re-parent rebuilds exactly the entities whose fields moved.
	// lodDist / childLodDist are never touched (law 26). Ordinals: source entities keep theirs (deletions
	// in lineage files are refused below); added entities are appended in the export's own order.
	// ---- 1a) lights edited in UE -> their instance inside the carried <extensions> (only those fields move)
	int32 LightsRewritten = 0;
	for (auto& KV : Groups)
	{
		for (FRudeExportEntity& E : KV.Value) { LightsRewritten += RudeSyncEntityLights(E.R->GetOwner(), E.R); }
	}
	int32 LineageDerived = 0, CarGensRebuilt = 0, CarGensAdded = 0, CarGensRemoved = 0;
	TMap<FString, FString> LineageBad;   // ymap -> why the whole file is refused
	{
		TMap<const AActor*, URudeEntityComponent*> Comp;
		TMap<const URudeEntityComponent*, int32> Kids;
		TArray<URudeEntityComponent*> AllComps;
		RudeCountLodKids(World, Comp, Kids, AllComps);
		TMap<const URudeEntityComponent*, int32> Ordinal;
		for (auto& KV : Groups)
		{
			KV.Value.Sort([](const FRudeExportEntity& A, const FRudeExportEntity& B)
			{
				const int32 IA = A.R->SourceIndex < 0 ? INT32_MAX : A.R->SourceIndex;
				const int32 IB = B.R->SourceIndex < 0 ? INT32_MAX : B.R->SourceIndex;
				if (IA != IB) { return IA < IB; }
				return A.R->ArchetypeName < B.R->ArchetypeName;
			});
			int32 Next = 0;
			for (const FRudeExportEntity& E : KV.Value)
			{
				if (E.R->SourceIndex >= 0) { Next = FMath::Max(Next, E.R->SourceIndex + 1); }
			}
			for (const FRudeExportEntity& E : KV.Value)
			{
				Ordinal.Add(E.R, E.R->SourceIndex >= 0 ? E.R->SourceIndex : Next++);
			}
		}
		for (auto& KV : Groups)
		{
			for (FRudeExportEntity& E : KV.Value)
			{
				FRudeLodDerived D;
				FString Why;
				if (!RudeDeriveLodFields(E.R, Comp, Kids, Ordinal, NewYmap, D, Why))
				{
					if (!LineageBad.Contains(KV.Key)) { LineageBad.Add(KV.Key, FString::Printf(TEXT("%s: %s"), *E.R->GetOwner()->GetActorLabel(), *Why)); }
					continue;
				}
				if (D.bChanged)
				{
					E.R->ParentIndex = D.ParentIndex;
					E.R->NumChildren = D.NumChildren;
					E.R->LodLevel = D.LodLevel;
					++LineageDerived;
				}
			}
		}
	}
	for (auto& KV : Groups)
	{
		for (FRudeExportEntity& E : KV.Value)
		{
			const URudeEntityComponent* R = E.R;
			E.bUntouched = !R->SourceXml.IsEmpty() && R->SourceIndex >= 0
				&& E.Xf.Equals(R->SourceTransform, 1e-3f)
				&& R->FieldsKey() == R->SourceFieldsKey;
		}
	}

	IFileManager::Get().MakeDirectory(*(OutDir / TEXT("stream")), true);
	int32 YmapsWritten = 0, YmapsRefused = 0;
	int32 Kept = 0, Edited = 0, Added = 0, Removed = 0, EditsNotRebuilt = 0, ExtentsGrown = 0;
	FString Files, Refused;
	for (auto& KV : Groups)
	{
		const FString& YmapName = KV.Key;
		TArray<FRudeExportEntity>& Ents = KV.Value;
		if (const FString* Bad = LineageBad.Find(YmapName))
		{
			++YmapsRefused;
			Refused += FString::Printf(TEXT("%s\"%s: LOD link the format cannot express - %s\""),
				Refused.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, *RudeJsonEscape(*Bad));
			continue;
		}
		Ents.Sort([](const FRudeExportEntity& A, const FRudeExportEntity& B)
		{
			const int32 IA = A.R->SourceIndex < 0 ? INT32_MAX : A.R->SourceIndex;
			const int32 IB = B.R->SourceIndex < 0 ? INT32_MAX : B.R->SourceIndex;
			if (IA != IB) { return IA < IB; }
			return A.R->ArchetypeName < B.R->ArchetypeName;
		});

		// ---- 2) the source document (the file the entities came from), or a fresh one
		const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("ymap"), YmapName);
		TUniquePtr<FXmlFile> Src;
		FString SrcText;   // the file's own bytes: the output is spliced from these, never re-spelled
		if (Row)
		{
			const FString SrcPath = Corpus->PathOf(*Row);
			FFileHelper::LoadFileToString(SrcText, *SrcPath);
			Src = MakeUnique<FXmlFile>(SrcPath);
		}
		const FXmlNode* Root = (Src.IsValid() && Src->IsValid()) ? Src->GetRootNode() : nullptr;
		const FXmlNode* SrcEnts = Root ? Root->FindChildNode(TEXT("entities")) : nullptr;
		int32 SourceCount = 0;
		bool bLineage = false;
		if (SrcEnts)
		{
			for (const FXmlNode* Item : SrcEnts->GetChildrenNodes())
			{
				++SourceCount;
				const FXmlNode* Pi = Item->FindChildNode(TEXT("parentIndex"));
				const FXmlNode* Nc = Item->FindChildNode(TEXT("numChildren"));
				if ((Pi && FCString::Atoi(*Pi->GetAttribute(TEXT("value"))) >= 0)
					|| (Nc && FCString::Atoi(*Nc->GetAttribute(TEXT("value"))) > 0)) { bLineage = true; }
			}
		}
		int32 Present = 0;
		for (const FRudeExportEntity& E : Ents) { if (E.R->SourceIndex >= 0) { ++Present; } }
		const int32 RemovedHere = FMath::Max(0, SourceCount - Present);
		if (RemovedHere > 0 && bLineage)
		{
			// Deleting an entity shifts every ordinal after it, and parentIndex values (in this
			// file and in child ymaps) are ordinals. Refuse rather than ship broken LOD links.
			++YmapsRefused;
			Refused += FString::Printf(TEXT("%s\"%s: %d entity(ies) deleted from a file with LOD lineage - ordinals would shift; hide instead\""),
				Refused.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, RemovedHere);
			continue;
		}

		// ---- 3) entities block
		FString EntXml;
		// Only EDITED/ADDED entities can push the file's extents: an untouched file keeps its
		// extents verbatim (the game's own values encode archetype bounds and a streaming rule RUDE
		// does not reproduce - measured 2026-09-05: recomputing them changed all 148 clean files).
		double MinX = DBL_MAX, MinY = DBL_MAX, MinZ = DBL_MAX, MaxX = -DBL_MAX, MaxY = -DBL_MAX, MaxZ = -DBL_MAX;
		int32 Movers = 0;
		for (const FRudeExportEntity& E : Ents)
		{
			const bool bRebuildable = E.R->ItemType.IsEmpty() || E.R->ItemType == TEXT("CEntityDef");
			if (E.bUntouched || (!bRebuildable && !E.R->SourceXml.IsEmpty()))
			{
				// Untouched: verbatim. An EDITED CMloInstanceDef (or any other item kind) has no Wave-1
				// rebuild path - it goes out as read and the edit is COUNTED, never silently dropped.
				if (!E.bUntouched) { ++EditsNotRebuilt; }
				EntXml += E.R->SourceXml;
				if (!E.R->SourceXml.EndsWith(TEXT("\n"))) { EntXml += TEXT("\n"); }
				++Kept;
			}
			else
			{
				EntXml += RudeEntityDefXml(E.R, E.Xf);
				if (E.R->SourceIndex >= 0) { ++Edited; } else { ++Added; }
			}
			if (!E.bUntouched)
			{
				const FVector P = E.Xf.GetLocation();
				const double X = P.X / 100.0, Y = -P.Y / 100.0, Z = P.Z / 100.0;
				MinX = FMath::Min(MinX, X); MinY = FMath::Min(MinY, Y); MinZ = FMath::Min(MinZ, Z);
				MaxX = FMath::Max(MaxX, X); MaxY = FMath::Max(MaxY, Y); MaxZ = FMath::Max(MaxZ, Z);
				++Movers;
			}
		}
		Removed += RemovedHere;

		// ---- 4) the document around it: every other node re-spelled as read (shape-safe by the
		// XmlShapeRoundTrip gate); the two extents pairs only ever GROW to cover the entities.
		auto Extents = [&](const FXmlNode* N, const TCHAR* Tag, bool bMin, double Margin) -> FString
		{
			// Verbatim unless an edited/added entity sits OUTSIDE the stored box; then grow by exactly
			// what covers it (+Margin). Counted in the verdict so nobody mistakes it for the game's rule.
			if (N && Movers == 0) { FString O; RudeXmlNodeToString(N, O, 1); return O; }
			double Sx = bMin ? MinX - Margin : MaxX + Margin;
			double Sy = bMin ? MinY - Margin : MaxY + Margin;
			double Sz = bMin ? MinZ - Margin : MaxZ + Margin;
			if (N)
			{
				const double Ox = FCString::Atod(*N->GetAttribute(TEXT("x")));
				const double Oy = FCString::Atod(*N->GetAttribute(TEXT("y")));
				const double Oz = FCString::Atod(*N->GetAttribute(TEXT("z")));
				const bool bGrew = bMin ? (Sx < Ox || Sy < Oy || Sz < Oz) : (Sx > Ox || Sy > Oy || Sz > Oz);
				if (!bGrew) { FString O; RudeXmlNodeToString(N, O, 1); return O; }
				++ExtentsGrown;
				Sx = bMin ? FMath::Min(Sx, Ox) : FMath::Max(Sx, Ox);
				Sy = bMin ? FMath::Min(Sy, Oy) : FMath::Max(Sy, Oy);
				Sz = bMin ? FMath::Min(Sz, Oz) : FMath::Max(Sz, Oz);
			}
			return FString::Printf(TEXT(" <%s x=\"%s\" y=\"%s\" z=\"%s\" />\n"), Tag, *RudeNum(Sx), *RudeNum(Sy), *RudeNum(Sz));
		};
		FString Doc;
		if (Root && !SrcText.IsEmpty())
		{
			// SPLICE: the source bytes with (a) the top-level <entities> block replaced and (b) any
			// extents line that had to grow replaced. Nothing else is touched - not even whitespace.
			Doc = SrcText;
			auto ReplaceTopLevel = [&Doc](const FString& OpenTag, const FString& CloseTag, const FString& EmptyTag, const FString& NewBlock) -> bool
			{
				const int32 Open = Doc.Find(OpenTag, ESearchCase::CaseSensitive);
				if (Open != INDEX_NONE)
				{
					const int32 Close = Doc.Find(CloseTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
					if (Close == INDEX_NONE) { return false; }
					const int32 End = Close + CloseTag.Len();
					Doc = Doc.Left(Open) + NewBlock + Doc.Mid(End);
					return true;
				}
				const int32 Empty = Doc.Find(EmptyTag, ESearchCase::CaseSensitive);
				if (Empty == INDEX_NONE) { return false; }
				Doc = Doc.Left(Empty) + NewBlock + Doc.Mid(Empty + EmptyTag.Len());
				return true;
			};
			const bool bEntitiesChanged = (Edited + Added + RemovedHere) > 0 || Movers > 0;
			if (bEntitiesChanged || Ents.Num() != SourceCount)
			{
				// (the block is rebuilt from verbatim rows + edited rows; identical text when nothing moved)
			}
			if (!ReplaceTopLevel(TEXT("\n <entities>\n"), TEXT(" </entities>\n"), TEXT("\n <entities />\n"),
				TEXT("\n <entities>\n") + EntXml + TEXT(" </entities>\n")))
			{
				++YmapsRefused;
				Refused += FString::Printf(TEXT("%s\"%s: no top-level <entities> block found to splice\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *YmapName);
				continue;
			}
			// ---- car generators: the block is rebuilt only when a marker moved / changed / was added / removed
			{
				TMap<int32, RudeCarGen::FMark> ByOrdinal; TArray<RudeCarGen::FMark> AddedMarks;
				RudeCarGen::Collect(World, YmapName, ByOrdinal, AddedMarks);
				if (ByOrdinal.Num() > 0 || AddedMarks.Num() > 0)
				{
					TArray<FString> RawCg;
					RudeRawItems(SrcText, TEXT("carGenerators"), RawCg);
					// never rewrite from a slicing that disagrees with the source: count the file's own items
					int32 SourceItems = 0;
					{
						const int32 O = SrcText.Find(TEXT("\n <carGenerators"), ESearchCase::CaseSensitive);
						const int32 Cl = O == INDEX_NONE ? INDEX_NONE : SrcText.Find(TEXT("</carGenerators>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, O);
						if (O != INDEX_NONE && Cl != INDEX_NONE) { FString Blk = SrcText.Mid(O, Cl - O); int32 At = 0; while ((At = Blk.Find(TEXT("\n  <Item"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At)) != INDEX_NONE) { ++SourceItems; ++At; } }
					}
					if (RawCg.Num() != SourceItems)
					{
						Refused += FString::Printf(TEXT("%s\"%s: car generators: raw slices %d != source items %d - block left verbatim\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, RawCg.Num(), SourceItems);
						AddedMarks.Reset(); ByOrdinal.Reset();
					}
					bool bChanged = AddedMarks.Num() > 0 || (ByOrdinal.Num() > 0 && ByOrdinal.Num() != RawCg.Num());
					for (const auto& MK : ByOrdinal) { if (!RudeCarGen::Untouched(MK.Value.C, MK.Value.Xf)) { bChanged = true; break; } }
					if (bChanged)
					{
						FString Block;
						for (int32 o = 0; o < RawCg.Num(); ++o)
						{
							const RudeCarGen::FMark* M = ByOrdinal.Find(o);
							if (!M) { ++CarGensRemoved; continue; }   // marker deleted: the item goes (nothing references it by ordinal)
							if (RudeCarGen::Untouched(M->C, M->Xf)) { Block += RawCg[o]; if (!RawCg[o].EndsWith(TEXT("\n"))) { Block += TEXT("\n"); } }
							else { Block += RudeCarGen::Spell(M->C, M->Xf); ++CarGensRebuilt; }
						}
						for (const RudeCarGen::FMark& M : AddedMarks) { Block += RudeCarGen::Spell(M.C, M.Xf); ++CarGensAdded; }
						if (!ReplaceTopLevel(TEXT("\n <carGenerators itemType=\"CCarGen\">\n"), TEXT(" </carGenerators>\n"), TEXT("\n <carGenerators itemType=\"CCarGen\" />\n"),
							Block.IsEmpty() ? TEXT("\n <carGenerators itemType=\"CCarGen\" />\n") : (TEXT("\n <carGenerators itemType=\"CCarGen\">\n") + Block + TEXT(" </carGenerators>\n"))))
						{
							Refused += FString::Printf(TEXT("%s\"%s: car generators changed but no top-level <carGenerators> block to splice\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *YmapName);
						}
					}
				}
			}
			if (Movers > 0)
			{
				auto ReplaceExtents = [&](const TCHAR* Tag, bool bMin)
				{
					const FXmlNode* N = Root->FindChildNode(Tag);
					if (!N) { return; }
					const FString NewLine = Extents(N, Tag, bMin, 0.0);
					FString OldLine;
					RudeXmlNodeToString(N, OldLine, 1);
					if (NewLine == OldLine) { return; }   // did not grow: leave the source bytes alone
					// the source line as the file spells it: " <tag x=.. y=.. z=.. />" on its own line
					const FString Needle = FString::Printf(TEXT("\n <%s "), Tag);
					const int32 At = Doc.Find(Needle, ESearchCase::CaseSensitive);
					if (At == INDEX_NONE) { return; }
					const int32 LineEnd = Doc.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1);
					if (LineEnd == INDEX_NONE) { return; }
					Doc = Doc.Left(At + 1) + NewLine.TrimEnd() + Doc.Mid(LineEnd);
				};
				ReplaceExtents(TEXT("streamingExtentsMin"), true);
				ReplaceExtents(TEXT("streamingExtentsMax"), false);
				ReplaceExtents(TEXT("entitiesExtentsMin"), true);
				ReplaceExtents(TEXT("entitiesExtentsMax"), false);
			}
		}
		else
		{
			Doc = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapData>\n");
			// No source: a NEW ymap (authored content). Minimal, FiveM-loadable header.
			Doc += FString::Printf(TEXT(" <name>%s</name>\n <parent />\n <flags value=\"0\" />\n <contentFlags value=\"1\" />\n"), *YmapName);
			// A NEW file has no stored box: entity extents = positions +-10 m, streaming = +-500 m
			// (a plain-prop reach; the game's rule is not reproduced - verdict says extentsGrown).
			Doc += Extents(nullptr, TEXT("streamingExtentsMin"), true, 500.0);
			Doc += Extents(nullptr, TEXT("streamingExtentsMax"), false, 500.0);
			Doc += Extents(nullptr, TEXT("entitiesExtentsMin"), true, 10.0);
			Doc += Extents(nullptr, TEXT("entitiesExtentsMax"), false, 10.0);
			Doc += TEXT(" <entities>\n"); Doc += EntXml; Doc += TEXT(" </entities>\n");
			Doc += TEXT(" <containerLods />\n <boxOccluders />\n <occludeModels />\n <physicsDictionaries />\n <instancedData>\n  <ImapLink />\n  <PropInstanceList />\n  <GrassInstanceList />\n </instancedData>\n <timeCycleModifiers />\n <carGenerators />\n <LODLightsSOA>\n  <direction />\n  <falloff />\n  <falloffExponent />\n  <timeAndStateFlags />\n  <hash />\n  <coneInnerAngle />\n  <coneOuterAngleOrCapExt />\n  <coronaIntensity />\n </LODLightsSOA>\n <DistantLODLightsSOA>\n  <position />\n  <RGBI />\n  <numStreetLights value=\"0\" />\n  <category value=\"0\" />\n </DistantLODLightsSOA>\n <block>\n  <version value=\"0\" />\n  <flags value=\"0\" />\n  <name>"); Doc += YmapName; Doc += TEXT("</name>\n  <exportedBy>RUDE</exportedBy>\n  <owner />\n  <time />\n </block>\n");
			Doc += TEXT("</CMapData>\n");
		}
		const FString OutPath = OutDir / TEXT("stream") / (YmapName + TEXT(".ymap"));
		if (!FFileHelper::SaveStringToFile(Doc, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return Fail(FString::Printf(TEXT("cannot write %s"), *OutPath));
		}
		++YmapsWritten;
		Files += FString::Printf(TEXT("%s\"%s\""), Files.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(OutPath));
	}
	const FString Manifest = TEXT("fx_version 'cerulean'\ngame 'gta5'\nthis_is_a_map 'yes'\n");
	FFileHelper::SaveStringToFile(Manifest, *(OutDir / TEXT("fxmanifest.lua")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool bOk = YmapsWritten > 0 && YmapsRefused == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"componentsSeen\":%d,\"unsourced\":%d,\"unsourcedDropped\":%d,")
		TEXT("\"ymapsWritten\":%d,\"ymapsRefused\":%d,\"kept\":%d,\"edited\":%d,\"added\":%d,\"removed\":%d,")
		TEXT("\"editsNotRebuilt\":%d,\"extentsGrown\":%d,\"lineageDerived\":%d,\"lightsRewritten\":%d,\"carGensRebuilt\":%d,\"carGensAdded\":%d,\"carGensRemoved\":%d,\"files\":[%s],\"refused\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), Seen, Unsourced, UnsourcedDropped,
		YmapsWritten, YmapsRefused, Kept, Edited, Added, Removed, EditsNotRebuilt, ExtentsGrown, LineageDerived, LightsRewritten, CarGensRebuilt, CarGensAdded, CarGensRemoved, *Files, *Refused);
}

// ---- MoveRudeEntity (agent; the scriptable edit for the export gate) ----------------------
FString URudeToolset::MoveRudeEntity(const FString& SourceYmap, const FString& SourceIndex, const FString& DeltaCm)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const int32 Idx = FCString::Atoi(*SourceIndex);
	TArray<FString> P;
	DeltaCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	if (P.Num() != 3) { return Fail(TEXT("DeltaCm must be x,y,z in UE centimetres")); }
	const FVector D(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]));
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
		if (!R || R->SourceIndex != Idx || !R->SourceYmap.Equals(SourceYmap, ESearchCase::IgnoreCase)) { continue; }
		const FVector Before = It->GetActorLocation();
		It->SetActorLocation(Before + D);
		It->MarkPackageDirty();
		return FString::Printf(TEXT("{\"ok\":true,\"archetype\":\"%s\",\"before\":[%f,%f,%f],\"after\":[%f,%f,%f]}"),
			*RudeJsonEscape(R->ArchetypeName), Before.X, Before.Y, Before.Z, Before.X + D.X, Before.Y + D.Y, Before.Z + D.Z);
	}
	return Fail(FString::Printf(TEXT("no entity %s[%d] in the level"), *SourceYmap, Idx));
}

// ---- ProbeWorldPartitionLevel (agent; the WP4 spike) -------------------------------------
// One question, one answer, in-engine: can RUDE make a World Partition level from code, put a
// Data Layer in it, place an actor on that layer, and save the lot - headless? Every step reports.
FString URudeToolset::ProbeWorldPartitionLevel(const FString& LevelPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	if (!GEditor) { return Fail(TEXT("no GEditor")); }
	const FString Path = LevelPath.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(Path)) { return Fail(TEXT("LevelPath must be a long package name like /Game/RUDE/Levels/Probe")); }
	// 1) a fresh world with World Partition
	UWorld* World = GEditor->NewMap(/*bIsPartitionedWorld*/ true);
	if (!World) { return Fail(TEXT("NewMap(partitioned) returned null")); }
	UWorldPartition* WP = World->GetWorldPartition();
	if (!WP) { return Fail(TEXT("new map has no WorldPartition")); }
	// 2) a Data Layer asset + instance
	const FString DlPkgName = Path + TEXT("_DL_probe");
	UPackage* DlPkg = CreatePackage(*DlPkgName);
	UDataLayerAsset* DlAsset = NewObject<UDataLayerAsset>(DlPkg, FName(*FPackageName::GetLongPackageAssetName(DlPkgName)), RF_Public | RF_Standalone);
	DlAsset->SetType(EDataLayerType::Runtime);
	DlPkg->MarkPackageDirty();
	UDataLayerEditorSubsystem* DlSub = UDataLayerEditorSubsystem::Get();
	if (!DlSub) { return Fail(TEXT("no DataLayerEditorSubsystem")); }
	FDataLayerCreationParameters P;
	P.DataLayerAsset = DlAsset;
	P.WorldDataLayers = World->GetWorldDataLayers();
	UDataLayerInstance* Dl = DlSub->CreateDataLayerInstance(P);
	if (!Dl) { return Fail(TEXT("CreateDataLayerInstance returned null")); }
	// 3) an actor on that layer
	AActor* A = World->SpawnActor<AActor>();
	if (!A) { return Fail(TEXT("SpawnActor failed in the new world")); }
	UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(A, TEXT("Mesh"));
	SMC->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	A->SetRootComponent(SMC);
	SMC->RegisterComponent();
	A->AddInstanceComponent(SMC);
	A->SetActorLabel(TEXT("RUDE_probe_cube"));
	const bool bAdded = DlSub->AddActorToDataLayer(A, Dl);
	// 4) save the map and the asset
	bool bSavedMap = FEditorFileUtils::SaveMap(World, Path);
	// Measured 2026-09-05: SaveMap answered true and wrote the external actors + the Data Layer
	// asset, but NO .umap landed. Second leg: the headless saver over maps + content, then check.
	{
		FString MapFileCheck;
		FPackageName::TryConvertLongPackageNameToFilename(Path, MapFileCheck, FPackageName::GetMapPackageExtension());
		if (!FPaths::FileExists(MapFileCheck))
		{
			World->GetOutermost()->MarkPackageDirty();
			bSavedMap = RudeSaveDirty(/*bMaps*/ true, /*bContent*/ true) && FPaths::FileExists(MapFileCheck);
		}
	}
	FString AssetFile;
	bool bSavedAsset = false;
	if (FPackageName::TryConvertLongPackageNameToFilename(DlPkgName, AssetFile, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.Error = GWarn;
		bSavedAsset = UPackage::Save(DlPkg, DlAsset, *AssetFile, Args) == ESavePackageResult::Success;
	}
	FString MapFile;
	FPackageName::TryConvertLongPackageNameToFilename(Path, MapFile, FPackageName::GetMapPackageExtension());
	const bool bMapOnDisk = FPaths::FileExists(MapFile);
	const bool bOk = bAdded && bSavedMap && bSavedAsset && bMapOnDisk;
	return FString::Printf(TEXT("{\"ok\":%s,\"worldPartition\":true,\"dataLayerInstance\":\"%s\",\"actorAdded\":%s,")
		TEXT("\"mapSaved\":%s,\"mapOnDisk\":%s,\"assetSaved\":%s,\"headlessSaved\":%d,\"headlessSaveFailed\":%d,\"map\":\"%s\",\"dataLayerAsset\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Dl->GetDataLayerShortName()),
		bAdded ? TEXT("true") : TEXT("false"), bSavedMap ? TEXT("true") : TEXT("false"),
		bMapOnDisk ? TEXT("true") : TEXT("false"), bSavedAsset ? TEXT("true") : TEXT("false"),
		GRudeLastSaved, GRudeLastSaveFailed, *RudeJsonEscape(MapFile), *RudeJsonEscape(DlPkgName));
}

// ---- OpenLevel (agent) ------------------------------------------------------------------
FString URudeToolset::OpenLevel(const FString& LevelPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	if (!GEditor) { return Fail(TEXT("no GEditor")); }
	FString File;
	if (!FPackageName::TryConvertLongPackageNameToFilename(LevelPath.TrimStartAndEnd(), File, FPackageName::GetMapPackageExtension()))
	{
		return Fail(TEXT("LevelPath must be a long package name like /Game/RUDE/Levels/Downtown"));
	}
	if (!FPaths::FileExists(File)) { return Fail(FString::Printf(TEXT("no level at %s"), *File)); }
	if (!FEditorFileUtils::LoadMap(File, /*bLoadAsTemplate*/ false, /*bShowProgress*/ false))
	{
		return Fail(TEXT("LoadMap failed"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	// A World Partition level loads NO actors by itself in a commandlet (measured 2026-09-05: 8 actors,
	// 0 entities after LoadMap of a 14,248-actor level). Hold a reference to every actor descriptor so
	// the actors load and STAY loaded for the tools that follow in this process.
	static TArray<FWorldPartitionReference> HeldRefs;
	HeldRefs.Reset();
	int32 Descs = 0;
	if (World && World->GetWorldPartition())
	{
		UWorldPartition* WP = World->GetWorldPartition();
		FWorldPartitionHelpers::ForEachActorDescInstance(WP, [&](const FWorldPartitionActorDescInstance* D)
		{
			++Descs;
			HeldRefs.Emplace(D->GetContainerInstance(), D->GetGuid());
			return true;
		});
	}
	int32 Actors = 0, Entities = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Actors;
			if (It->FindComponentByClass<URudeEntityComponent>()) { ++Entities; }
		}
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"world\":\"%s\",\"worldPartition\":%s,\"actorDescriptors\":%d,\"actors\":%d,\"rudeEntities\":%d}"),
		World ? TEXT("true") : TEXT("false"), World ? *RudeJsonEscape(World->GetOutermost()->GetName()) : TEXT(""),
		(World && World->GetWorldPartition()) ? TEXT("true") : TEXT("false"), Descs, Actors, Entities);
}

// ---- BuildDistrictLevel ------------------------------------------------------------------
// The WP4 projection: a World Partition level for a district; every ymap in the manifest becomes
// a Runtime Data Layer (toggle a ymap like a layer); every entity becomes an actor carrying its
// URudeEntityComponent, placed on its ymap's layer. Saved headless (map + layer assets + the
// external actor packages). Filter as ImportScene (empty = HD, ALL = every LOD level).
FString URudeToolset::BuildDistrictLevel(const FString& LevelPath, const FString& ManifestPath,
                                         const FString& MeshFolder, const FString& Filter)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	if (!GEditor) { return Fail(TEXT("no GEditor")); }
	const FString Path = LevelPath.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(Path)) { return Fail(TEXT("LevelPath must be a long package name like /Game/RUDE/Levels/Downtown")); }
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *ManifestPath)) { return Fail(TEXT("cannot read the manifest")); }
	TArray<TSharedPtr<FJsonValue>> Scenes;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Scenes)) { return Fail(TEXT("manifest is not a JSON array")); }
	}
	const bool bAll = Filter.TrimStartAndEnd().Equals(TEXT("ALL"), ESearchCase::IgnoreCase);

	const FString LevelName = FPackageName::GetLongPackageAssetName(Path);
	const FString LayerDir = FPackageName::GetLongPackagePath(Path) / (LevelName + TEXT("_Layers"));
	// A REBUILD replaces the level. World Partition keeps every actor as its own file under
	// __ExternalActors__/<level>/ and discovers them by scanning that folder, so a stale actor file
	// from the previous build would come back as a duplicate. Clear the level's files first.
	int32 Cleared = 0;
	{
		FString MapFile;
		if (FPackageName::TryConvertLongPackageNameToFilename(Path, MapFile, FPackageName::GetMapPackageExtension()) && FPaths::FileExists(MapFile))
		{
			const FString ContentRoot = FPaths::GetPath(FPackageName::LongPackageNameToFilename(TEXT("/Game/"), TEXT("")));
			const FString Rel = Path.Mid(FString(TEXT("/Game/")).Len());
			for (const TCHAR* Sub : { TEXT("__ExternalActors__"), TEXT("__ExternalObjects__") })
			{
				const FString Dir = FPaths::Combine(ContentRoot, Sub, Rel);
				if (IFileManager::Get().DirectoryExists(*Dir))
				{
					TArray<FString> Files;
					IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*"), true, false);
					Cleared += Files.Num();
					IFileManager::Get().DeleteDirectory(*Dir, false, true);
				}
			}
			IFileManager::Get().Delete(*MapFile, false, true, true);
			++Cleared;
		}
	}
	UWorld* World = GEditor->NewMap(/*bIsPartitionedWorld*/ true);
	if (!World || !World->GetWorldPartition()) { return Fail(TEXT("could not create a World Partition world")); }
	// Wave 1 = the bake-district: everything resident. With streaming ON, an editor session shows
	// NOTHING until regions are loaded by hand (measured 2026-09-05: a black frame over 14,248
	// actors). Streaming comes back with the corpus-streamed viewer (Wave 2+).
	World->GetWorldPartition()->SetEnableStreaming(false);
	UDataLayerEditorSubsystem* DlSub = UDataLayerEditorSubsystem::Get();
	if (!DlSub) { return Fail(TEXT("no DataLayerEditorSubsystem")); }
	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	// A fresh world has no light: sun + sky atmosphere + sky light, tagged RUDE_SKY (the
	// time-of-day rig lands here later; SetWorldHour drives the archetype masks, not this).
	{
		FActorSpawnParameters SP;
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-45.f, 30.f, 0.f), SP);
		if (Sun)
		{
			Sun->SetActorLabel(TEXT("RUDE_Sun"));
			Sun->Tags.Add(FName(TEXT("RUDE_SKY")));
			if (UDirectionalLightComponent* DLC = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				DLC->SetMobility(EComponentMobility::Movable);
				DLC->SetIntensity(8.f);
				DLC->bAtmosphereSunLight = true;
			}
		}
		if (ASkyAtmosphere* Atm = World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, SP))
		{
			Atm->SetActorLabel(TEXT("RUDE_SkyAtmosphere"));
			Atm->Tags.Add(FName(TEXT("RUDE_SKY")));
		}
		if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, SP))
		{
			Sky->SetActorLabel(TEXT("RUDE_SkyLight"));
			Sky->Tags.Add(FName(TEXT("RUDE_SKY")));
			if (USkyLightComponent* SLC = Sky->GetLightComponent())
			{
				SLC->SetMobility(EComponentMobility::Movable);
				SLC->bRealTimeCapture = true;
			}
		}
	}

	TMap<FString, UStaticMesh*> MeshCache;
	int32 NumYmaps = 0, NumLayers = 0, NumActors = 0, NumProxies = 0, NumFiltered = 0, NumMalformed = 0, LayerFailures = 0;
	int32 NumScriptYmaps = 0, NumScriptActors = 0, NumSuspect = 0;
	TMap<FString, int32> Missing;
	TMap<FString, FString> YmapParentMap;   // ymap (lower) -> CMapData/parent, for the lineage resolve
	for (const TSharedPtr<FJsonValue>& SceneVal : Scenes)
	{
		const TSharedPtr<FJsonObject>* SceneObj;
		if (!SceneVal.IsValid() || !SceneVal->TryGetObject(SceneObj)) { continue; }
		const TArray<TSharedPtr<FJsonValue>>* Entities;
		if (!(*SceneObj)->TryGetArrayField(TEXT("entities"), Entities) || Entities->Num() == 0) { continue; }
		const FString YmapName = (*SceneObj)->GetStringField(TEXT("ymap"));
		++NumYmaps;
		double YmapFlagsD = 0.0;
		(*SceneObj)->TryGetNumberField(TEXT("ymapFlags"), YmapFlagsD);
		{
			FString YP;
			(*SceneObj)->TryGetStringField(TEXT("ymapParent"), YP);
			YmapParentMap.Add(YmapName.ToLower(), YP);
		}
		const bool bScriptYmap = (((uint32)YmapFlagsD) & 1u) != 0;   // CMapData flags bit 0 (measured)
		if (bScriptYmap) { ++NumScriptYmaps; }
		// one Data Layer per ymap: asset DL_<ymap> beside the level, Runtime, loaded in editor
		UDataLayerInstance* Layer = nullptr;
		{
			const FString PkgName = LayerDir / (TEXT("DL_") + YmapName);
			UPackage* Pkg = CreatePackage(*PkgName);
			UDataLayerAsset* Asset = NewObject<UDataLayerAsset>(Pkg, FName(*FPackageName::GetLongPackageAssetName(PkgName)), RF_Public | RF_Standalone);
			Asset->SetType(EDataLayerType::Runtime);
			Pkg->MarkPackageDirty();
			FDataLayerCreationParameters P;
			P.DataLayerAsset = Asset;
			P.WorldDataLayers = World->GetWorldDataLayers();
			Layer = DlSub->CreateDataLayerInstance(P);
			if (Layer)
			{
				++NumLayers;
				DlSub->SetDataLayerIsLoadedInEditor(Layer, true, false);   // always loaded: the export needs every actor
				// a script-controlled ymap starts UNLOADED at runtime (the game loads it on demand)
				if (bScriptYmap) { Layer->SetInitialRuntimeState(EDataLayerRuntimeState::Unloaded); }
			}
			else { ++LayerFailures; }
		}
		TArray<AActor*> LayerActors;
		for (const TSharedPtr<FJsonValue>& EntVal : *Entities)
		{
			const TSharedPtr<FJsonObject>* Ent;
			if (!EntVal.IsValid() || !EntVal->TryGetObject(Ent)) { ++NumMalformed; continue; }
			FString Lod;
			(*Ent)->TryGetStringField(TEXT("lodLevel"), Lod);
			const bool bHd = Lod.IsEmpty() || Lod == TEXT("LODTYPES_DEPTH_HD") || Lod == TEXT("LODTYPES_DEPTH_ORPHANHD");
			if (!bAll && !bHd) { ++NumFiltered; continue; }
			const TArray<TSharedPtr<FJsonValue>>* Loc;
			const TArray<TSharedPtr<FJsonValue>>* Quat;
			if (!(*Ent)->TryGetArrayField(TEXT("ue_location"), Loc) || Loc->Num() != 3 ||
			    !(*Ent)->TryGetArrayField(TEXT("ue_quat"), Quat) || Quat->Num() != 4) { ++NumMalformed; continue; }
			const double SXY = (*Ent)->HasField(TEXT("scaleXY")) ? (*Ent)->GetNumberField(TEXT("scaleXY")) : 1.0;
			const double SZ = (*Ent)->HasField(TEXT("scaleZ")) ? (*Ent)->GetNumberField(TEXT("scaleZ")) : 1.0;
			FQuat Q((*Quat)[0]->AsNumber(), (*Quat)[1]->AsNumber(), (*Quat)[2]->AsNumber(), (*Quat)[3]->AsNumber());
			// not normalised: the source quaternion is unit within float32 and re-normalising in double moves its last digit (measured 2026-09-06: 0.9961947 -> 0.996194661 on an untouched entity)
			const FTransform Xf(Q, FVector((*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber()), FVector(SXY, SXY, SZ));
			FString Drawable;
			(*Ent)->TryGetStringField(TEXT("drawable"), Drawable);
			Drawable.ToLowerInline();
			UStaticMesh* Mesh = nullptr;
			if (!Drawable.IsEmpty())
			{
				if (UStaticMesh** Cached = MeshCache.Find(Drawable)) { Mesh = *Cached; }
				else { Mesh = LoadObject<UStaticMesh>(nullptr, *(MeshFolder / Drawable)); MeshCache.Add(Drawable, Mesh); }
			}
			uint32 TimeMask = 0;
			{
				int32 TF = 0;
				if ((*Ent)->TryGetNumberField(TEXT("timeFlags"), TF) && TF > 0) { TimeMask = (uint32)TF; }
			}
			if (!Mesh) { Missing.FindOrAdd(Drawable.IsEmpty() ? (*Ent)->GetStringField(TEXT("archetype")) : Drawable)++; }
			if (!Mesh && !ProxyCube) { continue; }
			if (AActor* A = RudeSpawnEntityActor(World, YmapName, *Ent, Xf, Mesh ? Mesh : ProxyCube, Mesh == nullptr, TimeMask))
			{
				++NumActors;
				if (!Mesh) { ++NumProxies; }
				if (A->Tags.Contains(FName(TEXT("RUDE_SUSPECT_BOUNDS")))) { ++NumSuspect; }
				if (bScriptYmap)
				{
					// placed, tagged, hidden: visible again through SetYmapVisible (the IPL toggle)
					A->Tags.Add(FName(TEXT("RUDE_SCRIPT_YMAP")));
					if (UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>()) { SMC->SetVisibility(false, true); SMC->SetHiddenInGame(true, true); }
					++NumScriptActors;
				}
				LayerActors.Add(A);
			}
		}
		if (Layer && LayerActors.Num() > 0) { DlSub->AddActorsToDataLayers(LayerActors, { Layer }); }
	}
	// LOD lineage: parentIndex links become LodParent / LodChildren (ENGINEERING_LOG law 24)
	int32 LodLinks = 0, LodUnresolved = 0, LodPartial = 0;
	RudeResolveLodLineage(World, YmapParentMap, LodLinks, LodUnresolved, LodPartial);
	// save: the map (SaveMap writes external actors + assets; the headless map leg writes the .umap)
	bool bSaved = FEditorFileUtils::SaveMap(World, Path);
	FString MapFile;
	FPackageName::TryConvertLongPackageNameToFilename(Path, MapFile, FPackageName::GetMapPackageExtension());
	if (!FPaths::FileExists(MapFile))
	{
		World->GetOutermost()->MarkPackageDirty();
		bSaved = RudeSaveDirty(/*bMaps*/ true, /*bContent*/ true) && FPaths::FileExists(MapFile);
	}
	else
	{
		RudeSaveDirty(/*bMaps*/ false, /*bContent*/ true);   // the layer assets
	}
	FString TopMissing;
	{
		TArray<TPair<FString, int32>> Sorted;
		for (const auto& KV : Missing) { Sorted.Add(KV); }
		Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
		for (int32 i = 0; i < Sorted.Num() && i < 12; ++i)
		{
			TopMissing += FString::Printf(TEXT("%s\"%s x%d\""), i ? TEXT(",") : TEXT(""), *RudeJsonEscape(Sorted[i].Key), Sorted[i].Value);
		}
	}
	const bool bOk = bSaved && NumActors > 0 && LayerFailures == 0 && NumMalformed == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"level\":\"%s\",\"worldPartition\":true,\"ymaps\":%d,\"layers\":%d,\"layerFailures\":%d,")
		TEXT("\"actors\":%d,\"proxies\":%d,\"filteredByLod\":%d,\"malformedEntities\":%d,\"missingMeshes\":%d,")
		TEXT("\"mapSaved\":%s,\"mapOnDisk\":%s,\"headlessSaved\":%d,\"headlessSaveFailed\":%d,\"previousFilesCleared\":%d,")
		TEXT("\"scriptYmaps\":%d,\"scriptActorsHidden\":%d,\"suspectBounds\":%d,\"lodLinks\":%d,\"lodUnresolved\":%d,\"lodPartial\":%d,\"topMissing\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Path), NumYmaps, NumLayers, LayerFailures,
		NumActors, NumProxies, NumFiltered, NumMalformed, Missing.Num(),
		bSaved ? TEXT("true") : TEXT("false"), FPaths::FileExists(MapFile) ? TEXT("true") : TEXT("false"),
		GRudeLastSaved, GRudeLastSaveFailed, Cleared, NumScriptYmaps, NumScriptActors, NumSuspect, LodLinks, LodUnresolved, LodPartial, *TopMissing);
}

// Raw item slices: the text of each "  <Item ...>" ... "  </Item>" (indent 2) inside the named
// top-level block of a ROUT XML, in file order, each ending with its newline. Byte-true by
// construction - the answer to FXmlFile flattening multi-line text (MLO <attachedObjects>).
static bool RudeRawItems(const FString& Text, const TCHAR* BlockTag, TArray<FString>& Out)
{
	// the opening tag may carry attributes ("<carGenerators itemType=\"CCarGen\">"): match "\n <tag" and
	// step past that line (measured 2026-09-06: the bare-tag match sliced 0 car generators and the hook
	// replaced the block with an empty one)
	const FString OpenBare = FString::Printf(TEXT("\n <%s"), BlockTag);
	const FString Close = FString::Printf(TEXT("\n </%s>"), BlockTag);
	int32 B = INDEX_NONE, Search = 0;
	while (true)
	{
		const int32 Hit = Text.Find(OpenBare, ESearchCase::CaseSensitive, ESearchDir::FromStart, Search);
		if (Hit == INDEX_NONE) { return false; }
		const TCHAR After = Text.IsValidIndex(Hit + OpenBare.Len()) ? Text[Hit + OpenBare.Len()] : 0;
		if (After == '>' || After == ' ') { B = Hit; break; }
		Search = Hit + 1;
	}
	const int32 OpenLineEnd = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, B + 1);
	if (OpenLineEnd == INDEX_NONE) { return false; }
	if (Text.Mid(B, OpenLineEnd - B).EndsWith(TEXT("/>"))) { return true; }   // the empty form: no items
	const int32 E = Text.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenLineEnd);
	if (E == INDEX_NONE) { return false; }
	int32 Pos = OpenLineEnd + 1;
	const FString ItemOpen = TEXT("  <Item");
	const FString ItemClose = TEXT("\n  </Item>\n");
	const FString ItemEmptyEnd = TEXT(" />\n");
	while (Pos < E)
	{
		if (!Text.Mid(Pos, ItemOpen.Len()).Equals(ItemOpen)) { break; }
		// self-closing "  <Item ... />" on one line, or a block ending at "\n  </Item>\n"
		const int32 Nl = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (Nl == INDEX_NONE) { break; }
		const FString FirstLine = Text.Mid(Pos, Nl - Pos + 1);
		int32 End;
		if (FirstLine.EndsWith(ItemEmptyEnd)) { End = Nl + 1; }
		else
		{
			const int32 C = Text.Find(ItemClose, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
			if (C == INDEX_NONE || C > E) { break; }
			End = C + ItemClose.Len();
		}
		Out.Add(Text.Mid(Pos, End - Pos));
		Pos = End;
	}
	return true;
}

// ---- BuildArchetypePalette ---------------------------------------------------------------
// A ytyp archetype item -> a URudeArchetype asset. Field order in the file (verified against
// downtown_01_metadata_001.ytyp.xml 2026-09-05): lodDist flags specialAttribute bbMin bbMax bsCentre
// bsRadius hdTextureDist name textureDictionary clipDictionary drawableDictionary physicsDictionary
// assetType assetName extensions [timeFlags] [mloFlags entities rooms portals entitySets ...].
static void RudeFillArchetype(URudeArchetype* A, const FXmlNode* Item)
{
	auto Text = [Item](const TCHAR* Tag) -> FString
	{
		const FXmlNode* N = Item->FindChildNode(Tag);
		return N ? N->GetContent().TrimStartAndEnd() : FString();
	};
	auto Val = [Item](const TCHAR* Tag, double Def) -> double
	{
		const FXmlNode* N = Item->FindChildNode(Tag);
		return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Def;
	};
	auto Vec = [Item](const TCHAR* Tag) -> FVector
	{
		const FXmlNode* N = Item->FindChildNode(Tag);
		if (!N) { return FVector::ZeroVector; }
		return FVector(FCString::Atod(*N->GetAttribute(TEXT("x"))), FCString::Atod(*N->GetAttribute(TEXT("y"))), FCString::Atod(*N->GetAttribute(TEXT("z"))));
	};
	A->ArchetypeKind = Item->GetAttribute(TEXT("type"));
	A->Name = Text(TEXT("name"));
	A->AssetName = Text(TEXT("assetName"));
	A->AssetType = Text(TEXT("assetType"));
	A->TextureDictionary = Text(TEXT("textureDictionary"));
	A->PhysicsDictionary = Text(TEXT("physicsDictionary"));
	A->DrawableDictionary = Text(TEXT("drawableDictionary"));
	A->ClipDictionary = Text(TEXT("clipDictionary"));
	A->LodDist = (float)Val(TEXT("lodDist"), 0.0);
	A->HdTextureDist = (float)Val(TEXT("hdTextureDist"), 0.0);
	A->BbMin = Vec(TEXT("bbMin")); A->BbMax = Vec(TEXT("bbMax")); A->BsCentre = Vec(TEXT("bsCentre"));
	A->BsRadius = (float)Val(TEXT("bsRadius"), 0.0);
	A->TimeFlags = (uint32)Val(TEXT("timeFlags"), 0.0);
	A->Flags = (uint32)Val(TEXT("flags"), 0.0);
	A->SpecialAttribute = (uint32)Val(TEXT("specialAttribute"), 0.0);
	A->ExtensionsXml.Reset();
	if (const FXmlNode* Ext = Item->FindChildNode(TEXT("extensions")))
	{
		if (Ext->GetChildrenNodes().Num() > 0) { RudeXmlNodeToString(Ext, A->ExtensionsXml, 0); }
	}
	A->MloXml.Reset();
	if (A->ArchetypeKind == TEXT("CMloArchetypeDef"))
	{
		// everything after <extensions> verbatim (mloFlags, entities, rooms, portals, entitySets, ...)
		bool bAfter = false;
		for (const FXmlNode* K : Item->GetChildrenNodes())
		{
			if (bAfter) { RudeXmlNodeToString(K, A->MloXml, 0); }
			if (K->GetTag() == TEXT("extensions")) { bAfter = true; }
		}
	}
	A->SourceXml.Reset();
	RudeXmlNodeToString(Item, A->SourceXml, 2);
	A->SourceFieldsKey = A->FieldsKey();
}

FString URudeToolset::BuildArchetypePalette(const FString& CorpusRoot, const FString& ManifestPath,
                                            const FString& DestFolder, const FString& MeshFolder)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	if (!FPackageName::IsValidLongPackageName(DestFolder / TEXT("x"))) { return Fail(TEXT("DestFolder must be a content path like /Game/RUDE/Palette/Downtown")); }
	// Which archetypes: every name the manifest's placements refer to (empty manifest = every archetype).
	TSet<FString> Wanted;
	if (!ManifestPath.TrimStartAndEnd().IsEmpty())
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *ManifestPath)) { return Fail(TEXT("cannot read the manifest")); }
		TArray<TSharedPtr<FJsonValue>> Scenes;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Scenes)) { return Fail(TEXT("manifest is not a JSON array")); }
		for (const TSharedPtr<FJsonValue>& SV : Scenes)
		{
			const TSharedPtr<FJsonObject>* SO;
			const TArray<TSharedPtr<FJsonValue>>* Ents;
			if (!SV.IsValid() || !SV->TryGetObject(SO) || !(*SO)->TryGetArrayField(TEXT("entities"), Ents)) { continue; }
			for (const TSharedPtr<FJsonValue>& EV : *Ents)
			{
				const TSharedPtr<FJsonObject>* EO;
				if (EV.IsValid() && EV->TryGetObject(EO)) { Wanted.Add((*EO)->GetStringField(TEXT("archetype")).ToLower()); }
			}
		}
	}
	// Walk every ytyp in load order; a later slot's definition of the same name overwrites the asset.
	TArray<const FRudeCorpusEntry*> Rows;
	Corpus->AllOfType(TEXT("ytyp"), Rows);
	int32 Files = 0, Seen = 0, Made = 0, Updated = 0, Invalid = 0, MeshLinked = 0;
	TMap<FString, int32> Kinds;
	TSet<FString> Done;
	for (const FRudeCorpusEntry* E : Rows)
	{
		FXmlFile Xml(Corpus->PathOf(*E));
		if (!Xml.IsValid() || !Xml.GetRootNode()) { continue; }
		const FXmlNode* Arche = Xml.GetRootNode()->FindChildNode(TEXT("archetypes"));
		if (!Arche) { continue; }
		++Files;
		FString RawText;
		TArray<FString> RawItems;
		FFileHelper::LoadFileToString(RawText, *Corpus->PathOf(*E));
		RudeRawItems(RawText, TEXT("archetypes"), RawItems);
		const bool bRawOk = RawItems.Num() == Arche->GetChildrenNodes().Num();
		int32 Ordinal = -1;
		for (const FXmlNode* Item : Arche->GetChildrenNodes())
		{
			++Ordinal;
			const FXmlNode* NameN = Item->FindChildNode(TEXT("name"));
			if (!NameN) { continue; }
			const FString NameLower = NameN->GetContent().TrimStartAndEnd().ToLower();
			if (NameLower.IsEmpty()) { continue; }
			++Seen;
			if (Wanted.Num() > 0 && !Wanted.Contains(NameLower)) { continue; }
			const FString PkgName = DestFolder / NameLower;
			if (!FPackageName::IsValidLongPackageName(PkgName)) { ++Invalid; continue; }
			// Load the existing asset FIRST (a package created over an unloaded file is later
			// re-serialised from disk by any LoadObject, overwriting the fresh in-memory fields -
			// measured 2026-09-05: the export read last run's flattened SourceXml).
			URudeArchetype* A = LoadObject<URudeArchetype>(nullptr, *(PkgName + TEXT(".") + NameLower));
			UPackage* Pkg = A ? A->GetOutermost() : CreatePackage(*PkgName);
			bool bNew = false;
			if (!A) { A = NewObject<URudeArchetype>(Pkg, FName(*NameLower), RF_Public | RF_Standalone); bNew = true; }
			RudeFillArchetype(A, Item);
			if (bRawOk) { A->SourceXml = RawItems[Ordinal]; }   // the file's own bytes for this item
			A->SourceYtyp = E->Name;
			A->SourceSlot = E->Slot;
			A->SourceIndex = Ordinal;
			if (!MeshFolder.TrimStartAndEnd().IsEmpty())
			{
				const FString Asset = A->AssetName.IsEmpty() ? NameLower : A->AssetName.ToLower();
				const FString MeshPkg = MeshFolder / Asset;
				if (FPackageName::DoesPackageExist(MeshPkg)) { A->Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(MeshPkg + TEXT(".") + Asset)); ++MeshLinked; }
			}
			Pkg->MarkPackageDirty();
			Kinds.FindOrAdd(A->ArchetypeKind)++;
			if (Done.Contains(NameLower)) { ++Updated; } else { Done.Add(NameLower); if (bNew) { ++Made; } else { ++Updated; } }
		}
	}
	FString KindsJson;
	for (const auto& KV : Kinds) { KindsJson += FString::Printf(TEXT("%s\"%s\":%d"), KindsJson.IsEmpty() ? TEXT("") : TEXT(","), *KV.Key, KV.Value); }
	const bool bOk = Done.Num() > 0 && Invalid == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ytypFiles\":%d,\"archetypesSeen\":%d,\"wanted\":%d,\"assets\":%d,\"created\":%d,\"overwrittenByLaterSlot\":%d,")
		TEXT("\"invalidNames\":%d,\"meshLinked\":%d,\"kinds\":{%s}}"),
		bOk ? TEXT("true") : TEXT("false"), Files, Seen, Wanted.Num(), Done.Num(), Made, Updated, Invalid, MeshLinked, *KindsJson);
}

// ---- ExportPaletteYtyps ------------------------------------------------------------------
static FString RudeArchetypeXml(const URudeArchetype* A)
{
	auto V3 = [](const FVector& V) { return FString::Printf(TEXT("x=\"%s\" y=\"%s\" z=\"%s\""), *RudeNum(V.X), *RudeNum(V.Y), *RudeNum(V.Z)); };
	FString O;
	O += FString::Printf(TEXT("  <Item type=\"%s\">\n"), *A->ArchetypeKind);
	O += FString::Printf(TEXT("   <lodDist value=\"%s\" />\n"), *RudeNum(A->LodDist));
	O += FString::Printf(TEXT("   <flags value=\"%u\" />\n"), A->Flags);
	O += FString::Printf(TEXT("   <specialAttribute value=\"%u\" />\n"), A->SpecialAttribute);
	O += FString::Printf(TEXT("   <bbMin %s />\n"), *V3(A->BbMin));
	O += FString::Printf(TEXT("   <bbMax %s />\n"), *V3(A->BbMax));
	O += FString::Printf(TEXT("   <bsCentre %s />\n"), *V3(A->BsCentre));
	O += FString::Printf(TEXT("   <bsRadius value=\"%s\" />\n"), *RudeNum(A->BsRadius));
	O += FString::Printf(TEXT("   <hdTextureDist value=\"%s\" />\n"), *RudeNum(A->HdTextureDist));
	auto Tag = [&O](const TCHAR* T, const FString& S)
	{
		if (S.IsEmpty()) { O += FString::Printf(TEXT("   <%s />\n"), T); return; }
		O += FString::Printf(TEXT("   <%s>"), T); RudeXmlEscapeInto(O, S); O += FString::Printf(TEXT("</%s>\n"), T);
	};
	Tag(TEXT("name"), A->Name);
	Tag(TEXT("textureDictionary"), A->TextureDictionary);
	Tag(TEXT("clipDictionary"), A->ClipDictionary);
	Tag(TEXT("drawableDictionary"), A->DrawableDictionary);
	Tag(TEXT("physicsDictionary"), A->PhysicsDictionary);
	Tag(TEXT("assetType"), A->AssetType);
	Tag(TEXT("assetName"), A->AssetName);
	if (A->ExtensionsXml.TrimStartAndEnd().IsEmpty()) { O += TEXT("   <extensions />\n"); }
	else
	{
		TArray<FString> Lines; A->ExtensionsXml.ParseIntoArrayLines(Lines);
		for (const FString& L : Lines) { O += TEXT("   "); O += L; O += TEXT("\n"); }
	}
	if (A->ArchetypeKind == TEXT("CTimeArchetypeDef")) { O += FString::Printf(TEXT("   <timeFlags value=\"%u\" />\n"), A->TimeFlags); }
	O += TEXT("  </Item>\n");
	return O;
}

FString URudeToolset::ExportPaletteYtyps(const FString& OutDir, const FString& PaletteFolder,
                                         const FString& YtypFilter, const FString& CorpusRoot)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an OutDir")); }
	TSet<FString> Wanted;
	{
		TArray<FString> Parts; YtypFilter.ParseIntoArray(Parts, TEXT(","), true);
		for (FString P : Parts) { P.TrimStartAndEndInline(); if (!P.IsEmpty()) { Wanted.Add(P.ToLower()); } }
	}
	// every palette asset, grouped by source ytyp
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().ScanPathsSynchronous({ PaletteFolder }, true);
	ARM.Get().GetAssetsByPath(FName(*PaletteFolder), Assets, true);
	TMap<FString, TArray<URudeArchetype*>> Groups;
	int32 Loaded = 0;
	for (const FAssetData& AD : Assets)
	{
		URudeArchetype* A = Cast<URudeArchetype>(AD.GetAsset());
		if (!A || A->SourceYtyp.IsEmpty()) { continue; }
		++Loaded;
		const FString Y = A->SourceYtyp.ToLower();
		if (Wanted.Num() > 0 && !Wanted.Contains(Y)) { continue; }
		Groups.FindOrAdd(Y).Add(A);
	}
	if (Groups.Num() == 0) { return Fail(FString::Printf(TEXT("no palette assets with a source ytyp under %s (%d loaded)"), *PaletteFolder, Loaded)); }
	IFileManager::Get().MakeDirectory(*(OutDir / TEXT("stream")), true);
	int32 Written = 0, Refused = 0, Kept = 0, Edited = 0, NotRebuilt = 0, Removed = 0, Added = 0;
	FString Files, RefusedJson;
	for (auto& KV : Groups)
	{
		const FString& Ytyp = KV.Key;
		TArray<URudeArchetype*>& As = KV.Value;
		As.Sort([](const URudeArchetype& X, const URudeArchetype& Y) { return X.SourceIndex < Y.SourceIndex; });
		const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("ytyp"), Ytyp);
		if (!Row) { ++Refused; RefusedJson += FString::Printf(TEXT("%s\"%s: no source ytyp in the corpus\""), RefusedJson.IsEmpty() ? TEXT("") : TEXT(","), *Ytyp); continue; }
		FString SrcText;
		if (!FFileHelper::LoadFileToString(SrcText, *Corpus->PathOf(*Row))) { ++Refused; continue; }
		FXmlFile Src(Corpus->PathOf(*Row));
		const FXmlNode* Arche = (Src.IsValid() && Src.GetRootNode()) ? Src.GetRootNode()->FindChildNode(TEXT("archetypes")) : nullptr;
		const int32 SourceCount = Arche ? Arche->GetChildrenNodes().Num() : 0;
		TArray<FString> RawItems;
		RudeRawItems(SrcText, TEXT("archetypes"), RawItems);
		if (RawItems.Num() != SourceCount)
		{
			++Refused;
			RefusedJson += FString::Printf(TEXT("%s\"%s: raw item slices %d != parsed items %d\""), RefusedJson.IsEmpty() ? TEXT("") : TEXT(","), *Ytyp, RawItems.Num(), SourceCount);
			continue;
		}
		// Walk the SOURCE items by ordinal: an item with a palette asset goes out from the asset
		// (verbatim when untouched, rebuilt when edited); an item WITHOUT one (the palette is filtered
		// to a district) goes out as read. Nothing is dropped - a ytyp is a definition table other
		// files reference by name; Wave 1 has no delete.
		TMap<int32, const URudeArchetype*> ByOrdinal;
		for (const URudeArchetype* A : As) { ByOrdinal.Add(A->SourceIndex, A); }
		FString Block;
		int32 Ordinal = -1;
		if (Arche)
		{
			for (const FXmlNode* Item : Arche->GetChildrenNodes())
			{
				++Ordinal;
				const URudeArchetype* const* Found = ByOrdinal.Find(Ordinal);
				if (!Found)
				{
					Block += RawItems[Ordinal];
					++Kept;
					continue;
				}
				const URudeArchetype* A = *Found;
				const bool bUntouched = !A->SourceXml.IsEmpty() && A->FieldsKey() == A->SourceFieldsKey;
				const bool bRebuildable = A->ArchetypeKind == TEXT("CBaseArchetypeDef") || A->ArchetypeKind == TEXT("CTimeArchetypeDef");
				if (bUntouched || !bRebuildable)
				{
					if (!bUntouched) { ++NotRebuilt; }
					Block += A->SourceXml; if (!A->SourceXml.EndsWith(TEXT("\n"))) { Block += TEXT("\n"); }
					++Kept;
				}
				else { Block += RudeArchetypeXml(A); ++Edited; }
			}
		}
		// NEW archetypes (no source ordinal: made in RUDE, e.g. MakeLodArchetype) go out APPENDED to
		// their target ytyp, after every source item, so no existing ordinal moves.
		for (const URudeArchetype* A : As)
		{
			if (A->SourceIndex < 0) { Block += RudeArchetypeXml(A); ++Added; }
		}
		FString Doc = SrcText;
		const int32 Open = Doc.Find(TEXT("\n <archetypes>\n"), ESearchCase::CaseSensitive);
		const int32 Close = Open == INDEX_NONE ? INDEX_NONE : Doc.Find(TEXT(" </archetypes>\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
		if (Open == INDEX_NONE || Close == INDEX_NONE) { ++Refused; RefusedJson += FString::Printf(TEXT("%s\"%s: no <archetypes> block to splice\""), RefusedJson.IsEmpty() ? TEXT("") : TEXT(","), *Ytyp); continue; }
		Doc = Doc.Left(Open) + TEXT("\n <archetypes>\n") + Block + TEXT(" </archetypes>\n") + Doc.Mid(Close + FString(TEXT(" </archetypes>\n")).Len());
		const FString OutPath = OutDir / TEXT("stream") / (Ytyp + TEXT(".ytyp"));
		if (!FFileHelper::SaveStringToFile(Doc, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return Fail(FString::Printf(TEXT("cannot write %s"), *OutPath)); }
		++Written;
		Files += FString::Printf(TEXT("%s\"%s\""), Files.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(OutPath));
	}
	FFileHelper::SaveStringToFile(TEXT("fx_version 'cerulean'\ngame 'gta5'\nthis_is_a_map 'yes'\n"), *(OutDir / TEXT("fxmanifest.lua")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool bOk = Written > 0 && Refused == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"paletteAssets\":%d,\"ytypsWritten\":%d,\"ytypsRefused\":%d,\"kept\":%d,\"edited\":%d,\"added\":%d,\"notRebuilt\":%d,\"removed\":%d,\"files\":[%s],\"refused\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), Loaded, Written, Refused, Kept, Edited, Added, NotRebuilt, Removed, *Files, *RefusedJson);
}

// ---- SetArchetypeField (agent; the scriptable palette edit) ------------------------------
FString URudeToolset::SetArchetypeField(const FString& PaletteFolder, const FString& ArchetypeName,
                                        const FString& Field, const FString& Value)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	const FString Name = ArchetypeName.TrimStartAndEnd().ToLower();
	const FString PkgName = PaletteFolder / Name;
	URudeArchetype* A = LoadObject<URudeArchetype>(nullptr, *(PkgName + TEXT(".") + Name));
	if (!A) { return Fail(FString::Printf(TEXT("no palette asset %s"), *PkgName)); }
	FProperty* Prop = A->GetClass()->FindPropertyByName(FName(*Field.TrimStartAndEnd()));
	if (!Prop) { return Fail(FString::Printf(TEXT("URudeArchetype has no property '%s'"), *Field)); }
	FString Before;
	Prop->ExportTextItem_Direct(Before, Prop->ContainerPtrToValuePtr<void>(A), nullptr, A, PPF_None);
	if (!Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(A), A, PPF_None))
	{
		return Fail(FString::Printf(TEXT("could not parse '%s' for %s"), *Value, *Field));
	}
	A->MarkPackageDirty();
	FString After;
	Prop->ExportTextItem_Direct(After, Prop->ContainerPtrToValuePtr<void>(A), nullptr, A, PPF_None);
	return FString::Printf(TEXT("{\"ok\":true,\"archetype\":\"%s\",\"field\":\"%s\",\"before\":\"%s\",\"after\":\"%s\",\"changed\":%s}"),
		*RudeJsonEscape(Name), *RudeJsonEscape(Field), *RudeJsonEscape(Before), *RudeJsonEscape(After),
		A->FieldsKey() != A->SourceFieldsKey ? TEXT("true") : TEXT("false"));
}

// ---- NewLevel / SaveLevel (agent) ---------------------------------------------------------
FString URudeToolset::NewLevel(const FString& Partitioned)
{
	if (!GEditor) { return TEXT("{\"ok\":false,\"error\":\"no GEditor\"}"); }
	const bool bWP = Partitioned.TrimStartAndEnd().Equals(TEXT("true"), ESearchCase::IgnoreCase) || Partitioned.TrimStartAndEnd() == TEXT("1");
	UWorld* World = GEditor->NewMap(bWP);
	return FString::Printf(TEXT("{\"ok\":%s,\"worldPartition\":%s,\"world\":\"%s\"}"),
		World ? TEXT("true") : TEXT("false"), (World && World->GetWorldPartition()) ? TEXT("true") : TEXT("false"),
		World ? *RudeJsonEscape(World->GetOutermost()->GetName()) : TEXT(""));
}

FString URudeToolset::SaveLevel(const FString& LevelPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	FString Path = LevelPath.TrimStartAndEnd();
	if (Path.IsEmpty()) { Path = World->GetOutermost()->GetName(); }
	if (!FPackageName::IsValidLongPackageName(Path) || Path.StartsWith(TEXT("/Temp"))) { return Fail(TEXT("give a content path for the level (it is untitled)")); }
	bool bSaved = FEditorFileUtils::SaveMap(World, Path);
	FString MapFile;
	FPackageName::TryConvertLongPackageNameToFilename(Path, MapFile, FPackageName::GetMapPackageExtension());
	if (!FPaths::FileExists(MapFile))
	{
		World->GetOutermost()->MarkPackageDirty();
		bSaved = RudeSaveDirty(true, true) && FPaths::FileExists(MapFile);
	}
	else { RudeSaveDirty(false, true); }
	return FString::Printf(TEXT("{\"ok\":%s,\"level\":\"%s\",\"mapOnDisk\":%s,\"headlessSaved\":%d,\"headlessSaveFailed\":%d}"),
		bSaved ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Path), FPaths::FileExists(MapFile) ? TEXT("true") : TEXT("false"),
		GRudeLastSaved, GRudeLastSaveFailed);
}

// ---- PlaceInterior --------------------------------------------------------------------------
// A packed interior level (PackAreaLevelInstance's output, built at the origin from ImportMlo) placed
// as a Level Instance at every CMloInstanceDef of that archetype in the open level. The entity actor
// (proxy cube + URudeEntityComponent) stays the placement's owner: the Level Instance attaches to it,
// so the export still reads one entity per placement and the interior follows the entity when moved.
FString URudeToolset::PlaceInterior(const FString& MloArchetypeName, const FString& LevelAsset)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Wanted = MloArchetypeName.TrimStartAndEnd().ToLower();
	if (Wanted.IsEmpty()) { return Fail(TEXT("MloArchetypeName is empty")); }
	const FString Pkg = LevelAsset.TrimStartAndEnd();
	if (!FPackageName::DoesPackageExist(Pkg)) { return Fail(FString::Printf(TEXT("no level asset at %s"), *Pkg)); }
	const TSoftObjectPtr<UWorld> WorldAsset(FSoftObjectPath(Pkg + TEXT(".") + FPackageName::GetShortName(Pkg)));
	ULevelInstanceSubsystem* Sub = World->GetSubsystem<ULevelInstanceSubsystem>();
	int32 Placements = 0, Placed = 0, AlreadyPlaced = 0, Refused = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
		if (!R || R->ItemType != TEXT("CMloInstanceDef") || R->ArchetypeName.ToLower() != Wanted) { continue; }
		++Placements;
		// already placed? (an attached Level Instance child)
		bool bHas = false;
		TArray<AActor*> Children;
		It->GetAttachedActors(Children);
		for (AActor* Ch : Children) { if (Cast<ILevelInstanceInterface>(Ch)) { bHas = true; break; } }
		if (bHas) { ++AlreadyPlaced; continue; }
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		AActor* LiActor = World->SpawnActor<AActor>(ALevelInstance::StaticClass(), It->GetActorTransform(), Spawn);
		ILevelInstanceInterface* LI = Cast<ILevelInstanceInterface>(LiActor);
		if (!LI || !LI->SetWorldAsset(WorldAsset))
		{
			++Refused;
			if (LiActor) { World->DestroyActor(LiActor); }
			continue;
		}
		LI->UpdateLevelInstanceFromWorldAsset();
		LiActor->SetActorLabel(TEXT("MLO_") + Wanted);
		LiActor->SetFolderPath(It->GetFolderPath());
		LiActor->AttachToActor(*It, FAttachmentTransformRules::KeepWorldTransform);
		// the proxy cube no longer needs to show: hide the entity's own mesh, keep the component
		if (UStaticMeshComponent* SMC = It->FindComponentByClass<UStaticMeshComponent>()) { SMC->SetVisibility(false); }
		if (Sub) { Sub->BlockLoadLevelInstance(LI); }
		++Placed;
	}
	const bool bOk = Placements > 0 && Refused == 0;
	return FString::Printf(TEXT("{\"ok\":%s,\"archetype\":\"%s\",\"placements\":%d,\"placed\":%d,\"alreadyPlaced\":%d,\"refused\":%d,\"levelAsset\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Wanted), Placements, Placed, AlreadyPlaced, Refused, *RudeJsonEscape(Pkg));
}

// ---- SetLodView -------------------------------------------------------------------------
// Which LOD level of the placed lineage is visible: HD (default: HD + ORPHANHD) | LOD | SLOD1 | SLOD2
// | SLOD3 | SLOD4 | ALL. Everything stays placed; only visibility changes (nothing to export).
FString URudeToolset::SetLodView(const FString& Level)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return TEXT("{\"ok\":false,\"error\":\"no editor world\"}"); }
	const FString Want = Level.TrimStartAndEnd().ToUpper();
	const bool bAll = Want == TEXT("ALL");
	TSet<FString> Show;
	if (Want.IsEmpty() || Want == TEXT("HD")) { Show.Add(TEXT("LODTYPES_DEPTH_HD")); Show.Add(TEXT("LODTYPES_DEPTH_ORPHANHD")); }
	else if (!bAll) { Show.Add(TEXT("LODTYPES_DEPTH_") + Want); }
	TMap<FString, int32> Shown, Hidden;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FString Lv;
		for (const FName& T : It->Tags)
		{
			const FString S = T.ToString();
			if (S.StartsWith(TEXT("RUDE_LOD:"))) { Lv = S.Mid(9); break; }
		}
		if (Lv.IsEmpty()) { continue; }
		UStaticMeshComponent* SMC = It->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC) { continue; }
		const bool bShow = bAll || Show.Contains(Lv);
		SMC->SetVisibility(bShow, true);
		SMC->SetHiddenInGame(!bShow, true);
		It->MarkPackageDirty();
		(bShow ? Shown : Hidden).FindOrAdd(Lv)++;
	}
	auto Json = [](const TMap<FString, int32>& M)
	{
		FString O;
		for (const auto& KV : M) { O += FString::Printf(TEXT("%s\"%s\":%d"), O.IsEmpty() ? TEXT("") : TEXT(","), *KV.Key.Replace(TEXT("LODTYPES_DEPTH_"), TEXT("")), KV.Value); }
		return O;
	};
	int32 NShown = 0, NHidden = 0;
	for (const auto& KV : Shown) { NShown += KV.Value; }
	for (const auto& KV : Hidden) { NHidden += KV.Value; }
	return FString::Printf(TEXT("{\"ok\":%s,\"view\":\"%s\",\"shown\":%d,\"hidden\":%d,\"shownByLevel\":{%s},\"hiddenByLevel\":{%s}}"),
		(NShown + NHidden) > 0 ? TEXT("true") : TEXT("false"), Want.IsEmpty() ? TEXT("HD") : *Want, NShown, NHidden, *Json(Shown), *Json(Hidden));
}

// ---- PlaceArchetype ---------------------------------------------------------------------
// Author a NEW placement: an entity actor for a palette archetype at a UE-space location/rotation,
// destined for TargetYmap (a new ymap name, or an existing one to append to). Fields default the way
// the game's own new content does: ORPHANHD, PRI_REQUIRED, flags 1572864 (v1's in-game-proven
// value), lodDist from the archetype, a guid hashed from ymap:archetype:position (v1's rule).
FString URudeToolset::PlaceArchetype(const FString& PaletteFolder, const FString& ArchetypeName,
                                     const FString& LocationCm, const FString& RotationDeg, const FString& TargetYmap)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Name = ArchetypeName.TrimStartAndEnd().ToLower();
	const FString Ymap = TargetYmap.TrimStartAndEnd().ToLower();
	if (Name.IsEmpty() || Ymap.IsEmpty()) { return Fail(TEXT("ArchetypeName and TargetYmap are required")); }
	URudeArchetype* A = LoadObject<URudeArchetype>(nullptr, *(PaletteFolder / Name + TEXT(".") + Name));
	if (!A) { return Fail(FString::Printf(TEXT("no palette asset for '%s' under %s - build the palette first"), *Name, *PaletteFolder)); }
	TArray<FString> L, R;
	LocationCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(L, TEXT(","), true);
	RotationDeg.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(R, TEXT(","), true);
	if (L.Num() != 3) { return Fail(TEXT("LocationCm must be x,y,z in UE centimetres")); }
	const FVector Loc(FCString::Atod(*L[0]), FCString::Atod(*L[1]), FCString::Atod(*L[2]));
	const FRotator Rot(R.Num() == 3 ? FCString::Atod(*R[0]) : 0.0, R.Num() == 3 ? FCString::Atod(*R[1]) : 0.0, R.Num() == 3 ? FCString::Atod(*R[2]) : 0.0);
	UStaticMesh* Mesh = A->Mesh.LoadSynchronous();
	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Mesh && !ProxyCube) { return Fail(TEXT("the archetype has no mesh and no proxy is available")); }
	// the same row shape the manifest uses, so the one spawn helper builds it
	TSharedPtr<FJsonObject> Ent = MakeShared<FJsonObject>();
	Ent->SetStringField(TEXT("archetype"), Name);
	Ent->SetStringField(TEXT("srcYmap"), Ymap);
	Ent->SetStringField(TEXT("srcSlot"), TEXT(""));
	Ent->SetNumberField(TEXT("srcIndex"), -1);
	Ent->SetStringField(TEXT("lodLevel"), TEXT("LODTYPES_DEPTH_ORPHANHD"));
	Ent->SetStringField(TEXT("priorityLevel"), TEXT("PRI_REQUIRED"));
	Ent->SetNumberField(TEXT("lodDist"), A->LodDist);
	Ent->SetNumberField(TEXT("childLodDist"), 0.0);
	Ent->SetNumberField(TEXT("parentIndex"), -1);
	Ent->SetNumberField(TEXT("flags"), 1572864.0);
	Ent->SetNumberField(TEXT("numChildren"), 0.0);
	Ent->SetNumberField(TEXT("aoMultiplier"), 255.0);
	Ent->SetNumberField(TEXT("artificialAo"), 255.0);
	Ent->SetNumberField(TEXT("tintValue"), 0.0);
	Ent->SetStringField(TEXT("itemType"), TEXT("CEntityDef"));
	const FTransform Xf(Rot, Loc, FVector::OneVector);
	{
		const double X = Loc.X / 100.0, Y = -Loc.Y / 100.0, Z = Loc.Z / 100.0;
		const uint32 Guid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%s:%f:%f:%f"), *Ymap, *Name, X, Y, Z));
		Ent->SetNumberField(TEXT("guid"), (double)Guid);
	}
	AActor* Actor = RudeSpawnEntityActor(World, Ymap, Ent, Xf, Mesh ? Mesh : ProxyCube, Mesh == nullptr, A->TimeFlags);
	if (!Actor) { return Fail(TEXT("spawn failed")); }
	// authored = never "untouched": no SourceXml, so the export rebuilds it from its fields
	if (URudeEntityComponent* C = Actor->FindComponentByClass<URudeEntityComponent>())
	{
		C->SourceXml.Reset();
		C->SourceFieldsKey.Reset();
	}
	Actor->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"archetype\":\"%s\",\"targetYmap\":\"%s\",\"mesh\":%s,\"actor\":\"%s\",\"lodDist\":%g}"),
		*RudeJsonEscape(Name), *RudeJsonEscape(Ymap), Mesh ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Actor->GetActorLabel()), A->LodDist);
}

// ---- SetYmapVisible ---------------------------------------------------------------------
// Show or hide one ymap's placed actors (folder RUDE_LS/<ymap>) - the editor's stand-in for the
// game's IPL toggle on a script-controlled map. LOD-hidden actors stay governed by SetLodView.
FString URudeToolset::SetYmapVisible(const FString& YmapName, const FString& Visible)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return TEXT("{\"ok\":false,\"error\":\"no editor world\"}"); }
	const FString Want = YmapName.TrimStartAndEnd().ToLower();
	const bool bShow = Visible.TrimStartAndEnd().Equals(TEXT("true"), ESearchCase::IgnoreCase) || Visible.TrimStartAndEnd() == TEXT("1");
	const FString Folder = TEXT("RUDE_LS/") + Want;
	int32 Touched = 0, SkippedLod = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->GetFolderPath().ToString().Equals(Folder, ESearchCase::IgnoreCase)) { continue; }
		bool bLodHidden = false;
		for (const FName& T : It->Tags)
		{
			const FString S = T.ToString();
			if (S.StartsWith(TEXT("RUDE_LOD:")) && !S.EndsWith(TEXT("_HD"))) { bLodHidden = true; }
		}
		if (bShow && bLodHidden) { ++SkippedLod; continue; }
		if (UStaticMeshComponent* SMC = It->FindComponentByClass<UStaticMeshComponent>())
		{
			SMC->SetVisibility(bShow, true);
			SMC->SetHiddenInGame(!bShow, true);
			It->MarkPackageDirty();
			++Touched;
		}
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"ymap\":\"%s\",\"visible\":%s,\"actors\":%d,\"lodGoverned\":%d}"),
		Touched > 0 ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Want), bShow ? TEXT("true") : TEXT("false"), Touched, SkippedLod);
}

// ---- LodLineage (agent + Matt) -------------------------------------------------------------
// The chain an entity hands over along: up through its LodParent links to the top, and its children.
// Labels are archetype names (not unique), so "ymap:index" (the entity's source ordinal) is accepted too.
static AActor* RudeFindActorByLabel(UWorld* World, const FString& Label)
{
	const FString L = Label.TrimStartAndEnd();
	FString Ymap, Idx;
	if (L.Split(TEXT(":"), &Ymap, &Idx) && (Idx == TEXT("new") || Idx.StartsWith(TEXT("new:"))))
	{
		// "<ymap>:new" / "<ymap>:new:<archetype>": an entity authored in RUDE (no source ordinal yet)
		const FString Arch = Idx.StartsWith(TEXT("new:")) ? Idx.Mid(4).ToLower() : FString();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
			if (R && R->SourceIndex < 0 && R->SourceYmap.Equals(Ymap, ESearchCase::IgnoreCase)
				&& (Arch.IsEmpty() || R->ArchetypeName.ToLower() == Arch)) { return *It; }
		}
		return nullptr;
	}
	if (L.Split(TEXT(":"), &Ymap, &Idx) && Idx.IsNumeric())
	{
		const int32 I = FCString::Atoi(*Idx);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
			if (R && R->SourceIndex == I && R->SourceYmap.Equals(Ymap, ESearchCase::IgnoreCase)) { return *It; }
		}
		return nullptr;
	}
	for (TActorIterator<AActor> It(World); It; ++It) { if (It->GetActorLabel() == L) { return *It; } }
	return nullptr;
}
static FString RudeLodRow(const AActor* A)
{
	const URudeEntityComponent* R = A ? A->FindComponentByClass<URudeEntityComponent>() : nullptr;
	if (!R) { return TEXT("null"); }
	return FString::Printf(TEXT("{\"actor\":\"%s\",\"archetype\":\"%s\",\"ymap\":\"%s\",\"index\":%d,\"lodLevel\":\"%s\",\"lodDist\":%g,\"childLodDist\":%g,\"parentIndex\":%d,\"numChildren\":%d,\"childrenHere\":%d,\"partial\":%s}"),
		*RudeJsonEscape(A->GetActorLabel()), *RudeJsonEscape(R->ArchetypeName), *RudeJsonEscape(R->SourceYmap), R->SourceIndex,
		*R->LodLevel, R->LodDist, R->ChildLodDist, R->ParentIndex, R->NumChildren, R->LodChildren.Num(), R->bLodPartial ? TEXT("true") : TEXT("false"));
}
FString URudeToolset::LodLineage(const FString& ActorLabel)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* A = RudeFindActorByLabel(World, ActorLabel);
	if (!A) { return Fail(FString::Printf(TEXT("no actor labelled '%s'"), *ActorLabel)); }
	const URudeEntityComponent* R = A->FindComponentByClass<URudeEntityComponent>();
	if (!R) { return Fail(TEXT("that actor carries no RUDE entity")); }
	FString Up;
	int32 Depth = 0;
	for (const AActor* P = R->LodParent.Get(); P && Depth < 8; ++Depth)
	{
		Up += (Up.IsEmpty() ? TEXT("") : TEXT(",")) + RudeLodRow(P);
		const URudeEntityComponent* PR = P->FindComponentByClass<URudeEntityComponent>();
		P = PR ? PR->LodParent.Get() : nullptr;
	}
	FString Kids;
	int32 N = 0;
	for (const TSoftObjectPtr<AActor>& C : R->LodChildren)
	{
		if (N++ < 24) { Kids += (Kids.IsEmpty() ? TEXT("") : TEXT(",")) + RudeLodRow(C.Get()); }
	}
	return FString::Printf(TEXT("{\"ok\":true,\"entity\":%s,\"ymapParent\":\"%s\",\"up\":[%s],\"children\":%d,\"childrenListed\":[%s]}"),
		*RudeLodRow(A), *RudeJsonEscape(R->SourceYmapParent), *Up, R->LodChildren.Num(), *Kids);
}

// ---- SetLodParent (agent + Matt) -----------------------------------------------------------
// Re-parent an entity (empty ParentLabel = orphan it). Refuses what the ymap format cannot express:
// a parent outside this ymap and its parent ymap, a parent not exactly one level coarser, or a parent
// whose children are not all in this level (its numChildren is verbatim). The fields the export will
// derive are previewed in the verdict; lodDist / childLodDist are left exactly as they were.
FString URudeToolset::SetLodParent(const FString& ActorLabel, const FString& ParentLabel)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* A = RudeFindActorByLabel(World, ActorLabel);
	if (!A) { return Fail(FString::Printf(TEXT("no actor labelled '%s'"), *ActorLabel)); }
	URudeEntityComponent* R = A->FindComponentByClass<URudeEntityComponent>();
	if (!R) { return Fail(TEXT("that actor carries no RUDE entity")); }
	AActor* PA = nullptr;
	URudeEntityComponent* P = nullptr;
	if (!ParentLabel.TrimStartAndEnd().IsEmpty())
	{
		PA = RudeFindActorByLabel(World, ParentLabel);
		if (!PA) { return Fail(FString::Printf(TEXT("no actor labelled '%s'"), *ParentLabel)); }
		if (PA == A) { return Fail(TEXT("an entity cannot be its own parent")); }
		P = PA->FindComponentByClass<URudeEntityComponent>();
		if (!P) { return Fail(TEXT("the parent actor carries no RUDE entity")); }
		if (RudeLodRank(P->LodLevel) != RudeLodRank(R->LodLevel) + 1)
		{
			return Fail(FString::Printf(TEXT("parent is %s; the parent of a %s must be the level one step coarser"), *P->LodLevel, *R->LodLevel));
		}
		const FString YmapR = R->SourceYmap.ToLower(), YmapP = P->SourceYmap.ToLower();
		if (!YmapR.IsEmpty() && !YmapP.IsEmpty() && YmapP != YmapR && YmapP != R->SourceYmapParent.ToLower())
		{
			return Fail(FString::Printf(TEXT("parent lives in '%s'; a parent must be in this entity's ymap '%s' or its parent ymap '%s' (ENGINEERING_LOG law 24)"), *YmapP, *YmapR, *R->SourceYmapParent.ToLower()));
		}
		if (P->bLodPartial && !P->LodChildren.Contains(A))
		{
			return Fail(TEXT("that parent's children are not all in this level, so its numChildren is kept verbatim and cannot absorb another child"));
		}
	}
	if (AActor* Old = R->LodParent.Get())
	{
		if (Old != PA)
		{
			if (URudeEntityComponent* OR = Old->FindComponentByClass<URudeEntityComponent>())
			{
				if (OR->bLodPartial) { return Fail(TEXT("this entity's current parent has children outside the level; its count is verbatim and cannot drop one")); }
				Old->Modify();
				OR->LodChildren.Remove(A);
			}
		}
	}
	A->Modify();
	R->LodParent = PA;
	if (PA && P)
	{
		PA->Modify();
		P->LodChildren.AddUnique(A);
	}
	// preview of what the export derives
	TMap<const AActor*, URudeEntityComponent*> Comp;
	TMap<const URudeEntityComponent*, int32> Kids;
	TArray<URudeEntityComponent*> All;
	RudeCountLodKids(World, Comp, Kids, All);
	FRudeLodDerived D;
	FString Why;
	const TMap<const URudeEntityComponent*, int32> NoOrdinals;
	const bool bOk = RudeDeriveLodFields(R, Comp, Kids, NoOrdinals, TEXT(""), D, Why);
	return FString::Printf(TEXT("{\"ok\":%s,\"actor\":\"%s\",\"parent\":\"%s\",\"willExport\":{\"parentIndex\":%d,\"lodLevel\":\"%s\",\"numChildren\":%d},\"parentChildrenNow\":%d,\"note\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(A->GetActorLabel()), PA ? *RudeJsonEscape(PA->GetActorLabel()) : TEXT(""),
		D.ParentIndex, *D.LodLevel, D.NumChildren, P ? P->LodChildren.Num() : 0,
		bOk ? TEXT("lodDist and childLodDist are authored values and were not touched (law 26)") : *RudeJsonEscape(Why));
}

// ---- LodAudit (agent) ----------------------------------------------------------------------
// Every entity: what the links derive vs what the fields store. On an untouched level this MUST be
// zero diffs - that is the proof the derivation reproduces the game's own data before it is trusted
// to write any.
FString URudeToolset::LodAudit()
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	TMap<const AActor*, URudeEntityComponent*> Comp;
	TMap<const URudeEntityComponent*, int32> Kids;
	TArray<URudeEntityComponent*> All;
	RudeCountLodKids(World, Comp, Kids, All);
	const TMap<const URudeEntityComponent*, int32> NoOrdinals;
	int32 Links = 0, Partial = 0, Unresolved = 0, Diffs = 0, Refusals = 0, DiffParent = 0, DiffCount = 0, DiffLevel = 0, Pending = 0;
	FString Rows;
	for (const URudeEntityComponent* R : All)
	{
		if (!R->LodParent.IsNull()) { ++Links; }
		if (R->bLodPartial) { ++Partial; }
		if (R->GetOwner()->Tags.Contains(FName(TEXT("RUDE_LOD_UNRESOLVED")))) { ++Unresolved; }
		FRudeLodDerived D;
		FString Why;
		if (!RudeDeriveLodFields(R, Comp, Kids, NoOrdinals, TEXT(""), D, Why))
		{
			++Refusals;
			if (Refusals <= 10) { Rows += FString::Printf(TEXT("%s{\"actor\":\"%s\",\"refused\":\"%s\"}"), Rows.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(R->GetOwner()->GetActorLabel()), *RudeJsonEscape(Why)); }
			continue;
		}
		if (!D.bChanged) { continue; }
		// an entity authored in RUDE (no source ordinal) or whose link was edited derives at export:
		// that is PENDING, not a disagreement with the game's data
		if (R->SourceIndex < 0 || R->SourceFieldsKey.IsEmpty() || R->FieldsKey() != R->SourceFieldsKey) { ++Pending; continue; }
		++Diffs;
		if (D.ParentIndex != R->ParentIndex) { ++DiffParent; }
		if (D.NumChildren != R->NumChildren) { ++DiffCount; }
		if (D.LodLevel != R->LodLevel) { ++DiffLevel; }
		if (Diffs <= 10)
		{
			Rows += FString::Printf(TEXT("%s{\"actor\":\"%s\",\"ymap\":\"%s\",\"stored\":[%d,%d,\"%s\"],\"derived\":[%d,%d,\"%s\"]}"),
				Rows.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(R->GetOwner()->GetActorLabel()), *RudeJsonEscape(R->SourceYmap),
				R->ParentIndex, R->NumChildren, *R->LodLevel, D.ParentIndex, D.NumChildren, *D.LodLevel);
		}
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"entities\":%d,\"links\":%d,\"unresolved\":%d,\"partial\":%d,\"diffs\":%d,\"diffParentIndex\":%d,\"diffNumChildren\":%d,\"diffLodLevel\":%d,\"pendingAtExport\":%d,\"refusals\":%d,\"first\":[%s]}"),
		(Diffs == 0 && Refusals == 0) ? TEXT("true") : TEXT("false"), All.Num(), Links, Unresolved, Partial, Diffs, DiffParent, DiffCount, DiffLevel, Pending, Refusals, *Rows);
}

// ---- MakeLodArchetype (Wave 2 / WP8 step 1) ------------------------------------------------
// "Swap a building, press rebuild, distances behave" (GDD Wave 2). Regenerates an HD entity's LOD
// parent from the HD mesh itself: a reduced copy of the HD static mesh becomes a NEW drawable
// asset, a NEW palette archetype wraps it (bounds from the mesh, the LOD distance given, textures
// the HD's), and then EITHER the existing LOD parent entity is re-pointed at the new archetype
// (its ordinal, parentIndex and numChildren stay: no lineage edit at all) OR, for an orphan, a
// LOD entity is placed where the format allows one (the HD ymap's parent ymap when it has one,
// else the HD's own ymap) and linked. Nothing existing loses its lodDist / childLodDist (law 26);
// a NEW parent gets childLodDist = the HD child's lodDist (the game's own rule 958/1,084 times).
// NewArchetypeName defaults to <hd>_rlod - a RUDE default, not the game's convention: rename freely.
FString URudeToolset::MakeLodArchetype(const FString& ActorLabel, const FString& NewArchetypeName,
                                       const FString& TrianglePercent, const FString& LodDist, const FString& PaletteFolder)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* A = RudeFindActorByLabel(World, ActorLabel);
	if (!A) { return Fail(FString::Printf(TEXT("no actor labelled '%s'"), *ActorLabel)); }
	URudeEntityComponent* R = A->FindComponentByClass<URudeEntityComponent>();
	if (!R) { return Fail(TEXT("that actor carries no RUDE entity")); }
	if (!RudeIsHdLevel(R->LodLevel)) { return Fail(FString::Printf(TEXT("'%s' is %s; give the HD entity whose LOD parent you want rebuilt"), *ActorLabel, *R->LodLevel)); }
	if (A->Tags.Contains(FName(TEXT("RUDE_PROXY")))) { return Fail(TEXT("that entity is a proxy cube (its drawable never imported) - nothing to reduce")); }
	UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>();
	UStaticMesh* Src = SMC ? SMC->GetStaticMesh() : nullptr;
	if (!Src) { return Fail(TEXT("the HD entity has no static mesh")); }
	const FString Palette = PaletteFolder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game/RUDE/Palette/Downtown")) : PaletteFolder.TrimStartAndEnd();
	const FString HdName = R->ArchetypeName.ToLower();
	URudeArchetype* HdArch = LoadObject<URudeArchetype>(nullptr, *(Palette / HdName + TEXT(".") + HdName));
	if (!HdArch) { return Fail(FString::Printf(TEXT("no palette asset for '%s' under %s - build the palette first"), *HdName, *Palette)); }
	FString NewName = NewArchetypeName.TrimStartAndEnd().ToLower();
	if (NewName.IsEmpty()) { NewName = HdName + TEXT("_rlod"); }
	// default 20: the game's own _lod drawables measure ~18% of their HD (dt1_05_hedge2 2,520 -> 446, 2026-09-06)
	const double Pct = FMath::Clamp(TrianglePercent.TrimStartAndEnd().IsEmpty() ? 20.0 : FCString::Atod(*TrianglePercent), 1.0, 100.0);

	// the existing LOD parent, if any (re-pointed, never re-linked)
	AActor* PA = R->LodParent.Get();
	URudeEntityComponent* PR = PA ? PA->FindComponentByClass<URudeEntityComponent>() : nullptr;
	URudeArchetype* OldLodArch = nullptr;
	if (PR) { OldLodArch = LoadObject<URudeArchetype>(nullptr, *(Palette / PR->ArchetypeName.ToLower() + TEXT(".") + PR->ArchetypeName.ToLower())); }
	double NewLodDist = LodDist.TrimStartAndEnd().IsEmpty() ? 0.0 : FCString::Atod(*LodDist);
	if (NewLodDist <= 0.0) { NewLodDist = PR ? PR->LodDist : (OldLodArch ? OldLodArch->LodDist : FMath::Max(R->LodDist * 4.0, 300.0)); }

	// a palette asset of that name: only one RUDE made before may be regenerated in place
	{
		if (URudeArchetype* Existing = LoadObject<URudeArchetype>(nullptr, *(Palette / NewName + TEXT(".") + NewName)))
		{
			if (Existing->SourceIndex >= 0 || !Existing->SourceXml.IsEmpty())
			{
				return Fail(FString::Printf(TEXT("'%s' is a game archetype in the palette; choose a NewArchetypeName the game does not use"), *NewName));
			}
		}
	}

	// 1) the reduced drawable: a copy of the HD mesh, LOD0 reduced to TrianglePercent, other LODs dropped
	const FString MeshFolder = FPackageName::GetLongPackagePath(Src->GetOutermost()->GetName());
	const FString MeshPkgName = MeshFolder / NewName;
	UPackage* MeshPkg = CreatePackage(*MeshPkgName);
	MeshPkg->FullyLoad();
	if (UObject* Stale = StaticFindObject(UStaticMesh::StaticClass(), MeshPkg, *NewName)) { Stale->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); }
	UStaticMesh* NewMesh = DuplicateObject<UStaticMesh>(Src, MeshPkg, FName(*NewName));
	if (!NewMesh) { return Fail(TEXT("could not duplicate the HD mesh")); }
	NewMesh->SetFlags(RF_Public | RF_Standalone);
	NewMesh->ClearFlags(RF_Transient);
	// Reduce the SOURCE geometry (the mesh description), not just the render data: the ydr writer
	// reads the description, so a render-only reduction would export the full HD mesh (measured
	// 2026-09-06: 2,520 triangles out of a "754-triangle" LOD).
	const FMeshDescription* SrcDesc = NewMesh->GetMeshDescription(0);
	if (!SrcDesc) { return Fail(TEXT("the HD mesh has no source geometry to reduce")); }
	const int32 TrisBefore = SrcDesc->Triangles().Num();
	IMeshReductionManagerModule& ReducerModule = FModuleManager::LoadModuleChecked<IMeshReductionManagerModule>("MeshReductionInterface");
	IMeshReduction* Reducer = ReducerModule.GetStaticMeshReductionInterface();
	if (!Reducer) { return Fail(TEXT("no static mesh reduction module is available in this editor")); }
	FMeshDescription Reduced;
	FStaticMeshAttributes(Reduced).Register();
	{
		FOverlappingCorners Corners;
		FStaticMeshOperations::FindOverlappingCorners(Corners, *SrcDesc, THRESH_POINTS_ARE_SAME);
		FMeshReductionSettings RS;
		RS.TerminationCriterion = EStaticMeshReductionTerimationCriterion::Triangles;
		RS.PercentTriangles = (float)(Pct / 100.0);
		RS.PercentVertices = 1.0f;
		float MaxDeviation = 0.f;
		Reducer->ReduceMeshDescription(Reduced, MaxDeviation, *SrcDesc, Corners, RS);
	}
	const int32 TrisAfter = Reduced.Triangles().Num();
	NewMesh->SetNumSourceModels(1);
	{
		FMeshDescription* Dst = NewMesh->CreateMeshDescription(0, MoveTemp(Reduced));
		if (!Dst) { return Fail(TEXT("could not store the reduced geometry")); }
		NewMesh->CommitMeshDescription(0);
		FStaticMeshSourceModel& SM = NewMesh->GetSourceModel(0);
		SM.ReductionSettings.PercentTriangles = 1.0f;   // already reduced at the source
		SM.ReductionSettings.PercentVertices = 1.0f;
		SM.BuildSettings.bRecomputeNormals = false;
		SM.BuildSettings.bRecomputeTangents = false;
	}
	NewMesh->Build(/*bSilent*/ true);
	NewMesh->PostEditChange();
	NewMesh->MarkPackageDirty();
	if (TrisAfter <= 0) { return Fail(TEXT("the reduced mesh has no triangles - the reduction did not run (is the mesh reduction module available?)")); }

	// 2) the palette archetype, appended to the HD archetype's ytyp by ExportPaletteYtyps
	const FString ArchPkgName = Palette / NewName;
	UPackage* ArchPkg = CreatePackage(*ArchPkgName);
	ArchPkg->FullyLoad();
	URudeArchetype* NA = FindObject<URudeArchetype>(ArchPkg, *NewName);
	if (!NA) { NA = NewObject<URudeArchetype>(ArchPkg, FName(*NewName), RF_Public | RF_Standalone); }
	NA->ArchetypeKind = TEXT("CBaseArchetypeDef");
	NA->Name = NewName;
	NA->AssetName = NewName;
	NA->AssetType = TEXT("ASSET_TYPE_DRAWABLE");
	NA->TextureDictionary = NewName;   // its own txd: ExportMeshTextures(mesh, <NewName>.ytd, MaxDim) writes it
	NA->PhysicsDictionary.Reset();
	NA->DrawableDictionary.Reset();
	NA->ClipDictionary.Reset();
	NA->LodDist = (float)NewLodDist;
	NA->HdTextureDist = OldLodArch ? OldLodArch->HdTextureDist : 0.f;
	NA->Flags = OldLodArch ? OldLodArch->Flags : HdArch->Flags;
	NA->SpecialAttribute = 0;
	NA->TimeFlags = 0;
	NA->ExtensionsXml.Reset();
	NA->MloXml.Reset();
	{
		// bounds from the mesh, spelled in RAGE metres/axes (x, -y, z; y flips min/max)
		const FBox B = NewMesh->GetBoundingBox();
		const FVector Mn(B.Min.X / 100.0, -B.Max.Y / 100.0, B.Min.Z / 100.0);
		const FVector Mx(B.Max.X / 100.0, -B.Min.Y / 100.0, B.Max.Z / 100.0);
		NA->BbMin = Mn; NA->BbMax = Mx;
		NA->BsCentre = (Mn + Mx) * 0.5;
		NA->BsRadius = (float)((Mx - Mn).Size() * 0.5);
	}
	NA->Mesh = NewMesh;
	NA->SourceYtyp = HdArch->SourceYtyp;   // the target file: appended after every source item
	NA->SourceSlot = HdArch->SourceSlot;
	NA->SourceIndex = -1;
	NA->SourceXml.Reset();
	NA->SourceFieldsKey.Reset();
	NA->MarkPackageDirty();
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(NewMesh);
		ARM.Get().AssetCreated(NA);
	}

	// 3) the entity: re-point the existing LOD parent, or place + link one for an orphan
	FString Mode, Touched;
	if (PR && PA)
	{
		PA->Modify();
		PR->ArchetypeName = NewName;
		if (UStaticMeshComponent* PS = PA->FindComponentByClass<UStaticMeshComponent>()) { PS->SetStaticMesh(NewMesh); }
		PA->Tags.Remove(FName(TEXT("RUDE_PROXY")));
		PA->MarkPackageDirty();
		Mode = TEXT("replaced");
		Touched = FString::Printf(TEXT("%s:%d"), *PR->SourceYmap, PR->SourceIndex);
	}
	else
	{
		const FString Ymap = R->SourceYmapParent.IsEmpty() ? R->SourceYmap.ToLower() : R->SourceYmapParent.ToLower();
		TSharedPtr<FJsonObject> Ent = MakeShared<FJsonObject>();
		Ent->SetStringField(TEXT("archetype"), NewName);
		Ent->SetStringField(TEXT("srcYmap"), Ymap);
		Ent->SetStringField(TEXT("srcSlot"), TEXT(""));
		Ent->SetNumberField(TEXT("srcIndex"), -1);
		Ent->SetStringField(TEXT("lodLevel"), TEXT("LODTYPES_DEPTH_LOD"));
		Ent->SetStringField(TEXT("priorityLevel"), R->PriorityLevel);
		Ent->SetNumberField(TEXT("lodDist"), NewLodDist);
		Ent->SetNumberField(TEXT("childLodDist"), R->LodDist);
		Ent->SetNumberField(TEXT("parentIndex"), -1);
		Ent->SetNumberField(TEXT("flags"), (double)R->Flags);
		Ent->SetNumberField(TEXT("numChildren"), 1.0);
		Ent->SetNumberField(TEXT("aoMultiplier"), 255.0);
		Ent->SetNumberField(TEXT("artificialAo"), 255.0);
		Ent->SetNumberField(TEXT("tintValue"), 0.0);
		Ent->SetStringField(TEXT("itemType"), TEXT("CEntityDef"));
		const FTransform Xf = A->GetActorTransform();
		{
			const FVector L = Xf.GetLocation();
			const uint32 Guid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%s:%f:%f:%f"), *Ymap, *NewName, L.X / 100.0, -L.Y / 100.0, L.Z / 100.0));
			Ent->SetNumberField(TEXT("guid"), (double)Guid);
		}
		AActor* NewA = RudeSpawnEntityActor(World, Ymap, Ent, Xf, NewMesh, false, 0);
		if (!NewA) { return Fail(TEXT("could not place the LOD entity")); }
		URudeEntityComponent* NR = NewA->FindComponentByClass<URudeEntityComponent>();
		NR->SourceXml.Reset();
		NR->SourceFieldsKey.Reset();
		NR->SourceYmapParent = R->SourceYmapParent.IsEmpty() ? FString() : FString();   // a LOD in the HD's parent ymap: that file's own parent is unknown here
		NR->LodChildren.Add(A);
		A->Modify();
		R->LodParent = NewA;
		R->LodLevel = TEXT("LODTYPES_DEPTH_HD");   // it has a parent now; parentIndex is assigned at export (ordinals)
		NewA->MarkPackageDirty();
		A->MarkPackageDirty();
		Mode = TEXT("placed");
		Touched = FString::Printf(TEXT("%s:new (%s)"), *Ymap, *NewA->GetActorLabel());
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"mode\":\"%s\",\"hd\":\"%s\",\"newArchetype\":\"%s\",\"mesh\":\"%s\",\"trianglesBefore\":%d,\"trianglesAfter\":%d,")
		TEXT("\"lodDist\":%g,\"childLodDist\":%g,\"bsRadius\":%g,\"targetYtyp\":\"%s\",\"lodEntity\":\"%s\",")
		TEXT("\"note\":\"then: ExportYdrBinary(mesh, <name>.ydr, NOBOUND) + ExportMeshTextures(mesh, <name>.ytd, 256) + ExportPaletteYtyps + ExportLevelYmaps; existing lodDist/childLodDist untouched (law 26)\"}"),
		*Mode, *RudeJsonEscape(R->ArchetypeName), *RudeJsonEscape(NewName), *RudeJsonEscape(MeshPkgName), TrisBefore, TrisAfter,
		NewLodDist, PR ? PR->ChildLodDist : R->LodDist, NA->BsRadius, *RudeJsonEscape(NA->SourceYtyp), *RudeJsonEscape(Touched));
}

// ---- RebuildLodChunk (Wave 2 / WP8 step 2) --------------------------------------------------
// The game's SLOD chunk, measured on downtown 2026-09-06: dt1_lod_03_04_05_11 (SLOD2) is FOUR blocks'
// LOD shells baked into ONE 7,984-triangle drawable over a 498 x 397 m footprint, textured by two
// 1024^2 atlases (+ 512^2 speculars) in its own txd, lodDist 1,500. That is what Unreal's merge-with-
// material-baking produces, so: merge the parent's children (the next-finer shells placed under it),
// bake their materials to an atlas of AtlasSize, reduce the merged geometry to TrianglePercent, wrap
// it in a palette archetype (own txd, the old parent's lodDist, bounds from the mesh) and re-point the
// parent entity at it. Lineage is untouched: ordinals, parentIndex, numChildren stay; the parent's
// position moves to the merged pivot (the export rebuilds that one entity).
// The baked textures are re-instanced onto RUDE's opaque master (Diffuse / Normal) so the ydr and ytd
// writers see the parameters they already know.
FString URudeToolset::RebuildLodChunk(const FString& ParentLabel, const FString& NewArchetypeName,
                                      const FString& TrianglePercent, const FString& AtlasSize, const FString& PaletteFolder)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* PA = RudeFindActorByLabel(World, ParentLabel);
	if (!PA) { return Fail(FString::Printf(TEXT("no actor labelled '%s'"), *ParentLabel)); }
	URudeEntityComponent* PR = PA->FindComponentByClass<URudeEntityComponent>();
	if (!PR) { return Fail(TEXT("that actor carries no RUDE entity")); }
	if (RudeIsHdLevel(PR->LodLevel)) { return Fail(TEXT("give a LOD/SLOD parent, not an HD entity (MakeLodArchetype rebuilds an HD's own LOD)")); }
	if (PR->LodChildren.Num() == 0) { return Fail(TEXT("that parent has no children placed in this level - nothing to merge")); }
	if (PR->bLodPartial) { return Fail(TEXT("that parent's children are not all in this level (RUDE_LOD_PARTIAL); merging a partial set would drop the rest from the chunk")); }
	const FString Palette = PaletteFolder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game/RUDE/Palette/Downtown")) : PaletteFolder.TrimStartAndEnd();
	const FString OldName = PR->ArchetypeName.ToLower();
	URudeArchetype* OldArch = LoadObject<URudeArchetype>(nullptr, *(Palette / OldName + TEXT(".") + OldName));
	FString NewName = NewArchetypeName.TrimStartAndEnd().ToLower();
	if (NewName.IsEmpty()) { NewName = OldName + TEXT("_rchunk"); }
	if (URudeArchetype* Existing = LoadObject<URudeArchetype>(nullptr, *(Palette / NewName + TEXT(".") + NewName)))
	{
		if (Existing->SourceIndex >= 0 || !Existing->SourceXml.IsEmpty()) { return Fail(FString::Printf(TEXT("'%s' is a game archetype in the palette; choose a NewArchetypeName the game does not use"), *NewName)); }
	}
	const double Pct = FMath::Clamp(TrianglePercent.TrimStartAndEnd().IsEmpty() ? 50.0 : FCString::Atod(*TrianglePercent), 1.0, 100.0);
	const int32 Atlas = FMath::Clamp(AtlasSize.TrimStartAndEnd().IsEmpty() ? 1024 : FCString::Atoi(*AtlasSize), 128, 4096);

	// 1) the components to merge: every child's mesh (proxies and missing meshes are skipped and counted)
	TArray<UPrimitiveComponent*> Comps;
	int32 ChildTris = 0, Skipped = 0;
	FString MeshFolder;
	for (const TSoftObjectPtr<AActor>& C : PR->LodChildren)
	{
		AActor* CA = C.Get();
		UStaticMeshComponent* SMC = CA ? CA->FindComponentByClass<UStaticMeshComponent>() : nullptr;
		if (!SMC || !SMC->GetStaticMesh() || CA->Tags.Contains(FName(TEXT("RUDE_PROXY")))) { ++Skipped; continue; }
		Comps.Add(SMC);
		if (const FMeshDescription* D = SMC->GetStaticMesh()->GetMeshDescription(0)) { ChildTris += D->Triangles().Num(); }
		if (MeshFolder.IsEmpty()) { MeshFolder = FPackageName::GetLongPackagePath(SMC->GetStaticMesh()->GetOutermost()->GetName()); }
	}
	if (Comps.Num() == 0) { return Fail(TEXT("none of the children has a real mesh to merge")); }

	// 2) merge + bake (the engine's flatten material; re-instanced onto the RUDE master below)
	IMeshMergeUtilities& Merge = FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();
	FMeshMergingSettings MS;
	MS.bMergeMaterials = true;
	MS.bMergePhysicsData = false;
	MS.bGenerateLightMapUV = false;
	MS.bComputedLightMapResolution = false;
	MS.bBakeVertexDataToMesh = false;
	MS.bUseVertexDataForBakingMaterial = false;
	MS.bReuseMeshLightmapUVs = false;
	MS.bMergeEquivalentMaterials = true;
	MS.bAllowDistanceField = false;
	MS.bSupportRayTracing = false;
	MS.LODSelectionType = EMeshLODSelectionType::SpecificLOD;
	MS.SpecificLOD = 0;
	MS.MaterialSettings.TextureSizingType = TextureSizingType_UseSingleTextureSize;
	MS.MaterialSettings.TextureSize = FIntPoint(Atlas, Atlas);
	MS.MaterialSettings.bNormalMap = true;
	MS.MaterialSettings.bSpecularMap = false;
	MS.MaterialSettings.bMetallicMap = false;
	MS.MaterialSettings.bRoughnessMap = false;
	MS.MaterialSettings.bEmissiveMap = false;
	MS.MaterialSettings.bOpacityMap = false;
	MS.MaterialSettings.bOpacityMaskMap = false;
	MS.MaterialSettings.bAmbientOcclusionMap = false;
	TArray<UObject*> Assets;
	FVector MergedLocation = FVector::ZeroVector;
	const FString MergePkgBase = MeshFolder / (NewName + TEXT("_merge"));
	// the engine's flatten material is the bake target (a null base material is dereferenced inside the
	// merge - measured as an access violation 2026-09-06); the result is re-instanced onto RUDE's master below
	UMaterialInterface* Flatten = GEngine ? GEngine->DefaultFlattenMaterial : nullptr;
	if (!Flatten) { return Fail(TEXT("no DefaultFlattenMaterial in this editor (Engine.DefaultFlattenMaterialName)")); }
	Merge.MergeComponentsToStaticMesh(Comps, World, MS, Flatten, nullptr, MergePkgBase, Assets, MergedLocation, 1.0f, /*bSilent*/ true);
	UStaticMesh* Merged = nullptr;
	UMaterialInstanceConstant* BakedMI = nullptr;
	TArray<UTexture2D*> BakedTex;
	for (UObject* O : Assets)
	{
		if (UStaticMesh* M = Cast<UStaticMesh>(O)) { Merged = M; }
		else if (UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(O)) { BakedMI = MI; }
		else if (UTexture2D* T = Cast<UTexture2D>(O)) { BakedTex.Add(T); }
	}
	if (!Merged) { return Fail(TEXT("the merge produced no static mesh")); }
	const FMeshDescription* MergedDesc = Merged->GetMeshDescription(0);
	if (!MergedDesc) { return Fail(TEXT("the merged mesh has no source geometry")); }
	const int32 MergedTris = MergedDesc->Triangles().Num();

	// 3) the chunk drawable: the merged geometry reduced to TrianglePercent, as its own asset
	const FString MeshPkgName = MeshFolder / NewName;
	UPackage* MeshPkg = CreatePackage(*MeshPkgName);
	MeshPkg->FullyLoad();
	if (UObject* Stale = StaticFindObject(UStaticMesh::StaticClass(), MeshPkg, *NewName)) { Stale->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); }
	UStaticMesh* NewMesh = DuplicateObject<UStaticMesh>(Merged, MeshPkg, FName(*NewName));
	if (!NewMesh) { return Fail(TEXT("could not copy the merged mesh")); }
	NewMesh->SetFlags(RF_Public | RF_Standalone);
	NewMesh->ClearFlags(RF_Transient);
	IMeshReduction* Reducer = FModuleManager::LoadModuleChecked<IMeshReductionManagerModule>("MeshReductionInterface").GetStaticMeshReductionInterface();
	if (!Reducer) { return Fail(TEXT("no static mesh reduction module is available in this editor")); }
	FMeshDescription Reduced;
	FStaticMeshAttributes(Reduced).Register();
	{
		FOverlappingCorners Corners;
		FStaticMeshOperations::FindOverlappingCorners(Corners, *MergedDesc, THRESH_POINTS_ARE_SAME);
		FMeshReductionSettings RS;
		RS.TerminationCriterion = EStaticMeshReductionTerimationCriterion::Triangles;
		RS.PercentTriangles = (float)(Pct / 100.0);
		RS.PercentVertices = 1.0f;
		float MaxDeviation = 0.f;
		Reducer->ReduceMeshDescription(Reduced, MaxDeviation, *MergedDesc, Corners, RS);
	}
	const int32 TrisAfter = Reduced.Triangles().Num();
	NewMesh->SetNumSourceModels(1);
	{
		FMeshDescription* Dst = NewMesh->CreateMeshDescription(0, MoveTemp(Reduced));
		if (!Dst) { return Fail(TEXT("could not store the reduced chunk geometry")); }
		NewMesh->CommitMeshDescription(0);
		FStaticMeshSourceModel& SM = NewMesh->GetSourceModel(0);
		SM.ReductionSettings.PercentTriangles = 1.0f;
		SM.ReductionSettings.PercentVertices = 1.0f;
		SM.BuildSettings.bRecomputeNormals = false;
		SM.BuildSettings.bRecomputeTangents = false;
	}
	// the baked textures onto the RUDE opaque master, under the sampler names the writers know
	FString TexNames;
	{
		UMaterial* Master = LoadObject<UMaterial>(nullptr, TEXT("/RUDE/Masters/M_RUDE_Opaque.M_RUDE_Opaque"));
		UTexture* Diffuse = nullptr; UTexture* Normal = nullptr;
		if (BakedMI)
		{
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids;
			BakedMI->GetAllTextureParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& I : Infos)
			{
				UTexture* T = nullptr;
				if (!BakedMI->GetTextureParameterValue(I, T) || !T) { continue; }
				const FString N = I.Name.ToString();
				if (N.Contains(TEXT("Diffuse")) || N.Contains(TEXT("BaseColor"))) { Diffuse = T; }
				else if (N.Contains(TEXT("Normal"))) { Normal = T; }
			}
		}
		// RAGE names for the atlases: <chunk>_a / <chunk>_n, by renaming the baked texture objects
		if (Diffuse) { Diffuse->Rename(*(NewName + TEXT("_a")), nullptr, REN_DontCreateRedirectors | REN_NonTransactional); Diffuse->MarkPackageDirty(); TexNames += TEXT("\"") + NewName + TEXT("_a\""); }
		if (Normal) { Normal->Rename(*(NewName + TEXT("_n")), nullptr, REN_DontCreateRedirectors | REN_NonTransactional); Normal->MarkPackageDirty(); TexNames += FString(TexNames.IsEmpty() ? TEXT("") : TEXT(",")) + TEXT("\"") + NewName + TEXT("_n\""); }
		if (Master)
		{
			const FString MIName = TEXT("MI_") + NewName;
			UPackage* MIPkg = CreatePackage(*(MeshFolder / MIName));
			MIPkg->FullyLoad();
			UMaterialInstanceConstant* MI = FindObject<UMaterialInstanceConstant>(MIPkg, *MIName);
			if (!MI) { MI = NewObject<UMaterialInstanceConstant>(MIPkg, FName(*MIName), RF_Public | RF_Standalone); }
			MI->SetParentEditorOnly(Master);
			if (Diffuse) { MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Diffuse")), Diffuse); }
			if (Normal) { MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Normal")), Normal); }
			MI->PostEditChange();
			MI->MarkPackageDirty();
			TArray<FStaticMaterial> Mats = NewMesh->GetStaticMaterials();
			for (FStaticMaterial& SMat : Mats) { SMat.MaterialInterface = MI; }
			if (Mats.Num() == 0) { Mats.Add(FStaticMaterial(MI, FName(TEXT("chunk")))); }
			NewMesh->SetStaticMaterials(Mats);
		}
	}
	NewMesh->Build(/*bSilent*/ true);
	NewMesh->PostEditChange();
	NewMesh->MarkPackageDirty();

	// 4) the palette archetype
	const FString ArchPkgName = Palette / NewName;
	UPackage* ArchPkg = CreatePackage(*ArchPkgName);
	ArchPkg->FullyLoad();
	URudeArchetype* NA = FindObject<URudeArchetype>(ArchPkg, *NewName);
	if (!NA) { NA = NewObject<URudeArchetype>(ArchPkg, FName(*NewName), RF_Public | RF_Standalone); }
	NA->ArchetypeKind = TEXT("CBaseArchetypeDef");
	NA->Name = NewName; NA->AssetName = NewName; NA->AssetType = TEXT("ASSET_TYPE_DRAWABLE");
	NA->TextureDictionary = NewName;
	NA->PhysicsDictionary.Reset(); NA->DrawableDictionary.Reset(); NA->ClipDictionary.Reset();
	NA->LodDist = PR->LodDist > 0.f ? PR->LodDist : (OldArch ? OldArch->LodDist : 1500.f);
	NA->HdTextureDist = OldArch ? OldArch->HdTextureDist : 0.f;
	NA->Flags = OldArch ? OldArch->Flags : 0u;
	NA->SpecialAttribute = 0; NA->TimeFlags = 0; NA->ExtensionsXml.Reset(); NA->MloXml.Reset();
	{
		const FBox B = NewMesh->GetBoundingBox();
		const FVector Mn(B.Min.X / 100.0, -B.Max.Y / 100.0, B.Min.Z / 100.0);
		const FVector Mx(B.Max.X / 100.0, -B.Min.Y / 100.0, B.Max.Z / 100.0);
		NA->BbMin = Mn; NA->BbMax = Mx; NA->BsCentre = (Mn + Mx) * 0.5; NA->BsRadius = (float)((Mx - Mn).Size() * 0.5);
	}
	NA->Mesh = NewMesh;
	NA->SourceYtyp = OldArch ? OldArch->SourceYtyp : FString();
	NA->SourceSlot = OldArch ? OldArch->SourceSlot : FString();
	NA->SourceIndex = -1; NA->SourceXml.Reset(); NA->SourceFieldsKey.Reset();
	NA->MarkPackageDirty();
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(NewMesh); ARM.Get().AssetCreated(NA);
	}

	// 5) re-point the parent: new archetype, new mesh, the merged pivot as its position
	PA->Modify();
	PR->ArchetypeName = NewName;
	if (UStaticMeshComponent* PS = PA->FindComponentByClass<UStaticMeshComponent>()) { PS->SetStaticMesh(NewMesh); }
	PA->Tags.Remove(FName(TEXT("RUDE_PROXY")));
	PA->SetActorTransform(FTransform(FQuat::Identity, MergedLocation, FVector::OneVector));
	PA->MarkPackageDirty();
	// the scratch merge assets are not kept (the reduced copy is the deliverable)
	if (Merged) { Merged->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); Merged->MarkAsGarbage(); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"parent\":\"%s:%d\",\"level\":\"%s\",\"childrenMerged\":%d,\"childrenSkipped\":%d,\"childTriangles\":%d,\"mergedTriangles\":%d,\"chunkTriangles\":%d,")
		TEXT("\"newArchetype\":\"%s\",\"mesh\":\"%s\",\"atlas\":%d,\"textures\":[%s],\"lodDist\":%g,\"bsRadius\":%g,\"pivotUE\":[%.1f,%.1f,%.1f],\"targetYtyp\":\"%s\",")
		TEXT("\"note\":\"then: ExportYdrBinary(mesh, <name>.ydr, NOBOUND) + ExportMeshTextures(mesh, <name>.ytd, %d) + ExportPaletteYtyps + ExportLevelYmaps\"}"),
		*RudeJsonEscape(PR->SourceYmap), PR->SourceIndex, *PR->LodLevel, Comps.Num(), Skipped, ChildTris, MergedTris, TrisAfter,
		*RudeJsonEscape(NewName), *RudeJsonEscape(MeshPkgName), Atlas, *TexNames, NA->LodDist, NA->BsRadius, MergedLocation.X, MergedLocation.Y, MergedLocation.Z,
		*RudeJsonEscape(NA->SourceYtyp), Atlas);
}

// ---- RebakeLodLights (Wave 2 / WP9) --------------------------------------------------------
// The game's LOD lights are BAKED from entity light extensions, one per light instance, into a file
// pair: distlodlights_* (flags 2: DistantLODLightsSOA position + RGBI) parent of lodlights_* (flags 0,
// contentFlags 128: LODLightsSOA direction / falloff / falloffExponent / timeAndStateFlags / hash /
// coneInnerAngle / coneOuterAngleOrCapExt / coronaIntensity), index-aligned. Measured on downtown
// 2026-09-06 (829/832 entity lights have a LOD light at their exact world position; laws 32-33):
//   position = entity transform applied to posn (+ offsetPosition)      direction = the rotation applied
//   RGBI = round(intensity*255/50)<<24 | r<<16 | g<<8 | b                (byte 478/829, rgb 808/829)
//   timeAndStateFlags = timeFlags | (point ? 4 : 8)<<24                   (low 24 bits 768/829)
//   falloff / falloffExponent verbatim (710 / 751)   cone bytes = trunc(angle*127.5/90) capped 127
//   coronaIntensity 0 (the game's 0 or 7 is not derivable)   hash: UNKNOWN formula (34 candidates
//   refuted) -> unique atDataHash(guid, light index); the in-game test judges (law 33).
// Written as XML (FiveM loads XML ymaps - witnessed July) in the game's own spelling, ten values per
// line, without ROUT's carried MetaSchema (a fresh file has none).
static uint32 RudeDataHash(const uint8* B, int32 N, uint32 Seed = 0)
{
	uint32 H = Seed;
	for (int32 i = 0; i < N; ++i) { H += B[i]; H += (H << 10); H ^= (H >> 6); }
	H += (H << 3); H ^= (H >> 11); H += (H << 15);
	return H;
}
FString URudeToolset::RebakeLodLights(const FString& OutDir, const FString& Name, const FString& YmapFilter)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an OutDir")); }
	const FString Base = Name.TrimStartAndEnd().IsEmpty() ? FString(TEXT("rude")) : Name.TrimStartAndEnd().ToLower();
	TSet<FString> Wanted;
	{
		TArray<FString> Parts; YmapFilter.ParseIntoArray(Parts, TEXT(","), true);
		for (FString P : Parts) { P.TrimStartAndEndInline(); if (!P.IsEmpty()) { Wanted.Add(P.ToLower()); } }
	}
	struct FLod { FVector Pos; FVector Dir; uint32 Rgbi; uint32 Ts; uint32 Hash; float Falloff, FalloffExp, ConeIn, ConeOut; };
	TArray<FLod> Lights;
	int32 Entities = 0, EntitiesWithLights = 0, Malformed = 0;
	TSet<FString> Ymaps;
	auto Num = [](const FXmlNode* N, const TCHAR* Tag, double Def) -> double
	{
		const FXmlNode* C = N ? N->FindChildNode(Tag) : nullptr;
		return C ? FCString::Atod(*C->GetAttribute(TEXT("value"))) : Def;
	};
	auto Vec = [](const FXmlNode* N, const TCHAR* Tag, FVector& Out) -> bool
	{
		const FXmlNode* C = N ? N->FindChildNode(Tag) : nullptr;
		if (!C) { return false; }
		TArray<FString> T; C->GetContent().ParseIntoArrayWS(T);
		if (T.Num() < 3) { return false; }
		Out = FVector(FCString::Atod(*T[0]), FCString::Atod(*T[1]), FCString::Atod(*T[2]));
		return true;
	};
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>();
		if (!R) { continue; }
		++Entities;
		if (Wanted.Num() > 0 && !Wanted.Contains(R->SourceYmap.ToLower())) { continue; }
		if (!R->ExtensionsXml.Contains(TEXT("CExtensionDefLightEffect"))) { continue; }
		const FString Buffer = TEXT("<root>") + R->ExtensionsXml + TEXT("</root>");
		FXmlFile Doc(Buffer, EConstructMethod::ConstructFromBuffer);
		const FXmlNode* Root = Doc.IsValid() ? Doc.GetRootNode() : nullptr;
		if (!Root) { ++Malformed; continue; }
		const FXmlNode* Ext = Root->FindChildNode(TEXT("extensions"));
		if (!Ext) { Ext = Root; }
		const FTransform Xf = It->GetActorTransform();
		int32 Li = 0;
		bool bAny = false;
		for (const FXmlNode* Item : Ext->GetChildrenNodes())
		{
			if (Item->GetAttribute(TEXT("type")) != TEXT("CExtensionDefLightEffect")) { continue; }
			FVector Off = FVector::ZeroVector;
			if (const FXmlNode* O = Item->FindChildNode(TEXT("offsetPosition")))
			{
				Off = FVector(FCString::Atod(*O->GetAttribute(TEXT("x"))), FCString::Atod(*O->GetAttribute(TEXT("y"))), FCString::Atod(*O->GetAttribute(TEXT("z"))));
			}
			const FXmlNode* Inst = Item->FindChildNode(TEXT("instances"));
			if (!Inst) { continue; }
			for (const FXmlNode* L : Inst->GetChildrenNodes())
			{
				FVector Posn, Col, Dir(0, 0, -1);
				if (!Vec(L, TEXT("posn"), Posn) || !Vec(L, TEXT("colour"), Col)) { ++Malformed; continue; }
				Vec(L, TEXT("direction"), Dir);
				const FVector LocalRage = Posn + Off;
				const FVector LocalUe(LocalRage.X * 100.0, -LocalRage.Y * 100.0, LocalRage.Z * 100.0);
				const FVector WorldUe = Xf.TransformPosition(LocalUe);
				const FVector DirUe = Xf.TransformVectorNoScale(FVector(Dir.X, -Dir.Y, Dir.Z)).GetSafeNormal();
				FLod E;
				E.Pos = FVector(WorldUe.X / 100.0, -WorldUe.Y / 100.0, WorldUe.Z / 100.0);
				E.Dir = FVector(DirUe.X, -DirUe.Y, DirUe.Z);
				const double I = Num(L, TEXT("intensity"), 0.0);
				const uint32 IB = (uint32)FMath::Clamp(FMath::RoundToInt(I * 255.0 / 50.0), 0, 255);
				const uint32 r = (uint32)FMath::Clamp((int32)Col.X, 0, 255), g = (uint32)FMath::Clamp((int32)Col.Y, 0, 255), b = (uint32)FMath::Clamp((int32)Col.Z, 0, 255);
				E.Rgbi = (IB << 24) | (r << 16) | (g << 8) | b;
				const int32 Type = (int32)Num(L, TEXT("lightType"), 2.0);
				const uint32 Tf = (uint32)Num(L, TEXT("timeFlags"), 0xFFFFFF) & 0xFFFFFFu;
				E.Ts = Tf | ((Type == 1 ? 4u : 8u) << 24);
				E.Falloff = (float)Num(L, TEXT("falloff"), 0.0);
				E.FalloffExp = (float)Num(L, TEXT("falloffExponent"), 0.0);
				auto ConeByte = [](double Deg) { return (float)FMath::Min(127, (int32)(Deg * 127.5 / 90.0)); };
				E.ConeIn = Type == 4 ? 127.f : ConeByte(Num(L, TEXT("coneInnerAngle"), 0.0));
				E.ConeOut = Type == 4 ? 127.f : ConeByte(Num(L, TEXT("coneOuterAngle"), 0.0));
				struct { uint32 Guid; uint32 Li; } Key = { R->Guid, (uint32)Li };
				E.Hash = RudeDataHash((const uint8*)&Key, sizeof(Key));
				Lights.Add(E);
				++Li;
				bAny = true;
			}
		}
		if (bAny) { ++EntitiesWithLights; Ymaps.Add(R->SourceYmap.ToLower()); }
	}
	if (Lights.Num() == 0) { return Fail(FString::Printf(TEXT("no entity light extensions found (%d entities seen)"), Entities)); }
	FVector Mn(DBL_MAX), Mx(-DBL_MAX);
	for (const FLod& E : Lights) { Mn = Mn.ComponentMin(E.Pos); Mx = Mx.ComponentMax(E.Pos); }
	auto V3 = [](const TCHAR* Tag, const FVector& V) { return FString::Printf(TEXT(" <%s x=\"%s\" y=\"%s\" z=\"%s\" />\n"), Tag, *RudeNum(V.X), *RudeNum(V.Y), *RudeNum(V.Z)); };
	auto Header = [&](const FString& FileName, const FString& Parent, int32 Flags, int32 Content) -> FString
	{
		FString O = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapData>\n");
		O += TEXT(" <name>") + FileName + TEXT("</name>\n");
		O += Parent.IsEmpty() ? TEXT(" <parent />\n") : (TEXT(" <parent>") + Parent + TEXT("</parent>\n"));
		O += FString::Printf(TEXT(" <flags value=\"%d\" />\n <contentFlags value=\"%d\" />\n"), Flags, Content);
		O += V3(TEXT("streamingExtentsMin"), Mn - FVector(500.0)); O += V3(TEXT("streamingExtentsMax"), Mx + FVector(500.0));
		O += V3(TEXT("entitiesExtentsMin"), Mn); O += V3(TEXT("entitiesExtentsMax"), Mx);
		O += TEXT(" <entities />\n <containerLods itemType=\"rage__fwContainerLodDef\" />\n <boxOccluders itemType=\"BoxOccluder\" />\n <occludeModels itemType=\"OccludeModel\" />\n <physicsDictionaries />\n");
		O += TEXT(" <instancedData>\n  <ImapLink />\n  <PropInstanceList itemType=\"rage__fwPropInstanceListDef\" />\n  <GrassInstanceList itemType=\"rage__fwGrassInstanceListDef\" />\n </instancedData>\n");
		O += TEXT(" <timeCycleModifiers itemType=\"CTimeCycleModifier\" />\n <carGenerators itemType=\"CCarGen\" />\n");
		return O;
	};
	auto Rows = [&](const TCHAR* Tag, TFunctionRef<FString(const FLod&)> Cell) -> FString
	{
		FString O = FString::Printf(TEXT("  <%s>\n"), Tag);
		for (int32 i = 0; i < Lights.Num(); ++i)
		{
			if (i % 10 == 0) { O += TEXT("   "); } else { O += TEXT(" "); }
			O += Cell(Lights[i]);
			if (i % 10 == 9 || i == Lights.Num() - 1) { O += TEXT("\n"); }
		}
		O += FString::Printf(TEXT("  </%s>\n"), Tag);
		return O;
	};
	auto XyzItems = [&](const TCHAR* Tag, TFunctionRef<FVector(const FLod&)> Get) -> FString
	{
		FString O = FString::Printf(TEXT("  <%s itemType=\"FloatXYZ\">\n"), Tag);
		for (const FLod& E : Lights)
		{
			const FVector V = Get(E);
			O += FString::Printf(TEXT("   <Item>\n    <x value=\"%s\" />\n    <y value=\"%s\" />\n    <z value=\"%s\" />\n   </Item>\n"), *RudeNum(V.X), *RudeNum(V.Y), *RudeNum(V.Z));
		}
		O += FString::Printf(TEXT("  </%s>\n"), Tag);
		return O;
	};
	const FString Footer = TEXT(" <block>\n  <version value=\"0\" />\n  <flags value=\"0\" />\n  <name></name>\n  <exportedBy>RUDE</exportedBy>\n  <owner></owner>\n  <time></time>\n </block>\n</CMapData>\n");
	const FString DistName = Base + TEXT("_distlodlights"), LodName = Base + TEXT("_lodlights");
	// the parent: positions + RGBI
	FString Dist = Header(DistName, TEXT(""), 2, 256);
	Dist += TEXT(" <LODLightsSOA>\n  <direction itemType=\"FloatXYZ\" />\n  <falloff />\n  <falloffExponent />\n  <timeAndStateFlags />\n  <hash />\n  <coneInnerAngle />\n  <coneOuterAngleOrCapExt />\n  <coronaIntensity />\n </LODLightsSOA>\n");
	Dist += TEXT(" <DistantLODLightsSOA>\n");
	Dist += XyzItems(TEXT("position"), [](const FLod& E) { return E.Pos; });
	Dist += Rows(TEXT("RGBI"), [](const FLod& E) { return FString::Printf(TEXT("%u"), E.Rgbi); });
	Dist += TEXT("  <numStreetLights value=\"0\" />\n  <category value=\"0\" />\n </DistantLODLightsSOA>\n");
	Dist += Footer;
	// the child: the detail
	FString Lod = Header(LodName, DistName, 0, 128);
	Lod += TEXT(" <LODLightsSOA>\n");
	Lod += XyzItems(TEXT("direction"), [](const FLod& E) { return E.Dir; });
	Lod += Rows(TEXT("falloff"), [](const FLod& E) { return RudeNum(E.Falloff); });
	Lod += Rows(TEXT("falloffExponent"), [](const FLod& E) { return RudeNum(E.FalloffExp); });
	Lod += Rows(TEXT("timeAndStateFlags"), [](const FLod& E) { return FString::Printf(TEXT("%u"), E.Ts); });
	Lod += Rows(TEXT("hash"), [](const FLod& E) { return FString::Printf(TEXT("%u"), E.Hash); });
	Lod += Rows(TEXT("coneInnerAngle"), [](const FLod& E) { return RudeNum(E.ConeIn); });
	Lod += Rows(TEXT("coneOuterAngleOrCapExt"), [](const FLod& E) { return RudeNum(E.ConeOut); });
	Lod += Rows(TEXT("coronaIntensity"), [](const FLod& E) { return FString(TEXT("0")); });
	Lod += TEXT(" </LODLightsSOA>\n <DistantLODLightsSOA>\n  <position itemType=\"FloatXYZ\" />\n  <RGBI />\n  <numStreetLights value=\"0\" />\n  <category value=\"0\" />\n </DistantLODLightsSOA>\n");
	Lod += Footer;
	IFileManager::Get().MakeDirectory(*(OutDir / TEXT("stream")), true);
	const FString DistPath = OutDir / TEXT("stream") / (DistName + TEXT(".ymap"));
	const FString LodPath = OutDir / TEXT("stream") / (LodName + TEXT(".ymap"));
	if (!FFileHelper::SaveStringToFile(Dist, *DistPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return Fail(TEXT("cannot write ") + DistPath); }
	if (!FFileHelper::SaveStringToFile(Lod, *LodPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return Fail(TEXT("cannot write ") + LodPath); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"lights\":%d,\"entitiesWithLights\":%d,\"entitiesSeen\":%d,\"ymaps\":%d,\"malformed\":%d,\"files\":[\"%s\",\"%s\"],")
		TEXT("\"extentsM\":[%s,%s],\"note\":\"packings per ENGINEERING_LOG law 32; hash = unique atDataHash(guid, index), formula unknown (law 33); corona 0\"}"),
		Lights.Num(), EntitiesWithLights, Entities, Ymaps.Num(), Malformed, *RudeJsonEscape(DistPath), *RudeJsonEscape(LodPath),
		*RudeJsonEscape(FString::Printf(TEXT("%.1f,%.1f,%.1f"), Mn.X, Mn.Y, Mn.Z)), *RudeJsonEscape(FString::Printf(TEXT("%.1f,%.1f,%.1f"), Mx.X, Mx.Y, Mx.Z)));
}

// ---- SetLightField (agent) ---------------------------------------------------------------
// Edit one light of an entity from the CLI (Matt edits the component in Details). Field: intensity
// (RAGE units), colour "r,g,b", falloff (m), falloffExponent, coneInner, coneOuter, position "x,y,z"
// (RAGE metres, entity-local). The export writes the change into the entity's extension instance.
FString URudeToolset::SetLightField(const FString& ActorLabel, const FString& LightIndex, const FString& Field, const FString& Value)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* A = RudeFindActorByLabel(World, ActorLabel);
	if (!A) { return Fail(FString::Printf(TEXT("no actor labelled '%s'"), *ActorLabel)); }
	const int32 Idx = FCString::Atoi(*LightIndex);
	TArray<UPointLightComponent*> Comps;
	A->GetComponents<UPointLightComponent>(Comps);
	UPointLightComponent* LC = nullptr;
	for (UPointLightComponent* C : Comps) { if (C->ComponentTags.Contains(FName(*FString::Printf(TEXT("RUDE_LIGHT:%d"), Idx)))) { LC = C; break; } }
	if (!LC) { return Fail(FString::Printf(TEXT("the entity has no RUDE light %d (%d lights)"), Idx, Comps.Num())); }
	const FString F = Field.TrimStartAndEnd().ToLower();
	TArray<FString> P; Value.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	A->Modify(); LC->Modify();
	if (F == TEXT("intensity")) { LC->Intensity = (float)(FCString::Atod(*Value) * 100.0); }
	else if (F == TEXT("colour") || F == TEXT("color")) { if (P.Num() != 3) { return Fail(TEXT("colour needs r,g,b")); } LC->SetLightColor(FLinearColor(FCString::Atof(*P[0]) / 255.f, FCString::Atof(*P[1]) / 255.f, FCString::Atof(*P[2]) / 255.f)); }
	else if (F == TEXT("falloff")) { LC->AttenuationRadius = (float)(FCString::Atod(*Value) * 100.0); }
	else if (F == TEXT("falloffexponent")) { LC->LightFalloffExponent = (float)FMath::Clamp(FCString::Atod(*Value), 2.0, 16.0); }
	else if (F == TEXT("coneinner") || F == TEXT("coneouter"))
	{
		USpotLightComponent* Sp = Cast<USpotLightComponent>(LC);
		if (!Sp) { return Fail(TEXT("that light is not a spot light")); }
		if (F == TEXT("coneinner")) { Sp->InnerConeAngle = (float)FCString::Atod(*Value); } else { Sp->OuterConeAngle = (float)FCString::Atod(*Value); }
	}
	else if (F == TEXT("position")) { if (P.Num() != 3) { return Fail(TEXT("position needs x,y,z")); } LC->SetRelativeLocation(FVector(FCString::Atod(*P[0]) * 100.0, -FCString::Atod(*P[1]) * 100.0, FCString::Atod(*P[2]) * 100.0)); }
	else { return Fail(TEXT("unknown field (intensity, colour, falloff, falloffExponent, coneInner, coneOuter, position)")); }
	LC->MarkRenderStateDirty();
	A->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"actor\":\"%s\",\"light\":%d,\"field\":\"%s\",\"value\":\"%s\",\"intensityCd\":%g,\"attenuationCm\":%g}"),
		*RudeJsonEscape(A->GetActorLabel()), Idx, *RudeJsonEscape(F), *RudeJsonEscape(Value), LC->Intensity, LC->AttenuationRadius);
}


// ---- SetEntitySet (agent + Matt) ----------------------------------------------------------
// Activate / deactivate one of an interior's entity sets in the editor (the game's per-instance
// defaultEntitySets, ActivateInteriorEntitySet): shows or hides the actor ImportMlo spawned for it.
FString URudeToolset::SetEntitySet(const FString& InteriorName, const FString& SetName, const FString& Visible)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FName IdTag(*(TEXT("RUDE_MLO:") + InteriorName.TrimStartAndEnd()));
	const FString SetLower = SetName.TrimStartAndEnd().ToLower();
	const bool bShow = Visible.TrimStartAndEnd().Equals(TEXT("true"), ESearchCase::IgnoreCase) || Visible.TrimStartAndEnd() == TEXT("1");
	int32 Touched = 0, Instances = 0;
	TArray<FString> Known;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(IdTag)) { continue; }
		FString ThisSet;
		for (const FName& T : It->Tags) { const FString S = T.ToString(); if (S.StartsWith(TEXT("RUDE_MLO_EntitySet:"))) { ThisSet = S.Mid(19); } }
		if (ThisSet.IsEmpty()) { continue; }
		Known.AddUnique(ThisSet);
		if (ThisSet.ToLower() != SetLower) { continue; }
		It->Modify();
		It->SetActorHiddenInGame(!bShow);
		TArray<UInstancedStaticMeshComponent*> Isms;
		It->GetComponents<UInstancedStaticMeshComponent>(Isms);
		for (UInstancedStaticMeshComponent* C : Isms) { C->SetVisibility(bShow, true); Instances += C->GetInstanceCount(); }
		// since the MLO export lane (2026-09-06) a set's entities are child ACTORS under the set actor, not ISM instances
		TArray<AActor*> Kids;
		It->GetAttachedActors(Kids, true, true);
		for (AActor* K : Kids)
		{
			K->Modify();
			K->SetActorHiddenInGame(!bShow);
			TArray<UPrimitiveComponent*> Prims;
			K->GetComponents<UPrimitiveComponent>(Prims);
			for (UPrimitiveComponent* Pc : Prims) { Pc->SetVisibility(bShow, true); }
			if (K->FindComponentByClass<UStaticMeshComponent>()) { ++Instances; }
		}
		It->MarkPackageDirty();
		++Touched;
	}
	if (Touched == 0) { return Fail(FString::Printf(TEXT("no entity set '%s' on interior '%s' (sets present: %s)"), *SetName, *InteriorName, *FString::Join(Known, TEXT(", ")))); }
	return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"set\":\"%s\",\"visible\":%s,\"actors\":%d,\"instances\":%d,\"setsPresent\":%d}"),
		*RudeJsonEscape(InteriorName), *RudeJsonEscape(SetName), bShow ? TEXT("true") : TEXT("false"), Touched, Instances, Known.Num());
}

// ---- ImportCarGenerators (agent + Matt) ---------------------------------------------------
// The ymap's <carGenerators> as slab markers: length = |orient| (the vehicle's length, 3.4-13 m
// measured), width = perpendicularLength, facing the orient vector; a URudeCarGenComponent carries the
// fields and the source slice. YmapFilter = comma list of ymap names ("ALL" = every ymap with entity
// actors in the level).
FString URudeToolset::ImportCarGenerators(const FString& CorpusRoot, const FString& YmapFilter)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	TSet<FString> Ymaps;
	if (YmapFilter.TrimStartAndEnd().Equals(TEXT("ALL"), ESearchCase::IgnoreCase))
	{
		for (TActorIterator<AActor> It(World); It; ++It) { if (const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>()) { if (!R->SourceYmap.IsEmpty()) { Ymaps.Add(R->SourceYmap.ToLower()); } } }
	}
	else
	{
		TArray<FString> Parts; YmapFilter.ParseIntoArray(Parts, TEXT(","), true);
		for (FString P : Parts) { P.TrimStartAndEndInline(); if (!P.IsEmpty()) { Ymaps.Add(P.ToLower()); } }
	}
	if (Ymaps.Num() == 0) { return Fail(TEXT("give ymap names, or ALL with a level that has RUDE entities")); }
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	int32 Spawned = 0, Cleared = 0, YmapsRead = 0, Malformed = 0;
	FString Files;
	for (const FString& Y : Ymaps)
	{
		const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("ymap"), Y);
		if (!Row) { continue; }
		const FString Path = Corpus->PathOf(*Row);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) { continue; }
		FXmlFile Doc(Path);
		const FXmlNode* Root = (Doc.IsValid() && Doc.GetRootNode()) ? Doc.GetRootNode() : nullptr;
		const FXmlNode* CG = Root ? Root->FindChildNode(TEXT("carGenerators")) : nullptr;
		++YmapsRead;
		if (!CG || CG->GetChildrenNodes().Num() == 0) { continue; }
		TArray<FString> Raw;
		RudeRawItems(Text, TEXT("carGenerators"), Raw);
		// a rebuild replaces this ymap's markers
		const FName YmapTag(*(TEXT("RUDE_CARGEN:") + Y));
		{
			TArray<AActor*> Old;
			for (TActorIterator<AActor> It(World); It; ++It) { if (It->Tags.Contains(YmapTag)) { Old.Add(*It); } }
			for (AActor* O : Old) { World->DestroyActor(O); ++Cleared; }
		}
		int32 Ordinal = -1;
		for (const FXmlNode* Item : CG->GetChildrenNodes())
		{
			++Ordinal;
			const FXmlNode* Pos = Item->FindChildNode(TEXT("position"));
			auto Num = [&](const TCHAR* Tag, double Def) { const FXmlNode* N = Item->FindChildNode(Tag); return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Def; };
			auto Txt = [&](const TCHAR* Tag) { const FXmlNode* N = Item->FindChildNode(Tag); return N ? N->GetContent().TrimStartAndEnd() : FString(); };
			if (!Pos) { ++Malformed; continue; }
			const double Px = FCString::Atod(*Pos->GetAttribute(TEXT("x"))), Py = FCString::Atod(*Pos->GetAttribute(TEXT("y"))), Pz = FCString::Atod(*Pos->GetAttribute(TEXT("z")));
			const double Ox = Num(TEXT("orientX"), 1.0), Oy = Num(TEXT("orientY"), 0.0);
			const double Len = FMath::Max(0.1, FMath::Sqrt(Ox * Ox + Oy * Oy));
			const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(-Oy, Ox));   // RAGE (x,y) -> UE (x,-y)
			const FTransform Xf(FRotator(0.0, Yaw, 0.0), FVector(Px * 100.0, -Py * 100.0, Pz * 100.0), FVector::OneVector);
			AActor* A = World->SpawnActor<AActor>();
			if (!A) { continue; }
			UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(A, TEXT("Marker"));
			SMC->SetStaticMesh(Cube);
			SMC->SetMobility(EComponentMobility::Movable);
			SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			A->SetRootComponent(SMC);
			SMC->RegisterComponent();
			A->AddInstanceComponent(SMC);
			A->SetActorTransform(Xf);
			const double Perp = FMath::Max(0.1, Num(TEXT("perpendicularLength"), 3.0));
			SMC->SetRelativeScale3D(FVector(Len, Perp, 0.3));   // the cube is 1 m: scale = metres
			URudeCarGenComponent* C = NewObject<URudeCarGenComponent>(A, TEXT("RudeCarGen"));
			C->Length = (float)Len; C->PerpendicularLength = (float)Perp;
			C->CarModel = Txt(TEXT("carModel")); C->Flags = (uint32)Num(TEXT("flags"), 0.0);
			C->BodyColorRemap1 = (int32)Num(TEXT("bodyColorRemap1"), -1.0); C->BodyColorRemap2 = (int32)Num(TEXT("bodyColorRemap2"), -1.0);
			C->BodyColorRemap3 = (int32)Num(TEXT("bodyColorRemap3"), -1.0); C->BodyColorRemap4 = (int32)Num(TEXT("bodyColorRemap4"), -1.0);
			C->PopGroup = Txt(TEXT("popGroup")); C->Livery = (int32)Num(TEXT("livery"), -1.0);
			C->SourceYmap = Y; C->SourceIndex = Ordinal;
			C->SourceXml = Raw.IsValidIndex(Ordinal) ? Raw[Ordinal] : FString();
			C->SourceTransform = Xf;
			{
				const FXmlNode* OX = Item->FindChildNode(TEXT("orientX")); const FXmlNode* OY = Item->FindChildNode(TEXT("orientY"));
				C->SourceOrientXText = OX ? OX->GetAttribute(TEXT("value")) : FString(); C->SourceOrientYText = OY ? OY->GetAttribute(TEXT("value")) : FString();
			}
			C->SourceFieldsKey = C->FieldsKey();
			C->RegisterComponent();
			A->AddInstanceComponent(C);
			A->Tags.Add(FName(TEXT("RUDE_CARGEN")));
			A->Tags.Add(YmapTag);
			A->SetActorLabel(FString::Printf(TEXT("cargen_%s_%d"), *Y, Ordinal));
			A->SetFolderPath(FName(*(TEXT("RUDE_CARGENS/") + Y)));
			++Spawned;
		}
		Files += FString::Printf(TEXT("%s\"%s:%d\""), Files.IsEmpty() ? TEXT("") : TEXT(","), *Y, Ordinal + 1);
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"ymapsRead\":%d,\"spawned\":%d,\"cleared\":%d,\"malformed\":%d,\"perYmap\":[%s]}"),
		Spawned > 0 ? TEXT("true") : TEXT("false"), YmapsRead, Spawned, Cleared, Malformed, *Files);
}

// ---- MoveCarGenerator (agent) ------------------------------------------------------------
FString URudeToolset::MoveCarGenerator(const FString& YmapName, const FString& Index, const FString& DeltaCm)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const int32 I = FCString::Atoi(*Index);
	TArray<FString> P; DeltaCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	if (P.Num() != 3) { return Fail(TEXT("DeltaCm must be x,y,z")); }
	const FVector D(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]));
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		URudeCarGenComponent* C = It->FindComponentByClass<URudeCarGenComponent>();
		if (!C || C->SourceIndex != I || !C->SourceYmap.Equals(YmapName.TrimStartAndEnd(), ESearchCase::IgnoreCase)) { continue; }
		It->Modify();
		const FVector Before = It->GetActorLocation();
		It->SetActorLocation(Before + D);
		It->MarkPackageDirty();
		return FString::Printf(TEXT("{\"ok\":true,\"ymap\":\"%s\",\"index\":%d,\"before\":[%.2f,%.2f,%.2f],\"after\":[%.2f,%.2f,%.2f]}"), *RudeJsonEscape(C->SourceYmap), I, Before.X, Before.Y, Before.Z, Before.X + D.X, Before.Y + D.Y, Before.Z + D.Z);
	}
	return Fail(FString::Printf(TEXT("no car generator %s:%d in the level (ImportCarGenerators first)"), *YmapName, I));
}

// ---- PickAt (agent) -----------------------------------------------------------------------
// What is under a pixel of a CaptureView frame? CamSpec = "x,y,z,pitch,yaw" (';' accepted) as
// CaptureView; U,V = 0..1 across the frame (aspect = the capture's, default 2103x1230, HFOV 90).
// Traces every hit along the ray and reports the first VISIBLE one plus what it passed through.
FString URudeToolset::PickAt(const FString& CamSpec, const FString& U, const FString& V, const FString& Aspect)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	TArray<FString> P;
	CamSpec.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	if (P.Num() != 5) { return Fail(TEXT("CamSpec must be x,y,z,pitch,yaw")); }
	const FVector Cam(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]));
	const FRotator Rot(FCString::Atod(*P[3]), FCString::Atod(*P[4]), 0.0);
	const double u = FCString::Atod(*U), v = FCString::Atod(*V);
	const double A = Aspect.TrimStartAndEnd().IsEmpty() ? (2103.0 / 1230.0) : FCString::Atod(*Aspect);
	const double TanH = FMath::Tan(FMath::DegreesToRadians(45.0));   // HFOV 90
	const FVector Local(1.0, TanH * (2.0 * u - 1.0), -(TanH / A) * (2.0 * v - 1.0));
	const FVector Dir = Rot.RotateVector(Local.GetSafeNormal());
	// No physics needed (a commandlet's World Partition actors may carry no collision state): a
	// ray-vs-bounds test over every placed entity, nearest visible first.
	struct FPick { double Dist; AActor* Actor; const UStaticMeshComponent* SMC; bool bVisible; };
	TArray<FPick> Picks;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const UStaticMeshComponent* SMC = It->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC || !SMC->GetStaticMesh()) { continue; }
		const FBox Box = SMC->Bounds.GetBox();
		if (!Box.IsValid) { continue; }
		FVector HitLoc, HitNormal; float HitTime;
		if (!FMath::LineExtentBoxIntersection(Box, Cam, Cam + Dir * 200000.0, FVector::ZeroVector, HitLoc, HitNormal, HitTime)) { continue; }
		FPick Pk; Pk.Dist = FVector::Dist(Cam, HitLoc); Pk.Actor = *It; Pk.SMC = SMC;
		Pk.bVisible = SMC->IsVisible() && !SMC->bHiddenInGame;
		Picks.Add(Pk);
	}
	Picks.Sort([](const FPick& A, const FPick& B) { return A.Dist < B.Dist; });
	FString Rows;
	int32 N = 0;
	FString FirstVisible;
	for (const FPick& Pk : Picks)
	{
		AActor* Actor = Pk.Actor;
		const URudeEntityComponent* R = Actor->FindComponentByClass<URudeEntityComponent>();
		FString Lod, Mats;
		for (const FName& T : Actor->Tags) { const FString S = T.ToString(); if (S.StartsWith(TEXT("RUDE_LOD:"))) { Lod = S.Mid(9 + 15); } }
		for (int32 i = 0; i < Pk.SMC->GetNumMaterials() && i < 6; ++i)
		{
			UMaterialInterface* M = Pk.SMC->GetMaterial(i);
			UMaterialInterface* Parent = M;
			if (UMaterialInstance* MI = Cast<UMaterialInstance>(M)) { Parent = MI->Parent; }
			Mats += FString::Printf(TEXT("%s%s"), Mats.IsEmpty() ? TEXT("") : TEXT("|"), Parent ? *Parent->GetName() : TEXT("none"));
		}
		const FBox Box = Pk.SMC->Bounds.GetBox();
		const FString Row = FString::Printf(TEXT("{\"actor\":\"%s\",\"archetype\":\"%s\",\"ymap\":\"%s\",\"lod\":\"%s\",\"itemType\":\"%s\",\"visible\":%s,\"distanceM\":%.1f,\"boundsM\":\"%.0fx%.0fx%.0f\",\"mesh\":\"%s\",\"masters\":\"%s\"}"),
			*RudeJsonEscape(Actor->GetActorLabel()), R ? *RudeJsonEscape(R->ArchetypeName) : TEXT(""), R ? *RudeJsonEscape(R->SourceYmap) : TEXT(""),
			*Lod, R ? *R->ItemType : TEXT(""), Pk.bVisible ? TEXT("true") : TEXT("false"), Pk.Dist / 100.0,
			Box.GetSize().X / 100.0, Box.GetSize().Y / 100.0, Box.GetSize().Z / 100.0,
			*RudeJsonEscape(Pk.SMC->GetStaticMesh()->GetName()), *RudeJsonEscape(Mats));
		if (Pk.bVisible && FirstVisible.IsEmpty()) { FirstVisible = Row; }
		if (N++ < 8) { Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(",")) + Row; }
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"u\":%g,\"v\":%g,\"hits\":%d,\"firstVisible\":%s,\"alongRay\":[%s]}"),
		Picks.Num() > 0 ? TEXT("true") : TEXT("false"), u, v, Picks.Num(), FirstVisible.IsEmpty() ? TEXT("null") : *FirstVisible, *Rows);
}

// ---- InspectMesh (agent) --------------------------------------------------------------------
// The numbers behind an imported mesh: render bounds, vertex/triangle counts, the ACTUAL vertex
// extents of LOD0 (from the mesh description), collision primitive counts + the farthest primitive.
FString URudeToolset::InspectMesh(const FString& AssetPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	const FString Path = AssetPath.TrimStartAndEnd();
	const FString Obj = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Obj);
	if (!Mesh) { return Fail(FString::Printf(TEXT("no static mesh at %s"), *Obj)); }
	const FBox B = Mesh->GetBoundingBox();
	FVector VMin(DBL_MAX), VMax(-DBL_MAX);
	int32 Verts = 0, Tris = 0;
	if (const FMeshDescription* MD = Mesh->GetMeshDescription(0))
	{
		FStaticMeshConstAttributes Attr(*MD);
		TVertexAttributesConstRef<FVector3f> Pos = Attr.GetVertexPositions();
		for (const FVertexID VID : MD->Vertices().GetElementIDs())
		{
			const FVector P(Pos[VID]);
			VMin = VMin.ComponentMin(P); VMax = VMax.ComponentMax(P); ++Verts;
		}
		Tris = MD->Triangles().Num();
	}
	int32 Spheres = 0, Boxes = 0, Capsules = 0, Convex = 0;
	double FarthestPrimM = 0;
	if (UBodySetup* BS = Mesh->GetBodySetup())
	{
		Spheres = BS->AggGeom.SphereElems.Num(); Boxes = BS->AggGeom.BoxElems.Num();
		Capsules = BS->AggGeom.SphylElems.Num(); Convex = BS->AggGeom.ConvexElems.Num();
		for (const FKSphereElem& E : BS->AggGeom.SphereElems) { FarthestPrimM = FMath::Max(FarthestPrimM, E.Center.Size() / 100.0); }
		for (const FKBoxElem& E : BS->AggGeom.BoxElems) { FarthestPrimM = FMath::Max(FarthestPrimM, E.Center.Size() / 100.0); }
		for (const FKSphylElem& E : BS->AggGeom.SphylElems) { FarthestPrimM = FMath::Max(FarthestPrimM, E.Center.Size() / 100.0); }
		for (const FKConvexElem& E : BS->AggGeom.ConvexElems) { FarthestPrimM = FMath::Max(FarthestPrimM, E.GetTransform().GetLocation().Size() / 100.0); }
	}
	// materials per slot: the instance and what its Diffuse resolved to (2026-09-06: the instrument that
	// names WHICH geometry of a checkered mesh lost its texture - a master shows the checker for an unbound
	// diffuse, and the district's bind report only counts misses)
	FString MatsJson;
	for (const FStaticMaterial& SM : Mesh->GetStaticMaterials())
	{
		FString Diffuse = TEXT("unbound"), MatPath = TEXT("null");
		if (SM.MaterialInterface)
		{
			MatPath = SM.MaterialInterface->GetPathName();
			if (const UMaterialInstance* MI = Cast<UMaterialInstance>(SM.MaterialInterface))
			{
				UTexture* T = nullptr;
				if (MI->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Diffuse")), T) && T && !T->GetPathName().StartsWith(TEXT("/Engine/"))) { Diffuse = T->GetName(); }
			}
		}
		MatsJson += FString::Printf(TEXT("%s{\"slot\":\"%s\",\"material\":\"%s\",\"diffuse\":\"%s\"}"), MatsJson.IsEmpty() ? TEXT("") : TEXT(","),
			*RudeJsonEscape(SM.MaterialSlotName.ToString()), *RudeJsonEscape(MatPath), *RudeJsonEscape(Diffuse));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"mesh\":\"%s\",\"renderBoundsM\":\"%.1fx%.1fx%.1f\",\"boundsCenterM\":\"%.1f,%.1f,%.1f\",")
		TEXT("\"lod0Verts\":%d,\"lod0Tris\":%d,\"vertexMinM\":\"%.1f,%.1f,%.1f\",\"vertexMaxM\":\"%.1f,%.1f,%.1f\",")
		TEXT("\"collision\":{\"spheres\":%d,\"boxes\":%d,\"capsules\":%d,\"convex\":%d,\"farthestPrimM\":%.1f},\"positiveBoundsExtM\":\"%.1f,%.1f,%.1f\",\"materials\":[%s]}"),
		*RudeJsonEscape(Mesh->GetName()), B.GetSize().X / 100.0, B.GetSize().Y / 100.0, B.GetSize().Z / 100.0,
		B.GetCenter().X / 100.0, B.GetCenter().Y / 100.0, B.GetCenter().Z / 100.0,
		Verts, Tris, VMin.X / 100.0, VMin.Y / 100.0, VMin.Z / 100.0, VMax.X / 100.0, VMax.Y / 100.0, VMax.Z / 100.0,
		Spheres, Boxes, Capsules, Convex, FarthestPrimM,
		Mesh->GetPositiveBoundsExtension().X / 100.0, Mesh->GetPositiveBoundsExtension().Y / 100.0, Mesh->GetPositiveBoundsExtension().Z / 100.0, *MatsJson);
}

// ---- XmlShapeRoundTrip -------------------------------------------------------------------
// Walk a parsed tree into (path -> [attr=value...] + leaf text) rows, order-preserving by path
// with sibling ordinals, so two parses compare exactly and a mismatch names its path.
static void RudeXmlShapeRows(const FXmlNode* N, const FString& Path, TArray<FString>& Rows)
{
	if (!N) { return; }
	FString Row = Path;
	for (const FXmlAttribute& A : N->GetAttributes())
	{
		Row += TEXT(" @"); Row += A.GetTag(); Row += TEXT("="); Row += A.GetValue();
	}
	const TArray<FXmlNode*>& Kids = N->GetChildrenNodes();
	if (Kids.Num() == 0)
	{
		Row += TEXT(" #"); Row += N->GetContent().TrimStartAndEnd();
	}
	Rows.Add(Row);
	TMap<FString, int32> Ordinal;
	for (const FXmlNode* K : Kids)
	{
		const int32 I = Ordinal.FindOrAdd(K->GetTag())++;
		RudeXmlShapeRows(K, FString::Printf(TEXT("%s/%s[%d]"), *Path, *K->GetTag(), I), Rows);
	}
}

FString URudeToolset::XmlShapeRoundTrip(const FString& ListPath, const FString& OutDir)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Files;
	if (ListPath.EndsWith(TEXT(".xml"), ESearchCase::IgnoreCase)) { Files.Add(ListPath); }
	else
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *ListPath)) { return Fail(TEXT("cannot read ListPath")); }
		Raw.ParseIntoArrayLines(Files);
	}
	int32 Identical = 0, Differing = 0, Unreadable = 0;
	int64 Elements = 0, Attributes = 0;
	FString Mismatches;
	int32 MismatchCount = 0;
	for (FString F : Files)
	{
		F.TrimStartAndEndInline();
		if (F.IsEmpty()) { continue; }
		FXmlFile A(F);
		if (!A.IsValid() || !A.GetRootNode()) { ++Unreadable; continue; }
		FString Text = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
		RudeXmlNodeToString(A.GetRootNode(), Text, 0);
		FString OutPath;
		if (!OutDir.TrimStartAndEnd().IsEmpty())
		{
			OutPath = OutDir / FPaths::GetCleanFilename(F);
			FFileHelper::SaveStringToFile(Text, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		FXmlFile B(Text, EConstructMethod::ConstructFromBuffer);
		if (!B.IsValid() || !B.GetRootNode())
		{
			++Differing;
			if (MismatchCount++ < 20) { Mismatches += FString::Printf(TEXT("%s\"%s: re-parse failed\""), Mismatches.IsEmpty() ? TEXT("") : TEXT(","), *F); }
			continue;
		}
		TArray<FString> RowsA, RowsB;
		RudeXmlShapeRows(A.GetRootNode(), A.GetRootNode()->GetTag(), RowsA);
		RudeXmlShapeRows(B.GetRootNode(), B.GetRootNode()->GetTag(), RowsB);
		Elements += RowsA.Num();
		for (const FString& R : RowsA) { for (TCHAR C : R) { if (C == TEXT('@')) { ++Attributes; } } }
		bool bSame = RowsA.Num() == RowsB.Num();
		FString FirstDiff;
		for (int32 i = 0; bSame && i < RowsA.Num(); ++i)
		{
			if (RowsA[i] != RowsB[i]) { bSame = false; FirstDiff = RowsA[i].Left(120) + TEXT(" != ") + RowsB[i].Left(120); }
		}
		if (bSame) { ++Identical; }
		else
		{
			++Differing;
			if (MismatchCount++ < 20)
			{
				Mismatches += FString::Printf(TEXT("%s\"%s: rows %d vs %d%s%s\""), Mismatches.IsEmpty() ? TEXT("") : TEXT(","),
					*FPaths::GetCleanFilename(F), RowsA.Num(), RowsB.Num(), FirstDiff.IsEmpty() ? TEXT("") : TEXT("; first "), *RudeJsonEscape(FirstDiff));
			}
		}
	}
	const bool bOk = (Differing == 0) && (Unreadable == 0) && (Identical > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"files\":%d,\"identical\":%d,\"differing\":%d,\"unreadable\":%d,")
		TEXT("\"elements\":%lld,\"attributes\":%lld,\"mismatches\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), Files.Num(), Identical, Differing, Unreadable,
		Elements, Attributes, *Mismatches);
}

