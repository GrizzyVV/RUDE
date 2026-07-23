// RUDE - RAGE <-> Unreal Development Environment
// The RUDE MCP toolset: RAGE format import/export exposed as agent-callable tools.
#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "RudeToolset.generated.h"

// RUDE toolset - RAGE (GTA V) format import/export tools.
// Import CodeWalker-XML drawables as StaticMesh assets, with the RUDE
// GTA<->UE transform convention applied (cm scale, Y mirror, winding flip).
UCLASS()
class URudeToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("0.1.0"); }

	// Smoke test: returns the RUDE plugin version and status.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString Ping();

	// Import a CodeWalker ydr XML file as a UStaticMesh asset.
	// XmlPath: absolute path to a *.ydr.xml file on disk.
	// DestFolder: content folder for the new asset (e.g. "/Game/RUDE/Meshes/Props").
	// Returns a JSON string: {ok, assetPath, geometries, vertices, triangles, slots}
	// or {ok:false, error} on failure.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ImportYdr(const FString& XmlPath, const FString& DestFolder);

	// Import a CodeWalker ytd XML manifest as UTexture2D assets with correct
	// semantics derived from each entry's Usage (NORMAL -> TC_Normalmap + sRGB off,
	// SPECULAR -> sRGB off, DIFFUSE -> sRGB on).
	// XmlPath: absolute path to a *.ytd.xml file.
	// PixelFolder: folder of decoded PNGs matching the entry names (BC-decode
	// happens offline until native decode lands).
	// DestFolder: content folder root; assets land in <DestFolder>/<TxdName>/.
	// Returns JSON: {ok, txd, imported, missingPixels:[...]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ImportYtd(const FString& XmlPath, const FString& PixelFolder,
	                         const FString& DestFolder);

	// Export a UStaticMesh as a CodeWalker-compatible ydr XML file (the reverse
	// lane). Positions/normals/UVs inverse-transformed per the RUDE convention;
	// shader presets recovered from material slot names; texture names recovered
	// from bound RUDE MaterialInstances where present.
	// AssetPath: content path of the StaticMesh (e.g. "/Game/RUDE/Meshes/Props/prop_x").
	// OutXmlPath: absolute file path for the emitted *.ydr.xml.
	// Returns JSON: {ok, xmlPath, geometries, vertices, triangles} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYdr(const FString& AssetPath, const FString& OutXmlPath);

	// Export a UStaticMesh's collision as a CodeWalker .ybn XML (physics bounds).
	// Mesh-accurate GeometryBVH from the render triangles. Drawable props get
	// collision from an EXTERNAL physics dict (proven in-game) - reference this
	// via the archetype's physicsDictionary + flag bit 0x20000. Verts inverse-
	// transformed per the RUDE convention.
	// Returns JSON: {ok, xmlPath, vertices, triangles} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYbn(const FString& AssetPath, const FString& OutXmlPath);
};
