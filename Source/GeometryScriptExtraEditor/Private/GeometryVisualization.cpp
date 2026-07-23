#include "GeometryVisualization.h"

#include "DynamicMesh/MeshNormals.h"
#include "Kismet/KismetSystemLibrary.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshEditor.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Engine/World.h"
#include "Operations/SmoothDynamicMeshAttributes.h"
#include "DrawDebugHelpers.h"
#include "TextureResource.h"
#include "Engine/TextureRenderTarget2D.h"

using namespace UE::Geometry;

void UGeometryVisualization::VisualizingVertexNormal(UDynamicMesh* TargetMesh, FTransform Transform, float Length)
{
	
	TArray<FVector> VertexNormals;
	TArray<FVector> VertexPositions;
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		int32 VertexCount = EditMesh.VertexCount();
		for (int32 i = 0; i < VertexCount; i++)
		{
			FVector3f Normal = EditMesh.GetVertexNormal(i);
			VertexNormals.Add(FVector(Normal.X, Normal.Y, Normal.Z));
			FVector Position = EditMesh.GetVertex(i);
			VertexPositions.Add(Position);
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	for (int32 i = 0; i < VertexNormals.Num(); i++)
	{
		FVector Start = Transform.TransformPosition(VertexPositions[i]);
		FVector End = Transform.TransformPosition(VertexPositions[i] + VertexNormals[i] * Length);
		UKismetSystemLibrary::DrawDebugLine(GWorld, Start, End, FLinearColor(0, 0, 1, 0), 3, 1);
	}
}

void UGeometryVisualization::VisualizeRenderTargetAsPointGrid(
	UTextureRenderTarget2D* RenderTarget,
	int32 GridSize,
	ERenderTargetChannelIndex ChannelIndex,
	FVector WorldCenter,
	FVector2D WorldExtent,
	float HeightScale,
	float Duration,
	FLinearColor PointColor,
	float PointSize)
{
	if (!RenderTarget || !GWorld)
	{
		return;
	}

	GridSize = FMath::Max(GridSize, 2);

	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return;
	}

	const int32 TexWidth = RenderTarget->SizeX;
	const int32 TexHeight = RenderTarget->SizeY;
	if (TexWidth <= 0 || TexHeight <= 0)
	{
		return;
	}

	// 从RenderTarget读回像素数据
	TArray<FLinearColor> Pixels;
	FIntRect SampleRect(0, 0, TexWidth - 1, TexHeight - 1);
	FReadSurfaceDataFlags ReadFlags;
	RTResource->ReadLinearColorPixels(Pixels, ReadFlags, SampleRect);

	if (Pixels.Num() == 0)
	{
		return;
	}

	// 计算世界空间网格步长
	const int32 ChannelIdx = static_cast<int32>(ChannelIndex);
	const float HalfExtX = WorldExtent.X * 0.5f;
	const float HalfExtY = WorldExtent.Y * 0.5f;
	const float StepX = (GridSize > 1) ? (WorldExtent.X / (GridSize - 1)) : 0.0f;
	const float StepY = (GridSize > 1) ? (WorldExtent.Y / (GridSize - 1)) : 0.0f;

	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			// 将网格位置映射到纹理UV
			const float U = (GridSize > 1) ? (static_cast<float>(X) / (GridSize - 1)) : 0.5f;
			const float V = (GridSize > 1) ? (static_cast<float>(Y) / (GridSize - 1)) : 0.5f;

			// 采样纹理像素
			const int32 TexX = FMath::Clamp(FMath::RoundToInt(U * (TexWidth - 1)), 0, TexWidth - 1);
			const int32 TexY = FMath::Clamp(FMath::RoundToInt(V * (TexHeight - 1)), 0, TexHeight - 1);
			const int32 PixelIndex = TexY * TexWidth + TexX;

			float ChannelValue = 0.0f;
			if (PixelIndex < Pixels.Num())
			{
				const FLinearColor& Pixel = Pixels[PixelIndex];
				switch (ChannelIdx)
				{
				case 0: ChannelValue = Pixel.R; break;
				case 1: ChannelValue = Pixel.G; break;
				case 2: ChannelValue = Pixel.B; break;
				case 3: ChannelValue = Pixel.A; break;
				default: break;
				}
			}

			// 计算世界空间坐标
			FVector WorldPos;
			WorldPos.X = WorldCenter.X - HalfExtX + static_cast<float>(X) * StepX;
			WorldPos.Y = WorldCenter.Y - HalfExtY + static_cast<float>(Y) * StepY;
			WorldPos.Z = WorldCenter.Z + ChannelValue * HeightScale;

			// 根据通道值调整颜色亮度
			const FColor DebugColor = FColor(
				FMath::Clamp(static_cast<int32>(PointColor.R * 255.0f), 0, 255),
				FMath::Clamp(static_cast<int32>(PointColor.G * 255.0f), 0, 255),
				FMath::Clamp(static_cast<int32>(PointColor.B * 255.0f), 0, 255),
				FMath::Clamp(static_cast<int32>(PointColor.A * 255.0f), 0, 255)
			);

			DrawDebugPoint(GWorld, WorldPos, PointSize, DebugColor, false, Duration);
		}
	}
}
