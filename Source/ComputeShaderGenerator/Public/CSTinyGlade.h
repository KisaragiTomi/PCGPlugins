#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CSTinyGlade.generated.h"

class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
struct FCSGpuMeshCPUData;

/**
 * Tiny Glade 交互对象的共同基类（TinyGladeHouse_Plan.md）：地面 / 房屋 / 窗户 / 链接墙
 * 都是"CPU 权威数据 → FCSGpuMeshCPUData 快照 → UCSMesh"的同构 actor，这里收敛三件事：
 *
 *  1. 网格底座：一个 UCSMeshRenderComponent + transient UCSMesh + 快照上传/材质绑定管道
 *     （UploadTinyGladeSnapshot）。GPU 数据不随关卡存盘，加载后由派生类从各自权威数据重建。
 *  2. 声明式重求值入口 ReevaluateSite()：任何唤醒（OnGroundChanged 直推、subsystem 变换
 *     快扫、窗户注册表直通）都汇到这一个虚函数——派生类在里面从权威源重导出目标状态、
 *     哈希比对、变了才重建。基类默认空实现：地面是权威数据源，不消费世界变化。
 *  3. UCSHouseSubsystem 的统一注册类型（P2 落地时在此接线注册/注销与变换哈希）。
 *
 * gpumesh 全线 NoCollision——派生类的拾取一律解析实现（镜像高度场 / 参数化 OBB），
 * 不给网格加碰撞体。
 */
UCLASS(Abstract)
class COMPUTESHADERGENERATOR_API ACSTinyGlade : public AActor
{
	GENERATED_BODY()

public:
	ACSTinyGlade();

	/**
	 * 声明式重求值：从权威源重导出目标状态 → 哈希比对 → 变了才重建。
	 * 必须幂等——直推架构下它会被高频、重复、甚至自发地调用，重复调用必须收敛为零成本。
	 * CallInEditor 按钮兼作调试入口：怀疑状态不同步时手动点一次即对齐。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS TinyGlade")
	virtual void ReevaluateSite() {}

	UFUNCTION(BlueprintPure, Category = "CS TinyGlade")
	UCSMesh* GetTinyGladeMesh() const { return TinyGladeMesh; }

	UFUNCTION(BlueprintPure, Category = "CS TinyGlade")
	UCSMeshRenderComponent* GetTinyGladeMeshComponent() const { return TinyGladeMeshComponent; }

protected:
	/**
	 * 快照 → GPU 的统一管道：按需建 UCSMesh、绑材质表、CopyFromMeshSnapshot 上传、
	 * SetGpuMesh 绑定。槽 i = Materials[i]（存盘资产的材质来源），组件 MeshMaterial =
	 * 槽 0（无 section 表时那一个绘制批次实际用的材质，分工同 ACSPointBrushActor 箭头路径）。
	 * 多材质派生类（房体多槽）上传后自行补 BuildMaterialSections。目前只服务基类主网格；
	 * 房屋的柱子组件（第二网格）落地时再参数化成"目标组件 + 目标 mesh"的可复用形式。
	 */
	bool UploadTinyGladeSnapshot(const FCSGpuMeshCPUData& Snapshot, const TArray<TObjectPtr<UMaterialInterface>>& Materials);

	/**
	 * 只重绑材质表，一个三角形都不重传 —— UCSMesh::SetMaterial / NotifyMaterialsChanged 明写
	 * "Deliberately does not touch Generation"，渲染组件的 HandleMeshChanged 收到后只重解
	 * batch 材质。这是计划 D14 那条纪律的执行面：**纯外观量绝不进 desc 哈希**，改材质走这条，
	 * 不走 UploadTinyGladeSnapshot —— 后者被几何哈希守卫，改材质会被整个吞掉（症状是细节面板
	 * 换材质画面零变化，必须手点重建）。尚未上传过（TinyGladeMesh 为空）时是 no-op，首次上传
	 * 自会带上材质。
	 */
	void BindTinyGladeMaterials(const TArray<TObjectPtr<UMaterialInterface>>& Materials);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS TinyGlade", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSMeshRenderComponent> TinyGladeMeshComponent;

	/** GPU 投影。Transient：属性只为压住 GC，不序列化网格数据。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> TinyGladeMesh;
};
