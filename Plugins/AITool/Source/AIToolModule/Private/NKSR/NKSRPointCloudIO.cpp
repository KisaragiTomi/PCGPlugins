#include "NKSRPointCloudIO.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringBuilder.h"

#include <cstdlib>

// Point-cloud / mesh IO. Pure input parsing: everything returns false + OutError on malformed
// data (never check()/ensure()). All binary formats are little-endian; UE runtime platforms
// are little-endian so scalar reads are plain memcpy.

namespace
{

// ---------------------------------------------------------------------------
// Shared text helpers
// ---------------------------------------------------------------------------

/** Splits a line on any mix of whitespace and commas; false if any token is not a number. */
bool SplitNumericTokens(const FString& Line, TArray<float>& Out)
{
	Out.Reset();
	const int32 Len = Line.Len();
	int32 I = 0;
	while (I < Len)
	{
		while (I < Len && (FChar::IsWhitespace(Line[I]) || Line[I] == TEXT(','))) ++I;
		const int32 Start = I;
		while (I < Len && !FChar::IsWhitespace(Line[I]) && Line[I] != TEXT(',')) ++I;
		if (I > Start)
		{
			// wcstod (not LexTryParseString): must accept scientific notation like 1e-5.
			const FString Token = Line.Mid(Start, I - Start);
			TCHAR* EndPtr = nullptr;
			const double V = wcstod(*Token, &EndPtr);
			if (EndPtr == nullptr || EndPtr == *Token || *EndPtr != TEXT('\0')) return false;
			Out.Add((float)V);
		}
	}
	return true;
}

/** Extracts the next line (without CR/LF) from Text starting at Pos; false when exhausted. */
bool NextLine(const FString& Text, int32& Pos, FString& OutLine)
{
	const int32 Len = Text.Len();
	if (Pos >= Len) return false;
	int32 End = Pos;
	while (End < Len && Text[End] != TEXT('\n')) ++End;
	int32 Trim = End;
	while (Trim > Pos && Text[Trim - 1] == TEXT('\r')) --Trim;
	OutLine = Text.Mid(Pos, Trim - Pos);
	Pos = (End < Len) ? End + 1 : Len;
	return true;
}

/** Whitespace-driven number tokenizer over a NUL-terminated ANSI buffer (PLY ascii body). */
struct FAnsiTokenizer
{
	const ANSICHAR* Ptr;
	const ANSICHAR* End;

	FAnsiTokenizer(const ANSICHAR* InPtr, const ANSICHAR* InEnd) : Ptr(InPtr), End(InEnd) {}

	bool NextDouble(double& Out)
	{
		while (Ptr < End && (uint8)*Ptr <= (uint8)' ') ++Ptr;
		if (Ptr >= End) return false;
		char* EndPtr = nullptr;
		Out = std::strtod(Ptr, &EndPtr);
		if (EndPtr == Ptr) return false;
		Ptr = EndPtr;
		return true;
	}
};

// ---------------------------------------------------------------------------
// PLY
// ---------------------------------------------------------------------------

enum class EPlyType : uint8 { I8, U8, I16, U16, I32, U32, F32, F64, Invalid };

int32 PlyTypeSize(EPlyType T)
{
	switch (T)
	{
	case EPlyType::I8: case EPlyType::U8: return 1;
	case EPlyType::I16: case EPlyType::U16: return 2;
	case EPlyType::I32: case EPlyType::U32: case EPlyType::F32: return 4;
	case EPlyType::F64: return 8;
	default: return 0;
	}
}

EPlyType ParsePlyType(const FString& S)
{
	if (S == TEXT("char") || S == TEXT("int8")) return EPlyType::I8;
	if (S == TEXT("uchar") || S == TEXT("uint8")) return EPlyType::U8;
	if (S == TEXT("short") || S == TEXT("int16")) return EPlyType::I16;
	if (S == TEXT("ushort") || S == TEXT("uint16")) return EPlyType::U16;
	if (S == TEXT("int") || S == TEXT("int32")) return EPlyType::I32;
	if (S == TEXT("uint") || S == TEXT("uint32")) return EPlyType::U32;
	if (S == TEXT("float") || S == TEXT("float32")) return EPlyType::F32;
	if (S == TEXT("double") || S == TEXT("float64")) return EPlyType::F64;
	return EPlyType::Invalid;
}

double ReadPlyScalarLE(const uint8* P, EPlyType T)
{
	switch (T)
	{
	case EPlyType::I8: return (double)*(const int8*)P;
	case EPlyType::U8: return (double)*P;
	case EPlyType::I16: { int16 V; FMemory::Memcpy(&V, P, 2); return (double)V; }
	case EPlyType::U16: { uint16 V; FMemory::Memcpy(&V, P, 2); return (double)V; }
	case EPlyType::I32: { int32 V; FMemory::Memcpy(&V, P, 4); return (double)V; }
	case EPlyType::U32: { uint32 V; FMemory::Memcpy(&V, P, 4); return (double)V; }
	case EPlyType::F32: { float V; FMemory::Memcpy(&V, P, 4); return (double)V; }
	case EPlyType::F64: { double V; FMemory::Memcpy(&V, P, 8); return V; }
	default: return 0.0;
	}
}

struct FPlyProperty
{
	bool bList = false;
	EPlyType CountType = EPlyType::Invalid;	// list only
	EPlyType ItemType = EPlyType::Invalid;	// scalar type, or list item type
	FString Name;
	int32 TargetIdx = -1;					// 0..5 = x,y,z,nx,ny,nz for the vertex element
};

struct FPlyElement
{
	FString Name;
	int64 Count = 0;
	TArray<FPlyProperty> Props;
};

bool LoadPly(const FString& Path, TArray<FVector3f>& OutPoints, TArray<FVector3f>& OutNormals, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("PLY: cannot read file '%s'"), *Path);
		return false;
	}
	Bytes.Add(0);	// NUL terminator for the ascii tokenizer; earlier offsets unaffected
	const int64 Size = (int64)Bytes.Num() - 1;

	int64 Pos = 0;
	auto ReadHeaderLine = [&Bytes, Size, &Pos](FString& OutLine) -> bool
	{
		if (Pos >= Size) return false;
		const int64 Start = Pos;
		while (Pos < Size && Bytes[Pos] != (uint8)'\n') ++Pos;
		int64 LineEnd = Pos;
		if (Pos < Size) ++Pos;	// consume '\n'
		while (LineEnd > Start && Bytes[LineEnd - 1] == (uint8)'\r') --LineEnd;
		const FUTF8ToTCHAR Conv((const ANSICHAR*)Bytes.GetData() + Start, (int32)(LineEnd - Start));
		OutLine = FString::ConstructFromPtrSize(Conv.Get(), Conv.Length());
		return true;
	};

	FString Line;
	if (!ReadHeaderLine(Line) || Line.TrimStartAndEnd() != TEXT("ply"))
	{
		OutError = TEXT("PLY: missing 'ply' magic line");
		return false;
	}

	bool bBinary = false;
	bool bHaveFormat = false;
	bool bEndHeader = false;
	TArray<FPlyElement> Elements;
	while (ReadHeaderLine(Line))
	{
		TArray<FString> Tok;
		Line.ParseIntoArrayWS(Tok);
		if (Tok.Num() == 0) continue;
		if (Tok[0] == TEXT("comment") || Tok[0] == TEXT("obj_info")) continue;
		if (Tok[0] == TEXT("end_header")) { bEndHeader = true; break; }
		if (Tok[0] == TEXT("format"))
		{
			if (Tok.Num() < 3 || Tok[2] != TEXT("1.0"))
			{
				OutError = FString::Printf(TEXT("PLY: unsupported format line '%s'"), *Line);
				return false;
			}
			if (Tok[1] == TEXT("ascii")) bBinary = false;
			else if (Tok[1] == TEXT("binary_little_endian")) bBinary = true;
			else
			{
				OutError = FString::Printf(TEXT("PLY: unsupported encoding '%s' (only ascii / binary_little_endian)"), *Tok[1]);
				return false;
			}
			bHaveFormat = true;
			continue;
		}
		if (Tok[0] == TEXT("element"))
		{
			int64 Count = -1;
			if (Tok.Num() != 3 || !LexTryParseString(Count, *Tok[2]) || Count < 0)
			{
				OutError = FString::Printf(TEXT("PLY: bad element line '%s'"), *Line);
				return false;
			}
			FPlyElement& E = Elements.AddDefaulted_GetRef();
			E.Name = Tok[1];
			E.Count = Count;
			continue;
		}
		if (Tok[0] == TEXT("property"))
		{
			if (Elements.Num() == 0)
			{
				OutError = TEXT("PLY: 'property' before any 'element'");
				return false;
			}
			FPlyProperty P;
			if (Tok.Num() >= 2 && Tok[1] == TEXT("list"))
			{
				if (Tok.Num() != 5)
				{
					OutError = FString::Printf(TEXT("PLY: bad list property line '%s'"), *Line);
					return false;
				}
				P.bList = true;
				P.CountType = ParsePlyType(Tok[2]);
				P.ItemType = ParsePlyType(Tok[3]);
				P.Name = Tok[4];
				if (P.CountType == EPlyType::Invalid || P.ItemType == EPlyType::Invalid)
				{
					OutError = FString::Printf(TEXT("PLY: unknown property type in '%s'"), *Line);
					return false;
				}
			}
			else
			{
				if (Tok.Num() != 3)
				{
					OutError = FString::Printf(TEXT("PLY: bad property line '%s'"), *Line);
					return false;
				}
				P.ItemType = ParsePlyType(Tok[1]);
				P.Name = Tok[2];
				if (P.ItemType == EPlyType::Invalid)
				{
					OutError = FString::Printf(TEXT("PLY: unknown property type '%s'"), *Tok[1]);
					return false;
				}
			}
			Elements.Last().Props.Add(P);
			continue;
		}
		OutError = FString::Printf(TEXT("PLY: unrecognized header line '%s'"), *Line);
		return false;
	}
	if (!bEndHeader || !bHaveFormat)
	{
		OutError = TEXT("PLY: header missing 'format' or 'end_header'");
		return false;
	}

	// Locate the vertex element and its x/y/z (+optional nx/ny/nz) scalar properties.
	static const TCHAR* TargetNames[6] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("nx"), TEXT("ny"), TEXT("nz") };
	int32 VertexElem = INDEX_NONE;
	bool bTargetFound[6] = { false, false, false, false, false, false };
	for (int32 EIdx = 0; EIdx < Elements.Num(); ++EIdx)
	{
		if (Elements[EIdx].Name != TEXT("vertex")) continue;
		VertexElem = EIdx;
		for (FPlyProperty& P : Elements[EIdx].Props)
		{
			if (P.bList) continue;
			for (int32 T = 0; T < 6; ++T)
			{
				if (P.Name != TargetNames[T]) continue;
				P.TargetIdx = T;
				bTargetFound[T] = true;
			}
		}
		break;
	}
	if (VertexElem == INDEX_NONE || !bTargetFound[0] || !bTargetFound[1] || !bTargetFound[2])
	{
		OutError = TEXT("PLY: no 'vertex' element with scalar x/y/z properties");
		return false;
	}
	const bool bHasNormals = bTargetFound[3] && bTargetFound[4] && bTargetFound[5];

	const int64 NumVerts = Elements[VertexElem].Count;
	if (NumVerts > (int64)MAX_int32)
	{
		OutError = TEXT("PLY: vertex count exceeds int32 range");
		return false;
	}
	OutPoints.SetNumUninitialized((int32)NumVerts);
	if (bHasNormals) OutNormals.SetNumUninitialized((int32)NumVerts);

	// Walk all elements in file order; faces (and anything else) are decoded only to be skipped.
	if (bBinary)
	{
		for (const FPlyElement& E : Elements)
		{
			bool bAllScalar = true;
			int64 RowSize = 0;
			for (const FPlyProperty& P : E.Props)
			{
				if (P.bList) { bAllScalar = false; break; }
				RowSize += PlyTypeSize(P.ItemType);
			}
			if (bAllScalar)
			{
				const int64 Total = E.Count * RowSize;
				if (Pos + Total > Size)
				{
					OutError = FString::Printf(TEXT("PLY: unexpected end of binary data in element '%s'"), *E.Name);
					return false;
				}
				if (&E == &Elements[VertexElem])
				{
					// Fixed per-row offsets for the six target properties.
					int32 TargetOffset[6] = { -1, -1, -1, -1, -1, -1 };
					EPlyType TargetType[6] = {};
					int64 Off = 0;
					for (const FPlyProperty& P : E.Props)
					{
						if (P.TargetIdx >= 0) { TargetOffset[P.TargetIdx] = (int32)Off; TargetType[P.TargetIdx] = P.ItemType; }
						Off += PlyTypeSize(P.ItemType);
					}
					const uint8* Base = Bytes.GetData() + Pos;
					for (int64 R = 0; R < E.Count; ++R)
					{
						const uint8* RowP = Base + R * RowSize;
						FVector3f& Pt = OutPoints[(int32)R];
						Pt.X = (float)ReadPlyScalarLE(RowP + TargetOffset[0], TargetType[0]);
						Pt.Y = (float)ReadPlyScalarLE(RowP + TargetOffset[1], TargetType[1]);
						Pt.Z = (float)ReadPlyScalarLE(RowP + TargetOffset[2], TargetType[2]);
						if (bHasNormals)
						{
							FVector3f& Nm = OutNormals[(int32)R];
							Nm.X = (float)ReadPlyScalarLE(RowP + TargetOffset[3], TargetType[3]);
							Nm.Y = (float)ReadPlyScalarLE(RowP + TargetOffset[4], TargetType[4]);
							Nm.Z = (float)ReadPlyScalarLE(RowP + TargetOffset[5], TargetType[5]);
						}
					}
				}
				Pos += Total;
			}
			else
			{
				// Variable row size (list properties, e.g. face indices): decode row by row.
				for (int64 R = 0; R < E.Count; ++R)
				{
					float Row[6] = { 0, 0, 0, 0, 0, 0 };
					for (const FPlyProperty& P : E.Props)
					{
						if (P.bList)
						{
							const int32 CountSize = PlyTypeSize(P.CountType);
							if (Pos + CountSize > Size)
							{
								OutError = FString::Printf(TEXT("PLY: unexpected end of binary data in element '%s'"), *E.Name);
								return false;
							}
							const int64 ListCount = (int64)ReadPlyScalarLE(Bytes.GetData() + Pos, P.CountType);
							Pos += CountSize;
							const int64 ListBytes = ListCount * PlyTypeSize(P.ItemType);
							if (ListCount < 0 || Pos + ListBytes > Size)
							{
								OutError = FString::Printf(TEXT("PLY: bad list length in element '%s'"), *E.Name);
								return false;
							}
							Pos += ListBytes;
						}
						else
						{
							const int32 ScalarSize = PlyTypeSize(P.ItemType);
							if (Pos + ScalarSize > Size)
							{
								OutError = FString::Printf(TEXT("PLY: unexpected end of binary data in element '%s'"), *E.Name);
								return false;
							}
							if (P.TargetIdx >= 0) Row[P.TargetIdx] = (float)ReadPlyScalarLE(Bytes.GetData() + Pos, P.ItemType);
							Pos += ScalarSize;
						}
					}
					if (&E == &Elements[VertexElem])
					{
						OutPoints[(int32)R] = FVector3f(Row[0], Row[1], Row[2]);
						if (bHasNormals) OutNormals[(int32)R] = FVector3f(Row[3], Row[4], Row[5]);
					}
				}
			}
		}
	}
	else
	{
		FAnsiTokenizer Tk((const ANSICHAR*)Bytes.GetData() + Pos, (const ANSICHAR*)Bytes.GetData() + Size);
		for (const FPlyElement& E : Elements)
		{
			const bool bIsVertex = (&E == &Elements[VertexElem]);
			for (int64 R = 0; R < E.Count; ++R)
			{
				float Row[6] = { 0, 0, 0, 0, 0, 0 };
				for (const FPlyProperty& P : E.Props)
				{
					double V = 0.0;
					if (!Tk.NextDouble(V))
					{
						OutError = FString::Printf(TEXT("PLY: unexpected end of ascii data in element '%s'"), *E.Name);
						return false;
					}
					if (P.bList)
					{
						const int64 ListCount = (int64)V;
						if (ListCount < 0)
						{
							OutError = FString::Printf(TEXT("PLY: bad list length in element '%s'"), *E.Name);
							return false;
						}
						for (int64 L = 0; L < ListCount; ++L)
						{
							double Dummy;
							if (!Tk.NextDouble(Dummy))
							{
								OutError = FString::Printf(TEXT("PLY: unexpected end of ascii data in element '%s'"), *E.Name);
								return false;
							}
						}
					}
					else if (P.TargetIdx >= 0) Row[P.TargetIdx] = (float)V;
				}
				if (bIsVertex)
				{
					OutPoints[(int32)R] = FVector3f(Row[0], Row[1], Row[2]);
					if (bHasNormals) OutNormals[(int32)R] = FVector3f(Row[3], Row[4], Row[5]);
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// OBJ (point-cloud read: v / vn only, faces ignored)
// ---------------------------------------------------------------------------

bool LoadObjPoints(const FString& Path, TArray<FVector3f>& OutPoints, TArray<FVector3f>& OutNormals, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("OBJ: cannot read file '%s'"), *Path);
		return false;
	}
	TArray<float> Tok;
	TArray<FVector3f> Normals;
	int32 Pos = 0;
	int32 LineNo = 0;
	FString Line;
	while (NextLine(Text, Pos, Line))
	{
		++LineNo;
		Line.TrimStartAndEndInline();
		const bool bV = Line.StartsWith(TEXT("v "), ESearchCase::CaseSensitive) || Line.StartsWith(TEXT("v\t"), ESearchCase::CaseSensitive);
		const bool bVn = Line.StartsWith(TEXT("vn "), ESearchCase::CaseSensitive) || Line.StartsWith(TEXT("vn\t"), ESearchCase::CaseSensitive);
		if (!bV && !bVn) continue;	// faces / groups / comments / everything else ignored
		if (!SplitNumericTokens(Line.Mid(bV ? 2 : 3), Tok) || Tok.Num() < 3)
		{
			OutError = FString::Printf(TEXT("OBJ: bad %s line %d: '%s'"), bV ? TEXT("v") : TEXT("vn"), LineNo, *Line);
			return false;
		}
		if (bV) OutPoints.Add(FVector3f(Tok[0], Tok[1], Tok[2]));
		else Normals.Add(FVector3f(Tok[0], Tok[1], Tok[2]));
	}
	// vn count must match v count to be usable as per-point normals.
	if (Normals.Num() == OutPoints.Num()) OutNormals = MoveTemp(Normals);
	else if (Normals.Num() > 0) UE_LOG(LogNKSR, Warning, TEXT("OBJ '%s': %d vn lines != %d v lines, normals ignored"), *Path, Normals.Num(), OutPoints.Num());
	return true;
}

// ---------------------------------------------------------------------------
// XYZ / TXT / CSV
// ---------------------------------------------------------------------------

bool LoadXyz(const FString& Path, TArray<FVector3f>& OutPoints, TArray<FVector3f>& OutNormals, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("XYZ: cannot read file '%s'"), *Path);
		return false;
	}
	TArray<float> Tok;
	int32 Pos = 0;
	int32 LineNo = 0;
	FString Line;
	while (NextLine(Text, Pos, Line))
	{
		++LineNo;
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line[0] == TEXT('#')) continue;
		if (!SplitNumericTokens(Line, Tok) || (Tok.Num() != 3 && Tok.Num() < 6))
		{
			OutError = FString::Printf(TEXT("XYZ: line %d needs 3 or >=6 numeric values: '%s'"), LineNo, *Line);
			return false;
		}
		OutPoints.Add(FVector3f(Tok[0], Tok[1], Tok[2]));
		if (Tok.Num() >= 6) OutNormals.Add(FVector3f(Tok[3], Tok[4], Tok[5]));
	}
	// Mixed rows (some with normals, some without) leave an unusable partial normal set.
	if (OutNormals.Num() > 0 && OutNormals.Num() != OutPoints.Num())
	{
		UE_LOG(LogNKSR, Warning, TEXT("XYZ '%s': only %d of %d rows carry normals, normals ignored"), *Path, OutNormals.Num(), OutPoints.Num());
		OutNormals.Reset();
	}
	return true;
}

// ---------------------------------------------------------------------------
// NPY helpers
// ---------------------------------------------------------------------------

/** Finds Key in the header dict and extracts the quoted string value after the following ':'. */
bool NpyExtractQuoted(const FString& Header, const TCHAR* Key, FString& Out)
{
	const int32 K = Header.Find(Key, ESearchCase::CaseSensitive);
	if (K == INDEX_NONE) return false;
	const int32 C = Header.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, K);
	if (C == INDEX_NONE) return false;
	int32 I = C + 1;
	while (I < Header.Len() && FChar::IsWhitespace(Header[I])) ++I;
	if (I >= Header.Len() || (Header[I] != TEXT('\'') && Header[I] != TEXT('"'))) return false;
	const TCHAR Quote = Header[I++];
	const int32 Start = I;
	while (I < Header.Len() && Header[I] != Quote) ++I;
	if (I >= Header.Len()) return false;
	Out = Header.Mid(Start, I - Start);
	return true;
}

/** Writes a v1.0 NPY file (C-order, little-endian) with the given descr and 2-D shape. */
bool SaveNpyRaw(const FString& Path, const TCHAR* Descr, int32 Rows, int32 Cols, const void* Data, int64 DataBytes, FString& OutError)
{
	if (Rows < 0 || Cols < 0)
	{
		OutError = FString::Printf(TEXT("NPY save '%s': negative shape (%d, %d)"), *Path, Rows, Cols);
		return false;
	}
	const FString Dir = FPaths::GetPath(Path);
	if (!Dir.IsEmpty() && !IFileManager::Get().DirectoryExists(*Dir) && !IFileManager::Get().MakeDirectory(*Dir, true))
	{
		OutError = FString::Printf(TEXT("NPY save: cannot create directory '%s'"), *Dir);
		return false;
	}

	const FString Dict = FString::Printf(TEXT("{'descr': '%s', 'fortran_order': False, 'shape': (%d, %d), }"), Descr, Rows, Cols);
	// Total header (magic 6 + version 2 + len 2 + dict + padding + '\n') must be a multiple of 64.
	const int32 Prefix = 10;
	int32 HeaderLen = Dict.Len() + 1;	// + trailing '\n'
	const int32 Pad = (64 - (Prefix + HeaderLen) % 64) % 64;
	HeaderLen += Pad;
	if (HeaderLen > 65535)
	{
		OutError = FString::Printf(TEXT("NPY save '%s': header too large for v1.0"), *Path);
		return false;
	}

	const int64 TotalBytes = (int64)Prefix + HeaderLen + DataBytes;
	if (DataBytes < 0 || TotalBytes > (int64)MAX_int32)
	{
		OutError = FString::Printf(TEXT("NPY save '%s': payload of %lld bytes exceeds int32 buffer range"), *Path, TotalBytes);
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.Reserve((int32)TotalBytes);
	const uint8 Magic[8] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0 };
	Bytes.Append(Magic, 8);
	Bytes.Add((uint8)(HeaderLen & 0xFF));
	Bytes.Add((uint8)((HeaderLen >> 8) & 0xFF));
	for (int32 I = 0; I < Dict.Len(); ++I) Bytes.Add((uint8)Dict[I]);	// dict is pure ASCII
	for (int32 I = 0; I < Pad; ++I) Bytes.Add((uint8)' ');
	Bytes.Add((uint8)'\n');
	const int64 DataStart = Bytes.Num();
	Bytes.AddUninitialized((int32)DataBytes);
	if (DataBytes > 0) FMemory::Memcpy(Bytes.GetData() + DataStart, Data, DataBytes);

	if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("NPY save: cannot write file '%s'"), *Path);
		return false;
	}
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool NKSRLoadPointCloudFile(const FString& Path, TArray<FVector3f>& OutPoints, TArray<FVector3f>& OutNormals, FString& OutError)
{
	OutPoints.Reset();
	OutNormals.Reset();
	const FString Ext = FPaths::GetExtension(Path).ToLower();

	bool bOk = false;
	if (Ext == TEXT("ply")) bOk = LoadPly(Path, OutPoints, OutNormals, OutError);
	else if (Ext == TEXT("obj")) bOk = LoadObjPoints(Path, OutPoints, OutNormals, OutError);
	else if (Ext == TEXT("xyz") || Ext == TEXT("txt") || Ext == TEXT("csv")) bOk = LoadXyz(Path, OutPoints, OutNormals, OutError);
	else if (Ext == TEXT("npy"))
	{
		FNKSRMatrix M;
		if (!NKSRLoadNpyFloat(Path, M, OutError)) return false;
		if (M.Cols != 3 && M.Cols < 6)
		{
			OutError = FString::Printf(TEXT("NPY '%s': shape (%d, %d), expected (N, 3) or (N, >=6)"), *Path, M.Rows, M.Cols);
			return false;
		}
		OutPoints.SetNumUninitialized(M.Rows);
		if (M.Cols >= 6) OutNormals.SetNumUninitialized(M.Rows);
		for (int32 R = 0; R < M.Rows; ++R)
		{
			const float* Row = M.Row(R);
			OutPoints[R] = FVector3f(Row[0], Row[1], Row[2]);
			if (M.Cols >= 6) OutNormals[R] = FVector3f(Row[3], Row[4], Row[5]);
		}
		bOk = true;
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported point cloud extension '.%s' (.ply/.obj/.xyz/.txt/.csv/.npy): '%s'"), *Ext, *Path);
		return false;
	}

	if (bOk && OutPoints.Num() == 0)
	{
		OutError = FString::Printf(TEXT("'%s' contains no points"), *Path);
		return false;
	}
	return bOk;
}

bool NKSRSaveObj(const FString& Path, const FNKSRMeshBuffers& Mesh, FString& OutError)
{
	const int32 NumV = Mesh.Vertices.Num();
	for (const FIntVector& F : Mesh.Triangles)
	{
		if (F.X < 0 || F.X >= NumV || F.Y < 0 || F.Y >= NumV || F.Z < 0 || F.Z >= NumV)
		{
			OutError = FString::Printf(TEXT("OBJ save '%s': face index out of range [0, %d)"), *Path, NumV);
			return false;
		}
	}

	const FString Dir = FPaths::GetPath(Path);
	if (!Dir.IsEmpty() && !IFileManager::Get().DirectoryExists(*Dir) && !IFileManager::Get().MakeDirectory(*Dir, true))
	{
		OutError = FString::Printf(TEXT("OBJ save: cannot create directory '%s'"), *Dir);
		return false;
	}

	// ANSI string builder: no per-line FString churn on large meshes.
	TAnsiStringBuilder<4096> Sb;
	for (const FVector3f& V : Mesh.Vertices) Sb.Appendf("v %.6f %.6f %.6f\n", V.X, V.Y, V.Z);
	for (const FIntVector& F : Mesh.Triangles) Sb.Appendf("f %d %d %d\n", F.X + 1, F.Y + 1, F.Z + 1);

	if (!FFileHelper::SaveArrayToFile(TArrayView64<const uint8>((const uint8*)Sb.GetData(), Sb.Len()), *Path))
	{
		OutError = FString::Printf(TEXT("OBJ save: cannot write file '%s'"), *Path);
		return false;
	}
	return true;
}

bool NKSRLoadNpyFloat(const FString& Path, FNKSRMatrix& Out, FString& OutError)
{
	Out = FNKSRMatrix();
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("NPY: cannot read file '%s'"), *Path);
		return false;
	}
	const uint8 Magic[6] = { 0x93, 'N', 'U', 'M', 'P', 'Y' };
	if (Bytes.Num() < 10 || FMemory::Memcmp(Bytes.GetData(), Magic, 6) != 0)
	{
		OutError = FString::Printf(TEXT("NPY '%s': bad magic"), *Path);
		return false;
	}
	const uint8 Major = Bytes[6];
	int64 HeaderStart = 0;
	int64 HeaderLen = 0;
	if (Major == 1)
	{
		HeaderStart = 10;
		HeaderLen = (int64)Bytes[8] | ((int64)Bytes[9] << 8);
	}
	else if (Major == 2)
	{
		if (Bytes.Num() < 12)
		{
			OutError = FString::Printf(TEXT("NPY '%s': truncated v2.0 header"), *Path);
			return false;
		}
		HeaderStart = 12;
		HeaderLen = (int64)Bytes[8] | ((int64)Bytes[9] << 8) | ((int64)Bytes[10] << 16) | ((int64)Bytes[11] << 24);
	}
	else
	{
		OutError = FString::Printf(TEXT("NPY '%s': unsupported version %d.%d"), *Path, (int32)Major, (int32)Bytes[7]);
		return false;
	}
	if (HeaderStart + HeaderLen > (int64)Bytes.Num())
	{
		OutError = FString::Printf(TEXT("NPY '%s': truncated header"), *Path);
		return false;
	}

	FString Header;
	Header.Reserve((int32)HeaderLen);
	for (int64 I = 0; I < HeaderLen; ++I) Header.AppendChar((TCHAR)Bytes[HeaderStart + I]);	// header dict is ASCII

	// descr
	FString Descr;
	if (!NpyExtractQuoted(Header, TEXT("descr"), Descr))
	{
		OutError = FString::Printf(TEXT("NPY '%s': header missing 'descr'"), *Path);
		return false;
	}
	int32 ElemSize = 0;
	if (Descr == TEXT("<f4")) ElemSize = 4;
	else if (Descr == TEXT("<f8")) ElemSize = 8;
	else
	{
		OutError = FString::Printf(TEXT("NPY '%s': unsupported dtype '%s' (only <f4 / <f8)"), *Path, *Descr);
		return false;
	}

	// fortran_order
	const int32 FoKey = Header.Find(TEXT("fortran_order"), ESearchCase::CaseSensitive);
	if (FoKey == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("NPY '%s': header missing 'fortran_order'"), *Path);
		return false;
	}
	{
		int32 I = Header.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FoKey);
		if (I == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("NPY '%s': malformed 'fortran_order'"), *Path);
			return false;
		}
		++I;
		while (I < Header.Len() && FChar::IsWhitespace(Header[I])) ++I;
		if (Header.Mid(I, 4) == TEXT("True"))
		{
			OutError = FString::Printf(TEXT("NPY '%s': fortran_order=True is not supported, re-save the array C-contiguous"), *Path);
			return false;
		}
		if (Header.Mid(I, 5) != TEXT("False"))
		{
			OutError = FString::Printf(TEXT("NPY '%s': malformed 'fortran_order'"), *Path);
			return false;
		}
	}

	// shape
	const int32 ShapeKey = Header.Find(TEXT("shape"), ESearchCase::CaseSensitive);
	const int32 ParenL = (ShapeKey == INDEX_NONE) ? INDEX_NONE : Header.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, ShapeKey);
	const int32 ParenR = (ParenL == INDEX_NONE) ? INDEX_NONE : Header.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ParenL);
	if (ParenR == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("NPY '%s': header missing 'shape'"), *Path);
		return false;
	}
	TArray<int64> Shape;
	{
		TArray<FString> Parts;
		Header.Mid(ParenL + 1, ParenR - ParenL - 1).ParseIntoArray(Parts, TEXT(","));
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (Part.IsEmpty()) continue;	// trailing comma of 1-tuples: "(N,)"
			int64 Dim = -1;
			if (!LexTryParseString(Dim, *Part) || Dim < 0)
			{
				OutError = FString::Printf(TEXT("NPY '%s': malformed shape"), *Path);
				return false;
			}
			Shape.Add(Dim);
		}
	}
	int64 Rows = 0, Cols = 0;
	if (Shape.Num() == 1) { Rows = Shape[0]; Cols = 1; }
	else if (Shape.Num() == 2) { Rows = Shape[0]; Cols = Shape[1]; }
	else
	{
		OutError = FString::Printf(TEXT("NPY '%s': %d-D arrays not supported (need 1-D or 2-D)"), *Path, Shape.Num());
		return false;
	}
	const int64 Count = Rows * Cols;
	if (Rows > (int64)MAX_int32 || Cols > (int64)MAX_int32 || Count > (int64)MAX_int32)
	{
		OutError = FString::Printf(TEXT("NPY '%s': shape (%lld, %lld) exceeds int32 range"), *Path, Rows, Cols);
		return false;
	}

	const int64 DataStart = HeaderStart + HeaderLen;
	if (DataStart + Count * ElemSize > (int64)Bytes.Num())
	{
		OutError = FString::Printf(TEXT("NPY '%s': truncated data (need %lld bytes)"), *Path, Count * ElemSize);
		return false;
	}

	Out.SetUninitialized((int32)Rows, (int32)Cols);
	const uint8* Src = Bytes.GetData() + DataStart;
	if (ElemSize == 4)
	{
		if (Count > 0) FMemory::Memcpy(Out.Data.GetData(), Src, Count * 4);
	}
	else
	{
		for (int64 I = 0; I < Count; ++I)
		{
			double V;
			FMemory::Memcpy(&V, Src + I * 8, 8);
			Out.Data[(int32)I] = (float)V;	// input conversion, not a GC-9 computation
		}
	}
	return true;
}

bool NKSRSaveNpyFloat(const FString& Path, const FNKSRMatrix& In, FString& OutError)
{
	if (In.Data.Num() != In.Rows * In.Cols)
	{
		OutError = FString::Printf(TEXT("NPY save '%s': matrix data size %d != %d x %d"), *Path, In.Data.Num(), In.Rows, In.Cols);
		return false;
	}
	return SaveNpyRaw(Path, TEXT("<f4"), In.Rows, In.Cols, In.Data.GetData(), (int64)In.Data.Num() * 4, OutError);
}

bool NKSRSaveNpyInt64(const FString& Path, TConstArrayView<int64> Data, int32 Rows, int32 Cols, FString& OutError)
{
	if (Data.Num() != Rows * Cols)
	{
		OutError = FString::Printf(TEXT("NPY save '%s': data size %d != %d x %d"), *Path, Data.Num(), Rows, Cols);
		return false;
	}
	return SaveNpyRaw(Path, TEXT("<i8"), Rows, Cols, Data.GetData(), (int64)Data.Num() * 8, OutError);
}

bool NKSRSaveNpyInt32(const FString& Path, TConstArrayView<int32> Data, int32 Rows, int32 Cols, FString& OutError)
{
	if (Data.Num() != Rows * Cols)
	{
		OutError = FString::Printf(TEXT("NPY save '%s': data size %d != %d x %d"), *Path, Data.Num(), Rows, Cols);
		return false;
	}
	return SaveNpyRaw(Path, TEXT("<i4"), Rows, Cols, Data.GetData(), (int64)Data.Num() * 4, OutError);
}
