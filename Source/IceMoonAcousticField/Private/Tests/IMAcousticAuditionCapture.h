#pragma once

#include "CoreMinimal.h"

enum class EIMAcousticAuditionStage : uint8
{
	WavePitch, WaveGain, LegacyPresence, Presence, UncompensatedPresence, Spatial, Wet, Nearfield, Count
};

namespace IMAcousticAuditionCapture
{
void Push(EIMAcousticAuditionStage Stage, const float* Left, const float* Right, int32 Frames, int32 SampleRate);
void Contact(int32 FrameInBlock);
// Reject is the EIMAcousticProbeReject code of a block that fell back to the dry path (0 when rendered); it names the cause in the report.
void Spatial(bool bRendered, float DistanceCm, float DistanceGain, const float* AirAbsorption, float Occlusion, uint8 Reject);
void Listener(const FVector& Desired, const FVector& Actual, float SweepRadiusCm, FName BlockingActor);
// Folder that receives every pass artifact of the current audition (stage buffers, event lists, traces, parameter dumps).
FString OutputDirectory();
bool Begin(const FString& Label);
bool End();
}
