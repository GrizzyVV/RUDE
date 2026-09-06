// RUDE - RAGE <-> Unreal Development Environment
// Private bridge from the binary lane's file-local machinery (RudeBinaryLanes.cpp) to the other binary
// writers (RudeSkinnedWriter.cpp, WP11). The implementations stay in RudeBinaryLanes.cpp - one law, one
// place; these are the seams the ydd writer needed and nothing more.
#pragma once

#include "CoreMinimal.h"

// RSC7 file -> inflated system + graphics segments (v165 only; a v159 Enhanced file is refused by name).
bool RudeBinLoadRsc7(const FString& Path, TArray<uint8>& OutSys, TArray<uint8>& OutGfx, uint32& OutVersion, FString& OutError);
// grcFvf decode: channel mask + the 16-nibble type table + the declared stride -> per-channel byte offsets
// (-1 = absent). False = a declaration the reader does not understand (refuse, never misalign).
bool RudeBinBuildDecl(uint32 Mask, uint64 Nibbles, int32 DeclStride, int32 OutOfs[16], FString& OutError);
// The uniform-page RSC7 flag encoding (low 28 bits) the ydr/ybn writers use; 0xFFFFFFFF = unencodable.
uint32 RudeBinSysPageFlagsUniform(uint32 RawSize, uint32 PageSize, uint32& OutPadded, uint32& OutPages);
// The single-ownership / geoBounds / declaration self-check over ONE gtaDrawable at Base inside Sys, folding
// its owners into InDeg (shared across a dictionary's entries). Adds to the three counters; returns their sum.
int32 RudeBinVerifyDrawable(const TArray<uint8>& Sys, int32 Base, TMap<int32, int32>& InDeg,
                            int32& OutShared, int32& OutDeclBad, int32& OutBoundsBad, FString& OutFirstProblem);
// Register one owner of a tagged system pointer in the shared map (the dictionary-level slots the drawable
// check does not know: hash array, entry array, records, per-geometry bone-id tables).
void RudeBinOwn(TMap<int32, int32>& InDeg, uint32 Tagged, const FString& Label, int32& OutShared, FString& OutFirstProblem);
