#pragma once

#include "CoreMinimal.h"
#include "Components/PoseableMeshComponent.h"
#include "GPUSkeletalTreeComponent.generated.h"

class USkeletalMesh;

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FTreeWindParams
{
	GENERATED_BODY()

	/** World-space direction the wind blows towards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	FVector WindDirection = FVector(1, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float WindStrength = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float WindFrequency = 1.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turbulence")
	float TurbulenceStrength = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turbulence")
	float TurbulenceFrequency = 2.8f;

	/** Extra sway for bones whose name starts with "Branch". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float BranchMultiplier = 2.0f;
};

/**
 * Poseable mesh that sways in the wind and lags behind its own motion.
 *
 * Drop it on any actor and assign a skeletal mesh: every bone bends about an axis derived from
 * its own direction, so strands pointing any which way all lean the same way in world space.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent), HideCategories = (Object, Mobility))
class COMPUTESHADERGENERATOR_API UGPUSkeletalTreeComponent : public UPoseableMeshComponent
{
	GENERATED_BODY()

public:
	UGPUSkeletalTreeComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Wind")
	FTreeWindParams WindParams;

	/** How far the mesh bends when it is moved around. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Interaction")
	float DragReactionStrength = 30.0f;

	/**
	 * Speed the bend builds up and dies away at — this is an interpolation speed, not a damping
	 * coefficient, so a SMALLER value means a slower, longer recovery. Never set it to 0: that
	 * makes the bend snap instantly instead of never recovering.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Interaction")
	float DragDamping = 1.0f;

	/**
	 * How quickly the bend catches up to sudden jumps in position. Editor gizmo drags and teleports
	 * deliver motion in steps; smoothing the input this much keeps a step from snapping the bend
	 * open within one frame. Keep it well above DragDamping or it will eat the bend itself.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Interaction")
	float InputSmoothSpeed = 12.0f;

	/** Assign the mesh and rebuild the bone hierarchy the wind animation runs on. */
	UFUNCTION(BlueprintCallable, Category = "Tree")
	void SetTreeMesh(USkeletalMesh* Mesh);

	/** Recompute the bone hierarchy from whatever mesh is currently assigned. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Tree")
	void RebuildBoneHierarchy();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void OnRegister() override;

private:
	struct FBoneData
	{
		FName Name;
		int32 ParentIndex;
		FTransform LocalTransform;
		float Depth;       // normalized against the deepest chain in the skeleton
		int32 ChainDepth;  // steps from the root bone
		int32 StrandId;    // which top-level chain this bone belongs to
	};
	TArray<FBoneData> BoneHierarchy;

	void ApplyWindAnimation(float Time);

	// Dragging a Blueprint actor by its gizmo reruns the construction script, which destroys and
	// recreates this component. UPROPERTY state is carried across by the instance cache, so keeping
	// the sway clock and lag position here stops the animation restarting on every mouse move.
	UPROPERTY()
	float AnimTime = 0.f;

	/**
	 * Where the mesh "wishes" it still were — it chases the real location at DragDamping speed, and
	 * the gap between the two is what bends the bones. Tracking a lagging position rather than
	 * differentiating this one matters: gizmo drags deliver position in mouse-event sized steps,
	 * and dividing those steps by frame time turns them into a jittery sawtooth.
	 */
	UPROPERTY()
	FVector LagLocation = FVector::ZeroVector;

	/**
	 * A fast follower of the real location. The bend is the gap between this and LagLocation, so a
	 * stepwise input ramps in over a few frames instead of snapping the bend open in one: with the
	 * raw location as reference, a 30cm gizmo step moves the whole bend within a single frame.
	 */
	UPROPERTY()
	FVector SmoothLocation = FVector::ZeroVector;

	// Rotation gets the same pair of followers as position. Spinning in place moves no bone root but
	// sweeps every bone through an arc, so without these a pure rotation bends nothing at all.
	UPROPERTY()
	FQuat LagRotation = FQuat::Identity;

	UPROPERTY()
	FQuat SmoothRotation = FQuat::Identity;

	UPROPERTY()
	bool bLagValid = false;

public:
	/** Diagnostics: bumped every time this component is registered, i.e. once per rebuild. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tree|Debug")
	int32 RegisterCount = 0;

	/** Diagnostics: the sway clock, so a restart caused by a rebuild is visible from outside. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tree|Debug")
	float DebugAnimTime = 0.f;
};
