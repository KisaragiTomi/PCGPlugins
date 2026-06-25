#include "ComputeShaderSceneCapture.h"

#include "ClearQuad.h"
#include "ComputeShaderGenerateHepler.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "ComputeShaderGenerateHepler.h"
#include "EngineUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "Landscape.h"
#include "Engine/SceneCapture.h"
#include "Engine/SceneCapture2D.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "Kismet/KismetSystemLibrary.h"

ACSGenerateCaptureScene::ACSGenerateCaptureScene()
: Super()
{
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CaptureRoot"));
	//SceneComponent->SetupAttachment(GetRootComponent(), TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50,50,50));
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	
	CaptureSceneDepth = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureSceneDepth"));
	CaptureSceneDepth->OrthoWidth = CaptureSize;
	CaptureSceneDepth->ProjectionType = ECameraProjectionMode::Orthographic;
	// CaptureSceneDepth->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureSceneDepth->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	CaptureSceneDepth->SetRelativeRotation(FRotator(-90, -90, 0));
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureSceneDepth->bCaptureEveryFrame = false;
	CaptureSceneDepth->bCaptureOnMovement = false;
	CaptureSceneDepth->SetupAttachment(SceneComponent, TEXT("CaptureSceneDepth"));

	CaptureSceneNormal = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureSceneNormal"));
	CaptureSceneNormal->OrthoWidth = CaptureSize;
	CaptureSceneNormal->ProjectionType = ECameraProjectionMode::Orthographic;
	// CaptureSceneNormal->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureSceneNormal->CaptureSource = ESceneCaptureSource::SCS_Normal;
	CaptureSceneNormal->bCaptureEveryFrame = false;
	CaptureSceneNormal->bCaptureOnMovement = false;
	CaptureSceneNormal->SetupAttachment(CaptureSceneDepth, TEXT("CaptureSceneNormal"));

	CaptureObjectDepth = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureObjectDepth"));
	CaptureObjectDepth->OrthoWidth = CaptureSize;
	CaptureObjectDepth->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureObjectDepth->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureObjectDepth->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	CaptureObjectDepth->SetRelativeRotation(FRotator(-90, -90, 0));
	CaptureObjectDepth->SetWorldLocation(FVector(0, 0, MaxHeight));
	CaptureObjectDepth->bCaptureEveryFrame = false;
	CaptureObjectDepth->bCaptureOnMovement = false;
	CaptureObjectDepth->SetupAttachment(SceneComponent, TEXT("CaptureObjectDepth"));
	
	CaptureObjNormal = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureObjNormal"));
	CaptureObjNormal->OrthoWidth = CaptureSize;
	CaptureObjNormal->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureObjNormal->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureObjNormal->CaptureSource = ESceneCaptureSource::SCS_Normal;
	CaptureObjNormal->bCaptureEveryFrame = false;
	CaptureObjNormal->bCaptureOnMovement = false;
	CaptureObjNormal->SetupAttachment(CaptureObjectDepth, TEXT("CaptureObjNormal"));
	
	CaptureObjBaseColor = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureObjBaseColor"));
	CaptureObjBaseColor->OrthoWidth = CaptureSize;
	CaptureObjBaseColor->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureObjBaseColor->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureObjBaseColor->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
	CaptureObjBaseColor->bCaptureEveryFrame = false;
	CaptureObjBaseColor->bCaptureOnMovement = false;
	CaptureObjBaseColor->SetupAttachment(CaptureObjectDepth, TEXT("CaptureObjBaseColor"));
	
}

void ACSGenerateCaptureScene::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	
	Box->SetRelativeScale3D(FVector(CaptureSceneDepth->OrthoWidth / 100, CaptureSceneDepth->OrthoWidth / 100, MaxHeight / 100));
	Box->SetRelativeLocation(FVector(0, 0, Scale3DZ * 50));
	Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureObjectDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureSceneDepth->OrthoWidth = CaptureSize;
	CaptureObjNormal->OrthoWidth = CaptureSize;
	CaptureObjBaseColor->OrthoWidth = CaptureSize;
	CaptureSceneNormal->OrthoWidth = CaptureSize;
	CaptureObjectDepth->OrthoWidth = CaptureSize;
}



void ACSGenerateCaptureScene::CaptureAll()
{
	if (CaptureSceneDepth->TextureTarget != nullptr)	CaptureSceneDepth->CaptureScene();
	// if (CaptureSceneNormal->TextureTarget != nullptr)	CaptureSceneNormal->CaptureScene();
	if (CaptureObjNormal->TextureTarget != nullptr)		CaptureObjNormal->CaptureScene();
	if (CaptureObjBaseColor->TextureTarget != nullptr)	CaptureObjBaseColor->CaptureScene();
	if (CaptureObjectDepth->TextureTarget != nullptr)	CaptureObjectDepth->CaptureScene();
}

void ACSGenerateCaptureScene::CaptureMeshsInBox()
{
	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	
	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes{ UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic) };
	TArray<AActor*> ActorsToIgnore ;
	TArray<AActor*> OverlapOutActors;
	UKismetSystemLibrary::ComponentOverlapActors(Box, Box->GetComponentTransform(), ObjectTypes, AStaticMeshActor::StaticClass(), ActorsToIgnore, OverlapOutActors);
	for (TActorIterator<AActor> It(GWorld, AActor::StaticClass()); It; ++It)
	{
		if (It->Tags.Contains(Tag)) OverlapOutActors.Add(*It);
	}
	CaptureObjectDepth->ShowOnlyActors = OverlapOutActors;
	CaptureObjNormal->ShowOnlyActors = OverlapOutActors;
	CaptureObjBaseColor->ShowOnlyActors = OverlapOutActors;
	CaptureSceneDepth->ShowOnlyActors = {Landscape};
	CaptureSceneNormal->ShowOnlyActors = {Landscape};
	if (CaptureObjNormal->TextureTarget != nullptr)		CaptureObjNormal->CaptureScene();
	// if (CaptureObjBaseColor->TextureTarget != nullptr)	CaptureObjBaseColor->CaptureScene();
	if (CaptureObjectDepth->TextureTarget != nullptr)	CaptureObjectDepth->CaptureScene();
	if (CaptureSceneDepth->TextureTarget != nullptr)	CaptureSceneDepth->CaptureScene();
	// if (CaptureSceneNormal->TextureTarget != nullptr)	CaptureSceneNormal->CaptureScene();

}

void ACSGenerateCaptureScene::CheckTexture()
{
	if (RT_ObjectDepth == nullptr)
	{
		RT_ObjectDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CaptureObjectDepth->TextureTarget = RT_ObjectDepth;
	}
	if (RT_ObjectBaseColor == nullptr)
	{
		RT_ObjectBaseColor = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CaptureObjBaseColor->TextureTarget = RT_ObjectBaseColor;
	}

	if (RT_ObjectNormal == nullptr)
	{
		RT_ObjectNormal = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CaptureObjNormal->TextureTarget = RT_ObjectNormal;
	}
		
	if (RT_SceneDepth == nullptr)
	{
		RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CaptureSceneDepth->TextureTarget = RT_SceneDepth;
	}

	if (RT_SceneNormal == nullptr)
	{
		RT_SceneNormal = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CaptureSceneNormal->TextureTarget = RT_SceneNormal;
	}

	if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_Result == nullptr) RT_Result = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
}

bool ACSGenerateCaptureScene::CreateLandscapeMesh()
{
;
	// FVector Center = Box->Bounds.Origin;
	// FVector Extent = Box->Bounds.BoxExtent;
	// if (PreCenter == Center && PreExtent == Extent && Mesh->GetTriangleCount() > 0) return false;
	//
	// PreCenter = Center;
	// PreExtent = Extent;
	//
	// ULandscapeExtra::CreateProjectPlane(Mesh, PreCenter, PreExtent, 3);
	// FGeometryScriptCalculateNormalsOptions CalculateOptions;
	// UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(Mesh, CalculateOptions);
	return true;
}

void ACSGenerateCaptureScene::Construction()
{
	OnConstruction(GetActorTransform());
}

// void ACSGenerateCaptureScene::ConstructionInternal()
// {
// 	
// }

void ACSGenerateCaptureScene::Generate(int32 NumIteration, float InSpawnSize)
{
}

