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

namespace RudeYdr
{
	// GTAV1 vertex layout semantic -> float width in a vertex Data line
	static int32 SemanticWidth(const FString& Tag)
	{
		if (Tag == TEXT("Position") || Tag == TEXT("Normal")) return 3;
		if (Tag == TEXT("Colour0") || Tag == TEXT("Colour1") || Tag == TEXT("Tangent")) return 4;
		if (Tag.StartsWith(TEXT("TexCoord"))) return 2;
		return 0;
	}

	struct FGeo
	{
		int32 ShaderIndex = 0;
		TArray<FVector3f> Positions;   // already RUDE-transformed to UE space (cm, Y-mirrored)
		TArray<FVector3f> Normals;     // Y-mirrored
		TArray<FVector2f> UVs;         // raw RAGE UVs (both engines are V-down; no flip)
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

		// Layout: ordered semantic list
		TArray<FString> Semantics;
		int32 LineWidth = 0;
		if (const FXmlNode* Layout = VB->FindChildNode(TEXT("Layout")))
		{
			for (const FXmlNode* Child : Layout->GetChildrenNodes())
			{
				Semantics.Add(Child->GetTag());
				LineWidth += SemanticWidth(Child->GetTag());
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
		const int32 NumVerts = Toks.Num() / LineWidth;
		for (int32 V = 0; V < NumVerts; ++V)
		{
			int32 Off = V * LineWidth;
			FVector3f Pos = FVector3f::ZeroVector;
			FVector3f Nrm(0, 0, 1);
			FVector2f UV = FVector2f::ZeroVector;
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
				Off += W;
			}
			// RUDE transform: gta meters -> ue cm, Y mirror
			Out.Positions.Add(FVector3f(Pos.X * 100.f, -Pos.Y * 100.f, Pos.Z * 100.f));
			Out.Normals.Add(FVector3f(Nrm.X, -Nrm.Y, Nrm.Z));
			Out.UVs.Add(UV);
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
		FString Diffuse, Normal, Specular;   // texture NAMES from the ydr
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

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> GroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	int32 TotalVerts = 0, TotalTris = 0;
	TArray<FString> SlotNames;

	for (int32 GeoIdx = 0; GeoIdx < Geos.Num(); ++GeoIdx)
	{
		const RudeYdr::FGeo& Geo = Geos[GeoIdx];
		const FString ShaderName = Geo.ShaderIndex < Shaders.Num()
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
			if (I0 >= VertexIDs.Num() || I1 >= VertexIDs.Num() || I2 >= VertexIDs.Num())
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
				InstUVs[Inst] = Geo.UVs.IsValidIndex(Idx) ? Geo.UVs[Idx] : FVector2f::ZeroVector;
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

	auto MasterForPreset = [](const FString& Preset) -> UMaterialInterface*
	{
		const FString P = Preset.ToLower();
		const TCHAR* Path = TEXT("/RUDE/Masters/M_RUDE_Opaque.M_RUDE_Opaque");
		if (P.Contains(TEXT("decal")))       { Path = TEXT("/RUDE/Masters/M_RUDE_Decal.M_RUDE_Decal"); }
		else if (P.Contains(TEXT("cutout"))) { Path = TEXT("/RUDE/Masters/M_RUDE_Cutout.M_RUDE_Cutout"); }
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
		UMaterialInterface* Master = Def ? MasterForPreset(Def->Preset) : nullptr;
		const FString ConfigKey = Def
			? (Def->Preset + TEXT("|") + Def->Diffuse + TEXT("|") + Def->Normal + TEXT("|") + Def->Specular).ToLower()
			: FString();
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
				UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
					MIPackage, FName(*MIName), RF_Public | RF_Standalone);
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
	const int32 TEX_BASE = 0x450, TEX_SZ = 0x90, NAME_SLOT = 0x20;
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
		for (int32 k = 0; k < Nm.Len() && (NameOff[i] + k) < (int32)SysSize; ++k)
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
	static const int32 POLYS_PER_LEAF = 4;

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
	auto Emit = [&Seg](const TArray<uint8>& D, int32 Align = 16) -> int32
	{
		if (Seg.Num() % Align) { Seg.AddZeroed(Align - (Seg.Num() % Align)); }
		const int32 O = Seg.Num(); Seg.Append(D); return O;
	};
	const int32 OPoly = Emit(PolyBytes);
	const int32 ONode = Emit(NodeBytes);
	const int32 OVert = Emit(VertBytes);
	// NOTE: child +0xb8 (m_CompressedShrunkVertices) is left NULL - CW's known-good binary
	// leaves it null and loads fine, so it is NOT required (an earlier theory that it caused
	// the fixup crash was disproven by diffing CW's working output).
	const int32 OMidx = Emit(MatIdxBytes);

	TArray<uint8> Bvh; Bvh.AddZeroed(0x60);                       // phOptimizedBvh header
	RudeYbn::PPTR(Bvh, 0x00, ONode);
	RudeYbn::PU32(Bvh, 0x08, (uint32)Nodes.Num()); RudeYbn::PU32(Bvh, 0x0c, (uint32)Nodes.Num());
	{
		const float WB0[3] = { NBMin[0]+WorldCtr[0], NBMin[1]+WorldCtr[1], NBMin[2]+WorldCtr[2] };
		const float WB1[3] = { NBMax[0]+WorldCtr[0], NBMax[1]+WorldCtr[1], NBMax[2]+WorldCtr[2] };
		RudeYbn::PVEC3(Bvh, 0x20, WB0); RudeYbn::PU32(Bvh, 0x2c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x30, WB1); RudeYbn::PU32(Bvh, 0x3c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x40, WorldCtr); RudeYbn::PU32(Bvh, 0x4c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x50, NInv); RudeYbn::PU32(Bvh, 0x5c, 0xffc00000u);
	}
	const int32 OBvh = Emit(Bvh);

	TArray<uint8> Xf; Xf.AddZeroed(0x40);                         // child transform = identity
	RudeYbn::PF32(Xf, 0x00, 1.f); RudeYbn::PF32(Xf, 0x14, 1.f); RudeYbn::PU32(Xf, 0x1c, 1);
	RudeYbn::PF32(Xf, 0x28, 1.f); RudeYbn::PU32(Xf, 0x2c, 1);
	const int32 OXf = Emit(Xf);

	TArray<uint8> F0; F0.AddZeroed(0x20);                          // child+0xf0 block (zero)
	const int32 OF0 = Emit(F0);
	TArray<uint8> BlockMap; BlockMap.AddZeroed(0x40); RudeYbn::PU32(BlockMap, 0x08, 2);
	const int32 OBm = Emit(BlockMap);

	TArray<uint8> ChildBox; ChildBox.AddZeroed(0x20);              // [BoxMin.vec4, BoxMax.vec4] WORLD
	RudeYbn::PVEC3(ChildBox, 0x00, WorldMin); RudeYbn::PU32(ChildBox, 0x0c, 1);
	RudeYbn::PVEC3(ChildBox, 0x10, WorldMax); RudeYbn::PF32(ChildBox, 0x1c, RudeYbn::CHILD_MARGIN);
	const int32 OBbox = Emit(ChildBox);
	TArray<uint8> Fl1; Fl1.AddZeroed(4); RudeYbn::PU32(Fl1, 0, RudeYbn::CHILD_FLAGS1);
	const int32 OFl1 = Emit(Fl1);
	TArray<uint8> Fl2; Fl2.AddZeroed(4); RudeYbn::PU32(Fl2, 0, RudeYbn::CHILD_FLAGS2);
	const int32 OFl2 = Emit(Fl2);

	// --- phBoundGeometryBVH child header (0x140) ---
	TArray<uint8> Ch; Ch.AddZeroed(0x140);
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
	RudeYbn::PPTR(Ch, 0xf0, OF0);
	RudeYbn::PPTR(Ch, 0x118, OMidx);
	RudeYbn::PPTR(Ch, 0x130, OBvh);
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

	// --- container: RSC7 v43 (system segment only). Page flags reverse-engineered from CW's
	// known-good output: system pages cap at 64KB (a 128KB single page is rejected at load).
	// SysPageFlags pads the segment and emits CW's exact encoding (0x20000 -> two 64KB pages
	// -> 0x20000040; <=64KB -> one pow2 page like real small ybns).
	uint32 Padded = 0;
	const uint32 SysFlag = 0x20000000u | RudeYbn::SysPageFlags((uint32)Seg.Num(), Padded);
	const uint32 GfxFlag = 0xb0000000u;
	Seg.SetNumZeroed((int32)Padded);

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
