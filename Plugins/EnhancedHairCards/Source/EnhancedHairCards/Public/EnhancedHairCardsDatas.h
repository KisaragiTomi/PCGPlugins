#pragma once

#include "CoreMinimal.h"
#include "EnhancedHairCardsDatas.generated.h"

USTRUCT(BlueprintType)
struct FEnhancedHairCardsGuideCurve
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guides")
	int32 SourceGroupIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guides")
	int32 SourceLODIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guides")
	int32 SourceCurveIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guides")
	TArray<FVector> Points;
};

USTRUCT(BlueprintType)
struct FEnhancedHairCardsDynamicsSettings
{
	GENERATED_BODY()

	// Preview-only local offset. Real Groom physics should be driven by UGroomComponent.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation")
	bool bGuideSimulationEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics", meta=(ClampMin="0.0"))
	float Strength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics")
	FVector LocalWindDirection = FVector(0.f, 1.f, 0.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics", meta=(ClampMin="0.0"))
	float WindStrength = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics", meta=(ClampMin="0.0"))
	float FlutterStrength = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics", meta=(ClampMin="0.01"))
	float FlutterFrequency = 1.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics", meta=(ClampMin="0.01"))
	float TipPower = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics")
	float GravityStrength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics")
	bool bInvertRootToTip = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation", meta=(ClampMin="0.0"))
	float GuideStiffness = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GuideDamping = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation", meta=(ClampMin="0", ClampMax="16"))
	int32 GuideConstraintIterations = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GuideMotionInertia = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation")
	bool bGuideRotationEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics|Guide Simulation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GuideRotationStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics", meta=(ClampMin="0.0"))
	float BoundsExtension = 20.0f;

	uint32 PackFlags() const
	{
		return bEnabled ? 1u : 0u;
	}

	float GetMaxDisplacement() const
	{
		return (bEnabled || bGuideSimulationEnabled)
			? BoundsExtension + Strength * (FMath::Abs(WindStrength) + FMath::Abs(FlutterStrength) + FMath::Abs(GravityStrength))
			: 0.f;
	}
};

USTRUCT(BlueprintType)
struct FEnhancedHairCardsGuideDebugSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug")
	bool bDrawGuides = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug")
	bool bDrawRoots = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug")
	bool bDrawInForeground = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug", meta=(ClampMin="0.0"))
	float LineThickness = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug", meta=(ClampMin="0.0"))
	float RootTickSize = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug")
	FLinearColor GuideColor = FLinearColor(169.0f / 255.0f, 7.0f / 255.0f, 228.0f / 255.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Guide Debug")
	FLinearColor RootColor = FLinearColor::Yellow;
};

// Complete card data configuration exposed to Blueprint
USTRUCT(BlueprintType)
struct FEnhancedHairCardsSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rendering")
	bool bRenderCardsMesh = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UV")
	int32 NumUVChannels = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rendering")
	bool bInvertAtlasV = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamics")
	FEnhancedHairCardsDynamicsSettings Dynamics;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug")
	FEnhancedHairCardsGuideDebugSettings GuideDebug;

	uint32 PackFlags() const
	{
		uint32 Flags = 0;
		if (bInvertAtlasV)
		{
			Flags |= 0x40; // EHairCardsVFlags_InvertedUV
		}
		Flags |= 0x80; // EHairCardsVFlags_VertexColor always on
		return Flags;
	}
};
