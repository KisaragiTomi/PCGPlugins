# FluxSurface Foam 参数说明

本文基于 `ShaderTest/FluxSurface.usf` 当前生成代码整理，主要对应材质实例里 `Foam` 分组的参数。截图中的 `UseFoam` 和 `UseFoamDistortion` 在这个 `.usf` 里已经被编译进当前排列，所以 shader 中看不到运行时 `if` 分支；也就是说，这份 `.usf` 展示的是“泡沫开启、泡沫扰动开启”后的实际计算路径。

## Shader 中泡沫的大致流程

1. 读取浅水模拟贴图 `Texture2D_0`，其中 `RG` 被当作速度方向/速度强度使用，并通过 `VelScale` 放大。
2. 根据世界坐标、速度、时间和 `FoamUV*` 参数生成泡沫 UV。
3. 如果启用 `UseFoamDistortion`，先采样一张扰动贴图 `Texture2D_3`，用 `FoamDistortion*` 参数扭曲泡沫 UV。
4. 使用 `FoamNormalSoftHeightMap` 对应的贴图，也就是 shader 里的 `Texture2D_4`，做三次错相采样并混合。这个贴图在代码中按 `NSH` 用法拆分：
   - `R/G`：泡沫法线 XY。
   - `B`：软泡沫/颜色细节权重。
   - `A`：泡沫高度/硬泡沫遮罩。
5. 用场景深度差、贴图 alpha、速度、表面倾斜和硬度参数合成泡沫可见遮罩。
6. 把泡沫结果写入 `Normal`、`Opacity`、`BaseColor`、`EmissiveColor`、`Specular` 和 `Roughness`。

## 截图参数说明

### UseFoam

泡沫总开关。当前 `.usf` 是开启后的编译结果，所以泡沫采样、遮罩、颜色、法线等逻辑都已经展开在代码里。如果在材质里关闭它，通常会走另一个静态编译排列，泡沫相关采样和混合会被剔除。

### FoamNormalSoftHeightMap

泡沫主贴图。截图里是 `T_Seafoam_03_NSH`，shader 中对应 `Texture2D_4`。它不是普通单通道遮罩，而是一张复合用途贴图：

- `RG` 控制泡沫给水面法线带来的细碎扰动。
- `B` 参与软泡沫的颜色/透明度贡献。
- `A` 是硬泡沫的高度/形状遮罩。

### FoamUVScale

泡沫主贴图的世界空间缩放。代码里最终类似 `(WorldXY + Distortion) * FoamUVScale`。数值越大，泡沫纹理越密、重复越多；数值越小，泡沫纹理越大、铺得更开。截图值 `0.0015` 属于很大的世界空间纹理尺度。

### FoamUVSpeed

泡沫三路错相采样的时间速度。代码中用 `frac(GameTime * FoamUVSpeed)` 生成三个相位，再用余弦权重平滑混合。数值越大，泡沫图案的循环/翻动越快。

### FoamUVVelocity

泡沫跟随水流速度移动的强度。代码中它会乘以浅水模拟贴图的速度 `RG`，并且在预处理里带了一个负号：`FoamUVVelocity * -1`。数值越大，泡沫越明显地被速度场推走；数值为 `0` 时，泡沫基本只按世界坐标和时间相位变化，不再随水流推移。

### FoamUVAdvectionOffset

速度推流相位的偏移。代码把它加到三个时间相位上，用来改变泡沫被速度场带动时的相位关系。调它主要会改变泡沫图案在流向上的错位和起始位置；负值会让推流相位向反方向偏移。

### FoamUVRandomization

三路泡沫采样中的第二、第三路 UV 偏移强度。shader 使用两个固定偏移值：

- `float2(0.124905, 0.836666) * FoamUVRandomization`
- `float2(0.500952, 0.887143) * FoamUVRandomization`

数值越大，三次采样越错开，重复感越弱；数值为 `0` 时，多个采样层会更容易重叠，图案更规整也更容易看出平铺。

### UseFoamDistortion

泡沫 UV 扰动开关。当前 `.usf` 是开启后的结果，会采样 `Texture2D_3` 的 `RG` 通道，把它从 `0.5` 居中后乘以 `FoamDistortionIntensity`，再加到泡沫的世界坐标 UV 上。关闭时，泡沫主贴图会少一层流动扭曲，图案更规则。

### FoamDistortionScale

扰动贴图自身的 UV 缩放。代码中是 `ActorLocalXY * FoamDistortionScale + GameTime * FoamDistortionSpeed`。数值越大，扰动噪声越密、扭曲变化越碎；数值越小，扰动块面越大。

### FoamDistortionSpeed

扰动贴图随时间滚动的速度。数值越大，泡沫边缘/纹理扭曲游动越快；数值为 `0` 时，扰动图案停在局部空间里。

### FoamDistortionIntensity

扰动强度。代码中近似为 `(DistortionRG - 0.5) * FoamDistortionIntensity`。数值越大，泡沫 UV 被扭曲得越厉害，边缘更破碎；过高会导致图案拉扯、跳动或显得脏。

### FoamShallowOffset

浅水/交界泡沫遮罩的偏移。代码先取泡沫贴图 alpha，再加上 `FoamShallowOffset`，然后乘以场景深度差。提高它会让浅水区域更容易出现泡沫，也会让原本较弱的贴图 alpha 更容易被激活。

### FoamShallowScale

浅水深度差的放大倍率。shader 会取 `SceneDepthWithoutWater - PixelDepth`，再乘以视角修正和 `FoamShallowScale`。数值越大，浅水、水体与几何交界处的泡沫越容易出现，范围也更明显；数值太大时，泡沫可能在较深区域也被拉出来。

### FoamHardnessWidth

硬泡沫阈值过渡宽度。shader 里预处理为 `FoamHardnessWidth - 1`，再与泡沫遮罩组合后用于从泡沫贴图 `A` 中扣出硬泡沫形状。它主要控制硬泡沫边缘从无到有的区间：数值变化会影响硬泡沫出现范围和边缘宽窄。

### FoamHardnessIntensity

硬泡沫阈值强度。shader 里参与 `(FoamHardnessIntensity + 1) / (FoamHardnessWidth - 1)`，用于控制硬泡沫遮罩扣减贴图 alpha 的力度。数值越高，越倾向于压掉不够强的泡沫，让硬泡沫边缘更挑剔、更硬；数值较低时，泡沫更容易保留下来。

### FoamSoftBase

软泡沫的基础强度。代码里它和速度项相加：`FoamSoftBase + length(Velocity) * FoamSoftVelocity * 0.001538`，再被 `FoamSoftMax` 限制。即使水流速度不高，它也能给软泡沫一个基础可见度。

### FoamSoftVelocity

速度对软泡沫的贡献。数值越大，水流越快的地方软泡沫越明显。它不会直接使用原始值，而是先乘以 `0.001538`，所以这是一个比较温和的速度增益。

### FoamSoftMax

软泡沫强度上限。代码中使用 `min(FoamSoftBase + VelocityTerm, FoamSoftMax)`。它限制软泡沫不会因为速度太大而无限增亮/增厚。

### FoamSoftIntensity

软泡沫颜色强度。它会乘以基础泡沫能量遮罩，再用来把 `FoamColorBase.rgb` 混入软泡沫颜色。截图里是 `0.0`，表示软泡沫颜色贡献被关掉；但 `FoamSoftBase/FoamSoftVelocity/FoamSoftMax` 仍可能参与透明度/遮罩计算。

### FoamNormalScale

泡沫法线强度。shader 从 `FoamNormalSoftHeightMap` 的 `RG` 还原泡沫法线方向，再乘以泡沫可见遮罩和 `FoamNormalScale` 混入水面原始法线。数值越大，泡沫区域的细节凹凸越强；过高会让高光破碎或显得发皱。

### FoamColorBase

泡沫基础颜色和基础透明度。`RGB` 是泡沫颜色，`A` 在 shader 里被 `saturate(FoamColorBase.a)` 后乘到泡沫遮罩上，所以它相当于泡沫整体可见度/覆盖强度。`A` 为 `0` 时，泡沫颜色和透明度基本会被压掉。

### FoamColorDetail

泡沫颜色细节强度。shader 会把泡沫贴图重建出的法线 `Z` 与贴图 `A` 相加，再乘以 `FoamColorDetail`，用于给 `FoamColorBase.rgb` 做明暗细节。数值越大，泡沫颜色越受贴图细节影响，亮暗变化更明显。

### FoamColorAlpha

泡沫颜色细节到纯色的混合值。代码近似为 `lerp(DetailFactor, 1, FoamColorAlpha)`：

- `0`：完全使用 `FoamColorDetail` 产生的贴图明暗细节。
- `1`：细节因子变为 `1`，泡沫更接近纯 `FoamColorBase.rgb`。

注意它不是 `FoamColorBase` 的透明度；真正影响泡沫可见度的是 `FoamColorBase.a`。

### FoamEmissive

泡沫自发光颜色和阈值。`RGB` 是自发光颜色/强度，`A` 在 shader 中从泡沫透明度遮罩里扣掉：`(FoamOpacityMask - FoamEmissive.a) * FoamEmissive.rgb`。因此：

- 提高 `RGB` 会让泡沫更发光。
- 提高 `A` 会抬高自发光门槛，只让更强的泡沫区域发光。

### FoamDitheringAlpha / FoamDitheringOffset / FoamDitheringScale

这三个参数在截图中存在，但在当前 `FluxSurface.usf` 的 `PreshaderBuffer` 参数映射和泡沫主体代码中没有出现。当前编译结果只包含 UE 通用的 dither/opacity mask 辅助函数，以及材质的普通 `OpacityMask` 计算，没有引用 `FoamDithering*`。

因此，按这份 `.usf` 判断，它们在当前材质编译排列里没有实际效果。它们可能是材质函数或材质实例中遗留的参数，或者只在另一个静态开关/质量等级的编译排列中生效。

## Shader 中还有但截图未显示的 Foam 参数

### FoamNormalBlend

shader 中存在，位于 `PreshaderBuffer[7].x`。它先乘到硬泡沫遮罩上，再限制泡沫法线和硬泡沫颜色的混合强度。可以理解为“泡沫法线/硬泡沫显著程度”的总混合开关。

### FoamSpecular

shader 中存在，位于 `PreshaderBuffer[13].y`。最终 `Specular` 使用 `lerp(WaterSpecularResult, FoamSpecular, HardFoamMask)`，所以泡沫越明显，镜面参数越趋向 `FoamSpecular`。

### FoamRoughness

shader 中存在，位于 `PreshaderBuffer[13].z`。最终 `Roughness` 使用 `lerp(WaterRoughnessMin, FoamRoughness, HardFoamMask)`，所以泡沫越明显，粗糙度越趋向 `FoamRoughness`。

## 快速调参建议

- 想让泡沫出现更多：优先提高 `FoamShallowScale`、`FoamShallowOffset`、`FoamColorBase.a`，或适当降低硬泡沫阈值感。
- 想让泡沫跟水流走：提高 `FoamUVVelocity`，必要时配合 `FoamUVAdvectionOffset` 调相位。
- 想减少平铺感：提高 `FoamUVRandomization`，并用适量 `FoamDistortionIntensity`。
- 想让泡沫更碎更活：提高 `FoamDistortionScale` 或 `FoamDistortionIntensity`，但不要过高。
- 想让泡沫更柔：提高 `FoamSoftBase` 或 `FoamSoftVelocity`，用 `FoamSoftMax` 限制上限。
- 想让泡沫更像贴在水面的白色纹理：提高 `FoamColorAlpha`，降低 `FoamColorDetail` 的影响。
- 想让泡沫更有凹凸：提高 `FoamNormalScale`，如果出现高光噪点就回调。
