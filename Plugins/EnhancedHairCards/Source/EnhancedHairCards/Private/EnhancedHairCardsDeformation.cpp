#include "EnhancedHairCardsDeformation.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

IMPLEMENT_GLOBAL_SHADER(
	FEnhancedCardsDeformCS,
	"/Plugin/EnhancedHairCards/Private/EnhancedHairCardsDeformation.usf",
	"MainCS",
	SF_Compute);

namespace EnhancedHairCardsDeformation
{

void DispatchStatic(
	FRHICommandList& RHICmdList,
	uint32 VertexCount,
	FRHIShaderResourceView* RestPositionSRV,
	FRHIShaderResourceView* RestTangentSRV,
	FRHIUnorderedAccessView* DeformedPositionUAV,
	FRHIUnorderedAccessView* DeformedTangentUAV)
{
	if (VertexCount == 0) return;

	FEnhancedCardsDeformCS::FPermutationDomain Perm;
	Perm.Set<FEnhancedCardsDeformCS::FDeformMode>(0); // Static

	TShaderMapRef<FEnhancedCardsDeformCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel), Perm);

	FEnhancedCardsDeformCS::FParameters Params;
	FMemory::Memzero(Params);
	Params.VertexCount           = VertexCount;
	Params.RestPositionBuffer    = RestPositionSRV;
	Params.RestTangentBuffer     = RestTangentSRV;
	Params.DeformedPositionBuffer = DeformedPositionUAV;
	Params.DeformedTangentBuffer  = DeformedTangentUAV;

	const uint32 GroupCount = FMath::DivideAndRoundUp(VertexCount, 64u);
	FComputeShaderUtils::Dispatch(RHICmdList, CS, Params, FIntVector(GroupCount, 1, 1));
}

} // namespace EnhancedHairCardsDeformation
