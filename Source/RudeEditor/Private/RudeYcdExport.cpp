// RUDE - RAGE <-> Unreal Development Environment
//
// WP12 ycd_export lane: UAnimSequence -> .ycd clip dictionary XML. The WRITE half of RudeAnims.cpp.
// Every structural claim below was MEASURED on the corpus (maintainer lane `ycd_export` (`LAWS.md`),
// 2026-09-06): 24,844 .ycd.xml in the population, a seeded 303-file sample (2,582 clips, 2,206
// animations, 2,673 sequences, 106,005 BoneIds rows, 278,075 channels), plus a 40-file draw used for
// the float-spelling and round-trip numbers (1,414,618 spelled floats; 10,746 QuantizeFloat channels
// / 1,382,180 frame values; 3,357 cached-quaternion items / 1,089,954 label values).
//
// THE THREE RESULTS THIS FILE IS BUILT ON (LAWS.md G1/G2/G3, F1, H1-H3):
//  * metres -> UE centimetres -> metres is NOT the float32 identity (1,199,600/1,382,180 = 86.79%
//    exact, worst 3.052e-05 m), and the quaternion normalise on import loses a little too
//    (1,259,661/1,259,820 = 99.9874%, worst 6.557e-07);
//  * BUT requantising against the DONOR channel's own Quantum/Offset absorbs both completely -
//    1,382,180/1,382,180 frame values and 1,089,954/1,089,954 rotation labels come back
//    byte-identical. So this exporter preserves Quantum/Offset and re-derives only the raws.
//    The one shape that cannot absorb it is a StaticFloat label (no quantum to round into): 53 of
//    them moved, so a StaticFloat is CARRIED VERBATIM and a genuine edit to one is REFUSED.
//  * the corpus float spelling is 7 significant digits widening to 9 when 7 does not round-trip
//    float32, ties AWAY FROM ZERO, %G fixed/scientific style - 1,414,618/1,414,618 exact. The two
//    plausible alternatives are refuted in LAWS.md F1 (shortest-round-trip: 1,161/131,483;
//    plain %.7G/%.9G half-to-even: 57,737/131,483).
//
// WHY XML AND NOT BINARY - MEASURED, NOT ASSUMED (LAWS.md H): the maintainer's own C++ exporter has
// no .ycd writer at all (37 .cpp, one names ycd and it is the reader, 0 named *_write or xml2*), and
// its Python write direction is binary->binary donor repack - no ycd module reads XML (0 hits for
// ElementTree/minidom across every ycd module and every *_write module; only 3 of ~26 lanes have an
// XML->binary entry point and ycd is not one of them). So RUDE emits XML, in the exact form the
// round-trip referee compares. Turning that XML into a .ycd image is NOT done and is NOT claimed.
//
// HOW IT WRITES: a template dictionary is REQUIRED (an animation carries eight fields, a sequence
// three, and the whole RecordUnknown00 block, that no UAnimSequence models - LAWS.md C1/C4/A4/B4/B5).
// The template's BYTES are copied and only the channel number lines being authored are replaced, so
// an untouched dictionary is byte-identical by construction and an authored one is a surgical diff.
// The line-oriented rewrite is deliberate: FXmlFile does not preserve line structure (AGENTS.md
// section 6.5) and "ten values per line" (131,483/131,483) and "one space per depth" (303/303) are
// line laws.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

namespace RudeYcdOut
{
	static FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	}
	static FString JStr(const FString& S) { return TEXT("\"") + RudeJsonEscape(S) + TEXT("\""); }

	// ---- F1: THE CORPUS FLOAT SPELLING -------------------------------------------------------------
	// 7 significant digits, widening to 9 only when 7 does not round-trip to the identical float32;
	// ties AWAY FROM ZERO; %G's fixed-vs-scientific split (scientific iff exponent < -4 or >= digits);
	// trailing zeros and a trailing point stripped; uppercase E with a sign and two exponent digits.
	// Measured 1,414,618/1,414,618 floats across 40 files (LAWS.md F1).
	//
	// The rounding is done on the EXACT decimal expansion of the float32, not on a printf, because C
	// and Python both round ties half-to-even and this format rounds them away from zero - that single
	// difference is the whole residual (LAWS.md F1: plain %.7G matches only 57,737/131,483 Offsets).
	// A float32 magnitude is m * 2^e with m < 2^24 and e in [-149, 104], so the expansion is exact in
	// at most ~112 decimal digits and is built here by repeated small multiplication.

	// Least-significant-digit-first decimal magnitude.
	static void DecFromU64(uint64 V, TArray<uint8>& Out)
	{
		Out.Reset();
		if (V == 0) { Out.Add(0); return; }
		while (V > 0) { Out.Add((uint8)(V % 10)); V /= 10; }
	}
	static void DecMulSmall(TArray<uint8>& D, uint8 K)
	{
		uint32 Carry = 0;
		for (int32 i = 0; i < D.Num(); ++i)
		{
			const uint32 T = (uint32)D[i] * (uint32)K + Carry;
			D[i] = (uint8)(T % 10);
			Carry = T / 10;
		}
		while (Carry > 0) { D.Add((uint8)(Carry % 10)); Carry /= 10; }
	}

	// The exact decimal of |F|: Digits (most significant first, no leading zero) and Adjusted, the
	// base-10 exponent of the leading digit (value == d0.d1d2... * 10^Adjusted).
	static void ExactDecimal(float F, TArray<uint8>& Digits, int32& Adjusted)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &F, sizeof(Bits));
		const uint32 Frac = Bits & 0x7FFFFFu;
		const int32 BiasedExp = (int32)((Bits >> 23) & 0xFFu);
		uint64 Mant = 0;
		int32 Exp2 = 0;
		if (BiasedExp == 0) { Mant = Frac; Exp2 = -149; }
		else { Mant = (uint64)Frac | 0x800000ull; Exp2 = BiasedExp - 150; }

		TArray<uint8> Low;   // least significant first
		DecFromU64(Mant, Low);
		int32 PointShift = 0;   // magnitude == integer(Low) * 10^-PointShift
		if (Exp2 >= 0)
		{
			for (int32 i = 0; i < Exp2; ++i) { DecMulSmall(Low, 2); }
		}
		else
		{
			PointShift = -Exp2;
			for (int32 i = 0; i < PointShift; ++i) { DecMulSmall(Low, 5); }
		}
		// strip the integer's leading zeros (they sit at the END of the LSB-first array)
		while (Low.Num() > 1 && Low.Last() == 0) { Low.Pop(); }
		Digits.Reset();
		Digits.Reserve(Low.Num());
		for (int32 i = Low.Num() - 1; i >= 0; --i) { Digits.Add(Low[i]); }
		Adjusted = (Digits.Num() - 1) - PointShift;
	}

	// Round Digits/Adjusted to NumSig significant digits, ties away from zero (the magnitude is
	// unsigned here, so half-up on the magnitude IS away from zero).
	static void RoundSig(TArray<uint8>& Digits, int32& Adjusted, int32 NumSig)
	{
		if (Digits.Num() <= NumSig) { return; }
		const bool bRoundUp = Digits[NumSig] >= 5;
		Digits.SetNum(NumSig);
		if (bRoundUp)
		{
			int32 i = NumSig - 1;
			for (; i >= 0; --i)
			{
				if (Digits[i] < 9) { ++Digits[i]; break; }
				Digits[i] = 0;
			}
			// every digit carried: the value became 1 followed by zeros, one decade up
			if (i < 0) { Digits.Insert((uint8)1, 0); Digits.SetNum(NumSig); ++Adjusted; }
		}
	}

	static FString RenderSig(const TArray<uint8>& DigitsIn, int32 Adjusted, int32 NumSig)
	{
		TArray<uint8> D = DigitsIn;
		while (D.Num() > 1 && D.Last() == 0) { D.Pop(); }   // trailing zeros never survive
		if (D.Num() == 1 && D[0] == 0) { return TEXT("0"); }
		FString S;
		if (Adjusted < -4 || Adjusted >= NumSig)
		{
			S.AppendChar((TCHAR)(TEXT('0') + D[0]));
			if (D.Num() > 1)
			{
				S.AppendChar(TEXT('.'));
				for (int32 i = 1; i < D.Num(); ++i) { S.AppendChar((TCHAR)(TEXT('0') + D[i])); }
			}
			S += FString::Printf(TEXT("E%s%02d"), Adjusted >= 0 ? TEXT("+") : TEXT("-"), FMath::Abs(Adjusted));
			return S;
		}
		if (Adjusted >= 0)
		{
			const int32 IntLen = Adjusted + 1;
			for (int32 i = 0; i < IntLen; ++i) { S.AppendChar(i < D.Num() ? (TCHAR)(TEXT('0') + D[i]) : TEXT('0')); }
			if (D.Num() > IntLen)
			{
				S.AppendChar(TEXT('.'));
				for (int32 i = IntLen; i < D.Num(); ++i) { S.AppendChar((TCHAR)(TEXT('0') + D[i])); }
			}
			return S;
		}
		S = TEXT("0.");
		for (int32 i = 0; i < (-Adjusted - 1); ++i) { S.AppendChar(TEXT('0')); }
		for (int32 i = 0; i < D.Num(); ++i) { S.AppendChar((TCHAR)(TEXT('0') + D[i])); }
		return S;
	}

	static FString SigSpell(float F, int32 NumSig)
	{
		TArray<uint8> Digits;
		int32 Adjusted = 0;
		ExactDecimal(F, Digits, Adjusted);
		RoundSig(Digits, Adjusted, NumSig);
		return RenderSig(Digits, Adjusted, NumSig);
	}

	// The spelling every number this exporter writes goes through.
	static FString YcdNum(double V)
	{
		const float F = (float)V;
		if (FMath::IsNaN(F))
		{
			uint32 W = 0;
			FMemory::Memcpy(&W, &F, sizeof(W));
			// The RAGE pad quiets to 0x7FC00001 the moment it becomes a double; any other payload
			// keeps its own reversible spelling (the corpus's own form - LAWS.md F1).
			return (W == 0x7FC00001u || W == 0x7F800001u) ? FString(TEXT("NaN")) : FString::Printf(TEXT("NaN:%08X"), W);
		}
		if (!FMath::IsFinite(F)) { return F > 0.f ? FString(TEXT("Infinity")) : FString(TEXT("-Infinity")); }
		if (F == 0.f)
		{
			uint32 W = 0;
			FMemory::Memcpy(&W, &F, sizeof(W));
			return (W & 0x80000000u) ? FString(TEXT("-0")) : FString(TEXT("0"));
		}
		const bool bNeg = F < 0.f;
		const float Mag = bNeg ? -F : F;
		FString S = SigSpell(Mag, 7);
		if ((float)FCString::Atod(*S) != Mag) { S = SigSpell(Mag, 9); }
		return bNeg ? (TEXT("-") + S) : S;
	}

	// ---- the template's line model -----------------------------------------------------------------
	static int32 IndentOf(const FString& L)
	{
		int32 N = 0;
		while (N < L.Len() && L[N] == TEXT(' ')) { ++N; }
		return N;
	}
	// value of attr `Key` on a one-line element, "" when absent
	static FString AttrOn(const FString& L, const TCHAR* Key)
	{
		const FString Needle = FString::Printf(TEXT("%s=\""), Key);
		const int32 At = L.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
		if (At == INDEX_NONE) { return FString(); }
		const int32 From = At + Needle.Len();
		const int32 To = L.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (To == INDEX_NONE) { return FString(); }
		return L.Mid(From, To - From);
	}
	// text between > and </ on a one-line element, "" when the line is not that shape
	static FString InnerOn(const FString& L)
	{
		const int32 A = L.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
		if (A == INDEX_NONE) { return FString(); }
		const int32 B = L.Find(TEXT("</"), ESearchCase::CaseSensitive, ESearchDir::FromStart, A);
		if (B == INDEX_NONE) { return FString(); }
		return L.Mid(A + 1, B - A - 1).TrimStartAndEnd();
	}
	// The tag test matches the WHOLE token, and that is not a nicety. It was a bare prefix test, and
	// the one character of slack was fatal: "<Values>" starts with "<Value", so the Value arm below
	// swallowed every <Values> line, the Values arm was unreachable, and EVERY QuantizeFloat channel
	// was scanned with an empty value list - an animation dictionary written with no animation data in
	// it, silently, because nothing downstream counted values against the text that holds them. The
	// name must END at the token: '>' (<Values>), ' ' (<Quantum value="...), '/' (<Value/>), or the
	// end of the line. Case-sensitive on purpose - FString::StartsWith defaults to IgnoreCase.
	static bool StartsTag(const FString& L, int32 Indent, const TCHAR* Tag)
	{
		if (IndentOf(L) != Indent) { return false; }
		const FString Open = FString(TEXT("<")) + Tag;
		const FString Rest = L.Mid(Indent);
		if (!Rest.StartsWith(Open, ESearchCase::CaseSensitive)) { return false; }
		if (Rest.Len() == Open.Len()) { return true; }
		const TCHAR After = Rest[Open.Len()];
		return After == TEXT('>') || After == TEXT(' ') || After == TEXT('/') || After == TEXT('	');
	}
	static bool IsExactly(const FString& L, const TCHAR* Text) { return L.Equals(Text, ESearchCase::CaseSensitive); }

	struct FChanRef
	{
		FString Type;
		int32 First = -1, Last = -1;             // the channel's <Item> .. </Item>, inclusive
		int32 QuantumLine = -1, OffsetLine = -1; // -1 when the type carries none
		int32 ValueLine = -1;                    // StaticFloat / StaticVector3 / StaticQuaternion
		int32 ValuesFirst = -1, ValuesLast = -1; // the <Values> block, inclusive
		int32 ValuesIndent = 0;
		int32 Pool = -1;                         // QuantizeFloat only: 4 (94,379) or 6 (37,104)
		bool bPayloadDescriptors = false;        // pool 6 carries RiceSelector + PayloadTail
		bool bRawValues = false;                 // the 11 channels whose quantiser inverse is not exact
		double Quantum = 0.0, Offset = 0.0;
		float StaticValue = 0.f;
		int32 QuatIndex = -1;
		TArray<float> Values;
	};
	struct FItemRef { TArray<FChanRef> Chans; };
	struct FSeqRef { int32 Frames = 0; TArray<FItemRef> Items; };
	struct FBoneRow { int32 Tag = -1; int32 Track = -1; };
	struct FAnimRef
	{
		FString Hash;
		int32 FrameCount = 0, Limit = 0;
		TArray<FBoneRow> Bones;
		TArray<FSeqRef> Seqs;
	};

	static void SplitFloats(const FString& Text, TArray<float>& Out)
	{
		TArray<FString> Parts;
		Text.ParseIntoArrayWS(Parts);
		for (const FString& P : Parts) { Out.Add((float)FCString::Atod(*P)); }
	}

	// Scan the template's <Animations> block. Refuses loudly on any shape it does not recognise -
	// there are no silent skips here, because a skipped channel is a silently dropped edit.
	static bool ScanAnimations(const TArray<FString>& L, TArray<FAnimRef>& Out, FString& Why)
	{
		int32 i = 0;
		while (i < L.Num() && !IsExactly(L[i], TEXT(" <Animations>"))) { ++i; }
		if (i >= L.Num()) { Why = TEXT("the template has no top-level <Animations> block (measured present in 303/303 corpus dictionaries)"); return false; }
		++i;
		while (i < L.Num() && !IsExactly(L[i], TEXT(" </Animations>")))
		{
			if (!IsExactly(L[i], TEXT("  <Item>"))) { ++i; continue; }
			FAnimRef A;
			++i;
			while (i < L.Num() && !IsExactly(L[i], TEXT("  </Item>")))
			{
				const FString& Line = L[i];
				if (StartsTag(Line, 3, TEXT("Hash"))) { A.Hash = InnerOn(Line); ++i; continue; }
				if (StartsTag(Line, 3, TEXT("FrameCount"))) { A.FrameCount = FCString::Atoi(*AttrOn(Line, TEXT("value"))); ++i; continue; }
				if (StartsTag(Line, 3, TEXT("SequenceFrameLimit"))) { A.Limit = FCString::Atoi(*AttrOn(Line, TEXT("value"))); ++i; continue; }
				if (IsExactly(Line, TEXT("   <BoneIds>")))
				{
					++i;
					while (i < L.Num() && !IsExactly(L[i], TEXT("   </BoneIds>")))
					{
						if (IsExactly(L[i], TEXT("    <Item>")))
						{
							FBoneRow B;
							++i;
							while (i < L.Num() && !IsExactly(L[i], TEXT("    </Item>")))
							{
								if (StartsTag(L[i], 5, TEXT("BoneId"))) { B.Tag = FCString::Atoi(*AttrOn(L[i], TEXT("value"))); }
								else if (StartsTag(L[i], 5, TEXT("Track"))) { B.Track = FCString::Atoi(*AttrOn(L[i], TEXT("value"))); }
								++i;
							}
							A.Bones.Add(B);
						}
						++i;
					}
					++i;
					continue;
				}
				if (IsExactly(Line, TEXT("   <Sequences>")))
				{
					++i;
					while (i < L.Num() && !IsExactly(L[i], TEXT("   </Sequences>")))
					{
						if (!IsExactly(L[i], TEXT("    <Item>"))) { ++i; continue; }
						FSeqRef Q;
						++i;
						while (i < L.Num() && !IsExactly(L[i], TEXT("    </Item>")))
						{
							if (StartsTag(L[i], 5, TEXT("FrameCount"))) { Q.Frames = FCString::Atoi(*AttrOn(L[i], TEXT("value"))); ++i; continue; }
							if (IsExactly(L[i], TEXT("     <SequenceData>")))
							{
								++i;
								while (i < L.Num() && !IsExactly(L[i], TEXT("     </SequenceData>")))
								{
									if (!IsExactly(L[i], TEXT("      <Item>"))) { ++i; continue; }
									FItemRef It;
									++i;
									while (i < L.Num() && !IsExactly(L[i], TEXT("      </Item>")))
									{
										if (!IsExactly(L[i], TEXT("       <Channels>"))) { ++i; continue; }
										++i;
										while (i < L.Num() && !IsExactly(L[i], TEXT("       </Channels>")))
										{
											if (!IsExactly(L[i], TEXT("        <Item>"))) { ++i; continue; }
											FChanRef Ch;
											Ch.First = i;
											++i;
											while (i < L.Num() && !IsExactly(L[i], TEXT("        </Item>")))
											{
												const FString& CL = L[i];
												// Longest tag FIRST, as well as the whole-token test in StartsTag: two
												// independent guards over the same collision, because when this one
												// slipped it produced a dictionary with no animation data and no error.
												if (StartsTag(CL, 9, TEXT("Type"))) { Ch.Type = AttrOn(CL, TEXT("value")); }
												else if (StartsTag(CL, 9, TEXT("Quantum"))) { Ch.QuantumLine = i; Ch.Quantum = FCString::Atod(*AttrOn(CL, TEXT("value"))); }
												else if (StartsTag(CL, 9, TEXT("Offset"))) { Ch.OffsetLine = i; Ch.Offset = FCString::Atod(*AttrOn(CL, TEXT("value"))); }
												else if (StartsTag(CL, 9, TEXT("QuatIndex"))) { Ch.QuatIndex = FCString::Atoi(*AttrOn(CL, TEXT("value"))); }
												else if (StartsTag(CL, 9, TEXT("Pool"))) { Ch.Pool = FCString::Atoi(*AttrOn(CL, TEXT("value"))); }
												// pool 6 (37,104 of 131,483 QuantizeFloat channels) carries these two
												// descriptors of the DONOR's packed payload. This writer rewrites decoded
												// numbers, never the payload, so they are carried unchanged and COUNTED -
												// a packer has to re-derive them (LAWS.md D2/H).
												else if (StartsTag(CL, 9, TEXT("RiceSelector")) || StartsTag(CL, 9, TEXT("PayloadTail"))) { Ch.bPayloadDescriptors = true; }
												// 11 of 131,483 QuantizeFloat channels carry <RawValues> because the
												// quantiser inverse is not exact there (LAWS.md D2/E6). Rewriting <Values>
												// under a stale <RawValues> would leave two disagreeing value lists in one
												// channel, so authoring one is REFUSED below, not silently half-written.
												else if (StartsTag(CL, 9, TEXT("RawValues")) || StartsTag(CL, 9, TEXT("RawPalette"))) { Ch.bRawValues = true; }
												else if (StartsTag(CL, 9, TEXT("Values")))
												{
													Ch.ValuesFirst = i;
													Ch.ValuesIndent = 9;
													if (CL.Contains(TEXT("</Values>")))
													{
														SplitFloats(InnerOn(CL), Ch.Values);
														Ch.ValuesLast = i;
													}
													else
													{
														int32 k = i + 1;
														while (k < L.Num() && !IsExactly(L[k], TEXT("         </Values>"))) { SplitFloats(L[k], Ch.Values); ++k; }
														Ch.ValuesLast = k;
														i = k;
													}
												}
												else if (StartsTag(CL, 9, TEXT("Value"))) { Ch.ValueLine = i; Ch.StaticValue = (float)FCString::Atod(*AttrOn(CL, TEXT("value"))); }
												++i;
											}
											Ch.Last = i;
											// A QuantizeFloat channel ALWAYS carries a <Values> block whose length is the
											// sequence's FrameCount (LAWS.md D2/E4, 131,483/131,483). Zero numbers means the
											// SCAN misread the file - the failure that produced a dictionary with no
											// animation data in it - so the template is REFUSED here, loudly, rather than
											// carried as an empty channel that every later count would score as fine.
											if (Ch.Type == TEXT("QuantizeFloat") && (Ch.ValuesFirst < 0 || Ch.Values.Num() == 0))
											{
												Why = FString::Printf(TEXT("line %d: a QuantizeFloat channel holds no <Values> numbers - every one of the 131,483 QuantizeFloat channels measured carries len(Values) == the sequence's FrameCount, so this is a misread file or a misreading scanner, not an empty channel"), Ch.First + 1);
												return false;
											}
											It.Chans.Add(MoveTemp(Ch));
											++i;
										}
										++i;
									}
									Q.Items.Add(MoveTemp(It));
									++i;
								}
								++i;
								continue;
							}
							++i;
						}
						A.Seqs.Add(MoveTemp(Q));
						++i;
					}
					++i;
					continue;
				}
				++i;
			}
			// LAWS.md C5: one SequenceData item per BoneIds row, in order - 2,673/2,673 sequences.
			for (int32 s = 0; s < A.Seqs.Num(); ++s)
			{
				if (A.Seqs[s].Items.Num() != A.Bones.Num())
				{
					Why = FString::Printf(TEXT("template animation '%s' sequence %d has %d items for %d BoneIds rows (the positional tie is 2,673/2,673 in the corpus)"),
						*A.Hash, s, A.Seqs[s].Items.Num(), A.Bones.Num());
					return false;
				}
			}
			Out.Add(MoveTemp(A));
			++i;
		}
		if (Out.Num() == 0) { Why = TEXT("the template's <Animations> block holds no <Item>"); return false; }
		return true;
	}

	// ---- bone tag -> name, off the outfit asset by reflection (the ped lane's own map) --------------
	static bool ReadTagMapOff(UObject* Obj, TMap<int32, FName>& Out, FString& Why)
	{
		FMapProperty* Chosen = nullptr;
		TArray<FString> Candidates;
		for (TFieldIterator<FMapProperty> It(Obj->GetClass()); It; ++It)
		{
			FMapProperty* MP = *It;
			if (!MP->KeyProp || !MP->ValueProp || !MP->KeyProp->IsA<FNameProperty>() || !MP->ValueProp->IsA<FIntProperty>()) { continue; }
			Candidates.Add(MP->GetName());
			if (MP->GetName() == TEXT("BoneTags")) { Chosen = MP; }
		}
		if (!Chosen && Candidates.Num() == 1) { Chosen = FindFProperty<FMapProperty>(Obj->GetClass(), *Candidates[0]); }
		if (!Chosen)
		{
			Why = FString::Printf(TEXT("%s has no TMap<FName,int32> named BoneTags (FName->int32 maps found: %s)"), *Obj->GetPathName(), *FString::Join(Candidates, TEXT(",")));
			return false;
		}
		FScriptMapHelper H(Chosen, Chosen->ContainerPtrToValuePtr<void>(Obj));
		for (int32 i = 0; i < H.GetMaxIndex(); ++i)
		{
			if (!H.IsValidIndex(i)) { continue; }
			const FName K = *reinterpret_cast<const FName*>(H.GetKeyPtr(i));
			const int32 V = *reinterpret_cast<const int32*>(H.GetValuePtr(i));
			Out.Add(V, K);
		}
		return true;
	}
	static FString NormalizeAsset(const FString& In)
	{
		FString P = In.TrimStartAndEnd();
		if (!P.IsEmpty() && !P.Contains(TEXT("."))) { P += TEXT(".") + FPackageName::GetShortName(P); }
		return P;
	}
	static FString AssetNameOfHash(const FString& In)
	{
		FString O;
		for (const TCHAR C : In) { O.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_')); }
		return O.IsEmpty() ? FString(TEXT("_")) : O;
	}

	// ---- options -----------------------------------------------------------------------------------
	struct FOpts
	{
		FString CorpusRoot, Template, OutfitPath;
		bool bAuthorRoot = false;    // LAWS.md G5 / YCD_LAYER_B scar 14
		bool bRescale = true;        // re-derive Quantum/Offset when the authored raws leave the donor's range
		double StaticTol = 1e-5;     // a StaticFloat that moves more than this is a REFUSAL, not a rewrite
	};
	static void ParseOpts(const FString& In, FOpts& O)
	{
		TArray<FString> Parts;
		In.ParseIntoArray(Parts, TEXT(";"), true);
		for (const FString& P : Parts)
		{
			FString K, V;
			if (!P.Split(TEXT("="), &K, &V)) { continue; }
			K = K.TrimStartAndEnd().ToLower(); V = V.TrimStartAndEnd();
			if (K == TEXT("corpus")) { O.CorpusRoot = V; }
			else if (K == TEXT("template")) { O.Template = V; }
			else if (K == TEXT("outfit")) { O.OutfitPath = V; }
			else if (K == TEXT("authorroot")) { O.bAuthorRoot = (V == TEXT("1") || V.ToLower() == TEXT("true")); }
			else if (K == TEXT("rescale")) { O.bRescale = !(V == TEXT("0") || V.ToLower() == TEXT("false")); }
			else if (K == TEXT("statictol")) { O.StaticTol = FCString::Atod(*V); }
		}
	}

	// LAWS.md E5, measured on the raw text (250 files, 64,537 <Values> elements): TEN per line, and
	// the boundary is exact - 2,198 one-line elements carry 2..10 numbers and never more, 62,339 block
	// elements carry 11 or more and hold exactly ceil(n/10) rows of ten with a short last row. 0
	// violations either way. <Values> sits at 9 spaces and its number lines at 10, in 64,537/64,537.
	static void EmitValues(const TArray<float>& V, int32 Indent, TArray<FString>& Out)
	{
		const FString Ind = FString::ChrN(Indent, TEXT(' '));
		if (V.Num() <= 10)
		{
			FString One;
			for (int32 i = 0; i < V.Num(); ++i) { One += (i ? TEXT(" ") : TEXT("")) + YcdNum(V[i]); }
			Out.Add(FString::Printf(TEXT("%s<Values>%s</Values>"), *Ind, *One));
			return;
		}
		Out.Add(Ind + TEXT("<Values>"));
		for (int32 i = 0; i < V.Num(); i += 10)
		{
			FString Row;
			for (int32 k = i; k < FMath::Min(i + 10, V.Num()); ++k) { Row += (k > i ? TEXT(" ") : TEXT("")) + YcdNum(V[k]); }
			Out.Add(Ind + TEXT(" ") + Row);
		}
		Out.Add(Ind + TEXT("</Values>"));
	}

	struct FEdit { int32 From = 0; int32 To = 0; TArray<FString> Lines; };
	struct FTally
	{
		int32 ChannelsWritten = 0, ChannelsCarried = 0, ChannelsRescaled = 0, ChannelsRefused = 0;
		int32 FramesWritten = 0, StaticCarried = 0, StaticRefused = 0;
		int32 RootGated = 0, TracksUnsupported = 0, BonesUnmapped = 0, BonesNoTrack = 0;
		int32 InverseNotExact = 0, Pool6Written = 0, RawValuesRefused = 0;
		FString FirstRefusal;
	};

	// One scalar component into one donor channel. Returns true when the channel is left in a state
	// the caller can account for; the tally carries WHICH state. Never silently reshapes.
	static void AuthorScalarChannel(FChanRef& Ch, const TArray<float>& Vals, const FOpts& Opt,
	                                TArray<FEdit>& Edits, FTally& T)
	{
		if (Ch.Type == TEXT("StaticFloat"))
		{
			// LAWS.md G4: no quantum to round into, so this is CARRIED. 53 static labels moved under the
			// import's normalise; anything larger than StaticTol is a real edit and a real refusal
			// (making a static channel vary is a size change - the maintainer's stage 2c, NOT built).
			double Worst = 0.0;
			for (const float V : Vals) { Worst = FMath::Max(Worst, FMath::Abs((double)V - (double)Ch.StaticValue)); }
			if (Worst > Opt.StaticTol)
			{
				++T.ChannelsRefused; ++T.StaticRefused;
				if (T.FirstRefusal.IsEmpty())
				{
					T.FirstRefusal = FString::Printf(TEXT("a StaticFloat channel would have to vary by %g to carry the authored motion; that is a size change (not built) - the donor value is kept"), Worst);
				}
				return;
			}
			++T.ChannelsCarried; ++T.StaticCarried;
			return;
		}
		if (Ch.Type != TEXT("QuantizeFloat"))
		{
			// IndirectQuantizeFloat (7,736) and RawFloat (317) of 278,075 channels - carried, counted.
			++T.ChannelsCarried; ++T.TracksUnsupported;
			return;
		}
		if (Ch.bRawValues)
		{
			// 11 of 131,483 QuantizeFloat channels carry <RawValues> (LAWS.md D2/E6). Rewriting
			// <Values> while that list stayed as the donor left it would put two disagreeing value
			// lists in one channel, and nothing downstream reads both - so this is a refusal.
			++T.ChannelsRefused; ++T.RawValuesRefused;
			if (T.FirstRefusal.IsEmpty())
			{
				T.FirstRefusal = TEXT("the donor channel carries a <RawValues> list beside its <Values> (11 of 131,483 measured); rewriting one and leaving the other stale is not a write this lane will make");
			}
			return;
		}
		if (Ch.ValuesFirst < 0 || Ch.Values.Num() != Vals.Num())
		{
			++T.ChannelsRefused;
			if (T.FirstRefusal.IsEmpty())
			{
				T.FirstRefusal = FString::Printf(TEXT("donor channel holds %d values, the animation offers %d (LAWS.md E4: len(Values) == sequence FrameCount, 131,483/131,483)"), Ch.Values.Num(), Vals.Num());
			}
			return;
		}

		// LAWS.md G3: preserve the donor's Quantum/Offset and re-derive only the raws - that is what
		// makes the round trip byte-identical (1,382,180/1,382,180 frame values).
		double Quantum = Ch.Quantum;
		double Offset = Ch.Offset;
		// Quantum > 0 holds on 131,483/131,483 corpus channels (LAWS.md E2) - and a hand-edited
		// template is not the corpus. The divisor is tested BEFORE anything divides by it, because an
		// unguarded divide here returns inf/NaN and casting that to int32 is undefined, not a refusal.
		// The test uses the float32 Quantum/Offset the write itself uses, so the fit test and the
		// write cannot disagree about what fits.
		const float QDonor32 = (float)Quantum;
		const float ODonor32 = (float)Offset;
		if (!(QDonor32 > 0.f))
		{
			++T.ChannelsRefused;
			if (T.FirstRefusal.IsEmpty())
			{
				T.FirstRefusal = FString::Printf(TEXT("the donor channel's Quantum is %s; every measured channel has Quantum > 0 (131,483/131,483, LAWS.md E2), and dividing by this one would produce an infinity where a raw index belongs"), *YcdNum(Quantum));
			}
			return;
		}
		int32 DonorMaxRaw = 0;
		for (const float V : Ch.Values) { DonorMaxRaw = FMath::Max(DonorMaxRaw, (int32)FMath::RoundToInt(((double)V - (double)ODonor32) / (double)QDonor32)); }
		bool bFits = true;
		for (const float V : Vals)
		{
			const int32 Raw = (int32)FMath::RoundToInt(((double)V - (double)ODonor32) / (double)QDonor32);
			if (Raw < 0 || Raw > DonorMaxRaw) { bFits = false; break; }
		}
		bool bRescaled = false;
		if (!bFits)
		{
			if (!Opt.bRescale)
			{
				++T.ChannelsRefused;
				if (T.FirstRefusal.IsEmpty()) { T.FirstRefusal = TEXT("authored values leave the donor channel's quantised range and rescale=0 was asked for"); }
				return;
			}
			// The maintainer's own choice, cited not re-derived: offset = min, quantum = span/(2^16-1),
			// a flat channel gets quantum 1. LAWS.md E1/E2 hold on the result by construction.
			float VMin = Vals[0], VMax = Vals[0];
			for (const float V : Vals) { VMin = FMath::Min(VMin, V); VMax = FMath::Max(VMax, V); }
			Offset = (double)VMin;
			const double Span = (double)VMax - (double)VMin;
			Quantum = (Span == 0.0) ? 1.0 : (Span / 65535.0);
			bRescaled = true;
		}

		const float Q32 = (float)Quantum;
		const float O32 = (float)Offset;
		TArray<float> Spelled;
		Spelled.Reserve(Vals.Num());
		bool bInverseExact = true;
		for (const float V : Vals)
		{
			int32 Raw = (int32)FMath::RoundToInt(((double)V - (double)O32) / (double)Q32);
			if (Raw < 0) { Raw = 0; }
			// the game's own decode law (LAWS.md G3): f32(Offset + f32(raw * Quantum))
			const float Back = O32 + (float)((float)Raw * Q32);
			Spelled.Add(Back);
			// LAWS.md E6: verify the inverse per channel, never assume it (131,482/131,483)
			const int32 ReRaw = (int32)FMath::RoundToInt(((double)Back - (double)O32) / (double)Q32);
			if (ReRaw != Raw) { bInverseExact = false; }
		}
		if (!bInverseExact) { ++T.InverseNotExact; }

		if (bRescaled)
		{
			if (Ch.QuantumLine >= 0)
			{
				FEdit E; E.From = Ch.QuantumLine; E.To = Ch.QuantumLine;
				E.Lines.Add(FString::Printf(TEXT("         <Quantum value=\"%s\" />"), *YcdNum(Q32)));
				Edits.Add(MoveTemp(E));
			}
			if (Ch.OffsetLine >= 0)
			{
				FEdit E; E.From = Ch.OffsetLine; E.To = Ch.OffsetLine;
				E.Lines.Add(FString::Printf(TEXT("         <Offset value=\"%s\" />"), *YcdNum(O32)));
				Edits.Add(MoveTemp(E));
			}
			++T.ChannelsRescaled;
		}
		FEdit VE;
		VE.From = Ch.ValuesFirst; VE.To = Ch.ValuesLast;
		EmitValues(Spelled, Ch.ValuesIndent, VE.Lines);
		Edits.Add(MoveTemp(VE));
		++T.ChannelsWritten;
		// The donor's RiceSelector/PayloadTail (pool 6: 37,104 of 131,483) describe the packed payload
		// this writer did not touch. They are carried unchanged and counted here so the number is in
		// the verdict rather than in nobody's head - a packer must re-derive them.
		if (Ch.Pool == 6 || Ch.bPayloadDescriptors) { ++T.Pool6Written; }
		T.FramesWritten += Spelled.Num();
	}
}

// ---- ExportClipDictionary ----------------------------------------------------------------------------
FString URudeToolset::ExportClipDictionary(const FString& AnimSequenceAssetPaths, const FString& ClipNames,
                                           const FString& OutYcdPath, const FString& Options)
{
	using namespace RudeYcdOut;
	FOpts Opt;
	ParseOpts(Options, Opt);
	if (Opt.Template.TrimStartAndEnd().IsEmpty())
	{
		return Fail(TEXT("give Options \"template=<dictionary name or .ycd.xml path>\" - a clip dictionary carries eight per-animation fields, three per-sequence fields and a RecordUnknown00 block that no UAnimSequence models (LAWS.md C1/C4/A4), so this exporter writes over a template and never invents them"));
	}
	if (OutYcdPath.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give OutYcdPath, e.g. C:/out/mydict.ycd.xml")); }

	// resolve the template: a corpus name (ledger type "ycd") or a path on disk
	FString TemplatePath = Opt.Template.TrimStartAndEnd();
	FString TemplateSource = TEXT("path");
	if (!FPaths::FileExists(TemplatePath))
	{
		FString Name = TemplatePath.ToLower();
		Name.RemoveFromEnd(TEXT(".xml")); Name.RemoveFromEnd(TEXT(".ycd"));
		if (FRudeCorpus::LooksLikeCorpus(Opt.CorpusRoot))
		{
			FString CorpusErr;
			TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(Opt.CorpusRoot, CorpusErr);
			if (!Corpus.IsValid()) { return Fail(CorpusErr); }
			const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("ycd"), Name);
			if (!Row) { return Fail(FString::Printf(TEXT("the corpus has no ycd named '%s'"), *Name)); }
			TemplatePath = Corpus->PathOf(*Row);
			TemplateSource = TEXT("corpus");
		}
		else if (!Opt.CorpusRoot.IsEmpty())
		{
			TemplatePath = Opt.CorpusRoot / (Name + TEXT(".ycd.xml"));
			TemplateSource = TEXT("folder");
		}
		if (!FPaths::FileExists(TemplatePath)) { return Fail(FString::Printf(TEXT("no template at %s"), *TemplatePath)); }
	}

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *TemplatePath)) { return Fail(FString::Printf(TEXT("cannot read %s"), *TemplatePath)); }
	// LAWS.md A2: LF, and a trailing newline. Splitting on \n and rejoining with \n preserves both,
	// and any \r that ever appeared would be preserved inside its own line rather than normalised away.
	TArray<FString> Lines;
	Raw.ParseIntoArray(Lines, TEXT("\n"), false);
	const bool bTrailingNewline = Lines.Num() > 0 && Lines.Last().IsEmpty();
	if (bTrailingNewline) { Lines.Pop(); }

	TArray<FAnimRef> Anims;
	FString Why;
	if (!ScanAnimations(Lines, Anims, Why)) { return Fail(Why); }

	// which animations are being authored: an asset whose name is A_<AssetNameOf(hash)> (the import's
	// own naming, so a round trip needs no mapping), or an explicit "hash=/Game/..." pair.
	TMap<FString, FString> HashToAsset;
	TArray<FString> Specs;
	AnimSequenceAssetPaths.ParseIntoArray(Specs, TEXT(";"), true);
	for (const FString& Spec : Specs)
	{
		FString Left, Right;
		if (Spec.Split(TEXT("="), &Left, &Right)) { HashToAsset.Add(Left.TrimStartAndEnd(), NormalizeAsset(Right)); continue; }
		const FString P = NormalizeAsset(Spec.TrimStartAndEnd());
		// "/Game/x/A_hash.A_hash" -> "A_hash" -> "hash": the import's own naming (A_ + AssetNameOf(hash)),
		// so an import/export round trip needs no explicit mapping.
		const FString Short = FPackageName::GetShortName(P);
		FString Stem;
		FString AfterDot;
		if (!Short.Split(TEXT("."), nullptr, &AfterDot)) { Stem = Short; }
		else { Stem = AfterDot.IsEmpty() ? Short : AfterDot; }
		Stem.RemoveFromStart(TEXT("A_"));
		for (const FAnimRef& A : Anims)
		{
			if (AssetNameOfHash(A.Hash) == Stem) { HashToAsset.Add(A.Hash, P); break; }
		}
	}
	TSet<FString> ClipFilter;
	{
		TArray<FString> CN;
		ClipNames.ParseIntoArray(CN, TEXT(";"), true);
		for (const FString& C : CN) { ClipFilter.Add(C.TrimStartAndEnd()); }
	}

	// the bone tag -> name map (the ped lane's outfit asset, read by reflection)
	TMap<int32, FName> TagToName;
	FString MapSource = TEXT("none");
	if (!Opt.OutfitPath.IsEmpty())
	{
		UObject* Outfit = LoadObject<UObject>(nullptr, *NormalizeAsset(Opt.OutfitPath));
		if (!Outfit) { return Fail(FString::Printf(TEXT("no outfit asset at %s"), *Opt.OutfitPath)); }
		if (!ReadTagMapOff(Outfit, TagToName, Why)) { return Fail(Why); }
		MapSource = TEXT("outfit:") + Outfit->GetPathName();
	}

	TArray<FEdit> Edits;
	FTally T;
	FString AnimJson;
	int32 Authored = 0;
	for (FAnimRef& A : Anims)
	{
		if (!ClipFilter.IsEmpty() && !ClipFilter.Contains(A.Hash)) { continue; }
		const FString* AssetPath = HashToAsset.Find(A.Hash);
		if (!AssetPath) { continue; }
		UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, **AssetPath);
		if (!AnimSeq) { return Fail(FString::Printf(TEXT("no UAnimSequence at %s (animation %s)"), **AssetPath, *A.Hash)); }
		const IAnimationDataModel* Model = AnimSeq->GetDataModelInterface().GetInterface();
		if (!Model) { return Fail(FString::Printf(TEXT("%s has no animation data model"), **AssetPath)); }
		USkeleton* Skel = AnimSeq->GetSkeleton();
		if (!Skel) { return Fail(FString::Printf(TEXT("%s has no skeleton"), **AssetPath)); }
		if (TagToName.Num() == 0 && !Opt.OutfitPath.IsEmpty()) { return Fail(TEXT("the outfit asset carried no bone tags")); }

		const int32 AnimWritten0 = T.ChannelsWritten;
		const int32 AnimRefused0 = T.ChannelsRefused;
		for (int32 j = 0; j < A.Bones.Num(); ++j)
		{
			const FBoneRow& B = A.Bones[j];
			if (B.Track != 0 && B.Track != 1)
			{
				// 43,916 of 106,005 BoneIds rows sit on the other 14 tracks (LAWS.md D6) - carried.
				++T.TracksUnsupported;
				continue;
			}
			if (B.Track == 0 && B.Tag == 0 && !Opt.bAuthorRoot)
			{
				// LAWS.md G5 / YCD_LAYER_B scar 14: a +0.5 m root edit crashed the game natively while
				// every structural referee stayed clean. Gated, never authored by accident.
				++T.RootGated;
				continue;
			}
			FName BoneName = NAME_None;
			if (const FName* N = TagToName.Find(B.Tag)) { BoneName = *N; }
			else
			{
				const FName ByTag(*FString::FromInt(B.Tag));
				if (Skel->GetReferenceSkeleton().FindBoneIndex(ByTag) != INDEX_NONE) { BoneName = ByTag; }
			}
			if (BoneName.IsNone()) { ++T.BonesUnmapped; continue; }
			TArray<FTransform> Keys;
			Model->GetBoneTrackTransforms(BoneName, Keys);
			if (Keys.Num() == 0) { ++T.BonesNoTrack; continue; }

			for (int32 s = 0; s < A.Seqs.Num(); ++s)
			{
				FSeqRef& Seq = A.Seqs[s];
				if (!Seq.Items.IsValidIndex(j)) { continue; }
				FItemRef& Item = Seq.Items[j];
				// LAWS.md C2: consecutive sequences share one frame, so sequence s starts at s * Limit.
				const int32 Base = (A.Seqs.Num() > 1) ? s * FMath::Max(1, A.Limit) : 0;
				TArray<float> C0, C1, C2, C3;
				C0.Reserve(Seq.Frames); C1.Reserve(Seq.Frames); C2.Reserve(Seq.Frames); C3.Reserve(Seq.Frames);
				const int32 QuatOmitted = (Item.Chans.Num() >= 4) ? Item.Chans.Last().QuatIndex : -1;
				for (int32 f = 0; f < Seq.Frames; ++f)
				{
					const int32 K = FMath::Clamp(Base + f, 0, Keys.Num() - 1);
					if (B.Track == 0)
					{
						// AGENTS.md section 6.1, inverted: UE cm -> GTA metres with the same Y mirror.
						const FVector P = Keys[K].GetTranslation();
						C0.Add((float)(P.X / 100.0)); C1.Add((float)(-P.Y / 100.0)); C2.Add((float)(P.Z / 100.0));
					}
					else
					{
						// the plain mirror a skeleton bone takes (RudeAnims.cpp), an involution
						const FQuat Q = Keys[K].GetRotation();
						double QC[4] = { Q.X, -Q.Y, Q.Z, Q.W };
						if (QuatOmitted >= 0 && QuatOmitted <= 3)
						{
							// SIGN CANONICALISATION - found by the GATE 2026-09-06, not by any draw. q and -q are the SAME
							// rotation, but the cached form cannot say so: the decoder rebuilds the omitted component as
							// +sqrt(1 - l0^2 - l1^2 - l2^2), NEVER negative (RudeAnims.cpp QuatAt). Unreal's animation data
							// model hands back whichever of the two signs it stored, so on every bone where it kept the
							// opposite one all three labels came out negated: 2,763 of 11,305 frame values differed on the
							// first gate run, magnitudes equal to ~1e-7 with the sign flipped. Pick the sign the decoder can
							// represent - which is the sign the game's own file carries.
							if (QC[QuatOmitted] < 0.0) { QC[0] = -QC[0]; QC[1] = -QC[1]; QC[2] = -QC[2]; QC[3] = -QC[3]; }
							// LAWS.md D5: QuatIndex names the OMITTED component; the three stored labels
							// fill the remaining indices IN ORDER.
							float Lab[3] = { 0.f, 0.f, 0.f };
							int32 Li = 0;
							for (int32 c = 0; c < 4; ++c) { if (c == QuatOmitted) { continue; } Lab[Li++] = (float)QC[c]; }
							C0.Add(Lab[0]); C1.Add(Lab[1]); C2.Add(Lab[2]);
						}
						else
						{
							C0.Add((float)QC[0]); C1.Add((float)QC[1]); C2.Add((float)QC[2]); C3.Add((float)QC[3]);
						}
					}
				}
				const int32 N = Item.Chans.Num();
				if (N == 3 || (N == 4 && QuatOmitted >= 0))
				{
					AuthorScalarChannel(Item.Chans[0], C0, Opt, Edits, T);
					AuthorScalarChannel(Item.Chans[1], C1, Opt, Edits, T);
					AuthorScalarChannel(Item.Chans[2], C2, Opt, Edits, T);
					if (N == 4) { ++T.ChannelsCarried; }   // the CachedQuaternion entry itself never moves
				}
				else if (N == 1 && (Item.Chans[0].Type == TEXT("StaticVector3") || Item.Chans[0].Type == TEXT("StaticQuaternion")))
				{
					// A static item can only be rewritten when the authored motion is genuinely static;
					// otherwise it needs channels the donor does not have (a size change - not built).
					bool bFlat = true;
					for (int32 f = 1; f < C0.Num(); ++f)
					{
						if (FMath::Abs(C0[f] - C0[0]) > Opt.StaticTol || FMath::Abs(C1[f] - C1[0]) > Opt.StaticTol || FMath::Abs(C2[f] - C2[0]) > Opt.StaticTol) { bFlat = false; break; }
					}
					if (!bFlat || Item.Chans[0].ValueLine < 0)
					{
						++T.ChannelsRefused;
						if (T.FirstRefusal.IsEmpty()) { T.FirstRefusal = FString::Printf(TEXT("bone tag %d track %d is a %s in the donor but the authored motion varies - that needs per-frame channels the donor does not carry (a size change, not built)"), B.Tag, B.Track, *Item.Chans[0].Type); }
						continue;
					}
					FEdit E; E.From = Item.Chans[0].ValueLine; E.To = E.From;
					if (Item.Chans[0].Type == TEXT("StaticVector3"))
					{
						E.Lines.Add(FString::Printf(TEXT("         <Value x=\"%s\" y=\"%s\" z=\"%s\" />"), *YcdNum(C0[0]), *YcdNum(C1[0]), *YcdNum(C2[0])));
					}
					else
					{
						const float W = C3.Num() > 0 ? C3[0] : 1.f;
						E.Lines.Add(FString::Printf(TEXT("         <Value x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />"), *YcdNum(C0[0]), *YcdNum(C1[0]), *YcdNum(C2[0]), *YcdNum(W)));
					}
					Edits.Add(MoveTemp(E));
					++T.ChannelsWritten;
				}
				else
				{
					++T.ChannelsRefused;
					if (T.FirstRefusal.IsEmpty()) { T.FirstRefusal = FString::Printf(TEXT("bone tag %d track %d: donor item has %d channels in a shape this writer does not fill (LAWS.md D2 lists the nine measured shapes)"), B.Tag, B.Track, N); }
				}
			}
		}
		++Authored;
		AnimJson += (AnimJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(
			TEXT("{\"hash\":%s,\"asset\":%s,\"frames\":%d,\"sequences\":%d,\"boneRows\":%d,\"channelsWritten\":%d,\"channelsRefused\":%d}"),
			*JStr(A.Hash), *JStr(*AssetPath), A.FrameCount, A.Seqs.Num(), A.Bones.Num(),
			T.ChannelsWritten - AnimWritten0, T.ChannelsRefused - AnimRefused0);
	}

	// apply the edits: every other byte of the template survives untouched
	Edits.Sort([](const FEdit& X, const FEdit& Y) { return X.From < Y.From; });
	FString Out;
	Out.Reserve(Raw.Len() + 4096);
	int32 Cursor = 0, EditIdx = 0;
	while (Cursor < Lines.Num())
	{
		if (EditIdx < Edits.Num() && Edits[EditIdx].From == Cursor)
		{
			for (const FString& EL : Edits[EditIdx].Lines) { Out += EL; Out += TEXT("\n"); }
			Cursor = Edits[EditIdx].To + 1;
			++EditIdx;
			while (EditIdx < Edits.Num() && Edits[EditIdx].From < Cursor) { ++EditIdx; }   // never apply an overlap twice
			continue;
		}
		Out += Lines[Cursor];
		Out += TEXT("\n");
		++Cursor;
	}
	if (!bTrailingNewline && Out.EndsWith(TEXT("\n"))) { Out.LeftChopInline(1); }

	const FString Dest = FPaths::ConvertRelativePathToFull(OutYcdPath.TrimStartAndEnd());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Dest), true);
	// LAWS.md A2: LF, no BOM - the corpus's own encoding, on 303/303 files.
	if (!FFileHelper::SaveStringToFile(Out, *Dest, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(FString::Printf(TEXT("cannot write %s"), *Dest));
	}
	const bool bByteIdentical = Out.Equals(Raw, ESearchCase::CaseSensitive);
	// A run that loaded animations and wrote NO channel is a no-op wearing a successful verdict:
	// with no outfit= the tag map is empty, every bone falls to bonesUnmapped, and the old ok said
	// true with byteIdenticalToTemplate true - indistinguishable from a clean round trip. Both
	// silent-skip counters are inside ok now, and the boolean is in the verdict by name.
	const bool bAuthoredButWroteNothing = (Authored > 0 && T.ChannelsWritten == 0);
	const bool bOk = (T.ChannelsRefused == 0) && (Authored > 0 || HashToAsset.Num() == 0)
		&& !bAuthoredButWroteNothing && (T.BonesUnmapped == 0);

	return FString::Printf(
		TEXT("{\"ok\":%s,\"file\":%s,\"template\":%s,\"templateSource\":%s,\"boneMap\":%s,\"animationsInTemplate\":%d,\"animationsAuthored\":%d,")
		TEXT("\"byteIdenticalToTemplate\":%s,\"channelsWritten\":%d,\"channelsCarried\":%d,\"channelsRescaled\":%d,\"channelsRefused\":%d,")
		TEXT("\"framesWritten\":%d,\"staticCarried\":%d,\"staticRefused\":%d,\"rootChannelsGated\":%d,\"tracksNotAuthored\":%d,")
		TEXT("\"bonesUnmapped\":%d,\"bonesWithoutTrack\":%d,\"inverseNotExact\":%d,\"channelsPool6Written\":%d,\"rawValuesRefused\":%d,")
		TEXT("\"authoredButWroteNothing\":%s,\"firstRefusal\":%s,\"anims\":[%s],")
		TEXT("\"note\":\"a rescaled channel needs a wider binary payload than the donor's; a pool-6 channel's RiceSelector and PayloadTail describe the DONOR's packed payload and are carried unchanged, so a packer must re-derive them for every one of channelsPool6Written (37,104 of 131,483 QuantizeFloat channels are pool 6); a channel carrying <RawValues> is refused rather than half-rewritten (11 of 131,483); this lane emits XML only and does NOT pack it - see LAWS.md H\"}"),
		bOk ? TEXT("true") : TEXT("false"), *JStr(Dest), *JStr(TemplatePath), *JStr(TemplateSource), *JStr(MapSource),
		Anims.Num(), Authored, bByteIdentical ? TEXT("true") : TEXT("false"),
		T.ChannelsWritten, T.ChannelsCarried, T.ChannelsRescaled, T.ChannelsRefused,
		T.FramesWritten, T.StaticCarried, T.StaticRefused, T.RootGated, T.TracksUnsupported,
		T.BonesUnmapped, T.BonesNoTrack, T.InverseNotExact, T.Pool6Written, T.RawValuesRefused,
		bAuthoredButWroteNothing ? TEXT("true") : TEXT("false"), *JStr(T.FirstRefusal), *AnimJson);
}

// ---- ProbeYcdXml -------------------------------------------------------------------------------------
FString URudeToolset::ProbeYcdXml(const FString& XmlPath)
{
	using namespace RudeYcdOut;
	const FString P = XmlPath.TrimStartAndEnd();
	if (P.IsEmpty()) { return Fail(TEXT("give the .ycd.xml path to re-read")); }
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *P)) { return Fail(FString::Printf(TEXT("cannot read %s"), *P)); }
	TArray<FString> Lines;
	Raw.ParseIntoArray(Lines, TEXT("\n"), false);
	const bool bTrailing = Lines.Num() > 0 && Lines.Last().IsEmpty();
	if (bTrailing) { Lines.Pop(); }

	const bool bDecl = Lines.Num() > 0 && Lines[0] == TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
	const bool bRoot = Lines.Num() > 1 && Lines[1] == TEXT("<ClipDictionary>");
	const bool bCrLf = Raw.Contains(TEXT("\r"));

	TArray<FAnimRef> Anims;
	FString Why;
	const bool bScan = ScanAnimations(Lines, Anims, Why);

	int32 Seqs = 0, Chans = 0, Qz = 0, OffMin = 0, QPos = 0, LenOk = 0, TenOk = 0, RawNonNeg = 0, InvOk = 0;
	int32 QzNoValues = 0;
	TMap<FString, int32> ByType;
	if (bScan)
	{
		for (const FAnimRef& A : Anims)
		{
			for (const FSeqRef& Q : A.Seqs)
			{
				++Seqs;
				for (const FItemRef& It : Q.Items)
				{
					for (const FChanRef& Ch : It.Chans)
					{
						++Chans;
						ByType.FindOrAdd(Ch.Type)++;
						if (Ch.Type != TEXT("QuantizeFloat")) { continue; }
						++Qz;
						// A QuantizeFloat channel with no numbers is a file with no animation data in
						// it (or a reader that cannot see it). Counted by name, and it fails ok.
						if (Ch.Values.Num() == 0) { ++QzNoValues; }
						const bool bQPos = Ch.Quantum > 0.0;
						if (bQPos) { ++QPos; }
						if (Ch.Values.Num() == Q.Frames) { ++LenOk; }
						float VMin = Ch.Values.Num() ? Ch.Values[0] : 0.f;
						for (const float V : Ch.Values) { VMin = FMath::Min(VMin, V); }
						if (Ch.Values.Num() && (float)Ch.Offset == VMin) { ++OffMin; }
						// LAWS.md E5, as MEASURED on the raw text rather than through a DOM: at most ten
						// numbers on ONE line, eleven or more as <Values> + ceil(n/10) rows + </Values>.
						// The one-line form is only correct for ten or fewer - the old test accepted any
						// one-line <Values>, which is the same blindness that let a wrong reader pass.
						const int32 Rows = Ch.ValuesLast - Ch.ValuesFirst - 1;
						const bool bOneLine = (Ch.ValuesLast == Ch.ValuesFirst);
						const bool bTen = bOneLine ? (Ch.Values.Num() >= 1 && Ch.Values.Num() <= 10)
						                           : (Ch.Values.Num() >= 11 && Rows == ((Ch.Values.Num() + 9) / 10));
						if (bTen) { ++TenOk; }
						// Quantum is the divisor of both laws below; an unguarded divide by a zero the
						// probe is here to FIND would return inf/NaN and cast it to int32 (undefined),
						// so a non-positive quantum fails those two laws instead of dividing.
						bool bNonNeg = bQPos, bInv = bQPos;
						if (bQPos)
						{
							for (const float V : Ch.Values)
							{
								const int32 R = (int32)FMath::RoundToInt(((double)V - Ch.Offset) / Ch.Quantum);
								if (R < 0) { bNonNeg = false; }
								const float Back = (float)Ch.Offset + (float)((float)R * (float)Ch.Quantum);
								if (Back != V) { bInv = false; }
							}
						}
						if (bNonNeg) { ++RawNonNeg; }
						if (bInv) { ++InvOk; }
					}
				}
			}
		}
	}
	FString TypeJson;
	for (const TPair<FString, int32>& Pr : ByType)
	{
		TypeJson += (TypeJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(TEXT("%s:%d"), *JStr(Pr.Key), Pr.Value);
	}
	const bool bOk = bDecl && bRoot && !bCrLf && bTrailing && bScan && QzNoValues == 0
		&& (Qz == 0 || (OffMin == Qz && QPos == Qz && LenOk == Qz && TenOk == Qz && RawNonNeg == Qz));
	return FString::Printf(
		TEXT("{\"ok\":%s,\"file\":%s,\"declaration\":%s,\"rootIsClipDictionary\":%s,\"lfOnly\":%s,\"trailingNewline\":%s,\"scan\":%s,\"scanError\":%s,")
		TEXT("\"animations\":%d,\"sequences\":%d,\"channels\":%d,\"channelsByType\":{%s},\"quantizeFloat\":%d,")
		TEXT("\"offsetEqualsMin\":%d,\"quantumPositive\":%d,\"valuesEqualFrameCount\":%d,\"tenPerLine\":%d,\"rawsNonNegative\":%d,\"inverseExact\":%d,")
		TEXT("\"quantizeFloatWithoutValues\":%d,")
		TEXT("\"denominator\":\"every law is counted over the %d QuantizeFloat channels in this file; the corpus holds Offset==min, Quantum>0, len(Values)==FrameCount and non-negative raws at 131,483/131,483 each, and the line form at 64,537/64,537 <Values> elements over 250 files (LAWS.md E1-E5)\"}"),
		bOk ? TEXT("true") : TEXT("false"), *JStr(P), bDecl ? TEXT("true") : TEXT("false"), bRoot ? TEXT("true") : TEXT("false"),
		bCrLf ? TEXT("false") : TEXT("true"), bTrailing ? TEXT("true") : TEXT("false"), bScan ? TEXT("true") : TEXT("false"), *JStr(Why),
		Anims.Num(), Seqs, Chans, *TypeJson, Qz, OffMin, QPos, LenOk, TenOk, RawNonNeg, InvOk, QzNoValues, Qz);
}
