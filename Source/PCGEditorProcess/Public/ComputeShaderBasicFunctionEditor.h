#pragma once
//
#include "CoreMinimal.h"
#include "ComputeShaderGeneral.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Components/SplineComponent.h"
#include "TextureResource.h"

#include "ComputeShaderBasicFunctionEditor.generated.h"



UCLASS()
class PCGEDITORPROCESS_API UComputeShaderBasicFunctionEditor : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static UTextureRenderTarget2D* GenerateHeightNormal(FVector Center, FVector Extent, int32 OutSize = 256);


};

