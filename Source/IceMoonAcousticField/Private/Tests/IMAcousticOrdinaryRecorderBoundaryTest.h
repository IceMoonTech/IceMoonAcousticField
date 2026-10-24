#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
class AIMAcousticFootstepCharacter;
class UPhysicalMaterial;

void IM_RecordOrdinaryFootstepSourceEvent(AIMAcousticFootstepCharacter& Character,
	bool bLanding, int32 EventIndex, const UPhysicalMaterial* Material,
	uint64 AudioComponentId, float Gain, float Pitch);
#endif
