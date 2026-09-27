# UETest574_2 项目文档

本目录集中存放项目级 Markdown 文档。文件名统一使用小写英文 kebab-case；插件或模块内部的 README、设计说明与第三方文档保留在各自目录中。

## 文档索引

- [项目优化方案：PCGPlugins 去冗余清理计划](../项目优化方案去冗余清理的计划.md)（根目录，PCGPlugins 专版：旧审计条目复核总账与执行调度）
- [项目清理计划](project-cleanup-plan.md)（逐条代码级审计证据，上述计划的依据）
- [GPU 三角形 Buffer 统一评估](gpu-triangle-buffer-unification.md)
- [Motion Matching 与动画预测插件移植说明](motion-matching-plugin-porting-guide.md)
- [CSSceneDirty3D：场景级三维 Dirty 区域系统](cs-scene-dirty-3d-design.md)（CS 生成系统局部更新设计稿，配图 [cs-scene-dirty-3d-architecture.svg](cs-scene-dirty-3d-architecture.svg)）
- [MeshBoolean 焊接流程的失败情况](meshboolean-weld-failure-cases.md)（Houdini 复现截图，构建脚本 [HoudiniMeshBooleanWeldFailureCases.py](../Scripts/HoudiniMeshBooleanWeldFailureCases.py)）
- [Tiny Glade 复刻：文档索引](../Plugins/PCGPlugins/Docs/TinyGlade/index.md)（`Plugins/PCGPlugins/Docs/TinyGlade/`：gpumesh 地面 + 顶点色笔刷 + 直推式房屋重求值，v1 不用 CSSceneDirty3D。设计裁决在 [TinyGladeHouse_Plan.md](../Plugins/PCGPlugins/Docs/TinyGlade/TinyGladeHouse_Plan.md)，进度在 [模块对照与进度合卷](../Plugins/PCGPlugins/Docs/TinyGlade/TinyGlade_模块对照与进度.md)）
