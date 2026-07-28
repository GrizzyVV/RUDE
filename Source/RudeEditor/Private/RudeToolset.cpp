// RUDE - RAGE <-> Unreal Development Environment
#include "RudeToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
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
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
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
		TEXT("{\"ok\":true,\"txd\":\"%s\",\"imported\":%d,\"missingPixels\":[%s]}"),
		*TxdName, Imported, *Missing);
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
	if (!Root || Root->GetTag() != TEXT("Drawable"))
	{
		return Fail(TEXT("root is not <Drawable>"));
	}

	// Drawable name (strip ".#dr" style suffix)
	FString Name = FPaths::GetBaseFilename(XmlPath);
	Name.RemoveFromEnd(TEXT(".ydr"));
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

	// Shader presets + their texture parameter bindings
	struct FShaderDef
	{
		FString Preset = TEXT("default");
		int32 RenderBucket = 0;              // RAGE draw bucket: 0 opaque, 1 alpha, 2 decal, 3 cutout
		FString Diffuse, Normal, Specular;   // texture NAMES from the ydr
		TMap<FString, FString> AllTex;       // every Texture param: samplerName -> texName (terrain layers)
	};
	TArray<FShaderDef> Shaders;
	if (const FXmlNode* SG = Root->FindChildNode(TEXT("ShaderGroup")))
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
	TArray<RudeYdr::FGeo> Geos;
	if (const FXmlNode* High = Root->FindChildNode(TEXT("DrawableModelsHigh")))
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
				}
			}
		}
	}
	if (Geos.Num() == 0)
	{
		return Fail(TEXT("no geometry in DrawableModelsHigh"));
	}

	// Create the StaticMesh asset
	const FString PackageName = DestFolder / Name;
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		return Fail(FString::Printf(TEXT("bad package name: %s"), *PackageName));
	}
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return Fail(TEXT("CreatePackage failed"));
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, FName(*Name), RF_Public | RF_Standalone);
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
		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game/RUDE/Textures"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
		TArray<FAssetData> Found;
		ARM.Get().GetAssets(Filter, Found);
		for (const FAssetData& AD : Found)
		{
			TextureByName.Add(AD.AssetName.ToString().ToLower(), AD);
		}
	}

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
		else if (Bucket == 1 || Bucket == 3 ||
		         P.Contains(TEXT("cutout")) || P.Contains(TEXT("alpha")))
		{
			Path = TEXT("/RUDE/Masters/M_RUDE_Cutout.M_RUDE_Cutout");
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
			Master = bTerrain ? EnsureTerrainMaster() : MasterForPreset(Def->Preset, Def->RenderBucket);
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
			const FString MIName = FString::Printf(TEXT("MI_%s_%d"), *Name, GeoIdx);
			const FString MIPackageName =
				FString::Printf(TEXT("/Game/RUDE/Materials/Instances/%s/%s"), *Name, *MIName);
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
				if (UTexture2D* T = FindTexture(Def->Diffuse))
				{
					MIC->SetTextureParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("Diffuse")), T);
					++BoundTextures;
				}
				if (UTexture2D* T = FindTexture(Def->Normal))
				{
					MIC->SetTextureParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("Normal")), T);
					++BoundTextures;
				}
				if (UTexture2D* T = FindTexture(Def->Specular))
				{
					MIC->SetTextureParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("Specular")), T);
					++BoundTextures;
				}
				// A decal whose texture isn't in the corpus must render as NOTHING, not as
				// an opaque white slab (the master's default texture is white).
				if (Def->RenderBucket == 2 || Def->Preset.Contains(TEXT("decal")))
				{
					const bool bHasDiffuse = FindTexture(Def->Diffuse) != nullptr;
					MIC->SetScalarParameterValueEditorOnly(
						FMaterialParameterInfo(TEXT("Visible")), bHasDiffuse ? 1.f : 0.f);
				}
				if (bTerrain)
				{
					// terrain_cb_* layer samplers -> the 4-layer master's Diffuse0..3/Normal0..3
					for (int32 li = 0; li < 4; ++li)
					{
						if (const FString* TN = Def->AllTex.Find(FString::Printf(TEXT("TextureSampler_layer%d"), li)))
						{
							if (UTexture2D* T = FindTexture(*TN))
							{
								MIC->SetTextureParameterValueEditorOnly(
									FMaterialParameterInfo(*FString::Printf(TEXT("Diffuse%d"), li)), T);
								++BoundTextures;
							}
						}
						if (const FString* TN = Def->AllTex.Find(FString::Printf(TEXT("BumpSampler_layer%d"), li)))
						{
							if (UTexture2D* T = FindTexture(*TN))
							{
								MIC->SetTextureParameterValueEditorOnly(
									FMaterialParameterInfo(*FString::Printf(TEXT("Normal%d"), li)), T);
								++BoundTextures;
							}
						}
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
	return FString::Printf(
		TEXT("{\"ok\":true,\"assetPath\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"boundTextures\":%d,\"slots\":[%s]}"),
		*PackageName, Geos.Num(), TotalVerts, TotalTris, BoundTextures, *SlotsJson);
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

FString URudeToolset::CaptureView(const FString& CamSpec, const FString& OutPng)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> C;
	CamSpec.ParseIntoArray(C, TEXT(","), true);
	if (C.Num() != 5) { return Fail(TEXT("CamSpec must be \"x,y,z,pitch,yaw\"")); }
	const FVector Loc(FCString::Atod(*C[0]), FCString::Atod(*C[1]), FCString::Atod(*C[2]));
	const FRotator Rot(FCString::Atod(*C[3]), FCString::Atod(*C[4]), 0.0);
	for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
	{
		if (VC && VC->IsPerspective())
		{
			VC->SetViewLocation(Loc);
			VC->SetViewRotation(Rot);
			VC->Invalidate();
			FScreenshotRequest::RequestScreenshot(OutPng, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
			return FString::Printf(TEXT("{\"ok\":true,\"requested\":\"%s\"}"), *OutPng);
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

FString URudeToolset::ImportMapArea(const FString& CorpusRoot, const FString& YmapPrefix,
                                    const FString& DestMeshFolder, const FString& Filter)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// ---- 1) archetype index from every ytyp XML (name -> drawable assetName) ----
	TMap<FString, FString> ArchToAsset;
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
				const FXmlNode* NameN = Item->FindChildNode(TEXT("name"));
				const FXmlNode* AssetN = Item->FindChildNode(TEXT("assetName"));
				const FXmlNode* TypeN = Item->FindChildNode(TEXT("assetType"));
				if (!NameN || !AssetN) { continue; }
				// drawable-backed archetypes only; ydd-dictionary / fragment archetypes
				// stay unresolved (proxy cubes) until those importers exist
				const FString AType = TypeN ? TypeN->GetContent().TrimStartAndEnd() : FString();
				if (!AType.IsEmpty() && AType != TEXT("ASSET_TYPE_DRAWABLE")) { continue; }
				ArchToAsset.Add(NameN->GetContent().TrimStartAndEnd().ToLower(),
				                AssetN->GetContent().TrimStartAndEnd().ToLower());
			}
		}
		if (ArchToAsset.Num() == 0) { return Fail(TEXT("no archetypes indexed - check CorpusRoot/ytyp")); }
	}
	// ---- 2) parse ymaps -> manifest scenes (IMPORT-lane transforms: pos Y-mirror*100,
	// quat = (x,-y,z,w) - the boardwalk-anchored map, NOT the export involution) ----
	TArray<FString> YmapFiles;
	IFileManager::Get().FindFiles(YmapFiles, *(CorpusRoot / TEXT("ymap") / (YmapPrefix + TEXT("*.xml"))), true, false);
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
			const FString* Asset = ArchToAsset.Find(Arch);
			++TotalEnts; ++SceneEnts;
			if (Asset) { ++Resolved; NeededDrawables.Add(*Asset); }
			EntJson += FString::Printf(TEXT(
				"%s{\"archetype\":\"%s\",\"drawable\":%s,\"lodLevel\":\"%s\","
				"\"ue_location\":[%f,%f,%f],\"ue_quat\":[%f,%f,%f,%f],\"scaleXY\":%f,\"scaleZ\":%f}"),
				SceneEnts > 1 ? TEXT(",") : TEXT(""), *Arch,
				Asset ? *FString::Printf(TEXT("\"%s\""), **Asset) : TEXT("null"), *Lod,
				Px * 100.0, -Py * 100.0, Pz * 100.0,
				Qx, -Qy, Qz, Qw,
				Val(TEXT("scaleXY"), 1.0), Val(TEXT("scaleZ"), 1.0));
		}
		if (SceneEnts == 0) { continue; }
		FString YmapName = FPaths::GetBaseFilename(F);
		YmapName.RemoveFromEnd(TEXT(".ymap"));
		ScenesJson += FString::Printf(TEXT("%s{\"ymap\":\"%s\",\"entities\":[%s]}"),
			ScenesJson.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, *EntJson);
	}
	const FString ManifestPath = FPaths::ProjectSavedDir() / TEXT("RUDE") /
		FString::Printf(TEXT("area_%s_manifest.json"), *YmapPrefix.Replace(TEXT("*"), TEXT("")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);
	if (!FFileHelper::SaveStringToFile(TEXT("[") + ScenesJson + TEXT("]"), *ManifestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write area manifest"));
	}
	// ---- 3) import every referenced drawable present in the corpus (skip-if-exists) ----
	int32 MeshOk = 0, MeshSkip = 0, MeshFail = 0, MeshMissing = 0, Done = 0;
	for (const FString& D : NeededDrawables)
	{
		++Done;
		const FString YdrPath = CorpusRoot / TEXT("ydr") / (D + TEXT(".ydr.xml"));
		if (!FPaths::FileExists(YdrPath)) { ++MeshMissing; continue; }
		if (FPackageName::DoesPackageExist(DestMeshFolder / D)) { ++MeshSkip; continue; }
		const FString R = ImportYdr(YdrPath, DestMeshFolder);
		if (R.Contains(TEXT("\"ok\":true"))) { ++MeshOk; } else { ++MeshFail; }
		if (Done % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportMapArea meshes %d/%d (ok %d, skip %d, fail %d)"),
				Done, NeededDrawables.Num(), MeshOk, MeshSkip, MeshFail);
			CollectGarbage(RF_NoFlags);
		}
	}
	// ---- 4) spawn through the proven ImportScene path ----
	const FString Spawn = ImportScene(ManifestPath, DestMeshFolder, Filter);
	return FString::Printf(TEXT(
		"{\"ok\":true,\"ymaps\":%d,\"entities\":%d,\"resolved\":%d,\"meshesImported\":%d,"
		"\"meshesSkipped\":%d,\"meshesFailed\":%d,\"meshesMissingFromCorpus\":%d,"
		"\"manifest\":\"%s\",\"spawn\":%s}"),
		YmapFiles.Num(), TotalEnts, Resolved, MeshOk, MeshSkip, MeshFail, MeshMissing,
		*ManifestPath, *Spawn);
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
		// collidable iff the drawable embeds a <Bounds> with >=1 child (gates bit 17)
		bool bCollidable = false;
		if (const FXmlNode* B = Root->FindChildNode(TEXT("Bounds")))
		{
			if (const FXmlNode* C = B->FindChildNode(TEXT("Children")))
			{
				bCollidable = C->GetChildrenNodes().Num() > 0;
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
		if (R.Contains(TEXT("\"ok\":true"))) { ++Imported; }
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
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportYdrBatch %d/%d (ok %d, skip %d, fail %d)"),
				i + 1, Lines.Num(), Imported, Skipped, Failed);
		}
		if ((i + 1) % 250 == 0)
		{
			CollectGarbage(RF_NoFlags);   // keep editor memory flat on long batches
		}
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"imported\":%d,\"skipped\":%d,\"failed\":%d,\"failedFiles\":[%s]}"),
		Imported, Skipped, Failed, *FailedFiles);
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

		auto GetIsm = [&](const FString& Key, UStaticMesh* Mesh) -> UInstancedStaticMeshComponent*
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
				if (UInstancedStaticMeshComponent* Ism = GetIsm(Drawable, Mesh))
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
