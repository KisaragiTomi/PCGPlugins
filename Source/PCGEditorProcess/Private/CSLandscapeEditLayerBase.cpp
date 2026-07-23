#include "CSLandscapeEditLayerBase.h"
#include "CSLandscapeEditLayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditLayerMergeRenderContext.h"
#include "EngineUtils.h"

ACSLandscapeEditLayerBase::ACSLandscapeEditLayerBase()
{
}

ALandscape* ACSLandscapeEditLayerBase::FindLandscape() const
{
	if (!GetWorld()) return nullptr;
	for (TActorIterator<ALandscape> It(GetWorld()); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void ACSLandscapeEditLayerBase::RequestLandscapeUpdate(bool bInUserTriggered)
{
#if WITH_EDITOR
	EnsureEditLayer();

	ALandscape* Landscape = FindLandscape();
	if (Landscape)
	{
		Landscape->RequestLayersContentUpdateForceAll(ELandscapeLayerUpdateMode::Update_All, bInUserTriggered);
	}
#endif
}

void ACSLandscapeEditLayerBase::EnsureEditLayer()
{
#if WITH_EDITOR
	ALandscape* Landscape = FindLandscape();
	if (!Landscape) return;

	if (OwnedEditLayerGuid.IsValid())
	{
		int32 Idx = Landscape->GetLayerIndex(OwnedEditLayerGuid);
		if (Idx != INDEX_NONE) return; // still alive
		OwnedEditLayerGuid.Invalidate();
	}

	FName LayerName = FName(*FString::Printf(TEXT("CS_%s"), *GetName()));
	int32 NewIdx = Landscape->CreateLayer(LayerName, UCSLandscapeEditLayer::StaticClass());
	if (NewIdx != INDEX_NONE)
	{
		const FLandscapeLayer* NewLayer = Landscape->GetLayerConst(NewIdx);
		if (NewLayer && NewLayer->EditLayer)
		{
			OwnedEditLayerGuid = NewLayer->EditLayer->GetGuid();
		}
	}

	UE_LOG(LogTemp, Log, TEXT("%s::EnsureEditLayer: %s -> layer %s"),
		*GetClass()->GetName(), *GetName(), *OwnedEditLayerGuid.ToString());
#endif
}

void ACSLandscapeEditLayerBase::RemoveEditLayer()
{
#if WITH_EDITOR
	if (!OwnedEditLayerGuid.IsValid()) return;

	ALandscape* Landscape = FindLandscape();
	if (!Landscape) return;

	int32 Idx = Landscape->GetLayerIndex(OwnedEditLayerGuid);
	if (Idx != INDEX_NONE)
	{
		Landscape->DeleteLayer(Idx);
		UE_LOG(LogTemp, Log, TEXT("%s::RemoveEditLayer: deleted layer %s"),
			*GetClass()->GetName(), *OwnedEditLayerGuid.ToString());
	}

	OwnedEditLayerGuid.Invalidate();
#endif
}

void ACSLandscapeEditLayerBase::PostLoad()
{
	Super::PostLoad();
}

void ACSLandscapeEditLayerBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Only remove the Edit Layer if the actor is explicitly destroyed (e.g. user deletes it).
	// Do NOT remove on level unload / editor shutdown / PIE end.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		RemoveEditLayer();
	}
	Super::EndPlay(EndPlayReason);
}

void ACSLandscapeEditLayerBase::Destroyed()
{
	RemoveEditLayer();
	Super::Destroyed();
}

void ACSLandscapeEditLayerBase::BakeResultToLandscape(
	UTextureRenderTarget2D* SourceRT,
	const FCSReadLandscapeData& LandscapeData,
	bool bClearLayerFirst,
	bool bWriteAlphaBlendAndFlags)
{
#if WITH_EDITOR
	if (!SourceRT) return;

	ALandscape* Landscape = FindLandscape();
	if (!Landscape) return;

	if (LandscapeData.TextureValidSize.X + LandscapeData.TextureValidSize.Y < 32) return;

	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(true);

	const int32 XNum = SourceRT->SizeX;
	TArray<FLinearColor> ResultColors;
	UKismetRenderingLibrary::ReadRenderTargetRaw(this, SourceRT, ResultColors, false);

	const int32 PixelCount = LandscapeData.TextureValidSize.X * LandscapeData.TextureValidSize.Y;
	TArray<uint16> HeightData;
	TArray<uint16> HeightAlphaBlendData;
	TArray<uint8> HeightFlagsData;
	HeightData.Reserve(PixelCount);
	if (bWriteAlphaBlendAndFlags)
	{
		HeightAlphaBlendData.Reserve(PixelCount);
		HeightFlagsData.Reserve(PixelCount);
	}

	const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
	for (int32 Y = 0; Y < LandscapeData.TextureValidSize.Y; Y++)
	{
		for (int32 X = 0; X < LandscapeData.TextureValidSize.X; X++)
		{
			const float ResultHeight = ResultColors[X + Y * XNum].A / LandscapeScaleZ;
			if (bWriteAlphaBlendAndFlags)
			{
				HeightAlphaBlendData.Add(0);
				HeightFlagsData.Add(0);
			}
			HeightData.Add(LandscapeDataAccess::GetTexHeight(ResultHeight));
		}
	}

	const FLandscapeLayer* Layer = Landscape->GetLayerConst(0);
	const FGuid LayerGuid = Layer ? Layer->EditLayer->GetGuid() : FGuid();

	if (bClearLayerFirst)
	{
		Landscape->ClearEditLayer(0, nullptr, ELandscapeToolTargetTypeFlags::Heightmap);
	}

	FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid,
		[=] { Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); });

	LandscapeEdit.SetHeightData(
		LandscapeData.ReadRange.X, LandscapeData.ReadRange.Y,
		LandscapeData.ReadRange.Z, LandscapeData.ReadRange.W,
		(uint16*)HeightData.GetData(), 0, true, nullptr,
		bWriteAlphaBlendAndFlags ? (uint16*)HeightAlphaBlendData.GetData() : nullptr,
		bWriteAlphaBlendAndFlags ? (uint8*)HeightFlagsData.GetData() : nullptr);
#endif
}

#if WITH_EDITOR

FString ACSLandscapeEditLayerBase::GetEditLayerRendererDebugName() const
{
	return FString::Printf(TEXT("%s_%s"), *GetClass()->GetName(), *GetName());
}

void ACSLandscapeEditLayerBase::GetRendererStateInfo(
	const UE::Landscape::EditLayers::FMergeContext* InMergeContext,
	UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutSupportedTargetTypeState,
	UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutEnabledTargetTypeState,
	TArray<UE::Landscape::EditLayers::FTargetLayerGroup>& OutTargetLayerGroups) const
{
	using namespace UE::Landscape::EditLayers;
	if (ShouldSupportHeightmap())
	{
		OutSupportedTargetTypeState.AddTargetTypeMask(ELandscapeToolTargetTypeFlags::Heightmap);
		if (IsLayerEnabledForMerge())
		{
			OutEnabledTargetTypeState.AddTargetTypeMask(ELandscapeToolTargetTypeFlags::Heightmap);
		}
	}
}

TArray<UE::Landscape::EditLayers::FEditLayerRenderItem> ACSLandscapeEditLayerBase::GetRenderItems(
	const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const
{
	using namespace UE::Landscape::EditLayers;

	FEditLayerTargetTypeState EnabledState(InMergeContext,
		ShouldSupportHeightmap() ? ELandscapeToolTargetTypeFlags::Heightmap : ELandscapeToolTargetTypeFlags::None);

	return { FEditLayerRenderItem(EnabledState, FInputWorldArea::CreateInfinite(), FOutputWorldArea::CreateLocalComponent(), false) };
}

UE::Landscape::EditLayers::ERenderFlags ACSLandscapeEditLayerBase::GetRenderFlags(
	const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const
{
	return UE::Landscape::EditLayers::ERenderFlags::RenderMode_Immediate;
}

#endif