#include "EnhancedHairCardsComponent.h"
#include "EnhancedHairCardsAsset.h"
#include "EnhancedHairCardsSceneProxy.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "GroomComponent.h"
#include "GroomInstance.h"
#include "GroomResources.h"
#include "Materials/MaterialInterface.h"
#include "RenderingThread.h"

namespace EnhancedHairCardsComponent
{
	static bool IsValidGuideCurveForSimulation(const FEnhancedHairCardsGuideCurve& GuideCurve)
	{
		return GuideCurve.Points.Num() >= 2;
	}
}

UEnhancedHairCardsComponent::UEnhancedHairCardsComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	bTickInEditor = true;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	bUseAsOccluder = false;
	CachedLocalBounds = FBoxSphereBounds(ForceInit);
}

void UEnhancedHairCardsComponent::OnRegister()
{
	Super::OnRegister();

	if (!SourceGroomComponent)
	{
		if (AActor* Owner = GetOwner())
		{
			TArray<UGroomComponent*> GroomComponents;
			Owner->GetComponents(GroomComponents);
			for (UGroomComponent* GroomComponent : GroomComponents)
			{
				if (GroomComponent)
				{
					SourceGroomComponent = GroomComponent;
					break;
				}
			}
		}
	}

	for (int32 i = 0; i < CustomPrimitiveFloats.Num(); ++i)
	{
		SetCustomPrimitiveDataFloat(i, CustomPrimitiveFloats[i]);
	}
	bLastSourceGroomCardsResourceUsable = HasUsableSourceGroomCardsResource();
	UpdateGuideSimulationTickState();
}

#if WITH_EDITOR
void UEnhancedHairCardsComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	InvalidateCachedLocalBounds();
	RebuildGuideSimulationState(true);
	UpdateGuideSimulationTickState();
	MarkRenderStateDirty();
}
#endif

void UEnhancedHairCardsComponent::SetCustomDataFloat(int32 Index, float Value)
{
	if (Index >= 0 && Index < 32)
	{
		while (CustomPrimitiveFloats.Num() <= Index)
		{
			CustomPrimitiveFloats.Add(0.f);
		}
		CustomPrimitiveFloats[Index] = Value;
		SetCustomPrimitiveDataFloat(Index, Value);
	}
}

void UEnhancedHairCardsComponent::SetDynamicsEnabled(bool bEnabled)
{
	CardSettings.Dynamics.bEnabled = bEnabled;
	UpdateGuideSimulationTickState();
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::SetDynamicsStrength(float Strength)
{
	CardSettings.Dynamics.Strength = FMath::Max(0.f, Strength);
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::SetDynamicsWind(FVector LocalDirection, float WindStrength)
{
	if (!LocalDirection.Normalize())
	{
		LocalDirection = FVector(0.f, 1.f, 0.f);
	}

	CardSettings.Dynamics.LocalWindDirection = LocalDirection;
	CardSettings.Dynamics.WindStrength = FMath::Max(0.f, WindStrength);
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::SetGuideSimulationEnabled(bool bEnabled)
{
	CardSettings.Dynamics.bGuideSimulationEnabled = bEnabled;
	if (bEnabled)
	{
		RebuildGuideSimulationState(false);
	}
	UpdateGuideSimulationTickState();
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::ResetGuideSimulation()
{
	RebuildGuideSimulationState(true);
	MarkRenderDynamicDataDirty();
}

void UEnhancedHairCardsComponent::SetSourceGroomComponent(UGroomComponent* InSourceGroomComponent)
{
	if (SourceGroomComponent == InSourceGroomComponent)
	{
		return;
	}

	SourceGroomComponent = InSourceGroomComponent;
	UpdateGuideSimulationTickState();
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::SetGuideDebugEnabled(bool bEnabled)
{
	CardSettings.GuideDebug.bDrawGuides = bEnabled;
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::ApplyCardSettings(const FEnhancedHairCardsSettings& InCardSettings, bool bResetGuideSimulation)
{
	const bool bWasGuideSimulationEnabled = CardSettings.Dynamics.bGuideSimulationEnabled;
	CardSettings = InCardSettings;

	const bool bShouldResetGuideSimulation = bResetGuideSimulation
		|| (!bWasGuideSimulationEnabled && CardSettings.Dynamics.bGuideSimulationEnabled);
	if (bShouldResetGuideSimulation)
	{
		RebuildGuideSimulationState(true);
	}
	else if (CardSettings.Dynamics.bGuideSimulationEnabled)
	{
		RebuildGuideSimulationState(false);
	}

	UpdateGuideSimulationTickState();
	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::ApplyHairCardsPart(const FEnhancedHairCardsPart& Part)
{
	SourceMesh = Part.SourceMesh;
	SourceGroupIndex = Part.SourceGroupIndex;
	SourceLODIndex = Part.SourceLODIndex;
	SourceCardsDescriptionIndex = Part.SourceCardsDescriptionIndex;
	GuideCurves = Part.GuideCurves;
	InvalidateCachedLocalBounds();
	ApplyCardSettings(Part.CardSettings, true);

	OverrideMaterials.Reset();
	if (Part.Material)
	{
		SetMaterial(0, Part.Material);
	}

	MarkRenderStateDirty();
}

void UEnhancedHairCardsComponent::RebuildRenderData()
{
	MarkRenderStateDirty();
}

FPrimitiveSceneProxy* UEnhancedHairCardsComponent::CreateSceneProxy()
{
	if (!SourceMesh || !SourceMesh->GetRenderData()
		|| SourceMesh->GetRenderData()->LODResources.Num() == 0)
	{
		return nullptr;
	}
	return new FEnhancedHairCardsSceneProxy(this);
}

int32 UEnhancedHairCardsComponent::GetNumMaterials() const
{
	const int32 SourceMaterialCount = SourceMesh ? SourceMesh->GetStaticMaterials().Num() : 0;
	return FMath::Max3(1, SourceMaterialCount, GetNumOverrideMaterials());
}

UMaterialInterface* UEnhancedHairCardsComponent::GetMaterial(int32 ElementIndex) const
{
	UMaterialInterface* Mat = (OverrideMaterials.IsValidIndex(ElementIndex) && OverrideMaterials[ElementIndex]) ? OverrideMaterials[ElementIndex].Get() : nullptr;
	if (!Mat && SourceMesh)
	{
		Mat = SourceMesh->GetMaterial(ElementIndex);
	}
	return Mat;
}

FBoxSphereBounds UEnhancedHairCardsComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBoxSphereBounds LocalBounds = GetCachedLocalBounds();
	if (LocalBounds.SphereRadius > 0.f)
	{
		return LocalBounds.ExpandBy(CardSettings.Dynamics.GetMaxDisplacement()).TransformBy(LocalToWorld);
	}

	return FBoxSphereBounds(ForceInit);
}

void UEnhancedHairCardsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const bool bSourceGroomCardsResourceUsable = HasUsableSourceGroomCardsResource();
	if (bLastSourceGroomCardsResourceUsable != bSourceGroomCardsResourceUsable)
	{
		bLastSourceGroomCardsResourceUsable = bSourceGroomCardsResourceUsable;
		UpdateGuideSimulationTickState();
		MarkRenderStateDirty();
	}

	if (SimulateGuideCurves(DeltaTime))
	{
		MarkRenderDynamicDataDirty();
	}
}

void UEnhancedHairCardsComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	if (!SceneProxy || !ShouldRunGuideSimulation())
	{
		return;
	}

	TArray<FEnhancedHairCardsGuideCurve> GuideCurvesForRender = SimulatedGuideCurves;
	FEnhancedHairCardsSceneProxy* HairCardsSceneProxy = static_cast<FEnhancedHairCardsSceneProxy*>(SceneProxy);
	ENQUEUE_RENDER_COMMAND(FEnhancedHairCardsUpdateSimulatedGuides)(
		[HairCardsSceneProxy, GuideCurvesForRender = MoveTemp(GuideCurvesForRender)](FRHICommandListImmediate& RHICmdList) mutable
		{
			HairCardsSceneProxy->UpdateSimulatedGuideCurves_RenderThread(RHICmdList, GuideCurvesForRender);
		});
}

void UEnhancedHairCardsComponent::InvalidateCachedLocalBounds()
{
	bCachedLocalBoundsValid = false;
}

FBoxSphereBounds UEnhancedHairCardsComponent::GetCachedLocalBounds() const
{
	if (bCachedLocalBoundsValid)
	{
		return CachedLocalBounds;
	}

	FBox LocalBox(ForceInit);
	if (SourceMesh)
	{
		LocalBox += SourceMesh->GetBoundingBox();
	}

	for (const FEnhancedHairCardsGuideCurve& GuideCurve : GuideCurves)
	{
		for (const FVector& Point : GuideCurve.Points)
		{
			LocalBox += Point;
		}
	}

	if (LocalBox.IsValid)
	{
		CachedLocalBounds = FBoxSphereBounds(LocalBox);
	}
	else
	{
		CachedLocalBounds = FBoxSphereBounds(ForceInit);
	}

	bCachedLocalBoundsValid = true;
	return CachedLocalBounds;
}

bool UEnhancedHairCardsComponent::ShouldRunGuideSimulation() const
{
	if (ShouldUseSourceGroomSimulation())
	{
		return false;
	}

	return CardSettings.Dynamics.bGuideSimulationEnabled
		&& GuideCurves.ContainsByPredicate(&EnhancedHairCardsComponent::IsValidGuideCurveForSimulation);
}

bool UEnhancedHairCardsComponent::ShouldUseSourceGroomSimulation() const
{
	return bUseSourceGroomSimulation && HasUsableSourceGroomCardsResource();
}

bool UEnhancedHairCardsComponent::HasUsableSourceGroomCardsResource() const
{
	const UGroomComponent* GroomComponent = SourceGroomComponent.Get();
	if (!GroomComponent)
	{
		return false;
	}

	const int32 GroupIndex = SourceGroupIndex;
	const int32 LODIndex = FMath::Max(SourceLODIndex, 0);
	const FHairGroupInstance* HairGroupInstance = GroomComponent->GetGroupInstance(GroupIndex);
	if (!HairGroupInstance || !HairGroupInstance->Cards.IsValid(LODIndex))
	{
		return false;
	}

	const FHairGroupInstance::FCards::FLOD& CardsLOD = HairGroupInstance->Cards.LODs[LODIndex];
	if (!CardsLOD.RestResource
		|| !CardsLOD.DeformedResource
		|| CardsLOD.RestResource->GetVertexCount() == 0
		|| CardsLOD.RestResource->GetPrimitiveCount() == 0)
	{
		return false;
	}

	return CardsLOD.DeformedResource->GetBuffer(FHairCardsDeformedResource::Current).SRV
		&& CardsLOD.DeformedResource->GetBuffer(FHairCardsDeformedResource::Previous).SRV
		&& CardsLOD.DeformedResource->DeformedNormalBuffer.SRV
		&& CardsLOD.RestResource->UVsBuffer.ShaderResourceViewRHI
		&& CardsLOD.RestResource->MaterialsBuffer.ShaderResourceViewRHI
		&& CardsLOD.RestResource->RestIndexBuffer.IndexBufferRHI;
}

void UEnhancedHairCardsComponent::UpdateGuideSimulationTickState()
{
	SetComponentTickEnabled(ShouldRunGuideSimulation() || bUseSourceGroomSimulation);
}

void UEnhancedHairCardsComponent::RebuildGuideSimulationState(bool bForceReset)
{
	if (!bForceReset && bGuideSimulationInitialized)
	{
		return;
	}

	GuideSimulationCurveStates.Reset();
	RestSimulationGuideCurves.Reset();
	SimulatedGuideCurves.Reset();
	SimulatedPreviousPoints.Reset();
	LastGuideSimulationComponentTransform = GetComponentTransform();

	int32 PointOffset = 0;
	for (const FEnhancedHairCardsGuideCurve& GuideCurve : GuideCurves)
	{
		if (!EnhancedHairCardsComponent::IsValidGuideCurveForSimulation(GuideCurve))
		{
			continue;
		}

		RestSimulationGuideCurves.Add(GuideCurve);
		FEnhancedHairCardsGuideCurve& SimulatedCurve = SimulatedGuideCurves.Add_GetRef(GuideCurve);

		FGuideSimulationCurveState& CurveState = GuideSimulationCurveStates.AddDefaulted_GetRef();
		CurveState.PointOffset = PointOffset;
		CurveState.PointCount = SimulatedCurve.Points.Num();

		for (const FVector& Point : SimulatedCurve.Points)
		{
			SimulatedPreviousPoints.Add(Point);
		}

		PointOffset += SimulatedCurve.Points.Num();
	}

	bGuideSimulationInitialized = true;
}

bool UEnhancedHairCardsComponent::SimulateGuideCurves(float DeltaTime)
{
	if (DeltaTime <= UE_SMALL_NUMBER || !ShouldRunGuideSimulation())
	{
		return false;
	}

	RebuildGuideSimulationState(false);
	if (SimulatedGuideCurves.Num() == 0)
	{
		return false;
	}

	const FEnhancedHairCardsDynamicsSettings& Dynamics = CardSettings.Dynamics;
	const float ClampedDeltaTime = FMath::Clamp(DeltaTime, 1.0f / 240.0f, 1.0f / 30.0f);
	const float DampingMultiplier = FMath::Clamp(1.0f - Dynamics.GuideDamping, 0.0f, 1.0f);
	const float Stiffness = FMath::Max(0.0f, Dynamics.GuideStiffness);
	const float MotionInertia = FMath::Clamp(Dynamics.GuideMotionInertia, 0.0f, 1.0f);
	const FTransform CurrentComponentTransform = GetComponentTransform();

	if (MotionInertia > UE_SMALL_NUMBER && !LastGuideSimulationComponentTransform.Equals(CurrentComponentTransform, UE_KINDA_SMALL_NUMBER))
	{
		for (int32 CurveIndex = 0; CurveIndex < SimulatedGuideCurves.Num(); ++CurveIndex)
		{
			if (!GuideSimulationCurveStates.IsValidIndex(CurveIndex) || !RestSimulationGuideCurves.IsValidIndex(CurveIndex))
			{
				continue;
			}

			FEnhancedHairCardsGuideCurve& SimulatedCurve = SimulatedGuideCurves[CurveIndex];
			const FEnhancedHairCardsGuideCurve& RestCurve = RestSimulationGuideCurves[CurveIndex];
			const FGuideSimulationCurveState& CurveState = GuideSimulationCurveStates[CurveIndex];
			for (int32 PointIndex = 1; PointIndex < SimulatedCurve.Points.Num(); ++PointIndex)
			{
				const int32 FlatPointIndex = CurveState.PointOffset + PointIndex;
				if (!SimulatedPreviousPoints.IsValidIndex(FlatPointIndex) || !RestCurve.Points.IsValidIndex(PointIndex))
				{
					continue;
				}

				const FVector InertialCurrent = CurrentComponentTransform.InverseTransformPosition(
					LastGuideSimulationComponentTransform.TransformPosition(SimulatedCurve.Points[PointIndex]));
				const FVector InertialPrevious = CurrentComponentTransform.InverseTransformPosition(
					LastGuideSimulationComponentTransform.TransformPosition(SimulatedPreviousPoints[FlatPointIndex]));

				SimulatedCurve.Points[PointIndex] = FMath::Lerp(RestCurve.Points[PointIndex], InertialCurrent, MotionInertia);
				SimulatedPreviousPoints[FlatPointIndex] = FMath::Lerp(RestCurve.Points[PointIndex], InertialPrevious, MotionInertia);
			}
		}
	}

	LastGuideSimulationComponentTransform = CurrentComponentTransform;

	FVector LocalWindDirection = Dynamics.LocalWindDirection;
	if (!LocalWindDirection.Normalize())
	{
		LocalWindDirection = FVector(0.0, 1.0, 0.0);
	}

	float ElapsedTime = 0.0f;
	if (UWorld* World = GetWorld())
	{
		ElapsedTime = (float)World->GetTimeSeconds();
	}

	const FVector WindAcceleration = LocalWindDirection * Dynamics.WindStrength * Dynamics.Strength * 60.0;
	const FVector GravityAcceleration = FVector(0.0, 0.0, -Dynamics.GravityStrength * Dynamics.Strength * 60.0);
	const int32 ConstraintIterations = FMath::Clamp(Dynamics.GuideConstraintIterations, 0, 16);
	bool bMoved = false;

	for (int32 CurveIndex = 0; CurveIndex < SimulatedGuideCurves.Num(); ++CurveIndex)
	{
		if (!GuideSimulationCurveStates.IsValidIndex(CurveIndex) || !RestSimulationGuideCurves.IsValidIndex(CurveIndex))
		{
			continue;
		}

		FEnhancedHairCardsGuideCurve& SimulatedCurve = SimulatedGuideCurves[CurveIndex];
		const FEnhancedHairCardsGuideCurve& RestCurve = RestSimulationGuideCurves[CurveIndex];
		const FGuideSimulationCurveState& CurveState = GuideSimulationCurveStates[CurveIndex];
		if (SimulatedCurve.Points.Num() != RestCurve.Points.Num()
			|| CurveState.PointCount != SimulatedCurve.Points.Num()
			|| !SimulatedPreviousPoints.IsValidIndex(CurveState.PointOffset + CurveState.PointCount - 1))
		{
			continue;
		}

		SimulatedCurve.Points[0] = RestCurve.Points[0];
		SimulatedPreviousPoints[CurveState.PointOffset] = RestCurve.Points[0];

		for (int32 PointIndex = 1; PointIndex < SimulatedCurve.Points.Num(); ++PointIndex)
		{
			const int32 FlatPointIndex = CurveState.PointOffset + PointIndex;
			const float RootToTip = (float)PointIndex / (float)FMath::Max(SimulatedCurve.Points.Num() - 1, 1);
			const float Mask = FMath::Pow(FMath::Clamp(RootToTip, 0.0f, 1.0f), FMath::Max(Dynamics.TipPower, 0.01f));
			const float FlutterPhase = ElapsedTime * Dynamics.FlutterFrequency * UE_TWO_PI
				+ (float)CurveIndex * 0.731f
				+ (float)PointIndex * 0.173f;
			const FVector FlutterAcceleration =
				LocalWindDirection.RotateAngleAxis(90.0f, FVector::UpVector)
				* FMath::Sin(FlutterPhase)
				* Dynamics.FlutterStrength
				* Dynamics.Strength
				* 60.0
				* Mask;
			const FVector SpringAcceleration = (RestCurve.Points[PointIndex] - SimulatedCurve.Points[PointIndex]) * Stiffness;
			const FVector TotalAcceleration = (WindAcceleration + GravityAcceleration) * Mask + FlutterAcceleration + SpringAcceleration;
			const FVector CurrentPosition = SimulatedCurve.Points[PointIndex];
			const FVector PreviousPosition = SimulatedPreviousPoints[FlatPointIndex];
			FVector NewPosition = CurrentPosition + (CurrentPosition - PreviousPosition) * DampingMultiplier + TotalAcceleration * ClampedDeltaTime * ClampedDeltaTime;

			SimulatedPreviousPoints[FlatPointIndex] = CurrentPosition;
			SimulatedCurve.Points[PointIndex] = NewPosition;
			bMoved = bMoved || !NewPosition.Equals(CurrentPosition, UE_KINDA_SMALL_NUMBER);
		}

		for (int32 IterationIndex = 0; IterationIndex < ConstraintIterations; ++IterationIndex)
		{
			SimulatedCurve.Points[0] = RestCurve.Points[0];
			for (int32 PointIndex = 1; PointIndex < SimulatedCurve.Points.Num(); ++PointIndex)
			{
				const FVector PreviousPoint = SimulatedCurve.Points[PointIndex - 1];
				FVector Segment = SimulatedCurve.Points[PointIndex] - PreviousPoint;
				const double CurrentLength = Segment.Length();
				const double RestLength = FVector::Dist(RestCurve.Points[PointIndex - 1], RestCurve.Points[PointIndex]);
				if (CurrentLength <= UE_SMALL_NUMBER || RestLength <= UE_SMALL_NUMBER)
				{
					continue;
				}

				SimulatedCurve.Points[PointIndex] = PreviousPoint + Segment * (RestLength / CurrentLength);
			}
		}
	}

	return bMoved;
}
