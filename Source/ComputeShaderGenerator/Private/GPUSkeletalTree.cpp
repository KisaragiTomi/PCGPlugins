#include "GPUSkeletalTree.h"

#include "Engine/SkeletalMesh.h"

FGPUSkeletalTreeGenerateMeshRequest AGPUSkeletalTree::OnGenerateTreeEditorRequest;

AGPUSkeletalTree::AGPUSkeletalTree()
{
	PrimaryActorTick.bCanEverTick = false;

	TreeMeshComp = CreateDefaultSubobject<UGPUSkeletalTreeComponent>(TEXT("TreeMesh"));
	RootComponent = TreeMeshComp;
}

void AGPUSkeletalTree::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (SkeletalMeshOverride)
	{
		ApplySkeletalMeshOverride();
	}
	else if (bAutoGenerate)
	{
		GenerateTree();
	}
}

void AGPUSkeletalTree::BeginPlay()
{
	Super::BeginPlay();
	if (SkeletalMeshOverride && TreeMeshComp && !TreeMeshComp->GetSkinnedAsset())
	{
		ApplySkeletalMeshOverride();
	}
}

void AGPUSkeletalTree::GenerateTree()
{
#if WITH_EDITOR
	if (OnGenerateTreeEditorRequest.IsBound())
	{
		OnGenerateTreeEditorRequest.Execute(this);
		return;
	}
#endif

	UE_LOG(LogTemp, Warning, TEXT("AGPUSkeletalTree::GenerateTree requires the PCGEditorProcess editor module. Assign SkeletalMeshOverride for runtime use."));
}

void AGPUSkeletalTree::ApplySkeletalMeshOverride()
{
	if (!TreeMeshComp) return;

	TreeMeshComp->SetTreeMesh(SkeletalMeshOverride);
	GeneratedMesh = nullptr;
	GeneratedSkeleton = nullptr;
}

void AGPUSkeletalTree::SetGeneratedSkeletalMesh(USkeletalMesh* Mesh, USkeleton* Skeleton)
{
	if (!TreeMeshComp) return;

	GeneratedMesh = Mesh;
	GeneratedSkeleton = Skeleton;
	TreeMeshComp->SetTreeMesh(Mesh);
}
