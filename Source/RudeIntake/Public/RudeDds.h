// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"

// What a DDS sidecar decoded to. Mip 0 only: UE regenerates the chain from the source image, and
// a stored chain carries nothing the top level does not.
struct RUDEINTAKE_API FRudeDdsImage
{
	int32 Width = 0;
	int32 Height = 0;
	int32 MipCount = 0;       // as the header declares (informational)
	FString Format;           // D3DFMT spelling, as ROUT's <Format> element spells it
	TArray<uint8> Bgra;       // Width * Height * 4, B G R A order (UE's TSF_BGRA8)
};

// Reads a DDS file as ROUT writes it (a 128-byte DX9 header, no DXT10 extension) and decodes mip 0
// to 8-bit BGRA. Clean-room from the public DDS + BCn block specifications.
//
// Decoded: DXT1 (BC1) · DXT3 (BC2) · DXT5 (BC3) · ATI1/BC4U (BC4) · ATI2/BC5U (BC5) ·
//          A8R8G8B8 · X8R8G8B8 · A8B8G8R8 · A1R5G5B5 · A8 · L8.
// Refused with a reason (never guessed): BC7 (ROUT spells it "DX10" without an extension header;
// no decoder here yet), any other FourCC, any mask layout outside the list above.
class RUDEINTAKE_API FRudeDds
{
public:
	static bool Load(const FString& Path, FRudeDdsImage& Out, FString& OutError);
	static bool Decode(const TArray<uint8>& Bytes, FRudeDdsImage& Out, FString& OutError);
};
