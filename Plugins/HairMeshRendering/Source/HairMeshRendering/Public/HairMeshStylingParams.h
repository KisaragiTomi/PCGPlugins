#pragma once

#include "CoreMinimal.h"
#include "HairMeshStylingParams.generated.h"

/** Attenuation curve along strand (root → tip) for a styling effect. */
UENUM(BlueprintType)
enum class EHairStrandAttenuation : uint8
{
	Constant,
	LinearIncrease,
	LinearDecrease,
	EaseIn,
	EaseOut,
};

/** Parameters for a single procedural styling operation. */
USTRUCT(BlueprintType)
struct FHairStylingOp
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bEnabled = true;

	/** Amplitude in world-space units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0"))
	float Amplitude = 0.5f;

	/** Frequency along the strand */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0"))
	float Frequency = 4.0f;

	/** Random seed for per-strand variation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Seed = 0;

	/** How the effect fades along the strand length */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EHairStrandAttenuation Attenuation = EHairStrandAttenuation::LinearIncrease;
};

/**
 * Complete set of procedural styling parameters for a hair mesh.
 * Each operation is applied sequentially to every strand vertex.
 */
USTRUCT(BlueprintType)
struct FHairMeshStylingParams
{
	GENERATED_BODY()

	/** Curl: helical perturbation around the strand axis */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling")
	FHairStylingOp Curl;

	/** Wave: sinusoidal side-to-side offset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling")
	FHairStylingOp Wave;

	/** Frizz: random per-vertex displacement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling")
	FHairStylingOp Frizz;

	/** Clump: pull strands toward bundle center */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling")
	FHairStylingOp Clump;

	/** Global strand width in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling", meta=(ClampMin="0.0001"))
	float StrandWidth = 0.02f;

	/** Tip thinning factor (0 = uniform width, 1 = fully tapered) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling", meta=(ClampMin="0", ClampMax="1"))
	float TipThinning = 0.5f;

	/** Per-strand random length variation (0..1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Styling", meta=(ClampMin="0", ClampMax="1"))
	float LengthVariation = 0.1f;
};
