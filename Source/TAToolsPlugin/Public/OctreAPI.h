// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
//#include "GenericOctree.h"
//#include "Box2D.h"
#include "GameFramework/Actor.h"

#include "OctreAPI.generated.h"

/**
 * 
 */
UCLASS()
class TATOOLSPLUGIN_API UOctreAPI : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	static void Test();
};

//USTRUCT()
//struct FOctreeElement
//{
	//GENERATED_USTRUCT_BODY()

	//	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree Element Struct")
	//	AActor* MyActor;

	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree Element Struct")
	//	FBoxSphereBounds BoxSphereBounds;

	//FOctreeElement()
	//{
	//	MyActor = nullptr;
	//	BoxSphereBounds = FBoxSphereBounds(FVector(0.0f, 0.0f, 0.0f), FVector(1.0f, 1.0f, 1.0f), 1.0f);
	//}
//};

//struct FOctreeSematics
//{
//	enum { MaxElementsPerLeaf = 2 }; // 16
//	enum { MinInclusiveElementsPerNode = 7 };
//	enum { MaxNodeDepth = 12 };
//
//	typedef TInlineAllocator<MaxElementsPerLeaf> ElementAllocator;
//
//	/**
//	* Get the bounding box of the provided octree element. In this case, the box
//	* is merely the point specified by the element.
//	*
//	* @param	Element	Octree element to get the bounding box for
//	*
//	* @return	Bounding box of the provided octree element
//	*/
//	FORCEINLINE static FBoxSphereBounds GetBoundingBox(const FOctreeElement& Element)
//	{
//		return Element.BoxSphereBounds;
//	}
//
//	FORCEINLINE static bool AreElementsEqual(const FOctreeElement& A, const FOctreeElement& B)
//	{
//		return A.MyActor == B.MyActor;
//	}
//
//	FORCEINLINE static void SetElementId(const FOctreeElement& Element, FOctreeElementId Id)
//	{
//	}
//
//	FORCEINLINE static void ApplyOffset(FOctreeElement& Element, FVector Offset)
//	{
//		FVector NewPostion = Element.MyActor->GetActorLocation() + Offset;
//		Element.MyActor->SetActorLocation(NewPostion);
//		Element.BoxSphereBounds.Origin = NewPostion;
//	}
//
//};
//
//typedef TOctree<FOctreeElement, FOctreeSematics> FSimpleOctree;

//UCLASS()
//class TATOOLSPLUGIN_API ASpacePartioner : public AActor
//{
//	GENERATED_BODY()
//
//public:
//	// Sets default values for this actor's properties
//	ASpacePartioner(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
//
//	/**
//	* Used in conjunction with a constructor to initialize the object.
//	* @param NewBounds	Intial size of the Octree
//	* @param inDrawDebugInfo	Whether or not to display debug boundaries
//	*/
//	UFUNCTION(BlueprintCallable, Category = Octree)
//		void Initialize(const FBox& inNewBounds, const bool& inDrawDebugInfo);
//
//	/**
//	* Used in conjunction with a constructor to initialize the object.
//	* @param inExtent	Intial size of the Octree
//	* @param inDrawDebugInfo	Whether or not to display debug boundaries
//	*/
//	void Initialize(const float& inExtent, const bool& inDrawDebugInfo);
//
//	// Called when the game starts or when spawned
//	virtual void BeginPlay() override;
//
//	// Called every frame
//	virtual void Tick(float DeltaSeconds) override;
//
//	/**
//	* Adds a spefic element to the Octree
//	* @param NewOctreeElement	FOctreeElement to be added.
//	*/
//	UFUNCTION(BlueprintCallable, Category = Octree)
//		void AddOctreeElement(const FOctreeElement& inNewOctreeElement);
//
//	/**
//	* Returns elements within the specified region.
//	* @param inBoundingBoxQuery	Box to query Octree.
//	* @return TArray of Elements
//	*/
//	UFUNCTION(BlueprintCallable, Category = Octree)
//		TArray<FOctreeElement> GetElementsWithinBounds(const FBoxSphereBounds& inBoundingBoxQuery);
//
//	/**
//	* Returns elements within the specified region.
//	* @param inBoundingBoxQuery	Box to query Octree.
//	* @return TArray of Elements
//	*/
//	TArray<FOctreeElement> GetElementsWithinBounds(const FBoxCenterAndExtent& inBoundingBoxQuery);
//
//	/** Draws Debug information at runtime */
//	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Debug)
//		bool bDrawDebugInfo = false;
//
//private:
//
//	void DrawOctreeBounds();
//
//	FSimpleOctree* OctreeData;
//	bool bInitialized;
//
//};
//
