// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Utilities/MaterialOverrideTool.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MaterialOverrideBPLibrary.generated.h"

class UMaterial;

/**
 * 蓝图函数库 — 提供视口材质覆盖的蓝图调用入口。
 */
UCLASS()
class ASSETHANDLERPLUGIN_API UMaterialOverrideBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Asset Inspection|Material Override",
		Meta = (DisplayName = "Apply Viewport Override Material", Keywords = "material override viewport render temporary"))
	static int32 K2_ApplyViewportOverrideMaterial(
		UMaterial* ViewportMaterial,
		FLinearColor TintColor = FLinearColor::White);
};
