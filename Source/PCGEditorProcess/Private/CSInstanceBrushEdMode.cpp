#include "CSInstanceBrushEdMode.h"

#include "MeshGeneratorBrushCache.h"

#include "Components/BrushComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/ModelComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/CollisionProfile.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FoliageHelper.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "InputCoreTypes.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "SceneView.h"

const FEditorModeID FCSInstanceBrushEdMode::EM_CSInstanceBrush = TEXT("CSInstanceBrushEdMode");

namespace
{
struct FCSInstanceBrushGeometryFilter
{
	TWeakObjectPtr<AComputeShaderMeshGenerator> TargetActor;
	bool bAllowLandscape = true;
	bool bAllowStaticMesh = true;
	bool bAllowBSP = false;
	bool bAllowFoliage = false;
	bool bAllowTranslucent = false;
	bool bAllowTargetPaintedInstancesAsSurface = false;

	bool operator()(const UPrimitiveComponent* Component) const
	{
		if (!Component)
		{
			return false;
		}

		const AActor* Owner = Component->GetOwner();
		const bool bFoliageOwned = Owner && FFoliageHelper::IsOwnedByFoliage(Owner);
		if (!bAllowTargetPaintedInstancesAsSurface && Owner == TargetActor.Get() && Component->IsA<UInstancedStaticMeshComponent>())
		{
			return false;
		}

		bool bAllowed =
			(bAllowLandscape && Component->IsA<ULandscapeHeightfieldCollisionComponent>()) ||
			(bAllowStaticMesh && Component->IsA<UStaticMeshComponent>() && !Component->IsA<UFoliageInstancedStaticMeshComponent>() && !bFoliageOwned) ||
			(bAllowBSP && (Component->IsA<UBrushComponent>() || Component->IsA<UModelComponent>())) ||
			(bAllowFoliage && (Component->IsA<UFoliageInstancedStaticMeshComponent>() || bFoliageOwned));

		const UMaterialInterface* Material = Component->GetMaterial(0);
		bAllowed &= bAllowTranslucent || !(Material && IsTranslucentBlendMode(*Material));

		return bAllowed;
	}
};

FCSInstanceBrushGeometryFilter MakeBrushGeometryFilter(AMeshGeneratorBrushCache* TargetActor)
{
	FCSInstanceBrushGeometryFilter Filter;
	Filter.TargetActor = TargetActor;
	return Filter;
}
}

FCSInstanceBrushEdMode::FCSInstanceBrushEdMode()
{
	CreateBrushComponent();
}

FCSInstanceBrushEdMode::~FCSInstanceBrushEdMode()
{
	DestroyBrushComponent();
}

void FCSInstanceBrushEdMode::SetTargetActor(AMeshGeneratorBrushCache* InTargetActor)
{
	TargetActor = InTargetActor;
	CancelStroke();
	bBrushTraceValid = false;
}

void FCSInstanceBrushEdMode::Enter()
{
	FEdMode::Enter();
	CreateBrushComponent();
}

void FCSInstanceBrushEdMode::Exit()
{
	CancelStroke();
	DestroyBrushComponent();
	TargetActor.Reset();
	FEdMode::Exit();
}

void FCSInstanceBrushEdMode::AddReferencedObjects(FReferenceCollector& Collector)
{
	FEdMode::AddReferencedObjects(Collector);
	Collector.AddReferencedObject(SphereBrushComponent);
	Collector.AddReferencedObject(BrushMID);
}

bool FCSInstanceBrushEdMode::MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y)
{
	UpdateBrushTraceFromMouse(ViewportClient, Viewport, x, y);
	UpdateBrushComponent(ViewportClient);
	if (bStrokeActive)
	{
		UpdateStroke();
		return true;
	}
	return false;
}

bool FCSInstanceBrushEdMode::CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y)
{
	UpdateBrushTraceFromMouse(ViewportClient, Viewport, x, y);
	UpdateBrushComponent(ViewportClient);
	if (bStrokeActive)
	{
		UpdateStroke();
		return true;
	}
	return false;
}

bool FCSInstanceBrushEdMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (!TargetActor.IsValid())
	{
		return false;
	}

	if (Key == EKeys::Escape && Event == IE_Pressed)
	{
		CancelStroke();
		ExitTemporaryMode();
		return true;
	}

	if (Key == EKeys::LeftMouseButton && Event == IE_Pressed)
	{
		const bool bMovingCamera =
			Viewport->KeyState(EKeys::MiddleMouseButton) ||
			Viewport->KeyState(EKeys::RightMouseButton) ||
			Viewport->KeyState(EKeys::LeftAlt) ||
			Viewport->KeyState(EKeys::RightAlt);

		if (!bMovingCamera && ViewportClient->GetCurrentWidgetAxis() == EAxisList::None)
		{
			BeginStroke();
			return true;
		}
	}

	if (Key == EKeys::LeftMouseButton && Event == IE_Released && bStrokeActive)
	{
		CommitStroke();
		return true;
	}

	return FEdMode::InputKey(ViewportClient, Viewport, Key, Event);
}

void FCSInstanceBrushEdMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	FEdMode::Tick(ViewportClient, DeltaTime);
	UpdateBrushComponent(ViewportClient);
	DrawPendingPreviewPoints();
}

void FCSInstanceBrushEdMode::CreateBrushComponent()
{
	if (SphereBrushComponent)
	{
		return;
	}

	UStaticMesh* BrushMesh = nullptr;
	UMaterial* BrushMaterial = nullptr;
	if (!IsRunningCommandlet())
	{
		BrushMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EditorLandscapeResources/FoliageBrushSphereMaterial.FoliageBrushSphereMaterial"));
		BrushMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Sphere.Sphere"));
	}

	BrushMID = BrushMaterial ? UMaterialInstanceDynamic::Create(BrushMaterial, GetTransientPackage()) : nullptr;
	SphereBrushComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), TEXT("CSInstanceBrushSphere"));
	SphereBrushComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SphereBrushComponent->SetCollisionObjectType(ECC_WorldDynamic);
	SphereBrushComponent->SetStaticMesh(BrushMesh);
	if (BrushMID)
	{
		SphereBrushComponent->SetMaterial(0, BrushMID);
	}
	SphereBrushComponent->SetAbsolute(true, true, true);
	SphereBrushComponent->CastShadow = false;
	SphereBrushComponent->SetVisibility(false);
}

void FCSInstanceBrushEdMode::DestroyBrushComponent()
{
	if (SphereBrushComponent && SphereBrushComponent->IsRegistered())
	{
		SphereBrushComponent->UnregisterComponent();
	}
	SphereBrushComponent = nullptr;
	BrushMID = nullptr;
}

void FCSInstanceBrushEdMode::UpdateBrushComponent(FEditorViewportClient* ViewportClient)
{
	if (!SphereBrushComponent)
	{
		CreateBrushComponent();
	}

	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!SphereBrushComponent || !World || !ViewportClient || !bBrushTraceValid)
	{
		if (SphereBrushComponent && SphereBrushComponent->IsRegistered())
		{
			SphereBrushComponent->SetVisibility(false);
		}
		return;
	}

	const float BrushRadius = FMath::Max(1.0f, Target->InstanceBrushRadius);
	SphereBrushComponent->SetRelativeTransform(FTransform(FQuat::Identity, BrushLocation, FVector(BrushRadius * 0.00625f)));
	SphereBrushComponent->SetVisibility(true);
	if (!SphereBrushComponent->IsRegistered())
	{
		SphereBrushComponent->RegisterComponentWithWorld(World);
	}
}

bool FCSInstanceBrushEdMode::UpdateBrushTraceFromMouse(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 MouseX, int32 MouseY)
{
	if (!ViewportClient || !Viewport)
	{
		bBrushTraceValid = false;
		return false;
	}

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		ViewportClient->GetScene(),
		ViewportClient->EngineShowFlags)
		.SetRealtimeUpdate(ViewportClient->IsRealtime()));

	FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);
	FViewportCursorLocation MouseViewportRay(View, ViewportClient, MouseX, MouseY);
	BrushTraceDirection = MouseViewportRay.GetDirection();

	FVector BrushTraceStart = MouseViewportRay.GetOrigin();
	if (ViewportClient->IsOrtho())
	{
		BrushTraceStart += -WORLD_MAX * BrushTraceDirection;
	}

	return TraceBrushRay(ViewportClient, BrushTraceStart, BrushTraceDirection);
}

bool FCSInstanceBrushEdMode::TraceBrushRay(FEditorViewportClient* ViewportClient, const FVector& RayOrigin, const FVector& RayDirection)
{
	bBrushTraceValid = false;
	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World || !ViewportClient || ViewportClient->IsMovingCamera() || !ViewportClient->IsVisible())
	{
		return false;
	}

	const FVector TraceStart = RayOrigin;
	const FVector TraceEnd = RayOrigin + RayDirection * HALF_WORLD_MAX;
	FHitResult Hit;
	if (TraceCandidatePoint(TraceStart, TraceEnd, Hit) && Target->IsInstanceBrushPointAllowed(Hit.Location))
	{
		BrushLocation = Hit.Location;
		BrushNormal = Hit.Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		bBrushTraceValid = true;
		return true;
	}

	return false;
}

bool FCSInstanceBrushEdMode::TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	FCSInstanceBrushGeometryFilter Filter = MakeBrushGeometryFilter(Target);
	FFoliageTraceFilterFunc TraceFilterFunc = [Filter](const UPrimitiveComponent* Component)
	{
		return Filter(Component);
	};

	static const FName NAME_CSInstanceBrush(TEXT("CSInstanceBrush"));
	const FDesiredFoliageInstance DesiredInstance(Start, End, nullptr, FMath::Max(0.0f, Target->InstanceBrushTraceRadius));
	return AInstancedFoliageActor::FoliageTrace(
		World,
		OutHit,
		DesiredInstance,
		NAME_CSInstanceBrush,
		false,
		TraceFilterFunc,
		true);
}

void FCSInstanceBrushEdMode::BeginStroke()
{
	CancelStroke();
	bStrokeActive = true;
	SamplePreviewPoints();
	DrawPendingPreviewPoints();
}

void FCSInstanceBrushEdMode::UpdateStroke()
{
	SamplePreviewPoints();
	DrawPendingPreviewPoints();
}

void FCSInstanceBrushEdMode::CommitStroke()
{
	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (Target && Target->InstanceBrushMesh && !PendingTransforms.IsEmpty())
	{
		const int32 AddedCount = Target->CommitPaintInstances(PendingTransforms, Target->InstanceBrushMesh);
		UE_LOG(LogTemp, Log, TEXT("[CSInstanceBrush] Added %d instances to %s."), AddedCount, *GetNameSafe(Target));
	}

	const bool bExitAfterCommit = Target && Target->bInstanceBrushExitAfterCommit;
	CancelStroke();
	if (bExitAfterCommit)
	{
		ExitTemporaryMode();
	}
}

void FCSInstanceBrushEdMode::CancelStroke()
{
	bStrokeActive = false;
	PendingPreviewPoints.Reset();
	PendingTransforms.Reset();
}

void FCSInstanceBrushEdMode::ExitTemporaryMode()
{
	if (FEditorModeTools* ModeTools = GetModeManager())
	{
		ModeTools->DeactivateMode(EM_CSInstanceBrush);
	}
}

void FCSInstanceBrushEdMode::SamplePreviewPoints()
{
	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target || !Target->InstanceBrushMesh || !bBrushTraceValid)
	{
		return;
	}

	const int32 SampleCount = FMath::Max(1, Target->InstanceBrushSamplesPerMouseMove);
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		GetRandomVectorInBrush(Start, End);

		FHitResult Hit;
		if (!TraceCandidatePoint(Start, End, Hit))
		{
			continue;
		}

		if (!IsCandidatePointAllowed(Hit.Location))
		{
			continue;
		}

		FCSInstanceBrushPreviewPoint& PreviewPoint = PendingPreviewPoints.AddDefaulted_GetRef();
		PreviewPoint.Location = Hit.Location;
		PreviewPoint.Normal = Hit.Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		PreviewPoint.WorldTransform = BuildInstanceTransform(Hit);
		PendingTransforms.Add(PreviewPoint.WorldTransform);
	}
}

bool FCSInstanceBrushEdMode::IsCandidatePointAllowed(const FVector& Location) const
{
	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target || !Target->IsInstanceBrushPointAllowed(Location))
	{
		return false;
	}

	const float MinSpacing = FMath::Max(0.0f, Target->InstanceBrushMinSpacing);
	if (MinSpacing <= 0.0f)
	{
		return true;
	}

	const float MinSpacingSq = FMath::Square(MinSpacing);
	return !IsTooCloseToPendingPoint(Location, MinSpacingSq) && !IsTooCloseToExistingInstance(Location, MinSpacingSq);
}

bool FCSInstanceBrushEdMode::IsTooCloseToPendingPoint(const FVector& Location, float MinSpacingSq) const
{
	for (const FCSInstanceBrushPreviewPoint& PreviewPoint : PendingPreviewPoints)
	{
		if (FVector::DistSquared(PreviewPoint.Location, Location) < MinSpacingSq)
		{
			return true;
		}
	}
	return false;
}

bool FCSInstanceBrushEdMode::IsTooCloseToExistingInstance(const FVector& Location, float MinSpacingSq) const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target || !Target->InstanceBrushMesh)
	{
		return false;
	}

	const UHierarchicalInstancedStaticMeshComponent* PaintComponent = Target->FindPaintComponent(Target->InstanceBrushMesh);
	if (!PaintComponent)
	{
		return false;
	}

	for (int32 InstanceIndex = 0; InstanceIndex < PaintComponent->GetInstanceCount(); ++InstanceIndex)
	{
		FTransform InstanceTransform = FTransform::Identity;
		if (PaintComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true) &&
			FVector::DistSquared(InstanceTransform.GetLocation(), Location) < MinSpacingSq)
		{
			return true;
		}
	}

	return false;
}

FTransform FCSInstanceBrushEdMode::BuildInstanceTransform(const FHitResult& Hit) const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target)
	{
		return FTransform::Identity;
	}

	const FVector Normal = Hit.Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const float RandomYawDegrees = FMath::FRandRange(0.0f, FMath::Max(0.0f, Target->InstanceBrushRandomYawDegrees));
	const FQuat Rotation = Target->bInstanceBrushAlignToNormal
		? FQuat(Normal, FMath::DegreesToRadians(RandomYawDegrees)) * FRotationMatrix::MakeFromZ(Normal).ToQuat()
		: FRotator(0.0f, RandomYawDegrees, 0.0f).Quaternion();

	const float MinScale = FMath::Min(Target->InstanceBrushUniformScaleRange.X, Target->InstanceBrushUniformScaleRange.Y);
	const float MaxScale = FMath::Max(Target->InstanceBrushUniformScaleRange.X, Target->InstanceBrushUniformScaleRange.Y);
	const float SafeMinScale = FMath::Max(UE_KINDA_SMALL_NUMBER, MinScale);
	const float SafeMaxScale = FMath::Max(SafeMinScale, MaxScale);
	const float UniformScale = FMath::FRandRange(SafeMinScale, SafeMaxScale);

	return FTransform(Rotation, Hit.Location, FVector(UniformScale));
}

void FCSInstanceBrushEdMode::GetRandomVectorInBrush(FVector& OutStart, FVector& OutEnd) const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	const float BrushRadius = Target ? FMath::Max(1.0f, Target->InstanceBrushRadius) : 1.0f;

	const float Ru = (2.0f * FMath::FRand()) - 1.0f;
	const float Rv = ((2.0f * FMath::FRand()) - 1.0f) * FMath::Sqrt(FMath::Max(1.0f - FMath::Square(Ru), 0.0f));

	FVector U = FVector::ForwardVector;
	FVector V = FVector::RightVector;
	BrushNormal.FindBestAxisVectors(U, V);
	const FVector Point = (Ru * U) + (Rv * V);
	const FVector Rw = FMath::Sqrt(FMath::Max(1.0f - (FMath::Square(Ru) + FMath::Square(Rv)), 0.001f)) * BrushNormal;

	OutStart = BrushLocation + BrushRadius * (Point + Rw);
	OutEnd = BrushLocation + BrushRadius * (Point - Rw);
}

void FCSInstanceBrushEdMode::DrawPendingPreviewPoints() const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World || PendingPreviewPoints.IsEmpty())
	{
		return;
	}

	const float PreviewPointSize = FMath::Max(0.1f, Target->InstanceBrushPreviewPointSize);
	const float PreviewLifetime = FMath::Max(0.01f, Target->InstanceBrushPreviewLifetime);
	for (const FCSInstanceBrushPreviewPoint& PreviewPoint : PendingPreviewPoints)
	{
		DrawDebugPoint(World, PreviewPoint.Location, PreviewPointSize, FColor::Cyan, false, PreviewLifetime, SDPG_World);
	}
}
