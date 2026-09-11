// RUDE - RAGE <-> Unreal Development Environment
//
// THE FRAGMENT CLONE LANE. A `.yft` is the block on authoring a new vehicle, weapon or breakable,
// and it is a block for a reason worth stating in full: nobody has a fragment WRITER. The writers
// that DO reproduce a fragment byte-exact all do it by taking the ORIGINAL BINARY and carrying most
// of its image forward unchanged. A writer that photocopies most of a file cannot construct one
// that has no original. (The survey behind that sentence, with its counts and its file:line
// citations, is in maintainer lane `yft_clone`, `LAWS.md` L22.)
//
// So this lane does the reachable thing instead: CLONE AND EDIT. Take a fragment the game already
// ships, put your own geometry in one of its slots, and carry every other byte forward untouched.
//
// MEASURED FACTS THIS LANE RESTS ON (maintainer lane `yft_clone`, `LAWS.md` + `measure_*.json`;
// 245 fragments drawn from the 61,413 in the filebase, seed 13):
//   * The corpus holds fragments as XML: 61,413 `.yft.xml` and 17 `.yft` (61,413 + 17 = the 61,430
//     the round-trip writer counts). For 61,413 of 61,430 there is NO binary template to splice.
//   * A fragment re-emitted as `prolog + each top-level child's ORIGINAL BYTES + tail` is byte
//     identical to the source: 245 of 245. That is why a clone with nothing replaced is byte
//     identical BY CONSTRUCTION, and why this tool COMPUTES that rather than claiming it.
//   * One `<Geometries><Item>` span is a contiguous run nothing else overlaps: replacing it with
//     its own bytes reproduces the file (237/237), replacing it with other bytes changes only that
//     span (237/237) and moves the length by exactly (new - old) (237/237).
//   * The drawable owns mean 90.46% of the file (min 38.89%, max 99.91%, n=237); the physics
//     hierarchy owns mean 7.67% (max 52.80%, n=241). The 10% nobody has modelled is exactly the
//     part this lane refuses to rebuild - and CARRY is the literal word, because that block is not
//     only collision. `Physics/LOD*/Children/*/Drawable` is a REAL drawable: 241 of the 245 files
//     carry at least one and there are 677 in all, and on 12 of the 245 those nested drawables own
//     GEOMETRY - 40 geometries, at most 6 in any one file (`measure_yft2.json`). Most are empty
//     shells (adder's 18 nested drawables hold 3 geometries between them), but a replacement never
//     touches any of them, so the donor's breakable-piece meshes stay in the output. Both counts
//     are in every verdict, and a replacement over a source with nested geometry WARNS.
//   * `Drawable/Skeleton/Bones` count == top-level `BoneTransforms` count on 237 of 237. Bones have
//     TWO owners, so this lane never touches a bone.
//   * `<Drawable>` is not universal (237/245 - the 8 without are cloth and flag fragments) and
//     `<Physics>` is not universal (241/245 - the 4 without are peds). Both are refusals, not
//     crashes.
//   * `GTAV1` is a NAME, not a layout: 1,736 of 1,736 geometries call their layout GTAV1 and that
//     name covers 11 different semantic lists across the draw. The semantic LIST is the contract.
//   * 1,736 of 1,736 `<ShaderIndex>` values sit inside [0, shaderCount). A replacement mesh must
//     present exactly as many material slots as the template model has geometries.
//   * The `_hi` twin is a SEPARATE fragment: 936 of 936 `_hi` stems have a base twin, the `_hi`
//     carries more geometries than its base on 8 of 8 sampled pairs, and on 7 of 8 the base carries
//     a `<VehicleGlassWindows>` block the `_hi` does not. Cloning the base and staying quiet about
//     the twin ships half a vehicle, so the verdict says so out loud.
//
// ⛔ WHAT THIS DOES NOT DO, MEASURED RATHER THAN ASSUMED. The question "should RUDE emit XML for the
// packer, or write binary itself" has an answer and it is not a preference: THERE IS NO PACKER.
// Nothing on this machine turns a fragment XML back into a `.yft`: the fragment code that exists
// DECODES, and the writers that do reproduce a fragment are keyed to its original binary rather
// than to text. (Counts and citations: maintainer lane `yft_clone`, `LAWS.md` L22.) And AGENTS.md
// section 6.7 is explicit that drawables need BINARY on both FiveM Legacy and Enhanced. So what
// this lane writes is an INTERCHANGE artifact that is diff-clean against its source everywhere the
// author did not author - not a file the game will load. The binary fragment writer is a separate,
// UNBUILT lane, and this file does not pretend to start it.

#include "RudeToolset.h"
#include "RudeCorpus.h"
#include "RudeToolsetInternal.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

namespace RudeYftCloneLane
{
	// ---------------------------------------------------------------------------------------------
	// 1. THE BYTE-LEVEL SPAN WALKER
	//
	// Deliberately NOT FXmlFile. The whole point of this lane is to hand bytes back unchanged, and a
	// DOM round trip re-spells everything it touches (AGENTS.md section 6.5 already warns that UE's
	// FXmlFile does not preserve line structure inside text content). This walker only ever returns
	// OFFSETS into the original buffer; the bytes themselves are never decoded, re-encoded, or
	// re-spelled unless this lane is deliberately authoring them.
	//
	// It mirrors, function for function, the Python instrument that measured the laws above, so the
	// spans the tool splices are the spans the measurement proved were spliceable.
	// ---------------------------------------------------------------------------------------------
	struct FYftSpan
	{
		FString Tag;
		int32 FirstLine = 0;
		int32 LastLine = 0;
		int32 ByteStart = 0;
		int32 ByteEnd = 0;
		bool bSelfClosing = false;
	};

	static void BuildLineIndex(const TArray<uint8>& Blob, TArray<FIntPoint>& OutLines)
	{
		OutLines.Reset();
		const int32 N = Blob.Num();
		int32 i = 0;
		while (i < N)
		{
			int32 j = i;
			while (j < N && Blob[j] != '\n') { ++j; }
			OutLines.Add(FIntPoint(i, (j < N) ? (j + 1) : N));
			i = (j < N) ? (j + 1) : N;
		}
	}

	static int32 IndentOf(const TArray<uint8>& Blob, const FIntPoint& Ln)
	{
		int32 k = Ln.X;
		while (k < Ln.Y && Blob[k] == ' ') { ++k; }
		return k - Ln.X;
	}

	static bool IsNameByte(uint8 C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9')
			|| C == '_' || C == '.';
	}

	// Tag name of an element line, or empty. Sets bOutClosing for `</Tag>`.
	static FString TagOfLine(const TArray<uint8>& Blob, const FIntPoint& Ln, bool& bOutClosing)
	{
		bOutClosing = false;
		int32 k = Ln.X;
		while (k < Ln.Y && (Blob[k] == ' ' || Blob[k] == '\t')) { ++k; }
		if (k >= Ln.Y || Blob[k] != '<') { return FString(); }
		++k;
		if (k < Ln.Y && (Blob[k] == '?' || Blob[k] == '!')) { return FString(); }
		if (k < Ln.Y && Blob[k] == '/') { bOutClosing = true; ++k; }
		const int32 Start = k;
		while (k < Ln.Y && IsNameByte(Blob[k])) { ++k; }
		if (k == Start) { return FString(); }
		FString Out;
		Out.Reserve(k - Start);
		for (int32 p = Start; p < k; ++p) { Out.AppendChar((TCHAR)Blob[p]); }
		return Out;
	}

	// The line's bytes with leading/trailing whitespace dropped, as an FString. ASCII only - the
	// corpus's own XML is ASCII inside element markup, and this is used for markup, never payload.
	static FString TrimmedLine(const TArray<uint8>& Blob, const FIntPoint& Ln)
	{
		int32 A = Ln.X;
		int32 B = Ln.Y;
		while (A < B && (Blob[A] == ' ' || Blob[A] == '\t')) { ++A; }
		while (B > A && (Blob[B - 1] == '\n' || Blob[B - 1] == '\r' || Blob[B - 1] == ' ' || Blob[B - 1] == '\t')) { --B; }
		FString Out;
		Out.Reserve(B - A);
		for (int32 p = A; p < B; ++p) { Out.AppendChar((TCHAR)Blob[p]); }
		return Out;
	}

	// Every element beginning at exactly `Depth` spaces of indent, inside lines [Lo, Hi).
	static TArray<FYftSpan> ChildrenAt(const TArray<uint8>& Blob, const TArray<FIntPoint>& Lines,
	                                   int32 Lo, int32 Hi, int32 Depth)
	{
		TArray<FYftSpan> Out;
		int32 i = Lo;
		while (i < Hi)
		{
			bool bClosing = false;
			const FString Tag = TagOfLine(Blob, Lines[i], bClosing);
			if (Tag.IsEmpty() || bClosing || IndentOf(Blob, Lines[i]) != Depth) { ++i; continue; }

			const FString Stripped = TrimmedLine(Blob, Lines[i]);
			FYftSpan S;
			S.Tag = Tag;
			S.FirstLine = i;
			S.LastLine = i;
			S.ByteStart = Lines[i].X;
			S.ByteEnd = Lines[i].Y;

			if (Stripped.EndsWith(TEXT("/>")))
			{
				S.bSelfClosing = true;
				Out.Add(S);
				++i;
				continue;
			}
			const FString CloseTag = FString::Printf(TEXT("</%s>"), *Tag);
			if (Stripped.EndsWith(CloseTag))
			{
				// opens and closes on one line, e.g. <Name>pack:/adder</Name>
				Out.Add(S);
				++i;
				continue;
			}
			int32 j = i + 1;
			int32 EndLine = INDEX_NONE;
			while (j < Hi)
			{
				if (IndentOf(Blob, Lines[j]) == Depth && TrimmedLine(Blob, Lines[j]) == CloseTag)
				{
					EndLine = j;
					break;
				}
				++j;
			}
			if (EndLine == INDEX_NONE)
			{
				Out.Add(S);
				++i;
				continue;
			}
			S.LastLine = EndLine;
			S.ByteEnd = Lines[EndLine].Y;
			Out.Add(S);
			i = EndLine + 1;
		}
		return Out;
	}

	static const FYftSpan* FindTag(const TArray<FYftSpan>& In, const TCHAR* Tag)
	{
		for (const FYftSpan& S : In)
		{
			if (S.Tag == Tag) { return &S; }
		}
		return nullptr;
	}

	static FString AttrOfLine(const TArray<uint8>& Blob, const FIntPoint& Ln, const TCHAR* Key)
	{
		const FString Line = TrimmedLine(Blob, Ln);
		const FString Needle = FString::Printf(TEXT("%s=\""), Key);
		const int32 K = Line.Find(Needle, ESearchCase::CaseSensitive);
		if (K == INDEX_NONE) { return FString(); }
		const int32 ValStart = K + Needle.Len();
		const int32 ValEnd = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValStart);
		if (ValEnd == INDEX_NONE) { return FString(); }
		return Line.Mid(ValStart, ValEnd - ValStart);
	}

	static void AppendSlice(TArray<uint8>& Out, const TArray<uint8>& Blob, int32 A, int32 B)
	{
		if (B > A && A >= 0 && B <= Blob.Num())
		{
			Out.Append(Blob.GetData() + A, B - A);
		}
	}

	// ⛔ This MASKS to 7 bits, so a non-ASCII character would land as a DIFFERENT ASCII byte rather
	// than as an error. That is safe only because every string that reaches it is either generated
	// by this file (digits, spaces, the float speller's own alphabet) or has already passed
	// `NameIsWritable`. Do not hand it an author's raw text.
	static void AppendAscii(TArray<uint8>& Out, const FString& S)
	{
		for (int32 i = 0; i < S.Len(); ++i)
		{
			Out.Add((uint8)(S[i] & 0x7F));
		}
	}

	// The alphabet a fragment name may use. Anything outside it is a REFUSAL, never a mask: a name
	// whose 'e' carries an acute accent masks to 7 bits as a DIFFERENT ASCII letter, and the run
	// would report `renamed:true` over a name nobody asked for.
	static bool NameIsWritable(const FString& S)
	{
		if (S.IsEmpty()) { return false; }
		for (int32 i = 0; i < S.Len(); ++i)
		{
			const TCHAR C = S[i];
			const bool bAllowed = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z')
				|| (C >= '0' && C <= '9') || C == '_' || C == '-';
			if (!bAllowed) { return false; }
		}
		return true;
	}

	// `<Name>default</Name>` -> `default`, for an element that opens and closes on one line.
	static FString InnerTextOfLine(const TArray<uint8>& Blob, const FIntPoint& Ln, const TCHAR* Tag)
	{
		const FString Line = TrimmedLine(Blob, Ln);
		const FString Open = FString::Printf(TEXT("<%s>"), Tag);
		const FString Close = FString::Printf(TEXT("</%s>"), Tag);
		if (!Line.StartsWith(Open, ESearchCase::CaseSensitive)
			|| !Line.EndsWith(Close, ESearchCase::CaseSensitive)
			|| Line.Len() < Open.Len() + Close.Len())
		{
			return FString();
		}
		return Line.Mid(Open.Len(), Line.Len() - Open.Len() - Close.Len());
	}

	// ---------------------------------------------------------------------------------------------
	// 2. THE FLOAT SPELLER
	//
	// Read off the exporter that wrote every `.yft.xml` in this corpus, so this is the spelling the
	// corpus is already in rather than a guess at it (the source and its file:line are in maintainer
	// lane `yft_clone`, `LAWS.md` L20). Rule: render the EXACT decimal expansion to 7 significant
	// figures with ties
	// AWAY FROM ZERO; widen to 9 iff 7 does not round-trip back to the identical float32.
	//
	// Verified 380,072 of 380,072 float tokens across the 245-file draw (maintainer lane
	// `yft_clone`, `measure_floatspell.json`). ⛔ The tie rule is not decoration: 37,707 of those
	// 380,072 tokens land exactly on a tie, so a speller that used the C library's round-half-EVEN
	// would be wrong on one token in ten (`measure_rounding.json`). That is why this computes the
	// exact expansion itself instead of calling printf.
	// ---------------------------------------------------------------------------------------------

	// Exact decimal digits of |V| as `Digits` (no leading zero) with `OutDecExp` such that the value
	// is 0.<Digits> * 10^(OutDecExp+1)... expressed here as: value = Digits * 10^OutDecExp.
	static void ExactDigitsOfFloat(float V, TArray<uint8>& OutDigits, int32& OutDecExp)
	{
		OutDigits.Reset();
		OutDecExp = 0;
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &V, sizeof(uint32));
		uint32 Mantissa = Bits & 0x007FFFFFu;
		const int32 RawExp = (int32)((Bits >> 23) & 0xFFu);
		int32 Exp2 = 0;
		if (RawExp == 0)
		{
			Exp2 = -149;                       // subnormal
		}
		else
		{
			Mantissa |= 0x00800000u;
			Exp2 = RawExp - 127 - 23;
		}
		if (Mantissa == 0)
		{
			OutDigits.Add(0);
			return;
		}
		// reduce to lowest terms so the coefficient is trailing-zero free the way the exporter's is
		while (Exp2 < 0 && (Mantissa & 1u) == 0u)
		{
			Mantissa >>= 1;
			++Exp2;
		}
		// decimal big number, most significant digit first
		TArray<uint8> Coeff;
		{
			uint32 M = Mantissa;
			TArray<uint8> Rev;
			while (M > 0) { Rev.Add((uint8)(M % 10u)); M /= 10u; }
			for (int32 i = Rev.Num() - 1; i >= 0; --i) { Coeff.Add(Rev[i]); }
		}
		auto MulSmall = [&Coeff](uint32 K)
		{
			uint32 Carry = 0;
			for (int32 i = Coeff.Num() - 1; i >= 0; --i)
			{
				const uint32 Prod = (uint32)Coeff[i] * K + Carry;
				Coeff[i] = (uint8)(Prod % 10u);
				Carry = Prod / 10u;
			}
			while (Carry > 0)
			{
				Coeff.Insert((uint8)(Carry % 10u), 0);
				Carry /= 10u;
			}
		};
		if (Exp2 >= 0)
		{
			for (int32 i = 0; i < Exp2; ++i) { MulSmall(2); }
			OutDecExp = 0;
		}
		else
		{
			const int32 K = -Exp2;
			for (int32 i = 0; i < K; ++i) { MulSmall(5); }
			OutDecExp = -K;
		}
		OutDigits = MoveTemp(Coeff);
	}

	// `Digits` significant figures, ties away from zero, spelled the way the exporter spells it.
	static FString SigSpell(float V, int32 Digits)
	{
		TArray<uint8> D;
		int32 DecExp = 0;
		ExactDigitsOfFloat(V, D, DecExp);
		const bool bNeg = (V < 0.0f);
		const int32 Nd = D.Num();
		const int32 OrigExp = DecExp + Nd - 1;             // decimal exponent of the leading digit
		int32 TargetExp = OrigExp - Digits + 1;

		TArray<uint8> Rounded;
		if (Nd <= Digits)
		{
			Rounded = D;
			for (int32 i = Nd; i < Digits; ++i) { Rounded.Add(0); }
		}
		else
		{
			for (int32 i = 0; i < Digits; ++i) { Rounded.Add(D[i]); }
			if (D[Digits] >= 5)
			{
				int32 i = Rounded.Num() - 1;
				while (i >= 0)
				{
					if (Rounded[i] < 9) { Rounded[i] += 1; break; }
					Rounded[i] = 0;
					--i;
				}
				if (i < 0) { Rounded.Insert(1, 0); }
			}
		}
		const int32 Adj = TargetExp + Rounded.Num() - 1;   // exponent of the leading digit after rounding

		// trailing-zero-free digit string
		int32 Last = Rounded.Num() - 1;
		while (Last > 0 && Rounded[Last] == 0) { --Last; }
		FString DigitStr;
		for (int32 i = 0; i <= Last; ++i) { DigitStr.AppendChar((TCHAR)('0' + Rounded[i])); }

		FString Out;
		if (Adj >= -4 && Adj < Digits)
		{
			// fixed notation
			if (Adj >= 0)
			{
				const int32 IntLen = Adj + 1;
				if (DigitStr.Len() <= IntLen)
				{
					Out = DigitStr;
					for (int32 i = DigitStr.Len(); i < IntLen; ++i) { Out.AppendChar(TEXT('0')); }
				}
				else
				{
					Out = DigitStr.Left(IntLen) + TEXT(".") + DigitStr.Mid(IntLen);
				}
			}
			else
			{
				Out = TEXT("0.");
				for (int32 i = 0; i < (-Adj - 1); ++i) { Out.AppendChar(TEXT('0')); }
				Out += DigitStr;
			}
		}
		else
		{
			Out = DigitStr.Left(1);
			if (DigitStr.Len() > 1) { Out += TEXT(".") + DigitStr.Mid(1); }
			Out += FString::Printf(TEXT("E%s%02d"), (Adj >= 0) ? TEXT("+") : TEXT("-"), FMath::Abs(Adj));
		}
		return bNeg ? (TEXT("-") + Out) : Out;
	}

	static FString FmtF32(float V)
	{
		if (FMath::IsNaN(V)) { return TEXT("NaN"); }
		if (!FMath::IsFinite(V)) { return (V > 0.0f) ? TEXT("Infinity") : TEXT("-Infinity"); }
		if (V == 0.0f)
		{
			uint32 Bits = 0;
			FMemory::Memcpy(&Bits, &V, sizeof(uint32));
			return (Bits & 0x80000000u) ? TEXT("-0") : TEXT("0");
		}
		FString S = SigSpell(V, 7);
		const float Back = (float)FCString::Atod(*S);
		if (Back != V) { S = SigSpell(V, 9); }
		return S;
	}

	// ---------------------------------------------------------------------------------------------
	// 3. THE TEMPLATE VIEW
	// ---------------------------------------------------------------------------------------------
	struct FYftGeomView
	{
		FYftSpan Span;                 // the whole <Item> under <Geometries>
		int32 ShaderIndex = INDEX_NONE;
		TArray<FString> Semantics;     // the <Layout>'s children IN ORDER - the column contract
		FString LayoutName;
		bool bHasBoneIds = false;
		FYftSpan VertexDataSpan;       // the <Data> element inside <VertexBuffer>
		FYftSpan IndexDataSpan;        // the <Data> element inside <IndexBuffer>
		bool bHasVertexData = false;
		bool bHasIndexData = false;
		FYftSpan BBoxMin;
		FYftSpan BBoxMax;
		bool bHasBBoxMin = false;
		bool bHasBBoxMax = false;
	};

	struct FYftModelView
	{
		FYftSpan Span;
		FString HasSkin;
		FString BoneIndex;
		TArray<FYftGeomView> Geometries;
	};

	// ⛔ Returns nullptr on anything it does not recognise, and the caller REFUSES. It used to fall
	// back to High, which meant `lod=Med`, `lod=Hgh` or a trailing space wrote the author's meshes
	// into a LOD block they never named, with ok:true and no counter that could show it. A pure
	// clone is byte-identical for every lod value, so no comparison could have caught it either.
	static const TCHAR* LodTagFor(const FString& Which)
	{
		const FString U = Which.ToUpper();
		if (U == TEXT("HIGH")) { return TEXT("DrawableModelsHigh"); }
		if (U == TEXT("MEDIUM")) { return TEXT("DrawableModelsMedium"); }
		if (U == TEXT("LOW")) { return TEXT("DrawableModelsLow"); }
		if (U == TEXT("VERYLOW")) { return TEXT("DrawableModelsVeryLow"); }
		return nullptr;
	}

	// The shader-group preset names in `<ShaderIndex>` order. `ImportYdr` names every material slot
	// it creates `<preset>__<geoIndex>` (RudeEditor's shared drawable importer), so this list is
	// exactly what a RUDE-imported mesh's slot names can be joined against - which is how this lane
	// pairs a mesh to a template geometry without assuming the two orders agree.
	static void ReadShaderPresets(const TArray<uint8>& Blob, const TArray<FIntPoint>& Lines,
	                              const TArray<FYftSpan>& DrawableChildren, TArray<FString>& OutPresets)
	{
		OutPresets.Reset();
		const FYftSpan* SG = FindTag(DrawableChildren, TEXT("ShaderGroup"));
		if (!SG) { return; }
		for (const FYftSpan& C : ChildrenAt(Blob, Lines, SG->FirstLine + 1, SG->LastLine, 3))
		{
			if (C.Tag != TEXT("Shaders")) { continue; }
			for (const FYftSpan& It : ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4))
			{
				const TArray<FYftSpan> IC = ChildrenAt(Blob, Lines, It.FirstLine + 1, It.LastLine, 5);
				const FYftSpan* Nm = FindTag(IC, TEXT("Name"));
				OutPresets.Add(Nm ? InnerTextOfLine(Blob, Lines[Nm->FirstLine], TEXT("Name")) : FString());
			}
		}
	}

	// `Physics/LOD*/Children/*/Drawable` - the breakable pieces. Measured over the 245-file draw
	// (`measure_yft2.json`): 241 files carry at least one nested drawable, 677 in all; 12 files carry
	// nested GEOMETRY, 40 in all, at most 6 in one file. This lane never edits one, so it counts them
	// and says so instead of leaving the author to assume there are none.
	static void CountPhysicsChildDrawables(const TArray<uint8>& Blob, const TArray<FIntPoint>& Lines,
	                                       const FYftSpan& Phys, int32& OutDrawables, int32& OutGeometries)
	{
		OutDrawables = 0;
		OutGeometries = 0;
		static const TCHAR* NestedLods[] = { TEXT("DrawableModelsHigh"), TEXT("DrawableModelsMedium"),
		                                     TEXT("DrawableModelsLow"), TEXT("DrawableModelsVeryLow") };
		for (const FYftSpan& PLod : ChildrenAt(Blob, Lines, Phys.FirstLine + 1, Phys.LastLine, 2))
		{
			if (!PLod.Tag.StartsWith(TEXT("LOD"), ESearchCase::CaseSensitive)) { continue; }
			for (const FYftSpan& PC : ChildrenAt(Blob, Lines, PLod.FirstLine + 1, PLod.LastLine, 3))
			{
				if (PC.Tag != TEXT("Children")) { continue; }
				for (const FYftSpan& Item : ChildrenAt(Blob, Lines, PC.FirstLine + 1, PC.LastLine, 4))
				{
					const TArray<FYftSpan> IC = ChildrenAt(Blob, Lines, Item.FirstLine + 1, Item.LastLine, 5);
					const FYftSpan* NestedDraw = FindTag(IC, TEXT("Drawable"));
					if (!NestedDraw) { continue; }
					++OutDrawables;
					const TArray<FYftSpan> NDC = ChildrenAt(Blob, Lines, NestedDraw->FirstLine + 1, NestedDraw->LastLine, 6);
					for (const TCHAR* NestedLod : NestedLods)
					{
						const FYftSpan* NL = FindTag(NDC, NestedLod);
						if (!NL) { continue; }
						for (const FYftSpan& NM : ChildrenAt(Blob, Lines, NL->FirstLine + 1, NL->LastLine, 7))
						{
							const TArray<FYftSpan> NMC = ChildrenAt(Blob, Lines, NM.FirstLine + 1, NM.LastLine, 8);
							if (const FYftSpan* NG = FindTag(NMC, TEXT("Geometries")))
							{
								OutGeometries += ChildrenAt(Blob, Lines, NG->FirstLine + 1, NG->LastLine, 9).Num();
							}
						}
					}
				}
			}
		}
	}

	static void ReadModels(const TArray<uint8>& Blob, const TArray<FIntPoint>& Lines,
	                       const FYftSpan& LodSpan, TArray<FYftModelView>& OutModels)
	{
		OutModels.Reset();
		for (const FYftSpan& M : ChildrenAt(Blob, Lines, LodSpan.FirstLine + 1, LodSpan.LastLine, 3))
		{
			FYftModelView MV;
			MV.Span = M;
			const TArray<FYftSpan> MC = ChildrenAt(Blob, Lines, M.FirstLine + 1, M.LastLine, 4);
			if (const FYftSpan* HS = FindTag(MC, TEXT("HasSkin"))) { MV.HasSkin = AttrOfLine(Blob, Lines[HS->FirstLine], TEXT("value")); }
			if (const FYftSpan* BI = FindTag(MC, TEXT("BoneIndex"))) { MV.BoneIndex = AttrOfLine(Blob, Lines[BI->FirstLine], TEXT("value")); }
			const FYftSpan* Geos = FindTag(MC, TEXT("Geometries"));
			if (Geos)
			{
				for (const FYftSpan& G : ChildrenAt(Blob, Lines, Geos->FirstLine + 1, Geos->LastLine, 5))
				{
					FYftGeomView GV;
					GV.Span = G;
					const TArray<FYftSpan> GC = ChildrenAt(Blob, Lines, G.FirstLine + 1, G.LastLine, 6);
					if (const FYftSpan* SI = FindTag(GC, TEXT("ShaderIndex")))
					{
						GV.ShaderIndex = FCString::Atoi(*AttrOfLine(Blob, Lines[SI->FirstLine], TEXT("value")));
					}
					if (const FYftSpan* BMin = FindTag(GC, TEXT("BoundingBoxMin"))) { GV.BBoxMin = *BMin; GV.bHasBBoxMin = true; }
					if (const FYftSpan* BMax = FindTag(GC, TEXT("BoundingBoxMax"))) { GV.BBoxMax = *BMax; GV.bHasBBoxMax = true; }
					GV.bHasBoneIds = (FindTag(GC, TEXT("BoneIDs")) != nullptr);
					if (const FYftSpan* VB = FindTag(GC, TEXT("VertexBuffer")))
					{
						for (const FYftSpan& VC : ChildrenAt(Blob, Lines, VB->FirstLine + 1, VB->LastLine, 7))
						{
							if (VC.Tag == TEXT("Layout"))
							{
								GV.LayoutName = AttrOfLine(Blob, Lines[VC.FirstLine], TEXT("type"));
								for (const FYftSpan& Sem : ChildrenAt(Blob, Lines, VC.FirstLine + 1, VC.LastLine, 8))
								{
									GV.Semantics.Add(Sem.Tag);
								}
							}
							else if (VC.Tag == TEXT("Data"))
							{
								GV.VertexDataSpan = VC;
								GV.bHasVertexData = true;
							}
						}
					}
					if (const FYftSpan* IB = FindTag(GC, TEXT("IndexBuffer")))
					{
						for (const FYftSpan& IC : ChildrenAt(Blob, Lines, IB->FirstLine + 1, IB->LastLine, 7))
						{
							if (IC.Tag == TEXT("Data"))
							{
								GV.IndexDataSpan = IC;
								GV.bHasIndexData = true;
							}
						}
					}
					MV.Geometries.Add(GV);
				}
			}
			OutModels.Add(MV);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// 4. THE AUTHORED GEOMETRY
	// ---------------------------------------------------------------------------------------------
	struct FYftAuthoredGeo
	{
		TArray<FVector3f> Pos;         // already in GTA metres, Y mirrored (AGENTS.md section 6.1)
		TArray<FVector3f> Nrm;
		TArray<TArray<FVector2f>> UVs; // per welded vertex, per UV channel
		TArray<int32> Indices;
		FVector3f BBoxMin = FVector3f::ZeroVector;
		FVector3f BBoxMax = FVector3f::ZeroVector;
	};

	static FString JoinStrings(const TArray<FString>& In, const TCHAR* Sep)
	{
		FString Out;
		for (int32 i = 0; i < In.Num(); ++i)
		{
			if (i) { Out += Sep; }
			Out += In[i];
		}
		return Out;
	}

	static FString JsonStringArray(const TArray<FString>& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Num(); ++i)
		{
			Out += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *RudeJsonEscape(In[i]));
		}
		return Out;
	}

	// The semantics this lane can author from a UStaticMesh. Anything outside it is a REFUSAL, never
	// a silent zero - a column filled with the wrong thing is invisible to every counter.
	static bool SemanticIsWritable(const FString& S)
	{
		return S == TEXT("Position") || S == TEXT("Normal") || S == TEXT("Colour0")
			|| S == TEXT("Colour1") || S == TEXT("TexCoord0") || S == TEXT("TexCoord1")
			|| S == TEXT("TexCoord2") || S == TEXT("TexCoord3") || S == TEXT("Tangent");
	}
}

// =================================================================================================
// ProbeYftFragment - what does this fragment actually own?
// =================================================================================================
FString URudeToolset::ProbeYftFragment(const FString& SourceName, const FString& CorpusRoot,
                                       const FString& Options)
{
	using namespace RudeYftCloneLane;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};

	FString Name = SourceName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml"));
	Name.RemoveFromEnd(TEXT(".yft"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a fragment name, e.g. adder")); }

	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::LooksLikeCorpus(CorpusRoot)
		? FRudeCorpus::Open(CorpusRoot, CorpusErr) : nullptr;
	FString XmlPath;
	FString Slot;
	bool bHiTwin = false;
	if (Corpus.IsValid())
	{
		if (const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("yft"), Name))
		{
			XmlPath = Corpus->PathOf(*Row);
			Slot = Row->Slot;
		}
		bHiTwin = (Corpus->Effective(TEXT("yft"), Name + TEXT("_hi")) != nullptr);
	}
	if (XmlPath.IsEmpty())
	{
		// the ad-hoc drop, so the tool stays drivable on a single exported file
		XmlPath = CorpusRoot / (Name + TEXT(".yft.xml"));
	}
	if (!FPaths::FileExists(XmlPath))
	{
		return Fail(FString::Printf(TEXT("the corpus has no yft named '%s' (%s)"), *Name,
			Corpus.IsValid() ? TEXT("looked through the ledger") : *CorpusErr));
	}

	TArray<uint8> Blob;
	if (!FFileHelper::LoadFileToArray(Blob, *XmlPath)) { return Fail(FString::Printf(TEXT("could not read %s"), *XmlPath)); }
	TArray<FIntPoint> Lines;
	BuildLineIndex(Blob, Lines);
	const TArray<FYftSpan> Roots = ChildrenAt(Blob, Lines, 0, Lines.Num(), 0);
	const FYftSpan* Root = FindTag(Roots, TEXT("Fragment"));
	if (!Root) { return Fail(TEXT("root is not <Fragment> - this is not a fragment XML")); }

	const TArray<FYftSpan> Top = ChildrenAt(Blob, Lines, Root->FirstLine + 1, Root->LastLine, 1);
	TArray<FString> TopTags;
	for (const FYftSpan& S : Top) { TopTags.Add(S.Tag); }

	const FYftSpan* Drawable = FindTag(Top, TEXT("Drawable"));
	TArray<FString> DrawTags;
	int32 Shaders = 0;
	int32 Bones = 0;
	int32 EmbeddedTextures = 0;
	FString LodJson;
	if (Drawable)
	{
		const TArray<FYftSpan> DC = ChildrenAt(Blob, Lines, Drawable->FirstLine + 1, Drawable->LastLine, 2);
		for (const FYftSpan& S : DC) { DrawTags.Add(S.Tag); }
		if (const FYftSpan* SG = FindTag(DC, TEXT("ShaderGroup")))
		{
			for (const FYftSpan& C : ChildrenAt(Blob, Lines, SG->FirstLine + 1, SG->LastLine, 3))
			{
				if (C.Tag == TEXT("Shaders")) { Shaders = ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4).Num(); }
				if (C.Tag == TEXT("TextureDictionary")) { EmbeddedTextures = ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4).Num(); }
			}
		}
		if (const FYftSpan* Sk = FindTag(DC, TEXT("Skeleton")))
		{
			for (const FYftSpan& C : ChildrenAt(Blob, Lines, Sk->FirstLine + 1, Sk->LastLine, 3))
			{
				if (C.Tag == TEXT("Bones")) { Bones = ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4).Num(); }
			}
		}
		static const TCHAR* AllLods[] = { TEXT("DrawableModelsHigh"), TEXT("DrawableModelsMedium"),
		                                  TEXT("DrawableModelsLow"), TEXT("DrawableModelsVeryLow") };
		for (const TCHAR* LodTag : AllLods)
		{
			const FYftSpan* L = FindTag(DC, LodTag);
			if (!L) { continue; }
			TArray<FYftModelView> Models;
			ReadModels(Blob, Lines, *L, Models);
			int32 Geos = 0;
			int32 Skinned = 0;
			TArray<FString> SemSets;
			for (const FYftModelView& M : Models)
			{
				Geos += M.Geometries.Num();
				if (M.HasSkin == TEXT("1")) { ++Skinned; }
				for (const FYftGeomView& G : M.Geometries)
				{
					const FString Sem = JoinStrings(G.Semantics, TEXT(","));
					SemSets.AddUnique(Sem);
				}
			}
			LodJson += FString::Printf(TEXT("%s\"%s\":{\"models\":%d,\"geometries\":%d,\"skinnedModels\":%d,\"layoutSemanticSets\":[%s]}"),
				LodJson.IsEmpty() ? TEXT("") : TEXT(","), LodTag, Models.Num(), Geos, Skinned,
				*JsonStringArray(SemSets));
		}
	}

	int32 BoneTransforms = -1;
	int32 PhysChildren = -1;
	int32 PhysGroups = -1;
	int32 PhysChildDrawables = -1;
	int32 PhysChildGeometries = -1;
	if (const FYftSpan* BT = FindTag(Top, TEXT("BoneTransforms")))
	{
		BoneTransforms = ChildrenAt(Blob, Lines, BT->FirstLine + 1, BT->LastLine, 2).Num();
	}
	if (const FYftSpan* Phys = FindTag(Top, TEXT("Physics")))
	{
		// the physics children are not just collision hulls - see CountPhysicsChildDrawables
		CountPhysicsChildDrawables(Blob, Lines, *Phys, PhysChildDrawables, PhysChildGeometries);
		for (const FYftSpan& L : ChildrenAt(Blob, Lines, Phys->FirstLine + 1, Phys->LastLine, 2))
		{
			if (L.Tag != TEXT("LOD1")) { continue; }
			for (const FYftSpan& C : ChildrenAt(Blob, Lines, L.FirstLine + 1, L.LastLine, 3))
			{
				if (C.Tag == TEXT("Children")) { PhysChildren = ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4).Num(); }
				if (C.Tag == TEXT("Groups")) { PhysGroups = ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4).Num(); }
			}
		}
	}

	const int32 DrawableBytes = Drawable ? (Drawable->ByteEnd - Drawable->ByteStart) : 0;
	const FYftSpan* PhysSpan = FindTag(Top, TEXT("Physics"));
	const int32 PhysicsBytes = PhysSpan ? (PhysSpan->ByteEnd - PhysSpan->ByteStart) : 0;

	// ok is COMPUTED, and it can be false: a fragment whose drawable yielded no shader table and no
	// LOD block is a probe that learned nothing, and saying "ok" about it would be the lie this
	// codebase keeps catching.
	const bool bOk = (Blob.Num() > 0)
		&& (Drawable == nullptr || (Shaders > 0 && !LodJson.IsEmpty()));
	return FString::Printf(TEXT("{\"ok\":%s,\"name\":\"%s\",\"path\":\"%s\",\"slot\":\"%s\",\"bytes\":%d,")
		TEXT("\"fragChildren\":[%s],\"hasDrawable\":%s,\"drawableChildren\":[%s],")
		TEXT("\"shaders\":%d,\"drawableBones\":%d,\"embeddedTextures\":%d,")
		TEXT("\"boneTransforms\":%d,\"physChildren\":%d,\"physGroups\":%d,")
		TEXT("\"physChildDrawables\":%d,\"physChildGeometries\":%d,")
		TEXT("\"drawableBytes\":%d,\"drawableFraction\":%.6f,\"physicsBytes\":%d,\"physicsFraction\":%.6f,")
		TEXT("\"lods\":{%s},\"hiTwinInCorpus\":%s,")
		TEXT("\"note\":\"the physics hierarchy is CARRIED WHOLE and it is not only collision: its children own real drawables (241 of 245 measured fragments carry at least one, 677 in all) and on 12 of 245 those own geometry (40 in all) that a visual replacement leaves as the donor's. Bones have TWO owners (Drawable/Skeleton/Bones == BoneTransforms on 237/237) - the clone lane never touches one\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), *RudeJsonEscape(XmlPath),
		*RudeJsonEscape(Slot), Blob.Num(), *JsonStringArray(TopTags),
		Drawable ? TEXT("true") : TEXT("false"), *JsonStringArray(DrawTags),
		Shaders, Bones, EmbeddedTextures, BoneTransforms, PhysChildren, PhysGroups,
		PhysChildDrawables, PhysChildGeometries,
		DrawableBytes, (double)DrawableBytes / FMath::Max(1, Blob.Num()),
		PhysicsBytes, (double)PhysicsBytes / FMath::Max(1, Blob.Num()),
		*LodJson, bHiTwin ? TEXT("true") : TEXT("false"));
}

// =================================================================================================
// ExportYftClone - clone a fragment, optionally replacing its visual geometry
// =================================================================================================
FString URudeToolset::ExportYftClone(const FString& SourceName, const FString& MeshAssetPaths,
                                     const FString& OutYftPath, const FString& CorpusRoot,
                                     const FString& Options)
{
	using namespace RudeYftCloneLane;
	TArray<FString> Refusals;
	TArray<FString> Warnings;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"written\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};

	// ---- 0) the source, through the ledger -------------------------------------------------------
	FString Name = SourceName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml"));
	Name.RemoveFromEnd(TEXT(".yft"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a source fragment name, e.g. adder")); }
	if (OutYftPath.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an output path, e.g. D:/out/mycar.yft.xml")); }

	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::LooksLikeCorpus(CorpusRoot)
		? FRudeCorpus::Open(CorpusRoot, CorpusErr) : nullptr;
	FString XmlPath;
	FString Slot;
	bool bHiTwin = false;
	if (Corpus.IsValid())
	{
		if (const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("yft"), Name))
		{
			XmlPath = Corpus->PathOf(*Row);
			Slot = Row->Slot;
		}
		bHiTwin = (Corpus->Effective(TEXT("yft"), Name + TEXT("_hi")) != nullptr);
	}
	if (XmlPath.IsEmpty()) { XmlPath = CorpusRoot / (Name + TEXT(".yft.xml")); }
	if (!FPaths::FileExists(XmlPath))
	{
		return Fail(FString::Printf(TEXT("the corpus has no yft named '%s' - 42,029 distinct fragment stems were counted in the filebase, and 16,199 of them exist in more than one load-order slot, so the name is resolved through the ledger, never by globbing"), *Name));
	}

	TArray<uint8> Blob;
	if (!FFileHelper::LoadFileToArray(Blob, *XmlPath)) { return Fail(FString::Printf(TEXT("could not read %s"), *XmlPath)); }
	TArray<FIntPoint> Lines;
	BuildLineIndex(Blob, Lines);
	const TArray<FYftSpan> Roots = ChildrenAt(Blob, Lines, 0, Lines.Num(), 0);
	const FYftSpan* Root = FindTag(Roots, TEXT("Fragment"));
	if (!Root) { return Fail(TEXT("root is not <Fragment> - this is not a fragment XML")); }

	const TArray<FYftSpan> Top = ChildrenAt(Blob, Lines, Root->FirstLine + 1, Root->LastLine, 1);
	TArray<FString> TopTags;
	for (const FYftSpan& S : Top) { TopTags.Add(S.Tag); }
	const FYftSpan* Drawable = FindTag(Top, TEXT("Drawable"));
	if (!Drawable)
	{
		return Fail(FString::Printf(TEXT("'%s' has no <Drawable> - 8 of 245 measured fragments are like this (cloth and flag fragments carry a <Cloths> block and no visual drawable) and there is nothing here to clone geometry into"), *Name));
	}

	// ---- 1) options -------------------------------------------------------------------------------
	FString LodWhich = TEXT("High");
	FString NewFragName;
	FString PairWhich = TEXT("name");
	{
		TArray<FString> Kvs;
		Options.ParseIntoArray(Kvs, TEXT(" "), true);
		for (const FString& Kv : Kvs)
		{
			FString K, V;
			if (!Kv.Split(TEXT("="), &K, &V)) { continue; }
			K = K.TrimStartAndEnd().ToLower();
			V = V.TrimStartAndEnd();
			if (K == TEXT("lod")) { LodWhich = V; }
			else if (K == TEXT("name")) { NewFragName = V; }
			else if (K == TEXT("pair")) { PairWhich = V.ToLower(); }
		}
	}
	const TCHAR* LodTag = LodTagFor(LodWhich);
	if (!LodTag)
	{
		return Fail(FString::Printf(TEXT("lod='%s' is not a LOD block this tool knows - spell it exactly High, Medium, Low or VeryLow (case-insensitive, no spaces). Measured presence over the 237 fragments that have a drawable: High 237, Medium 200, Low 198, VeryLow 5. Refusing rather than defaulting to High, because writing your meshes into a block you did not name would report ok:true and look identical to a correct run"), *LodWhich));
	}
	if (PairWhich != TEXT("name") && PairWhich != TEXT("index"))
	{
		return Fail(FString::Printf(TEXT("pair='%s' is not a pairing mode - it is either 'name' (the default: join each mesh material slot to a template geometry by its shader preset) or 'index' (mesh slot i -> template geometry i, an order NOBODY HAS MEASURED, which is why it has to be asked for by name)"), *PairWhich));
	}
	if (!NewFragName.IsEmpty() && !NameIsWritable(NewFragName))
	{
		return Fail(FString::Printf(TEXT("name='%s' contains a character this tool will not write - a fragment name here may use A-Z a-z 0-9 _ and - only. It is refused rather than masked to ASCII, because masking would write a DIFFERENT name and still report renamed:true. (Options are split on spaces, so a name cannot contain one either.)"), *NewFragName));
	}

	const TArray<FYftSpan> DC = ChildrenAt(Blob, Lines, Drawable->FirstLine + 1, Drawable->LastLine, 2);
	TArray<FString> DrawTags;
	for (const FYftSpan& S : DC) { DrawTags.Add(S.Tag); }
	int32 ShaderCount = 0;
	if (const FYftSpan* SG = FindTag(DC, TEXT("ShaderGroup")))
	{
		for (const FYftSpan& C : ChildrenAt(Blob, Lines, SG->FirstLine + 1, SG->LastLine, 3))
		{
			if (C.Tag == TEXT("Shaders")) { ShaderCount = ChildrenAt(Blob, Lines, C.FirstLine + 1, C.LastLine, 4).Num(); }
		}
	}
	TArray<FString> ShaderPresets;
	ReadShaderPresets(Blob, Lines, DC, ShaderPresets);

	// what the physics hierarchy is holding that this lane will not touch
	int32 PhysChildDrawables = 0;
	int32 PhysChildGeometries = 0;
	if (const FYftSpan* PhysSpan = FindTag(Top, TEXT("Physics")))
	{
		CountPhysicsChildDrawables(Blob, Lines, *PhysSpan, PhysChildDrawables, PhysChildGeometries);
	}

	const FYftSpan* LodSpan = FindTag(DC, LodTag);
	if (!LodSpan)
	{
		return Fail(FString::Printf(TEXT("'%s' has no <%s> - measured presence across 237 fragments with a drawable: High 237, Medium 200, Low 198, VeryLow 5"), *Name, LodTag));
	}
	TArray<FYftModelView> Models;
	ReadModels(Blob, Lines, *LodSpan, Models);

	int32 TemplateGeometries = 0;
	for (const FYftModelView& M : Models) { TemplateGeometries += M.Geometries.Num(); }

	// ---- 2) the meshes ----------------------------------------------------------------------------
	TArray<FString> MeshPaths;
	MeshAssetPaths.ParseIntoArray(MeshPaths, TEXT(","), true);
	for (FString& P : MeshPaths) { P = P.TrimStartAndEnd(); }
	MeshPaths.RemoveAll([](const FString& P) { return P.IsEmpty(); });

	const bool bPureClone = (MeshPaths.Num() == 0);
	if (!bPureClone && MeshPaths.Num() != Models.Num())
	{
		return Fail(FString::Printf(TEXT("this template's <%s> holds %d model(s) and %d mesh(es) were given - one mesh per model, in order, or none at all for a pure clone"),
			LodTag, Models.Num(), MeshPaths.Num()));
	}

	// per model, per geometry, the authored replacement (empty for a pure clone)
	TArray<TArray<FYftAuthoredGeo>> Authored;
	int32 ColourNeutral = 0;
	int32 TangentNeutral = 0;
	int32 UvChannelsSynthesised = 0;
	int32 VerticesWritten = 0;
	int32 TrianglesWritten = 0;
	int32 BBoxWFollowedX = 0;
	int32 PairedByName = 0;
	int32 PairedByIndex = 0;
	int32 PairsSharingAShaderName = 0;

#if WITH_EDITORONLY_DATA
	for (int32 Mi = 0; Mi < MeshPaths.Num(); ++Mi)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPaths[Mi]);
		if (!Mesh)
		{
			Refusals.Add(FString::Printf(TEXT("StaticMesh not found: %s"), *MeshPaths[Mi]));
			continue;
		}
		const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
		if (!MeshDesc)
		{
			Refusals.Add(FString::Printf(TEXT("no MeshDescription on LOD0 of %s"), *MeshPaths[Mi]));
			continue;
		}
		FStaticMeshConstAttributes Attributes(*MeshDesc);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesConstRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();

		const FYftModelView& TemplateModel = Models[Mi];
		TPolygonGroupAttributesConstRef<FName> GroupSlots = Attributes.GetPolygonGroupMaterialSlotNames();
		TArray<FPolygonGroupID> Groups;
		TArray<FString> SlotNames;
		TArray<FString> SlotPresets;
		bool bAllSlotsRudeShaped = true;
		for (const FPolygonGroupID GroupID : MeshDesc->PolygonGroups().GetElementIDs())
		{
			Groups.Add(GroupID);
			const FString SlotName = GroupSlots[GroupID].ToString();
			SlotNames.Add(SlotName);
			const int32 Sep = SlotName.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Sep == INDEX_NONE) { bAllSlotsRudeShaped = false; SlotPresets.Add(SlotName); }
			else { SlotPresets.Add(SlotName.Left(Sep)); }
		}
		if (Groups.Num() != TemplateModel.Geometries.Num())
		{
			Refusals.Add(FString::Printf(TEXT("%s presents %d material slot(s) but model %d of this template holds %d geometry(ies) - the shader index is the only join the file gives us (1,736 of 1,736 measured geometries carry one inside [0,%d)), so the counts have to match exactly"),
				*MeshPaths[Mi], Groups.Num(), Mi, TemplateModel.Geometries.Num(), FMath::Max(1, ShaderCount)));
			continue;
		}

		// ---- THE PAIRING, which used to be an unexamined assumption -----------------------------
		// Mesh slot i -> template geometry i is TWO DIFFERENT ORDERS assumed to agree: Unreal's
		// polygon-group order and the fragment's geometry document order. Nobody measured that they
		// do. So the default is a NAME join instead: `ImportYdr` writes every slot name as
		// `<preset>__<geoIndex>`, and every template geometry resolves its `<ShaderIndex>` to a
		// preset name (1,736 of 1,736 measured indices are in range). Match the preset names and the
		// order never has to be assumed. Where a preset name repeats inside one model - 216 of 644
		// measured models - the name cannot separate those geometries, so the residual order
		// assumption is confined to geometries that share a shader and is COUNTED
		// (`pairsSharingAShaderName`). If the names cannot be matched at all, this REFUSES; pairing
		// by index is available but has to be asked for by name (`pair=index`), and says so in the
		// verdict as `byIndexUnverified`. Denominators: `measure_yft2.json`.
		TArray<FString> TemplatePresets;
		bool bAllPresetsKnown = true;
		for (const FYftGeomView& TGeo : TemplateModel.Geometries)
		{
			const FString P = ShaderPresets.IsValidIndex(TGeo.ShaderIndex) ? ShaderPresets[TGeo.ShaderIndex] : FString();
			if (P.IsEmpty()) { bAllPresetsKnown = false; }
			TemplatePresets.Add(P);
		}
		TArray<int32> GeoToSlot;
		GeoToSlot.Init(INDEX_NONE, TemplateModel.Geometries.Num());
		bool bPairedByName = false;
		if (bAllSlotsRudeShaped && bAllPresetsKnown)
		{
			TArray<bool> Taken;
			Taken.Init(false, SlotPresets.Num());
			bool bMatchedAll = true;
			for (int32 g = 0; g < TemplatePresets.Num(); ++g)
			{
				int32 Hit = INDEX_NONE;
				for (int32 s = 0; s < SlotPresets.Num(); ++s)
				{
					if (!Taken[s] && SlotPresets[s] == TemplatePresets[g]) { Hit = s; break; }
				}
				if (Hit == INDEX_NONE) { bMatchedAll = false; break; }
				Taken[Hit] = true;
				GeoToSlot[g] = Hit;
			}
			bPairedByName = bMatchedAll;
		}
		if (bPairedByName)
		{
			PairedByName += TemplatePresets.Num();
			for (int32 g = 0; g < TemplatePresets.Num(); ++g)
			{
				int32 Same = 0;
				for (int32 h = 0; h < TemplatePresets.Num(); ++h)
				{
					if (TemplatePresets[h] == TemplatePresets[g]) { ++Same; }
				}
				if (Same > 1) { ++PairsSharingAShaderName; }
			}
		}
		else if (PairWhich == TEXT("index"))
		{
			for (int32 g = 0; g < GeoToSlot.Num(); ++g) { GeoToSlot[g] = g; }
			PairedByIndex += GeoToSlot.Num();
			Warnings.Add(FString::Printf(TEXT("%s was paired to model %d BY INDEX because you asked for pair=index - mesh slot i went to template geometry i. That is Unreal's polygon-group order matched against the fragment's document order, and nobody has measured that those agree. Slot names [%s] against template presets [%s]"),
				*MeshPaths[Mi], Mi, *JoinStrings(SlotNames, TEXT(", ")), *JoinStrings(TemplatePresets, TEXT(", "))));
		}
		else
		{
			Refusals.Add(FString::Printf(TEXT("cannot establish which material slot of %s belongs to which geometry of model %d. Its slot names are [%s] and this model's geometries want the shader presets [%s]; a RUDE-imported mesh names its slots '<preset>__<n>' and those presets have to match. Fix the mesh's slot names, or pass Options 'pair=index' to accept mesh slot i -> geometry i, an order NOBODY HAS MEASURED"),
				*MeshPaths[Mi], Mi, *JoinStrings(SlotNames, TEXT(", ")), *JoinStrings(TemplatePresets, TEXT(", "))));
			continue;
		}

		TArray<FYftAuthoredGeo> PerGeo;
		PerGeo.SetNum(TemplateModel.Geometries.Num());
		for (int32 Gi = 0; Gi < TemplateModel.Geometries.Num(); ++Gi)
		{
			const FPolygonGroupID GroupID = Groups[GeoToSlot[Gi]];
			const FYftGeomView& TG = TemplateModel.Geometries[Gi];
			// the layout is the contract, and it is per geometry
			bool bSkinned = false;
			bool bUnwritable = false;
			FString BadSemantic;
			for (const FString& Sem : TG.Semantics)
			{
				if (Sem == TEXT("BlendWeights") || Sem == TEXT("BlendIndices")) { bSkinned = true; }
				else if (!SemanticIsWritable(Sem)) { bUnwritable = true; BadSemantic = Sem; }
			}
			if (bSkinned)
			{
				Refusals.Add(FString::Printf(TEXT("model %d geometry %d of '%s' is SKINNED (its layout carries BlendWeights/BlendIndices; 34 of 644 measured models are) and %s is a UStaticMesh with no skinning to give it - refusing rather than writing a zero weight column"),
					Mi, Gi, *Name, *MeshPaths[Mi]));
			}
			if (bUnwritable)
			{
				Refusals.Add(FString::Printf(TEXT("model %d geometry %d of '%s' wants a '%s' column this lane cannot author - refusing rather than filling it with something that looks right and is not"),
					Mi, Gi, *Name, *BadSemantic));
			}

			FYftAuthoredGeo G;
			const int32 UvChannels = InstUVs.GetNumChannels();
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
						const FVector2f UV0 = InstUVs.Get(Inst, 0);
						const FString Key = FString::Printf(TEXT("%d|%.3f,%.3f,%.3f|%.4f,%.4f"),
							VID.GetValue(), N.X, N.Y, N.Z, UV0.X, UV0.Y);
						int32 Index = INDEX_NONE;
						if (const int32* Found = Weld.Find(Key)) { Index = *Found; }
						else
						{
							Index = G.Pos.Num();
							// AGENTS.md section 6.1, inverted: UE cm -> GTA metres with the Y mirror
							G.Pos.Add(FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f));
							G.Nrm.Add(FVector3f(N.X, -N.Y, N.Z));
							TArray<FVector2f> Uvs;
							for (int32 Ch = 0; Ch < 4; ++Ch)
							{
								if (Ch < UvChannels) { Uvs.Add(InstUVs.Get(Inst, Ch)); }
								else { Uvs.Add(UV0); }
							}
							G.UVs.Add(Uvs);
							Weld.Add(Key, Index);
						}
						G.Indices.Add(Index);
					}
				}
			}
			if (G.Pos.Num() == 0 || G.Indices.Num() < 3)
			{
				Refusals.Add(FString::Printf(TEXT("material slot '%s' of %s (paired to model %d geometry %d) has no triangles - a geometry the game reads must not be empty"),
					*SlotNames[GeoToSlot[Gi]], *MeshPaths[Mi], Mi, Gi));
			}
			for (int32 v = 0; v < G.Pos.Num(); ++v)
			{
				if (v == 0) { G.BBoxMin = G.Pos[v]; G.BBoxMax = G.Pos[v]; }
				else
				{
					G.BBoxMin = FVector3f(FMath::Min(G.BBoxMin.X, G.Pos[v].X), FMath::Min(G.BBoxMin.Y, G.Pos[v].Y), FMath::Min(G.BBoxMin.Z, G.Pos[v].Z));
					G.BBoxMax = FVector3f(FMath::Max(G.BBoxMax.X, G.Pos[v].X), FMath::Max(G.BBoxMax.Y, G.Pos[v].Y), FMath::Max(G.BBoxMax.Z, G.Pos[v].Z));
				}
			}
			for (const FString& Sem : TG.Semantics)
			{
				if (Sem == TEXT("Colour0") || Sem == TEXT("Colour1")) { ColourNeutral += G.Pos.Num(); }
				else if (Sem == TEXT("Tangent")) { TangentNeutral += G.Pos.Num(); }
				else if (Sem.StartsWith(TEXT("TexCoord")))
				{
					const int32 Ch = FCString::Atoi(*Sem.RightChop(8));
					if (Ch >= InstUVs.GetNumChannels()) { UvChannelsSynthesised += G.Pos.Num(); }
				}
			}
			VerticesWritten += G.Pos.Num();
			TrianglesWritten += G.Indices.Num() / 3;
			PerGeo[Gi] = MoveTemp(G);
		}
		Authored.Add(MoveTemp(PerGeo));
	}
#else
	if (!bPureClone)
	{
		Refusals.Add(TEXT("mesh replacement needs editor-only mesh data; this build has none"));
	}
#endif

	if (Refusals.Num() > 0)
	{
		// NOTHING is written on a refusal - an earlier good export is not clobbered by a bad run.
		return FString::Printf(TEXT("{\"ok\":false,\"written\":false,\"source\":\"%s\",\"refusals\":[%s]}"),
			*RudeJsonEscape(Name), *JsonStringArray(Refusals));
	}

	// ---- 3) build the output: source bytes, with only the authored spans replaced ------------------
	struct FYftEdit
	{
		int32 Start = 0;
		int32 End = 0;
		FString Text;
	};
	TArray<FYftEdit> Edits;

	// the fragment's own name, when the author asked for a new one
	bool bRenamed = false;
	if (!NewFragName.IsEmpty())
	{
		// ⛔ A missing <Name> is a REFUSAL, not a skip. Skipping it handed back a byte-identical file
		// with ok:true and only `renamed:false` to say the edit the author asked for never happened.
		const FYftSpan* NameSpan = FindTag(Top, TEXT("Name"));
		if (!NameSpan)
		{
			return Fail(FString::Printf(TEXT("you asked to rename '%s' but it has no top-level <Name> element to rewrite - nothing was written, because a clone that silently ignored the rename would be byte-identical to the source and still report ok"), *Name));
		}
		FYftEdit E;
		E.Start = NameSpan->ByteStart;
		E.End = NameSpan->ByteEnd;
		E.Text = FString::Printf(TEXT(" <Name>pack:/%s</Name>\n"), *NewFragName);
		Edits.Add(E);
		bRenamed = true;
	}

	for (int32 Mi = 0; Mi < Authored.Num(); ++Mi)
	{
		const FYftModelView& TemplateModel = Models[Mi];
		for (int32 Gi = 0; Gi < Authored[Mi].Num(); ++Gi)
		{
			const FYftGeomView& TG = TemplateModel.Geometries[Gi];
			const FYftAuthoredGeo& AG = Authored[Mi][Gi];

			// A `<Data>` that is self-closing or opens and closes on ONE line has no interior to splice
			// into. Refuse rather than emit an inverted span - an edit whose End precedes its Start is
			// exactly the silent corruption this lane's measure exists to catch.
			if (TG.bHasVertexData && TG.VertexDataSpan.LastLine <= TG.VertexDataSpan.FirstLine)
			{
				return Fail(FString::Printf(TEXT("model %d geometry %d of '%s' has a one-line <Data> element with no interior to replace"), Mi, Gi, *Name));
			}

			// (a) the vertex <Data> payload, in the TEMPLATE's own column order, spelled the way the
			//     corpus spells it: an 8-space indent (1,298,225 of 1,298,225 measured vertex rows)
			//     and a 3-space run between columns (7,128,690 of 7,128,690 gaps) - `measure_yft2.json`.
			if (TG.bHasVertexData)
			{
				FString Rows;
				for (int32 v = 0; v < AG.Pos.Num(); ++v)
				{
					FString Row = TEXT("        ");
					bool bFirstCol = true;
					for (const FString& Sem : TG.Semantics)
					{
						if (!bFirstCol) { Row += TEXT("   "); }
						bFirstCol = false;
						if (Sem == TEXT("Position"))
						{
							Row += FString::Printf(TEXT("%s %s %s"), *FmtF32(AG.Pos[v].X), *FmtF32(AG.Pos[v].Y), *FmtF32(AG.Pos[v].Z));
						}
						else if (Sem == TEXT("Normal"))
						{
							Row += FString::Printf(TEXT("%s %s %s"), *FmtF32(AG.Nrm[v].X), *FmtF32(AG.Nrm[v].Y), *FmtF32(AG.Nrm[v].Z));
						}
						else if (Sem == TEXT("Colour0") || Sem == TEXT("Colour1"))
						{
							Row += TEXT("255 255 255 255");     // NEUTRAL, and counted as such
						}
						else if (Sem == TEXT("Tangent"))
						{
							Row += TEXT("0 0 0 1");             // NEUTRAL, and counted as such
						}
						else if (Sem.StartsWith(TEXT("TexCoord")))
						{
							const int32 Ch = FMath::Clamp(FCString::Atoi(*Sem.RightChop(8)), 0, 3);
							Row += FString::Printf(TEXT("%s %s"), *FmtF32(AG.UVs[v][Ch].X), *FmtF32(AG.UVs[v][Ch].Y));
						}
					}
					Rows += Row + TEXT("\n");
				}
				FYftEdit E;
				E.Start = Lines[TG.VertexDataSpan.FirstLine].Y;      // just after `<Data>`
				E.End = Lines[TG.VertexDataSpan.LastLine].X;         // just before `</Data>`
				E.Text = Rows;
				Edits.Add(E);
			}

			// (b) the index <Data> payload - 24 indices to a line at an 8-space indent, the way the
			//     corpus spells them: 181,601 of 181,601 FULL index rows carry exactly 24 tokens and
			//     all 182,609 index rows (the 1,008 final short ones included) sit at 8 spaces
			//     (`measure_yft2.json`). The separator inside an index row is a single space.
			if (TG.bHasIndexData)
			{
				// THE COUNT DECIDES THE FORM, NOT THE TEMPLATE. Measured 2026-09-07 over 1,200 fragments: an
				// index block of 24 indices or fewer is written on ONE line (counts 3/6/9/12/15/18/21/24, 791
				// blocks, not one of them multi-line) and 27 or more is written in rows of exactly 24 (793,622
				// full rows, every one 24 wide) with the remainder last - a clean threshold, one-line max 24
				// against multi-line min 27. So the WHOLE <Data> element is rebuilt rather than its interior
				// spliced: a one-line template can take a large replacement and a large template a small one.
				// Before this the one-line form refused, and EVERY small fragment this lane can author writes
				// its indices on one line, so the replacement half could not run at all.
				const int32 Ind = IndentOf(Blob, Lines[TG.IndexDataSpan.FirstLine]);
				FString Pad;
				for (int32 sp = 0; sp < Ind; ++sp) { Pad += TEXT(" "); }
				FString Text;
				if (AG.Indices.Num() <= 24)
				{
					FString Inline;
					for (int32 k = 0; k < AG.Indices.Num(); ++k)
					{
						if (k > 0) { Inline += TEXT(" "); }
						Inline += FString::FromInt(AG.Indices[k]);
					}
					Text = Pad + TEXT("<Data>") + Inline + TEXT("</Data>\n");
				}
				else
				{
					FString Rows;
					for (int32 i = 0; i < AG.Indices.Num(); i += 24)
					{
						FString Row = Pad + TEXT("  ");
						const int32 Stop = FMath::Min(i + 24, AG.Indices.Num());
						for (int32 k = i; k < Stop; ++k)
						{
							if (k > i) { Row += TEXT(" "); }
							Row += FString::FromInt(AG.Indices[k]);
						}
						Rows += Row + TEXT("\n");
					}
					Text = Pad + TEXT("<Data>\n") + Rows + Pad + TEXT("</Data>\n");
				}
				FYftEdit E;
				E.Start = Lines[TG.IndexDataSpan.FirstLine].X;
				E.End = Lines[TG.IndexDataSpan.LastLine].Y;
				E.Text = Text;
				Edits.Add(E);
			}

			// (c) the geometry's bounding box, at the corpus's 6-space indent (3,472 of 3,472
			//     measured geometry bbox lines). x/y/z are recomputed from the authored mesh. `w` is
			//     NOT carried and this is a deliberate reversal: `w` equals `x` on 3,472 of 3,472
			//     measured lines (`measure_yft2.json`), so carrying the template's `w` while
			//     rewriting `x` would break the one invariant the corpus actually holds here.
			//     ⛔ Nobody has modelled what `w` MEANS - this follows the corpus's own invariant
			//     rather than an understanding of it, and every line written that way is counted as
			//     `bboxWFollowedX` so the choice cannot hide.
			auto EditBBox = [&](const FYftSpan& S, const FVector3f& V)
			{
				const FString W = AttrOfLine(Blob, Lines[S.FirstLine], TEXT("w"));
				const FString Tag = S.Tag;
				FYftEdit E;
				E.Start = S.ByteStart;
				E.End = S.ByteEnd;
				if (W.IsEmpty())
				{
					E.Text = FString::Printf(TEXT("      <%s x=\"%s\" y=\"%s\" z=\"%s\" />\n"), *Tag, *FmtF32(V.X), *FmtF32(V.Y), *FmtF32(V.Z));
				}
				else
				{
					++BBoxWFollowedX;
					E.Text = FString::Printf(TEXT("      <%s x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />\n"), *Tag, *FmtF32(V.X), *FmtF32(V.Y), *FmtF32(V.Z), *FmtF32(V.X));
				}
				Edits.Add(E);
			};
			if (TG.bHasBBoxMin) { EditBBox(TG.BBoxMin, AG.BBoxMin); }
			if (TG.bHasBBoxMax) { EditBBox(TG.BBoxMax, AG.BBoxMax); }
		}
	}

	// ---- (d) THE DRAWABLE'S OWN BOUNDS, RECOMPUTED (2026-09-11) ---------------------------------
	// Replacing a geometry rewrote THAT geometry's bbox (above) and left the DRAWABLE's bounds at the
	// donor's, so a replacement bigger than the mesh it displaced is culled at the old extent - the
	// "nothing recomputes the culling bounds" half of the fragment gap.
	// MEASURED CONVENTION, not assumed. Over 200 drawables: the sphere CENTRE is the bbox centre
	// (worst axis delta 6.2e-5) and the RADIUS is half the bbox diagonal (worst 1.4e-5) - float
	// rounding, 200/200 on both, so there is no second rule to discover. And over 60 fragments the
	// FRAGMENT-level radius equals the DRAWABLE-level radius 60/60, so both are written, same value.
	// ⛔ ONLY WHEN SOMETHING WAS REPLACED. A pure clone must stay byte-identical (the lane's 6/6
	// gate), and it does: `Authored` is empty on that path and this block writes nothing.
	int32 DrawableBoundsWritten = 0;
	if (Authored.Num() > 0)
	{
		auto SpanVec = [&](const FYftSpan& S) -> FVector3f
		{
			return FVector3f(
				FCString::Atof(*AttrOfLine(Blob, Lines[S.FirstLine], TEXT("x"))),
				FCString::Atof(*AttrOfLine(Blob, Lines[S.FirstLine], TEXT("y"))),
				FCString::Atof(*AttrOfLine(Blob, Lines[S.FirstLine], TEXT("z"))));
		};
		// The union has to span EVERY geometry in the drawable, replaced or not - a union over only
		// the replaced ones would shrink the box around whatever was left untouched.
		FVector3f DMin(0.f), DMax(0.f);
		bool bAnyGeo = false;
		for (int32 Mi = 0; Mi < Models.Num(); ++Mi)
		{
			for (int32 Gi = 0; Gi < Models[Mi].Geometries.Num(); ++Gi)
			{
				const FYftGeomView& GV = Models[Mi].Geometries[Gi];
				FVector3f GMin, GMax;
				if (Authored.IsValidIndex(Mi) && Authored[Mi].IsValidIndex(Gi))
				{
					GMin = Authored[Mi][Gi].BBoxMin;
					GMax = Authored[Mi][Gi].BBoxMax;
				}
				else if (GV.bHasBBoxMin && GV.bHasBBoxMax)
				{
					GMin = SpanVec(GV.BBoxMin);
					GMax = SpanVec(GV.BBoxMax);
				}
				else
				{
					continue;   // a geometry with no bbox to read contributes nothing, and says so below
				}
				if (!bAnyGeo) { DMin = GMin; DMax = GMax; bAnyGeo = true; }
				else
				{
					DMin = FVector3f(FMath::Min(DMin.X, GMin.X), FMath::Min(DMin.Y, GMin.Y), FMath::Min(DMin.Z, GMin.Z));
					DMax = FVector3f(FMath::Max(DMax.X, GMax.X), FMath::Max(DMax.Y, GMax.Y), FMath::Max(DMax.Z, GMax.Z));
				}
			}
		}
		if (bAnyGeo)
		{
			const FVector3f Centre = (DMin + DMax) * 0.5f;
			const float Radius = (DMax - DMin).Size() * 0.5f;

			// Indent comes from the line being replaced, never a constant: the fragment level and the
			// drawable level sit at different depths and the corpus is the authority on both.
			auto EditVecAt = [&](const FYftSpan& S, const FVector3f& V)
			{
				const int32 Ind = IndentOf(Blob, Lines[S.FirstLine]);
				const FString Pad = FString::ChrN(Ind, TCHAR(' '));
				const FString W = AttrOfLine(Blob, Lines[S.FirstLine], TEXT("w"));
				FYftEdit E;
				E.Start = S.ByteStart;
				E.End = S.ByteEnd;
				if (W.IsEmpty())
				{
					E.Text = Pad + FString::Printf(TEXT("<%s x=\"%s\" y=\"%s\" z=\"%s\" />\n"),
						*S.Tag, *FmtF32(V.X), *FmtF32(V.Y), *FmtF32(V.Z));
				}
				else
				{
					E.Text = Pad + FString::Printf(TEXT("<%s x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />\n"),
						*S.Tag, *FmtF32(V.X), *FmtF32(V.Y), *FmtF32(V.Z), *FmtF32(V.X));
				}
				Edits.Add(E);
				++DrawableBoundsWritten;
			};
			auto EditScalarAt = [&](const FYftSpan& S, float V)
			{
				const int32 Ind = IndentOf(Blob, Lines[S.FirstLine]);
				const FString Pad = FString::ChrN(Ind, TCHAR(' '));
				FYftEdit E;
				E.Start = S.ByteStart;
				E.End = S.ByteEnd;
				E.Text = Pad + FString::Printf(TEXT("<%s value=\"%s\" />\n"), *S.Tag, *FmtF32(V));
				Edits.Add(E);
				++DrawableBoundsWritten;
			};

			if (const FYftSpan* DrawSpan = FindTag(Top, TEXT("Drawable")))
			{
				const TArray<FYftSpan> DrawBoundsChildren = ChildrenAt(Blob, Lines, DrawSpan->FirstLine + 1, DrawSpan->LastLine, 2);
				if (const FYftSpan* S = FindTag(DrawBoundsChildren, TEXT("BoundingBoxMin")))       { EditVecAt(*S, DMin); }
				if (const FYftSpan* S = FindTag(DrawBoundsChildren, TEXT("BoundingBoxMax")))       { EditVecAt(*S, DMax); }
				if (const FYftSpan* S = FindTag(DrawBoundsChildren, TEXT("BoundingSphereCenter"))) { EditVecAt(*S, Centre); }
				if (const FYftSpan* S = FindTag(DrawBoundsChildren, TEXT("BoundingSphereRadius"))) { EditScalarAt(*S, Radius); }
			}
			// ...and the fragment's own radius, which the corpus holds equal to the drawable's 60/60.
			if (const FYftSpan* S = FindTag(Top, TEXT("BoundingSphereRadius"))) { EditScalarAt(*S, Radius); }
		}
	}

	int32 GeometriesReplaced = 0;
	for (const TArray<FYftAuthoredGeo>& Per : Authored) { GeometriesReplaced += Per.Num(); }

	Edits.Sort([](const FYftEdit& A, const FYftEdit& B) { return A.Start < B.Start; });
	for (int32 i = 1; i < Edits.Num(); ++i)
	{
		if (Edits[i].Start < Edits[i - 1].End)
		{
			return Fail(TEXT("internal: two edits overlap - refusing to write a file assembled from overlapping spans"));
		}
	}

	TArray<uint8> Out;
	Out.Reserve(Blob.Num() + 4096);
	int32 Cursor = 0;
	int64 BytesRewritten = 0;
	for (const FYftEdit& E : Edits)
	{
		AppendSlice(Out, Blob, Cursor, E.Start);
		AppendAscii(Out, E.Text);
		BytesRewritten += E.Text.Len();
		Cursor = E.End;
	}
	AppendSlice(Out, Blob, Cursor, Blob.Num());
	int64 BytesReplacedInSource = 0;
	for (const FYftEdit& E : Edits) { BytesReplacedInSource += (int64)(E.End - E.Start); }
	const int64 BytesCarried = (int64)Blob.Num() - BytesReplacedInSource;

	// ---- 4) THE MEASURE, computed rather than claimed ---------------------------------------------
	bool bByteIdentical = (Out.Num() == Blob.Num());
	if (bByteIdentical)
	{
		for (int32 i = 0; i < Out.Num(); ++i)
		{
			if (Out[i] != Blob[i]) { bByteIdentical = false; break; }
		}
	}
	if (bPureClone && !bRenamed && !bByteIdentical)
	{
		return Fail(TEXT("a clone with nothing replaced did NOT reproduce the source byte for byte - the splice is broken and nothing was written (measured 245/245 on the corpus, so this is a defect in this run, not in the file)"));
	}
	if (!bPureClone && bByteIdentical)
	{
		Warnings.Add(TEXT("meshes were given but the output is byte-identical to the source - nothing was actually replaced"));
	}

	if (!FFileHelper::SaveArrayToFile(Out, *OutYftPath))
	{
		return Fail(FString::Printf(TEXT("could not write %s"), *OutYftPath));
	}

	if (!bPureClone && PhysChildGeometries > 0)
	{
		Warnings.Add(FString::Printf(TEXT("the geometry was replaced but %d physics-child drawable(s) inside this fragment still hold the DONOR's geometry (%d geometries). `Physics/LOD*/Children/*/Drawable` is a real drawable - the breakable pieces - and this lane never edits one, so those pieces are still '%s'. Measured over 245 fragments: 241 carry at least one nested drawable (677 in all) and 12 carry nested geometry (40 in all)"),
			PhysChildDrawables, PhysChildGeometries, *Name));
	}

	if (bHiTwin)
	{
		Warnings.Add(FString::Printf(TEXT("'%s_hi' exists in the corpus and was NOT cloned - the _hi twin is a separate fragment (it carries more geometries than its base on 8 of 8 measured pairs, and on 7 of 8 the base carries a <VehicleGlassWindows> block the twin does not). Clone it separately or the asset ships at low detail"), *Name));
	}

	// A clone that authored NOTHING must be byte identical or this tool is broken; a clone that
	// authored something must actually have written bytes. Both are computed, neither is asserted.
	const bool bOk = (Refusals.Num() == 0)
		&& ((bPureClone && !bRenamed) ? bByteIdentical : (BytesRewritten > 0));

	// WHAT WAS CARRIED, named rather than implied. Every top-level block except the drawable is
	// copied byte for byte, and inside the drawable every block except the target LOD block is too -
	// so these two lists ARE the carry, spelled as the file spells them.
	TArray<FString> CarriedTop;
	for (const FString& T : TopTags)
	{
		if (bPureClone || T != TEXT("Drawable")) { CarriedTop.Add(T); }
	}
	TArray<FString> CarriedDraw;
	for (const FString& T : DrawTags)
	{
		if (bPureClone || T != LodTag) { CarriedDraw.Add(T); }
	}
	const FString PairingMode = bPureClone ? FString(TEXT("none"))
		: (PairedByIndex > 0 ? FString(TEXT("byIndexUnverified")) : FString(TEXT("byShaderName")));

	return FString::Printf(TEXT("{\"ok\":%s,\"written\":true,\"source\":\"%s\",\"sourcePath\":\"%s\",\"slot\":\"%s\",")
		TEXT("\"outPath\":\"%s\",\"sourceBytes\":%d,\"outBytes\":%d,\"byteIdentical\":%s,\"pureClone\":%s,\"renamed\":%s,")
		TEXT("\"lodRequested\":\"%s\",\"targetLod\":\"%s\",\"models\":%d,\"templateGeometries\":%d,\"shaders\":%d,")
		TEXT("\"meshesGiven\":%d,\"geometriesReplaced\":%d,\"verticesWritten\":%d,\"trianglesWritten\":%d,")
		TEXT("\"pairing\":\"%s\",\"pairedByShaderName\":%d,\"pairedByIndexUnverified\":%d,\"pairsSharingAShaderName\":%d,")
		TEXT("\"bytesRewritten\":%lld,\"bytesCarriedVerbatim\":%lld,\"carriedFraction\":%.6f,")
		TEXT("\"colourValuesNeutral\":%d,\"tangentValuesNeutral\":%d,\"uvChannelsSynthesised\":%d,\"bboxWFollowedX\":%d,")
		TEXT("\"physChildDrawables\":%d,\"physChildGeometries\":%d,")
		TEXT("\"drawableBoundsWritten\":%d,\"fragChildren\":[%s],\"drawableChildren\":[%s],\"carriedTopLevelBlocks\":[%s],\"carriedDrawableBlocks\":[%s],")
		TEXT("\"hiTwinInCorpus\":%s,\"refusals\":[%s],\"warnings\":[%s],")
		TEXT("\"note\":\"XML interchange only - nothing packs a fragment XML back into a .yft, so the game cannot load this yet\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), *RudeJsonEscape(XmlPath), *RudeJsonEscape(Slot),
		*RudeJsonEscape(OutYftPath), Blob.Num(), Out.Num(),
		bByteIdentical ? TEXT("true") : TEXT("false"), bPureClone ? TEXT("true") : TEXT("false"),
		bRenamed ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(LodWhich), LodTag, Models.Num(), TemplateGeometries, ShaderCount,
		MeshPaths.Num(), GeometriesReplaced, VerticesWritten, TrianglesWritten,
		*PairingMode, PairedByName, PairedByIndex, PairsSharingAShaderName,
		BytesRewritten, BytesCarried, (double)BytesCarried / FMath::Max(1, Blob.Num()),
		ColourNeutral, TangentNeutral, UvChannelsSynthesised, BBoxWFollowedX,
		PhysChildDrawables, PhysChildGeometries,
		DrawableBoundsWritten,
		*JsonStringArray(TopTags), *JsonStringArray(DrawTags),
		*JsonStringArray(CarriedTop), *JsonStringArray(CarriedDraw),
		bHiTwin ? TEXT("true") : TEXT("false"),
		*JsonStringArray(Refusals), *JsonStringArray(Warnings));
}
