#include "CSGpuMeshComponent.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSMesh.h"

#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

UCSGpuMeshComponent::UCSGpuMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

FBoxSphereBounds UCSGpuMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBox Box = LocalBounds.IsValid ? LocalBounds : FBox(FVector(-100.0), FVector(100.0));
	return FBoxSphereBounds(Box.TransformBy(LocalToWorld));
}

void UCSGpuMeshComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (UMaterialInterface* Mat = GetRenderMaterial()) OutMaterials.Add(Mat);
}

bool UCSGpuMeshComponent::ReadbackMeshSync(FCSGpuMeshCPUData& OutMeshData) const
{
	OutMeshData.Reset();
	if (!IsInGameThread() || !IsRegistered())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback rejected: GameThread=%d Registered=%d."),
			IsInGameThread() ? 1 : 0, IsRegistered() ? 1 : 0);
		return false;
	}

	// 下面的 static_cast 无法校验实际类型，因此必须先由子类确认当前代理确实是
	// FCSGpuMeshSceneProxy。子类若可能创建非本基座的代理、或可能压根没有代理（例如网格对象
	// 还没分配），不在此拦截的话 cast 出来就是野指针（UB）。
	if (!IsGpuMeshProxyActive())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback rejected: the component is not currently displaying a GPU mesh."));
		return false;
	}

	// Complete any pending proxy recreation before capturing the proxy pointer.
	FlushRenderingCommands();
	FCSGpuMeshSceneProxy* Proxy = static_cast<FCSGpuMeshSceneProxy*>(GetSceneProxy());
	if (!Proxy)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Readback rejected: scene proxy is unavailable after flushing render commands."));
		return false;
	}

	// 回读实现已从「作用于代理」提升为「作用于常驻集」，代理只负责交出自己的流视图。
	// 代理的 Streams 是渲染线程数据，故快照要在渲染线程取；取完这份视图自带引用，
	// 后续读取不再依赖代理存活。
	FCSMeshResident ResidentView;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshBuildResidentView)(
		[Proxy, &ResidentView](FRHICommandListImmediate&) { Proxy->BuildResidentView(ResidentView); });
	FlushRenderingCommands();

	return CSMeshReadback::ReadbackResidentSync(ResidentView, OutMeshData);
}

// -----------------------------------------------------------------------------
// GPU 快照 -> StaticMesh（原 CSGpuMeshConvert.cpp，2026-08 并入本 TU）
// -----------------------------------------------------------------------------

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Async/ParallelFor.h"

#if WITH_EDITOR

#include "CSGpuMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/Level.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetUtils/CreateStaticMeshUtil.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"

#endif

namespace
{
bool IsFiniteVec(const FVector& V)
{
	return !V.ContainsNaN() && FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
}

FName BuildMaterialSlotName(int32 Slot)
{
	return FName(*FString::Printf(TEXT("MaterialSlot_%d"), Slot));
}

// Shared by the transient and the saved-asset paths so both get identical material slots and
// build settings; only the outer/flags and whether the MeshDescription is committed differ.
bool PopulateStaticMeshFromDescription(
	UStaticMesh* StaticMesh,
	FMeshDescription& MeshDescription,
	const FCSGpuMeshCPUData& MeshData,
	const TArray<UMaterialInterface*>& Materials,
	bool bCommitMeshDescription,
	bool bEnableNanite)
{
	if (!StaticMesh) return false;

	// 必须在 BuildFromMeshDescriptions 之前设置：Nanite 数据是在那次构建里生成的，
	// 建完再改设置只会标脏，不会真的产出 Nanite 数据。
	{
		FMeshNaniteSettings NaniteSettings = StaticMesh->GetNaniteSettings();
		NaniteSettings.bEnabled = bEnableNanite;
		StaticMesh->SetNaniteSettings(NaniteSettings);
	}

	int32 RequiredMaterialSlots = FMath::Max(1, Materials.Num());
	for (int32 Slot : MeshData.TriangleMaterialSlots) RequiredMaterialSlots = FMath::Max(RequiredMaterialSlots, Slot + 1);
	for (int32 Slot = 0; Slot < RequiredMaterialSlots; ++Slot)
	{
		UMaterialInterface* Material = Materials.IsValidIndex(Slot) ? Materials[Slot] : nullptr;
		const FName SlotName = BuildMaterialSlotName(Slot);
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(Material, SlotName, SlotName));
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = false;
	// The fast path (UStaticMesh::BuildFromMeshDescription) emits one render vertex per vertex
	// instance and never merges them, so a perfectly welded MeshDescription still renders with
	// 3 vertices per triangle - welding looks like it did nothing and the buffers stay large.
	// Saved assets therefore take the full build, which merges vertices by attribute equality.
	// Transient results are throwaway previews, so they keep the fast path.
	BuildParams.bFastBuild = !bCommitMeshDescription;
	// A saved asset must keep its editable source data; a transient one must not pay for it.
	BuildParams.bCommitMeshDescription = bCommitMeshDescription;
	return StaticMesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams);
}

#if WITH_EDITOR
// Default save location when the caller passes an empty path: an "AutoResult" folder sitting next to
// the component's current level asset — e.g. level /Game/Maps/L_Foo -> /Game/Maps/AutoResult/SM_<owner>.
// Returns an empty string when the owning level can't be resolved to a content path (e.g. an
// unsaved /Temp map), in which case the caller must supply an explicit path.
FString BuildDefaultResultAssetPath(const AActor* Owner, const FString& NameSuffix = FString())
{
	const ULevel* Level = Owner ? Owner->GetLevel() : nullptr;
	const UPackage* LevelPackage = Level ? Level->GetPackage() : nullptr;
	const FString LevelPackageName = LevelPackage ? LevelPackage->GetName() : FString();
	if (!FPackageName::IsValidLongPackageName(LevelPackageName)) return FString();

	const FString LevelFolder = FPackageName::GetLongPackagePath(LevelPackageName); // e.g. /Game/Maps
	if (LevelFolder.IsEmpty()) return FString();

	FString BaseName = Owner->GetName();
	if (BaseName.IsEmpty()) BaseName = TEXT("GpuMesh");
	return FString::Printf(TEXT("%s/AutoResult/SM_%s%s"), *LevelFolder, *BaseName, *NameSuffix);
}
#endif
}

#if WITH_EDITOR
FString UCSGpuMeshComponent::BuildResultAssetPath(const AActor* OwnerActor, const FString& NameSuffix)
{
	return BuildDefaultResultAssetPath(OwnerActor, NameSuffix);
}
#endif

bool UCSGpuMeshComponent::BuildGpuMeshDescription(
	const FCSGpuMeshCPUData& MeshData,
	const FTransform& ActorTransform,
	bool bConvertToActorLocalSpace,
	FMeshDescription& OutMeshDescription)
{
	OutMeshDescription.Empty();
	if (!MeshData.IsValid()) return false;

	FStaticMeshAttributes Attributes(OutMeshDescription);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TPolygonGroupAttributesRef<FName> PolygonGroupMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
	// 只为真正填了数据的通道开 UV 槽：声明了 N 条但某条为空时不开，避免写出一整条零 UV。
	int32 ActiveTexCoordChannels = FMath::Clamp(
		MeshData.NumTexCoordChannels, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	while (ActiveTexCoordChannels > 1 && MeshData.TexCoordChannels[ActiveTexCoordChannels - 1].IsEmpty())
	{
		--ActiveTexCoordChannels;
	}
	if (VertexInstanceUVs.GetNumChannels() < ActiveTexCoordChannels) VertexInstanceUVs.SetNumChannels(ActiveTexCoordChannels);

	int32 MaxMaterialSlot = 0;
	for (int32 Slot : MeshData.TriangleMaterialSlots) MaxMaterialSlot = FMath::Max(MaxMaterialSlot, Slot);
	TArray<FPolygonGroupID> PolygonGroupIDs;
	PolygonGroupIDs.Reserve(MaxMaterialSlot + 1);
	for (int32 Slot = 0; Slot <= MaxMaterialSlot; ++Slot)
	{
		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		PolygonGroupMaterialSlotNames[PolygonGroupID] = BuildMaterialSlotName(Slot);
		PolygonGroupIDs.Add(PolygonGroupID);
	}

	// 预留全部元素容量：百万级网格逐个 Create 会反复触发内部数组扩容+搬迁，是这段最大的固定开销。
	const int32 ExpectedTriangles = MeshData.Indices.Num() / 3;
	OutMeshDescription.ReserveNewVertices(MeshData.Positions.Num());
	OutMeshDescription.ReserveNewVertexInstances(ExpectedTriangles * 3);
	OutMeshDescription.ReserveNewTriangles(ExpectedTriangles);
	OutMeshDescription.ReserveNewPolygons(ExpectedTriangles);

	// MeshDescription 的元素创建不是线程安全的，但坐标变换是纯计算。先并行算好再串行灌入，
	// 百万级网格下这部分（每顶点一次双精度逆变换）原本是本函数的主要开销之一。
	const int32 PositionCount = MeshData.Positions.Num();
	TArray<FVector3f> LocalPositions;
	LocalPositions.SetNumUninitialized(PositionCount);
	TBitArray<> ValidPositions(false, PositionCount);
	{
		TArray<bool> ValidFlags;
		ValidFlags.SetNumUninitialized(PositionCount);
		ParallelFor(PositionCount, [&](int32 VertexIndex)
		{
			const FVector SourcePosition(MeshData.Positions[VertexIndex]);
			const FVector Position = bConvertToActorLocalSpace
				? ActorTransform.InverseTransformPosition(SourcePosition)
				: SourcePosition;
			const bool bPositionValid = IsFiniteVec(Position);
			ValidFlags[VertexIndex] = bPositionValid;
			LocalPositions[VertexIndex] = bPositionValid ? FVector3f(Position) : FVector3f::ZeroVector;
		});
		for (int32 VertexIndex = 0; VertexIndex < PositionCount; ++VertexIndex) ValidPositions[VertexIndex] = ValidFlags[VertexIndex];
	}

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(PositionCount);
	for (int32 VertexIndex = 0; VertexIndex < PositionCount; ++VertexIndex)
	{
		const FVertexID VertexID = OutMeshDescription.CreateVertex();
		VertexPositions[VertexID] = LocalPositions[VertexIndex];
		VertexIDs.Add(VertexID);
	}

	const float TransformHandedness = bConvertToActorLocalSpace && ActorTransform.GetDeterminant() < 0.0 ? -1.0f : 1.0f;

	auto AttributeIndex = [&MeshData](int32 AttributeCount, int32 PositionIndex, int32 CornerIndex)
	{
		return AttributeCount == MeshData.Indices.Num() ? CornerIndex : PositionIndex;
	};

	// 法线/切线的坐标变换同样是纯计算，按源数组一次性并行算好；下面的建面循环直接查表。
	// 注意按源数组长度（可能是 per-vertex 也可能是 per-corner）预计算，避免重复变换同一条目。
	TArray<FVector> TransformedNormals;
	TArray<FVector> TransformedTangents;
	{
		TransformedNormals.SetNumUninitialized(MeshData.Normals.Num());
		ParallelFor(MeshData.Normals.Num(), [&](int32 Index)
		{
			FVector Normal(MeshData.Normals[Index]);
			if (bConvertToActorLocalSpace) Normal = ActorTransform.GetRotation().UnrotateVector(Normal) * ActorTransform.GetScale3D();
			TransformedNormals[Index] = Normal.GetSafeNormal();
		});
		TransformedTangents.SetNumUninitialized(MeshData.Tangents.Num());
		ParallelFor(MeshData.Tangents.Num(), [&](int32 Index)
		{
			const FVector Tangent(MeshData.Tangents[Index]);
			TransformedTangents[Index] = (bConvertToActorLocalSpace ? ActorTransform.InverseTransformVector(Tangent) : Tangent).GetSafeNormal();
		});
	}

	int32 CreatedTriangleCount = 0;
	// 循环外复用，避免每个三角在堆上新建一个 3 元素 TArray（百万级三角 = 百万次分配）。
	TArray<FVertexInstanceID, TInlineAllocator<3>> VertexInstanceIDs;
	VertexInstanceIDs.SetNumUninitialized(3);
	for (int32 IndexOffset = 0; IndexOffset + 2 < MeshData.Indices.Num(); IndexOffset += 3)
	{
		const int32 SourceIndices[3] = {
			int32(MeshData.Indices[IndexOffset + 0]),
			int32(MeshData.Indices[IndexOffset + 1]),
			int32(MeshData.Indices[IndexOffset + 2])
		};
		if (!VertexIDs.IsValidIndex(SourceIndices[0]) || !VertexIDs.IsValidIndex(SourceIndices[1]) || !VertexIDs.IsValidIndex(SourceIndices[2])) continue;
		if (!ValidPositions[SourceIndices[0]] || !ValidPositions[SourceIndices[1]] || !ValidPositions[SourceIndices[2]]) continue;
		if (SourceIndices[0] == SourceIndices[1] || SourceIndices[1] == SourceIndices[2] || SourceIndices[0] == SourceIndices[2]) continue;

		const FVector3f P0 = VertexPositions[VertexIDs[SourceIndices[0]]];
		const FVector3f P1 = VertexPositions[VertexIDs[SourceIndices[1]]];
		const FVector3f P2 = VertexPositions[VertexIDs[SourceIndices[2]]];
		// UE uses counter-clockwise winding in a left-handed coordinate system, so the
		// geometric normal for StaticMesh attributes requires the reversed cross product.
		const FVector FaceNormal = FVector::CrossProduct(FVector(P2 - P0), FVector(P1 - P0)).GetSafeNormal();
		if (FaceNormal.IsNearlyZero()) continue;

		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 SourceIndex = SourceIndices[Corner];
			const int32 CornerIndex = IndexOffset + Corner;
			const FVertexInstanceID VertexInstanceID = OutMeshDescription.CreateVertexInstance(VertexIDs[SourceIndex]);

			const int32 NormalIndex = AttributeIndex(MeshData.Normals.Num(), SourceIndex, CornerIndex);
			const int32 TangentIndex = AttributeIndex(MeshData.Tangents.Num(), SourceIndex, CornerIndex);
			const int32 UVIndex = AttributeIndex(MeshData.TexCoords().Num(), SourceIndex, CornerIndex);
			FVector Normal = TransformedNormals[NormalIndex];
			if (!IsFiniteVec(Normal) || Normal.IsNearlyZero()) Normal = FaceNormal;
			// 不做逐角点半球校正（曾经是 dot(Normal, FaceNormal) < 0 就把该角点法线取反）。
			// FaceNormal 由 float32 角点位置相减再叉积得到，布尔切出的窄条/尖角碎片上这一步会
			// 因灾难性抵消而方向失真；于是同一个三角的三个角点各自去和一条不可靠的参考轴比较，
			// 三个里往往只有一个被判反 —— 症状正是「偶尔某个三角的某一个角点法线朝里、
			// 该角出现异常暗部」。
			// 即使 FaceNormal 可靠，背朝面法线的角点法线也多半是作者有意的（双面卡片、硬边烘焙、
			// 外扩 shell、近切向法线），逐角点翻转是破坏而非修复，还会在三角内部撕开着色。
			// ComputeShaderMeshBoolean.cpp 的属性重建段已按同一理由移除该校正，这里是漏下的最后
			// 一处：布尔结果在烘成 StaticMesh 时又被翻回去了。两处必须保持一致。
			// 零长度/非有限法线仍由上一行兜底，那条才是真正的安全网。
			FVector Tangent = TransformedTangents[TangentIndex];
			Tangent = (Tangent - Normal * FVector::DotProduct(Tangent, Normal)).GetSafeNormal();
			if (!IsFiniteVec(Tangent) || Tangent.IsNearlyZero())
			{
				const FVector Helper = FMath::Abs(Normal.Z) < 0.99 ? FVector::UpVector : FVector::ForwardVector;
				Tangent = FVector::CrossProduct(Helper, Normal).GetSafeNormal();
			}

			const FVector2f UV = MeshData.TexCoords()[UVIndex];
			const FVector2f SafeUV = FMath::IsFinite(UV.X) && FMath::IsFinite(UV.Y) ? UV : FVector2f::ZeroVector;
			FVector4f Color(1.0f, 1.0f, 1.0f, 1.0f);
			if (!MeshData.Colors.IsEmpty())
			{
				const int32 ColorIndex = AttributeIndex(MeshData.Colors.Num(), SourceIndex, CornerIndex);
				Color = MeshData.Colors[ColorIndex];
				if (!FMath::IsFinite(Color.X) || !FMath::IsFinite(Color.Y) || !FMath::IsFinite(Color.Z) || !FMath::IsFinite(Color.W)) Color = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
			}
			float BinormalSign = 1.0f;
			if (!MeshData.BinormalSigns.IsEmpty())
			{
				const int32 SignIndex = AttributeIndex(MeshData.BinormalSigns.Num(), SourceIndex, CornerIndex);
				BinormalSign = MeshData.BinormalSigns[SignIndex] < 0.0f ? -1.0f : 1.0f;
			}
			VertexInstanceNormals[VertexInstanceID] = FVector3f(Normal);
			VertexInstanceTangents[VertexInstanceID] = FVector3f(Tangent);
			// 法线不再被逐角点翻转，切线基的手性就不需要再补那个补偿符号；镜像变换那一项仍要保留。
			VertexInstanceBinormalSigns[VertexInstanceID] = BinormalSign * TransformHandedness;
			VertexInstanceColors[VertexInstanceID] = Color;
			VertexInstanceUVs.Set(VertexInstanceID, 0, SafeUV);
			// 额外 UV 通道与通道 0 等长（IsValid 已校验），沿用同一个角点下标。
			for (int32 Channel = 1; Channel < ActiveTexCoordChannels; ++Channel)
			{
				const TArray<FVector2f>& ChannelUVs = MeshData.TexCoordChannels[Channel];
				const FVector2f ChannelUV = ChannelUVs.IsValidIndex(UVIndex) ? ChannelUVs[UVIndex] : FVector2f::ZeroVector;
				const bool bFinite = FMath::IsFinite(ChannelUV.X) && FMath::IsFinite(ChannelUV.Y);
				VertexInstanceUVs.Set(VertexInstanceID, Channel, bFinite ? ChannelUV : FVector2f::ZeroVector);
			}
			VertexInstanceIDs[Corner] = VertexInstanceID;
		}

		const int32 TriangleIndex = IndexOffset / 3;
		const int32 RequestedSlot = MeshData.TriangleMaterialSlots.IsValidIndex(TriangleIndex)
			? MeshData.TriangleMaterialSlots[TriangleIndex]
			: 0;
		const int32 MaterialSlot = FMath::Clamp(RequestedSlot, 0, PolygonGroupIDs.Num() - 1);
		// 输入恒为三角形，直接建三角面，省掉 CreatePolygon 的 n-gon 三角化簿记。
		OutMeshDescription.CreateTriangle(PolygonGroupIDs[MaterialSlot], VertexInstanceIDs);
		++CreatedTriangleCount;
	}

	return CreatedTriangleCount > 0;
}

UStaticMesh* UCSGpuMeshComponent::BuildTransientStaticMesh(
	UObject* Outer,
	const FCSGpuMeshCPUData& MeshData,
	const TArray<UMaterialInterface*>& Materials,
	const FTransform& ActorTransform,
	bool bConvertToActorLocalSpace,
	bool bEnableNanite)
{
	if (!Outer) return nullptr;

	FMeshDescription MeshDescription;
	if (!BuildGpuMeshDescription(MeshData, ActorTransform, bConvertToActorLocalSpace, MeshDescription)) return nullptr;

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
	if (!StaticMesh) return nullptr;
	// Transient results are rebuilt from scratch every run, so the editable MeshDescription
	// copy is dead weight; skipping the commit avoids ~1.5 GiB of resident bulk data per mesh.
	if (!PopulateStaticMeshFromDescription(StaticMesh, MeshDescription, MeshData, Materials, false, bEnableNanite)) return nullptr;
	return StaticMesh;
}

#if WITH_EDITOR

UStaticMesh* UCSGpuMeshComponent::SaveGpuMeshDataToStaticMesh(
	const AActor* OwnerActor,
	const FCSGpuMeshCPUData& MeshData,
	const TArray<UMaterialInterface*>& Materials,
	const FTransform& ActorTransform,
	bool bConvertToActorLocalSpace,
	const FString& AssetPathAndName,
	bool bReplaceExistingAsset,
	bool bSaveAsset,
	bool bEnableNanite)
{
	FString EffectiveAssetPath = AssetPathAndName.TrimStartAndEnd();
	if (EffectiveAssetPath.IsEmpty())
	{
		EffectiveAssetPath = BuildDefaultResultAssetPath(OwnerActor);
		if (EffectiveAssetPath.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: could not derive a 'result' folder from the owning level (unsaved map?). Pass an explicit /Game/... path."));
			return nullptr;
		}
	}

	const FString SanitizedAssetPathAndName = UPackageTools::SanitizePackageName(EffectiveAssetPath);
	if (!FPackageName::IsValidLongPackageName(SanitizedAssetPathAndName))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: invalid asset path '%s'."), *SanitizedAssetPathAndName);
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(SanitizedAssetPathAndName);
	if (AssetName.IsEmpty()) return nullptr;

	const FString AssetFolderPath = FPackageName::GetLongPackagePath(SanitizedAssetPathAndName);
	if (!AssetFolderPath.IsEmpty()
		&& !UEditorAssetLibrary::DoesDirectoryExist(AssetFolderPath)
		&& !UEditorAssetLibrary::MakeDirectory(AssetFolderPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: could not create asset folder '%s'."), *AssetFolderPath);
		return nullptr;
	}

	// 同名资产已存在时优先「就地重建」而不是删掉重建：删除会打断所有已有引用（本 actor 的
	// OutputStaticMesh、场上的 StaticMeshComponent、其他关卡），而且只要还有人在内存里引用它，
	// DeleteAsset 本身就会失败，重跑等于永远存不上。就地重建保留同一个 UObject，引用自然跟着更新。
	UStaticMesh* ExistingStaticMesh = nullptr;
	if (UEditorAssetLibrary::DoesAssetExist(SanitizedAssetPathAndName))
	{
		if (!bReplaceExistingAsset)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save skipped: asset already exists at '%s'."), *SanitizedAssetPathAndName);
			return nullptr;
		}
		ExistingStaticMesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(SanitizedAssetPathAndName));
		// 同名的不是 StaticMesh（材质、贴图……）就没法就地重建，只能按老路子删掉。
		if (!ExistingStaticMesh && !UEditorAssetLibrary::DeleteAsset(SanitizedAssetPathAndName))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: could not replace '%s'."), *SanitizedAssetPathAndName);
			return nullptr;
		}
	}

	const double SaveStartTime = FPlatformTime::Seconds();
	FMeshDescription MeshDescription;
	if (!BuildGpuMeshDescription(MeshData, ActorTransform, bConvertToActorLocalSpace, MeshDescription)) return nullptr;
	const double DescriptionBuiltTime = FPlatformTime::Seconds();

	UPackage* Package = ExistingStaticMesh ? ExistingStaticMesh->GetPackage() : CreatePackage(*SanitizedAssetPathAndName);
	if (!Package) return nullptr;
	Package->FullyLoad();

	UStaticMesh* StaticMesh = ExistingStaticMesh;
	if (StaticMesh)
	{
		StaticMesh->Modify();
		// 材质槽与 section 映射是按本次网格重新装配的，先清空，否则上一轮的槽位会残留在前面。
		StaticMesh->GetStaticMaterials().Empty();
		StaticMesh->GetSectionInfoMap().Clear();
	}
	else
	{
		StaticMesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone);
	}
	// Committed on purpose: this one is a real asset that must survive save/reload and stay editable.
	if (!PopulateStaticMeshFromDescription(StaticMesh, MeshDescription, MeshData, Materials, true, bEnableNanite))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: StaticMesh build failed for '%s'."), *SanitizedAssetPathAndName);
		return nullptr;
	}
	const double MeshBuiltTime = FPlatformTime::Seconds();

	// 就地重建的资产 registry 里本来就有，再报一次 AssetCreated 会多出一条重复记录。
	if (!ExistingStaticMesh) FAssetRegistryModule::AssetCreated(StaticMesh);
	StaticMesh->MarkPackageDirty();
	Package->SetDirtyFlag(true);
	UE_LOG(LogTemp, Log, TEXT("[CSGpuMesh] save timing: meshDescription=%.3fs staticMeshBuild=%.3fs registry=%.3fs"),
		DescriptionBuiltTime - SaveStartTime,
		MeshBuiltTime - DescriptionBuiltTime,
		FPlatformTime::Seconds() - MeshBuiltTime);

	if (bSaveAsset && !UEditorAssetLibrary::SaveLoadedAsset(StaticMesh, false))
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Created '%s' but failed to write the package to disk."), *SanitizedAssetPathAndName);

	UE_LOG(LogTemp, Display, TEXT("[CSGpuMesh] %s '%s' (dirty): vertices=%d triangles=%d slots=%d"),
		ExistingStaticMesh ? TEXT("Overwrote") : TEXT("Saved"),
		*SanitizedAssetPathAndName, MeshData.Positions.Num(), MeshData.Indices.Num() / 3, StaticMesh->GetStaticMaterials().Num());
	return StaticMesh;
}

UStaticMesh* UCSGpuMeshComponent::SaveRenderedMeshToStaticMesh(
	const FString& AssetPathAndName,
	UMaterialInterface* Material,
	const FTransform& ActorTransform,
	bool bConvertToActorLocalSpace,
	bool bReplaceExistingAsset,
	bool bSaveAsset)
{
	// 这个函数只做两件事：把本组件的 GPU 网格回读到 CPU，然后交给公用转换入口。
	// 路径校验、材质槽装配、绕序与退化面处理全部收敛在下面的 BuildStaticMesh 里，不再各写一份。
	FCSGpuMeshCPUData MeshData;
	if (!ReadbackMeshSync(MeshData))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: GPU mesh readback returned no valid triangles."));
		return nullptr;
	}

	// 调用方给的单材质仍然生效；组件回读若已带材质表则以它为准（多材质路径）。
	if (MeshData.Materials.IsEmpty() && Material) MeshData.Materials.Add(Material);

	FCSGpuMeshConvertOptions ConvertOptions;
	ConvertOptions.TargetTransform = ActorTransform;
	ConvertOptions.bBakeToLocalSpace = bConvertToActorLocalSpace;

	FCSGpuMeshAssetOptions AssetOptions;
	AssetOptions.AssetPath = AssetPathAndName.TrimStartAndEnd();
	if (AssetOptions.AssetPath.IsEmpty())
	{
		AssetOptions.AssetPath = BuildDefaultResultAssetPath(GetOwner());
		if (AssetOptions.AssetPath.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSGpuMesh] Save failed: no path given and could not derive a default 'result' folder from the current level (unsaved map?). Pass an explicit /Game/... path."));
			return nullptr;
		}
		UE_LOG(LogTemp, Display, TEXT("[CSGpuMesh] No path given; defaulting to '%s' (result folder next to the current level)."), *AssetOptions.AssetPath);
	}
	AssetOptions.bReplaceExisting = bReplaceExistingAsset;
	AssetOptions.bSaveToDisk = bSaveAsset;

	return BuildStaticMesh(
		this, GetOwner(), MeshData, TArray<UMaterialInterface*>(),
		ConvertOptions, AssetOptions);
}

#endif // WITH_EDITOR


UMaterialInterface* UCSGpuMeshComponent::GetDefaultSurfaceMaterial()
{
	return UMaterial::GetDefaultMaterial(MD_Surface);
}

bool UCSGpuMeshComponent::BuildMeshDescription(
	const FCSGpuMeshCPUData& MeshData,
	const FCSGpuMeshConvertOptions& Options,
	FMeshDescription& OutMeshDescription)
{
	// 属性装配仍复用已验证的实现（绕序、切线正交化、退化面过滤、per-corner/per-vertex 判别
	// 都在里面），这里只负责把统一的选项翻译过去。后续把那份实现搬进本文件时，调用方无感。
	//
	// 只有当数据确实是世界空间、且调用方要求烘到局部空间时才做变换。以前靠单个
	// bConvertToActorLocalSpace 表达，数据本就是局部空间的 leaf 只能靠默认 false 绕开，
	// 语义含混；现在由 MeshData.SourceSpace 说明数据自己是什么空间。
	const bool bNeedsBake = Options.bBakeToLocalSpace
		&& MeshData.SourceSpace == FCSGpuMeshCPUData::ESpace::World;
	return BuildGpuMeshDescription(
		MeshData, Options.TargetTransform, bNeedsBake, OutMeshDescription);
}

UStaticMesh* UCSGpuMeshComponent::BuildStaticMesh(
	UObject* Outer,
	const AActor* OwnerActor,
	const FCSGpuMeshCPUData& MeshData,
	const TArray<UMaterialInterface*>& Materials,
	const FCSGpuMeshConvertOptions& Options,
	const FCSGpuMeshAssetOptions& AssetOptions)
{
	// 材质表优先取数据自带的 MeshData.Materials；调用方仍可用 Materials 参数覆盖（迁移期的
	// 旧调用点走这条）。两者都空时下面的兜底会按槽位数补默认材质。
	TArray<UMaterialInterface*> ResolvedMaterials;
	if (Materials.Num() > 0)
	{
		ResolvedMaterials = Materials;
	}
	else
	{
		ResolvedMaterials.Reserve(MeshData.Materials.Num());
		for (const TObjectPtr<UMaterialInterface>& Material : MeshData.Materials) ResolvedMaterials.Add(Material.Get());
	}

	// 槽位数至少要覆盖 TriangleMaterialSlots 里出现过的最大槽号，否则落盘时会因槽位缺失而丢面。
	int32 RequiredSlots = ResolvedMaterials.Num();
	for (int32 Slot : MeshData.TriangleMaterialSlots) RequiredSlots = FMath::Max(RequiredSlots, Slot + 1);
	if (RequiredSlots > ResolvedMaterials.Num()) ResolvedMaterials.SetNumZeroed(RequiredSlots);

	// 空槽兜底：注册表允许材质为空（源网格未指定），但输出网格带空槽会渲染成默认灰，
	// 且在编辑器里无法区分「未指定」与「指定了灰材质」。统一填引擎默认表面材质，
	// 槽位数量和顺序不变，用户仍可逐槽替换。
	if (Options.bFillEmptySlotsWithDefaultMaterial)
	{
		UMaterialInterface* DefaultMaterial = GetDefaultSurfaceMaterial();
		for (UMaterialInterface*& Material : ResolvedMaterials)
		{
			if (!Material) Material = DefaultMaterial;
		}
	}

	// 与 BuildMeshDescription 同一判据：数据已是局部空间就不再变换。
	const bool bNeedsBake = Options.bBakeToLocalSpace
		&& MeshData.SourceSpace == FCSGpuMeshCPUData::ESpace::World;

	if (AssetOptions.bTransient)
	{
		return BuildTransientStaticMesh(
			Outer, MeshData, ResolvedMaterials, Options.TargetTransform, bNeedsBake, AssetOptions.bEnableNanite);
	}

#if WITH_EDITOR
	return SaveGpuMeshDataToStaticMesh(
		OwnerActor, MeshData, ResolvedMaterials, Options.TargetTransform, bNeedsBake,
		AssetOptions.AssetPath, AssetOptions.bReplaceExisting, AssetOptions.bSaveToDisk,
		AssetOptions.bEnableNanite);
#else
	// 非编辑器构建没有资产系统，只能退回 transient。
	return BuildTransientStaticMesh(
		Outer, MeshData, ResolvedMaterials, Options.TargetTransform, bNeedsBake, AssetOptions.bEnableNanite);
#endif
}
