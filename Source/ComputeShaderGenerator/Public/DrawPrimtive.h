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

#include "DrawPrimtive.generated.h"

#define NUM_THREADS_PER_GROUP_DIMENSION_X 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Y 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Z 1

USTRUCT()
struct FCSInstanceData
{
	GENERATED_BODY()
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
};

class FCSDrawPrimtive : public FCustomRenderPassBase
{
};
