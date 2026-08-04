#include "CSGpuMemoryBudget.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "Misc/App.h"
#include "Misc/MessageDialog.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "RHIStats.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "LandscapeComponent.h"
#endif

#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif

#define LOCTEXT_NAMESPACE "CSGpuMemoryBudget"

namespace CSGpuMemoryBudget
{
namespace
{
	// -------------------------------------------------------------------------
	// 每三角字节数常量
	//
	// 下面每个常量都对应真实的 CreateBuffer 容量式，改分配式必须同步改这里。
	// N = 源三角数，NodeCount = 2N-1，soup 顶点数 = 3N。
	// -------------------------------------------------------------------------

	/** soup 位置：3 顶点 × FVector4f。AddPreparedBoxSceneTrianglesToRDG。 */
	constexpr int64 SoupPositionBytesPerTriangle = 3 * 16;

	/** 法线 / 颜色 / 切线 / 副切线：各 3 顶点 × FVector4f（CornerAttributeBytes）。 */
	constexpr int64 SoupCornerAttributeBytesPerTriangle = 3 * 16;

	/** UV0：3 顶点 × FVector2f。 */
	constexpr int64 SoupUVBytesPerTriangle = 3 * 8;

	/** per-triangle 材质 id。 */
	constexpr int64 SoupMaterialBytesPerTriangle = 4;

	/**
	 * LBVH：Nodes 2×NodeCount×float4(64N) + Parent NodeCount×uint32(8N)
	 * + AtomicMin/Max 各 3(N-1)×uint32(24N) + Morton Keys/Payload 2×SortM×uint32
	 * （SortM 是 ≥N 的 2 的幂，最坏 2N，即 16N）。见 CSGpuTriangleUtilities.cpp
	 * AddTriangleLBVHBuildPasses。
	 */
	constexpr int64 LBVHBytesPerTriangle = 64 + 8 + 24 + 16;

	/**
	 * fast-winding 多极：MultipoleA/B 各 5×NodeCount×float4 = 160N，共 320N。
	 * 见 CSGpuTriangleUtilities.cpp AddFastWindingMultipolePasses。
	 */
	constexpr int64 WindingBytesPerTriangle = 320;

	/** 每条交线段：CutP0 + CutP1，各一个 FVector4f。 */
	constexpr int64 CutSegmentBytes = 2 * 16;

	/** 每个输出子三角：soup 3×FVector3f + source id uint32。 */
	constexpr int64 OutputTriangleBytes = 3 * 12 + 4;

	/** 每个输出子三角的焊接代表元：3 个角点 × uint32。 */
	constexpr int64 WeldTriangleBytes = 3 * 4;

	/**
	 * 非本地显存的保守占用系数：即使驱动报告的预算全空，也不假设我们能吃满整块显存
	 * （swap chain、编辑器视口 RT、streaming pool 都不在我们的 RDG 里）。仅用于
	 * 无法拿到实时预算、只能从总量推算时。
	 */
	constexpr double UnmeasuredUsableFraction = 0.9;

	/** 查询显卡本地显存的实时预算。失败返回 false，不改动输出参数。 */
	bool QueryLiveLocalVideoMemory(int64& OutBudget, int64& OutCurrentUsage)
	{
#if PLATFORM_WINDOWS
		if (!IsRHID3D12()) return false;

		ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
		ID3D12Device* Device = D3D12RHI ? D3D12RHI->RHIGetDevice(0) : nullptr;
		if (!Device) return false;

		TRefCountPtr<IDXGIFactory4> DXGIFactory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(DXGIFactory.GetInitReference())))) return false;

		TRefCountPtr<IDXGIAdapter3> Adapter;
		if (FAILED(DXGIFactory->EnumAdapterByLuid(Device->GetAdapterLuid(), IID_PPV_ARGS(Adapter.GetInitReference())))) return false;

		DXGI_QUERY_VIDEO_MEMORY_INFO VideoMemoryInfo{};
		if (FAILED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &VideoMemoryInfo))) return false;

		OutBudget = int64(VideoMemoryInfo.Budget);
		OutCurrentUsage = int64(VideoMemoryInfo.CurrentUsage);
		return true;
#else
		return false;
#endif
	}

	/** RHI 自己跟踪到的显存用量（贴图 + buffer）。只在拿不到实时预算时用。 */
	int64 GetRHITrackedVideoMemoryUsage()
	{
		const int64 TextureBytes = int64(GRHIGlobals.StreamingTextureMemorySizeInKB) * 1024
			+ int64(GRHIGlobals.NonStreamingTextureMemorySizeInKB) * 1024;
		return TextureBytes + int64(GRHIGlobals.BufferMemorySize);
	}

	/**
	 * 包围盒重叠折算系数。三角形分布在表面上，所以体积比按 2/3 次方折算成面积比；
	 * 退化轴（平面 mesh）不参与，此时按剩余轴的线性比例算。
	 */
	double ComputeBoundsOverlapRatio(const FBox& SourceBounds, const FBox& QueryBox)
	{
		if (!SourceBounds.IsValid || !QueryBox.IsValid) return 1.0;

		const FVector SourceSize = SourceBounds.GetSize();
		const FBox Overlap = SourceBounds.Overlap(QueryBox);
		if (!Overlap.IsValid) return 0.0;

		const FVector OverlapSize = Overlap.GetSize();
		double Ratio = 1.0;
		int32 NonDegenerateAxes = 0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (SourceSize[Axis] <= UE_KINDA_SMALL_NUMBER) continue;
			Ratio *= FMath::Clamp(OverlapSize[Axis] / SourceSize[Axis], 0.0, 1.0);
			++NonDegenerateAxes;
		}
		if (NonDegenerateAxes == 0) return 1.0;
		// 三维盒：体积比 → 面积比。一维/二维退化时线性比例本身就是面积比。
		return NonDegenerateAxes == 3 ? FMath::Pow(Ratio, 2.0 / 3.0) : Ratio;
	}

	/** landscape 是高度场，只在 XY 上折算；高度方向的包围盒厚度与三角数无关。 */
	double ComputeHeightfieldOverlapRatio(const FBox& SourceBounds, const FBox& QueryBox)
	{
		if (!SourceBounds.IsValid || !QueryBox.IsValid) return 1.0;

		const FVector SourceSize = SourceBounds.GetSize();
		const FBox Overlap = SourceBounds.Overlap(QueryBox);
		if (!Overlap.IsValid) return 0.0;

		const FVector OverlapSize = Overlap.GetSize();
		double Ratio = 1.0;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (SourceSize[Axis] <= UE_KINDA_SMALL_NUMBER) continue;
			Ratio *= FMath::Clamp(OverlapSize[Axis] / SourceSize[Axis], 0.0, 1.0);
		}
		return Ratio;
	}

	FText FormatBytes(int64 Bytes)
	{
		return FText::AsMemory(uint64(FMath::Max<int64>(Bytes, 0)));
	}
}

int64 FTriangleSoupCostModel::BytesPerSourceTriangle() const
{
	int64 Bytes = SoupPositionBytesPerTriangle;
	if (bSourceNormals) Bytes += SoupCornerAttributeBytesPerTriangle;
	if (bSourceTangents) Bytes += 2 * SoupCornerAttributeBytesPerTriangle;
	if (bSourceColors) Bytes += SoupCornerAttributeBytesPerTriangle;
	if (bSourceUVs) Bytes += SoupUVBytesPerTriangle;
	if (bSourceMaterialIds) Bytes += SoupMaterialBytesPerTriangle;
	if (bBuildLBVH) Bytes += LBVHBytesPerTriangle;
	if (bBuildWindingField) Bytes += WindingBytesPerTriangle;

	Bytes += int64(FMath::Max(0, CutSegmentsPerTriangle)) * CutSegmentBytes;

	const int64 OutputTriangles = int64(FMath::Max(0, OutputTrianglesPerSource));
	Bytes += OutputTriangles * OutputTriangleBytes;
	if (bWeldOutput) Bytes += OutputTriangles * WeldTriangleBytes;

	Bytes += int64(FMath::Max(0.0, ExtraBytesPerSourceTriangle));
	return FMath::Max<int64>(Bytes, 1);
}

FMemorySnapshot QueryMemorySnapshot()
{
	FMemorySnapshot Snapshot;

	FTextureMemoryStats TextureStats;
	RHIGetTextureMemoryStats(TextureStats);
	Snapshot.TotalVideoMemory = FMath::Max<int64>(TextureStats.GetTotalDeviceWorkingMemory(), 0);
	Snapshot.DemotedVideoMemory = int64(GRHIGlobals.DemotedLocalMemorySize);

	int64 Budget = 0;
	int64 CurrentUsage = 0;
	if (QueryLiveLocalVideoMemory(Budget, CurrentUsage))
	{
		Snapshot.bAvailableIsMeasured = true;
		Snapshot.AvailableVideoMemory = FMath::Max<int64>(Budget - CurrentUsage, 0);
		if (Snapshot.TotalVideoMemory <= 0) Snapshot.TotalVideoMemory = Budget;
	}
	else if (Snapshot.TotalVideoMemory > 0)
	{
		// 退路：总量 × 保守系数 − RHI 自己记账的用量。看不到其他进程，只能当粗略参考。
		const int64 Usable = int64(double(Snapshot.TotalVideoMemory) * UnmeasuredUsableFraction);
		Snapshot.AvailableVideoMemory = FMath::Max<int64>(Usable - GetRHITrackedVideoMemoryUsage(), 0);
	}

	const FPlatformMemoryStats PlatformStats = FPlatformMemory::GetStats();
	Snapshot.AvailablePhysicalMemory = int64(PlatformStats.AvailablePhysical);
	return Snapshot;
}

int64 EstimateMaxSourceTriangles(
	const FTriangleSoupCostModel& Cost,
	const FMemorySnapshot& Snapshot,
	float SafetyRatio)
{
	if (Snapshot.AvailableVideoMemory <= 0) return 0;

	const double Ratio = FMath::Clamp(double(SafetyRatio), 0.01, 1.0);
	const double UsableBytes = double(Snapshot.AvailableVideoMemory) * Ratio;
	return int64(UsableBytes / double(Cost.BytesPerSourceTriangle()));
}

FBoxSceneTriangleEstimate EstimateBoxSceneTriangles(
	UWorld* World,
	const FBox& QueryBox,
	int32 LODIndex,
	bool bIncludeLandscape)
{
	FBoxSceneTriangleEstimate Estimate;
	if (!World || !QueryBox.IsValid || !IsInGameThread()) return Estimate;

	// static mesh：枚举方式与 BuildBoxSceneTriangleRequestsInternal 一致，但三角数取自
	// 渲染数据的计数（O(1)），不加载 MeshDescription。两者的 LOD0 三角数通常一致；
	// 若 build settings 做了减面，这里会略偏保守。
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* StaticMeshComponent = *It;
		if (!IsValid(StaticMeshComponent)
			|| StaticMeshComponent->IsTemplate()
			|| !StaticMeshComponent->IsRegistered()
			|| StaticMeshComponent->GetWorld() != World)
		{
			continue;
		}

		UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
		if (!StaticMesh) continue;

		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0) continue;

		const int32 ClampedLOD = FMath::Clamp(LODIndex, 0, RenderData->LODResources.Num() - 1);
		const int64 MeshTriangles = int64(RenderData->LODResources[ClampedLOD].GetNumTriangles());
		if (MeshTriangles <= 0) continue;

		if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
		{
			const FBox LocalMeshBounds = StaticMesh->GetBoundingBox();
			for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
			{
				FTransform InstanceTransform = FTransform::Identity;
				InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);

				const FBox InstanceWorldBounds = LocalMeshBounds.TransformBy(InstanceTransform);
				if (!InstanceWorldBounds.Intersect(QueryBox)) continue;

				++Estimate.StaticMeshInstances;
				Estimate.UpperBoundTriangles += MeshTriangles;
				Estimate.EstimatedTriangles += int64(double(MeshTriangles)
					* ComputeBoundsOverlapRatio(InstanceWorldBounds, QueryBox));
			}
			continue;
		}

		const FBox ComponentWorldBounds = StaticMeshComponent->Bounds.GetBox();
		if (!ComponentWorldBounds.Intersect(QueryBox)) continue;

		++Estimate.StaticMeshInstances;
		Estimate.UpperBoundTriangles += MeshTriangles;
		Estimate.EstimatedTriangles += int64(double(MeshTriangles)
			* ComputeBoundsOverlapRatio(ComponentWorldBounds, QueryBox));
	}

#if WITH_EDITOR
	// landscape 提取本身就是 editor-only（FLandscapeComponentDataInterface），非编辑器下
	// 不会有地形三角进入 soup，也就不必计入预算。
	if (bIncludeLandscape)
	{
		for (TObjectIterator<ULandscapeComponent> It; It; ++It)
		{
			ULandscapeComponent* LandscapeComponent = *It;
			if (!IsValid(LandscapeComponent)
				|| LandscapeComponent->IsTemplate()
				|| !LandscapeComponent->IsRegistered()
				|| LandscapeComponent->GetWorld() != World)
			{
				continue;
			}

			const int32 ComponentSizeQuads = LandscapeComponent->ComponentSizeQuads;
			if (ComponentSizeQuads <= 0) continue;

			const FBox ComponentWorldBounds = LandscapeComponent->Bounds.GetBox();
			if (!ComponentWorldBounds.Intersect(QueryBox)) continue;

			// 每个 quad 两个三角（BuildBoxSceneLandscapeTrianglesInternal 逐 quad 发两个面）。
			const int64 ComponentTriangles = 2ll * int64(ComponentSizeQuads) * int64(ComponentSizeQuads);
			Estimate.UpperBoundTriangles += ComponentTriangles;

			const int64 OverlapTriangles = int64(double(ComponentTriangles)
				* ComputeHeightfieldOverlapRatio(ComponentWorldBounds, QueryBox));
			Estimate.LandscapeTriangles += OverlapTriangles;
			Estimate.EstimatedTriangles += OverlapTriangles;
		}
	}
#endif

	return Estimate;
}

FBudgetCheckResult CheckSourceTriangleBudget(
	const TCHAR* OperationName,
	int64 EstimatedTriangles,
	const FTriangleSoupCostModel& Cost,
	const FBudgetCheckSettings& Settings)
{
	const TCHAR* SafeOperationName = OperationName ? OperationName : TEXT("GPU operation");

	FBudgetCheckResult Result;
	Result.Snapshot = QueryMemorySnapshot();
	Result.EstimatedTriangles = FMath::Max<int64>(EstimatedTriangles, 0);
	Result.EstimatedBytes = Result.EstimatedTriangles * Cost.BytesPerSourceTriangle();
	Result.MaxTriangles = EstimateMaxSourceTriangles(Cost, Result.Snapshot, Settings.SafetyRatio);

	if (Result.Snapshot.AvailableVideoMemory <= 0)
	{
		// 拿不到任何显存信息就不要拦路：宁可让操作跑，也不要因为探测失败而无法工作。
		Result.Verdict = EBudgetVerdict::BudgetUnknown;
		UE_LOG(LogTemp, Warning,
			TEXT("[GpuMemoryBudget:%s] 无法获取显存信息，跳过预算检查（预估 %lld 三角，%.0f B/三角）"),
			SafeOperationName, Result.EstimatedTriangles, double(Cost.BytesPerSourceTriangle()));
		return Result;
	}

	const double EstimatedMiB = double(Result.EstimatedBytes) / (1024.0 * 1024.0);
	const double AvailableMiB = double(Result.Snapshot.AvailableVideoMemory) / (1024.0 * 1024.0);

	if (Result.EstimatedTriangles <= Result.MaxTriangles)
	{
		Result.Verdict = EBudgetVerdict::WithinBudget;
		UE_LOG(LogTemp, Log,
			TEXT("[GpuMemoryBudget:%s] 预估 %lld 三角 / 上限 %lld，峰值 %.0f MiB / 可用 %.0f MiB（%s，安全系数 %.2f）"),
			SafeOperationName, Result.EstimatedTriangles, Result.MaxTriangles,
			EstimatedMiB, AvailableMiB,
			Result.Snapshot.bAvailableIsMeasured ? TEXT("实时预算") : TEXT("推算"),
			Settings.SafetyRatio);
		return Result;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[GpuMemoryBudget:%s] 超出显存预算：预估 %lld 三角 > 上限 %lld，峰值 %.0f MiB > 可用 %.0f MiB × %.2f（%s；已降级显存 %.0f MiB）"),
		SafeOperationName, Result.EstimatedTriangles, Result.MaxTriangles,
		EstimatedMiB, AvailableMiB, Settings.SafetyRatio,
		Result.Snapshot.bAvailableIsMeasured ? TEXT("实时预算") : TEXT("推算"),
		double(Result.Snapshot.DemotedVideoMemory) / (1024.0 * 1024.0));

	const bool bCanPrompt = Settings.bPromptOnExceed
		&& IsInGameThread()
		&& !FApp::IsUnattended()
		&& !IsRunningCommandlet()
		&& FApp::CanEverRender();

	if (!bCanPrompt)
	{
		Result.Verdict = Settings.bProceedWhenUnattended
			? EBudgetVerdict::ProceededUnattended
			: EBudgetVerdict::CancelledUnattended;
		UE_LOG(LogTemp, Warning, TEXT("[GpuMemoryBudget:%s] 无法弹窗确认，按设置%s"),
			SafeOperationName, Settings.bProceedWhenUnattended ? TEXT("继续执行") : TEXT("中止"));
		return Result;
	}

	const FText Message = FText::Format(
		LOCTEXT("ExceedBudgetMessage",
			"{0}：预估场景三角形 {1}，超过当前显存可承受的上限 {2}。\n\n"
			"预计峰值显存：{3}（{4} / 三角）\n"
			"可用显存：{5}（{6}） / 总显存：{7}\n"
			"安全系数：{8}\n\n"
			"继续执行可能耗尽显存，导致设备移除（TDR）或编辑器崩溃。\n"
			"是否仍然继续？"),
		FText::FromString(SafeOperationName),
		FText::AsNumber(Result.EstimatedTriangles),
		FText::AsNumber(Result.MaxTriangles),
		FormatBytes(Result.EstimatedBytes),
		FormatBytes(Cost.BytesPerSourceTriangle()),
		FormatBytes(Result.Snapshot.AvailableVideoMemory),
		Result.Snapshot.bAvailableIsMeasured
			? LOCTEXT("AvailableMeasured", "实时预算")
			: LOCTEXT("AvailableEstimated", "推算值"),
		FormatBytes(Result.Snapshot.TotalVideoMemory),
		FText::AsNumber(Settings.SafetyRatio));

	const EAppReturnType::Type Response = FMessageDialog::Open(
		EAppMsgType::YesNo,
		EAppReturnType::No,
		Message,
		LOCTEXT("ExceedBudgetTitle", "显存预算警告"));

	Result.Verdict = Response == EAppReturnType::Yes
		? EBudgetVerdict::ConfirmedByUser
		: EBudgetVerdict::CancelledByUser;
	UE_LOG(LogTemp, Warning, TEXT("[GpuMemoryBudget:%s] 用户选择%s"),
		SafeOperationName, Response == EAppReturnType::Yes ? TEXT("继续") : TEXT("取消"));
	return Result;
}

FBudgetCheckResult CheckBoxSceneTriangleBudget(
	const TCHAR* OperationName,
	UWorld* World,
	const FBox& QueryBox,
	int32 LODIndex,
	bool bIncludeLandscape,
	const FTriangleSoupCostModel& Cost,
	const FBudgetCheckSettings& Settings)
{
	const FBoxSceneTriangleEstimate Estimate =
		EstimateBoxSceneTriangles(World, QueryBox, LODIndex, bIncludeLandscape);

	UE_LOG(LogTemp, Log,
		TEXT("[GpuMemoryBudget:%s] 场景预估：%lld 三角（上界 %lld，%d 个实例，地形 %lld）"),
		OperationName ? OperationName : TEXT("GPU operation"),
		Estimate.EstimatedTriangles, Estimate.UpperBoundTriangles,
		Estimate.StaticMeshInstances, Estimate.LandscapeTriangles);

	return CheckSourceTriangleBudget(OperationName, Estimate.EstimatedTriangles, Cost, Settings);
}
}

#undef LOCTEXT_NAMESPACE
