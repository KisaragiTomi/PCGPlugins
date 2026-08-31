#include "GPUSkeletalTreeComponent.h"

#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

UGPUSkeletalTreeComponent::UGPUSkeletalTreeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// Sway in the level viewport too, not only in PIE.
	bTickInEditor = true;
}

void UGPUSkeletalTreeComponent::OnRegister()
{
	Super::OnRegister();
	++RegisterCount;
	// Re-registration happens often — every gizmo drag reruns the construction script — so only
	// pay for the rebuild when there is nothing usable to animate.
	if (BoneHierarchy.Num() == 0) RebuildBoneHierarchy();
}

void UGPUSkeletalTreeComponent::SetTreeMesh(USkeletalMesh* Mesh)
{
	SetSkinnedAssetAndUpdate(Mesh, true);
	RebuildBoneHierarchy();
}

void UGPUSkeletalTreeComponent::RebuildBoneHierarchy()
{
	BoneHierarchy.Reset();

	USkeletalMesh* Mesh = Cast<USkeletalMesh>(GetSkinnedAsset());
	if (!Mesh) return;

	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	BoneHierarchy.Reserve(NumBones);

	// Reference skeletons guarantee parents precede children, so both chain depth and the
	// owning strand resolve in a single forward pass.
	TArray<int32> DepthSteps;
	TArray<int32> StrandIds;
	DepthSteps.SetNumZeroed(NumBones);
	StrandIds.Init(INDEX_NONE, NumBones);
	int32 MaxSteps = 1;
	int32 NextStrandId = 0;
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const int32 ParentIndex = RefSkel.GetParentIndex(BoneIndex);
		if (ParentIndex == INDEX_NONE) continue;

		DepthSteps[BoneIndex] = DepthSteps[ParentIndex] + 1;
		MaxSteps = FMath::Max(MaxSteps, DepthSteps[BoneIndex]);
		// A bone hanging directly off the root starts a new strand; everything below inherits it.
		StrandIds[BoneIndex] = StrandIds[ParentIndex] == INDEX_NONE ? NextStrandId++ : StrandIds[ParentIndex];
	}

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		BoneHierarchy.Add({
			RefSkel.GetBoneName(BoneIndex),
			RefSkel.GetParentIndex(BoneIndex),
			RefSkel.GetRefBonePose()[BoneIndex],
			float(DepthSteps[BoneIndex]) / float(MaxSteps),
			DepthSteps[BoneIndex],
			FMath::Max(StrandIds[BoneIndex], 0)
		});
	}
}

void UGPUSkeletalTreeComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                              FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AnimTime += DeltaTime;

	// Two followers chase the real location: a fast one that absorbs the stepwise input a gizmo
	// drag produces, and a slow one whose lag behind it is what bends the bones. Differentiating
	// the raw location instead would spike on step frames and collapse to zero in between.
	const FVector CurrentLocation = GetComponentLocation();
	const FQuat CurrentRotation = GetComponentQuat();
	if (bLagValid)
	{
		SmoothLocation = FMath::VInterpTo(SmoothLocation, CurrentLocation, DeltaTime, InputSmoothSpeed);
		LagLocation = FMath::VInterpTo(LagLocation, CurrentLocation, DeltaTime, DragDamping);
		SmoothRotation = FMath::QInterpTo(SmoothRotation, CurrentRotation, DeltaTime, InputSmoothSpeed);
		LagRotation = FMath::QInterpTo(LagRotation, CurrentRotation, DeltaTime, DragDamping);
	}
	else
	{
		SmoothLocation = CurrentLocation;
		LagLocation = CurrentLocation;
		SmoothRotation = CurrentRotation;
		LagRotation = CurrentRotation;
	}
	bLagValid = true;

	DebugAnimTime = AnimTime;
	ApplyWindAnimation(AnimTime);
}

void UGPUSkeletalTreeComponent::ApplyWindAnimation(float Time)
{
	if (!GetSkinnedAsset()) return;

	// BoneHierarchy is runtime-only state: it survives neither level load nor PIE duplication,
	// so rebuild it from the mesh whenever it comes up empty.
	if (BoneHierarchy.Num() == 0) RebuildBoneHierarchy();
	if (BoneHierarchy.Num() == 0) return;

	const int32 NumBones = BoneHierarchy.Num();
	const FTreeWindParams& W = WindParams;
	const FVector WindDir = W.WindDirection.GetSafeNormal();

	// Rest pose in component space. Every bone needs its own segment direction to work out which
	// way "sideways" is: a fixed pitch/yaw twists a bone about its own axis instead of leaning it,
	// and bends strands that point opposite ways in opposite world directions.
	TArray<FTransform> RestComp;
	RestComp.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		const int32 P = BoneHierarchy[i].ParentIndex;
		if (P == INDEX_NONE) RestComp[i] = BoneHierarchy[i].LocalTransform;
		else RestComp[i] = BoneHierarchy[i].LocalTransform * RestComp[P];
	}

	// How far the mesh got left behind, in component space. Measured against the fast followers
	// rather than the raw transform so a stepwise drag cannot snap the bend open in one frame.
	// Scaling by DragDamping keeps the bend the same size whatever the recovery speed, since the
	// lag settles at velocity / DragDamping.
	const FQuat WorldToComp = GetComponentTransform().GetRotation().Inverse();
	const FVector LinearLagComp = WorldToComp.RotateVector(LagLocation - SmoothLocation);
	const FVector WindDirComp = WorldToComp.RotateVector(WindDir);
	const float DragScale = DragDamping * DragReactionStrength * 0.01f;

	// Rotational lag as an axis and angle: a bone sitting at offset r from the origin was left
	// behind by roughly Angle * (Axis x r), so far-out bones get whipped and the root barely moves.
	FQuat RotLag = LagRotation * SmoothRotation.Inverse();
	RotLag.Normalize();
	if (RotLag.W < 0.f) RotLag = FQuat(-RotLag.X, -RotLag.Y, -RotLag.Z, -RotLag.W);  // shortest arc
	FVector RotAxisWorld;
	float RotAngle = 0.f;
	RotLag.ToAxisAndAngle(RotAxisWorld, RotAngle);
	const FVector RotAxisComp = WorldToComp.RotateVector(RotAxisWorld);

	TArray<FTransform> BoneTransforms;
	BoneTransforms.SetNum(NumBones);

	for (int32 i = 0; i < NumBones; ++i)
	{
		FTransform Local = BoneHierarchy[i].LocalTransform;
		const int32 P = BoneHierarchy[i].ParentIndex;

		if (P != INDEX_NONE)
		{
			const float Depth = BoneHierarchy[i].Depth;
			const bool bBranch = BoneHierarchy[i].Name.ToString().StartsWith(TEXT("Branch"));
			const float Mult = bBranch ? W.BranchMultiplier : 1.0f;

			// Phase per strand, advancing along the chain: bones within one strand stay nearly in
			// step so the sway travels to the tip, while separate strands stay offset from each
			// other. Keying phase off the global bone index instead makes neighbours ~93 degrees
			// apart, so a strand fights itself and barely moves.
			const float Phase = BoneHierarchy[i].StrandId * 2.399f + BoneHierarchy[i].ChainDepth * 0.35f;
			const float MainSway = FMath::Sin(Time * W.WindFrequency + Phase) * W.WindStrength * Depth * Mult;
			const float Turbulence = FMath::Sin(Time * W.TurbulenceFrequency + Phase * 2.3f)
				* FMath::Cos(Time * W.TurbulenceFrequency * 0.7f + Phase)
				* W.TurbulenceStrength * Depth * Mult;

			// Drag is per bone: the linear lag shifts every bone alike, while the rotational lag
			// grows with how far the bone sits from the pivot.
			const FVector BonePos = RestComp[i].GetLocation();
			const FVector DragPush =
				(LinearLagComp + RotAngle * FVector::CrossProduct(RotAxisComp, BonePos)) * DragScale;

			// Wind and drag are both pushes; resolve them into one before bending.
			const FVector Push = WindDirComp * ((MainSway + Turbulence) * 0.05f) + DragPush * Depth * Mult;

			FVector SegDir = (BonePos - RestComp[P].GetLocation()).GetSafeNormal();
			if (SegDir.IsNearlyZero()) SegDir = FVector::UpVector;

			// Bend about the axis perpendicular to both, by the push component across the bone —
			// a push along the bone cannot bend it, which the cross product gives for free.
			const FVector AxisComp = FVector::CrossProduct(SegDir, Push);
			const float AngleDeg = AxisComp.Size();
			if (AngleDeg > KINDA_SMALL_NUMBER)
			{
				// A bone's local rotation lives in its parent's frame, so the axis moves there too.
				const FVector LocalAxis = RestComp[P].GetRotation().UnrotateVector(AxisComp.GetSafeNormal());
				Local.SetRotation(FQuat(LocalAxis, FMath::DegreesToRadians(AngleDeg)) * Local.GetRotation());
			}
		}

		BoneTransforms[i] = Local;
	}

	TArray<FTransform> CompSpacePoses;
	CompSpacePoses.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		const int32 P = BoneHierarchy[i].ParentIndex;
		if (P == INDEX_NONE) CompSpacePoses[i] = BoneTransforms[i];
		else CompSpacePoses[i] = BoneTransforms[i] * CompSpacePoses[P];
	}

	for (int32 i = 0; i < NumBones; ++i)
	{
		SetBoneTransformByName(BoneHierarchy[i].Name, CompSpacePoses[i], EBoneSpaces::ComponentSpace);
	}
}
