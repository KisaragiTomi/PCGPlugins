#include "CSGroundPaintEdMode.h"

#include "CSGroundActor.h"
#include "Engine/HitResult.h"

const FEditorModeID FCSGroundPaintEdMode::EM_CSGroundPaint = TEXT("CSGroundPaintEdMode");

void FCSGroundPaintEdMode::SetTargetActor(ACSGroundActor* InTargetActor)
{
	// 换目标前把上一个目标的 stroke 括号关掉，脏包标记不丢。
	if (ACSGroundActor* Previous = TargetActor.Get(); Previous && Previous != InTargetActor) Previous->EndPaintStroke();
	TargetActor = InTargetActor;
	ResetBrushState();
}

AActor* FCSGroundPaintEdMode::GetBrushTargetActor() const
{
	return TargetActor.Get();
}

FCSBrushSettings FCSGroundPaintEdMode::GetBrushSettings() const
{
	FCSBrushSettings Settings;
	const ACSGroundActor* Target = TargetActor.Get();
	if (!Target) return Settings;

	Settings.Radius = Target->BrushRadius;
	Settings.TraceRadius = 0.0f;
	Settings.MinSpacing = 0.0f;        // 顶点色是覆写而非摆放，间距剔除无意义
	Settings.SamplesPerMouseMove = 1;  // 一次落笔就是一次全笔刷球写入
	Settings.bExitAfterCommit = Target->bExitAfterCommit;
	return Settings;
}

void FCSGroundPaintEdMode::SamplePendingPoints()
{
	ACSGroundActor* Target = TargetActor.Get();
	if (!Target || !IsBrushTraceValid()) return;

	// 立即落笔（镜像 + GPU 双写），pending 集保持为空 —— 基类的 stroke 生命周期照常运转，
	// 只是没有 preview point 可画。stroke 括号由 ApplyPaintStroke 自开、CommitSamples 收拢。
	Target->ApplyPaintStroke(GetBrushLocation());
}

void FCSGroundPaintEdMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	FCSBrushEdModeBase::Tick(ViewportClient, DeltaTime);
	if (ACSGroundActor* Target = TargetActor.Get()) Target->FlushPaintToGpu();
}

void FCSGroundPaintEdMode::CommitSamples(const TArray<FCSBrushSample>& /*Samples*/)
{
	// 颜色在拖拽期间已经写完；提交只负责关 stroke 括号（标脏包，后续在这里发布 dirty）。
	if (ACSGroundActor* Target = TargetActor.Get()) Target->EndPaintStroke();
}

void FCSGroundPaintEdMode::ClearBrushTarget()
{
	// Esc / 切模式不经过 CommitStroke，也要把括号关掉 —— 画上去的颜色是真实修改。
	if (ACSGroundActor* Target = TargetActor.Get()) Target->EndPaintStroke();
	TargetActor.Reset();
}

bool FCSGroundPaintEdMode::TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	const ACSGroundActor* Target = TargetActor.Get();
	if (!Target) return false;

	FVector Hit = FVector::ZeroVector;
	if (!Target->RaycastGround(Start, End - Start, Hit)) return false;

	OutHit = FHitResult();
	OutHit.bBlockingHit = true;
	OutHit.ImpactPoint = Hit;
	OutHit.Location = Hit;
	// 平地阶段法线恒 +Z；地面有起伏后（高度笔刷落地）改从镜像差分取。
	OutHit.ImpactNormal = FVector::UpVector;
	OutHit.Normal = FVector::UpVector;
	return true;
}
