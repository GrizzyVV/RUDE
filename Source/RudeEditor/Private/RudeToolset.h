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

	// Batch ImportYdr: ListPath = a text file of absolute *.ydr.xml paths, one per line.
	// Imports each as a UStaticMesh into DestFolder via the same path as ImportYdr,
	// SKIPPING files whose target asset already exists (idempotent re-runs; the P4
	// hash-manifest resumability model, name-level) UNLESS Mode is "FORCE" - then every
	// file reimports in place (rebinds MaterialInstances against currently-imported
	// textures; the texture-pass re-bind flow). Progress goes to the log.
	// Returns JSON: {ok, imported, skipped, failed, failedFiles:[...first 30]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ImportYdrBatch(const FString& ListPath, const FString& DestFolder,
	                              const FString& Mode);

	// Spawn a scene manifest (tools/ingest_ymap.py output: JSON array of
	// {ymap, entities:[{archetype, drawable, ue_location, ue_quat, scaleXY, scaleZ,
	// lodLevel, resolved}]}) into the CURRENT editor level: one actor per ymap holding
	// one InstancedStaticMeshComponent per unique drawable (the P4 ISM-first scale
	// model). Entities are filtered to HD/ORPHANHD lod levels unless Filter == "ALL".
	// Meshes resolve by lowercase drawable name under MeshFolder; unresolved archetypes
	// and missing meshes render as proxy cubes (corpus-hole policy). Transforms are the
	// manifest's UE-space values (already through the pinned GTA->UE convention).
	// Returns JSON: {ok, ymaps, entities, instances, proxies, uniqueMeshes,
	// missingMeshes, topMissing:[...first 20]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ImportScene(const FString& ManifestPath, const FString& MeshFolder,
	                           const FString& Filter);

	// Export a UStaticMesh as a CodeWalker-compatible ydr XML file (the reverse
	// lane). Positions/normals/UVs inverse-transformed per the RUDE convention;
	// shader presets recovered from material slot names; texture names recovered
	// from bound RUDE MaterialInstances where present.
	// AssetPath: content path of the StaticMesh (e.g. "/Game/RUDE/Meshes/Props/prop_x").
	// OutXmlPath: absolute file path for the emitted *.ydr.xml.
	// Returns JSON: {ok, xmlPath, geometries, vertices, triangles} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYdr(const FString& AssetPath, const FString& OutXmlPath);

	// Export a UStaticMesh's collision as a standalone CodeWalker .ybn XML (physics
	// bounds) - a valid, CW-previewable BoundsFile. NOTE (corrected 2026-07-24):
	// PROP collision actually comes from the ydr's EMBEDDED <Bounds> + archetype flag
	// bit 0x20000, NOT a standalone .ybn (props share NAMED-bound dictionaries; a
	// standalone unnamed bound never matches). ExportYdr embeds the real collider;
	// this tool remains for shared/world collision-dictionary work later. Verts
	// inverse-transformed per the RUDE convention.
	// Returns JSON: {ok, xmlPath, vertices, triangles} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYbn(const FString& AssetPath, const FString& OutXmlPath);

	// Export a UTexture2D's source pixels to a PNG on disk (feeds the PBR->RAGE
	// ytd pipeline). Reads the texture Source (BGRA8), writes PNG via ImageWrapper.
	// TexturePath: content path of the Texture2D. OutPngPath: absolute *.png path.
	// Returns JSON: {ok, pngPath, width, height} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportTexture(const FString& TexturePath, const FString& OutPngPath);

	// Export UTexture2D(s) directly to a binary FiveM .ytd (RSC7 v13) - CLEAN-ROOM,
	// no CodeWalker. The RSC7 container (pgDictionary<grcTexture> system segment +
	// page-aligned graphics segment + segment flags + raw deflate) was reversed from
	// our own CW diff pair and byte-verified (tools/write_ytd.py; ENGINEERING_LOG
	// "RSC7 binary container"). This is P5 step 1 - deleting CodeWalker for textures.
	// TextureSpecs: comma-separated entries "ContentPath;RageName[;Usage]" (Usage in
	// DIFFUSE|NORMAL|SPECULAR; drives grcTexture semantics). OutYtdPath: absolute *.ytd.
	// MaxDim: box-downscale any texture whose W or H exceeds this (power-of-two halving);
	// "0"/empty = no cap. Uncompressed A8R8G8B8 is heavy - a 4096^2 = 64MB and FiveM will
	// crash the GPU on oversized assets, so cap until DXT/BC compression lands (v2).
	// v1 emits A8R8G8B8, 1 mip, 4MB-page-aligned graphics. Returns JSON:
	// {ok, ytdPath, textures, bytes, sysFlags, gfxFlags} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYtdBinary(const FString& TextureSpecs, const FString& OutYtdPath,
	                               const FString& MaxDim);

	// Export a UStaticMesh directly to a binary FiveM .ydr (RSC7 v165) - CLEAN-ROOM,
	// no CodeWalker. P5 step 3, the LAST CodeWalker dependency. Emits a gtaDrawable:
	// GTAV1 vertex buffers (Pos/Normal/Colour0/UV, 36B stride) + u16 index buffers per
	// polygon group, ShaderGroup with the normal_spec/spec parameter template (external
	// -ytd texture stubs resolved by name), and the EMBEDDED phBoundComposite collision
	// (same serialization as ExportYbnBinary - whole-mesh GeometryBVH). All-in-system
	// (gfx=0), page-aware layout. Struct map: docs/ENGINEERING_LOG "ydr binary format".
	// AssetPath: content path of the StaticMesh. OutYdrPath: absolute *.ydr path.
	// Returns JSON: {ok, ydrPath, geometries, vertices, triangles, bytes, sysFlags}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYdrBinary(const FString& AssetPath, const FString& OutYdrPath);

	// Export a UStaticMesh's collision directly to a binary FiveM .ybn (RSC7 v43) -
	// CLEAN-ROOM, no CodeWalker. P5 step 2. Emits a phBoundComposite wrapping one
	// phBoundGeometryBVH: quantized vertices, 16-byte triangles, u8 material indices,
	// and a CONSTRUCTED stackless phOptimizedBvh (escape-index tree + the mandatory
	// m_Trees subtree table: maximal <=127-node subtree ranges). Every phBound
	// struct is built from pinned field offsets (docs/ENGINEERING_LOG "ybn binary
	// format") - no template bytes, so it generalizes to any mesh.
	// FRAME CONVENTION (pinned against the real ybn): header boxes, CenterGeom and the
	// BVH header boxes are WORLD space; stored vertices are s16 quantized RELATIVE to
	// CenterGeom, so a reader recovers world = s16*Quantum + CenterGeom.
	// AssetPath: content path of the StaticMesh. OutYbnPath: absolute *.ybn path.
	// WorldOffset: optional "x,y,z" in GTA world metres, ADDED to the mesh's gta-space
	// vertices. Static world collision (map tiles) stores geometry in ABSOLUTE world
	// coordinates, so this places the bound where it belongs on the map; empty = none.
	// Returns JSON: {ok, ybnPath, vertices, triangles, bvhNodes, bytes, sysFlags}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable))
	static FString ExportYbnBinary(const FString& AssetPath, const FString& OutYbnPath,
	                               const FString& WorldOffset);
};
