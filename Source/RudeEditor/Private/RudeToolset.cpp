// RUDE - RAGE <-> Unreal Development Environment
#include "RudeToolset.h"

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
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "XmlFile.h"
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
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, *Full))
	{
		return Existing;
	}
	UPackage* P = CreatePackage(*PkgName);
	if (!P) { return nullptr; }
	UMaterial* M = NewObject<UMaterial>(P, *Name, RF_Public | RF_Standalone);

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
		EO->Opacity.Expression = DiffuseTex;
		EO->Opacity.MaskA = 1; EO->Opacity.Mask = 1;
		EO->Opacity.MaskR = 0; EO->Opacity.MaskG = 0; EO->Opacity.MaskB = 0;
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
		TArray<FString> Toks;
		VData->GetContent().ParseIntoArrayWS(Toks);
		if (Toks.Num() % LineWidth != 0)
		{
			Error = FString::Printf(TEXT("vertex stream misaligned: %d tokens %% %d width = %d - layout mismatch"),
				Toks.Num(), LineWidth, Toks.Num() % LineWidth);
			return false;
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
				const int32 W = SemanticWidth(Sem);
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
	FString Missing;
	for (const FXmlNode* Item : Root->GetChildrenNodes())
	{
		const FXmlNode* NameNode = Item->FindChildNode(TEXT("Name"));
		if (!NameNode || NameNode->GetContent().TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		const FString TexName = NameNode->GetContent().TrimStartAndEnd();
		const FXmlNode* UsageNode = Item->FindChildNode(TEXT("Usage"));
		const FString Usage = UsageNode ? UsageNode->GetContent().TrimStartAndEnd() : TEXT("DIFFUSE");

		// decoded pixels (offline BC-decode bridge until native decode lands)
		const FString PngPath = PixelFolder / (TexName + TEXT(".png"));
		TArray<uint8> PngBytes;
		if (!FFileHelper::LoadFileToArray(PngBytes, *PngPath))
		{
			Missing += FString::Printf(TEXT("%s\"%s\""), Missing.IsEmpty() ? TEXT("") : TEXT(","), *TexName);
			continue;
		}
		TSharedPtr<IImageWrapper> Png = ImageWrapper.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetCompressed(PngBytes.GetData(), PngBytes.Num()))
		{
			Missing += FString::Printf(TEXT("%s\"%s\""), Missing.IsEmpty() ? TEXT("") : TEXT(","), *TexName);
			continue;
		}
		TArray<uint8> BGRA;
		if (!Png->GetRaw(ERGBFormat::BGRA, 8, BGRA))
		{
			Missing += FString::Printf(TEXT("%s\"%s\""), Missing.IsEmpty() ? TEXT("") : TEXT(","), *TexName);
			continue;
		}
		const int32 W = Png->GetWidth();
		const int32 H = Png->GetHeight();

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

	return FString::Printf(
		TEXT("{\"ok\":true,\"txd\":\"%s\",\"imported\":%d,\"invalidNames\":%d,")
		TEXT("\"missingPixels\":[%s]}"),
		*TxdName, Imported, InvalidNames, *Missing);
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
		TArray<FVector3f> Pos;
		TArray<FVector3f> Nrm;
		TArray<FVector2f> UV;
		TArray<int32> Indices;
	};
	TArray<FOutGeo> OutGeos;

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
			}
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
		Xml += TEXT("    <RenderBucket value=\"0\" />\n    <Parameters>\n");
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
	return FString::Printf(
		TEXT("{\"ok\":true,\"xmlPath\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"triangles\":%d}"),
		*OutXmlPath, OutGeos.Num(), TotalVerts, TotalTris);
}

FString URudeToolset::Ping()
{
	return TEXT("RUDE 0.1.0 - RAGE <-> Unreal Development Environment. Toolset alive.");
}

// Build a UStaticMesh asset (plus its per-slot MaterialInstances) from ONE drawable-shaped
// XML node - the body every import lane shares. DrawableRoot may be a standalone <Drawable>
// root or a <Fragment>'s inner <Drawable> (both via ImportYdr), or a <DrawableDictionary>
// <Item> (ImportYddEntry) - anything carrying ShaderGroup + DrawableModelsHigh children.
// MeshName is the ASSET name, decided by the CALLER (file stem, <Name>, or dictionary entry).
static FString ImportDrawableNode(const FXmlNode* DrawableRoot, const FString& MeshName,
                                  const FString& DestFolder);

FString URudeToolset::ImportYdr(const FString& XmlPath, const FString& DestFolder)
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
		Root = Root->FindChildNode(TEXT("Drawable"));
		bFragment = true;
	}
	if (!Root || Root->GetTag() != TEXT("Drawable"))
	{
		return Fail(TEXT("root is not <Drawable> (or <Fragment> wrapping one)"));
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

	return ImportDrawableNode(Root, Name, DestFolder);
}

static FString ImportDrawableNode(const FXmlNode* DrawableRoot, const FString& MeshName,
                                  const FString& DestFolder)
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
				continue;
			}
			if (I0 == I1 || I1 == I2 || I0 == I2)
			{
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
	// Texture lookup table: every UTexture2D under /Game/RUDE/Textures by lowercase name
	TMap<FString, FAssetData> TextureByName;
	{
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		// ⛔⛔ WAIT FOR THE REGISTRY, OR THIS QUERY LIES. GetAssets answers from whatever has been
		// scanned SO FAR - it does not block. Driven from -ExecCmds at editor startup (which is how
		// every agent/CI run drives RUDE) the initial scan is still in flight, so this returns few
		// or NO textures, TextureByName comes back empty, and every FindTexture misses. The import
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
			TextureByName.Add(AD.AssetName.ToString().ToLower(), AD);
		}
		// An empty texture library is never normal for a project that has imported any ytd. Say so
		// once, loudly, rather than letting thousands of silent misses look like missing source data.
		UE_LOG(LogTemp, Display, TEXT("[RUDE] texture library: %d textures visible under "
			"/Game/RUDE/Textures"), TextureByName.Num());
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

	auto FindTexture = [&TextureByName](const FString& TexName) -> UTexture2D*
	{
		if (TexName.IsEmpty())
		{
			return nullptr;
		}
		if (const FAssetData* AD = TextureByName.Find(TexName.ToLower()))
		{
			return Cast<UTexture2D>(AD->GetAsset());
		}
		return nullptr;
	};

	int32 BoundTextures = 0;
	// ⛔ THE COUNTER THAT CANNOT LIE. UMaterialInstance::SetTextureParameterValueInternal does NO
	// validation against the parent's parameter set (Engine/Private/Materials/MaterialInstance.cpp
	// ~4309): binding "Normal" onto a master that HAS no Normal succeeds, persists in the uasset,
	// shows in the MI editor - and renders nothing. So a rising "boundTextures" proves nothing.
	// UnsupportedByMaster counts exactly those silent no-ops; a non-zero value is a real defect.
	int32 ValueParamsBound = 0;      // value param the master DOES expose, so it took effect
	int32 ValueParamsUnsupported = 0;// value param arrived but no master parameter accepts it
	int32 UnsupportedByMaster = 0;   // sampler mapped, but the MASTER has no such parameter
	int32 MissingTextures = 0;       // XML named a texture that is not imported in this project
	int32 UnmappedSamplers = 0;      // a sampler name with no entry in GSamplerBinds (see below)

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
		FString ConfigKey;
		if (Def)
		{
			ConfigKey = Def->Preset + TEXT("|") + Def->Diffuse + TEXT("|") + Def->Normal + TEXT("|") + Def->Specular;
			if (bTerrain)
			{
				for (int32 li = 0; li < 4; ++li)
				{
					const FString* T = Def->AllTex.Find(FString::Printf(TEXT("TextureSampler_layer%d"), li));
					ConfigKey += TEXT("|") + (T ? *T : FString());
				}
			}
			ConfigKey.ToLowerInline();
		}
		if (Master && MIByConfig.Contains(ConfigKey))
		{
			SlotMaterial = MIByConfig[ConfigKey];
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
				if (MasterParams.Contains(FName(TEXT("Detail"))))
				{
					const FString* DetailTex = Def->AllTex.Find(TEXT("DetailSampler"));
					const bool bDetail = DetailTex && FindTexture(*DetailTex) != nullptr;
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("DetailAmount")), bDetail ? 1.f : 0.f);
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
				MIByConfig.Add(ConfigKey, MIC);
			}
		}

		FStaticMaterial Mat(SlotMaterial, FName(*Slot));
		Mat.UVChannelData.bInitialized = true;
		Mesh->GetStaticMaterials().Add(Mat);
	}

	Mesh->Build(true);
	// PIE-walkable collision: map meshes use their render triangles as collision
	// (complex-as-simple) - RAGE collision stays a separate lane (ybn/embedded bounds).
	if (!Mesh->GetBodySetup()) { Mesh->CreateBodySetup(); }
	if (UBodySetup* BS = Mesh->GetBodySetup())
	{
		BS->CollisionTraceFlag = CTF_UseComplexAsSimple;
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
	// ⭐ Report the VALUE params that arrived, split three ways: valueParamsSeen (the data reached
	// the engine), valueParamsBound (a master consumed it - the generated masters + the binding
	// loop above do bind spec/bump/detail/emissive/tint now), valueParamsUnsupported (arrived but
	// the routed master has no such parameter - a COUNTED no-op, because SetScalarParameterValue
	// validates nothing and a silent one is invisible). Seen >> bound is a coverage measurement,
	// not an error; silence on any of the three would look identical to QUARRY dropping the data,
	// which is exactly the confusion that cost a day.
	int32 ValueParams = 0;
	for (const FShaderDef& D : Shaders) { ValueParams += D.Values.Num(); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"assetPath\":\"%s\",\"geometries\":%d,\"geometriesDropped\":%d,")
		TEXT("\"geometryErrors\":[%s],\"vertices\":%d,\"triangles\":%d,")
		TEXT("\"boundTextures\":%d,\"unsupportedByMaster\":%d,\"missingTextures\":%d,")
		TEXT("\"unmappedSamplers\":%d,\"valueParamsSeen\":%d,\"valueParamsBound\":%d,")
		TEXT("\"valueParamsUnsupported\":%d,\"slots\":[%s]}"),
		*PackageName, Geos.Num(), GeosFailed, *GeoErrors, TotalVerts, TotalTris, BoundTextures,
		UnsupportedByMaster, MissingTextures, UnmappedSamplers, ValueParams,
		ValueParamsBound, ValueParamsUnsupported, *SlotsJson);
}

// Jenkins one-at-a-time over the LOWERCASED name - RAGE's name hash, pinned to QUARRY's
// convention (quarry/ngcrypto.py joaat: lowercase input; the unresolvable-name fallback is
// spelled "hash_%08X", UPPERCASE hex). ymap<->ytyp<->dictionary joins are hash-to-hash, so
// matching by hash is the join's native form, not a workaround.
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

FString URudeToolset::ImportYddEntry(const FString& XmlPath, const FString& EntryName,
                                     const FString& DestFolder)
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
	return ImportDrawableNode(Found, Entry, DestFolder);
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
	TArray<FString> YtypFiles;
	IFileManager::Get().FindFiles(YtypFiles, *(CorpusRoot / TEXT("ytyp") / TEXT("*.xml")), true, false);
	for (const FString& F : YtypFiles)
	{
		FXmlFile Xml(CorpusRoot / TEXT("ytyp") / F);
		if (!Xml.IsValid()) { continue; }
		const FXmlNode* Root = Xml.GetRootNode();
		const FXmlNode* Arche = Root ? Root->FindChildNode(TEXT("archetypes")) : nullptr;
		if (!Arche) { continue; }
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
						MloSearch->FoundFile = CorpusRoot / TEXT("ytyp") / F;
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
			if (bDictArch)
			{
				const FXmlNode* DictN = Item->FindChildNode(TEXT("drawableDictionary"));
				const FString Dict = DictN ? DictN->GetContent().TrimStartAndEnd().ToLower() : FString();
				if (Dict.IsEmpty()) { continue; }   // nothing to resolve against -> proxy cube
				// the manifest "drawable" stays the ENTRY (=archetype) name, so ImportScene's
				// name-based mesh lookup works unchanged
				Out.ArchToAsset.Add(ArchLower, ArchLower);
				Out.DictEntries.Add(ArchLower, Dict);
				continue;
			}
			const FString AssetLower = AssetN->GetContent().TrimStartAndEnd().ToLower();
			Out.ArchToAsset.Add(ArchLower, AssetLower);
			if (bFragmentArch) { Out.FragmentAssets.Add(AssetLower); }
		}
	}
	if (Out.ArchToAsset.Num() == 0)
	{
		Error = TEXT("no archetypes indexed - check CorpusRoot/ytyp");
		return false;
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] archetype index: %d (fragment %d, dictionary-entry %d)"),
		Out.ArchToAsset.Num(), Out.FragmentAssets.Num(), Out.DictEntries.Num());
	return true;
}

// Resolve ONE indexed drawable to its corpus XML and import it (skip-if-exists) - dictionary
// entries live INSIDE their dict's ydd XML (one file, many drawables); plain drawables and
// fragments stay one-file-per-asset under ydr/ and yft/. The skip check runs on the ENTRY
// mesh name for all three lanes.
static void ImportIndexedDrawable(const FString& CorpusRoot, const FRudeArchetypeIndex& Index,
                                  const FString& Drawable, const FString& DestMeshFolder,
                                  int32& MeshOk, int32& MeshSkip, int32& MeshFail, int32& MeshMissing,
                                  bool bForce = false)
{
	const FString* Dict = Index.DictEntries.Find(Drawable);
	const bool bFrag = !Dict && Index.FragmentAssets.Contains(Drawable);
	const FString XmlPath = Dict
		? CorpusRoot / TEXT("ydd") / (*Dict + TEXT(".ydd.xml"))
		: CorpusRoot / (bFrag ? TEXT("yft") : TEXT("ydr"))
			/ (Drawable + (bFrag ? TEXT(".yft.xml") : TEXT(".ydr.xml")));
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
	const FString R = Dict ? URudeToolset::ImportYddEntry(XmlPath, Drawable, DestMeshFolder)
	                       : URudeToolset::ImportYdr(XmlPath, DestMeshFolder);
	if (R.Contains(TEXT("\"ok\":true"))) { ++MeshOk; } else { ++MeshFail; }
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
	for (FString P : Prefixes)
	{
		P.TrimStartAndEndInline();
		if (P.IsEmpty()) { continue; }
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(CorpusRoot / TEXT("ymap") / (P + TEXT("*.xml"))), true, false);
		for (const FString& F : Found)
		{
			// TSet::Add's out-param is bIsAlreadyInSet - true for DUPLICATES, not new adds
			bool bAlready = false;
			SeenYmap.Add(F, &bAlready);
			if (!bAlready) { YmapFiles.Add(F); }
		}
	}
	YmapFiles.Sort();
	if (YmapFiles.Num() == 0) { return Fail(TEXT("no ymaps match the prefix")); }
	int32 TotalEnts = 0, Resolved = 0;
	TSet<FString> NeededDrawables;
	FString ScenesJson;
	for (const FString& F : YmapFiles)
	{
		FXmlFile Xml(CorpusRoot / TEXT("ymap") / F);
		if (!Xml.IsValid()) { continue; }
		const FXmlNode* Root = Xml.GetRootNode();
		const FXmlNode* Ents = Root ? Root->FindChildNode(TEXT("entities")) : nullptr;
		if (!Ents) { continue; }
		FString EntJson;
		int32 SceneEnts = 0;
		for (const FXmlNode* E : Ents->GetChildrenNodes())
		{
			const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
			const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
			if (!AN || !Pos) { continue; }
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
			EntJson += FString::Printf(TEXT(
				"%s{\"archetype\":\"%s\",\"drawable\":%s,\"lodLevel\":\"%s\","
				"\"ue_location\":[%f,%f,%f],\"ue_quat\":[%f,%f,%f,%f],\"scaleXY\":%f,"
				"\"scaleZ\":%f,\"timeFlags\":%u}"),
				SceneEnts > 1 ? TEXT(",") : TEXT(""), *Arch,
				Asset ? *FString::Printf(TEXT("\"%s\""), **Asset) : TEXT("null"), *Lod,
				Px * 100.0, -Py * 100.0, Pz * 100.0,
				Qx, -Qy, Qz, Qw,
				Val(TEXT("scaleXY"), 1.0), Val(TEXT("scaleZ"), 1.0),
				TFlags ? *TFlags : 0u);
		}
		if (SceneEnts == 0) { continue; }
		FString YmapName = FPaths::GetBaseFilename(F);
		YmapName.RemoveFromEnd(TEXT(".ymap"));
		ScenesJson += FString::Printf(TEXT("%s{\"ymap\":\"%s\",\"entities\":[%s]}"),
			ScenesJson.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, *EntJson);
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
	for (const FString& D : NeededDrawables)
	{
		++Done;
		if (Index.DictEntries.Contains(D)) { ++DictNeeded; }
		ImportIndexedDrawable(CorpusRoot, Index, D, DestMeshFolder, MeshOk, MeshSkip, MeshFail,
		                      MeshMissing, bForceMeshes);
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
	const FString Spawn = ImportScene(ManifestPath, DestMeshFolder, LodFilter);
	return FString::Printf(TEXT(
		"{\"ok\":true,\"ymaps\":%d,\"entities\":%d,\"resolved\":%d,\"meshesImported\":%d,"
		"\"meshesSkipped\":%d,\"meshesFailed\":%d,\"meshesMissingFromCorpus\":%d,"
		"\"manifest\":\"%s\",\"spawn\":%s}"),
		YmapFiles.Num(), TotalEnts, Resolved, MeshOk, MeshSkip, MeshFail, MeshMissing,
		*ManifestPath, *Spawn);
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
			*Wanted, *Search.WantHashName, Search.MloSeen, *(CorpusRoot / TEXT("ytyp")), *Search.Sample));
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
	};
	TArray<FMloEntity> Ents;
	int32 OtherExtensions = 0, LightsSkipped = 0;
	FString LightProblem;

	if (const FXmlNode* EntsN = Mlo->FindChildNode(TEXT("entities")))
	{
		for (const FXmlNode* E : EntsN->GetChildrenNodes())
		{
			const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
			const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
			if (!AN || !Pos) { continue; }
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
		if (E.Room < 0 && E.Portal < 0) { ++Unroomed; }
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
	for (const FString& D : Needed)
	{
		++Done;
		ImportIndexedDrawable(CorpusRoot, Index, D, DestMeshFolder, MeshOk, MeshSkip, MeshFail, MeshMissing);
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
	return FString::Printf(TEXT(
		"{\"ok\":true,\"archetype\":\"%s\",\"requested\":\"%s\",\"ytyp\":\"%s\","
		"\"rooms\":%d,\"roomNames\":[%s],\"portals\":%d,\"portalRooms\":[%s],"
		"\"entitySets\":[%s],\"entities\":%d,\"spawned\":%d,\"proxies\":%d,"
		"\"unresolvedArchetypes\":%d,\"lights\":%d,\"lightsSkipped\":%d,%s"
		"\"otherExtensions\":%d,\"badAttachedRefs\":%d,\"unroomedEntities\":%d,"
		"\"meshesImported\":%d,\"meshesSkipped\":%d,\"meshesFailed\":%d,"
		"\"meshesMissingFromCorpus\":%d}"),
		*Search.FoundName, *Wanted, *FPaths::GetCleanFilename(Search.FoundFile),
		Rooms.Num(), *RoomNamesJson, Portals.Num(), *PortalsJson,
		*SetsJson, Ents.Num(), Spawned, Proxies,
		Unresolved, NumLights, LightsSkipped, *LightProblemJson,
		OtherExtensions, BadRefs, Unroomed,
		MeshOk, MeshSkip, MeshFail, MeshMissing);
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
		float Bsr = 0.f;
		if (const FXmlNode* R = Root->FindChildNode(TEXT("BoundingSphereRadius")))
		{
			Bsr = FCString::Atof(*R->GetAttribute(TEXT("value")));
		}
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
			Bsc[0], Bsc[1], Bsc[2], Bsr, LodDist, *Name, *Txd, *PhysDict, *Name);
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
	for (const TSharedPtr<FJsonValue>& V : Ents)
	{
		const TSharedPtr<FJsonObject>* E;
		if (!V.IsValid() || !V->TryGetObject(E)) { continue; }
		const TSharedPtr<FJsonObject>* Ue;
		if (!(*E)->TryGetObjectField(TEXT("ue"), Ue)) { continue; }
		const FString Arch = (*E)->GetStringField(TEXT("archetype"));
		const double X = (*Ue)->GetNumberField(TEXT("x")) / 100.0;
		const double Y = -(*Ue)->GetNumberField(TEXT("y")) / 100.0;
		const double Z = (*Ue)->GetNumberField(TEXT("z")) / 100.0;
		double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
		const TSharedPtr<FJsonObject>* Q;
		if ((*E)->TryGetObjectField(TEXT("ue_quat"), Q))
		{
			// EXPORT-lane involution (bench-pinned): gta_quat = (-x, y, -z, w)
			Qx = -(*Q)->GetNumberField(TEXT("x"));
			Qy = (*Q)->GetNumberField(TEXT("y"));
			Qz = -(*Q)->GetNumberField(TEXT("z"));
			Qw = (*Q)->GetNumberField(TEXT("w"));
		}
		MinX = FMath::Min(MinX, X); MinY = FMath::Min(MinY, Y); MinZ = FMath::Min(MinZ, Z);
		MaxX = FMath::Max(MaxX, X); MaxY = FMath::Max(MaxY, Y); MaxZ = FMath::Max(MaxZ, Z);
		const uint32 Guid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%s:%f:%f:%f"), *MapName, *Arch, X, Y, Z));
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
	int32 Imported = 0, Skipped = 0, Failed = 0;
	// ⛔ THE BATCH USED TO THROW THESE AWAY, and that is why "did the rebind work?" was
	// unanswerable after 4,956 files ran with ok:true (2026-07-29). Every file reported its own
	// texture verdict; the batch summed only ok/skip/fail, so a run that bound ZERO textures and a
	// run that bound all of them printed the identical line. A batch must aggregate the counters
	// its unit reports - a silent contributor has to be as loud as a failing one.
	int32 Bound = 0, Unsupported = 0, MissingTex = 0, UnmappedSamp = 0;
	int32 ValSeen = 0, ValBound = 0, ValUnsupported = 0;
	auto SumField = [](const FString& Json, const TCHAR* Key) -> int32
	{
		const FString Needle = FString::Printf(TEXT("\"%s\":"), Key);
		const int32 At = Json.Find(Needle);
		if (At == INDEX_NONE) { return 0; }
		return FCString::Atoi(*Json.Mid(At + Needle.Len()));
	};
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
		const FString R = ImportYdr(Path, DestFolder);
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Imported;
			Bound        += SumField(R, TEXT("boundTextures"));
			Unsupported  += SumField(R, TEXT("unsupportedByMaster"));
			MissingTex   += SumField(R, TEXT("missingTextures"));
			UnmappedSamp += SumField(R, TEXT("unmappedSamplers"));
			ValSeen        += SumField(R, TEXT("valueParamsSeen"));
			ValBound       += SumField(R, TEXT("valueParamsBound"));
			ValUnsupported += SumField(R, TEXT("valueParamsUnsupported"));
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
		TEXT("[RUDE] ImportYdrBatch DONE: %d imported, %d skipped, %d failed | textures bound %d, "
		     "unsupportedByMaster %d, missing %d, unmappedSamplers %d | value params seen %d, "
		     "bound %d, unsupported %d"),
		Imported, Skipped, Failed, Bound, Unsupported, MissingTex, UnmappedSamp,
		ValSeen, ValBound, ValUnsupported);
	return FString::Printf(
		TEXT("{\"ok\":true,\"imported\":%d,\"skipped\":%d,\"failed\":%d,\"boundTextures\":%d,")
		TEXT("\"unsupportedByMaster\":%d,\"missingTextures\":%d,\"unmappedSamplers\":%d,")
		TEXT("\"valueParamsSeen\":%d,\"valueParamsBound\":%d,\"valueParamsUnsupported\":%d,")
		TEXT("\"failedFiles\":[%s]}"),
		Imported, Skipped, Failed, Bound, Unsupported, MissingTex, UnmappedSamp,
		ValSeen, ValBound, ValUnsupported, *FailedFiles);
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
static bool RudeSaveDirty(bool bMaps, bool bContent)
{
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
	return FString::Printf(TEXT("{\"ok\":%s,\"unattended\":%s}"),
		bOk ? TEXT("true") : TEXT("false"),
		FApp::IsUnattended() ? TEXT("true") : TEXT("false"));
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
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Imported;
			// accumulate the per-txd texture count from the tool's own verdict
			const int32 At = R.Find(TEXT("\"imported\":"));
			if (At != INDEX_NONE)
			{
				Textures += FCString::Atoi(*R.Mid(At + 11));
			}
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
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportYtdBatch %d/%d (ok %d, skip %d, fail %d, tex %d)"),
				i + 1, Lines.Num(), Imported, Skipped, Failed, Textures);
		}
		if ((i + 1) % 100 == 0)
		{
			// Textures are heavy; keep memory flat - KEEPFLAGS, never RF_NoFlags (the GC-sweep law)
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"imported\":%d,\"texturesImported\":%d,\"skipped\":%d,\"failed\":%d,\"failedFiles\":[%s]}"),
		Imported, Textures, Skipped, Failed, *FailedFiles);
}

FString URudeToolset::ImportScene(const FString& ManifestPath, const FString& MeshFolder,
                                  const FString& Filter)
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

	// Idempotent respawn: clear any previous RUDE_LS spawn first (re-running the tool
	// REPLACES the scene instead of stacking duplicates).
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetFolderPath() == FName(TEXT("RUDE_LS"))) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}

	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TMap<FString, UStaticMesh*> MeshCache;      // lowercase drawable -> mesh (nullptr = known-missing)
	TMap<FString, int32> Missing;               // drawable/archetype -> proxy instance count
	int32 NumYmaps = 0, NumEntities = 0, NumInstances = 0, NumProxies = 0;

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
			if (!EntVal.IsValid() || !EntVal->TryGetObject(Ent)) { continue; }
			if (!bAll)
			{
				FString Lod;
				(*Ent)->TryGetStringField(TEXT("lodLevel"), Lod);
				if (!Lod.IsEmpty() && Lod != TEXT("LODTYPES_DEPTH_HD") && Lod != TEXT("LODTYPES_DEPTH_ORPHANHD"))
				{
					continue;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Loc;
			const TArray<TSharedPtr<FJsonValue>>* Quat;
			if (!(*Ent)->TryGetArrayField(TEXT("ue_location"), Loc) || Loc->Num() != 3 ||
			    !(*Ent)->TryGetArrayField(TEXT("ue_quat"), Quat) || Quat->Num() != 4)
			{
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
				}
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
			else if (ProxyCube)
			{
				const FString Tag = Drawable.IsEmpty() ? (*Ent)->GetStringField(TEXT("archetype")) : Drawable;
				Missing.FindOrAdd(Tag)++;
				if (UInstancedStaticMeshComponent* Ism = GetIsm(TEXT("proxy"), ProxyCube))
				{
					Ism->AddInstance(Xf, /*bWorldSpace*/ true);
					++NumProxies;
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
	return FString::Printf(
		TEXT("{\"ok\":true,\"ymaps\":%d,\"entities\":%d,\"instances\":%d,\"proxies\":%d,")
		TEXT("\"uniqueMeshes\":%d,\"missingMeshes\":%d,\"topMissing\":[%s]}"),
		NumYmaps, NumEntities, NumInstances, NumProxies, MeshCache.Num(), Missing.Num(), *TopMissing);
}

// ======================= RudeYdrBin - READING binary .ydr (RSC7 v165) =======================
// The import side's keystone: RUDE could WRITE binary but only READ the XML form, so QUARRY's
// output (real game binaries) could not reach the importer at all. This parses the container and
// the drawable graph into the SAME RudeYdr::FGeo intermediate the XML lane produces, so the proven
// mesh builder is reused rather than duplicated (today's crash #6 was a duplication bug - do not
// grow a second mesh builder).
// Struct map: ENGINEERING_LOG "ydr binary format". Everything here reads UNTRUSTED bytes from the
// user's own game files, so every access is bounds-checked and a malformed file must return an
// error, never crash the editor.
namespace RudeYdrBin
{
	struct FRes
	{
		TArray<uint8> Sys;      // system (virtual) segment
		TArray<uint8> Gfx;      // graphics (physical) segment - often empty (gfxFlags size 0)
		uint32 Version = 0;

		// A stored pointer is tagged: high nibble 5 = system, 6 = graphics; low 28 bits = offset.
		// Returns the segment and validates the whole [off, off+Need) span before any read.
		const TArray<uint8>* Resolve(uint32 Tagged, int32 Need, int32& OutOff) const
		{
			const uint32 Tag = Tagged >> 28;
			const int32 Off = (int32)(Tagged & 0x0FFFFFFFu);
			const TArray<uint8>* S = (Tag == 5) ? &Sys : ((Tag == 6) ? &Gfx : nullptr);
			if (!S || Off < 0 || Need < 0 || Off + Need > S->Num()) { return nullptr; }
			OutOff = Off;
			return S;
		}
	};

	static bool Rd(const TArray<uint8>& B, int32 Off, void* Dst, int32 N)
	{
		if (Off < 0 || N < 0 || Off + N > B.Num()) { return false; }
		FMemory::Memcpy(Dst, B.GetData() + Off, N);
		return true;
	}
	static bool U16(const TArray<uint8>& B, int32 O, uint16& V) { return Rd(B, O, &V, 2); }
	static bool U32(const TArray<uint8>& B, int32 O, uint32& V) { return Rd(B, O, &V, 4); }
	static bool F32(const TArray<uint8>& B, int32 O, float& V) { return Rd(B, O, &V, 4); }

	// Total bytes described by an RSC7 flag word's page plan. base = 0x200<<ss, class-k page =
	// base<<k, counts packed at fixed bit positions. Same scheme the writer's pager encodes.
	static uint32 SegSizeFromFlags(uint32 Flags)
	{
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		static const uint32 KMask[9] = { 1, 1, 1, 1, 0x7F, 0x3F, 0xF, 3, 1 };
		const uint32 F = Flags & 0x0FFFFFFFu;
		const uint32 Base = 0x200u << (F & 0xFu);
		uint32 Total = 0;
		for (int32 k = 0; k <= 8; ++k)
		{
			Total += ((F >> KBit[k]) & KMask[k]) * (Base << k);
		}
		return Total;
	}

	// RSC7 file -> inflated system + graphics segments.
	static bool LoadFile(const FString& Path, FRes& Out, FString& Error)
	{
		TArray<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *Path)) { Error = TEXT("cannot read file"); return false; }
		if (File.Num() < 16) { Error = TEXT("shorter than an RSC7 header"); return false; }
		if (File[0] != 'R' || File[1] != 'S' || File[2] != 'C' || File[3] != '7')
		{
			Error = TEXT("not an RSC7 container (no 'RSC7' magic)");
			return false;
		}
		uint32 Ver = 0, SysF = 0, GfxF = 0;
		U32(File, 4, Ver); U32(File, 8, SysF); U32(File, 12, GfxF);
		if (Ver != 165)
		{
			// v159 = the GTA V ENHANCED drawable; loading it into a Legacy pipeline is the
			// classic "Invalid fixup" report, so name the mismatch explicitly.
			Error = FString::Printf(TEXT("version %u is not a Legacy drawable (want v165%s)"), Ver,
				Ver == 159 ? TEXT("; v159 = GTA V Enhanced") : TEXT(""));
			return false;
		}
		const uint32 SysSize = SegSizeFromFlags(SysF);
		const uint32 GfxSize = SegSizeFromFlags(GfxF);
		if (SysSize == 0 || SysSize > (1u << 30)) { Error = TEXT("implausible system segment size"); return false; }

		// The body is headerless (raw) DEFLATE of [system | graphics].
		TArray<uint8> Blob;
		Blob.SetNumUninitialized((int32)(SysSize + GfxSize));
		int32 OutSize = Blob.Num();
		if (!FCompression::UncompressMemory(NAME_Zlib, Blob.GetData(), OutSize,
		                                   File.GetData() + 16, File.Num() - 16,
		                                   COMPRESS_NoFlags, -15))
		{
			Error = TEXT("raw-DEFLATE inflate failed (truncated, or an Oodle-packed resource)");
			return false;
		}
		Out.Version = Ver;
		Out.Sys.Append(Blob.GetData(), (int32)SysSize);
		if (GfxSize > 0) { Out.Gfx.Append(Blob.GetData() + SysSize, (int32)GfxSize); }
		return true;
	}

	// ---- VERTEX DECLARATION DECODE ----------------------------------------------------------
	// Derived empirically from all 3,479 real base-game v165 ydrs / 17,370 geometries (see
	// ENGINEERING_LOG "ydr vertex declaration"). 14 distinct declarations exist; our WRITER only
	// ever emits mask 0x59 / stride 36, which is just 43% of the corpus - so a reader that assumes
	// one layout silently misreads the other 57%.
	// LAW: channels are laid out in ASCENDING BIT-INDEX order; a channel's offset is the sum of the
	// sizes of all lower set bits. The grcFvf u64 is a full 16-slot TYPE TABLE indexed by channel
	// bit (nibble i = type of channel i), NOT packed per used channel - which is why it is the same
	// constant 0x7755555555996996 in 17,370/17,370 geometries.
	enum : int32 { CH_POS = 0, CH_BLENDW = 1, CH_BLENDI = 2, CH_NRM = 3, CH_COL0 = 4,
	               CH_COL1 = 5, CH_TC0 = 6, CH_TANGENT = 14, CH_BINORMAL = 15 };

	// nibble -> byte size. Only 5/6/7/9 occur in the map corpus; everything else is UNOBSERVED.
	// Peds/vehicles/DLC use half-float and packed types that are certainly among the rest, so an
	// unknown nibble must SKIP the geometry with a diagnosable message - never guess a size, or a
	// wrong stride scrambles every vertex after the first (the "deformed mesh" class).
	static int32 NibbleSize(uint8 N)
	{
		switch (N)
		{
			case 5: return 8;    // float2
			case 6: return 12;   // float3
			case 7: return 16;   // float4
			case 9: return 4;    // ubyte4 / D3DCOLOR
			default: return -1;  // NOT OBSERVED in the map corpus - refuse rather than guess
		}
	}

	struct FDecl
	{
		uint32 Mask = 0;
		int32 Stride = 0;
		int32 Ofs[16];    // -1 = channel absent
		FDecl() { for (int32 i = 0; i < 16; ++i) { Ofs[i] = -1; } }
		bool Has(int32 Ch) const { return Ch >= 0 && Ch < 16 && Ofs[Ch] >= 0; }
	};

	static bool BuildDecl(uint32 Mask, uint64 Nibbles, int32 DeclStride, FDecl& Out, FString& Error);

	// ---- SELF-VERIFICATION: the export gate, run on our own freshly-built bytes -----------------
	// Wired into ExportYdrBinary so it CANNOT ship a violation, rather than relying on someone
	// remembering to run an offline script. Uses the reader, so there is ONE implementation of the
	// law (duplication is what caused crash #6 in the first place).
	// Checks, all of which correspond to a real in-game crash we paid for:
	//   #6  SINGLE OWNERSHIP - RAGE's fixup rewrites a pointer slot in place and is NOT idempotent,
	//       so a block reached from 2 owners is resolved twice -> "address is neither virtual nor
	//       physical". Counted by IN-DEGREE; the two sanctioned aliases (hdr+0xa0 == hdr+0x50, and
	//       vertex data reached via VB+0x20 / grmGeometry+0x78) are simply not counted as edges.
	//   ①  geoBounds must be N+1 pairs (union first) when N>1, and grmModel+0x2e == geometry count.
	//   decl - every emitted vertex declaration must decode to exactly the declared stride.
	struct FVerify
	{
		int32 SharedBlocks = 0;
		int32 DeclBad = 0;
		int32 BoundsBad = 0;
		FString FirstProblem;
	};

	static void Note(FVerify& V, const FString& What)
	{
		if (V.FirstProblem.IsEmpty()) { V.FirstProblem = What; }
	}

	// Records an owner for a target; the SECOND owner of any target is a violation.
	static void Own(TMap<int32, int32>& InDeg, FVerify& V, uint32 Tagged, const FString& Label)
	{
		if ((Tagged >> 28) != 5) { return; }              // only system-segment blocks here
		const int32 Off = (int32)(Tagged & 0x0FFFFFFFu);
		if (Off == 0) { return; }
		int32& N = InDeg.FindOrAdd(Off);
		if (++N > 1)
		{
			++V.SharedBlocks;
			Note(V, FString::Printf(TEXT("block @0x%x has %d owners (%s) - non-idempotent fixup "
			                             "will resolve it %d times"), Off, N, *Label, N));
		}
	}

	// Verify a drawable we just built, in place, before it is compressed and written.
	static FVerify VerifyDrawable(const TArray<uint8>& Sys)
	{
		FVerify V;
		TMap<int32, int32> InDeg;
		auto P = [&Sys](int32 O) -> uint32 { uint32 X = 0; U32(Sys, O, X); return X; };
		auto Deref = [&Sys](uint32 T, int32 Need, int32& O) -> bool
		{
			if ((T >> 28) != 5) { return false; }
			O = (int32)(T & 0x0FFFFFFFu);
			return O >= 0 && O + Need <= Sys.Num();
		};

		Own(InDeg, V, P(0x08), TEXT("hdr+0x08 blockmap"));
		Own(InDeg, V, P(0x10), TEXT("hdr+0x10 ShaderGroup"));
		Own(InDeg, V, P(0x50), TEXT("hdr+0x50 ModelsHigh"));
		Own(InDeg, V, P(0xa8), TEXT("hdr+0xa8 name"));
		Own(InDeg, V, P(0xc8), TEXT("hdr+0xc8 Bound"));
		// hdr+0xa0 intentionally NOT counted - byte-identical alias of +0x50 in 3,479/3,479 real files.

		// ShaderGroup -> shaders -> param tables -> texture stubs -> stub name strings
		int32 SG = 0;
		if (Deref(P(0x10), 0x40, SG))
		{
			Own(InDeg, V, P(SG + 0x10), TEXT("SG+0x10 shaderArr"));
			uint16 NSh = 0; U16(Sys, SG + 0x18, NSh);
			int32 Arr = 0;
			if (Deref(P(SG + 0x10), (int32)NSh * 8, Arr))
			{
				for (int32 si = 0; si < (int32)NSh; ++si)
				{
					Own(InDeg, V, P(Arr + si * 8), FString::Printf(TEXT("shaderArr[%d]"), si));
					int32 Blk = 0;
					if (!Deref(P(Arr + si * 8), 0x30, Blk)) { continue; }
					Own(InDeg, V, P(Blk + 0x00), FString::Printf(TEXT("shader%d paramTable"), si));
					uint32 NPar = 0; U32(Sys, Blk + 0x10, NPar);
					const int32 PC = (int32)(NPar & 0xFFFF);
					int32 Tbl = 0;
					if (PC <= 0 || PC > 64 || !Deref(P(Blk + 0x00), PC * 16, Tbl)) { continue; }
					for (int32 pi = 0; pi < PC; ++pi)
					{
						int32 Stub = 0;
						if (!Deref(P(Tbl + pi * 16 + 8), 0x34, Stub)) { continue; }
						uint32 Marker = 0; U32(Sys, Stub + 0x30, Marker);
						if (Marker != 0x00020001u) { continue; }   // inline vec4, not a texture stub
						Own(InDeg, V, P(Tbl + pi * 16 + 8), FString::Printf(TEXT("shader%d.param[%d] stub"), si, pi));
						Own(InDeg, V, P(Stub + 0x28), FString::Printf(TEXT("shader%d stub name"), si));
					}
				}
			}
		}

		// models -> grmModel -> geometries -> VB/IB/fvf/data
		int32 MH = 0;
		if (Deref(P(0x50), 0x10, MH))
		{
			Own(InDeg, V, P(MH + 0x00), TEXT("modelsHdr ptrArr"));
			uint16 NMod = 0; U16(Sys, MH + 0x08, NMod);
			int32 MArr = 0;
			if (Deref(P(MH + 0x00), (int32)NMod * 8, MArr))
			{
				for (int32 mi = 0; mi < (int32)NMod; ++mi)
				{
					Own(InDeg, V, P(MArr + mi * 8), FString::Printf(TEXT("modelArr[%d]"), mi));
					int32 M = 0;
					if (!Deref(P(MArr + mi * 8), 0x30, M)) { continue; }
					Own(InDeg, V, P(M + 0x08), TEXT("model geoArr"));
					Own(InDeg, V, P(M + 0x18), TEXT("model geoBounds"));
					Own(InDeg, V, P(M + 0x20), TEXT("model shaderMap"));
					uint16 NGeo = 0, NGeo2e = 0;
					U16(Sys, M + 0x10, NGeo);
					U16(Sys, M + 0x2e, NGeo2e);
					if (NGeo2e != NGeo)
					{
						++V.BoundsBad;
						Note(V, FString::Printf(TEXT("grmModel+0x2e is %u but geometry count is %u"), NGeo2e, NGeo));
					}
					// geoBounds: N+1 pairs (union first) when N>1, exactly 1 pair when N==1
					const int32 Pairs = (NGeo > 1) ? ((int32)NGeo + 1) : 1;
					int32 GB = 0;
					if (!Deref(P(M + 0x18), Pairs * 0x20, GB))
					{
						++V.BoundsBad;
						Note(V, FString::Printf(TEXT("geoBounds does not span %d pairs for %u geometries"),
						                        Pairs, NGeo));
					}
					int32 GArr = 0;
					if (!Deref(P(M + 0x08), (int32)NGeo * 8, GArr)) { continue; }
					for (int32 gi = 0; gi < (int32)NGeo; ++gi)
					{
						Own(InDeg, V, P(GArr + gi * 8), FString::Printf(TEXT("geoArr[%d]"), gi));
						int32 G = 0;
						if (!Deref(P(GArr + gi * 8), 0x80, G)) { continue; }
						Own(InDeg, V, P(G + 0x18), FString::Printf(TEXT("geo%d VB"), gi));
						Own(InDeg, V, P(G + 0x38), FString::Printf(TEXT("geo%d IB"), gi));
						// geo+0x78 NOT counted - sanctioned vertex-data alias
						int32 VB = 0;
						if (Deref(P(G + 0x18), 0x40, VB))
						{
							Own(InDeg, V, P(VB + 0x10), FString::Printf(TEXT("geo%d vertex data"), gi));
							Own(InDeg, V, P(VB + 0x30), FString::Printf(TEXT("geo%d grcFvf"), gi));
							// VB+0x20 NOT counted - sanctioned vertex-data alias
							uint16 Stride16 = 0; U16(Sys, G + 0x70, Stride16);
							uint32 Mask = 0; uint64 Nib = 0;
							int32 Fvf = 0;
							if (Deref(P(VB + 0x30), 0x10, Fvf))
							{
								U32(Sys, Fvf + 0x00, Mask);
								Rd(Sys, Fvf + 0x08, &Nib, 8);
								FDecl D; FString E;
								if (!BuildDecl(Mask, Nib, (int32)Stride16, D, E))
								{
									++V.DeclBad;
									Note(V, FString::Printf(TEXT("geo%d declaration: %s"), gi, *E));
								}
							}
						}
						int32 IB = 0;
						if (Deref(P(G + 0x38), 0x20, IB))
						{
							Own(InDeg, V, P(IB + 0x10), FString::Printf(TEXT("geo%d index data"), gi));
						}
					}
				}
			}
		}

		// embedded bound: composite -> children -> per-child arrays -> BVH nodes/trees
		int32 Comp = 0;
		if (Deref(P(0xc8), 0x80, Comp))
		{
			Own(InDeg, V, P(Comp + 0x70), TEXT("composite children array"));
			uint16 NCh = 0; U16(Sys, Comp + 0x78, NCh);
			int32 CArr = 0;
			if (Deref(P(Comp + 0x70), (int32)NCh * 8, CArr))
			{
				for (int32 ci = 0; ci < (int32)NCh; ++ci)
				{
					Own(InDeg, V, P(CArr + ci * 8), FString::Printf(TEXT("child[%d]"), ci));
					int32 Ch = 0;
					if (!Deref(P(CArr + ci * 8), 0x150, Ch)) { continue; }
					static const int32 ChildPtrs[5] = { 0x88, 0xb0, 0xf0, 0x118, 0x130 };
					for (int32 k = 0; k < 5; ++k)
					{
						Own(InDeg, V, P(Ch + ChildPtrs[k]),
						    FString::Printf(TEXT("child[%d]+0x%x"), ci, ChildPtrs[k]));
					}
					int32 Bvh = 0;
					if (Deref(P(Ch + 0x130), 0x80, Bvh))
					{
						Own(InDeg, V, P(Bvh + 0x00), FString::Printf(TEXT("child[%d] BVH nodes"), ci));
						Own(InDeg, V, P(Bvh + 0x70), FString::Printf(TEXT("child[%d] m_Trees"), ci));
					}
				}
			}
		}
		return V;
	}

	static bool BuildDecl(uint32 Mask, uint64 Nibbles, int32 DeclStride, FDecl& Out, FString& Error)
	{
		Out = FDecl();
		Out.Mask = Mask;
		int32 Off = 0;
		for (int32 Bit = 0; Bit < 16; ++Bit)
		{
			if (((Mask >> Bit) & 1u) == 0) { continue; }
			const uint8 Nb = (uint8)((Nibbles >> (Bit * 4)) & 0xFull);
			const int32 Sz = NibbleSize(Nb);
			if (Sz < 0)
			{
				Error = FString::Printf(
					TEXT("unsupported vertex channel type: mask 0x%x bit %d has nibble 0x%x "
					     "(only float2/3/4 and ubyte4 are derived from the map corpus)"), Mask, Bit, Nb);
				return false;
			}
			Out.Ofs[Bit] = Off;
			Off += Sz;
		}
		// The stride is declared in three places and agrees in 17,370/17,370, so a mismatch here
		// means the declaration is not one we understand - refuse instead of misaligning.
		if (Off != DeclStride)
		{
			Error = FString::Printf(TEXT("computed stride %d != declared %d for mask 0x%x"),
			                        Off, DeclStride, Mask);
			return false;
		}
		Out.Stride = Off;
		return true;
	}
}

// ======================= ExportYtdBinary - clean-room .ytd (RSC7 v13) =======================
// Reverse-engineered from our own CW-roundtripped diff pair + verified byte-identical
// (tools/write_ytd.py, docs/ENGINEERING_LOG "RSC7 binary container"). No CodeWalker code read.
// RSC7 header (16B: 'RSC7' | u32 version=13 | u32 sysFlags | u32 gfxFlags) + raw DEFLATE of
// [ system-segment | graphics-pages ]. System = pgDictionary<grcTexture>; graphics = pixel pages.
// Pointers are tagged fixups: 0x50000000|off -> system, 0x60000000|off -> graphics.
namespace RudeYtd
{
	// Jenkins one-at-a-time over the lowercased name (RAGE joaat). CONFIRMED against
	// the observed dictionary hashes (0x97f2c7c3 / 0x9a5d45aa).
	static uint32 Joaat(const FString& S)
	{
		uint32 H = 0;
		const FString L = S.ToLower();
		for (int32 i = 0; i < L.Len(); ++i)
		{
			H += (uint8)L[i];
			H += (H << 10);
			H ^= (H >> 6);
		}
		H += (H << 3);
		H ^= (H >> 11);
		H += (H << 15);
		return H;
	}

	// Low-28 RSC7 flag bits for a segment size: tile into power-of-two pages (largest
	// first, capped 4MB), base = smallest page / 16, class k holds pages of base*(1<<k).
	// Also returns the page count (for the blockmap). The segment-type high nibble
	// (system 0x0 / graphics 0xd, verified vs 400 real ytds) is OR'd in by the caller.
	static uint32 FlagsFromSize(uint32 Size, int32& OutPageCount)
	{
		const uint32 MAXPAGE = 0x400000;
		TArray<uint32> Pages;
		uint32 Rem = Size;
		while (Rem > 0)
		{
			uint32 P = MAXPAGE;
			while (P > Rem) { P >>= 1; }
			Pages.Add(P);
			Rem -= P;
		}
		OutPageCount = Pages.Num();
		uint32 Smallest = 0xFFFFFFFFu;
		for (uint32 P : Pages) { Smallest = FMath::Min(Smallest, P); }
		const uint32 Base = Smallest / 16;
		int32 ss = 0; { uint32 b = Base; while (b > 0x200) { b >>= 1; ++ss; } }
		static const int32 BitPos[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		int32 Counts[9] = { 0 };
		for (uint32 P : Pages)
		{
			int32 k = 0; uint32 r = P / Base; while (r > 1) { r >>= 1; ++k; }
			if (k >= 0 && k < 9) { Counts[k]++; }
		}
		uint32 Flag = (uint32)ss;
		for (int32 k = 0; k < 9; ++k) { Flag |= ((uint32)Counts[k]) << BitPos[k]; }
		return Flag;
	}

	static void PutU32(TArray<uint8>& B, int32 Off, uint32 V)
	{
		B[Off] = V & 0xFF; B[Off + 1] = (V >> 8) & 0xFF; B[Off + 2] = (V >> 16) & 0xFF; B[Off + 3] = (V >> 24) & 0xFF;
	}
	static void PutU16(TArray<uint8>& B, int32 Off, uint16 V) { B[Off] = V & 0xFF; B[Off + 1] = (V >> 8) & 0xFF; }

	// Box-downscale BGRA in place by 2x (average each 2x2 block). Power-of-two textures.
	static void HalveBGRA(TArray<uint8>& P, int32& W, int32& H)
	{
		const int32 NW = FMath::Max(1, W / 2), NH = FMath::Max(1, H / 2);
		TArray<uint8> Out; Out.SetNumUninitialized(NW * NH * 4);
		for (int32 y = 0; y < NH; ++y)
		{
			for (int32 x = 0; x < NW; ++x)
			{
				for (int32 c = 0; c < 4; ++c)
				{
					const int32 s0 = P[((2 * y) * W + 2 * x) * 4 + c];
					const int32 s1 = P[((2 * y) * W + 2 * x + 1) * 4 + c];
					const int32 s2 = P[((2 * y + 1) * W + 2 * x) * 4 + c];
					const int32 s3 = P[((2 * y + 1) * W + 2 * x + 1) * 4 + c];
					Out[(y * NW + x) * 4 + c] = (uint8)((s0 + s1 + s2 + s3) / 4);
				}
			}
		}
		P = MoveTemp(Out); W = NW; H = NH;
	}

	// ---- clean-room BC block compressors (faithful port of tools/build_ytd.py's numpy
	// encoders, which render DXT1 + ATI2 in-game). Public S3TC/DX spec, no lifted code. ----
	static uint16 Pack565(int r, int g, int b)
	{
		return (uint16)((((r >> 3) & 0x1F) << 11) | (((g >> 2) & 0x3F) << 5) | ((b >> 3) & 0x1F));
	}
	static void Expand565(uint16 c, int& r, int& g, int& b)
	{
		const int R = (c >> 11) & 0x1F, G = (c >> 5) & 0x3F, B = c & 0x1F;
		r = (R << 3) | (R >> 2); g = (G << 2) | (G >> 4); b = (B << 3) | (B >> 2);
	}
	// BC1 colour block: 16 RGB pixels -> 8 bytes (4-colour mode; c0>=c1 via component max/min).
	static void Bc1(const int rgb[16][3], uint8* out)
	{
		int mx[3] = { rgb[0][0], rgb[0][1], rgb[0][2] }, mn[3] = { rgb[0][0], rgb[0][1], rgb[0][2] };
		for (int i = 1; i < 16; ++i) { for (int c = 0; c < 3; ++c) { mx[c] = FMath::Max(mx[c], rgb[i][c]); mn[c] = FMath::Min(mn[c], rgb[i][c]); } }
		const uint16 c0 = Pack565(mx[0], mx[1], mx[2]), c1 = Pack565(mn[0], mn[1], mn[2]);
		int p[4][3];
		Expand565(c0, p[0][0], p[0][1], p[0][2]);
		Expand565(c1, p[1][0], p[1][1], p[1][2]);
		for (int c = 0; c < 3; ++c) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
		uint32 packed = 0;
		for (int i = 0; i < 16; ++i)
		{
			int best = 0x7FFFFFFF, bk = 0;
			for (int k = 0; k < 4; ++k)
			{
				const int dr = rgb[i][0] - p[k][0], dg = rgb[i][1] - p[k][1], db = rgb[i][2] - p[k][2];
				const int d = dr * dr + dg * dg + db * db;
				if (d < best) { best = d; bk = k; }
			}
			packed |= (uint32)bk << (2 * i);
		}
		out[0] = c0 & 0xFF; out[1] = (c0 >> 8) & 0xFF; out[2] = c1 & 0xFF; out[3] = (c1 >> 8) & 0xFF;
		for (int k = 0; k < 4; ++k) { out[4 + k] = (packed >> (8 * k)) & 0xFF; }
	}
	// BC4 single channel: 16 values -> 8 bytes (8-value interp; v0>=v1).
	static void Bc4(const int v[16], uint8* out)
	{
		int v0 = v[0], v1 = v[0];
		for (int i = 1; i < 16; ++i) { v0 = FMath::Max(v0, v[i]); v1 = FMath::Min(v1, v[i]); }
		int pal[8];
		for (int k = 0; k < 8; ++k) { pal[k] = ((7 - k) * v0 + k * v1) / 7; }
		pal[0] = v0; pal[1] = v1;
		uint64 packed = 0;
		for (int i = 0; i < 16; ++i)
		{
			int best = 0x7FFFFFFF, bk = 0;
			for (int k = 0; k < 8; ++k) { const int d = FMath::Abs(v[i] - pal[k]); if (d < best) { best = d; bk = k; } }
			packed |= (uint64)bk << (3 * i);
		}
		out[0] = (uint8)v0; out[1] = (uint8)v1;
		for (int k = 0; k < 6; ++k) { out[2 + k] = (packed >> (8 * k)) & 0xFF; }
	}
	// Encode one mip level of a BGRA buffer (LW x LH) in Mode -> append blocks to Out.
	// Block order row-major; pixels row-major; edge-replicate pad for sub-4 mips (matches build_ytd).
	static void EncodeLevel(const TArray<uint8>& BGRA, int32 LW, int32 LH, const FString& Mode, TArray<uint8>& Out)
	{
		const int32 BW = (LW + 3) / 4, BH = (LH + 3) / 4;
		const bool bAti2 = (Mode == TEXT("ATI2")), bDxt5 = (Mode == TEXT("DXT5"));
		uint8 blk[8];
		int rgb[16][3], chn[16], Rv[16], Gv[16], Av[16];
		for (int32 by = 0; by < BH; ++by)
		{
			for (int32 bx = 0; bx < BW; ++bx)
			{
				for (int py = 0; py < 4; ++py)
				{
					for (int px = 0; px < 4; ++px)
					{
						const int32 sx = FMath::Min(bx * 4 + px, LW - 1), sy = FMath::Min(by * 4 + py, LH - 1);
						const uint8* pxl = &BGRA[(sy * LW + sx) * 4];
						const int idx = py * 4 + px;
						rgb[idx][0] = pxl[2]; rgb[idx][1] = pxl[1]; rgb[idx][2] = pxl[0];   // R,G,B from BGRA
						Rv[idx] = pxl[2]; Gv[idx] = pxl[1]; Av[idx] = pxl[3];
					}
				}
				if (bAti2)
				{
					for (int i = 0; i < 16; ++i) { chn[i] = Rv[i]; } Bc4(chn, blk); Out.Append(blk, 8);
					for (int i = 0; i < 16; ++i) { chn[i] = Gv[i]; } Bc4(chn, blk); Out.Append(blk, 8);
				}
				else if (bDxt5)
				{
					for (int i = 0; i < 16; ++i) { chn[i] = Av[i]; } Bc4(chn, blk); Out.Append(blk, 8);
					Bc1(rgb, blk); Out.Append(blk, 8);
				}
				else { Bc1(rgb, blk); Out.Append(blk, 8); }
			}
		}
	}
}

FString URudeToolset::ExportYtdBinary(const FString& TextureSpecs, const FString& OutYtdPath,
                                      const FString& MaxDim)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
#if WITH_EDITORONLY_DATA
	struct FTexIn { FString Name; uint32 Hash = 0; int32 W = 0; int32 H = 0; uint32 U40 = 20;
	                uint32 Fmt = 21; int32 Stride = 0; int32 Mips = 1; TArray<uint8> Data; };
	TArray<FTexIn> Texs;

	TArray<FString> Entries;
	TextureSpecs.ParseIntoArray(Entries, TEXT(","), true);
	if (Entries.Num() == 0) { return Fail(TEXT("no texture specs (want ContentPath;RageName;Usage , ...)")); }
	const int32 Cap = FCString::Atoi(*MaxDim);   // 0/empty = no cap; else box-downscale oversized textures

	for (const FString& E : Entries)
	{
		TArray<FString> Fld;
		E.ParseIntoArray(Fld, TEXT(";"), true);
		if (Fld.Num() < 2) { return Fail(TEXT("each spec needs ContentPath;RageName[;Usage[;Format]]")); }
		const FString Path = Fld[0].TrimStartAndEnd();
		const FString Name = Fld[1].TrimStartAndEnd();
		const FString Usage = (Fld.Num() > 2) ? Fld[2].TrimStartAndEnd().ToUpper() : TEXT("DIFFUSE");
		FString FmtSel = (Fld.Num() > 3) ? Fld[3].TrimStartAndEnd().ToUpper() : TEXT("AUTO");

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *Path);
		if (!Tex) { return Fail(FString::Printf(TEXT("texture not found: %s"), *Path)); }
		FTextureSource& Src = Tex->Source;
		if (!Src.IsValid()) { return Fail(FString::Printf(TEXT("no editor source: %s"), *Path)); }
		int32 tw = Src.GetSizeX(), th = Src.GetSizeY();
		const ETextureSourceFormat SF = Src.GetFormat();
		TArray64<uint8> Mip;
		if (!Src.GetMipData(Mip, 0, 0, 0, nullptr)) { return Fail(FString::Printf(TEXT("GetMipData failed: %s"), *Path)); }

		TArray<uint8> BGRA; BGRA.SetNumUninitialized(tw * th * 4);
		if (SF == TSF_BGRA8 || SF == TSF_BGRE8)
		{
			FMemory::Memcpy(BGRA.GetData(), Mip.GetData(), FMath::Min<int64>(Mip.Num(), BGRA.Num()));
		}
		else if (SF == TSF_G8)
		{
			for (int32 i = 0; i < tw * th; ++i)
			{ const uint8 G = Mip[i]; BGRA[i * 4] = G; BGRA[i * 4 + 1] = G; BGRA[i * 4 + 2] = G; BGRA[i * 4 + 3] = 255; }
		}
		else { return Fail(FString::Printf(TEXT("unsupported source fmt %d (want BGRA8/G8): %s"), (int32)SF, *Path)); }

		// optional downscale (mainly for RAW; DXT/BC keeps full-res small enough)
		while (Cap > 0 && (tw > Cap || th > Cap) && tw > 1 && th > 1) { RudeYtd::HalveBGRA(BGRA, tw, th); }

		// resolve AUTO: NORMAL -> ATI2 (BC5); real alpha -> DXT5; else DXT1 (matches build_ytd)
		if (FmtSel == TEXT("AUTO"))
		{
			if (Usage == TEXT("NORMAL")) { FmtSel = TEXT("ATI2"); }
			else
			{
				bool bAlpha = false;
				for (int32 i = 3; i < BGRA.Num(); i += 4) { if (BGRA[i] < 255) { bAlpha = true; break; } }
				FmtSel = bAlpha ? TEXT("DXT5") : TEXT("DXT1");
			}
		}

		FTexIn T;
		T.Name = Name; T.Hash = RudeYtd::Joaat(Name); T.W = tw; T.H = th;
		T.U40 = (Usage == TEXT("NORMAL")) ? 22u : 20u;   // 🧠 usage-derived (reproduced; pending confirm)
		if (FmtSel == TEXT("RAW") || FmtSel == TEXT("A8R8G8B8"))
		{
			T.Fmt = 21; T.Stride = tw * 4; T.Mips = 1; T.Data = MoveTemp(BGRA);      // uncompressed
		}
		else if (FmtSel == TEXT("DXT1") || FmtSel == TEXT("DXT5") || FmtSel == TEXT("ATI2"))
		{
			const int32 blockBytes = (FmtSel == TEXT("DXT1")) ? 8 : 16;
			T.Fmt = (uint32)FmtSel[0] | ((uint32)FmtSel[1] << 8) | ((uint32)FmtSel[2] << 16) | ((uint32)FmtSel[3] << 24);  // FourCC
			T.Stride = ((tw + 3) / 4) * blockBytes / 4;                              // bytes per pixel-row
			int32 cw = tw, ch = th, mips = 0;
			TArray<uint8> lvl = MoveTemp(BGRA);
			while (true)                                                             // mip chain down to 4x4
			{
				RudeYtd::EncodeLevel(lvl, cw, ch, FmtSel, T.Data);
				++mips;
				if (cw <= 4 || ch <= 4) { break; }   // RAGE stops at the min DXT block (4x4); sub-4 mips break streaming
				RudeYtd::HalveBGRA(lvl, cw, ch);
			}
			T.Mips = mips;
		}
		else { return Fail(FString::Printf(TEXT("unknown format '%s' (AUTO|DXT1|DXT5|ATI2|RAW)"), *FmtSel)); }
		Texs.Add(MoveTemp(T));
	}

	// hash-sorted dictionary order (RAGE stores entries sorted by name hash)
	Texs.Sort([](const FTexIn& A, const FTexIn& B) { return A.Hash < B.Hash; });
	const int32 N = Texs.Num();

	// ---- graphics segment: pixel data, tightly packed. Each texture aligns to TA (8KB,
	// as real ytds do - NOT 4MB per texture, which oversized us). The TOTAL pads to a 4MB
	// page so the segment tiles into uniform 4MB pages -> segment-size flags always valid. ----
	const uint32 TA = 0x2000;         // per-texture alignment (real-ytd convention)
	const uint32 GP = 0x400000;       // total-segment page (keeps FlagsFromSize valid)
	TArray<uint8> Gfx;
	TArray<uint32> GfxOff; GfxOff.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		if (Gfx.Num() % TA) { Gfx.AddZeroed(TA - (Gfx.Num() % TA)); }
		GfxOff[i] = (uint32)Gfx.Num();
		Gfx.Append(Texs[i].Data);
	}
	if (Gfx.Num() % GP) { Gfx.AddZeroed(GP - (Gfx.Num() % GP)); }

	// ---- system segment: pgDictionary<grcTexture> at CW's observed offsets ----
	const int32 TEX_BASE = 0x450, TEX_SZ = 0x90;
	// NAME_SLOT was a fixed 0x20. RAGE texture names routinely exceed 31 chars (~10% of the Cayo
	// set), and the write loop below bounds only against the segment end - so a >=32-char name lost
	// its NUL and bled into the next slot, and the LAST name spilled into the pointer array's
	// never-written high dwords (chars 36+ landed in pointer[0]'s high dword => an invalid tagged
	// 64-bit pointer). The layout is fixup-driven, so the stride does not have to be 0x20: widen it
	// UNIFORMLY to fit the longest name + NUL, 16-byte aligned. Short-name dictionaries (the
	// in-game-proven rude_rockwall_tex, 17 chars) keep the 0x20 stride and stay byte-identical.
	int32 LongestName = 0;
	for (const auto& T : Texs) { LongestName = FMath::Max(LongestName, T.Name.Len()); }
	const int32 NAME_SLOT = FMath::Max(0x20, ((LongestName + 1 + 15) / 16) * 16);
	TArray<int32> TexOff, NameOff;
	for (int32 i = 0; i < N; ++i) { TexOff.Add(TEX_BASE + i * TEX_SZ); }
	const int32 NamesStart = TEX_BASE + N * TEX_SZ;
	for (int32 i = 0; i < N; ++i) { NameOff.Add(NamesStart + i * NAME_SLOT); }
	const int32 PtrArr = NamesStart + N * NAME_SLOT;
	const int32 HashArr = PtrArr + N * 8;
	const int32 SysEnd = HashArr + N * 4;
	uint32 SysSize = 0x2000;
	while (SysSize < (uint32)SysEnd) { SysSize <<= 1; }

	TArray<uint8> Sys; Sys.AddZeroed(SysSize);
	auto Sptr = [](int32 Off) { return 0x50000000u | (uint32)Off; };
	auto Gptr = [](uint32 Off) { return 0x60000000u | Off; };

	// pgDictionary header
	RudeYtd::PutU32(Sys, 0x00, 0); RudeYtd::PutU32(Sys, 0x04, 1);              // VFT const 0x100000000
	RudeYtd::PutU32(Sys, 0x08, Sptr(0x40));                                    // BlockMap*
	RudeYtd::PutU32(Sys, 0x18, 1);                                            // RefCount
	RudeYtd::PutU32(Sys, 0x20, Sptr(HashArr)); RudeYtd::PutU32(Sys, 0x28, (uint32)((N << 16) | N));
	RudeYtd::PutU32(Sys, 0x30, Sptr(PtrArr));  RudeYtd::PutU32(Sys, 0x38, (uint32)((N << 16) | N));

	// grcTexture structs
	for (int32 i = 0; i < N; ++i)
	{
		const int32 b = TexOff[i];
		const FTexIn& T = Texs[i];
		RudeYtd::PutU32(Sys, b + 0x00, 0); RudeYtd::PutU32(Sys, b + 0x04, 1);  // VFT
		RudeYtd::PutU32(Sys, b + 0x28, Sptr(NameOff[i]));                      // name*
		RudeYtd::PutU32(Sys, b + 0x30, 1);
		RudeYtd::PutU32(Sys, b + 0x40, T.U40);
		RudeYtd::PutU16(Sys, b + 0x50, (uint16)T.W); RudeYtd::PutU16(Sys, b + 0x52, (uint16)T.H);
		RudeYtd::PutU16(Sys, b + 0x54, 1); RudeYtd::PutU16(Sys, b + 0x56, (uint16)T.Stride);   // depth, stride
		RudeYtd::PutU32(Sys, b + 0x58, T.Fmt);                                 // D3DFMT enum (21=A8R8G8B8) or FourCC
		Sys[b + 0x5d] = (uint8)T.Mips;                                         // mip level count
		RudeYtd::PutU32(Sys, b + 0x70, Gptr(GfxOff[i]));                       // pixel data*
	}
	// names (ASCII, null-terminated)
	for (int32 i = 0; i < N; ++i)
	{
		const FString& Nm = Texs[i].Name;
		// clamp to the slot (leaving the NUL) as well as the segment - NAME_SLOT is sized to fit
		// the longest name above, so this cannot truncate; it is a backstop, not the mechanism.
		for (int32 k = 0; k < Nm.Len() && k < NAME_SLOT - 1 && (NameOff[i] + k) < (int32)SysSize; ++k)
		{
			Sys[NameOff[i] + k] = (uint8)Nm[k];
		}
	}
	// parallel hash + pointer arrays
	for (int32 i = 0; i < N; ++i)
	{
		RudeYtd::PutU32(Sys, PtrArr + i * 8, Sptr(TexOff[i]));
		RudeYtd::PutU32(Sys, HashArr + i * 4, Texs[i].Hash);
	}

	// flags + blockmap page counts
	int32 SysPages = 0, GfxPages = 0;
	const uint32 SysFlag = RudeYtd::FlagsFromSize(SysSize, SysPages);
	const uint32 GfxFlag = 0xd0000000u | RudeYtd::FlagsFromSize((uint32)Gfx.Num(), GfxPages);
	RudeYtd::PutU32(Sys, 0x48, (uint32)(((GfxPages & 0xFF) << 8) | (SysPages & 0xFF)));

	// ---- raw DEFLATE of [sys | gfx] (RSC7 uses headerless deflate) ----
	TArray<uint8> Payload;
	Payload.Append(Sys);
	Payload.Append(Gfx);
	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Payload.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Payload.GetData(), Payload.Num()))
	{
		return Fail(TEXT("zlib compress failed"));
	}
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream (need standard 2-byte header)")); }
	const int32 RawStart = 2, RawLen = ZSize - 6;   // strip 2-byte zlib header + 4-byte adler32

	// ---- assemble RSC7 file ----
	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V >> 8) & 0xFF); Out.Add((V >> 16) & 0xFF); Out.Add((V >> 24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(13); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + RawStart, RawLen);

	if (!FFileHelper::SaveArrayToFile(Out, *OutYtdPath)) { return Fail(TEXT("write .ytd failed")); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"ytdPath\":\"%s\",\"textures\":%d,\"bytes\":%d,\"sysFlags\":\"0x%08x\",\"gfxFlags\":\"0x%08x\"}"),
		*OutYtdPath, N, Out.Num(), SysFlag, GfxFlag);
#else
	return Fail(TEXT("editor-only"));
#endif
}

// ======================= ExportYbnBinary - clean-room .ybn (RSC7 v43) =======================
// UStaticMesh -> phBoundComposite[ phBoundGeometryBVH ], binary, no CodeWalker. P5 step 2.
// Format reversed from our own CW diff pair; every struct is CONSTRUCTED from pinned field
// offsets (docs/ENGINEERING_LOG "ybn binary format") - no template bytes, so it generalizes.
// Validated offline against the rock (2097v/4073t) + a synthetic cube: vertices round-trip
// within quantum, polys in range, BVH covers every poly exactly once.
namespace RudeYbn
{
	static const float UNK_F1 = 7.62962742e-08f;   // child+0x9c (constant in our emitter)
	static const float UNK_F2 = 0.0025f;           // child+0xac
	static const float CHILD_MARGIN = 0.005f;      // child+0x2c ; composite margin = 0
	static const uint32 CHILD_FLAGS1 = 0x3e;       // composite ChildrenFlags1 (single child)
	static const uint32 CHILD_FLAGS2 = 0x3e;       // composite ChildrenFlags2
	// Second u32 of each 16-byte ChildrenFlags entry, copied from CW's known-good binary
	// byte-for-byte (looks like don't-care/uninitialized in CW's writer; kept for parity).
	static const uint32 CHILD_FLAGS_PAD = 0x07f3bec0;
	static const int32 POLYS_PER_LEAF = 4;
	// phOptimizedBvh m_Trees: maximal subtrees of <= this many nodes. Pinned from the real
	// Legacy corpus (41 ybns, Desktop/fxserver): max observed tree span = 127 across every
	// file; spine (uncovered) nodes = treeCount-1 in EVERY file, i.e. trees are the maximal
	// <=127-node subtrees of one flat stackless BVH. Subtree node counts are always odd.
	static const int32 MAX_NODES_PER_TREE = 127;

	static void PU32(TArray<uint8>& B, int32 O, uint32 V)
	{ B[O] = V & 0xFF; B[O+1] = (V>>8) & 0xFF; B[O+2] = (V>>16) & 0xFF; B[O+3] = (V>>24) & 0xFF; }
	static void PU16(TArray<uint8>& B, int32 O, uint16 V) { B[O] = V & 0xFF; B[O+1] = (V>>8) & 0xFF; }
	static void PS16(TArray<uint8>& B, int32 O, int16 V) { PU16(B, O, (uint16)V); }
	static void PF32(TArray<uint8>& B, int32 O, float V)
	{ uint32 U; FMemory::Memcpy(&U, &V, 4); PU32(B, O, U); }
	// 8-byte tagged fixup into the system segment (0x50000000 | offset); high 4 bytes zero.
	static void PPTR(TArray<uint8>& B, int32 O, int32 Target)
	{ PU32(B, O, 0x50000000u | (uint32)Target); PU32(B, O + 4, 0); }
	static void PVEC3(TArray<uint8>& B, int32 O, const float V[3])
	{ PF32(B, O, V[0]); PF32(B, O+4, V[1]); PF32(B, O+8, V[2]); }

	struct FBvhNode
	{
		float Lo[3]; float Hi[3];
		int32 PolyStart = 0; int32 PolyCount = 0;
		bool bLeaf = false; int32 Escape = 0;
	};

	// Recursive median split over Idx[Lo,Hi). Nodes appended DFS pre-order; polygons
	// recorded in LEAF order so each leaf owns a CONTIGUOUS poly range.
	static int32 BuildBvh(const TArray<FVector3f>& Rel, const TArray<int32>& Indices,
		const TArray<FVector3f>& TriCtr, TArray<int32>& Idx, int32 Lo, int32 Hi,
		TArray<FBvhNode>& Nodes, TArray<int32>& PolyOrder)
	{
		const int32 NI = Nodes.Num();
		FBvhNode N;
		float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (int32 k = Lo; k < Hi; ++k)
		{
			const int32 T = Idx[k];
			for (int32 c = 0; c < 3; ++c)
			{
				const FVector3f& V = Rel[Indices[T * 3 + c]];
				for (int32 a = 0; a < 3; ++a)
				{ lo[a] = FMath::Min(lo[a], V[a]); hi[a] = FMath::Max(hi[a], V[a]); }
			}
		}
		for (int32 a = 0; a < 3; ++a) { N.Lo[a] = lo[a]; N.Hi[a] = hi[a]; }

		if (Hi - Lo <= POLYS_PER_LEAF)
		{
			N.bLeaf = true; N.PolyStart = PolyOrder.Num(); N.PolyCount = Hi - Lo;
			for (int32 k = Lo; k < Hi; ++k) { PolyOrder.Add(Idx[k]); }
			Nodes.Add(N);
			return NI;
		}
		N.bLeaf = false; Nodes.Add(N);

		int32 Axis = 0; float Best = hi[0] - lo[0];
		for (int32 a = 1; a < 3; ++a) { const float E = hi[a] - lo[a]; if (E > Best) { Best = E; Axis = a; } }
		{
			TArray<int32> Tmp; Tmp.Append(Idx.GetData() + Lo, Hi - Lo);
			Tmp.Sort([&TriCtr, Axis](const int32& A, const int32& B) { return TriCtr[A][Axis] < TriCtr[B][Axis]; });
			FMemory::Memcpy(Idx.GetData() + Lo, Tmp.GetData(), sizeof(int32) * (Hi - Lo));
		}
		const int32 Mid = Lo + (Hi - Lo) / 2;
		BuildBvh(Rel, Indices, TriCtr, Idx, Lo, Mid, Nodes, PolyOrder);
		BuildBvh(Rel, Indices, TriCtr, Idx, Mid, Hi, Nodes, PolyOrder);
		return NI;
	}

	// Escape index = the node AFTER this node's whole subtree (stackless skip). RELATIVE
	// for internal nodes when serialized.
	static int32 SetEscape(TArray<FBvhNode>& Nodes, int32 i)
	{
		if (Nodes[i].bLeaf) { Nodes[i].Escape = i + 1; return i + 1; }
		int32 n = SetEscape(Nodes, i + 1);
		n = SetEscape(Nodes, n);
		Nodes[i].Escape = n;
		return n;
	}

	// Uniform-page flag encoding for LARGE resources: N pages of size P (pow2 >= 0x2000).
	// Page size must be >= the largest single emitted block, or the block SPANS a page
	// boundary - RAGE pages are independently relocatable and a torn blob crashes the
	// allocator (ERR_MEM_MULTIALLOC_FREE, learned in-game on the first binary ydr: the
	// rock's 120KB vertex blob straddled uniform 64KB pages; CW's oracle avoids it with
	// a 128KB first page). Encoding: P = 0x200 << (ss+k); count of class-k pages lives
	// in a bounded bit-field, so pick the smallest ss whose k-field holds N.
	static uint32 SysPageFlagsUniform(uint32 RawSize, uint32 P, uint32& OutPadded, uint32& OutPages)
	{
		const uint32 N = (RawSize + P - 1) / P;
		OutPadded = N * P; OutPages = N;
		int32 Shift = 0; { uint32 v = P; while (v > 0x200u) { v >>= 1; ++Shift; } }   // ss+k
		// class k: bit position + capacity (k8..k4 usable; k3..k0 capacity 1)
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };   // k0..k8
		static const uint32 KMax[9] = { 1, 1, 1, 1, 127, 63, 15, 3, 1 };
		for (int32 k = FMath::Min(Shift, 8); k >= 0; --k)
		{
			const int32 ss = Shift - k;
			if (ss > 0xF || KMax[k] < N) { continue; }
			return (uint32)ss | (N << KBit[k]);
		}
		return 0xFFFFFFFFu;   // unencodable (absurd sizes) - caller must fail
	}

	// RSC7 system-segment page flags, reverse-engineered from CW's KNOWN-GOOD ybn output.
	// RAGE caps a system page at 0x10000 (64KB) - a single 128KB page is rejected at load
	// with "Invalid fixup, address is neither virtual nor physical". So:
	//   - segment <= 64KB : one page, size rounded up to a power of two (>=0x2000), base=size/16
	//     (matches real small ybns, e.g. itzmapz 0x4000 -> 0x20020001).
	//   - segment  > 64KB : rounded up to a 64KB multiple, N equal 64KB pages, base 0x200,
	//     class k7 (matches CW: 0x20000 -> two 64KB pages -> 0x20000040).
	// Returns the low-28 flag bits (caller ORs the 0x2 segment-type nibble) and the padded size.
	static uint32 SysPageFlags(uint32 RawSize, uint32& OutSize)
	{
		if (RawSize <= 0x10000u)
		{
			uint32 S = 0x2000u; while (S < RawSize) { S <<= 1; }
			OutSize = S;
			const uint32 Base = S / 16u;              // one k4 page of size S
			int32 ss = 0; { uint32 b = Base; while (b > 0x200u) { b >>= 1; ++ss; } }
			return (uint32)ss | (1u << 17);           // s4 = 1 (one page, class k4)
		}
		const uint32 S = (RawSize + 0xFFFFu) & ~0xFFFFu;   // ceil to 64KB
		OutSize = S;
		const uint32 NPages = S / 0x10000u;
		// 64KB page = (0x200<<ss) * 2^k ; pick the smallest ss whose count field holds NPages
		//   ss=0->k7(bit5,max3)  ss=1->k6(bit7,max15)  ss=2->k5(bit11,max63)  ss=3->k4(bit17,max127)
		const int32 SsT[4] = {0, 1, 2, 3};
		const int32 BitT[4] = {5, 7, 11, 17};
		const uint32 MaxT[4] = {3, 15, 63, 127};
		for (int32 t = 0; t < 4; ++t)
		{
			if (NPages <= MaxT[t]) { return (uint32)SsT[t] | (NPages << BitT[t]); }
		}
		// >8MB collision (unlikely): fall back to the 4MB-cap tiler (best effort)
		int32 dummy = 0;
		return RudeYtd::FlagsFromSize(S, dummy);
	}
}

// ======================= ExportYdrBinary - clean-room .ydr (RSC7 v165) =======================
// The LAST CodeWalker dependency. Every struct pinned against our own CW oracle
// (rude_rockwall.ydr) + its XML ground truth - docs/ENGINEERING_LOG "ydr binary format",
// "COMPLETE STRUCT MAP". Bound serialization: same structures as ExportYbnBinary (the
// phBound code below is intentionally duplicated from the in-game-proven ybn writer with
// only the root-at-zero difference; shared-helper refactor is queued with a byte-identity
// regression gate - do NOT let the two drift).
FString URudeToolset::ExportYdrBinaryBatch(const FString& AssetFolder, const FString& OutDir,
                                           const FString& Filter)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// \u2b50 THE CONTINUITY PRINCIPLE, made real (Matt, BENCHMARK_ADDON_CITY \u00a75): "everything we're
	// working on should facilitate something at this scale down to importing a simple trash can".
	// Export was per-asset only, so a district could be IMPORTED in one call and then had to be
	// exported one mesh at a time - the difference between one prop and a district was a different
	// workflow, not a batch size. This closes that: same code path, same conventions, N assets.
	//
	// AssetFolder: a content folder ("/Game/RUDE/World/Meshes") walked recursively, OR a text file
	// of content paths, one per line - whichever the caller already has.
	// Filter: optional case-insensitive substring the asset NAME must contain (e.g. "dt1_").
	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();

	TArray<FString> AssetPaths;
	if (FPaths::FileExists(AssetFolder))
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *AssetFolder);
		for (const FString& L : Lines)
		{
			const FString T = L.TrimStartAndEnd();
			if (!T.IsEmpty()) { AssetPaths.Add(T); }
		}
	}
	else
	{
		// \u26d4 Same registry law as the texture library: GetAssets answers from what has been
		// scanned SO FAR and does NOT block, so a batch driven at editor startup silently exports
		// nothing. Scan and wait first (2026-07-29 defect).
		AR.ScanPathsSynchronous({ AssetFolder }, /*bForceRescan*/ false);
		if (AR.IsLoadingAssets()) { AR.WaitForCompletion(); }
		FARFilter F;
		F.PackagePaths.Add(FName(*AssetFolder));
		F.bRecursivePaths = true;
		F.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
		TArray<FAssetData> Found;
		AR.GetAssets(F, Found);
		for (const FAssetData& AD : Found)
		{
			AssetPaths.Add(AD.GetSoftObjectPath().ToString());
		}
	}
	const FString Needle = Filter.TrimStartAndEnd();
	if (!Needle.IsEmpty())
	{
		AssetPaths.RemoveAll([&Needle](const FString& P)
		{
			return !FPaths::GetBaseFilename(P).Contains(Needle, ESearchCase::IgnoreCase);
		});
	}
	if (AssetPaths.Num() == 0)
	{
		return Fail(TEXT("no StaticMesh assets matched - check the folder path and filter"));
	}
	IFileManager::Get().MakeDirectory(*OutDir, true);

	int32 Exported = 0, Failed = 0;
	int64 Bytes = 0;
	FString FailedList;
	for (int32 i = 0; i < AssetPaths.Num(); ++i)
	{
		const FString& A = AssetPaths[i];
		const FString Name = FPaths::GetBaseFilename(A);
		const FString Out = OutDir / (Name + TEXT(".ydr"));
		const FString R = ExportYdrBinary(A, Out);
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Exported;
			Bytes += IFileManager::Get().FileSize(*Out);
		}
		else
		{
			++Failed;
			if (Failed <= 30)
			{
				FailedList += FString::Printf(TEXT("%s\"%s\""),
					FailedList.IsEmpty() ? TEXT("") : TEXT(","), *Name);
			}
		}
		if ((i + 1) % 50 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ExportYdrBinaryBatch %d/%d (ok %d, fail %d)"),
				i + 1, AssetPaths.Num(), Exported, Failed);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ExportYdrBinaryBatch DONE: %d exported, %d failed, %.1f MB"),
		Exported, Failed, Bytes / 1048576.0);
	return FString::Printf(
		TEXT("{\"ok\":true,\"considered\":%d,\"exported\":%d,\"failed\":%d,\"bytes\":%lld,")
		TEXT("\"outDir\":\"%s\",\"failedAssets\":[%s]}"),
		AssetPaths.Num(), Exported, Failed, Bytes, *OutDir, *FailedList);
}

FString URudeToolset::ExportYdrBinary(const FString& AssetPath, const FString& OutYdrPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
#if WITH_EDITORONLY_DATA
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh) { return Fail(TEXT("StaticMesh not found")); }
	const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
	if (!MeshDesc) { return Fail(TEXT("no MeshDescription on LOD0")); }
	FStaticMeshConstAttributes Attributes(*MeshDesc);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesConstRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesConstRef<FName> GroupSlots = Attributes.GetPolygonGroupMaterialSlotNames();
	// Drawable name = the OUTPUT FILE's basename (corpus convention: the stream name IS
	// the drawable identity; also keeps the emitted bytes final - no post-hoc patching).
	FString MeshName = FPaths::GetBaseFilename(OutYdrPath);
	MeshName.RemoveFromEnd(TEXT(".ydr"));
	MeshName.ToLowerInline();

	// --- gather per polygon group (same rules as the XML lane: weld by (vid,normal,uv),
	// inverse RUDE transform, preset + texture names from the slot's RUDE MI) ---
	struct FGeo
	{
		FString Preset = TEXT("default");
		FString Diffuse, Normal;
		TArray<FVector3f> Pos; TArray<FVector3f> Nrm; TArray<FVector2f> UV;
		TArray<int32> Indices;
	};
	TArray<FGeo> Geos;
	for (const FPolygonGroupID GroupID : MeshDesc->PolygonGroups().GetElementIDs())
	{
		FGeo G;
		FString SlotName = GroupSlots[GroupID].ToString();
		const int32 Sep = SlotName.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const bool bRudeSlot = (Sep != INDEX_NONE);
		G.Preset = bRudeSlot ? SlotName.Left(Sep) : TEXT("default");
		int32 SlotIdx = INDEX_NONE;
		for (int32 i = 0; i < Mesh->GetStaticMaterials().Num(); ++i)
		{
			if (Mesh->GetStaticMaterials()[i].MaterialSlotName == GroupSlots[GroupID]) { SlotIdx = i; break; }
		}
		if (Mesh->GetStaticMaterials().IsValidIndex(SlotIdx))
		{
			if (const UMaterialInstanceConstant* MIC =
				Cast<UMaterialInstanceConstant>(Mesh->GetStaticMaterials()[SlotIdx].MaterialInterface))
			{
				UTexture* T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Diffuse")), T) && T) { G.Diffuse = T->GetName(); }
				T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Normal")), T) && T) { G.Normal = T->GetName(); }
			}
		}
		if (!bRudeSlot)
		{
			if (!G.Normal.IsEmpty())       { G.Preset = TEXT("normal_spec"); }
			else if (!G.Diffuse.IsEmpty()) { G.Preset = TEXT("spec"); }
		}
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
					if (const int32* Found = Weld.Find(Key)) { Index = *Found; }
					else
					{
						Index = G.Pos.Num();
						G.Pos.Add(FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f));
						G.Nrm.Add(FVector3f(N.X, -N.Y, N.Z));
						G.UV.Add(UV);
						Weld.Add(Key, Index);
					}
					G.Indices.Add(Index);
				}
			}
		}
		if (G.Pos.Num() > 0 && G.Indices.Num() >= 3)
		{
			if (G.Pos.Num() > 65535) { return Fail(TEXT("geometry exceeds 65535 verts (u16 indices) - split the mesh")); }
			Geos.Add(MoveTemp(G));
		}
	}
	if (Geos.Num() == 0) { return Fail(TEXT("no polygon groups with geometry")); }

	FVector3f BMin(FLT_MAX), BMax(-FLT_MAX);
	for (const FGeo& G : Geos) { for (const FVector3f& P : G.Pos) { BMin = BMin.ComponentMin(P); BMax = BMax.ComponentMax(P); } }
	const FVector3f Center = (BMin + BMax) * 0.5f;
	const float Radius = (BMax - Center).Size();

	// --- collision soup (welded across all geometries) computed EARLY so the page plan
	// can account for every block size before anything is emitted ---
	TArray<FVector3f> CV; TArray<int32> CI;
	{
		TMap<FString, int32> W2;
		for (const FGeo& G : Geos)
		{
			for (int32 i = 0; i < G.Indices.Num(); ++i)
			{
				const FVector3f P = G.Pos[G.Indices[i]];
				const FString K = FString::Printf(TEXT("%.4f,%.4f,%.4f"), P.X, P.Y, P.Z);
				int32 Idx;
				if (const int32* F = W2.Find(K)) { Idx = *F; }
				else { Idx = CV.Num(); CV.Add(P); W2.Add(K, Idx); }
				CI.Add(Idx);
			}
		}
	}
	if (CV.Num() > 65535) { return Fail(TEXT("collision verts exceed 65535")); }
	// u16 COUNT CEILINGS (pinned 2026-07-26). The vertex guard above is NOT sufficient: BVH leaf
	// PolyStart and the m_Trees start/end node indices are u16 in the real format, and a closed
	// mesh runs ~2 triangles per vertex - so a mesh can pass the vertex guard and still wrap the
	// polygon index, emitting a bound whose tail leaves point back at the start of the poly array
	// (silently wrong collision, no crash). Real large bounds solve this with MULTIPLE composite
	// children; this writer emits one, so refuse until it does. Nodes ~= 0.5-0.8*NP.
	if ((CI.Num() / 3) > 65535)
	{
		return Fail(TEXT("collision triangles exceed 65535 (u16 BVH poly index) - split the mesh"));
	}

	// --- page plan: page size = pow2 >= the largest single block (a block may NEVER
	// span a page boundary; RAGE pages are independently relocatable - the in-game
	// ERR_MEM_MULTIALLOC_FREE law) ---
	uint32 Largest = 0x2000;
	for (const FGeo& G : Geos)
	{
		Largest = FMath::Max(Largest, (uint32)G.Pos.Num() * 36u);
		Largest = FMath::Max(Largest, (uint32)G.Indices.Num() * 2u);
	}
	Largest = FMath::Max(Largest, (uint32)(CI.Num() / 3) * 16u);   // bound polys; node array is smaller
	Largest = FMath::Max(Largest, (uint32)CV.Num() * 6u);
	// Blocks that scale with the GEOMETRY COUNT rather than with vertex/index counts. Omitting
	// these was a hole in the no-span law: a mesh with many tiny polygon groups could emit a
	// geoBounds/ptr-array block larger than PAGE, and Emit() below only page-aligns blocks that
	// FIT in a page - an oversized one straddles the boundary (the ERR_MEM_MULTIALLOC_FREE class).
	{
		const uint32 NG = (uint32)Geos.Num();
		Largest = FMath::Max(Largest, (NG + 1) * 0x20u);   // geoBounds (N+1 pairs when N>1)
		Largest = FMath::Max(Largest, NG * 8u);            // geometry ptr array
		Largest = FMath::Max(Largest, NG * 8u);            // shader ptr array
		Largest = FMath::Max(Largest, NG * 2u);            // shader map
	}
	const int32 PAGE = (int32)FMath::RoundUpToPowerOfTwo(Largest);

	// --- segment writer: header reserved @0, page-aware Emit ---
	TArray<uint8> Seg; Seg.AddZeroed(0xd0);
	bool bPageOverflow = false;
	auto Emit = [&Seg, PAGE, &bPageOverflow](const TArray<uint8>& D, int32 Align = 16) -> int32
	{
		if (Seg.Num() % Align) { Seg.AddZeroed(Align - (Seg.Num() % Align)); }
		if (D.Num() <= PAGE && (Seg.Num() % PAGE) + D.Num() > PAGE)
		{
			Seg.AddZeroed(PAGE - (Seg.Num() % PAGE));
		}
		// Backstop: PAGE is computed from every block we know about, so this cannot fire today.
		// If a future block is added without feeding the plan above, fail LOUDLY rather than
		// emitting a torn file that crashes the client with no diagnosis.
		else if (D.Num() > PAGE) { bPageOverflow = true; }
		const int32 O = Seg.Num(); Seg.Append(D); return O;
	};
	auto EmitStr = [&](const FString& S) -> int32
	{
		TArray<uint8> B; B.SetNumZeroed(S.Len() + 1);
		for (int32 i = 0; i < S.Len(); ++i) { B[i] = (uint8)S[i]; }
		return Emit(B);
	};

	// --- vertex + index data per geometry (GTAV1: Pos 3f, Normal 3f, Colour0 4xu8, UV 2f) ---
	TArray<int32> OVData, OIData;
	for (const FGeo& G : Geos)
	{
		TArray<uint8> VB; VB.SetNumZeroed(G.Pos.Num() * 36);
		for (int32 v = 0; v < G.Pos.Num(); ++v)
		{
			const int32 o = v * 36;
			RudeYbn::PF32(VB, o + 0, G.Pos[v].X); RudeYbn::PF32(VB, o + 4, G.Pos[v].Y); RudeYbn::PF32(VB, o + 8, G.Pos[v].Z);
			RudeYbn::PF32(VB, o + 12, G.Nrm[v].X); RudeYbn::PF32(VB, o + 16, G.Nrm[v].Y); RudeYbn::PF32(VB, o + 20, G.Nrm[v].Z);
			VB[o + 24] = 255; VB[o + 25] = 255; VB[o + 26] = 255; VB[o + 27] = 255;
			RudeYbn::PF32(VB, o + 28, G.UV[v].X); RudeYbn::PF32(VB, o + 32, G.UV[v].Y);
		}
		OVData.Add(Emit(VB));
		TArray<uint8> IB; IB.SetNumZeroed(G.Indices.Num() * 2);
		for (int32 i = 0; i < G.Indices.Num(); ++i) { RudeYbn::PU16(IB, i * 2, (uint16)G.Indices[i]); }
		OIData.Add(Emit(IB));
	}

	// --- grcFvf (GTAV1): mask 0x59, stride 36, 4 channels, format nibbles ---
	// ⛔⛔ CRASH #6: this used to be emitted ONCE here and pointed at by EVERY vertex buffer.
	// RAGE FORBIDS SHARED OWNERSHIP: datResource fixup rewrites a pointer slot IN PLACE and is
	// NOT idempotent, so a block reached from N owners is fixed up N times - the 2nd pass reads
	// an ALREADY-RESOLVED 64-bit address whose high nibble is neither 5 nor 6, which is verbatim
	// "address is neither virtual nor physical". Real files never alias an fvf: 0 of 3,479
	// base-game v165 ydrs share one, and 17,370/17,370 geometries carry their OWN - dt1_02_groundb
	// pays for 7 byte-identical 16-byte fvfs in consecutive slots rather than alias. Impossible to
	// see at N==1, which is exactly why the single-geometry rock loads and every multi-material
	// export died. Now emitted per geometry, inside the loop below.
	auto MakeFvf = [&]() -> int32
	{
		TArray<uint8> Fvf; Fvf.AddZeroed(0x10);
		RudeYbn::PU32(Fvf, 0x00, 0x59); RudeYbn::PU16(Fvf, 0x04, 36); Fvf[0x07] = 4;
		RudeYbn::PU32(Fvf, 0x08, 0x55996996u); RudeYbn::PU32(Fvf, 0x0c, 0x77555555u);
		return Emit(Fvf);
	};
	const int32 OName = EmitStr(MeshName);

	// (vector parameter values are emitted INLINE inside each shader's contiguous
	// parameter allocation below - the oracle's layout, load-bearing for teardown)

	// --- per-shader: texture stubs (0x50: refcount, name*, 0x00020001), param table,
	//     param block, shader struct ---
	// ⛔⛔ CRASH #6 (the other half): these stubs used to be MEMOIZED BY NAME, so two shaders
	// referencing the same texture shared one grcTexture stub AND one ASCII name string. Same
	// non-idempotent-fixup violation as the fvf above: a doubly-owned block is fixed up twice and
	// the second pass sees an already-resolved address -> "neither virtual nor physical".
	// R* never does this in EXTERNAL-ytd mode: 0 of 2,299 external-texdict files share a stub,
	// while 789 of them hit exactly this situation and DUPLICATE instead (db_apart_02_ carries 4
	// separate stubs AND 4 separate copies of "HW_tpageDingB_RO_01"). Sharing IS legal in
	// EMBEDDED-texdict mode, but only because the pgDictionary owns and places the texture once -
	// we emit external stubs, so we must duplicate. Memoization removed deliberately; the few
	// wasted bytes are the price of single ownership.
	auto TexStub = [&](const FString& Name) -> int32
	{
		const FString L = Name.ToLower();
		const int32 NameOfs = EmitStr(L);   // fresh string per stub, also single-owner
		TArray<uint8> St; St.AddZeroed(0x50);
		RudeYbn::PU32(St, 0x04, 1);
		RudeYbn::PPTR(St, 0x28, NameOfs);
		RudeYbn::PU32(St, 0x30, 0x00020001u);
		return Emit(St);
	};
	TArray<int32> ShaderOfs;
	TSet<FString> SubstitutedPresets;   // presets we had no verified param template for (crash #5)
	for (const FGeo& G : Geos)
	{
		// One CONTIGUOUS parameter allocation per shader (the oracle's load-bearing
		// layout - the crash-#2 root cause was missing it):
		//   [N entries x16: {meta, value*}] [V vector values x16, entries point INTO
		//   this] [N x u32 joaat(paramName) - the game binds parameters BY NAME HASH]
		// The param block's +0x14 encodes it: (allocSize<<16) | hashArrayOffset
		// (= 0x01500100 for the 9-param normal_spec template, matching the oracle).
		struct FPar { uint32 Meta; int32 StubOfs; const TCHAR* Name; };   // StubOfs<0 = inline vector
		TArray<FPar> Pars;
		// ALWAYS emit both samplers, so the block is always the full 9-register normal_spec
		// layout we declare (see crash #5 below). Shortening the block for a mesh with no
		// normal map would reintroduce exactly the count mismatch that crashes the loader.
		// A missing bump falls back to the stock `flatnormal`; a missing diffuse emits an
		// unresolved stub, which renders untextured rather than failing to load.
		Pars.Add({ 0x200u, TexStub(G.Diffuse.IsEmpty() ? TEXT("none") : G.Diffuse), TEXT("DiffuseSampler") });
		Pars.Add({ 0x300u, TexStub(G.Normal.IsEmpty() ? TEXT("flatnormal") : G.Normal), TEXT("BumpSampler") });
		static const uint32 VecMeta[7] = { 0xa601, 0xa501, 0xa401, 0xa301, 0xa201, 0xa101, 0xa001 };
		static const float VecVals[7] = { 0.9f, 40.f, 0.3f, 1.f, 1.f, 0.f, 1.f };
		static const TCHAR* VecName[7] = { TEXT("specularFresnel"), TEXT("specularFalloffMult"),
			TEXT("specularIntensityMult"), TEXT("bumpiness"), TEXT("wetnessMultiplier"),
			TEXT("useTessellation"), TEXT("HardAlphaBlend") };
		for (int32 i = 0; i < 7; ++i) { Pars.Add({ VecMeta[i], -1, VecName[i] }); }
		const int32 NPar = Pars.Num(), NVec = 7;
		const int32 HashOfs = NPar * 16 + NVec * 16;
		const int32 AllocSize = (NPar == 9) ? 0x150 : ((HashOfs + NPar * 4 + 15) & ~15);
		TArray<uint8> Zero; Zero.AddZeroed(AllocSize);
		const int32 OTbl = Emit(Zero);
		{
			int32 VecIdx = 0;
			for (int32 i = 0; i < NPar; ++i)
			{
				RudeYbn::PU32(Seg, OTbl + i * 16, Pars[i].Meta);
				if (Pars[i].StubOfs >= 0)
				{
					RudeYbn::PPTR(Seg, OTbl + i * 16 + 8, Pars[i].StubOfs);
				}
				else
				{
					const int32 VOfs = OTbl + NPar * 16 + VecIdx * 16;
					RudeYbn::PF32(Seg, VOfs, VecVals[VecIdx]);
					RudeYbn::PPTR(Seg, OTbl + i * 16 + 8, VOfs);
					++VecIdx;
				}
				RudeYbn::PU32(Seg, OTbl + HashOfs + i * 4, RudeYtd::Joaat(Pars[i].Name));
			}
		}
		// ⛔⛔ IN-GAME CRASH #5 ("Invalid fixup", 2026-07-26, Matt-witnessed on the first
		// multi-material export) - ROOT CAUSE AND FIX.
		// The old behaviour declared the shader by its OWN name hash while handing it
		// normal_spec's 9-register parameter block. That is harmless in the XML lane (CW
		// rebuilds the params) and FATAL in binary: the game resolves the shader by hash,
		// then walks THAT shader's real register layout over our block. A preset wanting
		// more params than normal_spec (e.g. normal_spec_detail) reads straight past our
		// 0x150 allocation into the next struct and interprets garbage as pointers ->
		// "Invalid fixup, address is neither virtual nor physical". Proof: a real 2-geo
		// oracle's shader carries 11 params with +0x14 = 0x01800130, not 9 / 0x01500100.
		// The rock survived a year of testing only because its preset genuinely WAS
		// normal_spec. v1 has exactly ONE verified parameter template, so v1 may only ever
		// DECLARE that preset: substitute rather than lie, and report what was substituted.
		// The real fix is a per-preset register table (the P2 material lane).
		// The shader OBJECT *is* the 0x30 param block (oracle: shader ptr-array entries
		// point straight at it - there is NO intermediate struct).
		const FString RawPreset = (G.Preset == TEXT("default")) ? TEXT("normal_spec") : G.Preset;
		const bool bTemplated = RawPreset.Equals(TEXT("normal_spec"), ESearchCase::IgnoreCase);
		if (!bTemplated) { SubstitutedPresets.Add(RawPreset); }
		const FString Preset = bTemplated ? RawPreset : TEXT("normal_spec");
		TArray<uint8> Blk; Blk.AddZeroed(0x30);
		RudeYbn::PPTR(Blk, 0x00, OTbl);
		RudeYbn::PU32(Blk, 0x08, RudeYtd::Joaat(Preset));
		RudeYbn::PU32(Blk, 0x10, 0x80000000u | (uint32)NPar);
		RudeYbn::PU32(Blk, 0x14, ((uint32)AllocSize << 16) | (uint32)HashOfs);
		RudeYbn::PU32(Blk, 0x18, RudeYtd::Joaat(Preset + TEXT(".sps")));
		RudeYbn::PU32(Blk, 0x20, 0x0000ff01u);
		RudeYbn::PU32(Blk, 0x24, 0x02000000u);
		ShaderOfs.Add(Emit(Blk));
	}
	TArray<uint8> ShArr; ShArr.AddZeroed(ShaderOfs.Num() * 8);
	for (int32 i = 0; i < ShaderOfs.Num(); ++i) { RudeYbn::PPTR(ShArr, i * 8, ShaderOfs[i]); }
	const int32 OShArr = Emit(ShArr);
	TArray<uint8> SG; SG.AddZeroed(0x40);
	RudeYbn::PU32(SG, 0x00, 0x406137f0u); RudeYbn::PU32(SG, 0x04, 1);   // VFT 0x1406137f0
	RudeYbn::PPTR(SG, 0x10, OShArr);
	RudeYbn::PU16(SG, 0x18, (uint16)ShaderOfs.Num()); RudeYbn::PU16(SG, 0x1a, (uint16)ShaderOfs.Num());
	RudeYbn::PU32(SG, 0x30, 4);
	const int32 OSG = Emit(SG);

	// --- blockmap (page count patched after final size) ---
	TArray<uint8> Bm; Bm.AddZeroed(0x40);
	const int32 OBm = Emit(Bm);

	// --- per-geometry: VB struct, IB struct, geometry struct ---
	TArray<int32> OGeoStructs;
	// geoBounds: for N>1 the real format is N+1 vec4-pairs - pair[0] = the UNION AABB, then one
	// pair per geometry. For N==1 it is exactly ONE pair (no union). Pinned 2026-07-26 against 47
	// grmModels in 25 real v165 ydrs (12 multi-geo drawables recomputed from their own vertex
	// buffers: pair[1+i]==geo[i] accepted 12/12, pair[i]==geo[i] rejected 12/12). The old code
	// emitted N pairs unconditionally - correct only for the single-geometry oracle it came from.
	// See ENGINEERING_LOG "ydr binary format" CORRECTED block.
	const int32 NGeo = Geos.Num();
	const bool bGeoUnion = NGeo > 1;
	const int32 GeoBoundsPairs = bGeoUnion ? NGeo + 1 : 1;
	TArray<uint8> GeoBounds; GeoBounds.AddZeroed(GeoBoundsPairs * 0x20);
	FVector3f UnionMin(FLT_MAX), UnionMax(-FLT_MAX);
	TArray<uint8> ShaderMap; ShaderMap.AddZeroed(FMath::Max(Geos.Num() * 2, 8));
	for (int32 gi = 0; gi < Geos.Num(); ++gi)
	{
		const FGeo& G = Geos[gi];
		// Struct sizes padded to the ORACLE's observed inter-struct spacing (its gaps are
		// zero tails = null pointer slots the loader fixes up; packing the next struct's
		// live data there reads as a bogus pointer -> "Invalid fixup", crash #4).
		TArray<uint8> Vb; Vb.AddZeroed(0x80);
		RudeYbn::PU32(Vb, 0x00, 0x4061d3f8u); RudeYbn::PU32(Vb, 0x04, 1);
		RudeYbn::PU16(Vb, 0x08, 36); RudeYbn::PU16(Vb, 0x0a, 0x59);
		RudeYbn::PPTR(Vb, 0x10, OVData[gi]);
		RudeYbn::PU32(Vb, 0x18, (uint32)G.Pos.Num());
		RudeYbn::PPTR(Vb, 0x20, OVData[gi]);
		RudeYbn::PPTR(Vb, 0x30, MakeFvf());   // OWN fvf per geometry - never shared (crash #6)
		const int32 OVb = Emit(Vb);
		TArray<uint8> Ib; Ib.AddZeroed(0x60);
		RudeYbn::PU32(Ib, 0x00, 0x4061d158u); RudeYbn::PU32(Ib, 0x04, 1);
		RudeYbn::PU32(Ib, 0x08, (uint32)G.Indices.Num());
		RudeYbn::PPTR(Ib, 0x10, OIData[gi]);
		const int32 OIb = Emit(Ib);
		TArray<uint8> Ge; Ge.AddZeroed(0xa0);
		RudeYbn::PU32(Ge, 0x00, 0x40618798u); RudeYbn::PU32(Ge, 0x04, 1);
		RudeYbn::PPTR(Ge, 0x18, OVb);
		RudeYbn::PPTR(Ge, 0x38, OIb);
		RudeYbn::PU32(Ge, 0x58, (uint32)G.Indices.Num());
		RudeYbn::PU32(Ge, 0x5c, (uint32)(G.Indices.Num() / 3));
		RudeYbn::PU16(Ge, 0x60, (uint16)G.Pos.Num()); RudeYbn::PU16(Ge, 0x62, 3);
		RudeYbn::PU32(Ge, 0x70, 36);
		RudeYbn::PPTR(Ge, 0x78, OVData[gi]);
		OGeoStructs.Add(Emit(Ge));
		FVector3f GMin(FLT_MAX), GMax(-FLT_MAX);
		for (const FVector3f& P : G.Pos) { GMin = GMin.ComponentMin(P); GMax = GMax.ComponentMax(P); }
		const float Mn[3] = { GMin.X, GMin.Y, GMin.Z }, Mx[3] = { GMax.X, GMax.Y, GMax.Z };
		const int32 GbPair = (bGeoUnion ? gi + 1 : gi) * 0x20;   // pair 0 is the union when N>1
		RudeYbn::PVEC3(GeoBounds, GbPair, Mn);
		RudeYbn::PVEC3(GeoBounds, GbPair + 0x10, Mx);
		UnionMin = UnionMin.ComponentMin(GMin); UnionMax = UnionMax.ComponentMax(GMax);
		RudeYbn::PU16(ShaderMap, gi * 2, (uint16)gi);
	}
	if (bGeoUnion)
	{
		const float Un[3] = { UnionMin.X, UnionMin.Y, UnionMin.Z };
		const float Ux[3] = { UnionMax.X, UnionMax.Y, UnionMax.Z };
		RudeYbn::PVEC3(GeoBounds, 0x00, Un);
		RudeYbn::PVEC3(GeoBounds, 0x10, Ux);
	}
	const int32 OGeoBounds = Emit(GeoBounds);
	const int32 OShaderMap = Emit(ShaderMap);
	TArray<uint8> GeoArr; GeoArr.AddZeroed(OGeoStructs.Num() * 8);
	for (int32 i = 0; i < OGeoStructs.Num(); ++i) { RudeYbn::PPTR(GeoArr, i * 8, OGeoStructs[i]); }
	const int32 OGeoArr = Emit(GeoArr);
	TArray<uint8> Model; Model.AddZeroed(0x30);
	RudeYbn::PU32(Model, 0x00, 0x40610a98u); RudeYbn::PU32(Model, 0x04, 1);
	RudeYbn::PPTR(Model, 0x08, OGeoArr);
	RudeYbn::PU16(Model, 0x10, (uint16)Geos.Num()); RudeYbn::PU16(Model, 0x12, (uint16)Geos.Num());
	RudeYbn::PPTR(Model, 0x18, OGeoBounds);
	RudeYbn::PPTR(Model, 0x20, OShaderMap);
	// +0x2c = RenderMask (u8 @+0x2c, 0xff = the XML lane's RenderMask 255) | 0 @+0x2d |
	// GEOMETRY COUNT (u16 @+0x2e). Pinned 2026-07-26: +0x2e == ngeo in 47/47 real v165 grmModels.
	// Was hardcoded 0x000100ffu, which is this expression at N==1 - so the in-game-proven
	// single-geometry artifact is byte-unchanged by this fix.
	RudeYbn::PU32(Model, 0x2c, 0x000000ffu | ((uint32)Geos.Num() << 16));
	const int32 OModel = Emit(Model);
	TArray<uint8> ModelArr; ModelArr.AddZeroed(8);
	RudeYbn::PPTR(ModelArr, 0, OModel);
	const int32 OModelArr = Emit(ModelArr);
	TArray<uint8> ModelsHdr; ModelsHdr.AddZeroed(0x10);
	RudeYbn::PPTR(ModelsHdr, 0x00, OModelArr);
	RudeYbn::PU16(ModelsHdr, 0x08, 1); RudeYbn::PU16(ModelsHdr, 0x0a, 1);
	const int32 OModelsHdr = Emit(ModelsHdr);

	// ---------- embedded phBoundComposite (whole-mesh GeometryBVH), duplicated from the
	// in-game-proven ExportYbnBinary with the composite emitted in place (not @0) ----------
	int32 OComposite = 0;
	{
		const int32 NV = CV.Num(), NP = CI.Num() / 3;
		FVector3f WMin(FLT_MAX), WMax(-FLT_MAX);
		for (const FVector3f& V : CV) { WMin = WMin.ComponentMin(V); WMax = WMax.ComponentMax(V); }
		const FVector3f Ctr = (WMin + WMax) * 0.5f;
		TArray<FVector3f> Rel; Rel.Reserve(NV);
		for (const FVector3f& V : CV) { Rel.Add(V - Ctr); }
		float RMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, RMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const FVector3f& R : Rel) { for (int32 a = 0; a < 3; ++a) { RMin[a] = FMath::Min(RMin[a], R[a]); RMax[a] = FMath::Max(RMax[a], R[a]); } }
		float Quant[3], Half[3];
		for (int32 a = 0; a < 3; ++a)
		{
			Half[a] = FMath::Max(FMath::Abs(RMin[a]), FMath::Abs(RMax[a]));
			Quant[a] = (Half[a] > 0.f) ? (Half[a] / 32767.0f) : 1.0f;
		}
		const float CornerR = FMath::Sqrt(Half[0]*Half[0] + Half[1]*Half[1] + Half[2]*Half[2]);
		float VertR = 0.f;
		for (const FVector3f& R : Rel) { VertR = FMath::Max(VertR, R.Size()); }
		const float WorldMin[3] = { WMin.X, WMin.Y, WMin.Z };
		const float WorldMax[3] = { WMax.X, WMax.Y, WMax.Z };
		const float WorldCtr[3] = { Ctr.X, Ctr.Y, Ctr.Z };

		TArray<FVector3f> TriCtr; TriCtr.Reserve(NP);
		for (int32 j = 0; j < NP; ++j) { TriCtr.Add((Rel[CI[j*3]] + Rel[CI[j*3+1]] + Rel[CI[j*3+2]]) / 3.0f); }
		TArray<int32> Order; Order.Reserve(NP);
		for (int32 j = 0; j < NP; ++j) { Order.Add(j); }
		TArray<RudeYbn::FBvhNode> Nodes; TArray<int32> PolyOrder;
		RudeYbn::BuildBvh(Rel, CI, TriCtr, Order, 0, NP, Nodes, PolyOrder);
		RudeYbn::SetEscape(Nodes, 0);
		if (PolyOrder.Num() != NP) { return Fail(TEXT("BVH leaf coverage broken")); }
		TArray<TPair<int32, int32>> Trees;
		{
			TArray<int32> Stack; Stack.Add(0);
			while (Stack.Num() > 0)
			{
				const int32 i = Stack.Pop();
				const int32 Size = Nodes[i].Escape - i;
				if (Size <= RudeYbn::MAX_NODES_PER_TREE || Nodes[i].bLeaf) { Trees.Add(TPair<int32, int32>(i, Nodes[i].Escape)); }
				else { Stack.Add(Nodes[i + 1].Escape); Stack.Add(i + 1); }
			}
		}
		float NBMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, NBMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const RudeYbn::FBvhNode& N : Nodes)
		{
			for (int32 a = 0; a < 3; ++a) { NBMin[a] = FMath::Min(NBMin[a], N.Lo[a]); NBMax[a] = FMath::Max(NBMax[a], N.Hi[a]); }
		}
		float NQ[3], NInv[3];
		for (int32 a = 0; a < 3; ++a)
		{
			const float M = FMath::Max(FMath::Abs(NBMin[a]), FMath::Abs(NBMax[a]));
			NQ[a] = (M > 0.f) ? (M / 32767.0f) : 1.0f;
			NInv[a] = (NQ[a] > 0.f) ? (1.0f / NQ[a]) : 0.f;
		}
		auto QS = [](float V, float Q) -> int16
		{ return (int16)FMath::Clamp<int32>(FMath::RoundToInt(V / Q), -32768, 32767); };

		TArray<uint8> PolyB; PolyB.AddZeroed(NP * 16);
		for (int32 k = 0; k < NP; ++k)
		{
			const int32 j = PolyOrder[k];
			const FVector3f& A = Rel[CI[j*3]]; const FVector3f& B2 = Rel[CI[j*3+1]]; const FVector3f& C = Rel[CI[j*3+2]];
			const float Area = 0.5f * FVector3f::CrossProduct(B2 - A, C - A).Size();
			uint32 AreaBits; FMemory::Memcpy(&AreaBits, &Area, 4);
			AreaBits &= 0xFFFFFFF8u;
			RudeYbn::PU32(PolyB, k*16, AreaBits);
			RudeYbn::PU16(PolyB, k*16+4, (uint16)CI[j*3]);
			RudeYbn::PU16(PolyB, k*16+6, (uint16)CI[j*3+1]);
			RudeYbn::PU16(PolyB, k*16+8, (uint16)CI[j*3+2]);
		}
		TArray<uint8> VertB; VertB.AddZeroed(NV * 6);
		for (int32 i = 0; i < NV; ++i)
		{
			for (int32 a = 0; a < 3; ++a) { RudeYbn::PS16(VertB, i*6 + a*2, QS(Rel[i][a], Quant[a])); }
		}
		TArray<uint8> MatB; MatB.AddZeroed(NP);
		TArray<uint8> NodeB; NodeB.AddZeroed(Nodes.Num() * 16);
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const RudeYbn::FBvhNode& N = Nodes[i];
			for (int32 a = 0; a < 3; ++a)
			{
				RudeYbn::PS16(NodeB, i*16 + a*2,     QS(N.Lo[a], NQ[a]));
				RudeYbn::PS16(NodeB, i*16 + 6 + a*2, QS(N.Hi[a], NQ[a]));
			}
			RudeYbn::PU16(NodeB, i*16 + 12, (uint16)(N.bLeaf ? N.PolyStart : (N.Escape - i)));
			RudeYbn::PU16(NodeB, i*16 + 14, (uint16)(N.bLeaf ? N.PolyCount : 0));
		}
		TArray<uint8> TreeB; TreeB.AddZeroed(Trees.Num() * 16);
		for (int32 t = 0; t < Trees.Num(); ++t)
		{
			const RudeYbn::FBvhNode& RootN = Nodes[Trees[t].Key];
			for (int32 a = 0; a < 3; ++a)
			{
				RudeYbn::PS16(TreeB, t*16 + a*2,     QS(RootN.Lo[a], NQ[a]));
				RudeYbn::PS16(TreeB, t*16 + 6 + a*2, QS(RootN.Hi[a], NQ[a]));
			}
			RudeYbn::PU16(TreeB, t*16 + 12, (uint16)Trees[t].Key);
			RudeYbn::PU16(TreeB, t*16 + 14, (uint16)Trees[t].Value);
		}
		const int32 OPoly = Emit(PolyB);
		const int32 ONode = Emit(NodeB);
		const int32 OVert = Emit(VertB);
		const int32 OMidx = Emit(MatB);
		const int32 OTrees = Emit(TreeB);
		TArray<uint8> Bvh; Bvh.AddZeroed(0x80);
		RudeYbn::PPTR(Bvh, 0x00, ONode);
		RudeYbn::PU32(Bvh, 0x08, (uint32)Nodes.Num()); RudeYbn::PU32(Bvh, 0x0c, (uint32)Nodes.Num());
		{
			const float WB0[3] = { NBMin[0]+WorldCtr[0], NBMin[1]+WorldCtr[1], NBMin[2]+WorldCtr[2] };
			const float WB1[3] = { NBMax[0]+WorldCtr[0], NBMax[1]+WorldCtr[1], NBMax[2]+WorldCtr[2] };
			RudeYbn::PVEC3(Bvh, 0x20, WB0); RudeYbn::PU32(Bvh, 0x2c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x30, WB1); RudeYbn::PU32(Bvh, 0x3c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x40, WorldCtr); RudeYbn::PU32(Bvh, 0x4c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x50, NInv); RudeYbn::PU32(Bvh, 0x5c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x60, NQ);   RudeYbn::PU32(Bvh, 0x6c, 0xffc00000u);
			RudeYbn::PPTR(Bvh, 0x70, OTrees);
			RudeYbn::PU16(Bvh, 0x78, (uint16)Trees.Num()); RudeYbn::PU16(Bvh, 0x7a, (uint16)Trees.Num());
		}
		const int32 OBvh = Emit(Bvh);
		TArray<uint8> Xf; Xf.AddZeroed(0x40);
		RudeYbn::PF32(Xf, 0x00, 1.f); RudeYbn::PF32(Xf, 0x14, 1.f); RudeYbn::PU32(Xf, 0x1c, 1);
		RudeYbn::PF32(Xf, 0x28, 1.f); RudeYbn::PU32(Xf, 0x2c, 1);
		const int32 OXf = Emit(Xf);
		TArray<uint8> F0; F0.AddZeroed(0x20);
		const int32 OF0 = Emit(F0);
		TArray<uint8> CBox; CBox.AddZeroed(0x20);
		RudeYbn::PVEC3(CBox, 0x00, WorldMin); RudeYbn::PU32(CBox, 0x0c, 1);
		RudeYbn::PVEC3(CBox, 0x10, WorldMax); RudeYbn::PF32(CBox, 0x1c, RudeYbn::CHILD_MARGIN);
		const int32 OCBox = Emit(CBox);
		TArray<uint8> F1; F1.AddZeroed(16);
		RudeYbn::PU32(F1, 0, RudeYbn::CHILD_FLAGS1); RudeYbn::PU32(F1, 4, RudeYbn::CHILD_FLAGS_PAD);
		const int32 OF1 = Emit(F1);
		TArray<uint8> F2; F2.AddZeroed(16);
		RudeYbn::PU32(F2, 0, RudeYbn::CHILD_FLAGS2); RudeYbn::PU32(F2, 4, RudeYbn::CHILD_FLAGS_PAD);
		const int32 OF2 = Emit(F2);
		TArray<uint8> Ch; Ch.AddZeroed(0x150);
		RudeYbn::PF32(Ch, 0x00, VertR); RudeYbn::PU32(Ch, 0x04, 1);
		Ch[0x10] = 0x08;
		RudeYbn::PF32(Ch, 0x14, CornerR);
		RudeYbn::PVEC3(Ch, 0x20, WorldMax); RudeYbn::PF32(Ch, 0x2c, RudeYbn::CHILD_MARGIN);
		RudeYbn::PVEC3(Ch, 0x30, WorldMin); RudeYbn::PU32(Ch, 0x3c, 1);
		RudeYbn::PVEC3(Ch, 0x40, WorldCtr);
		RudeYbn::PVEC3(Ch, 0x50, WorldCtr);
		RudeYbn::PF32(Ch, 0x60, 1.f); RudeYbn::PF32(Ch, 0x64, 1.f);
		RudeYbn::PF32(Ch, 0x68, 1.f); RudeYbn::PF32(Ch, 0x6c, 1.f);
		RudeYbn::PU32(Ch, 0x84, (uint32)NV);
		RudeYbn::PPTR(Ch, 0x88, OPoly);
		RudeYbn::PVEC3(Ch, 0x90, Quant); RudeYbn::PF32(Ch, 0x9c, RudeYbn::UNK_F1);
		RudeYbn::PVEC3(Ch, 0xa0, WorldCtr); RudeYbn::PF32(Ch, 0xac, RudeYbn::UNK_F2);
		RudeYbn::PPTR(Ch, 0xb0, OVert);
		RudeYbn::PU32(Ch, 0xd0, (uint32)NV); RudeYbn::PU32(Ch, 0xd4, (uint32)NP);
		RudeYbn::PPTR(Ch, 0xf0, OF0);
		RudeYbn::PPTR(Ch, 0x118, OMidx);
		RudeYbn::PU32(Ch, 0x120, 1);
		RudeYbn::PPTR(Ch, 0x130, OBvh);
		RudeYbn::PU16(Ch, 0x140, 0xffff);
		const int32 OChild = Emit(Ch);
		TArray<uint8> CArr; CArr.AddZeroed(8);
		RudeYbn::PPTR(CArr, 0, OChild);
		const int32 OCArr = Emit(CArr);
		TArray<uint8> Comp; Comp.AddZeroed(0xb0);
		RudeYbn::PF32(Comp, 0x00, VertR); RudeYbn::PU32(Comp, 0x04, 1);
		Comp[0x10] = 0x0a;
		RudeYbn::PF32(Comp, 0x14, CornerR);
		RudeYbn::PVEC3(Comp, 0x20, WorldMax); RudeYbn::PF32(Comp, 0x2c, 0.f);
		RudeYbn::PVEC3(Comp, 0x30, WorldMin); RudeYbn::PU32(Comp, 0x3c, 1);
		RudeYbn::PVEC3(Comp, 0x40, WorldCtr);
		RudeYbn::PVEC3(Comp, 0x50, WorldCtr);
		RudeYbn::PF32(Comp, 0x60, 1.f); RudeYbn::PF32(Comp, 0x64, 1.f);
		RudeYbn::PF32(Comp, 0x68, 1.f); RudeYbn::PF32(Comp, 0x6c, 1.f);
		RudeYbn::PPTR(Comp, 0x70, OCArr);
		RudeYbn::PPTR(Comp, 0x78, OXf); RudeYbn::PPTR(Comp, 0x80, OXf);
		RudeYbn::PPTR(Comp, 0x88, OCBox);
		RudeYbn::PPTR(Comp, 0x90, OF1); RudeYbn::PPTR(Comp, 0x98, OF2);
		RudeYbn::PU16(Comp, 0xa0, 1); RudeYbn::PU16(Comp, 0xa2, 1);
		OComposite = Emit(Comp);
	}

	// --- gtaDrawable header @0 ---
	RudeYbn::PU32(Seg, 0x00, 0x40573178u); RudeYbn::PU32(Seg, 0x04, 1);   // VFT 0x140573178
	RudeYbn::PPTR(Seg, 0x08, OBm);
	RudeYbn::PPTR(Seg, 0x10, OSG);
	{
		const float C[3] = { Center.X, Center.Y, Center.Z };
		const float Mn[3] = { BMin.X, BMin.Y, BMin.Z }, Mx[3] = { BMax.X, BMax.Y, BMax.Z };
		RudeYbn::PVEC3(Seg, 0x20, C); RudeYbn::PF32(Seg, 0x2c, Radius);
		RudeYbn::PVEC3(Seg, 0x30, Mn); RudeYbn::PU32(Seg, 0x3c, 0x7f800001u);
		RudeYbn::PVEC3(Seg, 0x40, Mx); RudeYbn::PU32(Seg, 0x4c, 0x7f800001u);
	}
	RudeYbn::PPTR(Seg, 0x50, OModelsHdr);
	for (int32 k = 0; k < 4; ++k) { RudeYbn::PF32(Seg, 0x70 + k*4, 9998.f); }
	RudeYbn::PU32(Seg, 0x80, 0x0000ff01u);
	RudeYbn::PU16(Seg, 0x9a, 0x0012);
	RudeYbn::PPTR(Seg, 0xa0, OModelsHdr);
	RudeYbn::PPTR(Seg, 0xa8, OName);
	RudeYbn::PPTR(Seg, 0xc8, OComposite);

	// --- container: RSC7 v165, sys hi-nibble 0xa, gfx 0x5 (gfx=0), uniform pages of PAGE ---
	uint32 Padded = 0, NPages = 0;
	const uint32 LowFlags = RudeYbn::SysPageFlagsUniform((uint32)Seg.Num(), (uint32)PAGE, Padded, NPages);
	if (LowFlags == 0xFFFFFFFFu) { return Fail(TEXT("unencodable page plan - resource too large")); }
	const uint32 SysFlag = 0xa0000000u | LowFlags;
	const uint32 GfxFlag = 0x50000000u;
	Seg.SetNumZeroed((int32)Padded);
	RudeYbn::PU32(Seg, OBm + 0x08, NPages);   // blockmap page count

	if (bPageOverflow)
	{
		return Fail(TEXT("internal: a block exceeded the page size and would span a page boundary ")
		            TEXT("(no-span law) - a block was added without feeding the page plan"));
	}

	// ⭐ SELF-VERIFY BEFORE WRITING. Every check here maps to an in-game crash already paid for:
	// single ownership (#6), the geoBounds N+1 / +0x2e geometry-count law (①), and vertex
	// declarations that decode to the declared stride. Wired in rather than left to an offline
	// script, because a gate nobody remembers to run is not a gate. Refuse rather than emit - a
	// corrupt resource costs an editor rebuild plus a game restart to diagnose.
	{
		const RudeYdrBin::FVerify V = RudeYdrBin::VerifyDrawable(Seg);
		if (V.SharedBlocks > 0 || V.DeclBad > 0 || V.BoundsBad > 0)
		{
			return Fail(FString::Printf(
				TEXT("SELF-CHECK FAILED, refusing to write: %d shared block(s), %d bad declaration(s), ")
				TEXT("%d bounds/count problem(s). First: %s"),
				V.SharedBlocks, V.DeclBad, V.BoundsBad, *V.FirstProblem));
		}
	}

	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Seg.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Seg.GetData(), Seg.Num()))
	{
		return Fail(TEXT("zlib compress failed"));
	}
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream")); }
	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V>>8) & 0xFF); Out.Add((V>>16) & 0xFF); Out.Add((V>>24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(165); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + 2, ZSize - 6);
	if (!FFileHelper::SaveArrayToFile(Out, *OutYdrPath)) { return Fail(TEXT("write .ydr failed")); }

	int32 TotalVerts = 0, TotalTris = 0;
	for (const FGeo& G : Geos) { TotalVerts += G.Pos.Num(); TotalTris += G.Indices.Num() / 3; }
	FString SubList;
	for (const FString& S : SubstitutedPresets)
	{
		SubList += (SubList.IsEmpty() ? TEXT("\"") : TEXT(",\"")) + S + TEXT("\"");
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"ydrPath\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"bytes\":%d,")
		TEXT("\"segSize\":%d,\"sysFlags\":\"0x%08x\",\"selfCheck\":\"passed (single-ownership + ")
		TEXT("geoBounds/count + declarations)\",\"presetsSubstitutedToNormalSpec\":[%s]}"),
		*OutYdrPath, Geos.Num(), TotalVerts, TotalTris, Out.Num(), Seg.Num(), SysFlag, *SubList);
#else
	return Fail(TEXT("editor-only"));
#endif
}

FString URudeToolset::ExportYbnBinary(const FString& AssetPath, const FString& OutYbnPath,
                                      const FString& WorldOffset)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
#if WITH_EDITORONLY_DATA
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh) { return Fail(TEXT("StaticMesh not found")); }
	const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
	if (!MeshDesc) { return Fail(TEXT("no MeshDescription on LOD0")); }
	FStaticMeshConstAttributes Attributes(*MeshDesc);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

	// Optional world placement: static map collision stores ABSOLUTE world coords.
	FVector3f Offset(0.f, 0.f, 0.f);
	if (!WorldOffset.TrimStartAndEnd().IsEmpty())
	{
		TArray<FString> C;
		WorldOffset.ParseIntoArray(C, TEXT(","), true);
		if (C.Num() != 3) { return Fail(TEXT("WorldOffset must be \"x,y,z\" (gta world metres)")); }
		Offset = FVector3f(FCString::Atof(*C[0]), FCString::Atof(*C[1]), FCString::Atof(*C[2]));
	}

	// --- collision soup, welded by position, inverse RUDE transform (cm->m, Y mirror) ---
	TArray<FVector3f> Verts;
	TArray<int32> Indices;
	TMap<FString, int32> Weld;
	for (const FTriangleID TriID : MeshDesc->Triangles().GetElementIDs())
	{
		for (const FVertexID VID : MeshDesc->GetTriangleVertices(TriID))
		{
			const FVector3f P = Positions[VID];
			const FVector3f G = FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f) + Offset;
			const FString Key = FString::Printf(TEXT("%.4f,%.4f,%.4f"), G.X, G.Y, G.Z);
			int32 Idx;
			if (const int32* F = Weld.Find(Key)) { Idx = *F; }
			else { Idx = Verts.Num(); Verts.Add(G); Weld.Add(Key, Idx); }
			Indices.Add(Idx);
		}
	}
	const int32 NV = Verts.Num(), NP = Indices.Num() / 3;
	if (NV == 0 || NP == 0) { return Fail(TEXT("no collision geometry")); }
	if (NV > 65535) { return Fail(TEXT("vertex count exceeds 65535 (u16 poly indices) - split the mesh")); }
	// u16 COUNT CEILING (pinned 2026-07-26) - the vertex guard above is NOT sufficient. BVH leaf
	// PolyStart (node+0x0c) and the m_Trees start/end node indices are u16 in the real format;
	// a closed mesh runs ~2 triangles per vertex, so NP wraps at ~32.8k verts - INSIDE the range
	// the vertex guard allows. The result passed every in-code check and emitted a bound whose
	// tail leaves index back into the start of the poly array: silently wrong collision, ok:true.
	// Real large bounds use MULTIPLE composite children; this writer emits exactly one.
	if (NP > 65535) { return Fail(TEXT("triangle count exceeds 65535 (u16 BVH poly index) - split the mesh")); }
	// (page size is derived from the largest block below - the uniform-P pager, same as
	// ExportYdrBinary; no struct may span a page boundary, and none can by construction)

	// --- WORLD aabb + center; vertices are stored RELATIVE to CenterGeom ---
	FVector3f WMin(FLT_MAX), WMax(-FLT_MAX);
	for (const FVector3f& V : Verts) { WMin = WMin.ComponentMin(V); WMax = WMax.ComponentMax(V); }
	const FVector3f Center = (WMin + WMax) * 0.5f;
	TArray<FVector3f> Rel; Rel.Reserve(NV);
	for (const FVector3f& V : Verts) { Rel.Add(V - Center); }

	float RMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, RMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const FVector3f& R : Rel)
	{
		for (int32 a = 0; a < 3; ++a) { RMin[a] = FMath::Min(RMin[a], R[a]); RMax[a] = FMath::Max(RMax[a], R[a]); }
	}
	float Quant[3], Half[3];
	for (int32 a = 0; a < 3; ++a)
	{
		Half[a] = FMath::Max(FMath::Abs(RMin[a]), FMath::Abs(RMax[a]));
		Quant[a] = (Half[a] > 0.f) ? (Half[a] / 32767.0f) : 1.0f;
	}
	const float CornerR = FMath::Sqrt(Half[0]*Half[0] + Half[1]*Half[1] + Half[2]*Half[2]);  // +0x14
	float VertR = 0.f;                                                                       // +0x00
	for (const FVector3f& R : Rel) { VertR = FMath::Max(VertR, R.Size()); }
	const float WorldMin[3] = { WMin.X, WMin.Y, WMin.Z };
	const float WorldMax[3] = { WMax.X, WMax.Y, WMax.Z };
	const float WorldCtr[3] = { Center.X, Center.Y, Center.Z };

	// --- BVH ---
	TArray<FVector3f> TriCtr; TriCtr.Reserve(NP);
	for (int32 j = 0; j < NP; ++j)
	{
		TriCtr.Add((Rel[Indices[j*3]] + Rel[Indices[j*3+1]] + Rel[Indices[j*3+2]]) / 3.0f);
	}
	TArray<int32> Order; Order.Reserve(NP);
	for (int32 j = 0; j < NP; ++j) { Order.Add(j); }
	TArray<RudeYbn::FBvhNode> Nodes; TArray<int32> PolyOrder;
	RudeYbn::BuildBvh(Rel, Indices, TriCtr, Order, 0, NP, Nodes, PolyOrder);
	RudeYbn::SetEscape(Nodes, 0);
	if (PolyOrder.Num() != NP) { return Fail(TEXT("BVH leaf coverage broken")); }

	// m_Trees: cut the flat stackless BVH into its maximal subtrees of <= MAX_NODES_PER_TREE
	// nodes (each entry = (startNode, endNode) absolute). MANDATORY: every real ybn has this
	// array; its absence was the final "Invalid fixup" root cause (the loader fixes up the
	// m_Trees pointer at bvhHdr+0x70, which our 0x60-byte header did not contain).
	TArray<TPair<int32, int32>> Trees;
	{
		TArray<int32> Stack; Stack.Add(0);
		while (Stack.Num() > 0)
		{
			const int32 i = Stack.Pop();
			const int32 Size = Nodes[i].Escape - i;   // Escape is absolute here: subtree = [i, Escape)
			if (Size <= RudeYbn::MAX_NODES_PER_TREE || Nodes[i].bLeaf)
			{
				Trees.Add(TPair<int32, int32>(i, Nodes[i].Escape));
			}
			else
			{
				// right child first so the stack pops left-to-right (entries stay in node order)
				Stack.Add(Nodes[i + 1].Escape);
				Stack.Add(i + 1);
			}
		}
	}

	// node quantization (relative to center, same frame as the stored vertices)
	float NBMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, NBMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const RudeYbn::FBvhNode& N : Nodes)
	{
		for (int32 a = 0; a < 3; ++a) { NBMin[a] = FMath::Min(NBMin[a], N.Lo[a]); NBMax[a] = FMath::Max(NBMax[a], N.Hi[a]); }
	}
	float NQ[3], NInv[3];
	for (int32 a = 0; a < 3; ++a)
	{
		const float M = FMath::Max(FMath::Abs(NBMin[a]), FMath::Abs(NBMax[a]));
		NQ[a] = (M > 0.f) ? (M / 32767.0f) : 1.0f;
		NInv[a] = (NQ[a] > 0.f) ? (1.0f / NQ[a]) : 0.f;
	}
	auto QuantS16 = [](float V, float Q) -> int16
	{ return (int16)FMath::Clamp<int32>(FMath::RoundToInt(V / Q), -32768, 32767); };

	// --- geometry blocks ---
	TArray<uint8> PolyBytes; PolyBytes.AddZeroed(NP * 16);
	for (int32 k = 0; k < NP; ++k)
	{
		const int32 j = PolyOrder[k];
		const FVector3f& A = Rel[Indices[j*3]];
		const FVector3f& B = Rel[Indices[j*3+1]];
		const FVector3f& C = Rel[Indices[j*3+2]];
		const float Area = 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
		uint32 AreaBits; FMemory::Memcpy(&AreaBits, &Area, 4);
		AreaBits &= 0xFFFFFFF8u;                       // low 3 bits = polygon TYPE (0 = triangle)
		RudeYbn::PU32(PolyBytes, k*16, AreaBits);
		RudeYbn::PU16(PolyBytes, k*16+4, (uint16)Indices[j*3]);
		RudeYbn::PU16(PolyBytes, k*16+6, (uint16)Indices[j*3+1]);
		RudeYbn::PU16(PolyBytes, k*16+8, (uint16)Indices[j*3+2]);
		// +10/+12/+14 = edge-neighbour indices (0 = none; adjacency is a later refinement)
	}
	TArray<uint8> VertBytes; VertBytes.AddZeroed(NV * 6);
	for (int32 i = 0; i < NV; ++i)
	{
		for (int32 a = 0; a < 3; ++a) { RudeYbn::PS16(VertBytes, i*6 + a*2, QuantS16(Rel[i][a], Quant[a])); }
	}
	TArray<uint8> MatIdxBytes; MatIdxBytes.AddZeroed(NP);          // u8 material index per poly (0)
	TArray<uint8> NodeBytes; NodeBytes.AddZeroed(Nodes.Num() * 16);
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const RudeYbn::FBvhNode& N = Nodes[i];
		for (int32 a = 0; a < 3; ++a)
		{
			RudeYbn::PS16(NodeBytes, i*16 + a*2,     QuantS16(N.Lo[a], NQ[a]));
			RudeYbn::PS16(NodeBytes, i*16 + 6 + a*2, QuantS16(N.Hi[a], NQ[a]));
		}
		// leaf: (polyStart, polyCount) ; internal: (RELATIVE escape, 0)
		RudeYbn::PU16(NodeBytes, i*16 + 12, (uint16)(N.bLeaf ? N.PolyStart : (N.Escape - i)));
		RudeYbn::PU16(NodeBytes, i*16 + 14, (uint16)(N.bLeaf ? N.PolyCount : 0));
	}

	// --- system segment: composite header reserved @0, then blocks, then structs ---
	TArray<uint8> Seg; Seg.AddZeroed(0xb0);
	// Page-aware emit: NO struct/array may span a page boundary (RAGE pages are independently
	// relocatable; a straddling block = "Invalid fixup"/torn-memory crash). Uniform-P pager:
	// page size = pow2 >= the largest single block, min 64KB (preserves the proven small-mesh
	// layout byte-for-byte: for <=64KB blocks this is exactly the old 64KB pager).
	uint32 LargestBlk = 0x10000;
	LargestBlk = FMath::Max(LargestBlk, (uint32)PolyBytes.Num());
	LargestBlk = FMath::Max(LargestBlk, (uint32)NodeBytes.Num());
	LargestBlk = FMath::Max(LargestBlk, (uint32)VertBytes.Num());
	const int32 YBN_PAGE = (int32)FMath::RoundUpToPowerOfTwo(LargestBlk);
	auto Emit = [&Seg, YBN_PAGE](const TArray<uint8>& D, int32 Align = 16) -> int32
	{
		if (Seg.Num() % Align) { Seg.AddZeroed(Align - (Seg.Num() % Align)); }
		if (D.Num() <= YBN_PAGE && (Seg.Num() % YBN_PAGE) + D.Num() > YBN_PAGE)
		{
			Seg.AddZeroed(YBN_PAGE - (Seg.Num() % YBN_PAGE));   // don't straddle a page boundary
		}
		const int32 O = Seg.Num(); Seg.Append(D); return O;
	};
	const int32 OPoly = Emit(PolyBytes);
	const int32 ONode = Emit(NodeBytes);
	const int32 OVert = Emit(VertBytes);
	// NOTE: child +0xb8 (m_CompressedShrunkVertices) is left NULL - CW's known-good binary
	// leaves it null and loads fine, so it is NOT required (an earlier theory that it caused
	// the fixup crash was disproven by diffing CW's working output).
	const int32 OMidx = Emit(MatIdxBytes);

	// m_Trees array: 16 bytes/entry = the root node's quantized AABB (s16 min/max, same
	// quantum+frame as the node array) + u16 startNode + u16 endNode (absolute, exclusive).
	TArray<uint8> TreeBytes; TreeBytes.AddZeroed(Trees.Num() * 16);
	for (int32 t = 0; t < Trees.Num(); ++t)
	{
		const RudeYbn::FBvhNode& RootN = Nodes[Trees[t].Key];
		for (int32 a = 0; a < 3; ++a)
		{
			RudeYbn::PS16(TreeBytes, t*16 + a*2,     QuantS16(RootN.Lo[a], NQ[a]));
			RudeYbn::PS16(TreeBytes, t*16 + 6 + a*2, QuantS16(RootN.Hi[a], NQ[a]));
		}
		RudeYbn::PU16(TreeBytes, t*16 + 12, (uint16)Trees[t].Key);
		RudeYbn::PU16(TreeBytes, t*16 + 14, (uint16)Trees[t].Value);
	}
	const int32 OTrees = Emit(TreeBytes);

	// phOptimizedBvh header is 0x80 bytes (NOT 0x60 - the 0x60 truncation dropped the
	// m_Trees pointer slot and was the final in-game "Invalid fixup" root cause):
	// +0x60 forward quantum vec4, +0x70 m_Trees ptr, +0x78 u16 count / +0x7a u16 capacity.
	TArray<uint8> Bvh; Bvh.AddZeroed(0x80);
	RudeYbn::PPTR(Bvh, 0x00, ONode);
	RudeYbn::PU32(Bvh, 0x08, (uint32)Nodes.Num()); RudeYbn::PU32(Bvh, 0x0c, (uint32)Nodes.Num());
	{
		const float WB0[3] = { NBMin[0]+WorldCtr[0], NBMin[1]+WorldCtr[1], NBMin[2]+WorldCtr[2] };
		const float WB1[3] = { NBMax[0]+WorldCtr[0], NBMax[1]+WorldCtr[1], NBMax[2]+WorldCtr[2] };
		RudeYbn::PVEC3(Bvh, 0x20, WB0); RudeYbn::PU32(Bvh, 0x2c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x30, WB1); RudeYbn::PU32(Bvh, 0x3c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x40, WorldCtr); RudeYbn::PU32(Bvh, 0x4c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x50, NInv); RudeYbn::PU32(Bvh, 0x5c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x60, NQ);   RudeYbn::PU32(Bvh, 0x6c, 0xffc00000u);
		RudeYbn::PPTR(Bvh, 0x70, OTrees);
		RudeYbn::PU16(Bvh, 0x78, (uint16)Trees.Num());
		RudeYbn::PU16(Bvh, 0x7a, (uint16)Trees.Num());
	}
	const int32 OBvh = Emit(Bvh);

	TArray<uint8> Xf; Xf.AddZeroed(0x40);                         // child transform = identity
	RudeYbn::PF32(Xf, 0x00, 1.f); RudeYbn::PF32(Xf, 0x14, 1.f); RudeYbn::PU32(Xf, 0x1c, 1);
	RudeYbn::PF32(Xf, 0x28, 1.f); RudeYbn::PU32(Xf, 0x2c, 1);
	const int32 OXf = Emit(Xf);

	TArray<uint8> F0; F0.AddZeroed(0x20);                          // child+0xf0 block (zero)
	const int32 OF0 = Emit(F0);
	TArray<uint8> BlockMap; BlockMap.AddZeroed(0x40);   // page count patched after the plan is known
	const int32 OBm = Emit(BlockMap);

	TArray<uint8> ChildBox; ChildBox.AddZeroed(0x20);              // [BoxMin.vec4, BoxMax.vec4] WORLD
	RudeYbn::PVEC3(ChildBox, 0x00, WorldMin); RudeYbn::PU32(ChildBox, 0x0c, 1);
	RudeYbn::PVEC3(ChildBox, 0x10, WorldMax); RudeYbn::PF32(ChildBox, 0x1c, RudeYbn::CHILD_MARGIN);
	const int32 OBbox = Emit(ChildBox);
	// ChildrenFlags arrays: 16 bytes PER CHILD (not 4 - CW and real ybns use a 16-byte
	// stride; second word copied from the known-good binary, trailing 8 bytes zero).
	TArray<uint8> Fl1; Fl1.AddZeroed(16);
	RudeYbn::PU32(Fl1, 0, RudeYbn::CHILD_FLAGS1); RudeYbn::PU32(Fl1, 4, RudeYbn::CHILD_FLAGS_PAD);
	const int32 OFl1 = Emit(Fl1);
	TArray<uint8> Fl2; Fl2.AddZeroed(16);
	RudeYbn::PU32(Fl2, 0, RudeYbn::CHILD_FLAGS2); RudeYbn::PU32(Fl2, 4, RudeYbn::CHILD_FLAGS_PAD);
	const int32 OFl2 = Emit(Fl2);

	// --- phBoundGeometryBVH child header (0x150: 0x140 of fields + the 0x0000ffff
	// sentinel at +0x140, present in CW's output and EVERY real corpus ybn) ---
	TArray<uint8> Ch; Ch.AddZeroed(0x150);
	RudeYbn::PF32(Ch, 0x00, VertR); RudeYbn::PU32(Ch, 0x04, 1);
	Ch[0x10] = 0x08;                                               // BoundType = GeometryBVH
	RudeYbn::PF32(Ch, 0x14, CornerR);
	RudeYbn::PVEC3(Ch, 0x20, WorldMax); RudeYbn::PF32(Ch, 0x2c, RudeYbn::CHILD_MARGIN);
	RudeYbn::PVEC3(Ch, 0x30, WorldMin); RudeYbn::PU32(Ch, 0x3c, 1);
	RudeYbn::PVEC3(Ch, 0x40, WorldCtr);
	RudeYbn::PVEC3(Ch, 0x50, WorldCtr);
	RudeYbn::PF32(Ch, 0x60, 1.f); RudeYbn::PF32(Ch, 0x64, 1.f);
	RudeYbn::PF32(Ch, 0x68, 1.f); RudeYbn::PF32(Ch, 0x6c, 1.f);    // Inertia + Volume
	RudeYbn::PU32(Ch, 0x84, (uint32)NV);
	RudeYbn::PPTR(Ch, 0x88, OPoly);
	RudeYbn::PVEC3(Ch, 0x90, Quant); RudeYbn::PF32(Ch, 0x9c, RudeYbn::UNK_F1);
	RudeYbn::PVEC3(Ch, 0xa0, WorldCtr); RudeYbn::PF32(Ch, 0xac, RudeYbn::UNK_F2);  // CenterGeom
	RudeYbn::PPTR(Ch, 0xb0, OVert);                // +0xb8 (shrunk verts) intentionally NULL, matches CW
	RudeYbn::PU32(Ch, 0xd0, (uint32)NV); RudeYbn::PU32(Ch, 0xd4, (uint32)NP);
	RudeYbn::PPTR(Ch, 0xf0, OF0);                  // materials array (one all-zero default material)
	RudeYbn::PPTR(Ch, 0x118, OMidx);
	RudeYbn::PU32(Ch, 0x120, 1);                   // material count = 1 (CW known-good; every real ybn >= 1)
	RudeYbn::PPTR(Ch, 0x130, OBvh);
	RudeYbn::PU16(Ch, 0x140, 0xffff);              // sentinel, universal in CW + real corpus
	const int32 OChild = Emit(Ch);

	TArray<uint8> CArr; CArr.AddZeroed(8); RudeYbn::PPTR(CArr, 0, OChild);
	const int32 OCArr = Emit(CArr);

	// --- phBoundComposite header @0 ---
	RudeYbn::PF32(Seg, 0x00, VertR); RudeYbn::PU32(Seg, 0x04, 1);
	RudeYbn::PPTR(Seg, 0x08, OBm);
	Seg[0x10] = 0x0a;                                              // BoundType = Composite
	RudeYbn::PF32(Seg, 0x14, CornerR);
	RudeYbn::PVEC3(Seg, 0x20, WorldMax); RudeYbn::PF32(Seg, 0x2c, 0.f);   // composite margin = 0
	RudeYbn::PVEC3(Seg, 0x30, WorldMin); RudeYbn::PU32(Seg, 0x3c, 1);
	RudeYbn::PVEC3(Seg, 0x40, WorldCtr);
	RudeYbn::PVEC3(Seg, 0x50, WorldCtr);
	RudeYbn::PF32(Seg, 0x60, 1.f); RudeYbn::PF32(Seg, 0x64, 1.f);
	RudeYbn::PF32(Seg, 0x68, 1.f); RudeYbn::PF32(Seg, 0x6c, 1.f);
	RudeYbn::PPTR(Seg, 0x70, OCArr);
	RudeYbn::PPTR(Seg, 0x78, OXf); RudeYbn::PPTR(Seg, 0x80, OXf);
	RudeYbn::PPTR(Seg, 0x88, OBbox);
	RudeYbn::PPTR(Seg, 0x90, OFl1); RudeYbn::PPTR(Seg, 0x98, OFl2);
	RudeYbn::PU16(Seg, 0xa0, 1); RudeYbn::PU16(Seg, 0xa2, 1);      // NumChildren, capacity

	// --- container: RSC7 v43 (system segment only), uniform pages of YBN_PAGE.
	// For <=64KB blocks this reproduces the in-game-proven plan exactly (rock wall:
	// 2x64KB, flags 0x20000040, byte-identity regression-gated). ---
	uint32 Padded = 0, NPages = 0;
	const uint32 LowFlags = RudeYbn::SysPageFlagsUniform((uint32)Seg.Num(), (uint32)YBN_PAGE, Padded, NPages);
	if (LowFlags == 0xFFFFFFFFu) { return Fail(TEXT("unencodable page plan - mesh too large")); }
	const uint32 SysFlag = 0x20000000u | LowFlags;
	const uint32 GfxFlag = 0xb0000000u;
	Seg.SetNumZeroed((int32)Padded);
	RudeYbn::PU32(Seg, OBm + 0x08, NPages);

	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Seg.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Seg.GetData(), Seg.Num()))
	{
		return Fail(TEXT("zlib compress failed"));
	}
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream (need standard 2-byte header)")); }

	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V>>8) & 0xFF); Out.Add((V>>16) & 0xFF); Out.Add((V>>24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(43); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + 2, ZSize - 6);                        // strip zlib header + adler32

	if (!FFileHelper::SaveArrayToFile(Out, *OutYbnPath)) { return Fail(TEXT("write .ybn failed")); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"ybnPath\":\"%s\",\"vertices\":%d,\"triangles\":%d,\"bvhNodes\":%d,\"bytes\":%d,\"segSize\":%d,\"sysFlags\":\"0x%08x\"}"),
		*OutYbnPath, NV, NP, Nodes.Num(), Out.Num(), Seg.Num(), SysFlag);
#else
	return Fail(TEXT("editor-only"));
#endif
}

// ======================= ProbeYdrBinary - verify the binary READ path =======================
// Step 1 of the import side: prove the C++ parse of a real binary .ydr before wiring it to the
// mesh builder. Reports the whole drawable graph as JSON so it can be diffed against an
// independent parse and against ExportYdrBinary's own output (round-trip).
FString URudeToolset::ProbeYdrBinary(const FString& BinPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	RudeYdrBin::FRes R;
	FString Err;
	if (!RudeYdrBin::LoadFile(BinPath, R, Err)) { return Fail(Err); }
	const TArray<uint8>& S = R.Sys;

	uint32 PtrSG = 0, PtrModels = 0, PtrName = 0, PtrBound = 0;
	RudeYdrBin::U32(S, 0x10, PtrSG);
	RudeYdrBin::U32(S, 0x50, PtrModels);
	RudeYdrBin::U32(S, 0xa8, PtrName);
	RudeYdrBin::U32(S, 0xc8, PtrBound);

	// drawable name (plain ASCII, NUL-terminated)
	FString DrawName;
	{
		int32 O = 0;
		if (R.Resolve(PtrName, 1, O))
		{
			while (O < S.Num() && S[O] != 0 && DrawName.Len() < 128) { DrawName.AppendChar((TCHAR)S[O++]); }
		}
	}

	// --- shaders: hash + the texture-stub names each one references ---
	int32 NumShaders = 0;
	FString ShaderJson;
	if (PtrSG != 0)
	{
		int32 SG = 0;
		if (!R.Resolve(PtrSG, 0x40, SG)) { return Fail(TEXT("ShaderGroup pointer does not resolve")); }
		uint32 PtrArr = 0; uint16 NSh = 0;
		RudeYdrBin::U32(S, SG + 0x10, PtrArr);
		RudeYdrBin::U16(S, SG + 0x18, NSh);
		int32 Arr = 0;
		if (NSh > 0 && R.Resolve(PtrArr, (int32)NSh * 8, Arr))
		{
			NumShaders = (int32)NSh;
			for (int32 si = 0; si < NSh; ++si)
			{
				uint32 PtrBlk = 0;
				RudeYdrBin::U32(S, Arr + si * 8, PtrBlk);
				int32 Blk = 0;
				if (!R.Resolve(PtrBlk, 0x30, Blk)) { continue; }
				uint32 Hash = 0, NPar = 0, PtrTbl = 0;
				RudeYdrBin::U32(S, Blk + 0x08, Hash);
				RudeYdrBin::U32(S, Blk + 0x10, NPar);
				RudeYdrBin::U32(S, Blk + 0x00, PtrTbl);
				const int32 ParCount = (int32)(NPar & 0xFFFF);
				FString Texs;
				int32 Tbl = 0;
				if (ParCount > 0 && ParCount <= 64 && R.Resolve(PtrTbl, ParCount * 16, Tbl))
				{
					for (int32 pi = 0; pi < ParCount; ++pi)
					{
						uint32 PtrVal = 0;
						RudeYdrBin::U32(S, Tbl + pi * 16 + 8, PtrVal);
						int32 Stub = 0;
						if (!R.Resolve(PtrVal, 0x34, Stub)) { continue; }
						uint32 Marker = 0;
						RudeYdrBin::U32(S, Stub + 0x30, Marker);
						if (Marker != 0x00020001u) { continue; }   // not a grcTexture stub
						uint32 PtrTexName = 0;
						RudeYdrBin::U32(S, Stub + 0x28, PtrTexName);
						int32 NO = 0;
						if (!R.Resolve(PtrTexName, 1, NO)) { continue; }
						FString TN;
						while (NO < S.Num() && S[NO] != 0 && TN.Len() < 96) { TN.AppendChar((TCHAR)S[NO++]); }
						if (!TN.IsEmpty()) { Texs += (Texs.IsEmpty() ? TEXT("\"") : TEXT(",\"")) + TN + TEXT("\""); }
					}
				}
				ShaderJson += FString::Printf(
					TEXT("%s{\"hash\":\"0x%08x\",\"params\":%d,\"textures\":[%s]}"),
					si ? TEXT(",") : TEXT(""), Hash, ParCount, *Texs);
			}
		}
	}

	// --- models -> geometries, over ALL FOUR LOD arrays ---
	// +0x50 High, +0x58 Med, +0x60 Low, +0x68 Vlow are all real {ptrArray*, u16 count} arrays.
	// Walking only +0x50 loses 21.2% of the corpus's geometries (3,677 of 17,370). ⚠ +0xa0 is a
	// byte-identical ALIAS of +0x50 in 3,479/3,479 files - walking it too would double-count.
	static const int32 LodSlot[4] = { 0x50, 0x58, 0x60, 0x68 };
	static const TCHAR* LodName[4] = { TEXT("high"), TEXT("med"), TEXT("low"), TEXT("vlow") };

	int32 TotalGeo = 0, TotalVerts = 0, TotalTris = 0, BadIdx = 0;
	int32 DeclOk = 0, DeclBad = 0, PosInAabb = 0, PosOutAabb = 0, NanVerts = 0, NoNormal = 0;
	FString FirstDeclError;
	TSet<FString> Decls;
	FString ModelJson;
	bool bAnyModels = false;

	for (int32 lod = 0; lod < 4; ++lod)
	{
	uint32 PtrLod = 0;
	RudeYdrBin::U32(S, LodSlot[lod], PtrLod);
	if (PtrLod == 0) { continue; }
	int32 MH = 0;
	if (!R.Resolve(PtrLod, 0x10, MH)) { continue; }
	uint32 PtrMArr = 0; uint16 NMod = 0;
	RudeYdrBin::U32(S, MH + 0x00, PtrMArr);
	RudeYdrBin::U16(S, MH + 0x08, NMod);
	int32 MArr = 0;
	if (NMod == 0 || !R.Resolve(PtrMArr, (int32)NMod * 8, MArr)) { continue; }
	bAnyModels = true;

	for (int32 mi = 0; mi < NMod; ++mi)
	{
		uint32 PtrM = 0;
		RudeYdrBin::U32(S, MArr + mi * 8, PtrM);
		int32 M = 0;
		if (!R.Resolve(PtrM, 0x30, M)) { continue; }
		uint32 PtrGArr = 0, PtrGB = 0, Rm = 0;
		uint16 NGeo = 0, NGeo2e = 0;
		RudeYdrBin::U32(S, M + 0x08, PtrGArr);
		RudeYdrBin::U16(S, M + 0x10, NGeo);
		RudeYdrBin::U32(S, M + 0x18, PtrGB);
		RudeYdrBin::U32(S, M + 0x2c, Rm);
		RudeYdrBin::U16(S, M + 0x2e, NGeo2e);
		int32 GArr = 0;
		if (NGeo == 0 || !R.Resolve(PtrGArr, (int32)NGeo * 8, GArr)) { continue; }

		// geoBounds is N+1 vec4-pairs when N>1 (union first), exactly 1 pair when N==1
		const int32 Pairs = (NGeo > 1) ? (NGeo + 1) : 1;
		int32 GB = 0;
		const bool bGB = R.Resolve(PtrGB, Pairs * 0x20, GB) != nullptr;

		FString GeoJson;
		for (int32 gi = 0; gi < NGeo; ++gi)
		{
			uint32 PtrG = 0;
			RudeYdrBin::U32(S, GArr + gi * 8, PtrG);
			int32 G = 0;
			if (!R.Resolve(PtrG, 0x80, G)) { continue; }
			uint32 PtrVB = 0, PtrIB = 0, IdxCount = 0, TriCount = 0;
			uint16 VCnt = 0, Stride16 = 0;
			RudeYdrBin::U32(S, G + 0x18, PtrVB);
			RudeYdrBin::U32(S, G + 0x38, PtrIB);
			RudeYdrBin::U32(S, G + 0x58, IdxCount);
			RudeYdrBin::U32(S, G + 0x5c, TriCount);
			RudeYdrBin::U16(S, G + 0x60, VCnt);
			// ⚠ grmGeometry+0x70 is a U16, not a u32. +0x72 is non-zero on skinned geometries
			// (mask 0x405f), where a u32 read yields 983,100 instead of 60 - prop_bin_14b.ydr.
			RudeYdrBin::U16(S, G + 0x70, Stride16);
			const uint32 Stride = (uint32)Stride16;

			// vertex buffer + its declaration
			uint32 Mask = 0; uint16 FvfStride = 0; uint8 ChanCount = 0; uint64 Nibbles = 0;
			int32 VB = 0, VData = 0; bool bVGfx = false, bHaveVData = false;
			if (R.Resolve(PtrVB, 0x40, VB))
			{
				uint32 PtrVData = 0, PtrFvf = 0;
				RudeYdrBin::U32(S, VB + 0x10, PtrVData);
				RudeYdrBin::U32(S, VB + 0x30, PtrFvf);
				bVGfx = (PtrVData >> 28) == 6;
				bHaveVData = R.Resolve(PtrVData, (int32)VCnt * (int32)FMath::Max(Stride, 1u), VData) != nullptr;
				int32 Fvf = 0;
				if (R.Resolve(PtrFvf, 0x10, Fvf))
				{
					RudeYdrBin::U32(S, Fvf + 0x00, Mask);
					RudeYdrBin::U16(S, Fvf + 0x04, FvfStride);
					uint8 CC = 0; RudeYdrBin::Rd(S, Fvf + 0x07, &CC, 1); ChanCount = CC;
					RudeYdrBin::Rd(S, Fvf + 0x08, &Nibbles, 8);
				}
			}
			Decls.Add(FString::Printf(TEXT("mask=0x%x,stride=%u,chans=%u"), Mask, Stride, ChanCount));

			// --- DECODE the declaration and verify positions against this geometry's own AABB ---
			// This is the gate that catches a wrong layout: a misread position lands outside the
			// box the file itself declares. 17,370/17,370 real geometries pass, so anything less
			// than 100% here means the decode is wrong, not the data.
			RudeYdrBin::FDecl Decl;
			FString DeclErr;
			const bool bDecl = RudeYdrBin::BuildDecl(Mask, Nibbles, (int32)Stride, Decl, DeclErr);
			if (bDecl) { ++DeclOk; } else { ++DeclBad; if (FirstDeclError.IsEmpty()) { FirstDeclError = DeclErr; } }
			if (bDecl && !Decl.Has(RudeYdrBin::CH_NRM)) { ++NoNormal; }
			if (bDecl && bHaveVData && bGB)
			{
				// per-geometry AABB: pair[gi+1] when N>1 (pair[0] is the union), else pair[0]
				const int32 PairOfs = GB + ((NGeo > 1) ? (gi + 1) : 0) * 0x20;
				float Mn[3], Mx[3];
				bool bBox = true;
				for (int32 a = 0; a < 3; ++a)
				{
					bBox &= RudeYdrBin::F32(S, PairOfs + a * 4, Mn[a]);
					bBox &= RudeYdrBin::F32(S, PairOfs + 0x10 + a * 4, Mx[a]);
				}
				if (bBox)
				{
					const int32 PO = Decl.Ofs[RudeYdrBin::CH_POS];
					for (int32 v = 0; v < (int32)VCnt; ++v)
					{
						float P[3];
						bool bR = true;
						for (int32 a = 0; a < 3; ++a) { bR &= RudeYdrBin::F32(S, VData + v * (int32)Stride + PO + a * 4, P[a]); }
						if (!bR) { break; }
						if (!FMath::IsFinite(P[0]) || !FMath::IsFinite(P[1]) || !FMath::IsFinite(P[2]))
						{
							++NanVerts; continue;   // real shipped assets contain NaN verts
						}
						const float E = 0.01f;
						const bool bIn = P[0] >= Mn[0]-E && P[0] <= Mx[0]+E && P[1] >= Mn[1]-E
						              && P[1] <= Mx[1]+E && P[2] >= Mn[2]-E && P[2] <= Mx[2]+E;
						if (bIn) { ++PosInAabb; } else { ++PosOutAabb; }
					}
				}
			}

			// index buffer: confirm u16 indices stay inside the vertex count
			int32 MaxIdx = -1;
			int32 IB = 0;
			if (R.Resolve(PtrIB, 0x20, IB))
			{
				uint32 PtrIData = 0, IBCount = 0;
				RudeYdrBin::U32(S, IB + 0x08, IBCount);
				RudeYdrBin::U32(S, IB + 0x10, PtrIData);
				int32 IData = 0;
				const int32 NIdx = (int32)FMath::Min(IdxCount, IBCount);
				if (NIdx > 0 && R.Resolve(PtrIData, NIdx * 2, IData))
				{
					for (int32 k = 0; k < NIdx; ++k)
					{
						uint16 V = 0; RudeYdrBin::U16(S, IData + k * 2, V);
						MaxIdx = FMath::Max(MaxIdx, (int32)V);
					}
				}
			}
			if (MaxIdx >= (int32)VCnt) { ++BadIdx; }

			GeoJson += FString::Printf(
				TEXT("%s{\"geo\":%d,\"verts\":%u,\"tris\":%u,\"stride\":%u,\"fvfStride\":%u,")
				TEXT("\"mask\":\"0x%x\",\"chans\":%u,\"maxIdx\":%d,\"vertsInGfxSeg\":%s}"),
				gi ? TEXT(",") : TEXT(""), gi, VCnt, TriCount, Stride, FvfStride, Mask, ChanCount,
				MaxIdx, bVGfx ? TEXT("true") : TEXT("false"));
			TotalVerts += (int32)VCnt; TotalTris += (int32)TriCount; ++TotalGeo;
		}
		ModelJson += FString::Printf(
			TEXT("%s{\"lod\":\"%s\",\"model\":%d,\"geoCount\":%u,\"countAt0x2e\":%u,\"renderMask\":%u,")
			TEXT("\"geoBoundsPairs\":%d,\"geoBoundsResolves\":%s,\"geos\":[%s]}"),
			ModelJson.IsEmpty() ? TEXT("") : TEXT(","), LodName[lod], mi, NGeo, NGeo2e, Rm & 0xFF,
			Pairs, bGB ? TEXT("true") : TEXT("false"), *GeoJson);
	}
	}   // LOD arrays

	if (!bAnyModels) { return Fail(TEXT("no model arrays resolve at hdr+0x50/58/60/68")); }

	FString DeclJson;
	for (const FString& D : Decls) { DeclJson += (DeclJson.IsEmpty() ? TEXT("\"") : TEXT(",\"")) + D + TEXT("\""); }

	// Run the same single-ownership gate the writer uses, so ANY ydr can be audited - not just one
	// we just built. ⚠ ADVISORY here, not a verdict: sharing a grcTexture stub is LEGAL in
	// EMBEDDED-texdict mode (the pgDictionary owns the texture once), which real R* files use, so a
	// non-zero count on a game file is not necessarily a defect. The HARD refusal stays in
	// ExportYdrBinary, where we know we emit external-ytd stubs and sharing is always wrong.
	const RudeYdrBin::FVerify Vf = RudeYdrBin::VerifyDrawable(R.Sys);

	return FString::Printf(
		TEXT("{\"ok\":true,\"name\":\"%s\",\"version\":%u,\"sysSize\":%d,\"gfxSize\":%d,")
		TEXT("\"sharedBlocks\":%d,\"declsRejected\":%d,\"boundsProblems\":%d,\"firstProblem\":\"%s\",")
		TEXT("\"hasEmbeddedBound\":%s,\"shaderCount\":%d,\"geometries\":%d,")
		TEXT("\"vertices\":%d,\"triangles\":%d,\"indicesOutOfRange\":%d,")
		TEXT("\"declsDecoded\":%d,\"declsUnsupported\":%d,\"declError\":\"%s\",")
		TEXT("\"posInAabb\":%d,\"posOutOfAabb\":%d,\"nanVerts\":%d,\"geosWithoutNormal\":%d,")
		TEXT("\"declarations\":[%s],\"shaders\":[%s],\"detail\":[%s]}"),
		*DrawName, R.Version, R.Sys.Num(), R.Gfx.Num(),
		Vf.SharedBlocks, Vf.DeclBad, Vf.BoundsBad, *Vf.FirstProblem,
		PtrBound ? TEXT("true") : TEXT("false"), NumShaders, TotalGeo,
		TotalVerts, TotalTris, BadIdx,
		DeclOk, DeclBad, *FirstDeclError,
		PosInAabb, PosOutAabb, NanVerts, NoNormal,
		*DeclJson, *ShaderJson, *ModelJson);
}
