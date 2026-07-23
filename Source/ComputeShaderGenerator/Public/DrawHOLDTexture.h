#pragma once
//
#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ComputeShaderGenerateHepler.h"
//
#include "DrawHOLDTexture.generated.h"
//
//
// //This struct act as a container for all the parameters that the client needs to pass to the Compute Shader Manager.
struct COMPUTESHADERGENERATOR_API FDRawHLODTextureCSParameters
{
	int X;
	int Y;
	int Z;

	FTextureRenderTargetResource* ColorRT;
	FTextureRenderTargetResource* UVRT;
	FTextureRenderTargetResource* DrawRT;
	
	FDRawHLODTextureCSParameters(int x, int y, int z)
		: X(x)
		, Y(y)
		, Z(z)
	{
	}
};

class COMPUTESHADERGENERATOR_API FDrawHOLDTextureCSInterface
{
public:
	// Executes this shader on the render thread

	static void DispatchRenderThread(FRHICommandListImmediate& RHICmdList, FDRawHLODTextureCSParameters Params);

	// Executes this shader on the render thread from the game thread via EnqueueRenderThreadCommand
	static void DispatchGameThread(FDRawHLODTextureCSParameters Params)
	{
		ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
			[Params](FRHICommandListImmediate& RHICmdList)
			{
				DispatchRenderThread(RHICmdList, Params);
			});
	}

	// Dispatches this shader. Can be called from any thread

	static void Dispatch(FDRawHLODTextureCSParameters Params)
	{
		if (IsInRenderingThread())
		{
			DispatchRenderThread(GetImmediateCommandList_ForRenderCommand(), Params);
		}
		else
		{
			DispatchGameThread(Params);
		}
	}

};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAsyncExecutionCompleted, const int, Value);

UCLASS() // Change the _API to match your project
class COMPUTESHADERGENERATOR_API UDrawHOLDTexture : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:

	// Execute the actual load
	//template<typename T>
	virtual void Activate() override
	{
		// Create a dispatch parameters struct and set our desired seed
		FDRawHLODTextureCSParameters Params(ColorRT->SizeX, ColorRT->SizeY, 1);
		Params.ColorRT = ColorRT->GameThread_GetRenderTargetResource();
		Params.UVRT = UVRT->GameThread_GetRenderTargetResource();
		Params.DrawRT = DrawRT->GameThread_GetRenderTargetResource();

		// Dispatch the compute shader and wait until it completes
		FDrawHOLDTextureCSInterface::Dispatch(Params);
	}

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", Category = "ComputeShader", WorldContext = "WorldContextObject"))
	static UDrawHOLDTexture* ExecuteDrawHoldTexture(UObject* WorldContextObject, UTextureRenderTarget2D* ColorRT, UTextureRenderTarget2D* UVRT, UTextureRenderTarget2D* DrawRT)
	{
		UDrawHOLDTexture* Action = NewObject<UDrawHOLDTexture>();
		Action->ColorRT = ColorRT;
		Action->UVRT = UVRT;
		Action->DrawRT = DrawRT;
		Action->RegisterWithGameInstance(WorldContextObject);
		
		return Action;
	}


	UTextureRenderTarget2D* DrawRT;
	UTextureRenderTarget2D* ColorRT;
	UTextureRenderTarget2D* UVRT;

	TArray<FTransform> CreateRandomTransforms(int32 Num, float SphereRadius);
	
};