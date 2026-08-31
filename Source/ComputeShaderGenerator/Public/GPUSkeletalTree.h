#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GPUSkeletalTreeComponent.h"
#include "GPUSkeletalTree.generated.h"

class USkeletalMesh;
class USkeleton;
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

/**
 * Convenience actor wrapping a single UGPUSkeletalTreeComponent.
 *
 * The sway itself lives on the component — drop that on any actor instead if this wrapper is not
 * wanted. This class stays because the editor module drives procedural tree generation through it.
 */
UCLASS(BlueprintType, Blueprintable)
class COMPUTESHADERGENERATOR_API AGPUSkeletalTree : public AActor
{
	GENERATED_BODY()

public:
	AGPUSkeletalTree();

	static FGPUSkeletalTreeGenerateMeshRequest OnGenerateTreeEditorRequest;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree")
	FTreeBranchParams TreeParams;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree")
	bool bAutoGenerate = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree")
	TObjectPtr<USkeletalMesh> SkeletalMeshOverride;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tree")
	TObjectPtr<UGPUSkeletalTreeComponent> TreeMeshComp;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Tree")
	void GenerateTree();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Tree")
	void ApplySkeletalMeshOverride();

	void SetGeneratedSkeletalMesh(USkeletalMesh* Mesh, USkeleton* Skeleton);

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY()
	TObjectPtr<USkeletalMesh> GeneratedMesh;

	UPROPERTY()
	TObjectPtr<USkeleton> GeneratedSkeleton;
};
