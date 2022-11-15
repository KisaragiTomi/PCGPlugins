// Fill out your copyright notice in the Description page of Project Settings.


#include "Octree.h"

 //Sets default values
//AOctree::AOctree(const FObjectInitializer& ObjectInitializer)
//	: Super(ObjectInitializer)
//	, OctreeData(NULL)
//{
//	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
//	PrimaryActorTick.bCanEverTick = true;
//
//	OctreeData = new FSimpleOctree(FVector(0.0f, 0.0f, 0.0f), 100.0f); // const FVector & InOrigin, float InExtent
//}
//
//// Called when the game starts or when spawned
//void AOctree::BeginPlay()
//{
//	Super::BeginPlay();
//	
//}
//
//// Called every frame
//void AOctree::Tick(float DeltaTime)
//{
//	Super::Tick(DeltaTime);
//
//}

AOctree::AOctree()
	: Super()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
}

// Called when the game starts or when spawned
void AOctree::BeginPlay()
{
	Super::BeginPlay();

}

