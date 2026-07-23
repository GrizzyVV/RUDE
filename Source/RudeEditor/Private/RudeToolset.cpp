// RUDE - RAGE <-> Unreal Development Environment
#include "RudeToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
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

	// Shader preset names (for material slot naming)
	TArray<FString> Shaders;
	if (const FXmlNode* SG = Root->FindChildNode(TEXT("ShaderGroup")))
	{
		if (const FXmlNode* Sh = SG->FindChildNode(TEXT("Shaders")))
		{
			for (const FXmlNode* Item : Sh->GetChildrenNodes())
			{
				const FXmlNode* SName = Item->FindChildNode(TEXT("Name"));
				Shaders.Add(SName ? SName->GetContent().TrimStartAndEnd() : TEXT("default"));
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
			? Shaders[Geo.ShaderIndex] : TEXT("default");
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

	// One material slot per geometry, named after its RAGE shader preset
	for (const FString& Slot : SlotNames)
	{
		FStaticMaterial Mat(UMaterial::GetDefaultMaterial(MD_Surface), FName(*Slot));
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
		TEXT("{\"ok\":true,\"assetPath\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"slots\":[%s]}"),
		*PackageName, Geos.Num(), TotalVerts, TotalTris, *SlotsJson);
}
