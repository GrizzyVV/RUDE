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
#include "Materials/MaterialExpressionAppendVector.h"
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
		// The same ONE rule (RudeGeneratedMasterHealth): this master's condition is a Normal TEXTURE
		// parameter. A poorer legacy asset has its graph wiped and rebuilt rather than having
		// expressions bolted onto an unknown one - this is OUR asset and fully regenerable.
		FString StaleWhy;
		if (RudeGeneratedMasterHealth(M, TEXT("M_RUDE_Cutout"), StaleWhy) != ERudeMasterHealth::Stale) { return M; }
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
// which ROUT was dropping along with every other non-texture shader parameter.
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
	bool bLivery = false;                   // a second diffuse (vehicle_paint*'s DiffuseSampler2: the livery) over the base
	int32 Bucket = 0;                       // 0 opaque - 1 alpha - 2 decal - 3 cutout

	FString Key() const                     // stable, readable asset name
	{
		FString K = TEXT("D");
		if (bNormal) { K += TEXT("N"); }
		if (bSpec)   { K += TEXT("S"); }
		if (bDetail) { K += TEXT("Dt"); }
		if (bTint)   { K += TEXT("T"); }
		if (bEmissive) { K += TEXT("E"); }
		if (bLivery) { K += TEXT("L"); }
		return FString::Printf(TEXT("M_RUDE_%s_b%d"), *K, Bucket);
	}
};

// ⛔ THE GRAPH VERSION - BUMP THIS ON EVERY CHANGE TO EnsureGeneratedMaster's GRAPH.
// The staleness rule below used to probe for NAMED PARAMETERS ("does this master declare
// TintAmount?"). That can only see a change that ADDS OR REMOVES A PARAMETER; it is blind to a
// change in the WIRING. Measured 2026-09-07: the alpha-mask fix (Connect() instead of a raw
// Expression assignment, in both the tint and livery branches) added no parameter, so every
// RegenerateMasters after it reported `regenerated: 0` and NOT ONE existing master was rebuilt -
// the fix reached the code and never reached an asset. The diagnostic that was supposed to prove
// the graph reaches the screen (BaseColor multiplied by 0.05) died the same way and read as
// "a master's graph has no observable effect", which was never what the run measured.
// So the version is STAMPED INTO THE ASSET as an unconnected scalar and compared here. A wiring
// change now only needs this number bumped, and the rule cannot silently miss it again.
//   1 = every master generated before the stamp existed (they read -1 and are all stale)
//   2 = the alpha-mask fix in the tint and livery branches (2026-09-07)
//   3 = detailSettings .zw read off their own pins - every Dt master had been FAILING TO COMPILE (2026-09-08)
static const int32 RudeMasterGraphVersion = 3;
static const TCHAR* RudeMasterGraphVersionParam = TEXT("RudeGraphVersion");

// ---- THE definition of "this master RUDE generates is stale" ----------------------------------
// ONE rule, in ONE place, because there were three and they disagreed: each generator below kept
// its own condition, and RudeDoctor RESTATED one of them in order to report it - so when the tint
// condition was added on 2026-09-07 the doctor kept reporting every tint master healthy. All three
// generators with an upgrade rule now ask this function, and so does the doctor.
// `AssetName` is exactly what FRudeMasterSpec::Key() spells for a generated master, so the decode
// here is that function's inverse - which is why the doctor can start from an asset on disk and
// get the same answer the generator would.
// Unreadable (NOT Healthy) when the material will not load or the name is not one this rule
// covers: a master we did not check is never reported as one that passed. The other four named
// masters (DecalGeo, Foliage, Terrain, Water) have no upgrade rule at all - their generators
// return any existing asset - so they are deliberately Unreadable here rather than given a
// condition this lane invented.
ERudeMasterHealth RudeGeneratedMasterHealth(UMaterial* M, const FString& AssetName, FString& OutWhy)
{
	OutWhy.Reset();
	if (!M) { return ERudeMasterHealth::Unreadable; }

	TArray<FMaterialParameterInfo> ScalarInfos, TextureInfos;
	TArray<FGuid> ScalarIds, TextureIds;
	M->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);
	M->GetAllTextureParameterInfo(TextureInfos, TextureIds);
	auto Has = [](const TArray<FMaterialParameterInfo>& Infos, const TCHAR* Param)
	{
		for (const FMaterialParameterInfo& I : Infos) { if (I.Name == FName(Param)) { return true; } }
		return false;
	};

	// The two NAMED masters the generators also upgrade in place.
	if (AssetName == TEXT("M_RUDE_Detail"))
	{
		if (Has(ScalarInfos, TEXT("DetailAmount"))) { return ERudeMasterHealth::Healthy; }
		OutWhy = TEXT("detail master with no DetailAmount");
		return ERudeMasterHealth::Stale;
	}
	if (AssetName == TEXT("M_RUDE_Cutout"))
	{
		if (Has(TextureInfos, TEXT("Normal"))) { return ERudeMasterHealth::Healthy; }
		OutWhy = TEXT("cutout master with no Normal texture parameter");
		return ERudeMasterHealth::Stale;
	}

	// Everything else this rule judges is M_RUDE_<letters>_b<bucket>.
	FString Sig, BucketText;
	if (!AssetName.StartsWith(TEXT("M_RUDE_"), ESearchCase::CaseSensitive)
		|| !AssetName.Mid(7).Split(TEXT("_b"), &Sig, &BucketText) || !BucketText.IsNumeric())
	{
		return ERudeMasterHealth::Unreadable;
	}
	const int32 Bucket = FCString::Atoi(*BucketText);

	// RULE 1 (2026-09-05). Bucket-1 (glass, alpha-blended) masters generated before that date used
	// UE's default volumetric translucency with no surface lighting: glass drew as a flat colour
	// slab (the blue tower). The new graph carries OpacityScale.
	if (Bucket == 1 && !Has(ScalarInfos, TEXT("OpacityScale")))
	{
		OutWhy = TEXT("bucket-1 glass master with no OpacityScale");
		return ERudeMasterHealth::Stale;
	}

	// RULE 2 (2026-09-07). A TINT master generated before that date exposed TintPalette and a
	// selector and wired NEITHER - the palette was bound and never sampled. The probe is
	// TintAmount, which only the new graph declares. 'T' is the tint letter, and "Dt" (detail)
	// also contains a t, so the detail pair is removed first - CASE-SENSITIVELY, because
	// FString::Replace and FString::Contains both default to IgnoreCase.
	const FString TintLetters = Sig.Replace(TEXT("Dt"), TEXT(""), ESearchCase::CaseSensitive);
	if (TintLetters.Contains(TEXT("T"), ESearchCase::CaseSensitive) && !Has(ScalarInfos, TEXT("TintAmount")))
	{
		OutWhy = TEXT("tint master with no TintAmount");
		return ERudeMasterHealth::Stale;
	}

	// RULE 0, and the LAST word (2026-09-07). Rules 1 and 2 above are kept because they name a
	// specific historical defect, which is a better message than a version number when they apply.
	// This one catches everything they cannot: ANY change to the graph's wiring. The stamp is read
	// off the EXPRESSION COLLECTION rather than GetAllScalarParameterInfo, because the parameter is
	// deliberately unconnected and an unreferenced parameter is not guaranteed to survive into the
	// compiled parameter list - the expression it was built from always does.
	// A master with no stamp reads -1, which is every master generated before this rule: stale, and
	// regenerated in place, so the instances parented to it pick the fix up with no re-import.
	int32 Stamped = -1;
	for (UMaterialExpression* E : M->GetExpressionCollection().Expressions)
	{
		if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(E))
		{
			if (SP->ParameterName == FName(RudeMasterGraphVersionParam))
			{
				Stamped = FMath::RoundToInt(SP->DefaultValue);
				break;
			}
		}
	}
	if (Stamped != RudeMasterGraphVersion)
	{
		OutWhy = FString::Printf(TEXT("graph version %d, builder writes %d"),
			Stamped, RudeMasterGraphVersion);
		return ERudeMasterHealth::Stale;
	}
	return ERudeMasterHealth::Healthy;
}

static UMaterialInterface* EnsureGeneratedMaster(const FRudeMasterSpec& Spec)
{
	const FString Name = Spec.Key();
	const FString PkgName = FString::Printf(TEXT("/RUDE/Masters/Gen/%s"), *Name);
	const FString Full = FString::Printf(TEXT("%s.%s"), *PkgName, *Name);
	UMaterial* M = nullptr;
	if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, *Full))
	{
		// THE staleness rule lives in ONE function - RudeGeneratedMasterHealth, just above - and the
		// reporter (RudeDoctor) calls the same one. It used to be restated there, which is how the
		// doctor went on calling every tint master healthy on the day the tint rule was added here.
		// A stale master is regenerated IN PLACE so every material instance parented to it updates
		// without a re-import.
		UMaterial* Old = Cast<UMaterial>(Existing);
		FString StaleWhy;
		if (RudeGeneratedMasterHealth(Old, Name, StaleWhy) != ERudeMasterHealth::Stale) { return Existing; }
		M = Old;
		M->GetExpressionCollection().Empty();
		UE_LOG(LogTemp, Display, TEXT("[RUDE] regenerating stale master %s (%s)"), *Name, *StaleWhy);
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

	// THE STAMP. Unconnected on purpose - it is a version marker, not an input - and read back off
	// the expression collection by RudeGeneratedMasterHealth. Every graph below this line is
	// covered by it, so a wiring change needs no new staleness probe, only a bump of the constant.
	MakeScalar(RudeMasterGraphVersionParam, (float)RudeMasterGraphVersion, -1200);

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
		// ⛔⛔ THE .W IS NOT ON THE DEFAULT OUTPUT (2026-09-08, and it broke every detail master).
		// A ComponentMask over a VectorParameter's DEFAULT output asks a float3 for its fourth
		// component, and the material compiler refuses the WHOLE material:
		//   "(Node ComponentMask) Not enough components in (...: float3) for component mask 0011"
		//   "Failed to compile Material for platform PCD3D_SM6, Default Material will be used in game."
		// Every generated master with `Dt` in its signature was failing to compile and falling back to
		// the DEFAULT material - which ignores every parameter, which is why binding a palette, a tint
		// index and an amount correctly still changed nothing on screen. The tint was never the bug.
		// A parameter's per-channel pins are separate outputs (1=R, 2=G, 3=B, 4=A), the same convention
		// the diffuse alpha is read through, so take z and w off their OWN pins and append them.
		UMaterialExpressionAppendVector* ZW = NewObject<UMaterialExpressionAppendVector>(M);
		ZW->A.Connect(3, Set);   // detailSettings.z - tile U
		ZW->B.Connect(4, Set);   // detailSettings.w - tile V
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

	if (Spec.bLivery)
	{
		// The livery: a second diffuse whose alpha masks it over the base colour (vehicle_paint3's
		// DiffuseSampler2, measured on burrito 2026-09-06). LiveryAmount defaults to 0 so a master
		// without a bound livery renders exactly as before; the MI sets it to 1 when Diffuse2 binds.
		UMaterialExpressionTextureSampleParameter2D* Liv = MakeTex(TEXT("Diffuse2"), DefWhite, SAMPLERTYPE_Color, -700);
		UMaterialExpressionScalarParameter* LivAmt = MakeScalar(TEXT("LiveryAmount"), 0.f, -760);
		UMaterialExpressionMultiply* Mask = NewObject<UMaterialExpressionMultiply>(M);
		// Connect(), not a raw assignment - see the tint branch below: an OutputIndex without its MASK
		// reads the whole output instead of the alpha, so the livery would blend by colour rather than by
		// its mask. Same defect, same fix, found together 2026-09-07.
		Mask->A.Connect(4, Liv);   // the alpha output of the sample, with its mask
		Mask->B.Expression = LivAmt; Add(Mask, -420, -700);
		UMaterialExpressionLinearInterpolate* Lerp = NewObject<UMaterialExpressionLinearInterpolate>(M);
		Lerp->A.Expression = BaseColor; Lerp->B.Expression = Liv; Lerp->Alpha.Expression = Mask; Add(Lerp, -170, -640);
		BaseColor = Lerp;
	}

	if (Spec.bTint)
	{
		// ---- THE PALETTE LOOKUP (maintainer lane `weapon_tint`, measured 2026-09-07) --------------
		// A weapon body is painted by a 2-D TABLE LOOKUP, not by a multiply, and the two axes are:
		//   u = the DIFFUSE'S ALPHA - a material-ZONE index, not a shade. Over the 171 diffuse+palette
		//       pairs whose pixels this corpus actually carries, the alpha holds a MEDIAN OF 10
		//       authored values and they are round numbers (36, 40, 50, 60 ... 150); it is
		//       uncorrelated with luminance (mean |r| 0.23, 127/171 below 0.3); and in 114/171 pairs
		//       EVERY authored value lands on its OWN column of the palette. Meanwhile 136/171 of
		//       those diffuses are at least 90% desaturated (119/171 at least 99%), so the RGB is
		//       shading and the colour has to arrive from somewhere else.
		//   v = the tint index. 94 of the 98 palette entries in the 804 effective weapon dictionaries
		//       are 128x32 A8R8G8B8 with ONE mip, and the game declares 8 tints (weapons.meta
		//       TINT_DEFAULT, referenced by 91/91 CWeaponInfo rows in the copy the game loads) - which is
		//       exactly the number of distinct LEADING rows in 27 of those palettes (9 in 34 more).
		// The row is (selector + 0.5) * TintRowScale so v lands on a texel CENTRE. TintRowScale is
		// 1/height of whatever palette bound: a property of the TEXTURE, never of the tint choice, so
		// changing the tint later touches only paletteSelector and nothing can drift.
		// paletteSelector and tintPaletteSelector are the game's OWN parameter names, so ImportYdr's
		// generic value binding lands the authored value (0 in 583/583) with no special case. They are
		// SUMMED because no shader item carries both (0/583).
		// (⛔) NOTHING READS THEM BACK. ExportYdr emits a fixed census-standard parameter block and the
		// diffuse/bump/spec samplers only - no shader VALUE parameters read off the instance, and no
		// palette sampler - so a tint chosen here is EDITOR STATE ONLY and does not survive an export.
		// Naming it in the export is unbuilt work (the lane's NOTES.md section 5), not a claim made here.
		// (⛔) NEUTRAL BY DEFAULT: TintAmount is 0 here, so a master with a palette bound renders
		// EXACTLY as it does today until ImportYdr proves the data supports the lookup AND the shader
		// preset is one the lookup is enabled for (RudePresetEnablesTint - weapons only today). This
		// master is SHARED with every other lane that binds a palette, which is why the enable lives on
		// the instance and not in the graph.
		// (⚠) INFERRED, NOT MEASURED - the BLEND. 2 * albedo * palette is the conventional 2x multiply;
		// it is the identity when the palette is mid-grey, so a wrong guess degrades to "looks like
		// today" rather than "looks worse". RAGE's own shader math is not read and is not claimed.
		UMaterialExpressionScalarParameter* PalSel = MakeScalar(TEXT("paletteSelector"), 0.f, 860);
		UMaterialExpressionScalarParameter* TntSel = MakeScalar(TEXT("tintPaletteSelector"), 0.f, 900);
		UMaterialExpressionScalarParameter* RowScale = MakeScalar(TEXT("TintRowScale"), 0.f, 940);
		UMaterialExpressionScalarParameter* TintAmt = MakeScalar(TEXT("TintAmount"), 0.f, 980);
		UMaterialExpressionAdd* SelSum = NewObject<UMaterialExpressionAdd>(M);
		SelSum->A.Expression = PalSel; SelSum->B.Expression = TntSel; Add(SelSum, -1350, 860);
		UMaterialExpressionConstant* HalfRow = NewObject<UMaterialExpressionConstant>(M);
		HalfRow->R = 0.5f; Add(HalfRow, -1350, 920);
		UMaterialExpressionAdd* SelCentre = NewObject<UMaterialExpressionAdd>(M);
		SelCentre->A.Expression = SelSum; SelCentre->B.Expression = HalfRow; Add(SelCentre, -1220, 860);
		UMaterialExpressionMultiply* RowV = NewObject<UMaterialExpressionMultiply>(M);
		RowV->A.Expression = SelCentre; RowV->B.Expression = RowScale; Add(RowV, -1090, 860);
		UMaterialExpressionAppendVector* PalUV = NewObject<UMaterialExpressionAppendVector>(M);
		// ⛔ Connect(), NOT a raw assignment. Setting Expression and OutputIndex alone leaves the input's
		// MASK at its default, and the compiler masks by the INPUT, so the alpha pin was selected by index
		// and then read as the whole output - the zone index collapsed and every weapon sampled one column
		// of the palette, which is why a tint changed nothing on screen. ConnectExpression copies
		// Mask/MaskR/G/B/A off the output (MaterialExpressions.cpp:2093), which is the whole difference.
		PalUV->A.Connect(4, DiffuseTex);   // the ALPHA output of the diffuse sample, with its mask
		PalUV->B.Expression = RowV; Add(PalUV, -960, 860);
		UMaterialExpressionTextureSampleParameter2D* PalTex =
			MakeTex(TEXT("TintPalette"), DefWhite, SAMPLERTYPE_Color, 1020);
		PalTex->Coordinates.Expression = PalUV;
		UMaterialExpressionConstant* TwoTint = NewObject<UMaterialExpressionConstant>(M);
		TwoTint->R = 2.f; Add(TwoTint, -900, 1080);
		UMaterialExpressionMultiply* PalDoubled = NewObject<UMaterialExpressionMultiply>(M);
		PalDoubled->A.Expression = PalTex; PalDoubled->B.Expression = TwoTint; Add(PalDoubled, -760, 1020);
		UMaterialExpressionMultiply* TintedRaw = NewObject<UMaterialExpressionMultiply>(M);
		TintedRaw->A.Expression = BaseColor; TintedRaw->B.Expression = PalDoubled; Add(TintedRaw, -620, 960);
		UMaterialExpressionSaturate* TintedSat = NewObject<UMaterialExpressionSaturate>(M);
		TintedSat->Input.Expression = TintedRaw; Add(TintedSat, -480, 960);
		UMaterialExpressionLinearInterpolate* TintLerp = NewObject<UMaterialExpressionLinearInterpolate>(M);
		TintLerp->A.Expression = BaseColor; TintLerp->B.Expression = TintedSat; TintLerp->Alpha.Expression = TintAmt;
		Add(TintLerp, -340, 960);
		BaseColor = TintLerp;
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

// ---- ONE SWITCH: WHICH SHADER PRESETS MAY TURN THE TINT LOOKUP ON ----------------------------
// ImportDrawableNode is the SHARED importer - peds, vehicles, map drawables and weapons all arrive
// here - and the palette samplers are NOT weapon-only. MEASURED over a seeded random sample of 400
// NON-weapon files each (maintainer lane `weapon_tint`, law 14): peds declare a palette sampler
// 1,577 times but only 4 carry a texture NAME; vehicles 8 declared, 8 named; map props and other
// drawables 92 declared, 42 named. So switching the lookup on everywhere would repaint vehicles and
// world props from a population this lane never measured.
// THE MECHANISM GOES IN EVERYWHERE; THE LOOKUP IS ENABLED FOR WEAPON PRESETS ONLY. Every other
// preset - including the `*_tnt` family the map uses - keeps its palette bound, visible in
// InspectMesh, and NEUTRAL: it renders exactly as it does today. Inside the weapon set itself that
// withholds 9 of the 428 bucket-0 palette bindings (`normal_spec_tnt` 8, `normal_spec_detail_tnt` 1;
// law 3 crossed with law 10), leaving 419.
// WIDENING IT IS THIS ONE EDIT (`return true;` enables every preset), and the import verdict's
// tintPalettesNeutralNonWeapon counts what is being deferred rather than letting it be forgotten.
static bool RudePresetEnablesTint(const FString& Preset)
{
	return Preset.StartsWith(TEXT("weapon"), ESearchCase::IgnoreCase);
}

// Does this texture's ALPHA carry information, or is it flat? The palette lookup reads the diffuse's
// alpha as a material-zone index; a source with no alpha imports as 255 everywhere, and feeding that
// to the lookup paints the WHOLE surface with the palette's last column - a uniform wrong colour on
// every texel. 137 of the weapon set's 583 palette bindings sit on such a diffuse (D3DFMT_DXT1),
// against 445 on one that carries alpha (maintainer lane `weapon_tint`).
// SAMPLED, not scanned: at most 4,096 texels spread across the top mip, so a 2048x2048 diffuse costs
// microseconds, and CACHED per texture because a district import asks the same question thousands of
// times. (⛔) The bar is TWO DISTINCT VALUES, not "some alpha below 255": a single compression
// artefact on a genuinely opaque map must not be able to switch a tint on.
static bool RudeTextureAlphaVaries(UTexture2D* T)
{
	if (!T) { return false; }
	// KEYED ON THE SOURCE, NOT THE OBJECT. ImportYtd edits textures IN PLACE, so the same UTexture2D
	// can gain real pixels later in the same session; an object-keyed cache would keep answering
	// "flat" and the weapon would stay untinted with no gate able to see it. FTextureSource::GetId is
	// the source's own identity (it changes when the source does) and is cheap - it either returns the
	// stored guid or hashes the header fields, never the payload.
	static TMap<FString, bool> Cache;
	const FString Key = T->GetPathName() + TEXT("|") + T->Source.GetIdString();
	if (const bool* Hit = Cache.Find(Key)) { return *Hit; }
	bool bVaries = false;
	TArray64<uint8> Mip;
	if (T->Source.IsValid() && T->Source.GetFormat() == TSF_BGRA8 && T->Source.GetMipData(Mip, 0))
	{
		const int64 Texels = Mip.Num() / 4;
		if (Texels > 0)
		{
			const int64 Stride = FMath::Max<int64>(1, Texels / 4096);
			const uint8 First = Mip[3];
			for (int64 i = 0; i < Texels; i += Stride)
			{
				if (Mip[i * 4 + 3] != First) { bVaries = true; break; }
			}
		}
	}
	Cache.Add(Key, bVaries);
	return bVaries;
}

static UMaterialInterface* EnsureDetailMaster()
{
	const TCHAR* FullPath = TEXT("/RUDE/Masters/M_RUDE_Detail.M_RUDE_Detail");
	UMaterial* M = LoadObject<UMaterial>(nullptr, FullPath);
	if (M)
	{
		// The same ONE rule (RudeGeneratedMasterHealth): this master's condition is DetailAmount.
		// It is stated there and nowhere else, so the doctor can report it without restating it.
		FString StaleWhy;
		if (RudeGeneratedMasterHealth(M, TEXT("M_RUDE_Detail"), StaleWhy) != ERudeMasterHealth::Stale) { return M; }
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

// The texture semantics a dictionary's own <Usage> implies. Lifted out of ImportYtd's loop
// (2026-09-07) because it now has TWO call sites: the import, and the settings-only repair of a
// texture whose PIXELS are unchanged but whose settings predate a new branch.
// TINTPALETTE is that new branch, and it is the one Usage where the defaults are actively wrong:
// a palette is a LOOKUP TABLE, not a picture. Block compression rewrites its swatches (BC1 fits a
// 4x4 block to two endpoints), a mip chain averages neighbouring TINTS together, and bilinear
// filtering bleeds one tint row into the next - all three destroy the only property the table has,
// which is that texel (u,v) is EXACTLY the colour authored there. The game keeps them uncompressed
// and unmipped itself: 98/98 TINTPALETTE entries across the 804 effective weapon dictionaries are
// D3DFMT_A8R8G8B8 with MipLevels 1 (maintainer lane `weapon_tint`).
static void RudeApplyTextureUsage(UTexture2D* Tex, const FString& Usage)
{
	if (!Tex) { return; }
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
	else if (Usage == TEXT("TINTPALETTE"))
	{
		Tex->CompressionSettings = TC_EditorIcon;   // UserInterface2D: RGBA, uncompressed
		Tex->SRGB = true;                           // it holds colours and it multiplies an sRGB albedo
		Tex->Filter = TF_Nearest;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
#if WITH_EDITORONLY_DATA
		Tex->MipGenSettings = TMGS_NoMipmaps;
#endif
	}
	else
	{
		Tex->CompressionSettings = TC_Default;
		Tex->SRGB = true;
	}
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

	int32 Imported = 0, Unchanged = 0;
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
	// A texture whose PIXELS are unchanged but whose SETTINGS predate a Usage branch (the palettes,
	// 2026-09-07). Repaired in place and counted, so "unchanged" never hides a stale compression mode.
	int32 SettingsRepaired = 0;
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
		// TINTPALETTE joined the vocabulary on 2026-09-07: 98 ITEMS across the 804 effective weapon
		// dictionaries, and 9,796 more .ytd.xml FILES elsewhere carry a TINTPALETTE entry (9,651 distinct
		// names, every one an __embedded dictionary, none load-order-resolved - a LOWER BOUND, not a
		// survey). So it is no longer "unknown" - RudeApplyTextureUsage now branches on it.
		if (Usage != TEXT("DIFFUSE") && Usage != TEXT("NORMAL") && Usage != TEXT("SPECULAR")
			&& Usage != TEXT("TINTPALETTE"))
		{
			++UsageUnknown;
		}

		// Pixels: the DDS sidecar first (named by the manifest's <FileName>, else "<name>.dds"),
		// then the offline PNG bridge.
		const FXmlNode* FileNode = Item->FindChildNode(TEXT("FileName"));
		const FString DdsName = FileNode && !FileNode->GetContent().TrimStartAndEnd().IsEmpty()
			? FileNode->GetContent().TrimStartAndEnd() : (TexName + TEXT(".dds"));
		// An EMPTY PixelFolder means the corpus's own sidecar folder beside the XML ("<dir>/<stem>/"),
		// the way ImportMapArea resolves it; a relative bare file name never exists (measured 2026-09-06:
		// burrito.ytd declared 11, imported 0 with the 11 DDS files sitting right there).
		FString Pix = PixelFolder.TrimStartAndEnd();
		if (Pix.IsEmpty())
		{
			FString Stem = FPaths::GetBaseFilename(XmlPath);   // "burrito.ytd"
			if (Stem.EndsWith(TEXT(".ytd"), ESearchCase::IgnoreCase)) { Stem.LeftChopInline(4); }
			Pix = FPaths::GetPath(XmlPath) / Stem;
		}
		const FString DdsPath = Pix / DdsName;
		const FString PngPath = Pix / (TexName + TEXT(".png"));
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
		// A texture that exists ON DISK but is not loaded is LOADED first (2026-09-06): creating a new object
		// over an unloaded package left two claimants - ours, and the disk copy pulled in later by a kept
		// mesh's material instance - and the save then failed with "invalid payload" (14 of 225 in the
		// vehicle re-body gate, every one referenced by an on-disk MI). Loading makes the edit in place real.
		if (!Tex && FPackageName::DoesPackageExist(PackageName))
		{
			Tex = LoadObject<UTexture2D>(nullptr, *(PackageName + TEXT(".") + TexName));
		}
		if (!Tex)
		{
			Tex = NewObject<UTexture2D>(Package, FName(*TexName), RF_Public | RF_Standalone);
		}
		// An UNCHANGED texture is left alone (2026-09-06): re-initialising a disk-loaded texture with the
		// same pixels dirtied it, and its lazily-loaded bulkdata then refused to save ("invalid payload",
		// 11 of the blista/vehshare set on the Downtown drive gate). Same size, format and bytes = no edit.
		{
			bool bSame = false;
			if (Tex->Source.IsValid() && Tex->Source.GetSizeX() == W && Tex->Source.GetSizeY() == H && Tex->Source.GetFormat() == TSF_BGRA8)
			{
				TArray64<uint8> Existing;
				if (Tex->Source.GetMipData(Existing, 0) && Existing.Num() == BGRA.Num()
					&& FMemory::Memcmp(Existing.GetData(), BGRA.GetData(), BGRA.Num()) == 0) { bSame = true; }
			}
			// (⚠) THE FAST PATH COMPARES PIXELS, AND SETTINGS ARE NOT PIXELS (2026-09-07). A palette
			// imported before the TINTPALETTE branch existed has byte-identical pixels and the WRONG
			// settings - block-compressed, mipped, bilinear - and would be skipped forever. Repair the
			// SETTINGS in place and still count it unchanged: no Source.Init, so the saved-package
			// bulkdata hazard (conventions 6.9) is never touched. Counted, so it is not silent.
			if (bSame && Usage == TEXT("TINTPALETTE") && Tex->CompressionSettings != TC_EditorIcon)
			{
				RudeApplyTextureUsage(Tex, Usage);
				Tex->UpdateResource();
				Tex->PostEditChange();
				Package->MarkPackageDirty();
				++SettingsRepaired;
			}
			if (bSame) { ++Imported; ++Unchanged; continue; }
		}
		Tex->PreEditChange(nullptr);
		Tex->Source.Init(W, H, 1, 1, TSF_BGRA8, BGRA.GetData());

		// Semantics from the ytd's own Usage - the thing generic importers can't know
		RudeApplyTextureUsage(Tex, Usage);

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
		TEXT("{\"ok\":%s,\"txd\":\"%s\",\"declared\":%d,\"imported\":%d,\"unchanged\":%d,\"invalidNames\":%d,")
		TEXT("\"itemsWithoutName\":%d,\"usageDefaulted\":%d,\"usageUnknown\":%d,\"settingsRepaired\":%d,")
		TEXT("\"missingPixelCount\":%d,\"missingPixels\":[%s],")
		TEXT("\"pixelsFromDds\":%d,\"pixelsFromPng\":%d,\"pixelsRefused\":%d,\"pixelsRefusedReasons\":[%s]}"),
		bTotalLoss ? TEXT("false") : TEXT("true"),
		*TxdName, Declared, Imported, Unchanged, InvalidNames, ItemsWithoutName, UsageDefaulted, UsageUnknown, SettingsRepaired,
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

// Build a UStaticMesh asset (plus its per-slot MaterialInstances) from ONE drawable-shaped
// XML node - the body every import lane shares. DrawableRoot may be a standalone <Drawable>
// root or a <Fragment>'s inner <Drawable> (both via ImportYdr), or a <DrawableDictionary>
// <Item> (ImportYddEntry) - anything carrying ShaderGroup + DrawableModelsHigh children.
// MeshName is the ASSET name, decided by the CALLER (file stem, <Name>, or dictionary entry).
// Scope = every scoping signal the caller can PROVE (see FRudeTextureScope). nullptr still means
// "no scope" - the two BlueprintCallable entry points ImportYdr/ImportYddEntry keep their
// signatures and pass nullptr, because a single-file call has no archetype to read one off, and
// that is also the do-nothing CONTROL every measurement of this fix is scored against.

// The body of ImportYdr, plus the texture SCOPE the public UFUNCTION has no parameter for.
// Callers that hold an archetype (the map/MLO lane, and ImportYdrBatch when given a CorpusRoot)
// come through here; URudeToolset::ImportYdr forwards with an empty scope, which is exactly the
// pre-2026-08-05 behaviour, so the single-file tool is byte-identical to what it was.
FString RudeImportYdrScoped(const FString& XmlPath, const FString& DestFolder,
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
	// A fragment's visual drawable imports through this same lane: ROUT's prototype fragment exporter (v1) emits
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

FString ImportDrawableNode(const FXmlNode* DrawableRoot, const FString& MeshName,
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
		// ⭐ VALUE params (2026-07-29). ROUT used to drop every non-texture shader parameter, so
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
			// The other spelling (351/583 palette bindings in the weapon set) - without this the commoner
			// family picks a master with no TintPalette parameter and every bind is a silent no-op.
			// ⛔ WEAPON PRESETS ONLY (RudePresetEnablesTint): the name is shared with peds, vehicles and
			// props, and raising bTint for them would re-parent an unmeasured population onto a different
			// master for no visual gain. They keep the master they get today. The `TintPalette*` spelling
			// above is UNCHANGED - it has raised bTint since before this lane, and still does.
			else if (S.Equals(TEXT("TextureSamplerDiffPal"), ESearchCase::IgnoreCase)
				&& RudePresetEnablesTint(D.Preset)) { Spec.bTint = true; }
			else if (S.Equals(TEXT("DiffuseSampler2"), ESearchCase::IgnoreCase)) { Spec.bLivery = true; }
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
	int32 DetailNormalMapsSkipped = 0;   // a NORMAL map bound to Detail: the albedo overlay is left off
	int32 TintPalettesBound = 0;         // a palette bound AND the lookup switched on (see the gate below)
	int32 TintPalettesNeutral = 0;       // a palette bound and the tint left OFF - wrong bucket, or a flat alpha
	int32 TintPalettesNeutralNonWeapon = 0;  // DEFERRED: a palette on a non-weapon preset - mechanism in, lookup off
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
		{ TEXT("DiffuseSampler2"),    TEXT("Diffuse2")    },  // the livery (vehicle_paint*): masters with the L flag
		{ TEXT("BumpSampler"),        TEXT("Normal")      },
		{ TEXT("SpecSampler"),        TEXT("Specular")    },
		{ TEXT("TextureSamp"),        TEXT("Diffuse")     },  // cable's albedo: 152/152 resolve
		{ TEXT("distanceMapSampler"), TEXT("Diffuse")     },  // distance_map's only colour source
		{ TEXT("DetailSampler"),      TEXT("Detail")      },  // inert until the masters gain Detail
		{ TEXT("TintPaletteSampler"),    TEXT("TintPalette") },  // the "tnt" family: 232 of 583 palette bindings
		// THE SECOND SPELLING OF THE SAME IDEA, and the commoner one. MEASURED over the 879 effective
		// weapon drawables (maintainer lane `weapon_tint`): 351 palette bindings ride
		// TextureSamplerDiffPal and 232 ride TintPaletteSampler, 0 shader items carry both, and each
		// name always travels with its own selector (TextureSamplerDiffPal+paletteSelector 351/351,
		// TintPaletteSampler+tintPaletteSelector 232/232). Both land on ONE master parameter.
		// ⛔ SHARED WITH THE PED / VEHICLE / PROP LANES: this name is declared outside the weapon set too,
		// so the BIND is gated on the preset (RudePresetEnablesTint) at the lookup site below. Only the
		// weapon set was measured - law 13 and law 14 in the lane's LAWS.md say how much was not.
		{ TEXT("TextureSamplerDiffPal"), TEXT("TintPalette") },  // the "palette" family: 351 of 583
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
		// Does THIS preset get the tint lookup? One switch, in RudePresetEnablesTint: the palette
		// samplers are shared with peds, vehicles and map props, and only the weapon set was measured.
		const bool bPresetTints = Def && RudePresetEnablesTint(Def->Preset);
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
				bool bBoundTint = false;
				UTexture2D* DetailTexBound = nullptr;   // WHICH texture landed in Detail - the KIND matters (below)
				UTexture2D* TintTexBound = nullptr;     // the palette, for its ROW COUNT (TintRowScale)
				UTexture2D* DiffuseTexBound = nullptr;  // the albedo, for its ALPHA (the palette lookup coordinate)
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
					if (PName == FName(TEXT("Detail")))  { bBoundDetail = true; DetailTexBound = T; }
					// The tint lookup needs BOTH textures by hand: the palette it samples, and the diffuse whose
					// ALPHA is the lookup coordinate (the gate below refuses a flat one).
					if (PName == FName(TEXT("TintPalette"))) { bBoundTint = true; TintTexBound = T; }
					if (PName == FName(TEXT("Diffuse")))     { DiffuseTexBound = T; }
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
					// (⛔) THE SECOND PALETTE SPELLING IS A SHARED SAMPLER. TextureSamplerDiffPal is declared
					// by peds, vehicles and map props too (see RudePresetEnablesTint for the counts), and this
					// is the SHARED importer. Outside a weapon preset it is left exactly as it was before this
					// lane - not bound, nothing re-parented, nothing repainted - and COUNTED here rather than
					// dropped silently, so the deferred population is a number in the verdict.
					// (The older TintPaletteSampler spelling has bound since before this lane and still does;
					// what stays off for it outside a weapon preset is TintAmount, in the gate below.)
					if (!bPresetTints && Tex.Key.Equals(TEXT("TextureSamplerDiffPal"), ESearchCase::IgnoreCase))
					{
						++TintPalettesNeutralNonWeapon;
						continue;
					}
					BindTex(Param, Tex.Value);
					if (FCString::Stricmp(Param, TEXT("Diffuse2")) == 0)
					{
						MIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LiveryAmount")), 1.f);
					}
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
				// AND THE RIGHT KIND OF TEXTURE, not merely a texture (2026-09-06). The detail master
				// applies a signed overlay to the ALBEDO: 1 + (Detail - 0.5) * 2 * strength. That is
				// neutral only for a COLOUR detail map around mid-grey. Several presets bind a NORMAL
				// map there instead (weapon_normal_spec_detail_palette binds env_smooth_concrete2,
				// measured to be a normal map), whose blue channel sits near 1.0 - so the overlay
				// multiplies base colour by a tiled, blue-biased pattern, seen as a repeating blocky
				// cast over weapon bodies. A bump-detail map belongs in the NORMAL path; until that
				// path exists, it must not colour the albedo. Counted, never silent.
				const bool bDetailIsNormalMap = DetailTexBound && DetailTexBound->CompressionSettings == TC_Normalmap;
				if (bDetailIsNormalMap) { ++DetailNormalMapsSkipped; }
				if (MasterParams.Contains(FName(TEXT("Detail"))))
				{
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("DetailAmount")), (bBoundDetail && !bDetailIsNormalMap) ? 1.f : 0.f);
				}

				// (⛔) THE TINT: PROVE IT BEFORE ENABLING IT - the same law as DetailAmount above. TintAmount
				// stays 0, i.e. the slot renders EXACTLY as it does today, unless all three arrived:
				//  1. a palette texture really landed in TintPalette (not the master's white default);
				//  2. the shader is RenderBucket 0. Buckets 2 and 3 wire the diffuse ALPHA into OpacityMask,
				//     and the alpha is the palette's lookup coordinate - one channel cannot be both.
				//     MEASURED (maintainer lane `weapon_tint`): of 583 palette bindings in the weapon set, 428
				//     are bucket 0, 130 bucket 2, 25 bucket 3 and NONE bucket 1; the bucket-2 diffuses average
				//     28.0% fully transparent texels against 1.4% in bucket 0, and 180 of 184 bucket-0 diffuses
				//     carry a DISCRETE alpha (<=24 authored values covering >=80% of the surface).
				//  3. the diffuse's alpha actually VARIES - see RudeTextureAlphaVaries. 137 of the 583 bindings
				//     sit on a diffuse with no alpha channel at all, which would read one palette column for
				//     every texel.
				//  4. the shader preset is one the lookup is ENABLED for - weapons only today, because this is
				//     the shared importer and only the weapon set was measured (RudePresetEnablesTint). Inside
				//     the weapon set that withholds 9 of the 428 bucket-0 bindings (`normal_spec_tnt` 8,
				//     `normal_spec_detail_tnt` 1), leaving 419; outside it, everything. The withheld ones are
				//     counted, not dropped: tintPalettesNeutralNonWeapon in the verdict.
				// TintRowScale is 1/rows of the palette that bound: a property of the TEXTURE, so the row stays
				// correct however the tint is changed later, and there is no second source of truth to drift.
				if (MasterParams.Contains(FName(TEXT("TintPalette"))))
				{
					// int32(): FTextureSource::GetSizeX/Y return int64 in 5.8, and a silent narrowing is a warning
					// this project's targets are one settings change away from making an error.
					const int32 PaletteRows = (bBoundTint && TintTexBound) ? int32(TintTexBound->Source.GetSizeY()) : 0;
					const bool bTintable = bBoundTint && bPresetTints && PaletteRows > 0 && Def->RenderBucket == 0
						&& RudeTextureAlphaVaries(DiffuseTexBound);
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("TintRowScale")), PaletteRows > 0 ? 1.f / float(PaletteRows) : 0.f);
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("TintAmount")), bTintable ? 1.f : 0.f);
					if (bTintable)                        { ++TintPalettesBound; }
					else if (bBoundTint && !bPresetTints) { ++TintPalettesNeutralNonWeapon; }
					else if (bBoundTint)                  { ++TintPalettesNeutral; }
				}

				// ---- VALUE params -> the MI, guarded exactly like textures ----
				// ⭐ These only became available on 2026-07-29, when ROUT stopped dropping every
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
		TEXT("\"valueParamsUnsupported\":%d,\"valueParamsDeduped\":%d,")
		TEXT("\"tintPalettesBound\":%d,\"tintPalettesNeutral\":%d,\"tintPalettesNeutralNonWeapon\":%d,")
		TEXT("%s,\"slots\":[%s]}"),
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
		TintPalettesBound, TintPalettesNeutral, TintPalettesNeutralNonWeapon,
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

// Jenkins one-at-a-time over the LOWERCASED name - RAGE's name hash, pinned to ROUT's
// convention (ROUT's joaat: lowercase input; the unresolvable-name fallback is
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

uint32 RudeJoaat(const FString& Name)
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
FString RudeImportYddEntryScoped(const FString& XmlPath, const FString& EntryName,
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
			else if (Rest.StartsWith(TEXT("L"))) { Spec.bLivery = true; Rest = Rest.Mid(1); }
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

