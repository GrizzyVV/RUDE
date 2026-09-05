// RUDE - RAGE <-> Unreal Development Environment
#include "RudeDds.h"
#include "Misc/FileHelper.h"

namespace
{
	uint32 U32(const uint8* P) { return (uint32)P[0] | ((uint32)P[1] << 8) | ((uint32)P[2] << 16) | ((uint32)P[3] << 24); }
	uint16 U16(const uint8* P) { return (uint16)((uint16)P[0] | ((uint16)P[1] << 8)); }

	struct FRgba { uint8 R, G, B, A; };

	FRgba From565(uint16 C)
	{
		const uint32 R5 = (C >> 11) & 31, G6 = (C >> 5) & 63, B5 = C & 31;
		return { (uint8)((R5 << 3) | (R5 >> 2)), (uint8)((G6 << 2) | (G6 >> 4)), (uint8)((B5 << 3) | (B5 >> 2)), 255 };
	}

	void PutTexel(FRudeDdsImage& Img, int32 X, int32 Y, FRgba C)
	{
		if (X >= Img.Width || Y >= Img.Height) { return; }
		uint8* D = Img.Bgra.GetData() + ((Y * Img.Width + X) * 4);
		D[0] = C.B; D[1] = C.G; D[2] = C.R; D[3] = C.A;
	}

	// BC1 colour block (8 bytes). bFourColour forces the 4-colour ramp, which BC2/BC3 always use.
	void DecodeColourBlock(const uint8* B, FRgba Out[16], bool bFourColour)
	{
		const uint16 C0 = U16(B), C1 = U16(B + 2);
		const FRgba P0 = From565(C0), P1 = From565(C1);
		FRgba P[4];
		P[0] = P0; P[1] = P1;
		if (bFourColour || C0 > C1)
		{
			P[2] = { (uint8)((2 * P0.R + P1.R) / 3), (uint8)((2 * P0.G + P1.G) / 3), (uint8)((2 * P0.B + P1.B) / 3), 255 };
			P[3] = { (uint8)((P0.R + 2 * P1.R) / 3), (uint8)((P0.G + 2 * P1.G) / 3), (uint8)((P0.B + 2 * P1.B) / 3), 255 };
		}
		else
		{
			P[2] = { (uint8)((P0.R + P1.R) / 2), (uint8)((P0.G + P1.G) / 2), (uint8)((P0.B + P1.B) / 2), 255 };
			P[3] = { 0, 0, 0, 0 };
		}
		const uint32 Idx = U32(B + 4);
		for (int32 i = 0; i < 16; ++i) { Out[i] = P[(Idx >> (2 * i)) & 3]; }
	}

	// BC4 block (8 bytes): two 8-bit endpoints + 16 × 3-bit indices, LSB first.
	void DecodeAlphaBlock(const uint8* B, uint8 Out[16])
	{
		const uint8 A0 = B[0], A1 = B[1];
		uint8 A[8];
		A[0] = A0; A[1] = A1;
		if (A0 > A1)
		{
			for (int32 i = 1; i <= 6; ++i) { A[i + 1] = (uint8)(((7 - i) * A0 + i * A1) / 7); }
		}
		else
		{
			for (int32 i = 1; i <= 4; ++i) { A[i + 1] = (uint8)(((5 - i) * A0 + i * A1) / 5); }
			A[6] = 0; A[7] = 255;
		}
		uint64 Bits = 0;
		for (int32 i = 0; i < 6; ++i) { Bits |= (uint64)B[2 + i] << (8 * i); }
		for (int32 i = 0; i < 16; ++i) { Out[i] = A[(Bits >> (3 * i)) & 7]; }
	}

	enum class EKind { BC1, BC2, BC3, BC4, BC5, Linear };

	struct FLayout
	{
		EKind Kind = EKind::Linear;
		int32 BlockBytes = 0;   // block formats
		int32 Bpp = 0;          // linear formats (bytes per pixel)
		uint32 RMask = 0, GMask = 0, BMask = 0, AMask = 0;
		bool bLuminance = false, bAlphaOnly = false;
		FString Name;
	};

	uint8 Extract(uint32 V, uint32 Mask)
	{
		if (Mask == 0) { return 0; }
		int32 Shift = 0; uint32 M = Mask;
		while (!(M & 1)) { M >>= 1; ++Shift; }
		int32 Bits = 0; while (M & 1) { M >>= 1; ++Bits; }
		uint32 X = (V & Mask) >> Shift;
		if (Bits >= 8) { return (uint8)(X >> (Bits - 8)); }
		// widen a narrow channel by bit replication (5 -> 8: x<<3 | x>>2)
		uint32 Out = X << (8 - Bits);
		Out |= Out >> Bits;
		return (uint8)Out;
	}
}

bool FRudeDds::Load(const FString& Path, FRudeDdsImage& Out, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	return Decode(Bytes, Out, OutError);
}

bool FRudeDds::Decode(const TArray<uint8>& Bytes, FRudeDdsImage& Out, FString& OutError)
{
	if (Bytes.Num() < 128 || Bytes[0] != 'D' || Bytes[1] != 'D' || Bytes[2] != 'S' || Bytes[3] != ' ')
	{
		OutError = TEXT("not a DDS file");
		return false;
	}
	const uint8* H = Bytes.GetData();
	if (U32(H + 4) != 124) { OutError = TEXT("DDS header size is not 124"); return false; }
	const int32 Height = (int32)U32(H + 12);
	const int32 Width = (int32)U32(H + 16);
	const int32 Mips = (int32)U32(H + 28);
	if (Width <= 0 || Height <= 0 || Width > 16384 || Height > 16384)
	{
		OutError = FString::Printf(TEXT("implausible size %dx%d"), Width, Height);
		return false;
	}
	const uint8* PF = H + 76;
	const uint32 PfFlags = U32(PF + 4);
	const uint32 FourCC = U32(PF + 8);
	const uint32 BitCount = U32(PF + 12);
	FLayout L;
	if (PfFlags & 0x4)   // DDPF_FOURCC
	{
		const char F[5] = { (char)(FourCC & 0xFF), (char)((FourCC >> 8) & 0xFF), (char)((FourCC >> 16) & 0xFF), (char)((FourCC >> 24) & 0xFF), 0 };
		const FString Four(ANSI_TO_TCHAR(F));
		if (Four == TEXT("DXT1")) { L.Kind = EKind::BC1; L.BlockBytes = 8; L.Name = TEXT("D3DFMT_DXT1"); }
		else if (Four == TEXT("DXT3")) { L.Kind = EKind::BC2; L.BlockBytes = 16; L.Name = TEXT("D3DFMT_DXT3"); }
		else if (Four == TEXT("DXT5")) { L.Kind = EKind::BC3; L.BlockBytes = 16; L.Name = TEXT("D3DFMT_DXT5"); }
		else if (Four == TEXT("ATI1") || Four == TEXT("BC4U")) { L.Kind = EKind::BC4; L.BlockBytes = 8; L.Name = TEXT("D3DFMT_ATI1"); }
		else if (Four == TEXT("ATI2") || Four == TEXT("BC5U")) { L.Kind = EKind::BC5; L.BlockBytes = 16; L.Name = TEXT("D3DFMT_ATI2"); }
		else if (Four == TEXT("DX10"))
		{
			OutError = TEXT("BC7 (DX10 FourCC): no decoder in RUDE yet - refused, counted");
			return false;
		}
		else
		{
			OutError = FString::Printf(TEXT("unknown FourCC '%s' - refused"), *Four);
			return false;
		}
	}
	else if (PfFlags & 0x40)   // DDPF_RGB (+ optional ALPHAPIXELS 0x1)
	{
		L.Kind = EKind::Linear;
		L.RMask = U32(PF + 16); L.GMask = U32(PF + 20); L.BMask = U32(PF + 24);
		L.AMask = (PfFlags & 0x1) ? U32(PF + 28) : 0;
		L.Bpp = (int32)(BitCount / 8);
		if (BitCount == 32 && L.RMask == 0x00FF0000) { L.Name = L.AMask ? TEXT("D3DFMT_A8R8G8B8") : TEXT("D3DFMT_X8R8G8B8"); }
		else if (BitCount == 32 && L.RMask == 0x000000FF) { L.Name = TEXT("D3DFMT_A8B8G8R8"); }
		else if (BitCount == 16 && L.RMask == 0x7C00) { L.Name = TEXT("D3DFMT_A1R5G5B5"); }
		else
		{
			OutError = FString::Printf(TEXT("unsupported RGB mask layout (%u bpp, R %08x) - refused"), BitCount, L.RMask);
			return false;
		}
	}
	else if (PfFlags & 0x2)   // DDPF_ALPHA: alpha only
	{
		L.Kind = EKind::Linear; L.bAlphaOnly = true; L.Bpp = 1; L.AMask = U32(PF + 28); L.Name = TEXT("D3DFMT_A8");
		if (BitCount != 8) { OutError = TEXT("alpha-only texture that is not 8 bpp - refused"); return false; }
	}
	else if (PfFlags & 0x20000)   // DDPF_LUMINANCE
	{
		L.Kind = EKind::Linear; L.bLuminance = true; L.Bpp = 1; L.RMask = U32(PF + 16); L.Name = TEXT("D3DFMT_L8");
		if (BitCount != 8) { OutError = TEXT("luminance texture that is not 8 bpp - refused"); return false; }
	}
	else
	{
		OutError = FString::Printf(TEXT("unrecognised pixel-format flags %08x - refused"), PfFlags);
		return false;
	}

	Out.Width = Width; Out.Height = Height; Out.MipCount = Mips; Out.Format = L.Name;
	Out.Bgra.SetNumUninitialized(Width * Height * 4);
	const uint8* Data = H + 128;
	const int64 Avail = Bytes.Num() - 128;

	if (L.Kind == EKind::Linear)
	{
		const int64 Need = (int64)Width * Height * L.Bpp;
		if (Avail < Need) { OutError = FString::Printf(TEXT("truncated: mip 0 needs %lld bytes, file holds %lld"), Need, Avail); return false; }
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const uint8* P = Data + ((int64)Y * Width + X) * L.Bpp;
				FRgba C;
				if (L.bAlphaOnly) { C = { 255, 255, 255, P[0] }; }
				else if (L.bLuminance) { C = { P[0], P[0], P[0], 255 }; }
				else
				{
					uint32 V = 0;
					for (int32 k = 0; k < L.Bpp; ++k) { V |= (uint32)P[k] << (8 * k); }
					C = { Extract(V, L.RMask), Extract(V, L.GMask), Extract(V, L.BMask), (uint8)(L.AMask ? Extract(V, L.AMask) : 255) };
				}
				PutTexel(Out, X, Y, C);
			}
		}
		return true;
	}

	const int32 BW = (Width + 3) / 4, BH = (Height + 3) / 4;
	const int64 Need = (int64)BW * BH * L.BlockBytes;
	if (Avail < Need) { OutError = FString::Printf(TEXT("truncated: mip 0 needs %lld bytes, file holds %lld"), Need, Avail); return false; }
	for (int32 By = 0; By < BH; ++By)
	{
		for (int32 Bx = 0; Bx < BW; ++Bx)
		{
			const uint8* Blk = Data + ((int64)By * BW + Bx) * L.BlockBytes;
			FRgba Tex[16];
			switch (L.Kind)
			{
			case EKind::BC1:
				DecodeColourBlock(Blk, Tex, false);
				break;
			case EKind::BC2:
			{
				DecodeColourBlock(Blk + 8, Tex, true);
				uint64 ABits = 0;
				for (int32 i = 0; i < 8; ++i) { ABits |= (uint64)Blk[i] << (8 * i); }
				for (int32 i = 0; i < 16; ++i) { const uint8 A4 = (uint8)((ABits >> (4 * i)) & 15); Tex[i].A = (uint8)(A4 * 17); }
				break;
			}
			case EKind::BC3:
			{
				DecodeColourBlock(Blk + 8, Tex, true);
				uint8 A[16]; DecodeAlphaBlock(Blk, A);
				for (int32 i = 0; i < 16; ++i) { Tex[i].A = A[i]; }
				break;
			}
			case EKind::BC4:
			{
				uint8 R[16]; DecodeAlphaBlock(Blk, R);
				for (int32 i = 0; i < 16; ++i) { Tex[i] = { R[i], R[i], R[i], 255 }; }
				break;
			}
			case EKind::BC5:
			{
				// Two channels: X (red) then Y (green). Z is reconstructed for a tangent-space
				// normal so the image reads right unlit; UE's normal-map path re-derives it anyway.
				uint8 R[16], G[16]; DecodeAlphaBlock(Blk, R); DecodeAlphaBlock(Blk + 8, G);
				for (int32 i = 0; i < 16; ++i)
				{
					const float Nx = R[i] / 127.5f - 1.f, Ny = G[i] / 127.5f - 1.f;
					const float Nz = FMath::Sqrt(FMath::Max(0.f, 1.f - Nx * Nx - Ny * Ny));
					Tex[i] = { R[i], G[i], (uint8)FMath::Clamp(FMath::RoundToInt((Nz + 1.f) * 127.5f), 0, 255), 255 };
				}
				break;
			}
			default: break;
			}
			for (int32 i = 0; i < 16; ++i) { PutTexel(Out, Bx * 4 + (i & 3), By * 4 + (i >> 2), Tex[i]); }
		}
	}
	return true;
}
