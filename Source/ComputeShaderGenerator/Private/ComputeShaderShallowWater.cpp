#include "ComputeShaderShallowWater.h"
#include "ComputeShaderGenerateHepler.h"
#include "DrawPrimtive.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "SparseTileDispatchHelper.h"
#include "Landscape.h"
#include "Components/BillboardComponent.h"
#include "Engine/DecalActor.h"
#include "Kismet/GameplayStatics.h"
#include "ClearQuad.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectIterator.h"
#include "DrawDebugHelpers.h"
#include "GDFSampleService.h"
#include "GlobalDistanceFieldParameters.h"
#include "RendererInterface.h"
#include "EngineModule.h"
#include "SceneView.h"
#include "Async/Async.h"

DECLARE_STATS_GROUP(TEXT("CSSW"), STATGROUP_CSSW, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("CSSW Execute"), STAT_CSSW_Execute, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Capture"), STAT_CSSW_Capture, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Total"), STAT_CSSW_Total, STATGROUP_CSSW);
DECLARE_GPU_STAT_NAMED(Stat_ShallowWater, TEXT("ShallowWater"));

static int32 GCSSWUseSparseIndirect = 0;
static FAutoConsoleVariableRef CVarCSSWUseSparseIndirect(
	TEXT("pcg.CSSW.UseSparseIndirect"),
	GCSSWUseSparseIndirect,
	TEXT("Use sparse-tile DispatchIndirect for CSSW simulation passes. Disabled by default to avoid D3D12 ExecuteIndirect failures while opening editor maps."));

static int32 GCSSWBlockGPUDuringConstruction = 1;
static FAutoConsoleVariableRef CVarCSSWBlockGPUDuringConstruction(
	TEXT("pcg.CSSW.BlockGPUDuringConstruction"),
	GCSSWBlockGPUDuringConstruction,
	TEXT("Block CSSW GPU work while an actor is running or finishing ConstructionScript. Enabled by default as an editor crash fallback."));

static int32 GCSSWMaxIterationsPerFrame = 32;
static FAutoConsoleVariableRef CVarCSSWMaxIterationsPerFrame(
	TEXT("pcg.CSSW.MaxIterationsPerFrame"),
	GCSSWMaxIterationsPerFrame,
	TEXT("Maximum CSSW simulation iterations dispatched by one solver frame."));

#ifdef NUM_THREADS_PER_GROUP_DIMENSION_X
#undef NUM_THREADS_PER_GROUP_DIMENSION_X
#endif

#define NUM_THREADS_PER_GROUP_DIMENSION_X 16

#ifdef NUM_THREADS_PER_GROUP_DIMENSION_Y
#undef NUM_THREADS_PER_GROUP_DIMENSION_Y
#endif

#define NUM_THREADS_PER_GROUP_DIMENSION_Y 16



class FShallowWaterSim : public FGlobalShader
{
public:
		
	enum class EShallowWaterSimStep : uint8
	{
		SW_CompactActiveTiles,
		SW_FinalizeCompact,
		SW_VelocityHeightSim,
		SW_ShallowIntegrate,
		SW_Result,
		SW_SetHeight,
		SW_SmoothHeight,
		MAX
	};
	class FShallowWaterSimStep : SHADER_PERMUTATION_ENUM_CLASS("SWS", EShallowWaterSimStep);
	class FSplineRange : SHADER_PERMUTATION_BOOL("USESPLINERANGE");
	class FWaterfallExtentPerm : SHADER_PERMUTATION_ENUM_CLASS("WATERFALL_PERM", EWaterfallExpansion);
	using FPermutationDomain = TShaderPermutationDomain<FShallowWaterSimStep, FSplineRange, FWaterfallExtentPerm>;

	static TShaderMapRef<FShallowWaterSim> CreatePermutation(EShallowWaterSimStep Permutation, bool UseSplineRange = false, EWaterfallExpansion WfExtent = EWaterfallExpansion::Expansion_5)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FSplineRange>(UseSplineRange);
		PermutationVector.Set<FShallowWaterSimStep>(Permutation);
		PermutationVector.Set<FWaterfallExtentPerm>(WfExtent);
		TShaderMapRef<FShallowWaterSim> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	DECLARE_GLOBAL_SHADER(FShallowWaterSim);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterSim, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_VelocityHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_Source)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SplineScaleDist)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_CopyLandscape)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ResultDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ResultSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VelHeightSimA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VelHeightSimB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultVelHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SmoothHeightA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SmoothHeightB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TileMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RWB_SourceUVRads)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWB_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWB_CompactCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWB_CompactIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, B_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, B_CompactCounter)
		RDG_BUFFER_ACCESS(CompactIndirectArgs, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER(int, CloseBound)
		SHADER_PARAMETER(int, BCount_SourceUVRads)
		SHADER_PARAMETER(int, DispatchExpandPixels)

		SHADER_PARAMETER(FVector4f, SourceUVRad)
		SHADER_PARAMETER(FVector4f, ModifierUVRad)
		SHADER_PARAMETER(FVector2f, CopyValidUV)
		SHADER_PARAMETER(float, DT)
		SHADER_PARAMETER(float, Friction)
		SHADER_PARAMETER(float, SeaLevel)
		SHADER_PARAMETER(float, ActorLocationZ)
		SHADER_PARAMETER(float, AdvectFoam)
		SHADER_PARAMETER(float, FoamFadeSpeed)
		SHADER_PARAMETER(float, MaxWaterRisePerFrame)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const auto Step = PermutationVector.Get<FShallowWaterSimStep>();
		const auto WfPerm = PermutationVector.Get<FWaterfallExtentPerm>();
		if (Step != EShallowWaterSimStep::SW_VelocityHeightSim && WfPerm != EWaterfallExpansion::Expansion_5)
			return false;
		return true;
	}
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_DIMENSION_Z);
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("SW_COMPACTACTIVETILES"),
			TEXT("SW_FINALIZECOMPACT"),
			TEXT("SW_VELOCITYHEIGHTSIM"),
			TEXT("SW_SHALLOWINTEGRATE"),
			TEXT("SW_RESULT"),
			TEXT("SW_SETHEIGHT"),
			TEXT("SW_SMOOTHHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EShallowWaterSimStep::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FShallowWaterSimStep>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
		OutEnvironment.SetDefine(TEXT("VELOCITY_CLAMP"), CSSW_VELOCITY_CLAMP);
		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_VelocityHeightSim)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalVelocityHeight_SmoothData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 0);
		}
		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_Result)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalShallowWaterResult_ShareData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 2);
		}
		if (PermutationVector.Get<FSplineRange>() == true)
		{
			OutEnvironment.SetDefine(TEXT("USESPLINERANGE"), 1);
		}

		constexpr int32 WaterfallExtentLUT[] = { 5, 7, 10 };
		const int32 WfIdx = FMath::Clamp(static_cast<int32>(PermutationVector.Get<FWaterfallExtentPerm>()), 0, 2);
		OutEnvironment.SetDefine(TEXT("WATERFALL_EXTENT"), WaterfallExtentLUT[WfIdx]);
	}
};

IMPLEMENT_GLOBAL_SHADER(FShallowWaterSim, "/Plugin/PCGPlugins/Shaders/Private/ShallowWater.usf", "ShallowWater", SF_Compute);

// ─── Terrain Voxel Grid Shaders ─────────────────────────────────────

#define CSSW_VOXEL_FILL_THREADS 64
#define CSSW_VOXEL_SUBSAMPLES 4
#define CSSW_VOXEL_SIM_TILE_SIZE NUM_THREADS_PER_GROUP_DIMENSION_X
#define CSSW_VOXEL_SCAN_BLOCK_SIZE 256

class FShallowWaterVoxelFill : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelFill);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelFill, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, VoxelTriangleData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VoxelTriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VoxelOccupancy)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VoxelCoverage)
		SHADER_PARAMETER(FIntVector, VoxelGridSize)
		SHADER_PARAMETER(float, VoxelBoxExtentXY)
		SHADER_PARAMETER(float, VoxelCellSizeXY)
		SHADER_PARAMETER(float, VoxelGridMinWorldZ)
		SHADER_PARAMETER(float, VoxelInvCellSizeZ)
		SHADER_PARAMETER(float, VoxelCellSizeZ)
		SHADER_PARAMETER(float, VoxelMaxTriangleWorldZ)
		SHADER_PARAMETER(FMatrix44f, WorldToBox)
		SHADER_PARAMETER(uint32, VoxelTriangleCount)
		SHADER_PARAMETER(uint32, bUseVoxelTriangleCounter)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_FILL"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSSW_VOXEL_FILL_THREADS);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_SUBSAMPLES"), CSSW_VOXEL_SUBSAMPLES);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelGDFFill : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelGDFFill);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelGDFFill, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VoxelOccupancy)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_VoxelCoverage)
		SHADER_PARAMETER(FIntVector, VoxelGridSize)
		SHADER_PARAMETER(float, VoxelBoxExtentXY)
		SHADER_PARAMETER(float, VoxelCellSizeXY)
		SHADER_PARAMETER(float, VoxelGridMinWorldZ)
		SHADER_PARAMETER(float, VoxelCellSizeZ)
		SHADER_PARAMETER(float, VoxelMaxTriangleWorldZ)
		SHADER_PARAMETER(FMatrix44f, VoxelBoxToWorld)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGlobalDistanceFieldParameters2, GlobalDistanceFieldParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_GDF_FILL"), 1);
		OutEnvironment.SetDefine(TEXT("USE_DISTANCEFIELD"), 1);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_SUBSAMPLES"), CSSW_VOXEL_SUBSAMPLES);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelCountRuns : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelCountRuns);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelCountRuns, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_VoxelOccupancy)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_ColumnRunCount)
		SHADER_PARAMETER(FIntVector, VoxelGridSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_COUNT_RUNS"), 1);
		OutEnvironment.SetDefine(TEXT("SIM_TILE_SIZE"), CSSW_VOXEL_SIM_TILE_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelEmitRuns : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelEmitRuns);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelEmitRuns, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_VoxelOccupancy)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_ColumnRunStart)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Runs)
		SHADER_PARAMETER(FIntVector, VoxelGridSize)
		SHADER_PARAMETER(uint32, TotalRunCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_EMIT_RUNS"), 1);
		OutEnvironment.SetDefine(TEXT("SIM_TILE_SIZE"), CSSW_VOXEL_SIM_TILE_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelScanBlocks : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelScanBlocks);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelScanBlocks, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_ColumnRunCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_ColumnRunStart)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_BlockSums)
		SHADER_PARAMETER(uint32, ColumnCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_SCAN_BLOCKS"), 1);
		OutEnvironment.SetDefine(TEXT("SCAN_BLOCK_SIZE"), CSSW_VOXEL_SCAN_BLOCK_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelScanBlockSums : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelScanBlockSums);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelScanBlockSums, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_BlockSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TotalCount)
		SHADER_PARAMETER(uint32, NumBlocks)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_SCAN_BLOCKSUMS"), 1);
		OutEnvironment.SetDefine(TEXT("SCAN_BLOCK_SIZE"), CSSW_VOXEL_SCAN_BLOCK_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelAddOffsets : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelAddOffsets);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelAddOffsets, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_BlockSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_ColumnRunStart)
		SHADER_PARAMETER(uint32, ColumnCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_ADD_OFFSETS"), 1);
		OutEnvironment.SetDefine(TEXT("SCAN_BLOCK_SIZE"), CSSW_VOXEL_SCAN_BLOCK_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

class FShallowWaterVoxelBuildHeightMap : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShallowWaterVoxelBuildHeightMap);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterVoxelBuildHeightMap, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_ColumnRunStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_ColumnRunCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_Runs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_VoxelCoverage)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_TileMask)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_PrevResultVelHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SceneDepth)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, B_SourceUVRads)
		SHADER_PARAMETER(FIntVector, VoxelGridSize)
		SHADER_PARAMETER(float, VoxelGridMinWorldZ)
		SHADER_PARAMETER(float, VoxelCellSizeZ)
		SHADER_PARAMETER(int32, bInitialBuild)
		SHADER_PARAMETER(uint32, VoxelCoverageThreshold)
		SHADER_PARAMETER(float, VoxelActorLocationZ)
		SHADER_PARAMETER(float, VoxelMaxRiseAboveLiquid)
		SHADER_PARAMETER(float, VoxelMaxAboveWaterSurface)
		SHADER_PARAMETER(int32, SourceCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_BUILD_HEIGHTMAP"), 1);
		OutEnvironment.SetDefine(TEXT("SIM_TILE_SIZE"), CSSW_VOXEL_SIM_TILE_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
	}
};

IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelFill, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "VoxelFillCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelGDFFill, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "GDFFillCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelCountRuns, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "CountRunsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelScanBlocks, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "ScanBlocksCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelScanBlockSums, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "ScanBlockSumsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelAddOffsets, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "AddOffsetsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelEmitRuns, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "EmitRunsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FShallowWaterVoxelBuildHeightMap, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterVoxel.usf", "BuildHeightMapCS", SF_Compute);


namespace
{
int32 ComputeDispatchExpandPixels(int32 Iteration, int32 TextureResolution)
{
	const int32 IterationMargin = FMath::Max(Iteration, 1) * 2;
	return FMath::Clamp(IterationMargin + 4, 4, FMath::Max(TextureResolution, 4));
}

int32 ClampCSSWIterationsPerFrame(int32 RequestedIterations, const UObject* Context)
{
	const int32 MaxIterations = FMath::Max(GCSSWMaxIterationsPerFrame, 1);
	const int32 ClampedIterations = FMath::Clamp(RequestedIterations, 1, MaxIterations);
	if (ClampedIterations != RequestedIterations)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Clamp iterations for %s: requested=%d clamped=%d max=%d"),
			*GetNameSafe(Context), RequestedIterations, ClampedIterations, MaxIterations);
	}
	return ClampedIterations;
}

bool IsOwnedByCSSWActor(const UObject* Object, const ACSShallowWaterCapture* Owner)
{
	return Object && Owner && (Object->GetOuter() == Owner || Object->GetTypedOuter<ACSShallowWaterCapture>() == Owner);
}

UMaterialInterface* GetNonTransientFallbackMaterial(UMaterialInterface* Candidate, const ACSShallowWaterCapture* Owner)
{
	return IsOwnedByCSSWActor(Candidate, Owner) ? nullptr : Candidate;
}

void ReleaseOwnedRenderTarget(UTextureRenderTarget2D*& RenderTarget, ACSShallowWaterCapture* Owner)
{
	if (!RenderTarget || !IsOwnedByCSSWActor(RenderTarget, Owner))
	{
		return;
	}

	RenderTarget->ReleaseResource();
	RenderTarget = nullptr;
}

template <typename ReadbackType>
void DeleteReadback(ReadbackType*& Readback)
{
	delete Readback;
	Readback = nullptr;
}
}


using namespace CSHepler;

static void ReleaseAllCSSWTransientResources()
{
	int32 ReleasedActorCount = 0;
	for (TObjectIterator<ACSShallowWaterCapture> It; It; ++It)
	{
		ACSShallowWaterCapture* Actor = *It;
		if (!Actor || Actor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Actor->IsTemplate())
		{
			continue;
		}

		Actor->ReleaseTransientRenderResources();
		ReleasedActorCount++;
	}

	UE_LOG(LogTemp, Warning, TEXT("[CSSW] Released transient render resources on %d CSSW actor object(s)."), ReleasedActorCount);
}

static FAutoConsoleCommand CmdCSSWReleaseAllTransientResources(
	TEXT("pcg.CSSW.ReleaseAllTransientResources"),
	TEXT("Release transient render resources on every in-memory CSSW actor, including deleted actors retained by editor undo/preview objects."),
	FConsoleCommandDelegate::CreateStatic(&ReleaseAllCSSWTransientResources));

ACSShallowWaterCapture::ACSShallowWaterCapture()
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50,50,50));
	ReusltMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualizationMesh"));
	ReusltMesh->BoundsScale = 100;
	ReusltMesh->bVisibleInRayTracing = false;
	ReusltMesh->SetupAttachment(SceneComponent, TEXT("VisualizationMesh"));
	SimVisHISM = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("SimVisHISM"));
	SimVisHISM->SetupAttachment(SceneComponent, TEXT("SimVisHISM"));
	SimVisHISM->SetVisibility(false);
	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->bVisibleInRayTracing = false;
	SimVisHISM->NumCustomDataFloats = 0;
	// Water surface is displaced by the material (WPO), so the component's real bounds don't grow.
	// Expand the bounds and disable distance culling so instances are never culled away.
	SimVisHISM->SetCullDistances(0, 0);
	SimVisHISM->BoundsScale = 100.0f;
	CausticsDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("CausticsDecal"));
	CausticsDecal->SetupAttachment(SceneComponent, TEXT("CausticsDecal"));
}

void ACSShallowWaterCapture::ClearSolverTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SolverTimerHandle);
	}
	SolverTimerHandle.Invalidate();
}

bool ACSShallowWaterCapture::IsSolverTimerActive() const
{
	if (UWorld* World = GetWorld())
	{
		return World->GetTimerManager().TimerExists(SolverTimerHandle);
	}
	return SolverTimerHandle.IsValid();
}

void ACSShallowWaterCapture::ScheduleSolverTimerTick()
{
	if (IsActorBeingDestroyed())
	{
		ClearSolverTimer();
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		ClearSolverTimer();
		return;
	}

	const int32 ExpectedSolverReadbackGeneration = SolverReadbackGeneration;
	FTimerDelegate SolverTimerDelegate = FTimerDelegate::CreateWeakLambda(this, [this, ExpectedSolverReadbackGeneration]()
	{
		HandleSolverTimerTick(ExpectedSolverReadbackGeneration);
	});

	if (SolverTimerRate > 0.0f)
	{
		World->GetTimerManager().SetTimer(SolverTimerHandle, SolverTimerDelegate, SolverTimerRate, false);
	}
	else
	{
		SolverTimerHandle = World->GetTimerManager().SetTimerForNextTick(SolverTimerDelegate);
	}
}

void ACSShallowWaterCapture::HandleSolverTimerTick(int32 ExpectedSolverReadbackGeneration)
{
	if (ExpectedSolverReadbackGeneration != SolverReadbackGeneration || IsActorBeingDestroyed())
	{
		return;
	}

	if (!CanRunShallowWaterGPUWork(TEXT("HandleSolverTimerTick")))
	{
		ClearSolverTimer();
		return;
	}

	if (ExpectedSolverReadbackGeneration == SolverReadbackGeneration && !IsActorBeingDestroyed())
	{
		ScheduleSolverTimerTick();
	}
	ShallowWaterSolverSoucePoint(SolverIterationsPerFrame);
}

void ACSShallowWaterCapture::StopSimulationRuntime(bool bResetVisualization)
{
	ClearSolverTimer();
	ResetSolverReadbackState(true, false);

	if (bResetVisualization)
	{
		bSimVisActive = false;
		if (SimVisHISM)
		{
			if (!IsShallowWaterConstructionBlocked())
			{
				ResetSimVisTiles();
			}
			SimVisHISM->SetVisibility(false);
		}
		if (ReusltMesh)
		{
			ReusltMesh->SetVisibility(true);
		}
	}
}

void ACSShallowWaterCapture::UpdateSimulationPreviewMesh()
{
	if (!ReusltMesh) return;

	if (SimulationPreviewMesh && ReusltMesh->GetStaticMesh() != SimulationPreviewMesh)
	{
		ReusltMesh->SetStaticMesh(SimulationPreviewMesh);
	}

	ReusltMesh->SetRelativeScale3D(FVector::OneVector * CaptureSize / 100);

	if (SimulationWaterMaterial)
	{
		WaterMaterial = SimulationWaterMaterial;
	}
	if (WaterMaterial)
	{
		ReusltMesh->SetMaterial(0, WaterMaterial);
	}

	if (SimulationDecalMaterial)
	{
		DecalMaterial = SimulationDecalMaterial;
	}
	if (CausticsDecal && DecalMaterial)
	{
		CausticsDecal->SetDecalMaterial(DecalMaterial);
	}

	ReusltMesh->MarkRenderStateDirty();
}

bool ACSShallowWaterCapture::EnsureSimVisHISMReady()
{
	if (!SimVisHISM) return false;

	if (!SimVisHISM->GetStaticMesh())
	{
		UStaticMesh* PreviewMesh = SimulationPreviewMesh;
		if (!PreviewMesh && ReusltMesh)
		{
			PreviewMesh = ReusltMesh->GetStaticMesh();
		}
		if (!PreviewMesh && DebugMesh)
		{
			PreviewMesh = DebugMesh;
		}
		if (!PreviewMesh) return false;

		SimVisHISM->SetStaticMesh(PreviewMesh);
		if (WaterMaterial)
		{
			SimVisHISM->SetMaterial(0, WaterMaterial);
		}
	}

	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->SetVisibility(bSimVisActive);
	return true;
}

void ACSShallowWaterCapture::ResetSimVisTiles()
{
	if (SimVisHISM)
	{
		SimVisHISM->ClearInstances();
		SimVisHISM->BuildTreeIfOutdated(false, true);
		SimVisHISM->MarkRenderStateDirty();
	}
	SimVisGridCountX = 0;
	SimVisGridCountY = 0;
	SimVisTileWorldSize = 0.0f;
}

void ACSShallowWaterCapture::BuildSimVisInstanceGrid()
{
	if (!EnsureSimVisHISMReady()) return;

	const float SafeCaptureSize = FMath::IsFinite(CaptureSize) && CaptureSize > 0.0f ? CaptureSize : 2000.0f;
	const int32 SafeTextureSize = ResolveTextureSize();

	// Grid resolution mirrors the legacy tile-mask density (TextureSize / threadgroup / subsample),
	// so the visual instance footprint matches the simulation tiling.
	constexpr int32 SimVisSubsampleFactor = 2;
	const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, NUM_THREADS_PER_GROUP_DIMENSION_Y);
	const int32 GridCountX = FMath::Max(1, FMath::DivideAndRoundUp(TileMaskWidth, SimVisSubsampleFactor));
	const int32 GridCountY = FMath::Max(1, FMath::DivideAndRoundUp(TileMaskHeight, SimVisSubsampleFactor));

	const float TileWorldSize = SafeCaptureSize / (float)GridCountX;
	const float HalfCapture = SafeCaptureSize * 0.5f;
	const FVector TileScale(TileWorldSize / 100.0f, TileWorldSize / 100.0f, 1.0f);

	SimVisHISM->ClearInstances();

	TArray<FTransform> Transforms;
	Transforms.Reserve(GridCountX * GridCountY);
	for (int32 TY = 0; TY < GridCountY; TY++)
	{
		for (int32 TX = 0; TX < GridCountX; TX++)
		{
			const float CenterX = (TX + 0.5f) * TileWorldSize - HalfCapture;
			const float CenterY = (TY + 0.5f) * TileWorldSize - HalfCapture;
			Transforms.Emplace(FQuat::Identity, FVector(CenterX, CenterY, 0.0f), TileScale);
		}
	}
	if (Transforms.Num() > 0)
	{
		SimVisHISM->AddInstances(Transforms, false, false);
	}

	SimVisGridCountX = GridCountX;
	SimVisGridCountY = GridCountY;
	SimVisTileWorldSize = TileWorldSize;

	SimVisHISM->BuildTreeIfOutdated(false, true);
	SimVisHISM->MarkRenderStateDirty();
	SimVisHISM->SetVisibility(bSimVisActive);
}

void ACSShallowWaterCapture::ResetSolverReadbackState(bool bAdvanceGeneration, bool bClearCachedResult)
{
	if (bAdvanceGeneration)
	{
		SolverReadbackGeneration++;
		if (SolverReadbackGeneration <= 0)
		{
			SolverReadbackGeneration = 1;
		}
	}

	ResultReadbackWriteIdx = 0;
	LastSolverFrameNumber = 0;
	for (int32 i = 0; i < ReadbackBufferCount; i++)
	{
		ResultReadbackCopyWidth[i] = 0;
		ResultReadbackCopyHeight[i] = 0;
		ResultReadbackGeneration[i] = 0;
	}
	if (bClearCachedResult)
	{
		CachedResultPixels.Reset();
		CachedResultWidth = 0;
		CachedResultHeight = 0;
	}
}

void ACSShallowWaterCapture::ReleaseTransientRenderResources()
{
	ReleaseShallowWaterTransientResources(TEXT("ManualRelease"));
}

void ACSShallowWaterCapture::ReleaseShallowWaterTransientResources(const TCHAR* Context)
{
	UE_LOG(LogTemp, Verbose, TEXT("[CSSW] Release transient render resources for %s on %s."),
		Context ? Context : TEXT("unknown context"), *GetNameSafe(this));

	ClearSolverTimer();
	bSimVisActive = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConstructionDebounceHandle);
		World->GetTimerManager().ClearTimer(DebugViewPlaneTimerHandle);
	}
	ConstructionDebounceHandle.Invalidate();
	DebugViewPlaneTimerHandle.Invalidate();

	TArray<AActor*> DebugViewActors;
	if (UWorld* World = GetWorld())
	{
		UGameplayStatics::GetAllActorsOfClassWithTag(World, AStaticMeshActor::StaticClass(), FName("CSSWVM"), DebugViewActors);
	}
	if (DebugViewPlaneActor.IsValid())
	{
		DebugViewActors.AddUnique(DebugViewPlaneActor.Get());
	}
	for (AActor* DebugActor : DebugViewActors)
	{
		if (!DebugActor)
		{
			continue;
		}

		if (AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(DebugActor))
		{
			if (UStaticMeshComponent* MeshComponent = MeshActor->GetStaticMeshComponent())
			{
				if (UStaticMesh* Mesh = MeshComponent->GetStaticMesh())
				{
					MeshComponent->SetMaterial(0, Mesh->GetMaterial(0));
				}
				MeshComponent->SetStaticMesh(nullptr);
				MeshComponent->MarkRenderStateDirty();
			}
		}
		DebugActor->Tags.Remove(FName("CSSWVM"));
		DebugActor->Destroy();
	}
	DebugViewPlaneActor.Reset();

	auto ResolveFallbackMaterial = [this](UMaterialInterface* Preferred, UMaterialInstanceDynamic* DynamicMaterial)
	{
		if (UMaterialInterface* Fallback = GetNonTransientFallbackMaterial(Preferred, this))
		{
			return Fallback;
		}
		return DynamicMaterial ? GetNonTransientFallbackMaterial(DynamicMaterial->Parent, this) : nullptr;
	};

	// --- 1. Before releasing render targets, clear all texture parameter references
	//    held by Material Instance Dynamics that point to CSSW-owned render targets.
	//    Otherwise RHI may hold GPU memory because those MIDs still reference the textures.
	auto ClearMIDTextureParams = [this](UMaterialInstanceDynamic* MID)
	{
		if (!MID || !IsOwnedByCSSWActor(MID, this))
		{
			return;
		}
		static const FName CSSW_VelHeight(TEXT("CSSW_VelHeight"));
		static const FName CSSW_DepthWet(TEXT("CSSW_DepthWet"));
		static const FName CSSW_SimCenter(TEXT("CSSW_SimCenter"));
		static const FName CSSW_SimInvSize(TEXT("CSSW_SimInvSize"));

		// Unbind render-target texture references so RHI can release the resources.
		MID->SetTextureParameterValue(CSSW_VelHeight, nullptr);
		MID->SetTextureParameterValue(CSSW_DepthWet, nullptr);
		// Reset scalar/vector parameters that point to dynamic sim data.
		MID->SetVectorParameterValue(CSSW_SimCenter, FLinearColor::Black);
		MID->SetScalarParameterValue(CSSW_SimInvSize, 0.0f);
	};

	if (ReusltMesh)
	{
		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(ReusltMesh->GetMaterial(0)))
		{
			if (IsOwnedByCSSWActor(MID, this))
			{
				ClearMIDTextureParams(MID);
				ReusltMesh->SetMaterial(0, ResolveFallbackMaterial(WaterMaterial, MID));
			}
		}
		ReusltMesh->MarkRenderStateDirty();
	}

	if (SimVisHISM)
	{
		SimVisHISM->ClearInstances();
		SimVisHISM->BuildTreeIfOutdated(false, true);
		if (UMaterialInstanceDynamic* HISM_MID = Cast<UMaterialInstanceDynamic>(SimVisHISM->GetMaterial(0)))
		{
			if (IsOwnedByCSSWActor(HISM_MID, this))
			{
				ClearMIDTextureParams(HISM_MID);
				SimVisHISM->SetMaterial(0, ResolveFallbackMaterial(WaterMaterial, HISM_MID));
			}
		}
		SimVisHISM->SetVisibility(false);
		SimVisHISM->MarkRenderStateDirty();
	}

	if (CausticsDecal)
	{
		if (UMaterialInstanceDynamic* DecalMID = Cast<UMaterialInstanceDynamic>(CausticsDecal->GetDecalMaterial()))
		{
			if (IsOwnedByCSSWActor(DecalMID, this))
			{
				ClearMIDTextureParams(DecalMID);
				CausticsDecal->SetDecalMaterial(ResolveFallbackMaterial(DecalMaterial, DecalMID));
			}
		}
	}

	if (IsOwnedByCSSWActor(WaterMaterial, this))
	{
		WaterMaterial = nullptr;
	}
	if (IsOwnedByCSSWActor(DecalMaterial, this))
	{
		DecalMaterial = nullptr;
	}
	VisWaterMaterial = nullptr;
	VisDecalMaterial = nullptr;

	// --- 2. Flush the render thread so all pending render commands finish before
	//    we delete readback objects and release render-target RHI resources.
	if (FApp::CanEverRender() && IsInGameThread())
	{
		FlushRenderingCommands();
	}

	for (int32 i = 0; i < ReadbackBufferCount; i++)
	{
		DeleteReadback(ResultReadback[i]);
	}

	ReleaseTerrainVoxelGrid();

	ReleaseOwnedRenderTarget(RT_DebugView, this);
	ReleaseOwnedRenderTarget(RT_VelocityHeight, this);
	ReleaseOwnedRenderTarget(RT_ResultVelHeight, this);
	ReleaseOwnedRenderTarget(RT_ResultDepthWet, this);
	ReleaseOwnedRenderTarget(RT_Source, this);
	ReleaseOwnedRenderTarget(RT_VoxelTerrain, this);
	ReleaseOwnedRenderTarget(RT_SmoothHeight, this);
	ReleaseOwnedRenderTarget(RT_TileMask, this);

	ResetSolverReadbackState(true, true);
	SimVisGridCountX = 0;
	SimVisGridCountY = 0;
	SimVisTileWorldSize = 0.0f;
	SimUVCenter = FVector2D::ZeroVector;
	SimUVSize = 0.0f;
	SimUVInvSize = 0.0f;
	TextureSize = ResolveTextureSize();

	// --- 3. Double-flush: the ReleaseResource() calls above enqueue render-thread
	//    work to actually free the RHI textures. Must flush again to process those.
	if (FApp::CanEverRender() && IsInGameThread())
	{
		FlushRenderingCommands();
	}
}

void ACSShallowWaterCapture::WaitForPendingShallowWaterRendering(const TCHAR* Context) const
{
	if (GCSSWBlockGPUDuringConstruction == 0 || !FApp::CanEverRender() || !IsInGameThread())
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("[CSSW] Flush render thread before %s on %s."),
		Context ? Context : TEXT("ConstructionScript work"), *GetNameSafe(this));
	FlushRenderingCommands();
}

bool ACSShallowWaterCapture::IsShallowWaterConstructionBlocked() const
{
	return GCSSWBlockGPUDuringConstruction != 0
		&& (bSWConstructionGuardActive || bSWConstructionWorkPending || IsRunningUserConstructionScript());
}

bool ACSShallowWaterCapture::CanRunShallowWaterGPUWork(const TCHAR* Context) const
{
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || IsTemplate() || IsActorBeingDestroyed())
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown)
	{
		return false;
	}

	if (IsShallowWaterConstructionBlocked())
	{
		UE_LOG(LogTemp, Verbose, TEXT("[CSSW] Skip %s while ConstructionScript is active or pending on %s."),
			Context ? Context : TEXT("GPU work"), *GetNameSafe(this));
		return false;
	}

	if (!Box || !ReusltMesh || !CausticsDecal)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Skip %s because required components are missing on %s."),
			Context ? Context : TEXT("GPU work"), *GetNameSafe(this));
		return false;
	}

	return true;
}

int32 ACSShallowWaterCapture::ResolveTextureSize() const
{
	const float SafeCaptureSize = FMath::IsFinite(CaptureSize) && CaptureSize > 0.0f ? CaptureSize : 2000.0f;
	const float SafeWorldPixelSize = FMath::IsFinite(WorldPixelSize) && WorldPixelSize > 0.0f ? WorldPixelSize : 40.0f;
	const int32 RequestedSize = FMath::RoundUpToPowerOfTwo(FMath::Max(16, FMath::CeilToInt32(SafeCaptureSize / SafeWorldPixelSize)));
	return RequestedSize;
}

bool ACSShallowWaterCapture::ShouldTickIfViewportsOnly() const
{
	return false;
}

void ACSShallowWaterCapture::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ACSShallowWaterCapture::PostLoad()
{
	Super::PostLoad();

	StopSimulationRuntime(true);

	if (!SimVisHISM)
	{
		SimVisHISM = FindComponentByClass<UHierarchicalInstancedStaticMeshComponent>();
		if (!SimVisHISM)
		{
			SimVisHISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, TEXT("SimVisHISM"));
			SimVisHISM->SetupAttachment(SceneComponent);
			if (!HasAnyFlags(RF_ClassDefaultObject))
			{
				SimVisHISM->RegisterComponent();
			}
		}
		SimVisHISM->SetVisibility(false);
		SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SimVisHISM->SetCastShadow(false);
		SimVisHISM->bVisibleInRayTracing = false;
		SimVisHISM->NumCustomDataFloats = 0;
		SimVisHISM->SetCullDistances(0, 0);
		SimVisHISM->BoundsScale = 100.0f;
		UE_LOG(LogTemp, Warning, TEXT("ACSShallowWaterCapture::PostLoad - Recreated SimVisHISM for %s"), *GetName());
	}
}

void ACSShallowWaterCapture::BeginPlay()
{
	Super::BeginPlay();

	StopSimulationRuntime(true);
	SetActorTickEnabled(false);
}

void ACSShallowWaterCapture::OnConstruction(const FTransform& Transform)
{
	bSWConstructionWorkPending = true;
	{
		TGuardValue<bool> ConstructionGuard(bSWConstructionGuardActive, true);
		StopSimulationRuntime(true);
		WaitForPendingShallowWaterRendering(TEXT("OnConstruction before releasing old CSSW resources"));
		ReleaseShallowWaterTransientResources(TEXT("OnConstruction old resources"));
		Super::OnConstruction(Transform);
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConstructionDebounceHandle);
		World->GetTimerManager().SetTimer(ConstructionDebounceHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]() { ConstructActor(); }),
			0.01f, false);
	}
	else
	{
		ConstructActor();
	}
}


void ACSShallowWaterCapture::ConstructionComponent()
{
	ConstructActor();
}

void ACSShallowWaterCapture::ConstructActor()
{
	// Safety: skip construction work if the actor is already being destroyed.
	// This can happen if the OnConstruction debounce timer fires after the
	// actor has been deleted from the world but before GC collects it.
	if (IsActorBeingDestroyed())
	{
		bSWConstructionWorkPending = false;
		return;
	}

	bSWConstructionWorkPending = true;
	TGuardValue<bool> ConstructionGuard(bSWConstructionGuardActive, true);
	StopSimulationRuntime(true);
	WaitForPendingShallowWaterRendering(TEXT("ConstructActor"));
	Clean();

	const float SafeCaptureSize = FMath::IsFinite(CaptureSize) && CaptureSize > 0.0f ? CaptureSize : 2000.0f;
	TextureSize = ResolveTextureSize();

	const float EffectiveOrthoWidth = SafeCaptureSize;
	if (Box)
	{
		FVector RelativeScale = FVector(EffectiveOrthoWidth / 100, EffectiveOrthoWidth / 100, MaxHeight / 100);
		Box->SetRelativeScale3D(RelativeScale);
		Box->SetRelativeLocation(FVector(0, 0, Scale3DZ * 50));
		Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	}

	if (CausticsDecal)
	{
		FVector DecalRelativeScale = FVector(MaxHeight / 100, EffectiveOrthoWidth / 100, EffectiveOrthoWidth / 100);
		CausticsDecal->SetRelativeScale3D(DecalRelativeScale);
		CausticsDecal->SetRelativeRotation(FRotator(-90, 0, 0));
		CausticsDecal->DecalSize = FVector(500, 50, 50);
		CausticsDecal->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	}
	if (bUseBakedResultMesh && BakedResultMesh)
	{
		if (ReusltMesh)
		{
			ReusltMesh->SetStaticMesh(BakedResultMesh);
			ReusltMesh->SetRelativeScale3D(FVector::OneVector);
			ReusltMesh->MarkRenderStateDirty();
		}
	}
	else
	{
		UpdateSimulationPreviewMesh();
	}
	if (ReusltMesh)
	{
		ReusltMesh->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	}

	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = SafeCaptureSize;
	SimUVInvSize = SafeCaptureSize > 0.f ? 1.f / SafeCaptureSize : 0.f;

	// Build the full static visualization instance grid once, sized to the whole fluid box.
	// Instances persist for the actor lifetime; the simulation never adds/removes them.
	BuildSimVisInstanceGrid();

	SetActorScale3D(FVector::OneVector);
	ClearSolverTimer();
	ResetSolverReadbackState(true, false);
	bSWConstructionWorkPending = false;
}

void ACSShallowWaterCapture::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	StopSimulationRuntime(true);
	ReleaseShallowWaterTransientResources(TEXT("PreSave"));
	Super::PreSave(ObjectSaveContext);
}

void ACSShallowWaterCapture::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSimulationRuntime(true);
	ReleaseShallowWaterTransientResources(TEXT("EndPlay"));
	Super::EndPlay(EndPlayReason);
}

void ACSShallowWaterCapture::Destroyed()
{
	StopSimulationRuntime(true);
	WaitForPendingShallowWaterRendering(TEXT("Destroyed before releasing CSSW resources"));
	ReleaseShallowWaterTransientResources(TEXT("Destroyed"));
	Super::Destroyed();
}

void ACSShallowWaterCapture::BeginDestroy()
{
	ReleaseShallowWaterTransientResources(TEXT("BeginDestroy"));
	Super::BeginDestroy();
}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePoint(int32 InIteration)
{
	if (!CanRunShallowWaterGPUWork(TEXT("ShallowWaterSolverSoucePoint")))
	{
		ClearSolverTimer();
		return;
	}

	if (bUseBakedResultMesh)
	{
		UseSimulationResultMesh();
	}

	if (GFrameCounter == LastSolverFrameNumber) return;
	LastSolverFrameNumber = GFrameCounter;

	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	InIteration = ClampCSSWIterationsPerFrame(InIteration, this);

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);

	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;

	TArray<FVector4> SourceData = GetSources();
	if (SourceData.Num() == 0)
	{
		// Static instance grid persists; dry tiles render nothing through the water material.
		return;
	}

	TArray<FVector4f> SourceUVRads;
	FBoxSphereBounds Bounds = Box->Bounds;
	FVector ActorLocation = GetActorLocation();
	SourceUVRads.Reserve(SourceData.Num());
	for (int i = 0; i < SourceData.Num(); i++)
	{
		FVector4 PerSourceData = SourceData[i];
		FVector SourceLocation = FVector(PerSourceData.X, PerSourceData.Y, PerSourceData.Z);
		FVector RelativeSourceLocation = SourceLocation - (Bounds.Origin - Bounds.BoxExtent);
		FVector SourceUV = RelativeSourceLocation / ( Bounds.BoxExtent * 2 );
		FVector4f SourceUVRad = FVector4f( SourceUV.X, SourceUV.Y, SourceLocation.Z, PerSourceData.W);
		SourceUVRads.Add(SourceUVRad);
	}
	
	RT_VoxelTerrain->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);
	
	FTextureRenderTargetResource* R_SceneDepth = RT_VoxelTerrain->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();
	if (!R_SceneDepth || !R_DebugView || !R_VelocityHeight || !R_ResultVelHeight || !R_ResultDepthWet || !R_ResultSmoothHeight)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Solver skipped because a render target resource is unavailable on %s."), *GetNameSafe(this));
		return;
	}

	const int32 TileMaskWidth = FMath::DivideAndRoundUp((int32)TextureSize, NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp((int32)TextureSize, NUM_THREADS_PER_GROUP_DIMENSION_Y);
	if (!RT_TileMask)
	{
		RT_TileMask = NewObject<UTextureRenderTarget2D>(this);
	}

	const bool bNeedsTileMaskResourceRebuild =
		RT_TileMask->SizeX != TileMaskWidth ||
		RT_TileMask->SizeY != TileMaskHeight ||
		RT_TileMask->RenderTargetFormat != ETextureRenderTargetFormat::RTF_RGBA16f ||
		!RT_TileMask->bCanCreateUAV;

	RT_TileMask->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	RT_TileMask->bCanCreateUAV = true;
	RT_TileMask->ClearColor = FLinearColor::Black;

	if (bNeedsTileMaskResourceRebuild)
	{
		RT_TileMask->InitAutoFormat(TileMaskWidth, TileMaskHeight);
		RT_TileMask->UpdateResourceImmediate(true);
	}
	FTextureRenderTargetResource* R_TileMask = RT_TileMask->GameThread_GetRenderTargetResource();
	if (!R_TileMask)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Solver skipped because tile mask resource is unavailable on %s."), *GetNameSafe(this));
		return;
	}

	// The visualization instance grid is static (built at construction). RT_TileMask is still produced
	// on the GPU and consumed by the voxel height-map feedback pass, but we no longer read it back to
	// add/remove instances per tick. Dry tiles render nothing through the water material.

	const int32 ResultReadIdx = 1 - ResultReadbackWriteIdx;
	if (ResultReadbackGeneration[ResultReadIdx] == SolverReadbackGeneration && ResultReadback[ResultReadIdx] && ResultReadback[ResultReadIdx]->IsReady())
	{
		int32 RowPitch = 0;
		int32 ReadbackBufferHeight = 0;
		const FFloat16Color* Data = static_cast<const FFloat16Color*>(ResultReadback[ResultReadIdx]->Lock(RowPitch, &ReadbackBufferHeight));
		if (Data && RowPitch > 0)
		{
			const int32 W = ResultReadbackCopyWidth[ResultReadIdx] > 0 ? ResultReadbackCopyWidth[ResultReadIdx] : (int32)TextureSize;
			const int32 H = ResultReadbackCopyHeight[ResultReadIdx] > 0 ? ResultReadbackCopyHeight[ResultReadIdx] : (int32)TextureSize;
			if (W > 0 && H > 0 && RowPitch >= W && ReadbackBufferHeight >= H)
			{
				CachedResultWidth = W;
				CachedResultHeight = H;
				CachedResultPixels.SetNumUninitialized(W * H);
				for (int32 Y = 0; Y < H; Y++)
				{
					FMemory::Memcpy(&CachedResultPixels[Y * W], &Data[Y * RowPitch], W * sizeof(FFloat16Color));
				}
			}
		}
		if (Data)
		{
			ResultReadback[ResultReadIdx]->Unlock();
		}
	}

	InIteration = FMath::Max(InIteration, 1);
	const int32 ResultWriteIdx = ResultReadbackWriteIdx;
	ResultReadbackCopyWidth[ResultWriteIdx] = (int32)TextureSize;
	ResultReadbackCopyHeight[ResultWriteIdx] = (int32)TextureSize;
	ResultReadbackGeneration[ResultWriteIdx] = SolverReadbackGeneration;
	const bool bUseSparseIndirect = GCSSWUseSparseIndirect != 0;
	const EWaterfallExpansion CapturedWaterfallExpansionIterations = WaterfallExpansionIterations;
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedAdvectFoam = AdvectFoam;
	const float CapturedFoamFadeSpeed = FoamFadeSpeed;
	const int32 CapturedCloseBound = CloseBound;
	const int32 CapturedInIteration = InIteration;
	const float CapturedMaxWaterRise = FMath::Max(MaxWaterRisePerFrame, 0.0f);

	// Terrain voxel grid (built once at StartSolver, sparse run-length per column). Rebuilds the sim terrain each frame.
	TRefCountPtr<FRDGPooledBuffer> CapturedVoxelRunStart = bVoxelGridValid ? VoxelColumnRunStartBuffer : nullptr;
	TRefCountPtr<FRDGPooledBuffer> CapturedVoxelRunCount = bVoxelGridValid ? VoxelColumnRunCountBuffer : nullptr;
	TRefCountPtr<FRDGPooledBuffer> CapturedVoxelRuns = bVoxelGridValid ? VoxelRunsBuffer : nullptr;
	TRefCountPtr<FRDGPooledBuffer> CapturedVoxelCoverage = bVoxelGridValid ? VoxelCoverageBuffer : nullptr;
	const FIntVector CapturedVoxelGridSize = VoxelGridSize;
	const float CapturedVoxelGridMinWorldZ = VoxelGridMinWorldZ;
	const float CapturedVoxelCellSizeZ = VoxelCellSizeZ;
	const int32 CapturedVoxelInitialBuild = bVoxelHeightMapInitialized ? 0 : 1;
	const float CapturedVoxelActorLocationZ = ActorLocation.Z;
	const uint32 CapturedVoxelCoverageThreshold = (uint32)FMath::CeilToInt(2.0f / 3.0f * (float)(CSSW_VOXEL_SUBSAMPLES * CSSW_VOXEL_SUBSAMPLES));
	const float CapturedVoxelMaxAboveWaterSurface = FMath::Max(VoxelMaxAboveWaterSurface, 0.0f);
	if (bVoxelGridValid)
	{
		bVoxelHeightMapInitialized = true;
	}

	if (!ResultReadback[ResultWriteIdx])
		ResultReadback[ResultWriteIdx] = new FRHIGPUTextureReadback(TEXT("ResultReadback"));
	FRHIGPUTextureReadback* ResultReadbackForRender = ResultReadback[ResultWriteIdx];

	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight, R_TileMask,
	 SourceUVRads = MoveTemp(SourceUVRads), bUseSparseIndirect, CapturedWaterfallExpansionIterations,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, ActorLocation, CapturedAdvectFoam, CapturedFoamFadeSpeed,
	 CapturedCloseBound, CapturedInIteration, TileMaskWidth, TileMaskHeight,
	 ResultReadbackForRender, CapturedMaxWaterRise, CapturedVoxelRunStart, CapturedVoxelRunCount,
	 CapturedVoxelRuns, CapturedVoxelCoverage,
	 CapturedVoxelGridSize, CapturedVoxelGridMinWorldZ, CapturedVoxelCellSizeZ, CapturedVoxelInitialBuild,
	 CapturedVoxelActorLocationZ, CapturedVoxelCoverageThreshold, CapturedVoxelMaxAboveWaterSurface](FRHICommandListImmediate& RHICmdList)
	{
		SCOPED_GPU_STAT(RHICmdList, Stat_ShallowWater);
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;

			TShaderMapRef<FShallowWaterSim> ComputeShader_CompactActiveTiles = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_CompactActiveTiles);
			TShaderMapRef<FShallowWaterSim> ComputeShader_FinalizeCompact = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_FinalizeCompact);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalSmoothHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SmoothHeight);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim, false, CapturedWaterfallExpansionIterations);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalShallowIntegrate = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_ShallowIntegrate);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			const FIntVector SingleGroupCount(1, 1, 1);
			
			CREATE_TEXTURE_UAV_16_OUT(DebugView)
			CREATE_TEXTURE_UAV_16_OUT(ResultVelHeight)
			CREATE_TEXTURE_UAV_16_OUTP(ResultDepthWet)
			
			CREATE_TEXTURE_UAV_32_OUT(ResultSmoothHeight)
			
			CREATE_TEXTURE_UAV_32(VelHeightSimA)
			CREATE_TEXTURE_UAV_32(VelHeightSimB)
			CREATE_TEXTURE_UAV_32(SmoothHeightA)
			CREATE_TEXTURE_UAV_32(SmoothHeightB)
			
			CREATE_RDG(SceneDepth)
			CREATE_RDG(VelocityHeight)
			
			CREATE_RDG_STRUCTURED_UPLOAD_SRV(SourceUVRads, FVector4f, SourceUVRads, TEXT("SourceUVRads"))

			FCompactTileBuffers CompactBuffers = bUseSparseIndirect
				? CreateCompactTileBuffers(
					GraphBuilder, (uint32)SizeX, (uint32)SizeY,
					NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y)
				: CreateFullScreenCompactTileBuffers(
					GraphBuilder, (uint32)SizeX, (uint32)SizeY,
					NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
			FCompactTileBuffers VisualTileMaskBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
			const FIntVector SimTileGroupCount((int32)CompactBuffers.MaxTileCount, 1, 1);

			FRDGTextureRef TRDG_TileMask = nullptr;
			FRDGTextureUAVRef RDGUAV_TileMask = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TRDG_TileMask, RDGUAV_TileMask, FIntPoint(TileMaskWidth, TileMaskHeight), PF_FloatRGBA, TEXT("UAV_TileMask"), FLinearColor::Black);
			FRDGTextureRef RDG_TileMask = RegisterExternalTexture(GraphBuilder, R_TileMask->GetRenderTargetTexture(), TEXT("TileMask_RT"));

			PassParameters->DT = CapturedDT;
			PassParameters->Friction = CapturedFriction;
			PassParameters->SeaLevel = CapturedSeaLevel;
			PassParameters->ActorLocationZ = ActorLocation.Z;
			PassParameters->AdvectFoam = CapturedAdvectFoam;
			PassParameters->FoamFadeSpeed = CapturedFoamFadeSpeed;
			PassParameters->CloseBound = CapturedCloseBound;
			PassParameters->BCount_SourceUVRads = SourceUVRads.Num();
			PassParameters->RWB_SourceUVRads = SourceUVRadsSRV;
			PassParameters->DispatchExpandPixels = ComputeDispatchExpandPixels(CapturedInIteration, TextureSize.X);
			PassParameters->MaxWaterRisePerFrame = CapturedMaxWaterRise;
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			// Rebuild the sim terrain (T_SceneDepth) from the persistent terrain voxel grid before the
			// solver reads it. Only active tiles are rebuilt; voxels rising > MaxWaterRise above the
			// previous liquid surface are excluded. RDG_TileMask / RDG_ResultVelHeight still hold the
			// previous frame's data at this point.
			if (CapturedVoxelRunStart && CapturedVoxelRunCount && CapturedVoxelRuns && CapturedVoxelCoverage
				&& CapturedVoxelGridSize.X > 0 && CapturedVoxelGridSize.Y > 0 && CapturedVoxelGridSize.Z > 0)
			{
				FRDGBufferRef RunStartBuffer = GraphBuilder.RegisterExternalBuffer(CapturedVoxelRunStart, TEXT("CSSW.VoxelColumnRunStart"));
				FRDGBufferRef RunCountBuffer = GraphBuilder.RegisterExternalBuffer(CapturedVoxelRunCount, TEXT("CSSW.VoxelColumnRunCount"));
				FRDGBufferRef RunsBuffer = GraphBuilder.RegisterExternalBuffer(CapturedVoxelRuns, TEXT("CSSW.VoxelRuns"));
				FRDGBufferRef CoverageBuffer = GraphBuilder.RegisterExternalBuffer(CapturedVoxelCoverage, TEXT("CSSW.VoxelCoverage"));

				FShallowWaterVoxelBuildHeightMap::FParameters* HeightMapParams = GraphBuilder.AllocParameters<FShallowWaterVoxelBuildHeightMap::FParameters>();
				HeightMapParams->B_ColumnRunStart = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RunStartBuffer, PF_R32_UINT));
				HeightMapParams->B_ColumnRunCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RunCountBuffer, PF_R32_UINT));
				HeightMapParams->B_Runs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RunsBuffer, PF_R32_UINT));
				HeightMapParams->B_VoxelCoverage = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CoverageBuffer, PF_R32_UINT));
				HeightMapParams->T_TileMask = RDG_TileMask;
				HeightMapParams->T_PrevResultVelHeight = RDG_ResultVelHeight;
				HeightMapParams->RW_SceneDepth = GraphBuilder.CreateUAV(RDG_SceneDepth);
				HeightMapParams->B_SourceUVRads = SourceUVRadsSRV;
				HeightMapParams->SourceCount = SourceUVRads.Num();
				HeightMapParams->VoxelGridSize = CapturedVoxelGridSize;
				HeightMapParams->VoxelGridMinWorldZ = CapturedVoxelGridMinWorldZ;
				HeightMapParams->VoxelCellSizeZ = CapturedVoxelCellSizeZ;
				HeightMapParams->bInitialBuild = CapturedVoxelInitialBuild;
				HeightMapParams->VoxelCoverageThreshold = CapturedVoxelCoverageThreshold;
				HeightMapParams->VoxelActorLocationZ = CapturedVoxelActorLocationZ;
				HeightMapParams->VoxelMaxRiseAboveLiquid = CapturedMaxWaterRise;
				HeightMapParams->VoxelMaxAboveWaterSurface = CapturedVoxelMaxAboveWaterSurface;

				TShaderMapRef<FShallowWaterVoxelBuildHeightMap> HeightMapShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				const FIntVector HeightMapGroups = FComputeShaderUtils::GetGroupCount(
					FIntVector(CapturedVoxelGridSize.X, CapturedVoxelGridSize.Y, 1),
					FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.BuildHeightMap"), ComputePassFlags,
					HeightMapShader, HeightMapParams, HeightMapGroups);
			}

			AddCopyTexturePass(GraphBuilder, RDG_VelocityHeight, TRDG_VelHeightSimA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, TRDG_SmoothHeightA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultVelHeight, TRDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultDepthWet, TRDG_ResultDepthWet, FRHICopyTextureInfo());

			const FIntVector FullTileGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
			if (bUseSparseIndirect)
			{
				ResetCompactCounter(GraphBuilder, CompactBuffers);

				FShallowWaterSim::FParameters* CompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*CompactPassParameters = *PassParameters;
				CompactPassParameters->B_CompactTileCoords = nullptr;
				CompactPassParameters->B_CompactCounter = nullptr;
				CompactPassParameters->CompactIndirectArgs = nullptr;
				CompactPassParameters->RWB_CompactIndirectArgs = nullptr;
				CompactPassParameters->RW_TileMask = RDGUAV_TileMask;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CompactActiveTiles"),
					ComputePassFlags,
					ComputeShader_CompactActiveTiles,
					CompactPassParameters,
					FullTileGroupCount);

				FShallowWaterSim::FParameters* FinalizeCompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*FinalizeCompactPassParameters = *PassParameters;
				NullifyAllCompactTileBindings(FinalizeCompactPassParameters);
				FinalizeCompactPassParameters->RWB_CompactCounter = CompactBuffers.CounterUAV;
				FinalizeCompactPassParameters->RWB_CompactIndirectArgs = CompactBuffers.IndirectArgsUAV;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FinalizeCompact"),
					ComputePassFlags,
					ComputeShader_FinalizeCompact,
					FinalizeCompactPassParameters,
					SingleGroupCount);
			}


			FRDGTextureRef CurrentVelHeightTextureA = TRDG_VelHeightSimA;
			FRDGTextureRef CurrentVelHeightTextureB = TRDG_VelHeightSimB;
			FRDGTextureRef CurrentSmoothHeightTextureA = TRDG_SmoothHeightA;
			FRDGTextureRef CurrentSmoothHeightTextureB = TRDG_SmoothHeightB;
			FRDGTextureUAVRef CurrentVelHeightSimA = RDGUAV_VelHeightSimA;
			FRDGTextureUAVRef CurrentVelHeightSimB = RDGUAV_VelHeightSimB;
			FRDGTextureUAVRef CurrentSmoothHeightA = RDGUAV_SmoothHeightA;
			FRDGTextureUAVRef CurrentSmoothHeightB = RDGUAV_SmoothHeightB;

			for (int32 i = 0 ; i < CapturedInIteration; i++)
			{
				FShallowWaterSim::FParameters* VelocityHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*VelocityHeightPassParameters = *PassParameters;
				VelocityHeightPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				VelocityHeightPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				VelocityHeightPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				VelocityHeightPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(VelocityHeightPassParameters);
				if (bUseSparseIndirect)
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalVelocityHeight"),
						ComputePassFlags,
						ComputeShader_CalVelocityHeight,
						VelocityHeightPassParameters,
						CompactBuffers.IndirectArgsBuffer,
						0);
				}
				else
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalVelocityHeight"),
						ComputePassFlags,
						ComputeShader_CalVelocityHeight,
						VelocityHeightPassParameters,
						SimTileGroupCount);
				}

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentSmoothHeightTextureA, CurrentSmoothHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
				Swap(CurrentSmoothHeightA, CurrentSmoothHeightB);

				FShallowWaterSim::FParameters* ShallowIntegratePassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*ShallowIntegratePassParameters = *PassParameters;
				ShallowIntegratePassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				ShallowIntegratePassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				ShallowIntegratePassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				ShallowIntegratePassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(ShallowIntegratePassParameters);
				if (bUseSparseIndirect)
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalShallowIntegrate"),
						ComputePassFlags,
						ComputeShader_CalShallowIntegrate,
						ShallowIntegratePassParameters,
						CompactBuffers.IndirectArgsBuffer,
						0);
				}
				else
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalShallowIntegrate"),
						ComputePassFlags,
						ComputeShader_CalShallowIntegrate,
						ShallowIntegratePassParameters,
						SimTileGroupCount);
				}

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
			}

			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			ResultPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			ResultPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			ResultPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
			ResultPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
			NullifyCompactTileUAVs(ResultPassParameters);
			if (bUseSparseIndirect)
			{
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("Result"),
					ComputePassFlags,
					ComputeShader_CalResult,
					ResultPassParameters,
					CompactBuffers.IndirectArgsBuffer,
					0);
			}
			else
			{
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("Result"),
					ComputePassFlags,
					ComputeShader_CalResult,
					ResultPassParameters,
					SimTileGroupCount);
			}

			ResetCompactCounter(GraphBuilder, VisualTileMaskBuffers);
			FShallowWaterSim::FParameters* VisualTileMaskPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*VisualTileMaskPassParameters = *PassParameters;
			BindCompactTileBuffers(VisualTileMaskPassParameters, VisualTileMaskBuffers);
			VisualTileMaskPassParameters->B_CompactTileCoords = nullptr;
			VisualTileMaskPassParameters->B_CompactCounter = nullptr;
			VisualTileMaskPassParameters->CompactIndirectArgs = nullptr;
			VisualTileMaskPassParameters->RWB_CompactIndirectArgs = nullptr;
			VisualTileMaskPassParameters->T_ResultDepthWet = TRDG_ResultDepthWet;
			VisualTileMaskPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			VisualTileMaskPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			VisualTileMaskPassParameters->RW_TileMask = RDGUAV_TileMask;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("CompactActiveTiles_SimVis"),
				ComputePassFlags,
				ComputeShader_CompactActiveTiles,
				VisualTileMaskPassParameters,
				FullTileGroupCount);
			
			AddCopyTexturePass(GraphBuilder, TRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultVelHeight, RDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, CurrentVelHeightTextureA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_TileMask, RDG_TileMask, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();

		ResultReadbackForRender->EnqueueCopy(RHICmdList, R_ResultVelHeight->GetRenderTargetTexture());
	});

	ResultReadbackWriteIdx = 1 - ResultWriteIdx;
}


void ACSShallowWaterCapture::SetHeight()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	
	RT_VoxelTerrain->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);

	FTextureRenderTargetResource* R_SceneDepth = RT_VoxelTerrain->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();
	if (!R_SceneDepth || !R_DebugView || !R_VelocityHeight || !R_ResultVelHeight || !R_ResultDepthWet || !R_ResultSmoothHeight)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] SetHeight skipped because a render target resource is unavailable on %s."), *GetNameSafe(this));
		return;
	}
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedActorLocationZ = GetActorLocation().Z;
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, CapturedActorLocationZ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;
			TShaderMapRef<FShallowWaterSim> ComputeShader_SetHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SetHeight);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("UAV_DebugView")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
			FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_A"));
			FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
			FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));
			FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);
			FRDGTextureRef TmpRDG_ResultDepthWet = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_ResultDepthWet"));
			FRDGTextureUAVRef RDGUAV_ResultDepthWet = GraphBuilder.CreateUAV(TmpRDG_ResultDepthWet);
			FRDGTextureRef TmpRDG_ResultSmoothHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_ResultSmoothHeight"));
			FRDGTextureUAVRef RDGUAV_ResultSmoothHeight = GraphBuilder.CreateUAV(TmpRDG_ResultSmoothHeight);
			FRDGTextureRef RDG_SmoothHeightA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightA"));
			FRDGTextureUAVRef RDGUAV_SmoothHeightA = GraphBuilder.CreateUAV(RDG_SmoothHeightA);
			
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
			FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
			FRDGTextureRef RDG_ResultDepthWet = RegisterExternalTexture(GraphBuilder, R_ResultDepthWet->GetRenderTargetTexture(), TEXT("ResultDepthWet_RT"));
			FRDGTextureRef RDG_ResultSmoothHeight = RegisterExternalTexture(GraphBuilder, R_ResultSmoothHeight->GetRenderTargetTexture(), TEXT("ResultSmoothHeight_RT"));

			FCompactTileBuffers CompactBuffers = CreateFullScreenCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);

			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_VelocityHeight = RDG_VelocityHeight;
			PassParameters->T_ResultDepthWet = RDG_ResultDepthWet;
			PassParameters->T_ResultSmoothHeight = RDG_ResultSmoothHeight;
			PassParameters->DT = CapturedDT;
			PassParameters->Friction = CapturedFriction;
			PassParameters->SeaLevel = CapturedSeaLevel;
			PassParameters->ActorLocationZ = CapturedActorLocationZ;
			PassParameters->BCount_SourceUVRads = 0;
			PassParameters->DispatchExpandPixels = 0;
			PassParameters->MaxWaterRisePerFrame = 1.0e30f; // disable per-frame rise clamp for the SetHeight path
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_ResultVelHeight = RDGUAV_Result;
			PassParameters->RW_ResultDepthWet = RDGUAV_ResultDepthWet;
			PassParameters->RW_ResultSmoothHeight = RDGUAV_ResultSmoothHeight;
			PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
			PassParameters->RW_SmoothHeightA = RDGUAV_SmoothHeightA;
			PassParameters->RWB_SourceUVRads = nullptr;
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, RDG_SmoothHeightA, FRHICopyTextureInfo());

			FShallowWaterSim::FParameters* SetHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*SetHeightPassParameters = *PassParameters;
			NullifyAllCompactTileBindings(SetHeightPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SetHeight"),
				ComputePassFlags,
				ComputeShader_SetHeight,
				SetHeightPassParameters,
				GroupCount);
			
			// Result pass — uses CompactIndirectArgs, only needs SRV reads
			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			NullifyCompactTileUAVs(ResultPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Result"),
				ComputePassFlags,
				ComputeShader_CalResult,
				ResultPassParameters,
				CompactBuffers.IndirectArgsBuffer,
				0);
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
}

void ACSShallowWaterCapture::HeightSmooth()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	
	RT_VoxelTerrain->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);

	FTextureRenderTargetResource* R_SceneDepth = RT_VoxelTerrain->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	if (!R_SceneDepth || !R_DebugView || !R_VelocityHeight || !R_ResultVelHeight)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] HeightSmooth skipped because a render target resource is unavailable on %s."), *GetNameSafe(this));
		return;
	}
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;
			TShaderMapRef<FShallowWaterSim> ComputeShader_SmoothHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SmoothHeight);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("UAV_DebugView")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
			FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_A"));
			FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
			FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));
			FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);

			FRDGTextureRef RDG_SmoothHeightA = nullptr;
			FRDGTextureUAVRef RDGUAV_SmoothHeightA = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, RDG_SmoothHeightA, RDGUAV_SmoothHeightA, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightA"));

			FRDGTextureRef RDG_SmoothHeightB = nullptr;
			FRDGTextureUAVRef RDGUAV_SmoothHeightB = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, RDG_SmoothHeightB, RDGUAV_SmoothHeightB, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightB"));
			
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
			FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
			
			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_SmoothHeightA = RDGUAV_SmoothHeightA;
			PassParameters->RW_SmoothHeightB = RDGUAV_SmoothHeightB;
			PassParameters->BCount_SourceUVRads = 0;
			PassParameters->DispatchExpandPixels = 0;
			PassParameters->RWB_SourceUVRads = nullptr;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SmoothHeight"),
				ComputePassFlags,
				ComputeShader_SmoothHeight,
				PassParameters,
				GroupCount);
			
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
}

void ACSShallowWaterCapture::Clean()
{
	if (!CanRunShallowWaterGPUWork(TEXT("Clean"))) return;

	if (RT_ResultVelHeight)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultVelHeight, FLinearColor(0, 0, -9999, 1));
	}
	if (RT_ResultDepthWet)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultDepthWet,  FLinearColor(-9999, -9999, -9999, -9999));
	}
	if (RT_VelocityHeight)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_VelocityHeight,  FLinearColor(0, 0, -9999, 1));
	}
}

void ACSShallowWaterCapture::CleanDepthWet_Construct()
{
	if (!CanRunShallowWaterGPUWork(TEXT("CleanDepthWet_Construct"))) return;

	if (RT_ResultDepthWet
	&& RT_ResultDepthWet->GetResource()
	&& GetWorld())
	{
		FTextureRenderTargetResource* RenderTargetResource = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
		FLinearColor ClearColor = FLinearColor(-9999, -9999, -9999, -9999);
		ENQUEUE_RENDER_COMMAND(ClearRTCommand)(
			[RenderTargetResource, ClearColor](FRHICommandList& RHICmdList)
		{
			FRHIRenderPassInfo RPInfo(RenderTargetResource->GetRenderTargetTexture(), ERenderTargetActions::DontLoad_Store);
			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::Unknown, ERHIAccess::RTV));
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearRT"));
			DrawClearQuad(RHICmdList, ClearColor);
			RHICmdList.EndRenderPass();

			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::RTV, ERHIAccess::SRVMask));
		});
	}
}

TArray<FVector4> ACSShallowWaterCapture::GetSources()
{
	TArray<FVector4> SourceLocations;
	UWorld* World = GetWorld();
	if (!World) return SourceLocations;

	for (TActorIterator<ACSSHallowWaterSource> It(World, ACSSHallowWaterSource::StaticClass()); It; ++It)
	{
		ACSSHallowWaterSource* Actor = *It;
		FVector Location = Actor->GetActorLocation();
		FBox AABB = FBox::BuildAABB(Box->Bounds.Origin, Box->Bounds.BoxExtent * FVector(1.2, 1.2, 9999));
		
		if (!AABB.IsInsideOrOn(Location)) continue;
		
		SourceLocations.Add(FVector4(Location.X, Location.Y, Location.Z, Actor->GetActorScale3D().X / CaptureSize * 500));
	}

	return SourceLocations;
}

void ACSShallowWaterCapture::CleanupAttachedActors()
{
	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	for (AActor* Actor : AttachedActors)
	{
		Actor->GetRootComponent()->SetVisibility(false);
		TArray<USceneComponent*> Components;
		Actor->GetComponents(USceneComponent::StaticClass(), Components);
		for (USceneComponent* Component : Components)
		{
			Component->SetVisibility(false);
		}
	}
	ReusltMesh->SetVisibility(false);
	CausticsDecal->SetVisibility(true);
}

void ACSShallowWaterCapture::SetMaterialParameter_Implementation()
{
	if (!CanRunShallowWaterGPUWork(TEXT("SetMaterialParameter"))) return;

	if (RT_ResultDepthWet
	&& RT_ResultDepthWet->GetResource()
	&& GetWorld())
	{
		FTextureRenderTargetResource* RenderTargetResource = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
		FLinearColor ClearColor = FLinearColor(-9999, -9999, -9999, -9999);
		ENQUEUE_RENDER_COMMAND(ClearRTCommand)(
			[RenderTargetResource, ClearColor](FRHICommandList& RHICmdList)
		{
			FRHIRenderPassInfo RPInfo(RenderTargetResource->GetRenderTargetTexture(), ERenderTargetActions::DontLoad_Store);
			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::Unknown, ERHIAccess::RTV));
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearRT"));
			DrawClearQuad(RHICmdList, ClearColor);
			RHICmdList.EndRenderPass();

			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::RTV, ERHIAccess::SRVMask));
		});
	}

	if (ReusltMesh && WaterMaterial)
	{
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(ReusltMesh->GetMaterial(0));
		if (!MID || MID->Parent != WaterMaterial)
		{
			MID = UMaterialInstanceDynamic::Create(WaterMaterial, this);
			ReusltMesh->SetMaterial(0, MID);
		}
		MID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		MID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) MID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) MID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
		VisWaterMaterial = MID;
	}

	if (SimVisHISM && WaterMaterial)
	{
		UMaterialInstanceDynamic* HISM_MID = Cast<UMaterialInstanceDynamic>(SimVisHISM->GetMaterial(0));
		if (!HISM_MID || HISM_MID->Parent != WaterMaterial)
		{
			HISM_MID = UMaterialInstanceDynamic::Create(WaterMaterial, this);
			SimVisHISM->SetMaterial(0, HISM_MID);
		}
		HISM_MID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		HISM_MID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) HISM_MID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) HISM_MID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
	}

	if (CausticsDecal && DecalMaterial)
	{
		UMaterialInstanceDynamic* DecalMID = Cast<UMaterialInstanceDynamic>(CausticsDecal->GetDecalMaterial());
		if (!DecalMID || DecalMID->Parent != DecalMaterial)
		{
			DecalMID = UMaterialInstanceDynamic::Create(DecalMaterial, this);
			CausticsDecal->SetDecalMaterial(DecalMID);
		}
		DecalMID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		DecalMID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) DecalMID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) DecalMID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
		VisDecalMaterial = DecalMID;
	}
}

ACSSHallowWaterSource::ACSSHallowWaterSource()
{
	
}


void ACSShallowWaterCapture::ReleaseTerrainVoxelGrid()
{
	const bool bHasAny = VoxelColumnRunStartBuffer.IsValid() || VoxelColumnRunCountBuffer.IsValid()
		|| VoxelRunsBuffer.IsValid() || VoxelCoverageBuffer.IsValid();
	if (FApp::CanEverRender() && IsInGameThread() && bHasAny)
	{
		FlushRenderingCommands();
	}
	VoxelColumnRunStartBuffer.SafeRelease();
	VoxelColumnRunCountBuffer.SafeRelease();
	VoxelRunsBuffer.SafeRelease();
	VoxelCoverageBuffer.SafeRelease();
	VoxelTotalRunCount = 0;
	VoxelGridSize = FIntVector::ZeroValue;
	VoxelBoxExtentXY = 0.0f;
	VoxelCellSizeXY = 0.0f;
	VoxelGridMinWorldZ = 0.0f;
	VoxelCellSizeZ = 0.0f;
	bVoxelGridValid = false;
	bVoxelHeightMapInitialized = false;
}

void ACSShallowWaterCapture::VisualizeVoxelRuns(float Duration)
{
	if (!bVoxelGridValid || !VoxelColumnRunStartBuffer.IsValid()
		|| !VoxelColumnRunCountBuffer.IsValid() || !VoxelRunsBuffer.IsValid()
		|| VoxelGridSize.X <= 0 || VoxelGridSize.Y <= 0 || VoxelTotalRunCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] VisualizeVoxelRuns: no valid voxel grid on %s. Run StartSolver first."), *GetNameSafe(this));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const uint32 ColumnCount = (uint32)VoxelGridSize.X * (uint32)VoxelGridSize.Y;
	const uint32 RunCapacity = VoxelTotalRunCount;

	TArray<uint32> RunStarts;
	TArray<uint32> RunCounts;
	TArray<uint32> Runs;
	RunStarts.SetNumZeroed(ColumnCount);
	RunCounts.SetNumZeroed(ColumnCount);
	Runs.SetNumZeroed(RunCapacity);

	TRefCountPtr<FRDGPooledBuffer> CapturedRunStart = VoxelColumnRunStartBuffer;
	TRefCountPtr<FRDGPooledBuffer> CapturedRunCount = VoxelColumnRunCountBuffer;
	TRefCountPtr<FRDGPooledBuffer> CapturedRuns = VoxelRunsBuffer;

	ENQUEUE_RENDER_COMMAND(CSSWVoxelVisReadback)(
		[CapturedRunStart, CapturedRunCount, CapturedRuns, ColumnCount, RunCapacity,
		 StartDst = RunStarts.GetData(), CountDst = RunCounts.GetData(), RunDst = Runs.GetData()]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRHIGPUBufferReadback StartReadback(TEXT("CSSW.VoxelVisRunStart"));
		FRHIGPUBufferReadback CountReadback(TEXT("CSSW.VoxelVisRunCount"));
		FRHIGPUBufferReadback RunReadback(TEXT("CSSW.VoxelVisRuns"));

		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RunStartBuf = GraphBuilder.RegisterExternalBuffer(CapturedRunStart, TEXT("CSSW.VisRunStart"));
			FRDGBufferRef RunCountBuf = GraphBuilder.RegisterExternalBuffer(CapturedRunCount, TEXT("CSSW.VisRunCount"));
			FRDGBufferRef RunsBuf = GraphBuilder.RegisterExternalBuffer(CapturedRuns, TEXT("CSSW.VisRuns"));
			AddEnqueueCopyPass(GraphBuilder, &StartReadback, RunStartBuf, ColumnCount * sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, &CountReadback, RunCountBuf, ColumnCount * sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, &RunReadback, RunsBuf, RunCapacity * sizeof(uint32));
			GraphBuilder.Execute();
		}

		RHICmdList.SubmitAndBlockUntilGPUIdle();

		auto CopyBack = [](FRHIGPUBufferReadback& Readback, uint32* Dst, uint32 Count)
		{
			if (Readback.IsReady())
			{
				if (const uint32* Src = static_cast<const uint32*>(Readback.Lock(Count * sizeof(uint32))))
				{
					FMemory::Memcpy(Dst, Src, Count * sizeof(uint32));
					Readback.Unlock();
				}
			}
		};
		CopyBack(StartReadback, StartDst, ColumnCount);
		CopyBack(CountReadback, CountDst, ColumnCount);
		CopyBack(RunReadback, RunDst, RunCapacity);
	});

	FlushRenderingCommands();

	// Reconstruct world-space geometry exactly like the build/height-map passes:
	//   XY cell center in box-local: -BoxExtentXY + (cell + 0.5) * CellSizeXY, transformed by the box frame.
	//   Z run endpoints in world space: MinWorldZ + cell * CellSizeZ.
	const FBoxSphereBounds Bounds = Box ? Box->Bounds : FBoxSphereBounds(GetActorLocation(), FVector::ZeroVector, 0.f);
	const FTransform BoxTransform(GetActorQuat(), Bounds.Origin, FVector::OneVector);

	const int32 GridX = VoxelGridSize.X;
	const int32 GridY = VoxelGridSize.Y;
	const float ExtentXY = VoxelBoxExtentXY;
	const float CellXY = VoxelCellSizeXY;
	const float MinZ = VoxelGridMinWorldZ;
	const float CellZ = VoxelCellSizeZ;
	const float LineThickness = FMath::Max(CellXY * 0.05f, 1.0f);

	int32 DrawnRuns = 0;
	for (int32 Y = 0; Y < GridY; ++Y)
	{
		const float LocalY = -ExtentXY + (Y + 0.5f) * CellXY;
		for (int32 X = 0; X < GridX; ++X)
		{
			const uint32 Col = (uint32)Y * (uint32)GridX + (uint32)X;
			const uint32 Count = RunCounts[Col];
			if (Count == 0)
			{
				continue;
			}
			const uint32 Start = RunStarts[Col];
			const float LocalX = -ExtentXY + (X + 0.5f) * CellXY;
			const FVector WorldXY = BoxTransform.TransformPosition(FVector(LocalX, LocalY, 0.0));

			for (uint32 r = 0; r < Count; ++r)
			{
				const uint32 Idx = Start + r;
				if (Idx >= RunCapacity)
				{
					break;
				}
				const uint32 Packed = Runs[Idx];
				const uint32 ZStart = Packed & 0xFFFFu;
				const uint32 ZEnd = (Packed >> 16u) & 0xFFFFu;

				const double BottomZ = MinZ + (double)ZStart * CellZ;
				const double TopZ = MinZ + (double)(ZEnd + 1) * CellZ;

				const FVector LineStart(WorldXY.X, WorldXY.Y, BottomZ);
				const FVector LineEnd(WorldXY.X, WorldXY.Y, TopZ);

				DrawDebugLine(World, LineStart, LineEnd, FColor::Cyan, /*bPersistent=*/false, Duration, /*DepthPriority=*/0, LineThickness);
				++DrawnRuns;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[CSSW] VisualizeVoxelRuns: %s drew %d runs (Grid=%dx%dx%d, Duration=%.1fs)."),
		*GetName(), DrawnRuns, GridX, GridY, VoxelGridSize.Z, Duration);
}

void ACSShallowWaterCapture::BuildTerrainVoxelGrid()
{
	ReleaseTerrainVoxelGrid();

	if (!CanRunShallowWaterGPUWork(TEXT("BuildTerrainVoxelGrid")) || !Box)
	{
		return;
	}

	const int32 GridXY = FMath::Max(16, (int32)ResolveTextureSize());
	int32 GridZ = GridXY; // request full Z resolution; sparse storage keeps persistent memory tiny

	// The persistent grid is sparse (per-column runs), but the build still rasterizes into a
	// TRANSIENT dense bit grid that is freed immediately after StartSolver. Bound GridZ by that
	// transient budget so the scratch buffer stays sane and the bit index never overflows uint32.
	// GridXY must equal the sim resolution (the height map writes 1:1 into the terrain texture),
	// so only GridZ is clamped. Z runs are encoded with 16-bit endpoints, so GridZ <= 65535.
	{
		constexpr uint64 MaxTransientDenseBits = 2048ull * 1024ull * 1024ull; // 2 Gbit => 256 MB transient scratch
		const uint64 XYCells = (uint64)GridXY * (uint64)GridXY;
		const int32 MaxGridZ = (int32)FMath::Clamp<uint64>(XYCells > 0 ? MaxTransientDenseBits / XYCells : 1, 1, 65535);
		if (GridZ > MaxGridZ)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSSW] BuildTerrainVoxelGrid: clamped GridZ %d -> %d to bound transient voxel scratch (GridXY=%d) on %s."),
				GridZ, MaxGridZ, GridXY, *GetNameSafe(this));
			GridZ = MaxGridZ;
		}
	}

	const float SafeCaptureSize = FMath::IsFinite(CaptureSize) && CaptureSize > 0.0f ? CaptureSize : 2000.0f;

	// Box-local XY footprint matches the simulation extent (CaptureSize x CaptureSize).
	const float BoxExtentXY = SafeCaptureSize * 0.5f;
	const float CellSizeXY = SafeCaptureSize / (float)GridXY;

	// Gather scene triangles (geometry + landscape) inside the simulation box.
	// Extend the capture volume DOWNWARD by one extra box height so terrain below the fluid box
	// is still voxelized (total capture depth = 2x the fluid box height). The top stays at the
	// fluid box top; the source-Z cap above still trims overhead geometry.
	const FBoxSphereBounds Bounds = Box->Bounds;
	const double BoxHalfHeightZ = Bounds.BoxExtent.Z;                 // H/2 (fluid box half height)
	const double CaptureTopZ = Bounds.Origin.Z + BoxHalfHeightZ;      // unchanged: fluid box top
	const double CaptureBottomZ = Bounds.Origin.Z - BoxHalfHeightZ - (BoxHalfHeightZ * 2.0); // box bottom - H
	const double CaptureCenterZ = (CaptureTopZ + CaptureBottomZ) * 0.5;
	const double CaptureHeightZ = CaptureTopZ - CaptureBottomZ;       // = 2H
	const FVector CaptureOrigin(Bounds.Origin.X, Bounds.Origin.Y, CaptureCenterZ);
	const FTransform BoxTransform(GetActorQuat(), CaptureOrigin, FVector::OneVector);
	const FVector BoxSize(SafeCaptureSize, SafeCaptureSize, CaptureHeightZ);

	// Compute the world-Z cap from the highest source point. Scene geometry above this cap (plus a
	// configurable margin) is excluded from the terrain voxelization so source-overhead meshes never
	// turn into terrain. With no sources, use a sentinel that disables the cap.
	float MaxTriangleWorldZ = TNumericLimits<float>::Max();
	{
		const TArray<FVector4> CapSourceData = GetSources();
		float HighestSourceZ = -TNumericLimits<float>::Max();
		for (const FVector4& Src : CapSourceData)
		{
			HighestSourceZ = FMath::Max(HighestSourceZ, (float)Src.Z);
		}
		if (HighestSourceZ > -TNumericLimits<float>::Max())
		{
			MaxTriangleWorldZ = HighestSourceZ + FMath::Max(VoxelMaxAboveSourceZ, 0.0f);
		}
	}

	// Gather LANDSCAPE-ONLY triangles. The GDF already covers all solid scene geometry; the landscape
	// is merged separately because it is the authored terrain surface. A reserved tag that no actor
	// carries excludes every static mesh, while bAlwaysIncludeLandscape still pulls the terrain in.
	static const FName LandscapeOnlyTag(TEXT("__CSSW_LandscapeOnly__"));
	FCSTriangleMeshData TriangleData = UCSDrawPrimtive::GetTaggedBoxSceneTriangles(
		this, BoxTransform, BoxSize, LandscapeOnlyTag, /*LODIndex=*/0, FMath::Max(1, VoxelMaxSceneTriangles),
		/*bAlwaysIncludeLandscape=*/true);

	TArray<FVector4f> TriangleVertices;
	const int32 EffectiveVertexCount = TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
	const int32 TriangleCount = EffectiveVertexCount / 3;
	// No early bail on zero triangles: the GDF fill alone can still produce terrain occupancy.
	TriangleVertices.Reserve(FMath::Max(TriangleCount * 3, 0));
	for (int32 v = 0; v < TriangleCount * 3; v++)
	{
		const FVector& P = TriangleData.Vertices[v];
		TriangleVertices.Add(FVector4f((float)P.X, (float)P.Y, (float)P.Z, 1.0f));
	}

	// Voxel volume Z spans the whole capture box (the GDF fills the entire volume, not just where
	// landscape triangles sit). The grid Z axis is world Z, matching the rasterizer convention.
	const float MinWorldZ = (float)CaptureBottomZ;
	const float MaxWorldZ = (float)CaptureTopZ;
	const float WorldHeight = FMath::Max(MaxWorldZ - MinWorldZ, 1.0f);
	const float CellSizeZ = WorldHeight / (float)GridZ;

	VoxelGridSize = FIntVector(GridXY, GridXY, GridZ);
	VoxelBoxExtentXY = BoxExtentXY;
	VoxelCellSizeXY = CellSizeXY;
	VoxelGridMinWorldZ = MinWorldZ;
	VoxelCellSizeZ = CellSizeZ;

	const uint64 TotalBits = (uint64)GridXY * (uint64)GridXY * (uint64)GridZ;
	const uint32 OccupancyWordCount = (uint32)FMath::DivideAndRoundUp<uint64>(TotalBits, 32ull);
	const uint32 ColumnCount = (uint32)GridXY * (uint32)GridXY;
	const FMatrix44f WorldToBox(FMatrix44f(BoxTransform.Inverse().ToMatrixWithScale()));
	const FMatrix44f BoxToWorld(FMatrix44f(BoxTransform.ToMatrixWithScale()));
	const uint32 NumTriangles = (uint32)FMath::Max(TriangleCount, 0);
	const FIntVector LocalGridSize = VoxelGridSize;
	const float MaxTriangleWorldZCaptured = MaxTriangleWorldZ;

	// ── Build the sparse terrain voxel grid fully on the GPU, on the engine's GDF-ready RDG (async).
	//   VoxelFill(landscape) -> GDFFill(OR-merge) -> CountRuns -> ScanBlocks -> ScanBlockSums ->
	//   AddOffsets -> EmitRuns. The Runs buffer is preallocated by an upper bound (GridZ/2+1 runs per
	//   column), so the whole chain stays on one graph with ZERO CPU round-trip (no readback, no Flush).
	const uint32 NumScanBlocks = (uint32)FMath::DivideAndRoundUp(ColumnCount, (uint32)CSSW_VOXEL_SCAN_BLOCK_SIZE);
	if (NumScanBlocks > (uint32)CSSW_VOXEL_SCAN_BLOCK_SIZE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] BuildTerrainVoxelGrid: column count %u exceeds single-pass scan capacity (%d) on %s. Reduce TextureSize."),
			ColumnCount, CSSW_VOXEL_SCAN_BLOCK_SIZE * CSSW_VOXEL_SCAN_BLOCK_SIZE, *GetNameSafe(this));
		return;
	}

	// Upper bound on vertical runs per column: alternating solid/empty caps at GridZ/2, +1 for a
	// leading run. Over-allocation is accepted to keep everything on one RDG with no CPU readback.
	const uint32 RunCapacityPerColumn = (uint32)(GridZ / 2 + 1);
	const uint32 RunCapacity = FMath::Max(ColumnCount * RunCapacityPerColumn, 1u);
	const int32 TriangleVertexCount = TriangleVertices.Num();
	VoxelTotalRunCount = RunCapacity;

	TWeakObjectPtr<ACSShallowWaterCapture> WeakThis(this);
	FGDFJobRequest Job;
	Job.Build = [WeakThis, TriangleVertices = MoveTemp(TriangleVertices), TriangleVertexCount, NumTriangles,
		OccupancyWordCount, ColumnCount, NumScanBlocks, LocalGridSize, BoxExtentXY, CellSizeXY,
		MinWorldZ, CellSizeZ, WorldToBox, BoxToWorld, MaxTriangleWorldZCaptured, RunCapacity]
		(FRDGBuilder& GraphBuilder, const FSceneView& View, const FGlobalDistanceFieldParameterData& GDFData) mutable
	{
		FRDGBufferRef DenseOccupancy = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(OccupancyWordCount, 1u)), TEXT("CSSW.VoxelDenseScratch"));
		FRDGBufferRef CoverageBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ColumnCount), TEXT("CSSW.VoxelCoverage"));
		FRDGBufferRef RunCountBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ColumnCount), TEXT("CSSW.VoxelColumnRunCount"));
		FRDGBufferRef RunStartBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ColumnCount), TEXT("CSSW.VoxelColumnRunStart"));
		FRDGBufferRef BlockSums = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(NumScanBlocks, 1u)), TEXT("CSSW.VoxelBlockSums"));
		FRDGBufferRef TotalCount = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSSW.VoxelTotalCount"));
		FRDGBufferRef RunsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), RunCapacity), TEXT("CSSW.VoxelRuns"));

		FRDGBufferUAVRef DenseUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DenseOccupancy, PF_R32_UINT));
		FRDGBufferUAVRef CoverageUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CoverageBuffer, PF_R32_UINT));
		FRDGBufferUAVRef RunCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RunCountBuffer, PF_R32_UINT));
		FRDGBufferUAVRef RunStartUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RunStartBuffer, PF_R32_UINT));
		FRDGBufferUAVRef BlockSumsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BlockSums, PF_R32_UINT));
		FRDGBufferUAVRef TotalCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TotalCount, PF_R32_UINT));
		FRDGBufferUAVRef RunsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RunsBuffer, PF_R32_UINT));
		AddClearUAVPass(GraphBuilder, DenseUAV, 0u);
		AddClearUAVPass(GraphBuilder, CoverageUAV, 0u);
		AddClearUAVPass(GraphBuilder, RunCountUAV, 0u);
		AddClearUAVPass(GraphBuilder, TotalCountUAV, 0u);
		AddClearUAVPass(GraphBuilder, RunsUAV, 0u);

		// Landscape triangle fill (skipped when there is no landscape geometry in the box).
		if (NumTriangles > 0 && TriangleVertexCount > 0)
		{
			FVector4f* UploadData = GraphBuilder.AllocPODArray<FVector4f>(TriangleVertexCount);
			FMemory::Memcpy(UploadData, TriangleVertices.GetData(), TriangleVertexCount * sizeof(FVector4f));
			FRDGBufferRef TriangleBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), TriangleVertexCount), TEXT("CSSW.VoxelTriangles"));
			GraphBuilder.QueueBufferUpload(TriangleBuffer, UploadData, TriangleVertexCount * sizeof(FVector4f));
			FRDGBufferSRVRef TriangleSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleBuffer, PF_A32B32G32R32F));

			FRDGBufferRef CounterBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSSW.VoxelTriangleCounter"));
			uint32* CounterUpload = GraphBuilder.AllocPODArray<uint32>(1);
			*CounterUpload = NumTriangles;
			GraphBuilder.QueueBufferUpload(CounterBuffer, CounterUpload, sizeof(uint32));
			FRDGBufferSRVRef CounterSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CounterBuffer, PF_R32_UINT));

			FShallowWaterVoxelFill::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelFill::FParameters>();
			P->VoxelTriangleData = TriangleSRV;
			P->VoxelTriangleCounter = CounterSRV;
			P->RW_VoxelOccupancy = DenseUAV;
			P->RW_VoxelCoverage = CoverageUAV;
			P->VoxelGridSize = LocalGridSize;
			P->VoxelBoxExtentXY = BoxExtentXY;
			P->VoxelCellSizeXY = CellSizeXY;
			P->VoxelGridMinWorldZ = MinWorldZ;
			P->VoxelInvCellSizeZ = CellSizeZ > 0.0f ? 1.0f / CellSizeZ : 0.0f;
			P->VoxelCellSizeZ = CellSizeZ;
			P->VoxelMaxTriangleWorldZ = MaxTriangleWorldZCaptured;
			P->WorldToBox = WorldToBox;
			P->VoxelTriangleCount = NumTriangles;
			P->bUseVoxelTriangleCounter = 0u;
			TShaderMapRef<FShallowWaterVoxelFill> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector Groups = FComputeShaderUtils::GetGroupCount(FIntVector((int32)NumTriangles, 1, 1), FIntVector(CSSW_VOXEL_FILL_THREADS, 1, 1));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.VoxelFill"), ERDGPassFlags::Compute, Shader, P, Groups);
		}

		// GDF fill: OR-merge the Global Distance Field's solid occupancy into the SAME dense grid.
		{
			FShallowWaterVoxelGDFFill::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelGDFFill::FParameters>();
			P->RW_VoxelOccupancy = DenseUAV;
			P->RW_VoxelCoverage = CoverageUAV;
			P->VoxelGridSize = LocalGridSize;
			P->VoxelBoxExtentXY = BoxExtentXY;
			P->VoxelCellSizeXY = CellSizeXY;
			P->VoxelGridMinWorldZ = MinWorldZ;
			P->VoxelCellSizeZ = CellSizeZ;
			P->VoxelMaxTriangleWorldZ = MaxTriangleWorldZCaptured;
			P->VoxelBoxToWorld = BoxToWorld;
			P->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
			P->View = View.ViewUniformBuffer;
			P->GlobalDistanceFieldParameters = SetupGlobalDistanceFieldParameters_Minimal(GDFData);
			P->GlobalDistanceFieldParameters.GlobalDistanceFieldCoverageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
			P->GlobalDistanceFieldParameters.GlobalDistanceFieldPageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
			P->GlobalDistanceFieldParameters.GlobalDistanceFieldMipTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			TShaderMapRef<FShallowWaterVoxelGDFFill> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector Groups = FComputeShaderUtils::GetGroupCount(LocalGridSize, FIntVector(8, 8, 1));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.VoxelGDFFill"), Shader, P, Groups);
		}

		FRDGBufferSRVRef DenseSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DenseOccupancy, PF_R32_UINT));

		// Count pass: per-column number of vertical solid runs.
		{
			FShallowWaterVoxelCountRuns::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelCountRuns::FParameters>();
			P->B_VoxelOccupancy = DenseSRV;
			P->RW_ColumnRunCount = RunCountUAV;
			P->VoxelGridSize = LocalGridSize;
			TShaderMapRef<FShallowWaterVoxelCountRuns> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector Groups = FComputeShaderUtils::GetGroupCount(
				FIntVector(LocalGridSize.X, LocalGridSize.Y, 1),
				FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.CountRuns"), ERDGPassFlags::Compute, Shader, P, Groups);
		}

		// Scan 1: per-block exclusive prefix sum -> RunStart, block totals -> BlockSums.
		{
			FShallowWaterVoxelScanBlocks::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelScanBlocks::FParameters>();
			P->B_ColumnRunCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RunCountBuffer, PF_R32_UINT));
			P->RW_ColumnRunStart = RunStartUAV;
			P->RW_BlockSums = BlockSumsUAV;
			P->ColumnCount = ColumnCount;
			TShaderMapRef<FShallowWaterVoxelScanBlocks> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.ScanBlocks"), ERDGPassFlags::Compute, Shader, P, FIntVector(NumScanBlocks, 1, 1));
		}

		// Scan 2: exclusive scan of block sums + grand total.
		{
			FShallowWaterVoxelScanBlockSums::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelScanBlockSums::FParameters>();
			P->RW_BlockSums = BlockSumsUAV;
			P->RW_TotalCount = TotalCountUAV;
			P->NumBlocks = NumScanBlocks;
			TShaderMapRef<FShallowWaterVoxelScanBlockSums> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.ScanBlockSums"), ERDGPassFlags::Compute, Shader, P, FIntVector(1, 1, 1));
		}

		// Scan 3: add scanned block offsets back to per-column starts.
		{
			FShallowWaterVoxelAddOffsets::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelAddOffsets::FParameters>();
			P->B_BlockSums = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(BlockSums, PF_R32_UINT));
			P->RW_ColumnRunStart = RunStartUAV;
			P->ColumnCount = ColumnCount;
			TShaderMapRef<FShallowWaterVoxelAddOffsets> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.AddOffsets"), ERDGPassFlags::Compute, Shader, P, FIntVector(NumScanBlocks, 1, 1));
		}

		// Emit pass: write packed runs into the preallocated CSR Runs buffer (no readback needed).
		{
			FShallowWaterVoxelEmitRuns::FParameters* P = GraphBuilder.AllocParameters<FShallowWaterVoxelEmitRuns::FParameters>();
			P->B_VoxelOccupancy = DenseSRV;
			P->B_ColumnRunStart = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RunStartBuffer, PF_R32_UINT));
			P->RW_Runs = RunsUAV;
			P->VoxelGridSize = LocalGridSize;
			P->TotalRunCapacity = RunCapacity;
			TShaderMapRef<FShallowWaterVoxelEmitRuns> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector Groups = FComputeShaderUtils::GetGroupCount(
				FIntVector(LocalGridSize.X, LocalGridSize.Y, 1),
				FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.EmitRuns"), ERDGPassFlags::Compute, Shader, P, Groups);
		}

		// Convert the CSR buffers to persistent external buffers; the engine executes this graph, and
		// the solver reads them on a later frame, so GPU ordering guarantees the data is ready by then.
		TRefCountPtr<FRDGPooledBuffer> CoveragePooled = GraphBuilder.ConvertToExternalBuffer(CoverageBuffer);
		TRefCountPtr<FRDGPooledBuffer> RunCountPooled = GraphBuilder.ConvertToExternalBuffer(RunCountBuffer);
		TRefCountPtr<FRDGPooledBuffer> RunStartPooled = GraphBuilder.ConvertToExternalBuffer(RunStartBuffer);
		TRefCountPtr<FRDGPooledBuffer> RunsPooled = GraphBuilder.ConvertToExternalBuffer(RunsBuffer);

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, CoveragePooled = MoveTemp(CoveragePooled), RunCountPooled = MoveTemp(RunCountPooled),
			 RunStartPooled = MoveTemp(RunStartPooled), RunsPooled = MoveTemp(RunsPooled)]() mutable
			{
				ACSShallowWaterCapture* Self = WeakThis.Get();
				if (!Self) return;
				Self->VoxelColumnRunStartBuffer = MoveTemp(RunStartPooled);
				Self->VoxelColumnRunCountBuffer = MoveTemp(RunCountPooled);
				Self->VoxelRunsBuffer = MoveTemp(RunsPooled);
				Self->VoxelCoverageBuffer = MoveTemp(CoveragePooled);
				Self->bVoxelGridValid = Self->VoxelColumnRunStartBuffer.IsValid()
					&& Self->VoxelColumnRunCountBuffer.IsValid()
					&& Self->VoxelRunsBuffer.IsValid() && Self->VoxelCoverageBuffer.IsValid();
				Self->bVoxelHeightMapInitialized = false;
				UE_LOG(LogTemp, Log, TEXT("[CSSW] BuildTerrainVoxelGrid: %s terrain voxel grid landed (Grid=%dx%dx%d)."),
					*Self->GetName(), Self->VoxelGridSize.X, Self->VoxelGridSize.Y, Self->VoxelGridSize.Z);
			});
	};

	FGDFSampleService::Get().EnqueueGDFJob(MoveTemp(Job));
	UE_LOG(LogTemp, Log, TEXT("[CSSW] BuildTerrainVoxelGrid: %s enqueued GDF terrain job (Grid=%dx%dx%d Tris=%d ZRange=[%.1f,%.1f] CellZ=%.2f)."),
		*GetName(), GridXY, GridXY, GridZ, TriangleCount, MinWorldZ, MaxWorldZ, CellSizeZ);
}

void ACSShallowWaterCapture::StartSolver(float TimerRate, int32 InIteration)
{
	StopSimulationRuntime(false);
	ClearSolverTimer();
	SolverTimerRate = FMath::Max(TimerRate, 0.0f);
	SolverIterationsPerFrame = ClampCSSWIterationsPerFrame(InIteration, this);
	if (!CanRunShallowWaterGPUWork(TEXT("StartSolver")))
	{
		StopSimulationRuntime(true);
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] StartSolver skipped for %s. ConstructionScript or invalid runtime state is active."),
			*GetNameSafe(this));
		return;
	}

	ResetSolverReadbackState(true, true);
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	if (RT_VoxelTerrain)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_VoxelTerrain, FLinearColor(MaxHeight + 9000, 0, 0, 1));
	}
	BuildTerrainVoxelGrid();
	if (bUseBakedResultMesh)
	{
		UseSimulationResultMesh();
	}
	else
	{
		UpdateSimulationPreviewMesh();
	}
	bSimVisActive = true;
	EnsureSimVisHISMReady();
	// Ensure the static instance grid exists (it is normally built at construction).
	if (!SimVisHISM || SimVisHISM->GetInstanceCount() == 0) BuildSimVisInstanceGrid();
	if (SimVisHISM) SimVisHISM->SetVisibility(true);
	if (ReusltMesh) ReusltMesh->SetVisibility(false);
	ScheduleSolverTimerTick();
	OnSolverStarted();
	UE_LOG(LogTemp, Log, TEXT("[CSSW] StartSolver: %s Iteration=%d CaptureSize=%.2f TextureSize=%.0f TimerRate=%.4f"),
		*GetName(), SolverIterationsPerFrame, CaptureSize, TextureSize, SolverTimerRate);
}

void ACSShallowWaterCapture::StopSolver()
{
	StopSimulationRuntime(false);
}

void ACSShallowWaterCapture::ToggleSimVisualization(int32 SimIterationsPerFrame)
{
	SolverIterationsPerFrame = ClampCSSWIterationsPerFrame(SimIterationsPerFrame, this);
	if (!CanRunShallowWaterGPUWork(TEXT("ToggleSimVisualization")))
	{
		StopSimulationRuntime(true);
		return;
	}

	if (!IsSolverTimerActive())
	{
		StartSolver(SolverTimerRate, SolverIterationsPerFrame);
		return;
	}

	if (bUseBakedResultMesh)
	{
		UseSimulationResultMesh();
	}

	bSimVisActive = !bSimVisActive;
	
	if (bSimVisActive)
	{
		UpdateSimulationPreviewMesh();
		if (!EnsureSimVisHISMReady()) return;

		// Grid is static; just ensure it exists (normally built at construction).
		if (SimVisHISM->GetInstanceCount() == 0) BuildSimVisInstanceGrid();

		if (ReusltMesh) ReusltMesh->SetVisibility(false);
		SimVisHISM->SetVisibility(true);
	}
	else
	{
		if (SimVisHISM) SimVisHISM->SetVisibility(false);
		if (ReusltMesh) ReusltMesh->SetVisibility(true);
	}
}

void ACSShallowWaterCapture::OnSolverStarted_Implementation()
{
	UE_LOG(LogTemp, Log, TEXT("OnSolverStarted_Implementation: Default C++ implementation called on %s"), *GetName());
}

ACSShallowWaterCapture::FOnBakeResultMesh ACSShallowWaterCapture::OnBakeResultMeshDelegate;

void ACSShallowWaterCapture::BakeResultMesh()
{
	if (OnBakeResultMeshDelegate.IsBound())
	{
		OnBakeResultMeshDelegate.Execute(this);
		OnBakeComplete();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeResultMesh: No handler bound (Editor module not loaded?) on %s"), *GetName());
	}
}

void ACSShallowWaterCapture::OnBakeComplete_Implementation()
{
	StopSolver();
	bSimVisActive = false;
	if (SimVisHISM) SimVisHISM->SetVisibility(false);
	if (ReusltMesh) ReusltMesh->SetVisibility(true);
}

void ACSShallowWaterCapture::UseBakedResultMesh(UStaticMesh* InBakedMesh, UMaterialInterface* InWaterMaterial, UMaterialInterface* InDecalMaterial)
{
	if (!InBakedMesh || !ReusltMesh) return;

	Modify();
	ReusltMesh->Modify();

	if (!SimulationPreviewMesh)
	{
		SimulationPreviewMesh = ReusltMesh->GetStaticMesh();
	}
	if (!SimulationWaterMaterial)
	{
		SimulationWaterMaterial = WaterMaterial;
	}
	if (!SimulationDecalMaterial)
	{
		SimulationDecalMaterial = DecalMaterial;
	}

	BakedResultMesh = InBakedMesh;
	bUseBakedResultMesh = true;
	WaterMaterial = InWaterMaterial ? InWaterMaterial : WaterMaterial;
	DecalMaterial = InDecalMaterial ? InDecalMaterial : DecalMaterial;

	StopSolver();
	bSimVisActive = false;
	if (SimVisHISM)
	{
		if (!IsShallowWaterConstructionBlocked())
		{
			ResetSimVisTiles();
		}
		SimVisHISM->SetVisibility(false);
	}

	ReusltMesh->SetStaticMesh(BakedResultMesh);
	ReusltMesh->SetRelativeScale3D(FVector::OneVector);
	if (WaterMaterial)
	{
		ReusltMesh->SetMaterial(0, WaterMaterial);
	}
	ReusltMesh->SetVisibility(true);
	ReusltMesh->MarkRenderStateDirty();
	if (CausticsDecal && DecalMaterial)
	{
		CausticsDecal->Modify();
		CausticsDecal->SetDecalMaterial(DecalMaterial);
		CausticsDecal->SetVisibility(true);
		CausticsDecal->MarkRenderStateDirty();
	}
	MarkPackageDirty();
}

void ACSShallowWaterCapture::UseSimulationResultMesh()
{
	if (!ReusltMesh) return;

	Modify();
	ReusltMesh->Modify();

	bUseBakedResultMesh = false;
	bSimVisActive = false;

	UpdateSimulationPreviewMesh();
	SetMaterialParameter();
	MarkPackageDirty();
}

void ACSShallowWaterCapture::ShowDebugViewPlane(float Duration)
{
	UWorld* World = GetWorld();
	if (!World) return;

	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClassWithTag(World, AStaticMeshActor::StaticClass(), FName("CSSWVM"), Found);
	for (AActor* A : Found)
	{
		if (A) A->Destroy();
	}
	DebugViewPlaneActor.Reset();

	if (!DebugMesh) return;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Spawned = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		ReusltMesh->GetComponentTransform(),
		SpawnParams);
	if (!Spawned) return;

	UStaticMeshComponent* SMC = Spawned->GetStaticMeshComponent();
	SMC->SetStaticMesh(DebugMesh);

	UMaterialInstanceDynamic* MID = SMC->CreateDynamicMaterialInstance(0, DebugMesh->GetMaterial(0));
	if (MID && RT_DebugView)
	{
		MID->SetTextureParameterValue(FName("Height"), RT_DebugView);
	}

	Spawned->Tags.Add(FName("CSSWVM"));
	Spawned->SetFlags(RF_Transient);
	SMC->SetFlags(RF_Transient);
	DebugViewPlaneActor = Spawned;

	if (Duration > 0.f)
	{
		World->GetTimerManager().ClearTimer(DebugViewPlaneTimerHandle);
		World->GetTimerManager().SetTimer(DebugViewPlaneTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (DebugViewPlaneActor.IsValid())
				{
					DebugViewPlaneActor->Destroy();
					DebugViewPlaneActor.Reset();
				}
			}),
			Duration, false);
	}
}

