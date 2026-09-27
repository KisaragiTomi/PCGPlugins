#include "CSNaniteCutHLODActor.h"

#include "CSBoundsVisual.h"
#include "CSFixedScreenBillboardComponent.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSNaniteCutBake.h"

#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSNaniteCutHLODActor, Log, All);

namespace
{
// 烘焙结果 actor 的识别标签（MeshBoolean 同款）：重烘只销毁带它的挂接 actor，用户自己挂的不动。
const FName CSNaniteCutHLOD_BakedTag(TEXT("CSNaniteCutBakedHLOD"));
}

ACSNaniteCutHLODActor::ACSNaniteCutHLODActor()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	GatherBox = CreateDefaultSubobject<UBoxComponent>(TEXT("GatherBox"));
	GatherBox->SetupAttachment(Root);
	GatherBox->SetBoxExtent(FVector(4000.0, 4000.0, 1500.0));
	GatherBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GatherBox->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	GatherBox->SetGenerateOverlapEvents(false);
	GatherBox->SetHiddenInGame(true);
	GatherBox->ShapeColor = FColor(80, 200, 255);

	// 常驻数据是世界空间的，组件自己的摆放不影响几何（见 UCSMeshRenderComponent 的类注释）。
	HLODMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("HLODMesh"));
	HLODMeshComponent->SetupAttachment(Root);

#if WITH_EDITORONLY_DATA
	// 收集盒最大角上的图标。配法与 ACSHouseFeatureMarker 的 PickSprite 同形。
	CornerBillboard = CreateEditorOnlyDefaultSubobject<UCSFixedScreenBillboardComponent>(TEXT("CornerBillboard"));
	if (CornerBillboard)
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_TriggerBox.S_TriggerBox"));
		CornerBillboard->Sprite = SpriteTexture.Get();
		CornerBillboard->SetupAttachment(GatherBox);
		CornerBillboard->SetRelativeLocation(GatherBox->GetUnscaledBoxExtent());
		CornerBillboard->SetRelativeScale3D(FVector::OneVector);
		// 贴图尺寸不影响视口显示大小，专用代理始终按 48x48 屏幕像素绘制。
		CornerBillboard->bIsScreenSizeScaled = false;
		CornerBillboard->SetHiddenInGame(true);
		// 使用收集盒的 bounds 做视口裁剪；挂到零尺寸的 Root 会让角上的图标消失。
		CornerBillboard->bUseAttachParentBound = true;
	}
#endif

	ExcludeTag = UCSNaniteCutOps::DefaultExcludeTag();
}

TArray<UStaticMeshComponent*> ACSNaniteCutHLODActor::GatherSourceComponents(int32& OutNumExcluded) const
{
	OutNumExcluded = 0;
	TArray<UStaticMeshComponent*> Result;
	UWorld* World = GetWorld();
	if (!World || !GatherBox) return Result;

	// 按组件包围盒中心落在盒子里算"在这一片"：地板这种大件与盒子相交却不属于这群物体。
	const FBox Box = GatherBox->Bounds.GetBox();
	for (AActor* Actor : TActorRange<AActor>(World))
	{
		// 挂在本 actor 下面的（烘出来的 HLOD 网格）不能再被当成源 —— 它的中心就在盒子里。
		if (!Actor || Actor == this || Actor->GetAttachParentActor() == this || Actor->Tags.Contains(CSNaniteCutHLOD_BakedTag)) continue;
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsRegistered() || !Component->IsVisible() || !Component->GetStaticMesh()) continue;
			if (!Box.IsInsideOrOn(Component->Bounds.Origin)) continue;
			// 填了 PickTag 就只收带它的；没填等于不限制。没被挑中的不算"被排除"，不计入 OutNumExcluded。
			if (!UCSNaniteCutOps::IsPickedByTag(Component, PickTag)) continue;
			if (UCSNaniteCutOps::IsExcludedByTag(Component, ExcludeTag))
			{
				++OutNumExcluded;
				continue;
			}
			Result.Add(Component);
		}
	}
	return Result;
}

void ACSNaniteCutHLODActor::PrepareSources(TArray<UStaticMeshComponent*>& OutComponents, TArray<FCSNaniteCutSource>& OutSources,
	TArray<int32>& OutSourceComponentIndices, int32& OutNumFoliage, int32& OutNumTexCoordSets)
{
	int32 NumExcludedByGather = 0;
	OutComponents = GatherSourceComponents(NumExcludedByGather);
	LastWorldCutError = UCSNaniteCutOps::CutErrorForScreenError(SwitchDistance, PixelError, ReferenceScreenWidth, ReferenceHorizontalFOV);
	int32 NumExcluded = 0;
	UCSNaniteCutOps::MakeSourcesFromComponents(OutComponents, LastWorldCutError, ExcludeTag, OutSources, NumExcluded, OutNumFoliage,
		OutNumTexCoordSets, &OutSourceComponentIndices);
	LastNumExcluded = NumExcludedByGather + NumExcluded;
	LastNumSources = OutSources.Num();
}

void ACSNaniteCutHLODActor::BuildHLOD()
{
	// 重建前先把上一轮藏起来的源放出来：这一轮收集到的集合可能变了。
	SetSourcesHidden(false);

	TArray<UStaticMeshComponent*> Components;
	TArray<FCSNaniteCutSource> Sources;
	TArray<int32> SourceComponentIndices;
	int32 NumFoliage = 0;
	int32 NumTexCoordSets = 1;
	PrepareSources(Components, Sources, SourceComponentIndices, NumFoliage, NumTexCoordSets);

	if (!HLODMesh)
	{
		// Outer 是显示组件：网格归它，组件销毁时自己把显存还掉（UCSMeshRenderComponent 的约定）。
		HLODMesh = NewObject<UCSMesh>(HLODMeshComponent, TEXT("NaniteCutHLODMesh"), RF_Transient);
	}
	// Nanite 最多存 4 组 UV；布局由网格的拥有方声明，抽取算子只按它写。
	UCSMeshOps::EnsureTexCoordSets(HLODMesh, FMath::Clamp(NumTexCoordSets, 1, 4));

	FCSNaniteCutOptions Options;
	Options.bAppend = false;
	Options.IncompletePolicy = IncompletePolicy;
	Options.bRequestMissingPages = bRequestMissingPages;
	UCSNaniteCutOps::AppendNaniteCuts(HLODMesh, Sources, Options, LastResult);

	// HLOD 只在切换距离以外显示：从那一圈视点都看不见的三角形永远画不出来，直接删。
	LastCullResult = FCSMeshVisibilityCullResult();
	if (bCullHidden && LastResult.WrittenTriangles > 0)
		UCSMeshVisibilityOps::CullHiddenTriangles(HLODMesh, SwitchDistance, CullOptions, LastCullResult);
	const int32 NumTriangles = LastCullResult.bApplied ? LastCullResult.TrianglesAfter : LastResult.WrittenTriangles;

	// 不排 section 整个网格只按一个材质画；排了才是每种源材质一个 draw。剔除改了三角形布局，所以排在它后面。
	if (NumTriangles > 0) UCSMeshOps::BuildMaterialSections(HLODMesh);
	HLODMeshComponent->SetGpuMesh(HLODMesh);

	// 显示 HLOD 时只藏真进了 HLOD 的源（写出了三角形）。非 Nanite、被 SkipSource 跳过、失败的源在 HLOD 里
	// 没有替身，藏了就凭空消失 —— 它们的 actor 留着显示。一个 actor 只要有一个组件进了 HLOD 就整个藏，
	// 与 WP 一致（HLOD 替换的单位是 actor）。
	SourceActors.Reset();
	TArray<AActor*> NotInHLOD;
	int32 NumComplete = 0;
	int32 NumIncomplete = 0;
	int32 NumSkippedOther = 0;
	for (int32 SourceIndex = 0; SourceIndex < LastResult.Sources.Num(); ++SourceIndex)
	{
		const FCSNaniteCutSourceResult& SourceResult = LastResult.Sources[SourceIndex];
		if (SourceResult.Status == ECSNaniteCutStatus::Complete) ++NumComplete;
		else if (SourceResult.Status == ECSNaniteCutStatus::Incomplete) ++NumIncomplete;
		else ++NumSkippedOther;

		AActor* SourceOwner = SourceComponentIndices.IsValidIndex(SourceIndex) ? Components[SourceComponentIndices[SourceIndex]]->GetOwner() : nullptr;
		if (!SourceOwner) continue;
		const bool bInHLOD = SourceResult.Triangles > 0
			&& (SourceResult.Status == ECSNaniteCutStatus::Complete || SourceResult.Status == ECSNaniteCutStatus::Incomplete);
		if (bInHLOD) SourceActors.AddUnique(SourceOwner);
		else NotInHLOD.AddUnique(SourceOwner);
	}
	FString LeftVisible;
	int32 NumLeftVisible = 0;
	for (AActor* Actor : NotInHLOD)
	{
		if (SourceActors.Contains(Actor)) continue;
		if (NumLeftVisible++ < 4) LeftVisible += (LeftVisible.IsEmpty() ? TEXT(" (") : TEXT(", ")) + Actor->GetActorNameOrLabel();
	}
	if (!LeftVisible.IsEmpty()) LeftVisible += NumLeftVisible > 4 ? TEXT(", ...)") : TEXT(")");

	const FString CullText = LastCullResult.bApplied
		? FString::Printf(TEXT(", hidden-triangle cull -> %d (%d visible, %d views)"), LastCullResult.TrianglesAfter, LastCullResult.TrianglesVisible, LastCullResult.NumViews)
		: FString();
	UE_LOG(LogCSNaniteCutHLODActor, Log,
		TEXT("[CSNaniteCutHLODActor] %s: %d source(s), %d excluded by tag '%s', %d foliage; CutError %.2f cm at %.0f cm; complete %d, incomplete %d, other %d; %d triangles%s; hides %d actor(s), %d not in HLOD left visible%s."),
		*GetActorNameOrLabel(), LastNumSources, LastNumExcluded, *ExcludeTag.ToString(), NumFoliage,
		LastWorldCutError, SwitchDistance, NumComplete, NumIncomplete, NumSkippedOther, LastResult.WrittenTriangles, *CullText,
		SourceActors.Num(), NumLeftVisible, *LeftVisible);

	// 刚建的 GPU 截面是这一轮要看的东西；烘焙结果留着，BakeHLOD 之后才切回它。
	bShowBaked = false;
	ShowHLOD();
}

void ACSNaniteCutHLODActor::BakeHLOD()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World || World->IsGameWorld())
	{
		UE_LOG(LogCSNaniteCutHLODActor, Warning, TEXT("[CSNaniteCutHLODActor] %s: BakeHLOD is editor-only."), *GetActorNameOrLabel());
		return;
	}
	const FString AssetFolder = GetBakeAssetFolder();
	if (AssetFolder.IsEmpty())
	{
		UE_LOG(LogCSNaniteCutHLODActor, Warning, TEXT("[CSNaniteCutHLODActor] %s: save the level first — baked assets go next to it (AutoResult/)."), *GetActorNameOrLabel());
		return;
	}

	SetSourcesHidden(false);
	TArray<UStaticMeshComponent*> Components;
	TArray<FCSNaniteCutSource> Sources;
	TArray<int32> SourceComponentIndices;
	int32 NumFoliage = 0;
	int32 NumTexCoordSets = 1;
	PrepareSources(Components, Sources, SourceComponentIndices, NumFoliage, NumTexCoordSets);
	if (Sources.IsEmpty())
	{
		UE_LOG(LogCSNaniteCutHLODActor, Warning, TEXT("[CSNaniteCutHLODActor] %s: nothing to bake."), *GetActorNameOrLabel());
		return;
	}

	// 烘焙用自己的一份截面：材质表按源不去重，逐三角材质号才能反查出源 —— 每个源的材质要喂它自己的
	// 变换与图元数据（WorldPosition / ObjectPosition / ActorPosition / 包围盒 / 自定义图元数据）。
	UCSMesh* BakeMesh = NewObject<UCSMesh>(this, NAME_None, RF_Transient);
	UCSMeshOps::EnsureTexCoordSets(BakeMesh, FMath::Clamp(NumTexCoordSets, 1, 4));
	FCSNaniteCutOptions Options;
	Options.bAppend = false;
	Options.IncompletePolicy = IncompletePolicy;
	Options.bRequestMissingPages = bRequestMissingPages;
	Options.bUniqueMaterialsPerSource = true;
	FCSNaniteCutResult CutResult;
	UCSNaniteCutOps::AppendNaniteCuts(BakeMesh, Sources, Options, CutResult);
	FCSMeshVisibilityCullResult CullResult;
	if (bCullHidden && CutResult.WrittenTriangles > 0)
		UCSMeshVisibilityOps::CullHiddenTriangles(BakeMesh, SwitchDistance, CullOptions, CullResult);

	TArray<FCSNaniteCutBakeSource> BakeSources;
	BakeSources.SetNum(Sources.Num());
	for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
	{
		const FCSNaniteCutSource& Source = Sources[SourceIndex];
		FCSNaniteCutBakeSource& BakeSource = BakeSources[SourceIndex];
		BakeSource.LocalToWorld = Source.Transform;
		BakeSource.LocalBounds = Source.Mesh ? Source.Mesh->GetBounds() : FBoxSphereBounds(ForceInitToZero);
		BakeSource.WorldBounds = BakeSource.LocalBounds.TransformBy(Source.Transform);
		const UStaticMeshComponent* Component = SourceComponentIndices.IsValidIndex(SourceIndex) ? Components[SourceComponentIndices[SourceIndex]] : nullptr;
		if (!Component) continue;
		BakeSource.ActorPosition = Component->GetActorPositionForRenderer();
		BakeSource.CustomPrimitiveData = &Component->GetCustomPrimitiveData();
		// ISM 的每个实例按上面的实例包围盒；普通组件用它自己的（含 BoundsScale）。
		if (!Component->IsA<UInstancedStaticMeshComponent>()) BakeSource.WorldBounds = Component->Bounds;
	}

	FCSNaniteCutBakeParams Params;
	Params.OutputTransform = GetActorTransform();
	Params.AssetFolder = AssetFolder;
	Params.AssetBaseName = GetBakeAssetBaseName();
	Params.TextureSize = BakeTextureSize;
	Params.bEnableNanite = bBakeNanite;
	FCSNaniteCutBakeResult BakeResult;
	const bool bBaked = CutResult.WrittenTriangles > 0 && CSNaniteCutBake::Bake(BakeMesh, CutResult.MaterialSources, BakeSources, Params, BakeResult);
	BakeMesh->ReleaseDeferred();
	if (!bBaked || !BakeResult.StaticMesh)
	{
		UE_LOG(LogCSNaniteCutHLODActor, Error, TEXT("[CSNaniteCutHLODActor] %s: bake failed (cut %d triangles)."), *GetActorNameOrLabel(), CutResult.WrittenTriangles);
		return;
	}

	LastBakedMesh = BakeResult.StaticMesh;
	LastBakeTriangles = BakeResult.Triangles;
	LastBakeSeconds = float(BakeResult.Seconds);
	SpawnBakedActor(BakeResult.StaticMesh);
	UE_LOG(LogCSNaniteCutHLODActor, Log,
		TEXT("[CSNaniteCutHLODActor] %s: baked %d source(s) -> %s (%d triangles after cull %d -> %d, %d bake job(s), %dx%d, %.1f s)."),
		*GetActorNameOrLabel(), Sources.Num(), *BakeResult.StaticMesh->GetPathName(), BakeResult.Triangles,
		CutResult.WrittenTriangles, CullResult.bApplied ? CullResult.TrianglesAfter : CutResult.WrittenTriangles,
		BakeResult.BakeJobs, BakeTextureSize, BakeTextureSize, BakeResult.Seconds);

	// 源在这一轮收集到的集合：显示烘焙结果时藏的就是它们。
	SourceActors.Reset();
	for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
	{
		const UStaticMeshComponent* Component = SourceComponentIndices.IsValidIndex(SourceIndex) ? Components[SourceComponentIndices[SourceIndex]] : nullptr;
		const bool bInHLOD = CutResult.Sources.IsValidIndex(SourceIndex) && CutResult.Sources[SourceIndex].Triangles > 0;
		if (Component && bInHLOD) SourceActors.AddUnique(Component->GetOwner());
	}
	bShowBaked = true;
	ShowHLOD();
#endif
}

void ACSNaniteCutHLODActor::ShowHLOD()
{
	bShowingHLOD = true;
	const bool bUseBaked = bShowBaked && BakedActor;
	if (HLODMeshComponent) HLODMeshComponent->SetVisibility(!bUseBaked);
	SetBakedVisible(bUseBaked);
	SetSourcesHidden(bHideSourcesWhenShowingHLOD);
}

void ACSNaniteCutHLODActor::ShowSources()
{
	bShowingHLOD = false;
	if (HLODMeshComponent) HLODMeshComponent->SetVisibility(false);
	SetBakedVisible(false);
	SetSourcesHidden(false);
}

void ACSNaniteCutHLODActor::SetBakedVisible(bool bVisible)
{
	if (!BakedActor) return;
	UWorld* World = GetWorld();
#if WITH_EDITOR
	if (!World || !World->IsGameWorld())
	{
		BakedActor->SetIsTemporarilyHiddenInEditor(!bVisible);
		return;
	}
#endif
	BakedActor->SetActorHiddenInGame(!bVisible);
}

FString ACSNaniteCutHLODActor::GetBakeAssetFolder() const
{
	// 与 MeshBoolean 一致：关卡所在目录下的 AutoResult/。未存盘的关卡（/Temp）没有内容路径。
	const ULevel* Level = GetLevel();
	const UPackage* LevelPackage = Level ? Level->GetPackage() : nullptr;
	const FString LevelPackageName = LevelPackage ? LevelPackage->GetName() : FString();
	if (!FPackageName::IsValidLongPackageName(LevelPackageName) || LevelPackageName.StartsWith(TEXT("/Temp/"))) return FString();
	return FPackageName::GetLongPackagePath(LevelPackageName) / TEXT("AutoResult");
}

FString ACSNaniteCutHLODActor::GetBakeAssetBaseName()
{
	// 同一个 actor 反复烘始终写同一组资产；复制出来的 actor 拿新编号（NonPIEDuplicateTransient），不会互相覆盖。
	if (!BakeAssetGuid.IsValid())
	{
		Modify();
		BakeAssetGuid = FGuid::NewGuid();
	}
	FString Label = GetActorNameOrLabel();
	for (TCHAR& Char : Label)
		if (!FChar::IsAlnum(Char) && Char != TEXT('_')) Char = TEXT('_');
	return FString::Printf(TEXT("HLOD_%s_%s"), *Label, *BakeAssetGuid.ToString(EGuidFormats::Digits).Left(8));
}

AStaticMeshActor* ACSNaniteCutHLODActor::SpawnBakedActor(UStaticMesh* Mesh)
{
	UWorld* World = GetWorld();
	if (!World || !Mesh) return nullptr;

	// 先清掉上一次的烘焙结果：只认带标签的挂接 actor，用户自己挂的不动（MeshBoolean 同款）。
	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	if (BakedActor) AttachedActors.AddUnique(BakedActor);
	for (AActor* Attached : AttachedActors)
	{
		if (!Attached || !Attached->Tags.Contains(CSNaniteCutHLOD_BakedTag)) continue;
		Attached->Modify();
		Attached->Destroy();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.OverrideLevel = GetLevel();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	SpawnParams.InitialActorLabel = GetActorNameOrLabel() + TEXT("_Baked");
#endif
	// 生成在本 actor 的变换上：网格按本 actor 的局部空间烘焙，对齐后才落在原位。
	AStaticMeshActor* Spawned = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), GetActorTransform(), SpawnParams);
	if (!Spawned) return nullptr;
	Spawned->Modify();
	Spawned->Tags.AddUnique(CSNaniteCutHLOD_BakedTag);
	if (UStaticMeshComponent* MeshComponent = Spawned->GetStaticMeshComponent())
	{
		MeshComponent->SetMobility(EComponentMobility::Movable);
		MeshComponent->SetStaticMesh(Mesh);
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->UpdateBounds();
		MeshComponent->MarkRenderStateDirty();
	}
	Spawned->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
#if WITH_EDITOR
	Spawned->SetActorLabel(GetActorNameOrLabel() + TEXT("_Baked"));
	Spawned->SetFolderPath(GetFolderPath());
#endif
	Spawned->MarkPackageDirty();
	Modify();
	BakedActor = Spawned;
	MarkPackageDirty();
	return Spawned;
}

void ACSNaniteCutHLODActor::ClearHLOD()
{
	ShowSources();
	if (HLODMeshComponent) HLODMeshComponent->SetGpuMesh(nullptr);
	if (HLODMesh) HLODMesh->ReleaseDeferred();
	SourceActors.Reset();
	LastResult = FCSNaniteCutResult();
	LastCullResult = FCSMeshVisibilityCullResult();
	LastNumSources = 0;
	LastNumExcluded = 0;
}

TArray<int32> ACSNaniteCutHLODActor::CountHLODTrianglesInBoxes(const TArray<FBox>& WorldBoxes)
{
	TArray<int32> Counts;
	Counts.Init(0, WorldBoxes.Num());
	FCSGpuMeshCPUData MeshData;
	if (!HLODMesh || WorldBoxes.IsEmpty() || !HLODMesh->ReadbackMeshSync(MeshData)) return Counts;

	const int32 NumPositions = MeshData.Positions.Num();
	for (int32 Corner = 0; Corner + 2 < MeshData.Indices.Num(); Corner += 3)
	{
		const int32 I0 = int32(MeshData.Indices[Corner]);
		const int32 I1 = int32(MeshData.Indices[Corner + 1]);
		const int32 I2 = int32(MeshData.Indices[Corner + 2]);
		if (I0 >= NumPositions || I1 >= NumPositions || I2 >= NumPositions) continue;
		// 常驻数据是世界空间的。
		const FVector Centroid = FVector(MeshData.Positions[I0] + MeshData.Positions[I1] + MeshData.Positions[I2]) / 3.0;
		for (int32 Box = 0; Box < WorldBoxes.Num(); ++Box)
			if (WorldBoxes[Box].IsInsideOrOn(Centroid)) ++Counts[Box];
	}
	return Counts;
}

TArray<AActor*> ACSNaniteCutHLODActor::GetSourceActors() const
{
	TArray<AActor*> Result;
	for (const TWeakObjectPtr<AActor>& Weak : SourceActors)
		if (AActor* Actor = Weak.Get()) Result.Add(Actor);
	return Result;
}

void ACSNaniteCutHLODActor::SetSourcesHidden(bool bHide)
{
	if (!bHide && !bSourcesHidden) return;
	UWorld* World = GetWorld();
	const bool bGameWorld = World && World->IsGameWorld();
	for (const TWeakObjectPtr<AActor>& Weak : SourceActors)
	{
		AActor* Actor = Weak.Get();
		if (!Actor) continue;
#if WITH_EDITOR
		// 编辑器里用"临时隐藏"（视口里按 H 那一种）：不改 actor 的属性，不进存盘。
		if (!bGameWorld)
		{
			Actor->SetIsTemporarilyHiddenInEditor(bHide);
			continue;
		}
#endif
		Actor->SetActorHiddenInGame(bHide);
	}
	bSourcesHidden = bHide;
}

void ACSNaniteCutHLODActor::UpdateCornerBillboard()
{
#if WITH_EDITORONLY_DATA
	if (!CornerBillboard || !GatherBox) return;
	// 角标以 GatherBox 为父组件，局部位置就是盒子的最大角；盒子或 actor 旋转时都会跟着转。
	CSBoundsVisual::PlaceCornerBillboard(CornerBillboard, GatherBox->GetUnscaledBoxExtent());
#endif
}

void ACSNaniteCutHLODActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// 改盒子尺寸、挪动 / 旋转 actor、撤销都会重跑构造脚本（蓝图实例拖动时逐帧跑）。
	UpdateCornerBillboard();
}

void ACSNaniteCutHLODActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// 关卡加载不跑构造脚本，这里补一次。
	UpdateCornerBillboard();
	// 关卡刚加载时源是显示着的（临时隐藏不存盘），烘焙结果也得先藏起来，不然两份叠在一起。
	// 此时烘焙 actor 可能还没注册 —— 临时隐藏只是一个标志，它注册时会照这个标志来。
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!bShowingHLOD && BakedActor && World && !World->IsGameWorld()) BakedActor->SetIsTemporarilyHiddenInEditor(true);
#endif
}

void ACSNaniteCutHLODActor::BeginPlay()
{
	Super::BeginPlay();
	// 运行时同理：默认显示源；要看 HLOD 调 ShowHLOD。
	if (BakedActor) BakedActor->SetActorHiddenInGame(true);
}

void ACSNaniteCutHLODActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SetSourcesHidden(false);
	Super::EndPlay(EndPlayReason);
}

void ACSNaniteCutHLODActor::Destroyed()
{
	// 编辑器里删掉本 actor 时，被临时隐藏的源不能一直藏着；烘焙结果成了普通 actor，也放出来。
	SetSourcesHidden(false);
	SetBakedVisible(true);
	Super::Destroyed();
}
