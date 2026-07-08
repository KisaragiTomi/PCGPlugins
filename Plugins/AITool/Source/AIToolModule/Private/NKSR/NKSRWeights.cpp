#include "NKSRWeights.h"

#include "HAL/CriticalSection.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"

namespace
{

// Sequential little-endian reader over the fully-loaded .nkw blob.
// UE runtime platforms are little-endian, so scalar reads are plain memcpy.
struct FNkwReader
{
	const uint8* Data;
	int64 Size;
	int64 Offset = 0;

	FNkwReader(const uint8* InData, int64 InSize) : Data(InData), Size(InSize) {}

	bool Read(void* Dst, int64 Num)
	{
		if (Num < 0 || Offset + Num > Size) return false;
		FMemory::Memcpy(Dst, Data + Offset, Num);
		Offset += Num;
		return true;
	}

	template <typename T>
	bool ReadScalar(T& Out) { return Read(&Out, sizeof(T)); }
};

constexpr uint32 NkwMagic = (uint32)'N' | ((uint32)'K' << 8) | ((uint32)'S' << 16) | ((uint32)'W' << 24); // bytes "NKSW"
constexpr uint32 NkwVersion = 1;
constexpr uint8 NkwDtypeF32 = 0;

} // namespace

const FNKSRWeightStore* FNKSRWeightStore::Get(FString& OutError)
{
	static FCriticalSection Cs;
	static FNKSRWeightStore Store;
	static bool bAttempted = false;
	static bool bLoaded = false;
	static FString LoadError;

	FScopeLock Lock(&Cs);
	if (!bAttempted)
	{
		bAttempted = true;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AITool"));
		if (!Plugin.IsValid())
		{
			LoadError = TEXT("FNKSRWeightStore: plugin 'AITool' not found by IPluginManager");
		}
		else
		{
			const FString Path = Plugin->GetBaseDir() / TEXT("Resources") / TEXT("nksr_ks.nkw");
			bLoaded = Store.LoadFromFile(Path, LoadError);
		}
		if (!bLoaded) UE_LOG(LogNKSR, Error, TEXT("%s"), *LoadError);
	}
	if (!bLoaded)
	{
		OutError = LoadError;
		return nullptr;
	}
	return &Store;
}

const FNKSRMatrix* FNKSRWeightStore::Find(const FString& Key) const
{
	return Tensors.Find(Key);
}

const FNKSRMatrix* FNKSRWeightStore::FindChecked(const FString& Key, TConstArrayView<int32> ExpectedDims, FString& OutError) const
{
	const FNKSRMatrix* Tensor = Tensors.Find(Key);
	const TArray<int32>* Dims = Shapes.Find(Key);
	if (!Tensor || !Dims)
	{
		OutError += FString::Printf(TEXT("FNKSRWeightStore: missing key '%s'\n"), *Key);
		return nullptr;
	}

	bool bMatch = Dims->Num() == ExpectedDims.Num();
	for (int32 D = 0; bMatch && D < Dims->Num(); ++D)
		bMatch = (*Dims)[D] == ExpectedDims[D];
	if (!bMatch)
	{
		FString Got, Want;
		for (int32 V : *Dims) Got += FString::Printf(TEXT("%d,"), V);
		for (int32 V : ExpectedDims) Want += FString::Printf(TEXT("%d,"), V);
		OutError += FString::Printf(TEXT("FNKSRWeightStore: key '%s' shape [%s] != expected [%s]\n"), *Key, *Got, *Want);
		return nullptr;
	}
	return Tensor;
}

bool FNKSRWeightStore::LoadFromFile(const FString& Path, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("FNKSRWeightStore: cannot read '%s'"), *Path);
		return false;
	}

	FNkwReader Reader(Bytes.GetData(), Bytes.Num());

	uint32 Magic = 0, Version = 0, Count = 0, Reserved = 0;
	if (!Reader.ReadScalar(Magic) || !Reader.ReadScalar(Version) || !Reader.ReadScalar(Count) || !Reader.ReadScalar(Reserved))
	{
		OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' truncated header"), *Path);
		return false;
	}
	if (Magic != NkwMagic)
	{
		OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' bad magic 0x%08X (expected 'NKSW')"), *Path, Magic);
		return false;
	}
	if (Version != NkwVersion)
	{
		OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' unsupported version %u"), *Path, Version);
		return false;
	}

	Tensors.Empty((int32)Count);
	Shapes.Empty((int32)Count);

	for (uint32 Idx = 0; Idx < Count; ++Idx)
	{
		uint16 NameLen = 0;
		if (!Reader.ReadScalar(NameLen))
		{
			OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' truncated at tensor %u (nameLen)"), *Path, Idx);
			return false;
		}
		TArray<ANSICHAR> NameUtf8;
		NameUtf8.SetNumUninitialized(NameLen);
		if (!Reader.Read(NameUtf8.GetData(), NameLen))
		{
			OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' truncated at tensor %u (name)"), *Path, Idx);
			return false;
		}
		const FUTF8ToTCHAR NameConv(NameUtf8.GetData(), NameLen);
		const FString Name(NameConv.Length(), NameConv.Get());

		uint8 Dtype = 0, Ndim = 0;
		if (!Reader.ReadScalar(Dtype) || !Reader.ReadScalar(Ndim))
		{
			OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' truncated at tensor '%s' (dtype/ndim)"), *Path, *Name);
			return false;
		}
		if (Dtype != NkwDtypeF32)
		{
			OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' tensor '%s' unsupported dtype %u"), *Path, *Name, Dtype);
			return false;
		}

		TArray<int32> Dims;
		Dims.SetNumUninitialized(Ndim);
		int64 Numel = 1;
		for (int32 D = 0; D < Ndim; ++D)
		{
			uint32 Dim = 0;
			if (!Reader.ReadScalar(Dim))
			{
				OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' truncated at tensor '%s' (dims)"), *Path, *Name);
				return false;
			}
			if (Dim > (uint32)MAX_int32)
			{
				OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' tensor '%s' dim %u too large"), *Path, *Name, Dim);
				return false;
			}
			Dims[D] = (int32)Dim;
			Numel *= (int64)Dim;
			if (Numel > MAX_int32)
			{
				OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' tensor '%s' element count overflow"), *Path, *Name);
				return false;
			}
		}

		if (Tensors.Contains(Name))
		{
			OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' duplicate key '%s'"), *Path, *Name);
			return false;
		}

		// Flatten: Rows = dim0, Cols = product of remaining dims. 1-D bias [C] -> Rows=C, Cols=1.
		FNKSRMatrix Matrix;
		Matrix.Rows = Ndim > 0 ? Dims[0] : 1;
		Matrix.Cols = (Matrix.Rows > 0) ? (int32)(Numel / Matrix.Rows) : 0;
		Matrix.Data.SetNumUninitialized((int32)Numel);
		if (!Reader.Read(Matrix.Data.GetData(), Numel * (int64)sizeof(float)))
		{
			OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' truncated at tensor '%s' (data, %lld floats)"), *Path, *Name, Numel);
			return false;
		}

		Tensors.Add(Name, MoveTemp(Matrix));
		Shapes.Add(Name, MoveTemp(Dims));
	}

	if (Reader.Offset != Reader.Size)
	{
		OutError = FString::Printf(TEXT("FNKSRWeightStore: '%s' has %lld trailing bytes"), *Path, Reader.Size - Reader.Offset);
		return false;
	}

	UE_LOG(LogNKSR, Log, TEXT("FNKSRWeightStore: loaded %u tensors from '%s' (%lld bytes)"), Count, *Path, Reader.Size);
	return true;
}
