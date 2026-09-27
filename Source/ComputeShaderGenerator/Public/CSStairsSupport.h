#pragma once

#include "CSStairs.h"
#include "CSHouseProfile.h"

namespace CSStairs
{
/** Stair-specific planning only. Openings and masonry use the shared house profile/frame/trim routines. */
COMPUTESHADERGENERATOR_API void PlanSupportOpenings(const FRun& Run, const FParams& Params,
	const FGroundSampler& Ground, TArray<FCSWallOpening>& OutOpenings);

/** Append brick masonry under the treads, including through arches and their rings. */
COMPUTESHADERGENERATOR_API void BuildRunSupports(const FRun& Run, const FParams& Params,
	const FGroundSampler& Ground, uint32 Seed, TArray<FBrick>& OutBricks);
}
