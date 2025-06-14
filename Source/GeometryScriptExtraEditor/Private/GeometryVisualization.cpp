#include "GeometryVisualization.h"

#include "DynamicMesh/MeshNormals.h"
#include "Kismet/KismetSystemLibrary.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshEditor.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Operations/SmoothDynamicMeshAttributes.h"

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
