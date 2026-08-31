#include "CSGroundShaperActor.h"

#include "CSGroundActor.h"
#include "CSGroundShaperField.h"
#include "Components/BillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"

ACSGroundShaperActor::ACSGroundShaperActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// 示意圆柱：只是编辑器里"这里有座土台"的把手，游戏里不存在。
	EditorShapeComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EditorShape"));
	EditorShapeComponent->SetupAttachment(RootComponent);
	EditorShapeComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	EditorShapeComponent->SetHiddenInGame(true);
	EditorShapeComponent->SetCastShadow(false);
	EditorShapeComponent->bIsEditorOnly = true;

	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	EditorShapeMesh = CylinderMesh.Get();

#if WITH_EDITORONLY_DATA
	SpriteComponent = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (SpriteComponent)
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_Actor"));
		SpriteComponent->Sprite = SpriteTexture.Get();
		SpriteComponent->SetupAttachment(RootComponent);
		SpriteComponent->bIsScreenSizeScaled = true;
		SpriteComponent->SetHiddenInGame(true);
	}
#endif
}

// -----------------------------------------------------------------------------
// Ground wiring
// -----------------------------------------------------------------------------

void ACSGroundShaperActor::ResolveGroundAndRegister()
{
	if (!Ground && GetWorld())
	{
		for (TActorIterator<ACSGroundActor> It(GetWorld()); It; ++It) { Ground = *It; break; }
	}
	if (!Ground) return;

	// ⚠️ **不订阅 `OnGroundChanged`**：这条订阅当初只为"别人画路 -> 本座重摆石阶"存在，
	// 随旧路（`RebuildSteps`）一起删了。塑形物现在只写高度场、不读回地面，是单向的。
	Ground->RegisterShaper(this);   // 已登记时是零成本的（区域更新由 RebuildTerrain 自己发）
}

void ACSGroundShaperActor::UnregisterFromGround(bool bRefreshGround)
{
	if (!IsValid(Ground)) return;
	Ground->UnregisterShaper(this);
	if (bRefreshGround) Ground->RefreshHeightsInRegion(LastAppliedFootprint.bIsValid ? LastAppliedFootprint : GetFootprintRect2D());
}

// -----------------------------------------------------------------------------
// Shape
// -----------------------------------------------------------------------------

double ACSGroundShaperActor::ComputeTopHeight() const
{
	const FVector Loc = GetActorLocation();
	const double GroundZ = Ground ? Ground->GetActorLocation().Z : Loc.Z;
	return FMath::Max(0.0, (Loc.Z - GroundZ) + LiftHeight);   // 把 actor 往上拖 = 整座台加高
}

float ACSGroundShaperActor::SampleShapeHeight(const FVector2D& WorldXY) const
{
	// 走的是与 GPU 逐行对照的那一份公式（Public/CSGroundShaperField.h ↔ CSGroundShaperField.ush），
	// 而且喂进去的是**同一份打包参数** —— 早先这里各写各的 double 版本，噪声一进来就没法保证
	// 两侧同数了（症状：房子/石阶浮在坡面上方几厘米，不报任何错）。
	FVector4f Profile, Top, Noise;
	GetHeightFieldParams(Profile, Top, Noise);
	return CSGroundShaperField::EvalShaper(FVector2f(float(WorldXY.X), float(WorldXY.Y)), Profile, Top, Noise);
}

void ACSGroundShaperActor::GetHeightFieldParams(FVector4f& OutProfile, FVector4f& OutTop, FVector4f& OutNoise) const
{
	const FVector Loc = GetActorLocation();
	OutProfile = FVector4f(float(Loc.X), float(Loc.Y), Radius, FMath::Max(FalloffDistance, UE_KINDA_SMALL_NUMBER));
	// 频率在这里算完再打包，而不是两侧各除一次：除法结果差 1 ulp 就够让 CPU/GPU 一致性断言报红。
	const float Frequency = 1.0f / FMath::Max(SkirtNoiseWavelength, 1.0f);
	OutTop = FVector4f(float(ComputeTopHeight()), FMath::Max(SkirtNoiseAmount, 0.0f), Frequency, FMath::Max(SecondaryLiftScale, 0.0f));
	// 种子过 float 再回 uint，所以只留 20 bit：更大的整数在 float 里就不是精确值了，
	// CPU 与 GPU 会各自截断到不同的数。
	OutNoise = FVector4f(float(uint32(SkirtNoiseSeed) & 0xFFFFFu), 0.0f, 0.0f, 0.0f);
}

FBox2D ACSGroundShaperActor::GetFootprintRect2D() const
{
	const FVector Loc = GetActorLocation();
	const double Reach = Radius + FalloffDistance;
	return FBox2D(FVector2D(Loc.X - Reach, Loc.Y - Reach), FVector2D(Loc.X + Reach, Loc.Y + Reach));
}

// -----------------------------------------------------------------------------
// Rebuild
// -----------------------------------------------------------------------------

void ACSGroundShaperActor::ReevaluateSite()
{
	RebuildTerrain();
}

void ACSGroundShaperActor::RebuildTerrain()
{
	if (IsTemplate() || !GetWorld()) return;   // CDO / 未进世界的模板不碰场景
	ResolveGroundAndRegister();
	UpdateEditorShape();

	if (Ground)
	{
		// 区域更新只重算 union(旧足迹, 新足迹) —— 撤掉旧位置的隆起与压出新位置的隆起是同一趟。
		const FBox2D NewFootprint = GetFootprintRect2D();
		FBox2D Region = NewFootprint;
		if (LastAppliedFootprint.bIsValid) Region += LastAppliedFootprint;
		LastAppliedFootprint = NewFootprint;
		Ground->RefreshHeightsInRegion(Region);   // 高度真变了才广播（地面自己的石阶随之重扫）
	}
}

void ACSGroundShaperActor::UpdateEditorShape()
{
	if (!EditorShapeComponent) return;
	if (EditorShapeComponent->GetStaticMesh() != EditorShapeMesh) EditorShapeComponent->SetStaticMesh(EditorShapeMesh);
	if (!EditorShapeMesh) return;

	// 与网格自身的轴心无关：按局部包围盒换算出"直径 2R、高 LiftHeight、底面贴 z=0"的摆放。
	const FBox Local = EditorShapeMesh->GetBoundingBox();
	const FVector Size = Local.GetSize();
	const FVector Scale(
		Size.X > UE_KINDA_SMALL_NUMBER ? 2.0 * Radius / Size.X : 1.0,
		Size.Y > UE_KINDA_SMALL_NUMBER ? 2.0 * Radius / Size.Y : 1.0,
		Size.Z > UE_KINDA_SMALL_NUMBER ? LiftHeight / Size.Z : 1.0);
	EditorShapeComponent->SetRelativeScale3D(Scale);
	EditorShapeComponent->SetRelativeLocation(FVector(0, 0, -Local.Min.Z * Scale.Z));
}

// -----------------------------------------------------------------------------
// AActor
// -----------------------------------------------------------------------------

void ACSGroundShaperActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RebuildTerrain();
}

void ACSGroundShaperActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// 加载/放置后主动登记一次：广播只覆盖"之后的变化"，赶不上地面自己那次重建。
	RebuildTerrain();
}

void ACSGroundShaperActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromGround(/*bRefreshGround=*/false);
	Super::EndPlay(EndPlayReason);
}

void ACSGroundShaperActor::Destroyed()
{
	// 删掉塑形物 = 地形塌回去：这条必须刷新地面，否则隆起会留在镜像里。
	UnregisterFromGround(/*bRefreshGround=*/true);
	Super::Destroyed();
}

#if WITH_EDITOR
void ACSGroundShaperActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RebuildTerrain();
}

void ACSGroundShaperActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// v1 直推：拖动中每帧重导出，但只重算受影响的矩形 + 一个 GPU compute pass（见计划 D3/D9）。
	RebuildTerrain();
}
#endif
