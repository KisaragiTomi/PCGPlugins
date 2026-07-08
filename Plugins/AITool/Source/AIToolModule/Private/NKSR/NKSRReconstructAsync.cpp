// UNKSRReconstructAsync: background-thread reconstruction with game-thread delegates.
// Cancellation is polled at pipeline stage boundaries via FNKSRProgressSink::ShouldCancel.

#include "NKSRReconstructAsync.h"

#include "NKSRReconstructor.h"
#include "NKSRPointCloudIO.h"
#include "NKSRNormals.h"

#include "Async/Async.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"

namespace
{
FNKSRRunSettings ToRunSettings(const FNKSRSettings& Settings)
{
	FNKSRRunSettings Run;
	Run.DetailLevel = Settings.DetailLevel;
	Run.VoxelSizeOverride = Settings.VoxelSize;
	Run.MiseIter = Settings.MiseIter;
	Run.SolverMaxIter = Settings.SolverMaxIter;
	Run.SolverTol = Settings.SolverTol;
	Run.bVerbose = Settings.bVerboseLog;
	return Run;
}

/** Full worker: gather input -> (optional) normal estimation -> reconstruct -> (optional) OBJ save. */
void RunAsyncWork(
	bool bFromFile,
	const FString& InputPath,
	const FString& OutputPath,
	const FNKSRSettings& Settings,
	const TArray<FVector>& InPoints,
	const TArray<FVector>& InNormals,
	TFunction<bool()> ShouldCancel,
	FNKSRResult& OutResult,
	FNKSRMeshData& OutMeshData)
{
	const double StartTime = FPlatformTime::Seconds();
	auto Finish = [&](bool bSuccess, const FString& Error)
	{
		OutResult.bSuccess = bSuccess;
		OutResult.ErrorMessage = Error;
		OutResult.ElapsedSeconds = (float)(FPlatformTime::Seconds() - StartTime);
		if (!bSuccess) UE_LOG(LogNKSR, Error, TEXT("NKSR async: %s"), *Error);
	};

	TArray<FVector3f> Points;
	TArray<FVector3f> Normals;
	FString Error;

	if (bFromFile)
	{
		if (!NKSRLoadPointCloudFile(InputPath, Points, Normals, Error))
		{
			Finish(false, Error);
			return;
		}
	}
	else
	{
		Points.Reserve(InPoints.Num());
		for (const FVector& P : InPoints) Points.Emplace((float)P.X, (float)P.Y, (float)P.Z);
		Normals.Reserve(InNormals.Num());
		for (const FVector& N : InNormals) Normals.Emplace((float)N.X, (float)N.Y, (float)N.Z);
	}

	if (Points.Num() == 0)
	{
		Finish(false, TEXT("Empty input point cloud."));
		return;
	}
	if (Normals.Num() != Points.Num())
	{
		if (Normals.Num() != 0)
		{
			Finish(false, FString::Printf(TEXT("Normals (%d) do not match points (%d)."), Normals.Num(), Points.Num()));
			return;
		}
		if (!Settings.bEstimateNormalsIfMissing)
		{
			Finish(false, TEXT("Input has no normals and bEstimateNormalsIfMissing is disabled."));
			return;
		}
		if (!NKSREstimateNormals(Points, Settings.NormalKnn, Normals, Error))
		{
			Finish(false, Error);
			return;
		}
	}

	FNKSRProgressSink Sink;
	Sink.ShouldCancel = MoveTemp(ShouldCancel);

	FNKSRMeshBuffers Mesh;
	if (!NKSRReconstruct(Points, Normals, ToRunSettings(Settings), Mesh, Error, &Sink))
	{
		Finish(false, Error);
		return;
	}

	if (bFromFile)
	{
		FString OutPath = OutputPath;
		if (OutPath.IsEmpty()) OutPath = FPaths::ChangeExtension(InputPath, FString()) + TEXT("_nksr.obj");
		if (!NKSRSaveObj(OutPath, Mesh, Error))
		{
			Finish(false, Error);
			return;
		}
		OutResult.OutputFilePath = OutPath;
	}

	OutResult.VertexCount = Mesh.Vertices.Num();
	OutResult.FaceCount = Mesh.Triangles.Num();
	OutMeshData.Vertices.Reserve(Mesh.Vertices.Num());
	for (const FVector3f& V : Mesh.Vertices) OutMeshData.Vertices.Emplace(V);
	OutMeshData.Triangles.Reserve(Mesh.Triangles.Num() * 3);
	for (const FIntVector& T : Mesh.Triangles)
	{
		OutMeshData.Triangles.Add(T.X);
		OutMeshData.Triangles.Add(T.Y);
		OutMeshData.Triangles.Add(T.Z);
	}
	Finish(true, FString());
}
}

UNKSRReconstructAsync* UNKSRReconstructAsync::ReconstructFileAsync(const FString& InputFilePath, const FString& OutputFilePath, FNKSRSettings Settings)
{
	UNKSRReconstructAsync* Node = NewObject<UNKSRReconstructAsync>();
	Node->InputPath = InputFilePath;
	Node->OutputPath = OutputFilePath;
	Node->RunSettings = Settings;
	Node->bFromFile = true;
	return Node;
}

UNKSRReconstructAsync* UNKSRReconstructAsync::ReconstructPointsAsync(const TArray<FVector>& Points, const TArray<FVector>& Normals, FNKSRSettings Settings)
{
	UNKSRReconstructAsync* Node = NewObject<UNKSRReconstructAsync>();
	Node->InPoints = Points;
	Node->InNormals = Normals;
	Node->RunSettings = Settings;
	Node->bFromFile = false;
	return Node;
}

void UNKSRReconstructAsync::Cancel()
{
	bCancelRequested = true;
}

void UNKSRReconstructAsync::Activate()
{
	// Not registered with a game instance (no world context in the factories) — root the node
	// for the duration of the background task instead; released on the game thread after broadcast.
	AddToRoot();

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]
	{
		FNKSRResult Result;
		FNKSRMeshData MeshData;
		RunAsyncWork(bFromFile, InputPath, OutputPath, RunSettings, InPoints, InNormals,
			[this] { return (bool)bCancelRequested; },
			Result, MeshData);

		AsyncTask(ENamedThreads::GameThread, [this, Result = MoveTemp(Result), MeshData = MoveTemp(MeshData)]
		{
			if (Result.bSuccess) OnCompleted.Broadcast(Result, MeshData);
			else OnFailed.Broadcast(Result, MeshData);
			RemoveFromRoot();
			SetReadyToDestroy();
		});
	});
}
