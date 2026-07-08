#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GPUSkeletalTree.generated.h"

class USkeletalMesh;
class USkeleton;
class UPoseableMeshComponent;
class AGPUSkeletalTree;

DECLARE_DELEGATE_OneParam(FGPUSkeletalTreeGenerateMeshRequest, AGPUSkeletalTree*);

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FTreeBranchParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk")
	int32 TrunkSegments = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk")
	float TrunkHeight = 400.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk")
	float TrunkRadiusBase = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk")
	float TrunkRadiusTip = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	int32 BranchesPerLevel = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	int32 BranchSegments = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	float BranchLength = 180.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branch")
	float BranchRadius = 6.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	int32 RadialSegments = 8;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FTreeWindParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	FVector WindDirection = FVector(1, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float WindStrength = 15.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float WindFrequency = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turbulence")
	float TurbulenceStrength = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turbulence")
	float TurbulenceFrequency = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wind")
	float BranchMultiplier = 2.0f;
};

UCLASS(BlueprintType, Blueprintable)
class COMPUTESHADERGENERATOR_API AGPUSkeletalTree : public AActor
{
	GENERATED_BODY()

public:
	AGPUSkeletalTree();

	static FGPUSkeletalTreeGenerateMeshRequest OnGenerateTreeEditorRequest;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree")
	FTreeBranchParams TreeParams;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Wind")
	FTreeWindParams WindParams;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Interaction")
	float DragReactionStrength = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree|Interaction")
	float DragDamping = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree")
	bool bAutoGenerate = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree")
	TObjectPtr<USkeletalMesh> SkeletalMeshOverride;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Tree")
	void GenerateTree();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Tree")
	void ApplySkeletalMeshOverride();

	void SetGeneratedSkeletalMesh(USkeletalMesh* Mesh, USkeleton* Skeleton);

	virtual void Tick(float DeltaTime) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY()
	TObjectPtr<UPoseableMeshComponent> TreeMeshComp;

	UPROPERTY()
	TObjectPtr<USkeletalMesh> GeneratedMesh;

	UPROPERTY()
	TObjectPtr<USkeleton> GeneratedSkeleton;

	struct FBoneData
	{
		FName Name;
		int32 ParentIndex;
		FTransform LocalTransform;
		float Depth;
	};
	TArray<FBoneData> BoneHierarchy;

	void BuildSkeleton();
	void BuildMesh();
	void ApplySkeletalMesh(USkeletalMesh* Mesh);
	void BuildBoneHierarchyFromMesh(USkeletalMesh* Mesh);
	void ApplyWindAnimation(float Time);
	void AddCylinderSegment(
		TArray<FVector3f>& Vertices, TArray<int32>& Indices,
		TArray<FVector3f>& Normals, TArray<FVector2f>& UVs,
		TArray<int32>& BoneIdxArr, TArray<float>& BoneWeightArr,
		const FVector& Start, const FVector& End,
		float RadiusStart, float RadiusEnd,
		int32 Radial, int32 BoneIdxStart, int32 BoneIdxEnd);

	float AnimTime = 0.f;
	FVector PrevLocation = FVector::ZeroVector;
	FVector DragVelocity = FVector::ZeroVector;
	bool bPrevLocationValid = false;
};
