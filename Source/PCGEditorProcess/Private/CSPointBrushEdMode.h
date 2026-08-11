#pragma once

#include "CoreMinimal.h"
#include "CSBrushEdModeBase.h"

class ACSPointBrushActor;

/**
 * Paints points onto ACSPointBrushActor.
 *
 * The stroke itself belongs to FCSBrushEdModeBase, but the sampling does not: instead of tracing
 * the world, every stroke update hands the brush sphere to FCSDepthBrushSampleService, which
 * scatters candidates in the brush's screen-space footprint and reads the scene depth buffer.
 * A sample can therefore only land on a surface the viewport actually shows — the sphere's rim
 * used to let trace rays enter meshes and stick to interior faces.
 *
 * Nothing comes back. The survivors are appended straight into the actor's GPU point buffer, so
 * points are committed as they are sampled (Esc no longer takes them back) and the CPU never
 * learns how many there are.
 */
class FCSPointBrushEdMode : public FCSBrushEdModeBase
{
public:
	static const FEditorModeID EM_CSPointBrush;

	void SetTargetActor(ACSPointBrushActor* InTargetActor);

protected:
	virtual AActor* GetBrushTargetActor() const override;
	virtual FCSBrushSettings GetBrushSettings() const override;
	virtual void SamplePendingPoints() override;
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) override;
	virtual void ClearBrushTarget() override;
	virtual bool IsPointAllowed(const FVector& Location) const override;

private:
	TWeakObjectPtr<ACSPointBrushActor> TargetActor;

	/** Decorrelates one stroke update's scatter from the next; the shader hashes it. */
	uint32 SampleSequence = 0;
};
