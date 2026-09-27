#pragma once

#include "CoreMinimal.h"
#include "CSMeshVisibilityCull.h"
#include "CSNaniteCut.h"
#include "GameFramework/Actor.h"
#include "CSNaniteCutHLODActor.generated.h"

class AStaticMeshActor;
class UBillboardComponent;
class UBoxComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * 截面 HLOD 的关卡内试验台：收集盒子里的静态网格，按各自的 Nanite 截面合并成一份 GPU 网格就地显示，
 * 可以在源物体与 HLOD 之间来回切换做对照。
 *
 * 不依赖 World Partition。WP 关卡正式出 HLOD 用 UCSNaniteCutHLODBuilder；两者的筛选与抽取都走
 * UCSNaniteCutOps（带 ExcludeTag 的 actor / 组件与 foliage 组件不进 HLOD），结果一致。
 *
 * HLOD 网格是 GPU 常驻数据、不存盘：关卡重新加载后要再 BuildHLOD 一次。要落地成资产用 BakeHLOD：重新分 UV、
 * 用源材质烘出 BaseColor / Normal / Roughness 三张贴图，存成开 Nanite 的静态网格（关卡同级 AutoResult/），
 * 生成一个 StaticMeshActor 挂在本 actor 下面（MeshBoolean 同款）。
 */
UCLASS(Blueprintable, ClassGroup = Rendering)
class COMPUTESHADERGENERATOR_API ACSNaniteCutHLODActor : public AActor
{
	GENERATED_BODY()

public:
	ACSNaniteCutHLODActor();

	/** 收集范围：包围盒中心落在盒子里的静态网格组件进 HLOD（地板这种与盒子相交的大件不算）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NaniteCut HLOD")
	TObjectPtr<UBoxComponent> GatherBox;

	/** 显示合并后的截面。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NaniteCut HLOD")
	TObjectPtr<UCSMeshRenderComponent> HLODMeshComponent;

#if WITH_EDITORONLY_DATA
	/** 收集盒包围盒最大角上的图标，盒子改尺寸 / actor 挪动旋转后跟着走（UpdateCornerBillboard）。 */
	UPROPERTY()
	TObjectPtr<UBillboardComponent> CornerBillboard;
#endif

	/** actor 或组件带这个标签就不进 HLOD。None = 不按标签排除。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD")
	FName ExcludeTag;

	/**
	 * 只收带这个标签的 actor / 组件（正向挑选，与 ExcludeTag 相反）。None = 不限制，盒子里的都收。
	 * 两个一起填就是先挑后排：带 PickTag 又带 ExcludeTag 的不进。建议名 NaniteCutHLOD_Pick
	 * （UCSNaniteCutOps::DefaultPickTag）。两个标签比较时都忽略空格 / 下划线 / 连字符与大小写。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD")
	FName PickTag;

	/** 切换距离（cm），等同 WP 的 MinVisibleDistance：CutError 按"在这个距离上误差 PixelError 个像素"反推。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD", meta = (ClampMin = "0"))
	float SwitchDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD", meta = (ClampMin = "0.1"))
	float PixelError = 1.0f;

	/** 参考视图（默认同引擎 HLOD Simplify 构建器：1920 宽、水平 FOV 90°）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD", meta = (ClampMin = "1"))
	float ReferenceScreenWidth = 1920.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD", meta = (ClampMin = "1", ClampMax = "179"))
	float ReferenceHorizontalFOV = 90.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD")
	ECSNaniteCutIncompletePolicy IncompletePolicy = ECSNaniteCutIncompletePolicy::UseAvailable;

	/** 不完整的源顺手请求全部流送页，过几帧再 BuildHLOD 就是完整的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD")
	bool bRequestMissingPages = true;

	/** 显示 HLOD 时把源物体藏起来。编辑器里是临时隐藏（不改属性、不进存盘），运行时是 HiddenInGame。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD")
	bool bHideSourcesWhenShowingHLOD = true;

	/**
	 * 剔除从外面看不见的三角形：被别的物体整个挡住的物体、互相穿插的内部面、封闭房间与山洞深处。
	 * 视点放在 SwitchDistance 上（HLOD 只在那以外显示）；只算 HLOD 自己的遮挡，地面等外部几何不参与。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD")
	bool bCullHidden = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD", meta = (EditCondition = "bCullHidden"))
	FCSMeshVisibilityCullOptions CullOptions;

	/** 上一次 BuildHLOD 的抽取结果，逐源状态在 Sources 里。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Result")
	FCSNaniteCutResult LastResult;

	/** 上一次交给抽取的源数（ISM 每个实例算一个；非 Nanite 的也算，逐源状态是 NotNanite、不进 HLOD 也不被藏）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Result")
	int32 LastNumSources = 0;

	/** 上一次因排除标签没进 HLOD 的组件数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Result")
	int32 LastNumExcluded = 0;

	/** 上一次用的世界空间 CutError（cm），每个源再除以自己的最小轴缩放。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Result")
	float LastWorldCutError = 0.0f;

	/** 上一次的可见性剔除（没开时全 0）。最终三角数 = TrianglesAfter（bApplied 时），否则 LastResult.WrittenTriangles。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Result")
	FCSMeshVisibilityCullResult LastCullResult;

	/** 烘焙贴图（BaseColor / Normal / Roughness 各一张）的边长。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD|Bake", meta = (ClampMin = "64", ClampMax = "8192"))
	int32 BakeTextureSize = 2048;

	/** 烘出的静态网格开 Nanite（顺带打开显式切线，法线贴图才对得上）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NaniteCut HLOD|Bake")
	bool bBakeNanite = true;

	/** 上一次烘出、挂在本 actor 下面的静态网格 actor（带 CSNaniteCutBakedHLOD 标签，重烘时替换）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NaniteCut HLOD|Bake")
	TObjectPtr<AStaticMeshActor> BakedActor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Bake")
	TObjectPtr<UStaticMesh> LastBakedMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Bake")
	int32 LastBakeTriangles = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "NaniteCut HLOD|Bake")
	float LastBakeSeconds = 0.0f;

	/** 收集 → 抽取 → 显示 HLOD（按 bHideSourcesWhenShowingHLOD 藏源）。阻塞，编辑器 / 离线用。 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "NaniteCut HLOD")
	void BuildHLOD();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "NaniteCut HLOD")
	void ShowHLOD();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "NaniteCut HLOD")
	void ShowSources();

	/** 放掉 HLOD 网格的显存并恢复显示源物体。 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "NaniteCut HLOD")
	void ClearHLOD();

	/**
	 * 重新抽取截面（材质表按源不去重）+ 可见性剔除 → 重新分 UV，用源材质把 BaseColor / Normal / Roughness
	 * 烘进 BakeTextureSize 的图集 → 存成开 Nanite 的静态网格与材质实例（关卡同级 AutoResult/，只标脏不写盘）→
	 * 生成 StaticMeshActor 挂在本 actor 下面并显示它。逐源烘：材质里用了 WorldPosition / ObjectPosition /
	 * ActorPosition / 包围盒 / 自定义图元数据 / 顶点色的，结果与原来一致。编辑器专用，关卡要先存过盘。
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "NaniteCut HLOD|Bake")
	void BakeHLOD();

	/** 盒子里要进 HLOD 的静态网格组件：跳过本 actor、不可见的、带排除标签的（计入 OutNumExcluded）。 */
	UFUNCTION(BlueprintCallable, Category = "NaniteCut HLOD")
	TArray<UStaticMeshComponent*> GatherSourceComponents(int32& OutNumExcluded) const;

	/** 上一次进 HLOD 的源 actor —— ShowHLOD / ShowSources 藏与放的就是它们。 */
	UFUNCTION(BlueprintPure, Category = "NaniteCut HLOD")
	TArray<AActor*> GetSourceActors() const;

	/**
	 * 诊断：HLOD 网格里重心落在各个世界盒子里的三角形数（整份回读一次，阻塞）。
	 * 验收可见性剔除用：框住一块外面看不见的区域（封闭房间、隧道深处），剔除后应当一个不剩。
	 */
	UFUNCTION(BlueprintCallable, Category = "NaniteCut HLOD")
	TArray<int32> CountHLODTrianglesInBoxes(const TArray<FBox>& WorldBoxes);

	UFUNCTION(BlueprintPure, Category = "NaniteCut HLOD")
	bool IsShowingHLOD() const { return bShowingHLOD; }

	UFUNCTION(BlueprintPure, Category = "NaniteCut HLOD")
	UCSMesh* GetHLODMesh() const { return HLODMesh; }

	//~ AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

private:
	void SetSourcesHidden(bool bHide);
	void SetBakedVisible(bool bVisible);

	/** 把角标挪到收集盒包围盒的最大角（世界空间）。非编辑器构建是空函数。 */
	void UpdateCornerBillboard();

	/** 收集 + 生成抽取源（BuildHLOD 与 BakeHLOD 共用），顺带更新 LastNumSources / LastNumExcluded / LastWorldCutError。 */
	void PrepareSources(TArray<UStaticMeshComponent*>& OutComponents, TArray<FCSNaniteCutSource>& OutSources,
		TArray<int32>& OutSourceComponentIndices, int32& OutNumFoliage, int32& OutNumTexCoordSets);

	FString GetBakeAssetFolder() const;
	FString GetBakeAssetBaseName();
	AStaticMeshActor* SpawnBakedActor(UStaticMesh* Mesh);

	/** 烘焙资产名里的稳定编号；复制出来的 actor 拿新的，不会互相覆盖资产。 */
	UPROPERTY(NonPIEDuplicateTransient)
	FGuid BakeAssetGuid;

	/** ShowHLOD 显示烘焙结果（有的话）还是 GPU 截面：BakeHLOD 之后为真，BuildHLOD 之后为假。 */
	bool bShowBaked = true;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> HLODMesh;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AActor>> SourceActors;

	bool bShowingHLOD = false;
	bool bSourcesHidden = false;
};
