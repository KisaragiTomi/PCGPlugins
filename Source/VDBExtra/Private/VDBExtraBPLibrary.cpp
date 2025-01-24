// Copyright Epic Games, Inc. All Rights Reserved.

#include "VDBExtraBPLibrary.h"
#include "UDynamicMesh.h"
#include "ProxyLOD/Private/ProxyLODMeshAttrTransfer.h"
#include "ProxyLOD/Private/ProxyLODMeshConvertUtils.h"
#include "ProxyLOD/Private/ProxyLODMeshTypes.h"
#include "ProxyLOD/Private/ProxyLODMeshUtilities.h"
#include <openvdb/tools/ParticlesToLevelSet.h>
#include "VDBExtra.h"

using namespace ProxyLOD;

UVDBExtraBPLibrary::UVDBExtraBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

UDynamicMesh* UVDBExtraBPLibrary::ParticlesToVDBMesh(UDynamicMesh* TargetMesh,  TArray<FParticleRasterize> Particles)
{

	const float VoxelSize = 1.0f, HalfWidth = 2.0f;
	openvdb::FloatGrid::Ptr ls = openvdb::createLevelSet<openvdb::FloatGrid>(VoxelSize, HalfWidth);
	float Rmax = 100;
	float Rmin = 1.5;
	for (int32 i = 0; i < Particles.Num(); i++)
	{
		float Rad = Particles[i].Rad;
		if (Rad > Rmax || Rad < Rmin)
		{
			UE_LOG(LogTemp, Warning, TEXT("Particles are too large or too small!"));
		}
	}
	VDBParticleList pa(1, 1);
	pa.Append(Particles);
	
	openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> raster(*ls);
	raster.setRmax(Rmax);
	raster.setRmin(Rmin);
	raster.rasterizeTrails(pa, 0.75);
	raster.finalize(true);
	ls->setTransform(openvdb::math::Transform::createLinearTransform(1));
	openvdb::CoordBBox treeIndexBbox = pa.getBBox(*ls);
	ls->setGridClass(openvdb::GRID_LEVEL_SET);
	ls->setName("LevelSetSphere");
	// Create a VDB file object.
	openvdb::io::File file("mygrids.vdb");
	// Add the grid pointer to a container.
	openvdb::GridPtrVec grids;

	// Write out the contents of the container.
	grids.push_back(ls);
	file.write(grids);
	file.close();


	//ProxyLOD::ExtractIsosurfaceWithNormals(ls, 0, 0.001, AOSMeshedVolume); // QUestion should IsoSurface be worldspace?
	// 	
	//ProxyLOD::AddNormals(AOSMeshedVolume);																					
	// Evidently this conversion code makes certain assumptions about the FMeshDescription. that were introduced when it was converted from FRawMesh...
	FMeshDescription MergedMeshesDescription;
	ConvertMeshVDBExtra(ls, MergedMeshesDescription);
	//ProxyLOD::ConvertMesh(AOSMeshedVolume, MergedMeshesDescription);
	//ProxyLOD::AOSMeshToRawMesh(AOSMeshedVolume, MergedMeshesDescription);
	//AOSMeshToRawMesh(AOSMeshedVolume, MergedMeshesDescription);
	FDynamicMesh3 ConvertlMesh;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.Convert(&MergedMeshesDescription, ConvertlMesh);
	
	TargetMesh->SetMesh(MoveTemp(ConvertlMesh));
	return TargetMesh;
	
}

void UVDBExtraBPLibrary::ConvertMeshVDBExtra(const openvdb::FloatGrid::ConstPtr SDFVolume, FMeshDescription& OutRawMesh)
{
	FAOSMesh AOSMesh;
	ProxyLOD::SDFVolumeToMesh(SDFVolume, 0, 0.001, AOSMesh);
	OutRawMesh.Empty();
	//FStaticMeshAttributes Attributes(OutRawMesh);


	FStaticMeshAttributes Attributes(OutRawMesh);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TEdgeAttributesRef<bool> EdgeHardnesses = Attributes.GetEdgeHardnesses();
	TPolygonGroupAttributesRef<FName> PolygonGroupImportedMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();

	const uint32 DstNumPositions = AOSMesh.GetNumVertexes();
	const uint32 DstNumIndexes = AOSMesh.GetNumIndexes();

	if (VertexInstanceUVs.GetNumChannels() < 1)
	{
		VertexInstanceUVs.SetNumChannels(1);
	}

	FPolygonGroupID PolygonGroupID = INDEX_NONE;
	if (OutRawMesh.PolygonGroups().Num() == 0)
	{
		PolygonGroupID = OutRawMesh.CreatePolygonGroup();
		PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = FName(*FString::Printf(TEXT("ProxyLOD_Material_%d"), FMath::Rand()));
	}
	else
	{
		PolygonGroupID = OutRawMesh.PolygonGroups().GetFirstValidID();
	}

	checkSlow(DstNumIndexes % 3 == 0);
	// Copy the vertices over
	TMap<int32, FVertexID> VertexIDMap;
	VertexIDMap.Reserve(DstNumPositions);
	{
		const auto& AOSVertexes = AOSMesh.Vertexes;
		for (uint32 i = 0, I = DstNumPositions; i < I; ++i)
		{
			const FVector3f& Position = AOSVertexes[i].GetPos();
			const FVertexID NewVertexID = OutRawMesh.CreateVertex();
			VertexPositions[NewVertexID] = Position;
			VertexIDMap.Add(i, NewVertexID);
		}

		checkSlow(VertexPositions.GetNumElements() == DstNumPositions);
	}

	const uint32* AOSIndexes = AOSMesh.Indexes;

	// Connectivity: 
	auto CreateTriangle = [&OutRawMesh, PolygonGroupID, &VertexInstanceNormals, &VertexInstanceTangents, &VertexInstanceBinormalSigns, &VertexInstanceColors, &VertexInstanceUVs, &EdgeHardnesses](const FVertexID TriangleIndex[3], const FVector3f Normals[3])
	{
		TArray<FVertexInstanceID> VertexInstanceIDs;
		VertexInstanceIDs.SetNum(3);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			VertexInstanceIDs[Corner] = OutRawMesh.CreateVertexInstance(TriangleIndex[Corner]);
			VertexInstanceTangents[VertexInstanceIDs[Corner]] = FVector3f(1, 0, 0);
			VertexInstanceNormals[VertexInstanceIDs[Corner]] = Normals[Corner];
			VertexInstanceBinormalSigns[VertexInstanceIDs[Corner]] = GetBasisDeterminantSign((FVector)VertexInstanceTangents[VertexInstanceIDs[Corner]].GetSafeNormal(),
																							 (FVector)(VertexInstanceNormals[VertexInstanceIDs[Corner]] ^ VertexInstanceTangents[VertexInstanceIDs[Corner]]).GetSafeNormal(),
																							 (FVector)VertexInstanceNormals[VertexInstanceIDs[Corner]].GetSafeNormal());
			VertexInstanceColors[VertexInstanceIDs[Corner]] = FVector4f(1.0f);
			VertexInstanceUVs.Set(VertexInstanceIDs[Corner], 0, FVector2f(0.0f, 0.0f));
		}

		// Insert a polygon into the mesh
		OutRawMesh.CreatePolygon(PolygonGroupID, VertexInstanceIDs);
	};

	{
		uint32 IndexStop = DstNumIndexes/3;
		for (uint32 t = 0, T = IndexStop; t < T; ++t)
		{
			FVertexID VertexIndexes[3];
			FVector3f Normals[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				VertexIndexes[Corner] = VertexIDMap[AOSIndexes[(t*3) + Corner]];
				const auto& AOSVertex = AOSMesh.Vertexes[AOSIndexes[(t*3) + Corner]];
				Normals[Corner] = AOSVertex.Normal;
			}
			CreateTriangle(VertexIndexes, Normals);
		}
	}
}

