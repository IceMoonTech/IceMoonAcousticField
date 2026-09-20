#pragma once
#include "CoreMinimal.h"
#include <phonon.h>

// Versioned bake quality recipe. These values are persisted with the bake and
// hashed into the scene fingerprint. Changing them requires a new recipe version.
// Version 1's two bounces changed gain but not decay for absorption .1 vs .8
// (sdk-decay-20260908-03). Version 2 preserves enough reflection orders and tail
// for that falsification test; its practical acceptance is measured separately.
namespace IM_AcousticRecipe
{
inline constexpr int Version=2;
inline constexpr IPLint32 PathNumSamples=1;
inline constexpr IPLfloat32 PathRadiusM=.5f;
inline constexpr IPLfloat32 PathThreshold=.5f;
inline constexpr IPLfloat32 PathVisRangeM=8;
inline constexpr IPLfloat32 PathRangeM=16;
inline constexpr IPLint32 ReverbNumRays=4096;
inline constexpr IPLint32 ReverbNumDiffuse=32;
inline constexpr IPLint32 ReverbNumBounces=64;
inline constexpr IPLfloat32 ReverbSimDurationS=2;
inline constexpr IPLfloat32 ReverbSavedDurationS=2;
// Runtime room-send policy, intentionally independent from the SDK direct
// inverse-distance model and from pathing coverage. The send is full inside
// the near distance, then fades smoothly through the wider room tail.
inline constexpr float ReverbSendNearDistanceM=2.0f;
inline constexpr float ReverbSendFarDistanceM=32.0f;
inline float ReverbSendDistanceGain(float DistanceM)
{
    if (!FMath::IsFinite(DistanceM)) return 0.0f;
    if (DistanceM <= ReverbSendNearDistanceM) return 1.0f;
    if (DistanceM >= ReverbSendFarDistanceM) return 0.0f;
    const float T = (DistanceM - ReverbSendNearDistanceM)
        / (ReverbSendFarDistanceM - ReverbSendNearDistanceM);
    return 1.0f - T * T * (3.0f - 2.0f * T);
}
inline constexpr IPLint32 BakeThreads=1;
inline constexpr IPLfloat32 IrradianceMinM=.01f;
inline constexpr IPLint32 MaxSources=32;
inline constexpr IPLfloat32 VisRadiusM=.5f;
inline constexpr IPLfloat32 VisThreshold=.5f;
inline constexpr IPLfloat32 VisRangeM=8;
}
