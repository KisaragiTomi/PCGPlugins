#include "DrawPrimtive.h"

#include "ComputeShaderBasicFunction.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "RHIResourceUtils.h"
#include "Kismet/KismetRenderingLibrary.h"

struct FCSVertexData
{
public:
	FVector4f Position;
	FVector2f UV;
	// FVector4f Normal;

};

struct FDrawRasterVertexData
{
	FVector4f Position;
	FVector4f Normal;
};

class FVertexDataDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclaration;

	// Destructor
	virtual ~FVertexDataDeclaration() {}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		FVertexDeclarationElementList Elements;
		
		uint16 Stride = sizeof(FCSVertexData);
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FCSVertexData, Position), VET_Float4, 0, Stride));
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FCSVertexData, UV), VET_Float2, 1, Stride));
		// Elements.Add(FVertexElement(0, STRUCT_OFFSET(FCSVertexData, Normal), VET_Float4, 2, Stride));
		VertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclaration.SafeRelease();
	}

	
};

class FDrawRasterVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclaration;

	virtual ~FDrawRasterVertexDeclaration() {}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		FVertexDeclarationElementList Elements;
		const uint16 Stride = sizeof(FDrawRasterVertexData);
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FDrawRasterVertexData, Position), VET_Float4, 0, Stride));
		Elements.Add(FVertexElement(0, STRUCT_OFFSET(FDrawRasterVertexData, Normal), VET_Float4, 1, Stride));
		VertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclaration.SafeRelease();
	}
};

class FSimpleScreenVertexBuffer : public FVertexBuffer
{
public:
	/** Initialize the RHI for this rendering resource */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
};
void FSimpleScreenVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	TResourceArray<FFilterVertex, VERTEXBUFFER_ALIGNMENT> Vertices;
	Vertices.SetNumUninitialized(6);

	Vertices[0].Position = FVector4f(-1, 1, 0, 1);
	Vertices[0].UV = FVector2f(0, 0);

	Vertices[1].Position = FVector4f(1, 1, 0, 1);
	Vertices[1].UV = FVector2f(1, 0);

	Vertices[2].Position = FVector4f(-1, -1, 0, 1);
	Vertices[2].UV = FVector2f(0, 1);

	Vertices[3].Position = FVector4f(1, -1, 0, 1);
	Vertices[3].UV = FVector2f(1, 1);

	FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::CreateVertex(TEXT("ShaderDemoSquare"), Vertices.GetResourceDataSize())
		.DetermineInitialState()
		.SetInitActionResourceArray(&Vertices);
	VertexBufferRHI = RHICmdList.CreateBuffer(CreateDesc);
}



class FDrawInstanceHeightVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstanceHeightVS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstanceHeightVS, FGlobalShader);

public:

	enum class EDrawTypeVS : uint8
	{
		DIVS_Test,
		DIVS_DrawHeight,
		MAX
	};

	class FDrawType : SHADER_PERMUTATION_ENUM_CLASS("DIVS", EDrawTypeVS);
	using FPermutationDomain = TShaderPermutationDomain<FDrawType>;
	static TShaderMapRef<FDrawInstanceHeightVS> CreatePermutation(EDrawTypeVS Permutation)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FDrawType>(Permutation);
		TShaderMapRef<FDrawInstanceHeightVS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InstanceTransform)

		// SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GPUSceneInstancePayloadData)
		// SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GPUScenePrimitiveSceneData)
		SHADER_PARAMETER(FMatrix44f, V2P)
		SHADER_PARAMETER(FMatrix44f, L2WTest)
		
	END_SHADER_PARAMETER_STRUCT()

	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("DIVS_TEST"),
			TEXT("DIVS_DRAWHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EDrawTypeVS::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FDrawType>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

	}
};



class FDrawInstanceHeightPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstanceHeightPS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstanceHeightPS, FGlobalShader);

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform);
	}


	enum class EDrawTypePS : uint8
	{
		DIPS_PreDepth,
		DIPS_DrawHeight,
		MAX
	};

	class FDrawType : SHADER_PERMUTATION_ENUM_CLASS("DIPS", EDrawTypePS);
	using FPermutationDomain = TShaderPermutationDomain<FDrawType>;
	static TShaderMapRef<FDrawInstanceHeightPS> CreatePermutation(EDrawTypePS Permutation)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FDrawType>(Permutation);
		TShaderMapRef<FDrawInstanceHeightPS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Normal)
	END_SHADER_PARAMETER_STRUCT()


	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("DIPS_PREDEPTH"),
			TEXT("DIPS_DRAWHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EDrawTypePS::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FDrawType>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

	}
};

class FDrawInstancesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstancesCS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstancesCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleData)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_OutputTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugTexture)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(float, CaptureWidth)
		SHADER_PARAMETER(float, MinDepth)
		SHADER_PARAMETER(float, DepthRange)
		SHADER_PARAMETER(float, MaxDepth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
	}
};

class FDrawInstancesRasterVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstancesRasterVS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstancesRasterVS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, InvHalfCaptureWidth)
		SHADER_PARAMETER(float, InvHalfCaptureHeight)
		SHADER_PARAMETER(float, RasterMinDepth)
		SHADER_PARAMETER(float, RasterInvDepthRange)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

class FDrawInstancesRasterPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawInstancesRasterPS);
	SHADER_USE_PARAMETER_STRUCT(FDrawInstancesRasterPS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

IMPLEMENT_GLOBAL_SHADER(FDrawInstanceHeightVS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "MainVertexShader", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FDrawInstanceHeightPS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "MainPixelShader", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FDrawInstancesCS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawInstancesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FDrawInstancesRasterVS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawInstancesRasterVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FDrawInstancesRasterPS, "/Plugin/PCGPlugins/Shaders/Private/DrawPrimtive.usf", "DrawInstancesRasterPS", SF_Pixel);


BEGIN_SHADER_PARAMETER_STRUCT(FDrawInstanceHeight, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstanceHeightVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstanceHeightPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FDrawInstancesRasterPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstancesRasterVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FDrawInstancesRasterPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()


TGlobalResource<FVertexDataDeclaration> VertexDataResource;
TGlobalResource<FDrawRasterVertexDeclaration> DrawRasterVertexDataResource;
TGlobalResource<FSimpleScreenVertexBuffer> GSimpleScreenVertexBuffer;
void UCSDrawPrimtive::DrawInstances(UTextureRenderTarget2D* RT_TextureTarget, UTextureRenderTarget2D* RT_Depth, UTextureRenderTarget2D* RT_DebugView, FTransform CameraTransform, float CaptureWidth, TArray<FTransform> InstanceTransforms, UStaticMesh* InStaticMesh)
{
	constexpr float EmptyDepthValue = 100000.0f;

	if (RT_TextureTarget == nullptr || RT_Depth == nullptr || InStaticMesh == nullptr)
	{
		return;
	}
	if (CaptureWidth <= KINDA_SMALL_NUMBER || InstanceTransforms.Num() == 0)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_TextureTarget);
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_Depth, FLinearColor(EmptyDepthValue, EmptyDepthValue, EmptyDepthValue, 1.0f));
		if (RT_DebugView != nullptr)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_DebugView);
		}
		return;
	}

	FStaticMeshRenderData* RenderData = InStaticMesh->GetRenderData();
	if (RenderData == nullptr || RenderData->LODResources.Num() == 0)
	{
		return;
	}

	FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	if (LOD.GetNumTriangles() == 0)
	{
		return;
	}

	if (RT_TextureTarget->SizeX != RT_Depth->SizeX || RT_TextureTarget->SizeY != RT_Depth->SizeY)
	{
		RT_TextureTarget->ResizeTarget(RT_Depth->SizeX, RT_Depth->SizeY);
	}
	if (RT_DebugView != nullptr && (RT_DebugView->SizeX != RT_Depth->SizeX || RT_DebugView->SizeY != RT_Depth->SizeY))
	{
		RT_DebugView->ResizeTarget(RT_Depth->SizeX, RT_Depth->SizeY);
	}

	FTextureRenderTargetResource* R_Output = RT_TextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Depth = RT_Depth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView != nullptr ? RT_DebugView->GameThread_GetRenderTargetResource() : nullptr;

	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FTransform CameraActorTransform(CameraTransform.GetRotation(), CameraTransform.GetLocation(), FVector::OneVector);
	const FTransform CaptureRelativeTransform(FRotator(-90.0f, -90.0f, 0.0f), FVector::ZeroVector, FVector::OneVector);
	const FTransform DirectCaptureTransform = CameraActorTransform;
	const FTransform DerivedCaptureTransform = CaptureRelativeTransform * CameraActorTransform;
	const FBox LocalBounds = InStaticMesh->GetBoundingBox();
	const FVector BoundsMin = LocalBounds.Min;
	const FVector BoundsMax = LocalBounds.Max;
	const FVector BoundsCorners[8] =
	{
		FVector(BoundsMin.X, BoundsMin.Y, BoundsMin.Z),
		FVector(BoundsMin.X, BoundsMin.Y, BoundsMax.Z),
		FVector(BoundsMin.X, BoundsMax.Y, BoundsMin.Z),
		FVector(BoundsMin.X, BoundsMax.Y, BoundsMax.Z),
		FVector(BoundsMax.X, BoundsMin.Y, BoundsMin.Z),
		FVector(BoundsMax.X, BoundsMin.Y, BoundsMax.Z),
		FVector(BoundsMax.X, BoundsMax.Y, BoundsMin.Z),
		FVector(BoundsMax.X, BoundsMax.Y, BoundsMax.Z)
	};
	const float HalfCaptureWidth = CaptureWidth * 0.5f;
	const float HalfCaptureHeight = HalfCaptureWidth * (float(R_Depth->GetSizeXY().Y) / FMath::Max((float)R_Depth->GetSizeXY().X, 1.0f));

	auto ScoreCaptureTransform = [&](const FTransform& CandidateTransform)
	{
		int32 Score = 0;
		for (const FTransform& InstanceTransform : InstanceTransforms)
		{
			for (const FVector& LocalCorner : BoundsCorners)
			{
				const FVector WorldCorner = InstanceTransform.TransformPosition(LocalCorner);
				const FVector CaptureCorner = CandidateTransform.InverseTransformPosition(WorldCorner);
				if (CaptureCorner.X >= 0.0f &&
					FMath::Abs(CaptureCorner.Y) <= HalfCaptureWidth &&
					FMath::Abs(CaptureCorner.Z) <= HalfCaptureHeight)
				{
					++Score;
				}
			}
		}
		return Score;
	};

	const int32 DirectTransformScore = ScoreCaptureTransform(DirectCaptureTransform);
	const int32 DerivedTransformScore = ScoreCaptureTransform(DerivedCaptureTransform);
	const FTransform CaptureTransform = DerivedTransformScore >= DirectTransformScore ? DerivedCaptureTransform : DirectCaptureTransform;

	struct FCaptureTriangleData
	{
		FVector3f P0;
		FVector3f P1;
		FVector3f P2;
		float MinDepth;
	};

	TArray<FCaptureTriangleData> CaptureTriangles;
	CaptureTriangles.Reserve(LOD.GetNumTriangles() * InstanceTransforms.Num());

	float MinVisibleDepth = TNumericLimits<float>::Max();
	float MaxVisibleDepth = 0.0f;
	bool bHasVisibleDepth = false;
	for (const FTransform& InstanceTransform : InstanceTransforms)
	{
		float InstanceMinX = TNumericLimits<float>::Max();
		float InstanceMaxX = -TNumericLimits<float>::Max();
		float InstanceMinY = TNumericLimits<float>::Max();
		float InstanceMaxY = -TNumericLimits<float>::Max();
		float InstanceMinZ = TNumericLimits<float>::Max();
		float InstanceMaxZ = -TNumericLimits<float>::Max();
		for (const FVector& LocalCorner : BoundsCorners)
		{
			const FVector WorldCorner = InstanceTransform.TransformPosition(LocalCorner);
			const FVector CaptureCorner = CaptureTransform.InverseTransformPosition(WorldCorner);
			InstanceMinX = FMath::Min(InstanceMinX, (float)CaptureCorner.X);
			InstanceMaxX = FMath::Max(InstanceMaxX, (float)CaptureCorner.X);
			InstanceMinY = FMath::Min(InstanceMinY, (float)CaptureCorner.Y);
			InstanceMaxY = FMath::Max(InstanceMaxY, (float)CaptureCorner.Y);
			InstanceMinZ = FMath::Min(InstanceMinZ, (float)CaptureCorner.Z);
			InstanceMaxZ = FMath::Max(InstanceMaxZ, (float)CaptureCorner.Z);
		}

		const bool bInstanceCulled =
			InstanceMaxX < 0.0f ||
			InstanceMaxY < -HalfCaptureWidth ||
			InstanceMinY > HalfCaptureWidth ||
			InstanceMaxZ < -HalfCaptureHeight ||
			InstanceMinZ > HalfCaptureHeight;

		if (bInstanceCulled)
		{
			continue;
		}

		for (int32 TriangleIndex = 0; TriangleIndex < LOD.GetNumTriangles(); ++TriangleIndex)
		{
			const uint32 I0 = Indices[TriangleIndex * 3 + 0];
			const uint32 I1 = Indices[TriangleIndex * 3 + 1];
			const uint32 I2 = Indices[TriangleIndex * 3 + 2];

			const FVector WorldP0 = InstanceTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I0)));
			const FVector WorldP1 = InstanceTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I1)));
			const FVector WorldP2 = InstanceTransform.TransformPosition(FVector(PositionBuffer.VertexPosition(I2)));

			const FVector CaptureP0 = CaptureTransform.InverseTransformPosition(WorldP0);
			const FVector CaptureP1 = CaptureTransform.InverseTransformPosition(WorldP1);
			const FVector CaptureP2 = CaptureTransform.InverseTransformPosition(WorldP2);
			FCaptureTriangleData Triangle;
			Triangle.P0 = FVector3f(CaptureP0);
			Triangle.P1 = FVector3f(CaptureP1);
			Triangle.P2 = FVector3f(CaptureP2);
			Triangle.MinDepth = FMath::Min3((float)CaptureP0.X, (float)CaptureP1.X, (float)CaptureP2.X);
			CaptureTriangles.Add(Triangle);

			if (CaptureP0.X >= 0.0f)
			{
				bHasVisibleDepth = true;
				MinVisibleDepth = FMath::Min(MinVisibleDepth, (float)CaptureP0.X);
				MaxVisibleDepth = FMath::Max(MaxVisibleDepth, (float)CaptureP0.X);
			}
			if (CaptureP1.X >= 0.0f)
			{
				bHasVisibleDepth = true;
				MinVisibleDepth = FMath::Min(MinVisibleDepth, (float)CaptureP1.X);
				MaxVisibleDepth = FMath::Max(MaxVisibleDepth, (float)CaptureP1.X);
			}
			if (CaptureP2.X >= 0.0f)
			{
				bHasVisibleDepth = true;
				MinVisibleDepth = FMath::Min(MinVisibleDepth, (float)CaptureP2.X);
				MaxVisibleDepth = FMath::Max(MaxVisibleDepth, (float)CaptureP2.X);
			}
		}
	}

	if (!bHasVisibleDepth)
	{
		MinVisibleDepth = 0.0f;
		MaxVisibleDepth = 1.0f;
	}

	if (CaptureTriangles.Num() == 0)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_TextureTarget);
		UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_Depth, FLinearColor(EmptyDepthValue, EmptyDepthValue, EmptyDepthValue, 1.0f));
		if (RT_DebugView != nullptr)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(GWorld, RT_DebugView);
		}
		return;
	}

	CaptureTriangles.Sort([](const FCaptureTriangleData& A, const FCaptureTriangleData& B)
	{
		return A.MinDepth < B.MinDepth;
	});

	TArray<FDrawRasterVertexData> RasterVertices;
	RasterVertices.Reserve(CaptureTriangles.Num() * 3);
	for (const FCaptureTriangleData& Triangle : CaptureTriangles)
	{
		FVector3f TriangleNormal = FVector3f::CrossProduct(Triangle.P1 - Triangle.P0, Triangle.P2 - Triangle.P0);
		if (!TriangleNormal.Normalize())
		{
			TriangleNormal = FVector3f(1.0f, 0.0f, 0.0f);
		}

		FDrawRasterVertexData V0;
		V0.Position = FVector4f(Triangle.P0, 1.0f);
		V0.Normal = FVector4f(TriangleNormal, 0.0f);
		RasterVertices.Add(V0);

		FDrawRasterVertexData V1;
		V1.Position = FVector4f(Triangle.P1, 1.0f);
		V1.Normal = FVector4f(TriangleNormal, 0.0f);
		RasterVertices.Add(V1);

		FDrawRasterVertexData V2;
		V2.Position = FVector4f(Triangle.P2, 1.0f);
		V2.Normal = FVector4f(TriangleNormal, 0.0f);
		RasterVertices.Add(V2);
	}

	const float DepthRange = FMath::Max(MaxVisibleDepth - MinVisibleDepth, 1.0f);


	ENQUEUE_RENDER_COMMAND(FRPCSRunner)(
		[RasterVertices = MoveTemp(RasterVertices), R_Output, R_Depth, R_Debug, CaptureWidth, MinVisibleDepth, DepthRange](FRHICommandListImmediate& RHICmdList)
		{
			if (RasterVertices.Num() == 0)
			{
				return;
			}

			TResourceArray<FDrawRasterVertexData, VERTEXBUFFER_ALIGNMENT> VertexResourceArray;
			VertexResourceArray.SetNumUninitialized(RasterVertices.Num());
			FMemory::Memcpy(VertexResourceArray.GetData(), RasterVertices.GetData(), RasterVertices.Num() * sizeof(FDrawRasterVertexData));

			FRHIBufferCreateDesc VertexCreateDesc = FRHIBufferCreateDesc::CreateVertex(TEXT("DrawInstanceRasterVB"), VertexResourceArray.GetResourceDataSize())
				.DetermineInitialState()
				.SetInitActionResourceArray(&VertexResourceArray);
			FBufferRHIRef VertexBufferRHI = RHICmdList.CreateBuffer(VertexCreateDesc);

			FRDGBuilder GraphBuilder(RHICmdList);
			{
				const float MaxDepth = EmptyDepthValue;
				const FIntPoint TextureSize = R_Depth->GetSizeXY();
				const float HalfCaptureWidth = CaptureWidth * 0.5f;
				const float HalfCaptureHeight = HalfCaptureWidth * ((float)TextureSize.Y / FMath::Max((float)TextureSize.X, 1.0f));
				const EPixelFormat OutputFormat = R_Output->GetRenderTargetTexture()->GetFormat();
				const EPixelFormat DepthFormat = R_Depth->GetRenderTargetTexture()->GetFormat();
				const EPixelFormat DebugFormat = R_Debug != nullptr ? R_Debug->GetRenderTargetTexture()->GetFormat() : PF_A32B32G32R32F;

				FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(TextureSize, OutputFormat, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
				FRDGTextureDesc DepthPreviewDesc = FRDGTextureDesc::Create2D(TextureSize, DepthFormat, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
				FRDGTextureDesc DebugDesc = FRDGTextureDesc::Create2D(TextureSize, DebugFormat, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
				FRDGTextureRef TmpOutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("DrawInstance_Output"));
				FRDGTextureRef TmpDepthTexture = GraphBuilder.CreateTexture(DepthPreviewDesc, TEXT("DrawInstance_Depth"));
				FRDGTextureRef TmpDebugTexture = GraphBuilder.CreateTexture(DebugDesc, TEXT("DrawInstance_Debug"));
				FRDGTextureDesc RasterDepthDesc = FRDGTextureDesc::Create2D(
					TextureSize,
					PF_DepthStencil,
					FClearValueBinding::DepthFar,
					TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
				FRDGTextureRef RasterDepthTexture = GraphBuilder.CreateTexture(RasterDepthDesc, TEXT("DrawInstance_RasterDepth"));

				FRDGTextureRef OutputTexture = RegisterExternalTexture(GraphBuilder, R_Output->GetRenderTargetTexture(), TEXT("DrawInstance_OutputRT"));
				FRDGTextureRef DepthTexture = RegisterExternalTexture(GraphBuilder, R_Depth->GetRenderTargetTexture(), TEXT("DrawInstance_DepthRT"));
				AddClearRenderTargetPass(GraphBuilder, TmpOutputTexture, FLinearColor::Black);
				AddClearRenderTargetPass(GraphBuilder, TmpDepthTexture, FLinearColor(MaxDepth, MaxDepth, MaxDepth, 1.0f));
				AddClearRenderTargetPass(GraphBuilder, TmpDebugTexture, FLinearColor::Black);

				FDrawInstancesRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FDrawInstancesRasterPassParameters>();
				PassParameters->VS.InvHalfCaptureWidth = 1.0f / FMath::Max(HalfCaptureWidth, KINDA_SMALL_NUMBER);
				PassParameters->VS.InvHalfCaptureHeight = 1.0f / FMath::Max(HalfCaptureHeight, KINDA_SMALL_NUMBER);
				PassParameters->VS.RasterMinDepth = MinVisibleDepth;
				PassParameters->VS.RasterInvDepthRange = 1.0f / FMath::Max(DepthRange, KINDA_SMALL_NUMBER);
				PassParameters->RenderTargets[0] = FRenderTargetBinding(TmpOutputTexture, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets[1] = FRenderTargetBinding(TmpDepthTexture, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets[2] = FRenderTargetBinding(TmpDebugTexture, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
					RasterDepthTexture,
					ERenderTargetLoadAction::EClear,
					ERenderTargetLoadAction::ENoAction,
					FExclusiveDepthStencil::DepthWrite_StencilNop);

				TShaderMapRef<FDrawInstancesRasterVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				TShaderMapRef<FDrawInstancesRasterPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("DrawInstancesRaster"),
					PassParameters,
					ERDGPassFlags::Raster,
					[PassParameters, VertexShader, PixelShader, VertexBufferRHI, TextureSize, PrimitiveCount = RasterVertices.Num() / 3](FRHICommandList& RHICmdList)
					{
						FGraphicsPipelineStateInitializer GraphicsPSOInit;
						RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
						GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
						GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
						GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_LessEqual>::GetRHI();
						GraphicsPSOInit.PrimitiveType = PT_TriangleList;
						GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = DrawRasterVertexDataResource.VertexDeclaration;
						GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
						SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

						RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, (float)TextureSize.X, (float)TextureSize.Y, 1.0f);
						RHICmdList.SetStreamSource(0, VertexBufferRHI, 0);
						RHICmdList.DrawPrimitive(0, PrimitiveCount, 1);
					});

				AddCopyTexturePass(GraphBuilder, TmpOutputTexture, OutputTexture, FRHICopyTextureInfo());
				AddCopyTexturePass(GraphBuilder, TmpDepthTexture, DepthTexture, FRHICopyTextureInfo());
				if (R_Debug != nullptr)
				{
					FRDGTextureRef DebugTexture = RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("DrawInstance_DebugRT"));
					AddCopyTexturePass(GraphBuilder, TmpDebugTexture, DebugTexture, FRHICopyTextureInfo());
				}
			}
			GraphBuilder.Execute();
		});

	FlushRenderingCommands();
}


