#include "StaticMeshRenderDataPointSampler.h"

#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "RawIndexBuffer.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"

class FStaticMeshPointSamplerCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FStaticMeshPointSamplerCS);
	SHADER_USE_PARAMETER_STRUCT(FStaticMeshPointSamplerCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<uint>, IndexBuffer)
		SHADER_PARAMETER_SRV(Buffer<float>, PositionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutPoints)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, SampleCount)
		SHADER_PARAMETER(uint32, TriangleStep)
		SHADER_PARAMETER(uint32, PositionStrideFloat)
		SHADER_PARAMETER(uint32, OutputOffset)
		SHADER_PARAMETER(uint32, SourceRequestIndex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FStaticMeshPointSamplerCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "SampleStaticMeshPointsCS", SF_Compute);

class FStaticMeshPointCompactCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FStaticMeshPointCompactCS);
	SHADER_USE_PARAMETER_STRUCT(FStaticMeshPointCompactCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InPoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_CompactPoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VoxelSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_RequestCounts)
		SHADER_PARAMETER(FVector3f, HashOrigin)
		SHADER_PARAMETER(float, VoxelCellSize)
		SHADER_PARAMETER(uint32, RawPointCount)
		SHADER_PARAMETER(uint32, CompactCapacity)
		SHADER_PARAMETER(uint32, HashSlotCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FStaticMeshPointCompactCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "CompactStaticMeshPointsCS", SF_Compute);

namespace
{
struct FResolvedStaticMeshPointSampleRequest
{
	TRefCountPtr<const FStaticMeshLODResources> LODResource;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FBox3f WorldBounds = FBox3f(EForceInit::ForceInit);
	int32 SourceRequestIndex = INDEX_NONE;
	int32 MaxPoints = 0;
	int32 TriangleCount = 0;
	int32 PositionStrideFloat = 3;
	float VoxelCellSize = 1.0f;
};

bool ResolveRequest(const FStaticMeshRenderDataPointSampleRequest& Request, int32 SourceRequestIndex, FResolvedStaticMeshPointSampleRequest& OutResolved)
{
	if (!Request.StaticMesh || Request.MaxPoints <= 0 || !Request.WorldBounds.IsValid)
	{
		return false;
	}

	FStaticMeshRenderData* RenderData = Request.StaticMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return false;
	}

	const int32 LODIndex = FMath::Clamp(Request.LODIndex, 0, RenderData->LODResources.Num() - 1);
	const int32 CurrentFirstLOD = RenderData->GetCurrentFirstLODIdx(Request.StaticMesh->GetMinLODIdx());
	if (LODIndex < CurrentFirstLOD || !RenderData->LODResources.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FStaticMeshLODResources* LODResource = &RenderData->LODResources[LODIndex];
	if (!LODResource || LODResource->GetNumTriangles() <= 0 || LODResource->GetNumVertices() <= 0 || LODResource->BuffersSize <= 0)
	{
		return false;
	}

	if (!LODResource->VertexBuffers.PositionVertexBuffer.GetSRV())
	{
		return false;
	}

	const FBufferRHIRef& IndexBufferRHI = LODResource->IndexBuffer.GetRHI();
	if (!IndexBufferRHI.IsValid())
	{
		return false;
	}

	OutResolved.LODResource = LODResource;
	OutResolved.LocalToWorld = FMatrix44f(Request.LocalToWorld.ToMatrixWithScale());
	OutResolved.WorldBounds = FBox3f(FVector3f(Request.WorldBounds.Min), FVector3f(Request.WorldBounds.Max));
	OutResolved.SourceRequestIndex = SourceRequestIndex;
	OutResolved.MaxPoints = Request.MaxPoints;
	OutResolved.TriangleCount = LODResource->GetNumTriangles();
	OutResolved.PositionStrideFloat = FMath::Max(3, int32(LODResource->VertexBuffers.PositionVertexBuffer.GetStride() / sizeof(float)));
	OutResolved.VoxelCellSize = FMath::Max(Request.VoxelCellSize, 1.0f);
	return true;
}

FShaderResourceViewRHIRef CreateIndexBufferSRV(FRHICommandListImmediate& RHICmdList, const FStaticMeshLODResources* LODResource)
{
	if (!LODResource)
	{
		return nullptr;
	}

	const FBufferRHIRef& IndexBufferRHI = LODResource->IndexBuffer.GetRHI();
	if (!IndexBufferRHI.IsValid())
	{
		return nullptr;
	}

	return RHICmdList.CreateShaderResourceView(
		IndexBufferRHI,
		FRHIViewDesc::CreateBufferSRV()
			.SetType(FRHIViewDesc::EBufferType::Typed)
			.SetFormat(LODResource->IndexBuffer.Is32Bit() ? PF_R32_UINT : PF_R16_UINT));
}

int32 AddReadbackPoints(const FVector4f* Data, int32 Count, TArray<FVector>& OutPoints)
{
	if (!Data || Count <= 0)
	{
		return 0;
	}

	const int32 StartCount = OutPoints.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector4f& Point = Data[Index];
		if (Point.W > 0.0f)
		{
			OutPoints.Add(FVector(Point.X, Point.Y, Point.Z));
		}
	}
	return OutPoints.Num() - StartCount;
}

uint32 GetPointHashSlotCount(int32 PointCount)
{
	const uint32 TargetSlots = FMath::Max(1024u, uint32(PointCount) * 2u);
	uint32 SlotCount = 1u;
	while (SlotCount < TargetSlots && SlotCount < (1u << 30))
	{
		SlotCount <<= 1u;
	}
	return SlotCount;
}
}

bool FStaticMeshRenderDataPointSampler::SamplePointsSync(const TArray<FStaticMeshRenderDataPointSampleRequest>& Requests, TArray<FVector>& OutPoints)
{
	TArray<int32> PointsPerRequest;
	return SamplePointsSync(Requests, OutPoints, PointsPerRequest);
}

bool FStaticMeshRenderDataPointSampler::SamplePointsSync(const TArray<FStaticMeshRenderDataPointSampleRequest>& Requests, TArray<FVector>& OutPoints, TArray<int32>& OutPointsPerRequest)
{
	TArray<FResolvedStaticMeshPointSampleRequest> ResolvedRequests;
	ResolvedRequests.Reserve(Requests.Num());
	int32 TotalSampleCount = 0;
	FBox3f CombinedWorldBounds(EForceInit::ForceInit);
	float CompactCellSize = TNumericLimits<float>::Max();
	OutPointsPerRequest.SetNumZeroed(Requests.Num());

	for (int32 RequestIndex = 0; RequestIndex < Requests.Num(); ++RequestIndex)
	{
		FResolvedStaticMeshPointSampleRequest ResolvedRequest;
		if (!ResolveRequest(Requests[RequestIndex], RequestIndex, ResolvedRequest))
		{
			continue;
		}

		const int32 SampleCount = FMath::Clamp(ResolvedRequest.MaxPoints, 1, ResolvedRequest.TriangleCount);
		TotalSampleCount += SampleCount;
		CombinedWorldBounds += ResolvedRequest.WorldBounds;
		CompactCellSize = FMath::Min(CompactCellSize, ResolvedRequest.VoxelCellSize);
		ResolvedRequests.Add(MoveTemp(ResolvedRequest));
	}

	if (ResolvedRequests.IsEmpty() || TotalSampleCount <= 0)
	{
		return false;
	}

	CompactCellSize = CompactCellSize == TNumericLimits<float>::Max() ? 1.0f : FMath::Max(CompactCellSize, 1.0f);
	const FVector3f HashOrigin = CombinedWorldBounds.IsValid ? CombinedWorldBounds.Min : FVector3f::ZeroVector;
	const int32 CompactCapacity = TotalSampleCount;
	const int32 RequestCountBufferNum = Requests.Num() + 1;
	const uint32 HashSlotCount = GetPointHashSlotCount(TotalSampleCount);
	const uint64 PointReadbackBytes64 = sizeof(FVector4f) * uint64(CompactCapacity);
	const uint64 RequestCountReadbackBytes64 = sizeof(uint32) * uint64(RequestCountBufferNum);
	if (PointReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()) ||
		RequestCountReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[StaticMeshPointSampler] Readback request too large. Points=%d Requests=%d"), CompactCapacity, RequestCountBufferNum);
		return false;
	}
	const uint32 PointReadbackBytes = uint32(PointReadbackBytes64);
	const uint32 RequestCountReadbackBytes = uint32(RequestCountReadbackBytes64);
	FRHIGPUBufferReadback* PointReadback = new FRHIGPUBufferReadback(TEXT("StaticMeshPointSamplerPointReadback"));
	FRHIGPUBufferReadback* CountReadback = new FRHIGPUBufferReadback(TEXT("StaticMeshPointSamplerCountReadback"));
	bool bRenderWorkQueued = false;

	ENQUEUE_RENDER_COMMAND(StaticMeshPointSampler)(
		[ResolvedRequests = MoveTemp(ResolvedRequests), PointReadback, CountReadback, PointReadbackBytes, RequestCountReadbackBytes,
		 TotalSampleCount, CompactCapacity, RequestCountBufferNum, HashSlotCount, HashOrigin, CompactCellSize,
		 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RawPointsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), TotalSampleCount),
				TEXT("StaticMeshPointSampler_RawPoints"));
			FRDGBufferUAVRef RawPointsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RawPointsBuffer, PF_A32B32G32R32F));
			AddClearUAVPass(GraphBuilder, RawPointsUAV, 0.0f);

			TShaderMapRef<FStaticMeshPointSamplerCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			TArray<FShaderResourceViewRHIRef> IndexBufferSRVs;
			IndexBufferSRVs.Reserve(ResolvedRequests.Num());
			int32 OutputOffset = 0;
			for (const FResolvedStaticMeshPointSampleRequest& Request : ResolvedRequests)
			{
				const int32 SampleCount = FMath::Clamp(Request.MaxPoints, 1, Request.TriangleCount);
				FShaderResourceViewRHIRef IndexBufferSRV = CreateIndexBufferSRV(RHICmdList, Request.LODResource.GetReference());
				FRHIShaderResourceView* PositionBufferSRV = Request.LODResource->VertexBuffers.PositionVertexBuffer.GetSRV();
				if (!IndexBufferSRV.IsValid() || !PositionBufferSRV)
				{
					OutputOffset += SampleCount;
					continue;
				}
				IndexBufferSRVs.Add(IndexBufferSRV);

				FStaticMeshPointSamplerCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStaticMeshPointSamplerCS::FParameters>();
				PassParameters->IndexBuffer = IndexBufferSRVs.Last().GetReference();
				PassParameters->PositionBuffer = PositionBufferSRV;
				PassParameters->RW_OutPoints = RawPointsUAV;
				PassParameters->LocalToWorld = Request.LocalToWorld;
				PassParameters->BoundsMin = Request.WorldBounds.Min;
				PassParameters->BoundsMax = Request.WorldBounds.Max;
				PassParameters->TriangleCount = uint32(Request.TriangleCount);
				PassParameters->SampleCount = uint32(SampleCount);
				PassParameters->TriangleStep = uint32(FMath::Max(1, Request.TriangleCount / SampleCount));
				PassParameters->PositionStrideFloat = uint32(Request.PositionStrideFloat);
				PassParameters->OutputOffset = uint32(OutputOffset);
				PassParameters->SourceRequestIndex = uint32(Request.SourceRequestIndex);

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("StaticMeshPointSampler"),
					PassParameters,
					ERDGPassFlags::Compute,
					[PassParameters, ComputeShader, SampleCount](FRHIComputeCommandList& InRHICmdList)
					{
						FComputeShaderUtils::Dispatch(InRHICmdList, ComputeShader, *PassParameters, FComputeShaderUtils::GetGroupCount(FIntVector(SampleCount, 1, 1), 64));
					});

				OutputOffset += SampleCount;
			}

			FRDGBufferRef CompactPointsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), CompactCapacity),
				TEXT("StaticMeshPointSampler_CompactPoints"));
			FRDGBufferUAVRef CompactPointsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CompactPointsBuffer, PF_A32B32G32R32F));
			AddClearUAVPass(GraphBuilder, CompactPointsUAV, 0.0f);

			FRDGBufferRef VoxelSlotsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), HashSlotCount),
				TEXT("StaticMeshPointSampler_VoxelSlots"));
			FRDGBufferUAVRef VoxelSlotsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VoxelSlotsBuffer, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, VoxelSlotsUAV, 0u);

			FRDGBufferRef RequestCountsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), RequestCountBufferNum),
				TEXT("StaticMeshPointSampler_RequestCounts"));
			FRDGBufferUAVRef RequestCountsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RequestCountsBuffer, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, RequestCountsUAV, 0u);

			TShaderMapRef<FStaticMeshPointCompactCS> CompactShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FStaticMeshPointCompactCS::FParameters* CompactParameters = GraphBuilder.AllocParameters<FStaticMeshPointCompactCS::FParameters>();
			CompactParameters->InPoints = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RawPointsBuffer, PF_A32B32G32R32F));
			CompactParameters->RW_CompactPoints = CompactPointsUAV;
			CompactParameters->RW_VoxelSlots = VoxelSlotsUAV;
			CompactParameters->RW_RequestCounts = RequestCountsUAV;
			CompactParameters->HashOrigin = HashOrigin;
			CompactParameters->VoxelCellSize = CompactCellSize;
			CompactParameters->RawPointCount = uint32(TotalSampleCount);
			CompactParameters->CompactCapacity = uint32(CompactCapacity);
			CompactParameters->HashSlotCount = HashSlotCount;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("StaticMeshPointCompact"),
				CompactParameters,
				ERDGPassFlags::Compute,
				[CompactParameters, CompactShader, TotalSampleCount](FRHIComputeCommandList& InRHICmdList)
				{
					FComputeShaderUtils::Dispatch(InRHICmdList, CompactShader, *CompactParameters, FComputeShaderUtils::GetGroupCount(FIntVector(TotalSampleCount, 1, 1), 64));
				});

			AddEnqueueCopyPass(GraphBuilder, PointReadback, CompactPointsBuffer, PointReadbackBytes);
			AddEnqueueCopyPass(GraphBuilder, CountReadback, RequestCountsBuffer, RequestCountReadbackBytes);
			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	FlushRenderingCommands();

	if (!bRenderWorkQueued)
	{
		delete PointReadback;
		delete CountReadback;
		return false;
	}

	TArray<uint32> RequestCounts;
	RequestCounts.SetNumZeroed(RequestCountBufferNum);
	TArray<FVector4f> CompactPointData;
	CompactPointData.SetNumZeroed(CompactCapacity);
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(StaticMeshPointSamplerReadback)(
		[PointReadback, CountReadback, PointReadbackBytes, RequestCountReadbackBytes,
		 RequestCountBufferNum, CompactCapacity, &RequestCounts, &CompactPointData, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			if (!PointReadback || !CountReadback)
			{
				return;
			}

			if (!PointReadback->IsReady() || !CountReadback->IsReady())
			{
				RHICmdList.SubmitAndBlockUntilGPUIdle();
			}

			if (!PointReadback->IsReady() || !CountReadback->IsReady())
			{
				UE_LOG(LogTemp, Warning, TEXT("[StaticMeshPointSampler] GPU readback was not ready after flush."));
				delete PointReadback;
				delete CountReadback;
				return;
			}

			if (PointReadback->GetGPUSizeBytes() < PointReadbackBytes ||
				CountReadback->GetGPUSizeBytes() < RequestCountReadbackBytes)
			{
				UE_LOG(LogTemp, Warning, TEXT("[StaticMeshPointSampler] GPU readback size mismatch. Point=%llu/%u Count=%llu/%u"),
					PointReadback->GetGPUSizeBytes(),
					PointReadbackBytes,
					CountReadback->GetGPUSizeBytes(),
					RequestCountReadbackBytes);
				delete PointReadback;
				delete CountReadback;
				return;
			}

			bool bCountLocked = false;
			if (const uint32* CountPtr = static_cast<const uint32*>(CountReadback->Lock(RequestCountReadbackBytes)))
			{
				FMemory::Memcpy(RequestCounts.GetData(), CountPtr, RequestCountReadbackBytes);
				CountReadback->Unlock();
				bCountLocked = true;
			}
			if (!bCountLocked)
			{
				UE_LOG(LogTemp, Warning, TEXT("[StaticMeshPointSampler] Failed to lock count readback."));
				delete PointReadback;
				delete CountReadback;
				return;
			}

			bool bPointLocked = false;
			if (const FVector4f* ResultPtr = static_cast<const FVector4f*>(PointReadback->Lock(PointReadbackBytes)))
			{
				FMemory::Memcpy(CompactPointData.GetData(), ResultPtr, PointReadbackBytes);
				PointReadback->Unlock();
				bPointLocked = true;
			}
			if (!bPointLocked)
			{
				UE_LOG(LogTemp, Warning, TEXT("[StaticMeshPointSampler] Failed to lock point readback."));
				delete PointReadback;
				delete CountReadback;
				return;
			}

			bReadbackSucceeded = true;
			delete PointReadback;
			delete CountReadback;
		});

	FlushRenderingCommands();

	if (!bReadbackSucceeded)
	{
		return false;
	}

	const int32 CompactPointCount = FMath::Min(int32(RequestCounts[0]), CompactCapacity);
	for (int32 RequestIndex = 0; RequestIndex < Requests.Num(); ++RequestIndex)
	{
		OutPointsPerRequest[RequestIndex] = FMath::Clamp(int32(RequestCounts[RequestIndex + 1]), 0, CompactCapacity);
	}

	const int32 AddedCount = AddReadbackPoints(CompactPointData.GetData(), CompactPointCount, OutPoints);
	return AddedCount > 0;
}
