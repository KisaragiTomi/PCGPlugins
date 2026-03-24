#pragma once

#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "ComputeShaderGenerateHepler.h"
#include "EngineUtils.h"
#include "GlobalDistanceFieldParameters.h"
#include "CommonRenderResources.h"
#include "Containers/DynamicRHIResourceArray.h"



#define NUM_THREADS_PER_GROUP_DIMENSION_X 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Y 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Z 1
/// <summary>
///// This class carries our parameter declarations and acts as the bridge between cpp and HLSL.
/// </summary>
///

class FCalculateGradient : public FGlobalShader
{
public:
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FCalculateGradient);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FCalculateGradient, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_Height)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Gradient)

		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
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

		// if (Parameters.PermutationId == 0)
		// {
		// 	OutEnvironment.SetDefine(TEXT("ENTRY_FUNCTION"), TEXT("Init"));
		// }
		
	}
	
};

IMPLEMENT_GLOBAL_SHADER(FCalculateGradient, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "CalculateGradient", SF_Compute);

class FConnectivityPixel : public FGlobalShader
{
public:
	enum class EConnectivityStep : uint8
	{
		CP_Init,
		CP_FindIslands,
		CP_Count,
		CP_NormalizeResult,
		CP_DrawTexture,
		MAX
	};
	class FConnectivityPixelStep : SHADER_PERMUTATION_ENUM_CLASS("CONNECTIVITYSTEP", EConnectivityStep);
	using FPermutationDomain = TShaderPermutationDomain<FConnectivityPixelStep>;

	static TShaderMapRef<FConnectivityPixel> CreateConnectivityPermutation(EConnectivityStep Permutation)
	{
		typename FConnectivityPixel::FPermutationDomain PermutationVector;
		PermutationVector.Set<FConnectivityPixel::FConnectivityPixelStep>(Permutation);
		TShaderMapRef<FConnectivityPixel> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FConnectivityPixel);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FConnectivityPixel, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ConnectivityPixel)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LabelBufferA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LabelBufferB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SaveBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<int32>, RW_LabelCounters)
		SHADER_PARAMETER(int, Channel)
		SHADER_PARAMETER(int, PieceNum)
		// SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_LabelCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_NormalizeCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_ResultBuffer)

		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	//Called by the engine to determine which permutations to compile for this shader
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
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
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("CONNECTIVITYSTEP_CP_Init"),
			TEXT("CONNECTIVITYSTEP_CP_FindIslands"),
			TEXT("CONNECTIVITYSTEP_CP_Count"),
			TEXT("CONNECTIVITYSTEP_CP_NormalizeResult"),
			TEXT("CONNECTIVITYSTEP_CP_DrawTexture")
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EConnectivityStep::MAX, "Enum doesn't match define table.");
		
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FConnectivityPixelStep>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);
		
	}
};

IMPLEMENT_GLOBAL_SHADER(FConnectivityPixel, "/Plugin/PCGPlugins/Shaders/Private/Connectivity.usf", "ConnectivityPixel", SF_Compute);


class FBlurTexture : public FGlobalShader
{
public:

	enum class EBlurType : uint8
	{
		BT_BLUR3X3,
		BT_BLUR7X7,
		BT_BLUR9X9,
		BT_BLUR15X15,
		BT_BLURNORMAL3X3,
		BT_BLURNORMAL7X7,
		BT_BLURNORMAL9X9,
		BT_BLURNORMAL15X15,
		MAX
	};
	class FBlurFunctionSet : SHADER_PERMUTATION_ENUM_CLASS("BLURTEXTURE", EBlurType);
	using FPermutationDomain = TShaderPermutationDomain<FBlurFunctionSet>;
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FBlurTexture);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FBlurTexture, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_BlurTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_BlurTexture)
		SHADER_PARAMETER(float, BlurScale)

		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	//Called by the engine to determine which permutations to compile for this shader
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
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
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("BLURTEXTURE_BT_BLUR3X3"),
			TEXT("BLURTEXTURE_BT_BLUR7X7"),
			TEXT("BLURTEXTURE_BT_BLUR9X9"),
			TEXT("BLURTEXTURE_BT_BLUR15X15"),
			TEXT("BLURTEXTURE_BT_BLURNORMAL3X3"),
			TEXT("BLURTEXTURE_BT_BLURNORMAL7X7"),
			TEXT("BLURTEXTURE_BT_BLURNORMAL9X9"),
			TEXT("BLURTEXTURE_BT_BLURNORMAL15X15"),
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EBlurType::MAX, "Enum doesn't match define table.");
		
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FBlurFunctionSet>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		EBlurType BlurType = PermutationVector.Get<FBlurFunctionSet>();
		if (BlurType == EBlurType::BT_BLUR3X3 || BlurType == EBlurType::BT_BLURNORMAL3X3)
		{
			OutEnvironment.SetDefine(TEXT("BT_SHAREFIND"), 1);
		}
		if (BlurType == EBlurType::BT_BLUR7X7 || BlurType == EBlurType::BT_BLURNORMAL7X7)
		{
			OutEnvironment.SetDefine(TEXT("BT_SHAREFIND"), 3);
		}
		if (BlurType == EBlurType::BT_BLUR9X9 || BlurType == EBlurType::BT_BLURNORMAL9X9)
		{
			OutEnvironment.SetDefine(TEXT("BT_SHAREFIND"), 4);
		}
		if (BlurType == EBlurType::BT_BLUR15X15 || BlurType == EBlurType::BT_BLURNORMAL15X15)
		{
			OutEnvironment.SetDefine(TEXT("BT_SHAREFIND"), 7);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FBlurTexture, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "BlurTexture", SF_Compute);

class FUpPixelsMask : public FGlobalShader
{
public:

	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FUpPixelsMask);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FUpPixelsMask, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_UpPixel)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_UpPixel)
		SHADER_PARAMETER(float, UpPixelThreshold)
		SHADER_PARAMETER(int, Channel)

		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
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

IMPLEMENT_GLOBAL_SHADER(FUpPixelsMask, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "UpPixel", SF_Compute);

class FGeneralFunctionShader : public FGlobalShader
{
public:
	enum class EGeneralShader : uint8
	{
		GTS_ProcessMeshHeightTexture,
		GTS_BuildTextureArray,
		GTS_ConvertHeightDataToTexture,
		GTS_MaskExtendFast,
		GTS_DrawPrimHeight,
		GTS_DrawTexture16,
		MAX
	};
	class FGeneralFunctionSet : SHADER_PERMUTATION_ENUM_CLASS("GeneralFunction", EGeneralShader);
	using FPermutationDomain = TShaderPermutationDomain<FGeneralFunctionSet>;

	static TShaderMapRef<FGeneralFunctionShader> CreateShaderPermutation(EGeneralShader Permutation)
	{
		typename FGeneralFunctionShader::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGeneralFunctionShader::FGeneralFunctionSet>(Permutation);
		TShaderMapRef<FGeneralFunctionShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FGeneralFunctionShader);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FGeneralFunctionShader, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ProcssTexture0)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ProcssTexture1)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, TA_ProcssTexture0)
	
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ProcssTexture0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ProcssTexture1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, RW_TextureArray0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWB_Float4_0)
		SHADER_PARAMETER(float, InputData0)
		SHADER_PARAMETER(int32, InputIntData0)
		SHADER_PARAMETER(int32, InputIntData1)
		SHADER_PARAMETER(FVector3f, InputVectorData0)
		SHADER_PARAMETER(FVector3f, InputVectorData1)

		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
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
		
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("GTS_PROCESSMESHHEIGHTTEXTURE"),
			TEXT("GTS_BUILDTEXTUREARRAY"),
			TEXT("GTS_CONVERTHEIGHTDATATEXTURE"),
			TEXT("GTS_MASKEXTENDFAST"),
			TEXT("GTS_DRAWPRIMHEIGHT"),
			TEXT("GTS_DRAWTEXTURE16")
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EGeneralShader::MAX, "Enum doesn't match define table.");
		
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FGeneralFunctionSet>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		EGeneralShader FunctionSelect = PermutationVector.Get<FGeneralFunctionSet>();
		if (FunctionSelect == EGeneralShader::GTS_BuildTextureArray)
		{
			OutEnvironment.SetDefine(TEXT("USE_DISTANCEFIELD"), 1);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FGeneralFunctionShader, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "GeneralFunctionSet", SF_Compute);


class FTreeWindShader : public FGlobalShader
{
public:
	
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FTreeWindShader);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FTreeWindShader, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_PivotIndex)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_DirExtent)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_DebugView)
	
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_PivotIndex)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DirExtent)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWB_PivotIndex)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWB_DirExtent)
		SHADER_PARAMETER(float, InputData0)
		SHADER_PARAMETER(int32, InputIntData0)
		SHADER_PARAMETER(int32, InputIntData1)
		SHADER_PARAMETER(FVector3f, InputVectorData0)
		SHADER_PARAMETER(FVector3f, InputVectorData1)

		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
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

IMPLEMENT_GLOBAL_SHADER(FTreeWindShader, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "TreeWind", SF_Compute);


class FDrawPrimHeightVertexBuffer : public FVertexBuffer
{
public:
	/** Initialize the RHI for this rendering resource */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
};


class FSampleSpline : public FGlobalShader
{
public:
	enum class ESampleStep : uint8
	{
		SS_SampleSpline,
		SS_RasterizeSpline,
		MAX
	};
	class FSampleSplineStep : SHADER_PERMUTATION_ENUM_CLASS("GeneralFunction", ESampleStep);
	using FPermutationDomain = TShaderPermutationDomain<FSampleSplineStep>;

	static TShaderMapRef<FSampleSpline> CreateTempShaderPermutation(ESampleStep Permutation)
	{
		typename FSampleSpline::FPermutationDomain PermutationVector;
		PermutationVector.Set<FSampleSpline::FSampleSplineStep>(Permutation);
		TShaderMapRef<FSampleSpline> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FSampleSpline);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FSampleSpline, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ProcssTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SampleResult_DirMinDistRotate)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SampleResult_GradientHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_PointsToSampleBuffer)
	
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SplinePointCount)
		SHADER_PARAMETER(int32, NumSpline)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsSize)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
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
		
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("SS_SAMPLESPLINE"),
			TEXT("SS_RASTERIZESPLINE"),
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ESampleStep::MAX, "Enum doesn't match define table.");
		
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FSampleSplineStep>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		ESampleStep BlurType = PermutationVector.Get<FSampleSplineStep>();
		if (BlurType == ESampleStep::SS_RasterizeSpline)
		{
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 1024);
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 1);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FSampleSpline, "/Plugin/PCGPlugins/Shaders/Private/SampleSpline.usf", "SampleSpline", SF_Compute);


class FGlobalDistanceFieldForCS : public FGlobalShader
{
public:
	enum class ESDFShader : uint8
	{
		GDF_DistanceToNearestSurface,
		MAX
	};
	class FSDFSet : SHADER_PERMUTATION_ENUM_CLASS("GlobalDistanceFieldForCS", ESDFShader);
	using FPermutationDomain = TShaderPermutationDomain<FSDFSet>;

	static TShaderMapRef<FGlobalDistanceFieldForCS> CreateTempShaderPermutation(ESDFShader Permutation)
	{
		typename FGlobalDistanceFieldForCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGlobalDistanceFieldForCS::FSDFSet>(Permutation);
		TShaderMapRef<FGlobalDistanceFieldForCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FGlobalDistanceFieldForCS);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FGlobalDistanceFieldForCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ProcssTexture0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_OutTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ProcssTexture1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PointsToSampleBuffer0)
		SHADER_PARAMETER(float, InputData0)
		SHADER_PARAMETER(int32, InputIntData0)
		SHADER_PARAMETER(int32, InputIntData1)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGlobalDistanceFieldParameters2, GlobalDistanceFieldParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
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
		OutEnvironment.SetDefine(TEXT("USE_DISTANCEFIELD"), 1);
		
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("GDF_DISTANCETONEARESTSURFACE"),
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ESDFShader::MAX, "Enum doesn't match define table.");
		
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FSDFSet>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		ESDFShader FunctionSelect = PermutationVector.Get<FSDFSet>();
		if (FunctionSelect == ESDFShader::GDF_DistanceToNearestSurface)
		{
			OutEnvironment.SetDefine(TEXT("USE_DISTANCEFIELD"), 1);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FGlobalDistanceFieldForCS, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "GeneralFunctionSet", SF_Compute);
