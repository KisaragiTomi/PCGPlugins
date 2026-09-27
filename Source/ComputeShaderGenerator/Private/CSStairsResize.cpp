#include "CSStairsActor.h"

#include "CSStairsWidthHandleActor.h"
#include "Engine/World.h"

float ACSStairsActor::SetStairWidth(float NewWidth)
{
	if (!FMath::IsFinite(NewWidth)) return Width;
	const float Clamped = FMath::Clamp(NewWidth, 45.0f, 400.0f);
	if (FMath::IsNearlyEqual(Width, Clamped)) return Width;
	Modify();
	Width = Clamped;
	RebuildStairs();
	return Width;
}

bool ACSStairsActor::GetWidthHandleFrame(FVector& Center, FVector& Right, float& WidthScale) const
{
	TArray<CSStairs::FRun> Runs;
	BuildRuns(Runs);
	double Length = 0.0;
	for (const CSStairs::FRun& Run : Runs) for (int32 I = 1; I < Run.Samples.Num(); ++I) Length += FVector::Distance(Run.Samples[I - 1].Position, Run.Samples[I].Position);
	if (Length <= UE_KINDA_SMALL_NUMBER || !FMath::IsFinite(Width) || Width <= UE_KINDA_SMALL_NUMBER) return false;
	double Remaining = Length * 0.5;
	for (const CSStairs::FRun& Run : Runs)
	{
		for (int32 I = 1; I < Run.Samples.Num(); ++I)
		{
			const CSStairs::FSample& A = Run.Samples[I - 1];
			const CSStairs::FSample& B = Run.Samples[I];
			const double Segment = FVector::Distance(A.Position, B.Position);
			if (Segment <= UE_KINDA_SMALL_NUMBER) continue;
			if (Remaining > Segment)
			{
				Remaining -= Segment;
				continue;
			}
			const double T = Remaining / Segment;
			Center = FMath::Lerp(A.Position, B.Position, T);
			FVector2D Dir = FMath::Lerp(A.Dir, B.Dir, T).GetSafeNormal();
			if (Dir.IsNearlyZero()) Dir = A.Dir.GetSafeNormal();
			Right = FVector(-Dir.Y, Dir.X, 0.0);
			WidthScale = FMath::Lerp(A.Width, B.Width, float(T)) / Width;
			return !Right.IsNearlyZero() && FMath::IsFinite(WidthScale) && WidthScale > UE_KINDA_SMALL_NUMBER;
		}
	}
	return false;
}

void ACSStairsActor::EnterResizeMode()
{
	if (IsTemplate() || !GetWorld() || IsActorBeingDestroyed()) return;
	RebuildStairs();
	FVector Center, Right;
	float WidthScale;
	if (!GetWidthHandleFrame(Center, Right, WidthScale)) return;
	ResizeHandles.RemoveAll([](const TObjectPtr<ACSStairsWidthHandleActor>& Handle) { return !IsValid(Handle); });
	for (const int32 Side : { -1, 1 })
	{
		if (ResizeHandles.ContainsByPredicate([Side](const TObjectPtr<ACSStairsWidthHandleActor>& Handle) { return Handle->GetSide() == Side; })) continue;
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.ObjectFlags = RF_Transient | RF_Transactional;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACSStairsWidthHandleActor* Handle = GetWorld()->SpawnActor<ACSStairsWidthHandleActor>(GetActorLocation(), GetActorRotation(), Params);
		if (!Handle) continue;
		Handle->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		Handle->InitializeHandle(this, Side);
#if WITH_EDITOR
		Handle->SetActorLabel(FString::Printf(TEXT("%s_Width_%s"), *GetActorLabel(), Side < 0 ? TEXT("Left") : TEXT("Right")));
#endif
		ResizeHandles.Add(Handle);
	}
	SnapResizeHandles();
}

void ACSStairsActor::ExitResizeMode()
{
	TArray<TObjectPtr<ACSStairsWidthHandleActor>> Doomed = MoveTemp(ResizeHandles);
	ResizeHandles.Reset();
	for (ACSStairsWidthHandleActor* Handle : Doomed) if (IsValid(Handle)) Handle->Destroy();
}

TArray<ACSStairsWidthHandleActor*> ACSStairsActor::GetResizeHandles() const
{
	TArray<ACSStairsWidthHandleActor*> Handles;
	for (ACSStairsWidthHandleActor* Handle : ResizeHandles) if (IsValid(Handle)) Handles.Add(Handle);
	return Handles;
}

bool ACSStairsActor::IsInResizeMode() const
{
	return !GetResizeHandles().IsEmpty();
}

void ACSStairsActor::SnapResizeHandles()
{
	if (ResizeHandles.IsEmpty()) return;
	FVector Center, Right;
	float WidthScale;
	if (!GetWidthHandleFrame(Center, Right, WidthScale))
	{
		ExitResizeMode();
		return;
	}
	for (ACSStairsWidthHandleActor* Handle : ResizeHandles) if (IsValid(Handle)) Handle->SnapToCanonical();
}

void ACSStairsActor::NotifyResizeHandleDestroyed(ACSStairsWidthHandleActor* Handle)
{
	ResizeHandles.Remove(Handle);
}
