// RUDE - RAGE <-> Unreal Development Environment
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

// Build /RUDE/Masters/M_RUDE_Terrain on demand if absent: the GTA terrain_cb_* family
// blends 4 diffuse+normal layers by VERTEX COLOUR (Colour0) - base = layer0, then lerp
// to layer1/2/3 by VC.R/G/B. Texture params Diffuse0..3 / Normal0..3 match ImportYdr's
// terrain binding. (v1 approximation of the RAGE blend - acceptance test is Matt's
// "patchwork gone" report; the 2tex_blend lookup variants refine later.)
// RAGE decal geometry is COPLANAR with the surface it sits on; imported as ordinary
// opaque meshes it z-fights (the black striping across the Cayo runway, 2026-07-25).
// Fix WITHOUT touching geometry (export round-trip stays byte-exact): a masked
// material that pushes the pixels off the surface via World Position Offset along the
// vertex normal. 2cm is invisible at map scale and clears the depth tie.
static UMaterialInterface* EnsureDecalGeoMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_DecalGeo.M_RUDE_DecalGeo");
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, FullPath))
	{
		return Existing;
	}
	UPackage* Pkg = CreatePackage(TEXT("/RUDE/Masters/M_RUDE_DecalGeo"));
	if (!Pkg) { return nullptr; }
	UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_RUDE_DecalGeo"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Masked;
	M->TwoSided = true;
	UTexture* DefWhite = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* DefNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));

	auto* Diff = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Diff->ParameterName = TEXT("Diffuse"); Diff->SamplerType = SAMPLERTYPE_Color; Diff->Texture = DefWhite;
	Diff->MaterialExpressionEditorX = -600;
	M->GetExpressionCollection().AddExpression(Diff);
	auto* Nrm = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Nrm->ParameterName = TEXT("Normal"); Nrm->SamplerType = SAMPLERTYPE_Normal; Nrm->Texture = DefNormal;
	Nrm->MaterialExpressionEditorX = -600; Nrm->MaterialExpressionEditorY = 300;
	M->GetExpressionCollection().AddExpression(Nrm);
	auto* VN = NewObject<UMaterialExpressionVertexNormalWS>(M);
	VN->MaterialExpressionEditorX = -600; VN->MaterialExpressionEditorY = 600;
	M->GetExpressionCollection().AddExpression(VN);
	auto* Off = NewObject<UMaterialExpressionConstant>(M); Off->R = 2.0f;   // cm along the normal
	M->GetExpressionCollection().AddExpression(Off);
	auto* WPO = NewObject<UMaterialExpressionMultiply>(M);
	WPO->A.Expression = VN; WPO->B.Expression = Off;
	WPO->MaterialExpressionEditorX = -300; WPO->MaterialExpressionEditorY = 600;
	M->GetExpressionCollection().AddExpression(WPO);

	// Opacity = Diffuse.A * Visible. ImportYdr sets Visible=0 when the decal's texture
	// isn't in the corpus, so an unresolved decal DISAPPEARS instead of painting an
	// opaque white slab across the beach (2026-07-25 regression, Matt-spotted).
	auto* Vis = NewObject<UMaterialExpressionScalarParameter>(M);
	Vis->ParameterName = TEXT("Visible"); Vis->DefaultValue = 1.f;
	M->GetExpressionCollection().AddExpression(Vis);
	auto* AlphaSrc = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	AlphaSrc->ParameterName = TEXT("Diffuse"); AlphaSrc->SamplerType = SAMPLERTYPE_Color; AlphaSrc->Texture = DefWhite;
	M->GetExpressionCollection().AddExpression(AlphaSrc);
	auto* OpMul = NewObject<UMaterialExpressionMultiply>(M);
	OpMul->A.Expression = AlphaSrc; OpMul->A.MaskA = 1; OpMul->A.Mask = 1;
	OpMul->A.MaskR = 0; OpMul->A.MaskG = 0; OpMul->A.MaskB = 0;
	OpMul->B.Expression = Vis;
	M->GetExpressionCollection().AddExpression(OpMul);

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = Diff;
	EO->Normal.Expression = Nrm;
	EO->WorldPositionOffset.Expression = WPO;
	EO->OpacityMask.Expression = OpMul;
	M->PostEditChange();
	Pkg->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	return M;
}

// RAGE foliage is alpha-tested AND double-sided (leaf cards are single quads). Imported
// single-sided, every leaf facing away from the light renders near-black (the dark trees
// on Cayo, 2026-07-25). Two-sided + masked is the correct foliage material.
static UMaterialInterface* EnsureFoliageMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_Foliage.M_RUDE_Foliage");
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, FullPath))
	{
		return Existing;
	}
	UPackage* Pkg = CreatePackage(TEXT("/RUDE/Masters/M_RUDE_Foliage"));
	if (!Pkg) { return nullptr; }
	UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_RUDE_Foliage"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Masked;
	M->TwoSided = true;
	M->SetShadingModel(MSM_TwoSidedFoliage);   // light transmits through leaf cards
	UTexture* DefWhite = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* DefNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));

	auto* Diff = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Diff->ParameterName = TEXT("Diffuse"); Diff->SamplerType = SAMPLERTYPE_Color; Diff->Texture = DefWhite;
	Diff->MaterialExpressionEditorX = -600;
	M->GetExpressionCollection().AddExpression(Diff);
	auto* Nrm = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Nrm->ParameterName = TEXT("Normal"); Nrm->SamplerType = SAMPLERTYPE_Normal; Nrm->Texture = DefNormal;
	Nrm->MaterialExpressionEditorX = -600; Nrm->MaterialExpressionEditorY = 300;
	M->GetExpressionCollection().AddExpression(Nrm);
	auto* Sub = NewObject<UMaterialExpressionConstant>(M); Sub->R = 0.35f;   // subsurface strength
	M->GetExpressionCollection().AddExpression(Sub);
	auto* SubCol = NewObject<UMaterialExpressionMultiply>(M);
	SubCol->A.Expression = Diff; SubCol->B.Expression = Sub;
	M->GetExpressionCollection().AddExpression(SubCol);

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = Diff;
	EO->Normal.Expression = Nrm;
	EO->SubsurfaceColor.Expression = SubCol;
	EO->OpacityMask.Expression = Diff;
	EO->OpacityMask.MaskA = 1; EO->OpacityMask.Mask = 1;
	EO->OpacityMask.MaskR = 0; EO->OpacityMask.MaskG = 0; EO->OpacityMask.MaskB = 0;
	M->PostEditChange();
	Pkg->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	return M;
}

// ⛔⛔ THE CUTOUT MASTER, BUILT IN CODE - and it must be REBUILT if an older, poorer version is
// already on disk. Why this exists (2026-07-28): render buckets used to be hardcoded 0 by the
// converter, so almost everything routed to M_RUDE_Opaque. With REAL buckets, buckets 1 and 3
// route here - measured at ~11.8% of downtown, up from 0.9% - and the M_RUDE_Cutout asset that
// shipped has only Diffuse and Roughness. Binding Normal/Specular onto it SUCCEEDS SILENTLY
// (UMaterialInstance does not validate against the parent) and renders nothing, so ~900 downtown
// instances would have quietly LOST normal-mapping they had the day before, while the
// "boundTextures" counter went up. The upgrade check below is therefore not optional: an existing
// asset without a Normal parameter is a defect to repair, not a master to reuse.
static UMaterialInterface* EnsureCutoutMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_Cutout.M_RUDE_Cutout");
	UMaterial* M = LoadObject<UMaterial>(nullptr, FullPath);
	if (M)
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		M->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& I : Infos)
		{
			if (I.Name == FName(TEXT("Normal"))) { return M; }   // already the good version
		}
		// Poorer legacy asset: wipe its graph and rebuild, rather than bolting expressions onto
		// an unknown one. This is OUR asset and fully regenerable.
		M->GetExpressionCollection().Empty();
	}
	else
	{
		UPackage* Pkg = CreatePackage(TEXT("/RUDE/Masters/M_RUDE_Cutout"));
		if (!Pkg) { return nullptr; }
		M = NewObject<UMaterial>(Pkg, TEXT("M_RUDE_Cutout"), RF_Public | RF_Standalone);
	}

	M->BlendMode = BLEND_Masked;
	M->TwoSided = true;    // RAGE cutout geometry (fences, foliage cards, grilles) is single-sided
	UTexture* DefWhite = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* DefNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));

	auto* Diff = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Diff->ParameterName = TEXT("Diffuse"); Diff->SamplerType = SAMPLERTYPE_Color; Diff->Texture = DefWhite;
	Diff->MaterialExpressionEditorX = -600;
	M->GetExpressionCollection().AddExpression(Diff);

	auto* Nrm = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Nrm->ParameterName = TEXT("Normal"); Nrm->SamplerType = SAMPLERTYPE_Normal; Nrm->Texture = DefNormal;
	Nrm->MaterialExpressionEditorX = -600; Nrm->MaterialExpressionEditorY = 300;
	M->GetExpressionCollection().AddExpression(Nrm);

	auto* Spec = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Spec->ParameterName = TEXT("Specular"); Spec->SamplerType = SAMPLERTYPE_Color; Spec->Texture = DefWhite;
	Spec->MaterialExpressionEditorX = -600; Spec->MaterialExpressionEditorY = 600;
	M->GetExpressionCollection().AddExpression(Spec);

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = Diff;
	EO->Normal.Expression = Nrm;
	// RAGE spec maps are colour maps; take one channel for UE's scalar Specular input.
	EO->Specular.Expression = Spec;
	EO->Specular.MaskR = 1; EO->Specular.Mask = 1;
	EO->Specular.MaskG = 0; EO->Specular.MaskB = 0; EO->Specular.MaskA = 0;
	// The cutout IS the diffuse alpha - the same wiring EnsureFoliageMaster uses.
	EO->OpacityMask.Expression = Diff;
	EO->OpacityMask.MaskA = 1; EO->OpacityMask.Mask = 1;
	EO->OpacityMask.MaskR = 0; EO->OpacityMask.MaskG = 0; EO->OpacityMask.MaskB = 0;
	M->PostEditChange();
	M->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	return M;
}

// The detail-map master. RAGE's *_detail presets bind a high-frequency DetailSampler that tiles
// over the albedo, and until 2026-07-29 we could not use it: the tiling lives in `detailSettings`,
// which QUARRY was dropping along with every other non-texture shader parameter.
//
// MEASURED (1,994 samples, whole base set) - `detailSettings` component semantics:
//   .x  strength      0.0-8.0, typically 0.8-1.5
//   .y  secondary     0.0-6.0, but 64.9% are ZERO -> not load-bearing, left unused here
//   .z  tile U        0.0-32.0, typically 4-8
//   .w  tile V        0.0-48.0, typically 3-8
//
// \u26d4 NEUTRAL BY DEFAULT, deliberately. `DetailAmount` defaults to 0, so this master renders
// EXACTLY like the opaque one until ImportYdr proves a detail texture actually bound and sets it to
// 1. That property is the whole reason it is safe to ship without Matt having seen it yet: the
// failure mode of a wrong strength guess is "looks like today", not "looks worse than today".
// \u26a0 The exact RAGE blend is NOT measured - this is the standard signed overlay around mid-grey
// (\U0001f9e0 INFERRED, not \u2705). It is the conventional detail-map formula and is neutral when the detail
// texture is flat grey, which is why a wrong guess degrades gracefully.
// ---------------------------------------------------------------------------------------------
// GENERATED MASTERS - one per CAPABILITY SIGNATURE, derived from what presets actually bind.
//
// ⭐ WHY GENERATED, NOT AUTHORED (2026-07-30, Matt: "I'm not going to be able to design that at all
// by myself... we need to figure something out about how to generate the materials needed"):
// the corpus measures 90 shader presets but only **32 distinct sampler signatures**
// (reports/preset_inventory.json). A master is fully determined by which textures a preset binds
// and which scalars shape them - both MEASURED from real drawables - so the material set can be
// emitted rather than designed. Nobody hand-authors 90 materials, and nobody guesses a parameter
// list.
//
// ⭐ THE BAR IS A REPRESENTATION, NOT RAGE'S SHADER MATH (Matt's steer, same day): fidelity matters
// for the ROUND TRIP - does an exported asset look the same going back into GTA - and later for UE
// sequences. Everywhere else "reads right" is the target, so these aim for plausible rather than
// pixel-equivalent, while every parameter still round-trips because ExportYdr reads the MI back.
struct FRudeMasterSpec
{
	bool bNormal = false, bSpec = false, bDetail = false, bTint = false;
	bool bEmissive = false;
	int32 Bucket = 0;                       // 0 opaque - 1 alpha - 2 decal - 3 cutout

	FString Key() const                     // stable, readable asset name
	{
		FString K = TEXT("D");
		if (bNormal) { K += TEXT("N"); }
		if (bSpec)   { K += TEXT("S"); }
		if (bDetail) { K += TEXT("Dt"); }
		if (bTint)   { K += TEXT("T"); }
		if (bEmissive) { K += TEXT("E"); }
		return FString::Printf(TEXT("M_RUDE_%s_b%d"), *K, Bucket);
	}
};

static UMaterialInterface* EnsureGeneratedMaster(const FRudeMasterSpec& Spec)
{
	const FString Name = Spec.Key();
	const FString PkgName = FString::Printf(TEXT("/RUDE/Masters/Gen/%s"), *Name);
	const FString Full = FString::Printf(TEXT("%s.%s"), *PkgName, *Name);
	UMaterial* M = nullptr;
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, *Full))
	{
		// Bucket-1 (glass, alpha-blended) masters generated before 2026-09-05 used UE's default
		// volumetric translucency with no surface lighting: glass drew as a flat colour slab (the
		// blue tower). The new version carries an OpacityScale parameter; an old one is regenerated
		// IN PLACE so every material instance parented to it updates without a re-import.
		UMaterial* Old = Cast<UMaterial>(Existing);
		bool bStale = false;
		if (Old && Spec.Bucket == 1)
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Old->GetAllScalarParameterInfo(Infos, Ids);
			bStale = true;
			for (const FMaterialParameterInfo& I : Infos) { if (I.Name == FName(TEXT("OpacityScale"))) { bStale = false; break; } }
		}
		if (!bStale) { return Existing; }
		M = Old;
		M->GetExpressionCollection().Empty();
		UE_LOG(LogTemp, Display, TEXT("[RUDE] regenerating stale glass master %s"), *Name);
	}
	if (!M)
	{
		UPackage* P = CreatePackage(*PkgName);
		if (!P) { return nullptr; }
		M = NewObject<UMaterial>(P, *Name, RF_Public | RF_Standalone);
	}

	UTexture* DefWhite  = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* DefNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
	auto Add = [M](UMaterialExpression* E, int32 X, int32 Y)
	{
		E->MaterialExpressionEditorX = X; E->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	auto MakeTex = [&](const TCHAR* Param, UTexture* Def, EMaterialSamplerType T, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* S =
			NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
		S->ParameterName = Param; S->SamplerType = T; S->Texture = Def;
		Add(S, -1100, Y);
		return S;
	};
	auto MakeScalar = [&](const TCHAR* Param, float Def, int32 Y)
	{
		UMaterialExpressionScalarParameter* S = NewObject<UMaterialExpressionScalarParameter>(M);
		S->ParameterName = Param; S->DefaultValue = Def;
		Add(S, -1500, Y);
		return S;
	};

	// RAGE draw bucket -> UE blend mode. Bucket 1 is alpha-BLENDED and must never be alpha-TESTED
	// (that perforates glass); bucket 3 is the cutout. Same law as MasterForPreset.
	switch (Spec.Bucket)
	{
		case 1:  M->BlendMode = BLEND_Translucent; break;
		case 2:  M->BlendMode = BLEND_Masked; break;
		case 3:  M->BlendMode = BLEND_Masked; M->TwoSided = true; break;
		default: M->BlendMode = BLEND_Opaque; break;
	}

	UMaterialExpressionTextureSampleParameter2D* DiffuseTex =
		MakeTex(TEXT("Diffuse"), DefWhite, SAMPLERTYPE_Color, 0);
	UMaterialExpression* BaseColor = DiffuseTex;

	if (Spec.bDetail)
	{
		// Detail overlay with MEASURED tiling: detailSettings.zw is tile U/V (4-8 typical), .x is
		// strength. Neutral until a detail texture actually binds (DetailAmount default 0), so a
		// wrong strength guess degrades to "looks like today" rather than "looks worse".
		UMaterialExpressionVectorParameter* Set = NewObject<UMaterialExpressionVectorParameter>(M);
		Set->ParameterName = TEXT("detailSettings");
		Set->DefaultValue = FLinearColor(1.f, 0.f, 1.f, 1.f);
		Add(Set, -1500, -500);
		UMaterialExpressionScalarParameter* Amt = MakeScalar(TEXT("DetailAmount"), 0.f, -380);
		UMaterialExpressionTextureCoordinate* UV = NewObject<UMaterialExpressionTextureCoordinate>(M);
		Add(UV, -1500, -260);
		UMaterialExpressionComponentMask* ZW = NewObject<UMaterialExpressionComponentMask>(M);
		ZW->Input.Expression = Set; ZW->R = false; ZW->G = false; ZW->B = true; ZW->A = true;
		Add(ZW, -1350, -500);
		UMaterialExpressionMultiply* UVm = NewObject<UMaterialExpressionMultiply>(M);
		UVm->A.Expression = UV; UVm->B.Expression = ZW; Add(UVm, -1220, -320);
		UMaterialExpressionTextureSampleParameter2D* Det =
			MakeTex(TEXT("Detail"), DefWhite, SAMPLERTYPE_Color, -300);
		Det->Coordinates.Expression = UVm;
		UMaterialExpressionConstant* Half = NewObject<UMaterialExpressionConstant>(M);
		Half->R = 0.5f; Add(Half, -950, -520);
		UMaterialExpressionSubtract* Sub = NewObject<UMaterialExpressionSubtract>(M);
		Sub->A.Expression = Det; Sub->B.Expression = Half; Add(Sub, -820, -420);
		UMaterialExpressionConstant* Two = NewObject<UMaterialExpressionConstant>(M);
		Two->R = 2.f; Add(Two, -950, -300);
		UMaterialExpressionMultiply* Sgn = NewObject<UMaterialExpressionMultiply>(M);
		Sgn->A.Expression = Sub; Sgn->B.Expression = Two; Add(Sgn, -690, -420);
		UMaterialExpressionComponentMask* Xm = NewObject<UMaterialExpressionComponentMask>(M);
		Xm->Input.Expression = Set; Xm->R = true; Xm->G = false; Xm->B = false; Xm->A = false;
		Add(Xm, -820, -260);
		UMaterialExpressionMultiply* Str = NewObject<UMaterialExpressionMultiply>(M);
		Str->A.Expression = Sgn; Str->B.Expression = Xm; Add(Str, -560, -420);
		UMaterialExpressionMultiply* Gate = NewObject<UMaterialExpressionMultiply>(M);
		Gate->A.Expression = Str; Gate->B.Expression = Amt; Add(Gate, -430, -420);
		UMaterialExpressionConstant* One = NewObject<UMaterialExpressionConstant>(M);
		One->R = 1.f; Add(One, -560, -180);
		UMaterialExpressionAdd* Gain = NewObject<UMaterialExpressionAdd>(M);
		Gain->A.Expression = One; Gain->B.Expression = Gate; Add(Gain, -300, -320);
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
		Mul->A.Expression = BaseColor; Mul->B.Expression = Gain; Add(Mul, -170, -60);
		BaseColor = Mul;
	}

	if (Spec.bTint)
	{
		// The tint palette is a lookup texture and a faithful selector needs the palette ROW, which
		// is not yet decoded. Exposing the parameters keeps the binding real and round-trippable
		// while the visual stays the untinted albedo - an honest placeholder, not an invented tint.
		MakeTex(TEXT("TintPalette"), DefWhite, SAMPLERTYPE_Color, 900);
		MakeScalar(TEXT("tintPaletteSelector"), 0.f, 960);
	}

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = BaseColor;

	if (Spec.bNormal)
	{
		UMaterialExpressionTextureSampleParameter2D* N =
			MakeTex(TEXT("Normal"), DefNormal, SAMPLERTYPE_Normal, 300);
		EO->Normal.Expression = N;
		MakeScalar(TEXT("bumpiness"), 1.f, 340);
	}
	if (Spec.bSpec)
	{
		UMaterialExpressionTextureSampleParameter2D* S =
			MakeTex(TEXT("Specular"), DefWhite, SAMPLERTYPE_Color, 600);
		UMaterialExpressionScalarParameter* Int =
			MakeScalar(TEXT("specularIntensityMult"), 1.f, 640);
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
		Mul->A.Expression = S; Mul->B.Expression = Int; Add(Mul, -700, 600);
		EO->Specular.Expression = Mul;
		EO->Specular.MaskR = 1; EO->Specular.Mask = 1;
		EO->Specular.MaskG = 0; EO->Specular.MaskB = 0; EO->Specular.MaskA = 0;
		MakeScalar(TEXT("specularFalloffMult"), 100.f, 700);
		MakeScalar(TEXT("specularFresnel"), 0.97f, 760);
	}
	else
	{
		// Matt's calibration, 2026-07-30: roads and sidewalks want roughly 0.35-0.5 specular. A
		// preset with no spec map gets the middle of that range rather than UE's flat default, so
		// an unmapped surface still reads as a surface and not as paper.
		UMaterialExpressionScalarParameter* Flat = MakeScalar(TEXT("Specular"), 0.42f, 600);
		EO->Specular.Expression = Flat;
	}
	if (Spec.bEmissive)
	{
		UMaterialExpressionScalarParameter* Mult =
			MakeScalar(TEXT("emissiveMultiplier"), 1.f, 1100);
		// ⛔ NO TIME GATE HERE, AND THAT WAS A REAL MISTAKE (corrected 2026-07-30 by Matt).
		// I first multiplied emissive by a global NightFactor so lit windows would not glow at
		// noon. Matt: "the textures for these are tied to meshes and the meshes are rendered via
		// ymap... the structure more resembles datasets". MEASURED, and he is right: the game gates
		// them at the ARCHETYPE level - 3,936 CTimeArchetypeDef carrying a 24-bit `timeFlags` hour
		// mask (e.g. 32505919 = hours 0-5 + 20-23, night; 16777215 = all 24h). It swaps WHICH
		// ARCHETYPE IS VISIBLE per hour; it is not a shader effect at all.
		// A shader-side gate would therefore be a UE-only invention that does NOT round-trip to
		// GTA - and round-trip is one of the two places fidelity actually matters. So emissive is
		// just emissive here, and the time behaviour belongs to entity visibility driven by
		// timeFlags, which the archetype index now carries.
		UMaterialExpressionMultiply* Emit = NewObject<UMaterialExpressionMultiply>(M);
		Emit->A.Expression = DiffuseTex; Emit->B.Expression = Mult; Add(Emit, -700, 1100);
		EO->EmissiveColor.Expression = Emit;
	}
	if (Spec.Bucket == 3 || Spec.Bucket == 2)
	{
		EO->OpacityMask.Expression = DiffuseTex;
		EO->OpacityMask.MaskA = 1; EO->OpacityMask.Mask = 1;
		EO->OpacityMask.MaskR = 0; EO->OpacityMask.MaskG = 0; EO->OpacityMask.MaskB = 0;
	}
	else if (Spec.Bucket == 1)
	{
		// Glass that reads right: surface-lit translucency (specular + reflections, not the volumetric
		// default), opacity = diffuse alpha x OpacityScale (glass alphas are often 1.0), low roughness.
		M->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		UMaterialExpressionScalarParameter* OpScale = MakeScalar(TEXT("OpacityScale"), 0.55f, 1300);
		UMaterialExpressionMultiply* Op = NewObject<UMaterialExpressionMultiply>(M);
		Op->A.Expression = DiffuseTex; Op->A.Mask = 1; Op->A.MaskA = 1; Op->A.MaskR = 0; Op->A.MaskG = 0; Op->A.MaskB = 0;
		Op->B.Expression = OpScale; Add(Op, -700, 1300);
		EO->Opacity.Expression = Op;
		UMaterialExpressionScalarParameter* Rough = MakeScalar(TEXT("Roughness"), 0.12f, 1400);
		EO->Roughness.Expression = Rough;
	}
	M->PostEditChange();
	M->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	UE_LOG(LogTemp, Display, TEXT("[RUDE] generated master %s"), *Name);
	return M;
}

static UMaterialInterface* EnsureDetailMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_Detail.M_RUDE_Detail");
	UMaterial* M = LoadObject<UMaterial>(nullptr, FullPath);
	if (M)
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		M->GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& I : Infos)
		{
			if (I.Name == FName(TEXT("DetailAmount"))) { return M; }
		}
		M->GetExpressionCollection().Empty();   // ours, regenerable
	}
	else
	{
		UPackage* Pkg = CreatePackage(TEXT("/RUDE/Masters/M_RUDE_Detail"));
		if (!Pkg) { return nullptr; }
		M = NewObject<UMaterial>(Pkg, TEXT("M_RUDE_Detail"), RF_Public | RF_Standalone);
	}

	UTexture* DefWhite = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* DefNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));

	auto Add = [M](UMaterialExpression* E, int32 X, int32 Y)
	{
		E->MaterialExpressionEditorX = X; E->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(E);
	};

	auto* Diff = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Diff->ParameterName = TEXT("Diffuse"); Diff->SamplerType = SAMPLERTYPE_Color; Diff->Texture = DefWhite;
	Add(Diff, -1000, 0);
	auto* Nrm = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Nrm->ParameterName = TEXT("Normal"); Nrm->SamplerType = SAMPLERTYPE_Normal; Nrm->Texture = DefNormal;
	Add(Nrm, -1000, 700);
	auto* Spec = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Spec->ParameterName = TEXT("Specular"); Spec->SamplerType = SAMPLERTYPE_Color; Spec->Texture = DefWhite;
	Add(Spec, -1000, 1000);

	// detailSettings: the name MATCHES the RAGE parameter, so ImportYdr's generic value-param
	// binding sets it with no special case anywhere.
	auto* Settings = NewObject<UMaterialExpressionVectorParameter>(M);
	Settings->ParameterName = TEXT("detailSettings");
	Settings->DefaultValue = FLinearColor(1.f, 0.f, 1.f, 1.f);
	Add(Settings, -1600, -400);

	auto* Amount = NewObject<UMaterialExpressionScalarParameter>(M);
	Amount->ParameterName = TEXT("DetailAmount");
	Amount->DefaultValue = 0.f;          // \u26d4 neutral until a detail texture really bound
	Add(Amount, -1600, -250);

	// Detail UV = TexCoord * detailSettings.zw
	auto* UV = NewObject<UMaterialExpressionTextureCoordinate>(M);
	Add(UV, -1600, -100);
	auto* TileZW = NewObject<UMaterialExpressionComponentMask>(M);
	TileZW->Input.Expression = Settings;
	TileZW->R = false; TileZW->G = false; TileZW->B = true; TileZW->A = true;
	Add(TileZW, -1400, -400);
	auto* UVMul = NewObject<UMaterialExpressionMultiply>(M);
	UVMul->A.Expression = UV; UVMul->B.Expression = TileZW;
	Add(UVMul, -1250, -150);

	auto* Det = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
	Det->ParameterName = TEXT("Detail"); Det->SamplerType = SAMPLERTYPE_Color; Det->Texture = DefWhite;
	Det->Coordinates.Expression = UVMul;
	Add(Det, -1000, -300);

	// signed overlay: 1 + (Detail - 0.5) * 2 * strength(.x) * DetailAmount
	auto* Half = NewObject<UMaterialExpressionConstant>(M); Half->R = 0.5f; Add(Half, -900, -520);
	auto* Sub = NewObject<UMaterialExpressionSubtract>(M);
	Sub->A.Expression = Det; Sub->B.Expression = Half; Add(Sub, -760, -400);
	auto* Two = NewObject<UMaterialExpressionConstant>(M); Two->R = 2.f; Add(Two, -900, -300);
	auto* Signed = NewObject<UMaterialExpressionMultiply>(M);
	Signed->A.Expression = Sub; Signed->B.Expression = Two; Add(Signed, -620, -400);
	auto* StrX = NewObject<UMaterialExpressionComponentMask>(M);
	StrX->Input.Expression = Settings;
	StrX->R = true; StrX->G = false; StrX->B = false; StrX->A = false;
	Add(StrX, -760, -250);
	auto* ByStr = NewObject<UMaterialExpressionMultiply>(M);
	ByStr->A.Expression = Signed; ByStr->B.Expression = StrX; Add(ByStr, -480, -400);
	auto* ByAmt = NewObject<UMaterialExpressionMultiply>(M);
	ByAmt->A.Expression = ByStr; ByAmt->B.Expression = Amount; Add(ByAmt, -350, -400);
	auto* One = NewObject<UMaterialExpressionConstant>(M); One->R = 1.f; Add(One, -480, -160);
	auto* Gain = NewObject<UMaterialExpressionAdd>(M);
	Gain->A.Expression = One; Gain->B.Expression = ByAmt; Add(Gain, -220, -300);
	auto* Final = NewObject<UMaterialExpressionMultiply>(M);
	Final->A.Expression = Diff; Final->B.Expression = Gain; Add(Final, -80, 0);

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = Final;
	EO->Normal.Expression = Nrm;
	EO->Specular.Expression = Spec;
	EO->Specular.MaskR = 1; EO->Specular.Mask = 1;
	EO->Specular.MaskG = 0; EO->Specular.MaskB = 0; EO->Specular.MaskA = 0;
	M->PostEditChange();
	M->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	return M;
}

static UMaterialInterface* EnsureTerrainMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_Terrain.M_RUDE_Terrain");
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, FullPath))
	{
		return Existing;
	}
	UPackage* Pkg = CreatePackage(TEXT("/RUDE/Masters/M_RUDE_Terrain"));
	if (!Pkg) { return nullptr; }
	UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_RUDE_Terrain"), RF_Public | RF_Standalone);
	UTexture* DefWhite = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* DefNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));

	auto TexParam = [&](const FString& Name, bool bNormal, int32 X, int32 Y) -> UMaterialExpressionTextureSampleParameter2D*
	{
		auto* E = NewObject<UMaterialExpressionTextureSampleParameter2D>(M);
		E->ParameterName = FName(*Name);
		E->SamplerType = bNormal ? SAMPLERTYPE_Normal : SAMPLERTYPE_Color;
		E->Texture = bNormal ? DefNormal : DefWhite;
		E->MaterialExpressionEditorX = X; E->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	auto* VC = NewObject<UMaterialExpressionVertexColor>(M);
	VC->MaterialExpressionEditorX = -900;
	M->GetExpressionCollection().AddExpression(VC);
	auto Mask = [&](int32 R, int32 G, int32 B, int32 Y) -> UMaterialExpressionComponentMask*
	{
		auto* E = NewObject<UMaterialExpressionComponentMask>(M);
		E->R = R; E->G = G; E->B = B; E->A = 0;
		E->Input.Expression = VC;
		E->MaterialExpressionEditorX = -700; E->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	// Blend weights: RAGE terrain masks live in a vertex COLOUR stream. Which one is
	// per-shader-family ambiguous (corpus survey: Colour1.G/B vary in ~48/60 meshes,
	// Colour0.A/B in ~44/33) - so ImportYdr ships BOTH (Colour0 -> vertex colour,
	// Colour1 -> UV2/UV3) and this master blends from a chosen set. w1..w3 drive
	// layers 1-3, layer0 takes the remainder: NORMALIZED weighted sum (a nested-lerp
	// chain averages everything toward grey - the washed-out first attempt).
	UMaterialExpressionComponentMask* Masks[3] = { Mask(1,0,0,-100), Mask(0,1,0,0), Mask(0,0,1,100) };
	auto Chain = [&](UMaterialExpressionTextureSampleParameter2D* const T[4], int32 Y) -> UMaterialExpression*
	{
		// w0 = saturate(1 - (w1+w2+w3))
		auto* Sum12 = NewObject<UMaterialExpressionAdd>(M);
		Sum12->A.Expression = Masks[0]; Sum12->B.Expression = Masks[1];
		M->GetExpressionCollection().AddExpression(Sum12);
		auto* Sum123 = NewObject<UMaterialExpressionAdd>(M);
		Sum123->A.Expression = Sum12; Sum123->B.Expression = Masks[2];
		M->GetExpressionCollection().AddExpression(Sum123);
		auto* One = NewObject<UMaterialExpressionConstant>(M); One->R = 1.f;
		M->GetExpressionCollection().AddExpression(One);
		auto* W0raw = NewObject<UMaterialExpressionSubtract>(M);
		W0raw->A.Expression = One; W0raw->B.Expression = Sum123;
		M->GetExpressionCollection().AddExpression(W0raw);
		auto* W0 = NewObject<UMaterialExpressionSaturate>(M);
		W0->Input.Expression = W0raw;
		M->GetExpressionCollection().AddExpression(W0);

		UMaterialExpression* Weights[4] = { W0, Masks[0], Masks[1], Masks[2] };
		UMaterialExpression* Acc = nullptr;
		UMaterialExpression* WSum = nullptr;
		for (int32 i = 0; i < 4; ++i)
		{
			auto* Mul = NewObject<UMaterialExpressionMultiply>(M);
			Mul->A.Expression = T[i]; Mul->B.Expression = Weights[i];
			Mul->MaterialExpressionEditorX = -400; Mul->MaterialExpressionEditorY = Y + i * 90;
			M->GetExpressionCollection().AddExpression(Mul);
			if (!Acc) { Acc = Mul; }
			else
			{
				auto* Add = NewObject<UMaterialExpressionAdd>(M);
				Add->A.Expression = Acc; Add->B.Expression = Mul;
				M->GetExpressionCollection().AddExpression(Add);
				Acc = Add;
			}
			if (!WSum) { WSum = Weights[i]; }
			else
			{
				auto* AddW = NewObject<UMaterialExpressionAdd>(M);
				AddW->A.Expression = WSum; AddW->B.Expression = Weights[i];
				M->GetExpressionCollection().AddExpression(AddW);
				WSum = AddW;
			}
		}
		auto* Eps = NewObject<UMaterialExpressionConstant>(M); Eps->R = 0.0001f;
		M->GetExpressionCollection().AddExpression(Eps);
		auto* SafeSum = NewObject<UMaterialExpressionAdd>(M);
		SafeSum->A.Expression = WSum; SafeSum->B.Expression = Eps;
		M->GetExpressionCollection().AddExpression(SafeSum);
		auto* Div = NewObject<UMaterialExpressionDivide>(M);
		Div->A.Expression = Acc; Div->B.Expression = SafeSum;
		Div->MaterialExpressionEditorX = -150; Div->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(Div);
		return Div;
	};
	UMaterialExpressionTextureSampleParameter2D* D[4];
	UMaterialExpressionTextureSampleParameter2D* N[4];
	for (int32 i = 0; i < 4; ++i)
	{
		D[i] = TexParam(FString::Printf(TEXT("Diffuse%d"), i), false, -1300, -400 + i * 150);
		N[i] = TexParam(FString::Printf(TEXT("Normal%d"), i), true, -1300, 300 + i * 150);
	}
	auto* Rough = NewObject<UMaterialExpressionConstant>(M);
	Rough->R = 0.85f;
	M->GetExpressionCollection().AddExpression(Rough);

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = Chain(D, -200);
	EO->Normal.Expression = Chain(N, 400);
	EO->Roughness.Expression = Rough;

	// ⭐ The terrain presets DO bind spec, and this master used to expose none of it, so every
	// specularIntensityMult / specularFalloffMult on a terrain shader counted as
	// unsupportedByMaster and did nothing (measured 2026-07-30 from reports/preset_inventory.json:
	// all 5 terrain_cb_w_4lyr* presets bind both, plus bumpiness / bumpSelfShadowAmount /
	// materialWetnessMultiplier). Exposing them costs nothing and lets the corpus values land.
	auto Scalar = [&](const TCHAR* Name, float Def, int32 Y)
	{
		auto* E = NewObject<UMaterialExpressionScalarParameter>(M);
		E->ParameterName = Name; E->DefaultValue = Def;
		E->MaterialExpressionEditorX = -1400; E->MaterialExpressionEditorY = Y;
		M->GetExpressionCollection().AddExpression(E);
		return E;
	};
	auto* SpecInt = Scalar(TEXT("specularIntensityMult"), 0.42f, 800);
	Scalar(TEXT("specularFalloffMult"), 100.f, 860);
	Scalar(TEXT("bumpiness"), 1.f, 920);
	Scalar(TEXT("materialWetnessMultiplier"), 0.f, 980);
	Scalar(TEXT("bumpSelfShadowAmount"), 0.f, 1040);
	// Matt's calibration: ground surfaces sit around 0.35-0.5 specular. Terrain IS ground, so the
	// default matches the flat value the generated masters use for spec-less presets.
	EO->Specular.Expression = SpecInt;

	// ⛔ NOT IMPLEMENTED, AND DELIBERATELY NOT FAKED: heightMapSamplerLayer0-3 with their
	// heightScale0-3 / heightBias0-3 / parallaxSelfShadowAmount. The measured scales are ~0.015-0.03
	// — those are PARALLAX DEPTHS in UV units, not blend weights. Multiplying blend weights by them
	// (the obvious-looking shortcut) would crush every layer to near zero and look worse than the
	// plain blend. A faithful version is per-layer parallax offset ×4, which is a real graph and a
	// real cost. Recorded in the LOG as the next terrain step rather than approximated here.
	M->PostEditChange();
	Pkg->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	return M;
}

namespace RudeYdr
{
	// Vertex layout semantic -> token width in a vertex Data line. Covers every semantic
	// observed across the corpus (14 distinct layouts incl. skinned props). A width-0
	// (unknown) semantic MUST abort the geometry, not misalign the stream - width
	// misalignment scrambles every vertex after the first (the "deformed mesh" class,
	// caught on the Cayo slice: 6 skinned props with BlendWeights/BlendIndices).
	static int32 SemanticWidth(const FString& Tag)
	{
		if (Tag == TEXT("Position") || Tag == TEXT("Normal")) return 3;
		if (Tag == TEXT("Colour0") || Tag == TEXT("Colour1") || Tag == TEXT("Tangent")) return 4;
		if (Tag == TEXT("BlendWeights") || Tag == TEXT("BlendIndices")) return 4;   // skin data, parsed + ignored
		if (Tag.StartsWith(TEXT("TexCoord"))) return 2;
		return 0;
	}

	struct FGeo
	{
		int32 ShaderIndex = 0;
		TArray<FVector3f> Positions;   // already RUDE-transformed to UE space (cm, Y-mirrored)
		TArray<FVector3f> Normals;     // Y-mirrored
		TArray<FVector2f> UVs;         // raw RAGE UVs (both engines are V-down; no flip)
		TArray<FVector4f> Colors;      // Colour0 as 0-1 RGBA -> UE vertex colour
		TArray<FVector4f> Colors1;     // Colour1 as 0-1 RGBA -> smuggled into UV2/UV3
		TArray<int32> Indices;         // winding already flipped for the mirror
		// ⛔ A geometry with NO TexCoord0 in its layout gets UV (0,0) on every vertex, which hands
		// MikkTSpace a zero-AREA triangle for the whole geometry - the tangent basis it returns for
		// that section is arbitrary, and any normal map bound to it lights wrong. The importer used
		// to do this in total silence (the UV default is the `IsValidIndex ? : ZeroVector` ternary in
		// the builder, which reads as a bounds guard, not as a substitution). MEASURED over 900
		// resolved ydr / 3,444 geometries: 8 (0.232%) carry no TexCoord0 - and they are not all
		// proxies (prop_tree_fallen_01, prop_bush_lrg_03 are shipped props). Small, real, and
		// previously unreportable.
		bool bHasUV0 = false;
	};

	static bool ParseGeometry(const FXmlNode* GeoNode, FGeo& Out, FString& Error)
	{
		const FXmlNode* ShaderIndexNode = GeoNode->FindChildNode(TEXT("ShaderIndex"));
		if (ShaderIndexNode)
		{
			Out.ShaderIndex = FCString::Atoi(*ShaderIndexNode->GetAttribute(TEXT("value")));
		}

		const FXmlNode* VB = GeoNode->FindChildNode(TEXT("VertexBuffer"));
		const FXmlNode* IB = GeoNode->FindChildNode(TEXT("IndexBuffer"));
		if (!VB || !IB)
		{
			Error = TEXT("geometry missing VertexBuffer/IndexBuffer");
			return false;
		}

		// Layout: ordered semantic list. An UNKNOWN semantic aborts (width guess = stream
		// misalignment = scrambled geometry); report it instead of emitting garbage.
		TArray<FString> Semantics;
		int32 LineWidth = 0;
		if (const FXmlNode* Layout = VB->FindChildNode(TEXT("Layout")))
		{
			for (const FXmlNode* Child : Layout->GetChildrenNodes())
			{
				const int32 W = SemanticWidth(Child->GetTag());
				if (W == 0)
				{
					Error = FString::Printf(TEXT("unknown vertex semantic '%s' - refusing to misalign"), *Child->GetTag());
					return false;
				}
				if (Child->GetTag() == TEXT("TexCoord0")) { Out.bHasUV0 = true; }
				Semantics.Add(Child->GetTag());
				LineWidth += W;
			}
		}
		if (LineWidth == 0)
		{
			Error = TEXT("empty vertex layout");
			return false;
		}

		const FXmlNode* VData = VB->FindChildNode(TEXT("Data"));
		const FXmlNode* IData = IB->FindChildNode(TEXT("Data"));
		if (!VData || !IData)
		{
			Error = TEXT("missing Data payloads");
			return false;
		}

		// Vertices: parse the whole token STREAM, LineWidth floats per vertex.
		// (FXmlFile content does not preserve line structure - the 1-vertex-per-
		// geometry bug of 2026-07-24. Never rely on newlines in XML payloads.)
		// ⛔ THE NORMAL'S WIDTH COMES FROM THE DATA (2026-09-05). GTAV2 lines carry a 4-component
		// normal (x y z 0): 17 tokens against the semantic sum of 16, and the old "tokens % width"
		// check passed by coincidence (288 x 17 = 4896, divisible by 16) - 306 shifted vertices out
		// to 255 m for a 7 m cloth tarp. FXmlFile flattens <Data> to one line, so the stream stays
		// flat; the width is chosen among {sum, sum+1} by divisibility AND by the first vertex's
		// Colour0 decoding as four integers 0..255, with a GTAV2+ layout breaking a tie toward 4.
		TArray<FString> Toks;
		VData->GetContent().ParseIntoArrayWS(Toks);
		FString LayoutType;
		if (const FXmlNode* Layout = VB->FindChildNode(TEXT("Layout"))) { LayoutType = Layout->GetAttribute(TEXT("type")); }
		int32 NormalWidth = SemanticWidth(TEXT("Normal"));
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
				return true;   // no colour semantic to check against
			};
			const bool bHasNormal = Semantics.Contains(TEXT("Normal"));
			const int32 W3 = LineWidth, W4 = LineWidth + 1;
			const bool bDiv3 = Toks.Num() % W3 == 0, bDiv4 = bHasNormal && Toks.Num() % W4 == 0;
			const bool bOk3 = bDiv3 && ColourSane(SemanticWidth(TEXT("Normal")));
			const bool bOk4 = bDiv4 && ColourSane(SemanticWidth(TEXT("Normal")) + 1);
			if (bOk3 && bOk4)
			{
				// both decode: the layout type decides (GTAV1 = 3-wide; GTAV2 and later = 4-wide)
				NormalWidth = LayoutType.Equals(TEXT("GTAV1"), ESearchCase::IgnoreCase) ? SemanticWidth(TEXT("Normal")) : SemanticWidth(TEXT("Normal")) + 1;
			}
			else if (bOk4) { NormalWidth = SemanticWidth(TEXT("Normal")) + 1; }
			else if (bOk3) { NormalWidth = SemanticWidth(TEXT("Normal")); }
			else
			{
				Error = FString::Printf(TEXT("vertex stream misaligned: %d tokens, layout '%s' sums to %d (+1 tried) - neither decodes"),
					Toks.Num(), *LayoutType, LineWidth);
				return false;
			}
			if (NormalWidth != SemanticWidth(TEXT("Normal"))) { LineWidth += 1; }
		}
		const int32 NumVerts = Toks.Num() / LineWidth;
		for (int32 V = 0; V < NumVerts; ++V)
		{
			int32 Off = V * LineWidth;
			FVector3f Pos = FVector3f::ZeroVector;
			FVector3f Nrm(0, 0, 1);
			FVector2f UV = FVector2f::ZeroVector;
			FVector4f Col(1, 1, 1, 1);
			FVector4f Col1(0, 0, 0, 0);
			for (const FString& Sem : Semantics)
			{
				const int32 W = (Sem == TEXT("Normal")) ? NormalWidth : SemanticWidth(Sem);
				if (Sem == TEXT("Position"))
				{
					Pos = FVector3f(FCString::Atof(*Toks[Off]),
					                FCString::Atof(*Toks[Off + 1]),
					                FCString::Atof(*Toks[Off + 2]));
				}
				else if (Sem == TEXT("Normal"))
				{
					Nrm = FVector3f(FCString::Atof(*Toks[Off]),
					                FCString::Atof(*Toks[Off + 1]),
					                FCString::Atof(*Toks[Off + 2]));
				}
				else if (Sem == TEXT("TexCoord0"))
				{
					UV = FVector2f(FCString::Atof(*Toks[Off]),
					               FCString::Atof(*Toks[Off + 1]));
				}
				else if (Sem == TEXT("Colour0"))
				{
					// terrain blend weights live in a colour stream (0-255 per channel)
					Col = FVector4f(FCString::Atof(*Toks[Off]) / 255.f,
					                FCString::Atof(*Toks[Off + 1]) / 255.f,
					                FCString::Atof(*Toks[Off + 2]) / 255.f,
					                FCString::Atof(*Toks[Off + 3]) / 255.f);
				}
				else if (Sem == TEXT("Colour1"))
				{
					// UE static meshes carry ONE vertex-colour set, so Colour1 (the other
					// terrain-mask candidate) rides in UV channels 2/3 - the material can
					// then blend from either stream without a re-import.
					Col1 = FVector4f(FCString::Atof(*Toks[Off]) / 255.f,
					                 FCString::Atof(*Toks[Off + 1]) / 255.f,
					                 FCString::Atof(*Toks[Off + 2]) / 255.f,
					                 FCString::Atof(*Toks[Off + 3]) / 255.f);
				}
				Off += W;
			}
			// RUDE transform: gta meters -> ue cm, Y mirror
			Out.Positions.Add(FVector3f(Pos.X * 100.f, -Pos.Y * 100.f, Pos.Z * 100.f));
			Out.Normals.Add(FVector3f(Nrm.X, -Nrm.Y, Nrm.Z));
			Out.UVs.Add(UV);
			Out.Colors.Add(Col);
			Out.Colors1.Add(Col1);
		}

		TArray<FString> IdxToks;
		IData->GetContent().ParseIntoArrayWS(IdxToks);
		for (const FString& T : IdxToks)
		{
			Out.Indices.Add(FCString::Atoi(*T));
		}
		if (Out.Positions.Num() == 0 || Out.Indices.Num() < 3)
		{
			Error = TEXT("no usable vertex/index data");
			return false;
		}
		return true;
	}
}

// ============================================================================================
// RudeBound - RAGE phBound  ->  UE UBodySetup (open item #40, the IMPORT half)
// ============================================================================================
// ⛔ WHAT WAS WRONG: RUDE had NO collision importer at all. Grep for ImportYbn / ImportBound /
// ImportCollision returned zero hits plugin-wide (re-measured 2026-08-05), so every imported asset's
// AggGeom was empty and every export re-derived collision from the RENDER mesh. 13,926 .ybn files
// and the <Bounds> block embedded in 17.3% of every ydr (432 of a 2,500-file sample) had no consumer.
//
// ✅ EVERY CONVENTION BELOW IS MEASURED AGAINST THE EMITTED CORPUS, NOT TAKEN FROM DOCS.
//   * CompositeTransform is ROW-VECTOR (p' = p*M, translation in the LAST ROW) - the same layout
//     ExportYdr's XformRows already writes. Adjudicated WITH A CONTROL over the 900-file ydr sample,
//     scored on composites that have a genuinely ROTATED child (an identity-rotation child cannot
//     testify), by transforming each child's own BoxMin/BoxMax and unioning them against the parent
//     composite's declared BoxMin/BoxMax - a DERIVED FIELD, so it settles the convention with no
//     external reference:  row 62/62 pass  ·  column 37/62  ·  translation-only 9/62.
//   * A child transform is a pure rotation+translation: worst |rowLength-1| = 5.0e-7 and worst
//     |row dot row| = 9.9e-4 over 1,829 child transforms, zero negative determinants. There is
//     therefore no scale to carry - and FKBoxElem has nowhere to put one, so a scaled transform is
//     REFUSED rather than silently flattened.
//   * <Vertices> in a Geometry/GeometryBVH bound are stored RELATIVE TO <GeometryCenter>. Proven by
//     the same kind of derived-field oracle: GeometryCenter + vertex reproduces the bound's own
//     BoxMin/BoxMax on 224 of 226 triangle-only bounds once <Margin> is allowed for, worst residual
//     0.0116 (float32 noise on world-scale coordinates).
//   * Sphere: BoxMin/BoxMax == SphereCenter +- SphereRadius EXACTLY, 16 of 16.
//   * Capsule: the long axis is Y in 82 of 82 sampled bounds, halfLong == SphereRadius and the two
//     short half-extents == <Margin> == the radius. The axis is nevertheless DERIVED from the
//     dominant extent rather than hardcoded to Y - a catalogue is a lower bound, and deriving it
//     also makes the importer agree with ExportYdr, which writes its capsules Z-dominant.
//
// ⛔ THE REFUSAL POLICY, split by CAUSE because the two are diagnosed differently:
//   Unmapped  = a bound RUDE understands and has no faithful UE target for. Today that is Cylinder:
//               UE's FKAggregateGeom has no cylinder, and a convex N-gon prism would be a silent
//               geometric substitution of exactly the class this project keeps paying for. It fires
//               on healthy shipped data (240 of ~3,900 sampled child bounds), so it must NOT gate.
//   Malformed = a type RUDE DOES support whose fields could not be read or failed their own
//               cross-check, OR a <Bounds type=""> string never seen before. Both mean the TOOL
//               failed, not the data. Measured cost on healthy corpus data: zero. It GATES.
namespace RudeBound
{
	// Every counter the import verdict reports, plus the geometry itself.
	struct FResult
	{
		int32 BoundsSeen = 0;          // leaf bounds visited (a Composite that recursed is not a leaf)
		int32 PrimitivesImported = 0;  // Box/Sphere/Capsule -> FKAggregateGeom
		int32 MeshesImported = 0;      // Geometry/GeometryBVH that contributed >= 1 triangle
		int32 BoundsUnmapped = 0;      // no UE target - counted, named, does NOT gate
		int32 BoundsMalformed = 0;     // could not parse / unknown type - GATES ok
		int32 PolysDropped = 0;        // non-triangle polygons inside a mesh bound
		int32 TrisOutOfRange = 0;
		int32 TrisDegenerate = 0;
		FKAggregateGeom Agg;
		TArray<FVector3f> Verts;       // UE-space (cm, Y-mirrored) collision triangle soup
		TArray<int32> Indices;
		TSet<FString> Reasons;         // distinct named reasons, so a refusal is never anonymous

		void Refuse(bool bMalformed, const FString& Why)
		{
			if (bMalformed) { ++BoundsMalformed; } else { ++BoundsUnmapped; }
			Reasons.Add(Why);
		}
	};

	// gta metres -> UE cm with the pinned Y mirror (ENGINEERING_LOG "Coordinate & geometry
	// conventions"). The same map ImportYdr applies to render positions, so collision and render
	// geometry cannot land in different spaces.
	static FVector ToUePos(const FVector& G) { return FVector(G.X * 100.0, -G.Y * 100.0, G.Z * 100.0); }
	static FVector ToUeDir(const FVector& G) { return FVector(G.X, -G.Y, G.Z); }

	static bool ReadVec(const FXmlNode* Parent, const TCHAR* Tag, FVector& Out)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Tag) : nullptr;
		if (!N) { return false; }
		Out = FVector(FCString::Atod(*N->GetAttribute(TEXT("x"))),
		              FCString::Atod(*N->GetAttribute(TEXT("y"))),
		              FCString::Atod(*N->GetAttribute(TEXT("z"))));
		return true;
	}

	static bool ReadVal(const FXmlNode* Parent, const TCHAR* Tag, double& Out)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Tag) : nullptr;
		if (!N) { return false; }
		Out = FCString::Atod(*N->GetAttribute(TEXT("value")));
		return true;
	}

	// <CompositeTransform> -> the child's local-to-parent transform, already mirrored into UE space.
	// ABSENT is legal and means identity: the root <Bounds> carries no transform at all.
	// Returns false (with Why) only when a transform is PRESENT and unusable - the refusing case.
	static bool ReadTransform(const FXmlNode* Node, FTransform& Out, FString& Why)
	{
		Out = FTransform::Identity;
		const FXmlNode* CT = Node->FindChildNode(TEXT("CompositeTransform"));
		if (!CT) { return true; }
		TArray<FString> Toks;
		CT->GetContent().ParseIntoArrayWS(Toks);
		if (Toks.Num() != 16)
		{
			Why = FString::Printf(TEXT("compositeTransform:%dTokens"), Toks.Num());
			return false;
		}
		double M[16];
		for (int32 i = 0; i < 16; ++i) { M[i] = FCString::Atod(*Toks[i]); }
		// Rows 0..2 are the rotated basis vectors. Refuse anything carrying scale or shear: an
		// FKBoxElem is Center+Rotation+extents with nowhere to put a scale, so flattening one would
		// silently resize the collider. Measured worst deviation on the corpus is 5e-7, so a 1e-2
		// tolerance cannot fire on healthy data.
		const FVector RX(M[0], M[1], M[2]), RY(M[4], M[5], M[6]), RZ(M[8], M[9], M[10]);
		if (FMath::Abs(RX.Size() - 1.0) > 1e-2 || FMath::Abs(RY.Size() - 1.0) > 1e-2
			|| FMath::Abs(RZ.Size() - 1.0) > 1e-2)
		{
			Why = TEXT("compositeTransform:scaled");
			return false;
		}
		// gta rotation -> UE rotation is the mirror conjugation M*R*M with M = diag(1,-1,1), which in
		// quaternion form is the involution (-x, y, -z, w) - the SAME map ExportYdr uses in the other
		// direction (ToGtaQuat), which is what makes import and export inverses of each other.
		const FQuat Qg = FMatrix(RX, RY, RZ, FVector::ZeroVector).ToQuat();
		const FQuat Que(-Qg.X, Qg.Y, -Qg.Z, Qg.W);
		Out = FTransform(Que, ToUePos(FVector(M[12], M[13], M[14])));
		return true;
	}

	static void ParseBound(const FXmlNode* Node, const FTransform& Parent, int32 Depth, FResult& R);

	// Geometry / GeometryBVH -> a triangle soup in UE space, for complex-as-simple collision.
	static void ParseGeometryBound(const FXmlNode* Node, const FTransform& World,
	                               const FString& TypeName, FResult& R)
	{
		FVector GC;
		if (!ReadVec(Node, TEXT("GeometryCenter"), GC))
		{
			// Every sampled Geometry/GeometryBVH carries one, and the vertices are meaningless
			// without it - a missing centre would place the whole collider at the origin.
			R.Refuse(true, FString::Printf(TEXT("%s:noGeometryCenter"), *TypeName));
			return;
		}
		const FXmlNode* VN = Node->FindChildNode(TEXT("Vertices"));
		const FXmlNode* PN = Node->FindChildNode(TEXT("Polygons"));
		if (!VN || !PN)
		{
			R.Refuse(true, FString::Printf(TEXT("%s:noVerticesOrPolygons"), *TypeName));
			return;
		}
		TArray<FString> Toks;
		VN->GetContent().ParseIntoArrayWS(Toks);
		if (Toks.Num() == 0 || (Toks.Num() % 3) != 0)
		{
			R.Refuse(true, FString::Printf(TEXT("%s:vertexTokens%%3"), *TypeName));
			return;
		}
		const int32 NumV = Toks.Num() / 3;
		const int32 Base = R.Verts.Num();
		R.Verts.Reserve(Base + NumV);
		for (int32 i = 0; i < NumV; ++i)
		{
			// tokens are "x, y, z" - Atod stops at the trailing comma
			const FVector Local(FCString::Atod(*Toks[i * 3 + 0]),
			                    FCString::Atod(*Toks[i * 3 + 1]),
			                    FCString::Atod(*Toks[i * 3 + 2]));
			const FVector W = World.TransformPosition(ToUePos(GC + Local));
			R.Verts.Add(FVector3f((float)W.X, (float)W.Y, (float)W.Z));
		}
		int32 TrisHere = 0;
		for (const FXmlNode* P : PN->GetChildrenNodes())
		{
			if (P->GetTag() != TEXT("Triangle"))
			{
				// ⛔ A BVH also carries POLY-primitives (Box/Sphere/Capsule/Cylinder), measured at
				// 5.7% of all polygons (46,627 of 828,501 across 120 ybn). polySphere and polyCapsule
				// are exactly mappable and polyBox's 4-index encoding is not derived here; rather
				// than invent any of them, every one is counted and NAMED. Registered as the next
				// slice of #40 - a marker beats an invented shape.
				++R.PolysDropped;
				R.Reasons.Add(FString::Printf(TEXT("poly:%s"), *P->GetTag()));
				continue;
			}
			const int32 I0 = FCString::Atoi(*P->GetAttribute(TEXT("v1")));
			const int32 I1 = FCString::Atoi(*P->GetAttribute(TEXT("v2")));
			const int32 I2 = FCString::Atoi(*P->GetAttribute(TEXT("v3")));
			// Lower bound matters as much as upper: these come from Atoi on untrusted XML and a
			// negative index passed an upper-bound-only check straight into TArray's fatal RangeCheck
			// on the render lane once already.
			if (I0 < 0 || I1 < 0 || I2 < 0 || I0 >= NumV || I1 >= NumV || I2 >= NumV)
			{
				++R.TrisOutOfRange;
				continue;
			}
			if (I0 == I1 || I1 == I2 || I0 == I2) { ++R.TrisDegenerate; continue; }
			// Winding passes through as-is, the same rule ImportYdr's render lane is pinned to: with
			// the Y-mirror applied to positions, RAGE winding already faces outward in UE.
			R.Indices.Add(Base + I0);
			R.Indices.Add(Base + I1);
			R.Indices.Add(Base + I2);
			++TrisHere;
		}
		if (TrisHere > 0)
		{
			++R.MeshesImported;
		}
		else
		{
			// A bound made entirely of poly-primitives contributes NOTHING to the collision mesh.
			// Counting it as an imported mesh would be the "presence is not coverage" defect.
			R.Verts.SetNum(Base);
			R.Refuse(false, FString::Printf(TEXT("%s:noTriangles"), *TypeName));
		}
	}

	static void ParseBound(const FXmlNode* Node, const FTransform& Parent, int32 Depth, FResult& R)
	{
		if (Depth > 8)
		{
			++R.BoundsSeen;
			R.Refuse(true, TEXT("depthExceeded"));
			return;
		}
		FTransform Local;
		FString Why;
		if (!ReadTransform(Node, Local, Why))
		{
			++R.BoundsSeen;
			R.Refuse(true, Why);
			return;
		}
		const FTransform World = Local * Parent;
		const FString Type = Node->GetAttribute(TEXT("type"));

		if (Type == TEXT("Composite"))
		{
			const FXmlNode* Children = Node->FindChildNode(TEXT("Children"));
			if (!Children)
			{
				++R.BoundsSeen;
				R.Refuse(true, TEXT("Composite:noChildren"));
				return;
			}
			for (const FXmlNode* Item : Children->GetChildrenNodes())
			{
				ParseBound(Item, World, Depth + 1, R);
			}
			return;   // a container is not a leaf - it is not counted in BoundsSeen
		}

		++R.BoundsSeen;

		FVector BMin, BMax;
		const bool bHaveBox = ReadVec(Node, TEXT("BoxMin"), BMin) && ReadVec(Node, TEXT("BoxMax"), BMax);

		if (Type == TEXT("Box"))
		{
			if (!bHaveBox) { R.Refuse(true, TEXT("Box:noBoxMinMax")); return; }
			const FVector Dim = BMax - BMin;
			if (Dim.X <= 0.0 || Dim.Y <= 0.0 || Dim.Z <= 0.0)
			{
				R.Refuse(true, TEXT("Box:nonPositiveExtent"));
				return;
			}
			FKBoxElem E;
			E.Center = World.TransformPosition(ToUePos((BMin + BMax) * 0.5));
			E.Rotation = World.Rotator();
			// FKBoxElem X/Y/Z are FULL extents in cm - the same reading ExportYdr encodes when it
			// divides by 200 to get half-extents in metres.
			E.X = (float)(Dim.X * 100.0);
			E.Y = (float)(Dim.Y * 100.0);
			E.Z = (float)(Dim.Z * 100.0);
			R.Agg.BoxElems.Add(E);
			++R.PrimitivesImported;
			return;
		}

		if (Type == TEXT("Sphere"))
		{
			double Rad = 0.0;
			if (!ReadVal(Node, TEXT("SphereRadius"), Rad) || Rad <= 0.0)
			{
				R.Refuse(true, TEXT("Sphere:noRadius"));
				return;
			}
			FVector SC(ForceInitToZero);
			ReadVec(Node, TEXT("SphereCenter"), SC);
			// Cross-check the radius against the bound's own bbox, which is a DERIVED field. If the
			// two ever disagree the format assumption has moved and a silent wrong-sized collider is
			// the worst possible outcome; refuse instead. Measured agreement: 16 of 16, error 0.0.
			if (bHaveBox)
			{
				const double H = ((BMax - BMin) * 0.5).GetMax();
				if (FMath::Abs(H - Rad) > FMath::Max(1e-3, 0.01 * Rad))
				{
					R.Refuse(true, TEXT("Sphere:radiusBboxMismatch"));
					return;
				}
			}
			FKSphereElem E;
			E.Center = World.TransformPosition(ToUePos(SC));
			E.Radius = (float)(Rad * 100.0);
			R.Agg.SphereElems.Add(E);
			++R.PrimitivesImported;
			return;
		}

		if (Type == TEXT("Capsule"))
		{
			if (!bHaveBox) { R.Refuse(true, TEXT("Capsule:noBoxMinMax")); return; }
			const FVector Half = (BMax - BMin) * 0.5;
			// DERIVE the long axis from the dominant extent instead of hardcoding it. Rockstar's
			// capsules are Y-dominant in 82 of 82 sampled bounds and ExportYdr writes Z-dominant, so
			// a hardcoded axis would be wrong for one of the two lanes; the data says which it is.
			int32 Axis = 0;
			if (Half.Y > Half[Axis]) { Axis = 1; }
			if (Half.Z > Half[Axis]) { Axis = 2; }
			const double A = Half[(Axis + 1) % 3];
			const double B = Half[(Axis + 2) % 3];
			if (A <= 0.0 || B <= 0.0 || FMath::Abs(A - B) > FMath::Max(1e-3, 0.01 * FMath::Max(A, B)))
			{
				// A capsule's cross-section is a circle by definition. Two different short extents
				// mean this is not the shape the type claims, and picking one of them would invent a
				// radius.
				R.Refuse(true, TEXT("Capsule:nonCircularCrossSection"));
				return;
			}
			const double Rad = (A + B) * 0.5;
			const double HalfLong = Half[Axis];
			if (HalfLong + 1e-4 < Rad)
			{
				R.Refuse(true, TEXT("Capsule:shorterThanRadius"));
				return;
			}
			FVector AxisLocal(ForceInitToZero);
			AxisLocal[Axis] = 1.0;
			const FVector AxisWorld = World.TransformVectorNoScale(ToUeDir(AxisLocal)).GetSafeNormal();
			FKSphylElem E;
			E.Center = World.TransformPosition(ToUePos((BMin + BMax) * 0.5));
			// FKSphylElem is Z-aligned; rotate its Z onto the capsule's actual axis.
			E.Rotation = FQuat::FindBetweenNormals(FVector::ZAxisVector, AxisWorld).Rotator();
			E.Radius = (float)(Rad * 100.0);
			// Length is the LINE SEGMENT only - UE adds Radius at each end.
			E.Length = (float)(FMath::Max(0.0, HalfLong - Rad) * 2.0 * 100.0);
			R.Agg.SphylElems.Add(E);
			++R.PrimitivesImported;
			return;
		}

		if (Type == TEXT("Geometry") || Type == TEXT("GeometryBVH"))
		{
			ParseGeometryBound(Node, World, Type, R);
			return;
		}

		if (Type == TEXT("Cylinder"))
		{
			// UE's FKAggregateGeom has box, sphere, sphyl, convex, tapered capsule and level set -
			// no cylinder. The nearest shapes all change contact behaviour: a sphyl rounds the caps,
			// a convex N-gon facets the barrel. Both would be a SILENT substitution of a shape the
			// user never authored, so this refuses and says so. 240 of ~3,900 sampled child bounds
			// are cylinders, which is why it does not gate.
			R.Refuse(false, TEXT("Cylinder:noUeEquivalent"));
			return;
		}

		// ⛔ A CATALOGUE IS A LOWER BOUND. The seven types above are what a 900-ydr + 800-ybn census
		// happened to contain; "never observed" describes that sample, not the format. An unseen type
		// string is the format moving underneath the tool, it costs ZERO on today's corpus, and it is
		// exactly what a gate is for - so it counts as MALFORMED and reddens the verdict.
		R.Refuse(true, FString::Printf(TEXT("unknownType:%s"),
			Type.IsEmpty() ? TEXT("<none>") : *Type));
	}

	// Entry point: read a <Bounds> node into R. Returns false if there was no <Bounds> at all,
	// which is the ordinary case for 82.7% of the corpus's drawables and is NOT an error.
	static bool ImportBounds(const FXmlNode* DrawableRoot, FResult& R)
	{
		const FXmlNode* B = DrawableRoot ? DrawableRoot->FindChildNode(TEXT("Bounds")) : nullptr;
		if (!B) { return false; }
		ParseBound(B, FTransform::Identity, 0, R);
		return true;
	}

	// Build the separate UStaticMesh that carries the RAGE collision triangles. UE routes complex
	// collision through UStaticMesh::ComplexCollisionMesh, so the triangles cannot live on the render
	// mesh itself without overwriting the geometry the user sees.
	static UStaticMesh* BuildCollisionMesh(const FResult& R, const FString& DestFolder,
	                                       const FString& MeshName, FString& Why)
	{
		if (R.Indices.Num() < 3) { Why = TEXT("no triangles"); return nullptr; }
		const FString ColName = MeshName + TEXT("_col");
		const FString PackageName = DestFolder / ColName;
		if (!FPackageName::IsValidLongPackageName(PackageName)) { Why = TEXT("bad package name"); return nullptr; }
		UPackage* Pkg = CreatePackage(*PackageName);
		if (!Pkg) { Why = TEXT("CreatePackage failed"); return nullptr; }
		UStaticMesh* Col = NewObject<UStaticMesh>(Pkg, FName(*ColName), RF_Public | RF_Standalone);
		if (!Col) { Why = TEXT("NewObject failed"); return nullptr; }
		Col->SetNumSourceModels(0);
		Col->GetStaticMaterials().Empty();
		Col->AddSourceModel();
		FStaticMeshSourceModel& SM = Col->GetSourceModel(0);
		SM.BuildSettings.bRecomputeNormals = true;
		SM.BuildSettings.bRecomputeTangents = false;
		SM.BuildSettings.bGenerateLightmapUVs = false;
		FMeshDescription* MD = Col->CreateMeshDescription(0);
		if (!MD) { Why = TEXT("CreateMeshDescription failed"); return nullptr; }
		FStaticMeshAttributes Attr(*MD);
		Attr.Register();
		TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
		TPolygonGroupAttributesRef<FName> Slots = Attr.GetPolygonGroupMaterialSlotNames();
		const FPolygonGroupID Group = MD->CreatePolygonGroup();
		Slots[Group] = FName(TEXT("collision"));
		TArray<FVertexID> VIDs;
		VIDs.Reserve(R.Verts.Num());
		for (const FVector3f& P : R.Verts)
		{
			const FVertexID V = MD->CreateVertex();
			Pos[V] = P;
			VIDs.Add(V);
		}
		int32 Made = 0;
		for (int32 k = 0; k + 2 < R.Indices.Num(); k += 3)
		{
			TArray<FVertexInstanceID> Inst;
			for (int32 j = 0; j < 3; ++j)
			{
				Inst.Add(MD->CreateVertexInstance(VIDs[R.Indices[k + j]]));
			}
			MD->CreatePolygon(Group, Inst);
			++Made;
		}
		if (Made == 0) { Why = TEXT("no polygons created"); return nullptr; }
		Col->CommitMeshDescription(0);
		Col->GetStaticMaterials().Add(
			FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), FName(TEXT("collision"))));
		Col->Build(true);
		Col->PostEditChange();
		// ⛔ BLOCK ON THE ASYNC BUILD BEFORE HANDING THIS MESH BACK, AND DO IT *AFTER*
		// PostEditChange. Build() only QUEUES the compile; the caller then assigns this mesh as
		// ComplexCollisionMesh and cooks the render mesh's physics, which calls
		// ContainsPhysicsTriMeshData -> SectionHasCollisionEnabled -> SectionInfoMap on THIS mesh
		// while it is still compiling. Measured, not theorised: the trimesh case printed
		// `"ok":true` and still exited 1, on `Handled ensure ... Accessing property SectionInfoMap of
		// the StaticMesh while it is still being built asynchronously` (StaticMesh.cpp:4790) - the
		// verdict and the exit code disagreeing, which is the one thing a verdict must never do.
		// ⛔ AND THE FIRST FIX WAS ONE LINE TOO EARLY: flushing before PostEditChange still ensured,
		// because PostEditChange RE-QUEUES the build. Order is load-bearing here.
		// Targeted rather than FinishAllCompilation: this path runs on ~17% of a batch's files and
		// flushing every pending asset each time would be a large avoidable stall.
		FStaticMeshCompilingManager::Get().FinishCompilation({ Col });
		Pkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Col);
		return Col;
	}

	// The JSON fragment every import lane appends. Kept in ONE place so the drawable lane and any
	// future .ybn lane cannot drift into reporting the same numbers under different names.
	static FString VerdictJson(const FResult& R, const FString& ColAssetPath)
	{
		FString Reasons;
		TArray<FString> Sorted = R.Reasons.Array();
		Sorted.Sort();
		for (int32 i = 0; i < Sorted.Num(); ++i)
		{
			Reasons += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *Sorted[i]);
		}
		return FString::Printf(
			TEXT("\"collisionBoundsSeen\":%d,\"collisionPrimitivesImported\":%d,")
			TEXT("\"collisionMeshesImported\":%d,\"collisionBoundsUnmapped\":%d,")
			TEXT("\"collisionBoundsMalformed\":%d,\"collisionPolysDropped\":%d,")
			TEXT("\"collisionTriangles\":%d,\"collisionTrisOutOfRange\":%d,")
			TEXT("\"collisionTrisDegenerate\":%d,\"collisionMeshAsset\":\"%s\",")
			TEXT("\"collisionReasons\":[%s]"),
			R.BoundsSeen, R.PrimitivesImported, R.MeshesImported, R.BoundsUnmapped,
			R.BoundsMalformed, R.PolysDropped, R.Indices.Num() / 3, R.TrisOutOfRange,
			R.TrisDegenerate, *ColAssetPath, *Reasons);
	}
}

FString URudeToolset::ExportTexture(const FString& TexturePath, const FString& OutPngPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *TexturePath);
	if (!Tex)
	{
		return Fail(TEXT("Texture2D not found"));
	}
#if WITH_EDITORONLY_DATA
	FTextureSource& Src = Tex->Source;
	if (!Src.IsValid())
	{
		return Fail(TEXT("texture has no editor source data"));
	}
	const int32 W = Src.GetSizeX();
	const int32 H = Src.GetSizeY();
	const ETextureSourceFormat Fmt = Src.GetFormat();

	TArray64<uint8> Mip;
	if (!Src.GetMipData(Mip, 0, 0, 0, nullptr))
	{
		return Fail(TEXT("GetMipData failed"));
	}

	// Normalise to BGRA8 for the PNG wrapper (UE stores most source as BGRA8)
	TArray<uint8> BGRA;
	BGRA.SetNumUninitialized(W * H * 4);
	if (Fmt == TSF_BGRA8 || Fmt == TSF_BGRE8)
	{
		FMemory::Memcpy(BGRA.GetData(), Mip.GetData(), FMath::Min<int64>(Mip.Num(), BGRA.Num()));
	}
	else if (Fmt == TSF_G8)
	{
		for (int32 i = 0; i < W * H; ++i)
		{
			const uint8 G = Mip[i];
			BGRA[i * 4 + 0] = G; BGRA[i * 4 + 1] = G; BGRA[i * 4 + 2] = G; BGRA[i * 4 + 3] = 255;
		}
	}
	else if (Fmt == TSF_G16)
	{
		const uint16* Src16 = reinterpret_cast<const uint16*>(Mip.GetData());
		for (int32 i = 0; i < W * H; ++i)
		{
			const uint8 G = (uint8)(Src16[i] >> 8);
			BGRA[i * 4 + 0] = G; BGRA[i * 4 + 1] = G; BGRA[i * 4 + 2] = G; BGRA[i * 4 + 3] = 255;
		}
	}
	else
	{
		return Fail(FString::Printf(TEXT("unsupported source format %d (want BGRA8/G8/G16)"), (int32)Fmt));
	}

	IImageWrapperModule& IWM =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
	if (!Png.IsValid() || !Png->SetRaw(BGRA.GetData(), BGRA.Num(), W, H, ERGBFormat::BGRA, 8))
	{
		return Fail(TEXT("PNG wrap failed"));
	}
	const TArray64<uint8>& Out = Png->GetCompressed();
	if (!FFileHelper::SaveArrayToFile(TArray<uint8>(Out.GetData(), Out.Num()), *OutPngPath))
	{
		return Fail(TEXT("write PNG failed"));
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"pngPath\":\"%s\",\"width\":%d,\"height\":%d}"), *OutPngPath, W, H);
#else
	return Fail(TEXT("editor-only"));
#endif
}

FString URudeToolset::ExportYbn(const FString& AssetPath, const FString& OutXmlPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		return Fail(TEXT("StaticMesh not found"));
	}
	const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
	if (!MeshDesc)
	{
		return Fail(TEXT("no MeshDescription on LOD0"));
	}
	FStaticMeshConstAttributes Attributes(*MeshDesc);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

	// Merge ALL triangles into one collision soup, welded by position (gta space)
	TArray<FVector3f> Verts;
	TArray<int32> Indices;
	TMap<FString, int32> Weld;
	for (const FTriangleID TriID : MeshDesc->Triangles().GetElementIDs())
	{
		for (const FVertexID VID : MeshDesc->GetTriangleVertices(TriID))
		{
			const FVector3f P = Positions[VID];
			// inverse RUDE transform: cm->m, Y mirror
			const FVector3f G(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f);
			const FString Key = FString::Printf(TEXT("%.4f,%.4f,%.4f"), G.X, G.Y, G.Z);
			int32 Idx;
			if (const int32* F = Weld.Find(Key)) { Idx = *F; }
			else { Idx = Verts.Num(); Verts.Add(G); Weld.Add(Key, Idx); }
			Indices.Add(Idx);
		}
	}
	if (Verts.Num() == 0 || Indices.Num() < 3)
	{
		return Fail(TEXT("no collision geometry"));
	}

	FVector3f BMin(FLT_MAX), BMax(-FLT_MAX);
	for (const FVector3f& V : Verts) { BMin = BMin.ComponentMin(V); BMax = BMax.ComponentMax(V); }
	const FVector3f Center = (BMin + BMax) * 0.5f;
	const float Radius = (BMax - Center).Size();

	auto Header = [&](const FString& Ind, float Margin)
	{
		FString H;
		H += FString::Printf(TEXT("%s<BoxMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *Ind, BMin.X, BMin.Y, BMin.Z);
		H += FString::Printf(TEXT("%s<BoxMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *Ind, BMax.X, BMax.Y, BMax.Z);
		H += FString::Printf(TEXT("%s<BoxCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *Ind, Center.X, Center.Y, Center.Z);
		H += FString::Printf(TEXT("%s<SphereCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *Ind, Center.X, Center.Y, Center.Z);
		H += FString::Printf(TEXT("%s<SphereRadius value=\"%f\" />\n"), *Ind, Radius);
		H += FString::Printf(TEXT("%s<Margin value=\"%f\" />\n"), *Ind, Margin);
		H += FString::Printf(TEXT("%s<Volume value=\"1\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<Inertia x=\"1\" y=\"1\" z=\"1\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<MaterialIndex value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<MaterialColourIndex value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<ProceduralID value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<RoomID value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<PedDensity value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<UnkFlags value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<PolyFlags value=\"0\" />\n"), *Ind);
		H += FString::Printf(TEXT("%s<UnkType value=\"1\" />\n"), *Ind);
		return H;
	};

	FString Xml = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<BoundsFile>\n");
	Xml += TEXT(" <Bounds type=\"Composite\">\n");
	Xml += Header(TEXT("  "), 0.f);
	Xml += TEXT("  <Children>\n   <Item type=\"GeometryBVH\">\n");
	Xml += Header(TEXT("    "), 0.005f);
	Xml += TEXT("    <CompositeTransform>\n     1 0 0 0\n     0 1 0 0\n     0 0 1 0\n     0 0 0 1\n    </CompositeTransform>\n");
	Xml += TEXT("    <CompositeFlags1>MAP_WEAPON, MAP_DYNAMIC, MAP_ANIMAL, MAP_COVER, MAP_VEHICLE</CompositeFlags1>\n");
	Xml += TEXT("    <CompositeFlags2>VEHICLE_NOT_BVH, VEHICLE_BVH, PED, RAGDOLL, ANIMAL, ANIMAL_RAGDOLL, OBJECT, PLANT, PROJECTILE, EXPLOSION, FORKLIFT_FORKS, TEST_WEAPON, TEST_CAMERA, TEST_AI, TEST_SCRIPT, TEST_VEHICLE_WHEEL, GLASS</CompositeFlags2>\n");
	Xml += FString::Printf(TEXT("    <GeometryCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Center.X, Center.Y, Center.Z);
	Xml += TEXT("    <UnkFloat1 value=\"7.62962742E-08\" />\n    <UnkFloat2 value=\"0.0025\" />\n");
	Xml += TEXT("    <Materials>\n     <Item>\n      <Type value=\"0\" />\n      <ProceduralID value=\"0\" />\n      <RoomID value=\"0\" />\n      <PedDensity value=\"0\" />\n      <Flags>NONE</Flags>\n      <MaterialColourIndex value=\"0\" />\n      <Unk value=\"0\" />\n     </Item>\n    </Materials>\n");
	// Vertices are relative to GeometryCenter
	Xml += TEXT("    <Vertices>\n");
	for (const FVector3f& V : Verts)
	{
		Xml += FString::Printf(TEXT("     %f, %f, %f\n"), V.X - Center.X, V.Y - Center.Y, V.Z - Center.Z);
	}
	Xml += TEXT("    </Vertices>\n    <Polygons>\n");
	for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
	{
		Xml += FString::Printf(TEXT("     <Triangle m=\"0\" v1=\"%d\" v2=\"%d\" v3=\"%d\" f1=\"0\" f2=\"0\" f3=\"0\" />\n"),
			Indices[i], Indices[i + 1], Indices[i + 2]);
	}
	Xml += TEXT("    </Polygons>\n   </Item>\n  </Children>\n </Bounds>\n</BoundsFile>\n");

	if (!FFileHelper::SaveStringToFile(Xml, *OutXmlPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write output file"));
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"xmlPath\":\"%s\",\"vertices\":%d,\"triangles\":%d}"),
		*OutXmlPath, Verts.Num(), Indices.Num() / 3);
}

FString URudeToolset::ImportYtd(const FString& XmlPath, const FString& PixelFolder,
                                const FString& DestFolder)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("TextureDictionary"))
	{
		return Fail(TEXT("root is not <TextureDictionary>"));
	}

	FString TxdName = FPaths::GetBaseFilename(XmlPath);
	TxdName.RemoveFromEnd(TEXT(".ytd"));

	IImageWrapperModule& ImageWrapper =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	int32 Imported = 0;
	int32 InvalidNames = 0;
	// ⛔ THREE SILENT SUBSTITUTIONS, NOW COUNTED (2026-08-04). Each one is latent in today's corpus
	// and each one is invisible if it ever stops being latent - which is the definition of the bug
	// class this file keeps paying for.
	//   Declared        - items the manifest actually offered. Without it, "imported" cannot be read:
	//                     imported 0 out of 0 (43 of 40,077 shared manifests declare no texture at
	//                     all) is a no-op, imported 0 out of 40 is a total loss, and the verdict
	//                     spelled them identically.
	//   ItemsWithoutName- an item with no <Name> was dropped by a bare `continue`; not even
	//                     "declared" saw it. MEASURED over the whole resolved corpus (82,241
	//                     manifests / 428,210 items): 0 today.
	//   UsageDefaulted  - a MISSING <Usage> silently becomes DIFFUSE, which sets CompressionSettings,
	//                     SRGB and LODGroup. A normal map that arrives without <Usage> is imported as
	//                     an sRGB colour texture and renders as garbage lighting with every counter
	//                     reading clean. MEASURED corpus-wide: 0 items lack <Usage> today, so this is
	//                     a tripwire on a converter regression, not a live loss.
	//   UsageUnknown    - <Usage> present but outside the vocabulary this function branches on
	//                     (DIFFUSE/NORMAL/SPECULAR). MEASURED corpus-wide: UNKNOWN 24,458 + TERRAIN
	//                     2,041 + FENCE 234 = 26,733 of 428,210 items (6.24%) land in the sRGB
	//                     else-branch. That is defensible and it is now DISCLOSED rather than assumed.
	int32 Declared = 0, ItemsWithoutName = 0, UsageDefaulted = 0, UsageUnknown = 0;
	int32 MissingPixelCount = 0;
	// Where the pixels came from. The corpus ships DDS sidecars ("<stem>/<tex>.dds", the game's
	// own block data behind a DDS header); the PNG path is the older offline bridge, kept as the
	// fallback. A DDS that exists but cannot be decoded (BC7, an unknown layout) is REFUSED and
	// counted under pixelsRefused with its reason - never silently skipped, never guessed.
	int32 PixelsFromDds = 0, PixelsFromPng = 0, PixelsRefused = 0;
	FString RefusedReasons;
	FString Missing;
	for (const FXmlNode* Item : Root->GetChildrenNodes())
	{
		// The dictionary's own trailing fields (BuildAddress, Unknown18/2A/3A - the D10 elements)
		// are siblings of the items, not items: skip them or they count as nameless textures
		// (2026-09-05: 1,808 phantom "itemsWithoutName" over 452 dictionaries = 4 each).
		if (Item->GetTag() != TEXT("Item")) { continue; }
		const FXmlNode* NameNode = Item->FindChildNode(TEXT("Name"));
		if (!NameNode || NameNode->GetContent().TrimStartAndEnd().IsEmpty())
		{
			++ItemsWithoutName;
			continue;
		}
		++Declared;
		const FString TexName = NameNode->GetContent().TrimStartAndEnd();
		const FXmlNode* UsageNode = Item->FindChildNode(TEXT("Usage"));
		if (!UsageNode) { ++UsageDefaulted; }
		const FString Usage = UsageNode ? UsageNode->GetContent().TrimStartAndEnd() : TEXT("DIFFUSE");
		if (Usage != TEXT("DIFFUSE") && Usage != TEXT("NORMAL") && Usage != TEXT("SPECULAR"))
		{
			++UsageUnknown;
		}

		// Pixels: the DDS sidecar first (named by the manifest's <FileName>, else "<name>.dds"),
		// then the offline PNG bridge.
		const FXmlNode* FileNode = Item->FindChildNode(TEXT("FileName"));
		const FString DdsName = FileNode && !FileNode->GetContent().TrimStartAndEnd().IsEmpty()
			? FileNode->GetContent().TrimStartAndEnd() : (TexName + TEXT(".dds"));
		const FString DdsPath = PixelFolder / DdsName;
		const FString PngPath = PixelFolder / (TexName + TEXT(".png"));
		TArray<uint8> PngBytes;
		TArray<uint8> BGRA;
		int32 W = 0, H = 0;
		bool bHavePixels = false;
		// ⛔ The missingPixels ARRAY is capped by nothing and read by no batch; MissingPixelCount is
		// the scalar a caller can actually aggregate (ImportYtdBatch could not sum a JSON list).
		auto NoPixels = [&]()
		{
			++MissingPixelCount;
			if (MissingPixelCount <= 30)
			{
				Missing += FString::Printf(TEXT("%s\"%s\""), Missing.IsEmpty() ? TEXT("") : TEXT(","), *TexName);
			}
		};
		if (FPaths::FileExists(DdsPath))
		{
			FRudeDdsImage Img;
			FString DdsErr;
			if (FRudeDds::Load(DdsPath, Img, DdsErr))
			{
				W = Img.Width; H = Img.Height; BGRA = MoveTemp(Img.Bgra);
				bHavePixels = true;
				++PixelsFromDds;
			}
			else
			{
				++PixelsRefused;
				if (PixelsRefused <= 10)
				{
					RefusedReasons += FString::Printf(TEXT("%s\"%s: %s\""), RefusedReasons.IsEmpty() ? TEXT("") : TEXT(","), *TexName, *DdsErr);
				}
				continue;
			}
		}
		if (!bHavePixels)
		{
			if (!FFileHelper::LoadFileToArray(PngBytes, *PngPath))
			{
				NoPixels();
				continue;
			}
			TSharedPtr<IImageWrapper> Png = ImageWrapper.CreateImageWrapper(EImageFormat::PNG);
			if (!Png.IsValid() || !Png->SetCompressed(PngBytes.GetData(), PngBytes.Num()))
			{
				NoPixels();
				continue;
			}
			if (!Png->GetRaw(ERGBFormat::BGRA, 8, BGRA))
			{
				NoPixels();
				continue;
			}
			W = Png->GetWidth();
			H = Png->GetHeight();
			++PixelsFromPng;
		}

		const FString PackageName = DestFolder / TxdName / TexName;
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			// ⛔ THIS USED TO `continue` IN SILENCE, and that silence cost a whole import cycle
			// (2026-07-30): 1,943 texture dictionaries imported with texturesImported=0 and ok:true
			// on every one. The cause was a DOT in the folder name - a package path segment may not
			// contain '.', UE reserves it to separate package from object - so every texture failed
			// this check and vanished without a word.
			// It is NOT silently sanitised: renaming the caller's asset path behind their back
			// trades one invisible problem for another. Say what was rejected and why.
			++InvalidNames;
			if (InvalidNames <= 5)
			{
				UE_LOG(LogTemp, Warning, TEXT("[RUDE] ImportYtd: '%s' is not a valid package name "
					"- a path segment cannot contain '.'; texture skipped"), *PackageName);
			}
			continue;
		}
		UPackage* Package = CreatePackage(*PackageName);
		// TRUE edit-in-place: reuse the existing object if the package already
		// holds one. NewObject-over-existing displaces the old object and
		// corrupts its bulkdata registration ("invalid payload" save failures,
		// 2026-07-24) - the texture becomes unsaveable.
		UTexture2D* Tex = FindObject<UTexture2D>(Package, *TexName);
		if (!Tex)
		{
			Tex = NewObject<UTexture2D>(Package, FName(*TexName), RF_Public | RF_Standalone);
		}
		Tex->PreEditChange(nullptr);
		Tex->Source.Init(W, H, 1, 1, TSF_BGRA8, BGRA.GetData());

		// Semantics from the ytd's own Usage - the thing generic importers can't know
		if (Usage == TEXT("NORMAL"))
		{
			Tex->CompressionSettings = TC_Normalmap;
			Tex->SRGB = false;
			Tex->LODGroup = TEXTUREGROUP_WorldNormalMap;
		}
		else if (Usage == TEXT("SPECULAR"))
		{
			Tex->CompressionSettings = TC_Default;
			Tex->SRGB = false;
		}
		else
		{
			Tex->CompressionSettings = TC_Default;
			Tex->SRGB = true;
		}

		Tex->UpdateResource();
		Tex->PostEditChange();
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		++Imported;
	}

	// ⛔ ok IS COMPUTED, AND ONLY ON THE TOTAL-LOSS SHAPE (2026-08-04). The 2026-07-30 incident was
	// exactly this: 1,943 dictionaries, every texture rejected by the package-name check, imported=0,
	// and a hardcoded ok:true on every one - so the batch counted 1,943 successes. The gate is
	// "I had work, I rejected all of it, I produced nothing", NOT "invalidNames > 0":
	// MEASURED over the whole resolved corpus (82,241 manifests incl. the 42,164 __embedded
	// siblings), 45 dictionaries carry at least one texture whose NAME is not a legal package
	// segment (78 items - names with a space, e.g. baller2 'generic_ cav_detail_dash'), and only 2
	// manifests have a bad STEM. Failing all 45 would report a 39-of-40 partial import as a total
	// failure AND drop its 39 textures out of the batch's texturesImported sum, because the batch
	// only aggregates the ok path. A partial loss is a NUMBER (invalidNames), not a failure.
	// Note the second half of the gate: Declared == 0 stays ok - 43 of 40,077 shared manifests
	// genuinely declare no texture, and "nothing to do" is not "nothing worked".
	const bool bTotalLoss = (Declared > 0 && Imported == 0 && InvalidNames > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"txd\":\"%s\",\"declared\":%d,\"imported\":%d,\"invalidNames\":%d,")
		TEXT("\"itemsWithoutName\":%d,\"usageDefaulted\":%d,\"usageUnknown\":%d,")
		TEXT("\"missingPixelCount\":%d,\"missingPixels\":[%s],")
		TEXT("\"pixelsFromDds\":%d,\"pixelsFromPng\":%d,\"pixelsRefused\":%d,\"pixelsRefusedReasons\":[%s]}"),
		bTotalLoss ? TEXT("false") : TEXT("true"),
		*TxdName, Declared, Imported, InvalidNames, ItemsWithoutName, UsageDefaulted, UsageUnknown,
		MissingPixelCount, *Missing, PixelsFromDds, PixelsFromPng, PixelsRefused, *RefusedReasons);
}

FString URudeToolset::ExportYdr(const FString& AssetPath, const FString& OutXmlPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		return Fail(TEXT("StaticMesh not found"));
	}
	const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
	if (!MeshDesc)
	{
		return Fail(TEXT("no MeshDescription on LOD0"));
	}
	FStaticMeshConstAttributes Attributes(*MeshDesc);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesConstRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesConstRef<FName> GroupSlots = Attributes.GetPolygonGroupMaterialSlotNames();

	const FString MeshName = Mesh->GetName();

	// Per polygon group: gather welded (pos,normal,uv) vertices + index list
	struct FOutGeo
	{
		FString Preset = TEXT("default");
		FString Diffuse, Normal, Specular;
		int32 Bucket = 0;                    // RAGE draw bucket: 0 opaque, 1 alpha, 2 decal, 3 cutout
		TArray<FVector3f> Pos;
		TArray<FVector3f> Nrm;
		TArray<FVector2f> UV;
		TArray<int32> Indices;
	};
	TArray<FOutGeo> OutGeos;
	int32 BucketUnrecovered = 0;

	for (const FPolygonGroupID GroupID : MeshDesc->PolygonGroups().GetElementIDs())
	{
		FOutGeo Geo;

		// preset from slot name "<preset>__<n>" (RUDE-imported). A raw non-RUDE
		// mesh (e.g. a Fab import) has an arbitrary material name with no "__"
		// convention (e.g. "lambert1") which is NOT a valid RAGE shader preset -
		// fall back to "default" so the drawable is valid. The full UE-material
		// -> RAGE-preset + PBR->RAGE-texture mapping is the P2 material lane.
		FString SlotName = GroupSlots[GroupID].ToString();
		int32 Sep = SlotName.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const bool bRudeSlot = (Sep != INDEX_NONE);
		Geo.Preset = bRudeSlot ? SlotName.Left(Sep) : TEXT("default");

		// texture names from the slot's RUDE MaterialInstance, if any
		// (manual scan: we author MaterialSlotName, not ImportedMaterialSlotName)
		int32 SlotIdx = INDEX_NONE;
		for (int32 i = 0; i < Mesh->GetStaticMaterials().Num(); ++i)
		{
			if (Mesh->GetStaticMaterials()[i].MaterialSlotName == GroupSlots[GroupID])
			{
				SlotIdx = i;
				break;
			}
		}
		if (Mesh->GetStaticMaterials().IsValidIndex(SlotIdx))
		{
			if (const UMaterialInstanceConstant* MIC =
				Cast<UMaterialInstanceConstant>(Mesh->GetStaticMaterials()[SlotIdx].MaterialInterface))
			{
				UTexture* T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Diffuse")), T) && T)
				{
					Geo.Diffuse = T->GetName();
				}
				T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Normal")), T) && T)
				{
					Geo.Normal = T->GetName();
				}
				T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Specular")), T) && T)
				{
					Geo.Specular = T->GetName();
				}
				// FIXED 2026-08-03 - <RenderBucket> was the literal 0 on every shader this exporter
				// emitted. ImportYdr treats RenderBucket as authoritative for master selection
				// ("preset names lie" - :1961), so the UE -> ydr half of the round trip flattened
				// every alpha, decal and cutout shader to opaque and returned the same ok:true it
				// returns for a perfect export: glass and windows turn opaque, decals z-fight
				// instead of offsetting, cutout foliage renders as solid quads. MEASURED over a
				// 2,500-file random sample of the resolved base-game ydr corpus: bucket != 0 on
				// 3,326 of 9,389 shaders = 35.4%, and on 1,417 of 2,500 FILES = 56.7%
				// (histogram {0:6063, 2:2108, 3:781, 1:423, 6:14} - note bucket 6 exists and is
				// outside the {0,1,2,3} the code's own comment documents).
				// RECOVERY IS EXACT, NOT SNIFFED: the generated masters encode the bucket in their
				// own asset name as the "_b<N>" suffix (FRudeMasterSpec::Key at :290 builds
				// "M_RUDE_<flags>_b<bucket>"), and that value came from the file's RenderBucket on
				// import (:1942). So parsing the suffix returns the original number rather than
				// guessing from substrings like "Decal"/"Alpha", which would invent a bucket for
				// the name-routed masters (M_RUDE_Foliage/Terrain/Detail/Water carry no bucket at
				// all, and M_RUDE_DecalGeo is also reached by a preset merely NAMED "*decal*").
				// Anything without the suffix stays 0 and is COUNTED - see renderBucketUnrecovered.
				bool bGotBucket = false;
				if (const UMaterialInterface* Parent = MIC->Parent)
				{
					const FString PN = Parent->GetName();
					int32 BPos = INDEX_NONE;
					if (PN.FindLastChar(TEXT('b'), BPos) && BPos > 0 && PN[BPos - 1] == TEXT('_')
						&& BPos + 1 < PN.Len())
					{
						const FString Digits = PN.Mid(BPos + 1);
						if (Digits.IsNumeric())
						{
							Geo.Bucket = FCString::Atoi(*Digits);
							bGotBucket = true;
						}
					}
				}
				if (!bGotBucket) { ++BucketUnrecovered; }
			}
			else
			{
				++BucketUnrecovered;   // slot is not a MaterialInstanceConstant
			}
		}
		else
		{
			++BucketUnrecovered;       // no material slot matched this polygon group
		}

		// Non-RUDE slot: pick the preset from which textures are present, so a
		// textured Fab mesh gets a normal-mapped shader instead of bare "default".
		if (!bRudeSlot)
		{
			if (!Geo.Normal.IsEmpty())        { Geo.Preset = TEXT("normal_spec"); }
			else if (!Geo.Diffuse.IsEmpty())  { Geo.Preset = TEXT("spec"); }
			// else stays "default"
		}

		// weld corners into unique vertices per (vertexID, normal, uv)
		TMap<FString, int32> Weld;
		for (const FPolygonID PolyID : MeshDesc->GetPolygonGroupPolygonIDs(GroupID))
		{
			for (const FTriangleID TriID : MeshDesc->GetPolygonTriangles(PolyID))
			{
				for (const FVertexInstanceID Inst : MeshDesc->GetTriangleVertexInstances(TriID))
				{
					const FVertexID VID = MeshDesc->GetVertexInstanceVertex(Inst);
					const FVector3f P = Positions[VID];
					const FVector3f N = InstNormals[Inst];
					const FVector2f UV = InstUVs.Get(Inst, 0);
					const FString Key = FString::Printf(TEXT("%d|%.3f,%.3f,%.3f|%.4f,%.4f"),
						VID.GetValue(), N.X, N.Y, N.Z, UV.X, UV.Y);
					int32 Index;
					if (const int32* Found = Weld.Find(Key))
					{
						Index = *Found;
					}
					else
					{
						Index = Geo.Pos.Num();
						// inverse RUDE transform: cm -> meters, Y mirror back
						Geo.Pos.Add(FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f));
						Geo.Nrm.Add(FVector3f(N.X, -N.Y, N.Z));
						Geo.UV.Add(UV);
						Weld.Add(Key, Index);
					}
					Geo.Indices.Add(Index);   // winding: pass-through (involution)
				}
			}
		}
		if (Geo.Pos.Num() > 0 && Geo.Indices.Num() >= 3)
		{
			OutGeos.Add(MoveTemp(Geo));
		}
	}
	if (OutGeos.Num() == 0)
	{
		return Fail(TEXT("no polygon groups with geometry"));
	}

	// bounds in gta space
	FVector3f BMin(FLT_MAX), BMax(-FLT_MAX);
	for (const FOutGeo& G : OutGeos)
	{
		for (const FVector3f& P : G.Pos)
		{
			BMin = BMin.ComponentMin(P);
			BMax = BMax.ComponentMax(P);
		}
	}
	const FVector3f Center = (BMin + BMax) * 0.5f;
	const float Radius = (BMax - Center).Size();

	FString Xml;
	Xml += TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Drawable>\n");
	Xml += FString::Printf(TEXT(" <Name>%s</Name>\n"), *MeshName);
	Xml += FString::Printf(TEXT(" <BoundingSphereCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"),
		Center.X, Center.Y, Center.Z);
	Xml += FString::Printf(TEXT(" <BoundingSphereRadius value=\"%f\" />\n"), Radius);
	Xml += FString::Printf(TEXT(" <BoundingBoxMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"), BMin.X, BMin.Y, BMin.Z);
	Xml += FString::Printf(TEXT(" <BoundingBoxMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"), BMax.X, BMax.Y, BMax.Z);
	Xml += TEXT(" <LodDistHigh value=\"9998\" />\n <LodDistMed value=\"9998\" />\n");
	Xml += TEXT(" <LodDistLow value=\"9998\" />\n <LodDistVlow value=\"9998\" />\n");
	Xml += TEXT(" <FlagsHigh value=\"1\" />\n <FlagsMed value=\"0\" />\n");
	Xml += TEXT(" <FlagsLow value=\"0\" />\n <FlagsVlow value=\"0\" />\n");

	// ShaderGroup: one shader per geometry (census-standard param block)
	Xml += TEXT(" <ShaderGroup>\n  <Shaders>\n");
	for (const FOutGeo& G : OutGeos)
	{
		Xml += TEXT("   <Item>\n");
		Xml += FString::Printf(TEXT("    <Name>%s</Name>\n"), *G.Preset);
		Xml += FString::Printf(TEXT("    <FileName>%s.sps</FileName>\n"), *G.Preset);
		Xml += FString::Printf(TEXT("    <RenderBucket value=\"%d\" />\n    <Parameters>\n"), G.Bucket);
		if (!G.Diffuse.IsEmpty())
		{
			Xml += FString::Printf(TEXT("     <Item name=\"DiffuseSampler\" type=\"Texture\">\n      <Name>%s</Name>\n     </Item>\n"), *G.Diffuse);
		}
		if (!G.Normal.IsEmpty())
		{
			Xml += FString::Printf(TEXT("     <Item name=\"BumpSampler\" type=\"Texture\">\n      <Name>%s</Name>\n     </Item>\n"), *G.Normal);
		}
		if (!G.Specular.IsEmpty())
		{
			Xml += FString::Printf(TEXT("     <Item name=\"SpecSampler\" type=\"Texture\">\n      <Name>%s</Name>\n     </Item>\n"), *G.Specular);
		}
		Xml += TEXT("     <Item name=\"specularFresnel\" type=\"Vector\" x=\"0.9\" y=\"0\" z=\"0\" w=\"0\" />\n");
		Xml += TEXT("     <Item name=\"specularFalloffMult\" type=\"Vector\" x=\"40\" y=\"0\" z=\"0\" w=\"0\" />\n");
		Xml += TEXT("     <Item name=\"specularIntensityMult\" type=\"Vector\" x=\"0.3\" y=\"0\" z=\"0\" w=\"0\" />\n");
		if (!G.Normal.IsEmpty())
		{
			Xml += TEXT("     <Item name=\"bumpiness\" type=\"Vector\" x=\"1\" y=\"0\" z=\"0\" w=\"0\" />\n");
		}
		Xml += TEXT("     <Item name=\"wetnessMultiplier\" type=\"Vector\" x=\"1\" y=\"0\" z=\"0\" w=\"0\" />\n");
		Xml += TEXT("     <Item name=\"useTessellation\" type=\"Vector\" x=\"0\" y=\"0\" z=\"0\" w=\"0\" />\n");
		Xml += TEXT("     <Item name=\"HardAlphaBlend\" type=\"Vector\" x=\"1\" y=\"0\" z=\"0\" w=\"0\" />\n");
		Xml += TEXT("    </Parameters>\n   </Item>\n");
	}
	Xml += TEXT("  </Shaders>\n </ShaderGroup>\n");

	// Geometry
	int32 TotalVerts = 0, TotalTris = 0;
	Xml += TEXT(" <DrawableModelsHigh>\n  <Item>\n   <RenderMask value=\"255\" />\n");
	Xml += TEXT("   <Flags value=\"0\" />\n   <HasSkin value=\"0\" />\n");
	Xml += TEXT("   <BoneIndex value=\"0\" />\n   <Unknown1 value=\"0\" />\n   <Geometries>\n");
	for (int32 GeoIdx = 0; GeoIdx < OutGeos.Num(); ++GeoIdx)
	{
		const FOutGeo& G = OutGeos[GeoIdx];
		FVector3f GMin(FLT_MAX), GMax(-FLT_MAX);
		for (const FVector3f& P : G.Pos)
		{
			GMin = GMin.ComponentMin(P);
			GMax = GMax.ComponentMax(P);
		}
		Xml += TEXT("    <Item>\n");
		Xml += FString::Printf(TEXT("     <ShaderIndex value=\"%d\" />\n"), GeoIdx);
		Xml += FString::Printf(TEXT("     <BoundingBoxMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"), GMin.X, GMin.Y, GMin.Z);
		Xml += FString::Printf(TEXT("     <BoundingBoxMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"), GMax.X, GMax.Y, GMax.Z);
		Xml += TEXT("     <VertexBuffer>\n      <Flags value=\"89\" />\n");
		Xml += TEXT("      <Layout type=\"GTAV1\">\n       <Position />\n       <Normal />\n       <Colour0 />\n       <TexCoord0 />\n      </Layout>\n");
		Xml += TEXT("      <Data>\n");
		for (int32 V = 0; V < G.Pos.Num(); ++V)
		{
			Xml += FString::Printf(TEXT("       %f %f %f   %f %f %f   255 255 255 255   %f %f\n"),
				G.Pos[V].X, G.Pos[V].Y, G.Pos[V].Z,
				G.Nrm[V].X, G.Nrm[V].Y, G.Nrm[V].Z,
				G.UV[V].X, G.UV[V].Y);
		}
		Xml += TEXT("      </Data>\n     </VertexBuffer>\n     <IndexBuffer>\n      <Data>\n");
		for (int32 I = 0; I < G.Indices.Num(); I += 3)
		{
			Xml += FString::Printf(TEXT("       %d %d %d\n"),
				G.Indices[I], G.Indices[I + 1], G.Indices[I + 2]);
		}
		Xml += TEXT("      </Data>\n     </IndexBuffer>\n    </Item>\n");
		TotalVerts += G.Pos.Num();
		TotalTris += G.Indices.Num() / 3;
	}
	Xml += TEXT("   </Geometries>\n  </Item>\n </DrawableModelsHigh>\n");

	// EMBEDDED collision. CORRECTED 2026-07-24 (Matt in-game + census): prop collision
	// comes from the ydr's EMBEDDED <Bounds> + archetype flag bit 0x20000 - NOT an
	// external physicsDictionary/.ybn (props share NAMED-bound dicts; a standalone
	// bound never matches). Real props embed a Composite of PRIMITIVES (Box/Sphere/
	// Capsule) when simple collision exists, else a per-triangle GeometryBVH. We MIRROR
	// the mesh's AggGeom below (primitives-first, whole-mesh BVH fallback); the BVH
	// path is the rock-wall's in-game-proven structure (shared with ExportYbn).
	// Merge all geometries into one collision soup (gta space) for the BVH fallback.
	TArray<FVector3f> CVerts; TArray<int32> CIdx;
	{
		int32 Off = 0;
		for (const FOutGeo& G : OutGeos)
		{
			for (const FVector3f& P : G.Pos) { CVerts.Add(P); }
			for (int32 I : G.Indices) { CIdx.Add(I + Off); }
			Off += G.Pos.Num();
		}
	}
	auto BHdr = [&](const FString& I, float Margin)
	{
		FString H;
		H += FString::Printf(TEXT("%s<BoxMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *I, BMin.X, BMin.Y, BMin.Z);
		H += FString::Printf(TEXT("%s<BoxMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *I, BMax.X, BMax.Y, BMax.Z);
		H += FString::Printf(TEXT("%s<BoxCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *I, Center.X, Center.Y, Center.Z);
		H += FString::Printf(TEXT("%s<SphereCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), *I, Center.X, Center.Y, Center.Z);
		H += FString::Printf(TEXT("%s<SphereRadius value=\"%f\" />\n"), *I, Radius);
		H += FString::Printf(TEXT("%s<Margin value=\"%f\" />\n"), *I, Margin);
		H += FString::Printf(TEXT("%s<Volume value=\"1\" />\n"), *I);
		H += FString::Printf(TEXT("%s<Inertia x=\"1\" y=\"1\" z=\"1\" />\n"), *I);
		H += FString::Printf(TEXT("%s<MaterialIndex value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<MaterialColourIndex value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<ProceduralID value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<RoomID value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<PedDensity value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<UnkFlags value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<PolyFlags value=\"0\" />\n"), *I);
		H += FString::Printf(TEXT("%s<UnkType value=\"1\" />\n"), *I);
		return H;
	};
	// ----- Collision children (Composite): mirror the mesh's actual collision -----
	// A RAGE Composite child set == UE BodySetup.AggGeom (both = a set of collision
	// primitives - Matt's design). Census-verified field schema
	// (reports/schema_registry.json): Item[Box|Sphere|Capsule] share the SAME fields
	// (BoxMin/Max/Center, SphereCenter/Radius, Margin, Volume, Inertia, Material*,
	// CompositeTransform, CompositeFlags1/2) - the shape is the local AABB inscribed by
	// the <type>, positioned+oriented by CompositeTransform. Convex hulls -> a per-hull
	// GeometryBVH child. No simple collision at all -> one whole-mesh GeometryBVH (the
	// rock-wall's in-game-proven path). Rotation: UE quat -> GTA under the Y-mirror is
	// the PINNED involution gta_quat = (-x, y, -z, w) (ENGINEERING_LOG "Rotation
	// quaternions", Matt-witnessed for entity placement; identical similarity transform).
	const TCHAR* PrimFlags =
		TEXT("    <CompositeFlags1>MAP_WEAPON, MAP_DYNAMIC, MAP_ANIMAL, MAP_COVER, MAP_VEHICLE</CompositeFlags1>\n")
		TEXT("    <CompositeFlags2>VEHICLE_NOT_BVH, VEHICLE_BVH, PED, RAGDOLL, ANIMAL, ANIMAL_RAGDOLL, OBJECT, PLANT, PROJECTILE, EXPLOSION, FORKLIFT_FORKS, TEST_WEAPON, TEST_CAMERA, TEST_AI, TEST_SCRIPT, TEST_VEHICLE_WHEEL, GLASS</CompositeFlags2>\n");

	// 4x4 row-major CompositeTransform from a GTA-space rotation quat + center.
	// UE FMatrix / RAGE bounds are both row-vector (p' = p*M): the rows are the
	// rotated basis vectors (GetAxisX/Y/Z), translation in the last row.
	auto XformRows = [](const FQuat& Q, const FVector& C) -> FString
	{
		const FVector X = Q.GetAxisX(), Y = Q.GetAxisY(), Z = Q.GetAxisZ();
		return FString::Printf(TEXT(
			"    <CompositeTransform>\n     %f %f %f 0\n     %f %f %f 0\n     %f %f %f 0\n     %f %f %f 1\n    </CompositeTransform>\n"),
			X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z, C.X, C.Y, C.Z);
	};
	auto ToGtaQuat = [](const FQuat& Q) { return FQuat(-Q.X, Q.Y, -Q.Z, Q.W); };          // pinned Y-mirror involution
	auto ToGtaPos  = [](const FVector& P) { return FVector(P.X / 100.0, -P.Y / 100.0, P.Z / 100.0); };

	// Box / Sphere / Capsule share one emitter (identical schema; local AABB + xform).
	auto EmitAABBChild = [&](const TCHAR* Type, const FVector& Half, double SphereRad,
	                         const FQuat& Qgta, const FVector& Cgta) -> FString
	{
		FString S = FString::Printf(TEXT("   <Item type=\"%s\">\n"), Type);
		S += FString::Printf(TEXT("    <BoxMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"), -Half.X, -Half.Y, -Half.Z);
		S += FString::Printf(TEXT("    <BoxMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Half.X, Half.Y, Half.Z);
		S += TEXT("    <BoxCenter x=\"0\" y=\"0\" z=\"0\" />\n    <SphereCenter x=\"0\" y=\"0\" z=\"0\" />\n");
		S += FString::Printf(TEXT("    <SphereRadius value=\"%f\" />\n"), SphereRad);
		S += TEXT("    <Margin value=\"0.04\" />\n    <Volume value=\"1\" />\n    <Inertia x=\"1\" y=\"1\" z=\"1\" />\n");
		S += TEXT("    <MaterialIndex value=\"0\" />\n    <MaterialColourIndex value=\"0\" />\n    <ProceduralID value=\"0\" />\n    <RoomID value=\"0\" />\n    <PedDensity value=\"0\" />\n    <UnkFlags value=\"0\" />\n    <PolyFlags value=\"0\" />\n    <UnkType value=\"1\" />\n");
		S += XformRows(Qgta, Cgta);
		S += PrimFlags;
		S += TEXT("   </Item>\n");
		return S;
	};

	// One GeometryBVH child from gta-space verts + triangle indices (convex hulls
	// and the whole-mesh fallback). Verts stored relative to the child's own
	// GeometryCenter (RAGE convention); identity CompositeTransform.
	auto EmitBVHChild = [&](const TArray<FVector3f>& V, const TArray<int32>& I) -> FString
	{
		FVector3f Mn(FLT_MAX), Mx(-FLT_MAX);
		for (const FVector3f& P : V) { Mn = Mn.ComponentMin(P); Mx = Mx.ComponentMax(P); }
		const FVector3f Ctr = (Mn + Mx) * 0.5f;
		const float Rad = (Mx - Ctr).Size();
		FString S = TEXT("   <Item type=\"GeometryBVH\">\n");
		S += FString::Printf(TEXT("    <BoxMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Mn.X, Mn.Y, Mn.Z);
		S += FString::Printf(TEXT("    <BoxMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Mx.X, Mx.Y, Mx.Z);
		S += FString::Printf(TEXT("    <BoxCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Ctr.X, Ctr.Y, Ctr.Z);
		S += FString::Printf(TEXT("    <SphereCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Ctr.X, Ctr.Y, Ctr.Z);
		S += FString::Printf(TEXT("    <SphereRadius value=\"%f\" />\n"), Rad);
		S += TEXT("    <Margin value=\"0.005\" />\n    <Volume value=\"1\" />\n    <Inertia x=\"1\" y=\"1\" z=\"1\" />\n");
		S += TEXT("    <MaterialIndex value=\"0\" />\n    <MaterialColourIndex value=\"0\" />\n    <ProceduralID value=\"0\" />\n    <RoomID value=\"0\" />\n    <PedDensity value=\"0\" />\n    <UnkFlags value=\"0\" />\n    <PolyFlags value=\"0\" />\n    <UnkType value=\"1\" />\n");
		S += TEXT("    <CompositeTransform>\n     1 0 0 0\n     0 1 0 0\n     0 0 1 0\n     0 0 0 1\n    </CompositeTransform>\n");
		S += PrimFlags;
		S += FString::Printf(TEXT("    <GeometryCenter x=\"%f\" y=\"%f\" z=\"%f\" />\n"), Ctr.X, Ctr.Y, Ctr.Z);
		S += TEXT("    <UnkFloat1 value=\"7.62962742E-08\" />\n    <UnkFloat2 value=\"0.0025\" />\n");
		S += TEXT("    <Materials>\n     <Item>\n      <Type value=\"0\" />\n      <ProceduralID value=\"0\" />\n      <RoomID value=\"0\" />\n      <PedDensity value=\"0\" />\n      <Flags>NONE</Flags>\n      <MaterialColourIndex value=\"0\" />\n      <Unk value=\"0\" />\n     </Item>\n    </Materials>\n    <Vertices>\n");
		for (const FVector3f& P : V)
		{
			S += FString::Printf(TEXT("     %f, %f, %f\n"), P.X - Ctr.X, P.Y - Ctr.Y, P.Z - Ctr.Z);
		}
		S += TEXT("    </Vertices>\n    <Polygons>\n");
		for (int32 k = 0; k + 2 < I.Num(); k += 3)
		{
			S += FString::Printf(TEXT("     <Triangle m=\"0\" v1=\"%d\" v2=\"%d\" v3=\"%d\" f1=\"0\" f2=\"0\" f3=\"0\" />\n"),
				I[k], I[k + 1], I[k + 2]);
		}
		S += TEXT("    </Polygons>\n   </Item>\n");
		return S;
	};

	FString Children;
	int32 NumChildren = 0;
	if (UBodySetup* BS = Mesh->GetBodySetup())
	{
		const FKAggregateGeom& Agg = BS->AggGeom;
		for (const FKBoxElem& B : Agg.BoxElems)
		{
			const FVector Half(B.X / 200.0, B.Y / 200.0, B.Z / 200.0);   // full-extent cm -> half-extent m
			Children += EmitAABBChild(TEXT("Box"), Half, Half.Size(),
				ToGtaQuat(B.Rotation.Quaternion()), ToGtaPos(B.Center));
			++NumChildren;
		}
		for (const FKSphereElem& Sp : Agg.SphereElems)
		{
			const double R = Sp.Radius / 100.0;
			Children += EmitAABBChild(TEXT("Sphere"), FVector(R, R, R), R,
				FQuat::Identity, ToGtaPos(Sp.Center));   // sphere: rotation irrelevant
			++NumChildren;
		}
		for (const FKSphylElem& Cap : Agg.SphylElems)
		{
			// UE sphyl is Z-aligned: Radius + Length (cylinder segment, hemispheres extra).
			// Local AABB half-extents = (R, R, L/2 + R). RAGE infers the capsule long axis
			// from the box's dominant extent (Z here). ** Axis mapping pending Matt's
			// in-game check ** - if the capsule reads sideways, swap the local axis order.
			const double R = Cap.Radius / 100.0;
			const double HZ = Cap.Length / 200.0 + R;
			Children += EmitAABBChild(TEXT("Capsule"), FVector(R, R, HZ), HZ,
				ToGtaQuat(Cap.Rotation.Quaternion()), ToGtaPos(Cap.Center));
			++NumChildren;
		}
		for (const FKConvexElem& Cx : Agg.ConvexElems)
		{
			if (Cx.IndexData.Num() < 3 || Cx.VertexData.Num() == 0) { continue; }   // uncooked hull -> skip
			const FTransform T = Cx.GetTransform();
			TArray<FVector3f> V; V.Reserve(Cx.VertexData.Num());
			for (const FVector& P : Cx.VertexData)
			{
				const FVector W = T.TransformPosition(P);   // hull-local -> body space (UE cm)
				V.Add(FVector3f(W.X / 100.f, -W.Y / 100.f, W.Z / 100.f));
			}
			Children += EmitBVHChild(V, Cx.IndexData);
			++NumChildren;
		}
	}
	// ⛔ THE SUBSTITUTION IS NOW COUNTED (2026-08-05, open item #40). This fallback is not a bug -
	// a whole-mesh BVH is the rock-wall's in-game-proven structure - but it was SILENT, and the
	// thing that makes it silent is a gap one lane earlier: RUDE has no ImportYbn/ImportBound/
	// ImportCollision at all (grep, re-measured 08-05: zero hits plugin-wide), so an imported
	// asset's AggGeom is ALWAYS empty and this branch ALWAYS taken. The corpus now ships 13,926
	// .ybn files with no consumer. So every export of an imported prop re-derives its collision
	// from the RENDER mesh - a different shape from the one Rockstar authored, at a different
	// triangle budget - and reported ok:true with no number anywhere saying so.
	// ⇒ collisionFromRenderMesh is a DECLARED GAP, not a failure: it does NOT gate ok, because the
	// substitution is the tool's honest current capability and a gate that fires on every run is a
	// gate nobody reads. What it MUST do is stop a run reading as complete while substituting.
	// Wiring <Bounds> into AggGeom stays registered as expansion (#40); this is its repair half.
	const int32 CollisionFromRenderMesh = (NumChildren == 0) ? 1 : 0;
	const int32 CollisionPrimitives = NumChildren;
	if (NumChildren == 0)
	{
		Children += EmitBVHChild(CVerts, CIdx);   // no simple collision -> exact whole-mesh BVH
	}
	Xml += TEXT(" <Bounds type=\"Composite\">\n");
	Xml += BHdr(TEXT("  "), 0.f);
	Xml += TEXT("  <Children>\n") + Children + TEXT("  </Children>\n </Bounds>\n");

	Xml += TEXT(" <Lights />\n</Drawable>\n");

	if (!FFileHelper::SaveStringToFile(Xml, *OutXmlPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write output file"));
	}
	// renderBucketUnrecovered: shader slots whose draw bucket could not be read back from the
	// material (no MIC, or a parent master that does not carry the "_b<N>" suffix - the name-routed
	// M_RUDE_Foliage/Terrain/Detail/Water/DecalGeo masters do not). Those slots emit bucket 0, which
	// is the old behaviour for EVERY slot; reporting the count is what stops a lossy export from
	// looking identical to a lossless one. A non-zero value here means the round trip is not yet
	// lossless for this mesh.
	// collisionFromRenderMesh 1 = this asset shipped collision RE-DERIVED FROM ITS RENDER MESH
	// because it had no AggGeom (see the block above; open item #40). collisionPrimitives is the
	// count that came from real UE collision primitives instead - the two are exclusive.
	return FString::Printf(
		TEXT("{\"ok\":true,\"xmlPath\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,")
		TEXT("\"renderBucketUnrecovered\":%d,\"collisionPrimitives\":%d,")
		TEXT("\"collisionFromRenderMesh\":%d}"),
		*OutXmlPath, OutGeos.Num(), TotalVerts, TotalTris, BucketUnrecovered,
		CollisionPrimitives, CollisionFromRenderMesh);
}

FString URudeToolset::Ping()
{
	return TEXT("RUDE 0.1.0 - RAGE <-> Unreal Development Environment. Toolset alive.");
}

// ---- THE TEXTURE SCOPE (open items #43 + #21b, 2026-08-05) --------------------------------
// ⛔⛔ ONE DEFECT, SEEN TWICE: texture resolution is keyed on a NAME, and a name is not a
// dictionary. 8,791 of this project's 22,405 distinct texture names live in more than one
// dictionary, so the importer's "pick the lexicographically first candidate" rule was a guess on
// 95.8% of all binds - and it is INVISIBLE to missingTextures, because a wrong pick still binds
// *a* texture. That is why it is a correctness defect, not a cosmetic one. (#21b is the same
// join seen from the producer side.)
//
// A scope is the ONLY thing that turns a guess into an answer. This struct carries every signal
// the caller is able to prove, and the ladder in ImportDrawableNode::FindTexture consumes them
// in a DELIBERATE PRECEDENCE, strongest evidence first:
//
//   1. the drawable's OWN "<mesh>__embedded" dictionary   - exact BY CONSTRUCTION: that
//      dictionary was carved out of this very drawable.
//   2. ArchetypeTxd + its +hi/+hidr/+hidd siblings        - the dictionary the archetype
//      DECLARES for this asset (<textureDictionary> in the ytyp). RAGE's own first answer.
//   3. ParentTxdChain, NEAREST ancestor first             - RAGE's own FALLBACK: when an asset's
//      txd lacks a texture, the engine walks the parent chain declared in gtxd's
//      CMapParentTxds. Rockstar's data, read out of the game's own file.
//   4. YtypNeighbours                                     - dictionaries declared by OTHER
//      archetypes in the SAME ytyp, i.e. authored in one pack. NOT a RAGE rule - provenance.
//   5. AssetSlot + DictSlots                              - the build slot the asset resolved
//      from (_RESOLVED.json). A same-named texture out of the SAME slot beats one from an
//      unrelated DLC. Also provenance, not a RAGE rule.
//
// ⛔ Tiers 1-3 are AUTHORITATIVE (they are the engine's own lookup order and each is a total
// order, so "the first match wins" is a rule and not a coin toss). Tiers 4-5 are PROVENANCE and
// are counted separately - and they only count as SCOPED when they narrow the candidates to
// EXACTLY ONE. Measured reason: on the 400-drawable list the slot matches MORE THAN ONE
// candidate 350 times against 24 unique ones, so accepting a non-unique slot hit would have
// moved 350 guesses into the "scoped" column without resolving anything. That is scoring your
// own exam; the uniqueness requirement is what stops it.
// ⛔ NOTHING here is inferred from a path or a filename. A guessed scope is a wrong texture.
struct FRudeTextureScope
{
	FString ArchetypeTxd;                              // tier 2 (lowercase; empty = none)
	TArray<FString> ParentTxdChain;                    // tier 3, nearest ancestor first
	const TArray<FString>* YtypNeighbours = nullptr;   // tier 4 (sorted, unique, owned by the index)
	FString AssetSlot;                                 // tier 5 (empty = unknown)
	const TMap<FString, FString>* DictSlots = nullptr; // tier 5: dictionary -> winning slot

	bool IsEmpty() const
	{
		return ArchetypeTxd.IsEmpty() && ParentTxdChain.Num() == 0
			&& (YtypNeighbours == nullptr || YtypNeighbours->Num() == 0)
			&& (AssetSlot.IsEmpty() || DictSlots == nullptr);
	}
};

// Build a UStaticMesh asset (plus its per-slot MaterialInstances) from ONE drawable-shaped
// XML node - the body every import lane shares. DrawableRoot may be a standalone <Drawable>
// root or a <Fragment>'s inner <Drawable> (both via ImportYdr), or a <DrawableDictionary>
// <Item> (ImportYddEntry) - anything carrying ShaderGroup + DrawableModelsHigh children.
// MeshName is the ASSET name, decided by the CALLER (file stem, <Name>, or dictionary entry).
// Scope = every scoping signal the caller can PROVE (see FRudeTextureScope). nullptr still means
// "no scope" - the two BlueprintCallable entry points ImportYdr/ImportYddEntry keep their
// signatures and pass nullptr, because a single-file call has no archetype to read one off, and
// that is also the do-nothing CONTROL every measurement of this fix is scored against.
static FString ImportDrawableNode(const FXmlNode* DrawableRoot, const FString& MeshName,
                                  const FString& DestFolder,
                                  const FRudeTextureScope* Scope = nullptr);

// The body of ImportYdr, plus the texture SCOPE the public UFUNCTION has no parameter for.
// Callers that hold an archetype (the map/MLO lane, and ImportYdrBatch when given a CorpusRoot)
// come through here; URudeToolset::ImportYdr forwards with an empty scope, which is exactly the
// pre-2026-08-05 behaviour, so the single-file tool is byte-identical to what it was.
static FString RudeImportYdrScoped(const FString& XmlPath, const FString& DestFolder,
                                   const FRudeTextureScope* TextureScope)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	// A fragment's visual drawable imports through this same lane: QUARRY's yft2xml (v1) emits
	// <Fragment> wrapping a <Drawable> child. The mesh must keep the FILE's stem - a fragment's
	// inner drawable names itself "skel", which would otherwise become the asset name.
	bool bFragment = false;
	if (Root && Root->GetTag() == TEXT("Fragment"))
	{
		const FXmlNode* Frag = Root;
		Root = Frag->FindChildNode(TEXT("Drawable"));
		// A CLOTH fragment owns its main drawable under <Cloths>/<Item>/<Drawable> (the cloth lane
		// ROUT ported 2026-09-04); 165/3,183 downtown yft are this shape and were refused as
		// "root is not <Drawable>" until 2026-09-05. First cloth item wins (one per file measured).
		if (!Root)
		{
			if (const FXmlNode* Cloths = Frag->FindChildNode(TEXT("Cloths")))
			{
				for (const FXmlNode* Item : Cloths->GetChildrenNodes())
				{
					if (const FXmlNode* D = Item->FindChildNode(TEXT("Drawable"))) { Root = D; break; }
				}
			}
		}
		bFragment = true;
	}
	if (!Root || Root->GetTag() != TEXT("Drawable"))
	{
		return Fail(TEXT("root is not <Drawable> (or <Fragment> wrapping one, or a cloth Fragment's Cloths/Item/Drawable)"));
	}

	// Drawable name (strip ".#dr" style suffix)
	FString Name = FPaths::GetBaseFilename(XmlPath);
	Name.RemoveFromEnd(TEXT(".ydr"));
	Name.RemoveFromEnd(TEXT(".yft"));
	if (!bFragment)
	{
		if (const FXmlNode* NameNode = Root->FindChildNode(TEXT("Name")))
		{
			FString N = NameNode->GetContent().TrimStartAndEnd();
			int32 Dot;
			if (N.FindChar(TEXT('.'), Dot))
			{
				N.LeftInline(Dot);
			}
			if (!N.IsEmpty())
			{
				Name = N;
			}
		}
	}

	return ImportDrawableNode(Root, Name, DestFolder, TextureScope);
}

FString URudeToolset::ImportYdr(const FString& XmlPath, const FString& DestFolder)
{
	return RudeImportYdrScoped(XmlPath, DestFolder, nullptr);
}

// ⛔⛔ THIS TOOL EXISTS BECAUSE A COUNTER CANNOT VERIFY ITSELF (2026-08-05, open item #40).
// ImportYdr now reports collisionPrimitivesImported / collisionMeshesImported, but reading those
// back to prove collision landed is circular - they are incremented by the very code under test. So
// this tool LOADS the finished asset and asks the ENGINE what its UBodySetup actually contains:
// FKAggregateGeom element counts straight off the body setup, and the complex triangle count via
// UStaticMesh::GetPhysicsTriMeshData, which is the same call the physics cooker makes and which
// follows ComplexCollisionMesh itself. Nothing here reads RUDE's own JSON.
FString URudeToolset::InspectCollision(const FString& AssetPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		return Fail(FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));
	}
	// GetPhysicsTriMeshData check()s HasValidRenderData(), so an async-compiling mesh would take the
	// whole editor down rather than report anything.
	FAssetCompilingManager::Get().FinishAllCompilation();
	UBodySetup* BS = Mesh->GetBodySetup();
	if (!BS)
	{
		return Fail(TEXT("mesh has no BodySetup"));
	}
	const FKAggregateGeom& Agg = BS->AggGeom;
	FString ComplexPath = TEXT("");
#if WITH_EDITORONLY_DATA
	if (Mesh->ComplexCollisionMesh)
	{
		ComplexPath = Mesh->ComplexCollisionMesh->GetPackage()->GetName();
	}
#endif
	// bInUseAllTriData=true, and the CheckComplex overload follows ComplexCollisionMesh. Without a
	// ComplexCollisionMesh this returns the RENDER mesh's own triangles, which is the honest reading:
	// that is literally what the asset would collide with.
	int32 ComplexTris = 0, ComplexVerts = 0;
	FTriMeshCollisionData TriData;
	if (Mesh->ContainsPhysicsTriMeshData(/*bInUseAllTriData*/ true)
		&& Mesh->GetPhysicsTriMeshData(&TriData, /*bInUseAllTriData*/ true))
	{
		ComplexTris = TriData.Indices.Num();     // FTriIndices, so this IS the triangle count
		ComplexVerts = TriData.Vertices.Num();
	}
	// One representative box, printed so the NUMBERS can be checked against the source XML by hand -
	// an element count proves something was added, not that it is the right size or in the right
	// place. This is the field that catches a unit or mirror error.
	FString FirstBox = TEXT("null");
	if (Agg.BoxElems.Num() > 0)
	{
		const FKBoxElem& B = Agg.BoxElems[0];
		FirstBox = FString::Printf(
			TEXT("{\"center\":[%.4f,%.4f,%.4f],\"extent\":[%.4f,%.4f,%.4f],\"yaw\":%.3f,\"pitch\":%.3f,\"roll\":%.3f}"),
			B.Center.X, B.Center.Y, B.Center.Z, B.X, B.Y, B.Z,
			B.Rotation.Yaw, B.Rotation.Pitch, B.Rotation.Roll);
	}
	FString FirstCapsule = TEXT("null");
	if (Agg.SphylElems.Num() > 0)
	{
		const FKSphylElem& C = Agg.SphylElems[0];
		FirstCapsule = FString::Printf(
			TEXT("{\"center\":[%.4f,%.4f,%.4f],\"radius\":%.4f,\"length\":%.4f,\"yaw\":%.3f,\"pitch\":%.3f,\"roll\":%.3f}"),
			C.Center.X, C.Center.Y, C.Center.Z, C.Radius, C.Length,
			C.Rotation.Yaw, C.Rotation.Pitch, C.Rotation.Roll);
	}
	FString FirstSphere = TEXT("null");
	if (Agg.SphereElems.Num() > 0)
	{
		const FKSphereElem& S = Agg.SphereElems[0];
		FirstSphere = FString::Printf(TEXT("{\"center\":[%.4f,%.4f,%.4f],\"radius\":%.4f}"),
			S.Center.X, S.Center.Y, S.Center.Z, S.Radius);
	}
	// ⛔ ok is COMPUTED, and it is deliberately NOT "has collision". A mesh with no bounds in its
	// source legitimately has an empty AggGeom, and failing that would make this instrument useless
	// for the control case it exists to score. ok:false means the QUERY failed.
	return FString::Printf(
		TEXT("{\"ok\":true,\"assetPath\":\"%s\",\"boxElems\":%d,\"sphereElems\":%d,")
		TEXT("\"sphylElems\":%d,\"convexElems\":%d,\"aggGeomTotal\":%d,\"traceFlag\":%d,")
		TEXT("\"complexCollisionMesh\":\"%s\",\"complexTriangles\":%d,\"complexVertices\":%d,")
		TEXT("\"firstBox\":%s,\"firstSphere\":%s,\"firstCapsule\":%s}"),
		*AssetPath, Agg.BoxElems.Num(), Agg.SphereElems.Num(), Agg.SphylElems.Num(),
		Agg.ConvexElems.Num(), Agg.GetElementCount(), (int32)BS->CollisionTraceFlag.GetValue(),
		*ComplexPath, ComplexTris, ComplexVerts, *FirstBox, *FirstSphere, *FirstCapsule);
}

static FString ImportDrawableNode(const FXmlNode* DrawableRoot, const FString& MeshName,
                                  const FString& DestFolder, const FRudeTextureScope* Scope)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	// Shader presets + their texture parameter bindings
	struct FShaderDef
	{
		FString Preset = TEXT("default");
		int32 RenderBucket = 0;              // RAGE draw bucket: 0 opaque, 1 alpha, 2 decal, 3 cutout
		FString Diffuse, Normal, Specular;   // texture NAMES from the ydr
		TMap<FString, FString> AllTex;       // every Texture param: samplerName -> texName (terrain layers)
		// ⭐ VALUE params (2026-07-29). QUARRY used to drop every non-texture shader parameter, so
		// detail tiling, specular intensity/falloff, bump scale and wetness never reached the
		// engine at all. They are emitted now as <Item type="Vector">, so carry them through.
		TMap<FString, FVector4> Values;
	};
	TArray<FShaderDef> Shaders;
	if (const FXmlNode* SG = DrawableRoot->FindChildNode(TEXT("ShaderGroup")))
	{
		if (const FXmlNode* Sh = SG->FindChildNode(TEXT("Shaders")))
		{
			for (const FXmlNode* Item : Sh->GetChildrenNodes())
			{
				FShaderDef Def;
				if (const FXmlNode* SName = Item->FindChildNode(TEXT("Name")))
				{
					Def.Preset = SName->GetContent().TrimStartAndEnd();
				}
				if (const FXmlNode* RB = Item->FindChildNode(TEXT("RenderBucket")))
				{
					Def.RenderBucket = FCString::Atoi(*RB->GetAttribute(TEXT("value")));
				}
				if (const FXmlNode* Params = Item->FindChildNode(TEXT("Parameters")))
				{
					for (const FXmlNode* P : Params->GetChildrenNodes())
					{
						if (P->GetAttribute(TEXT("type")) == TEXT("Vector"))
						{
							// Only the FIRST float4 is taken here: every parameter RUDE currently
							// understands is a single vec4, and silently averaging an array would
							// invent a value. Multi-vec4 params stay in the XML for later.
							if (const FXmlNode* V = P->FindChildNode(TEXT("Value")))
							{
								const FVector4 Val(
									FCString::Atof(*V->GetAttribute(TEXT("x"))),
									FCString::Atof(*V->GetAttribute(TEXT("y"))),
									FCString::Atof(*V->GetAttribute(TEXT("z"))),
									FCString::Atof(*V->GetAttribute(TEXT("w"))));
								Def.Values.Add(P->GetAttribute(TEXT("name")), Val);
							}
							continue;
						}
						if (P->GetAttribute(TEXT("type")) != TEXT("Texture"))
						{
							continue;
						}
						const FXmlNode* TexName = P->FindChildNode(TEXT("Name"));
						if (!TexName)
						{
							continue;
						}
						const FString Tex = TexName->GetContent().TrimStartAndEnd();
						const FString Sampler = P->GetAttribute(TEXT("name"));
						if (!Tex.IsEmpty()) { Def.AllTex.Add(Sampler, Tex); }
						if (Sampler == TEXT("DiffuseSampler"))       { Def.Diffuse = Tex; }
						else if (Sampler == TEXT("BumpSampler"))     { Def.Normal = Tex; }
						else if (Sampler == TEXT("SpecSampler"))     { Def.Specular = Tex; }
					}
				}
				Shaders.Add(MoveTemp(Def));
			}
		}
	}

	// Geometries under DrawableModelsHigh
	// ⛔ A DROPPED GEOMETRY IS COUNTED AND NAMED (2026-07-31). This loop used to `if (Parse) add;`
	// with no else - a drawable whose 4th of 6 geometries failed to parse imported "ok":true with
	// a quietly smaller mesh, and NOTHING in the verdict could tell that apart from a model that
	// genuinely has 4 geometries. Same law the batch counters already follow: a skip with no
	// counter is indistinguishable from "nothing to do".
	TArray<RudeYdr::FGeo> Geos;
	int32 GeosFailed = 0;
	FString GeoErrors;
	if (const FXmlNode* High = DrawableRoot->FindChildNode(TEXT("DrawableModelsHigh")))
	{
		for (const FXmlNode* ModelItem : High->GetChildrenNodes())
		{
			if (const FXmlNode* Geometries = ModelItem->FindChildNode(TEXT("Geometries")))
			{
				for (const FXmlNode* GeoItem : Geometries->GetChildrenNodes())
				{
					RudeYdr::FGeo Geo;
					FString Error;
					if (RudeYdr::ParseGeometry(GeoItem, Geo, Error))
					{
						Geos.Add(MoveTemp(Geo));
					}
					else
					{
						++GeosFailed;
						if (GeosFailed <= 4)
						{
							GeoErrors += FString::Printf(TEXT("%s\"%s\""),
								GeoErrors.IsEmpty() ? TEXT("") : TEXT(","), *Error);
						}
						UE_LOG(LogTemp, Warning,
							TEXT("[RUDE] ImportYdr %s: geometry %d dropped - %s"),
							*MeshName, GeosFailed, *Error);
					}
				}
			}
		}
	}
	if (Geos.Num() == 0)
	{
		return Fail(GeosFailed > 0
			? FString::Printf(TEXT("no geometry survived parsing (%d dropped)"), GeosFailed)
			: TEXT("no geometry in DrawableModelsHigh"));
	}

	// Create the StaticMesh asset
	const FString PackageName = DestFolder / MeshName;
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		return Fail(FString::Printf(TEXT("bad package name: %s"), *PackageName));
	}
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return Fail(TEXT("CreatePackage failed"));
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, FName(*MeshName), RF_Public | RF_Standalone);
	if (!Mesh)
	{
		return Fail(TEXT("NewObject<UStaticMesh> failed"));
	}

	// Reimport-over-existing must RESET prior state - appending to a mesh that
	// already has slots/LODs leaves stale slot 0 winning section resolution
	// (the WorldGridMaterial crate incident, 2026-07-24).
	Mesh->SetNumSourceModels(0);
	Mesh->GetStaticMaterials().Empty();

	Mesh->AddSourceModel();
	FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
	SourceModel.BuildSettings.bRecomputeNormals = false;
	SourceModel.BuildSettings.bRecomputeTangents = true;   // no tangents in v1 parse
	SourceModel.BuildSettings.bUseMikkTSpace = true;
	SourceModel.BuildSettings.bGenerateLightmapUVs = false;

	FMeshDescription* MeshDesc = Mesh->CreateMeshDescription(0);
	if (!MeshDesc)
	{
		return Fail(TEXT("CreateMeshDescription failed"));
	}
	FStaticMeshAttributes Attributes(*MeshDesc);
	Attributes.Register();
	// UV0 = the real UVs; UV2/UV3 smuggle Colour1 (RG / BA) for terrain blending
	Attributes.GetVertexInstanceUVs().SetNumChannels(4);

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> InstColors = Attributes.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> GroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	int32 TotalVerts = 0, TotalTris = 0;
	// ⛔ "triangles" USED TO REPORT ONLY THE SURVIVORS. Both `continue`s below drop a triangle and
	// ++TotalTris fires only on the ones that were created, so a file whose indices had been
	// scrambled by an emitter or binary-reader regression produced a plausible-looking triangle
	// count and no complaint at all. Split, because the two mean opposite things: DEGENERATE is
	// normal in shipped art, OUT-OF-RANGE is never normal and means the index buffer is not what
	// the vertex buffer says it is. MEASURED over 900 resolved ydr / 2,560,879 triangles:
	// out-of-range 0, degenerate 30 (0.0012%). Today it is an honest sub-0.01% loss; the point is
	// that a future 30% loss would look identical without these two fields.
	int32 TrisOutOfRange = 0, TrisDegenerate = 0;
	int32 GeometriesWithoutUV = 0;
	TArray<FString> SlotNames;

	for (int32 GeoIdx = 0; GeoIdx < Geos.Num(); ++GeoIdx)
	{
		const RudeYdr::FGeo& Geo = Geos[GeoIdx];
		// lower bound matters: ShaderIndex comes from FCString::Atoi on untrusted XML, and a
		// negative value passed an upper-bound-only check straight into TArray's fatal RangeCheck.
		const FString ShaderName = Shaders.IsValidIndex(Geo.ShaderIndex)
			? Shaders[Geo.ShaderIndex].Preset : TEXT("default");
		const FString SlotName = FString::Printf(TEXT("%s__%d"), *ShaderName, GeoIdx);
		SlotNames.Add(SlotName);
		if (!Geo.bHasUV0) { ++GeometriesWithoutUV; }

		const FPolygonGroupID GroupID = MeshDesc->CreatePolygonGroup();
		GroupSlotNames[GroupID] = FName(*SlotName);

		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(Geo.Positions.Num());
		for (const FVector3f& P : Geo.Positions)
		{
			const FVertexID VID = MeshDesc->CreateVertex();
			VertexPositions[VID] = P;
			VertexIDs.Add(VID);
		}
		TotalVerts += Geo.Positions.Num();

		const int32 NumTris = Geo.Indices.Num() / 3;
		for (int32 T = 0; T < NumTris; ++T)
		{
			// Winding: pass through AS-IS. Empirically pinned 2026-07-24: with the
			// Y-mirror applied to positions, RAGE's native winding already faces
			// outward in UE ("inside-out spiner" incident - a reversal here double-
			// flips). The OBJ lane still reverses because UE's OBJ importer adds
			// its own handedness flip; net conventions differ per lane.
			const int32 I0 = Geo.Indices[T * 3 + 0];
			const int32 I1 = Geo.Indices[T * 3 + 1];
			const int32 I2 = Geo.Indices[T * 3 + 2];
			// NEGATIVE indices must be rejected too - these come from FCString::Atoi on untrusted
			// XML, and an upper-bound-only check let -1 through into TArray's fatal RangeCheck.
			if (!VertexIDs.IsValidIndex(I0) || !VertexIDs.IsValidIndex(I1) || !VertexIDs.IsValidIndex(I2))
			{
				++TrisOutOfRange;
				continue;
			}
			if (I0 == I1 || I1 == I2 || I0 == I2)
			{
				++TrisDegenerate;
				continue;   // degenerate
			}
			TArray<FVertexInstanceID> Insts;
			for (const int32 Idx : { I0, I1, I2 })
			{
				const FVertexInstanceID Inst = MeshDesc->CreateVertexInstance(VertexIDs[Idx]);
				InstNormals[Inst] = Geo.Normals.IsValidIndex(Idx) ? Geo.Normals[Idx] : FVector3f(0, 0, 1);
				InstUVs.Set(Inst, 0, Geo.UVs.IsValidIndex(Idx) ? Geo.UVs[Idx] : FVector2f::ZeroVector);
				const FVector4f C1 = Geo.Colors1.IsValidIndex(Idx) ? Geo.Colors1[Idx] : FVector4f(0, 0, 0, 0);
				InstUVs.Set(Inst, 2, FVector2f(C1.X, C1.Y));
				InstUVs.Set(Inst, 3, FVector2f(C1.Z, C1.W));
				InstColors[Inst] = Geo.Colors.IsValidIndex(Idx) ? Geo.Colors[Idx] : FVector4f(1, 1, 1, 1);
				Insts.Add(Inst);
			}
			MeshDesc->CreatePolygon(GroupID, Insts);
			++TotalTris;
		}
	}

	Mesh->CommitMeshDescription(0);

	// --- Material auto-derive: family master -> MaterialInstanceConstant per slot ---
	// ⛔⛔ THE TEXTURE LIBRARY IS KEYED BY BARE NAME, AND RAGE TEXTURE NAMES ARE NOT UNIQUE.
	// This used to be TMap<FString, FAssetData> with TMap::Add's last-writer-wins, so which
	// dictionary's copy a mesh bound depended on the order the ASSET REGISTRY happened to enumerate
	// packages in - i.e. it could differ between two runs of the same import, while boundTextures
	// counted every one of them as a success. That makes a material screenshot non-reproducible,
	// which is fatal to "look at the screen" as evidence independently of whether anything renders
	// wrong.
	// MEASURED over the WHOLE resolved corpus (82,241 dictionaries = 40,077 shared + 42,164
	// __embedded siblings, 428,210 texture items): 120,326 distinct names, of which 47,839 (39.8%)
	// live in more than one dictionary and 23,324 (19.4%) disagree on Width/Height between
	// dictionaries. From the CONSUMER side, over 2,000 sampled ydr / 21,409 texture references:
	// 91.35% of every reference a drawable makes is satisfiable from more than one dictionary.
	//
	// ⚠ SO "REFUSE THE AMBIGUOUS BIND AND COUNT IT" - the obvious-looking fix, and the one the
	// audit recommended - IS WRONG AT THIS SCALE: it would drop 76.5% of all texture bindings and
	// render the city untextured. The 2,260-name figure that fix was sized against came from a
	// 6,000-dictionary sample that excluded the embedded manifests entirely.
	// WHAT IS ACTUALLY DONE: a name that is not unique is resolved by a SCOPE, and the full
	// precedence with its rationale lives on FRudeTextureScope - one place, because five call sites
	// each holding "roughly the right order" is how two lanes bind two different textures.
	// A name held by exactly ONE dictionary needs no rule and moves no counter.
	// ✅ HISTORY, so the numbers stay comparable. The 08-05 400-drawable run bound 2,495 textures of
	// which 2,390 (95.8%) fell to the lexicographic tie-break - the project holds 22,405 distinct
	// texture names with 8,791 present in more than one dictionary, so nearly every bind on nearly
	// every asset was a guess nothing had proven right. It was INVISIBLE to the counter everyone
	// reads: a wrong pick still binds *a* texture, so missingTextures stays 0. That is the "wrong
	// property tested" shape - missingTextures 0 says a texture was FOUND, never that it was the
	// RIGHT one. Scoping to the archetype's own <textureDictionary> took it to 1,041 / 1,349, and
	// adding RAGE's parent-txd chain plus ytyp-neighbourhood and same-slot provenance took it
	// further (see the register row and the ImportYdrBatch verdict for the measured split).
	// The fix is scope, not refusal (refusing at this scale would drop 76.5% of all binds and
	// render the city untextured - see the warning above).
	// ⛔ THE COUNTERS ARE SPLIT PER TIER so the residual stays measurable and cannot be optimised
	// away by redefining "scoped": texturesResolvedScoped is a DERIVED SUM of the five tier
	// counters, texturesScopedAuthoritative is the strict subset RAGE's own lookup order accounts
	// for, and texturesTieBroken - split further by REASON - is what is left. texturesFromEmbedded
	// stays as the __embedded tier, and ambiguousTextures is KEPT, unchanged in meaning
	// (== texturesTieBroken), purely so the pre-fix 2,390 stays directly comparable to a post-fix
	// run. Two spellings of one number is a smell; silently redefining a figure the register
	// quotes is worse.
	TMap<FString, TArray<FAssetData>> TexturesByName;
	// Every dictionary FOLDER present under /Game/RUDE/Textures. Used only to split the tie-break
	// residual BY CAUSE: "the archetype named a dictionary this project has never imported" is a
	// corpus/import gap, while "the dictionary is here and does not hold this name" is a real
	// resolution miss that the parent chain has to answer. One number for both would hide which.
	TSet<FString> DictsPresent;
	{
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		// ⛔⛔ WAIT FOR THE REGISTRY, OR THIS QUERY LIES. GetAssets answers from whatever has been
		// scanned SO FAR - it does not block. Driven from -ExecCmds at editor startup (which is how
		// every agent/CI run drives RUDE) the initial scan is still in flight, so this returns few
		// or NO textures, TexturesByName comes back empty, and every FindTexture misses. The import
		// then "succeeds" with boundTextures 0 and writes MaterialInstances with nothing bound.
		// MEASURED 2026-07-29: a FORCE rebind of 4,956 downtown meshes bound essentially nothing
		// this way - the city rendered untextured with default-checker patches - while 13 of the 17
		// textures a single building wanted were sitting in the project the whole time.
		AR.ScanPathsSynchronous({ TEXT("/Game/RUDE/Textures") }, /*bForceRescan*/ false);
		if (AR.IsLoadingAssets())
		{
			AR.WaitForCompletion();
		}
		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game/RUDE/Textures"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
		TArray<FAssetData> Found;
		AR.GetAssets(Filter, Found);
		for (const FAssetData& AD : Found)
		{
			TexturesByName.FindOrAdd(AD.AssetName.ToString().ToLower()).Add(AD);
			const FString Pkg = AD.PackagePath.ToString();
			int32 SlashAt;
			DictsPresent.Add(Pkg.FindLastChar(TEXT('/'), SlashAt)
				? Pkg.RightChop(SlashAt + 1).ToLower() : Pkg.ToLower());
		}
		// ⛔ SORT, DO NOT TRUST THE REGISTRY'S ORDER. GetAssets answers in whatever order the
		// registry discovered packages, which is the exact non-determinism this whole block exists
		// to remove. FName::LexicalLess gives the same winner on every machine and every run.
		int32 MultiName = 0;
		for (TPair<FString, TArray<FAssetData>>& P : TexturesByName)
		{
			if (P.Value.Num() > 1)
			{
				++MultiName;
				P.Value.Sort([](const FAssetData& A, const FAssetData& B)
					{ return A.PackageName.LexicalLess(B.PackageName); });
			}
		}
		// An empty texture library is never normal for a project that has imported any ytd. Say so
		// once, loudly, rather than letting thousands of silent misses look like missing source data.
		UE_LOG(LogTemp, Display, TEXT("[RUDE] texture library: %d distinct names under "
			"/Game/RUDE/Textures (%d of them present in more than one dictionary)"),
			TexturesByName.Num(), MultiName);
	}

	// The shader def drives master selection: build the capability signature from what this shader
	// ACTUALLY binds, then let the generator emit (once) the master for that signature.
	auto MasterForDef = [](const FShaderDef& D) -> UMaterialInterface*
	{
		FRudeMasterSpec Spec;
		Spec.Bucket = D.RenderBucket;
		for (const TPair<FString, FString>& T : D.AllTex)
		{
			const FString& S = T.Key;
			if (S.StartsWith(TEXT("Bump"), ESearchCase::IgnoreCase))          { Spec.bNormal = true; }
			else if (S.StartsWith(TEXT("Spec"), ESearchCase::IgnoreCase))     { Spec.bSpec = true; }
			else if (S.StartsWith(TEXT("Detail"), ESearchCase::IgnoreCase))   { Spec.bDetail = true; }
			else if (S.StartsWith(TEXT("TintPalette"), ESearchCase::IgnoreCase)) { Spec.bTint = true; }
		}
		// The preset name still tells us a surface EMITS; when it emits is archetype data
		// (timeFlags), not shader data - see the comment in EnsureGeneratedMaster.
		if (D.Preset.ToLower().Contains(TEXT("emissive"))) { Spec.bEmissive = true; }
		return EnsureGeneratedMaster(Spec);
	};

	auto MasterForPreset = [](const FString& Preset, int32 Bucket) -> UMaterialInterface*
	{
		const FString P = Preset.ToLower();
		const TCHAR* Path = TEXT("/RUDE/Masters/M_RUDE_Opaque.M_RUDE_Opaque");
		// RenderBucket is RAGE's authoritative signal (0 opaque, 1 alpha, 2 decal,
		// 3 cutout) - preset names lie (the airstrip weeds are "default" @ bucket 3).
		// Bucket first; name rules as fallback for bucket-0 oddities.
		if (Bucket == 2 || P.Contains(TEXT("decal")))
		{
			return EnsureDecalGeoMaster();   // coplanar-safe (WPO offset), masked
		}
		else if (P.StartsWith(TEXT("trees")) || P.StartsWith(TEXT("grass")) ||
		         P.Contains(TEXT("foliage")) || P.Contains(TEXT("plant")))
		{
			return EnsureFoliageMaster();    // two-sided foliage shading (leaf cards)
		}
		else if (P.Contains(TEXT("detail")) && Bucket != 1)
		{
			// ⭐ *_detail presets (normal_spec_detail, normal_detail, default_detail,
			// normal_spec_detail_tnt, normal_spec_decal_detail) - 6,264 DetailSampler bindings in
			// the corpus. Routed here ONLY for opaque/cutout-ish buckets: bucket 1 is alpha-blended
			// and keeps its existing treatment, and decal/foliage above still win because those are
			// about BLEND MODE, which matters more than a detail overlay.
			return EnsureDetailMaster();
		}
		else if (Bucket == 3 || P.Contains(TEXT("cutout")))
		{
			// ⛔ Bucket 3 is RAGE's CUTOUT (alpha-TESTED). Bucket 1 is alpha-BLENDED - glass -
			// and routing it here would alpha-TEST glass, punching holes in it. Until a proper
			// translucent master exists, bucket 1 stays on Opaque: rendering glass opaque is
			// today's behaviour and is strictly less wrong than perforating it. (2026-07-28;
			// bucket 1 measured at 2.0% of downtown, bucket 3 at 14.6%.)
			return EnsureCutoutMaster();
		}
		return LoadObject<UMaterialInterface>(nullptr, Path);
	};

	// Dictionary a texture asset came from = the LAST segment of its package path, because
	// ImportYtd writes every texture to <DestFolder>/<TxdName>/<TexName>. Reading it off the
	// FAssetData costs nothing and needs no asset load.
	auto DictOf = [](const FAssetData& AD) -> FString
	{
		const FString Path = AD.PackagePath.ToString();
		int32 Slash;
		return Path.FindLastChar(TEXT('/'), Slash) ? Path.RightChop(Slash + 1).ToLower() : Path.ToLower();
	};
	const FString EmbeddedDict = (MeshName + TEXT("__embedded")).ToLower();
	// The names this drawable's OWN embedded <TextureDictionary> manifest declares. Read purely to
	// split the residual by cause: if a tie-break was needed for a name the drawable actually
	// SHIPS, the right answer exists in the corpus and simply is not imported here - a corpus gap,
	// not an unresolvable ambiguity. Measuring that separately is what stops "1,349 unproven" from
	// reading as "1,349 unresolvable".
	TSet<FString> EmbeddedNames;
	if (const FXmlNode* SGForTex = DrawableRoot->FindChildNode(TEXT("ShaderGroup")))
	{
		if (const FXmlNode* TD = SGForTex->FindChildNode(TEXT("TextureDictionary")))
		{
			for (const FXmlNode* It : TD->GetChildrenNodes())
			{
				if (const FXmlNode* N = It->FindChildNode(TEXT("Name")))
				{
					const FString Nm = N->GetContent().TrimStartAndEnd().ToLower();
					if (!Nm.IsEmpty()) { EmbeddedNames.Add(Nm); }
				}
			}
		}
	}
	// ✅ FED 2026-08-05 (#43 + #21b). The four spellings per dictionary are the declared name and
	// RAGE's high-detail siblings: a prop's textures routinely live in "<txd>+hi" while the
	// archetype names the base, so scoping to the bare name alone would miss the very dictionary
	// the asset ships with. Order is deliberate - base first, then +hi/+hidr/+hidd - so each scope
	// remains a TOTAL ORDER and the pick is reproducible run to run and machine to machine.
	auto AddTxdFamily = [](TArray<FString>& Out, const FString& Txd)
	{
		if (Txd.IsEmpty()) { return; }
		const FString S = Txd.ToLower();
		Out.Add(S);
		Out.Add(S + TEXT("+hi"));
		Out.Add(S + TEXT("+hidr"));
		Out.Add(S + TEXT("+hidd"));
	};
	TArray<FString> ScopeArchetype;   // tier 2
	TArray<FString> ScopeParents;     // tier 3, already nearest-ancestor-first
	if (Scope)
	{
		AddTxdFamily(ScopeArchetype, Scope->ArchetypeTxd);
		for (const FString& P : Scope->ParentTxdChain) { AddTxdFamily(ScopeParents, P); }
	}
	const TArray<FString>* ScopeYtyp = Scope ? Scope->YtypNeighbours : nullptr;
	const FString ScopeSlot = Scope ? Scope->AssetSlot : FString();
	const TMap<FString, FString>* DictSlots = Scope ? Scope->DictSlots : nullptr;
	// Was ANY dictionary this asset's declared scope names actually imported into the project?
	// A "no" turns every miss below into a declared corpus gap instead of an unexplained guess.
	bool bScopeDictPresent = false;
	for (const FString& S : ScopeArchetype) { if (DictsPresent.Contains(S)) { bScopeDictPresent = true; break; } }
	if (!bScopeDictPresent)
	{
		for (const FString& S : ScopeParents) { if (DictsPresent.Contains(S)) { bScopeDictPresent = true; break; } }
	}
	const bool bNoScopeAtAll = (Scope == nullptr) || Scope->IsEmpty();

	// ⛔ THE COUNTERS ARE SPLIT PER TIER so the residual can never be optimised away by loosening
	// what "scoped" means. texturesResolvedScoped is a SUM of these, never a primitive.
	int32 TexturesAmbiguousTotal    = 0;  // every bind whose name had >1 candidate dictionary
	int32 TexturesFromEmbedded      = 0;  // tier 1
	int32 TexturesFromArchetypeTxd  = 0;  // tier 2
	int32 TexturesFromParentTxd     = 0;  // tier 3
	int32 TexturesFromYtypNeighbour = 0;  // tier 4 (unique hit only)
	int32 TexturesFromSameSlot      = 0;  // tier 5 (unique hit only)
	int32 TexturesTieBroken         = 0;  // the residual: no scope selected a winner
	// ...and the tie-break residual is split BY CAUSE. These partition TexturesTieBroken exactly.
	int32 TieBreakEmbeddedNotImported = 0; // the drawable ships this texture; its __embedded txd is absent
	int32 TieBreakNoScope             = 0; // caller had nothing to scope with (the CONTROL path)
	int32 TieBreakSlotAmbiguous       = 0; // slot narrowed to >1 candidate - narrowed, still a guess
	int32 TieBreakYtypAmbiguous       = 0; // ytyp neighbours narrowed to >1 candidate
	int32 TieBreakScopeDictAbsent     = 0; // every dictionary the scope names is absent from the project
	int32 TieBreakNameNotInScope      = 0; // the scope IS here and genuinely does not hold this name
	auto FindTexture = [&](const FString& TexName) -> UTexture2D*
	{
		if (TexName.IsEmpty())
		{
			return nullptr;
		}
		const FString Lower = TexName.ToLower();
		const TArray<FAssetData>* Cands = TexturesByName.Find(Lower);
		if (!Cands || Cands->Num() == 0)
		{
			return nullptr;
		}
		if (Cands->Num() == 1)
		{
			// Not ambiguous at all: one dictionary in the whole project holds this name, so no rule
			// had to choose and no counter moves. Counting this as "scoped" would inflate the
			// fix with binds it had nothing to do with.
			return Cast<UTexture2D>((*Cands)[0].GetAsset());
		}
		++TexturesAmbiguousTotal;
		// --- tier 1: the drawable's own embedded dictionary. Exact by construction.
		for (const FAssetData& AD : *Cands)
		{
			if (DictOf(AD) == EmbeddedDict)
			{
				++TexturesFromEmbedded;
				return Cast<UTexture2D>(AD.GetAsset());
			}
		}
		// --- tier 2: the dictionary the ARCHETYPE declares (+ its hi-detail siblings).
		for (const FString& S : ScopeArchetype)
		{
			for (const FAssetData& AD : *Cands)
			{
				if (DictOf(AD) == S)
				{
					++TexturesFromArchetypeTxd;
					return Cast<UTexture2D>(AD.GetAsset());
				}
			}
		}
		// --- tier 3: RAGE's OWN fallback - the gtxd CMapParentTxds chain, nearest ancestor first.
		// This is the engine's documented behaviour for a texture the asset's txd does not hold,
		// read out of Rockstar's own file rather than reasoned about.
		for (const FString& S : ScopeParents)
		{
			for (const FAssetData& AD : *Cands)
			{
				if (DictOf(AD) == S)
				{
					++TexturesFromParentTxd;
					return Cast<UTexture2D>(AD.GetAsset());
				}
			}
		}
		// --- tier 4 (PROVENANCE): a dictionary named by another archetype in the SAME ytyp.
		// ⛔ Accepted ONLY when it selects exactly one candidate. With two, the ytyp neighbourhood
		// is not an ordered rule and the pick would be a coin toss wearing a scope's name.
		const FAssetData* YtypHit = nullptr;
		int32 YtypHits = 0;
		if (ScopeYtyp && ScopeYtyp->Num() > 0)
		{
			for (const FAssetData& AD : *Cands)
			{
				if (ScopeYtyp->Contains(DictOf(AD)))
				{
					++YtypHits;
					if (!YtypHit) { YtypHit = &AD; }
				}
			}
			if (YtypHits == 1)
			{
				++TexturesFromYtypNeighbour;
				return Cast<UTexture2D>(YtypHit->GetAsset());
			}
		}
		// --- tier 5 (PROVENANCE): the build SLOT this asset resolved from (_RESOLVED.json).
		// Same uniqueness rule, and it is load-bearing: measured 350 non-unique against 24 unique
		// on the 400-drawable list, so without it this tier alone would have manufactured 350
		// "scoped" binds that resolved nothing.
		const FAssetData* SlotHit = nullptr;
		int32 SlotHits = 0;
		if (!ScopeSlot.IsEmpty() && DictSlots)
		{
			for (const FAssetData& AD : *Cands)
			{
				const FString* S = DictSlots->Find(DictOf(AD));
				if (S && *S == ScopeSlot)
				{
					++SlotHits;
					if (!SlotHit) { SlotHit = &AD; }
				}
			}
			if (SlotHits == 1)
			{
				++TexturesFromSameSlot;
				return Cast<UTexture2D>(SlotHit->GetAsset());
			}
		}
		// --- tier 6: deterministic, NOT correct. The pick is taken from the narrowest evidence
		// available (same slot, else same ytyp, else everything) because a narrowed guess is a
		// better guess - but it is still counted as a TIE-BREAK, because narrowing is not
		// selecting. The reason split is what makes the residual actionable instead of opaque.
		++TexturesTieBroken;
		const FAssetData* Pick = SlotHit ? SlotHit : (YtypHit ? YtypHit : &(*Cands)[0]);
		if (EmbeddedNames.Contains(Lower))   { ++TieBreakEmbeddedNotImported; }
		else if (bNoScopeAtAll)              { ++TieBreakNoScope; }
		else if (SlotHits > 1)               { ++TieBreakSlotAmbiguous; }
		else if (YtypHits > 1)               { ++TieBreakYtypAmbiguous; }
		else if (!bScopeDictPresent)         { ++TieBreakScopeDictAbsent; }
		else                                 { ++TieBreakNameNotInScope; }
		return Cast<UTexture2D>(Pick->GetAsset());
	};

	int32 BoundTextures = 0;
	// ⛔ THE COUNTER THAT CANNOT LIE. UMaterialInstance::SetTextureParameterValueInternal does NO
	// validation against the parent's parameter set (Engine/Private/Materials/MaterialInstance.cpp
	// ~4309): binding "Normal" onto a master that HAS no Normal succeeds, persists in the uasset,
	// shows in the MI editor - and renders nothing. So a rising "boundTextures" proves nothing.
	// UnsupportedByMaster counts exactly those silent no-ops; a non-zero value is a real defect.
	int32 ValueParamsSeen = 0;       // params that REACHED the bind decision (see the verdict note)
	int32 ValueParamsBound = 0;      // value param the master DOES expose, so it took effect
	int32 ValueParamsUnsupported = 0;// value param arrived but no master parameter accepts it
	int32 ValueParamsDeduped = 0;    // params on a geometry that reused a cached MI - never bound
	int32 UnsupportedByMaster = 0;   // sampler mapped, but the MASTER has no such parameter
	int32 MissingTextures = 0;       // XML named a texture that is not imported in this project
	int32 UnmappedSamplers = 0;      // a sampler name with no entry in GSamplerBinds (see below)
	int32 SlotsWithoutShaderDef = 0; // geometry's ShaderIndex resolves to no shader definition
	int32 SlotsWithoutMaterial = 0;  // slot kept WorldGridMaterial - see the block below the loop

	// Sampler name -> master parameter. Keyed by NAME; FString== and TMap<FString,> hashing are
	// both case-INSENSITIVE, so no normalisation is needed. The converter's older 3-name emission
	// is a strict SUBSET of this table, so pre-regeneration XML still binds exactly as before.
	// ⛔ NO unknown-sampler fallback to Diffuse: that is the one rule that could shove a fur-shell
	// or a runtime-bound hash texture into an albedo slot. UnmappedSamplers makes the residual
	// visible instead of guessing.
	// DELIBERATELY UNMAPPED (RAGE concepts UE replaces or cannot express): EnvironmentSampler
	// (UE uses reflection captures), StippleSampler (dither LOD fade), ComboHeightSamplerFur*
	// (fur shells), hash_* (runtime-bound, carry no texture).
	static const TPair<const TCHAR*, const TCHAR*> GSamplerBinds[] = {
		{ TEXT("DiffuseSampler"),     TEXT("Diffuse")     },
		{ TEXT("BumpSampler"),        TEXT("Normal")      },
		{ TEXT("SpecSampler"),        TEXT("Specular")    },
		{ TEXT("TextureSamp"),        TEXT("Diffuse")     },  // cable's albedo: 152/152 resolve
		{ TEXT("distanceMapSampler"), TEXT("Diffuse")     },  // distance_map's only colour source
		{ TEXT("DetailSampler"),      TEXT("Detail")      },  // inert until the masters gain Detail
		{ TEXT("TintPaletteSampler"), TEXT("TintPalette") },  // inert until the palettes import
		{ TEXT("DirtSampler"),        TEXT("Dirt")        },  // inert until those textures import
	};
	TMap<FString, UMaterialInstanceConstant*> MIByConfig;   // dedupe: same shader config -> shared MI
	for (int32 GeoIdx = 0; GeoIdx < Geos.Num(); ++GeoIdx)
	{
		const FString& Slot = SlotNames[GeoIdx];
		const int32 ShaderIdx = Geos[GeoIdx].ShaderIndex;
		const FShaderDef* Def = Shaders.IsValidIndex(ShaderIdx) ? &Shaders[ShaderIdx] : nullptr;
		if (!Def) { ++SlotsWithoutShaderDef; }
		else { ValueParamsSeen += Def->Values.Num(); }

		UMaterialInterface* SlotMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		const bool bTerrain = Def && Def->Preset.StartsWith(TEXT("terrain"), ESearchCase::IgnoreCase);
		UMaterialInterface* Master = nullptr;
		if (Def)
		{
			// Terrain, decals and foliage keep their hand-built masters: those encode behaviour the
			// generator does not model (4-layer vertex-colour blend, coplanar WPO offset, two-sided
			// leaf shading). EVERYTHING ELSE now comes from the generator, so a preset's master is
			// determined by what it binds rather than by a name-matching ladder.
			const FString LowerPreset = Def->Preset.ToLower();
			const bool bSpecialCase =
				bTerrain
				|| Def->RenderBucket == 2 || LowerPreset.Contains(TEXT("decal"))
				|| LowerPreset.StartsWith(TEXT("trees")) || LowerPreset.StartsWith(TEXT("grass"))
				|| LowerPreset.Contains(TEXT("foliage")) || LowerPreset.Contains(TEXT("plant"));
			Master = bSpecialCase
				? (bTerrain ? EnsureTerrainMaster() : MasterForPreset(Def->Preset, Def->RenderBucket))
				: MasterForDef(*Def);
		}
		// ⛔⛔ THE DEDUPE KEY MUST CARRY EVERYTHING THE MATERIALINSTANCE IS BUILT FROM.
		// It used to be Preset|Diffuse|Normal|Specular (+4 terrain layer names). That key omits
		// RenderBucket - which SELECTS THE MASTER two blocks above, and therefore the blend mode -
		// every sampler outside the legacy three (Detail, TintPalette, Dirt, TextureSamp,
		// distanceMapSampler, BumpSampler_layerN), and ALL value params. Two geometries whose
		// shaders differed only in those fields collided, and the second silently got the first
		// one's MI: wrong detail texture, wrong specular response, or an opaque master on an
		// alpha-blended surface. Nothing counted it, because a cache HIT is indistinguishable from
		// a correct share.
		// MEASURED by replicating both keys over 3,000 resolved ydr / 12,181 geometries:
		// 170 geometries (1.396%) received an MI built from a DIFFERENT shader - 160 differing in
		// value params (bumpiness 0.2 vs 0.4 on kt1_11_apt_01; specular hash_27B9B2FA 0.5 vs 0.93
		// on cs1_02_biln019), 7 in bound textures, 3 in RenderBucket (an opaque master reused for
		// a bucket-1 geometry). 101 of 3,000 files (3.37%) are affected.
		// COST OF THE FIX, measured on the same sample: 11,186 -> 11,346 MaterialInstances, +1.43%.
		// Dedupe still works; it can no longer merge two different shaders.
		FString ConfigKey;
		if (Def)
		{
			ConfigKey = FString::Printf(TEXT("%s|b%d"), *Def->Preset, Def->RenderBucket);
			// TMap iteration order is not a contract - sort, or the same shader can produce two
			// different keys and the dedupe silently stops deduping.
			TArray<FString> Keys;
			Def->AllTex.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const FString& K : Keys)
			{
				ConfigKey += FString::Printf(TEXT("|%s=%s"), *K, **Def->AllTex.Find(K));
			}
			Keys.Reset();
			Def->Values.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const FString& K : Keys)
			{
				const FVector4& V = *Def->Values.Find(K);
				ConfigKey += FString::Printf(TEXT("|%s=%.6g,%.6g,%.6g,%.6g"),
					*K, V.X, V.Y, V.Z, V.W);
			}
			ConfigKey.ToLowerInline();
		}
		bool bSlotResolved = false;
		if (Master && MIByConfig.Contains(ConfigKey))
		{
			SlotMaterial = MIByConfig[ConfigKey];
			bSlotResolved = true;
			// The dedupe branch never reaches the bind loop, so its value params were counted in
			// "seen" and accounted for by nothing. They are a THIRD residual, not a loss.
			ValueParamsDeduped += Def->Values.Num();
		}
		else if (Master)
		{
			// MI per unique shader CONFIG: /Game/RUDE/Materials/Instances/<prop>/MI_<prop>_<idx>
			const FString MIName = FString::Printf(TEXT("MI_%s_%d"), *MeshName, GeoIdx);
			const FString MIPackageName =
				FString::Printf(TEXT("/Game/RUDE/Materials/Instances/%s/%s"), *MeshName, *MIName);
			if (UPackage* MIPackage = CreatePackage(*MIPackageName))
			{
				// TRUE edit-in-place (same law as textures): reuse an existing MI on
				// reimport - NewObject over an existing object displaces it.
				UMaterialInstanceConstant* MIC = FindObject<UMaterialInstanceConstant>(MIPackage, *MIName);
				if (!MIC)
				{
					MIC = NewObject<UMaterialInstanceConstant>(
						MIPackage, FName(*MIName), RF_Public | RF_Standalone);
				}
				MIC->SetParentEditorOnly(Master);

				// What parameters does this MASTER actually expose? Asking is the whole point -
				// see the UnsupportedByMaster comment where it is declared.
				TSet<FName> MasterParams;
				{
					TArray<FMaterialParameterInfo> Infos;
					TArray<FGuid> Ids;
					Master->GetAllTextureParameterInfo(Infos, Ids);
					for (const FMaterialParameterInfo& I : Infos) { MasterParams.Add(I.Name); }
				}
				bool bBoundDiffuse = false;
				bool bBoundDetail = false;
				auto BindTex = [&](const TCHAR* Param, const FString& TexName) -> bool
				{
					if (TexName.IsEmpty()) { return false; }
					UTexture2D* T = FindTexture(TexName);
					if (!T) { ++MissingTextures; return false; }
					const FName PName(Param);
					if (!MasterParams.Contains(PName)) { ++UnsupportedByMaster; return false; }
					MIC->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PName), T);
					++BoundTextures;
					if (PName == FName(TEXT("Diffuse"))) { bBoundDiffuse = true; }
					if (PName == FName(TEXT("Detail")))  { bBoundDetail = true; }
					return true;
				};

				// Walk EVERY texture parameter the shader actually declared, through the table.
				// (Def->Diffuse/Normal/Specular are the legacy three and are already inside
				// AllTex - binding from AllTex alone therefore loses nothing and gains the rest.)
				for (const TPair<FString, FString>& Tex : Def->AllTex)
				{
					// Terrain's 4 blend layers are indexed, so they are matched by PREFIX rather
					// than by table entry: TextureSampler_layerN -> DiffuseN, BumpSampler_layerN
					// -> NormalN. Every preset carrying them is terrain_cb_w_4lyr*, which is also
					// what selects the 4-layer master, so the pairing cannot drift.
					FString Idx;
					if (Tex.Key.StartsWith(TEXT("TextureSampler_layer"), ESearchCase::IgnoreCase))
					{
						Idx = Tex.Key.RightChop(20);
						BindTex(*FString::Printf(TEXT("Diffuse%s"), *Idx), Tex.Value);
						continue;
					}
					if (Tex.Key.StartsWith(TEXT("BumpSampler_layer"), ESearchCase::IgnoreCase))
					{
						Idx = Tex.Key.RightChop(17);
						BindTex(*FString::Printf(TEXT("Normal%s"), *Idx), Tex.Value);
						continue;
					}
					const TCHAR* Param = nullptr;
					for (const TPair<const TCHAR*, const TCHAR*>& B : GSamplerBinds)
					{
						if (Tex.Key.Equals(B.Key, ESearchCase::IgnoreCase)) { Param = B.Value; break; }
					}
					if (!Param)
					{
						++UnmappedSamplers;
						continue;
					}
					BindTex(Param, Tex.Value);
				}

				// A decal whose texture isn't in the corpus must render as NOTHING, not as
				// an opaque white slab (the master's default texture is white - Matt spotted the
				// white slabs across the beach, 2026-07-25).
				// ⚠ The predicate tracks whether a Diffuse bind SUCCEEDED, not whether the legacy
				// DiffuseSampler name happened to resolve: with real render buckets this gate now
				// fires on all of bucket 2, and presets whose colour arrives under another sampler
				// name (distance_map carries only distanceMapSampler) would otherwise flip from
				// "white slab" to "invisible".
				if (Def->RenderBucket == 2 || Def->Preset.Contains(TEXT("decal")))
				{
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("Visible")), bBoundDiffuse ? 1.f : 0.f);
				}
				// (The terrain_cb_* layer samplers used to be bound by a separate block here. They
				// now go through the SAME guarded walk above - matched by prefix - so no bind can
				// bypass the parent-parameter check. bTerrain still selects the 4-layer master.)

				// ⛔ PROVE IT BEFORE ENABLING IT. DetailAmount defaults to 0 in the master, so the
				// detail overlay is inert until a Detail texture genuinely bound. Same shape as the
				// decal 'Visible' gate: never switch an effect on because a parameter EXISTS - only
				// because the data it needs arrived.
				// ⚠ The predicate is now the BIND RESULT (bBoundDetail), not a second FindTexture on
				// DetailSampler. Same answer - the walk above already bound it through the same
				// guard - but the old spelling resolved the name TWICE, which double-counted it in
				// the ambiguity counter added 2026-08-04 and loaded the asset a second time.
				if (MasterParams.Contains(FName(TEXT("Detail"))))
				{
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("DetailAmount")), bBoundDetail ? 1.f : 0.f);
				}

				// ---- VALUE params -> the MI, guarded exactly like textures ----
				// ⭐ These only became available on 2026-07-29, when QUARRY stopped dropping every
				// non-texture shader parameter. Same discipline as BindTex: ask the master what it
				// exposes and count the misses, so an unbindable value is LOUD rather than silent.
				TSet<FName> MasterVectors, MasterScalars;
				{
					TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids;
					Master->GetAllVectorParameterInfo(Infos, Ids);
					for (const FMaterialParameterInfo& I : Infos) { MasterVectors.Add(I.Name); }
					Infos.Reset(); Ids.Reset();
					Master->GetAllScalarParameterInfo(Infos, Ids);
					for (const FMaterialParameterInfo& I : Infos) { MasterScalars.Add(I.Name); }
				}
				for (const TPair<FString, FVector4>& V : Def->Values)
				{
					const FName VName(*V.Key);
					if (MasterVectors.Contains(VName))
					{
						MIC->SetVectorParameterValueEditorOnly(
							FMaterialParameterInfo(VName),
							FLinearColor(V.Value.X, V.Value.Y, V.Value.Z, V.Value.W));
						++ValueParamsBound;
					}
					else if (MasterScalars.Contains(VName))
					{
						// A single-float RAGE param still arrives as a vec4 with the value in .x.
						MIC->SetScalarParameterValueEditorOnly(
							FMaterialParameterInfo(VName), V.Value.X);
						++ValueParamsBound;
					}
					else
					{
						++ValueParamsUnsupported;
					}
				}
				MIC->PostEditChange();
				MIPackage->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(MIC);
				SlotMaterial = MIC;
				bSlotResolved = true;
				MIByConfig.Add(ConfigKey, MIC);
			}
		}
		// ⛔⛔ THE WORST-CASE FAILURE IN THIS FILE WAS INVISIBLE. SlotMaterial starts as
		// UMaterial::GetDefaultMaterial(MD_Surface) - WorldGridMaterial - and BOTH branches above
		// require a non-null Master. Master is null when the ShaderIndex is out of range, when any
		// Ensure*Master hits `if (!Pkg) return nullptr`, or when MasterForPreset's LoadObject of
		// /RUDE/Masters/M_RUDE_Opaque fails because the plugin's content is not mounted. In that
		// last case EVERY slot of EVERY mesh silently gets WorldGridMaterial while boundTextures,
		// missingTextures and unsupportedByMaster all read 0 - a verdict IDENTICAL to "this
		// drawable has no textures", on an entire city. There was no counter and no field.
		// Split by cause, because they are diagnosed differently: a non-zero slotsWithoutShaderDef
		// is bad corpus data, a non-zero slotsWithoutMaterial on the first mesh of a batch means
		// the masters are unreachable. (MEASURED: 0 out-of-range ShaderIndex in 900 resolved ydr /
		// 3,444 geometries, so today only the master path can fire this.)
		if (!bSlotResolved) { ++SlotsWithoutMaterial; }

		FStaticMaterial Mat(SlotMaterial, FName(*Slot));
		Mat.UVChannelData.bInitialized = true;
		Mesh->GetStaticMaterials().Add(Mat);
	}

	// ⭐ COLLISION IMPORT (open item #40, 2026-08-05). A drawable's <Bounds> is the phBound Rockstar
	// authored; 17.3% of the corpus's ydr carry one (432 of a 2,500-file sample). Until now RUDE read
	// none of them and every imported asset walked on its own RENDER triangles instead.
	// Parsed BEFORE Build() because ComplexCollisionMesh has to be in place when the physics data is
	// cooked, and the collision mesh is a SEPARATE asset - the render mesh cannot hold both.
	RudeBound::FResult Col;
	RudeBound::ImportBounds(DrawableRoot, Col);
	FString ColAssetPath;
	UStaticMesh* ColMeshBuilt = nullptr;
	if (Col.Indices.Num() >= 3)
	{
		FString ColWhy;
		UStaticMesh* ColMesh = RudeBound::BuildCollisionMesh(Col, DestFolder, MeshName, ColWhy);
#if !WITH_EDITORONLY_DATA
		// ComplexCollisionMesh is editor-only. RudeEditor is an editor module, so this branch is
		// unreachable in practice - it exists so the file cannot silently compile into a build where
		// the collision mesh is created and then thrown away.
		if (ColMesh) { ColWhy = TEXT("ComplexCollisionMesh unavailable (non-editor build)"); }
		ColMesh = nullptr;
#endif
		if (ColMesh)
		{
#if WITH_EDITORONLY_DATA
			Mesh->ComplexCollisionMesh = ColMesh;
#endif
			ColMeshBuilt = ColMesh;
			ColAssetPath = ColMesh->GetPackage()->GetName();
		}
		else
		{
			// The triangles were parsed and then LOST. That is a tool failure, not a data gap, so it
			// takes the gating counter rather than disappearing into a log line.
			++Col.BoundsMalformed;
			Col.Reasons.Add(FString::Printf(TEXT("collisionMesh:%s"), *ColWhy));
			Col.MeshesImported = 0;
			Col.Indices.Reset();
		}
	}

	Mesh->Build(true);
	if (ColMeshBuilt)
	{
		// The physics cook below reads BOTH meshes (the render mesh for its own sections, the
		// collision mesh through ComplexCollisionMesh), so both must be finished compiling first.
		// Gated on ColMeshBuilt so the 82.7%-of-drawables no-bounds path pays nothing new.
		FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh, ColMeshBuilt });
	}
	if (!Mesh->GetBodySetup()) { Mesh->CreateBodySetup(); }
	if (UBodySetup* BS = Mesh->GetBodySetup())
	{
		BS->AggGeom = Col.Agg;
		// Three cases, and the flag differs in each:
		//  - RAGE primitives present: they ARE the simple collision, so simple must stay live.
		//    Complex falls through to ComplexCollisionMesh when there is one, else to the render mesh
		//    (unchanged from the old behaviour).
		//  - only a triangle bound: AggGeom is empty, so simple has to come from complex or the asset
		//    would have no simple collision at all.
		//  - neither (82.7% of drawables, and every ydd entry): EXACTLY the old behaviour, render
		//    triangles as complex-as-simple. ExportYdr's existing collisionFromRenderMesh counter
		//    still describes this case correctly.
		BS->CollisionTraceFlag = (Col.PrimitivesImported > 0) ? CTF_UseSimpleAndComplex
		                                                      : CTF_UseComplexAsSimple;
		BS->InvalidatePhysicsData();
		BS->CreatePhysicsMeshes();
	}
	Mesh->PostEditChange();
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Mesh);

	FString SlotsJson;
	for (int32 i = 0; i < SlotNames.Num(); ++i)
	{
		SlotsJson += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *SlotNames[i]);
	}
	// ⭐ Report the VALUE params that arrived, split FOUR ways so the residual closes exactly:
	//     valueParamsSeen == valueParamsBound + valueParamsUnsupported + valueParamsDeduped
	// holds identically whenever slotsWithoutMaterial == 0, and any gap is now a real defect
	// rather than a shrug.
	// ⛔ WHAT WAS WRONG: seen was `for (const FShaderDef& D : Shaders) ValueParams += D.Values.Num()`
	// - a sum over the whole shader TABLE - while bound/unsupported only ever incremented for a
	// geometry that WON its config group. Params on shaders no geometry references, and params on
	// every geometry that hit the MI cache, were counted as "seen" and then accounted for by
	// nothing. MEASURED over 2,000 corpus ydr: seen reported 56,507 against 54,000 reachable -
	// 2,507 params (4.4%) that appeared in seen and in neither residual. The comment above this
	// block called that gap "a coverage measurement", which is exactly how a 4% regression in
	// binding would have hidden: it would have looked like today's healthy run.
	// Seen is now summed PER GEOMETRY over the shader that geometry actually uses, which is the
	// same population the bind loop walks. It is a count of bind DECISIONS, not of distinct params
	// in the file - that is the only definition that can balance.
	// ⛔ ok IS COMPUTED. slotsWithoutMaterial > 0 means at least one slot silently kept
	// WorldGridMaterial, and the pathological case (masters unmounted) makes that EVERY slot of
	// EVERY mesh while every other counter reads 0. A tool whose ok cannot express its own
	// worst-case failure is not reporting, it is decorating.
	// ⛔ THE COLLISION GATE, decided deliberately and split by CAUSE (2026-08-05, #40).
	// collisionBoundsMalformed GATES. It means a bound RUDE claims to support could not be read, or
	// carried a <Bounds type=""> string this build has never seen. Either way the TOOL failed - the
	// collider is silently absent while every other counter reads healthy - and its measured cost on
	// the corpus is zero, so it can never become the gate nobody reads.
	// collisionBoundsUnmapped does NOT gate. It means RUDE understood the bound and UE has no
	// faithful target for it (Cylinder today). That is a capability gap on VALID data, the same class
	// as missingMeshes and missingPixels, and it fires on healthy shipped assets.
	// collisionPolysDropped does NOT gate, for the same reason: 5.7% of BVH polygons are
	// poly-primitives on perfectly healthy data.
	const bool bMeshOk = (SlotsWithoutMaterial == 0) && (Col.BoundsMalformed == 0);
	// #43/#21b: texturesResolvedScoped is DERIVED, never a primitive - the tiers are the source of
	// truth and this is only their sum, so "scoped" cannot be inflated without a tier moving in
	// plain sight. texturesScopedAuthoritative is the STRICT number (tiers 1-3: the engine's own
	// lookup order); texturesScopedProvenance is the softer half (tiers 4-5, unique hits only).
	// ambiguousTextures is KEPT with its old meaning (== texturesTieBroken) purely so the pre-fix
	// 2,390 stays directly comparable; texturesAmbiguousTotal is the partition's total, and
	// scoped + tieBroken == texturesAmbiguousTotal is a checkable identity, not a claim.
	const int32 TexturesScopedAuthoritative =
		TexturesFromEmbedded + TexturesFromArchetypeTxd + TexturesFromParentTxd;
	const int32 TexturesScopedProvenance = TexturesFromYtypNeighbour + TexturesFromSameSlot;
	const int32 TexturesResolvedScoped = TexturesScopedAuthoritative + TexturesScopedProvenance;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"assetPath\":\"%s\",\"geometries\":%d,\"geometriesDropped\":%d,")
		TEXT("\"geometryErrors\":[%s],\"geometriesWithoutUV\":%d,\"vertices\":%d,\"triangles\":%d,")
		TEXT("\"trianglesOutOfRange\":%d,\"trianglesDegenerate\":%d,")
		TEXT("\"boundTextures\":%d,\"texturesFromEmbedded\":%d,\"texturesResolvedScoped\":%d,")
		TEXT("\"texturesTieBroken\":%d,\"ambiguousTextures\":%d,\"texturesAmbiguousTotal\":%d,")
		TEXT("\"texturesFromArchetypeTxd\":%d,\"texturesFromParentTxd\":%d,")
		TEXT("\"texturesFromYtypNeighbour\":%d,\"texturesFromSameSlot\":%d,")
		TEXT("\"texturesScopedAuthoritative\":%d,\"texturesScopedProvenance\":%d,")
		TEXT("\"tieBreakEmbeddedNotImported\":%d,\"tieBreakNoScope\":%d,")
		TEXT("\"tieBreakSlotAmbiguous\":%d,\"tieBreakYtypAmbiguous\":%d,")
		TEXT("\"tieBreakScopeDictAbsent\":%d,\"tieBreakNameNotInScope\":%d,")
		TEXT("\"unsupportedByMaster\":%d,\"missingTextures\":%d,")
		TEXT("\"unmappedSamplers\":%d,\"slotsWithoutShaderDef\":%d,\"slotsWithoutMaterial\":%d,")
		TEXT("\"valueParamsSeen\":%d,\"valueParamsBound\":%d,")
		TEXT("\"valueParamsUnsupported\":%d,\"valueParamsDeduped\":%d,%s,\"slots\":[%s]}"),
		bMeshOk ? TEXT("true") : TEXT("false"),
		*PackageName, Geos.Num(), GeosFailed, *GeoErrors, GeometriesWithoutUV, TotalVerts, TotalTris,
		TrisOutOfRange, TrisDegenerate,
		BoundTextures, TexturesFromEmbedded, TexturesResolvedScoped,
		TexturesTieBroken, TexturesTieBroken, TexturesAmbiguousTotal,
		TexturesFromArchetypeTxd, TexturesFromParentTxd,
		TexturesFromYtypNeighbour, TexturesFromSameSlot,
		TexturesScopedAuthoritative, TexturesScopedProvenance,
		TieBreakEmbeddedNotImported, TieBreakNoScope,
		TieBreakSlotAmbiguous, TieBreakYtypAmbiguous,
		TieBreakScopeDictAbsent, TieBreakNameNotInScope,
		UnsupportedByMaster, MissingTextures, UnmappedSamplers,
		SlotsWithoutShaderDef, SlotsWithoutMaterial,
		ValueParamsSeen, ValueParamsBound, ValueParamsUnsupported, ValueParamsDeduped,
		*RudeBound::VerdictJson(Col, ColAssetPath), *SlotsJson);
}

// Read one integer field out of a tool's JSON verdict. Lifted to file scope from ImportYdrBatch's
// local lambda (2026-08-04) because THREE aggregators now need it and a second copy is how two
// batches drift apart. The leading quote in the needle is load-bearing: it stops "imported" from
// matching inside "texturesImported" and "triangles" from matching inside "trianglesDegenerate".
// A missing field reads 0, which is the right answer for a unit that predates the field.
int32 RudeSumField(const FString& Json, const TCHAR* Key)
{
	const FString Needle = FString::Printf(TEXT("\"%s\":"), Key);
	const int32 At = Json.Find(Needle);
	if (At == INDEX_NONE) { return 0; }
	return FCString::Atoi(*Json.Mid(At + Needle.Len()));
}

// Jenkins one-at-a-time over the LOWERCASED name - RAGE's name hash, pinned to QUARRY's
// convention (quarry/ngcrypto.py joaat: lowercase input; the unresolvable-name fallback is
// spelled "hash_%08X", UPPERCASE hex). ymap<->ytyp<->dictionary joins are hash-to-hash, so
// matching by hash is the join's native form, not a workaround.
// JSON string escape for text that rides inside the manifest (extension XML, names with quotes).
FString RudeJsonEscape(const FString& In)
{
	FString O;
	O.Reserve(In.Len() + 8);
	for (TCHAR C : In)
	{
		switch (C)
		{
		case TEXT('"'): O += TEXT("\\\""); break;
		case TEXT('\\'): O += TEXT("\\\\"); break;
		case TEXT('\n'): O += TEXT("\\n"); break;
		case TEXT('\r'): break;
		case TEXT('\t'): O += TEXT("\\t"); break;
		default: O.AppendChar(C);
		}
	}
	return O;
}

// Re-spell an XML subtree as text, as read: tag, attributes in file order, text content, children.
// FXmlFile parses but cannot write, and the entity's <extensions> must survive verbatim into the
// manifest and the component so export re-emits what the game shipped.
void RudeXmlEscapeInto(FString& O, const FString& In)
{
	for (TCHAR C : In)
	{
		switch (C)
		{
		case TEXT('&'): O += TEXT("&amp;"); break;
		case TEXT('<'): O += TEXT("&lt;"); break;
		case TEXT('>'): O += TEXT("&gt;"); break;
		case TEXT('"'): O += TEXT("&quot;"); break;
		default: O.AppendChar(C);
		}
	}
}
void RudeXmlNodeToString(const FXmlNode* N, FString& O, int32 Depth)
{
	if (!N) { return; }
	for (int32 i = 0; i < Depth; ++i) { O += TEXT(" "); }
	O += TEXT("<"); O += N->GetTag();
	for (const FXmlAttribute& A : N->GetAttributes())
	{
		O += TEXT(" "); O += A.GetTag(); O += TEXT("=\"");
		RudeXmlEscapeInto(O, A.GetValue());
		O += TEXT("\"");
	}
	const TArray<FXmlNode*>& Kids = N->GetChildrenNodes();
	const FString Text = N->GetContent().TrimStartAndEnd();
	if (Kids.Num() == 0 && Text.IsEmpty()) { O += TEXT(" />\n"); return; }
	O += TEXT(">");
	if (Kids.Num() == 0)
	{
		RudeXmlEscapeInto(O, Text);
		O += TEXT("</"); O += N->GetTag(); O += TEXT(">\n");
		return;
	}
	O += TEXT("\n");
	for (const FXmlNode* K : Kids) { RudeXmlNodeToString(K, O, Depth + 1); }
	for (int32 i = 0; i < Depth; ++i) { O += TEXT(" "); }
	O += TEXT("</"); O += N->GetTag(); O += TEXT(">\n");
}

static uint32 RudeJoaat(const FString& Name)
{
	uint32 H = 0;
	for (const TCHAR C : Name)
	{
		H += static_cast<uint8>(FChar::ToLower(C));
		H += H << 10;
		H ^= H >> 6;
	}
	H += H << 3;
	H ^= H >> 11;
	H += H << 15;
	return H;
}

// ImportYddEntry's body plus the texture scope, mirroring RudeImportYdrScoped - the dictionary
// lane resolves its archetype the same way the drawable lane does, and letting the two diverge is
// how one of them silently keeps guessing (2026-08-05, #43).
static FString RudeImportYddEntryScoped(const FString& XmlPath, const FString& EntryName,
                                        const FString& DestFolder, const FRudeTextureScope* TextureScope)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	const FString Entry = EntryName.TrimStartAndEnd();
	if (Entry.IsEmpty())
	{
		return Fail(TEXT("EntryName is empty"));
	}
	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("DrawableDictionary"))
	{
		return Fail(TEXT("root is not <DrawableDictionary>"));
	}

	// Entry match, hash-tolerant BOTH ways (measured corpus 2026-07-28: every sampled ydd
	// entry <Name> is a hash_%08X spelling; ytyp-side archetype names are hash_ in 72,067 of
	// 72,074 - so either side of the join may be the unresolved spelling of the other):
	//   1) plain case-insensitive name equality
	//   2) the entry is hash_<joaat(EntryName)> - we were given the real name
	//   3) EntryName is hash_XXXXXXXX and joaat(entry name) matches it - the entry resolved
	const FString WantHashName = FString::Printf(TEXT("hash_%08X"), RudeJoaat(Entry));
	uint32 WantHash = 0;
	bool bEntryIsHashName = false;
	if (Entry.Len() == 13 && Entry.StartsWith(TEXT("hash_"), ESearchCase::IgnoreCase))
	{
		bEntryIsHashName = true;
		WantHash = static_cast<uint32>(FCString::Strtoui64(*Entry.Mid(5), nullptr, 16));
	}

	const FXmlNode* Found = nullptr;
	int32 Entries = 0;
	FString Available;   // leading entry names for the loud not-found error
	for (const FXmlNode* Item : Root->GetChildrenNodes())
	{
		const FXmlNode* NameN = Item->FindChildNode(TEXT("Name"));
		const FString ItemName = NameN ? NameN->GetContent().TrimStartAndEnd() : FString();
		++Entries;
		if (ItemName.IsEmpty())
		{
			continue;
		}
		if (Available.Len() < 512)
		{
			Available += (Available.IsEmpty() ? TEXT("") : TEXT(", ")) + ItemName;
		}
		if (ItemName.Equals(Entry, ESearchCase::IgnoreCase) ||
		    ItemName.Equals(WantHashName, ESearchCase::IgnoreCase) ||
		    (bEntryIsHashName && RudeJoaat(ItemName) == WantHash))
		{
			Found = Item;
			break;
		}
	}
	if (!Found)
	{
		return Fail(FString::Printf(
			TEXT("entry '%s' not found in %s (%d entries; also tried %s). Entries here: %s"),
			*Entry, *FPaths::GetCleanFilename(XmlPath), Entries, *WantHashName, *Available));
	}
	// The ENTRY name becomes the asset name - it is the archetype-facing identity; the item's
	// own <Name> is usually an unresolvable hash_ spelling of the same thing.
	return ImportDrawableNode(Found, Entry, DestFolder, TextureScope);
}

FString URudeToolset::ImportYddEntry(const FString& XmlPath, const FString& EntryName,
                                     const FString& DestFolder)
{
	return RudeImportYddEntryScoped(XmlPath, EntryName, DestFolder, nullptr);
}

// Translucent sea material - a reference surface, deliberately simple (no waves/refraction;
// RUDE is a mapping DCC, not a renderer).
static UMaterialInterface* EnsureWaterMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_Water.M_RUDE_Water");
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, FullPath))
	{
		return Existing;
	}
	UPackage* Pkg = CreatePackage(TEXT("/RUDE/Masters/M_RUDE_Water"));
	if (!Pkg) { return nullptr; }
	UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_RUDE_Water"), RF_Public | RF_Standalone);
	M->BlendMode = BLEND_Translucent;
	M->TwoSided = true;
	auto* Col = NewObject<UMaterialExpressionVectorParameter>(M);
	Col->ParameterName = TEXT("WaterColor");
	Col->DefaultValue = FLinearColor(0.012f, 0.055f, 0.075f, 1.f);
	M->GetExpressionCollection().AddExpression(Col);
	auto* Op = NewObject<UMaterialExpressionScalarParameter>(M);
	Op->ParameterName = TEXT("Opacity"); Op->DefaultValue = 0.82f;
	M->GetExpressionCollection().AddExpression(Op);
	auto* Rough = NewObject<UMaterialExpressionScalarParameter>(M);
	Rough->ParameterName = TEXT("Roughness"); Rough->DefaultValue = 0.06f;
	M->GetExpressionCollection().AddExpression(Rough);
	auto* Spec = NewObject<UMaterialExpressionConstant>(M); Spec->R = 1.f;
	M->GetExpressionCollection().AddExpression(Spec);

	UMaterialEditorOnlyData* EO = M->GetEditorOnlyData();
	EO->BaseColor.Expression = Col;
	EO->Opacity.Expression = Op;
	EO->Roughness.Expression = Rough;
	EO->Specular.Expression = Spec;
	M->PostEditChange();
	Pkg->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(M);
	return M;
}

FString URudeToolset::SpawnSeaLevel(const FString& SizeMetres, const FString& ZMetres)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const double SizeM = FMath::Max(1.0, FCString::Atod(*SizeMetres));
	const double ZM = ZMetres.TrimStartAndEnd().IsEmpty() ? 0.0 : FCString::Atod(*ZMetres);
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!Plane) { return Fail(TEXT("engine Plane mesh not found")); }
	UMaterialInterface* Water = EnsureWaterMaster();

	// replace any previous sea plane (idempotent, like ImportScene)
	TArray<AActor*> Stale;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetFolderPath() == FName(TEXT("RUDE_ENV")) && It->GetActorLabel() == TEXT("RUDE_SeaLevel"))
		{
			Stale.Add(*It);
		}
	}
	for (AActor* A : Stale) { World->DestroyActor(A); }

	AActor* Actor = World->SpawnActor<AActor>();
	if (!Actor) { return Fail(TEXT("spawn failed")); }
	UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(Actor, TEXT("Sea"));
	Actor->SetRootComponent(SMC);
	SMC->SetStaticMesh(Plane);
	if (Water) { SMC->SetMaterial(0, Water); }
	SMC->SetMobility(EComponentMobility::Static);
	SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// engine Plane is 100x100cm -> scale to the requested half-extent (metres -> cm)
	const double Scale = (SizeM * 100.0 * 2.0) / 100.0;
	SMC->SetWorldTransform(FTransform(FQuat::Identity, FVector(0, 0, ZM * 100.0), FVector(Scale, Scale, 1.0)));
	SMC->RegisterComponent();
	Actor->AddInstanceComponent(SMC);
	Actor->SetActorLabel(TEXT("RUDE_SeaLevel"));
	Actor->SetFolderPath(FName(TEXT("RUDE_ENV")));
	World->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"actor\":\"RUDE_SeaLevel\",\"sizeM\":%.0f,\"zM\":%.2f}"), SizeM, ZM);
}

FString URudeToolset::CaptureView(const FString& CamSpec, const FString& OutPng,
                                  const FString& ViewMode, const FString& SettleSeconds)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// ⛔⛔ BLOCK UNTIL EVERY ASSET HAS FINISHED COMPILING, or this tool lies. The shot lands on the
	// NEXT DRAW, and a StaticMesh that is still compiling renders NOTHING - so a capture fired
	// straight after an import photographs a HALF-BUILT scene. Compilation completes
	// smallest-first, so the artifact is SIZE-CORRELATED: small props present, large meshes
	// missing. That is indistinguishable by eye from a real "big meshes don't render" defect, and
	// on 2026-07-28 it cost a whole false investigation (LOG: "CaptureView WITHOUT A COMPILE
	// BARRIER"). An unsynchronised screenshot is not a measurement.
	//
	// ⛔⛔ AND ASSET COMPILATION IS ONLY ONE OF FOUR THINGS TO WAIT ON (2026-07-29, Matt: "your
	// screenshots aren't accurate, they're being taken too early while everything is mounting").
	// The 07-28 fix blocked on FinishAllCompilation and stopped there, so the shot still fired
	// while the scene was mid-mount. Each remaining gate fails as a DIFFERENT convincing lie:
	//   · shaders still compiling  -> default material, i.e. flat grey
	//   · levels still streaming   -> actors simply absent
	//   · TEXTURE MIPS not resident-> surfaces draw untextured. ⭐ THIS is the one that makes a
	//     correctly-bound city photograph as an untextured one, which is exactly the "most of it
	//     isn't textured" reading this tool produced.
	// All four must be closed before the camera moves, or the picture is not evidence.
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	if (UWorld* CapWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		CapWorld->FlushLevelStreaming(EFlushLevelStreamingType::Full);
	}
	// Blocking: pull every streamable texture to full residency rather than letting the streamer
	// decide from a camera position it has not seen yet.
	IStreamingManager::Get().StreamAllResources(0.0f);

	TArray<FString> C;
	// Semicolons are accepted as separators because -ExecCmds splits its command list on
	// commas - a comma CamSpec cannot survive the launch-argument path at all.
	CamSpec.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(C, TEXT(","), true);
	if (C.Num() != 5) { return Fail(TEXT("CamSpec must be \"x,y,z,pitch,yaw\" (or ;-separated)")); }
	const FVector Loc(FCString::Atod(*C[0]), FCString::Atod(*C[1]), FCString::Atod(*C[2]));
	const FRotator Rot(FCString::Atod(*C[3]), FCString::Atod(*C[4]), 0.0);
	for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
	{
		if (VC && VC->IsPerspective())
		{
			// ⛔ WHY UNLIT EXISTS (2026-07-29): "did the textures bind?" is a question about
			// ALBEDO, and a Lit shot answers a different question - it multiplies albedo by the
			// level's lighting, so an untextured scene and a correctly-textured scene under a dark
			// sky look nearly identical grey. That confound cost a full re-litigation of the
			// texture lane. UNLIT shows base colour and nothing else, which is the property under
			// test. Lit remains the default: it is what the operator actually ships.
			const FString VM = ViewMode.TrimStartAndEnd();
			if (VM.Equals(TEXT("UNLIT"), ESearchCase::IgnoreCase))
			{
				VC->SetViewMode(VMI_Unlit);
			}
			else if (VM.Equals(TEXT("WIREFRAME"), ESearchCase::IgnoreCase))
			{
				VC->SetViewMode(VMI_Wireframe);
			}
			else if (!VM.IsEmpty() && !VM.Equals(TEXT("LIT"), ESearchCase::IgnoreCase))
			{
				return Fail(TEXT("ViewMode must be LIT, UNLIT or WIREFRAME"));
			}
			else
			{
				VC->SetViewMode(VMI_Lit);
			}
			VC->SetViewLocation(Loc);
			VC->SetViewRotation(Rot);
			VC->Invalidate();

			// ⭐⭐ THE CAPTURE IS DEFERRED, NOT FORCED - and that is the whole lesson.
			// Three attempts to make this synchronous each produced a NEW false reading: no
			// barrier (half-built scene), a compile-only barrier (still mid-mount), and a blocking
			// wait (ok:false for a shot that landed 140s later). Matt, twice: "your screenshots
			// are being taken too early while everything is mounting."
			// The editor settles on its OWN tick and nothing a command does from inside that tick
			// can present a frame. So stop fighting it: set the camera now (the streamer needs the
			// viewpoint to start pulling for it), then hand a ticker the job of firing the shot
			// once the world has actually gone quiet across REAL frames. Sky/reflection capture,
			// mip residency and shader compilation all resolve in that window - none of them can
			// be waited on from here.
			GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies*/ true);
			IStreamingManager::Get().StreamAllResources(0.0f);

			const float MinSettle = SettleSeconds.IsEmpty()
				? 25.0f : FMath::Clamp(FCString::Atof(*SettleSeconds), 0.0f, 600.0f);
			const double StartedAt = FPlatformTime::Seconds();
			const FString PngPath = OutPng;
			// Quiet must be SUSTAINED: compilation dips to zero between batches, so a single
			// quiet sample fires early. Require several consecutive quiet ticks.
			TSharedRef<int32> QuietTicks = MakeShared<int32>(0);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[PngPath, StartedAt, MinSettle, QuietTicks](float) -> bool
				{
					const bool bBusy =
						FAssetCompilingManager::Get().GetNumRemainingAssets() > 0
						|| (GShaderCompilingManager && GShaderCompilingManager->IsCompiling());
					*QuietTicks = bBusy ? 0 : (*QuietTicks + 1);
					const double Elapsed = FPlatformTime::Seconds() - StartedAt;
					if (Elapsed < MinSettle || *QuietTicks < 6)
					{
						return true;   // keep ticking
					}
					IStreamingManager::Get().StreamAllResources(0.0f);
					FScreenshotRequest::RequestScreenshot(PngPath, /*bShowUI*/ false,
					                                      /*bAddFilenameSuffix*/ false);
					UE_LOG(LogTemp, Display,
						TEXT("[RUDE] CaptureView: scene quiet after %.1fs, shot requested -> %s"),
						Elapsed, *PngPath);
					return false;  // done
				}), 0.5f);

			// ⭐ Because each capture now waits for its own settle on a real tick, a LIT+UNLIT
			// pair in ONE chain no longer clobbers itself the way the old immediate requests did -
			// but they must be given DIFFERENT settle times so the frames they fire on are
			// genuinely separate. Same settle in one chain still collapses to one file.
			return FString::Printf(
				TEXT("{\"ok\":true,\"requested\":\"%s\",\"settleSeconds\":%.1f,\"note\":\"")
				TEXT("deferred - fires once compilation is quiet for 6 consecutive ticks AND ")
				TEXT("%.0fs have passed; poll for the file\"}"),
				*OutPng, MinSettle, MinSettle);
		}
	}
	return Fail(TEXT("no perspective level viewport"));
}

// ---- RUDE FILEBASE ---------------------------------------------------------------
// The user exports their own game files into this tree. Its job is to keep
// BUILD-VERSION-ACCURATE assets separable: the same name (prop_x.ydr) legitimately
// exists in the base game, in update.rpf, and in several DLC packs, and the LAST one
// in load order is the one the game actually uses. Numeric prefixes make that order
// explicit on disk, so a resolver just walks folders high-to-low.
namespace RudeFilebase
{
	// Types RUDE consumes directly, then context types worth keeping alongside.
	static const TCHAR* CORE_TYPES[] = { TEXT("ydr"), TEXT("ydd"), TEXT("ytd"), TEXT("ybn"),
	                                     TEXT("ytyp"), TEXT("ymap") };
	static const TCHAR* ALL_TYPES[] = { TEXT("ydr"), TEXT("ydd"), TEXT("ytd"), TEXT("ybn"),
	                                    TEXT("ytyp"), TEXT("ymap"), TEXT("yft"), TEXT("ycd"),
	                                    TEXT("ynv"), TEXT("ynd"), TEXT("yed"), TEXT("ymt"),
	                                    TEXT("ymf"), TEXT("ypt"), TEXT("yld"), TEXT("awc"),
	                                    TEXT("rel"), TEXT("meta"), TEXT("gxt2"), TEXT("xml") };

	static int32 MakeTypeFolders(const FString& Base, bool bAll)
	{
		int32 n = 0;
		const TCHAR* const* Types = bAll ? ALL_TYPES : CORE_TYPES;
		const int32 Count = bAll ? UE_ARRAY_COUNT(ALL_TYPES) : UE_ARRAY_COUNT(CORE_TYPES);
		for (int32 i = 0; i < Count; ++i)
		{
			if (IFileManager::Get().MakeDirectory(*(Base / Types[i]), true)) { ++n; }
		}
		return n;
	}
}

FString URudeToolset::IngestExport(const FString& DumpFolder, const FString& SourceName,
                                   const FString& FilebaseRoot, const FString& Move)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	IFileManager& FM = IFileManager::Get();
	if (!FPaths::DirectoryExists(DumpFolder)) { return Fail(TEXT("DumpFolder does not exist")); }
	if (!FPaths::DirectoryExists(FilebaseRoot)) { return Fail(TEXT("FilebaseRoot does not exist - run CreateFilebase first")); }
	const bool bMove = !Move.TrimStartAndEnd().Equals(TEXT("COPY"), ESearchCase::IgnoreCase);

	FString Src = SourceName.TrimStartAndEnd();
	if (Src.IsEmpty())
	{
		FString Trimmed = DumpFolder;
		while (Trimmed.EndsWith(TEXT("/")) || Trimmed.EndsWith(TEXT("\\")))
		{
			Trimmed.LeftChopInline(1);
		}
		Src = FPaths::GetCleanFilename(Trimmed);
	}
	const FString SrcLower = Src.ToLower();

	// resolve the destination slot: base / update / a numbered DLC folder
	FString Dest;
	if (SrcLower == TEXT("base") || SrcLower.StartsWith(TEXT("x64")) || SrcLower == TEXT("common"))
	{
		Dest = FilebaseRoot / TEXT("00_base");
	}
	else if (SrcLower.StartsWith(TEXT("update")))
	{
		Dest = FilebaseRoot / TEXT("10_update");
	}
	else
	{
		TArray<FString> DlcDirs;
		FM.FindFiles(DlcDirs, *(FilebaseRoot / TEXT("20_dlc") / TEXT("*")), false, true);
		for (const FString& D : DlcDirs)
		{
			// folders are NNN_<name>; match on the name half so callers never type numbers
			FString Name = FPaths::GetCleanFilename(D);
			int32 us;
			if (Name.FindChar(TEXT('_'), us)) { Name = Name.Mid(us + 1); }
			if (Name.Equals(SrcLower, ESearchCase::IgnoreCase))
			{
				Dest = FilebaseRoot / TEXT("20_dlc") / FPaths::GetCleanFilename(D);
				break;
			}
		}
		if (Dest.IsEmpty())
		{
			return Fail(FString::Printf(
				TEXT("unknown source '%s' - use base, update, or a DLC pack name from _FILEBASE.json"), *Src));
		}
	}

	// every file, recursively; type = the extension before any .xml
	TArray<FString> Files;
	FM.FindFilesRecursive(Files, *DumpFolder, TEXT("*.*"), true, false);
	TMap<FString, int32> ByType;
	int32 Filed = 0, Skipped = 0;
	for (const FString& F : Files)
	{
		FString Name = FPaths::GetCleanFilename(F);
		FString Type = FPaths::GetExtension(Name).ToLower();
		if (Type == TEXT("xml"))
		{
			// prop_x.ydr.xml -> ydr
			FString Base = FPaths::GetBaseFilename(Name);
			Type = FPaths::GetExtension(Base).ToLower();
		}
		if (Type.IsEmpty() || Type == TEXT("rpf")) { ++Skipped; continue; }
		const FString TypeDir = Dest / Type;
		FM.MakeDirectory(*TypeDir, true);
		const FString Target = TypeDir / Name;
		bool bOk = bMove ? FM.Move(*Target, *F, true) : (FM.Copy(*Target, *F, true) == COPY_OK);
		if (bOk) { ++Filed; ByType.FindOrAdd(Type)++; }
		else { ++Skipped; }
		if (Filed && Filed % 2000 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] IngestExport %d filed..."), Filed);
		}
	}
	FString TypeJson;
	for (const TPair<FString, int32>& P : ByType)
	{
		TypeJson += FString::Printf(TEXT("%s\"%s\":%d"), TypeJson.IsEmpty() ? TEXT("") : TEXT(","),
			*P.Key, P.Value);
	}
	return FString::Printf(TEXT(
		"{\"ok\":true,\"source\":\"%s\",\"dest\":\"%s\",\"filed\":%d,\"byType\":{%s},\"skipped\":%d}"),
		*Src, *FPaths::GetCleanFilename(Dest), Filed, *TypeJson, Skipped);
}

FString URudeToolset::CreateFilebase(const FString& FilebaseRoot, const FString& GameRoot,
                                     const FString& Options)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	IFileManager& FM = IFileManager::Get();
	if (!FPaths::DirectoryExists(GameRoot)) { return Fail(TEXT("GameRoot does not exist")); }
	const bool bAll = Options.TrimStartAndEnd().Equals(TEXT("ALL"), ESearchCase::IgnoreCase);

	// --- read the user's install: base archives + DLC pack NAMES (directory listing
	// only; nothing is opened, decrypted, copied or redistributed) ---
	TArray<FString> BaseArchives;
	FM.FindFiles(BaseArchives, *(GameRoot / TEXT("*.rpf")), true, false);
	BaseArchives.Sort();

	TArray<FString> DlcDirs;
	const FString DlcRoot = GameRoot / TEXT("update/x64/dlcpacks");
	FM.FindFiles(DlcDirs, *(DlcRoot / TEXT("*")), false, true);
	// Heuristic order: year-bearing pack names are newer, so they sort last; otherwise
	// alphabetical. NOT authoritative - the real order lives in dlclist.xml inside the
	// encrypted update.rpf. The manifest says so, and _manifest/ has a slot for it.
	DlcDirs.Sort([](const FString& A, const FString& B)
	{
		auto YearOf = [](const FString& S) -> int32
		{
			for (int32 Y = 2013; Y <= 2035; ++Y)
			{
				if (S.Contains(FString::FromInt(Y))) { return Y; }
			}
			return 0;
		};
		const int32 YA = YearOf(A), YB = YearOf(B);
		if (YA != YB) { return YA < YB; }
		return A < B;
	});

	// --- build the tree ---
	int32 Folders = 0;
	auto Mk = [&](const FString& P) { if (FM.MakeDirectory(*P, true)) { ++Folders; } };
	Mk(FilebaseRoot);
	Mk(FilebaseRoot / TEXT("_manifest"));
	Mk(FilebaseRoot / TEXT("_incoming"));
	const FString BaseDir = FilebaseRoot / TEXT("00_base");
	const FString UpdDir = FilebaseRoot / TEXT("10_update");
	Mk(BaseDir); Mk(UpdDir);
	// Type folders are created ON DEMAND by IngestExport - pre-seeding hundreds of empty
	// ones only made the tree look like work the user has to do. "ALL" restores them.
	if (bAll)
	{
		Folders += RudeFilebase::MakeTypeFolders(BaseDir, true);
		Folders += RudeFilebase::MakeTypeFolders(UpdDir, true);
	}
	const FString DlcOut = FilebaseRoot / TEXT("20_dlc");
	Mk(DlcOut);
	FString DlcJson;
	for (int32 i = 0; i < DlcDirs.Num(); ++i)
	{
		const FString Name = FPaths::GetCleanFilename(DlcDirs[i]);
		const FString Dir = DlcOut / FString::Printf(TEXT("%03d_%s"), i + 1, *Name);
		Mk(Dir);
		if (bAll) { Folders += RudeFilebase::MakeTypeFolders(Dir, false); }
		DlcJson += FString::Printf(TEXT("%s\n  {\"order\": %d, \"name\": \"%s\", \"folder\": \"%s\"}"),
			i ? TEXT(",") : TEXT(""), i + 1, *Name, *FPaths::GetCleanFilename(Dir));
	}

	// --- build fingerprint: identifies WHICH game build this filebase was cut for ---
	FString ExeName = TEXT("GTA5.exe");
	int64 ExeSize = FM.FileSize(*(GameRoot / ExeName));
	if (ExeSize <= 0) { ExeName = TEXT("GTA5_Enhanced.exe"); ExeSize = FM.FileSize(*(GameRoot / ExeName)); }
	const FDateTime ExeStamp = FM.GetTimeStamp(*(GameRoot / ExeName));

	FString BaseJson;
	for (int32 i = 0; i < BaseArchives.Num(); ++i)
	{
		BaseJson += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(", ") : TEXT(""), *BaseArchives[i]);
	}
	// ⭐ SAME SHAPE AS QUARRY'S _FILEBASE.json, deliberately (Matt's call, 2026-07-27): QUARRY OWNS
	// the project tree, and this is the no-QUARRY fallback. Two tools writing the same contract in
	// two shapes is how a contract drifts - which already cost us once when QUARRY emitted binary
	// and the importer read XML with nothing to announce the mismatch. Keys mirror `quarry.py`'s
	// `write_manifest`; `createdBy` is the only addition, so a consumer can tell which tool cut it.
	const FString Manifest = FString::Printf(TEXT(
		"{\n"
		" \"quarryVersion\": 1,\n"
		" \"createdBy\": \"RUDE CreateFilebase\",\n"
		" \"title\": \"%s\",\n"
		" \"gameRoot\": \"%s\",\n"
		" \"build\": { \"exe\": \"%s\", \"bytes\": %lld, \"modified\": \"%s\" },\n"
		" \"precedence\": [\"00_base\", \"10_update\", \"20_dlc/<order>_<name>\"],\n"
		" \"precedenceNote\": \"Later wins. A name in several sources resolves to the "
		"highest-ordered copy - that is what keeps a project build-accurate.\",\n"
		" \"dlcOrderAuthoritative\": false,\n"
		" \"dlcOrderNote\": \"HEURISTIC (year-bearing names last, else alphabetical). RUDE cannot "
		"open update.rpf to read the real dlclist.xml - it ships no archive or crypto code by "
		"design. Run QUARRY's init/extract for an authoritative order; do not author a DLC "
		"override against this one.\",\n"
		" \"baseArchives\": [%s],\n"
		" \"dlcPacks\": [%s\n ]\n}\n"),
		ExeName.Equals(TEXT("GTA5_Enhanced.exe")) ? TEXT("gtav-enhanced") : TEXT("gtav-legacy"),
		*GameRoot.ReplaceCharWithEscapedChar(), *ExeName, ExeSize, *ExeStamp.ToString(),
		*BaseJson, *DlcJson);
	FFileHelper::SaveStringToFile(Manifest, *(FilebaseRoot / TEXT("_FILEBASE.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const FString Readme = FString::Printf(TEXT(
		"# RUDE Filebase\n\n"
		"This is an EMPTY folder tree, shaped to your own game install. Nothing here was read out\n"
		"of your game - RUDE only listed directory names. Filling it is a separate step.\n\n"
		"## The easy path: QUARRY\n\n"
		"QUARRY reads your own archives and fills this tree for you, in the right order:\n\n"
		"    quarry.py extract --game \"<your install>\" --out \"<this folder>\" --xml \\\n"
		"                      --types ydr,ytd,ytyp,ymap\n"
		"    quarry.py meta    --out \"<this folder>\"\n"
		"    quarry.py resolve --out \"<this folder>\"\n\n"
		"It also reads the real DLC load order out of your update.rpf, which RUDE cannot - RUDE\n"
		"ships no archive or crypto code by design, so the order below is only a guess.\n\n"
		"## The other path: an extractor you already have\n\n"
		"Export ONE source at a time (e.g. `x64a.rpf`, or `update.rpf`, or a single DLC) with\n"
		"whatever extraction tool you already use. Dump it anywhere - a flat folder is fine, do\n"
		"NOT sort it. Then tell RUDE to file it:\n\n"
		"    IngestExport(DumpFolder, SourceName, FilebaseRoot)\n"
		"      SourceName = \"base\", \"update\", or the DLC pack name (e.g. \"mpbiker\")\n\n"
		"RUDE sorts every file by type into the correct precedence slot. You never create a\n"
		"folder, never type a number, never sort anything by hand.\n\n"
		"Repeat per source. Start with `base` and `update` - that is the city; DLC packs only\n"
		"matter when you want their content.\n\n"
		"## What the numbers mean (you can ignore them)\n"
		"The same asset name exists in the base game, in update.rpf, and in several DLC packs;\n"
		"the game uses the LAST one in load order. The folders encode that order so RUDE always\n"
		"resolves the build-accurate copy:\n\n"
		"    00_base/             the base x64*.rpf / common.rpf archives\n"
		"    10_update/           update.rpf - overrides base\n"
		"    20_dlc/NNN_<name>/   DLC packs, higher NNN wins\n\n"
		"Type folders (`ydr/ ytd/ ybn/ ...`) are created for you as files arrive.\n"
		"Keep sources in their own slots - not merging them is what makes this work.\n\n"
		"## Slots\n"
		"    _manifest/    scratch space for anything that pins this build\n"
		"    _incoming/    somewhere to dump before ingesting, if you want it\n\n"
		"WARNING: the DLC order below is a GUESS (year-bearing names last, else alphabetical).\n"
		"The real order lives in dlclist.xml inside the encrypted update.rpf. If you intend to\n"
		"author an override that must land above a particular DLC, use QUARRY - guessing wrong\n"
		"means your override loses silently.\n\n"
		"## This filebase was cut for\n"
		"    %s  (%s, %lld bytes, modified %s)\n"
		"    %d base archives, %d DLC packs\n\n"
		"Re-run CreateFilebase after a game patch: the build fingerprint in _FILEBASE.json is how\n"
		"a mismatch gets caught before it corrupts a project.\n"),
		*GameRoot, *ExeName, ExeSize, *ExeStamp.ToString(), BaseArchives.Num(), DlcDirs.Num());
	FFileHelper::SaveStringToFile(Readme, *(FilebaseRoot / TEXT("README.md")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	return FString::Printf(TEXT(
		"{\"ok\":true,\"root\":\"%s\",\"dlcPacks\":%d,\"baseArchives\":%d,\"foldersCreated\":%d,"
		"\"typeMode\":\"%s\"}"),
		*FilebaseRoot, DlcDirs.Num(), BaseArchives.Num(), Folders, bAll ? TEXT("ALL") : TEXT("CORE"));
}

// ---- shared corpus resolution (ImportMapArea + ImportMlo) --------------------------------
// The archetype index and the per-drawable ydr/yft/ydd import lane started life inside
// ImportMapArea (in-game-proven: downtown 13,135 instances, LOG "FRAGMENT LANE"). ImportMlo
// needs the SAME resolution for an MLO's interior entities - factored out rather than
// copied, so the two tools cannot drift. Behavior-preserving extraction, not a rewrite.
// ---- gtxd: RAGE's PARENT-TEXTURE-DICTIONARY chain ---------------------------------------
// ⭐ 2026-08-05 (#43 tier 3). When an asset's own txd does not hold a texture, the engine walks
// the parent chain declared in CMapParentTxds. That table ships as `gtxd.ymt` - an RBF0 binary,
// which QUARRY's ymt stage refuses (35 RBF0 refusals on record), so the plugin reads it directly.
// ⛔ THE FORMAT WAS DERIVED FROM THE BYTES, not from any third-party implementation, and the
// derivation is checkable: both files in the corpus parse to EXACTLY their own length with a
// balanced element stack (gtxd.ymt 210,467 bytes -> 2,983 relationships; mph4_gtxd.ymt 3,848 ->
// 45), every `item` carries both a <parent> and a <child>, and every child has exactly one
// parent (2,983 distinct children of 2,983 items - a tree, not a graph).
//   header : "RBF0"
//   token  : uint16 id.  0xFFFF closes the current element.  0xFFFD introduces a VALUE:
//            uint32 byte-length then that many bytes, NUL-terminated ASCII.
//            anything else OPENS an element with that id, followed by uint16 nameLength; when
//            nameLength > 0 the name follows and DEFINES id (plus two pad bytes), when it is 0
//            the id was defined earlier. Four more bytes follow in both cases.
// ⛔ REFUSE, DO NOT GUESS: any deviation (short read, undefined id, unbalanced stack, non-zero
// pad/tail) abandons THAT FILE and is counted. A desynchronised parse would emit plausible-looking
// parent relationships, and a wrong parent is a wrong texture - strictly worse than no chain.
static bool RudeReadParentTxdFile(const TArray<uint8>& B, TMap<FString, FString>& Out, int32& Added)
{
	if (B.Num() < 8 || B[0] != 'R' || B[1] != 'B' || B[2] != 'F' || B[3] != '0') { return false; }
	auto U16 = [&B](int32 P) { return (uint16)(B[P] | ((uint16)B[P + 1] << 8)); };
	auto U32 = [&B](int32 P)
	{
		return (uint32)B[P] | ((uint32)B[P + 1] << 8) | ((uint32)B[P + 2] << 16) | ((uint32)B[P + 3] << 24);
	};
	TMap<uint16, FString> Names;
	TArray<FString> Stack;
	FString Parent, Child, Pending;
	int32 P = 4;
	while (P < B.Num())
	{
		if (P + 2 > B.Num()) { return false; }
		const uint16 Tag = U16(P); P += 2;
		if (Tag == 0xFFFF)
		{
			if (Stack.Num() == 0) { return false; }
			const FString Elem = Stack.Pop();
			if (Elem == TEXT("parent")) { Parent = Pending; }
			else if (Elem == TEXT("child")) { Child = Pending; }
			else if (Elem == TEXT("item"))
			{
				if (Parent.IsEmpty() || Child.IsEmpty()) { return false; }
				const FString C = Child.ToLower();
				if (!Out.Contains(C)) { Out.Add(C, Parent.ToLower()); ++Added; }
				Parent.Empty(); Child.Empty();
			}
			Pending.Empty();
			continue;
		}
		if (Tag == 0xFFFD)
		{
			if (P + 4 > B.Num()) { return false; }
			const int32 Len = (int32)U32(P); P += 4;
			if (Len < 0 || P + Len > B.Num()) { return false; }
			FString S;
			for (int32 i = 0; i < Len; ++i)
			{
				const uint8 C = B[P + i];
				if (C == 0) { break; }
				if (C > 0x7F) { return false; }   // non-ASCII in a dictionary name = not this shape
				S.AppendChar((TCHAR)C);
			}
			P += Len;
			Pending = S;
			continue;
		}
		if (P + 2 > B.Num()) { return false; }
		const uint16 NameLen = U16(P); P += 2;
		if (NameLen > 0)
		{
			if (P + NameLen + 2 > B.Num()) { return false; }
			FString Nm;
			for (int32 i = 0; i < NameLen; ++i)
			{
				const uint8 C = B[P + i];
				if (C == 0 || C > 0x7F) { return false; }
				Nm.AppendChar((TCHAR)C);
			}
			P += NameLen;
			if (B[P] != 0 || B[P + 1] != 0) { return false; }   // pad must be zero, or we are desynced
			P += 2;
			Names.Add(Tag, Nm);
		}
		if (P + 4 > B.Num()) { return false; }
		if (B[P] != 0 || B[P + 1] != 0 || B[P + 2] != 0 || B[P + 3] != 0) { return false; }
		P += 4;
		const FString* Known = Names.Find(Tag);
		if (!Known) { return false; }            // an id used before it was ever defined
		Stack.Add(*Known);
		Pending.Empty();
	}
	return Stack.Num() == 0;
}

// Read every CMapParentTxds table under <CorpusRoot>/ymt. Selection is by CONTENT (RBF0 header
// naming CMapParentTxds), never by filename - the corpus holds gtxd.ymt and mph4_gtxd.ymt today
// and a census is a lower bound, so a DLC whose table is named differently must still be found.
static void RudeReadParentTxds(const FRudeCorpus& Corpus, TMap<FString, FString>& Out,
                               int32& Files, int32& Relationships, int32& Refused)
{
	// The corpus converts CMapParentTxds (gtxd.ymt, RBF0) to "<name>.ymt.rbf.xml". Every copy across
	// slots is read lowest-slot first so a DLC's table overrides the base's for the same child -
	// the game's own load order. A copy still kept binary goes through the RBF0 reader.
	TArray<const FRudeCorpusEntry*> Rows;
	Corpus.AllOfType(TEXT("ymt"), Rows);
	for (const FRudeCorpusEntry* E : Rows)
	{
		if (E->Name != TEXT("gtxd")) { continue; }
		const FString Path = Corpus.PathOf(*E);
		if (E->bConverted)
		{
			FXmlFile Xml(Path);
			if (!Xml.IsValid()) { ++Refused; continue; }
			const FXmlNode* Root = Xml.GetRootNode();
			const FXmlNode* Rel = Root ? Root->FindChildNode(TEXT("txdRelationships")) : nullptr;
			if (!Rel) { ++Refused; continue; }
			++Files;
			for (const FXmlNode* Item : Rel->GetChildrenNodes())
			{
				const FXmlNode* P = Item->FindChildNode(TEXT("parent"));
				const FXmlNode* C = Item->FindChildNode(TEXT("child"));
				if (!P || !C) { continue; }
				const FString Child = C->GetContent().TrimStartAndEnd().ToLower();
				const FString Parent = P->GetContent().TrimStartAndEnd().ToLower();
				if (Child.IsEmpty() || Parent.IsEmpty()) { continue; }
				Out.Add(Child, Parent);
				++Relationships;
			}
			continue;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { ++Refused; continue; }
		if (Bytes.Num() < 32 || Bytes[0] != 'R' || Bytes[1] != 'B' || Bytes[2] != 'F' || Bytes[3] != '0') { continue; }
		++Files;
		int32 Added = 0;
		if (RudeReadParentTxdFile(Bytes, Out, Added)) { Relationships += Added; }
		else { ++Refused; }
	}
}

// ---- _RESOLVED.json: which build SLOT each file was won from ------------------------------
// ⭐ 2026-08-05 (#43 tier 5). `winners` is a FLAT object of "<type>/<name>.<ext>": "<slot>", so
// this reads it with a small purpose-built string scanner rather than a general JSON parse: the
// file is ~15 MB / 262,124 entries and only four of its eight lanes are wanted here. Only the
// two escapes JSON can legally put in these strings are handled (\\ and \"), and anything else
// aborts the read rather than inventing a value.
static void RudeReadCorpusSlots(const FRudeCorpus& Corpus, TMap<FString, FString>& DictSlot,
                                TMap<FString, FString>& AssetSlot, int32& Entries)
{
	// Tier 5 of the texture scope: which build slot an asset resolves from. The ledger carries
	// every copy; Effective() is the copy the game loads. (v1 read an agent-built _RESOLVED.json
	// for this; that file was a derived view Matt killed - the ledger is the source.)
	static const TCHAR* DictTypes[] = { TEXT("ytd") };
	static const TCHAR* AssetTypes[] = { TEXT("ydr"), TEXT("yft"), TEXT("ydd") };
	TArray<const FRudeCorpusEntry*> Rows;
	for (const TCHAR* T : DictTypes)
	{
		Rows.Reset();
		Corpus.ByPrefix(T, TEXT(""), Rows);
		for (const FRudeCorpusEntry* E : Rows) { DictSlot.Add(E->Name, E->Slot); ++Entries; }
	}
	for (const TCHAR* T : AssetTypes)
	{
		Rows.Reset();
		Corpus.ByPrefix(T, TEXT(""), Rows);
		for (const FRudeCorpusEntry* E : Rows)
		{
			AssetSlot.Add(E->Name, E->Slot);
			DictSlot.Add(E->Name + TEXT("__embedded"), E->Slot);
			++Entries;
		}
	}
}

struct FRudeArchetypeIndex
{
	TMap<FString, FString> ArchToAsset;   // lowercase archetype name -> lowercase drawable asset
	TSet<FString> FragmentAssets;         // assets that resolve under yft/ instead of ydr/
	TMap<FString, FString> DictEntries;   // entry mesh name -> ydd dictionary stem (under ydd/)
	// ⭐ timeFlags per archetype: a 24-bit hour mask on CTimeArchetypeDef (bit N = visible during
	// hour N). 3,936 archetypes carry one; the common masks are night windows (0-5 + 20-23). THIS
	// is how the game shows lit windows after dusk - by swapping which archetype is visible, not by
	// changing a material. Captured so the behaviour can be driven in UE and still round-trip.
	TMap<FString, uint32> ArchTimeFlags;  // lowercase archetype name -> hour mask
	TMap<FString, float> ArchRadius;      // lowercase archetype name -> bsRadius (m), the size the definition claims
	// ⭐ 2026-08-05 (#43): the archetype's declared <textureDictionary>, keyed by the DRAWABLE ASSET
	// the archetype resolves to - which is the key ImportIndexedDrawable and ImportYdrBatch have in
	// hand. This is the ONLY place the shared-txd scope exists: a .ydr.xml declares its EMBEDDED
	// dictionary and nothing else, so without walking the ytyp there is no way for the importer to
	// know which of 8,791 multi-dictionary names it is supposed to want. The walk already parses
	// every archetype once; this is one more FindChildNode on data already in memory.
	TMap<FString, FString> AssetTxd;      // lowercase drawable asset -> lowercase texture dictionary
	// ⭐ 2026-08-05 (#43/#21b): the three scoping signals BEYOND the archetype's own dictionary.
	TMap<FString, FString> ParentTxd;     // gtxd CMapParentTxds: child dictionary -> parent
	TArray<TArray<FString>> YtypTxdSets;  // per ytyp FILE, its declared dictionaries (sorted, unique)
	TMap<FString, int32> AssetYtyp;       // asset -> index into YtypTxdSets (first file, files sorted)
	TMap<FString, FString> DictSlot;      // dictionary -> the build slot it was won from
	TMap<FString, FString> AssetSlot;     // drawable asset -> the build slot it was won from
	int32 GtxdFiles = 0, GtxdRelationships = 0, GtxdRefusals = 0, ResolvedEntries = 0;
	TSharedPtr<FRudeCorpus> Corpus;      // the ledger index every lookup below goes through

	// Assemble everything provable about ONE asset. Nothing is inferred from a path or a filename.
	FRudeTextureScope MakeScope(const FString& AssetLower) const
	{
		FRudeTextureScope S;
		if (const FString* T = AssetTxd.Find(AssetLower)) { S.ArchetypeTxd = *T; }
		// Walk the parent chain nearest-ancestor-first. The corpus tables are a tree (every child
		// has exactly one parent, measured), but a cycle in DLC data would hang the import, so the
		// visited set and the depth cap are load-bearing, not decoration.
		FString Cur = S.ArchetypeTxd;
		TSet<FString> Seen;
		if (!Cur.IsEmpty()) { Seen.Add(Cur); }
		while (!Cur.IsEmpty() && S.ParentTxdChain.Num() < 16)
		{
			const FString* P = ParentTxd.Find(Cur);
			if (!P || Seen.Contains(*P)) { break; }
			S.ParentTxdChain.Add(*P);
			Seen.Add(*P);
			Cur = *P;
		}
		if (const int32* Y = AssetYtyp.Find(AssetLower))
		{
			if (YtypTxdSets.IsValidIndex(*Y)) { S.YtypNeighbours = &YtypTxdSets[*Y]; }
		}
		if (const FString* Sl = AssetSlot.Find(AssetLower)) { S.AssetSlot = *Sl; }
		S.DictSlots = &DictSlot;
		return S;
	}
};

// Optional MLO lookup riding the index walk: the walk already parses every ytyp once, and a
// second whole-corpus scan for one archetype would double the tool's dominant XML cost.
// Matching is hash-tolerant BOTH ways, the ImportYddEntry convention - MLO archetype names
// are hash_XXXXXXXX in the corpus whenever the reverse table lacks them (e.g. the trailer
// interior stores as hash_CB21C865 == joaat("ch3_01_trlr_int")).
struct FRudeMloSearch
{
	FString Wanted;                // caller's spelling
	FString WantHashName;          // "hash_%08X" of joaat(Wanted)
	uint32 WantHash = 0;           // parsed hash when Wanted is itself hash_XXXXXXXX
	bool bWantedIsHashName = false;
	FString FoundFile;             // absolute path of the declaring ytyp XML
	FString FoundName;             // the archetype <name> as the corpus stores it
	int32 MloSeen = 0;             // CMloArchetypeDef items encountered corpus-wide
	FString Sample;                // leading MLO names for the loud not-found error
};

static bool BuildCorpusArchetypeIndex(const FString& CorpusRoot, FRudeArchetypeIndex& Out,
                                      FString& Error, FRudeMloSearch* MloSearch)
{
	// One archetype's texture dictionary, recorded against the asset it resolves to. Two archetypes
	// CAN name the same asset with different dictionaries (a DLC re-texturing a base prop is the
	// common case), so the tie is broken lexicographically rather than by which ytyp the file
	// enumerator happened to reach first - the same determinism law the sorted candidate list in
	// ImportDrawableNode exists for. A machine-dependent scope would be worse than no scope: it
	// would make a material screenshot non-reproducible again.
	auto NoteTxd = [&Out](const FString& Key, const FXmlNode* Item)
	{
		const FXmlNode* TxdN = Item->FindChildNode(TEXT("textureDictionary"));
		if (!TxdN) { return; }
		const FString Txd = TxdN->GetContent().TrimStartAndEnd().ToLower();
		if (Txd.IsEmpty()) { return; }
		if (FString* Existing = Out.AssetTxd.Find(Key))
		{
			if (Txd < *Existing) { *Existing = Txd; }
			return;
		}
		Out.AssetTxd.Add(Key, Txd);
	};
	// ⭐ 2026-08-05 (#43 tier 4): the dictionaries declared inside ONE ytyp file are a real
	// neighbourhood - those archetypes were authored and shipped together. Recorded per file so
	// FindTexture can prefer a sibling's dictionary over an unrelated DLC's. FindFiles answers in
	// filesystem order, which is not a contract, so the list is SORTED: "the first file wins"
	// then means "lexicographically first", the same determinism rule AssetTxd's tie-break exists
	// for. A machine-dependent scope would make a material screenshot non-reproducible again.
	// The corpus index answers "every ytyp, every slot" in load order (base first); later rows
	// overwrite earlier ones below, which yields the game's own effective archetype table.
	{
		FString CorpusErr;
		Out.Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
		if (!Out.Corpus.IsValid()) { Error = CorpusErr; return false; }
	}
	TArray<FString> YtypFiles;
	{
		TArray<const FRudeCorpusEntry*> Rows;
		Out.Corpus->AllOfType(TEXT("ytyp"), Rows);
		for (const FRudeCorpusEntry* E : Rows) { YtypFiles.Add(Out.Corpus->PathOf(*E)); }
	}
	TArray<FString> PendingAssets;   // reused per file
	TSet<FString> PendingTxds;
	for (const FString& F : YtypFiles)
	{
		FXmlFile Xml(F);
		if (!Xml.IsValid()) { continue; }
		const FXmlNode* Root = Xml.GetRootNode();
		const FXmlNode* Arche = Root ? Root->FindChildNode(TEXT("archetypes")) : nullptr;
		if (!Arche) { continue; }
		PendingAssets.Reset();
		PendingTxds.Reset();
		for (const FXmlNode* Item : Arche->GetChildrenNodes())
		{
			// MLO lookup first: MLO archetypes are ASSET_TYPE_ASSETLESS, so the index
			// gate below skips them and their name must be read here.
			if (MloSearch && Item->GetAttribute(TEXT("type")) == TEXT("CMloArchetypeDef"))
			{
				const FXmlNode* MloN = Item->FindChildNode(TEXT("name"));
				const FString MloName = MloN ? MloN->GetContent().TrimStartAndEnd() : FString();
				if (!MloName.IsEmpty())
				{
					++MloSearch->MloSeen;
					if (MloSearch->Sample.Len() < 400)
					{
						MloSearch->Sample += FString::Printf(TEXT("%s%s"),
							MloSearch->Sample.IsEmpty() ? TEXT("") : TEXT(", "), *MloName);
					}
					if (MloSearch->FoundFile.IsEmpty() &&
					    (MloName.Equals(MloSearch->Wanted, ESearchCase::IgnoreCase) ||
					     MloName.Equals(MloSearch->WantHashName, ESearchCase::IgnoreCase) ||
					     (MloSearch->bWantedIsHashName && RudeJoaat(MloName) == MloSearch->WantHash)))
					{
						MloSearch->FoundFile = F;
						MloSearch->FoundName = MloName;
					}
				}
			}
			const FXmlNode* NameN = Item->FindChildNode(TEXT("name"));
			const FXmlNode* AssetN = Item->FindChildNode(TEXT("assetName"));
			const FXmlNode* TypeN = Item->FindChildNode(TEXT("assetType"));
			if (!NameN || !AssetN) { continue; }
			// drawable + fragment + drawable-dictionary archetypes resolve (fragments via
			// QUARRY's yft.xml, visual drawable v1; dictionary archetypes via the ydd
			// entry-selection lane). A dictionary archetype's mesh is ONE entry inside
			// <drawableDictionary>'s ydd, and the ARCHETYPE name names that entry
			// (measured corpus-wide 2026-07-28: 72,074/72,074 dict archetypes carry a
			// plain dict name and name==assetName; the join to the entry is hash-to-hash).
			const FString AType = TypeN ? TypeN->GetContent().TrimStartAndEnd() : FString();
			const bool bDrawableArch = AType.IsEmpty() || AType == TEXT("ASSET_TYPE_DRAWABLE");
			const bool bFragmentArch = AType == TEXT("ASSET_TYPE_FRAGMENT");
			const bool bDictArch = AType == TEXT("ASSET_TYPE_DRAWABLEDICTIONARY");
			if (!bDrawableArch && !bFragmentArch && !bDictArch) { continue; }
			const FString ArchLower = NameN->GetContent().TrimStartAndEnd().ToLower();
			// ⭐ CAPTURE timeFlags. Only CTimeArchetypeDef carries it - a 24-bit hour mask where
			// bit N means "visible during hour N". This is the dataset that makes lit windows
			// appear after dusk (the common masks are hours 0-5 + 20-23), and losing it here is
			// what forced an earlier attempt to fake the behaviour in a shader.
			if (const FXmlNode* TimeN = Item->FindChildNode(TEXT("timeFlags")))
			{
				const uint32 Mask = (uint32)FCString::Strtoui64(
					*TimeN->GetAttribute(TEXT("value")), nullptr, 10);
				if (Mask != 0) { Out.ArchTimeFlags.Add(ArchLower, Mask); }
			}
			if (const FXmlNode* RadN = Item->FindChildNode(TEXT("bsRadius")))
			{
				Out.ArchRadius.Add(ArchLower, (float)FCString::Atod(*RadN->GetAttribute(TEXT("value"))));
			}
			if (bDictArch)
			{
				const FXmlNode* DictN = Item->FindChildNode(TEXT("drawableDictionary"));
				const FString Dict = DictN ? DictN->GetContent().TrimStartAndEnd().ToLower() : FString();
				if (Dict.IsEmpty()) { continue; }   // nothing to resolve against -> proxy cube
				// the manifest "drawable" stays the ENTRY (=archetype) name, so ImportScene's
				// name-based mesh lookup works unchanged
				Out.ArchToAsset.Add(ArchLower, ArchLower);
				Out.DictEntries.Add(ArchLower, Dict);
				NoteTxd(ArchLower, Item);   // dictionary lane keys on the ENTRY name
				PendingAssets.Add(ArchLower);
				if (const FXmlNode* TxdN2 = Item->FindChildNode(TEXT("textureDictionary")))
				{
					const FString T2 = TxdN2->GetContent().TrimStartAndEnd().ToLower();
					if (!T2.IsEmpty()) { PendingTxds.Add(T2); }
				}
				continue;
			}
			const FString AssetLower = AssetN->GetContent().TrimStartAndEnd().ToLower();
			Out.ArchToAsset.Add(ArchLower, AssetLower);
			NoteTxd(AssetLower, Item);
			PendingAssets.Add(AssetLower);
			if (const FXmlNode* TxdN2 = Item->FindChildNode(TEXT("textureDictionary")))
			{
				const FString T2 = TxdN2->GetContent().TrimStartAndEnd().ToLower();
				if (!T2.IsEmpty()) { PendingTxds.Add(T2); }
			}
			if (bFragmentArch) { Out.FragmentAssets.Add(AssetLower); }
		}
		// One neighbourhood per ytyp FILE. Empty sets are not stored: a file that declares no
		// dictionary can only produce an empty scope, and an empty scope must never be mistaken
		// for a scope that was consulted and missed.
		if (PendingTxds.Num() > 0 && PendingAssets.Num() > 0)
		{
			TArray<FString> Sorted = PendingTxds.Array();
			Sorted.Sort();
			const int32 SetIdx = Out.YtypTxdSets.Add(MoveTemp(Sorted));
			for (const FString& A : PendingAssets)
			{
				if (!Out.AssetYtyp.Contains(A)) { Out.AssetYtyp.Add(A, SetIdx); }
			}
		}
	}
	if (Out.ArchToAsset.Num() == 0)
	{
		Error = FString::Printf(TEXT("no archetypes indexed - the corpus at %s lists %d ytyp rows"), *CorpusRoot, YtypFiles.Num());
		return false;
	}
	// Tiers 3 and 5 come from outside the ytyp walk and are loaded here so every lane that scopes
	// gets all five signals from one call - two index builders would be two ways to disagree.
	RudeReadParentTxds(*Out.Corpus, Out.ParentTxd, Out.GtxdFiles, Out.GtxdRelationships, Out.GtxdRefusals);
	RudeReadCorpusSlots(*Out.Corpus, Out.DictSlot, Out.AssetSlot, Out.ResolvedEntries);
	// Every scope source reports its own SIZE, because an index that silently carried ZERO of any
	// of them would make the scoping a no-op that still looks wired - the exact shape of a gate
	// that cannot fail. These numbers are what tell the next reader the scope actually arrived,
	// and gtxdRefused is what tells them a table was met and REFUSED rather than quietly ignored.
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] archetype index: %d (fragment %d, dictionary-entry %d, with textureDictionary %d) "
		     "| gtxd parent chain: %d files, %d relationships, %d refused | ytyp neighbourhoods: %d "
		     "sets covering %d assets | slots: %d entries read, %d dictionaries, %d assets"),
		Out.ArchToAsset.Num(), Out.FragmentAssets.Num(), Out.DictEntries.Num(), Out.AssetTxd.Num(),
		Out.GtxdFiles, Out.GtxdRelationships, Out.GtxdRefusals,
		Out.YtypTxdSets.Num(), Out.AssetYtyp.Num(),
		Out.ResolvedEntries, Out.DictSlot.Num(), Out.AssetSlot.Num());
	return true;
}

// ⛔⛔ THE MAP LANE USED TO REDUCE A WHOLE UNIT VERDICT TO ONE BOOLEAN. This function called
// ImportYdr/ImportYddEntry, tested `R.Contains("\"ok\":true")`, and dropped R on the floor -
// so geometriesDropped, boundTextures, missingTextures, unsupportedByMaster, unmappedSamplers and
// every value-param field were discarded on the PRIMARY path. ImportMapArea, ImportArea and
// ImportMlo are the tools an operator and an agent actually run, and this is the ONLY path that
// serves the yft (302,847 entity placements) and ydd (165,552) lanes - there is no per-lane batch
// for them the way ImportYdrBatch serves ydr. A whole-city import that bound zero textures and
// dropped hundreds of geometries printed the same verdict as a perfect one. That is the exact
// failure ImportYdrBatch was fixed for on 2026-07-29, still live one level up.
struct FRudeImportTally
{
	int32 GeosDropped = 0, GeosNoUV = 0, TrisOutOfRange = 0, TrisDegenerate = 0;
	int32 Bound = 0, FromEmbedded = 0, Ambiguous = 0, Unsupported = 0, MissingTex = 0;
	int32 Scoped = 0, TieBroken = 0;   // #43 split: provenance vs lexicographic guess
	// #43/#21b per-tier split, 2026-08-05. A single "scoped" number can be inflated by loosening
	// what counts; five numbers that must still add up to it cannot.
	int32 AmbTotal = 0, FromArchTxd = 0, FromParentTxd = 0, FromYtyp = 0, FromSlot = 0;
	int32 ScopedAuth = 0, ScopedProv = 0;
	int32 TbEmbedded = 0, TbNoScope = 0, TbSlotAmb = 0, TbYtypAmb = 0, TbDictAbsent = 0, TbNotInScope = 0;
	int32 UnmappedSamp = 0, NoShaderDef = 0, NoMaterial = 0;
	int32 ValSeen = 0, ValBound = 0, ValUnsupported = 0, ValDeduped = 0;
	// #40 collision, 2026-08-05: the map/MLO lane must surface these too, or a whole-city import
	// would once again report the geometry it moved and stay silent about the collision it did not.
	int32 ColSeen = 0, ColPrims = 0, ColMeshes = 0, ColUnmapped = 0, ColMalformed = 0;
	int32 ColPolysDropped = 0, ColTris = 0;

	void Accumulate(const FString& R)
	{
		GeosDropped    += RudeSumField(R, TEXT("geometriesDropped"));
		GeosNoUV       += RudeSumField(R, TEXT("geometriesWithoutUV"));
		TrisOutOfRange += RudeSumField(R, TEXT("trianglesOutOfRange"));
		TrisDegenerate += RudeSumField(R, TEXT("trianglesDegenerate"));
		Bound          += RudeSumField(R, TEXT("boundTextures"));
		FromEmbedded   += RudeSumField(R, TEXT("texturesFromEmbedded"));
		Ambiguous      += RudeSumField(R, TEXT("ambiguousTextures"));
		Scoped         += RudeSumField(R, TEXT("texturesResolvedScoped"));
		TieBroken      += RudeSumField(R, TEXT("texturesTieBroken"));
		AmbTotal       += RudeSumField(R, TEXT("texturesAmbiguousTotal"));
		FromArchTxd    += RudeSumField(R, TEXT("texturesFromArchetypeTxd"));
		FromParentTxd  += RudeSumField(R, TEXT("texturesFromParentTxd"));
		FromYtyp       += RudeSumField(R, TEXT("texturesFromYtypNeighbour"));
		FromSlot       += RudeSumField(R, TEXT("texturesFromSameSlot"));
		ScopedAuth     += RudeSumField(R, TEXT("texturesScopedAuthoritative"));
		ScopedProv     += RudeSumField(R, TEXT("texturesScopedProvenance"));
		TbEmbedded     += RudeSumField(R, TEXT("tieBreakEmbeddedNotImported"));
		TbNoScope      += RudeSumField(R, TEXT("tieBreakNoScope"));
		TbSlotAmb      += RudeSumField(R, TEXT("tieBreakSlotAmbiguous"));
		TbYtypAmb      += RudeSumField(R, TEXT("tieBreakYtypAmbiguous"));
		TbDictAbsent   += RudeSumField(R, TEXT("tieBreakScopeDictAbsent"));
		TbNotInScope   += RudeSumField(R, TEXT("tieBreakNameNotInScope"));
		Unsupported    += RudeSumField(R, TEXT("unsupportedByMaster"));
		MissingTex     += RudeSumField(R, TEXT("missingTextures"));
		UnmappedSamp   += RudeSumField(R, TEXT("unmappedSamplers"));
		NoShaderDef    += RudeSumField(R, TEXT("slotsWithoutShaderDef"));
		NoMaterial     += RudeSumField(R, TEXT("slotsWithoutMaterial"));
		ValSeen        += RudeSumField(R, TEXT("valueParamsSeen"));
		ValBound       += RudeSumField(R, TEXT("valueParamsBound"));
		ValUnsupported += RudeSumField(R, TEXT("valueParamsUnsupported"));
		ValDeduped     += RudeSumField(R, TEXT("valueParamsDeduped"));
		ColSeen        += RudeSumField(R, TEXT("collisionBoundsSeen"));
		ColPrims       += RudeSumField(R, TEXT("collisionPrimitivesImported"));
		ColMeshes      += RudeSumField(R, TEXT("collisionMeshesImported"));
		ColUnmapped    += RudeSumField(R, TEXT("collisionBoundsUnmapped"));
		ColMalformed   += RudeSumField(R, TEXT("collisionBoundsMalformed"));
		ColPolysDropped+= RudeSumField(R, TEXT("collisionPolysDropped"));
		ColTris        += RudeSumField(R, TEXT("collisionTriangles"));
	}

	FString ToJson() const
	{
		return FString::Printf(
			TEXT("\"geometriesDropped\":%d,\"geometriesWithoutUV\":%d,\"trianglesOutOfRange\":%d,")
			TEXT("\"trianglesDegenerate\":%d,\"boundTextures\":%d,\"texturesFromEmbedded\":%d,")
			TEXT("\"texturesResolvedScoped\":%d,\"texturesTieBroken\":%d,")
			TEXT("\"ambiguousTextures\":%d,\"texturesAmbiguousTotal\":%d,")
			TEXT("\"texturesFromArchetypeTxd\":%d,\"texturesFromParentTxd\":%d,")
			TEXT("\"texturesFromYtypNeighbour\":%d,\"texturesFromSameSlot\":%d,")
			TEXT("\"texturesScopedAuthoritative\":%d,\"texturesScopedProvenance\":%d,")
			TEXT("\"tieBreakEmbeddedNotImported\":%d,\"tieBreakNoScope\":%d,")
			TEXT("\"tieBreakSlotAmbiguous\":%d,\"tieBreakYtypAmbiguous\":%d,")
			TEXT("\"tieBreakScopeDictAbsent\":%d,\"tieBreakNameNotInScope\":%d,")
			TEXT("\"unsupportedByMaster\":%d,\"missingTextures\":%d,")
			TEXT("\"unmappedSamplers\":%d,\"slotsWithoutShaderDef\":%d,\"slotsWithoutMaterial\":%d,")
			TEXT("\"valueParamsSeen\":%d,\"valueParamsBound\":%d,\"valueParamsUnsupported\":%d,")
			TEXT("\"valueParamsDeduped\":%d,\"collisionBoundsSeen\":%d,")
			TEXT("\"collisionPrimitivesImported\":%d,\"collisionMeshesImported\":%d,")
			TEXT("\"collisionBoundsUnmapped\":%d,\"collisionBoundsMalformed\":%d,")
			TEXT("\"collisionPolysDropped\":%d,\"collisionTriangles\":%d"),
			GeosDropped, GeosNoUV, TrisOutOfRange, TrisDegenerate, Bound, FromEmbedded,
			Scoped, TieBroken, Ambiguous, AmbTotal,
			FromArchTxd, FromParentTxd, FromYtyp, FromSlot, ScopedAuth, ScopedProv,
			TbEmbedded, TbNoScope, TbSlotAmb, TbYtypAmb, TbDictAbsent, TbNotInScope,
			Unsupported, MissingTex, UnmappedSamp, NoShaderDef, NoMaterial,
			ValSeen, ValBound, ValUnsupported, ValDeduped,
			ColSeen, ColPrims, ColMeshes, ColUnmapped, ColMalformed, ColPolysDropped, ColTris);
	}
};

// Resolve ONE indexed drawable to its corpus XML and import it (skip-if-exists) - dictionary
// entries live INSIDE their dict's ydd XML (one file, many drawables); plain drawables and
// fragments stay one-file-per-asset under ydr/ and yft/. The skip check runs on the ENTRY
// mesh name for all three lanes.
static void ImportIndexedDrawable(const FString& CorpusRoot, const FRudeArchetypeIndex& Index,
                                  const FString& Drawable, const FString& DestMeshFolder,
                                  int32& MeshOk, int32& MeshSkip, int32& MeshFail, int32& MeshMissing,
                                  FRudeImportTally& Tally, bool bForce = false)
{
	const FString* Dict = Index.DictEntries.Find(Drawable);
	const bool bFrag = !Dict && Index.FragmentAssets.Contains(Drawable);
	// Located through the ledger: the copy the game loads, whichever slot it sits in.
	const FRudeCorpusEntry* Row = Index.Corpus.IsValid()
		? Index.Corpus->Effective(Dict ? TEXT("ydd") : (bFrag ? TEXT("yft") : TEXT("ydr")), Dict ? *Dict : Drawable)
		: nullptr;
	if (!Row) { ++MeshMissing; return; }
	const FString XmlPath = Index.Corpus->PathOf(*Row);
	if (!FPaths::FileExists(XmlPath)) { ++MeshMissing; return; }
	// ⛔ WHY bForce EXISTS (2026-07-30). This skip is the ONLY gate on the fragment and dictionary
	// lanes, and those lanes are reachable ONLY through ImportArea/ImportMapArea - there is no
	// per-lane batch tool for them the way ImportYdrBatch serves ydr. So after the corpus gained
	// value params and embedded textures, `ImportYdrBatch ... FORCE` refreshed the ydr meshes while
	// every yft and ydd mesh stayed at its pre-fix vintage, and the project became a MIX of two
	// generations that no counter could distinguish. A refresh path is not optional once the corpus
	// can change underneath the project.
	if (!bForce && FPackageName::DoesPackageExist(DestMeshFolder / Drawable))
	{
		++MeshSkip;
		return;
	}
	// ✅ #43/#21b: hand the unit EVERY scoping signal the index can prove for this asset - the
	// archetype's declared <textureDictionary>, its gtxd parent chain, its ytyp neighbourhood and
	// the build slot it was won from. All five arrive together or the two lanes would disagree
	// about which dictionary a mesh belongs to. An asset the index knows nothing about produces an
	// empty scope, which is exactly the old behaviour - and the tieBreak* counters say how often
	// that happens instead of leaving it to be assumed.
	const FRudeTextureScope Scope = Index.MakeScope(Drawable);
	const FString R = Dict ? RudeImportYddEntryScoped(XmlPath, Drawable, DestMeshFolder, &Scope)
	                       : RudeImportYdrScoped(XmlPath, DestMeshFolder, &Scope);
	// Accumulate on BOTH paths: a mesh that came back ok:false because every slot fell to
	// WorldGridMaterial still reports the counters that say WHY, and throwing them away because
	// of the boolean is how this got lost the first time.
	Tally.Accumulate(R);
	if (R.Contains(TEXT("\"ok\":true"))) { ++MeshOk; }
	else
	{
		++MeshFail;
		// A failed mesh is a NAMED failure: the batch sums it, but only the log can say which
		// drawable and why (2026-09-05: 165/3,183 downtown meshes failed and nothing said why).
		UE_LOG(LogTemp, Warning, TEXT("[RUDE] mesh import FAILED '%s' (%s): %s"), *Drawable,
			Dict ? TEXT("ydd") : (bFrag ? TEXT("yft") : TEXT("ydr")), *R.Left(400));
	}
}

FString URudeToolset::ImportMapArea(const FString& CorpusRoot, const FString& YmapPrefix,
                                    const FString& DestMeshFolder, const FString& Filter,
                                    const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// ⭐ Mode="FORCE" re-imports meshes that already exist. This is the ONLY refresh path the yft
	// (fragment) and ydd (dictionary) lanes have - they are reachable solely through this tool, with
	// no per-lane batch equivalent to ImportYdrBatch. Without it, a corpus that gains data (value
	// params, embedded textures) can refresh only its ydr meshes, leaving the project a MIX of two
	// vintages that no counter can tell apart (2026-07-30).
	//
	// ⚠ THIS WAS FIRST BUILT AS A "+FORCE" TOKEN ON Filter, on the stated grounds that adding a
	// parameter would break existing RUDE.Run calls. Matt challenged that and it was WRONG:
	// FRudeInvoke::Call binds arguments with `if (Values.IsValidIndex(ValueIdx))` and performs NO
	// arity check, so a missing trailing argument simply stays an empty FString. Old 5-argument calls
	// therefore keep working with Mode empty, which means "not FORCE" - the previous behaviour.
	// A separate Mode is the right shape anyway: it matches ImportYdrBatch, and `Filter` means LOD
	// LEVELS - a mode flag riding in it makes the parameter mean two things.
	// The "+FORCE" spelling is still ACCEPTED, because silently reinterpreting it as an unknown lod
	// filter would turn a deliberate FORCE into a no-op, and a silent no-op is worse than an alias.
	FString LodFilter = Filter;
	bool bForceMeshes = Mode.TrimStartAndEnd().Equals(TEXT("FORCE"), ESearchCase::IgnoreCase);
	{
		TArray<FString> Parts;
		Filter.ParseIntoArray(Parts, TEXT("+"), true);
		LodFilter.Empty();
		for (const FString& Part : Parts)
		{
			const FString T = Part.TrimStartAndEnd();
			if (T.Equals(TEXT("FORCE"), ESearchCase::IgnoreCase)) { bForceMeshes = true; }
			else if (!T.IsEmpty()) { LodFilter = T; }
		}
		if (LodFilter.IsEmpty()) { LodFilter = TEXT("HD"); }
	}
	// ---- 1) archetype index from every ytyp XML (name -> drawable assetName) ----
	// (factored to BuildCorpusArchetypeIndex, shared with ImportMlo - behavior unchanged)
	FRudeArchetypeIndex Index;
	{
		FString IndexErr;
		if (!BuildCorpusArchetypeIndex(CorpusRoot, Index, IndexErr, /*MloSearch*/ nullptr))
		{
			return Fail(IndexErr);
		}
	}
	// ---- 2) parse ymaps -> manifest scenes (IMPORT-lane transforms: pos Y-mirror*100,
	// quat = (x,-y,z,w) - the boardwalk-anchored map, NOT the export involution) ----
	// YmapPrefix accepts a COMMA-SEPARATED list of prefixes ("dt1_,dt_additions") so a named
	// area spanning several families imports as ONE scene (ImportArea builds such lists from the
	// catalog). An exact basename rides along as "<name>.ymap" - the glob "<name>.ymap*.xml"
	// matches only that file. Duplicates are unioned.
	TArray<FString> Prefixes;
	YmapPrefix.ParseIntoArray(Prefixes, TEXT(","), true);
	TSet<FString> SeenYmap;
	TArray<FString> YmapFiles;
	TMap<FString, FString> YmapSlotByPath;   // provenance: which build slot each ymap copy came from
	for (FString P : Prefixes)
	{
		P.TrimStartAndEndInline();
		if (P.IsEmpty()) { continue; }
		// Every ymap whose NAME starts with the prefix, the effective copy of each (a patchday
		// re-issue of dt1_00 wins over the base's), as full paths.
		TArray<const FRudeCorpusEntry*> Found;
		Index.Corpus->ByPrefix(TEXT("ymap"), P, Found);
		for (const FRudeCorpusEntry* E : Found)
		{
			// TSet::Add's out-param is bIsAlreadyInSet - true for DUPLICATES, not new adds
			bool bAlready = false;
			SeenYmap.Add(E->Name, &bAlready);
			if (!bAlready)
			{
				const FString Path = Index.Corpus->PathOf(*E);
				YmapFiles.Add(Path);
				YmapSlotByPath.Add(Path, E->Slot);
			}
		}
	}
	YmapFiles.Sort();
	if (YmapFiles.Num() == 0) { return Fail(TEXT("no ymaps match the prefix")); }
	int32 TotalEnts = 0, Resolved = 0;
	// ⛔ "ymaps" REPORTED THE GLOB MATCH COUNT. YmapFiles.Num() is how many FILENAMES matched the
	// prefix, and it was emitted verbatim as the number of ymaps imported - while THREE separate
	// `continue`s could eliminate a file with no counter at all: an FXmlFile parse failure, a
	// missing <entities> node, and a file that contributed zero entities. MEASURED over 1,500
	// resolved ymap XML: 0 parse failures and 0 missing <entities> today, but 219 files (14.6%)
	// carry an EMPTY <entities> element and so produce no scene while still counting toward
	// "ymaps" - i.e. the number overstates coverage by ~15% on a typical prefix, and a wholesale
	// XML-corruption event would be invisible because the glob would still match.
	// ⚠ YmapsUnreadable is also the only place UE's FXmlFile can disagree with the offline
	// analysis: the corpus join rate was measured with lxml, and FXmlFile is a different,
	// hand-rolled tokenizer. Until this field exists and reads 0 on a full run, that rate is an
	// upper bound.
	int32 YmapsParsed = 0, YmapsUnreadable = 0, YmapsNoEntitiesNode = 0, YmapsWithEntities = 0;
	int32 EntitiesSkipped = 0;   // entity missing <archetypeName> or <position> - was a bare continue
	TSet<FString> NeededDrawables;
	FString ScenesJson;
	for (const FString& F : YmapFiles)
	{
		FXmlFile Xml(F);
		if (!Xml.IsValid()) { ++YmapsUnreadable; continue; }
		++YmapsParsed;
		const FXmlNode* Root = Xml.GetRootNode();
		const FXmlNode* Ents = Root ? Root->FindChildNode(TEXT("entities")) : nullptr;
		if (!Ents) { ++YmapsNoEntitiesNode; continue; }
		FString EntJson;
		int32 SceneEnts = 0;
		// Provenance for every entity: the ymap's asset name, its build slot, and the entity's
		// ordinal in the file's list (parentIndex values refer to ordinals, so it is identity).
		FString SrcYmapName = FPaths::GetBaseFilename(F);
		SrcYmapName.RemoveFromEnd(TEXT(".ymap"));
		const FString* SrcSlotPtr = YmapSlotByPath.Find(F);
		const FString SrcSlot = SrcSlotPtr ? *SrcSlotPtr : FString();
		int32 EntOrdinal = -1;
		for (const FXmlNode* E : Ents->GetChildrenNodes())
		{
			++EntOrdinal;
			const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
			const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
			// Counted, not silent. No index consequence on THIS lane (a ymap entity is not
			// referenced by ordinal the way an MLO's attachedObjects references one), but an
			// entity that vanishes between the file and the manifest must be sayable.
			if (!AN || !Pos) { ++EntitiesSkipped; continue; }
			const FString Arch = AN->GetContent().TrimStartAndEnd().ToLower();
			const double Px = FCString::Atod(*Pos->GetAttribute(TEXT("x")));
			const double Py = FCString::Atod(*Pos->GetAttribute(TEXT("y")));
			const double Pz = FCString::Atod(*Pos->GetAttribute(TEXT("z")));
			double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
			if (const FXmlNode* Rot = E->FindChildNode(TEXT("rotation")))
			{
				Qx = FCString::Atod(*Rot->GetAttribute(TEXT("x")));
				Qy = FCString::Atod(*Rot->GetAttribute(TEXT("y")));
				Qz = FCString::Atod(*Rot->GetAttribute(TEXT("z")));
				Qw = FCString::Atod(*Rot->GetAttribute(TEXT("w")));
			}
			auto Val = [&](const TCHAR* Tag, double Def) -> double
			{
				const FXmlNode* N = E->FindChildNode(Tag);
				return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Def;
			};
			const FXmlNode* LodN = E->FindChildNode(TEXT("lodLevel"));
			const FString Lod = LodN ? LodN->GetContent().TrimStartAndEnd() : FString();
			const FString* Asset = Index.ArchToAsset.Find(Arch);
			++TotalEnts; ++SceneEnts;
			if (Asset) { ++Resolved; NeededDrawables.Add(*Asset); }
			// ⭐ timeFlags travels WITH the entity. It belongs to the archetype, but the spawn works
			// per entity, and carrying it here means the hour mask survives into the manifest that
			// ImportScene re-spawns from - so a respawn keeps the day/night behaviour without
			// re-reading every ytyp. 0 = no mask = always visible.
			const uint32* TFlags = Index.ArchTimeFlags.Find(Arch);
			// Every CEntityDef field rides in the manifest (the ymap spec's 17: 11 T1 + 6 T2), so the
			// per-entity actor's component can be filled without re-reading the ymap, and so the
			// manifest is a faithful record of what the file said. Text fields as spelled; the
			// <extensions> subtree verbatim.
			auto Text = [&](const TCHAR* Tag) -> FString
			{
				const FXmlNode* N = E->FindChildNode(Tag);
				return N ? N->GetContent().TrimStartAndEnd() : FString();
			};
			FString ExtXml;
			if (const FXmlNode* Ext = E->FindChildNode(TEXT("extensions")))
			{
				if (Ext->GetChildrenNodes().Num() > 0) { RudeXmlNodeToString(Ext, ExtXml, 0); }
			}
			FString ItemXml;   // the whole <Item type="CEntityDef"> as spelled - the byte-safe seam
			RudeXmlNodeToString(E, ItemXml, 2);
			EntJson += FString::Printf(TEXT(
				"%s{\"archetype\":\"%s\",\"drawable\":%s,\"lodLevel\":\"%s\","
				"\"ue_location\":[%f,%f,%f],\"ue_quat\":[%f,%f,%f,%f],\"scaleXY\":%f,"
				"\"scaleZ\":%f,\"timeFlags\":%u,"
				"\"flags\":%.0f,\"guid\":%.0f,\"lodDist\":%f,\"childLodDist\":%f,"
				"\"numChildren\":%.0f,\"parentIndex\":%.0f,\"priorityLevel\":\"%s\","
				"\"aoMultiplier\":%f,\"artificialAo\":%f,\"tintValue\":%.0f,"
				"\"extensions\":\"%s\",\"srcYmap\":\"%s\",\"srcSlot\":\"%s\",\"srcIndex\":%d,"
				"\"xml\":\"%s\",\"itemType\":\"%s\",\"bsRadius\":%g}"),
				SceneEnts > 1 ? TEXT(",") : TEXT(""), *Arch,
				Asset ? *FString::Printf(TEXT("\"%s\""), **Asset) : TEXT("null"), *Lod,
				Px * 100.0, -Py * 100.0, Pz * 100.0,
				Qx, -Qy, Qz, Qw,
				Val(TEXT("scaleXY"), 1.0), Val(TEXT("scaleZ"), 1.0),
				TFlags ? *TFlags : 0u,
				Val(TEXT("flags"), 0.0), Val(TEXT("guid"), 0.0), Val(TEXT("lodDist"), 0.0), Val(TEXT("childLodDist"), 0.0),
				Val(TEXT("numChildren"), 0.0), Val(TEXT("parentIndex"), -1.0), *RudeJsonEscape(Text(TEXT("priorityLevel"))),
				Val(TEXT("ambientOcclusionMultiplier"), 255.0), Val(TEXT("artificialAmbientOcclusion"), 255.0), Val(TEXT("tintValue"), 0.0),
				*RudeJsonEscape(ExtXml), *RudeJsonEscape(SrcYmapName), *RudeJsonEscape(SrcSlot), EntOrdinal,
				*RudeJsonEscape(ItemXml), *RudeJsonEscape(E->GetAttribute(TEXT("type"))),
				Index.ArchRadius.Contains(Arch) ? Index.ArchRadius[Arch] : 0.f);
		}
		if (SceneEnts == 0) { continue; }
		++YmapsWithEntities;
		FString YmapName = FPaths::GetBaseFilename(F);
		YmapName.RemoveFromEnd(TEXT(".ymap"));
		// CMapData/flags: bit 0 = script-controlled (mission/variant content the game loads on
		// demand), bit 1 = LOD container - measured over downtown's 158 ymaps 2026-09-05.
		uint32 YmapFlags = 0;
		if (const FXmlNode* FN = Root->FindChildNode(TEXT("flags"))) { YmapFlags = (uint32)FCString::Strtoui64(*FN->GetAttribute(TEXT("value")), nullptr, 10); }
		ScenesJson += FString::Printf(TEXT("%s{\"ymap\":\"%s\",\"ymapFlags\":%u,\"entities\":[%s]}"),
			ScenesJson.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, YmapFlags, *EntJson);
	}
	const FString ManifestPath = FPaths::ProjectSavedDir() / TEXT("RUDE") /
		FString::Printf(TEXT("area_%s_manifest.json"),
			*YmapPrefix.Replace(TEXT("*"), TEXT("")).Replace(TEXT(","), TEXT("+")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);
	if (!FFileHelper::SaveStringToFile(TEXT("[") + ScenesJson + TEXT("]"), *ManifestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write area manifest"));
	}
	// ---- 3) import every referenced drawable present in the corpus (skip-if-exists) ----
	// (per-drawable lane selection factored to ImportIndexedDrawable, shared with ImportMlo)
	int32 MeshOk = 0, MeshSkip = 0, MeshFail = 0, MeshMissing = 0, Done = 0, DictNeeded = 0;
	FRudeImportTally Tally;
	for (const FString& D : NeededDrawables)
	{
		++Done;
		if (Index.DictEntries.Contains(D)) { ++DictNeeded; }
		ImportIndexedDrawable(CorpusRoot, Index, D, DestMeshFolder, MeshOk, MeshSkip, MeshFail,
		                      MeshMissing, Tally, bForceMeshes);
		if (Done % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportMapArea meshes %d/%d (ok %d, skip %d, fail %d)"),
				Done, NeededDrawables.Num(), MeshOk, MeshSkip, MeshFail);
			// KEEPFLAGS (= RF_Standalone in editor), NEVER RF_NoFlags: the meshes just imported are
			// unsaved and unreferenced until the spawn phase, so a no-keep GC deletes them. RF_NoFlags
			// here swept 1,600 of 1,667 downtown meshes; only imports after the last GC survived.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportMapArea dictionary-entry drawables: %d of %d needed (ok %d, skip %d, fail %d, missing %d overall)"),
		DictNeeded, NeededDrawables.Num(), MeshOk, MeshSkip, MeshFail, MeshMissing);
	// ---- 4) spawn through the proven ImportScene path ----
	// The spawn only understands lod levels - never hand it the FORCE token.
	// ACTORS in Mode = one actor per entity carrying its URudeEntityComponent (the editable
	// scene); otherwise the ISM display path. FORCE never reaches the spawn.
	const bool bSpawnActors = Mode.Contains(TEXT("ACTORS"), ESearchCase::IgnoreCase);
	const FString Spawn = ImportScene(ManifestPath, DestMeshFolder, LodFilter, bSpawnActors ? TEXT("ACTORS") : TEXT(""));
	// ⛔⛔ ok WAS A LITERAL, AND THE SPAWN'S OWN VERDICT WAS NESTED UNDERNEATH IT. This function
	// calls ImportScene and then opens its format string with a hardcoded "ok":true - so
	// "no editor world", the failure that produces an import with nothing placed in it, was
	// reported as a success by the only field the CLI exit code, the RUDE.Run console path and the
	// Slate status line actually read (all three go through FRudeInvoke::ReportedFailure, a
	// substring search for "ok":false). Worse, that substring search scans the WHOLE string, so a
	// failed nested spawn turned the PANEL red while the JSON an agent parses still said
	// ok:true - the two observers disagreed about the same run.
	// THE GATE, EXACTLY: forward the spawn's ok; fail if NO ymap contributed a scene; fail if ANY
	// ymap failed to parse. The last one is deliberately zero-tolerance and it is the cheap answer
	// to an open question - the 99.4% corpus join rate was measured with lxml, while UE's FXmlFile
	// is a different hand-rolled tokenizer whose failures were dropped by an uncounted `continue`.
	// MEASURED: 0 of 1,500 resolved ymaps fail to parse offline, so a non-zero ymapsUnreadable
	// means the two parsers disagree, and that is exactly what should stop a run.
	// meshesFailed and meshesMissingFromCorpus stay NUMBERS - a city always has some of both, and
	// a gate that fires on every run is a gate nobody reads.
	const bool bSpawnOk = !Spawn.Contains(TEXT("\"ok\":false"));
	const bool bAreaOk = bSpawnOk && (YmapsWithEntities > 0) && (YmapsUnreadable == 0);
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"ymapsMatched\":%d,\"ymapsParsed\":%d,\"ymapsUnreadable\":%d,"
		"\"ymapsWithoutEntitiesNode\":%d,\"ymapsWithEntities\":%d,\"ymaps\":%d,"
		"\"entities\":%d,\"entitiesSkipped\":%d,\"resolved\":%d,\"meshesImported\":%d,"
		"\"meshesSkipped\":%d,\"meshesFailed\":%d,\"meshesMissingFromCorpus\":%d,%s,"
		"\"manifest\":\"%s\",\"spawn\":%s}"),
		bAreaOk ? TEXT("true") : TEXT("false"),
		YmapFiles.Num(), YmapsParsed, YmapsUnreadable, YmapsNoEntitiesNode, YmapsWithEntities,
		// "ymaps" is KEPT and now means what its name says - ymaps that contributed a scene.
		// Renaming it outright would silently change every existing caller's reading; the four
		// new fields say where the difference went.
		YmapsWithEntities,
		TotalEnts, EntitiesSkipped, Resolved, MeshOk, MeshSkip, MeshFail, MeshMissing,
		*Tally.ToJson(), *ManifestPath, *Spawn);
}

FString URudeToolset::ImportArea(const FString& AreaName, const FString& CatalogPath,
                                 const FString& CorpusRoot, const FString& DestMeshFolder,
                                 const FString& Filter, const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *CatalogPath))
	{
		return Fail(TEXT("cannot read the area catalog (CatalogPath)"));
	}
	TArray<TSharedPtr<FJsonValue>> Entries;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Entries))
		{
			return Fail(TEXT("area catalog is not a JSON array"));
		}
	}
	// Underscores count as spaces so multi-word names survive console arg splitting
	// (RUDE.Run ImportArea Downtown_Los_Santos ...), and a unique case-insensitive substring
	// is accepted ("downtown") - a human should not have to type a catalog string exactly.
	const FString Want = AreaName.TrimStartAndEnd().Replace(TEXT("_"), TEXT(" "));
	FString Aliases;
	const TSharedPtr<FJsonObject>* Match = nullptr;
	FString MatchAlias;
	int32 Substrings = 0;
	for (const TSharedPtr<FJsonValue>& V : Entries)
	{
		const TSharedPtr<FJsonObject>* E;
		if (!V.IsValid() || !V->TryGetObject(E)) { continue; }
		const FString Alias = (*E)->GetStringField(TEXT("alias"));
		if (Want.IsEmpty())
		{
			// No name given: answer with the menu instead of an error - the panel user's
			// discovery path ("what can I type here?").
			Aliases += FString::Printf(TEXT("%s\"%s\""), Aliases.IsEmpty() ? TEXT("") : TEXT(","), *Alias);
			continue;
		}
		if (Alias.Equals(Want, ESearchCase::IgnoreCase))
		{
			Match = E;
			MatchAlias = Alias;
			Substrings = 1;
			break;
		}
		if (Alias.Contains(Want, ESearchCase::IgnoreCase))
		{
			Match = E;
			MatchAlias = Alias;
			++Substrings;
		}
	}
	if (Match && Substrings == 1)
	{
		const TSharedPtr<FJsonObject>* E = Match;
		TArray<FString> Parts;
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if ((*E)->TryGetArrayField(TEXT("prefixes"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& P : *Arr) { Parts.Add(P->AsString()); }
		}
		// exact basenames ride as "<name>.ymap" prefixes - "<name>.ymap*.xml" matches only
		// that file (see ImportMapArea's comma-list note)
		if ((*E)->TryGetArrayField(TEXT("exact"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& P : *Arr) { Parts.Add(P->AsString() + TEXT(".ymap")); }
		}
		if (Parts.Num() == 0)
		{
			return Fail(TEXT("catalog entry has no prefixes"));
		}
		return ImportMapArea(CorpusRoot, FString::Join(Parts, TEXT(",")), DestMeshFolder, Filter, Mode);
	}
	if (Want.IsEmpty())
	{
		return FString::Printf(TEXT("{\"ok\":true,\"areas\":[%s]}"), *Aliases);
	}
	if (Substrings > 1)
	{
		return Fail(FString::Printf(TEXT("'%s' matches %d areas - be more specific (empty AreaName lists them)"),
			*Want, Substrings));
	}
	return Fail(FString::Printf(TEXT("unknown area '%s' - run with an empty AreaName to list them"), *Want));
}

// One tunable, deliberately named: no measured RAGE->UE photometric law exists (LOG
// "EXTENSIONS DECODED" carries the LIGHT FIELDS, not their units). RAGE MLO intensities
// cluster ~1-20; read directly as candela those are invisible, so v1 scales them into a
// plausible domestic range (a "5" bulb -> 500 cd). 🧠 AGENT CALL, UNCALIBRATED - Matt's
// eyes tune this one constant; nothing else in the importer encodes brightness.
static const float RudeMloLightCandelaScale = 100.f;

FString URudeToolset::ImportMlo(const FString& CorpusRoot, const FString& MloArchetypeName,
                                const FString& DestMeshFolder, const FString& Filter)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	const FString Wanted = MloArchetypeName.TrimStartAndEnd();
	if (Wanted.IsEmpty()) { return Fail(TEXT("MloArchetypeName is empty")); }
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }

	// ---- 1) ONE corpus walk: archetype index (resolves the interior's entities) + MLO
	// lookup riding it (hash-tolerant both ways - see FRudeMloSearch) ----
	FRudeMloSearch Search;
	Search.Wanted = Wanted;
	Search.WantHashName = FString::Printf(TEXT("hash_%08X"), RudeJoaat(Wanted));
	if (Wanted.Len() == 13 && Wanted.StartsWith(TEXT("hash_"), ESearchCase::IgnoreCase))
	{
		Search.bWantedIsHashName = true;
		Search.WantHash = static_cast<uint32>(FCString::Strtoui64(*Wanted.Mid(5), nullptr, 16));
	}
	FRudeArchetypeIndex Index;
	{
		FString IndexErr;
		if (!BuildCorpusArchetypeIndex(CorpusRoot, Index, IndexErr, &Search))
		{
			return Fail(IndexErr);
		}
	}
	if (Search.FoundFile.IsEmpty())
	{
		return Fail(FString::Printf(
			TEXT("MLO archetype '%s' not found (also tried %s) among %d MLO archetypes under %s. Leading names: %s"),
			*Wanted, *Search.WantHashName, Search.MloSeen, *CorpusRoot, *Search.Sample));
	}

	// ---- 2) parse the declaring ytyp's MLO node ----
	FXmlFile Xml(Search.FoundFile);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	const FXmlNode* Arche = Root ? Root->FindChildNode(TEXT("archetypes")) : nullptr;
	const FXmlNode* Mlo = nullptr;
	if (Arche)
	{
		for (const FXmlNode* Item : Arche->GetChildrenNodes())
		{
			if (Item->GetAttribute(TEXT("type")) != TEXT("CMloArchetypeDef")) { continue; }
			const FXmlNode* N = Item->FindChildNode(TEXT("name"));
			if (N && N->GetContent().TrimStartAndEnd().Equals(Search.FoundName, ESearchCase::IgnoreCase))
			{
				Mlo = Item;
				break;
			}
		}
	}
	if (!Mlo)
	{
		return Fail(FString::Printf(TEXT("re-parse lost archetype '%s' in %s - file changed mid-run?"),
			*Search.FoundName, *FPaths::GetCleanFilename(Search.FoundFile)));
	}

	// "x y z" space-separated element CONTENT (posn/direction/extents/attachedObjects use the
	// scalar-list rendering: <=10 values inline, 11+ wrapped ten-per-line - so ALWAYS
	// whitespace-parse the whole content, never split on newlines; the FXmlFile line-structure
	// lesson from the vertex parser applies here too).
	auto Vec3Content = [](const FXmlNode* N, FVector& Out) -> bool
	{
		if (!N) { return false; }
		TArray<FString> T;
		N->GetContent().ParseIntoArrayWS(T);
		if (T.Num() < 3) { return false; }
		Out = FVector(FCString::Atod(*T[0]), FCString::Atod(*T[1]), FCString::Atod(*T[2]));
		return true;
	};
	auto Val = [](const FXmlNode* P, const TCHAR* Tag, double Def) -> double
	{
		const FXmlNode* N = P->FindChildNode(Tag);
		return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Def;
	};

	// CLightAttrDef fields the importer consumes; everything else the emitter carries
	// (flags/timeFlags/corona*/vol*/shadow*/cullingPlane/projectedTextureKey/tangent/
	// falloffExponent) is deliberately NOT mapped - see the header comment for why each.
	struct FMloLight
	{
		FVector LocalPos = FVector::ZeroVector;   // UE cm, entity-local (posn + offsetPosition)
		FVector LocalDir = FVector(0, 0, -1);     // UE, entity-local
		FLinearColor Color = FLinearColor::White;
		float Intensity = 0.f;
		float Falloff = 0.f;                      // GTA metres
		float ConeInner = 0.f, ConeOuter = 0.f;   // degrees (half-angles)
		int32 Type = -1;                          // 1 point / 2 spot / 4 capsule (observed set)
		float ExtentX = 0.f;                      // capsule length (metres)
	};
	struct FMloEntity
	{
		FString ArchLower;
		FTransform Xf;          // MLO-LOCAL, UE space (import-lane transform)
		int32 Room = -1;        // index into Rooms (from rooms' attachedObjects)
		int32 Portal = -1;      // index into Portals (doors attach to portals, not rooms)
		TArray<FMloLight> Lights;
		// ⛔⛔ A MALFORMED ENTITY KEEPS ITS SLOT. Rooms and portals reference their contents by
		// ORDINAL - "0-based into <entities>" is this file's own stated contract - so dropping an
		// entity from the array re-bases every attachedObjects index after it and silently attaches
		// the wrong props to the wrong rooms. badAttachedRefs cannot catch it: after a drop the
		// shifted indices are all still IN RANGE, so every check passes and the interior is quietly
		// wrong. RudeScenario.cpp:139-157 already solves exactly this for points and nodes with the
		// same reasoning ("Compacting the array renumbers everything after it - a corruption that no
		// in-range check can catch"); the pattern existed and was simply not applied here.
		// MEASURED over 105 MLO archetypes / 11,451 entities: 0 occurrences today. Latent, and
		// latent is the point - the trigger is a hand-edited, truncated or re-emitted ytyp.
		bool bValid = true;
	};
	TArray<FMloEntity> Ents;
	int32 OtherExtensions = 0, LightsSkipped = 0, EntitiesMissingTransform = 0;
	FString LightProblem;

	if (const FXmlNode* EntsN = Mlo->FindChildNode(TEXT("entities")))
	{
		for (const FXmlNode* E : EntsN->GetChildrenNodes())
		{
			const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
			const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
			if (!AN || !Pos)
			{
				// Slot preserved, entity marked dead - see FMloEntity::bValid.
				++EntitiesMissingTransform;
				FMloEntity Dead;
				Dead.bValid = false;
				Ents.Add(MoveTemp(Dead));
				continue;
			}
			FMloEntity Ent;
			Ent.ArchLower = AN->GetContent().TrimStartAndEnd().ToLower();
			// MLO-LOCAL transform through the pinned IMPORT-lane convention (pos Y-mirror*100,
			// quat = (x,-y,z,w)) - identical to ImportMapArea's manifest transforms, so a later
			// Build Interior can place the whole root at a CMloInstanceDef world transform
			// without touching the entities.
			const double Px = FCString::Atod(*Pos->GetAttribute(TEXT("x")));
			const double Py = FCString::Atod(*Pos->GetAttribute(TEXT("y")));
			const double Pz = FCString::Atod(*Pos->GetAttribute(TEXT("z")));
			double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
			if (const FXmlNode* Rot = E->FindChildNode(TEXT("rotation")))
			{
				Qx = FCString::Atod(*Rot->GetAttribute(TEXT("x")));
				Qy = FCString::Atod(*Rot->GetAttribute(TEXT("y")));
				Qz = FCString::Atod(*Rot->GetAttribute(TEXT("z")));
				Qw = FCString::Atod(*Rot->GetAttribute(TEXT("w")));
			}
			FQuat Q(Qx, -Qy, Qz, Qw);
			Q.Normalize();
			Ent.Xf = FTransform(Q, FVector(Px * 100.0, -Py * 100.0, Pz * 100.0),
				FVector(Val(E, TEXT("scaleXY"), 1.0), Val(E, TEXT("scaleXY"), 1.0), Val(E, TEXT("scaleZ"), 1.0)));

			// per-entity extensions: consume CExtensionDefLightEffect instances; COUNT the
			// rest (doors/spawn points/particles...) so the verdict says what v1 left behind.
			if (const FXmlNode* Ext = E->FindChildNode(TEXT("extensions")))
			{
				for (const FXmlNode* X : Ext->GetChildrenNodes())
				{
					if (X->GetAttribute(TEXT("type")) != TEXT("CExtensionDefLightEffect"))
					{
						++OtherExtensions;
						continue;
					}
					FVector Off(0, 0, 0);
					if (const FXmlNode* O = X->FindChildNode(TEXT("offsetPosition")))
					{
						Off = FVector(FCString::Atod(*O->GetAttribute(TEXT("x"))),
						              FCString::Atod(*O->GetAttribute(TEXT("y"))),
						              FCString::Atod(*O->GetAttribute(TEXT("z"))));
					}
					const FXmlNode* Inst = X->FindChildNode(TEXT("instances"));
					if (!Inst) { continue; }
					for (const FXmlNode* L : Inst->GetChildrenNodes())
					{
						FVector P, D;
						TArray<FString> ColT;
						if (const FXmlNode* C = L->FindChildNode(TEXT("colour")))
						{
							C->GetContent().ParseIntoArrayWS(ColT);
						}
						if (!Vec3Content(L->FindChildNode(TEXT("posn")), P) ||
						    !Vec3Content(L->FindChildNode(TEXT("direction")), D) || ColT.Num() < 3)
						{
							// The emitter has NO silent defaults (LOG "MLO EMISSION"), so a
							// missing field here means corrupt input - count it, name the first.
							++LightsSkipped;
							if (LightProblem.IsEmpty())
							{
								LightProblem = FString::Printf(
									TEXT("entity %d: light instance missing posn/colour/direction"), Ents.Num());
							}
							continue;
						}
						FMloLight ML;
						ML.LocalPos = FVector((P.X + Off.X) * 100.0, -(P.Y + Off.Y) * 100.0, (P.Z + Off.Z) * 100.0);
						ML.LocalDir = FVector(D.X, -D.Y, D.Z);   // same Y-mirror as every import-lane vector
						ML.Color = FLinearColor(
							FCString::Atof(*ColT[0]) / 255.f,
							FCString::Atof(*ColT[1]) / 255.f,
							FCString::Atof(*ColT[2]) / 255.f);
						ML.Intensity = (float)Val(L, TEXT("intensity"), 0.0);
						ML.Falloff = (float)Val(L, TEXT("falloff"), 0.0);
						ML.ConeInner = (float)Val(L, TEXT("coneInnerAngle"), 0.0);
						ML.ConeOuter = (float)Val(L, TEXT("coneOuterAngle"), 0.0);
						ML.Type = (int32)Val(L, TEXT("lightType"), -1.0);
						FVector Ex(0, 0, 0);
						Vec3Content(L->FindChildNode(TEXT("extents")), Ex);
						ML.ExtentX = (float)Ex.X;
						Ent.Lights.Add(ML);
					}
				}
			}
			Ents.Add(MoveTemp(Ent));
		}
	}

	// rooms + portals: membership comes from their attachedObjects index lists (0-based into
	// <entities>; oracle-proven in-range corpus-wide, so an out-of-range index is corrupt
	// input - counted loudly, never clamped).
	struct FMloRoom { FString Name; FString NameLower; };
	struct FMloPortal { int32 From = -1; int32 To = -1; };
	TArray<FMloRoom> Rooms;
	TArray<FMloPortal> Portals;
	int32 BadRefs = 0;
	if (const FXmlNode* RoomsN = Mlo->FindChildNode(TEXT("rooms")))
	{
		for (const FXmlNode* R : RoomsN->GetChildrenNodes())
		{
			FMloRoom Room;
			if (const FXmlNode* N = R->FindChildNode(TEXT("name")))
			{
				Room.Name = N->GetContent().TrimStartAndEnd();
			}
			Room.NameLower = Room.Name.ToLower();
			const int32 RoomIdx = Rooms.Add(Room);
			if (const FXmlNode* AO = R->FindChildNode(TEXT("attachedObjects")))
			{
				TArray<FString> T;
				AO->GetContent().ParseIntoArrayWS(T);
				for (const FString& S : T)
				{
					const int32 EIdx = FCString::Atoi(*S);
					if (Ents.IsValidIndex(EIdx)) { Ents[EIdx].Room = RoomIdx; }
					else { ++BadRefs; }
				}
			}
		}
	}
	if (const FXmlNode* PortalsN = Mlo->FindChildNode(TEXT("portals")))
	{
		for (const FXmlNode* P : PortalsN->GetChildrenNodes())
		{
			FMloPortal Portal;
			Portal.From = (int32)Val(P, TEXT("roomFrom"), -1.0);
			Portal.To = (int32)Val(P, TEXT("roomTo"), -1.0);
			const int32 PortalIdx = Portals.Add(Portal);
			if (const FXmlNode* AO = P->FindChildNode(TEXT("attachedObjects")))
			{
				TArray<FString> T;
				AO->GetContent().ParseIntoArrayWS(T);
				for (const FString& S : T)
				{
					const int32 EIdx = FCString::Atoi(*S);
					if (Ents.IsValidIndex(EIdx)) { Ents[EIdx].Portal = PortalIdx; }
					else { ++BadRefs; }
				}
			}
		}
	}
	// entity sets: SUMMARIZED, not spawned (v1) - they are optional overlays the game toggles
	// at runtime (LOG "MLO INTERIORS"), so spawning them all would misrepresent the interior.
	FString SetsJson;
	int32 NumSets = 0;
	if (const FXmlNode* Sets = Mlo->FindChildNode(TEXT("entitySets")))
	{
		for (const FXmlNode* S : Sets->GetChildrenNodes())
		{
			const FXmlNode* SN = S->FindChildNode(TEXT("name"));
			const FXmlNode* SE = S->FindChildNode(TEXT("entities"));
			SetsJson += FString::Printf(TEXT("%s{\"name\":\"%s\",\"entities\":%d}"),
				NumSets ? TEXT(",") : TEXT(""),
				SN ? *SN->GetContent().TrimStartAndEnd() : TEXT(""),
				SE ? SE->GetChildrenNodes().Num() : 0);
			++NumSets;
		}
	}

	// ---- Filter = ROOM-name list (🧠 agent's design; see header comment) ----
	TSet<FString> RoomFilter;
	{
		const FString F = Filter.TrimStartAndEnd();
		if (!F.IsEmpty() && !F.Equals(TEXT("ALL"), ESearchCase::IgnoreCase))
		{
			TArray<FString> Toks;
			F.ParseIntoArray(Toks, TEXT(","), true);
			for (FString T : Toks)
			{
				T.TrimStartAndEndInline();
				if (!T.IsEmpty()) { RoomFilter.Add(T.ToLower()); }
			}
		}
	}
	FString RoomNamesJson, RoomNamesPlain;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		RoomNamesJson += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *Rooms[i].Name);
		RoomNamesPlain += FString::Printf(TEXT("%s%s"), i ? TEXT(", ") : TEXT(""), *Rooms[i].Name);
	}
	for (const FString& Tok : RoomFilter)
	{
		bool bKnown = false;
		for (const FMloRoom& R : Rooms)
		{
			if (R.NameLower == Tok) { bKnown = true; break; }
		}
		if (!bKnown)
		{
			return Fail(FString::Printf(TEXT("Filter room '%s' is not a room of %s - rooms here: %s"),
				*Tok, *Search.FoundName, *RoomNamesPlain));
		}
	}
	auto RoomPasses = [&](int32 RoomIdx) -> bool
	{
		return RoomFilter.IsEmpty() ||
			(Rooms.IsValidIndex(RoomIdx) && RoomFilter.Contains(Rooms[RoomIdx].NameLower));
	};
	auto Passes = [&](const FMloEntity& E) -> bool
	{
		// A slot-preserving placeholder is not a thing to import or spawn - it exists only so the
		// entities after it keep their ordinals.
		if (!E.bValid) { return false; }
		if (E.Room >= 0) { return RoomPasses(E.Room); }
		if (Portals.IsValidIndex(E.Portal))
		{
			// a door belongs to BOTH sides of its portal - it spawns when either room does
			return RoomPasses(Portals[E.Portal].From) || RoomPasses(Portals[E.Portal].To);
		}
		return RoomFilter.IsEmpty();
	};
	int32 Unroomed = 0;
	for (const FMloEntity& E : Ents)
	{
		if (E.bValid && E.Room < 0 && E.Portal < 0) { ++Unroomed; }
	}

	// ---- 3) import every referenced drawable present in the corpus (skip-if-exists;
	// the exact ydr/yft/ydd lane ImportMapArea proved, via the shared helper) ----
	TSet<FString> Needed;
	for (const FMloEntity& E : Ents)
	{
		if (!Passes(E)) { continue; }
		if (const FString* Asset = Index.ArchToAsset.Find(E.ArchLower)) { Needed.Add(*Asset); }
	}
	int32 MeshOk = 0, MeshSkip = 0, MeshFail = 0, MeshMissing = 0, Done = 0;
	FRudeImportTally Tally;
	for (const FString& D : Needed)
	{
		++Done;
		ImportIndexedDrawable(CorpusRoot, Index, D, DestMeshFolder, MeshOk, MeshSkip, MeshFail,
		                      MeshMissing, Tally);
		if (Done % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportMlo meshes %d/%d (ok %d, skip %d, fail %d)"),
				Done, Needed.Num(), MeshOk, MeshSkip, MeshFail);
			// KEEPFLAGS (= RF_Standalone in editor), NEVER RF_NoFlags - a no-keep GC deletes
			// the unsaved meshes this very run imported (the GC-sweep law).
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	// ---- 4) spawn: rooms' entities at MLO-LOCAL transforms, root at the WORLD ORIGIN ----
	// Idempotent respawn is per-ARCHETYPE and clear-by-TAG (🧠 agent's call): OFPA can rewrite
	// folder paths (BUILD_AREA_DESIGN R12), and a folder clear would also kill OTHER imported
	// interiors. Every actor of this interior carries IdTag, so root + room actors all die
	// here even though DestroyActor does not cascade to attached children.
	const FName IdTag(*(TEXT("RUDE_MLO:") + Search.FoundName));
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(IdTag)) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}
	// Outliner label prefers the caller's REAL spelling when the corpus stores only the hash
	// (labels are cosmetic; TAGS carry the corpus spelling as the deterministic identity).
	const FString Label = (Search.FoundName.StartsWith(TEXT("hash_")) && !Search.bWantedIsHashName)
		? Wanted : Search.FoundName;

	AActor* RootActor = World->SpawnActor<AActor>();
	if (!RootActor) { return Fail(TEXT("root actor spawn failed")); }
	{
		USceneComponent* RootComp = NewObject<USceneComponent>(RootActor, TEXT("Root"));
		RootActor->SetRootComponent(RootComp);
		RootComp->SetMobility(EComponentMobility::Static);
		RootComp->RegisterComponent();
		RootActor->AddInstanceComponent(RootComp);
		RootActor->SetActorLabel(TEXT("MLO_") + Label);
		RootActor->SetFolderPath(FName(TEXT("RUDE_MLO")));
		RootActor->Tags.Add(IdTag);
		RootActor->Tags.Add(FName(TEXT("RUDE_MLO_ROOT")));
	}

	// One actor per room (plus a portal-doors bucket and an unroomed bucket when needed),
	// attached under the root; inside each, the proven ImportScene ISM pattern - one
	// InstancedStaticMeshComponent per unique drawable, proxy cubes for corpus holes.
	struct FBucket
	{
		AActor* Actor = nullptr;
		USceneComponent* Root = nullptr;
		TMap<FString, UInstancedStaticMeshComponent*> IsmByMesh;
		int32 NumLights = 0;
	};
	TMap<int32, FBucket> Buckets;   // room index; -2 = portal-attached, -3 = unroomed
	auto GetBucket = [&](int32 Key) -> FBucket*
	{
		if (FBucket* B = Buckets.Find(Key)) { return B; }
		AActor* A = World->SpawnActor<AActor>();
		if (!A) { return nullptr; }
		USceneComponent* R = NewObject<USceneComponent>(A, TEXT("Root"));
		A->SetRootComponent(R);
		R->SetMobility(EComponentMobility::Static);
		R->RegisterComponent();
		A->AddInstanceComponent(R);
		const FString Suffix = (Key >= 0) ? Rooms[Key].Name
			: FString(Key == -2 ? TEXT("portalDoors") : TEXT("unroomed"));
		A->SetActorLabel(Label + TEXT("_") + Suffix);
		A->SetFolderPath(FName(TEXT("RUDE_MLO")));
		A->Tags.Add(IdTag);
		A->Tags.Add((Key >= 0) ? FName(*(TEXT("RUDE_MLO_Room:") + Rooms[Key].Name))
			: FName(Key == -2 ? TEXT("RUDE_MLO_Portal") : TEXT("RUDE_MLO_Room:(none)")));
		A->AttachToActor(RootActor, FAttachmentTransformRules::KeepWorldTransform);
		return &Buckets.Add(Key, FBucket{ A, R });
	};
	auto GetBucketIsm = [&](int32 Key, const FString& MeshKey, UStaticMesh* Mesh)
		-> UInstancedStaticMeshComponent*
	{
		FBucket* B = GetBucket(Key);
		if (!B) { return nullptr; }
		if (UInstancedStaticMeshComponent** Found = B->IsmByMesh.Find(MeshKey)) { return *Found; }
		UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(
			B->Actor, FName(*FString::Printf(TEXT("ISM_%d"), B->IsmByMesh.Num())));
		Ism->SetStaticMesh(Mesh);
		Ism->SetMobility(EComponentMobility::Static);
		Ism->SetupAttachment(B->Root);
		Ism->RegisterComponent();
		B->Actor->AddInstanceComponent(Ism);
		B->IsmByMesh.Add(MeshKey, Ism);
		return Ism;
	};

	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TMap<FString, UStaticMesh*> MeshCache;   // lowercase drawable -> mesh (nullptr = known-missing)
	int32 Spawned = 0, Proxies = 0, NumLights = 0, Unresolved = 0;
	for (int32 i = 0; i < Ents.Num(); ++i)
	{
		const FMloEntity& E = Ents[i];
		if (!Passes(E)) { continue; }
		const int32 Key = (E.Room >= 0) ? E.Room : (E.Portal >= 0 ? -2 : -3);

		const FString* Asset = Index.ArchToAsset.Find(E.ArchLower);
		UStaticMesh* Mesh = nullptr;
		if (Asset)
		{
			if (UStaticMesh** Cached = MeshCache.Find(*Asset)) { Mesh = *Cached; }
			else
			{
				Mesh = LoadObject<UStaticMesh>(nullptr, *(DestMeshFolder / *Asset));
				MeshCache.Add(*Asset, Mesh);
			}
		}
		else { ++Unresolved; }
		if (Mesh)
		{
			if (UInstancedStaticMeshComponent* Ism = GetBucketIsm(Key, *Asset, Mesh))
			{
				Ism->AddInstance(E.Xf, /*bWorldSpace*/ true);
				++Spawned;
			}
		}
		else if (ProxyCube)
		{
			if (UInstancedStaticMeshComponent* Ism = GetBucketIsm(Key, TEXT("proxy"), ProxyCube))
			{
				Ism->AddInstance(E.Xf, /*bWorldSpace*/ true);
				++Proxies;
			}
		}

		// lights: one component per CLightAttrDef instance, on the entity's room actor
		for (const FMloLight& L : E.Lights)
		{
			FBucket* B = GetBucket(Key);
			if (!B) { continue; }
			ULocalLightComponent* LC = nullptr;
			if (L.Type == 2)
			{
				USpotLightComponent* Spot = NewObject<USpotLightComponent>(B->Actor,
					FName(*FString::Printf(TEXT("Light_%d_%d"), i, B->NumLights)));
				// RAGE cone angles are half-angle degrees like UE's; UE's outer cone tops out
				// at 80, so RAGE's 90-degree hemisphere washes clamp (documented narrowing).
				Spot->SetOuterConeAngle(FMath::Clamp(L.ConeOuter, 1.f, 80.f));
				Spot->SetInnerConeAngle(FMath::Clamp(L.ConeInner, 0.f, Spot->OuterConeAngle));
				LC = Spot;
			}
			else if (L.Type == 1 || L.Type == 4)
			{
				UPointLightComponent* Pt = NewObject<UPointLightComponent>(B->Actor,
					FName(*FString::Printf(TEXT("Light_%d_%d"), i, B->NumLights)));
				if (L.Type == 4)
				{
					// capsule: a line emitter along `direction` - UE's point light expresses
					// exactly that as SourceLength (extents.x carries the length, measured on
					// the corpus tube lights).
					Pt->SetSourceLength(FMath::Max(0.f, L.ExtentX) * 100.f);
				}
				LC = Pt;
			}
			else
			{
				// only 1/2/4 are observed in the resolved corpus - an unknown type is refused
				// loudly per light, never guessed into some default shape
				++LightsSkipped;
				if (LightProblem.IsEmpty())
				{
					LightProblem = FString::Printf(
						TEXT("entity %d: lightType %d has no derived mapping (observed set: 1 point / 2 spot / 4 capsule)"),
						i, L.Type);
				}
				continue;
			}
			// Movable, not Static: the imported content has no lightmap-UV story, so the whole
			// RUDE lighting model is dynamic-only (BUILD_AREA_DESIGN section 4) - a Static light
			// here would render as unbuilt preview forever.
			LC->SetMobility(EComponentMobility::Movable);
			LC->SetupAttachment(B->Root);
			LC->RegisterComponent();
			B->Actor->AddInstanceComponent(LC);
			LC->SetLightColor(L.Color);
			LC->SetIntensityUnits(ELightUnits::Candelas);
			LC->SetIntensity(L.Intensity * RudeMloLightCandelaScale);
			LC->SetAttenuationRadius(FMath::Max(10.f, L.Falloff * 100.f));   // falloff metres -> cm
			const FVector WPos = E.Xf.TransformPosition(L.LocalPos);
			FRotator WRot = FRotator::ZeroRotator;
			const FVector WDir = E.Xf.TransformVectorNoScale(L.LocalDir);
			if (!WDir.IsNearlyZero())
			{
				WRot = FRotationMatrix::MakeFromX(WDir.GetSafeNormal()).Rotator();
			}
			LC->SetWorldLocationAndRotation(WPos, WRot);
			++B->NumLights;
			++NumLights;
		}
	}
	World->MarkPackageDirty();

	// portal summary: room names when the indices resolve, raw indices otherwise
	auto RoomLabel = [&Rooms](int32 Idx) -> FString
	{
		return Rooms.IsValidIndex(Idx) ? Rooms[Idx].Name : FString::FromInt(Idx);
	};
	FString PortalsJson;
	for (int32 i = 0; i < Portals.Num(); ++i)
	{
		PortalsJson += FString::Printf(TEXT("%s\"%s->%s\""), i ? TEXT(",") : TEXT(""),
			*RoomLabel(Portals[i].From), *RoomLabel(Portals[i].To));
	}
	const FString LightProblemJson = LightProblem.IsEmpty()
		? FString()
		: FString::Printf(TEXT("\"lightProblem\":\"%s\","), *LightProblem);
	// "entities" stays the SLOT COUNT (that is what rooms and portals index into); the placeholders
	// added for malformed records are broken out separately so the two can never be confused.
	// ok is computed: an interior that indexed nothing or placed nothing is not a success, and the
	// per-mesh tally is forwarded so this lane - which together with ImportMapArea is the ONLY
	// consumer of the yft and ydd import paths - finally reports the quality of what it imported.
	// ⚠ DELIBERATELY WEAK. I have no measurement of how often a legitimate room filter leaves an
	// interior with nothing to place, so the gate fires only on shapes that cannot be legitimate:
	// an MLO with neither a room nor a portal (it did not parse as an interior at all), or one
	// that placed NOTHING - not even a proxy cube - while having entity slots to place. A gate
	// that fires on a normal run is a gate that gets ignored; tightening this one needs an
	// in-editor run across several interiors, which is on the handoff list.
	const bool bMloOk = (Rooms.Num() > 0 || Portals.Num() > 0)
		&& (Spawned + Proxies > 0 || Ents.Num() == 0);
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"archetype\":\"%s\",\"requested\":\"%s\",\"ytyp\":\"%s\","
		"\"rooms\":%d,\"roomNames\":[%s],\"portals\":%d,\"portalRooms\":[%s],"
		"\"entitySets\":[%s],\"entities\":%d,\"entitiesMissingTransform\":%d,"
		"\"spawned\":%d,\"proxies\":%d,"
		"\"unresolvedArchetypes\":%d,\"lights\":%d,\"lightsSkipped\":%d,%s"
		"\"otherExtensions\":%d,\"badAttachedRefs\":%d,\"unroomedEntities\":%d,"
		"\"meshesImported\":%d,\"meshesSkipped\":%d,\"meshesFailed\":%d,"
		"\"meshesMissingFromCorpus\":%d,%s}"),
		bMloOk ? TEXT("true") : TEXT("false"),
		*Search.FoundName, *Wanted, *FPaths::GetCleanFilename(Search.FoundFile),
		Rooms.Num(), *RoomNamesJson, Portals.Num(), *PortalsJson,
		*SetsJson, Ents.Num(), EntitiesMissingTransform, Spawned, Proxies,
		Unresolved, NumLights, LightsSkipped, *LightProblemJson,
		OtherExtensions, BadRefs, Unroomed,
		MeshOk, MeshSkip, MeshFail, MeshMissing, *Tally.ToJson());
}

// Ported from the in-game-proven tools/emit_ytyp.py - the archetype flag +
// physicsDictionary laws are load-bearing (FULL COLLISION MODEL, 2026-07-24).
FString URudeToolset::ExportYtyp(const FString& YdrSpecs, const FString& YtypName,
                                 const FString& OutYtypPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Specs;
	YdrSpecs.ParseIntoArray(Specs, TEXT(","), true);
	if (Specs.Num() == 0) { return Fail(TEXT("no ydr specs (want absPath[;txd[;physDict]], ...)")); }

	FString Archetypes;
	// ADDED 2026-08-03 - two specs whose drawables share a <Name> used to emit two same-named
	// <Item>s and report archetypes:2, while the game keeps exactly one. Silent halving of the
	// archetype set, with a count that says otherwise.
	TSet<FString> SeenNames;
	for (const FString& SpecStr : Specs)
	{
		TArray<FString> F;
		SpecStr.TrimStartAndEnd().ParseIntoArray(F, TEXT(";"), false);
		FXmlFile Xml(F[0]);
		if (!Xml.IsValid()) { return Fail(FString::Printf(TEXT("XML load failed: %s"), *F[0])); }
		const FXmlNode* Root = Xml.GetRootNode();
		if (!Root || Root->GetTag() != TEXT("Drawable")) { return Fail(FString::Printf(TEXT("not a Drawable: %s"), *F[0])); }

		FString Name = FPaths::GetBaseFilename(F[0]);
		Name.RemoveFromEnd(TEXT(".ydr"));
		if (const FXmlNode* N = Root->FindChildNode(TEXT("Name")))
		{
			FString S = N->GetContent().TrimStartAndEnd();
			int32 Dot; if (S.FindChar(TEXT('.'), Dot)) { S.LeftInline(Dot); }
			if (!S.IsEmpty()) { Name = S; }
		}
		Name.ToLowerInline();
		if (SeenNames.Contains(Name))
		{
			return Fail(FString::Printf(TEXT("duplicate archetype name '%s' (from %s); the game keeps only one"),
			                            *Name, *F[0]));
		}
		SeenNames.Add(Name);
		auto Vec = [&](const TCHAR* Tag, float V[3]) -> bool
		{
			const FXmlNode* E = Root->FindChildNode(Tag);
			if (!E) { return false; }
			V[0] = FCString::Atof(*E->GetAttribute(TEXT("x")));
			V[1] = FCString::Atof(*E->GetAttribute(TEXT("y")));
			V[2] = FCString::Atof(*E->GetAttribute(TEXT("z")));
			return true;
		};
		float BbMin[3], BbMax[3], Bsc[3];
		if (!Vec(TEXT("BoundingBoxMin"), BbMin) || !Vec(TEXT("BoundingBoxMax"), BbMax) ||
		    !Vec(TEXT("BoundingSphereCenter"), Bsc))
		{
			return Fail(FString::Printf(TEXT("missing bounds fields: %s"), *F[0]));
		}
		// FIXED 2026-08-03 - bsRadius was OPTIONAL while its three sibling bounds fields above are
		// hard requirements: a drawable with no <BoundingSphereRadius> shipped bsRadius 0 with
		// ok:true. bsRadius is the archetype's cull sphere, so radius 0 is an entity the engine can
		// cull immediately - an invisible prop reported as a successful export.
		const FXmlNode* RNode = Root->FindChildNode(TEXT("BoundingSphereRadius"));
		if (!RNode) { return Fail(FString::Printf(TEXT("missing bounds fields: %s"), *F[0])); }
		const float Bsr = FCString::Atof(*RNode->GetAttribute(TEXT("value")));
		// Collidable iff the drawable embeds a <Bounds> that actually describes collision.
		// ⛔ THIS USED TO REQUIRE <Children>, WHICH IS ONLY TRUE OF A *COMPOSITE* ROOT (fixed
		// 2026-07-31). A phBound root may legitimately be a primitive - Box, Sphere, Cylinder -
		// and those carry no <Children> at all: measured 220 of 1,012 bound-bearing base-game
		// ydr (21.7%; Box 160 / Sphere 53 / Cylinder 7), a figure the converter's own docstring
		// records. Every one of them was exported with the collidable bit CLEAR, so a fifth of
		// all collidable props shipped as pass-through geometry - invisible in the editor,
		// visible only by walking through a crate in game. Presence of a <Bounds> with a known
		// type is the real signal; <Children> is one shape of it.
		// ⚠ The type is an ATTRIBUTE - `<Bounds type="Composite">` - NOT a <Type> child element.
		// Checked against real emitted corpus XML before trusting it: a FindChildNode("Type")
		// test would have compiled, run, and never once fired.
		bool bCollidable = false;
		if (const FXmlNode* B = Root->FindChildNode(TEXT("Bounds")))
		{
			const FString BoundType = B->GetAttribute(TEXT("type")).TrimStartAndEnd();
			if (const FXmlNode* C = B->FindChildNode(TEXT("Children")))
			{
				bCollidable = C->GetChildrenNodes().Num() > 0;
			}
			// A primitive (or BVH/Geometry) root IS collision, with no children to count.
			if (!bCollidable && !BoundType.IsEmpty()
				&& !BoundType.Equals(TEXT("Composite"), ESearchCase::IgnoreCase))
			{
				bCollidable = true;
			}
		}
		// embedded ShaderGroup TextureDictionary -> empty archetype txd
		bool bEmbeddedTex = false;
		if (const FXmlNode* SG = Root->FindChildNode(TEXT("ShaderGroup")))
		{
			if (const FXmlNode* TD = SG->FindChildNode(TEXT("TextureDictionary")))
			{
				bEmbeddedTex = TD->GetChildrenNodes().Num() > 0;
			}
		}
		const FString Txd = (F.Num() > 1 && !F[1].IsEmpty()) ? F[1] : (bEmbeddedTex ? TEXT("") : Name);
		const FString PhysDict = (F.Num() > 2 && !F[2].IsEmpty()) ? F[2] : (bCollidable ? Name : TEXT(""));
		const uint32 Flags = (bCollidable || !PhysDict.IsEmpty()) ? 537001984u : 536870912u;
		const int32 LodDist = FMath::Max(100, (int32)(Bsr * 4.f));
		// FIXED 2026-08-03 - hdTextureDist was emitted as lodDist. It is the HD-texture streaming
		// radius, NOT the draw distance, and the two are unrelated in the game's own data: measured
		// over 12,582 ASSET_TYPE_DRAWABLE archetypes from a 300-file sample of the resolved base-game
		// ytyp corpus, hdTextureDist == lodDist in 1.61% of archetypes. The distributions barely
		// overlap - modal hdTextureDist 5.0 (3,078), then 50.0 (1,015) and 149.5 (910); modal
		// lodDist 100 (3,020), then 299 (1,096). RUDE emitted >= 100 for EVERY archetype, i.e. at
		// least 20x the game's single most common value, so the engine resident-loaded HD textures
		// for every RUDE prop far earlier than for a base-game prop and any streaming/memory
		// comparison between a RUDE area and a Rockstar area was measuring this default.
		// The 3.0 factor is the measured median hdTextureDist/bsRadius ratio (p25 1.19, median 2.94,
		// p75 8.70) and the 5..150 clamp brackets the observed value band (p10 5, p50 50, p90 168).
		// HONEST LIMIT: the real distribution is multi-modal and per-archetype intent is NOT
		// recoverable from bsRadius alone - this is a defensible default, not a recovered value.
		const int32 HdDist = FMath::Clamp((int32)(Bsr * 3.f), 5, 150);
		Archetypes += FString::Printf(TEXT(
			"  <Item type=\"CBaseArchetypeDef\">\n"
			"   <lodDist value=\"%d\" />\n   <flags value=\"%u\" />\n"
			"   <specialAttribute value=\"0\" />\n"
			"   <bbMin x=\"%f\" y=\"%f\" z=\"%f\" />\n   <bbMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"
			"   <bsCentre x=\"%f\" y=\"%f\" z=\"%f\" />\n   <bsRadius value=\"%f\" />\n"
			"   <hdTextureDist value=\"%d\" />\n   <name>%s</name>\n"
			"   <textureDictionary>%s</textureDictionary>\n   <clipDictionary />\n"
			"   <drawableDictionary />\n   <physicsDictionary>%s</physicsDictionary>\n"
			"   <assetType>ASSET_TYPE_DRAWABLE</assetType>\n   <assetName>%s</assetName>\n"
			"   <extensions />\n  </Item>\n"),
			LodDist, Flags, BbMin[0], BbMin[1], BbMin[2], BbMax[0], BbMax[1], BbMax[2],
			Bsc[0], Bsc[1], Bsc[2], Bsr, HdDist, *Name, *Txd, *PhysDict, *Name);
	}
	const FString Ytyp = FString::Printf(TEXT(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapTypes>\n <extensions />\n <archetypes>\n%s"
		" </archetypes>\n <name>%s</name>\n <dependencies />\n"
		" <compositeEntityTypes itemType=\"CCompositeEntityType\" />\n</CMapTypes>\n"),
		*Archetypes, *YtypName);
	if (!FFileHelper::SaveStringToFile(Ytyp, *OutYtypPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write ytyp"));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"ytypPath\":\"%s\",\"archetypes\":%d}"),
		*OutYtypPath, Specs.Num());
}

// Ported from the in-game-proven tools/emit_ymap.py (P0-validated placement lane;
// EXPORT-side transform + quat conventions, bench-pinned).
FString URudeToolset::ExportYmap(const FString& EntitiesJsonPath, const FString& MapName,
                                 const FString& OutDir)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *EntitiesJsonPath)) { return Fail(TEXT("cannot read entities JSON")); }
	TArray<TSharedPtr<FJsonValue>> Ents;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Ents) || Ents.Num() == 0)
		{
			return Fail(TEXT("entities JSON must be a non-empty array"));
		}
	}
	FString Rows;
	double MinX = 1e18, MinY = 1e18, MinZ = 1e18, MaxX = -1e18, MaxY = -1e18, MaxZ = -1e18;
	int32 Count = 0;
	// ⛔ A FIELD-LESS ENTITY USED TO EXPORT AS A SUCCESS (fixed 2026-08-03).
	// FJsonObject::GetStringField / GetNumberField DO NOT fail on a missing field - they log a
	// LogJson warning and hand back ""/0.0 (UE 5.8 JsonObject.cpp:474/404 -> GetField:338 returns
	// FJsonValueNull; JsonValue.cpp:26/13). So {"ue":{}} became <archetypeName></archetypeName> at
	// (0,0,0), that origin was folded into streamingExtents, and the call returned ok:true with the
	// bogus entity COUNTED. Measured on the 12-entity showcase scene: ONE such entity inflates the
	// streaming box from 636x666 m to 828x1570 m - 3.1x the area - which changes when the whole
	// resource streams in. <archetypeName> is never empty in the game's own data (0 of 136,786
	// across 900 base-game ymaps). Three further paths dropped an entity with an uncounted
	// `continue`, so an upstream producer bug silently shrank the map with nothing in the JSON
	// saying so.
	// An entity the caller asked us to place that cannot exist in game is a REFUSAL, not a default,
	// and it is refused BEFORE it touches the extents fold.
	for (int32 EntIdx = 0; EntIdx < Ents.Num(); ++EntIdx)
	{
		const TSharedPtr<FJsonValue>& V = Ents[EntIdx];
		const TSharedPtr<FJsonObject>* E;
		if (!V.IsValid() || !V->TryGetObject(E))
		{
			return Fail(FString::Printf(TEXT("entity %d is not a JSON object"), EntIdx));
		}
		const TSharedPtr<FJsonObject>* Ue;
		if (!(*E)->TryGetObjectField(TEXT("ue"), Ue))
		{
			return Fail(FString::Printf(TEXT("entity %d has no 'ue' object (want {x,y,z} in UE cm)"), EntIdx));
		}
		FString Arch;
		if (!(*E)->TryGetStringField(TEXT("archetype"), Arch) || Arch.TrimStartAndEnd().IsEmpty())
		{
			return Fail(FString::Printf(
				TEXT("entity %d has no non-empty 'archetype'; an empty archetypeName resolves to no archetype in game"),
				EntIdx));
		}
		// ExportYtyp lowercases the archetype name it emits (:3892) while this lane used to pass the
		// caller's string through verbatim - the two halves of RUDE's own round trip could disagree
		// on case, and the symptom would be "the prop just doesn't appear" with a ytyp and a ymap
		// that both read correctly by eye. Every real archetype name in the sampled corpus is
		// lowercase, so normalising here makes the question moot rather than betting on the
		// downstream converter hashing case-insensitively.
		Arch = Arch.TrimStartAndEnd().ToLower();
		if (Arch.Contains(TEXT("<")) || Arch.Contains(TEXT(">")) || Arch.Contains(TEXT("&"))
			|| Arch.Contains(TEXT("\"")) || Arch.Contains(TEXT("'")))
		{
			// this value is interpolated straight into XML with no escaping
			return Fail(FString::Printf(
				TEXT("entity %d archetype '%s' contains an XML-special character"), EntIdx, *Arch));
		}
		double Ux = 0, Uy = 0, Uz = 0;
		if (!(*Ue)->TryGetNumberField(TEXT("x"), Ux) || !(*Ue)->TryGetNumberField(TEXT("y"), Uy)
			|| !(*Ue)->TryGetNumberField(TEXT("z"), Uz))
		{
			return Fail(FString::Printf(TEXT("entity %d ('%s') is missing ue.x / ue.y / ue.z"), EntIdx, *Arch));
		}
		const double X = Ux / 100.0;
		const double Y = -Uy / 100.0;
		const double Z = Uz / 100.0;
		double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
		const TSharedPtr<FJsonObject>* Q;
		if ((*E)->TryGetObjectField(TEXT("ue_quat"), Q))
		{
			// ⛔⛔ THIS WAS THE INVERSE OF THE CORRECT ROTATION, AND IT SHIPPED (fixed 2026-08-03).
			// The import lane applies g(q) = (x, -y, z, w) (:3146, :3427); this wrote
			// m(q) = (-x, y, -z, w). Both are involutions, but they are DIFFERENT ones, so
			// g(m(q)) = conj(q): a UE -> GTA -> UE round trip inverted every rotation. Every
			// entity authored in Unreal shipped to FiveM facing the wrong way, and the old
			// comment called that "bench-pinned".
			// WHICH ONE IS RIGHT WAS PROVEN AGAINST ROCKSTAR'S OWN DATA, not reasoned: a ymap
			// declares <entitiesExtentsMin/Max>, so transforming each archetype's bbMin/bbMax by
			// the entity transform and unioning must reproduce it. On the single-entity ymap
			// facelobbyfake_lod (one entity, rot z=-0.2377 w=0.9713) the INVERSE reproduces the
			// declared extents EXACTLY (max error 0.0000 m) while the forward quaternion is
			// 22.77 m out in Y - re-verified in this session, independently of the audit that
			// found it. Corpus-wide, 81.25% of 1,690,098 entity rotations have forward != inverse
			// and 79.54% differ by more than 5 degrees: unmistakable by eye, had anyone looked at
			// an exported placement in game.
			// ⇒ A ymap <rotation> stores the entity's INVERSE orientation, so ue->gta is
			// conj(mirror(q_ue)) == (x, -y, z, w) - THE SAME involution the import lane uses. The
			// two lanes must share this formula; it is its own inverse.
			// ⚠ NOT a global rule: a phBound CompositeTransform stores a FORWARD matrix and keeps
			// the pure mirror at :1480 (verified 4,031/4,031 real composite children). The
			// distinction is "ymap entity = inverse-stored, phBound = forward-stored".
			// A PARTIAL ue_quat used to default silently: a missing "w" became 0, giving a
			// zero-norm quaternion that no rotation can be recovered from, with ok:true.
			double Rx = 0, Ry = 0, Rz = 0, Rw = 0;
			if (!(*Q)->TryGetNumberField(TEXT("x"), Rx) || !(*Q)->TryGetNumberField(TEXT("y"), Ry)
				|| !(*Q)->TryGetNumberField(TEXT("z"), Rz) || !(*Q)->TryGetNumberField(TEXT("w"), Rw))
			{
				return Fail(FString::Printf(
					TEXT("entity %d ('%s') has a partial 'ue_quat'; x,y,z,w are all required "
					     "(a missing w defaults to 0 = a zero-norm quaternion)"), EntIdx, *Arch));
			}
			const double N2 = Rx * Rx + Ry * Ry + Rz * Rz + Rw * Rw;
			if (FMath::Abs(N2 - 1.0) > 1e-3)
			{
				return Fail(FString::Printf(
					TEXT("entity %d ('%s') ue_quat is not unit-length (|q|^2 = %f)"), EntIdx, *Arch, N2));
			}
			// ⚠ the involution below is the conductor's 2026-08-03 rotation fix - (x, -y, z, w),
			// the SAME one the import lane uses. Do not "restore" (-x, y, -z, w): that is the
			// inverse, and it is what shipped every UE-authored entity facing the wrong way.
			Qx = Rx;
			Qy = -Ry;
			Qz = Rz;
			Qw = Rw;
		}
		MinX = FMath::Min(MinX, X); MinY = FMath::Min(MinY, Y); MinZ = FMath::Min(MinZ, Z);
		MaxX = FMath::Max(MaxX, X); MaxY = FMath::Max(MaxY, Y); MaxZ = FMath::Max(MaxZ, Z);
		// EntIdx folded in 2026-08-03: the guid was a CRC over map:arch:x:y:z, so two entities of the
		// same archetype within 1e-6 m collided, and an entity-dedup pass downstream would fold them
		// to one. The index makes it unconditionally unique.
		const uint32 Guid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%d:%s:%f:%f:%f"), *MapName, EntIdx, *Arch, X, Y, Z));
		Rows += FString::Printf(TEXT(
			"  <Item type=\"CEntityDef\">\n   <archetypeName>%s</archetypeName>\n"
			"   <flags value=\"1572864\" />\n   <guid value=\"%u\" />\n"
			"   <position x=\"%f\" y=\"%f\" z=\"%f\" />\n"
			"   <rotation x=\"%f\" y=\"%f\" z=\"%f\" w=\"%f\" />\n"
			"   <scaleXY value=\"1\" />\n   <scaleZ value=\"1\" />\n   <parentIndex value=\"-1\" />\n"
			"   <lodDist value=\"500\" />\n   <childLodDist value=\"0\" />\n"
			"   <lodLevel>LODTYPES_DEPTH_ORPHANHD</lodLevel>\n   <numChildren value=\"0\" />\n"
			"   <priorityLevel>PRI_REQUIRED</priorityLevel>\n   <extensions />\n"
			"   <ambientOcclusionMultiplier value=\"255\" />\n"
			"   <artificialAmbientOcclusion value=\"255\" />\n   <tintValue value=\"0\" />\n  </Item>\n"),
			*Arch, Guid, X, Y, Z, Qx, Qy, Qz, Qw);
		++Count;
	}
	if (Count == 0) { return Fail(TEXT("no valid entities")); }
	const double M = 10.0, S = 300.0;
	const FString Ymap = FString::Printf(TEXT(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapData>\n <name>%s</name>\n <parent />\n"
		" <flags value=\"0\" />\n <contentFlags value=\"1\" />\n"
		" <streamingExtentsMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"
		" <streamingExtentsMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"
		" <entitiesExtentsMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"
		" <entitiesExtentsMax x=\"%f\" y=\"%f\" z=\"%f\" />\n <entities>\n%s </entities>\n"
		" <containerLods itemType=\"rage__fwContainerLodDef\" />\n <boxOccluders itemType=\"BoxOccluder\" />\n"
		" <occludeModels itemType=\"OccludeModel\" />\n <physicsDictionaries />\n <instancedData>\n"
		"  <ImapLink />\n  <PropInstanceList itemType=\"rage__fwPropInstanceListDef\" />\n"
		"  <GrassInstanceList itemType=\"rage__fwGrassInstanceListDef\" />\n </instancedData>\n"
		" <timeCycleModifiers itemType=\"CTimeCycleModifier\" />\n <carGenerators itemType=\"CCarGen\" />\n"
		" <LODLightsSOA>\n  <direction itemType=\"FloatXYZ\" />\n  <falloff />\n  <falloffExponent />\n"
		"  <timeAndStateFlags />\n  <hash />\n  <coneInnerAngle />\n  <coneOuterAngleOrCapExt />\n"
		"  <coronaIntensity />\n </LODLightsSOA>\n <DistantLODLightsSOA>\n"
		"  <position itemType=\"FloatXYZ\" />\n  <RGBI />\n  <numStreetLights value=\"0\" />\n"
		"  <category value=\"0\" />\n </DistantLODLightsSOA>\n <block>\n  <version value=\"0\" />\n"
		"  <flags value=\"0\" />\n  <name>%s</name>\n  <exportedBy>RUDE</exportedBy>\n  <owner></owner>\n"
		"  <time></time>\n </block>\n</CMapData>\n"),
		*MapName,
		MinX - S, MinY - S, MinZ - S, MaxX + S, MaxY + S, MaxZ + S,
		MinX - M, MinY - M, MinZ - M, MaxX + M, MaxY + M, MaxZ + M,
		*Rows, *MapName);
	const FString StreamDir = OutDir / TEXT("stream");
	IFileManager::Get().MakeDirectory(*StreamDir, true);
	const FString YmapPath = StreamDir / (MapName + TEXT(".ymap"));
	if (!FFileHelper::SaveStringToFile(Ymap, *YmapPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write ymap"));
	}
	const FString Manifest = TEXT(
		"fx_version 'cerulean'\ngame 'gta5'\n\n"
		"author 'RUDE - RAGE <-> Unreal Development Environment'\n"
		"description 'RUDE-authored placement resource'\n\n"
		"-- Required for streamed ymaps to take effect (reloads map storage on load).\n"
		"this_is_a_map 'yes'\n");
	FFileHelper::SaveStringToFile(Manifest, *(OutDir / TEXT("fxmanifest.lua")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return FString::Printf(TEXT("{\"ok\":true,\"ymapPath\":\"%s\",\"entities\":%d}"), *YmapPath, Count);
}

FString URudeToolset::ImportYdrBatch(const FString& ListPath, const FString& DestFolder,
                                     const FString& Mode, const FString& CorpusRoot)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *ListPath))
	{
		return Fail(TEXT("cannot read list file"));
	}
	const bool bForce = Mode.TrimStartAndEnd().Equals(TEXT("FORCE"), ESearchCase::IgnoreCase);
	// ⭐ CorpusRoot is OPTIONAL and NEW (2026-08-05, #43). This batch takes a list of absolute XML
	// paths and has no archetype - which is exactly why the 08-05 400-drawable run tie-broke 95.8%
	// of its texture binds. Given a corpus root it builds the SAME archetype index the map lane
	// builds and scopes each file's textures by its archetype's declared <textureDictionary>.
	// Left empty it behaves exactly as before, so no existing 3-argument caller changes meaning:
	// FRudeInvoke::Call performs no arity check and pads a missing trailing argument with an empty
	// FString (the same property that let ImportMapArea gain Mode).
	// ⛔ Deliberately NOT derived from the list paths. Guessing "..\..\" off the first line would
	// silently point at whatever happened to be two directories up and produce a scope nobody
	// asked for - and a wrong scope is a wrong texture, which is the defect this fixes.
	FRudeArchetypeIndex ScopeIndex;
	bool bHaveScope = false;
	if (!CorpusRoot.TrimStartAndEnd().IsEmpty())
	{
		FString IndexErr;
		if (!BuildCorpusArchetypeIndex(CorpusRoot, ScopeIndex, IndexErr, /*MloSearch*/ nullptr))
		{
			// REFUSE, do not carry on unscoped. A CorpusRoot was passed on purpose; silently
			// ignoring it would report the pre-fix numbers under a post-fix command line.
			return Fail(FString::Printf(TEXT("CorpusRoot given but the archetype index failed: %s"),
				*IndexErr));
		}
		bHaveScope = true;
	}
	int32 Imported = 0, Skipped = 0, Failed = 0;
	// ⛔ THE BATCH USED TO THROW THESE AWAY, and that is why "did the rebind work?" was
	// unanswerable after 4,956 files ran with ok:true (2026-07-29). Every file reported its own
	// texture verdict; the batch summed only ok/skip/fail, so a run that bound ZERO textures and a
	// run that bound all of them printed the identical line. A batch must aggregate the counters
	// its unit reports - a silent contributor has to be as loud as a failing one.
	int32 Bound = 0, Unsupported = 0, MissingTex = 0, UnmappedSamp = 0;
	int32 ValSeen = 0, ValBound = 0, ValUnsupported = 0, ValDeduped = 0;
	// ⛔ THE BATCH SUMMED SEVEN FIELDS AND STOPPED, and the two it left out were the ones that
	// report LOST GEOMETRY. ImportYdr has emitted geometriesDropped + geometryErrors since
	// 2026-07-31; the batch never read them, so a run in which a fraction of every mesh failed to
	// parse printed imported=N with nothing else moving - the identical failure this batch was
	// fixed for on 2026-07-29, one level down. MEASURED drop rate on today's corpus is 0 (900
	// resolved ydr / 3,444 geometries: no unknown semantics, no misalignment), which is exactly
	// when a blind spot goes unnoticed. Everything ImportYdr counts is now summed here.
	int32 GeosDropped = 0, GeoErrors = 0, GeosNoUV = 0;
	int32 TrisOOR = 0, TrisDegen = 0;
	int32 FromEmbedded = 0, Ambiguous = 0, NoShaderDef = 0, NoMaterial = 0;
	int32 Scoped = 0, TieBroken = 0;      // #43 split
	int32 FilesWithScope = 0;             // files whose archetype named a textureDictionary
	// #43/#21b per-tier split + the reason a tie-break was still needed.
	int32 AmbTotal = 0, FromArchTxd = 0, FromParentTxd = 0, FromYtyp = 0, FromSlot = 0;
	int32 ScopedAuth = 0, ScopedProv = 0;
	int32 TbEmbedded = 0, TbNoScope = 0, TbSlotAmb = 0, TbYtypAmb = 0, TbDictAbsent = 0, TbNotInScope = 0;
	// Per-FILE scope availability. Separated because "no chain" and "an unresolvable hash_ scope"
	// are different gaps with different fixes, and one number would hide both.
	int32 FilesWithParentChain = 0;       // archetype txd has at least one gtxd ancestor
	int32 FilesWithHashTxd = 0;           // <textureDictionary> is an unresolved joaat (hash_XXXXXXXX)
	int32 FilesWithSlot = 0;              // _RESOLVED.json knows which slot this asset was won from
	int32 FilesWithYtypSet = 0;           // the asset's ytyp declares at least one dictionary
	// #40: the batch sums EVERY counter its unit reports, including the new collision ones. A batch
	// that summed geometry but not collision would be the same blind spot this batch has now been
	// fixed for twice (textures 2026-07-29, geometry 2026-07-31).
	int32 ColSeen = 0, ColPrims = 0, ColMeshes = 0, ColUnmapped = 0, ColMalformed = 0;
	int32 ColPolysDropped = 0, ColTris = 0, FilesWithCollision = 0;
	FString FailedFiles;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Path = Lines[i].TrimStartAndEnd();
		if (Path.IsEmpty()) { continue; }
		// skip-if-exists on the FILENAME base (corpus files are named <drawable>.ydr.xml,
		// matching the drawable <Name> ImportYdr derives) - idempotent re-runs.
		// FORCE mode reimports in place (MI re-bind after a texture pass).
		FString Base = FPaths::GetBaseFilename(Path);
		Base.RemoveFromEnd(TEXT(".ydr"));
		if (!bForce && FPackageName::DoesPackageExist(DestFolder / Base))
		{
			++Skipped;
			continue;
		}
		// The list names <drawable>.ydr.xml, and the archetype index is keyed on the drawable
		// assetName - the same join ImportIndexedDrawable makes, so the two lanes cannot disagree
		// about which dictionary a mesh belongs to.
		FRudeTextureScope Scope;
		if (bHaveScope)
		{
			Scope = ScopeIndex.MakeScope(Base.ToLower());
			if (!Scope.ArchetypeTxd.IsEmpty())
			{
				++FilesWithScope;
				if (Scope.ArchetypeTxd.StartsWith(TEXT("hash_"))) { ++FilesWithHashTxd; }
			}
			if (Scope.ParentTxdChain.Num() > 0) { ++FilesWithParentChain; }
			if (Scope.YtypNeighbours && Scope.YtypNeighbours->Num() > 0) { ++FilesWithYtypSet; }
			if (!Scope.AssetSlot.IsEmpty()) { ++FilesWithSlot; }
		}
		const FString R = RudeImportYdrScoped(Path, DestFolder, bHaveScope ? &Scope : nullptr);
		// ⚠ Counters are read on BOTH paths, matching FRudeImportTally. Since ImportYdr's ok is now
		// computed (slotsWithoutMaterial), a mesh can come back ok:false while still carrying the
		// counters that explain WHY - and a hard failure carries none of them, so they read 0.
		// Dropping them because of the boolean is the same blindness one level down.
		Bound          += RudeSumField(R, TEXT("boundTextures"));
		Unsupported    += RudeSumField(R, TEXT("unsupportedByMaster"));
		MissingTex     += RudeSumField(R, TEXT("missingTextures"));
		UnmappedSamp   += RudeSumField(R, TEXT("unmappedSamplers"));
		ValSeen        += RudeSumField(R, TEXT("valueParamsSeen"));
		ValBound       += RudeSumField(R, TEXT("valueParamsBound"));
		ValUnsupported += RudeSumField(R, TEXT("valueParamsUnsupported"));
		ValDeduped     += RudeSumField(R, TEXT("valueParamsDeduped"));
		GeosDropped    += RudeSumField(R, TEXT("geometriesDropped"));
		GeosNoUV       += RudeSumField(R, TEXT("geometriesWithoutUV"));
		TrisOOR        += RudeSumField(R, TEXT("trianglesOutOfRange"));
		TrisDegen      += RudeSumField(R, TEXT("trianglesDegenerate"));
		FromEmbedded   += RudeSumField(R, TEXT("texturesFromEmbedded"));
		Ambiguous      += RudeSumField(R, TEXT("ambiguousTextures"));
		Scoped         += RudeSumField(R, TEXT("texturesResolvedScoped"));
		TieBroken      += RudeSumField(R, TEXT("texturesTieBroken"));
		AmbTotal       += RudeSumField(R, TEXT("texturesAmbiguousTotal"));
		FromArchTxd    += RudeSumField(R, TEXT("texturesFromArchetypeTxd"));
		FromParentTxd  += RudeSumField(R, TEXT("texturesFromParentTxd"));
		FromYtyp       += RudeSumField(R, TEXT("texturesFromYtypNeighbour"));
		FromSlot       += RudeSumField(R, TEXT("texturesFromSameSlot"));
		ScopedAuth     += RudeSumField(R, TEXT("texturesScopedAuthoritative"));
		ScopedProv     += RudeSumField(R, TEXT("texturesScopedProvenance"));
		TbEmbedded     += RudeSumField(R, TEXT("tieBreakEmbeddedNotImported"));
		TbNoScope      += RudeSumField(R, TEXT("tieBreakNoScope"));
		TbSlotAmb      += RudeSumField(R, TEXT("tieBreakSlotAmbiguous"));
		TbYtypAmb      += RudeSumField(R, TEXT("tieBreakYtypAmbiguous"));
		TbDictAbsent   += RudeSumField(R, TEXT("tieBreakScopeDictAbsent"));
		TbNotInScope   += RudeSumField(R, TEXT("tieBreakNameNotInScope"));
		NoShaderDef    += RudeSumField(R, TEXT("slotsWithoutShaderDef"));
		NoMaterial     += RudeSumField(R, TEXT("slotsWithoutMaterial"));
		const int32 SeenHere = RudeSumField(R, TEXT("collisionBoundsSeen"));
		ColSeen        += SeenHere;
		ColPrims       += RudeSumField(R, TEXT("collisionPrimitivesImported"));
		ColMeshes      += RudeSumField(R, TEXT("collisionMeshesImported"));
		ColUnmapped    += RudeSumField(R, TEXT("collisionBoundsUnmapped"));
		ColMalformed   += RudeSumField(R, TEXT("collisionBoundsMalformed"));
		ColPolysDropped+= RudeSumField(R, TEXT("collisionPolysDropped"));
		ColTris        += RudeSumField(R, TEXT("collisionTriangles"));
		if (SeenHere > 0) { ++FilesWithCollision; }
		// geometryErrors is a JSON ARRAY, so it cannot be summed - count the FILES that carry a
		// non-empty one. Zero here alongside a non-zero geometriesDropped would mean every dropped
		// geometry is unexplained, which is itself a defect worth seeing.
		if (R.Contains(TEXT("\"geometryErrors\":[\""))) { ++GeoErrors; }
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Imported;
		}
		else
		{
			++Failed;
			if (Failed <= 30)
			{
				FailedFiles += FString::Printf(TEXT("%s\"%s\""), FailedFiles.IsEmpty() ? TEXT("") : TEXT(","), *Base);
			}
		}
		if ((i + 1) % 50 == 0)
		{
			UE_LOG(LogTemp, Display,
				TEXT("[RUDE] ImportYdrBatch %d/%d (ok %d, skip %d, fail %d | tex bound %d, "
				     "unsupported %d, missing %d, unmapped %d)"),
				i + 1, Lines.Num(), Imported, Skipped, Failed,
				Bound, Unsupported, MissingTex, UnmappedSamp);
		}
		if ((i + 1) % 250 == 0)
		{
			// Keep editor memory flat on long batches - but with KEEPFLAGS (= RF_Standalone in
			// editor), NEVER RF_NoFlags, which deletes the unsaved meshes this very batch imported.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportYdrBatch DONE: %d imported, %d skipped, %d failed | geometries dropped %d "
		     "(%d files explained), no-UV %d | triangles out-of-range %d, degenerate %d | textures "
		     "bound %d, unsupportedByMaster %d, missing %d, unmappedSamplers %d | ambiguous %d = "
		     "scoped %d (embedded %d + archetypeTxd %d + parentTxd %d | ytypNeighbour %d + sameSlot "
		     "%d) + tieBroken %d [embeddedNotImported %d, noScope %d, slotAmbiguous %d, "
		     "ytypAmbiguous %d, scopeDictAbsent %d, nameNotInScope %d] | files: %d archetype txd "
		     "(%d unresolved hash_), %d parent chain, %d ytyp set, %d slot | slots without shader "
		     "def %d, without material %d | value params seen %d = bound %d + unsupported %d + "
		     "deduped %d | collision: %d files carried bounds, %d seen = %d primitives + %d meshes "
		     "+ %d unmapped + %d malformed, %d tris, %d polys dropped"),
		Imported, Skipped, Failed, GeosDropped, GeoErrors, GeosNoUV, TrisOOR, TrisDegen,
		Bound, Unsupported, MissingTex, UnmappedSamp,
		AmbTotal, Scoped, FromEmbedded, FromArchTxd, FromParentTxd, FromYtyp, FromSlot, TieBroken,
		TbEmbedded, TbNoScope, TbSlotAmb, TbYtypAmb, TbDictAbsent, TbNotInScope,
		FilesWithScope, FilesWithHashTxd, FilesWithParentChain, FilesWithYtypSet, FilesWithSlot,
		NoShaderDef, NoMaterial, ValSeen, ValBound, ValUnsupported, ValDeduped,
		FilesWithCollision, ColSeen, ColPrims, ColMeshes, ColUnmapped, ColMalformed, ColTris,
		ColPolysDropped);
	// ⛔ `ok` IS COMPUTED, NEVER HARDCODED (fixed 2026-08-05). It used to be the literal `true`, so a
	// batch in which EVERY file failed returned `"ok":true,"failed":400` and the commandlet exited 0.
	// Measured, not theorised: a shell-quoting bug produced 400 non-existent paths, and no automated
	// check could tell that run from a clean one - the CLI exit code is derived from `ok`, so the one
	// gate a headless caller has was blind to total failure. Two conditions, both load-bearing:
	//   Failed == 0            - any failure is a failure; `failedFiles` already names them.
	//   Imported + Skipped > 0 - a run that did NOTHING is not a success. An all-skipped batch
	//                            (nothing to re-import outside Mode=FORCE) IS legitimate, which is
	//                            why Skipped counts as work done and Imported alone does not.
	// Same law as `quarry regress --strict`: a gate that cannot fail is worse than no gate.
	// See ENGINEERING_LOG "MEASUREMENT LAWS".
	const bool bOk = (Failed == 0) && (Imported + Skipped > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"imported\":%d,\"skipped\":%d,\"failed\":%d,")
		TEXT("\"geometriesDropped\":%d,\"filesWithGeometryErrors\":%d,\"geometriesWithoutUV\":%d,")
		TEXT("\"trianglesOutOfRange\":%d,\"trianglesDegenerate\":%d,\"boundTextures\":%d,")
		TEXT("\"texturesFromEmbedded\":%d,\"texturesResolvedScoped\":%d,\"texturesTieBroken\":%d,")
		TEXT("\"filesWithArchetypeTxd\":%d,\"ambiguousTextures\":%d,\"texturesAmbiguousTotal\":%d,")
		TEXT("\"texturesFromArchetypeTxd\":%d,\"texturesFromParentTxd\":%d,")
		TEXT("\"texturesFromYtypNeighbour\":%d,\"texturesFromSameSlot\":%d,")
		TEXT("\"texturesScopedAuthoritative\":%d,\"texturesScopedProvenance\":%d,")
		TEXT("\"tieBreakEmbeddedNotImported\":%d,\"tieBreakNoScope\":%d,")
		TEXT("\"tieBreakSlotAmbiguous\":%d,\"tieBreakYtypAmbiguous\":%d,")
		TEXT("\"tieBreakScopeDictAbsent\":%d,\"tieBreakNameNotInScope\":%d,")
		TEXT("\"filesWithParentChain\":%d,\"filesWithHashTxd\":%d,\"filesWithYtypSet\":%d,")
		TEXT("\"filesWithSlot\":%d,\"gtxdFiles\":%d,\"gtxdRelationships\":%d,\"gtxdRefused\":%d,")
		TEXT("\"resolvedEntries\":%d,")
		TEXT("\"unsupportedByMaster\":%d,\"missingTextures\":%d,\"unmappedSamplers\":%d,")
		TEXT("\"slotsWithoutShaderDef\":%d,\"slotsWithoutMaterial\":%d,")
		TEXT("\"valueParamsSeen\":%d,\"valueParamsBound\":%d,\"valueParamsUnsupported\":%d,")
		TEXT("\"valueParamsDeduped\":%d,\"filesWithCollision\":%d,\"collisionBoundsSeen\":%d,")
		TEXT("\"collisionPrimitivesImported\":%d,\"collisionMeshesImported\":%d,")
		TEXT("\"collisionBoundsUnmapped\":%d,\"collisionBoundsMalformed\":%d,")
		TEXT("\"collisionPolysDropped\":%d,\"collisionTriangles\":%d,\"failedFiles\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		Imported, Skipped, Failed, GeosDropped, GeoErrors, GeosNoUV, TrisOOR, TrisDegen,
		Bound, FromEmbedded, Scoped, TieBroken, FilesWithScope, Ambiguous, AmbTotal,
		FromArchTxd, FromParentTxd, FromYtyp, FromSlot, ScopedAuth, ScopedProv,
		TbEmbedded, TbNoScope, TbSlotAmb, TbYtypAmb, TbDictAbsent, TbNotInScope,
		FilesWithParentChain, FilesWithHashTxd, FilesWithYtypSet, FilesWithSlot,
		ScopeIndex.GtxdFiles, ScopeIndex.GtxdRelationships, ScopeIndex.GtxdRefusals,
		ScopeIndex.ResolvedEntries,
		Unsupported, MissingTex, UnmappedSamp,
		NoShaderDef, NoMaterial, ValSeen, ValBound, ValUnsupported, ValDeduped,
		FilesWithCollision, ColSeen, ColPrims, ColMeshes, ColUnmapped, ColMalformed,
		ColPolysDropped, ColTris, *FailedFiles);
}

// ⛔⛔ WHY THIS WRAPPER EXISTS — `-unattended` SILENTLY CANCELS EVERY SAVE (measured 2026-07-31,
// root cause read out of the engine source, not guessed).
// `FEditorFileUtils::SaveDirtyPackages` → `InternalSavePackages` → `PromptForCheckoutAndSave`,
// which begins (FileHelpers.cpp:4659-4667):
//     if (GIsRunningUnattendedScript) { return UEditorLoadingAndSavingUtils::SavePackages(...); }
//     if (FApp::IsUnattended() && !bAlreadyCheckedOut) { return PR_Cancelled; }
// A commandlet/`-ExecCmds` run sets `FApp::IsUnattended()` but NOT `GIsRunningUnattendedScript`
// (that flag belongs to scripted automation), so the save fell into the SECOND branch: cancelled,
// nothing written, `ok:false`, and the whole chain's work lost with a false-looking summary.
// The engine's own escape hatch is the first branch — it guards with exactly this TGuardValue when
// it needs a modal-free save (FileHelpers.cpp:5919). Setting it ONLY while unattended keeps the
// interactive path (checkout prompts, source control) untouched for a human at the editor.
// ⛔ AND IN A COMMANDLET THERE IS NO SLATE AT ALL (2026-09-05): `SaveDirtyPackages` reaches into
// the Slate application for its notifications and asserted `CurrentBaseApplication.IsValid()`
// the moment the CLI tried to persist 1,954 freshly imported dictionaries. Headless, every dirty
// content package is saved directly through UPackage::SavePackage - no prompt, no notification,
// no Slate - and the count of what was written is what the caller gets.
int32 GRudeLastSaved = 0, GRudeLastSaveFailed = 0;
bool RudeSaveDirty(bool bMaps, bool bContent)   // declared in RudeToolsetInternal.h
{
	GRudeLastSaved = 0; GRudeLastSaveFailed = 0;
	if (!FSlateApplication::IsInitialized())
	{
		TArray<UPackage*> Dirty;
		if (bContent) { FEditorFileUtils::GetDirtyContentPackages(Dirty); }
		if (bMaps) { FEditorFileUtils::GetDirtyWorldPackages(Dirty); }
		for (UPackage* Pkg : Dirty)
		{
			if (!Pkg) { continue; }
			const bool bIsMap = UWorld::FindWorldInPackage(Pkg) != nullptr;
			const FString Ext = bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), Filename, Ext)) { ++GRudeLastSaveFailed; continue; }
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			Args.Error = GWarn;
			// A FORCED re-import builds a NEW in-memory package over a file that already exists on
			// disk, and the raw save answers Canceled (measured 2026-09-05: 6,888 canceled, every
			// one an existing file; 3,032 new files saved). Set the old file aside, save, then
			// drop the old copy - and put it back if the save fails, so a failure costs nothing.
			IFileManager& FM = IFileManager::Get();
			const FString Aside = Filename + TEXT(".rude_prev");
			const bool bExisted = FM.FileExists(*Filename);
			if (bExisted) { FM.Delete(*Aside, false, true, true); FM.Move(*Aside, *Filename, true, true, true, true); }
			const FSavePackageResultStruct R = UPackage::Save(Pkg, nullptr, *Filename, Args);
			if (R == ESavePackageResult::Success)
			{
				++GRudeLastSaved;
				if (bExisted) { FM.Delete(*Aside, false, true, true); }
			}
			else
			{
				++GRudeLastSaveFailed;
				if (bExisted) { FM.Move(*Filename, *Aside, true, true, true, true); }
				UE_LOG(LogTemp, Warning, TEXT("[RUDE] save FAILED %s -> %s (result %d%s)"), *Pkg->GetName(), *Filename,
					(int32)R.Result, bExisted ? TEXT(", existing file restored") : TEXT(""));
			}
		}
		return GRudeLastSaveFailed == 0;
	}
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript,
		FApp::IsUnattended() ? true : GIsRunningUnattendedScript);
	return FEditorFileUtils::SaveDirtyPackages(
		/*bPromptUserToSave*/ false, bMaps, bContent, /*bFastSave*/ false,
		/*bNotifyNoPackagesSaved*/ false, /*bCanBeDeclined*/ false);
}

FString URudeToolset::SaveAssets()
{
	// ⛔ THE COMPILE-BEFORE-SAVE LAW, ENFORCED HERE (it was documented but wired NOWHERE - the
	// BUILD_AREA_DESIGN grounded catch): saving while async texture/mesh builds are in flight is
	// exactly the 381-asset bulkdata corruption incident. Block until every compilation settles,
	// THEN save. This is the single choke point every agent chain saves through.
	FAssetCompilingManager::Get().FinishAllCompilation();
	// Content packages only (bSaveMapPackages=false) - an agent persisting its imports must not
	// silently commit the operator's level edits.
	const bool bOk = RudeSaveDirty(/*bMaps*/ false, /*bContent*/ true);
	return FString::Printf(TEXT("{\"ok\":%s,\"saved\":%d,\"saveFailed\":%d,\"headless\":%s,\"unattended\":%s}"),
		bOk ? TEXT("true") : TEXT("false"), GRudeLastSaved, GRudeLastSaveFailed,
		FSlateApplication::IsInitialized() ? TEXT("false") : TEXT("true"),
		FApp::IsUnattended() ? TEXT("true") : TEXT("false"));
}

// ---- RegenerateMasters (agent) ------------------------------------------------------------
// Walk the generated master library (/RUDE/Masters/Gen/M_RUDE_<sig>_b<bucket>) and run each through
// the generator, which regenerates a stale one in place. Verdict: names touched.
FString URudeToolset::RegenerateMasters()
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	ARM.Get().ScanPathsSynchronous({ TEXT("/RUDE/Masters/Gen") }, true);
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByPath(FName(TEXT("/RUDE/Masters/Gen")), Assets, false);
	int32 Seen = 0, Regenerated = 0, Unparsed = 0;
	FString Names;
	for (const FAssetData& AD : Assets)
	{
		const FString N = AD.AssetName.ToString();
		if (!N.StartsWith(TEXT("M_RUDE_"))) { continue; }
		++Seen;
		// parse "M_RUDE_<D[N][S][Dt][T][E]>_b<N>"
		FString Sig, BucketStr;
		if (!N.Mid(7).Split(TEXT("_b"), &Sig, &BucketStr) || !Sig.StartsWith(TEXT("D"))) { ++Unparsed; continue; }
		FRudeMasterSpec Spec;
		Spec.Bucket = FCString::Atoi(*BucketStr);
		FString Rest = Sig.Mid(1);
		while (!Rest.IsEmpty())
		{
			if (Rest.StartsWith(TEXT("Dt"))) { Spec.bDetail = true; Rest = Rest.Mid(2); }
			else if (Rest.StartsWith(TEXT("N"))) { Spec.bNormal = true; Rest = Rest.Mid(1); }
			else if (Rest.StartsWith(TEXT("S"))) { Spec.bSpec = true; Rest = Rest.Mid(1); }
			else if (Rest.StartsWith(TEXT("T"))) { Spec.bTint = true; Rest = Rest.Mid(1); }
			else if (Rest.StartsWith(TEXT("E"))) { Spec.bEmissive = true; Rest = Rest.Mid(1); }
			else { break; }
		}
		if (!Rest.IsEmpty() || Spec.Key() != N) { ++Unparsed; continue; }
		UMaterialInterface* Before = LoadObject<UMaterialInterface>(nullptr, *(AD.PackageName.ToString() + TEXT(".") + N));
		const bool bWasDirty = Before && Before->GetOutermost()->IsDirty();
		UMaterialInterface* After = EnsureGeneratedMaster(Spec);
		if (After && After->GetOutermost()->IsDirty() && !bWasDirty)
		{
			++Regenerated;
			Names += FString::Printf(TEXT("%s\"%s\""), Names.IsEmpty() ? TEXT("") : TEXT(","), *N);
		}
	}
	return FString::Printf(TEXT("{\"ok\":%s,\"masters\":%d,\"regenerated\":%d,\"unparsed\":%d,\"names\":[%s]}"),
		Seen > 0 ? TEXT("true") : TEXT("false"), Seen, Regenerated, Unparsed, *Names);
}

FString URudeToolset::SetWorldHour(const FString& Hour)
{
	// ⭐ THE DAY/NIGHT DATASET, DRIVEN (2026-07-30, Matt corrected the model that produced this).
	// GTA does not fade lit windows in a shader - it ships 3,936 CTimeArchetypeDef whose `timeFlags`
	// is a 24-bit mask, bit N meaning "visible during hour N". The common masks are night windows
	// (hours 0-5 + 20-23). ImportScene groups every gated archetype into its own ISM component
	// tagged RUDE_TIME:<mask>, so setting the hour is a visibility sweep over exactly those
	// components and nothing else.
	//
	// ⛔ WHY NOT A SHADER GATE: I first multiplied emissive by a global NightFactor. It looked
	// right and was wrong - a UE-only invention that cannot round-trip to GTA, and round-trip is
	// one of the only two places fidelity actually matters here. The mask is the game's own data;
	// driving it keeps import and export talking about the same thing.
	const FString H = Hour.TrimStartAndEnd();
	if (H.IsEmpty() || !H.IsNumeric())
	{
		return TEXT("{\"ok\":false,\"error\":\"Hour must be 0-23\"}");
	}
	const int32 Hr = FCString::Atoi(*H);
	if (Hr < 0 || Hr > 23)
	{
		return TEXT("{\"ok\":false,\"error\":\"Hour must be 0-23\"}");
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return TEXT("{\"ok\":false,\"error\":\"no editor world\"}"); }

	const uint32 Bit = 1u << Hr;
	int32 Gated = 0, Shown = 0, Hidden = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<UInstancedStaticMeshComponent*> Comps;
		It->GetComponents<UInstancedStaticMeshComponent>(Comps);
		for (UInstancedStaticMeshComponent* C : Comps)
		{
			for (const FName& Tag : C->ComponentTags)
			{
				FString T = Tag.ToString();
				if (!T.StartsWith(TEXT("RUDE_TIME:"))) { continue; }
				T.RightChopInline(10);
				const uint32 Mask = (uint32)FCString::Strtoui64(*T, nullptr, 10);
				const bool bVisible = (Mask & Bit) != 0;
				C->SetVisibility(bVisible, /*bPropagateToChildren*/ true);
				C->SetHiddenInGame(!bVisible);
				++Gated;
				bVisible ? ++Shown : ++Hidden;
				break;
			}
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] SetWorldHour %02d:00 - %d gated components, %d shown, %d hidden"),
		Hr, Gated, Shown, Hidden);
	return FString::Printf(
		TEXT("{\"ok\":true,\"hour\":%d,\"gatedComponents\":%d,\"shown\":%d,\"hidden\":%d}"),
		Hr, Gated, Shown, Hidden);
}

FString URudeToolset::FixLevelRefs(const FString& Mode)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return TEXT("{\"ok\":false,\"error\":\"no editor world\"}");
	}
	const bool bApply = Mode.TrimStartAndEnd().Equals(TEXT("APPLY"), ESearchCase::IgnoreCase);

	// Collect first, mutate second - RemoveStreamingLevel edits the array we would be walking.
	TArray<ULevelStreaming*> Dangling;
	FString Names;
	const TArray<ULevelStreaming*>& Streaming = World->GetStreamingLevels();
	const int32 Checked = Streaming.Num();
	for (ULevelStreaming* Level : Streaming)
	{
		if (!Level)
		{
			continue;
		}
		const FString PackageName = Level->GetWorldAssetPackageName();
		// DoesPackageExist is the authority here, not the asset registry: a package deleted while
		// the editor was open can still sit in the registry's cache, which is exactly the state
		// that produces the load error.
		if (PackageName.IsEmpty() || !FPackageName::DoesPackageExist(PackageName))
		{
			Dangling.Add(Level);
			Names += FString::Printf(TEXT("%s\"%s\""), Names.IsEmpty() ? TEXT("") : TEXT(","),
			                         *PackageName);
		}
	}

	// ⭐ AND THE OTHER KIND, which is the one that actually bit (2026-07-29): a Level Instance is
	// an ACTOR holding a soft world-asset pointer, not an entry in the streaming array. Delete the
	// level package and the persistent map still spawns an ALevelInstance pointing nowhere - it
	// reports the same "Failed to find streamed level ..." text, so the message alone does not
	// tell you which of the two you have. Checking only the streaming array reported
	// "checked:0, dangling:0" on a map that was visibly broken. Check both, always.
	TArray<ALevelInstance*> DanglingLI;
	for (TActorIterator<ALevelInstance> It(World); It; ++It)
	{
		ALevelInstance* LI = *It;
		if (!LI) { continue; }
		const FString Pkg = LI->GetWorldAssetPackage();
		if (Pkg.IsEmpty() || !FPackageName::DoesPackageExist(Pkg))
		{
			DanglingLI.Add(LI);
			Names += FString::Printf(TEXT("%s\"%s (LevelInstance)\""),
			                         Names.IsEmpty() ? TEXT("") : TEXT(","), *Pkg);
		}
	}

	// ⭐⭐ AND THE THIRD KIND, which is the one that was ACTUALLY broken (2026-07-29). On a WORLD
	// PARTITION map every actor is its own external package and is NOT LOADED at startup, so
	// TActorIterator sees none of them: both checks above returned a confident "0 dangling" for a
	// map that threw "Failed to find streamed level" on every open. A check that cannot see the
	// broken thing is worse than no check - it reports healthy.
	// The asset registry knows the dependency graph WITHOUT loading anything, so ask it: does any
	// external actor package of this world depend on a /Game package that no longer exists? That
	// is the dangling reference, found headlessly and by name.
	// ⛔ Do NOT try to answer this by grepping the .umap - an object path is not stored as plain
	// text there, and that assumption is what produced this broken state to begin with.
	TArray<FString> DanglingActorPkgs;
	{
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		const FString ExtPath = ULevel::GetExternalActorsPath(World->GetPackage()->GetName());
		if (!ExtPath.IsEmpty())
		{
			AR.ScanPathsSynchronous({ ExtPath }, /*bForceRescan*/ true);
			if (AR.IsLoadingAssets()) { AR.WaitForCompletion(); }
			TArray<FAssetData> ActorAssets;
			AR.GetAssetsByPath(FName(*ExtPath), ActorAssets, /*bRecursive*/ true);
			for (const FAssetData& AD : ActorAssets)
			{
				TArray<FName> Deps;
				AR.GetDependencies(AD.PackageName, Deps,
				                   UE::AssetRegistry::EDependencyCategory::Package);
				for (const FName& Dep : Deps)
				{
					const FString DepStr = Dep.ToString();
					if (!DepStr.StartsWith(TEXT("/Game/"))) { continue; }
					if (FPackageName::DoesPackageExist(DepStr)) { continue; }
					DanglingActorPkgs.AddUnique(AD.PackageName.ToString());
					Names += FString::Printf(TEXT("%s\"%s -> MISSING %s\""),
					                         Names.IsEmpty() ? TEXT("") : TEXT(","),
					                         *AD.PackageName.ToString(), *DepStr);
				}
			}
		}
	}

	// ⭐⭐ AND THE PLACE I NEVER LOOKED - which is where it actually was (2026-07-30, reproduced by
	// Matt on Lvl_ThirdPerson while all three checks above reported clean).
	// The MAP PACKAGE ITSELF depends on the missing levels. Asking the registry
	// GetDependencies(<world package>) listed /Game/RUDE/Areas/DowntownHL3, HL4 and HL5 directly -
	// not via any external actor. So the reference lives in the world's own saved package, which is
	// why the streaming array was empty, no LevelInstance actor was loaded, and the external-actor
	// sweep found nothing. Three checks, all looking past the obvious one.
	// ⚠ A stale import like this is dropped by RE-SAVING the map, because nothing live holds it.
	// That is the repair, and APPLY verifies it afterwards rather than assuming.
	TArray<FString> DanglingMapDeps;
	{
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		const FName WorldPkg(*World->GetPackage()->GetName());
		TArray<FName> Deps;
		AR.GetDependencies(WorldPkg, Deps, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Dep : Deps)
		{
			const FString D = Dep.ToString();
			if (!D.StartsWith(TEXT("/Game/"))) { continue; }
			if (FPackageName::DoesPackageExist(D)) { continue; }
			DanglingMapDeps.Add(D);
			Names += FString::Printf(TEXT("%s\"MAP DEPENDS ON MISSING %s\""),
			                         Names.IsEmpty() ? TEXT("") : TEXT(","), *D);
		}
	}

	int32 Removed = 0;
	bool bSaved = false;
	bool bMapDepsCleared = false;
	if (bApply && DanglingMapDeps.Num() > 0)
	{
		// Re-save the map so the stale imports are rewritten away, then RE-ASK the registry. The
		// verification is the point: if the dependency survives, something live still holds it and
		// this repair does not apply - say so instead of reporting success.
		// ⛔ THROUGH RudeSaveDirty (2026-08-01). This was a RAW SaveDirtyPackages call and it is the
		// third save site the headless-cancel bug hid in: under `-unattended` the save silently did
		// nothing, so the re-save that IS the repair never happened and the tool honestly reported
		// mapDepsCleared=false. The verification was doing its job - the repair was not.
		World->MarkPackageDirty();
		const bool bRepairSaved = RudeSaveDirty(/*bMaps*/ true, /*bContent*/ false);
		UE_LOG(LogTemp, Display, TEXT("[RUDE] FixLevelRefs: repair re-save %s"),
			bRepairSaved ? TEXT("OK") : TEXT("FAILED"));
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		const FString PkgName = World->GetPackage()->GetName();
		AR.ScanModifiedAssetFiles({ PkgName });
		TArray<FName> After;
		AR.GetDependencies(FName(*PkgName), After, UE::AssetRegistry::EDependencyCategory::Package);
		bMapDepsCleared = true;
		for (const FName& Dep : After)
		{
			const FString D = Dep.ToString();
			if (D.StartsWith(TEXT("/Game/")) && !FPackageName::DoesPackageExist(D))
			{
				bMapDepsCleared = false;
				break;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("[RUDE] FixLevelRefs: map had %d dangling dependencies; "
			"after re-save cleared=%s"), DanglingMapDeps.Num(),
			bMapDepsCleared ? TEXT("true") : TEXT("false"));
	}
	if (bApply && DanglingActorPkgs.Num() > 0)
	{
		// The external actor package IS the actor. Its target is gone and cannot be restored, so
		// deleting the package is the repair - and it works while the actor is UNLOADED, which is
		// the whole reason this goes through the registry instead of the actor iterator.
		for (const FString& Pkg : DanglingActorPkgs)
		{
			FString Filename;
			if (FPackageName::DoesPackageExist(Pkg, &Filename)
				&& IFileManager::Get().Delete(*Filename))
			{
				++Removed;
			}
		}
	}
	if (bApply && (Dangling.Num() > 0 || DanglingLI.Num() > 0))
	{
		for (ULevelStreaming* Level : Dangling)
		{
			World->RemoveStreamingLevel(Level);
			++Removed;
		}
		for (ALevelInstance* LI : DanglingLI)
		{
			// The actor is the only thing holding the broken pointer - with its target gone there
			// is nothing to repair it to, so removing it IS the repair.
			World->EditorDestroyActor(LI, /*bShouldModifyLevel*/ true);
			++Removed;
		}
		World->MarkPackageDirty();
		// Maps ONLY here - repairing the map package is this tool's entire purpose, and it is the
		// one thing SaveAssets deliberately refuses to touch. Through RudeSaveDirty, so an
		// unattended APPLY actually writes instead of being cancelled (see its comment).
		bSaved = RudeSaveDirty(/*bMaps*/ true, /*bContent*/ false);
	}
	UE_LOG(LogTemp, Display,
	       TEXT("[RUDE] FixLevelRefs: %d streaming levels (%d dangling), %d dangling level "
	            "instances, %d removed"),
	       Checked, Dangling.Num(), DanglingLI.Num(), Removed);
	return FString::Printf(
		TEXT("{\"ok\":true,\"checked\":%d,\"dangling\":%d,\"danglingLevelInstances\":%d,")
		TEXT("\"danglingMapDependencies\":%d,\"mapDepsCleared\":%s,")
		TEXT("\"removed\":%d,\"saved\":%s,\"names\":[%s]}"),
		Checked, Dangling.Num(), DanglingLI.Num(), DanglingMapDeps.Num(),
		bMapDepsCleared ? TEXT("true") : TEXT("false"), Removed,
		bSaved ? TEXT("true") : TEXT("false"), *Names);
}

FString URudeToolset::ImportYtdBatch(const FString& ListPath, const FString& DestFolder,
                                     const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *ListPath))
	{
		return Fail(TEXT("cannot read list file"));
	}
	const bool bForce = Mode.TrimStartAndEnd().Equals(TEXT("FORCE"), ESearchCase::IgnoreCase);
	int32 Imported = 0, Textures = 0, Skipped = 0, Failed = 0;
	// ⛔ THIS BATCH PARSED EXACTLY ONE FIELD OUT OF THE UNIT VERDICT ("imported") AND DROPPED THE
	// REST - including invalidNames and missingPixels, the two counters ImportYtd was given
	// precisely so a silent texture loss could never recur. On 2026-07-30 the unit reported
	// texturesImported=0 across 1,943 dictionaries and the batch said ok:true; the fix put the
	// counter on the UNIT and left the aggregator blind, so the same run today would still print
	// only a total that had gone to zero. MEASURED over the whole resolved corpus: 45 of 82,241
	// dictionaries carry a texture whose name is not a legal package segment (78 items), so
	// invalidNames is a small, sharp signal that must not be averaged into silence.
	int32 Declared = 0, InvalidNames = 0, MissingPixels = 0;
	int32 UsageDefaulted = 0, UsageUnknown = 0, ItemsWithoutName = 0;
	FString FailedFiles;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Path = Lines[i].TrimStartAndEnd();
		if (Path.IsEmpty()) { continue; }
		FString Txd = FPaths::GetBaseFilename(Path);
		Txd.RemoveFromEnd(TEXT(".ytd"));
		// The pixel folder is DERIVED: QUARRY writes "<stem>/" beside the XML and resolve
		// carries the sidecar with the winning copy - the pair is self-describing.
		const FString PixelFolder = FPaths::GetPath(Path) / Txd;
		// Skip-if-exists on the txd's CONTENT FOLDER on disk (assets inside are named per
		// texture, unknowable here). FORCE re-imports in place - the texture-refresh law says
		// fresh packages, and ImportYtd's own edit-in-place handling owns that concern.
		const FString ContentDir = FPackageName::LongPackageNameToFilename(DestFolder / Txd, TEXT(""));
		if (!bForce && IFileManager::Get().DirectoryExists(*ContentDir))
		{
			++Skipped;
			continue;
		}
		const FString R = ImportYtd(Path, PixelFolder, DestFolder);
		// ⚠ The counters are read on BOTH paths. A dictionary that failed still declared textures
		// and still rejected names, and dropping its numbers because its ok flipped is the same
		// class of blindness this block is fixing.
		Declared         += RudeSumField(R, TEXT("declared"));
		InvalidNames     += RudeSumField(R, TEXT("invalidNames"));
		MissingPixels    += RudeSumField(R, TEXT("missingPixelCount"));
		UsageDefaulted   += RudeSumField(R, TEXT("usageDefaulted"));
		UsageUnknown     += RudeSumField(R, TEXT("usageUnknown"));
		ItemsWithoutName += RudeSumField(R, TEXT("itemsWithoutName"));
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Imported;
			// accumulate the per-txd texture count from the tool's own verdict
			Textures += RudeSumField(R, TEXT("imported"));
		}
		else
		{
			++Failed;
			if (Failed <= 30)
			{
				FailedFiles += FString::Printf(TEXT("%s\"%s\""), FailedFiles.IsEmpty() ? TEXT("") : TEXT(","), *Txd);
			}
		}
		if ((i + 1) % 25 == 0)
		{
			UE_LOG(LogTemp, Display,
				TEXT("[RUDE] ImportYtdBatch %d/%d (ok %d, skip %d, fail %d | declared %d, tex %d, "
				     "invalidNames %d, missingPixels %d)"),
				i + 1, Lines.Num(), Imported, Skipped, Failed, Declared, Textures,
				InvalidNames, MissingPixels);
		}
		if ((i + 1) % 100 == 0)
		{
			// Textures are heavy; keep memory flat - KEEPFLAGS, never RF_NoFlags (the GC-sweep law)
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportYtdBatch DONE: %d dictionaries ok, %d skipped, %d failed | declared %d, "
		     "textures imported %d, invalidNames %d, missingPixels %d, usage defaulted %d, "
		     "usage outside DIFFUSE/NORMAL/SPECULAR %d, nameless items %d"),
		Imported, Skipped, Failed, Declared, Textures, InvalidNames, MissingPixels,
		UsageDefaulted, UsageUnknown, ItemsWithoutName);
	// ⛔ Computed, not hardcoded - same class as ImportYdrBatch (fixed 2026-08-05).
	// `missingPixels` deliberately does NOT gate `ok`: it is a CORPUS gap (a manifest whose PNGs were
	// pruned), not a tool failure, and #37 measures it as 41.4% availability corpus-wide - failing on
	// it would make every honest run red. `Failed` is the tool's own failure and does gate.
	const bool bOk = (Failed == 0) && (Imported + Skipped > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"imported\":%d,\"texturesImported\":%d,\"texturesDeclared\":%d,")
		TEXT("\"skipped\":%d,\"failed\":%d,\"invalidNames\":%d,\"missingPixels\":%d,")
		TEXT("\"usageDefaulted\":%d,\"usageUnknown\":%d,\"itemsWithoutName\":%d,")
		TEXT("\"failedFiles\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		Imported, Textures, Declared, Skipped, Failed, InvalidNames, MissingPixels,
		UsageDefaulted, UsageUnknown, ItemsWithoutName, *FailedFiles);
}

// One actor per entity: a StaticMeshComponent root (the drawable, or the proxy cube when the
// mesh is absent) plus a URudeEntityComponent filled from the manifest row. Folder RUDE_LS/<ymap>
// so the idempotent clear and the per-ymap grouping both work; label = archetype name.
AActor* RudeSpawnEntityActor(UWorld* World, const FString& YmapName, const TSharedPtr<FJsonObject>& Ent,
                             const FTransform& Xf, UStaticMesh* Mesh, bool bProxy, uint32 TimeMask)
{
	AActor* A = World->SpawnActor<AActor>();
	if (!A) { return nullptr; }
	UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(A, TEXT("Mesh"));
	SMC->SetStaticMesh(Mesh);
	SMC->SetMobility(EComponentMobility::Static);
	A->SetRootComponent(SMC);
	SMC->RegisterComponent();
	A->AddInstanceComponent(SMC);
	A->SetActorTransform(Xf);
	if (TimeMask != 0 && TimeMask != 0xFFFFFFu)
	{
		SMC->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_TIME:%u"), TimeMask)));
	}
	if (bProxy) { A->Tags.Add(FName(TEXT("RUDE_PROXY"))); }
	// LOD accounting: the game shows ONE level of a lineage at a time. Every level is PLACED (the
	// export needs them all) but only HD / ORPHANHD is VISIBLE by default; LOD and SLOD shells stay
	// hidden and tagged RUDE_LOD:<level> so SetLodView can switch the view (Matt, 2026-09-05: the
	// blue "glass tower" was an SLOD shell stacked on its HD building).
	{
		FString LodLv;
		Ent->TryGetStringField(TEXT("lodLevel"), LodLv);
		if (LodLv.IsEmpty()) { LodLv = TEXT("LODTYPES_DEPTH_HD"); }
		A->Tags.Add(FName(*(TEXT("RUDE_LOD:") + LodLv)));
		const bool bHd = LodLv == TEXT("LODTYPES_DEPTH_HD") || LodLv == TEXT("LODTYPES_DEPTH_ORPHANHD");
		double FlagsD = 0.0;
		Ent->TryGetNumberField(TEXT("flags"), FlagsD);
		const bool bReflectionOnly = (((uint32)FlagsD) & 0x02000000u) != 0;   // ONLY_RENDER_IN_REFLECTIONS
		if (bReflectionOnly) { A->Tags.Add(FName(TEXT("RUDE_REFLECTION_ONLY"))); }
		// Sanity: a mesh far bigger than its own definition claims is a mis-import (2026-09-05: a
		// 7 m cloth tarp came in as a 260 m sheet). Placed, tagged, hidden, counted - never shown as fact.
		bool bSuspect = false;
		{
			double BsR = 0.0;
			Ent->TryGetNumberField(TEXT("bsRadius"), BsR);
			if (!bProxy && BsR > 0.0 && Mesh)
			{
				const double MeshR = Mesh->GetBoundingBox().GetExtent().Size() / 100.0;   // metres
				if (MeshR > 3.0 * BsR + 5.0) { bSuspect = true; A->Tags.Add(FName(TEXT("RUDE_SUSPECT_BOUNDS"))); }
			}
		}
		if (!bHd || bReflectionOnly || bSuspect)
		{
			SMC->SetVisibility(false, true);
			SMC->SetHiddenInGame(true, true);
		}
	}
	URudeEntityComponent* R = NewObject<URudeEntityComponent>(A, TEXT("RudeEntity"));
	auto Str = [&Ent](const TCHAR* K) { FString V; Ent->TryGetStringField(K, V); return V; };
	auto Num = [&Ent](const TCHAR* K, double Def) { double V = Def; Ent->TryGetNumberField(K, V); return V; };
	R->ArchetypeName = Str(TEXT("archetype"));
	R->SourceYmap = Str(TEXT("srcYmap"));
	R->SourceSlot = Str(TEXT("srcSlot"));
	R->SourceIndex = (int32)Num(TEXT("srcIndex"), -1.0);
	R->LodDist = (float)Num(TEXT("lodDist"), 0.0);
	R->ChildLodDist = (float)Num(TEXT("childLodDist"), 0.0);
	{
		const FString Lod = Str(TEXT("lodLevel"));
		if (!Lod.IsEmpty()) { R->LodLevel = Lod; }
	}
	R->ParentIndex = (int32)Num(TEXT("parentIndex"), -1.0);
	{
		const FString Pri = Str(TEXT("priorityLevel"));
		if (!Pri.IsEmpty()) { R->PriorityLevel = Pri; }
	}
	R->ExtensionsXml = Str(TEXT("extensions"));
	R->Flags = (uint32)Num(TEXT("flags"), 0.0);
	R->Guid = (uint32)Num(TEXT("guid"), 0.0);
	R->NumChildren = (int32)Num(TEXT("numChildren"), 0.0);
	R->AmbientOcclusionMultiplier = (float)Num(TEXT("aoMultiplier"), 255.0);
	R->ArtificialAmbientOcclusion = (float)Num(TEXT("artificialAo"), 255.0);
	R->TintValue = (uint32)Num(TEXT("tintValue"), 0.0);
	R->SourceXml = Str(TEXT("xml"));
	{
		const FString T = Str(TEXT("itemType"));
		if (!T.IsEmpty()) { R->ItemType = T; }
	}
	R->SourceTransform = Xf;
	R->SourceFieldsKey = R->FieldsKey();
	R->RegisterComponent();
	A->AddInstanceComponent(R);
	A->SetActorLabel(R->ArchetypeName.IsEmpty() ? YmapName : R->ArchetypeName);
	A->SetFolderPath(FName(*(TEXT("RUDE_LS/") + YmapName)));
	return A;
}

FString URudeToolset::ImportScene(const FString& ManifestPath, const FString& MeshFolder,
                                  const FString& Filter, const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *ManifestPath))
	{
		return Fail(TEXT("cannot read manifest"));
	}
	TArray<TSharedPtr<FJsonValue>> Scenes;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Scenes))
		{
			return Fail(TEXT("manifest is not a JSON array"));
		}
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return Fail(TEXT("no editor world"));
	}
	const bool bAll = Filter.TrimStartAndEnd().Equals(TEXT("ALL"), ESearchCase::IgnoreCase);
	// ACTORS = one actor per entity with its RUDE entity component (editable, exportable);
	// empty = the ISM display path (fast, not per-entity addressable).
	const bool bActors = Mode.TrimStartAndEnd().Equals(TEXT("ACTORS"), ESearchCase::IgnoreCase);
	int32 NumActors = 0;

	// Idempotent respawn: clear any previous RUDE_LS spawn first (re-running the tool
	// REPLACES the scene instead of stacking duplicates).
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			// RUDE_LS itself (ISM mode) and RUDE_LS/<ymap> (ACTORS mode) are both this tool's.
			const FString Folder = It->GetFolderPath().ToString();
			if (Folder == TEXT("RUDE_LS") || Folder.StartsWith(TEXT("RUDE_LS/"))) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}

	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TMap<FString, UStaticMesh*> MeshCache;      // lowercase drawable -> mesh (nullptr = known-missing)
	TMap<FString, int32> Missing;               // drawable/archetype -> proxy instance count
	int32 NumYmaps = 0, NumEntities = 0, NumInstances = 0, NumProxies = 0;
	// ⛔ "entities" WAS THE POST-FILTER NUMBER, AND THE FILTER WAS SILENT. ++NumEntities fires
	// AFTER both `continue`s below, so a caller could not tell a manifest that is 12% LOD content
	// from a manifest whose entities failed to parse - and "entities" reads as "the entities in
	// this manifest". MEASURED over 1,500 resolved ymap / 239,662 entities: 25,185 (10.51%) are
	// dropped by the default HD filter (LOD 22,745, SLOD1 2,274, SLOD2 112, SLOD3 37), and 17
	// carry the unresolvable string hash_6F5D45B3 as their lodLevel - neither HD nor a known LOD
	// tier, so they get their own bucket rather than being folded into either.
	// ⚠ An EMPTY lodLevel is KEPT by the predicate below (treated as HD). 0 corpus entities lack
	// the element today; the count is here so that if one ever does, the assumption is visible
	// instead of silently spawning something the game would not have drawn.
	int32 EntitiesInManifest = 0, FilteredByLod = 0, UnknownLodLevel = 0, EmptyLodLevel = 0;
	int32 MalformedEntities = 0, UniqueMeshLookups = 0;

	for (const TSharedPtr<FJsonValue>& SceneVal : Scenes)
	{
		const TSharedPtr<FJsonObject>* SceneObj;
		if (!SceneVal.IsValid() || !SceneVal->TryGetObject(SceneObj)) { continue; }
		const TArray<TSharedPtr<FJsonValue>>* Entities;
		if (!(*SceneObj)->TryGetArrayField(TEXT("entities"), Entities)) { continue; }
		const FString YmapName = (*SceneObj)->GetStringField(TEXT("ymap"));

		AActor* Actor = nullptr;
		USceneComponent* Root = nullptr;
		TMap<FString, UInstancedStaticMeshComponent*> IsmByMesh;   // key: mesh name or "proxy:<name>"
		if (bActors && Entities->Num() > 0) { ++NumYmaps; }   // ACTORS mode has no per-ymap parent actor

		// ⭐ The key carries the HOUR MASK as well as the mesh, so entities that appear only at
		// certain hours land in their OWN component. Visibility is a per-component switch in UE,
		// so grouping by mask is what makes the game's dataset drivable at all - mixing a
		// night-only archetype into a shared component would force per-instance work for something
		// the data expresses per archetype.
		auto GetIsm = [&](const FString& Key, UStaticMesh* Mesh, uint32 TimeMask = 0)
			-> UInstancedStaticMeshComponent*
		{
			if (UInstancedStaticMeshComponent** Found = IsmByMesh.Find(Key)) { return *Found; }
			if (!Actor)
			{
				Actor = World->SpawnActor<AActor>();
				if (!Actor) { return nullptr; }
				Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
				Actor->SetRootComponent(Root);
				Root->SetMobility(EComponentMobility::Static);
				Root->RegisterComponent();
				Actor->AddInstanceComponent(Root);
				Actor->SetActorLabel(YmapName);
				Actor->SetFolderPath(FName(TEXT("RUDE_LS")));
				++NumYmaps;
			}
			UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(
				Actor, FName(*FString::Printf(TEXT("ISM_%d"), IsmByMesh.Num())));
			Ism->SetStaticMesh(Mesh);
			Ism->SetMobility(EComponentMobility::Static);
			Ism->SetupAttachment(Root);
			Ism->RegisterComponent();
			// ALWAYS_VISIBLE (0xFFFFFF) and "no mask" need no tag - tagging only what is genuinely
			// gated keeps SetWorldHour's sweep proportional to the gated set, not the whole city.
			if (TimeMask != 0 && TimeMask != 0xFFFFFFu)
			{
				Ism->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_TIME:%u"), TimeMask)));
			}
			Actor->AddInstanceComponent(Ism);
			IsmByMesh.Add(Key, Ism);
			return Ism;
		};

		for (const TSharedPtr<FJsonValue>& EntVal : *Entities)
		{
			const TSharedPtr<FJsonObject>* Ent;
			if (!EntVal.IsValid() || !EntVal->TryGetObject(Ent)) { ++MalformedEntities; continue; }
			++EntitiesInManifest;
			{
				FString Lod;
				(*Ent)->TryGetStringField(TEXT("lodLevel"), Lod);
				const bool bHd = Lod.IsEmpty()
					|| Lod == TEXT("LODTYPES_DEPTH_HD") || Lod == TEXT("LODTYPES_DEPTH_ORPHANHD");
				if (Lod.IsEmpty()) { ++EmptyLodLevel; }
				else if (!bHd && !Lod.StartsWith(TEXT("LODTYPES_DEPTH_"))) { ++UnknownLodLevel; }
				if (!bAll && !bHd)
				{
					++FilteredByLod;
					continue;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Loc;
			const TArray<TSharedPtr<FJsonValue>>* Quat;
			if (!(*Ent)->TryGetArrayField(TEXT("ue_location"), Loc) || Loc->Num() != 3 ||
			    !(*Ent)->TryGetArrayField(TEXT("ue_quat"), Quat) || Quat->Num() != 4)
			{
				++MalformedEntities;
				continue;
			}
			++NumEntities;
			const double SXY = (*Ent)->HasField(TEXT("scaleXY")) ? (*Ent)->GetNumberField(TEXT("scaleXY")) : 1.0;
			const double SZ = (*Ent)->HasField(TEXT("scaleZ")) ? (*Ent)->GetNumberField(TEXT("scaleZ")) : 1.0;
			FQuat Q((*Quat)[0]->AsNumber(), (*Quat)[1]->AsNumber(), (*Quat)[2]->AsNumber(), (*Quat)[3]->AsNumber());
			Q.Normalize();
			const FTransform Xf(Q,
				FVector((*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber()),
				FVector(SXY, SXY, SZ));

			FString Drawable;
			(*Ent)->TryGetStringField(TEXT("drawable"), Drawable);   // null for unresolved archetypes
			Drawable.ToLowerInline();
			UStaticMesh* Mesh = nullptr;
			if (!Drawable.IsEmpty())
			{
				if (UStaticMesh** Cached = MeshCache.Find(Drawable)) { Mesh = *Cached; }
				else
				{
					Mesh = LoadObject<UStaticMesh>(nullptr, *(MeshFolder / Drawable));
					MeshCache.Add(Drawable, Mesh);
					// ⛔ MeshCache.Num() was reported as "uniqueMeshes". The cache DELIBERATELY
					// stores nullptr for a known-missing drawable (its own comment says so), so
					// every drawable whose asset failed to load inflated the figure that reads as
					// "how many distinct meshes this scene placed". Count the ones that LOADED;
					// keep the lookup total too, because their difference is exactly the missing
					// set and that is worth having as a number rather than a subtraction.
					++UniqueMeshLookups;
				}
			}
			if (bActors)
			{
				uint32 TimeMaskA = 0;
				{
					int32 TF = 0;
					if ((*Ent)->TryGetNumberField(TEXT("timeFlags"), TF) && TF > 0) { TimeMaskA = (uint32)TF; }
				}
				if (!Mesh)
				{
					const FString Tag = Drawable.IsEmpty() ? (*Ent)->GetStringField(TEXT("archetype")) : Drawable;
					Missing.FindOrAdd(Tag)++;
				}
				if (Mesh || ProxyCube)
				{
					if (RudeSpawnEntityActor(World, YmapName, *Ent, Xf, Mesh ? Mesh : ProxyCube, Mesh == nullptr, TimeMaskA))
					{
						++NumActors;
						if (Mesh) { ++NumInstances; } else { ++NumProxies; }
					}
				}
				continue;
			}
			if (Mesh)
			{
				uint32 TimeMask = 0;
				{
					int32 TF = 0;
					if ((*Ent)->TryGetNumberField(TEXT("timeFlags"), TF) && TF > 0)
					{
						TimeMask = (uint32)TF;
					}
				}
				const FString IsmKey = TimeMask ? FString::Printf(TEXT("%s#t%u"), *Drawable, TimeMask)
				                                : Drawable;
				if (UInstancedStaticMeshComponent* Ism = GetIsm(IsmKey, Mesh, TimeMask))
				{
					Ism->AddInstance(Xf, /*bWorldSpace*/ true);
					++NumInstances;
				}
			}
			else
			{
				// ⛔ THE TALLY MOVED OUT OF THE `if (ProxyCube)` BRANCH. An entity with no mesh was
				// only recorded as missing when the /Engine/BasicShapes/Cube proxy happened to
				// load; if that LoadObject ever failed, every unresolved entity in the scene
				// vanished from missingMeshes and topMissing as well as from the viewport, and the
				// verdict read like a clean import. What is missing is a property of the DATA, not
				// of whether the placeholder was available.
				const FString Tag = Drawable.IsEmpty() ? (*Ent)->GetStringField(TEXT("archetype")) : Drawable;
				Missing.FindOrAdd(Tag)++;
				if (ProxyCube)
				{
					if (UInstancedStaticMeshComponent* Ism = GetIsm(TEXT("proxy"), ProxyCube))
					{
						Ism->AddInstance(Xf, /*bWorldSpace*/ true);
						++NumProxies;
					}
				}
			}
		}
	}
	World->MarkPackageDirty();

	Missing.ValueSort(TGreater<int32>());
	FString TopMissing;
	int32 Shown = 0;
	for (const TPair<FString, int32>& M : Missing)
	{
		if (++Shown > 20) { break; }
		TopMissing += FString::Printf(TEXT("%s\"%s x%d\""), Shown > 1 ? TEXT(",") : TEXT(""), *M.Key, M.Value);
	}
	int32 UniqueMeshes = 0;
	for (const TPair<FString, UStaticMesh*>& M : MeshCache)
	{
		if (M.Value) { ++UniqueMeshes; }
	}
	// unknownLodLevel is a FLAG, not a partition: an entity with an unrecognisable tier is also
	// counted in filteredByLod (it is not HD, so the default filter drops it). The partition that
	// closes is entitiesInManifest == entities + filteredByLod + malformedEntities.
	// ⛔ `ok` IS COMPUTED, NEVER HARDCODED (fixed 2026-08-05, open item #44 - the same class as
	// ImportYdrBatch/ImportYtdBatch/ExportYdrBinaryBatch in 65248c2, but this one needed a SEMANTICS
	// decision first because it counts two failures that mean opposite things. #42's rule was
	// deliberately NOT copied here; what follows is that decision, and it is MINE - the agent's -
	// not Matt's. He was never asked and never ruled on it.
	//
	// This function is the SPAWN. ImportMapArea forwards its verdict through
	// `bSpawnOk = !Spawn.Contains("\"ok\":false")`, so whatever is decided here is also the map
	// lane's gate; getting it wrong reddens or greens the whole import tool.
	//
	//   malformedEntities > 0  ->  FALSE. An entity the tool could not parse - not a JSON object at
	//     all, or missing/short ue_location or ue_quat - is the tool meeting input it does not
	//     understand, and this codebase REFUSES rather than defaults (the same rule that made
	//     ExportYmap reject an empty <archetypeName> instead of placing it at the origin, #23).
	//     The entity is DROPPED: it is in entitiesInManifest and in nothing else, so the scene is
	//     silently short by exactly that many placements. MEASURED shape of the risk: 0 of 33,973
	//     entities in the emitted corpus are malformed today (INTERCHANGE_CONTRACT re-census,
	//     2026-08-05), so this gate costs nothing on healthy data and fires only when the ymap
	//     emitter or the manifest writer has actually broken - which is precisely when a headless
	//     run must stop instead of scoring 0.
	//
	//   missingMeshes  ->  DOES NOT GATE. It is a CORPUS gap, not a tool failure: the drawable was
	//     never converted, or its lane was never extracted. #37 measures corpus texture/mesh
	//     availability at 41.4% overall and wildly per lane (ydr 14.2% unavailable, ydd 63.3%,
	//     yft 74.6%), so gating on it would make EVERY honest run red, and a gate that fires on
	//     every run is a gate nobody reads - the same reasoning that keeps missingPixels out of
	//     ImportYtdBatch's ok and meshesMissingFromCorpus out of ImportMapArea's. The number stays
	//     loud (missingMeshes + topMissing), which is where a corpus gap belongs.
	//
	//   ZERO WORK  ->  FALSE. `ymaps` counts ymaps that actually produced an actor and `entities`
	//     the placements that survived the LOD filter; if either is 0 nothing was placed, and a
	//     manifest that spawned an empty level must not report success. PRESENCE IS NOT COVERAGE -
	//     a gate that cannot fail is worse than no gate (ENGINEERING_LOG "MEASUREMENT LAWS").
	//     Note this also catches the case a filter typo produces: every entity filtered out by LOD,
	//     nothing spawned, previously ok:true.
	const bool bOk = (MalformedEntities == 0) && (NumYmaps > 0) && (NumEntities > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ymaps\":%d,\"entitiesInManifest\":%d,\"entities\":%d,")
		TEXT("\"filteredByLod\":%d,\"unknownLodLevel\":%d,\"emptyLodLevel\":%d,")
		TEXT("\"malformedEntities\":%d,\"instances\":%d,\"proxies\":%d,")
		TEXT("\"uniqueMeshes\":%d,\"uniqueMeshLookups\":%d,\"missingMeshes\":%d,\"topMissing\":[%s],")
		TEXT("\"mode\":\"%s\",\"actors\":%d}"),
		bOk ? TEXT("true") : TEXT("false"),
		NumYmaps, EntitiesInManifest, NumEntities, FilteredByLod, UnknownLodLevel, EmptyLodLevel,
		MalformedEntities, NumInstances, NumProxies,
		UniqueMeshes, UniqueMeshLookups, Missing.Num(), *TopMissing,
		bActors ? TEXT("ACTORS") : TEXT("ISM"), NumActors);
}
