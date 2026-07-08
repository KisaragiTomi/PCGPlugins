#pragma once

// Headless entry for validation & batch use:
//   UnrealEditor-Cmd.exe <proj> -run=NKSR -Input=<file> [-Output=<obj>] [-DetailLevel=1.0]
//                        [-VoxelSize=0] [-MiseIter=1] [-DumpDir=<dir>] [-SelfTest]
// -DumpDir writes per-stage .npy dumps (golden comparison, Tools/compare_golden.py).
// -SelfTest runs the built-in synthetic sphere end-to-end check and returns non-zero on failure.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "NKSRCommandlet.generated.h"

UCLASS()
class UNKSRCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
