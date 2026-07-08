// Blueprint API over the pure C++ NKSR port. No Python, no external processes, no machine paths.

#include "NKSRReconstruct.h"

#include "NKSR/NKSRCommon.h"
#include "NKSR/NKSRReconstructor.h"
#include "NKSR/NKSRPointCloudIO.h"
#include "NKSR/NKSRNormals.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "Interfaces/IPluginManager.h"
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

/** Ensures Normals matches Points 1:1, estimating them when allowed. */
bool EnsureNormals(TConstArrayView<FVector3f> Points, TArray<FVector3f>& Normals, const FNKSRSettings& Settings, FString& OutError)
{
	if (Normals.Num() == Points.Num() && Points.Num() > 0) return true;
	if (Normals.Num() != 0)
	{
		OutError = FString::Printf(TEXT("Normals (%d) do not match points (%d)."), Normals.Num(), Points.Num());
		return false;
	}
	if (!Settings.bEstimateNormalsIfMissing)
	{
		OutError = TEXT("Input has no normals and bEstimateNormalsIfMissing is disabled.");
		return false;
	}
	return NKSREstimateNormals(Points, Settings.NormalKnn, Normals, OutError);
}

FString ResolveOutputPath(const FString& InputFilePath, const FString& OutputFilePath)
{
	if (!OutputFilePath.IsEmpty()) return OutputFilePath;
	return FPaths::ChangeExtension(InputFilePath, FString()) + TEXT("_nksr.obj");
}
}

FNKSRResult UNKSRReconstructLibrary::RunNKSRReconstruction(
	const FString& InputFilePath,
	const FString& OutputFilePath,
	const FNKSRSettings& Settings)
{
	FNKSRResult Result;
	const double StartTime = FPlatformTime::Seconds();
	auto Fail = [&](const FString& Error) -> FNKSRResult
	{
		Result.bSuccess = false;
		Result.ErrorMessage = Error;
		Result.ElapsedSeconds = (float)(FPlatformTime::Seconds() - StartTime);
		UE_LOG(LogNKSR, Error, TEXT("RunNKSRReconstruction: %s"), *Error);
		return Result;
	};

	TArray<FVector3f> Points;
	TArray<FVector3f> Normals;
	FString Error;
	if (!NKSRLoadPointCloudFile(InputFilePath, Points, Normals, Error)) return Fail(Error);
	if (!EnsureNormals(Points, Normals, Settings, Error)) return Fail(Error);

	FNKSRMeshBuffers Mesh;
	if (!NKSRReconstruct(Points, Normals, ToRunSettings(Settings), Mesh, Error)) return Fail(Error);

	const FString OutPath = ResolveOutputPath(InputFilePath, OutputFilePath);
	if (!NKSRSaveObj(OutPath, Mesh, Error)) return Fail(Error);

	Result.bSuccess = true;
	Result.OutputFilePath = OutPath;
	Result.VertexCount = Mesh.Vertices.Num();
	Result.FaceCount = Mesh.Triangles.Num();
	Result.ElapsedSeconds = (float)(FPlatformTime::Seconds() - StartTime);
	UE_LOG(LogNKSR, Log, TEXT("RunNKSRReconstruction: %d verts, %d faces in %.2f s -> %s"),
		Result.VertexCount, Result.FaceCount, Result.ElapsedSeconds, *Result.OutputFilePath);
	return Result;
}

bool UNKSRReconstructLibrary::ReconstructPointCloud(
	const TArray<FVector>& Points,
	const TArray<FVector>& Normals,
	const FNKSRSettings& Settings,
	FNKSRMeshData& OutMesh,
	FString& OutError)
{
	OutMesh.Vertices.Reset();
	OutMesh.Triangles.Reset();

	TArray<FVector3f> Pts;
	Pts.Reserve(Points.Num());
	for (const FVector& P : Points) Pts.Emplace((float)P.X, (float)P.Y, (float)P.Z);

	TArray<FVector3f> Nrm;
	Nrm.Reserve(Normals.Num());
	for (const FVector& N : Normals) Nrm.Emplace((float)N.X, (float)N.Y, (float)N.Z);

	if (Pts.Num() == 0)
	{
		OutError = TEXT("ReconstructPointCloud: empty input point array.");
		UE_LOG(LogNKSR, Error, TEXT("%s"), *OutError);
		return false;
	}
	if (!EnsureNormals(Pts, Nrm, Settings, OutError))
	{
		UE_LOG(LogNKSR, Error, TEXT("ReconstructPointCloud: %s"), *OutError);
		return false;
	}

	FNKSRMeshBuffers Mesh;
	if (!NKSRReconstruct(Pts, Nrm, ToRunSettings(Settings), Mesh, OutError))
	{
		UE_LOG(LogNKSR, Error, TEXT("ReconstructPointCloud: %s"), *OutError);
		return false;
	}

	OutMesh.Vertices.Reserve(Mesh.Vertices.Num());
	for (const FVector3f& V : Mesh.Vertices) OutMesh.Vertices.Emplace(V);
	OutMesh.Triangles.Reserve(Mesh.Triangles.Num() * 3);
	for (const FIntVector& T : Mesh.Triangles)
	{
		OutMesh.Triangles.Add(T.X);
		OutMesh.Triangles.Add(T.Y);
		OutMesh.Triangles.Add(T.Z);
	}
	return true;
}

bool UNKSRReconstructLibrary::MeshDataToDynamicMesh(const FNKSRMeshData& MeshData, UDynamicMesh* TargetMesh)
{
	if (!TargetMesh)
	{
		UE_LOG(LogNKSR, Error, TEXT("MeshDataToDynamicMesh: TargetMesh is null."));
		return false;
	}
	if (MeshData.Triangles.Num() % 3 != 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("MeshDataToDynamicMesh: triangle index count (%d) is not a multiple of 3."), MeshData.Triangles.Num());
		return false;
	}

	UE::Geometry::FDynamicMesh3 DynMesh;
	for (const FVector& V : MeshData.Vertices) DynMesh.AppendVertex(V);

	const int32 NumVerts = MeshData.Vertices.Num();
	int32 NumSkipped = 0;
	for (int32 I = 0; I + 2 < MeshData.Triangles.Num(); I += 3)
	{
		const int32 A = MeshData.Triangles[I];
		const int32 B = MeshData.Triangles[I + 1];
		const int32 C = MeshData.Triangles[I + 2];
		if (A < 0 || B < 0 || C < 0 || A >= NumVerts || B >= NumVerts || C >= NumVerts)
		{
			++NumSkipped;
			continue;
		}
		if (DynMesh.AppendTriangle(A, B, C) < 0) ++NumSkipped;   // InvalidID / NonManifoldID / duplicate
	}
	if (NumSkipped > 0) UE_LOG(LogNKSR, Warning, TEXT("MeshDataToDynamicMesh: skipped %d invalid/non-manifold triangles."), NumSkipped);

	TargetMesh->SetMesh(MoveTemp(DynMesh));
	return true;
}

FString UNKSRReconstructLibrary::GetAIToolPluginDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AITool"));
	if (Plugin.IsValid()) return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("AITool")));
}
