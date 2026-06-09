#pragma once

#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "ComputeShaderGenerateHepler.h"
#include "EngineUtils.h"
#include "GlobalDistanceFieldParameters.h"
#include "CommonRenderResources.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "Rendering/CustomRenderPass.h"
#include "ComputeShaderMeshGenerator.h"

#include "DrawPrimtive.generated.h"

#define NUM_THREADS_PER_GROUP_DIMENSION_X 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Y 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Z 1

USTRUCT()
struct FCSInstanceData
{
	GENERATED_BODY()
};

struct COMPUTESHADERGENERATOR_API FCSBoxWorldZHeightRDGInput
{
	FRDGBufferSRVRef TriangleVerticesSRV = nullptr;
	FRDGBufferSRVRef TriangleCounterSRV = nullptr;
	uint32 TriangleCount = 0;
	FTransform BoxTransform = FTransform::Identity;
	FVector BoxSize = FVector::ZeroVector;
	FIntPoint OutputSize = FIntPoint::ZeroValue;
	FRDGTextureRef OutputTexture = nullptr;
	float EmptyHeight = 0.0f;
	const TCHAR* DebugName = TEXT("CS.BoxWorldZHeight");
};

struct COMPUTESHADERGENERATOR_API FCSBoxWorldZHeightRDGOutput
{
	FRDGTextureRef HeightTexture = nullptr;
	FIntPoint OutputSize = FIntPoint::ZeroValue;
};

UCLASS()
class COMPUTESHADERGENERATOR_API UCSDrawPrimtive : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawInstances(UTextureRenderTarget2D* RT_TextureTarget, UTextureRenderTarget2D* RT_Depth, UTextureRenderTarget2D* RT_DebugView, FTransform
	                          CameraTransform, float
	                          CaptureWidth, TArray<FTransform> InstanceTransforms, UStaticMesh* InStaticMesh);

	/** Adds a composable RDG pass that writes top-down box hits as world-space Z height into a PF_R32_FLOAT texture. */
	static FCSBoxWorldZHeightRDGOutput AddBoxWorldZHeightToRDG(
		FRDGBuilder& GraphBuilder,
		const FCSBoxWorldZHeightRDGInput& Input);

	static FCSBoxWorldZHeightRDGOutput AddStaticMeshTrianglesWorldZHeightToRDG(
		FRDGBuilder& GraphBuilder,
		const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
		FTransform BoxTransform,
		FVector BoxSize,
		FIntPoint OutputSize,
		FRDGTextureRef OutputTexture = nullptr,
		float EmptyHeight = 0.0f,
		const TCHAR* DebugName = TEXT("CS.StaticMeshTrianglesWorldZHeight"));

	/** Uploads CPU triangle mesh data and writes top-down box hits as world-space Z height into a PF_R32_FLOAT texture. */
	static FCSBoxWorldZHeightRDGOutput AddTriangleMeshWorldZHeightToRDG(
		FRDGBuilder& GraphBuilder,
		const FCSTriangleMeshData& TriangleData,
		FTransform BoxTransform,
		FVector BoxSize,
		FIntPoint OutputSize,
		FRDGTextureRef OutputTexture = nullptr,
		float EmptyHeight = 0.0f,
		const TCHAR* DebugName = TEXT("CS.TriangleMeshWorldZHeight"));

	/** Blueprint wrapper: gathers scene mesh triangles in BoxTransform/BoxSize, filters by actor tag when set, and writes R32 world Z height. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|DrawPrimtive", meta = (WorldContext = "WorldContextObject"))
	static bool DrawTaggedBoxSceneWorldZHeight(
		UObject* WorldContextObject,
		FTransform BoxTransform,
		FVector BoxSize,
		UTextureRenderTarget2D* HeightRenderTarget,
		FName RequiredActorTag = NAME_None,
		int32 LODIndex = 0,
		int32 MaxTriangles = 200000,
		float EmptyHeight = 0.0f);

	/** Reads a world-Z height render target and draws a fixed 100x100 debug point grid. Duration <= 0 skips debug drawing. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|DrawPrimtive|Debug", meta = (WorldContext = "WorldContextObject"))
	static int32 DrawDebugBoxSceneWorldZHeightSamples(
		UObject* WorldContextObject,
		FTransform BoxTransform,
		FVector BoxSize,
		UTextureRenderTarget2D* HeightRenderTarget,
		float Duration);

	/** Blueprint helper: gathers scene mesh triangles in BoxTransform/BoxSize, filtering by actor tag when set. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|DrawPrimtive", meta = (WorldContext = "WorldContextObject"))
	static FCSTriangleMeshData GetTaggedBoxSceneTriangles(
		UObject* WorldContextObject,
		FTransform BoxTransform,
		FVector BoxSize,
		FName RequiredActorTag = NAME_None,
		int32 LODIndex = 0,
		int32 MaxTriangles = 200000);
};

class FCSDrawPrimtive : public FCustomRenderPassBase
{
};
