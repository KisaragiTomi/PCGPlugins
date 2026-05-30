#include "GPUSkeletalTree.h"

#include "Engine/SkeletalMesh.h"
#include "Components/PoseableMeshComponent.h"
#include "ReferenceSkeleton.h"

#if WITH_EDITOR
#include "Animation/Skeleton.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#endif

AGPUSkeletalTree::AGPUSkeletalTree()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	TreeMeshComp = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("TreeMesh"));
	RootComponent = TreeMeshComp;
}

void AGPUSkeletalTree::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (SkeletalMeshOverride)
	{
		ApplySkeletalMesh(SkeletalMeshOverride);
	}
	else if (bAutoGenerate)
	{
		GenerateTree();
	}
}

void AGPUSkeletalTree::BeginPlay()
{
	Super::BeginPlay();
	if (SkeletalMeshOverride && (!TreeMeshComp || !TreeMeshComp->GetSkinnedAsset()))
	{
		ApplySkeletalMesh(SkeletalMeshOverride);
	}
}

void AGPUSkeletalTree::GenerateTree()
{
#if WITH_EDITOR
	BuildSkeleton();
	BuildMesh();
#endif
}

void AGPUSkeletalTree::ApplySkeletalMeshOverride()
{
	ApplySkeletalMesh(SkeletalMeshOverride);
}

void AGPUSkeletalTree::ApplySkeletalMesh(USkeletalMesh* Mesh)
{
	if (!TreeMeshComp)
	{
		return;
	}

	TreeMeshComp->SetSkinnedAssetAndUpdate(Mesh);
	GeneratedMesh = nullptr;
	GeneratedSkeleton = nullptr;
	BuildBoneHierarchyFromMesh(Mesh);
}

void AGPUSkeletalTree::BuildBoneHierarchyFromMesh(USkeletalMesh* Mesh)
{
	BoneHierarchy.Reset();
	if (!Mesh)
	{
		return;
	}

	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	BoneHierarchy.Reserve(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const int32 ParentIndex = RefSkel.GetParentIndex(BoneIndex);
		const float Depth = NumBones > 1 ? float(BoneIndex) / float(NumBones - 1) : 0.f;
		BoneHierarchy.Add({
			RefSkel.GetBoneName(BoneIndex),
			ParentIndex,
			RefSkel.GetRefBonePose()[BoneIndex],
			Depth
		});
	}
}

void AGPUSkeletalTree::BuildSkeleton()
{
	BoneHierarchy.Reset();

	const FTreeBranchParams& P = TreeParams;
	const float SegHeight = P.TrunkHeight / P.TrunkSegments;

	BoneHierarchy.Add({FName("Root"), INDEX_NONE, FTransform::Identity, 0.f});

	for (int32 i = 0; i < P.TrunkSegments; ++i)
	{
		FString Name = FString::Printf(TEXT("Trunk_%02d"), i);
		FTransform Local(FQuat::Identity, FVector(0, 0, SegHeight));
		BoneHierarchy.Add({FName(*Name), i, Local, float(i + 1) / P.TrunkSegments});
	}

	int32 BoneIdx = BoneHierarchy.Num();
	for (int32 Level = 2; Level < P.TrunkSegments; ++Level)
	{
		int32 TrunkBoneIdx = Level;
		float AngleStep = 360.f / P.BranchesPerLevel;
		float BaseAngle = Level * 137.5f;

		for (int32 b = 0; b < P.BranchesPerLevel; ++b)
		{
			float Angle = FMath::DegreesToRadians(BaseAngle + b * AngleStep);
			float Pitch = FMath::DegreesToRadians(-30.f - FMath::RandRange(0.f, 20.f));

			FVector Dir(FMath::Cos(Angle) * FMath::Cos(Pitch),
			            FMath::Sin(Angle) * FMath::Cos(Pitch),
			            FMath::Sin(Pitch));
			float SegLen = P.BranchLength / P.BranchSegments;

			int32 ParentIdx = TrunkBoneIdx;
			for (int32 s = 0; s < P.BranchSegments; ++s)
			{
				FString Name = FString::Printf(TEXT("Branch_%d_%d_%d"), Level, b, s);
				FTransform Local(FQuat::Identity, Dir * SegLen);
				float Depth = float(Level) / P.TrunkSegments + 0.5f * float(s + 1) / P.BranchSegments;
				BoneHierarchy.Add({FName(*Name), ParentIdx, Local, FMath::Clamp(Depth, 0.f, 1.f)});
				ParentIdx = BoneIdx++;
			}
		}
	}
}

void AGPUSkeletalTree::AddCylinderSegment(
	TArray<FVector3f>& Vertices, TArray<int32>& Indices,
	TArray<FVector3f>& Normals, TArray<FVector2f>& UVs,
	TArray<int32>& BoneIdxArr, TArray<float>& BoneWeightArr,
	const FVector& Start, const FVector& End,
	float RadiusStart, float RadiusEnd,
	int32 Radial, int32 BoneIdxStart, int32 BoneIdxEnd)
{
	int32 BaseVert = Vertices.Num();
	FVector Axis = (End - Start).GetSafeNormal();
	FVector Perp1, Perp2;
	Axis.FindBestAxisVectors(Perp1, Perp2);

	for (int32 Ring = 0; Ring < 2; ++Ring)
	{
		FVector Center = Ring == 0 ? Start : End;
		float Radius = Ring == 0 ? RadiusStart : RadiusEnd;
		float V = Ring == 0 ? 0.f : 1.f;
		int32 BIdx = Ring == 0 ? BoneIdxStart : BoneIdxEnd;

		for (int32 r = 0; r <= Radial; ++r)
		{
			float Angle = 2.f * PI * r / Radial;
			FVector Normal = Perp1 * FMath::Cos(Angle) + Perp2 * FMath::Sin(Angle);
			FVector Pos = Center + Normal * Radius;

			Vertices.Add(FVector3f(Pos));
			Normals.Add(FVector3f(Normal));
			UVs.Add(FVector2f(float(r) / Radial, V));

			if (Ring == 0)
			{
				BoneIdxArr.Add(BoneIdxStart);
				BoneWeightArr.Add(1.0f);
			}
			else
			{
				BoneIdxArr.Add(BoneIdxEnd);
				BoneWeightArr.Add(1.0f);
			}
		}
	}

	for (int32 r = 0; r < Radial; ++r)
	{
		int32 A = BaseVert + r;
		int32 B = BaseVert + r + 1;
		int32 C = BaseVert + (Radial + 1) + r;
		int32 D = BaseVert + (Radial + 1) + r + 1;

		Indices.Add(A); Indices.Add(C); Indices.Add(B);
		Indices.Add(B); Indices.Add(C); Indices.Add(D);
	}
}

void AGPUSkeletalTree::BuildMesh()
{
#if WITH_EDITOR
	if (BoneHierarchy.Num() == 0) return;

	GeneratedSkeleton = NewObject<USkeleton>(this, NAME_None, RF_Transient);

	FReferenceSkeleton RefSkel;
	{
		FReferenceSkeletonModifier Modifier(RefSkel, nullptr);
		for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
		{
			const FBoneData& BD = BoneHierarchy[i];
			FMeshBoneInfo Info(BD.Name, BD.Name.ToString(), BD.ParentIndex);
			Modifier.Add(Info, BD.LocalTransform, i == 0);
		}
	}

	{
		FReferenceSkeletonModifier SkelMod(GeneratedSkeleton);
		for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
		{
			const FBoneData& BD = BoneHierarchy[i];
			FMeshBoneInfo Info(BD.Name, BD.Name.ToString(), BD.ParentIndex);
			SkelMod.Add(Info, BD.LocalTransform, i == 0);
		}
	}

	TArray<FVector3f> AllVerts;
	TArray<int32> AllIndices;
	TArray<FVector3f> AllNormals;
	TArray<FVector2f> AllUVs;
	TArray<int32> AllBoneIdx;
	TArray<float> AllBoneWeight;

	TArray<FTransform> WorldBonePoses;
	WorldBonePoses.SetNum(BoneHierarchy.Num());
	for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
	{
		if (BoneHierarchy[i].ParentIndex == INDEX_NONE)
			WorldBonePoses[i] = BoneHierarchy[i].LocalTransform;
		else
			WorldBonePoses[i] = BoneHierarchy[i].LocalTransform * WorldBonePoses[BoneHierarchy[i].ParentIndex];
	}

	const FTreeBranchParams& P = TreeParams;
	for (int32 i = 1; i < BoneHierarchy.Num(); ++i)
	{
		int32 ParentIdx = BoneHierarchy[i].ParentIndex;
		FVector Start = WorldBonePoses[ParentIdx].GetLocation();
		FVector End = WorldBonePoses[i].GetLocation();

		bool bTrunk = BoneHierarchy[i].Name.ToString().StartsWith(TEXT("Trunk"));
		float RadS, RadE;
		if (bTrunk)
		{
			int32 TrunkOrdinal = i - 1;
			int32 ParentOrdinal = ParentIdx == 0 ? 0 : ParentIdx - 1;
			float T0 = float(ParentOrdinal) / P.TrunkSegments;
			float T1 = float(TrunkOrdinal) / P.TrunkSegments;
			RadS = FMath::Lerp(P.TrunkRadiusBase, P.TrunkRadiusTip, T0);
			RadE = FMath::Lerp(P.TrunkRadiusBase, P.TrunkRadiusTip, T1);
		}
		else
		{
			RadS = P.BranchRadius;
			RadE = P.BranchRadius * 0.4f;
		}

		AddCylinderSegment(AllVerts, AllIndices, AllNormals, AllUVs,
			AllBoneIdx, AllBoneWeight,
			Start, End, RadS, RadE, P.RadialSegments, ParentIdx, i);
	}

	GeneratedMesh = NewObject<USkeletalMesh>(this, NAME_None, RF_Transient);
	GeneratedMesh->SetSkeleton(GeneratedSkeleton);
	GeneratedMesh->SetRefSkeleton(RefSkel);

	FMeshDescription MeshDesc;
	FSkeletalMeshAttributes SkelAttrs(MeshDesc);
	SkelAttrs.Register();

	for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
	{
		FBoneID BoneID = SkelAttrs.CreateBone();
		SkelAttrs.GetBoneNames().Set(BoneID, BoneHierarchy[i].Name);
		SkelAttrs.GetBoneParentIndices().Set(BoneID, BoneHierarchy[i].ParentIndex);
		SkelAttrs.GetBonePoses().Set(BoneID, BoneHierarchy[i].LocalTransform);
	}

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(AllVerts.Num());
	auto VertexPositions = SkelAttrs.GetVertexPositions();
	for (int32 v = 0; v < AllVerts.Num(); ++v)
	{
		FVertexID VID = MeshDesc.CreateVertex();
		VertexPositions.Set(VID, AllVerts[v]);
		VertexIDs.Add(VID);
	}

	FSkinWeightsVertexAttributesRef SkinWeights = SkelAttrs.GetVertexSkinWeights();
	for (int32 v = 0; v < AllVerts.Num(); ++v)
	{
		UE::AnimationCore::FBoneWeights BW;
		BW.SetBoneWeight(UE::AnimationCore::FBoneWeight(AllBoneIdx[v], AllBoneWeight[v]));
		SkinWeights.Set(FVertexID(v), BW);
	}

	FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();

	auto VertInstanceNormals = SkelAttrs.GetVertexInstanceNormals();
	auto VertInstanceUVs = SkelAttrs.GetVertexInstanceUVs();

	for (int32 t = 0; t < AllIndices.Num(); t += 3)
	{
		TArray<FVertexInstanceID> TriVerts;
		TriVerts.Reserve(3);

		for (int32 c = 0; c < 3; ++c)
		{
			int32 Idx = AllIndices[t + c];
			FVertexInstanceID VIID = MeshDesc.CreateVertexInstance(VertexIDs[Idx]);
			VertInstanceNormals.Set(VIID, AllNormals[Idx]);
			VertInstanceUVs.Set(VIID, 0, AllUVs[Idx]);
			TriVerts.Add(VIID);
		}

		MeshDesc.CreatePolygon(PolyGroup, TriVerts);
	}

	FSkeletalMeshLODInfo& LODInfo = GeneratedMesh->AddLODInfo();
	LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	LODInfo.LODHysteresis = 0.02f;
	LODInfo.bAllowCPUAccess = true;

	GeneratedMesh->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
	GeneratedMesh->CreateMeshDescription(0, MoveTemp(MeshDesc));

	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	GeneratedMesh->CommitMeshDescription(0, CommitParams);

	GeneratedMesh->CalculateInvRefMatrices();

	FBoxSphereBounds Bounds(FBox(FVector(-100, -100, 0), FVector(100, 100, P.TrunkHeight)));
	GeneratedMesh->SetImportedBounds(Bounds);

	GeneratedMesh->Build();

	TreeMeshComp->SetSkinnedAssetAndUpdate(GeneratedMesh);
#endif
}

void AGPUSkeletalTree::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	AnimTime += DeltaTime;

	const FVector CurrentLocation = GetActorLocation();
	if (bPrevLocationValid && DeltaTime > KINDA_SMALL_NUMBER)
	{
		FVector InstantVel = (CurrentLocation - PrevLocation) / DeltaTime;
		DragVelocity = FMath::VInterpTo(DragVelocity, InstantVel, DeltaTime, DragDamping);
	}
	else
	{
		DragVelocity = FVector::ZeroVector;
	}
	PrevLocation = CurrentLocation;
	bPrevLocationValid = true;

	ApplyWindAnimation(AnimTime);
}

void AGPUSkeletalTree::ApplyWindAnimation(float Time)
{
	if (!TreeMeshComp || !TreeMeshComp->GetSkinnedAsset()) return;

	TArray<FTransform> BoneTransforms;
	const int32 NumBones = BoneHierarchy.Num();
	BoneTransforms.SetNum(NumBones);

	const FTreeWindParams& W = WindParams;
	FVector WindDir = W.WindDirection.GetSafeNormal();

	for (int32 i = 0; i < NumBones; ++i)
	{
		FTransform Local = BoneHierarchy[i].LocalTransform;

		if (i > 0)
		{
			float Depth = BoneHierarchy[i].Depth;
			bool bBranch = BoneHierarchy[i].Name.ToString().StartsWith(TEXT("Branch"));
			float Mult = bBranch ? W.BranchMultiplier : 1.0f;

			float Phase = i * 1.618f;
			float MainSway = FMath::Sin(Time * W.WindFrequency + Phase) * W.WindStrength * Depth * Mult;
			float Turbulence = FMath::Sin(Time * W.TurbulenceFrequency + Phase * 2.3f)
				* FMath::Cos(Time * W.TurbulenceFrequency * 0.7f + Phase)
				* W.TurbulenceStrength * Depth * Mult;

			float DragPitch = -DragVelocity.X * DragReactionStrength * Depth * Mult * 0.01f;
			float DragYaw = -DragVelocity.Y * DragReactionStrength * Depth * Mult * 0.01f;
			float DragRoll = DragVelocity.Z * DragReactionStrength * Depth * Mult * 0.005f;

			float PitchDeg = (MainSway + Turbulence) * 0.05f + DragPitch;
			float YawDeg = Turbulence * 0.03f + DragYaw;

			FQuat WindRot = FQuat(FRotator(PitchDeg, YawDeg, DragRoll));
			Local.SetRotation(WindRot * Local.GetRotation());
		}

		BoneTransforms[i] = Local;
	}

	TArray<FTransform> CompSpacePoses;
	CompSpacePoses.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		if (BoneHierarchy[i].ParentIndex == INDEX_NONE)
			CompSpacePoses[i] = BoneTransforms[i];
		else
			CompSpacePoses[i] = BoneTransforms[i] * CompSpacePoses[BoneHierarchy[i].ParentIndex];
	}

	for (int32 i = 0; i < NumBones; ++i)
	{
		TreeMeshComp->SetBoneTransformByName(BoneHierarchy[i].Name, CompSpacePoses[i], EBoneSpaces::ComponentSpace);
	}
}
