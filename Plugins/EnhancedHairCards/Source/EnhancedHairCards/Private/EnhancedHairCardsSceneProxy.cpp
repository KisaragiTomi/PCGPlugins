#include "EnhancedHairCardsSceneProxy.h"
#include "EnhancedHairCardsComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "GroomInstance.h"
#include "GroomResources.h"
#include "GroomComponent.h"
#include "HairCardsDatas.h"
#include "StaticMeshResources.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveDrawInterface.h"
#include "RHICommandList.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void PackFloat4SRV(
	FRHICommandListBase& RHICmdList,
	const void* SrcData, uint32 NumElements,
	FBufferRHIRef& OutBuffer, FShaderResourceViewRHIRef& OutSRV)
{
	const uint32 Stride = sizeof(FVector4f);
	FRHIResourceCreateInfo CreateInfo(TEXT("EnhancedHairCardsBuf"));
	OutBuffer = RHICmdList.CreateBuffer(
		NumElements * Stride, BUF_Static | BUF_ShaderResource | BUF_VertexBuffer,
		Stride, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask, CreateInfo);

	void* Dest = RHICmdList.LockBuffer(OutBuffer, 0, NumElements * Stride, RLM_WriteOnly);
	FMemory::Memcpy(Dest, SrcData, NumElements * Stride);
	RHICmdList.UnlockBuffer(OutBuffer);

	OutSRV = RHICmdList.CreateShaderResourceView(OutBuffer, Stride, PF_A32B32G32R32F);
}

static float PackCardLengthAndGroupIndex(float InCardLength, uint32 InCardGroupIndex)
{
	const uint32 EncodedW = FFloat16(InCardLength).Encoded | (FMath::Min(InCardGroupIndex, 0xFFFFu) << 16u);
	float PackedW = 0.f;
	FMemory::Memcpy(&PackedW, &EncodedW, sizeof(float));
	return PackedW;
}

static float GetAxisValue(const FVector3f& InValue, int32 AxisIndex)
{
	return AxisIndex == 0 ? InValue.X : (AxisIndex == 1 ? InValue.Y : InValue.Z);
}

static int32 GetDominantAxis(const FVector3f& InSize)
{
	if (InSize.X >= InSize.Y && InSize.X >= InSize.Z)
	{
		return 0;
	}
	return InSize.Y >= InSize.Z ? 1 : 2;
}

static float HashToUnitFloat(uint32 InValue)
{
	InValue ^= InValue >> 16;
	InValue *= 0x7feb352du;
	InValue ^= InValue >> 15;
	InValue *= 0x846ca68bu;
	InValue ^= InValue >> 16;
	return (InValue & 0x00ffffffu) / 16777215.0f;
}

static FVector3f ToLocalFloatVector(const FVector& InValue)
{
	return FVector3f((float)InValue.X, (float)InValue.Y, (float)InValue.Z);
}

static FVector ToDoubleVector(const FVector3f& InValue)
{
	return FVector((double)InValue.X, (double)InValue.Y, (double)InValue.Z);
}

static FVector ClosestPointOnSegment(const FVector& Point, const FVector& SegmentStart, const FVector& SegmentEnd, float& OutAlpha)
{
	const FVector Segment = SegmentEnd - SegmentStart;
	const double SegmentLengthSquared = Segment.SizeSquared();
	if (SegmentLengthSquared <= UE_SMALL_NUMBER)
	{
		OutAlpha = 0.0f;
		return SegmentStart;
	}

	const double Alpha = FVector::DotProduct(Point - SegmentStart, Segment) / SegmentLengthSquared;
	OutAlpha = (float)FMath::Clamp(Alpha, 0.0, 1.0);
	return SegmentStart + Segment * OutAlpha;
}

class FEnhancedHairCardsGuideMaterialRenderProxy final : public FMaterialRenderProxy
{
public:
	FEnhancedHairCardsGuideMaterialRenderProxy(const FMaterialRenderProxy* InParent, const FLinearColor& InColor)
		: FMaterialRenderProxy(InParent ? InParent->GetMaterialName() : TEXT("FEnhancedHairCardsGuideMaterialRenderProxy"))
		, Parent(InParent)
		, Color(InColor)
	{
	}

	virtual const FMaterial* GetMaterialNoFallback(ERHIFeatureLevel::Type InFeatureLevel) const override
	{
		return Parent ? Parent->GetMaterialNoFallback(InFeatureLevel) : nullptr;
	}

	virtual const FMaterialRenderProxy* GetFallback(ERHIFeatureLevel::Type InFeatureLevel) const override
	{
		return Parent ? Parent->GetFallback(InFeatureLevel) : nullptr;
	}

	virtual bool GetParameterValue(
		EMaterialParameterType Type,
		const FHashedMaterialParameterInfo& ParameterInfo,
		FMaterialParameterValue& OutValue,
		const FMaterialRenderContext& Context) const override
	{
		if (Type == EMaterialParameterType::Vector &&
			(ParameterInfo.Name == NAME_VectorProperty || ParameterInfo.Name == NAME_Color))
		{
			OutValue = FVector3f(Color);
			return true;
		}
		if (Type == EMaterialParameterType::Scalar)
		{
			if (ParameterInfo.Name == NAME_FloatProperty)
			{
				OutValue = 0.f;
				return true;
			}
			if (ParameterInfo.Name == NAME_ByteProperty)
			{
				OutValue = 0.f;
				return true;
			}
			if (ParameterInfo.Name == NAME_IntProperty)
			{
				OutValue = 1.f;
				return true;
			}
		}

		return Parent ? Parent->GetParameterValue(Type, ParameterInfo, OutValue, Context) : false;
	}

private:
	const FMaterialRenderProxy* Parent = nullptr;
	FLinearColor Color = FLinearColor::White;
};

static UMaterialInterface* ResolveDefaultGroomGuideMaterial()
{
	const UGroomComponent* GroomComponentCDO = GetDefault<UGroomComponent>();
	if (GroomComponentCDO && GroomComponentCDO->Strands_DebugMaterial)
	{
		return GroomComponentCDO->Strands_DebugMaterial;
	}

	return GEngine ? GEngine->DebugMeshMaterial : nullptr;
}

static UMaterialInterface* ResolveDefaultGroomCardsMaterial()
{
	const UGroomComponent* GroomComponentCDO = GetDefault<UGroomComponent>();
	if (GroomComponentCDO && GroomComponentCDO->Cards_DefaultMaterial)
	{
		return GroomComponentCDO->Cards_DefaultMaterial;
	}

	return UMaterial::GetDefaultMaterial(MD_Surface);
}

static const FMaterialRenderProxy* MakeColoredGuideMaterialProxy(
	const UMaterialInterface* InMaterial,
	const FLinearColor& InColor,
	FMeshElementCollector& Collector)
{
	if (InMaterial)
	{
		const FMaterialRenderProxy* BaseProxy = InMaterial->GetRenderProxy();
		if (BaseProxy)
		{
			FEnhancedHairCardsGuideMaterialRenderProxy* GuideProxy =
				new FEnhancedHairCardsGuideMaterialRenderProxy(BaseProxy, InColor);
			Collector.RegisterOneFrameMaterialProxy(GuideProxy);
			return GuideProxy;
		}
	}

	if (GEngine && GEngine->DebugMeshMaterial)
	{
		FColoredMaterialRenderProxy* ColoredProxy =
			new FColoredMaterialRenderProxy(GEngine->DebugMeshMaterial->GetRenderProxy(), InColor);
		Collector.RegisterOneFrameMaterialProxy(ColoredProxy);
		return ColoredProxy;
	}

	return nullptr;
}

static float ComputeWorldThicknessForGuideLine(const FSceneView* View, const FVector& WorldPosition, float ScreenThickness)
{
	if (!View)
	{
		return FMath::Max(ScreenThickness, 0.1f);
	}

	const float SafeScreenThickness = FMath::Max(ScreenThickness, 0.1f);
	const float PixelDiameter = (float)FMath::Max(View->UnscaledViewRect.Width(), View->UnscaledViewRect.Height());
	const float ScreenSize = PixelDiameter > 1.f ? SafeScreenThickness / PixelDiameter : SafeScreenThickness * 0.001f;
	const float ScreenMultiple = FMath::Max(
		0.5f * (float)View->ViewMatrices.GetProjectionMatrix().M[0][0],
		0.5f * (float)View->ViewMatrices.GetProjectionMatrix().M[1][1]);

	if (ScreenMultiple <= UE_SMALL_NUMBER)
	{
		return SafeScreenThickness;
	}

	const float Distance = (float)FVector::Dist(WorldPosition, View->ViewMatrices.GetViewOrigin());
	return FMath::Max(0.05f, (ScreenSize * FMath::Max(1.0f, Distance)) / ScreenMultiple);
}

template <typename ColorArrayType>
static int32 FindOrAddGuideLineColor(const FLinearColor& InColor, ColorArrayType& InOutColors)
{
	const FColor TargetColor = InColor.ToFColor(true);
	for (int32 ColorIndex = 0; ColorIndex < InOutColors.Num(); ++ColorIndex)
	{
		if (InOutColors[ColorIndex].ToFColor(true) == TargetColor)
		{
			return ColorIndex;
		}
	}

	return InOutColors.Add(InColor);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void FEnhancedHairCardsSceneProxy::AppendStoredGuideDebugLines(const TArray<FEnhancedHairCardsGuideCurve>& InGuideCurves)
{
	if (!Settings.GuideDebug.bDrawGuides)
	{
		return;
	}

	for (const FEnhancedHairCardsGuideCurve& GuideCurve : InGuideCurves)
	{
		if (GuideCurve.Points.Num() < 2)
		{
			continue;
		}

		for (int32 PointIndex = 1; PointIndex < GuideCurve.Points.Num(); ++PointIndex)
		{
			FGuideDebugLine& Line = GuideDebugLines.AddDefaulted_GetRef();
			Line.Start = ToLocalFloatVector(GuideCurve.Points[PointIndex - 1]);
			Line.End = ToLocalFloatVector(GuideCurve.Points[PointIndex]);
			Line.Color = Settings.GuideDebug.GuideColor;
		}

		if (Settings.GuideDebug.bDrawRoots)
		{
			const FVector3f Root = ToLocalFloatVector(GuideCurve.Points[0]);
			FVector3f Tangent = ToLocalFloatVector(GuideCurve.Points[1]) - Root;
			if (!Tangent.Normalize())
			{
				Tangent = FVector3f(0.f, 0.f, 1.f);
			}

			FVector3f TickAxis = FVector3f::CrossProduct(Tangent, FVector3f(0.f, 0.f, 1.f));
			if (!TickAxis.Normalize())
			{
				TickAxis = FVector3f::CrossProduct(Tangent, FVector3f(1.f, 0.f, 0.f));
			}
			if (!TickAxis.Normalize())
			{
				TickAxis = FVector3f(1.f, 0.f, 0.f);
			}

			const FVector3f TickOffset = TickAxis * Settings.GuideDebug.RootTickSize;
			FGuideDebugLine& RootLine = GuideDebugLines.AddDefaulted_GetRef();
			RootLine.Start = Root - TickOffset;
			RootLine.End = Root + TickOffset;
			RootLine.Color = Settings.GuideDebug.RootColor;
		}
	}
}

void FEnhancedHairCardsSceneProxy::BuildGuideVertexBindings()
{
	GuideVertexBindings.Reset();
	GuideVertexBindings.SetNum(CachedRestPositions.Num());

	if (RestGuideCurves.Num() == 0 || CachedRestPositions.Num() == 0)
	{
		return;
	}

	for (int32 VertexIndex = 0; VertexIndex < CachedRestPositions.Num(); ++VertexIndex)
	{
		const FVector VertexPosition = ToDoubleVector(CachedRestPositions[VertexIndex]);
		double BestDistanceSquared = TNumericLimits<double>::Max();
		FGuideVertexBinding BestBinding;

		for (int32 GuideIndex = 0; GuideIndex < RestGuideCurves.Num(); ++GuideIndex)
		{
			const FEnhancedHairCardsGuideCurve& GuideCurve = RestGuideCurves[GuideIndex];
			for (int32 SegmentIndex = 0; SegmentIndex + 1 < GuideCurve.Points.Num(); ++SegmentIndex)
			{
				float SegmentAlpha = 0.0f;
				const FVector ClosestPoint = ClosestPointOnSegment(
					VertexPosition,
					GuideCurve.Points[SegmentIndex],
					GuideCurve.Points[SegmentIndex + 1],
					SegmentAlpha);
				const double DistanceSquared = FVector::DistSquared(VertexPosition, ClosestPoint);
				if (DistanceSquared < BestDistanceSquared)
				{
					BestDistanceSquared = DistanceSquared;
					BestBinding.GuideIndex = GuideIndex;
					BestBinding.SegmentIndex = SegmentIndex;
					BestBinding.SegmentAlpha = SegmentAlpha;
				}
			}
		}

		GuideVertexBindings[VertexIndex] = BestBinding;
	}
}

void FEnhancedHairCardsSceneProxy::ApplyGuideDeformation(const TArray<FEnhancedHairCardsGuideCurve>& InGuideCurves)
{
	if (CachedRestPositions.Num() == 0 || GuideVertexBindings.Num() != CachedRestPositions.Num())
	{
		return;
	}

	CachedPreviousPositions = CachedPositions;
	CachedPositions = CachedRestPositions;
	CachedTangentX = CachedRestTangentX;
	CachedTangentZ = CachedRestTangentZ;

	const bool bApplyGuideRotation = Settings.Dynamics.bGuideRotationEnabled;
	const float GuideRotationStrength = FMath::Clamp(Settings.Dynamics.GuideRotationStrength, 0.0f, 1.0f);
	for (int32 VertexIndex = 0; VertexIndex < CachedRestPositions.Num(); ++VertexIndex)
	{
		const FGuideVertexBinding& Binding = GuideVertexBindings[VertexIndex];
		if (!RestGuideCurves.IsValidIndex(Binding.GuideIndex)
			|| !InGuideCurves.IsValidIndex(Binding.GuideIndex))
		{
			continue;
		}

		const FEnhancedHairCardsGuideCurve& RestCurve = RestGuideCurves[Binding.GuideIndex];
		const FEnhancedHairCardsGuideCurve& SimulatedCurve = InGuideCurves[Binding.GuideIndex];
		if (!RestCurve.Points.IsValidIndex(Binding.SegmentIndex + 1)
			|| !SimulatedCurve.Points.IsValidIndex(Binding.SegmentIndex + 1))
		{
			continue;
		}

		const FVector RestGuidePoint = FMath::Lerp(
			RestCurve.Points[Binding.SegmentIndex],
			RestCurve.Points[Binding.SegmentIndex + 1],
			(double)Binding.SegmentAlpha);
		const FVector SimulatedGuidePoint = FMath::Lerp(
			SimulatedCurve.Points[Binding.SegmentIndex],
			SimulatedCurve.Points[Binding.SegmentIndex + 1],
			(double)Binding.SegmentAlpha);

		FQuat GuideRotation = FQuat::Identity;
		if (bApplyGuideRotation && GuideRotationStrength > UE_SMALL_NUMBER)
		{
			const FVector RestGuideDirection = (RestCurve.Points[Binding.SegmentIndex + 1] - RestCurve.Points[Binding.SegmentIndex]).GetSafeNormal();
			const FVector SimulatedGuideDirection = (SimulatedCurve.Points[Binding.SegmentIndex + 1] - SimulatedCurve.Points[Binding.SegmentIndex]).GetSafeNormal();
			if (!RestGuideDirection.IsNearlyZero() && !SimulatedGuideDirection.IsNearlyZero())
			{
				GuideRotation = FQuat::FindBetweenNormals(RestGuideDirection, SimulatedGuideDirection);
				if (GuideRotationStrength < 1.0f)
				{
					GuideRotation = FQuat::Slerp(FQuat::Identity, GuideRotation, GuideRotationStrength);
				}
				GuideRotation.Normalize();
			}
		}

		const FVector RestVertexPosition = ToDoubleVector(CachedRestPositions[VertexIndex]);
		const FVector RestGuideOffset = RestVertexPosition - RestGuidePoint;
		const FVector SimulatedVertexPosition = SimulatedGuidePoint + GuideRotation.RotateVector(RestGuideOffset);
		CachedPositions[VertexIndex] = ToLocalFloatVector(SimulatedVertexPosition);

		if (CachedRestTangentX.IsValidIndex(VertexIndex) && CachedTangentX.IsValidIndex(VertexIndex))
		{
			const FVector RotatedTangentX = GuideRotation.RotateVector(ToDoubleVector(CachedRestTangentX[VertexIndex]));
			CachedTangentX[VertexIndex] = ToLocalFloatVector(RotatedTangentX.GetSafeNormal());
		}
		if (CachedRestTangentZ.IsValidIndex(VertexIndex) && CachedTangentZ.IsValidIndex(VertexIndex))
		{
			const FVector4f& RestTangentZ = CachedRestTangentZ[VertexIndex];
			const FVector RotatedTangentZ = GuideRotation.RotateVector(FVector(RestTangentZ.X, RestTangentZ.Y, RestTangentZ.Z));
			const FVector NormalizedTangentZ = RotatedTangentZ.GetSafeNormal();
			CachedTangentZ[VertexIndex] = FVector4f(
				(float)NormalizedTangentZ.X,
				(float)NormalizedTangentZ.Y,
				(float)NormalizedTangentZ.Z,
				RestTangentZ.W);
		}
	}

	if (Settings.GuideDebug.bDrawGuides)
	{
		GuideDebugLines.Reset();
		AppendStoredGuideDebugLines(InGuideCurves);
	}
}

void FEnhancedHairCardsSceneProxy::BindSourceGroomCardsResource(const UEnhancedHairCardsComponent* InComponent)
{
	if (!InComponent || !InComponent->bUseSourceGroomSimulation)
	{
		return;
	}

	const UGroomComponent* GroomComponent = InComponent->SourceGroomComponent.Get();
	if (!GroomComponent)
	{
		return;
	}

	const int32 GroupIndex = InComponent->SourceGroupIndex;
	const int32 LODIndex = FMath::Max(InComponent->SourceLODIndex, 0);
	const FHairGroupInstance* HairGroupInstance = GroomComponent->GetGroupInstance(GroupIndex);
	if (!HairGroupInstance || !HairGroupInstance->Cards.IsValid(LODIndex))
	{
		return;
	}

	const FHairGroupInstance::FCards::FLOD& CardsLOD = HairGroupInstance->Cards.LODs[LODIndex];
	const FHairCardsRestResource* RestResource = CardsLOD.RestResource;
	FHairCardsDeformedResource* DeformedResource = CardsLOD.DeformedResource;
	if (!RestResource || !DeformedResource || !RestResource->RestIndexBuffer.IsInitialized())
	{
		return;
	}

	const uint32 VertexCount = RestResource->GetVertexCount();
	const uint32 IndexCount = RestResource->BulkData.Indices.Num();
	if (VertexCount == 0 || IndexCount == 0)
	{
		return;
	}

	const FRDGExternalBuffer& CurrentPositionBuffer = DeformedResource->GetBuffer(FHairCardsDeformedResource::Current);
	const FRDGExternalBuffer& PreviousPositionBuffer = DeformedResource->GetBuffer(FHairCardsDeformedResource::Previous);
	const FRDGExternalBuffer& DeformedNormalBuffer = DeformedResource->DeformedNormalBuffer;
	if (!CurrentPositionBuffer.SRV || !PreviousPositionBuffer.SRV || !DeformedNormalBuffer.SRV)
	{
		return;
	}

	bUsingSourceGroomCardsResource = true;
	Settings.Dynamics.bEnabled = false;
	VertexBuffers.NumVertices = VertexCount;
	VertexBuffers.NumIndices = IndexCount;
	VertexBuffers.NumUVs = FMath::Clamp(RestResource->BulkData.GetNumUVs(), 1u, (uint32)ENHANCED_HAIR_CARDS_MAX_UV);
	VertexBuffers.BoundingBox = RestResource->BulkData.GetBounds();
	NumUVs = VertexBuffers.NumUVs;

	VertexBuffers.PositionSRV = CurrentPositionBuffer.SRV;
	VertexBuffers.PreviousPositionSRV = PreviousPositionBuffer.SRV;
	VertexBuffers.NormalsSRV = DeformedNormalBuffer.SRV;
	VertexBuffers.UVsSRV = RestResource->UVsBuffer.ShaderResourceViewRHI;
	VertexBuffers.MaterialsSRV = RestResource->MaterialsBuffer.ShaderResourceViewRHI;
	VertexBuffers.VertexColorsSRV = RestResource->BulkData.Header.bVertexColor
		? RestResource->VertexColorsBuffer.ShaderResourceViewRHI
		: RestResource->MaterialsBuffer.ShaderResourceViewRHI;
	SourceGroomIndexBuffer = &RestResource->RestIndexBuffer;

	Sections.Reset();
	FEnhancedHairCardsMeshSection& Section = Sections.AddDefaulted_GetRef();
	Section.FirstIndex = 0;
	Section.NumPrimitives = RestResource->GetPrimitiveCount();
	Section.MinVertexIndex = 0;
	Section.MaxVertexIndex = VertexCount - 1;
	Section.GroupIndex = (uint32)FMath::Max(InComponent->SourceGroupIndex, 0);
	Section.bCastShadow = true;
	Section.Material = InComponent->GetMaterial(0);
	if (!Section.Material)
	{
		Section.Material = ResolveDefaultGroomCardsMaterial();
	}
	if (Section.Material)
	{
		MaterialRelevance |= Section.Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	}

	CachedDynamicsData.SetNumZeroed(VertexCount);
}

FEnhancedHairCardsSceneProxy::FEnhancedHairCardsSceneProxy(const UEnhancedHairCardsComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
	, VertexFactory(GetScene().GetFeatureLevel(), "FEnhancedHairCardsVF")
{
	bWillEverBeLit = true;

	Settings  = InComponent->CardSettings;
	NumUVs    = (uint32)FMath::Clamp(Settings.NumUVChannels, 1, ENHANCED_HAIR_CARDS_MAX_UV);
	for (const FEnhancedHairCardsGuideCurve& GuideCurve : InComponent->GuideCurves)
	{
		if (GuideCurve.Points.Num() >= 2)
		{
			RestGuideCurves.Add(GuideCurve);
		}
	}

	const bool bUseStoredGuideCurves = RestGuideCurves.Num() > 0;
	if (bUseStoredGuideCurves)
	{
		AppendStoredGuideDebugLines(RestGuideCurves);
	}
	if (Settings.GuideDebug.bDrawGuides)
	{
		GuideDebugMaterial = ResolveDefaultGroomGuideMaterial();
		if (!GuideDebugMaterial)
		{
			GuideDebugMaterial = ResolveDefaultGroomCardsMaterial();
		}
		MaterialRelevance |= GuideDebugMaterial->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	}

	UMaterialInterface* DefaultGroomCardsMaterial = ResolveDefaultGroomCardsMaterial();
	BindSourceGroomCardsResource(InComponent);

	// Extract geometry from StaticMesh LOD0
	const UStaticMesh* SM = InComponent->SourceMesh;
	if (!bUsingSourceGroomCardsResource && SM && SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
	{
		const FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
		const uint32 NumVerts   = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 NumIndices = LOD.IndexBuffer.GetNumIndices();

		CachedPositions.SetNumUninitialized(NumVerts);
		CachedTangentX.SetNumUninitialized(NumVerts);
		CachedTangentZ.SetNumUninitialized(NumVerts);
		CachedColors.SetNumUninitialized(NumVerts);
		CachedDynamicsData.SetNumZeroed(NumVerts);

		const uint32 MeshUVCount = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
		const uint32 PackedUVCount = (NumUVs + 1) / 2;
		CachedUVs.SetNumZeroed(NumVerts * PackedUVCount * 2);

		for (uint32 i = 0; i < NumVerts; ++i)
		{
			CachedPositions[i] = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(i);
			CachedTangentX[i]  = FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i));
			CachedTangentZ[i]  = FVector4f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));

			if (LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0)
			{
				CachedColors[i] = LOD.VertexBuffers.ColorVertexBuffer.VertexColor(i);
			}
			else
			{
				CachedColors[i] = FColor::White;
			}

			for (uint32 uv = 0; uv < NumUVs; ++uv)
			{
				const uint32 SourceUVIndex = MeshUVCount > 0 ? FMath::Min(uv, MeshUVCount - 1) : 0;
				FVector2f UV = MeshUVCount > 0
					? LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, SourceUVIndex)
					: FVector2f::ZeroVector;
				CachedUVs[i * PackedUVCount * 2 + uv] = UV;
			}
		}
		CachedRestPositions = CachedPositions;
		CachedPreviousPositions = CachedPositions;
		CachedRestTangentX = CachedTangentX;
		CachedRestTangentZ = CachedTangentZ;

		LOD.IndexBuffer.GetCopy(CachedIndices);

		for (const FStaticMeshSection& SourceSection : LOD.Sections)
		{
			if (SourceSection.NumTriangles == 0)
			{
				continue;
			}

			FEnhancedHairCardsMeshSection& Section = Sections.AddDefaulted_GetRef();
			Section.FirstIndex = SourceSection.FirstIndex;
			Section.NumPrimitives = SourceSection.NumTriangles;
			Section.MinVertexIndex = SourceSection.MinVertexIndex;
			Section.MaxVertexIndex = SourceSection.MaxVertexIndex;
			Section.GroupIndex = (uint32)FMath::Max(SourceSection.MaterialIndex, 0);
			Section.bCastShadow = SourceSection.bCastShadow;
			Section.Material = InComponent->GetMaterial(SourceSection.MaterialIndex);
			if (!Section.Material)
			{
				Section.Material = DefaultGroomCardsMaterial;
			}
			MaterialRelevance |= Section.Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
		}

		if (Sections.Num() == 0 && NumIndices > 0)
		{
			FEnhancedHairCardsMeshSection& Section = Sections.AddDefaulted_GetRef();
			Section.FirstIndex = 0;
			Section.NumPrimitives = NumIndices / 3;
			Section.MinVertexIndex = 0;
			Section.MaxVertexIndex = NumVerts > 0 ? NumVerts - 1 : 0;
			Section.GroupIndex = 0;
			Section.Material = InComponent->GetMaterial(0);
			if (!Section.Material)
			{
				Section.Material = DefaultGroomCardsMaterial;
			}
			MaterialRelevance |= Section.Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
		}

		CachedPackedPositionW.SetNumZeroed(NumVerts);
		const FBox MeshBoundingBox = SM->GetBoundingBox();
		const FVector3f MeshCenter = FVector3f(MeshBoundingBox.GetCenter());
		uint32 DynamicsComponentIndex = 0;
		for (const FEnhancedHairCardsMeshSection& Section : Sections)
		{
			const uint32 TriCount = Section.NumPrimitives;
			if (TriCount == 0)
			{
				continue;
			}

			TMap<uint32, TArray<uint32>> VertexToTriangles;
			VertexToTriangles.Reserve(TriCount * 3);
			for (uint32 TriIt = 0; TriIt < TriCount; ++TriIt)
			{
				const uint32 BaseIndex = Section.FirstIndex + TriIt * 3;
				if (BaseIndex + 2 >= (uint32)CachedIndices.Num())
				{
					continue;
				}

				VertexToTriangles.FindOrAdd(CachedIndices[BaseIndex + 0]).Add(TriIt);
				VertexToTriangles.FindOrAdd(CachedIndices[BaseIndex + 1]).Add(TriIt);
				VertexToTriangles.FindOrAdd(CachedIndices[BaseIndex + 2]).Add(TriIt);
			}

			TArray<uint8> VisitedTriangles;
			VisitedTriangles.Init(0, TriCount);
			TArray<uint32> TriangleStack;
			TArray<uint32> ComponentVertices;
			TSet<uint32> ComponentVertexSet;

			for (uint32 StartTri = 0; StartTri < TriCount; ++StartTri)
			{
				if (VisitedTriangles[StartTri])
				{
					continue;
				}

				TriangleStack.Reset();
				ComponentVertices.Reset();
				ComponentVertexSet.Reset();
				TriangleStack.Add(StartTri);
				VisitedTriangles[StartTri] = 1;

				while (TriangleStack.Num() > 0)
				{
					const uint32 TriIt = TriangleStack.Pop(EAllowShrinking::No);
					const uint32 BaseIndex = Section.FirstIndex + TriIt * 3;
					if (BaseIndex + 2 >= (uint32)CachedIndices.Num())
					{
						continue;
					}

					for (uint32 CornerIt = 0; CornerIt < 3; ++CornerIt)
					{
						const uint32 VertexIndex = CachedIndices[BaseIndex + CornerIt];
						if (!ComponentVertexSet.Contains(VertexIndex))
						{
							ComponentVertexSet.Add(VertexIndex);
							ComponentVertices.Add(VertexIndex);
						}

						if (const TArray<uint32>* AdjacentTriangles = VertexToTriangles.Find(VertexIndex))
						{
							for (uint32 AdjacentTri : *AdjacentTriangles)
							{
								if (AdjacentTri < TriCount && !VisitedTriangles[AdjacentTri])
								{
									VisitedTriangles[AdjacentTri] = 1;
									TriangleStack.Add(AdjacentTri);
								}
							}
						}
					}
				}

				FBox3f ComponentBounds(ForceInit);
				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex < (uint32)CachedPositions.Num())
					{
						ComponentBounds += CachedPositions[VertexIndex];
					}
				}

				const FVector3f BoundsSize = ComponentBounds.GetSize();
				const float CardLength = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
				const float PackedW = PackCardLengthAndGroupIndex(CardLength, Section.GroupIndex);
				const int32 DynamicsAxis = GetDominantAxis(BoundsSize);
				float MinCoord = TNumericLimits<float>::Max();
				float MaxCoord = -TNumericLimits<float>::Max();
				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex < (uint32)CachedPositions.Num())
					{
						const float Coord = GetAxisValue(CachedPositions[VertexIndex], DynamicsAxis);
						MinCoord = FMath::Min(MinCoord, Coord);
						MaxCoord = FMath::Max(MaxCoord, Coord);
					}
				}

				const float CoordSpan = FMath::Max(MaxCoord - MinCoord, KINDA_SMALL_NUMBER);
				float LowDistance = 0.f;
				float HighDistance = 0.f;
				uint32 LowCount = 0;
				uint32 HighCount = 0;
				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex < (uint32)CachedPositions.Num())
					{
						const FVector3f& Position = CachedPositions[VertexIndex];
						const float RootToTipCandidate = (GetAxisValue(Position, DynamicsAxis) - MinCoord) / CoordSpan;
						const float DistanceToCenter = (Position - MeshCenter).SizeSquared();
						if (RootToTipCandidate <= 0.35f)
						{
							LowDistance += DistanceToCenter;
							++LowCount;
						}
						if (RootToTipCandidate >= 0.65f)
						{
							HighDistance += DistanceToCenter;
							++HighCount;
						}
					}
				}

				const float LowAverageDistance = LowCount > 0 ? LowDistance / LowCount : TNumericLimits<float>::Max();
				const float HighAverageDistance = HighCount > 0 ? HighDistance / HighCount : TNumericLimits<float>::Max();
				const bool bRootAtMaxCoord = HighAverageDistance < LowAverageDistance;
				const float ComponentPhase = HashToUnitFloat(DynamicsComponentIndex + Section.GroupIndex * 4099u);
				const float ComponentSideSign = HashToUnitFloat(DynamicsComponentIndex * 17u + 23u) < 0.5f ? -1.f : 1.f;

				if (Settings.GuideDebug.bDrawGuides && !bUseStoredGuideCurves && ComponentVertices.Num() > 1)
				{
					constexpr int32 GuideBinCount = 8;
					TArray<FVector3f, TInlineAllocator<GuideBinCount>> GuideCenters;
					TArray<uint32, TInlineAllocator<GuideBinCount>> GuideCounts;
					GuideCenters.Init(FVector3f::ZeroVector, GuideBinCount);
					GuideCounts.Init(0, GuideBinCount);

					for (uint32 VertexIndex : ComponentVertices)
					{
						if (VertexIndex >= (uint32)CachedPositions.Num())
						{
							continue;
						}

						float RootToTip = (GetAxisValue(CachedPositions[VertexIndex], DynamicsAxis) - MinCoord) / CoordSpan;
						RootToTip = bRootAtMaxCoord ? 1.f - RootToTip : RootToTip;
						if (Settings.Dynamics.bInvertRootToTip)
						{
							RootToTip = 1.f - RootToTip;
						}

						const int32 BinIndex = FMath::Clamp(FMath::FloorToInt(FMath::Clamp(RootToTip, 0.f, 0.9999f) * GuideBinCount), 0, GuideBinCount - 1);
						GuideCenters[BinIndex] += CachedPositions[VertexIndex];
						++GuideCounts[BinIndex];
					}

					int32 PreviousBinIndex = INDEX_NONE;
					for (int32 BinIndex = 0; BinIndex < GuideBinCount; ++BinIndex)
					{
						if (GuideCounts[BinIndex] == 0)
						{
							continue;
						}

						GuideCenters[BinIndex] /= (float)GuideCounts[BinIndex];
						if (PreviousBinIndex != INDEX_NONE)
						{
							FGuideDebugLine& Line = GuideDebugLines.AddDefaulted_GetRef();
							Line.Start = GuideCenters[PreviousBinIndex];
							Line.End = GuideCenters[BinIndex];
							Line.Color = Settings.GuideDebug.GuideColor;
						}
						PreviousBinIndex = BinIndex;
					}

					if (Settings.GuideDebug.bDrawRoots && PreviousBinIndex != INDEX_NONE)
					{
						int32 RootBinIndex = INDEX_NONE;
						for (int32 BinIndex = 0; BinIndex < GuideBinCount; ++BinIndex)
						{
							if (GuideCounts[BinIndex] > 0)
							{
								RootBinIndex = BinIndex;
								break;
							}
						}

						if (RootBinIndex != INDEX_NONE)
						{
							const FVector3f TickAxis = DynamicsAxis == 0 ? FVector3f(0.f, 1.f, 0.f) : FVector3f(1.f, 0.f, 0.f);
							const FVector3f TickOffset = TickAxis * Settings.GuideDebug.RootTickSize;
							FGuideDebugLine& RootLine = GuideDebugLines.AddDefaulted_GetRef();
							RootLine.Start = GuideCenters[RootBinIndex] - TickOffset;
							RootLine.End = GuideCenters[RootBinIndex] + TickOffset;
							RootLine.Color = Settings.GuideDebug.RootColor;
						}
					}
				}

				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex < (uint32)CachedPackedPositionW.Num())
					{
						CachedPackedPositionW[VertexIndex] = PackedW;

						float RootToTip = (GetAxisValue(CachedPositions[VertexIndex], DynamicsAxis) - MinCoord) / CoordSpan;
						RootToTip = bRootAtMaxCoord ? 1.f - RootToTip : RootToTip;
						if (Settings.Dynamics.bInvertRootToTip)
						{
							RootToTip = 1.f - RootToTip;
						}

						CachedDynamicsData[VertexIndex] = FVector4f(
							FMath::Clamp(RootToTip, 0.f, 1.f),
							ComponentPhase,
							ComponentSideSign,
							CardLength);
					}
				}
				++DynamicsComponentIndex;
			}
		}

		VertexBuffers.NumVertices = NumVerts;
		VertexBuffers.NumIndices  = NumIndices;
		VertexBuffers.NumUVs      = NumUVs;
		VertexBuffers.BoundingBox = SM->GetBoundingBox();
		BuildGuideVertexBindings();
	}
}

FEnhancedHairCardsSceneProxy::~FEnhancedHairCardsSceneProxy()
{
	VertexFactory.ReleaseResource();
	if (!bUsingSourceGroomCardsResource && VertexBuffers.IndexBuffer.IsInitialized())
	{
		VertexBuffers.IndexBuffer.ReleaseResource();
	}
}

SIZE_T FEnhancedHairCardsSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FEnhancedHairCardsSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

// ---------------------------------------------------------------------------
// GPU resource upload
// ---------------------------------------------------------------------------

void FEnhancedHairCardsSceneProxy::UploadVertexData(FRHICommandListBase& RHICmdList)
{
	const uint32 NV = VertexBuffers.NumVertices;
	if (NV == 0) return;

	if (!bUsingSourceGroomCardsResource)
	{
		UploadPositionData(RHICmdList);
		UploadNormalData(RHICmdList);
	}

	// UVs (packed float4)
	if (!bUsingSourceGroomCardsResource)
	{
		const uint32 PackedUVCount = (NumUVs + 1) / 2;
		const uint32 TotalElements = NV * PackedUVCount;
		TArray<FVector4f> UVData;
		UVData.SetNumZeroed(TotalElements);

		for (uint32 v = 0; v < NV; ++v)
		{
			for (uint32 p = 0; p < PackedUVCount; ++p)
			{
				uint32 uvIdx0 = p * 2;
				uint32 uvIdx1 = p * 2 + 1;
				FVector2f uv0 = (uvIdx0 < (uint32)CachedUVs.Num() / NV)
					? CachedUVs[v * PackedUVCount * 2 + uvIdx0]
					: FVector2f::ZeroVector;
				FVector2f uv1 = (uvIdx1 < (uint32)CachedUVs.Num() / NV)
					? CachedUVs[v * PackedUVCount * 2 + uvIdx1]
					: FVector2f::ZeroVector;
				UVData[v * PackedUVCount + p] = FVector4f(uv0.X, uv0.Y, uv1.X, uv1.Y);
			}
		}
		PackFloat4SRV(RHICmdList, UVData.GetData(), TotalElements,
			VertexBuffers.UVsBuffer, VertexBuffers.UVsSRV);
	}

	// Material fallback data for Hair Attribute material nodes.
	if (!bUsingSourceGroomCardsResource)
	{
		TArray<FVector4f> MatData;
		MatData.Init(FVector4f(1.f, 1.f, 1.f, 0.5f), NV);
		PackFloat4SRV(RHICmdList, MatData.GetData(), NV,
			VertexBuffers.MaterialsBuffer, VertexBuffers.MaterialsSRV);
	}

	// Vertex Colors (float4 RGBA)
	if (!bUsingSourceGroomCardsResource)
	{
		TArray<FVector4f> ColData;
		ColData.SetNumUninitialized(NV);
		for (uint32 i = 0; i < NV; ++i)
		{
			const FColor& C = CachedColors[i];
			ColData[i] = FVector4f(C.R / 255.f, C.G / 255.f, C.B / 255.f, C.A / 255.f);
		}
		PackFloat4SRV(RHICmdList, ColData.GetData(), NV,
			VertexBuffers.VertexColorsBuffer, VertexBuffers.VertexColorsSRV);
	}

	// Dynamics data (x=root-to-tip mask, y=random phase, z=side sign, w=card length)
	{
		TArray<FVector4f> DynamicsData;
		DynamicsData.SetNumZeroed(NV);
		for (uint32 i = 0; i < NV; ++i)
		{
			DynamicsData[i] = CachedDynamicsData.IsValidIndex(i)
				? CachedDynamicsData[i]
				: FVector4f(0.f, 0.f, 1.f, 0.f);
		}
		PackFloat4SRV(RHICmdList, DynamicsData.GetData(), NV,
			VertexBuffers.DynamicsBuffer, VertexBuffers.DynamicsSRV);
	}

	// Index buffer
	if (!bUsingSourceGroomCardsResource)
	{
		if (VertexBuffers.NumIndices == 0 || CachedIndices.Num() == 0)
		{
			return;
		}

		const uint32 ISize = VertexBuffers.NumIndices * sizeof(uint32);
		FRHIResourceCreateInfo CreateInfo(TEXT("EnhancedHairCardsIB"));
		VertexBuffers.IndexBufferRHI = RHICmdList.CreateBuffer(
			ISize, BUF_Static | BUF_IndexBuffer, sizeof(uint32),
			ERHIAccess::VertexOrIndexBuffer, CreateInfo);
		void* Dest = RHICmdList.LockBuffer(VertexBuffers.IndexBufferRHI, 0, ISize, RLM_WriteOnly);
		FMemory::Memcpy(Dest, CachedIndices.GetData(), ISize);
		RHICmdList.UnlockBuffer(VertexBuffers.IndexBufferRHI);
		VertexBuffers.IndexBuffer.IndexBufferRHI = VertexBuffers.IndexBufferRHI;
		VertexBuffers.IndexBuffer.InitResource(RHICmdList);
	}
}

void FEnhancedHairCardsSceneProxy::UploadPositionData(FRHICommandListBase& RHICmdList)
{
	const uint32 NV = VertexBuffers.NumVertices;
	if (NV == 0 || CachedPositions.Num() < (int32)NV)
	{
		return;
	}

	TArray<FVector4f> PosData;
	PosData.SetNumUninitialized(NV);
	TArray<FVector4f> PrevPosData;
	PrevPosData.SetNumUninitialized(NV);
	for (uint32 i = 0; i < NV; ++i)
	{
		const float PackedW = CachedPackedPositionW.IsValidIndex(i) ? CachedPackedPositionW[i] : 0.f;
		PosData[i] = FVector4f(CachedPositions[i], PackedW);
		PrevPosData[i] = FVector4f(
			CachedPreviousPositions.IsValidIndex(i) ? CachedPreviousPositions[i] : CachedPositions[i],
			PackedW);
	}

	PackFloat4SRV(RHICmdList, PosData.GetData(), NV,
		VertexBuffers.PositionBuffer, VertexBuffers.PositionSRV);
	PackFloat4SRV(RHICmdList, PrevPosData.GetData(), NV,
		VertexBuffers.PreviousPositionBuffer, VertexBuffers.PreviousPositionSRV);

	if (VertexFactory.GetData().UniformBuffer)
	{
		FEnhancedHairCardsVertexFactory::FDataType VFData = VertexFactory.GetData();
		VFData.Buffers = &VertexBuffers;
		VFData.UniformBuffer = CreateEnhancedHairCardsVFUniformBuffer(&VertexBuffers, NumUVs, &Settings);
		VertexFactory.Data = VFData;
	}
}

void FEnhancedHairCardsSceneProxy::UploadNormalData(FRHICommandListBase& RHICmdList)
{
	const uint32 NV = VertexBuffers.NumVertices;
	if (NV == 0 || CachedTangentX.Num() < (int32)NV || CachedTangentZ.Num() < (int32)NV)
	{
		return;
	}

	TArray<FVector4f> NormData;
	NormData.SetNumUninitialized(NV * 2);
	for (uint32 i = 0; i < NV; ++i)
	{
		NormData[i * 2 + 0] = FVector4f(CachedTangentX[i], 0.f);
		NormData[i * 2 + 1] = CachedTangentZ[i];
	}
	PackFloat4SRV(RHICmdList, NormData.GetData(), NV * 2,
		VertexBuffers.NormalsBuffer, VertexBuffers.NormalsSRV);
}

void FEnhancedHairCardsSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	UploadVertexData(RHICmdList);

	FEnhancedHairCardsVertexFactory::FDataType VFData;
	VFData.Buffers = &VertexBuffers;
	VFData.UniformBuffer = CreateEnhancedHairCardsVFUniformBuffer(&VertexBuffers, NumUVs, &Settings);

	VertexFactory.SetData(VFData);
	VertexFactory.InitResource(RHICmdList);
}

void FEnhancedHairCardsSceneProxy::UpdateSimulatedGuideCurves_RenderThread(
	FRHICommandListBase& RHICmdList,
	const TArray<FEnhancedHairCardsGuideCurve>& InGuideCurves)
{
	if (bUsingSourceGroomCardsResource)
	{
		return;
	}

	if (InGuideCurves.Num() == 0 || RestGuideCurves.Num() == 0 || VertexBuffers.NumVertices == 0)
	{
		return;
	}

	ApplyGuideDeformation(InGuideCurves);
	UploadNormalData(RHICmdList);
	UploadPositionData(RHICmdList);
}

// ---------------------------------------------------------------------------
// Mesh element collection
// ---------------------------------------------------------------------------

void FEnhancedHairCardsSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	const bool bCanRenderCards =
		Settings.bRenderCardsMesh &&
		VertexBuffers.NumVertices > 0 &&
		Sections.Num() > 0 &&
		((bUsingSourceGroomCardsResource && SourceGroomIndexBuffer && SourceGroomIndexBuffer->IndexBufferRHI)
			|| (!bUsingSourceGroomCardsResource && VertexBuffers.IndexBuffer.IsInitialized() && VertexBuffers.IndexBuffer.IndexBufferRHI));
	const bool bCanDrawGuides = Settings.GuideDebug.bDrawGuides && GuideDebugLines.Num() > 0;
	if (!bCanRenderCards && !bCanDrawGuides)
	{
		return;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (!(VisibilityMap & (1 << ViewIndex)))
			continue;

		const FSceneView* View = Views[ViewIndex];
		if (bCanRenderCards)
		{
			for (const FEnhancedHairCardsMeshSection& Section : Sections)
			{
				if (!Section.Material || Section.NumPrimitives == 0 ||
					Section.FirstIndex + Section.NumPrimitives * 3 > VertexBuffers.NumIndices)
				{
					continue;
				}

				FMeshBatch& Mesh = Collector.AllocateMesh();
				Mesh.VertexFactory          = &VertexFactory;
				Mesh.MaterialRenderProxy    = Section.Material->GetRenderProxy();
				Mesh.ReverseCulling         = IsLocalToWorldDeterminantNegative();
				Mesh.Type                   = PT_TriangleList;
				Mesh.DepthPriorityGroup     = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = true;
				Mesh.CastShadow             = Section.bCastShadow;
				Mesh.bUseAsOccluder          = false;

				FMeshBatchElement& BatchElement = Mesh.Elements[0];
				BatchElement.IndexBuffer        = bUsingSourceGroomCardsResource ? SourceGroomIndexBuffer : &VertexBuffers.IndexBuffer;
				BatchElement.PrimitiveIdMode    = VertexFactory.GetPrimitiveIdMode(GetScene().GetFeatureLevel());
				BatchElement.FirstIndex         = Section.FirstIndex;
				BatchElement.NumPrimitives      = Section.NumPrimitives;
				BatchElement.NumInstances       = 1;
				BatchElement.MinVertexIndex     = Section.MinVertexIndex;
				BatchElement.MaxVertexIndex     = Section.MaxVertexIndex;

				FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
					Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
				DynamicPrimitiveUniformBuffer.Set(
					Collector.GetRHICommandList(),
					GetLocalToWorld(),
					GetLocalToWorld(),
					GetBounds(),
					GetLocalBounds(),
					false,
					false,
					AlwaysHasVelocity());
				BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

				Collector.AddMesh(ViewIndex, Mesh);
			}
		}

		if (bCanDrawGuides)
		{
			DrawGuideDebugMesh(View, ViewIndex, Collector);
		}
	}
}

void FEnhancedHairCardsSceneProxy::DrawGuideDebugMesh(
	const FSceneView* View,
	int32 ViewIndex,
	FMeshElementCollector& Collector) const
{
	if (!View || GuideDebugLines.Num() == 0)
	{
		return;
	}

	TArray<FLinearColor, TInlineAllocator<2>> GuideColors;
	TArray<int32> LineColorIndices;
	LineColorIndices.SetNumUninitialized(GuideDebugLines.Num());
	for (int32 LineIndex = 0; LineIndex < GuideDebugLines.Num(); ++LineIndex)
	{
		LineColorIndices[LineIndex] = FindOrAddGuideLineColor(GuideDebugLines[LineIndex].Color, GuideColors);
	}

	const FMatrix& ProxyLocalToWorld = GetLocalToWorld();
	const FVector ViewOrigin = View->ViewMatrices.GetViewOrigin();
	const FVector ViewRight = View->GetViewRight().GetSafeNormal();
	const FVector ViewUp = View->GetViewUp().GetSafeNormal();

	for (int32 ColorIndex = 0; ColorIndex < GuideColors.Num(); ++ColorIndex)
	{
		const FMaterialRenderProxy* MaterialProxy =
			MakeColoredGuideMaterialProxy(GuideDebugMaterial, GuideColors[ColorIndex], Collector);
		if (!MaterialProxy)
		{
			continue;
		}

		FDynamicMeshBuilder MeshBuilder(View->GetFeatureLevel());
		MeshBuilder.ReserveVertices(GuideDebugLines.Num() * 4);
		MeshBuilder.ReserveTriangles(GuideDebugLines.Num() * 2);

		const FColor VertexColor = GuideColors[ColorIndex].ToFColor(true);
		int32 QuadCount = 0;
		for (int32 LineIndex = 0; LineIndex < GuideDebugLines.Num(); ++LineIndex)
		{
			if (LineColorIndices[LineIndex] != ColorIndex)
			{
				continue;
			}

			const FGuideDebugLine& Line = GuideDebugLines[LineIndex];
			const FVector LocalStart(Line.Start);
			const FVector LocalEnd(Line.End);
			const FVector WorldStart = ProxyLocalToWorld.TransformPosition(LocalStart);
			const FVector WorldEnd = ProxyLocalToWorld.TransformPosition(LocalEnd);
			const FVector Segment = WorldEnd - WorldStart;
			const double SegmentLength = Segment.Length();
			if (SegmentLength <= UE_KINDA_SMALL_NUMBER)
			{
				continue;
			}

			const FVector SegmentDir = Segment / SegmentLength;
			const FVector Midpoint = (WorldStart + WorldEnd) * 0.5;
			FVector ViewDir = ViewOrigin - Midpoint;
			if (!ViewDir.Normalize())
			{
				ViewDir = -View->GetViewDirection();
			}

			FVector Side = FVector::CrossProduct(SegmentDir, ViewDir);
			if (!Side.Normalize())
			{
				Side = FVector::CrossProduct(SegmentDir, ViewRight);
			}
			if (!Side.Normalize())
			{
				Side = FVector::CrossProduct(SegmentDir, ViewUp);
			}
			if (!Side.Normalize())
			{
				continue;
			}

			const float HalfThickness = ComputeWorldThicknessForGuideLine(View, Midpoint, Settings.GuideDebug.LineThickness) * 0.5f;
			const FVector LocalOffset = ProxyLocalToWorld.InverseTransformVector(Side * HalfThickness);
			const FVector3f LocalOffset3f = FVector3f((float)LocalOffset.X, (float)LocalOffset.Y, (float)LocalOffset.Z);
			const FVector3f Tangent = FVector3f((float)SegmentDir.X, (float)SegmentDir.Y, (float)SegmentDir.Z);
			const FVector3f Normal = FVector3f((float)ViewDir.X, (float)ViewDir.Y, (float)ViewDir.Z);

			const int32 V0 = MeshBuilder.AddVertex(FDynamicMeshVertex(Line.Start - LocalOffset3f, Tangent, Normal, FVector2f(0.f, 0.f), VertexColor));
			const int32 V1 = MeshBuilder.AddVertex(FDynamicMeshVertex(Line.Start + LocalOffset3f, Tangent, Normal, FVector2f(0.f, 1.f), VertexColor));
			const int32 V2 = MeshBuilder.AddVertex(FDynamicMeshVertex(Line.End + LocalOffset3f, Tangent, Normal, FVector2f(1.f, 1.f), VertexColor));
			const int32 V3 = MeshBuilder.AddVertex(FDynamicMeshVertex(Line.End - LocalOffset3f, Tangent, Normal, FVector2f(1.f, 0.f), VertexColor));
			MeshBuilder.AddTriangle(V0, V1, V2);
			MeshBuilder.AddTriangle(V0, V2, V3);
			++QuadCount;
		}

		if (QuadCount > 0)
		{
			FDynamicMeshBuilderSettings MeshSettings;
			MeshSettings.CastShadow = false;
			MeshSettings.bDisableBackfaceCulling = true;
			MeshSettings.bReceivesDecals = false;
			MeshSettings.bUseSelectionOutline = false;
			MeshSettings.bCanApplyViewModeOverrides = false;

			const uint8 DepthPriority = Settings.GuideDebug.bDrawInForeground ? SDPG_Foreground : SDPG_World;
			MeshBuilder.GetMesh(ProxyLocalToWorld, MaterialProxy, DepthPriority, MeshSettings, nullptr, ViewIndex, Collector);
		}
	}
}

FPrimitiveViewRelevance FEnhancedHairCardsSceneProxy::GetViewRelevance(
	const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance        = IsShown(View);
	Result.bShadowRelevance      = IsShadowCast(View);
	Result.bDynamicRelevance     = true;
	Result.bStaticRelevance      = false;
	Result.bRenderInMainPass     = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}
