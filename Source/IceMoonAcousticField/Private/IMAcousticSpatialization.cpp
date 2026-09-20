#include "IMAcousticSpatialization.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSDKContext.h"
#include "IAudioExtensionPlugin.h"
#include "Features/IModularFeatures.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace
{
FCriticalSection IMDeviceRegistryMutex;
TMap<FAudioDevice*, TWeakPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe>> IMDeviceRegistry;
void* IMPhononLibrary = nullptr;

// Copies worker-result identity plus GT snapshot keys into a trace entry.
// POD stores only; callable from audio callbacks (no log, I/O, wait, lock).
static void IM_FillAcousticProbeFromLatest(IM_AcousticBlockProbe& Out, uint32 Voice,
    uint64 CallbackComponentId, const IM_AcousticVoiceResult& Latest, uint32 Routes, double Now)
{
    Out.Voice = Voice;
    Out.CallbackAudioComponentId = CallbackComponentId;
    Out.ResultAudioComponentId = Latest.AudioComponentId;
    Out.ResultWorldGeneration = Latest.WorldGeneration;
    Out.ResultVoiceGeneration = Latest.Frame.Generation;
    Out.ResultSequence = Latest.Frame.Sequence;
    Out.SnapshotCaptured = Latest.PublishedSeconds;
    Out.ConsumedSeconds = Now;
    Out.AgeMs = (Now - Latest.PublishedSeconds) * 1000.0;
    Out.ListenerX = Latest.Frame.Listener.origin.x;
    Out.ListenerY = Latest.Frame.Listener.origin.y;
    Out.ListenerZ = Latest.Frame.Listener.origin.z;
    Out.DirX = Latest.Frame.ListenerLocalDirection.x;
    Out.DirY = Latest.Frame.ListenerLocalDirection.y;
    Out.DirZ = Latest.Frame.ListenerLocalDirection.z;
    Out.Occlusion = Latest.Frame.Direct.occlusion;
    Out.DistanceGain = Latest.Frame.Direct.distanceAttenuation;
    Out.DirectFlags = static_cast<uint32>(Latest.Frame.Direct.flags);
    Out.Routes = Routes;
    Out.DirectValid = Latest.Frame.DirectValid ? 1 : 0;
    Out.PathValid = Latest.Frame.PathValid ? 1 : 0;
}

constexpr float IMConstantPowerMonoToStereo = 0.7071067811865475f;
// Astra decision A' (2026-09-13): distance factor for the degraded output,
// taken from THIS block's own callback params (listener->emitter, UE cm; the
// field is filled for every spatialized source regardless of the attenuation
// asset). Read-only: no UObject, no SDK, no cross-thread state, no allocation.
// Returns 0 when the distance is unavailable/invalid, so callers keep the
// original constant-power fallback and record the unavailability.
float IMDegradedDistanceGain(const FAudioPluginSourceInputData& In, float& OutDistanceCm)
{
    OutDistanceCm = 0.0f;
    const FSpatializationParams* P = In.SpatializationParams;
    if (!P) { return 0.0f; }
    const float D = P->Distance;
    if (!FMath::IsFinite(D) || D <= 0.0f) { return 0.0f; }
    OutDistanceCm = D;
    // Same shape as the SDK's active model (IPL_DISTANCEATTENUATIONTYPE_DEFAULT,
    // minDistance 1 m in IMAcousticSimulation.cpp) so the degraded level stays
    // continuous with the normal path at the field boundary.
    return 1.0f / FMath::Max(D * 0.01f, 1.0f);
}
// MonoToStereoGain folds both the constant-power mono->stereo factor and, for
// stale/missing/renderer-failed blocks, the live distance factor. The default
// keeps the original zero-behavior degraded output for every other caller.
void IMDryFallback(const FAudioPluginSourceInputData& In, FAudioPluginSourceOutputData& Out,
    float MonoToStereoGain=IMConstantPowerMonoToStereo, uint32 AudibleRoutes=1u)
{
    FMemory::Memzero(Out.AudioBuffer.GetData(), Out.AudioBuffer.Num() * sizeof(float));
    if (!In.AudioBuffer || In.NumChannels != 1 || Out.AudioBuffer.Num() != In.AudioBuffer->Num() * 2) { return; }
    // A stale/missing path result must not bypass an audition route mask. In
    // path-only mode the safe degraded result is silence; direct-route mode
    // keeps the established dry fallback for continuity.
    if ((AudibleRoutes & 1u) == 0u) { return; }
    // Explicit degraded mode: preserve finite dry input at constant power, centered.
    // No stale occlusion/path/reverb is reused and this never counts as a V2 render.
    for (int32 I = 0; I < In.AudioBuffer->Num(); ++I)
    {
        const float Sample = (*In.AudioBuffer)[I];
        const float Safe = FMath::IsFinite(Sample) ? Sample * MonoToStereoGain : 0.0f;
        Out.AudioBuffer[2 * I] = Safe;
        Out.AudioBuffer[2 * I + 1] = Safe;
    }
}

uint32 IMCallbackMicroseconds(uint64 StartCycles, uint64 EndCycles)
{
    if (!StartCycles || EndCycles < StartCycles) { return 0; }
    const double Us = FPlatformTime::ToSeconds64(EndCycles - StartCycles) * 1.e6;
    return static_cast<uint32>(FMath::Clamp(Us, 0.0, double(MAX_uint32)));
}

struct IM_AcousticAudioVoice
{
    TUniquePtr<IM_AcousticAudioRenderer> Renderer;
    uint64 Generation = 0;
    uint64 LastWorldGeneration = 0;
    IM_AcousticVoiceResult Latest;
};

class IM_AcousticSpatialization final : public IAudioSpatialization
{
public:
    explicit IM_AcousticSpatialization(FAudioDevice* InDevice) : Device(InDevice) {}
    ~IM_AcousticSpatialization() override { Shutdown(); }

    void Initialize(const FAudioPluginInitializationParams Params) override
    {
        Shutdown();
        if (!IMPhononLibrary || Params.NumSources == 0 || Params.BufferLength == 0
            || Params.SampleRate == 0) { return; }
        // UE 5.8 FMixerDevice initializes rate/frames/sources but leaves
        // NumOutputChannels unset here. The GT world owner checks the actual mixer
        // device's channels before enrollment; callbacks separately require stereo.
        const IPLContext SharedContext = IM_GetAcousticSDKContext();
        if (!SharedContext) { return; }
        Context = iplContextRetain(SharedContext);
        Bridge = MakeShared<IM_AcousticDeviceBridge, ESPMode::ThreadSafe>();
        Bridge->SampleRate = Params.SampleRate;
        Bridge->BlockFrames = Params.BufferLength;
        Bridge->InitProbes(); // zero probe completion flags before Alive publishes the bridge
        Bridge->Voices.Reserve(Params.NumSources);
        Voices.Reserve(Params.NumSources);
        for (uint32 I = 0; I < Params.NumSources; ++I)
        {
            Bridge->Voices.Add(MakeUnique<IM_AcousticVoiceBridge>());
            Bridge->Voices.Last()->InitDry(Params.BufferLength);
            Voices.Add(MakeUnique<IM_AcousticAudioVoice>());
        }
        Bridge->Alive.store(true, std::memory_order_release);
        FScopeLock Lock(&IMDeviceRegistryMutex);
        IMDeviceRegistry.Add(Device, Bridge);
    }

    void Shutdown() override
    {
        if (Bridge)
        {
            Bridge->Alive.store(false, std::memory_order_release);
            FScopeLock Lock(&IMDeviceRegistryMutex);
            if (auto* Entry = IMDeviceRegistry.Find(Device); Entry && Entry->Pin() == Bridge)
            {
                IMDeviceRegistry.Remove(Device);
            }
        }
        // UE calls Shutdown after source processing is quiescent. Effects own SDK
        // references until Voices is cleared, before releasing the context.
        Voices.Empty();
        Bridge.Reset();
        if (Context) { iplContextRelease(&Context); }
    }

    void OnDeviceShutdown(FAudioDevice*) override { Shutdown(); }
    bool IsSpatializationEffectInitialized() const override { return Context && Bridge; }

    void OnInitSource(uint32 SourceId, const FName&, uint32 NumChannels,
        USpatializationPluginSourceSettingsBase*) override
    {
        if (!Bridge || !Voices.IsValidIndex(SourceId)) { return; }
        auto& Voice = *Voices[SourceId];
        ++Voice.Generation;
        Bridge->Voices[SourceId]->LiveGeneration.store(Voice.Generation,std::memory_order_release);
        Voice.Latest = {};
        Voice.LastWorldGeneration = 0;
        Voice.Renderer.Reset();
        if (NumChannels != 1) { return; }
        // SDK 4.8.1 HRTFDatabase::interpolatedHRTF writes database-owned FFT
        // scratch. Parallel UE source jobs must not share that HRTF object.
        // Creation is source initialization, never steady ProcessAudio work.
        IPLAudioSettings Audio{int(Bridge->SampleRate),int(Bridge->BlockFrames)};
        IPLHRTFSettings Settings{};Settings.type=IPL_HRTFTYPE_DEFAULT;Settings.volume=1.f;
        IPLHRTF VoiceHRTF=nullptr;
        if(iplHRTFCreate(Context,&Audio,&Settings,&VoiceHRTF)!=IPL_STATUS_SUCCESS||!VoiceHRTF)return;
        Voice.Renderer = MakeUnique<IM_AcousticAudioRenderer>();
        const bool Initialized=Voice.Renderer->Initialize(Context,VoiceHRTF,Bridge->SampleRate,Bridge->BlockFrames);
        iplHRTFRelease(&VoiceHRTF); // Renderer retains its exclusive source lease.
        if (!Initialized)
        {
            Voice.Renderer.Reset();
        }
    }

    void OnReleaseSource(uint32 SourceId) override
    {
        if (!Bridge || !Voices.IsValidIndex(SourceId)) { return; }
        auto& Voice = *Voices[SourceId];
        Bridge->Voices[SourceId]->Requests.Push({SourceId, Voice.Generation, 0, false});
        Voice.Renderer.Reset();
        Voice.Latest = {};
        ++Voice.Generation;
        Bridge->Voices[SourceId]->LiveGeneration.store(Voice.Generation,std::memory_order_release);
    }

    void ProcessAudio(const FAudioPluginSourceInputData& In, FAudioPluginSourceOutputData& Out) override
    {
        // UE preallocates output to 2*frames. Fail closed on an unsupported block
        // instead of growing buffers or reading UObjects on the render callback.
        const bool CallbackTrace = Bridge && Bridge->CallbackDiagnosticsEnabled.load(std::memory_order_relaxed);
        const uint64 CallbackStartCycles = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        const uint64 CallbackAudioBlock = CallbackTrace ? Bridge->CallbackAudioBlock.load(std::memory_order_relaxed) : 0;
        const double CallbackStartSeconds = CallbackTrace ? FPlatformTime::Seconds() : 0.0;
        uint64 CallbackDrainEnd = CallbackStartCycles, CallbackValidityEnd = CallbackStartCycles;
        uint64 CallbackDryPushStart = 0, CallbackDryPushEnd = 0;
        uint64 CallbackRenderStart = 0, CallbackRenderEnd = 0;
        uint64 CallbackFallbackStart = 0, CallbackFallbackEnd = 0;
        auto EmitCallbackTrace = [&](IM_AcousticCallbackProbe& Probe, uint64 EndCycles)
        {
            if (!CallbackTrace || !Bridge) { return; }
            Probe.AudioBlock = CallbackAudioBlock;
            Probe.StartSeconds = CallbackStartSeconds;
            Probe.EndSeconds = FPlatformTime::Seconds();
            Probe.ResultDrainUs = IMCallbackMicroseconds(CallbackStartCycles, CallbackDrainEnd);
            Probe.ValidityUs = IMCallbackMicroseconds(CallbackDrainEnd, CallbackValidityEnd);
            Probe.DryPushUs = IMCallbackMicroseconds(CallbackDryPushStart, CallbackDryPushEnd);
            Probe.RenderUs = IMCallbackMicroseconds(CallbackRenderStart, CallbackRenderEnd);
            Probe.FallbackUs = IMCallbackMicroseconds(CallbackFallbackStart, CallbackFallbackEnd);
            Probe.TotalUs = IMCallbackMicroseconds(CallbackStartCycles, EndCycles);
            Bridge->TraceCallback(Probe);
        };
        if (!In.AudioBuffer || !Bridge || !Voices.IsValidIndex(In.SourceId))
        {
            IMDryFallback(In, Out);
            IM_AcousticBlockProbe Bad{}; // Unsupported block shape: no result to join.
            Bad.ConsumedSeconds = FPlatformTime::Seconds();
            Bad.Reject = IM_AcousticProbeReject::BadBlockShape;
            Bad.Fallback = 1;
            if (Bridge) Bridge->RecordPressureReject(1, static_cast<uint32>(FMath::Max(In.SourceId, 0)), Bad.ConsumedSeconds);
            // A' evidence only: this branch writes silence, so the distance is
            // recorded without applying any gain (DegradedGain 1 = unity).
            { float BadDistanceCm=0.0f; IMDegradedDistanceGain(In,BadDistanceCm);
              Bad.CallbackDistanceCm=BadDistanceCm; Bad.DegradedGain=1.0f; }
            if (Bridge) { Bad.RenderedAt = Bridge->RenderedBlocks.load(std::memory_order_relaxed); Bad.RejectedAt = Bridge->RejectedBlocks.load(std::memory_order_relaxed); Bridge->TraceBlock(Bad); }
            IM_AcousticCallbackProbe CallbackProbe{};CallbackProbe.Voice=static_cast<uint32>(FMath::Max(In.SourceId,0));CallbackProbe.Outcome=3;CallbackProbe.RejectDetail=1;
            EmitCallbackTrace(CallbackProbe,CallbackTrace?FPlatformTime::Cycles64():0);
            return;
        }
        auto& Voice = *Voices[In.SourceId];
        IM_AcousticTimingScope Timing(Bridge->ProfilingEnabled.load(std::memory_order_relaxed)?&Bridge->SourceTiming:nullptr,&Bridge->PendingSourceCycles);
        auto& Queues = *Bridge->Voices[In.SourceId];
        Queues.Requests.Push({static_cast<uint32>(In.SourceId), Voice.Generation, In.AudioComponentId, true});
        IM_AcousticVoiceResult Incoming;
        while (Queues.Results.Pop(Incoming))
        {
            if (Incoming.Frame.Generation == Voice.Generation
                && Incoming.AudioComponentId == In.AudioComponentId
                && Incoming.WorldGeneration != 0
                && Incoming.WorldGeneration == Bridge->WorldGeneration.load(std::memory_order_acquire)
                && (Incoming.WorldGeneration != Voice.Latest.WorldGeneration
                    || Incoming.Frame.Sequence > Voice.Latest.Frame.Sequence))
            {
                Voice.Latest = Incoming;
            }
        }
        CallbackDrainEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        if(!Bridge->Enabled.load(std::memory_order_acquire))
        {
            if(Voice.Renderer)Voice.Renderer->Reset();
            CallbackFallbackStart = CallbackTrace ? FPlatformTime::Cycles64() : 0;
            IMDryFallback(In,Out);Bridge->BypassedBlocks.fetch_add(1,std::memory_order_relaxed);
            CallbackFallbackEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
            IM_AcousticBlockProbe Bypass{}; // Whole-V2 bypass: dry reference by design.
            { float BypassDistanceCm=0.0f; IMDegradedDistanceGain(In,BypassDistanceCm);
              Bypass.CallbackDistanceCm=BypassDistanceCm; Bypass.DegradedGain=1.0f; }
            const uint32 BypassRoutes=Bridge->RenderRoutes.load(std::memory_order_relaxed);
            IM_FillAcousticProbeFromLatest(Bypass,static_cast<uint32>(In.SourceId),In.AudioComponentId,Voice.Latest,BypassRoutes,FPlatformTime::Seconds());
            Bypass.Reject=IM_AcousticProbeReject::Bypassed;Bypass.Fallback=1;Bypass.ResetReason=3;
            double BypassInputEnergy=0;for(int32 SI=0;SI<In.AudioBuffer->Num();++SI){const double S=(*In.AudioBuffer)[SI];BypassInputEnergy+=S*S;}
            Bypass.InputEnergy=BypassInputEnergy;Bypass.OutputEnergy=BypassInputEnergy;
            Bypass.RenderedAt=Bridge->RenderedBlocks.load(std::memory_order_relaxed);
            Bypass.RejectedAt=Bridge->RejectedBlocks.load(std::memory_order_relaxed);
            Bridge->TraceBlock(Bypass);
            IM_AcousticCallbackProbe CallbackProbe{};CallbackProbe.Voice=static_cast<uint32>(In.SourceId);CallbackProbe.Outcome=0;
            EmitCallbackTrace(CallbackProbe,CallbackTrace?FPlatformTime::Cycles64():0);return;
        }
        // 250ms is a stale-result safety lease (5 updates at the 20Hz simulation
        // cadence), not an audio smoothing time or a physical propagation delay.
        // RejectDetail records every failed conjunct with identical verdict semantics.
        constexpr double IMParameterLeaseSeconds = 0.250;
        const double IMRejectNow = FPlatformTime::Seconds();
        uint32 IMDetail = 0;
        if (!Voice.Renderer) IMDetail |= 1;
        if (In.NumChannels != 1) IMDetail |= 2;
        if (In.AudioBuffer->Num() != static_cast<int>(Bridge->BlockFrames)) IMDetail |= 4;
        if (Out.AudioBuffer.Num() != 2 * static_cast<int>(Bridge->BlockFrames)) IMDetail |= 8;
        if (Voice.Latest.WorldGeneration == 0) IMDetail |= 16;
        else if (Voice.Latest.WorldGeneration != Bridge->WorldGeneration.load(std::memory_order_acquire)) IMDetail |= 32;
        if (Voice.Latest.Frame.Generation != Voice.Generation) IMDetail |= 64;
        if (Voice.Latest.AudioComponentId != In.AudioComponentId) IMDetail |= 128;
        if (!(Voice.Latest.Frame.DirectValid || Voice.Latest.Frame.PathValid)) IMDetail |= 256;
        if (IMRejectNow < Voice.Latest.PublishedSeconds) IMDetail |= 512;
        else if (IMRejectNow - Voice.Latest.PublishedSeconds > IMParameterLeaseSeconds) IMDetail |= 1024;
        const bool Valid = (IMDetail == 0);
        CallbackValidityEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        if (Valid && CallbackTrace) Bridge->CallbackValidSources.fetch_add(1, std::memory_order_relaxed);
        if (!Valid)
        {
            Bridge->RecordPressureReject(IMDetail, static_cast<uint32>(In.SourceId), IMRejectNow);
            Bridge->InvalidResultBlocks.fetch_add(1,std::memory_order_relaxed);
            const bool RejectStale = Voice.Latest.WorldGeneration != 0 && (IMDetail & 1024) != 0;
            if(RejectStale)
                Bridge->StaleResultBlocks.fetch_add(1,std::memory_order_relaxed);
            else Bridge->MissingResultBlocks.fetch_add(1,std::memory_order_relaxed);
            if (Voice.Renderer) { Voice.Renderer->Reset(); }
            // A' (Astra 2026-09-13): the degraded output keeps this block's live
            // listener-emitter distance factor; 0 = distance unavailable keeps the
            // original constant-power fallback and is recorded as such.
            float RejectDistanceCm=0.0f;
            const float RejectDistanceGain=IMDegradedDistanceGain(In,RejectDistanceCm);
            const uint32 RejectRoutes=Bridge->RenderRoutes.load(std::memory_order_relaxed);
            CallbackFallbackStart = CallbackTrace ? FPlatformTime::Cycles64() : 0;
            IMDryFallback(In, Out, RejectDistanceGain>0.0f
                ? RejectDistanceGain*IMConstantPowerMonoToStereo : IMConstantPowerMonoToStereo,
                RejectRoutes);
            CallbackFallbackEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
            Bridge->RejectedBlocks.fetch_add(1, std::memory_order_relaxed);
            IM_AcousticBlockProbe Reject{}; // Degraded branch: identity, age, mask, occlusion travel with the verdict.
            IM_FillAcousticProbeFromLatest(Reject,static_cast<uint32>(In.SourceId),In.AudioComponentId,Voice.Latest,RejectRoutes,IMRejectNow);
            Reject.Reject=RejectStale?IM_AcousticProbeReject::StaleResult:IM_AcousticProbeReject::MissingResult;
            Reject.Fallback=1;Reject.ResetReason=1;Reject.RejectDetail=IMDetail;
            Reject.CallbackDistanceCm=RejectDistanceCm;Reject.DegradedGain=RejectDistanceGain;
            double RejectInputEnergy=0;for(int32 SI=0;SI<In.AudioBuffer->Num();++SI){const double S=(*In.AudioBuffer)[SI];RejectInputEnergy+=S*S;}
            // Real output measurement (was a copy of the input energy).
            double RejectOutputEnergy=0;for(int32 SI=0;SI<Out.AudioBuffer.Num();++SI){const double S=Out.AudioBuffer[SI];RejectOutputEnergy+=S*S;}
            Reject.InputEnergy=RejectInputEnergy;Reject.OutputEnergy=RejectOutputEnergy;
            Reject.RenderedAt=Bridge->RenderedBlocks.load(std::memory_order_relaxed);
            Reject.RejectedAt=Bridge->RejectedBlocks.load(std::memory_order_relaxed);
            Bridge->TraceBlock(Reject);
            IM_AcousticCallbackProbe CallbackProbe{};CallbackProbe.Voice=static_cast<uint32>(In.SourceId);
            CallbackProbe.Outcome=1;CallbackProbe.RejectDetail=IMDetail;
            EmitCallbackTrace(CallbackProbe,CallbackTrace?FPlatformTime::Cycles64():0);
            return;
        }
        uint8 IMWorldReset = 0;
        if (Voice.LastWorldGeneration != Voice.Latest.WorldGeneration)
        {
            Voice.Renderer->Reset();
            Voice.LastWorldGeneration = Voice.Latest.WorldGeneration;
            IMWorldReset = 2;
        }
        IM_AcousticAudioMetrics Metrics;
        // Capture the same unoccluded mono input for the dedicated reverb submix.
        // Native submix sends happen after spatialization, so those buffers would
        // incorrectly inherit direct occlusion. They are not our reverb input.
        CallbackDryPushStart = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        if(Bridge->ReverbWorldGeneration.load(std::memory_order_acquire)==Voice.Latest.WorldGeneration)
        {
            const FSpatializationParams* ReverbParams = In.SpatializationParams;
            const float DistanceCm = ReverbParams && FMath::IsFinite(ReverbParams->Distance)
                && ReverbParams->Distance > 0.0f ? ReverbParams->Distance : 0.0f;
            const float PushGain = DistanceCm > 0.0f
                ? IM_AcousticRecipe::ReverbSendDistanceGain(DistanceCm * 0.01f) : 0.0f;
            Bridge->PushDryCalls.fetch_add(1,std::memory_order_relaxed);
            // Retain these counters as diagnostics for direct-stem validity;
            // direct invalidity no longer mutes an otherwise valid room send.
            if(!Voice.Latest.Frame.DirectValid)Bridge->PushDryInvalidDirect.fetch_add(1,std::memory_order_relaxed);
            if(PushGain==0.f)Bridge->PushDryZeroGain.fetch_add(1,std::memory_order_relaxed);
            double PushInputEnergy=0;for(int32 SI=0;SI<In.AudioBuffer->Num();++SI){const double S=(*In.AudioBuffer)[SI];PushInputEnergy+=S*S;}
            if(PushInputEnergy>0)Bridge->PushDryInputNonzero.fetch_add(1,std::memory_order_relaxed);
            if(!Queues.PushDry(In.AudioBuffer->GetData(),Bridge->BlockFrames,Voice.Latest.WorldGeneration,
                Voice.Generation,FPlatformTime::Seconds(),PushGain))
                Bridge->DryDroppedBlocks.fetch_add(1,std::memory_order_relaxed);
        }
        CallbackDryPushEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        const uint32 Routes=Bridge->RenderRoutes.load(std::memory_order_relaxed);
        double RenderInputEnergy=0;for(int32 SI=0;SI<In.AudioBuffer->Num();++SI){const double S=(*In.AudioBuffer)[SI];RenderInputEnergy+=S*S;}
        if(RenderInputEnergy>0)Bridge->RenderInputNonzero.fetch_add(1,std::memory_order_relaxed);
        CallbackRenderStart = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        const bool Rendered = Voice.Renderer->Render(In.AudioBuffer->GetData(), Bridge->BlockFrames,
                Voice.Latest.Frame, Out.AudioBuffer.GetData(), nullptr, nullptr, &Metrics, Routes);
        CallbackRenderEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
        if (Rendered)
        {
            Bridge->RenderedBlocks.fetch_add(1, std::memory_order_relaxed);
            if (Metrics.DirectEnergy > 0.0) { Bridge->DirectNonzeroBlocks.fetch_add(1, std::memory_order_relaxed); }
            if (Metrics.PathEnergy > 0.0) { Bridge->PathNonzeroBlocks.fetch_add(1, std::memory_order_relaxed); }
            IM_AcousticBlockProbe Hit{}; // Consumed render: energies join the PCM window by ConsumedSeconds.
            IM_FillAcousticProbeFromLatest(Hit,static_cast<uint32>(In.SourceId),In.AudioComponentId,Voice.Latest,Routes,FPlatformTime::Seconds());
            Hit.Reject=IM_AcousticProbeReject::Accepted;Hit.Fallback=0;Hit.ResetReason=IMWorldReset;
            // A' evidence: measurement only; the accepted path's distance is
            // applied by the SDK, so DegradedGain records unity (not applied).
            { float HitDistanceCm=0.0f; IMDegradedDistanceGain(In,HitDistanceCm);
              Hit.CallbackDistanceCm=HitDistanceCm; Hit.DegradedGain=1.0f; }
            Hit.InputEnergy=RenderInputEnergy;Hit.DirectEnergy=Metrics.DirectEnergy;Hit.PathEnergy=Metrics.PathEnergy;
            double HitOutputEnergy=0;for(int32 SI=0;SI<Out.AudioBuffer.Num();++SI){const double S=Out.AudioBuffer[SI];HitOutputEnergy+=S*S;}
            Hit.OutputEnergy=HitOutputEnergy;
            Hit.RenderedAt=Bridge->RenderedBlocks.load(std::memory_order_relaxed);
            Hit.RejectedAt=Bridge->RejectedBlocks.load(std::memory_order_relaxed);
            Bridge->TraceBlock(Hit);
            IM_AcousticCallbackProbe CallbackProbe{};CallbackProbe.Voice=static_cast<uint32>(In.SourceId);CallbackProbe.Valid=1;CallbackProbe.Outcome=0;
            EmitCallbackTrace(CallbackProbe,CallbackTrace?FPlatformTime::Cycles64():0);
        }
        else
        {
            // A' (Astra 2026-09-13): same degraded distance factor as the stale /
            // missing branch, taken from this block's callback params.
            float FailDistanceCm=0.0f;
            const float FailDistanceGain=IMDegradedDistanceGain(In,FailDistanceCm);
            CallbackFallbackStart = CallbackTrace ? FPlatformTime::Cycles64() : 0;
            IMDryFallback(In, Out, FailDistanceGain>0.0f
                ? FailDistanceGain*IMConstantPowerMonoToStereo : IMConstantPowerMonoToStereo,
                Routes);
            CallbackFallbackEnd = CallbackTrace ? FPlatformTime::Cycles64() : 0;
            Bridge->RendererFailedBlocks.fetch_add(1,std::memory_order_relaxed);
            Bridge->RenderFailures[size_t(Metrics.Failure)].fetch_add(1,std::memory_order_relaxed);
            Bridge->RejectedBlocks.fetch_add(1, std::memory_order_relaxed);
            IM_AcousticBlockProbe Fail{}; // Renderer failure: keep the failure code with the fallback.
            IM_FillAcousticProbeFromLatest(Fail,static_cast<uint32>(In.SourceId),In.AudioComponentId,Voice.Latest,Routes,FPlatformTime::Seconds());
            Fail.Reject=IM_AcousticProbeReject::RendererFailed;Fail.Fallback=1;Fail.ResetReason=4;
            Fail.RenderFailure=uint8(Metrics.Failure);
            Fail.CallbackDistanceCm=FailDistanceCm;Fail.DegradedGain=FailDistanceGain;
            double FailOutputEnergy=0;for(int32 SI=0;SI<Out.AudioBuffer.Num();++SI){const double S=Out.AudioBuffer[SI];FailOutputEnergy+=S*S;}
            Fail.InputEnergy=RenderInputEnergy;Fail.OutputEnergy=FailOutputEnergy;
            Fail.RenderedAt=Bridge->RenderedBlocks.load(std::memory_order_relaxed);
            Fail.RejectedAt=Bridge->RejectedBlocks.load(std::memory_order_relaxed);
            Bridge->TraceBlock(Fail);
            IM_AcousticCallbackProbe CallbackProbe{};CallbackProbe.Voice=static_cast<uint32>(In.SourceId);CallbackProbe.Outcome=2;CallbackProbe.RejectDetail=uint32(Metrics.Failure);
            EmitCallbackTrace(CallbackProbe,CallbackTrace?FPlatformTime::Cycles64():0);
        }
    }

    void OnAllSourcesProcessed() override
    {
        if(!Bridge)return;
        // UE joins source jobs before this hook. Summed callback work is a
        // conservative CPU cost even when individual sources ran in parallel.
        const uint64 Cycles=Bridge->PendingSourceCycles.exchange(0,std::memory_order_acq_rel);
        Bridge->FinalizeCallbackBlock(Cycles);
        if(Cycles&&Bridge->ProfilingEnabled.load(std::memory_order_relaxed))Bridge->SourceBlockTiming.RecordCycles(Cycles);
    }
private:
    FAudioDevice* Device = nullptr; // Identity only; callbacks never dereference it.
    IPLContext Context = nullptr;
    TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
    TArray<TUniquePtr<IM_AcousticAudioVoice>> Voices;
};

class IM_AcousticSpatializationFactory final : public IAudioSpatializationFactory
{
public:
    FString GetDisplayName() override { return TEXT("IceMoon Acoustic Field"); }
    bool SupportsPlatform(const FString& Platform) override { return Platform == TEXT("Windows"); }
    int32 GetMaxSupportedChannels() override { return 1; }
    UClass* GetCustomSpatializationSettingsClass() const override { return UIMAcousticSpatializationSettings::StaticClass(); }
    TAudioSpatializationPtr CreateNewSpatializationPlugin(FAudioDevice* Device) override
    {
        return MakeShared<IM_AcousticSpatialization, ESPMode::ThreadSafe>(Device);
    }
};
TUniquePtr<IM_AcousticSpatializationFactory> IMFactory;
}

TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> IM_FindAcousticDevice(FAudioDevice* Device)
{
    check(IsInGameThread());
    FScopeLock Lock(&IMDeviceRegistryMutex);
    if (const auto* Found = IMDeviceRegistry.Find(Device)) { return Found->Pin(); }
    return nullptr;
}

bool IM_RegisterAcousticSpatialization()
{
    if (IMFactory) { return true; }
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("IceMoonAcousticField"));
    if (!Plugin) { return false; }
    const FString Library = FPaths::Combine(Plugin->GetBaseDir(),
        TEXT("Binaries/ThirdParty/SteamAudio/Win64/phonon.dll"));
    IMPhononLibrary = FPlatformProcess::GetDllHandle(*Library);
    if (!IMPhononLibrary)
    {
        UE_LOG(LogTemp, Error, TEXT("IMACOUSTIC_SDK_LOAD_FAILED %s"), *Library);
        return false;
    }
    IMFactory = MakeUnique<IM_AcousticSpatializationFactory>();
    IModularFeatures::Get().RegisterModularFeature(IAudioSpatializationFactory::GetModularFeatureName(), IMFactory.Get());
    return true;
}

void IM_UnregisterAcousticSpatialization()
{
    if (IMFactory)
    {
        IModularFeatures::Get().UnregisterModularFeature(IAudioSpatializationFactory::GetModularFeatureName(), IMFactory.Get());
        IMFactory.Reset();
    }
    // phonon.dll deliberately remains loaded until process exit: SDK effect leases
    // can outlive module shutdown ordering. Dynamic module reload is disabled.
}
