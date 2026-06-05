// Copyright Epic Games, Inc. All Rights Reserved.


#include "TwinStickAoEAttack.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "TwinStickNPC.h"

ATwinStickAoEAttack::ATwinStickAoEAttack()
{
 	PrimaryActorTick.bCanEverTick = true;

	// create the root component
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// create the mesh that provides the visual representation for the AoE
	SphereVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sphere Visual"));
	SphereVisual->SetupAttachment(RootComponent);

	SphereVisual->SetCollisionProfileName(FName("NoCollision"));

	// create the collision sphere
	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("Collision Sphere"));
	CollisionSphere->SetupAttachment(RootComponent);

	CollisionSphere->SetSphereRadius(750.0f);
	CollisionSphere->SetNotifyRigidBodyCollision(true);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &ATwinStickAoEAttack::OnAoEOverlap);
}

void ATwinStickAoEAttack::BeginPlay()
{
	Super::BeginPlay();
	
	// set up the AoE timers
	GetWorld()->GetTimerManager().SetTimer(StartAoETimer, this, &ATwinStickAoEAttack::StartAoE, StartAoETime, false);
	GetWorld()->GetTimerManager().SetTimer(StopAoETimer, this, &ATwinStickAoEAttack::StopAoE, StopAoETime, false);

}

void ATwinStickAoEAttack::EndPlay(EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	// clear the timers
	GetWorld()->GetTimerManager().ClearTimer(StartAoETimer);
	GetWorld()->GetTimerManager().ClearTimer(StopAoETimer);
}

void ATwinStickAoEAttack::StartAoE()
{
	// raise the active flag
	bIsAoEActive = true;

	// find all actors overlapping the NPC
	TArray<AActor*> Overlaps;
	CollisionSphere->GetOverlappingActors(Overlaps, ATwinStickNPC::StaticClass());

	// process each overlapping actor
	for (AActor* Current : Overlaps)
	{
		if (ATwinStickNPC* NPC = Cast<ATwinStickNPC>(Current))
		{
			// tell the NPC it's been hit
			NPC->ProjectileImpact(FVector::ZeroVector);
		}
	}
}

void ATwinStickAoEAttack::StopAoE()
{
	// drop the active flag
	bIsAoEActive = false;

	// stop the damage tick timer
	GetWorld()->GetTimerManager().ClearTimer(StartAoETimer);

	// call the BP handler. It will be responsible for destroying the Actor when it's done
	BP_AoEFinished();
}

void ATwinStickAoEAttack::OnAoEOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	// is the explosion active?
	if (bIsAoEActive)
	{
		// did we overlap an NPC?
		if (ATwinStickNPC* NPC = Cast<ATwinStickNPC>(OtherActor))
		{
			// tell the NPC it's been hit
			NPC->ProjectileImpact(FVector::ZeroVector);
		}
	}
}
