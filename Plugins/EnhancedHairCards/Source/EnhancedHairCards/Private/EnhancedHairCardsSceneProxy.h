#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "EnhancedHairCardsVertexFactory.h"
#include "EnhancedHairCardsDatas.h"
#include "DynamicMeshBuilder.h"

class UEnhancedHairCardsComponent;
class UMaterialInterface;

class FEnhancedHairCardsSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FEnhancedHairCardsSceneProxy(const UEnhancedHairCardsComponent* InComponent);
	virtual ~FEnhancedHairCardsSceneProxy();

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	void UpdateSimulatedGuideCurves_RenderThread(
		FRHICommandListBase& RHICmdList,
		const TArray<FEnhancedHairCardsGuideCurve>& InGuideCurves);

private:
	void BindSourceGroomCardsResource(const UEnhancedHairCardsComponent* InComponent);
	void UploadVertexData(FRHICommandListBase& RHICmdList);
	void UploadPositionData(FRHICommandListBase& RHICmdList);
	void UploadNormalData(FRHICommandListBase& RHICmdList);
	void AppendStoredGuideDebugLines(const TArray<FEnhancedHairCardsGuideCurve>& InGuideCurves);
	void BuildGuideVertexBindings();
	void ApplyGuideDeformation(const TArray<FEnhancedHairCardsGuideCurve>& InGuideCurves);
	void DrawGuideDebugMesh(const FSceneView* View, int32 ViewIndex, FMeshElementCollector& Collector) const;

	struct FEnhancedHairCardsMeshSection
	{
		UMaterialInterface* Material = nullptr;
		uint32 FirstIndex = 0;
		uint32 NumPrimitives = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 GroupIndex = 0;
		bool bCastShadow = true;
	};

	struct FGuideDebugLine
	{
		FVector3f Start = FVector3f::ZeroVector;
		FVector3f End = FVector3f::ZeroVector;
		FLinearColor Color = FLinearColor::White;
	};

	struct FGuideVertexBinding
	{
		int32 GuideIndex = INDEX_NONE;
		int32 SegmentIndex = INDEX_NONE;
		float SegmentAlpha = 0.f;
	};

	FEnhancedHairCardsVertexFactory   VertexFactory;
	FEnhancedHairCardsVertexBuffers   VertexBuffers;

	FEnhancedHairCardsSettings        Settings;
	TArray<FEnhancedHairCardsMeshSection> Sections;
	FMaterialRelevance                MaterialRelevance;
	uint32                            NumUVs = 2;
	bool                              bUsingSourceGroomCardsResource = false;
	const FIndexBuffer*               SourceGroomIndexBuffer = nullptr;

	TArray<FVector3f>  CachedPositions;
	TArray<FVector3f>  CachedRestPositions;
	TArray<FVector3f>  CachedPreviousPositions;
	TArray<float>      CachedPackedPositionW;
	TArray<FVector3f>  CachedTangentX;
	TArray<FVector4f>  CachedTangentZ;
	TArray<FVector3f>  CachedRestTangentX;
	TArray<FVector4f>  CachedRestTangentZ;
	TArray<FVector2f>  CachedUVs;
	TArray<FColor>     CachedColors;
	TArray<FVector4f>  CachedDynamicsData;
	TArray<uint32>     CachedIndices;
	TArray<FEnhancedHairCardsGuideCurve> RestGuideCurves;
	TArray<FGuideVertexBinding> GuideVertexBindings;
	TArray<FGuideDebugLine> GuideDebugLines;
	UMaterialInterface* GuideDebugMaterial = nullptr;
};
