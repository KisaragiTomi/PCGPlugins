#include "ComputeShaderLandscape.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "ComputeShaderGenerateHepler.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderShallowWater.h"
#include "LandscapeExtra.h"
#include "Kismet/GameplayStatics.h"
#include "GeometryGeneral.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "Generators/RectangleMeshGenerator.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"

class ACSShallowWaterCapture;
DECLARE_CYCLE_STAT(TEXT("CSL Execute"), STAT_CSL_Execute, STATGROUP_CSTest)


class FCSLandscape : public FGlobalShader
{
public:

	enum class ELandscapeFunction : uint8
	{
		L_CreateRiverBed,
		L_LandscapeBrushTexture,
		L_LandscapePaste,
		
		MAX
	};
	class FLandscapeFunction : SHADER_PERMUTATION_ENUM_CLASS("Landscape", ELandscapeFunction);
	using FPermutationDomain = TShaderPermutationDomain<FLandscapeFunction>;

	static TShaderMapRef<FCSLandscape> CreatePermutation(ELandscapeFunction Permutation)
	{
		typename FCSLandscape::FPermutationDomain PermutationVector;
		PermutationVector.Set<FCSLandscape::FLandscapeFunction>(Permutation);
		TShaderMapRef<FCSLandscape> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FCSLandscape);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FCSLandscape, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_OrigLandscapeData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_CopyLandscapeData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SplineRotateDist)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SplineGradientHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_Noise0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Result)
		SHADER_PARAMETER(FVector4f, ValidUVRange)
		SHADER_PARAMETER(float, BlurRange)
		SHADER_PARAMETER(float, RiverWidth)
		SHADER_PARAMETER(float, RiverDepth)

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

			TEXT("L_CREATERIVERBED"),
			TEXT("L_LANDSCAPEBRUSHTEXTURE"),
			TEXT("L_LANDSCAPEPASTE"),
			
			

		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ELandscapeFunction::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FLandscapeFunction>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
		//OutEnvironment.SetDefine(TEXT("FINDPIXELTHREADSIZE"), 256);

		// if (PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_FillVerticalRock)
		// {
		// 	OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_X);
		// 	OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_Y);
		// }

	}
};

IMPLEMENT_GLOBAL_SHADER(FCSLandscape, "/Plugin/PCGPlugins/Shaders/Private/Landscape.usf", "CSLandscapeFunction", SF_Compute);

using namespace CSHepler;

ACSLandscape::ACSLandscape()
{
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50,50,50));
	
	VisMesh = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("VisMesh"));
	VisMesh->SetupAttachment(SceneComponent, TEXT("VisMesh"));
	
	AffectHeightmap = true;

}


void ACSLandscape::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Box->SetRelativeScale3D(FVector(100, 100, 100));
	Box->SetRelativeLocation(FVector(0, 0, 50));
	Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

	BoxMin = Box->Bounds.Origin - Box->Bounds.BoxExtent;
	BoxMax = Box->Bounds.Origin + Box->Bounds.BoxExtent;

	// VisMesh->SetWorldScale3D(FVector::OneVector);
	// VisMesh->SetRelativeLocation(FVector(0, 0, 5));
	// VisMesh->SetHiddenInGame(true);
	// VisMesh->BoundsScale = 10;
	// VisMesh->SetupAttachment(SceneComponent, TEXT("VisMesh"));

	// ReadLandscapeDataToTexture();
}

void ACSLandscape::InitRT()
{
	if (RT_LandscapeData == nullptr) RT_LandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_CopyLandscapeData == nullptr) RT_CopyLandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_Result == nullptr) RT_Result = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false); 
	
}


void ACSLandscape::ReadLandscapeDataToTexture()
{
	if (!IsParameterValidMult()) return;
	
	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	FReadLandscapeData LandscapeData;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);
	if (LandscapeData.TextureSize.X + LandscapeData.TextureSize.Y < 32) return;
	RT_LandscapeData->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);
	
	FVector TextureMin = Center - Extent;
	FVector TextureMax = Center + Extent;
	FVector Range = LandscapeData.MapMax - LandscapeData.MapMin + FVector(0, 0, 1);
	FVector MinUV = (TextureMin - LandscapeData.MapMin) / Range;
	FVector MaxUV = (TextureMax - LandscapeData.MapMin) / Range;
	FVector UVRange = MaxUV - MinUV;
	
	UComputeShaderBasicFunction::DrawLinearColorsToRenderTarget32(RT_LandscapeData, LandscapeData.Colors);
	Orig_LandscapeData = LandscapeData;
}



void ACSLandscape::CopyLandscapeData()
{
	if (RT_LandscapeData == nullptr) RT_LandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_CopyLandscapeData == nullptr) RT_CopyLandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_Result == nullptr) RT_Result = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);
	if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);

	ReadLandscapeDataToTexture();
	RT_CopyLandscapeData->ResizeTarget(RT_LandscapeData->SizeX, RT_LandscapeData->SizeY);
	UComputeShaderBasicFunction::CopyTexture(RT_LandscapeData, RT_CopyLandscapeData);
	Copy_LandscapeData = Orig_LandscapeData;
	RT_Result->ResizeTarget(RT_LandscapeData->SizeX, RT_LandscapeData->SizeY);

	FTextureRenderTargetResource* R_LandscapeData = RT_LandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FVector4f ValidUVRange =  FVector4f(Orig_LandscapeData.ValidUVRange, Copy_LandscapeData.ValidUVRange);
	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	FVector TextureMin = Center - Extent;
	FVector TextureMax = Center + Extent;
	FVector Range = Orig_LandscapeData.MapMax - Orig_LandscapeData.MapMin + FVector(0, 0, 1);
	FVector MinUV = (TextureMin - Orig_LandscapeData.MapMin) / Range;
	FVector MaxUV = (TextureMax - Orig_LandscapeData.MapMin) / Range;
	FVector UVRange = MaxUV - MinUV;
	LandscapeTexMinUV = MinUV;
	LandscapeTexUVRange = UVRange;
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_Result->GetSizeXY().X;
			float SizeY = R_Result->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
						
			TShaderMapRef<FCSLandscape> ComputeShader_BrushTexture = FCSLandscape::CreatePermutation(FCSLandscape::ELandscapeFunction::L_LandscapeBrushTexture);
			
			FCSLandscape::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSLandscape::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
			
			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));

			FRDGTextureRef TmpRDG_DebugView = nullptr;
			FRDGTextureUAVRef RDGUAV_DebugView = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_DebugView, RDGUAV_DebugView, TextureSize, PF_FloatRGBA, TEXT("UAV_DebugView"));
			
			// FRDGTextureRef RDG_CopyLandscapeData = RegisterExternalTexture(GraphBuilder, R_CopyData->GetRenderTargetTexture(), TEXT("R_CopyLandscapeData"));
			FRDGTextureRef RDG_LandscapeData = RegisterExternalTexture(GraphBuilder, R_LandscapeData->GetRenderTargetTexture(), TEXT("R_TmpLandscapeData"));
			// FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("R_DebugView"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("R_Result"));

			
			
			PassParameters->T_OrigLandscapeData = RDG_LandscapeData;
			// PassParameters->T_CopyLandscapeData = RDG_CopyLandscapeData;
			PassParameters->RW_Result = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->ValidUVRange = ValidUVRange;
			PassParameters->BlurRange = BlurRange;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
            	RDG_EVENT_NAME("PasteLandscape"),
            	PassParameters,
            	ERDGPassFlags::AsyncCompute,
            	[&PassParameters, ComputeShader_BrushTexture, GroupCount](FRHIComputeCommandList& RHICmdList)
            	{
            		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_BrushTexture, *PassParameters, GroupCount);
            	});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			// AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());	
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();


	MapMax = Orig_LandscapeData.MapMax;
	MapMin = Orig_LandscapeData.MapMin;
}

void ACSLandscape::PasteLandscapeData()
{
	if (RT_LandscapeData == nullptr || RT_CopyLandscapeData == nullptr ) return;

	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	if (!Landscape) return;

	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);

	ReadLandscapeDataToTexture();

	if (RT_Result == nullptr) RT_Result = UKismetRenderingLibrary::CreateRenderTarget2D(this, RT_LandscapeData->SizeX, RT_LandscapeData->SizeY, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);
	RT_Result->ResizeTarget(RT_LandscapeData->SizeX, RT_LandscapeData->SizeY);
	RT_DebugView->ResizeTarget(RT_LandscapeData->SizeX, RT_LandscapeData->SizeY);

	
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_LandscapeData = RT_LandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_CopyLandscapeData = RT_CopyLandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FVector4f ValidUVRange =  FVector4f(Orig_LandscapeData.ValidUVRange, Copy_LandscapeData.ValidUVRange);
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_Result->GetSizeXY().X;
			float SizeY = R_Result->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
						
			TShaderMapRef<FCSLandscape> ComputeShader_Paste = FCSLandscape::CreatePermutation(FCSLandscape::ELandscapeFunction::L_LandscapePaste);
			
			FCSLandscape::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSLandscape::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
			
			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));

			FRDGTextureRef TmpRDG_DebugView = nullptr;
			FRDGTextureUAVRef RDGUAV_DebugView = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_DebugView, RDGUAV_DebugView, TextureSize, PF_FloatRGBA, TEXT("UAV_DebugView"));
			
			FRDGTextureRef RDG_CopyLandscapeData = RegisterExternalTexture(GraphBuilder, R_CopyLandscapeData->GetRenderTargetTexture(), TEXT("R_CopyLandscapeData"));
			FRDGTextureRef RDG_LandscapeData = RegisterExternalTexture(GraphBuilder, R_LandscapeData->GetRenderTargetTexture(), TEXT("R_TmpLandscapeData"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("R_DebugView"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("R_Result"));

			
			
			PassParameters->T_OrigLandscapeData = RDG_LandscapeData;
			PassParameters->T_CopyLandscapeData = RDG_CopyLandscapeData;
			PassParameters->RW_Result = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->ValidUVRange = ValidUVRange;
			PassParameters->BlurRange = BlurRange;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
            	RDG_EVENT_NAME("PasteLandscape"),
            	PassParameters,
            	ERDGPassFlags::AsyncCompute,
            	[&PassParameters, ComputeShader_Paste, GroupCount](FRHIComputeCommandList& RHICmdList)
            	{
            		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Paste, *PassParameters, GroupCount);
            	});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());	
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();

	int32 XNum = RT_Result->SizeX;
	TArray<FLinearColor> ResultColors;

	UKismetRenderingLibrary::ReadRenderTargetRaw(this, RT_Result, ResultColors, false);
	TArray<uint16> HeightData;
	TArray<uint16> HeightAlphaBlendData;
	TArray<uint8> HeightFlagsData;
	HeightData.Reserve(Orig_LandscapeData.TextureValidSize.X * Orig_LandscapeData.TextureValidSize.Y);
	HeightAlphaBlendData.Reserve(Orig_LandscapeData.TextureValidSize.X * Orig_LandscapeData.TextureValidSize.Y);
	HeightFlagsData.Reserve(Orig_LandscapeData.TextureValidSize.X * Orig_LandscapeData.TextureValidSize.Y);
	for (int32 Y = 0; Y < Orig_LandscapeData.TextureValidSize.Y; Y++)
	{
		for (int32 X = 0; X < Orig_LandscapeData.TextureValidSize.X; X++)
		{
			float ResultHeight = ResultColors[ X + Y * XNum].A / Landscape->GetActorScale3D().Z;
			HeightAlphaBlendData.Add(0);
			HeightFlagsData.Add(0);
			HeightData.Add(LandscapeDataAccess::GetTexHeight(ResultHeight));
		}
	}

	const FLandscapeLayer* Layer = Landscape->GetLayerConst(0);
	FGuid LayerGuid = Layer ? Layer->EditLayer->GetGuid() : FGuid();
	Landscape->ClearEditLayer(0, nullptr, ELandscapeToolTargetTypeFlags::Heightmap);
	FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid, [=] { Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); });
	LandscapeEdit.SetHeightData(Orig_LandscapeData.ReadRange.X, Orig_LandscapeData.ReadRange.Y,Orig_LandscapeData.ReadRange.Z , Orig_LandscapeData.ReadRange.W, (uint16*)HeightData.GetData(), 0, true, nullptr, (uint16*)HeightAlphaBlendData.GetData(), (uint8*)HeightFlagsData.GetData());
	// LandscapeEdit.SetAlphaData(LayerInfo, MinX, MinY, MaxX, MaxY, Data.GetData(), 0, ELandscapeLayerPaintingRestriction::None, !LayerInfo->bNoWeightBlend, false);

}

void ACSLandscape::BP_InitRT()
{
	InitRT();
}

ACSLandscapeRiver::ACSLandscapeRiver()
{
	SplineComponent = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	SplineComponent->SetupAttachment(SceneComponent, TEXT("Spline"));
}

void ACSLandscapeRiver::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	SplineComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	BoxMatchSpline();

	// RT_LandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	// ReadLandscapeDataToTexture();
}

inline void ACSLandscapeRiver::InitRT()
{
	Super::InitRT();
	if (RT_SplineRotateDist == nullptr ) RT_SplineRotateDist = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_SplineGradientHeight == nullptr ) RT_SplineGradientHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
}

void ACSLandscapeRiver::ProjectLineToLandscape()
{
	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	if (!Landscape) return;
	int32 NumPoints = SplineComponent->GetNumberOfSplinePoints();
	for (int32 i = 0; i < NumPoints; i++)
	{
		FVector SourceLocation = SplineComponent->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		
		FVector OutLocation = FVector::ZeroVector;
		FVector OutNormal = FVector::ZeroVector;
		if (ULandscapeExtra::ProjectPoint(SourceLocation, OutLocation, OutNormal))
		{
			SplineComponent->SetWorldLocationAtSplinePoint(i, OutLocation);
		}
		
	}

	SplineComponent->UpdateBounds();

	RecenterSpline();
	BoxMatchSpline();
}

void ACSLandscapeRiver::BoxMatchSpline()
{
	SplineComponent->UpdateBounds();
	FBoxSphereBounds SplineBounds = SplineComponent->Bounds;
	FVector SplineMax = SplineBounds.Origin + SplineBounds.BoxExtent;
	FVector SplineMin = SplineBounds.Origin - SplineBounds.BoxExtent;
	float BoxScale = (SplineMax - SplineMin).X + TargetRiverWidth * 4;

	Box->SetRelativeScale3D(FVector::OneVector * BoxScale / 100);
	Box->SetWorldLocation(SplineBounds.Origin);
	Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
}



void ACSLandscapeRiver::RecenterSpline()
{
	int32 NumPoints = SplineComponent->GetNumberOfSplinePoints();
	if (NumPoints == 0)	return;
	FVector SplineWorld = SplineComponent->GetComponentLocation();
	FVector FirstPoint = SplineComponent->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
	FVector Change = FirstPoint - SplineWorld;
	SetActorLocation(FirstPoint);

	// SplineComponent->SetWorldLocation(SplineWorld);
	for (int32 i = 0; i < NumPoints; i++)
	{
		FVector PointLocation = SplineComponent->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		SplineComponent->SetWorldLocationAtSplinePoint(i, PointLocation - Change);
	}
	BoxMatchSpline();
	// PostEditChange();
	// PostEditMove(true);

}

void ACSLandscapeRiver::GenerateRiverBed()
{
	if (RT_LandscapeData == nullptr ) RT_LandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false); 
	if (RT_SplineRotateDist == nullptr) RT_SplineRotateDist = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false); 
	if (RT_SplineGradientHeight == nullptr) RT_SplineGradientHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_Result == nullptr) RT_Result = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);

	CopyLandscapeData();
	ReadLandscapeDataToTexture();
	TArray<USplineComponent*> SplineComponents;
	GetComponents(USplineComponent::StaticClass(), SplineComponents);
	UComputeShaderBasicFunction::SampleSpline(RT_SplineRotateDist, RT_SplineGradientHeight, RT_DebugView, SplineComponents, Box->Bounds, RT_LandscapeData->SizeX);
	RT_Result->ResizeTarget(RT_LandscapeData->SizeX, RT_LandscapeData->SizeY);
	
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SplineRotateDist = RT_SplineRotateDist->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SplineGradientHeight = RT_SplineGradientHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_LandscapeData = RT_LandscapeData->GameThread_GetRenderTargetResource();

	
	FVector4f ValidUVRange =  FVector4f(Orig_LandscapeData.ValidUVRange, FVector2f(0, 0));
	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	FVector TextureMin = Center - Extent;
	FVector TextureMax = Center + Extent;
	FVector Range = Orig_LandscapeData.MapMax - Orig_LandscapeData.MapMin + FVector(0, 0, 1);
	FVector MinUV = (TextureMin - Orig_LandscapeData.MapMin) / Range;
	FVector MaxUV = (TextureMax - Orig_LandscapeData.MapMin) / Range;
	FVector UVRange = MaxUV - MinUV;
	LandscapeTexMinUV = MinUV;
	LandscapeTexUVRange = UVRange;
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_Result->GetSizeXY().X;
			float SizeY = R_Result->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
						
			TShaderMapRef<FCSLandscape> ComputeShader_CreateRiverBed = FCSLandscape::CreatePermutation(FCSLandscape::ELandscapeFunction::L_CreateRiverBed);
			
			FCSLandscape::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSLandscape::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
			
			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));

			FRDGTextureRef TmpRDG_DebugView = nullptr;
			FRDGTextureUAVRef RDGUAV_DebugView = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_DebugView, RDGUAV_DebugView, TextureSize, PF_FloatRGBA, TEXT("UAV_DebugView"));
			
			FRDGTextureRef RDG_SplineRotateDist = RegisterExternalTexture(GraphBuilder, R_SplineRotateDist->GetRenderTargetTexture(), TEXT("R_SplineRotateDist"));
			FRDGTextureRef RDG_SplingGradientHeight = RegisterExternalTexture(GraphBuilder, R_SplineGradientHeight->GetRenderTargetTexture(), TEXT("R_SplineGradientHeight"));
			FRDGTextureRef RDG_LandscapeData = RegisterExternalTexture(GraphBuilder, R_LandscapeData->GetRenderTargetTexture(), TEXT("R_TmpLandscapeData"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("R_DebugView"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("R_Result"));

			
			
			PassParameters->T_OrigLandscapeData = RDG_LandscapeData;
			PassParameters->T_SplineRotateDist = RDG_SplineRotateDist;
			PassParameters->T_SplineGradientHeight = RDG_SplingGradientHeight;
			PassParameters->RW_Result = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->ValidUVRange = ValidUVRange;
			PassParameters->BlurRange = BlurRange;
			PassParameters->RiverWidth = TargetRiverWidth;
			PassParameters->RiverDepth = RiverDepth;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
            	RDG_EVENT_NAME("CreateRiverBed"),
            	PassParameters,
            	ERDGPassFlags::AsyncCompute,
            	[&PassParameters, ComputeShader_CreateRiverBed, GroupCount](FRHIComputeCommandList& RHICmdList)
            	{
            		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_CreateRiverBed, *PassParameters, GroupCount);
            	});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());	
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
	
	MapMax = Orig_LandscapeData.MapMax;
	MapMin = Orig_LandscapeData.MapMin;
}

void ACSLandscapeRiver::SimRiver(TSubclassOf<AActor> ActorClass, int32 SimIteration, FVector SourcePoint, float Size)
{
	if (RT_SplineRotateDist == nullptr) return;
	ACSShallowWaterCapture* SimActor = nullptr;
	TArray<AActor*> Actors;
	UGameplayStatics::GetAllActorsOfClass(GWorld, ActorClass, Actors);
	
	for (TActorIterator<AActor> It(GWorld, ActorClass); It; ++It)
	{
		SimActor = Cast<ACSShallowWaterCapture>(*It);
	}
	if (SimActor == nullptr)
	{
		SimActor = GWorld->SpawnActor<ACSShallowWaterCapture>(ActorClass);
	}
	// UKismetRenderingLibrary::ClearRenderTarget2D(this, SimActor->RT_Result);
	// UKismetRenderingLibrary::ClearRenderTarget2D(this, SimActor->RT_VelocityHeight);
	SimActor->CaptureSize = (Box->Bounds.BoxExtent * 2).X;
	SimActor->SetActorLocation(Box->Bounds.Origin * FVector(1, 1, 0));
	SimActor->OnConstruction(SimActor->GetTransform());
	// SimActor->ShallowWaterSolverSplineRange(RT_SplineRotateDist, RT_CopyLandscapeData, SourcePoint, Copy_LandscapeData.ValidUVRange, SimIteration, Size);
}