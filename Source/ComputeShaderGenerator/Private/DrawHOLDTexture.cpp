#include "DrawHOLDTexture.h"

#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"

// DECLARE_STATS_GROUP(TEXT("ComputeShaderGenerator"), STATGROUP_ComputeShaderGenerator, STATCAT_Advanced);
// DECLARE_CYCLE_STAT(TEXT("ComputeShaderGenerator Execute"), STAT_ComputeShaderGenerator_Execute, STATGROUP_ComputeShaderGenerator);

#define NUM_THREADS_PER_GROUP_DIMENSION_X 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Y 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Z 1
/// <summary>
///// This class carries our parameter declarations and acts as the bridge between cpp and HLSL.
/// </summary>
/// 
class FDrawHLODTexture : public FGlobalShader
{
public:
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FDrawHLODTexture);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FDrawHLODTexture, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ColorRT)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, UVRT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, DrawRT)
		SHADER_PARAMETER(float, Seed)
		SHADER_PARAMETER_UAV(RWBuffer<float4>, OutBounds)
		SHADER_PARAMETER_SRV(Buffer<float4>, InParticleIndices)
	END_SHADER_PARAMETER_STRUCT()
public:
	//Called by the engine to determine which permutations to compile for this shader
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		

		return true;
	}
	//Modifies the compilations environment of the shader
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		//We're using it here to add some preprocessor defines. That way we don't have to change both C++ and HLSL code 
		// when we change the value for NUM_THREADS_PER_GROUP_DIMENSION
	
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_DIMENSION_Z);
	}
};

// This will tell the engine to create the shader and where the shader entry point is.
//												 ShaderType              ShaderPath             Shader function name    Type
IMPLEMENT_GLOBAL_SHADER(FDrawHLODTexture, "/Plugin/PCGPlugins/Shaders/Private/DrawHLODTexture.usf", "MainComputeShader", SF_Compute);

void FDrawHOLDTextureCSInterface::DispatchRenderThread(FRHICommandListImmediate& RHICmdList, FDRawHLODTextureCSParameters Params)
{
	FRDGBuilder GraphBuilder(RHICmdList);
	{
		// SCOPE_CYCLE_COUNTER(STAT_ComputeShaderGenerator_Execute);
		// DECLARE_GPU_STAT(ComputeShaderGenerator)
		// RDG_EVENT_SCOPE(GraphBuilder, "ComputeShaderGenerator");
		// RDG_GPU_STAT_SCOPE(GraphBuilder, ComputeShaderGenerator);
		
		
		typename FDrawHLODTexture::FPermutationDomain PermutationVector;
	
		TShaderMapRef<FDrawHLODTexture> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
	
		bool bIsShaderValid = ComputeShader.IsValid();
	
		if (bIsShaderValid)
		{
			FDrawHLODTexture::FParameters* PassParameters = GraphBuilder.AllocParameters<FDrawHLODTexture::FParameters>();


			const int32 BufferSize = 64;
			FRHIBufferCreateDesc BoundsCreateDesc = FRHIBufferCreateDesc::Create(
				TEXT("BoundsVertexBuffer"), BufferSize, 0,
				EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::KeepCPUAccessible)
				.DetermineInitialState();
			FBufferRHIRef BoundsVertexBufferRHI = RHICmdList.CreateBuffer(BoundsCreateDesc);
			FUnorderedAccessViewRHIRef BoundsVertexBufferUAV = RHICmdList.CreateUnorderedAccessView(
				BoundsVertexBufferRHI,
				FRHIViewDesc::CreateBufferUAV()
					.SetType(FRHIViewDesc::EBufferType::Typed)
					.SetFormat(PF_A32B32G32R32F));
			PassParameters->OutBounds = BoundsVertexBufferUAV;
			
			//FRHIShaderResourceView* VertexBufferSRV;
			
			FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(Params.ColorRT->GetSizeXY(), PF_FloatRGBA, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
			FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc, TEXT("ComputeShaderGenerator_TempTexture"));
			FRDGTextureRef TargetTexture = RegisterExternalTexture(GraphBuilder, Params.ColorRT->GetRenderTargetTexture(), TEXT("ComputeShaderGenerator_RT"));
			PassParameters->ColorRT = TargetTexture;
			PassParameters->DrawRT = GraphBuilder.CreateUAV(TmpTexture);
			
			//PassParameters->Seed = 1;
			
			auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(Params.X, Params.Y, Params.Z), FComputeShaderUtils::kGolden2DGroupSize);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExecuteExampleComputeShader"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[&PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});
			
			FRDGTextureRef RDGSourceTexture = RegisterExternalTexture(GraphBuilder, Params.DrawRT->GetRenderTargetTexture(), TEXT("DrawHOLDTexture_Gradient"));
			AddCopyTexturePass(GraphBuilder, TmpTexture, RDGSourceTexture, FRHICopyTextureInfo());
		}
	}
	GraphBuilder.Execute();
	
}

TArray<FTransform> UDrawHOLDTexture::CreateRandomTransforms(int32 Num, float SphereRadius)
{
	// FVector Center;
	// FRotator RRot;
	// RRot.Yaw = FMath::FRand() * 360.f;
	// RRot.Pitch = FMath::FRand() * 360.f;
	// RRot.Roll = 0;
	//
	// FVector RandomVector = Center + RRot.Vector() * SphereRadius * -1;
	// set
	TArray<FTransform> Transforms;
	return Transforms;
}
