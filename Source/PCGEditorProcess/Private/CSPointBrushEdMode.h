#pragma once

#include "CoreMinimal.h"
#include "CSBrushEdModeBase.h"

class ACSPointBrushActor;

/**
 * Paints points onto ACSPointBrushActor.
 *
 * The stroke itself belongs to FCSBrushEdModeBase; this only says which actor is being painted
 * and turns a finished stroke into painted points. Committing is what rebuilds the actor's GPU
 * buffer, so the drag stays free of GPU work.
 */
class FCSPointBrushEdMode : public FCSBrushEdModeBase
{
public:
	static const FEditorModeID EM_CSPointBrush;

	void SetTargetActor(ACSPointBrushActor* InTargetActor);

protected:
	virtual AActor* GetBrushTargetActor() const override;
	virtual FCSBrushSettings GetBrushSettings() const override;
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) override;
	virtual bool IsTooCloseToCommitted(const FVector& Location, float MinSpacingSq) const override;
	virtual bool IsPointAllowed(const FVector& Location) const override;

private:
	TWeakObjectPtr<ACSPointBrushActor> TargetActor;
};
