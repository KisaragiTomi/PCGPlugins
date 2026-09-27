#include "CSNaniteCutBake.h"

#if WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSStaticMeshAssetSink.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Engine.h"
#include "Engine/MaterialMerging.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "IMaterialBakingModule.h"
#include "MaterialBakingStructures.h"
#include "MaterialUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialDomain.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSNaniteCutBake, Log, All);

namespace
{
static const TCHAR* CSNaniteCutBake_LogPrefix = TEXT("[CSNaniteCutBake]");

/** 回读把打包切线的 W（副法线符号）丢了 —— 直接把切线流原样读成 uint，取法线那个字的最高字节。 */
void CSNaniteCutBake_ReadBinormalSigns(UCSMesh* Mesh, int32 NumVertices, TArray<float>& OutSigns)
{
	OutSigns.Init(1.0f, NumVertices);
	const FCSMeshResident* Resident = Mesh->GetResidentPtr();
	if (!Resident) return;
	for (const FCSMeshResident::FStream& Stream : Resident->Streams)
	{
		if (Stream.Desc.Role != ECSGpuStreamRole::TangentBasis || !Stream.Pooled.IsValid()) continue;
		TArray<uint32> Words;
		if (!CSMeshReadback::ReadUintBufferSync(Stream.Pooled, uint32(NumVertices) * 2u, ECSGpuStreamRole::TangentBasis, Words)
			|| Words.Num() < NumVertices * 2)
		{
			UE_LOG(LogCSNaniteCutBake, Warning, TEXT("%s Tangent stream readback failed; binormal signs default to +1."), CSNaniteCutBake_LogPrefix);
			return;
		}
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			OutSigns[Vertex] = int8((Words[Vertex * 2 + 1] >> 24) & 0xffu) < 0 ? -1.0f : 1.0f;
		return;
	}
}

/** 纹素中心 (x + 0.5, y + 0.5) 落在三角形里（含边）才算覆盖。A/B/C 以纹素为单位。 */
template <typename FEmit>
void CSNaniteCutBake_Raster(const FVector2D& A, const FVector2D& B, const FVector2D& C, int32 Size, FEmit&& Emit)
{
	auto Edge = [](const FVector2D& P0, const FVector2D& P1, const FVector2D& P)
	{
		return (P1.X - P0.X) * (P.Y - P0.Y) - (P1.Y - P0.Y) * (P.X - P0.X);
	};
	double Area = Edge(A, B, C);
	if (FMath::Abs(Area) < 1e-12) return;
	const FVector2D& V0 = A;
	const FVector2D& V1 = Area > 0.0 ? B : C;
	const FVector2D& V2 = Area > 0.0 ? C : B;
	const int32 X0 = FMath::Max(FMath::CeilToInt32(FMath::Min3(A.X, B.X, C.X) - 0.5), 0);
	const int32 X1 = FMath::Min(FMath::FloorToInt32(FMath::Max3(A.X, B.X, C.X) - 0.5), Size - 1);
	const int32 Y0 = FMath::Max(FMath::CeilToInt32(FMath::Min3(A.Y, B.Y, C.Y) - 0.5), 0);
	const int32 Y1 = FMath::Min(FMath::FloorToInt32(FMath::Max3(A.Y, B.Y, C.Y) - 0.5), Size - 1);
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			const FVector2D P(X + 0.5, Y + 0.5);
			if (Edge(V1, V2, P) >= 0.0 && Edge(V2, V0, P) >= 0.0 && Edge(V0, V1, P) >= 0.0) Emit(X, Y);
		}
	}
}

/** 世界 → 局部的法线变换（伴随矩阵的转置，镜像时再翻一次号），与 UE 自己变换法线的写法一致。 */
FVector3f CSNaniteCutBake_InverseTransformNormal(const FTransform& Transform, const FVector3f& Normal)
{
	const FMatrix WorldToLocal = Transform.ToInverseMatrixWithScale();
	const FVector Transformed = WorldToLocal.TransposeAdjoint().TransformVector(FVector(Normal))
		* (WorldToLocal.Determinant() < 0.0 ? -1.0 : 1.0);
	return FVector3f(Transformed.GetSafeNormal());
}

/** 与合成用的颜色空间对齐：烘焙端线性 / sRGB 与贴图的 SRGB 标志不一致时换算。 */
FColor CSNaniteCutBake_ConvertColor(const FColor& In, bool bSourceLinear, bool bTargetSRGB)
{
	if (bSourceLinear == !bTargetSRGB) return In;
	if (bSourceLinear)
	{
		// 线性 → sRGB 编码
		return FLinearColor(In.R / 255.0f, In.G / 255.0f, In.B / 255.0f, In.A / 255.0f).ToFColor(true);
	}
	// sRGB → 线性
	return FLinearColor(In).ToFColor(false);
}

struct FCSNaniteCutBakeTarget
{
	EFlattenMaterialProperties Flatten;
	EMaterialProperty Bake;
	bool bTargetSRGB = true;
};
}

bool CSNaniteCutBake::Bake(
	UCSMesh* Mesh,
	TConstArrayView<int32> MaterialSources,
	TConstArrayView<FCSNaniteCutBakeSource> Sources,
	const FCSNaniteCutBakeParams& Params,
	FCSNaniteCutBakeResult& OutResult)
{
	OutResult = FCSNaniteCutBakeResult();
	const double StartSeconds = FPlatformTime::Seconds();
	if (!Mesh || !IsInGameThread() || !GEngine || !GEngine->DefaultFlattenMaterial) return false;
	if (Params.AssetFolder.IsEmpty() || Params.AssetBaseName.IsEmpty())
	{
		UE_LOG(LogCSNaniteCutBake, Warning, TEXT("%s No asset folder / name (unsaved map?)."), CSNaniteCutBake_LogPrefix);
		return false;
	}
	const int32 Size = FMath::Clamp(Params.TextureSize, 64, 8192);

	// ---- 1. 回读 ------------------------------------------------------------------------------
	FCSGpuMeshCPUData Data;
	if (!Mesh->ReadbackMeshSync(Data) || Data.Indices.Num() < 3 || Data.AttrLayout != FCSGpuMeshCPUData::EAttrLayout::PerVertex)
	{
		UE_LOG(LogCSNaniteCutBake, Warning, TEXT("%s Mesh readback failed or empty."), CSNaniteCutBake_LogPrefix);
		return false;
	}
	const int32 NumVertices = Data.Positions.Num();
	const int32 NumTriangles = Data.Indices.Num() / 3;
	for (uint32 Index : Data.Indices)
	{
		if (int32(Index) >= NumVertices)
		{
			UE_LOG(LogCSNaniteCutBake, Warning, TEXT("%s Index out of range in readback."), CSNaniteCutBake_LogPrefix);
			return false;
		}
	}
	TArray<float> BinormalSigns;
	CSNaniteCutBake_ReadBinormalSigns(Mesh, NumVertices, BinormalSigns);
	const int32 NumUVChannels = FMath::Clamp(Data.NumTexCoordChannels, 1, 4);
	auto VertexUV = [&Data](int32 Channel, int32 Vertex)
	{
		const TArray<FVector2f>& UVs = Data.TexCoordChannels[Channel];
		return UVs.IsValidIndex(Vertex) ? UVs[Vertex] : FVector2f::ZeroVector;
	};
	// 顶点色流存的是渲染用的 FColor 字节（sRGB 编码值，材质里的 VertexColor 读的就是它）；
	// MeshDescription 的颜色是线性的，MaterialBaking 再 ToFColor(true) 编回字节 —— 先解码才能原样还原。
	auto VertexColor = [&Data](int32 Vertex)
	{
		if (!Data.Colors.IsValidIndex(Vertex)) return FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		const FVector4f& C = Data.Colors[Vertex];
		const FLinearColor Linear = FLinearColor::FromSRGBColor(FColor(
			uint8(FMath::RoundToInt(C.X * 255.0f)), uint8(FMath::RoundToInt(C.Y * 255.0f)),
			uint8(FMath::RoundToInt(C.Z * 255.0f)), uint8(FMath::RoundToInt(C.W * 255.0f))));
		return FVector4f(Linear.R, Linear.G, Linear.B, Linear.A);
	};
	auto TriangleMaterial = [&Data](int32 Triangle)
	{
		return Data.TriangleMaterialSlots.IsValidIndex(Triangle) ? Data.TriangleMaterialSlots[Triangle] : 0;
	};
	auto TriangleSource = [&MaterialSources, &TriangleMaterial](int32 Triangle)
	{
		const int32 Material = TriangleMaterial(Triangle);
		return MaterialSources.IsValidIndex(Material) ? MaterialSources[Material] : INDEX_NONE;
	};

	// ---- 2. 新 UV：按源 UV0 的岛重新打包（每个三角的三个角各一个顶点实例，下标 = 三角 × 3 + 角） -----
	TArray<FVector2D> AtlasUVs;
	{
		FMeshDescription UVMesh;
		FStaticMeshAttributes Attributes(UVMesh);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		UVMesh.ReserveNewVertices(NumTriangles * 3);
		UVMesh.ReserveNewVertexInstances(NumTriangles * 3);
		UVMesh.ReserveNewTriangles(NumTriangles);
		const FPolygonGroupID Group = UVMesh.CreatePolygonGroup();
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			FVertexInstanceID Corners[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = int32(Data.Indices[Triangle * 3 + Corner]);
				const FVertexID VertexID = UVMesh.CreateVertex();
				Positions[VertexID] = Data.Positions[Vertex];
				Corners[Corner] = UVMesh.CreateVertexInstance(VertexID);
				UVs.Set(Corners[Corner], 0, VertexUV(0, Vertex));
				Normals[Corners[Corner]] = Data.Normals.IsValidIndex(Vertex) ? Data.Normals[Vertex] : FVector3f::UnitZ();
			}
			UVMesh.CreateTriangle(Group, Corners);
		}

		FStaticMeshOperations::FGenerateUVOptions Options;
		Options.TextureResolution = Size;
		Options.bMergeTrianglesWithIdenticalAttributes = false;
		Options.UVMethod = FStaticMeshOperations::EGenerateUVMethod::Legacy;
		if (!FStaticMeshOperations::GenerateUV(UVMesh, Options, AtlasUVs) || AtlasUVs.Num() != NumTriangles * 3)
		{
			UE_LOG(LogCSNaniteCutBake, Warning, TEXT("%s Legacy UV layout failed (%d UVs for %d corners), trying the auto-UV method."),
				CSNaniteCutBake_LogPrefix, AtlasUVs.Num(), NumTriangles * 3);
			Options.UVMethod = FStaticMeshOperations::EGenerateUVMethod::Default;
			AtlasUVs.Reset();
			if (!FStaticMeshOperations::GenerateUV(UVMesh, Options, AtlasUVs) || AtlasUVs.Num() != NumTriangles * 3)
			{
				UE_LOG(LogCSNaniteCutBake, Error, TEXT("%s UV layout failed."), CSNaniteCutBake_LogPrefix);
				return false;
			}
		}
	}

	// ---- 3. 纹素 → 三角 -----------------------------------------------------------------------
	TArray<int32> TexelTriangle;
	TexelTriangle.Init(INDEX_NONE, Size * Size);
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		CSNaniteCutBake_Raster(AtlasUVs[Triangle * 3] * Size, AtlasUVs[Triangle * 3 + 1] * Size, AtlasUVs[Triangle * 3 + 2] * Size, Size,
			[&TexelTriangle, Size, Triangle](int32 X, int32 Y) { TexelTriangle[Y * Size + X] = Triangle; });
	}
	int32 CoveredTexels = 0;
	for (int32 Triangle : TexelTriangle) CoveredTexels += Triangle != INDEX_NONE ? 1 : 0;
	OutResult.Coverage = float(CoveredTexels) / float(Size * Size);

	// ---- 4. 按源分组 --------------------------------------------------------------------------
	TMap<int32, TArray<int32>> TrianglesBySource;
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle) TrianglesBySource.FindOrAdd(TriangleSource(Triangle)).Add(Triangle);

	// 扁平化材质的默认贴图决定三张贴图的 sRGB 标志；烘焙结果按它换算。
	UMaterialInterface* FlattenBase = GEngine->DefaultFlattenMaterial;
	FCSNaniteCutBakeTarget Targets[3] = {
		{ EFlattenMaterialProperties::Diffuse, MP_BaseColor, true },
		{ EFlattenMaterialProperties::Normal, MP_Normal, false },
		{ EFlattenMaterialProperties::Roughness, MP_Roughness, false } };
	for (FCSNaniteCutBakeTarget& Target : Targets)
	{
		UTexture* Default = nullptr;
		if (FlattenBase->GetTextureParameterValue(FName(*FMaterialUtilities::GetFlattenMaterialTextureName(Target.Flatten, FlattenBase)), Default) && Default)
			Target.bTargetSRGB = Default->SRGB;
	}

	IMaterialBakingModule& Baking = FModuleManager::Get().LoadModuleChecked<IMaterialBakingModule>("MaterialBaking");

	// ---- 5. 所有 (源, 材质) 一次烘进同一套整图集大小的渲染目标 ------------------------------------------
	// 走 MaterialBaking 的单输出路径：每个 (源, 材质) 只画自己的三角，图集里各块互不重叠，画完就是合成好的图集；
	// 渲染目标不进模块的池子，烘完随 GC 释放。多输出路径按"尺寸 + 格式"把渲染目标永久挂在模块池里，逐源矩形
	// 尺寸各不相同 —— 实测烘一次就多挂了 94 张、约 485 MB 显存，直到关编辑器才还。
	TArray<TUniquePtr<FMeshDescription>> SourceMeshes;
	TArray<TUniquePtr<FMaterialData>> MaterialDatas;
	TArray<TUniquePtr<FMeshData>> MeshDatas;
	for (const TPair<int32, TArray<int32>>& SourceTriangles : TrianglesBySource)
	{
		const FCSNaniteCutBakeSource Source = Sources.IsValidIndex(SourceTriangles.Key) ? Sources[SourceTriangles.Key] : FCSNaniteCutBakeSource();
		const TArray<int32>& Triangles = SourceTriangles.Value;

		// 源自己的网格：位置、法线、切线都在世界空间，LocalToWorld 给单位矩阵（WorldPosition = 位置）。
		// 不给源的真实变换：MaterialBaking 把图集平面（z = 0）上的点先乘 LocalToWorld 的逆、再由着色器乘回去，
		// 源带非均匀缩放时 z 会落成 ±ε，负的那一半被近裁剪面裁掉 —— 实测随机丢了 21% 的纹素。引擎 MeshMerge
		// 同样用单位矩阵。代价：LocalPosition / ObjectOrientation / 转到局部空间的变换按世界空间算。
		FMeshDescription& SourceMesh = *SourceMeshes.Add_GetRef(MakeUnique<FMeshDescription>());
		FStaticMeshAttributes Attributes(SourceMesh);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(NumUVChannels);

		TMap<int32, FPolygonGroupID> GroupByMaterial;
		TArray<FVector2D> CustomUVs;
		CustomUVs.Reserve(Triangles.Num() * 3);
		for (int32 Triangle : Triangles)
		{
			const int32 Material = TriangleMaterial(Triangle);
			FPolygonGroupID* Group = GroupByMaterial.Find(Material);
			if (!Group) Group = &GroupByMaterial.Add(Material, SourceMesh.CreatePolygonGroup());
			FVertexInstanceID Corners[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = int32(Data.Indices[Triangle * 3 + Corner]);
				const FVertexID VertexID = SourceMesh.CreateVertex();
				Positions[VertexID] = Data.Positions[Vertex];
				const FVertexInstanceID Instance = SourceMesh.CreateVertexInstance(VertexID);
				Normals[Instance] = Data.Normals.IsValidIndex(Vertex) ? Data.Normals[Vertex] : FVector3f::UnitZ();
				Tangents[Instance] = Data.Tangents.IsValidIndex(Vertex) ? Data.Tangents[Vertex] : FVector3f::UnitX();
				Signs[Instance] = BinormalSigns[Vertex];
				Colors[Instance] = VertexColor(Vertex);
				for (int32 Channel = 0; Channel < NumUVChannels; ++Channel) UVs.Set(Instance, Channel, VertexUV(Channel, Vertex));
				Corners[Corner] = Instance;
				CustomUVs.Add(AtlasUVs[Triangle * 3 + Corner]);
			}
			SourceMesh.CreateTriangle(*Group, Corners);
		}

		// ObjectPosition 取的是 WorldBounds 的中心，ActorPosition / 包围盒 / 自定义图元数据都与 LocalToWorld 无关，照旧逐源给。
		FPrimitiveData PrimitiveData(Source.LocalBounds);
		PrimitiveData.LocalToWorld = FMatrix::Identity;
		PrimitiveData.ActorPosition = Source.ActorPosition;
		PrimitiveData.WorldBounds = Source.WorldBounds;
		PrimitiveData.LocalBounds = Source.LocalBounds;
		PrimitiveData.PreSkinnedLocalBounds = Source.LocalBounds;
		PrimitiveData.CustomPrimitiveData = Source.CustomPrimitiveData;

		for (const TPair<int32, FPolygonGroupID>& MaterialGroup : GroupByMaterial)
		{
			UMaterialInterface* Material = Mesh->Materials.IsValidIndex(MaterialGroup.Key) ? Mesh->Materials[MaterialGroup.Key].Get() : nullptr;
			if (!Material) Material = UMaterial::GetDefaultMaterial(MD_Surface);

			// 单输出路径要求所有条目的尺寸与各开关完全一致（引擎里是 check）。
			FMaterialData& MaterialData = *MaterialDatas.Add_GetRef(MakeUnique<FMaterialData>());
			MaterialData.Material = Material;
			MaterialData.PropertySizes.Add(MP_BaseColor, FIntPoint(Size, Size));
			MaterialData.PropertySizes.Add(MP_Normal, FIntPoint(Size, Size));
			MaterialData.PropertySizes.Add(MP_Roughness, FIntPoint(Size, Size));
			MaterialData.bPerformBorderSmear = false;   // 缝隙最后统一外扩
			MaterialData.bPerformShrinking = false;     // 不许把整块纯色缩成 1×1
			MaterialData.bTangentSpaceNormal = true;    // 世界空间法线的材质也换到切线空间
			MaterialData.BackgroundColor = FColor::Magenta;

			FMeshData& MeshData = *MeshDatas.Add_GetRef(MakeUnique<FMeshData>());
			MeshData.MeshDescription = &SourceMesh;
			MeshData.MaterialIndices.Add(MaterialGroup.Value.GetValue());
			MeshData.CustomTextureCoordinates = CustomUVs;
			MeshData.TextureCoordinateBox = FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
			MeshData.TextureCoordinateIndex = 0;
			MeshData.PrimitiveData = PrimitiveData;
		}
	}
	OutResult.BakeJobs = MaterialDatas.Num();
	if (MaterialDatas.IsEmpty())
	{
		UE_LOG(LogCSNaniteCutBake, Warning, TEXT("%s Nothing to bake."), CSNaniteCutBake_LogPrefix);
		return false;
	}

	FBakeOutput Baked;
	{
		TArray<FMaterialData*> MaterialSettings;
		TArray<FMeshData*> MeshSettings;
		for (const TUniquePtr<FMaterialData>& MaterialData : MaterialDatas) MaterialSettings.Add(MaterialData.Get());
		for (const TUniquePtr<FMeshData>& MeshData : MeshDatas) MeshSettings.Add(MeshData.Get());
		const bool bPreviousLinear = Baking.IsLinearBake(MP_Roughness);
		Baking.SetLinearBake(true);
		Baking.BakeMaterials(MaterialSettings, MeshSettings, Baked);
		Baking.SetLinearBake(bPreviousLinear);
	}
	MeshDatas.Empty();
	MaterialDatas.Empty();
	SourceMeshes.Empty();

	TArray<FColor> Atlas[3];
	bool bLinear[3] = {};
	for (int32 Plane = 0; Plane < 3; ++Plane)
	{
		TArray<FColor>* Samples = Baked.PropertyData.Find(Targets[Plane].Bake);
		const FIntPoint* PlaneSize = Baked.PropertySizes.Find(Targets[Plane].Bake);
		if (!Samples || !PlaneSize || *PlaneSize != FIntPoint(Size, Size) || Samples->Num() != Size * Size)
		{
			UE_LOG(LogCSNaniteCutBake, Error, TEXT("%s Material bake returned no usable data for property %d."), CSNaniteCutBake_LogPrefix, int32(Targets[Plane].Bake));
			return false;
		}
		Atlas[Plane] = MoveTemp(*Samples);
		const bool* Linear = Baked.PropertyIsLinearColor.Find(Targets[Plane].Bake);
		bLinear[Plane] = Linear ? *Linear : Targets[Plane].Bake == MP_Normal;
	}

	// 品红 = 背景，没有三角画到。三角盖住了纹素中心、GPU 按左上规则把它判给了邻三角的也是品红 —— 留给外扩补。
	TBitArray<> Filled(false, Size * Size);
	for (int32 Texel = 0; Texel < Size * Size; ++Texel)
	{
		const FColor BaseColor = Atlas[0][Texel];
		if (BaseColor.R == 255 && BaseColor.G == 0 && BaseColor.B == 255)
		{
			if (TexelTriangle[Texel] != INDEX_NONE) ++OutResult.PatchedTexels;
			continue;
		}
		for (int32 Plane = 0; Plane < 3; ++Plane)
		{
			FColor& Value = Atlas[Plane][Texel];
			Value = CSNaniteCutBake_ConvertColor(Value, bLinear[Plane], Targets[Plane].bTargetSRGB);
			Value.A = 255;
		}
		Filled[Texel] = true;
	}

	// ---- 6. 外扩：没写到的纹素取已写邻居的平均，一圈一圈往外推（mip 采样不会串到别的岛上的颜色） -------
	{
		TArray<int32> Frontier;
		TBitArray<> Queued(false, Size * Size);
		auto PushNeighbours = [&](int32 Texel)
		{
			const int32 X = Texel % Size;
			const int32 Y = Texel / Size;
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const int32 NX = X + DX;
					const int32 NY = Y + DY;
					if ((DX == 0 && DY == 0) || NX < 0 || NY < 0 || NX >= Size || NY >= Size) continue;
					const int32 Neighbour = NY * Size + NX;
					if (Filled[Neighbour] || Queued[Neighbour]) continue;
					Queued[Neighbour] = true;
					Frontier.Add(Neighbour);
				}
			}
		};
		for (int32 Texel = 0; Texel < Size * Size; ++Texel)
			if (Filled[Texel]) PushNeighbours(Texel);

		constexpr int32 MaxRings = 16;
		for (int32 Ring = 0; Ring < MaxRings && Frontier.Num() > 0; ++Ring)
		{
			TArray<TTuple<int32, FColor, FColor, FColor>> Updates;
			Updates.Reserve(Frontier.Num());
			for (int32 Texel : Frontier)
			{
				const int32 X = Texel % Size;
				const int32 Y = Texel / Size;
				FLinearColor Sum[3] = { FLinearColor::Transparent, FLinearColor::Transparent, FLinearColor::Transparent };
				int32 Count = 0;
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const int32 NX = X + DX;
						const int32 NY = Y + DY;
						if (NX < 0 || NY < 0 || NX >= Size || NY >= Size || !Filled[NY * Size + NX]) continue;
						for (int32 Plane = 0; Plane < 3; ++Plane)
						{
							const FColor& C = Atlas[Plane][NY * Size + NX];
							Sum[Plane] += FLinearColor(C.R, C.G, C.B, 255.0f);
						}
						++Count;
					}
				}
				if (Count == 0) continue;
				auto Average = [Count](const FLinearColor& S)
				{
					return FColor(uint8(FMath::RoundToInt(S.R / Count)), uint8(FMath::RoundToInt(S.G / Count)), uint8(FMath::RoundToInt(S.B / Count)), 255);
				};
				Updates.Emplace(Texel, Average(Sum[0]), Average(Sum[1]), Average(Sum[2]));
			}
			Frontier.Reset();
			for (const TTuple<int32, FColor, FColor, FColor>& Update : Updates)
			{
				const int32 Texel = Update.Get<0>();
				Atlas[0][Texel] = Update.Get<1>();
				Atlas[1][Texel] = Update.Get<2>();
				Atlas[2][Texel] = Update.Get<3>();
				Filled[Texel] = true;
			}
			for (const TTuple<int32, FColor, FColor, FColor>& Update : Updates) PushNeighbours(Update.Get<0>());
		}
		// 岛与岛之间的大片空白：填中性值（平面法线、灰色、中等粗糙度），低 mip 里不会渗出怪色。
		for (int32 Texel = 0; Texel < Size * Size; ++Texel)
		{
			if (Filled[Texel]) continue;
			Atlas[0][Texel] = FColor(128, 128, 128, 255);
			Atlas[1][Texel] = FColor(128, 128, 255, 255);
			Atlas[2][Texel] = FColor(128, 128, 128, 255);
		}
	}

	// ---- 7. 贴图 + 材质实例（引擎扁平化材质） ---------------------------------------------------
	FFlattenMaterial Flatten;
	Flatten.RenderSize = FIntPoint(Size, Size);
	Flatten.UVChannel = 0;
	Flatten.BlendMode = BLEND_Opaque;
	for (int32 Plane = 0; Plane < 3; ++Plane)
	{
		Flatten.SetPropertySize(Targets[Plane].Flatten, FIntPoint(Size, Size));
		Flatten.GetPropertySamples(Targets[Plane].Flatten) = MoveTemp(Atlas[Plane]);
	}
	FMaterialProxySettings ProxySettings;
	ProxySettings.TextureSize = FIntPoint(Size, Size);
	ProxySettings.bNormalMap = true;
	ProxySettings.bRoughnessMap = true;
	ProxySettings.bMetallicMap = false;
	ProxySettings.bSpecularMap = false;
	ProxySettings.bEmissiveMap = false;
	ProxySettings.bOpacityMap = false;
	ProxySettings.bOpacityMaskMap = false;
	ProxySettings.bAmbientOcclusionMap = false;
	ProxySettings.bTangentMap = false;
	ProxySettings.bAnisotropyMap = false;
	ProxySettings.MetallicConstant = 0.0f;
	ProxySettings.SpecularConstant = 0.5f;
	ProxySettings.BlendMode = BLEND_Opaque;
	TArray<UObject*> CreatedAssets;
	UMaterialInstanceConstant* MaterialInstance = FMaterialUtilities::CreateFlattenMaterialInstance(
		nullptr, ProxySettings, FlattenBase, Flatten, Params.AssetFolder + TEXT("/"), Params.AssetBaseName, CreatedAssets);
	if (!MaterialInstance)
	{
		UE_LOG(LogCSNaniteCutBake, Error, TEXT("%s Creating the flatten material instance failed."), CSNaniteCutBake_LogPrefix);
		return false;
	}
	for (UObject* Asset : CreatedAssets)
	{
		if (!Asset) continue;
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
	}

	// ---- 8. 输出网格：保留源的法线切线，UV0 = 图集 -------------------------------------------------
	FMeshDescription OutputMesh;
	{
		FStaticMeshAttributes Attributes(OutputMesh);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		const FPolygonGroupID Group = OutputMesh.CreatePolygonGroup();
		SlotNames[Group] = CSStaticMeshAsset::MaterialSlotName(0);
		const float MirrorSign = Params.OutputTransform.GetDeterminant() < 0.0f ? -1.0f : 1.0f;
		OutputMesh.ReserveNewVertices(NumTriangles * 3);
		OutputMesh.ReserveNewVertexInstances(NumTriangles * 3);
		OutputMesh.ReserveNewTriangles(NumTriangles);
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			FVertexInstanceID Corners[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = int32(Data.Indices[Triangle * 3 + Corner]);
				const FVertexID VertexID = OutputMesh.CreateVertex();
				Positions[VertexID] = FVector3f(Params.OutputTransform.InverseTransformPosition(FVector(Data.Positions[Vertex])));
				const FVertexInstanceID Instance = OutputMesh.CreateVertexInstance(VertexID);
				const FVector3f WorldNormal = Data.Normals.IsValidIndex(Vertex) ? Data.Normals[Vertex] : FVector3f::UnitZ();
				const FVector3f WorldTangent = Data.Tangents.IsValidIndex(Vertex) ? Data.Tangents[Vertex] : FVector3f::UnitX();
				Normals[Instance] = CSNaniteCutBake_InverseTransformNormal(Params.OutputTransform, WorldNormal);
				Tangents[Instance] = FVector3f(Params.OutputTransform.InverseTransformVector(FVector(WorldTangent)).GetSafeNormal());
				Signs[Instance] = BinormalSigns[Vertex] * MirrorSign;
				Colors[Instance] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
				UVs.Set(Instance, 0, FVector2f(AtlasUVs[Triangle * 3 + Corner]));
				Corners[Corner] = Instance;
			}
			OutputMesh.CreateTriangle(Group, Corners);
		}
	}

	FCSStaticMeshAssetTarget AssetTarget;
	if (!CSStaticMeshAsset::ResolveTarget(Params.AssetFolder / (TEXT("SM_") + Params.AssetBaseName), /*bReplaceExisting*/ true,
		CSNaniteCutBake_LogPrefix, AssetTarget)) return false;
	UStaticMesh* StaticMesh = CSStaticMeshAsset::PrepareMesh(AssetTarget, CSNaniteCutBake_LogPrefix);
	if (!StaticMesh) return false;

	// 默认构建设置会重算法线切线、生成光照 UV、半精度 UV：法线贴图烘在源的切线空间里，切线必须原样保留；
	// 2048 的图集半精度 UV 在 0.5～1 之间只剩一个纹素的精度。
	StaticMesh->SetNumSourceModels(1);
	FMeshBuildSettings& BuildSettings = StaticMesh->GetSourceModel(0).BuildSettings;
	BuildSettings.bRecomputeNormals = false;
	BuildSettings.bRecomputeTangents = false;
	BuildSettings.bGenerateLightmapUVs = false;
	BuildSettings.bUseFullPrecisionUVs = true;
	BuildSettings.bUseHighPrecisionTangentBasis = true;
	{
		// 不存切线的 Nanite 按 UV 在像素里现推切线，会和烘法线用的源切线对不上。
		FMeshNaniteSettings NaniteSettings = StaticMesh->GetNaniteSettings();
		NaniteSettings.bExplicitTangents = true;
		StaticMesh->SetNaniteSettings(NaniteSettings);
	}
	if (!CSStaticMeshAsset::PopulateFromDescription(StaticMesh, OutputMesh, TArray<UMaterialInterface*>{ MaterialInstance }, {},
		/*bCommitMeshDescription*/ true, Params.bEnableNanite))
	{
		UE_LOG(LogCSNaniteCutBake, Error, TEXT("%s StaticMesh build failed for '%s'."), CSNaniteCutBake_LogPrefix, *AssetTarget.SanitizedPath);
		return false;
	}
	CSStaticMeshAsset::Finalize(StaticMesh, AssetTarget, /*bSaveToDisk*/ false, CSNaniteCutBake_LogPrefix);

	OutResult.StaticMesh = StaticMesh;
	OutResult.Material = MaterialInstance;
	OutResult.Triangles = NumTriangles;
	OutResult.Seconds = FPlatformTime::Seconds() - StartSeconds;
	UE_LOG(LogCSNaniteCutBake, Log,
		TEXT("%s %s: %d triangles, %d source(s), %d bake job(s), %dx%d atlas (%.1f%% covered, %d edge texel(s) patched), Nanite %s, %.1f s."),
		CSNaniteCutBake_LogPrefix, *AssetTarget.SanitizedPath, NumTriangles, TrianglesBySource.Num(), OutResult.BakeJobs, Size, Size,
		OutResult.Coverage * 100.0f, OutResult.PatchedTexels, Params.bEnableNanite ? TEXT("on") : TEXT("off"), OutResult.Seconds);
	return true;
}

#endif // WITH_EDITOR
