#include "EnhancedHairCardsConverterLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "EnhancedHairCardsAsset.h"
#include "EnhancedHairCardsComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GroomAsset.h"
#include "GroomAssetCards.h"
#include "GroomAssetPhysics.h"
#include "GroomComponent.h"
#include "HairStrandsDatas.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "ObjectTools.h"
#include "StaticMeshResources.h"
#include "UObject/GCObjectScopeGuard.h"

namespace EnhancedHairCardsConverter
{
	static FString NormalizePackagePath(const FString& InPackagePath)
	{
		FString Path = InPackagePath;
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (Path.EndsWith(TEXT(".uasset")))
		{
			Path.LeftChopInline(7);
		}

		const FString ContentMarker = TEXT("/Content/");
		int32 ContentIndex = INDEX_NONE;
		if (Path.FindChar(TEXT(':'), ContentIndex))
		{
			const FString EnhancedHairCardsContent = TEXT("/Plugins/EnhancedHairCards/Content/");
			int32 PluginContentIndex = Path.Find(EnhancedHairCardsContent, ESearchCase::IgnoreCase);
			if (PluginContentIndex != INDEX_NONE)
			{
				Path = TEXT("/EnhancedHairCards/") + Path.Mid(PluginContentIndex + EnhancedHairCardsContent.Len());
			}
			else
			{
				ContentIndex = Path.Find(ContentMarker, ESearchCase::IgnoreCase);
				if (ContentIndex != INDEX_NONE)
				{
					Path = TEXT("/Game/") + Path.Mid(ContentIndex + ContentMarker.Len());
				}
			}
		}

		return Path;
	}

	static FString ResolveGroomSiblingBlueprintPackagePath(const UGroomAsset* GroomAsset, const FString& RequestedOutputPackagePath)
	{
		const FString GroomPackageName = GroomAsset ? GroomAsset->GetOutermost()->GetName() : FString();
		const FString GroomPackagePath = FPackageName::GetLongPackagePath(GroomPackageName);

		FString OutputAssetName;
		const FString NormalizedRequestedPath = NormalizePackagePath(RequestedOutputPackagePath);
		if (FPackageName::IsValidLongPackageName(NormalizedRequestedPath))
		{
			OutputAssetName = FPackageName::GetLongPackageAssetName(NormalizedRequestedPath);
		}

		if (OutputAssetName.IsEmpty())
		{
			OutputAssetName = FString::Printf(TEXT("BP_%s_EHC"), GroomAsset ? *GroomAsset->GetName() : TEXT("EnhancedHairCards"));
		}

		return GroomPackagePath / OutputAssetName;
	}

	static bool IsBetterCardDescription(const FHairGroupsCardsSourceDescription& Candidate, const FHairGroupsCardsSourceDescription& Current, int32 WantedGroupIndex, int32 WantedLODIndex)
	{
		if (!Current.ImportedMesh)
		{
			return true;
		}

		const bool bCandidateGroupMatch = WantedGroupIndex < 0 || Candidate.GroupIndex == WantedGroupIndex;
		const bool bCurrentGroupMatch = WantedGroupIndex < 0 || Current.GroupIndex == WantedGroupIndex;
		if (bCandidateGroupMatch != bCurrentGroupMatch)
		{
			return bCandidateGroupMatch;
		}

		const bool bCandidateLODMatch = WantedLODIndex < 0 || Candidate.LODIndex == WantedLODIndex;
		const bool bCurrentLODMatch = WantedLODIndex < 0 || Current.LODIndex == WantedLODIndex;
		if (bCandidateLODMatch != bCurrentLODMatch)
		{
			return bCandidateLODMatch;
		}

		if (Candidate.LODIndex != Current.LODIndex)
		{
			return Candidate.LODIndex >= 0 && (Current.LODIndex < 0 || Candidate.LODIndex < Current.LODIndex);
		}

		return Candidate.GroupIndex < Current.GroupIndex;
	}

	struct FSelectedCardDescription
	{
		const FHairGroupsCardsSourceDescription* Desc = nullptr;
		int32 SourceIndex = INDEX_NONE;
	};

	static TArray<FSelectedCardDescription> CollectCardDescriptions(const UGroomAsset* GroomAsset, int32 GroupIndex, int32 LODIndex)
	{
		TArray<FSelectedCardDescription> Result;
		const TArray<FHairGroupsCardsSourceDescription>& Cards = GroomAsset->GetHairGroupsCards();

		for (int32 SourceIndex = 0; SourceIndex < Cards.Num(); ++SourceIndex)
		{
			const FHairGroupsCardsSourceDescription& Desc = Cards[SourceIndex];
			if (!Desc.ImportedMesh)
			{
				continue;
			}
			if (GroupIndex >= 0 && Desc.GroupIndex != GroupIndex)
			{
				continue;
			}
			if (LODIndex >= 0 && Desc.LODIndex != LODIndex)
			{
				continue;
			}

			if (LODIndex >= 0)
			{
				FSelectedCardDescription& Selected = Result.AddDefaulted_GetRef();
				Selected.Desc = &Desc;
				Selected.SourceIndex = SourceIndex;
				continue;
			}

			const int32 ExistingIndex = Result.IndexOfByPredicate([&Desc](const FSelectedCardDescription& Existing)
			{
				return Existing.Desc && Existing.Desc->GroupIndex == Desc.GroupIndex;
			});

			if (ExistingIndex == INDEX_NONE)
			{
				FSelectedCardDescription& Selected = Result.AddDefaulted_GetRef();
				Selected.Desc = &Desc;
				Selected.SourceIndex = SourceIndex;
			}
			else if (Result[ExistingIndex].Desc && IsBetterCardDescription(Desc, *Result[ExistingIndex].Desc, GroupIndex, LODIndex))
			{
				Result[ExistingIndex].Desc = &Desc;
				Result[ExistingIndex].SourceIndex = SourceIndex;
			}
		}

		Result.Sort([](const FSelectedCardDescription& A, const FSelectedCardDescription& B)
		{
			const int32 AGroup = A.Desc ? A.Desc->GroupIndex : MAX_int32;
			const int32 BGroup = B.Desc ? B.Desc->GroupIndex : MAX_int32;
			if (AGroup != BGroup)
			{
				return AGroup < BGroup;
			}

			const int32 ALOD = A.Desc ? A.Desc->LODIndex : MAX_int32;
			const int32 BLOD = B.Desc ? B.Desc->LODIndex : MAX_int32;
			if (ALOD != BLOD)
			{
				return ALOD < BLOD;
			}

			return A.SourceIndex < B.SourceIndex;
		});

		return Result;
	}

	static FString MakeCardComponentName(const FHairGroupsCardsSourceDescription& Desc, int32 ComponentIndex, int32 ComponentCount)
	{
		if (ComponentCount == 1)
		{
			return TEXT("EnhancedHairCards");
		}

		const FString LODLabel = Desc.LODIndex >= 0
			? FString::Printf(TEXT("L%d"), Desc.LODIndex)
			: FString(TEXT("LAny"));
		return FString::Printf(TEXT("EnhancedHairCards_G%d_%s_%02d"), Desc.GroupIndex, *LODLabel, ComponentIndex);
	}

	static FName GetNiagaraSolverName(EGroomNiagaraSolvers NiagaraSolver)
	{
		if (const UEnum* SolverEnum = StaticEnum<EGroomNiagaraSolvers>())
		{
			return FName(*SolverEnum->GetNameStringByValue(static_cast<int64>(NiagaraSolver)));
		}
		return NAME_None;
	}

	static UNiagaraSystem* ResolveGroomNiagaraSystem(const FHairSolverSettings& SolverSettings)
	{
		switch (SolverSettings.NiagaraSolver)
		{
		case EGroomNiagaraSolvers::AngularSprings:
			return LoadObject<UNiagaraSystem>(nullptr, TEXT("/HairStrands/Emitters/StableSpringsSystem.StableSpringsSystem"));
		case EGroomNiagaraSolvers::CosseratRods:
			return LoadObject<UNiagaraSystem>(nullptr, TEXT("/HairStrands/Emitters/StableRodsSystem.StableRodsSystem"));
		case EGroomNiagaraSolvers::CustomSolver:
			return SolverSettings.CustomSystem.LoadSynchronous();
		default:
			return nullptr;
		}
	}

	static void PopulateSimulationSettingsFromGroom(
		FEnhancedHairCardsSimulationSettings& OutSimulationSettings,
		const UGroomAsset* GroomAsset,
		const TArray<FSelectedCardDescription>& SelectedCards)
	{
		OutSimulationSettings = FEnhancedHairCardsSimulationSettings();
		if (!GroomAsset)
		{
			return;
		}

		const TArray<FHairGroupsPhysics>& HairGroupsPhysics = GroomAsset->GetHairGroupsPhysics();
		TSet<int32> AddedGroupIndices;

		for (const FSelectedCardDescription& SelectedCard : SelectedCards)
		{
			if (!SelectedCard.Desc)
			{
				continue;
			}

			const int32 SourceGroupIndex = SelectedCard.Desc->GroupIndex;
			const int32 SourceLODIndex = FMath::Max(SelectedCard.Desc->LODIndex, 0);
			if (AddedGroupIndices.Contains(SourceGroupIndex) || !HairGroupsPhysics.IsValidIndex(SourceGroupIndex))
			{
				continue;
			}

			const FHairSolverSettings& SolverSettings = HairGroupsPhysics[SourceGroupIndex].SolverSettings;
			UNiagaraSystem* NiagaraSystem = ResolveGroomNiagaraSystem(SolverSettings);
			if (!NiagaraSystem)
			{
				continue;
			}

			FEnhancedHairCardsNiagaraSimulationGroup& SimulationGroup = OutSimulationSettings.Groups.AddDefaulted_GetRef();
			SimulationGroup.SourceGroupIndex = SourceGroupIndex;
			SimulationGroup.SourceLODIndex = SourceLODIndex;
			SimulationGroup.bEnableSimulation = GroomAsset->IsSimulationEnable(SourceGroupIndex, SourceLODIndex);
			SimulationGroup.SourceNiagaraSolver = GetNiagaraSolverName(SolverSettings.NiagaraSolver);
			SimulationGroup.NiagaraSystem = NiagaraSystem;
			AddedGroupIndices.Add(SourceGroupIndex);
		}
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

	static FVector ToDoubleVector(const FVector3f& InValue)
	{
		return FVector((double)InValue.X, (double)InValue.Y, (double)InValue.Z);
	}

	static UMaterialInterface* ResolveSourceMaterial(const FHairGroupsCardsSourceDescription& CardDesc)
	{
		UMaterialInterface* SourceMaterial = CardDesc.Material;
		if (!SourceMaterial && CardDesc.ImportedMesh)
		{
			SourceMaterial = CardDesc.ImportedMesh->GetMaterial(0);
		}
		return SourceMaterial;
	}

	static UMaterialInterface* ResolveDefaultGroomCardsMaterial()
	{
		const UGroomComponent* GroomComponentCDO = GetDefault<UGroomComponent>();
		return GroomComponentCDO ? GroomComponentCDO->Cards_DefaultMaterial : nullptr;
	}

	static int32 ResolveCardsPlatformLODIndex(const UGroomAsset* GroomAsset, const FHairGroupsCardsSourceDescription& CardDesc)
	{
		if (!GroomAsset)
		{
			return INDEX_NONE;
		}

		const TArray<FHairGroupPlatformData>& GroupsPlatformData = GroomAsset->GetHairGroupsPlatformData();
		if (!GroupsPlatformData.IsValidIndex(CardDesc.GroupIndex))
		{
			return INDEX_NONE;
		}

		const TArray<FHairGroupPlatformData::FCards::FLOD>& LODs = GroupsPlatformData[CardDesc.GroupIndex].Cards.LODs;
		if (CardDesc.LODIndex >= 0 && LODs.IsValidIndex(CardDesc.LODIndex) && LODs[CardDesc.LODIndex].GuideBulkData.IsValid())
		{
			return CardDesc.LODIndex;
		}

		for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
		{
			if (LODs[LODIndex].GuideBulkData.IsValid())
			{
				return LODIndex;
			}
		}

		return INDEX_NONE;
	}

	static bool ExtractGuideCurvesFromBulkData(
		const FHairStrandsBulkData& GuideBulkData,
		int32 SourceGroupIndex,
		int32 SourceLODIndex,
		TArray<FEnhancedHairCardsGuideCurve>& OutGuideCurves)
	{
		const uint32 NumCurves = GuideBulkData.GetNumCurves();
		const uint32 NumPoints = GuideBulkData.GetNumPoints();
		if (NumCurves == 0 || NumPoints == 0)
		{
			return false;
		}

		const int64 ExpectedCurveBytes = int64(NumCurves) * sizeof(FHairStrandsCurveFormat::Type);
		const int64 ExpectedPositionBytes = int64(NumPoints) * sizeof(FHairStrandsPositionFormat::Type);
		if (GuideBulkData.Data.Curves.GetBulkDataSize() < ExpectedCurveBytes ||
			GuideBulkData.Data.Positions.GetBulkDataSize() < ExpectedPositionBytes)
		{
			return false;
		}

		const FHairStrandsCurveFormat::Type* Curves =
			reinterpret_cast<const FHairStrandsCurveFormat::Type*>(GuideBulkData.Data.Curves.Data.LockReadOnly());
		if (!Curves)
		{
			return false;
		}

		const FHairStrandsPositionFormat::Type* Positions =
			reinterpret_cast<const FHairStrandsPositionFormat::Type*>(GuideBulkData.Data.Positions.Data.LockReadOnly());
		if (!Positions)
		{
			GuideBulkData.Data.Curves.Data.Unlock();
			return false;
		}

		const int32 InitialCurveCount = OutGuideCurves.Num();
		const FVector PositionOffset = GuideBulkData.GetPositionOffset();
		for (uint32 CurveIndex = 0; CurveIndex < NumCurves; ++CurveIndex)
		{
			const uint32 PointOffset = Curves[CurveIndex].PointOffset;
			const uint32 PointCount = Curves[CurveIndex].PointCount;
			if (PointCount < 2 || PointOffset + PointCount > NumPoints)
			{
				continue;
			}

			FEnhancedHairCardsGuideCurve& GuideCurve = OutGuideCurves.AddDefaulted_GetRef();
			GuideCurve.SourceGroupIndex = SourceGroupIndex;
			GuideCurve.SourceLODIndex = SourceLODIndex;
			GuideCurve.SourceCurveIndex = (int32)CurveIndex;
			GuideCurve.Points.Reserve((int32)PointCount);
			for (uint32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
			{
				const FHairStrandsPositionFormat::Type& PackedPosition = Positions[PointOffset + PointIndex];
				GuideCurve.Points.Add(PositionOffset + FVector(PackedPosition.X, PackedPosition.Y, PackedPosition.Z));
			}
		}

		GuideBulkData.Data.Positions.Data.Unlock();
		GuideBulkData.Data.Curves.Data.Unlock();

		return OutGuideCurves.Num() > InitialCurveCount;
	}

	static void ExtractGuideCurvesFromGroomCards(
		const UGroomAsset* GroomAsset,
		const FHairGroupsCardsSourceDescription& CardDesc,
		TArray<FEnhancedHairCardsGuideCurve>& OutGuideCurves)
	{
		const int32 PlatformLODIndex = ResolveCardsPlatformLODIndex(GroomAsset, CardDesc);
		if (PlatformLODIndex == INDEX_NONE)
		{
			return;
		}

		const FHairGroupPlatformData::FCards::FLOD& CardsLOD =
			GroomAsset->GetHairGroupsPlatformData()[CardDesc.GroupIndex].Cards.LODs[PlatformLODIndex];
		ExtractGuideCurvesFromBulkData(CardsLOD.GuideBulkData, CardDesc.GroupIndex, PlatformLODIndex, OutGuideCurves);
	}

	static FVector3f GetStaticMeshPosition(const FStaticMeshLODResources& LOD, uint32 VertexIndex)
	{
		return LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex);
	}

	static void ExtractFallbackGuideCurvesFromMesh(
		const FHairGroupsCardsSourceDescription& CardDesc,
		TArray<FEnhancedHairCardsGuideCurve>& OutGuideCurves)
	{
		const UStaticMesh* StaticMesh = CardDesc.ImportedMesh;
		if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
		{
			return;
		}

		const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
		const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 NumIndices = LOD.IndexBuffer.GetNumIndices();
		if (NumVerts == 0 || NumIndices < 3)
		{
			return;
		}

		TArray<uint32> Indices;
		LOD.IndexBuffer.GetCopy(Indices);

		TArray<FVector3f> Positions;
		Positions.SetNumUninitialized(NumVerts);
		for (uint32 VertexIndex = 0; VertexIndex < NumVerts; ++VertexIndex)
		{
			Positions[VertexIndex] = GetStaticMeshPosition(LOD, VertexIndex);
		}

		const FVector MeshCenterDouble = StaticMesh->GetBoundingBox().GetCenter();
		const FVector3f MeshCenter((float)MeshCenterDouble.X, (float)MeshCenterDouble.Y, (float)MeshCenterDouble.Z);
		int32 GeneratedCurveIndex = OutGuideCurves.Num();

		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			const uint32 TriCount = Section.NumTriangles;
			if (TriCount == 0)
			{
				continue;
			}

			TMap<uint32, TArray<uint32>> VertexToTriangles;
			VertexToTriangles.Reserve(TriCount * 3);
			for (uint32 TriIt = 0; TriIt < TriCount; ++TriIt)
			{
				const uint32 BaseIndex = Section.FirstIndex + TriIt * 3;
				if (BaseIndex + 2 >= (uint32)Indices.Num())
				{
					continue;
				}

				VertexToTriangles.FindOrAdd(Indices[BaseIndex + 0]).Add(TriIt);
				VertexToTriangles.FindOrAdd(Indices[BaseIndex + 1]).Add(TriIt);
				VertexToTriangles.FindOrAdd(Indices[BaseIndex + 2]).Add(TriIt);
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
					if (BaseIndex + 2 >= (uint32)Indices.Num())
					{
						continue;
					}

					for (uint32 CornerIt = 0; CornerIt < 3; ++CornerIt)
					{
						const uint32 VertexIndex = Indices[BaseIndex + CornerIt];
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

				if (ComponentVertices.Num() < 2)
				{
					continue;
				}

				FBox3f ComponentBounds(ForceInit);
				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex < (uint32)Positions.Num())
					{
						ComponentBounds += Positions[VertexIndex];
					}
				}

				const int32 DynamicsAxis = GetDominantAxis(ComponentBounds.GetSize());
				float MinCoord = TNumericLimits<float>::Max();
				float MaxCoord = -TNumericLimits<float>::Max();
				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex < (uint32)Positions.Num())
					{
						const float Coord = GetAxisValue(Positions[VertexIndex], DynamicsAxis);
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
					if (VertexIndex < (uint32)Positions.Num())
					{
						const FVector3f& Position = Positions[VertexIndex];
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

				constexpr int32 GuideBinCount = 8;
				TArray<FVector3f, TInlineAllocator<GuideBinCount>> GuideCenters;
				TArray<uint32, TInlineAllocator<GuideBinCount>> GuideCounts;
				GuideCenters.Init(FVector3f::ZeroVector, GuideBinCount);
				GuideCounts.Init(0, GuideBinCount);

				for (uint32 VertexIndex : ComponentVertices)
				{
					if (VertexIndex >= (uint32)Positions.Num())
					{
						continue;
					}

					float RootToTip = (GetAxisValue(Positions[VertexIndex], DynamicsAxis) - MinCoord) / CoordSpan;
					RootToTip = bRootAtMaxCoord ? 1.f - RootToTip : RootToTip;
					const int32 BinIndex = FMath::Clamp(FMath::FloorToInt(FMath::Clamp(RootToTip, 0.f, 0.9999f) * GuideBinCount), 0, GuideBinCount - 1);
					GuideCenters[BinIndex] += Positions[VertexIndex];
					++GuideCounts[BinIndex];
				}

				FEnhancedHairCardsGuideCurve GuideCurve;
				GuideCurve.SourceGroupIndex = CardDesc.GroupIndex;
				GuideCurve.SourceLODIndex = CardDesc.LODIndex;
				GuideCurve.SourceCurveIndex = GeneratedCurveIndex++;
				for (int32 BinIndex = 0; BinIndex < GuideBinCount; ++BinIndex)
				{
					if (GuideCounts[BinIndex] == 0)
					{
						continue;
					}

					GuideCenters[BinIndex] /= (float)GuideCounts[BinIndex];
					GuideCurve.Points.Add(ToDoubleVector(GuideCenters[BinIndex]));
				}

				if (GuideCurve.Points.Num() >= 2)
				{
					OutGuideCurves.Add(MoveTemp(GuideCurve));
				}
			}
		}
	}

	static void PopulatePartFromCardDescription(
		FEnhancedHairCardsPart& OutPart,
		const UGroomAsset* GroomAsset,
		const FHairGroupsCardsSourceDescription& CardDesc,
		int32 SourceDescriptionIndex);

	static void PopulateComponentFromCardDescription(
		UEnhancedHairCardsComponent* ComponentTemplate,
		const UGroomAsset* GroomAsset,
		const FHairGroupsCardsSourceDescription& CardDesc,
		int32 SourceDescriptionIndex)
	{
		FEnhancedHairCardsPart Part;
		PopulatePartFromCardDescription(Part, GroomAsset, CardDesc, SourceDescriptionIndex);
		ComponentTemplate->ApplyHairCardsPart(Part);
	}

	static void PopulatePartFromCardDescription(
		FEnhancedHairCardsPart& OutPart,
		const UGroomAsset* GroomAsset,
		const FHairGroupsCardsSourceDescription& CardDesc,
		int32 SourceDescriptionIndex)
	{
		OutPart.SourceMesh = CardDesc.ImportedMesh;
		OutPart.SourceGroupIndex = CardDesc.GroupIndex;
		OutPart.SourceLODIndex = CardDesc.LODIndex;
		OutPart.SourceCardsDescriptionIndex = SourceDescriptionIndex;
		OutPart.SourceMaterialSlotName = CardDesc.MaterialSlotName;
		OutPart.CardSettings.bRenderCardsMesh = false;
		OutPart.CardSettings.NumUVChannels = 2;
		OutPart.CardSettings.bInvertAtlasV = CardDesc.bInvertUV;
		OutPart.CardSettings.Dynamics.bEnabled = false;
		OutPart.CardSettings.Dynamics.bGuideSimulationEnabled = true;

		OutPart.Material = ResolveDefaultGroomCardsMaterial();
		if (!OutPart.Material)
		{
			OutPart.Material = ResolveSourceMaterial(CardDesc);
		}

		OutPart.GuideCurves.Reset();
		ExtractGuideCurvesFromGroomCards(GroomAsset, CardDesc, OutPart.GuideCurves);
		if (OutPart.GuideCurves.Num() == 0)
		{
			ExtractFallbackGuideCurvesFromMesh(CardDesc, OutPart.GuideCurves);
		}
	}

	static void ApplyGroomComponentMaterial(
		UGroomComponent* GroomComponentTemplate,
		const UGroomAsset* GroomAsset,
		const FHairGroupsCardsSourceDescription& CardDesc)
	{
		UMaterialInterface* SourceMaterial = ResolveSourceMaterial(CardDesc);
		if (!GroomComponentTemplate || !GroomAsset || !SourceMaterial)
		{
			return;
		}

		int32 MaterialIndex = CardDesc.MaterialSlotName != NAME_None ? GroomAsset->GetMaterialIndex(CardDesc.MaterialSlotName) : INDEX_NONE;
		if (MaterialIndex == INDEX_NONE)
		{
			MaterialIndex = 0;
		}

		GroomComponentTemplate->SetMaterial(MaterialIndex, SourceMaterial);
	}

	static void ApplyGroomComponentMaterial(
		UGroomComponent* GroomComponentTemplate,
		const UGroomAsset* GroomAsset,
		const FEnhancedHairCardsPart& Part)
	{
		if (!GroomComponentTemplate || !GroomAsset || !Part.Material)
		{
			return;
		}

		int32 MaterialIndex = Part.SourceMaterialSlotName != NAME_None ? GroomAsset->GetMaterialIndex(Part.SourceMaterialSlotName) : INDEX_NONE;
		if (MaterialIndex == INDEX_NONE)
		{
			MaterialIndex = 0;
		}

		GroomComponentTemplate->SetMaterial(MaterialIndex, Part.Material);
	}
}

UEnhancedHairCardsAsset* UEnhancedHairCardsConverterLibrary::ConvertGroomToEnhancedHairCardsAsset(
	UGroomAsset* GroomAsset,
	const FString& OutputPackagePath,
	int32 GroupIndex,
	int32 LODIndex,
	bool bOverwriteExisting)
{
	using namespace EnhancedHairCardsConverter;

	if (!GroomAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: GroomAsset is null."));
		return nullptr;
	}

	const TArray<FSelectedCardDescription> SelectedCards = CollectCardDescriptions(GroomAsset, GroupIndex, LODIndex);
	if (SelectedCards.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: %s has no imported Groom Cards meshes for Group=%d LOD=%d."),
			*GroomAsset->GetPathName(), GroupIndex, LODIndex);
		return nullptr;
	}

	const FString PackagePath = NormalizePackagePath(OutputPackagePath);
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: invalid output package path '%s'."), *OutputPackagePath);
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	if (UObject* ExistingAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
	{
		if (!bOverwriteExisting)
		{
			UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: output asset already exists: %s."), *ObjectPath);
			return nullptr;
		}

		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(ExistingAsset);
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: could not create package %s."), *PackagePath);
		return nullptr;
	}

	UEnhancedHairCardsAsset* HairCardsAsset = NewObject<UEnhancedHairCardsAsset>(
		Package,
		UEnhancedHairCardsAsset::StaticClass(),
		FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!HairCardsAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: could not create asset %s."), *PackagePath);
		return nullptr;
	}

	HairCardsAsset->SourceGroom = GroomAsset;
	HairCardsAsset->PreviewSettings.bShowOriginalGroom = true;
	HairCardsAsset->PreviewSettings.bKeepSourceGroomVisibleForSimulation = true;
	HairCardsAsset->PreviewSettings.bShowEnhancedCards = true;
	HairCardsAsset->PreviewSettings.bShowGuides = true;
	HairCardsAsset->PreviewSettings.bRenderEnhancedCardsMesh = false;
	PopulateSimulationSettingsFromGroom(HairCardsAsset->SimulationSettings, GroomAsset, SelectedCards);
	HairCardsAsset->Parts.Reserve(SelectedCards.Num());

	TArray<FString> MeshNames;
	MeshNames.Reserve(SelectedCards.Num());
	for (const FSelectedCardDescription& SelectedCard : SelectedCards)
	{
		if (!SelectedCard.Desc)
		{
			continue;
		}

		FEnhancedHairCardsPart& Part = HairCardsAsset->Parts.AddDefaulted_GetRef();
		PopulatePartFromCardDescription(Part, GroomAsset, *SelectedCard.Desc, SelectedCard.SourceIndex);
		if (Part.SourceMesh)
		{
			MeshNames.Add(Part.SourceMesh->GetPathName());
		}
	}

	FAssetRegistryModule::AssetCreated(HairCardsAsset);
	Package->MarkPackageDirty();

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);

	UE_LOG(LogTemp, Display, TEXT("EnhancedHairCards conversion created asset %s from %s using %d card mesh(es), copied %d guide curve(s): %s."),
		*HairCardsAsset->GetPathName(),
		*GroomAsset->GetPathName(),
		HairCardsAsset->Parts.Num(),
		HairCardsAsset->GetTotalGuideCurveCount(),
		*FString::Join(MeshNames, TEXT(", ")));

	UE_LOG(LogTemp, Display, TEXT("EnhancedHairCards conversion copied %d Groom Niagara simulation group(s)."),
		HairCardsAsset->SimulationSettings.Groups.Num());

	return HairCardsAsset;
}

UBlueprint* UEnhancedHairCardsConverterLibrary::ExportEnhancedHairCardsAssetToBlueprint(
	UEnhancedHairCardsAsset* HairCardsAsset,
	const FString& OutputPackagePath,
	bool bOverwriteExisting)
{
	using namespace EnhancedHairCardsConverter;

	if (!HairCardsAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: HairCardsAsset is null."));
		return nullptr;
	}

	FGCObjectScopeGuard HairCardsAssetGuard(HairCardsAsset);
	const FString SourceAssetPath = HairCardsAsset->GetPathName();
	const int32 SourcePartCount = HairCardsAsset->Parts.Num();
	UGroomAsset* GroomAsset = HairCardsAsset->SourceGroom;

	const FString PackagePath = NormalizePackagePath(OutputPackagePath);
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: invalid output package path '%s'."), *OutputPackagePath);
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	if (UObject* ExistingAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
	{
		if (!bOverwriteExisting)
		{
			UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: output asset already exists: %s."), *ObjectPath);
			return nullptr;
		}

		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(ExistingAsset);
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: could not create package %s."), *PackagePath);
		return nullptr;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("EnhancedHairCardsConverter")));

	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: could not create Blueprint %s."), *PackagePath);
		return nullptr;
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* RootNode = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("SceneRoot"));
	if (!RootNode || !Cast<USceneComponent>(RootNode->ComponentTemplate))
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: could not create scene root template."));
		return nullptr;
	}
	SCS->AddNode(RootNode);

	UGroomComponent* GroomComponentTemplate = nullptr;
	if (GroomAsset)
	{
		USCS_Node* GroomComponentNode = SCS->CreateNode(UGroomComponent::StaticClass(), TEXT("OriginalGroomCards"));
		GroomComponentTemplate = Cast<UGroomComponent>(GroomComponentNode ? GroomComponentNode->ComponentTemplate : nullptr);
		if (!GroomComponentTemplate)
		{
			UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: could not create original Groom component template."));
			return nullptr;
		}

		GroomComponentTemplate->SetUseCards(true);
		GroomComponentTemplate->SetGroomAsset(GroomAsset);
		const bool bKeepSourceGroomVisibleForSimulation =
			HairCardsAsset->PreviewSettings.bKeepSourceGroomVisibleForSimulation
			&& HairCardsAsset->Parts.Num() > 0;
		const bool bShowSourceGroom =
			bKeepSourceGroomVisibleForSimulation
			||
			HairCardsAsset->PreviewSettings.bShowOriginalGroom;
		GroomComponentTemplate->SetVisibility(bShowSourceGroom, true);
		GroomComponentTemplate->SetHiddenInGame(!bShowSourceGroom);
		RootNode->AddChildNode(GroomComponentNode);
	}

	int32 TotalGuideCurveCount = 0;
	TArray<FString> MeshNames;
	MeshNames.Reserve(HairCardsAsset->Parts.Num());

	for (int32 CardIndex = 0; CardIndex < HairCardsAsset->Parts.Num(); ++CardIndex)
	{
		const FEnhancedHairCardsPart& Part = HairCardsAsset->Parts[CardIndex];
		const FString ComponentName = HairCardsAsset->Parts.Num() == 1
			? FString(TEXT("EnhancedHairCards"))
			: FString::Printf(TEXT("EnhancedHairCards_G%d_L%d_%02d"), Part.SourceGroupIndex, Part.SourceLODIndex, CardIndex);
		USCS_Node* ComponentNode = SCS->CreateNode(UEnhancedHairCardsComponent::StaticClass(), FName(*ComponentName));
		UEnhancedHairCardsComponent* ComponentTemplate = Cast<UEnhancedHairCardsComponent>(ComponentNode ? ComponentNode->ComponentTemplate : nullptr);
		if (!ComponentTemplate)
		{
			UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards export failed: could not create component template for %s."),
				*ComponentName);
			return nullptr;
		}

		ComponentTemplate->ApplyHairCardsPart(Part);
		ComponentTemplate->SourceGroomComponent = GroomComponentTemplate;
		ComponentTemplate->SetVisibility(HairCardsAsset->PreviewSettings.bShowEnhancedCards, true);
		ComponentTemplate->SetHiddenInGame(!HairCardsAsset->PreviewSettings.bShowEnhancedCards);
		ComponentTemplate->CardSettings.bRenderCardsMesh = HairCardsAsset->PreviewSettings.bRenderEnhancedCardsMesh;
		ComponentTemplate->CardSettings.GuideDebug.bDrawGuides = HairCardsAsset->PreviewSettings.bShowGuides;
		ApplyGroomComponentMaterial(GroomComponentTemplate, GroomAsset, Part);
		RootNode->AddChildNode(ComponentNode);

		TotalGuideCurveCount += Part.GuideCurves.Num();
		if (Part.SourceMesh)
		{
			MeshNames.Add(Part.SourceMesh->GetPathName());
		}
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FAssetRegistryModule::AssetCreated(Blueprint);
	Package->MarkPackageDirty();

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);

	UE_LOG(LogTemp, Display, TEXT("EnhancedHairCards export created %s from %s using %d card mesh(es), copied %d guide curve(s): %s."),
		*Blueprint->GetPathName(),
		*SourceAssetPath,
		SourcePartCount,
		TotalGuideCurveCount,
		*FString::Join(MeshNames, TEXT(", ")));

	return Blueprint;
}

bool UEnhancedHairCardsConverterLibrary::RebuildEnhancedHairCardsAssetFromGroom(
	UEnhancedHairCardsAsset* HairCardsAsset,
	int32 GroupIndex,
	int32 LODIndex)
{
	using namespace EnhancedHairCardsConverter;

	if (!HairCardsAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards rebuild failed: HairCardsAsset is null."));
		return false;
	}

	UGroomAsset* GroomAsset = HairCardsAsset->SourceGroom;
	if (!GroomAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards rebuild failed: %s has no SourceGroom."), *HairCardsAsset->GetPathName());
		return false;
	}

	const TArray<FSelectedCardDescription> SelectedCards = CollectCardDescriptions(GroomAsset, GroupIndex, LODIndex);
	if (SelectedCards.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards rebuild failed: %s has no imported Groom Cards meshes for Group=%d LOD=%d."),
			*GroomAsset->GetPathName(), GroupIndex, LODIndex);
		return false;
	}

	HairCardsAsset->Modify();
	HairCardsAsset->Parts.Reset(SelectedCards.Num());
	PopulateSimulationSettingsFromGroom(HairCardsAsset->SimulationSettings, GroomAsset, SelectedCards);

	TArray<FString> MeshNames;
	MeshNames.Reserve(SelectedCards.Num());
	for (const FSelectedCardDescription& SelectedCard : SelectedCards)
	{
		if (!SelectedCard.Desc)
		{
			continue;
		}

		FEnhancedHairCardsPart& Part = HairCardsAsset->Parts.AddDefaulted_GetRef();
		PopulatePartFromCardDescription(Part, GroomAsset, *SelectedCard.Desc, SelectedCard.SourceIndex);
		if (Part.SourceMesh)
		{
			MeshNames.Add(Part.SourceMesh->GetPathName());
		}
	}

	HairCardsAsset->MarkPackageDirty();

	UE_LOG(LogTemp, Display, TEXT("EnhancedHairCards rebuild updated %s from %s using %d card mesh(es), copied %d guide curve(s): %s."),
		*HairCardsAsset->GetPathName(),
		*GroomAsset->GetPathName(),
		HairCardsAsset->Parts.Num(),
		HairCardsAsset->GetTotalGuideCurveCount(),
		*FString::Join(MeshNames, TEXT(", ")));

	UE_LOG(LogTemp, Display, TEXT("EnhancedHairCards rebuild copied %d Groom Niagara simulation group(s)."),
		HairCardsAsset->SimulationSettings.Groups.Num());

	return true;
}

UBlueprint* UEnhancedHairCardsConverterLibrary::ConvertGroomToEnhancedHairCardsBlueprint(
	UGroomAsset* GroomAsset,
	const FString& OutputPackagePath,
	int32 GroupIndex,
	int32 LODIndex,
	bool bOverwriteExisting)
{
	using namespace EnhancedHairCardsConverter;

	if (!GroomAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: GroomAsset is null."));
		return nullptr;
	}

	const TArray<FSelectedCardDescription> SelectedCards = CollectCardDescriptions(GroomAsset, GroupIndex, LODIndex);
	if (SelectedCards.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("EnhancedHairCards conversion failed: %s has no imported Groom Cards meshes for Group=%d LOD=%d."),
			*GroomAsset->GetPathName(), GroupIndex, LODIndex);
		return nullptr;
	}

	UEnhancedHairCardsAsset* TransientAsset = NewObject<UEnhancedHairCardsAsset>(GetTransientPackage(), NAME_None, RF_Transient);
	TransientAsset->SourceGroom = GroomAsset;
	TransientAsset->PreviewSettings.bShowOriginalGroom = true;
	TransientAsset->PreviewSettings.bKeepSourceGroomVisibleForSimulation = true;
	TransientAsset->PreviewSettings.bShowEnhancedCards = true;
	TransientAsset->PreviewSettings.bShowGuides = true;
	TransientAsset->PreviewSettings.bRenderEnhancedCardsMesh = false;
	PopulateSimulationSettingsFromGroom(TransientAsset->SimulationSettings, GroomAsset, SelectedCards);
	TransientAsset->Parts.Reserve(SelectedCards.Num());

	for (const FSelectedCardDescription& SelectedCard : SelectedCards)
	{
		if (SelectedCard.Desc)
		{
			FEnhancedHairCardsPart& Part = TransientAsset->Parts.AddDefaulted_GetRef();
			PopulatePartFromCardDescription(Part, GroomAsset, *SelectedCard.Desc, SelectedCard.SourceIndex);
		}
	}

	const FString BlueprintPackagePath = ResolveGroomSiblingBlueprintPackagePath(GroomAsset, OutputPackagePath);
	if (BlueprintPackagePath != NormalizePackagePath(OutputPackagePath))
	{
		UE_LOG(LogTemp, Display, TEXT("EnhancedHairCards Blueprint output redirected to source Groom directory: %s"), *BlueprintPackagePath);
	}

	return ExportEnhancedHairCardsAssetToBlueprint(TransientAsset, BlueprintPackagePath, bOverwriteExisting);
}
