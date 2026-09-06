// RUDE - RAGE <-> Unreal Development Environment
// One ymap car generator (CCarGen) as a marker actor's component: the fields edit-native, the source
// slice for a verbatim round trip. The marker's transform IS the generator's position + heading.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RudeCarGenComponent.generated.h"

UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudeCarGenComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	// |orientX, orientY| in metres: the generator's length along its heading
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen", meta = (ClampMin = "0.1"))
	float Length = 6.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen", meta = (ClampMin = "0.1"))
	float PerpendicularLength = 3.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	FString CarModel;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	uint32 Flags = 3936;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	int32 BodyColorRemap1 = -1;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	int32 BodyColorRemap2 = -1;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	int32 BodyColorRemap3 = -1;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	int32 BodyColorRemap4 = -1;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	FString PopGroup;
	UPROPERTY(EditAnywhere, Category = "RUDE|CarGen")
	int32 Livery = -1;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYmap;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FTransform SourceTransform;
	// the file's own spelling of the heading, reused verbatim while the heading is unchanged
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceOrientXText;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceOrientYText;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	FString FieldsKey() const
	{
		return FString::Printf(TEXT("%g|%g|%s|%u|%d|%d|%d|%d|%s|%d"), Length, PerpendicularLength, *CarModel, Flags,
			BodyColorRemap1, BodyColorRemap2, BodyColorRemap3, BodyColorRemap4, *PopGroup, Livery);
	}
};
