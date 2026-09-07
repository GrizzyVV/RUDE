// RUDE - RAGE <-> Unreal Development Environment
// The MLO authoring volumes' constructors. Both are pure editor scenery: no collision, no tick, drawn always
// (not only when selected) so an author can see the room graph while placing props inside it.
#include "RudeMloVolumes.h"

#include "Components/BoxComponent.h"

ARudeMloRoomVolume::ARudeMloRoomVolume()
{
	PrimaryActorTick.bCanEverTick = false;
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	RootComponent = Box;
	Box->SetMobility(EComponentMobility::Static);
	// 10 x 10 x 5 m: a room-sized default, so a freshly dropped volume is already a plausible room.
	Box->InitBoxExtent(FVector(500.f, 500.f, 250.f));
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->SetGenerateOverlapEvents(false);
	Box->ShapeColor = FColor(64, 160, 255);
	Box->bDrawOnlyIfSelected = false;
}

ARudeMloPortalVolume::ARudeMloPortalVolume()
{
	PrimaryActorTick.bCanEverTick = false;
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	RootComponent = Box;
	Box->SetMobility(EComponentMobility::Static);
	// 2 m wide, 0.2 m thick, 2.4 m tall - a doorway. The THIN axis is what picks the portal plane, so the
	// default must already have an unambiguous thinnest axis.
	Box->InitBoxExtent(FVector(100.f, 10.f, 120.f));
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->SetGenerateOverlapEvents(false);
	Box->ShapeColor = FColor(255, 176, 48);
	Box->bDrawOnlyIfSelected = false;
}
