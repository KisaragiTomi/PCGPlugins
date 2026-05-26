#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class AComputeShaderMeshGenerator;
class FEditorViewportClient;
class FPrimitiveDrawInterface;
class FSceneView;
class FViewport;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

struct FCSInstanceBrushPreviewPoint
{
	FVector Location = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	FTransform WorldTransform = FTransform::Identity;
};

class FCSInstanceBrushEdMode : public FEdMode
{
public:
	static const FEditorModeID EM_CSInstanceBrush;

	FCSInstanceBrushEdMode();
	virtual ~FCSInstanceBrushEdMode() override;

	void SetTargetActor(AComputeShaderMeshGenerator* InTargetActor);

	virtual void Enter() override;
	virtual void Exit() override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FCSInstanceBrushEdMode"); }
	virtual bool MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y) override;
	virtual bool CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y) override;
	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event) override;
	virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;
	virtual bool UsesTransformWidget() const override { return false; }
	virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override { return false; }
	virtual bool ShouldDrawWidget() const override { return false; }
	virtual EAxisList::Type GetWidgetAxisToDraw(UE::Widget::EWidgetMode InWidgetMode) const override { return EAxisList::None; }

private:
	void CreateBrushComponent();
	void DestroyBrushComponent();
	void UpdateBrushComponent(FEditorViewportClient* ViewportClient);
	bool UpdateBrushTraceFromMouse(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 MouseX, int32 MouseY);
	bool TraceBrushRay(FEditorViewportClient* ViewportClient, const FVector& RayOrigin, const FVector& RayDirection);
	bool TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	void BeginStroke();
	void UpdateStroke();
	void CommitStroke();
	void CancelStroke();
	void ExitTemporaryMode();
	void SamplePreviewPoints();
	bool IsCandidatePointAllowed(const FVector& Location) const;
	bool IsTooCloseToPendingPoint(const FVector& Location, float MinSpacingSq) const;
	bool IsTooCloseToExistingInstance(const FVector& Location, float MinSpacingSq) const;
	FTransform BuildInstanceTransform(const FHitResult& Hit) const;
	void GetRandomVectorInBrush(FVector& OutStart, FVector& OutEnd) const;
	void DrawPendingPreviewPoints() const;

	TWeakObjectPtr<AComputeShaderMeshGenerator> TargetActor;
	TObjectPtr<UStaticMeshComponent> SphereBrushComponent;
	TObjectPtr<UMaterialInstanceDynamic> BrushMID;

	bool bBrushTraceValid = false;
	bool bStrokeActive = false;
	FVector BrushLocation = FVector::ZeroVector;
	FVector BrushNormal = FVector::UpVector;
	FVector BrushTraceDirection = FVector::ForwardVector;
	TArray<FCSInstanceBrushPreviewPoint> PendingPreviewPoints;
	TArray<FTransform> PendingTransforms;
};
