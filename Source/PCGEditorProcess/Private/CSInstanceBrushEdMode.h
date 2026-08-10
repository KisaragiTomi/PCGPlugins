#pragma once

#include "CoreMinimal.h"
#include "CSBrushEdModeBase.h"

class AMeshGeneratorBrushCache;

/**
 * Paints static mesh instances onto AMeshGeneratorBrushCache's HISM components.
 *
 * The stroke itself belongs to FCSBrushEdModeBase; this only says which actor is being painted
 * and turns a finished stroke into instance transforms. Per-instance yaw and scale are randomised
 * at commit time, so a drag costs traces and nothing else.
 */
class FCSInstanceBrushEdMode : public FCSBrushEdModeBase
{
public:
	static const FEditorModeID EM_CSInstanceBrush;

	void SetTargetActor(AMeshGeneratorBrushCache* InTargetActor);

protected:
	virtual AActor* GetBrushTargetActor() const override;
	virtual FCSBrushSettings GetBrushSettings() const override;
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) override;
	virtual bool IsTooCloseToCommitted(const FVector& Location, float MinSpacingSq) const override;
	virtual bool IsPointAllowed(const FVector& Location) const override;
	virtual bool IsReadyToPaint() const override;

private:
	FTransform BuildInstanceTransform(const FCSBrushSample& Sample) const;

	TWeakObjectPtr<AMeshGeneratorBrushCache> TargetActor;
};
