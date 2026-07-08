// Headless NKSR entry point:
//   UnrealEditor-Cmd.exe <proj> -run=NKSR -Input=<file> [-Output=<obj>] [-DetailLevel=1.0]
//                        [-VoxelSize=0] [-MiseIter=1] [-DumpDir=<dir>] [-SelfTest]
// -DumpDir emits per-stage .npy dumps for Tools/compare_golden.py.
// -SelfTest reconstructs a deterministic fibonacci sphere (8192 pts, radius 50, analytic
// normals, no randomness) and asserts mean |r - 50| < 2.5 (5% of the radius). Returns 0/1.

#include "NKSRCommandlet.h"

#include "NKSRCommon.h"
#include "NKSRReconstructor.h"
#include "NKSRPointCloudIO.h"
#include "NKSRNormals.h"

#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"

namespace
{
/** Deterministic fibonacci sphere: golden-angle spiral, analytic outward normals, no RNG. */
void BuildSelfTestSphere(int32 NumPoints, float Radius, TArray<FVector3f>& OutPoints, TArray<FVector3f>& OutNormals)
{
	OutPoints.Reset(NumPoints);
	OutNormals.Reset(NumPoints);
	const double GoldenAngle = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
	for (int32 I = 0; I < NumPoints; ++I)
	{
		const double Y = 1.0 - 2.0 * ((double)I + 0.5) / (double)NumPoints;
		const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Y * Y));
		const double Theta = GoldenAngle * (double)I;
		const FVector3f N((float)(R * FMath::Cos(Theta)), (float)Y, (float)(R * FMath::Sin(Theta)));
		OutNormals.Add(N);
		OutPoints.Add(N * Radius);
	}
}
}

int32 UNKSRCommandlet::Main(const FString& Params)
{
	FNKSRRunSettings Run;
	Run.bVerbose = true;
	FParse::Value(*Params, TEXT("DetailLevel="), Run.DetailLevel);
	FParse::Value(*Params, TEXT("VoxelSize="), Run.VoxelSizeOverride);
	FParse::Value(*Params, TEXT("MiseIter="), Run.MiseIter);

	FString InputPath, OutputPath, DumpDir;
	FParse::Value(*Params, TEXT("Input="), InputPath);
	FParse::Value(*Params, TEXT("Output="), OutputPath);
	FParse::Value(*Params, TEXT("DumpDir="), DumpDir);
	const bool bSelfTest = FParse::Param(*Params, TEXT("SelfTest"));

	FNKSRProgressSink Sink;
	Sink.DumpDir = DumpDir;
	Sink.OnStage = [](const TCHAR* StageName) { UE_LOG(LogNKSR, Display, TEXT("NKSR stage: %s"), StageName); };

	constexpr float SphereRadius = 50.f;
	TArray<FVector3f> Points;
	TArray<FVector3f> Normals;
	FString Error;

	if (bSelfTest)
	{
		// Density-heuristic scaling is meaningless for a synthetic sphere whose point spacing
		// (~1.96 units) far exceeds the network voxel (0.1): force a working voxel near the
		// spacing unless the caller overrides it, so the splatting grids stay connected.
		if (Run.VoxelSizeOverride <= 0.f) Run.VoxelSizeOverride = 2.f;
		BuildSelfTestSphere(8192, SphereRadius, Points, Normals);
		UE_LOG(LogNKSR, Display, TEXT("NKSR self-test: fibonacci sphere, %d points, radius %.1f, voxel size %.2f."),
			Points.Num(), SphereRadius, Run.VoxelSizeOverride);
	}
	else
	{
		if (InputPath.IsEmpty())
		{
			UE_LOG(LogNKSR, Error, TEXT("Usage: -run=NKSR -Input=<file> [-Output=<obj>] [-DetailLevel=1.0] [-VoxelSize=0] [-MiseIter=1] [-DumpDir=<dir>] | -run=NKSR -SelfTest"));
			return 1;
		}
		if (!NKSRLoadPointCloudFile(InputPath, Points, Normals, Error))
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSR: failed to load '%s': %s"), *InputPath, *Error);
			return 1;
		}
		if (Normals.Num() == 0)
		{
			UE_LOG(LogNKSR, Display, TEXT("NKSR: input has no normals, estimating (k=30)."));
			if (!NKSREstimateNormals(Points, 30, Normals, Error))
			{
				UE_LOG(LogNKSR, Error, TEXT("NKSR: normal estimation failed: %s"), *Error);
				return 1;
			}
		}
	}

	const double StartTime = FPlatformTime::Seconds();
	FNKSRMeshBuffers Mesh;
	if (!NKSRReconstruct(Points, Normals, Run, Mesh, Error, &Sink))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSR: reconstruction failed: %s"), *Error);
		return 1;
	}
	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogNKSR, Display, TEXT("NKSR: reconstructed %d vertices / %d faces in %.2f s."), Mesh.Vertices.Num(), Mesh.Triangles.Num(), Elapsed);

	// Optional OBJ output (default path only when a file input names one).
	FString OutPath = OutputPath;
	if (OutPath.IsEmpty() && !InputPath.IsEmpty()) OutPath = FPaths::ChangeExtension(InputPath, FString()) + TEXT("_nksr.obj");
	if (!OutPath.IsEmpty())
	{
		if (!NKSRSaveObj(OutPath, Mesh, Error))
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSR: failed to write '%s': %s"), *OutPath, *Error);
			return 1;
		}
		UE_LOG(LogNKSR, Display, TEXT("NKSR: wrote %s"), *OutPath);
	}

	if (bSelfTest)
	{
		if (Mesh.Vertices.Num() == 0)
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSR self-test: FAIL (empty mesh)."));
			return 1;
		}
		double SumDev = 0.0;
		double MaxDev = 0.0;
		for (const FVector3f& V : Mesh.Vertices)
		{
			const double Dev = FMath::Abs((double)V.Length() - (double)SphereRadius);
			SumDev += Dev;
			MaxDev = FMath::Max(MaxDev, Dev);
		}
		const double MeanDev = SumDev / (double)Mesh.Vertices.Num();
		const bool bPass = MeanDev < 2.5;   // 5% of radius 50
		UE_LOG(LogNKSR, Display, TEXT("NKSR self-test: |r - %.0f| mean=%.4f max=%.4f over %d vertices (pass threshold: mean < 2.5)."),
			SphereRadius, MeanDev, MaxDev, Mesh.Vertices.Num());
		UE_LOG(LogNKSR, Display, TEXT("NKSR self-test: %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		return bPass ? 0 : 1;
	}

	return 0;
}
