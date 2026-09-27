#include "CSGroundCollisionComponent.h"

#include "CSGroundActor.h"
#include "Engine/CollisionProfile.h"
#include "PhysicsEngine/BodySetup.h"

UCSGroundCollisionComponent::UCSGroundCollisionComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	// 和一块普通地面一样挡一切：编辑器放置、End 贴地、PIE 里的角色与物理都认它。
	SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	// 地面只认 actor 的位置（高度场与采样都不看旋转 / 缩放），碰撞跟着同一个口径。
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
	bCanEverAffectNavigation = true;
	SetGenerateOverlapEvents(false);
}

void UCSGroundCollisionComponent::RebuildFromMirror(const FCSGroundMirror& Mirror)
{
	CollisionVertices.Reset();
	CollisionIndices.Reset();
	FBox NewBounds(ForceInit);

	if (Mirror.IsInitialized())
	{
		const int32 CellsX = Mirror.NumVertsX - 1;
		const int32 CellsY = Mirror.NumVertsY - 1;
		const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(FMath::Max(CellsX, CellsY), MaxQuadsPerAxis));

		// 抽稀后的格点列：每 Stride 取一列，末列一定是镜像的最后一列 —— 否则地面边缘会缩进一截。
		auto Columns = [Stride](int32 Cells)
		{
			TArray<int32> Out;
			for (int32 I = 0; I < Cells; I += Stride) Out.Add(I);
			Out.Add(Cells);
			return Out;
		};
		const TArray<int32> Xs = Columns(CellsX);
		const TArray<int32> Ys = Columns(CellsY);

		CollisionVertices.Reserve(Xs.Num() * Ys.Num());
		for (const int32 Y : Ys)
		{
			for (const int32 X : Xs)
			{
				const FVector3f P(float(X) * Mirror.CellSize, float(Y) * Mirror.CellSize, Mirror.Heights[Mirror.VertexIndex(X, Y)]);
				CollisionVertices.Add(P);
				NewBounds += FVector(P);
			}
		}

		const int32 RowStride = Xs.Num();
		CollisionIndices.Reserve((Xs.Num() - 1) * (Ys.Num() - 1) * 2);
		for (int32 Row = 0; Row + 1 < Ys.Num(); ++Row)
		{
			for (int32 Col = 0; Col + 1 < Xs.Num(); ++Col)
			{
				const int32 V00 = Row * RowStride + Col;
				const int32 V10 = V00 + 1;
				const int32 V01 = V00 + RowStride;
				const int32 V11 = V01 + 1;
				FTriIndices A; A.v0 = V00; A.v1 = V11; A.v2 = V10;
				FTriIndices B; B.v0 = V00; B.v1 = V01; B.v2 = V11;
				CollisionIndices.Add(A);
				CollisionIndices.Add(B);
			}
		}
	}

	// 以最新请求为准：在途的那次作废（被取消的 cook 不会回调）。
	if (PendingBodySetup) PendingBodySetup->AbortPhysicsMeshAsyncCreation();
	PendingBodySetup = nullptr;

	if (CollisionIndices.IsEmpty())
	{
		CollisionBodySetup = nullptr;
		LocalBounds = FBox(ForceInit);
		RecreatePhysicsState();
		UpdateBounds();
		return;
	}

	UBodySetup* NewBodySetup = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
	// 每次新 GUID：同一个 GUID 会让 cook 命中 DDC 里上一份几何。
	NewBodySetup->BodySetupGuid = FGuid::NewGuid();
	NewBodySetup->bGenerateMirroredCollision = false;
	// 放置射线可能从地面下方打上来（相机钻到地下时），双面才稳。
	NewBodySetup->bDoubleSidedGeometry = true;
	NewBodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;

	// 先登记再发起：没有可 cook 的数据时引擎会在调用里当场回调。
	PendingBodySetup = NewBodySetup;
	PendingLocalBounds = NewBounds;
	NewBodySetup->CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateUObject(this, &UCSGroundCollisionComponent::FinishAsyncCook, NewBodySetup));
}

void UCSGroundCollisionComponent::FinishAsyncCook(bool bSuccess, UBodySetup* FinishedBodySetup)
{
	// 已被更新的请求取代（取消的本来不回调，这里再兜一道）。
	if (FinishedBodySetup != PendingBodySetup) return;
	PendingBodySetup = nullptr;
	// 失败就留着旧碰撞：少一次更新好过一段时间没有碰撞。
	if (!bSuccess) return;

	CollisionBodySetup = FinishedBodySetup;
	LocalBounds = PendingLocalBounds;
	RecreatePhysicsState();
	UpdateBounds();
}

void UCSGroundCollisionComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	if (PendingBodySetup) PendingBodySetup->AbortPhysicsMeshAsyncCreation();
	PendingBodySetup = nullptr;
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

bool UCSGroundCollisionComponent::GetPhysicsTriMeshData(FTriMeshCollisionData* CollisionData, bool /*InUseAllTriData*/)
{
	if (!CollisionData || CollisionIndices.IsEmpty()) return false;
	CollisionData->Vertices = CollisionVertices;
	CollisionData->Indices = CollisionIndices;
	CollisionData->MaterialIndices.Init(0, CollisionIndices.Num());
	CollisionData->bFlipNormals = true;
	CollisionData->bDeformableMesh = true;
	CollisionData->bFastCook = true;
	return true;
}

bool UCSGroundCollisionComponent::ContainsPhysicsTriMeshData(bool /*InUseAllTriData*/) const
{
	return !CollisionIndices.IsEmpty();
}

FBoxSphereBounds UCSGroundCollisionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBounds.IsValid) return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}
