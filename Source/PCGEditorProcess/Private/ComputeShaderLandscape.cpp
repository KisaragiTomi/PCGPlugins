#include "ComputeShaderLandscape.h"
#include "CSLandscapeEditLayer.h"
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
#include "ComputeShaderMeshGenerator.h"
#include "ComputeShaderShallowWater.h"
#include "LandscapeExtra.h"
#include "Kismet/GameplayStatics.h"
#include "GeometryGeneral.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditLayerRenderer.h"
#include "LandscapeEditLayerRendererState.h"
#include "LandscapeEditLayerTargetTypeState.h"
#include "LandscapeEditLayerMergeRenderContext.h"
#include "LandscapeEditResourcesSubsystem.h"
#include "LandscapeUtils.h"
#include "Generators/RectangleMeshGenerator.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "EngineUtils.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "UObject/SavePackage.h"

class ACSShallowWaterCapture;
DECLARE_CYCLE_STAT(TEXT("CSL Execute"), STAT_CSL_Execute, STATGROUP_CSTest)


class FCSLandscape : public FGlobalShader
{
public:

		enum class ELandscapeFunction : uint8
		{
			L_CreateRiverBed,
			L_LandscapeBrushTexture,
			L_BrushApplyResult,
			L_RealtimeBlend,
			L_RealtimeBlendDebug,
		
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
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ExternalHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Result)
		SHADER_PARAMETER(FVector4f, ValidUVRange)
		SHADER_PARAMETER(float, BlurRange)
		SHADER_PARAMETER(float, RiverWidth)
		SHADER_PARAMETER(float, RiverDepth)
		SHADER_PARAMETER(float, LandscapeZScale)
		SHADER_PARAMETER(float, LandscapeZOffset)
		SHADER_PARAMETER(float, TempLayerAlpha)
		SHADER_PARAMETER(float, TempHeightOffset)
		SHADER_PARAMETER(float, TempNoiseFrequency)
		SHADER_PARAMETER(float, TempNoiseAmplitude)
		SHADER_PARAMETER(int32, TempNoiseOctaves)
		SHADER_PARAMETER(float, TempFalloffWidth)
		SHADER_PARAMETER(float, TempLandscapeScaleZ)
		SHADER_PARAMETER(int32, TempBlendMode)
		SHADER_PARAMETER(int32, TempSourceMode)
		SHADER_PARAMETER(FVector3f, TempBoxOrigin)
		SHADER_PARAMETER(FVector3f, TempBoxExtent)
		SHADER_PARAMETER(FVector3f, TempBoxAxisX)
		SHADER_PARAMETER(FVector3f, TempBoxAxisY)
		SHADER_PARAMETER(FVector3f, TempRenderAreaOrigin)
		SHADER_PARAMETER(FVector3f, TempRenderAreaAxisX)
		SHADER_PARAMETER(FVector3f, TempRenderAreaAxisY)

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
			TEXT("L_BRUSHAPPLYRESULT"),
			TEXT("L_TEMPLAYERBLEND"),
			TEXT("L_TEMPLAYERDEBUG"),
			
			

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

IMPLEMENT_GLOBAL_SHADER(FCSLandscape, "/Plugin/PCGPlugins/Shaders/Private/CSLandscape.usf", "CSLandscapeFunction", SF_Compute);

using namespace CSHepler;

ACSLandscape::ACSLandscape()
{
	// Reuse the inherited AComputeShaderMeshGenerator components instead of creating duplicates.
	// SceneComponent/Box/VisMesh are kept as aliases so the existing capture code is unchanged.
	SceneComponent = SceneRoot;
	Box = GeneratorBounds;
	VisMesh = DynamicMeshComponent;

	// Restore the interactive capture-box behaviour (base marks GeneratorBounds as a fixed viz component).
	Box->SetBoxExtent(FVector(50, 50, 50));
	Box->bEditableWhenInherited = true;
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

void ACSLandscape::SetExternalHeightRT(UTextureRenderTarget2D* InRT)
{
	ExternalHeightRT = InRT;
	RequestLandscapeUpdate(true);
}

void ACSLandscape::RefreshLayer()
{
	RequestLandscapeUpdate(true);
}

void ACSLandscape::ApplyHeightmapRTToLandscape(UTextureRenderTarget2D* HeightRT)
{
	if (!HeightRT) return;

	// Route the input height RT through the realtime external-RT blend, which the edit-layer
	// merge already consumes -> the terrain deforms from the RT. (NOTE: subclasses that supply a
	// depth-encoded RT should convert it to a height delta first for geometrically-correct results.)
	ExternalHeightRT = HeightRT;
	SourceMode = ETempLayerSourceMode::ExternalRT;
	bRealtimeUpdate = true;
	RequestLandscapeUpdate(true);
}

void ACSLandscape::EnsureRealtimeRTs(const FIntPoint& InSize)
{
	auto CreateOrReplace = [this, &InSize](UTextureRenderTarget2D*& RT)
	{
		if (RT && RT->SizeX == InSize.X && RT->SizeY == InSize.Y
			&& RT->RenderTargetFormat == ETextureRenderTargetFormat::RTF_RGBA16f) return;

		RT = UKismetRenderingLibrary::CreateRenderTarget2D(
			this, InSize.X, InSize.Y, ETextureRenderTargetFormat::RTF_RGBA16f,
			FLinearColor::Black, true, false);
	};

	CreateOrReplace(RT_RealtimeResult);
	if (bShowDebugView) CreateOrReplace(RT_DebugView);
}

void ACSLandscape::InitRT()
{
	EnsureRTs(256, 256);
}

void ACSLandscape::EnsureRTs(int32 SizeX, int32 SizeY)
{
	auto CreateOrResize = [this](UTextureRenderTarget2D*& RT, int32 W, int32 H, ETextureRenderTargetFormat Fmt, FLinearColor Clear)
	{
		if (!RT)
		{
			RT = UKismetRenderingLibrary::CreateRenderTarget2D(this, W, H, Fmt, Clear, true, false);
		}
		else if (RT->SizeX != W || RT->SizeY != H)
		{
			RT->ResizeTarget(W, H);
		}
	};

	CreateOrResize(RT_LandscapeData, SizeX, SizeY, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black);
	CreateOrResize(RT_DebugView, SizeX, SizeY, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black);
	CreateOrResize(RT_Result, SizeX, SizeY, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0));
}


void ACSLandscape::ReadLandscapeDataToTexture()
{
	ALandscape* Landscape = FindLandscape();
	if (!Landscape || !Box) return;

	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;

	EnsureRTs(RT_LandscapeData ? RT_LandscapeData->SizeX : 256,
	          RT_LandscapeData ? RT_LandscapeData->SizeY : 256);

	if (!AComputeShaderMeshGenerator::RenderLandscapeToNormalHeightRT(
		Landscape, Center, Extent, RT_LandscapeData))
	{
		return;
	}

	Orig_LandscapeData.MapMin = Center - Extent;
	Orig_LandscapeData.MapMax = Center + Extent;
	Orig_LandscapeData.ValidUVRange = FVector2f(1.0f, 1.0f);
	Orig_LandscapeData.TextureSize = FIntVector2(RT_LandscapeData->SizeX, RT_LandscapeData->SizeY);
	Orig_LandscapeData.TextureValidSize = Orig_LandscapeData.TextureSize;
	Orig_LandscapeData.Transform = Landscape->GetTransform();
}



void ACSLandscape::CopyLandscapeData()
{
	// A previous Bake may have removed our Edit Layer; re-create it so Copy has somewhere to publish.
	EnsureEditLayer();

	ReadLandscapeDataToTexture();
	if (!RT_LandscapeData) return;

	LandscapeTexMinUV = FVector::ZeroVector;
	LandscapeTexUVRange = FVector(1.0, 1.0, 0.0);
	MapMax = Orig_LandscapeData.MapMax;
	MapMin = Orig_LandscapeData.MapMin;

	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_LandscapeData = RT_LandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	const FVector4f CopyValidUVRange(Orig_LandscapeData.ValidUVRange, FVector2f::ZeroVector);
	const float CapturedBlurRange = BlurRange;

	ENQUEUE_RENDER_COMMAND(CSLandscapeCopyResult)(
	[R_DebugView, R_LandscapeData, R_Result, CopyValidUVRange, CapturedBlurRange](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			const FIntPoint TextureSize = R_Result->GetSizeXY();
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntVector(TextureSize.X, TextureSize.Y, 1), 32);
			TShaderMapRef<FCSLandscape> CS = FCSLandscape::CreatePermutation(
				FCSLandscape::ELandscapeFunction::L_LandscapeBrushTexture);
			FCSLandscape::FParameters* Params = GraphBuilder.AllocParameters<FCSLandscape::FParameters>();

			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(
				GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_CopyResult"));

			FRDGTextureRef TmpRDG_DebugView = nullptr;
			FRDGTextureUAVRef RDGUAV_DebugView = nullptr;
			ConvertToUVATextureFormat(
				GraphBuilder, TmpRDG_DebugView, RDGUAV_DebugView, TextureSize, PF_FloatRGBA, TEXT("UAV_CopyDebug"));

			FRDGTextureRef RDG_LandscapeData = RegisterExternalTexture(
				GraphBuilder, R_LandscapeData->GetRenderTargetTexture(), TEXT("R_CopyLandscapeSource"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(
				GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("R_CopyResult"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(
				GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("R_CopyDebug"));

			Params->T_OrigLandscapeData = RDG_LandscapeData;
			Params->RW_Result = RDGUAV_Result;
			Params->RW_DebugView = RDGUAV_DebugView;
			Params->ValidUVRange = CopyValidUVRange;
			Params->BlurRange = CapturedBlurRange;
			Params->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CSLandscapeCopyResult"),
				Params,
				ERDGPassFlags::AsyncCompute,
				[Params, CS, GroupCount](FRHIComputeCommandList& CmdList)
				{
					FComputeShaderUtils::Dispatch(CmdList, CS, *Params, GroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();

	bHasResult = true;
	SaveResultToPersistent();
	RequestLandscapeUpdate(true);

	UE_LOG(LogTemp, Log, TEXT("ACSLandscape::CopyLandscapeData: captured %s (%dx%d) and enabled Box-scoped merge"),
		*GetName(), RT_Result->SizeX, RT_Result->SizeY);
}

void ACSLandscape::BakeLandscape()
{
#if WITH_EDITOR
	if (bRealtimeUpdate)
	{
		// Realtime procedural layer: bake the already-packed realtime result.
		if (!RT_RealtimeResult || LastRenderAreaSectionRect.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("ACSLandscape::BakeLandscape: realtime result not ready for %s"), *GetName());
			RequestLandscapeUpdate(true);
			return;
		}
		BakePackedHeightToLandscape(RT_RealtimeResult, LastRenderAreaSectionRect);
		bRealtimeUpdate = false;
	}
	else
	{
		if (!RT_Result || !bHasResult || !Box)
		{
			UE_LOG(LogTemp, Warning, TEXT("ACSLandscape::BakeLandscape: nothing to bake for %s (Copy first)"), *GetName());
			return;
		}
		// Build the region descriptor for the Box's CURRENT footprint (it may have moved since Copy).
		// This populates ReadRange/TextureValidSize -- mirrors ACSLandscapeLayer::CommitToLandscape.
		FCSReadLandscapeData BakeData;
		ULandscapeExtra::CreateLandscapeTextureData(BakeData, Box->Bounds.Origin, Box->Bounds.BoxExtent);

		// RT_Result stores the captured world-Z height in the RED channel (Alpha = falloff mask).
		// Bake ONLY the Box region (bClearLayerFirst=false) so the surrounding terrain is preserved.
		BakeResultToLandscape(RT_Result, BakeData,
			/*bClearLayerFirst*/ false, /*bWriteAlphaBlendAndFlags*/ true, /*bHeightInAlpha*/ false);
	}

	bHasResult = false;
	PersistentResult = nullptr;
	MarkPackageDirty();

	// The effect now lives permanently in the base heightmap -> drop this actor's Edit Layer.
	RemoveEditLayer();
	RequestLandscapeUpdate(true);

	UE_LOG(LogTemp, Log, TEXT("ACSLandscape::BakeLandscape: baked %s into base heightmap and removed edit layer"), *GetName());
#endif
}

void ACSLandscape::SaveResultToPersistent()
{
#if WITH_EDITOR
	if (!RT_Result || RT_Result->SizeX == 0) return;

	UWorld* World = GetWorld();
	if (!World) return;

	// Create or reuse PersistentResult in our own package (serialized with the actor)
	if (!PersistentResult || PersistentResult->GetSizeX() != RT_Result->SizeX || PersistentResult->GetSizeY() != RT_Result->SizeY)
	{
		PersistentResult = NewObject<UTexture2D>(this, TEXT("PersistentResult"), RF_NoFlags);
	}

	UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(World, RT_Result, PersistentResult);
	PersistentResult->UpdateResource();
	MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("ACSLandscape::SaveResultToPersistent: %s (%dx%d)"),
		*GetName(), RT_Result->SizeX, RT_Result->SizeY);
#endif
}

void ACSLandscape::RestoreResultFromPersistent()
{
#if WITH_EDITOR
	if (!PersistentResult) return;

	int32 W = PersistentResult->GetSizeX();
	int32 H = PersistentResult->GetSizeY();
	if (W == 0 || H == 0) return;

	// Ensure RT_Result exists with matching size
	EnsureRTs(W, H);
	if (!RT_Result) return;

	// Copy PersistentResult → RT_Result via GPU
	ENQUEUE_RENDER_COMMAND(RestorePersistent)(
	[RT = RT_Result, Tex = PersistentResult](FRHICommandListImmediate& RHICmdList)
	{
		FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
		FTextureResource* TexResource = Tex->GetResource();
		if (RTResource && TexResource)
		{
			FRHICopyTextureInfo CopyInfo;
			RHICmdList.CopyTexture(
				TexResource->GetTexture2DRHI(),
				RTResource->GetRenderTargetTexture(),
				CopyInfo);
		}
	});
	FlushRenderingCommands();

	bHasResult = true;
	UE_LOG(LogTemp, Log, TEXT("ACSLandscape::RestoreResultFromPersistent: %s (%dx%d)"),
		*GetName(), W, H);
#endif
}

void ACSLandscape::PostLoad()
{
	Super::PostLoad();
	RestoreResultFromPersistent();
}

#if WITH_EDITOR

bool ACSLandscape::RenderLayer(
	UE::Landscape::EditLayers::FRenderParams& RenderParams,
	UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder)
{
	if (!RenderParams.MergeRenderContext->IsHeightmapMerge()) return false;

	const bool bApplyCachedResult = bHasResult && RT_Result;
	const bool bExternalSourceReady = SourceMode != ETempLayerSourceMode::ExternalRT || ExternalHeightRT;
	const bool bApplyRealtime = bRealtimeUpdate && bExternalSourceReady && !FMath::IsNearlyZero(LayerAlpha);
	if (!bApplyCachedResult && !bApplyRealtime) return false;

	RenderParams.MergeRenderContext->CycleBlendRenderTargets(RDGBuilderRecorder);
	ULandscapeScratchRenderTarget* WriteRT = RenderParams.MergeRenderContext->GetBlendRenderTargetWrite();
	ULandscapeScratchRenderTarget* ReadRT = RenderParams.MergeRenderContext->GetBlendRenderTargetRead();

	WriteRT->TransitionTo(ERHIAccess::RTV, RDGBuilderRecorder);
	ReadRT->TransitionTo(ERHIAccess::SRVMask, RDGBuilderRecorder);

	const FIntPoint Size = RenderParams.RenderAreaSectionRect.Size();
	UTextureRenderTarget2D* ReadRT2D = ReadRT->GetRenderTarget2D();
	UTextureRenderTarget2D* WriteRT2D = WriteRT->GetRenderTarget2D();
	EnsureRealtimeRTs(Size);
	EnqueueLayerMerge(ReadRT2D, WriteRT2D, Size, RenderParams.RenderAreaWorldTransform,
		bApplyCachedResult, bApplyRealtime, RDGBuilderRecorder);

	LastRenderAreaSectionRect = RenderParams.RenderAreaSectionRect;
	return true;
}

void ACSLandscape::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName = PropertyChangedEvent.GetMemberPropertyName();

	// Actor moved/rotated/scaled: rebuild the pasted preview at the new bounds without replacing the copied source.
	if (PropertyName == USceneComponent::GetRelativeLocationPropertyName()
		|| PropertyName == USceneComponent::GetRelativeRotationPropertyName()
		|| PropertyName == USceneComponent::GetRelativeScale3DPropertyName())
	{
		BoxMin = Box->Bounds.Origin - Box->Bounds.BoxExtent;
		BoxMax = Box->Bounds.Origin + Box->Bounds.BoxExtent;
	}

	// Moving/reshaping the Box (or tweaking BlurRange) just re-merges: the Edit Layer
	// re-stamps the captured RT_Result at the Box's new footprint.
	RequestLandscapeUpdate(true);
}

void ACSLandscape::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	if (Box)
	{
		BoxMin = Box->Bounds.Origin - Box->Bounds.BoxExtent;
		BoxMax = Box->Bounds.Origin + Box->Bounds.BoxExtent;
	}

	RequestLandscapeUpdate(true);
}
#endif

void ACSLandscape::EnqueueLayerMerge(
	UTextureRenderTarget2D* InCombinedResult,
	UTextureRenderTarget2D* OutResult,
	const FIntPoint& Size,
	const FTransform& RenderAreaWorldTransform,
	bool bApplyCachedResult,
	bool bApplyRealtime,
	UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder)
{
	if (!InCombinedResult || !OutResult || !RT_RealtimeResult || !Box) return;

	float ZScale = 1.0f;
	float ZOffset = 0.0f;
	const ALandscape* Landscape = FindLandscape();
	if (Landscape)
	{
		const FTransform LT = Landscape->GetTransform();
		ZScale = LT.GetScale3D().Z;
		ZOffset = LT.GetLocation().Z;
	}

	FTextureRenderTargetResource* R_Combined = InCombinedResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Output = OutResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Preview = RT_RealtimeResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_CachedResult = bApplyCachedResult && RT_Result
		? RT_Result->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* R_External = bApplyRealtime
		&& SourceMode == ETempLayerSourceMode::ExternalRT && ExternalHeightRT
		? ExternalHeightRT->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* R_Debug = bApplyRealtime && bShowDebugView && RT_DebugView
		? RT_DebugView->GameThread_GetRenderTargetResource() : nullptr;
	if (!R_Combined || !R_Output || !R_Preview || (bApplyCachedResult && !R_CachedResult)) return;

	const FVector3f CapturedBoxOrigin(Box->Bounds.Origin);
	const FVector3f CapturedBoxExtent(Box->GetScaledBoxExtent());
	const FVector3f CapturedBoxAxisX(Box->GetComponentQuat().GetAxisX().GetSafeNormal());
	const FVector3f CapturedBoxAxisY(Box->GetComponentQuat().GetAxisY().GetSafeNormal());
	const FVector3f CapturedRenderOrigin(RenderAreaWorldTransform.GetLocation());
	const FVector3f CapturedRenderAxisX(RenderAreaWorldTransform.TransformVector(FVector::ForwardVector));
	const FVector3f CapturedRenderAxisY(RenderAreaWorldTransform.TransformVector(FVector::RightVector));
	const float CapturedAlpha = FMath::Clamp(LayerAlpha, 0.0f, 1.0f);
	const float CapturedHeightOffset = HeightOffset;
	const float CapturedNoiseFreq = NoiseFrequency;
	const float CapturedNoiseAmp = NoiseAmplitude;
	const int32 CapturedNoiseOct = FMath::Clamp(NoiseOctaves, 1, 16);
	const float CapturedFalloff = FMath::Max(FalloffWidth, 0.0f);
	const float CapturedLandscapeScaleZ = Landscape ? Landscape->GetActorScale3D().Z : 1.0f;
	const int32 CapturedBlendMode = static_cast<int32>(BlendMode);
	const int32 CapturedSourceMode = static_cast<int32>(SourceMode);
	const bool CapturedShowDebug = R_Debug != nullptr;

	auto RDGCommand = [=](FRDGBuilder& GraphBuilder)
	{
		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(Size.X, Size.Y, 1), 32);
		FRDGTextureRef CurrentResult = RegisterExternalTexture(
			GraphBuilder, R_Combined->GetRenderTargetTexture(), TEXT("R_LandscapeMergeInput"));
		FRDGTextureRef DebugResult = nullptr;

		if (bApplyCachedResult)
		{
			TShaderMapRef<FCSLandscape> CS = FCSLandscape::CreatePermutation(FCSLandscape::ELandscapeFunction::L_BrushApplyResult);
			FCSLandscape::FParameters* Params = GraphBuilder.AllocParameters<FCSLandscape::FParameters>();

			FRDGTextureRef CachedBlendResult = nullptr;
			FRDGTextureUAVRef CachedBlendUAV = nullptr;
			ConvertToUVATextureFormat(
				GraphBuilder, CachedBlendResult, CachedBlendUAV, Size, PF_FloatRGBA, TEXT("UAV_CachedLandscapeResult"));
			FRDGTextureRef CachedResult = RegisterExternalTexture(
				GraphBuilder, R_CachedResult->GetRenderTargetTexture(), TEXT("R_CachedLandscapeResult"));

			Params->T_OrigLandscapeData = CurrentResult;
			Params->T_CopyLandscapeData = CachedResult;
			Params->RW_Result = CachedBlendUAV;
			Params->LandscapeZScale = ZScale;
			Params->LandscapeZOffset = ZOffset;
			Params->TempBoxOrigin = CapturedBoxOrigin;
			Params->TempBoxExtent = CapturedBoxExtent;
			Params->TempBoxAxisX = CapturedBoxAxisX;
			Params->TempBoxAxisY = CapturedBoxAxisY;
			Params->TempRenderAreaOrigin = CapturedRenderOrigin;
			Params->TempRenderAreaAxisX = CapturedRenderAxisX;
			Params->TempRenderAreaAxisY = CapturedRenderAxisY;
			Params->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("BrushApplyCSResult"),
				Params,
				ERDGPassFlags::AsyncCompute,
				[Params, CS, GroupCount](FRHIComputeCommandList& CmdList)
				{
					FComputeShaderUtils::Dispatch(CmdList, CS, *Params, GroupCount);
				});
			CurrentResult = CachedBlendResult;
		}

		if (bApplyRealtime)
		{
			const FCSLandscape::ELandscapeFunction Function = CapturedShowDebug
				? FCSLandscape::ELandscapeFunction::L_RealtimeBlendDebug
				: FCSLandscape::ELandscapeFunction::L_RealtimeBlend;
			TShaderMapRef<FCSLandscape> CS = FCSLandscape::CreatePermutation(Function);
			FCSLandscape::FParameters* Params = GraphBuilder.AllocParameters<FCSLandscape::FParameters>();

			FRDGTextureRef RealtimeBlendResult = nullptr;
			FRDGTextureUAVRef RealtimeBlendUAV = nullptr;
			ConvertToUVATextureFormat(
				GraphBuilder, RealtimeBlendResult, RealtimeBlendUAV, Size, PF_FloatRGBA, TEXT("UAV_RealtimeLandscapeResult"));

			Params->T_OrigLandscapeData = CurrentResult;
			Params->T_ExternalHeight = R_External
				? RegisterExternalTexture(GraphBuilder, R_External->GetRenderTargetTexture(), TEXT("R_ExternalHeight"))
				: CurrentResult;
			Params->RW_Result = RealtimeBlendUAV;
			Params->TempLayerAlpha = CapturedAlpha;
			Params->TempHeightOffset = CapturedHeightOffset;
			Params->TempNoiseFrequency = CapturedNoiseFreq;
			Params->TempNoiseAmplitude = CapturedNoiseAmp;
			Params->TempNoiseOctaves = CapturedNoiseOct;
			Params->TempFalloffWidth = CapturedFalloff;
			Params->TempLandscapeScaleZ = CapturedLandscapeScaleZ;
			Params->TempBlendMode = CapturedBlendMode;
			Params->TempSourceMode = CapturedSourceMode;
			Params->TempBoxOrigin = CapturedBoxOrigin;
			Params->TempBoxExtent = CapturedBoxExtent;
			Params->TempBoxAxisX = CapturedBoxAxisX;
			Params->TempBoxAxisY = CapturedBoxAxisY;
			Params->TempRenderAreaOrigin = CapturedRenderOrigin;
			Params->TempRenderAreaAxisX = CapturedRenderAxisX;
			Params->TempRenderAreaAxisY = CapturedRenderAxisY;
			Params->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			if (CapturedShowDebug)
			{
				FRDGTextureUAVRef DebugUAV = nullptr;
				ConvertToUVATextureFormat(
					GraphBuilder, DebugResult, DebugUAV, Size, PF_FloatRGBA, TEXT("UAV_RealtimeLandscapeDebug"));
				Params->RW_DebugView = DebugUAV;
			}
			else Params->RW_DebugView = nullptr;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CSLandscapeRealtimeBlend"),
				Params,
				ERDGPassFlags::AsyncCompute,
				[Params, CS, GroupCount](FRHIComputeCommandList& CmdList)
				{
					FComputeShaderUtils::Dispatch(CmdList, CS, *Params, GroupCount);
				});
			CurrentResult = RealtimeBlendResult;
		}

		FRDGTextureRef RDG_Output = RegisterExternalTexture(
			GraphBuilder, R_Output->GetRenderTargetTexture(), TEXT("R_LandscapeMergeOutput"));
		FRDGDrawTextureInfo DrawInfo;
		DrawInfo.Size = Size;
		AddDrawTexturePass(
			GraphBuilder, GetGlobalShaderMap(GMaxRHIFeatureLevel), CurrentResult, RDG_Output, DrawInfo);

		FRDGTextureRef RDG_Preview = RegisterExternalTexture(
			GraphBuilder, R_Preview->GetRenderTargetTexture(), TEXT("R_LandscapeMergePreview"));
		AddCopyTexturePass(GraphBuilder, CurrentResult, RDG_Preview, FRHICopyTextureInfo());

		if (DebugResult && R_Debug)
		{
			FRDGTextureRef RDG_Debug = RegisterExternalTexture(
				GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_RealtimeLandscapeDebug"));
			AddCopyTexturePass(GraphBuilder, DebugResult, RDG_Debug, FRHICopyTextureInfo());
		}
	};

	using FExternalAccess = UE::Landscape::FRDGBuilderRecorder::FRDGExternalTextureAccessFinal;
	TArray<FExternalAccess> ExternalAccesses =
	{
		{ R_Combined, ERHIAccess::SRVMask },
		{ R_Output, ERHIAccess::RTV },
		{ R_Preview, ERHIAccess::CopyDest }
	};
	if (R_CachedResult) ExternalAccesses.Add({ R_CachedResult, ERHIAccess::SRVMask });
	if (R_External) ExternalAccesses.Add({ R_External, ERHIAccess::SRVMask });
	if (R_Debug) ExternalAccesses.Add({ R_Debug, ERHIAccess::CopyDest });
	RDGBuilderRecorder.EnqueueRDGCommand(MoveTemp(RDGCommand), ExternalAccesses);
}

void ACSLandscape::BP_InitRT()
{
	InitRT();
}

void ACSLandscape::DebugExportRT(UTextureRenderTarget2D* InRT, const FString& AssetName)
{
#if WITH_EDITOR
	if (!InRT)
	{
		UE_LOG(LogTemp, Warning, TEXT("DebugExportRT: InRT is null"));
		return;
	}

	// Build package path: <LevelPackagePath>/DebugRT/<AssetName>
	UWorld* World = GetWorld();
	FString LevelPkgPath;
	if (World && World->GetOutermost())
	{
		LevelPkgPath = FPackageName::GetLongPackagePath(World->GetOutermost()->GetName());
	}
	if (LevelPkgPath.IsEmpty())
	{
		LevelPkgPath = TEXT("/Game");
	}
	const FString PackagePath = LevelPkgPath / TEXT("DebugRT");
	const FString FullAssetPath = PackagePath / AssetName;

	// Create or find the package
	UPackage* Package = CreatePackage(*FullAssetPath);
	Package->FullyLoad();

	// Check if asset already exists — delete it so we can overwrite
	UTexture2D* ExistingTexture = FindObject<UTexture2D>(Package, *AssetName);
	if (ExistingTexture)
	{
		ExistingTexture->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
		ExistingTexture->MarkAsGarbage();
	}

	// Create a new UTexture2D in the package, then fill it from the RT
	UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(World, InRT, NewTexture);

	// Mark dirty and save
	NewTexture->MarkPackageDirty();
	Package->MarkPackageDirty();

	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(FullAssetPath, PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, NewTexture, *PackageFilename, SaveArgs);
	}

	UE_LOG(LogTemp, Log, TEXT("DebugExportRT: Saved %s (%dx%d) → %s"),
		*AssetName, InRT->SizeX, InRT->SizeY, *FullAssetPath);
#endif
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
	if (!RT_SplineRotateDist) RT_SplineRotateDist = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (!RT_SplineGradientHeight) RT_SplineGradientHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
}

void ACSLandscapeRiver::ProjectLineToLandscape()
{
	ALandscape* Landscape = FindLandscape();
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
	CopyLandscapeData();
	if (!RT_LandscapeData) return;

	// Ensure spline RTs exist
	if (!RT_SplineRotateDist) RT_SplineRotateDist = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (!RT_SplineGradientHeight) RT_SplineGradientHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);

	ReadLandscapeDataToTexture();
	TArray<USplineComponent*> SplineComponents;
	GetComponents(USplineComponent::StaticClass(), SplineComponents);
	UComputeShaderBasicFunction::SampleSpline(RT_SplineRotateDist, RT_SplineGradientHeight, RT_DebugView, SplineComponents, Box->Bounds, RT_LandscapeData->SizeX);
	
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
	const float CapturedBlurRange = BlurRange;
	const float CapturedTargetRiverWidth = TargetRiverWidth;
	const float CapturedRiverDepth = RiverDepth;
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_Result, R_SplineRotateDist, R_SplineGradientHeight, R_DebugView, R_LandscapeData,
	 ValidUVRange, CapturedBlurRange, CapturedTargetRiverWidth, CapturedRiverDepth](FRHICommandListImmediate& RHICmdList)
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
			PassParameters->BlurRange = CapturedBlurRange;
			PassParameters->RiverWidth = CapturedTargetRiverWidth;
			PassParameters->RiverDepth = CapturedRiverDepth;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
            	RDG_EVENT_NAME("CreateRiverBed"),
            	PassParameters,
            	ERDGPassFlags::AsyncCompute,
            	[PassParameters, ComputeShader_CreateRiverBed, GroupCount](FRHIComputeCommandList& RHICmdList)
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
	bHasResult = true;
	SaveResultToPersistent();
	RequestLandscapeUpdate(true);
}

void ACSLandscapeRiver::SimRiver(TSubclassOf<AActor> ActorClass, int32 SimIteration, FVector SourcePoint, float Size)
{
	if (RT_SplineRotateDist == nullptr) return;
	ACSShallowWaterCapture* SimActor = nullptr;
	TArray<AActor*> Actors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ActorClass, Actors);
	
	for (TActorIterator<AActor> It(GetWorld(), ActorClass); It; ++It)
	{
		SimActor = Cast<ACSShallowWaterCapture>(*It);
	}
	if (SimActor == nullptr)
	{
		SimActor = GetWorld()->SpawnActor<ACSShallowWaterCapture>(ActorClass);
	}
	// UKismetRenderingLibrary::ClearRenderTarget2D(this, SimActor->RT_Result);
	// UKismetRenderingLibrary::ClearRenderTarget2D(this, SimActor->RT_VelocityHeight);
	SimActor->CaptureSize = (Box->Bounds.BoxExtent * 2).X;
	SimActor->SetActorLocation(Box->Bounds.Origin * FVector(1, 1, 0));
	SimActor->OnConstruction(SimActor->GetTransform());
	// SimActor->ShallowWaterSolverSplineRange(RT_SplineRotateDist, RT_CopyLandscapeData, SourcePoint, Copy_LandscapeData.ValidUVRange, SimIteration, Size);
}
