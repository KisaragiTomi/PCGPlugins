#pragma once

#include "Components/BillboardComponent.h"
#include "CSFixedScreenBillboardComponent.generated.h"

/** Billboard texture rendered at a fixed pixel size in editor viewports. */
UCLASS()
class UCSFixedScreenBillboardComponent : public UBillboardComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = Sprite, meta = (ClampMin = "1.0"))
	float ScreenPixelSize = 48.0f;

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
};
