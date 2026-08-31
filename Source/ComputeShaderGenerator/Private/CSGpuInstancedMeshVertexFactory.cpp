#include "CSGpuInstancedMeshVertexFactory.h"

#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "RHIStaticStates.h"
#include "ShaderParameterUtils.h"

IMPLEMENT_TYPE_LAYOUT(FCSGpuInstancedMeshVFShaderParameters);

bool FCSGpuInstancedMeshVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// Same gate as the engine's instanced factory: only materials that opted into instancing
	// pay for this extra permutation. Reusing the existing "Used with Instanced Static Meshes"
	// flag keeps the material setup identical to HISM's.
	return (Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes || Parameters.MaterialParameters.bIsSpecialEngineMaterial)
		&& FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

void FCSGpuInstancedMeshVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	// Force the manual-fetch instancing path unconditionally. FInstancedStaticMeshVertexFactory
	// picks USE_INSTANCE_CULLING (GPU Scene) whenever UseGPUScene() is true, which would take the
	// instance transforms out of our hands; VF_SUPPORTS_PRIMITIVE_SCENE_DATA is already 0 for this
	// factory because it is registered without SupportsPrimitiveIdStream.
	OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), TEXT("1"));

	// No dithered LOD cross-fade: LOD is picked per instance by the cull pass and the instance
	// simply moves to another draw, so there is no transition to dither. Defining it to 0 also
	// keeps NEEDS_PER_INSTANCE_PARAMS off the RandomID-driven code path.
	OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), TEXT("0"));
}

void FCSGpuInstancedMeshVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// The base builds the declaration and the LocalVF uniform buffer (manual vertex fetch).
	// AddPrimitiveIdStreamElement inside it is a no-op for this factory type, and the instance
	// attributes are not vertex streams here — they are fetched from InstanceVF.
	FLocalVertexFactory::InitRHI(RHICmdList);

	// 两件事同时挂在这条断言上，而两件事的失效都是无声的：
	//  ① 实例变换必须从 InstanceVF 的 SRV 里手取 —— 有了 primitive-id 流，
	//     LocalVertexFactory.ush 会改走 GPU Scene，而 GPU Scene 的实例数据只能从 CPU 灌，
	//     整个 cull-on-GPU 的前提就没了。
	//  ② UCSGpuInstancedMeshComponent 的 CastShadow = true 在 CSM 下能有影子（2026-08-30 实测），
	//     **全靠**这个索引是 -1：MeshPassProcessor.cpp:1304 的 bDoOverrideArgs 要求
	//     PrimitiveIdStreamIndex >= 0，一旦 >= 0，阴影通路就用 GPU-Scene 的 args 顶掉 cull pass
	//     写的那份，而我们在 GPU-Scene 里没有实例 ⇒ 实例数 0 ⇒ 阴影里画 0 个三角形。
	//     （VSM 另有一道更靠前的关卡，两条 gpumesh 路都过不去，见 CSGpuInstancedMeshComponent.cpp
	//      构造函数的注释；那一条不是这个 flag 能救的。）
	// 症状会是"主 pass 一切正常，只是影子没了"，没人会回到注册宏或基类的 InitRHI 上找原因。
	checkf(GetPrimitiveIdStreamIndex(GetFeatureLevel(), EVertexInputStreamType::Default) == INDEX_NONE,
		TEXT("FCSGpuInstancedMeshVertexFactory picked up a primitive-id stream. Manual-fetch instancing ")
		TEXT("and VSM shadow casting both depend on it being absent - see the EVertexFactoryFlags list ")
		TEXT("at the bottom of this file."));

	// 深度预通道的**入场券**，它没了的症状是「实例只在天空背景上看得见」—— 见头文件里
	// SupportsPositionOnlyStream / SupportsPositionAndNormalOnlyStream 那两条 override 的长注释。
	//
	// 断言的是引擎那条**两套口径**的契约：`FDepthPassMeshProcessor::ShouldRender` 按**实例**的
	// SupportsPositionOnlyStream() 选 position-only 着色器，而 `TDepthOnlyVS<true>` 只为声明了
	// `EVertexFactoryFlags::SupportsPositionOnly` 的**类型**编译。两者一旦不一致，
	// GetDepthPassShaders 取不到着色器，整个 batch 就被**静默地**踢出深度预通道 ——
	// 没有日志、没有断言，只有"后画的不透明物把它整片盖掉"这一个远端症状。
	//
	// 这一条会在删掉那两条 override（或反过来只给类型加 flag）时立刻报红。
	checkf(!SupportsPositionOnlyStream() && !SupportsPositionAndNormalOnlyStream(),
		TEXT("FCSGpuInstancedMeshVertexFactory advertises a position-only vertex stream. The depth ")
		TEXT("prepass then asks for TDepthOnlyVS<true>, which is only compiled for vertex factory ")
		TEXT("types flagged SupportsPositionOnly - this one is not - so the batch is dropped from ")
		TEXT("the prepass without a word and the instances stop occupying depth."));

	FInstancedStaticMeshVertexFactoryUniformShaderParameters UniformParameters;
	UniformParameters.VertexFetch_InstanceOriginBuffer = InstanceOriginSRV;
	UniformParameters.VertexFetch_InstanceTransformBuffer = InstanceTransformSRV;
	UniformParameters.VertexFetch_InstanceLightmapBuffer = InstanceLightmapSRV;
	UniformParameters.InstanceCustomDataBuffer = InstanceOriginSRV; // unused; must be non-null
	UniformParameters.NumCustomDataFloats = 0;
	InstanceUniformBuffer = TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters>::CreateUniformBufferImmediate(
		UniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
}

void FCSGpuInstancedMeshVertexFactory::ReleaseRHI()
{
	InstanceUniformBuffer.SafeRelease();
	FLocalVertexFactory::ReleaseRHI();
}

void FCSGpuInstancedMeshVFShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams) const
{
	const auto* InstancedVF = static_cast<const FCSGpuInstancedMeshVertexFactory*>(VertexFactory);

	// The batch never sets VertexFactoryUserData, so fall back to the factory's own LocalVF
	// uniform buffer (which manual vertex fetch needs).
	FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
	if (!VertexFactoryUniformBuffer) VertexFactoryUniformBuffer = InstancedVF->GetUniformBuffer();

	FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
		Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement,
		VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

	// Start of this batch's region in the visible-instance buffers; SV_InstanceID is added to it.
	ShaderBindings.Add(InstanceOffsetParameter, (uint32)FMath::Max(BatchElement.UserIndex, 0));

	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVertexFactoryUniformShaderParameters>(),
		InstancedVF->GetInstanceUniformBuffer());
	ShaderBindings.Add(VertexFetch_InstanceOriginBufferParameter, InstancedVF->GetInstanceOriginSRV());
	ShaderBindings.Add(VertexFetch_InstanceTransformBufferParameter, InstancedVF->GetInstanceTransformSRV());
	ShaderBindings.Add(VertexFetch_InstanceLightmapBufferParameter, InstancedVF->GetInstanceLightmapSRV());

	// Per-LOD pivot correction (HISM uses it for its LOD-range batches); nothing to offset here.
	ShaderBindings.Add(InstancingOffsetParameter, FVector4f(ForceInit));

	// InstancingFadeOutParams / view-Z compare live here; the leaf fills them per batch so that
	// PerInstanceParams resolves to "fully visible, no fade" (see FCSGpuInstancedMeshSceneProxy).
	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVFLooseUniformShaderParameters>(),
		BatchElement.LooseParametersUniformBuffer);
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedMeshVertexFactory, SF_Vertex, FCSGpuInstancedMeshVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedMeshVertexFactory, SF_Pixel, FCSGpuInstancedMeshVFShaderParameters);

// No SupportsPrimitiveIdStream: that flag is what would switch LocalVertexFactory.ush over to
// GPU-Scene instance data. No SupportsCachingMeshDrawCommands / SupportsStaticLighting either —
// this factory only ever draws through the dynamic-relevance path of FCSGpuMeshSceneProxy.
IMPLEMENT_VERTEX_FACTORY_TYPE(FCSGpuInstancedMeshVertexFactory, "/Engine/Private/LocalVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsManualVertexFetch
	| EVertexFactoryFlags::DoesNotSupportNullPixelShader
	| EVertexFactoryFlags::SupportsPSOPrecaching
);
