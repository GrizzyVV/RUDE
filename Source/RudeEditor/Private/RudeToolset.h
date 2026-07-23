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
};
