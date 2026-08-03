#pragma once

#include "CoreMinimal.h"

// One resampled point of one input spline, uploaded to the GPU.
// Positions are in the road actor's local space.
struct FRoadSplinePoint
{
	FVector3f Position;
	float DistanceAlongSpline = 0.0f;
};

// Per-spline range into the flat sample-point array.
struct FRoadSplineRange
{
	uint32 FirstPoint = 0;
	uint32 NumPoints = 0;
	float Length = 0.0f;
	float HalfWidth = 0.0f;
};

// CPU-side snapshot of everything the GPU build needs.
struct FRoadBuildInput
{
	TArray<FRoadSplinePoint> Points;
	TArray<FRoadSplineRange> Splines;

	// Houdini parameter translation (see RoadBuilder.usf for usage).
	float SampleStep = 100.0f;          // resample step in cm
	float RoadHalfWidth = 300.0f;       // half road width in cm
	float IntersectionMergeRadius = 1.0f; // multiplier on road width used to merge nearby crossings

	// U is the longitudinal road coordinate (V spans the width, 0..1). U is derived
	// from the CENTRE-LINE distance and shared across the width ON PURPOSE. Do NOT try
	// to re-parameterise U from the road EDGES to "even out" the curb tiling on bends
	// (this was tried in RoadBuilder.usf and reverted — see below for why it can't win).
	//
	// PITFALL: a curved cross-section has three lines of DIFFERENT length —
	// inner edge < centre < outer edge (a 90 deg, R=500, width=600 bend is
	// ~314 / 785 / 1257 cm, a 4x spread). U is a single scalar per vertex, so it can
	// match at most ONE of them; matching one distorts the others:
	//   * centre-line U : curbs squeezed on the inside / stretched on the outside of a
	//                     bend, BUT no cross-road shear and no seams; the distortion is
	//                     split symmetrically. <-- chosen; the bend squeeze is accepted.
	//   * per-edge   U  : curbs evenly tiled, BUT the cells SHEAR across the width, and
	//                     SEAMS appear where separately-built surface patches meet:
	//                       - the road's centre SPINE, where a leg's two corner halves
	//                         reference its opposite (different-length) edges;
	//                       - a JUNCTION CENTRE, where different splines cross and each
	//                         carries its own U origin/direction (a genuine seam, and it
	//                         is inherent — it exists even with centre-line U).
	// You cannot have "even curbs + square cells + no seams" on a curved surface at once
	// (same reason a map projection can't preserve everything). The only real cure for
	// junction squeeze/seams is a dedicated PER-JUNCTION planar-projected UV patch
	// (detect the intersection polygon, project a texture onto it) — NOT a longitudinal
	// reparameterisation of U.
	float UVLengthScale = 0.001f;       // world cm -> U tiling

	// Conservative allocation sizes, computed on the CPU (never read back).
	uint32 MaxVertices = 0;
	uint32 MaxIndices = 0;
	uint32 MaxIntersections = 0;

	FBox LocalBounds = FBox(ForceInit);
};
