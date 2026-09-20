#include "IMAcousticMetaSound.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticBakeRecipe.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundParamHelper.h"
#include "MetasoundPrimitives.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#define LOCTEXT_NAMESPACE "IMAcousticMetaSound"

namespace Metasound
{
namespace IMAcousticPins
{
DEFINE_METASOUND_PARAM(Mono, "Mono", "完整单声道输入；不预加遮挡或混响。");
DEFINE_METASOUND_PARAM(Voice, "Voice", "由声场普通入口绑定的 voice slot。");
DEFINE_METASOUND_PARAM(Left, "Left", "左声道。");
DEFINE_METASOUND_PARAM(Right, "Right", "右声道。");
DEFINE_METASOUND_PARAM(Send, "Send", "独立混响发送距离曲线后的原始输入，送共享环境总线。");
}

template<typename ParamsType>
static IM_AcousticMetaSoundContextPtr IMContextFromBuild(const ParamsType& Params)
{
    const FName Key = Frontend::SourceInterface::Environment::DeviceID;
    return Params.Environment.Contains<uint32>(Key)
        ? IM_FindAcousticMetaSoundContext(Params.Environment.GetValue<uint32>(Key)) : nullptr;
}

static IPLHRTF IMCreateOperatorHRTF(const FOperatorSettings& Settings)
{
    IPLContext SDK = IM_GetAcousticSDKContext();
    if (!SDK) return nullptr;
    IPLAudioSettings Audio{int(Settings.GetSampleRate()), Settings.GetNumFramesPerBlock()};
    IPLHRTFSettings HRTFSettings{};
    HRTFSettings.type = IPL_HRTFTYPE_DEFAULT;
    HRTFSettings.volume = 1.0f;
    IPLHRTF HRTF = nullptr;
    return iplHRTFCreate(SDK, &Audio, &HRTFSettings, &HRTF) == IPL_STATUS_SUCCESS ? HRTF : nullptr;
}

class IM_AcousticSourceOperator final : public TExecutableOperator<IM_AcousticSourceOperator>
{
public:
    static const FVertexInterface& GetVertexInterface()
    {
        using namespace IMAcousticPins;
        static const FVertexInterface Interface(
            FInputVertexInterface(TInputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Mono)),
                TInputDataVertex<int32>(METASOUND_GET_PARAM_NAME_AND_METADATA(Voice), -1)),
            FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Left)),
                TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Right)),
                TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Send))));
        return Interface;
    }
    static const FNodeClassMetadata& GetNodeInfo()
    {
        static const FNodeClassMetadata Info = []
        {
            FNodeClassMetadata M;
            M.ClassName = {TEXT("IM"), TEXT("Acoustic Source"), TEXT("Stereo")};
            M.MajorVersion = 1; M.MinorVersion = 0;
            M.DisplayName = LOCTEXT("SourceTitle", "Acoustic Source");
            M.Description = LOCTEXT("SourceDescription", "图内直达声和绕射处理，共享环境发送。");
            M.Author = TEXT("IceMoon"); M.DefaultInterface = GetVertexInterface();
            return M;
        }();
        return Info;
    }
    static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& P, FBuildResults&)
    {
        return MakeUnique<IM_AcousticSourceOperator>(P,
            P.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Mono"), P.OperatorSettings),
            P.InputData.GetOrCreateDefaultDataReadReference<int32>(TEXT("Voice"), P.OperatorSettings));
    }
    IM_AcousticSourceOperator(const FBuildOperatorParams& P, FAudioBufferReadRef InMono, FInt32ReadRef InVoice)
        : Mono(InMono), Voice(InVoice), Left(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)),
          Right(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)), Send(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)),
          Context(IMContextFromBuild(P)), Frames(P.OperatorSettings.GetNumFramesPerBlock())
    {
        Reset(P);
    }
    void Reset(const IOperator::FResetParams& P)
    {
        ReleaseSource();
        Context = IMContextFromBuild(P);
        Frames = P.OperatorSettings.GetNumFramesPerBlock();
        Stereo.SetNumZeroed(Frames * 2);
        if (!Context || Frames != int32(IM_AcousticMetaSoundContext::Frames)
            || P.OperatorSettings.GetSampleRate() != Context->Device->SampleRate) return;
        IPLHRTF HRTF = IMCreateOperatorHRTF(P.OperatorSettings);
        if (HRTF)
        {
            Ready = Renderer.Initialize(IM_GetAcousticSDKContext(), HRTF, int(P.OperatorSettings.GetSampleRate()), Frames);
            iplHRTFRelease(&HRTF);
        }
    }
    ~IM_AcousticSourceOperator() override { ReleaseSource(); }
    void ReleaseSource()
    {
        Renderer.Shutdown();
        if (Context && Slot != INDEX_NONE)
        {
            Context->Device->Voices[Slot]->LiveGeneration.fetch_add(1, std::memory_order_acq_rel);
            Context->SourceConsumers[Slot].store(false, std::memory_order_release);
        }
        Slot = INDEX_NONE; Latest = {}; Generation = 0; AudioId = 0; Ready = false;
    }
    void BindInputs(FInputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Mono"), Mono); D.BindReadVertex(TEXT("Voice"), Voice); }
    void BindOutputs(FOutputVertexInterfaceData& D) override
    { D.BindReadVertex(TEXT("Left"), Left); D.BindReadVertex(TEXT("Right"), Right); D.BindReadVertex(TEXT("Send"), Send); }

    void Execute()
    {
        Left->Zero(); Right->Zero(); Send->Zero();
        if (!Context || Context->Stopped.load(std::memory_order_acquire)) return;
        if (!Ready || Mono->Num() != Frames || Frames != int32(IM_AcousticMetaSoundContext::Frames))
        { Context->InvalidBlocks.fetch_add(1, std::memory_order_relaxed); return; }
        if (Slot == INDEX_NONE)
        {
            if (*Voice < 0 || *Voice >= int32(IM_AcousticMetaSoundContext::MaxVoices)) return;
            const uint64 Id = Context->AudioIds[*Voice].load(std::memory_order_acquire);
            if (!Id) return;
            bool Unowned = false;
            if (!Context->SourceConsumers[*Voice].compare_exchange_strong(Unowned, true, std::memory_order_acq_rel))
            { Context->DuplicateConsumers.fetch_add(1, std::memory_order_relaxed); return; }
            Slot = *Voice; AudioId = Id;
            Generation = Context->Device->Voices[Slot]->LiveGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        auto& Device = *Context->Device;
        auto& Bridge = *Device.Voices[Slot];
        const uint64 ProfileStart = Device.ProfilingEnabled.load(std::memory_order_relaxed)
            ? FPlatformTime::Cycles64() : 0;
        const double Now = FPlatformTime::Seconds();
        // Source operator is the sole request producer and result consumer.
        // Requests renew the existing 250 ms worker lease; a full ring simply
        // already contains newer work than the serial worker can drain.
        Bridge.Requests.Push({uint32(Slot), Generation, AudioId, true});
        IM_AcousticVoiceResult Next;
        for (uint32 Drained = 0; Drained < 8 && Bridge.Results.Pop(Next); ++Drained)
        {
            if (Next.Frame.Generation == Generation && Next.AudioComponentId == AudioId
                && Next.WorldGeneration == Context->Pool->WorldGeneration && Next.Frame.Sequence > Latest.Frame.Sequence) Latest = Next;
        }
        const uint32 Routes = Device.RenderRoutes.load(std::memory_order_relaxed);
        const bool Fresh = Latest.Frame.Sequence && Now >= Latest.PublishedSeconds && Now - Latest.PublishedSeconds <= .25
            && Device.Enabled.load(std::memory_order_acquire);
        IM_AcousticProbeReject ProbeReject = IM_AcousticProbeReject::Accepted;
        if (!Fresh)
        {
            if (!Device.Enabled.load(std::memory_order_acquire)) ProbeReject = IM_AcousticProbeReject::Bypassed;
            else if (!Latest.Frame.Sequence || Now < Latest.PublishedSeconds) ProbeReject = IM_AcousticProbeReject::MissingResult;
            else ProbeReject = IM_AcousticProbeReject::StaleResult;
        }
        bool Rendered = false;
        IM_AcousticAudioMetrics Metrics;
        if (Fresh)
        {
            Rendered = Renderer.Render(Mono->GetData(), Frames, Latest.Frame, Stereo.GetData(), nullptr, nullptr, &Metrics, Routes);
            const float Gain = Context->ReverbSendGains[Slot].load(std::memory_order_relaxed);
            if (FMath::IsFinite(Gain))
                for (int32 I = 0; I < Frames; ++I) Send->GetData()[I] = FMath::IsFinite(Mono->GetData()[I]) ? Mono->GetData()[I] * Gain : 0.f;
        }
        if (!Rendered)
        {
            Renderer.Reset();
            Device.RejectedBlocks.fetch_add(1, std::memory_order_relaxed);
            if (Fresh) ProbeReject = IM_AcousticProbeReject::RendererFailed;
            const float Gain = (Routes & 1u) ? 0.7071067811865475f * Context->DistanceGains[Slot].load(std::memory_order_relaxed) : 0.f;
            for (int32 I = 0; I < Frames; ++I)
            {
                const float Value = FMath::IsFinite(Mono->GetData()[I]) ? Mono->GetData()[I] * Gain : 0.f;
                Stereo[2 * I] = Value; Stereo[2 * I + 1] = Value;
            }
        }
        else Device.RenderedBlocks.fetch_add(1, std::memory_order_relaxed);
        double InputEnergy = 0.0, OutputEnergy = 0.0;
        for (int32 I = 0; I < Frames; ++I)
        {
            Left->GetData()[I] = Stereo[2 * I]; Right->GetData()[I] = Stereo[2 * I + 1];
            const float In = Mono->GetData()[I];
            const float L = Left->GetData()[I], R = Right->GetData()[I];
            if (FMath::IsFinite(In)) InputEnergy += double(In) * double(In);
            if (FMath::IsFinite(L)) OutputEnergy += double(L) * double(L);
            if (FMath::IsFinite(R)) OutputEnergy += double(R) * double(R);
        }
        // The graph is now the production source consumer. Publish the same
        // device-level observability used by the legacy adapter so existing
        // pressure/cross-floor gates measure actual graph blocks rather than
        // mistaking an empty legacy callback ring for silence.
        Device.PushDryCalls.fetch_add(1, std::memory_order_relaxed);
        if (InputEnergy > 1.e-12)
        {
            Device.PushDryInputNonzero.fetch_add(1, std::memory_order_relaxed);
            Device.RenderInputNonzero.fetch_add(1, std::memory_order_relaxed);
        }
        if (Rendered)
        {
            if ((Routes & 1u) && Metrics.DirectEnergy > 1.e-12)
                Device.DirectNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
            if ((Routes & 2u) && Metrics.PathEnergy > 1.e-12)
                Device.PathNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        if (ProfileStart)
        {
            const uint64 Cycles = FPlatformTime::Cycles64() - ProfileStart;
            Device.SourceTiming.RecordCycles(Cycles);
            Device.SourceBlockTiming.RecordCycles(Cycles);
        }
        Context->SourceBlocks.fetch_add(1, std::memory_order_relaxed);
        Context->SourceFrames.fetch_add(Frames, std::memory_order_relaxed);
        if (Slot == 0)
        {
            const uint32 Offset = Context->CapturedSourceFrames.load(std::memory_order_relaxed);
            if (Offset + Frames <= uint32(Context->CapturedSource.Num()))
            {
                FMemory::Memcpy(Context->CapturedSource.GetData() + Offset, Mono->GetData(), Frames * sizeof(float));
                FMemory::Memcpy(Context->CapturedDry.GetData() + 2 * Offset, Stereo.GetData(), Frames * 2 * sizeof(float));
                Context->CapturedSourceFrames.store(Offset + Frames, std::memory_order_release);
            }
            const uint32 Block = Context->CapturedSourceBlockCount.fetch_add(1, std::memory_order_relaxed);
            if (Context->CapturedSourceBlocks.IsValidIndex(Block))
            {
                IM_AcousticBlockProbe& Probe = Context->CapturedSourceBlocks[Block];
                Probe = {};
                Probe.Block = Block;
                Probe.Voice = uint32(Slot);
                Probe.CallbackAudioComponentId = AudioId;
                Probe.ResultAudioComponentId = Latest.AudioComponentId;
                Probe.ResultWorldGeneration = Latest.WorldGeneration;
                Probe.ResultVoiceGeneration = Latest.Frame.Generation;
                Probe.ResultSequence = Latest.Frame.Sequence;
                Probe.SnapshotCaptured = Latest.PublishedSeconds;
                Probe.ConsumedSeconds = Now;
                Probe.AgeMs = Latest.Frame.Sequence ? (Now - Latest.PublishedSeconds) * 1000.0 : 0.0;
                Probe.ListenerX = Latest.Frame.Listener.origin.x;
                Probe.ListenerY = Latest.Frame.Listener.origin.y;
                Probe.ListenerZ = Latest.Frame.Listener.origin.z;
                Probe.DirX = Latest.Frame.ListenerLocalDirection.x;
                Probe.DirY = Latest.Frame.ListenerLocalDirection.y;
                Probe.DirZ = Latest.Frame.ListenerLocalDirection.z;
                Probe.Occlusion = Latest.Frame.Direct.occlusion;
                Probe.DistanceGain = Latest.Frame.Direct.distanceAttenuation;
                Probe.Routes = Routes;
                Probe.DirectValid = Latest.Frame.DirectValid ? 1 : 0;
                Probe.PathValid = Latest.Frame.PathValid ? 1 : 0;
                Probe.Reject = ProbeReject;
                Probe.Fallback = Rendered ? 0 : 1;
                Probe.InputEnergy = InputEnergy;
                Probe.OutputEnergy = OutputEnergy;
                Probe.RenderedAt = Device.RenderedBlocks.load(std::memory_order_relaxed);
                Probe.RejectedAt = Device.RejectedBlocks.load(std::memory_order_relaxed);
                Probe.CallbackDistanceCm = Context->DistanceCm[Slot].load(std::memory_order_relaxed);
                Probe.DegradedGain = (ProbeReject == IM_AcousticProbeReject::StaleResult
                    || ProbeReject == IM_AcousticProbeReject::MissingResult)
                    ? Context->DistanceGains[Slot].load(std::memory_order_relaxed) : 1.f;
                // Publish the completed POD only after all fields and the PCM
                // capture have been written; GT consumes the released count.
                Context->CapturedSourceBlockCount.store(Block + 1, std::memory_order_release);
            }
        }
    }
private:
    FAudioBufferReadRef Mono;
    FInt32ReadRef Voice;
    FAudioBufferWriteRef Left, Right, Send;
    IM_AcousticMetaSoundContextPtr Context;
    IM_AcousticAudioRenderer Renderer;
    TArray<float> Stereo;
    IM_AcousticVoiceResult Latest;
    int32 Frames, Slot = INDEX_NONE;
    uint64 AudioId = 0, Generation = 0;
    bool Ready = false;
};

class IM_AcousticEnvironmentOperator final : public TExecutableOperator<IM_AcousticEnvironmentOperator>
{
public:
    static const FVertexInterface& GetVertexInterface()
    {
        using namespace IMAcousticPins;
        static const FVertexInterface Interface(
            FInputVertexInterface(TInputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Mono))),
            FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Left)),
                TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Right))));
        return Interface;
    }
    static const FNodeClassMetadata& GetNodeInfo()
    {
        static const FNodeClassMetadata Info = []
        {
            FNodeClassMetadata M;
            M.ClassName = {TEXT("IM"), TEXT("Acoustic Environment"), TEXT("Stereo")};
            M.MajorVersion = 1; M.MinorVersion = 0;
            M.DisplayName = LOCTEXT("EnvironmentTitle", "Acoustic Environment");
            M.Description = LOCTEXT("EnvironmentDescription", "唯一环境 IR 消费者：卷积与双耳解码在图内执行。");
            M.Author = TEXT("IceMoon"); M.DefaultInterface = GetVertexInterface();
            return M;
        }();
        return Info;
    }
    static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& P, FBuildResults&)
    {
        return MakeUnique<IM_AcousticEnvironmentOperator>(P,
            P.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Mono"), P.OperatorSettings));
    }
    IM_AcousticEnvironmentOperator(const FBuildOperatorParams& P, FAudioBufferReadRef InMono)
        : Mono(InMono), Left(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)),
          Right(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)), Context(IMContextFromBuild(P)),
          Frames(P.OperatorSettings.GetNumFramesPerBlock())
    {
        Reset(P);
    }
    void Reset(const IOperator::FResetParams& P)
    {
        ReleaseEnvironment();
        Context = IMContextFromBuild(P);
        Frames = P.OperatorSettings.GetNumFramesPerBlock();
        Stereo.SetNumZeroed(Frames * 2);
        if (!Context || Frames != int32(IM_AcousticMetaSoundContext::Frames)
            || P.OperatorSettings.GetSampleRate() != Context->Device->SampleRate) return;
        bool Expected = false;
        Owned = Context->EnvironmentConsumer.compare_exchange_strong(Expected, true, std::memory_order_acq_rel);
        if (!Owned) { Context->DuplicateConsumers.fetch_add(1, std::memory_order_relaxed); return; }
        IPLHRTF HRTF = IMCreateOperatorHRTF(P.OperatorSettings);
        if (HRTF)
        {
            Ready = Renderer.Initialize(IM_GetAcousticSDKContext(), HRTF, int(P.OperatorSettings.GetSampleRate()), Frames,
                int(P.OperatorSettings.GetSampleRate() * IM_AcousticRecipe::ReverbSavedDurationS));
            iplHRTFRelease(&HRTF);
        }
        Context->Device->ReverbEffectInstances.fetch_add(1, std::memory_order_relaxed);
    }
    ~IM_AcousticEnvironmentOperator() override { ReleaseEnvironment(); }
    void ReleaseEnvironment()
    {
        Renderer.Shutdown(); ReleaseCurrent();
        if (Owned)
        {
            Context->Device->ReverbEffectInstances.fetch_sub(1, std::memory_order_relaxed);
            Context->EnvironmentConsumer.store(false, std::memory_order_release);
        }
        Owned = false; Ready = false;
    }
    void BindInputs(FInputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Mono"), Mono); }
    void BindOutputs(FOutputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Left"), Left); D.BindReadVertex(TEXT("Right"), Right); }
    void Execute()
    {
        Left->Zero(); Right->Zero();
        if (!Context || !Owned) return;
        if (Context->Stopped.load(std::memory_order_acquire) || Context->Pool->Stopped.load(std::memory_order_acquire))
        { Renderer.Reset(); ReleaseCurrent(); return; }
        if (!Ready || Mono->Num() != Frames || Frames != int32(IM_AcousticMetaSoundContext::Frames))
        { Context->InvalidBlocks.fetch_add(1, std::memory_order_relaxed); return; }
        auto& Device = *Context->Device;
        const uint64 ProfileStart = Device.ProfilingEnabled.load(std::memory_order_relaxed)
            ? FPlatformTime::Cycles64() : 0;
        const double Now = FPlatformTime::Seconds();
        IM_AcousticReverbSlot* Next = nullptr;
        for (auto& Candidate : Context->Pool->Slots)
        {
            auto Expected = IM_AcousticIRState::Ready;
            if (!Candidate.State.compare_exchange_strong(Expected, IM_AcousticIRState::Reading, std::memory_order_acq_rel)) continue;
            if ((Current && Candidate.Sequence <= Current->Sequence) || (Next && Candidate.Sequence <= Next->Sequence))
            { Candidate.State.store(IM_AcousticIRState::Free, std::memory_order_release); continue; }
            if (Next) Next->State.store(IM_AcousticIRState::Free, std::memory_order_release);
            Next = &Candidate;
        }
        IM_AcousticReverbSlot* Retired = Next ? Current : nullptr;
        if (Next) Current = Next;
        const bool Fresh = Current && Now >= Current->CapturedSeconds && Now - Current->CapturedSeconds <= .25
            && Device.WorldGeneration.load(std::memory_order_acquire) == Context->Pool->WorldGeneration
            && Device.Enabled.load(std::memory_order_acquire)
            && (Device.RenderRoutes.load(std::memory_order_relaxed) & 4u);
        IM_AcousticReverbMetrics Metrics;
        if (Fresh && Renderer.Render(Mono->GetData(), Frames, Current->Params, Current->Listener, Stereo.GetData(), &Metrics))
        {
            Current->Applied = true;
            Context->LastIRSequence.store(Current->Sequence, std::memory_order_release);
            Device.ReverbProcessedBlocks.fetch_add(1, std::memory_order_relaxed);
            const float Gain = (Device.RenderRoutes.load(std::memory_order_relaxed) & 4u) ? Context->WetGain.load(std::memory_order_relaxed) : 0.f;
            double Energy = 0;
            for (int32 I = 0; I < Frames; ++I)
            {
                Left->GetData()[I] = Stereo[2 * I] * Gain; Right->GetData()[I] = Stereo[2 * I + 1] * Gain;
                Energy += double(Left->GetData()[I]) * Left->GetData()[I] + double(Right->GetData()[I]) * Right->GetData()[I];
            }
            if (Metrics.InputEnergy > 0)
            {
                Device.ReverbDryNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
                Device.ReverbDryBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            if (Metrics.AmbisonicsEnergy > 0) Device.ReverbRawNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
            if (Energy > 0) Device.ReverbNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            if (!Fresh) Device.ReverbNotFreshBlocks.fetch_add(1, std::memory_order_relaxed);
            else Device.ReverbRejectedBlocks.fetch_add(1, std::memory_order_relaxed);
            Renderer.Reset(); ReleaseCurrent();
            Context->NoIRBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        // Retire only after Apply, preserving SDK's mutable triple-buffer lease.
        if (Retired) Retired->State.store(IM_AcousticIRState::Free, std::memory_order_release);
        Context->EnvironmentBlocks.fetch_add(1, std::memory_order_relaxed);
        Context->EnvironmentFrames.fetch_add(Frames, std::memory_order_relaxed);
        const uint32 Offset = Context->CapturedEnvironmentFrames.load(std::memory_order_relaxed);
        const uint32 Block = Context->CapturedBlockCount.load(std::memory_order_relaxed);
        if (Offset + Frames <= uint32(Context->CapturedBus.Num()) && Context->CapturedBlocks.IsValidIndex(Block))
        {
            FMemory::Memcpy(Context->CapturedBus.GetData() + Offset, Mono->GetData(), Frames * sizeof(float));
            for (int32 I = 0; I < Frames; ++I)
            {
                Context->CapturedWet[2 * (Offset + I)] = Left->GetData()[I];
                Context->CapturedWet[2 * (Offset + I) + 1] = Right->GetData()[I];
            }
            auto& Trace = Context->CapturedBlocks[Block];
            Trace.Frame = Offset; Trace.Seconds = Now; Trace.Fresh = Fresh;
            if (Current)
            {
                Trace.Sequence = Current->Sequence;
                Trace.ListenerX = Current->Listener.origin.x;
                Trace.ListenerY = Current->Listener.origin.y;
                Trace.ListenerZ = Current->Listener.origin.z;
            }
            Context->CapturedEnvironmentFrames.store(Offset + Frames, std::memory_order_release);
            Context->CapturedBlockCount.store(Block + 1, std::memory_order_release);
        }
        if (ProfileStart)
        {
            const uint64 Cycles = FPlatformTime::Cycles64() - ProfileStart;
            Device.ReverbTiming.RecordCycles(Cycles);
        }
    }
private:
    void ReleaseCurrent() { if (Current) { Current->State.store(IM_AcousticIRState::Free, std::memory_order_release); Current = nullptr; } }
    FAudioBufferReadRef Mono;
    FAudioBufferWriteRef Left, Right;
    IM_AcousticMetaSoundContextPtr Context;
    IM_AcousticReverbRenderer Renderer;
    TArray<float> Stereo;
    IM_AcousticReverbSlot* Current = nullptr;
    int32 Frames;
    bool Owned = false, Ready = false;
};

using IM_AcousticSourceNode = TNodeFacade<IM_AcousticSourceOperator>;
using IM_AcousticEnvironmentNode = TNodeFacade<IM_AcousticEnvironmentOperator>;
METASOUND_REGISTER_NODE(IM_AcousticSourceNode)
METASOUND_REGISTER_NODE(IM_AcousticEnvironmentNode)
}
#undef LOCTEXT_NAMESPACE

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticAudioRenderer.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticCoordinates.h"
#include "IMAcousticSimulation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticMetaSoundBoundaries, "IceMoon.AcousticField.MetaSound.OperatorBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticMetaSoundBoundaries::RunTest(const FString&)
{
    using namespace Metasound;
    constexpr uint32 TestDevice = MAX_uint32 - 1;
    auto Context = IM_CreateAcousticMetaSoundContext(TestDevice, 48000, 123);
    if (!TestNotNull(TEXT("Isolated operator context"), Context.Get())) return false;
    Context->Device->WorldGeneration.store(123);
    Context->Device->Enabled.store(true);
    Context->Device->RenderRoutes.store(7);
    const int32 Slot = IM_RegisterAcousticMetaSoundSource(Context, 42);
    TestEqual(TEXT("First voice slot"), Slot, 0);
    Context->SendGains[0].store(1.f); Context->ReverbSendGains[0].store(1.f); Context->DistanceGains[0].store(.5f);
    Context->CapturedSource.SetNumZeroed(512);
    Context->CapturedDry.SetNumZeroed(1024);
    Context->CapturedWet.SetNumZeroed(4096);
    Context->CapturedBus.SetNumZeroed(2048);
    Context->CapturedBlocks.SetNumZeroed(4);
    const FOperatorSettings Settings(48000, 93.75f);
    FMetasoundEnvironment Environment;
    Environment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
    IM_AcousticSourceNode Node(TEXT("BoundarySource"), FGuid::NewGuid());
    IM_AcousticEnvironmentNode EnvNode(TEXT("BoundaryEnvironment"), FGuid::NewGuid());
    FInputVertexInterfaceData Inputs(IM_AcousticSourceOperator::GetVertexInterface().GetInputInterface());
    FInputVertexInterfaceData EnvInputs(IM_AcousticEnvironmentOperator::GetVertexInterface().GetInputInterface());
    auto Mono = FAudioBufferWriteRef::CreateNew(Settings);
    for (int32 I = 0; I < Mono->Num(); ++I) Mono->GetData()[I] = 1.f;
    auto Voice = FInt32WriteRef::CreateNew(0);
    const FBuildOperatorParams Params(Node, Settings, Inputs, Environment);
    const FBuildOperatorParams EnvParams(EnvNode, Settings, EnvInputs, Environment);
    {
        IM_AcousticSourceOperator Source(Params, Mono, Voice);
        Source.Execute();
        TestEqual(TEXT("No-response source completes one block"), Context->SourceFrames.load(), uint64(512));
        TestTrue(TEXT("No IR preserves route-aware distance-scaled dry fallback"), FMath::IsNearlyEqual(Context->CapturedDry[0], .35355339f, 1.e-6f));
        IM_AcousticSourceOperator DuplicateSource(Params, Mono, Voice);
        DuplicateSource.Execute();
        TestEqual(TEXT("Duplicate source consumer rejected"), Context->DuplicateConsumers.load(), uint64(1));
        IM_AcousticEnvironmentOperator Wet(EnvParams, Mono);
        Wet.Execute();
        TestEqual(TEXT("No-IR environment explicitly degrades"), Context->NoIRBlocks.load(), uint64(1));
        TestEqual(TEXT("No-IR wet is silent"), Context->CapturedWet[0], 0.f);
        IM_AcousticEnvironmentOperator DuplicateWet(EnvParams, Mono);
        DuplicateWet.Execute();
        TestEqual(TEXT("Duplicate environment consumer rejected"), Context->DuplicateConsumers.load(), uint64(2));
        auto& InvalidIR = Context->Pool->Slots[0];
        InvalidIR.Sequence = 1; InvalidIR.CapturedSeconds = FPlatformTime::Seconds();
        InvalidIR.State.store(IM_AcousticIRState::Ready, std::memory_order_release);
        Wet.Execute();
        TestTrue(TEXT("Invalid IR lease released without acknowledgement"), InvalidIR.State.load() == IM_AcousticIRState::Free && !InvalidIR.Applied);
        const FOperatorSettings BadSettings(48000, 100.f);
        const FBuildOperatorParams BadParams(Node, BadSettings, Inputs, Environment);
        auto BadMono = FAudioBufferWriteRef::CreateNew(BadSettings);
        IM_AcousticSourceOperator BadBlock(BadParams, BadMono, Voice);
        BadBlock.Execute();
        TestEqual(TEXT("480-frame graph rejects incompatible 512-frame SDK contract"), Context->InvalidBlocks.load(), uint64(1));
    }
    TestEqual(TEXT("Environment resources returned"), Context->Device->ReverbEffectInstances.load(), int32(0));
    TestFalse(TEXT("Environment lease returned"), Context->EnvironmentConsumer.load());
    TestFalse(TEXT("Source lease returned"), Context->SourceConsumers[0].load());
    IM_StopAcousticMetaSoundContext(Context);
    UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound operator boundaries"), HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
    UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound operator boundaries %s"), HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticReverbSendDistance,
    "IceMoon.AcousticField.MetaSound.ReverbSendDistance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticReverbSendDistance::RunTest(const FString&)
{
    const float Near = IM_AcousticRecipe::ReverbSendDistanceGain(IM_AcousticRecipe::ReverbSendNearDistanceM);
    const float Mid = IM_AcousticRecipe::ReverbSendDistanceGain(16.0f);
    const float Far = IM_AcousticRecipe::ReverbSendDistanceGain(IM_AcousticRecipe::ReverbSendFarDistanceM);
    TestTrue(TEXT("Reverb send is unity inside its near field"), FMath::IsNearlyEqual(Near, 1.0f));
    TestTrue(TEXT("Reverb send remains broader than direct inverse distance at 16m"), Mid > (1.0f / 16.0f));
    TestTrue(TEXT("Reverb send reaches zero at its explicit far boundary"), FMath::IsNearlyZero(Far));
    UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound independent reverb send distance"),
        HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
    UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSoundReverbSendDistance %s"),
        HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
    return !HasAnyErrors();
}

namespace
{
constexpr int32 IMReferenceSampleRate = 48000;
constexpr int32 IMReferenceGraphFrames = 512;
constexpr uint32 IMReferenceSourceToEnvironmentLagFrames = 4608;
constexpr int32 IMReferenceWarmupBlocks = IMReferenceSourceToEnvironmentLagFrames / IMReferenceGraphFrames;
constexpr float IMReferenceInputGain = 0.7f;
constexpr float IMReferenceWetGain = 0.25f;
// The comparison runs the same Steam Audio SDK with independent effect
// instances. The absolute term covers float accumulation and the one
// interleaved stereo sum; the relative term scales with the actual quiet
// signal. A scale floor of 1 would hide the known low-level gain control.
constexpr float IMReferenceAbsoluteTolerance = 1.0e-7f;
constexpr float IMReferenceRelativeTolerance = 2.0e-4f;

struct IMReferenceComparison
{
    bool Equal = false;
    float MaxAbsError = 0.0f;
    float MaxAllowedError = 0.0f;
    int32 MaxErrorIndex = INDEX_NONE;
    FString Summary;
};

IMReferenceComparison IMCompareStereo(const TArray<float>& Actual, const TArray<float>& Expected)
{
    IMReferenceComparison Result;
    if (Actual.Num() != Expected.Num() || Actual.Num() == 0 || (Actual.Num() & 1) != 0)
    {
        Result.Summary = FString::Printf(TEXT("shape actual=%d expected=%d"), Actual.Num(), Expected.Num());
        return Result;
    }
    Result.Equal = true;
    for (int32 I = 0; I < Actual.Num(); ++I)
    {
        const float Difference = FMath::Abs(Actual[I] - Expected[I]);
        const float Scale = FMath::Max(FMath::Abs(Actual[I]), FMath::Abs(Expected[I]));
        const float Allowed = IMReferenceAbsoluteTolerance + IMReferenceRelativeTolerance * Scale;
        if (Difference > Result.MaxAbsError)
        {
            Result.MaxAbsError = Difference;
            Result.MaxAllowedError = Allowed;
            Result.MaxErrorIndex = I;
        }
        if (Difference > Allowed) Result.Equal = false;
    }
    Result.Summary = FString::Printf(TEXT("max_abs=%g max_allowed_at_max=%g index=%d"),
        Result.MaxAbsError, Result.MaxAllowedError, Result.MaxErrorIndex);
    return Result;
}

float IMStereoEnergy(const TArray<float>& Samples)
{
    double Energy = 0.0;
    for (const float Sample : Samples) Energy += double(Sample) * double(Sample);
    return float(Energy);
}

float IMStereoChannelAsymmetry(const TArray<float>& Samples)
{
    float MaxDifference = 0.0f;
    for (int32 I = 0; I + 1 < Samples.Num(); I += 2)
        MaxDifference = FMath::Max(MaxDifference, FMath::Abs(Samples[I] - Samples[I + 1]));
    return MaxDifference;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticMetaSoundSDKReference,
    "IceMoon.AcousticField.MetaSound.SDKReference", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticMetaSoundSDKReference::RunTest(const FString&)
{
    using namespace Metasound;
    constexpr uint32 TestDevice = MAX_uint32 - 2;
    constexpr uint64 TestAudioId = 42;
    constexpr uint64 TestEpoch = 0x5344325245464552ull; // frozen S2 reference epoch
    const FOperatorSettings Settings(IMReferenceSampleRate, 93.75f);
    const FString BakePath = TEXT("/IceMoonAcousticField/Bakes/IM_IM_V2Audition_51EB95704ADA37C752DC909BA5B90830.IM_IM_V2Audition_51EB95704ADA37C752DC909BA5B90830");
    const FString PCMPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundClosure/runtime/source.f32"));

    auto Fail = [this](const FString& Reason)
    {
        AddError(Reason);
        UE_LOG(LogTemp, Error, TEXT("IMLogs MetaSoundSDKReference FAIL %s"), *Reason);
        return false;
    };

    UIMAcousticBakeAsset* Bake = LoadObject<UIMAcousticBakeAsset>(nullptr, *BakePath);
    if (!Bake || Bake->SceneData.IsEmpty() || Bake->ProbeData.IsEmpty())
        return Fail(TEXT("Frozen current bake asset could not be loaded with scene and probe payloads."));

    TArray<FVector4> ProbePreview;
    FVector ProbeOrigin = FVector::ZeroVector;
    FString PreviewError;
    if (!Bake->GetProbePreview(Bake->WorldPackage, Bake->SceneFingerprint, ProbePreview, ProbeOrigin, PreviewError)
        || ProbePreview.IsEmpty())
        return Fail(FString::Printf(TEXT("Frozen bake probe coverage readback failed: %s"), *PreviewError));

    IM_AcousticBakeData BakeData;
    BakeData.Scene.assign(Bake->SceneData.GetData(), Bake->SceneData.GetData() + Bake->SceneData.Num());
    BakeData.ProbeBatch.assign(Bake->ProbeData.GetData(), Bake->ProbeData.GetData() + Bake->ProbeData.Num());
    BakeData.CoverageProbes.reserve(ProbePreview.Num());
    for (const FVector4& Probe : ProbePreview)
        BakeData.CoverageProbes.push_back({{float(Probe.X), float(Probe.Y), float(Probe.Z)}, float(Probe.W)});

    IM_AcousticSimulation Simulation;
    std::string SimulationError;
    if (!Simulation.SetPathingOptions(IM_AcousticPathingOptions::DefaultHybrid(), SimulationError))
        return Fail(FString::Printf(TEXT("SDK reference pathing options failed: %s"), ANSI_TO_TCHAR(SimulationError.c_str())));
    if (!Simulation.Load(BakeData, IMReferenceSampleRate, IMReferenceGraphFrames, SimulationError))
        return Fail(FString::Printf(TEXT("SDK reference current bake load failed: %s"), ANSI_TO_TCHAR(SimulationError.c_str())));

    const FVector SourceUE(550.0f, 300.0f, 150.0f);
    const FVector RequestedA(750.0f, 300.0f, 150.0f);
    const FVector RequestedB(1350.0f, 300.0f, 150.0f);
    struct IMReferenceListenerCandidate
    {
        FString Name;
        FVector Position = FVector::ZeroVector;
    };
    TArray<IMReferenceListenerCandidate> ListenerCandidates;
    auto AddListenerCandidate = [&ListenerCandidates](const TCHAR* Name, const FVector& Position)
    {
        IMReferenceListenerCandidate Candidate;
        Candidate.Name = Name;
        Candidate.Position = Position;
        ListenerCandidates.Add(MoveTemp(Candidate));
    };
    AddListenerCandidate(TEXT("A"), RequestedA);
    AddListenerCandidate(TEXT("B"), RequestedB);
    TArray<FVector> CoverageCandidates;
    CoverageCandidates.Reserve(ProbePreview.Num());
    for (const FVector4& Probe : ProbePreview)
    {
        CoverageCandidates.Add(IMFromSDKPosition(IPLVector3{float(Probe.X), float(Probe.Y), float(Probe.Z)}, ProbeOrigin));
    }
    CoverageCandidates.Sort([&](const FVector& Left, const FVector& Right)
    {
        const float LeftDistance = FMath::Min((Left - RequestedA).SizeSquared(), (Left - RequestedB).SizeSquared());
        const float RightDistance = FMath::Min((Right - RequestedA).SizeSquared(), (Right - RequestedB).SizeSquared());
        return LeftDistance < RightDistance;
    });
    for (const FVector& Candidate : CoverageCandidates)
    {
        if (ListenerCandidates.Num() >= 34) break;
        AddListenerCandidate(TEXT("coverage_probe"), Candidate);
    }

    IM_AcousticAudioFrame FrozenFrame;
    IPLCoordinateSpace3 SourceSpace = IMToSDKSpace(FTransform(FQuat::Identity, SourceUE), ProbeOrigin);
    IPLCoordinateSpace3 ListenerSpace{};
    FVector ListenerUE = FVector::ZeroVector;
    FString SelectedListenerName;
    FString CandidateDiagnostics;
    for (int32 CandidateIndex = 0; CandidateIndex < ListenerCandidates.Num(); ++CandidateIndex)
    {
        const IMReferenceListenerCandidate& Candidate = ListenerCandidates[CandidateIndex];
        const IPLCoordinateSpace3 CandidateSpace = IMToSDKSpace(FTransform(FQuat::Identity, Candidate.Position), ProbeOrigin);
        IM_AcousticAudioFrame CandidateFrame;
        std::string CandidateError;
        const uint64 CandidateKey = TestAudioId + 1000ull + uint64(CandidateIndex);
        const bool Evaluated = Simulation.Evaluate(CandidateKey, 1, SourceSpace, CandidateSpace, CandidateFrame, CandidateError);
        Simulation.Remove(CandidateKey);
        const bool Valid = Evaluated && CandidateFrame.DirectValid && CandidateFrame.PathValid;
        if (CandidateDiagnostics.Len() < 4000)
        {
            CandidateDiagnostics += FString::Printf(TEXT("%s[%s,d=%d,p=%d,occ=%g];"), *Candidate.Name,
                Valid ? TEXT("valid") : TEXT("reject"), CandidateFrame.DirectValid ? 1 : 0,
                CandidateFrame.PathValid ? 1 : 0, CandidateFrame.Direct.occlusion);
        }
        if (Valid)
        {
            FrozenFrame = CandidateFrame;
            ListenerSpace = CandidateSpace;
            ListenerUE = Candidate.Position;
            SelectedListenerName = Candidate.Name;
            break;
        }
    }
    if (SelectedListenerName.IsEmpty())
        return Fail(FString::Printf(TEXT("Frozen current snapshot did not produce both valid direct and path stems after A/B and coverage-probe candidates: %s origin_cm=(%g,%g,%g)"),
            *CandidateDiagnostics, ProbeOrigin.X, ProbeOrigin.Y, ProbeOrigin.Z));
    TArray<uint8> RawPCM;
    if (!FFileHelper::LoadFileToArray(RawPCM, *PCMPath)
        || RawPCM.Num() < (IMReferenceSourceToEnvironmentLagFrames + IMReferenceGraphFrames) * int32(sizeof(float)))
        return Fail(FString::Printf(TEXT("Frozen PCM input is missing or shorter than the declared 4608-frame adaptation offset plus one 512-frame block: %s"), *PCMPath));
    TArray<float> Mono;
    Mono.SetNumUninitialized(IMReferenceGraphFrames);
    for (int32 I = 0; I < IMReferenceGraphFrames; ++I)
    {
        float Sample = 0.0f;
        const int32 SourceFrame = int32(IMReferenceSourceToEnvironmentLagFrames) + I;
        FMemory::Memcpy(&Sample, RawPCM.GetData() + SourceFrame * sizeof(float), sizeof(float));
        if (!FMath::IsFinite(Sample)) return Fail(TEXT("Frozen PCM input contains a non-finite sample."));
        Mono[I] = Sample * IMReferenceInputGain;
    }
    if (IMStereoEnergy(Mono) <= 0.0f)
        return Fail(TEXT("Frozen PCM input is silent in the declared phase window."));

    auto MeasureCandidateEnergy = [&](const IM_AcousticAudioFrame& CandidateFrame, const FVector& CandidatePosition,
        double& OutDirectEnergy, double& OutPathEnergy, double& OutWetEnergy) -> bool
    {
        OutDirectEnergy = 0.0;
        OutPathEnergy = 0.0;
        OutWetEnergy = 0.0;
        auto RenderSourceRoute = [&](uint32 Routes, double& OutEnergy) -> bool
        {
            IPLHRTF HRTF = IMCreateOperatorHRTF(Settings);
            if (!HRTF) return false;
            IM_AcousticAudioRenderer Renderer;
            const bool Initialized = Renderer.Initialize(IM_GetAcousticSDKContext(), HRTF,
                IMReferenceSampleRate, IMReferenceGraphFrames);
            iplHRTFRelease(&HRTF);
            if (!Initialized) return false;
            TArray<float> Stereo;
            Stereo.SetNumZeroed(IMReferenceGraphFrames * 2);
            for (int32 Warmup = 0; Warmup < IMReferenceWarmupBlocks; ++Warmup)
            {
                if (!Renderer.Render(Mono.GetData(), IMReferenceGraphFrames, CandidateFrame,
                    Stereo.GetData(), nullptr, nullptr, nullptr, Routes)) return false;
            }
            OutEnergy = double(IMStereoEnergy(Stereo));
            return true;
        };
        if (!RenderSourceRoute(1, OutDirectEnergy) || !RenderSourceRoute(2, OutPathEnergy)) return false;

        IM_AcousticReverbSlot CandidateIR;
        CandidateIR.State.store(IM_AcousticIRState::Writing, std::memory_order_release);
        std::string CandidateReverbError;
        if (!Simulation.EvaluateReverb(CandidateIR, CandidateFrame.Listener, CandidateReverbError)) return false;
        if (!CandidateIR.Params.ir || CandidateIR.Params.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
            || CandidateIR.Params.numChannels != IM_AcousticAudioFrame::Coefficients) return false;
        IPLHRTF HRTF = IMCreateOperatorHRTF(Settings);
        if (!HRTF) return false;
        IM_AcousticReverbRenderer Renderer;
        const bool Initialized = Renderer.Initialize(IM_GetAcousticSDKContext(), HRTF,
            IMReferenceSampleRate, IMReferenceGraphFrames, CandidateIR.Params.irSize);
        iplHRTFRelease(&HRTF);
        if (!Initialized) return false;
        TArray<float> Send;
        Send.SetNumUninitialized(IMReferenceGraphFrames);
        const float ReverbSendGain = IM_AcousticRecipe::ReverbSendDistanceGain(
            FVector::Distance(SourceUE, CandidatePosition) * 0.01f);
        for (int32 I = 0; I < IMReferenceGraphFrames; ++I)
            Send[I] = Mono[I] * ReverbSendGain;
        TArray<float> Wet;
        Wet.SetNumZeroed(IMReferenceGraphFrames * 2);
        for (int32 Warmup = 0; Warmup < IMReferenceWarmupBlocks; ++Warmup)
        {
            if (!Renderer.Render(Send.GetData(), IMReferenceGraphFrames, CandidateIR.Params,
                CandidateIR.Listener, Wet.GetData())) return false;
        }
        OutWetEnergy = double(IMStereoEnergy(Wet)) * double(IMReferenceWetGain) * double(IMReferenceWetGain);
        // The current V2 fixture intentionally exercises the blocked-direct
        // path: Steam Audio's occlusion gain is zero while the validated
        // alternate path and listener IR remain audible. Direct is still
        // compared and must stay zero for this frozen physical snapshot.
        return OutPathEnergy > 1.0e-12 && OutWetEnergy > 1.0e-12;
    };

    FString AudibleCandidateDiagnostics;
    bool AudibleCandidateFound = false;
    for (int32 CandidateIndex = 0; CandidateIndex < ListenerCandidates.Num(); ++CandidateIndex)
    {
        const IMReferenceListenerCandidate& Candidate = ListenerCandidates[CandidateIndex];
        const IPLCoordinateSpace3 CandidateSpace = IMToSDKSpace(FTransform(FQuat::Identity, Candidate.Position), ProbeOrigin);
        IM_AcousticAudioFrame CandidateFrame;
        std::string CandidateError;
        const uint64 CandidateKey = TestAudioId + 2000ull + uint64(CandidateIndex);
        const bool Evaluated = Simulation.Evaluate(CandidateKey, 1, SourceSpace, CandidateSpace, CandidateFrame, CandidateError);
        Simulation.Remove(CandidateKey);
        if (!Evaluated || !CandidateFrame.DirectValid || !CandidateFrame.PathValid)
        {
            AudibleCandidateDiagnostics += FString::Printf(TEXT("%s[invalid];"), *Candidate.Name);
            continue;
        }
        double DirectEnergy = 0.0, PathEnergy = 0.0, WetEnergy = 0.0;
        const bool Audible = MeasureCandidateEnergy(CandidateFrame, Candidate.Position, DirectEnergy, PathEnergy, WetEnergy);
        AudibleCandidateDiagnostics += FString::Printf(TEXT("%s[audible=%d,d=%g,p=%g,w=%g];"), *Candidate.Name,
            Audible ? 1 : 0, DirectEnergy, PathEnergy, WetEnergy);
        if (Audible)
        {
            FrozenFrame = CandidateFrame;
            ListenerSpace = CandidateSpace;
            ListenerUE = Candidate.Position;
            SelectedListenerName = Candidate.Name;
            AudibleCandidateFound = true;
            UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKReference listener_candidate=%s listener_ue=(%g,%g,%g) listener_sdk=(%g,%g,%g) candidate_energy direct=%g path=%g wet=%g"),
                *SelectedListenerName, ListenerUE.X, ListenerUE.Y, ListenerUE.Z,
                ListenerSpace.origin.x, ListenerSpace.origin.y, ListenerSpace.origin.z,
                DirectEnergy, PathEnergy, WetEnergy);
            break;
        }
    }
    if (!AudibleCandidateFound)
        return Fail(FString::Printf(TEXT("Current bake has no candidate with audible direct/path/wet reference energy after A/B and coverage-probe scan: %s"),
            *AudibleCandidateDiagnostics));
    FrozenFrame.Sequence = 1;

    auto Context = IM_CreateAcousticMetaSoundContext(TestDevice, IMReferenceSampleRate, TestEpoch);
    if (!Context) return Fail(TEXT("SDK reference MetaSound context could not be created."));
    const int32 Slot = IM_RegisterAcousticMetaSoundSource(Context, TestAudioId);
    if (Slot != 0) { IM_StopAcousticMetaSoundContext(Context); return Fail(TEXT("SDK reference did not acquire frozen voice slot zero.")); }
    Context->Device->WorldGeneration.store(TestEpoch, std::memory_order_release);
    Context->Device->Enabled.store(true, std::memory_order_release);
    Context->Device->RenderRoutes.store(7, std::memory_order_release);
    Context->SendGains[0].store(1.0f, std::memory_order_release);
    const float FrozenReverbSendDistanceGain = IM_AcousticRecipe::ReverbSendDistanceGain(
        FVector::Distance(SourceUE, ListenerUE) * 0.01f);
    Context->ReverbSendGains[0].store(FrozenReverbSendDistanceGain, std::memory_order_release);
    Context->DistanceGains[0].store(1.0f, std::memory_order_release);
    Context->WetGain.store(IMReferenceWetGain, std::memory_order_release);

    IM_AcousticReverbSlot& FrozenIR = Context->Pool->Slots[0];
    FrozenIR.State.store(IM_AcousticIRState::Writing, std::memory_order_release);
    if (!Simulation.EvaluateReverb(FrozenIR, FrozenFrame.Listener, SimulationError))
    {
        IM_StopAcousticMetaSoundContext(Context);
        return Fail(FString::Printf(TEXT("Frozen current IR evaluation failed: %s"), ANSI_TO_TCHAR(SimulationError.c_str())));
    }
    FrozenIR.Sequence = 1;
    FrozenIR.Applied = false;
    FrozenIR.CapturedSeconds = FPlatformTime::Seconds();
    FrozenIR.State.store(IM_AcousticIRState::Ready, std::memory_order_release);
    if (!FrozenIR.Params.ir || FrozenIR.Params.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
        || FrozenIR.Params.numChannels != IM_AcousticAudioFrame::Coefficients)
    {
        IM_StopAcousticMetaSoundContext(Context);
        return Fail(TEXT("Frozen current IR does not satisfy the order-1 four-channel convolution contract."));
    }

    TArray<float> ExpectedSend;
    ExpectedSend.SetNumUninitialized(IMReferenceGraphFrames);
    const float ExpectedSendGain = Context->ReverbSendGains[0].load(std::memory_order_acquire);
    if (!FMath::IsFinite(ExpectedSendGain) || ExpectedSendGain <= 0.0f)
    {
        IM_StopAcousticMetaSoundContext(Context);
        return Fail(TEXT("Frozen current snapshot has no positive finite environmental send gain."));
    }
    for (int32 I = 0; I < IMReferenceGraphFrames; ++I) ExpectedSend[I] = Mono[I] * ExpectedSendGain;

    auto RenderReferenceSource = [&](const IM_AcousticAudioFrame& Frame, uint32 Routes, TArray<float>& Out) -> bool
    {
        IPLHRTF HRTF = IMCreateOperatorHRTF(Settings);
        if (!HRTF) return false;
        IM_AcousticAudioRenderer Renderer;
        const bool Initialized = Renderer.Initialize(IM_GetAcousticSDKContext(), HRTF, IMReferenceSampleRate, IMReferenceGraphFrames);
        iplHRTFRelease(&HRTF);
        if (!Initialized) return false;
        Out.SetNumZeroed(IMReferenceGraphFrames * 2);
        for (int32 Warmup = 0; Warmup < IMReferenceWarmupBlocks; ++Warmup)
        {
            if (!Renderer.Render(Mono.GetData(), IMReferenceGraphFrames, Frame, Out.GetData(), nullptr, nullptr, nullptr, Routes))
                return false;
        }
        return true;
    };

    auto RenderReferenceWet = [&](TArray<float>& Out) -> bool
    {
        IM_AcousticReverbSlot ReferenceIR;
        ReferenceIR.State.store(IM_AcousticIRState::Writing, std::memory_order_release);
        std::string ReferenceReverbError;
        if (!Simulation.EvaluateReverb(ReferenceIR, FrozenFrame.Listener, ReferenceReverbError)) return false;
        if (!ReferenceIR.Params.ir || ReferenceIR.Params.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
            || ReferenceIR.Params.numChannels != IM_AcousticAudioFrame::Coefficients) return false;
        IPLHRTF HRTF = IMCreateOperatorHRTF(Settings);
        if (!HRTF) return false;
        IM_AcousticReverbRenderer Renderer;
        const bool Initialized = Renderer.Initialize(IM_GetAcousticSDKContext(), HRTF, IMReferenceSampleRate,
            IMReferenceGraphFrames, ReferenceIR.Params.irSize);
        iplHRTFRelease(&HRTF);
        if (!Initialized) return false;
        TArray<float> RawWet;
        RawWet.SetNumZeroed(IMReferenceGraphFrames * 2);
        if (!Renderer.Render(ExpectedSend.GetData(), IMReferenceGraphFrames, ReferenceIR.Params,
            ReferenceIR.Listener, RawWet.GetData())) return false;
        Out.SetNumUninitialized(RawWet.Num());
        for (int32 Warmup = 0; Warmup < IMReferenceWarmupBlocks; ++Warmup)
        {
            if (Warmup > 0 && !Renderer.Render(ExpectedSend.GetData(), IMReferenceGraphFrames, ReferenceIR.Params,
                ReferenceIR.Listener, RawWet.GetData())) return false;
        }
        for (int32 I = 0; I < RawWet.Num(); ++I) Out[I] = RawWet[I] * IMReferenceWetGain;
        return true;
    };

    auto RunGraph = [&](uint32 Routes, TArray<float>& OutSource, TArray<float>& OutWet,
        TArray<float>& OutSend, FString& OutError) -> bool
    {
        OutError.Reset();
        Context->Device->RenderRoutes.store(Routes, std::memory_order_release);
        FrozenIR.Applied = false;
        FrozenIR.State.store(IM_AcousticIRState::Writing, std::memory_order_release);
        std::string GraphReverbError;
        if (!Simulation.EvaluateReverb(FrozenIR, FrozenFrame.Listener, GraphReverbError))
        {
            OutError = FString::Printf(TEXT("fresh frozen IR refresh failed: %s"), ANSI_TO_TCHAR(GraphReverbError.c_str()));
            return false;
        }
        FrozenIR.Sequence = 1;
        FrozenIR.CapturedSeconds = FPlatformTime::Seconds();
        FrozenIR.State.store(IM_AcousticIRState::Ready, std::memory_order_release);
        IM_AcousticAudioFrame Frame = FrozenFrame;
        Frame.Generation = Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire) + 1;
        Frame.Sequence = 1;
        IM_AcousticVoiceResult Result;
        Result.Frame = Frame;
        Result.AudioComponentId = TestAudioId;
        Result.WorldGeneration = TestEpoch;
        Result.PublishedSeconds = FPlatformTime::Seconds();
        if (!Context->Device->Voices[0]->Results.Push(Result))
        {
            OutError = TEXT("frozen source result ring was not empty");
            return false;
        }

        FMetasoundEnvironment MetaEnvironment;
        MetaEnvironment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
        IM_AcousticSourceNode SourceNode(TEXT("SDKReferenceSource"), FGuid::NewGuid());
        IM_AcousticEnvironmentNode EnvironmentNode(TEXT("SDKReferenceEnvironment"), FGuid::NewGuid());
        FInputVertexInterfaceData Inputs(IM_AcousticSourceOperator::GetVertexInterface().GetInputInterface());
        FInputVertexInterfaceData EnvironmentInputs(IM_AcousticEnvironmentOperator::GetVertexInterface().GetInputInterface());
        auto MonoReference = FAudioBufferWriteRef::CreateNew(Settings);
        for (int32 I = 0; I < IMReferenceGraphFrames; ++I) MonoReference->GetData()[I] = Mono[I];
        auto Voice = FInt32WriteRef::CreateNew(0);
        const FBuildOperatorParams SourceParams(SourceNode, Settings, Inputs, MetaEnvironment);
        const FBuildOperatorParams EnvironmentParams(EnvironmentNode, Settings, EnvironmentInputs, MetaEnvironment);
        IM_AcousticSourceOperator Source(SourceParams, MonoReference, Voice);
        FOutputVertexInterfaceData SourceOutputs(IM_AcousticSourceOperator::GetVertexInterface().GetOutputInterface());
        Source.BindOutputs(SourceOutputs);
        const FAudioBufferReadRef Send = SourceOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Send"));
        const FAudioBufferReadRef SourceLeft = SourceOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Left"));
        const FAudioBufferReadRef SourceRight = SourceOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Right"));
        IM_AcousticEnvironmentOperator Environment(EnvironmentParams, Send);
        FOutputVertexInterfaceData EnvironmentOutputs(IM_AcousticEnvironmentOperator::GetVertexInterface().GetOutputInterface());
        Environment.BindOutputs(EnvironmentOutputs);
        for (int32 Warmup = 0; Warmup < IMReferenceWarmupBlocks; ++Warmup)
        {
            const uint64 RejectedBefore = Context->Device->RejectedBlocks.load(std::memory_order_acquire);
            Source.Execute();
            if (Context->Device->RejectedBlocks.load(std::memory_order_acquire) != RejectedBefore)
            {
                OutError = TEXT("valid frozen source frame was rejected by the graph operator");
                return false;
            }
            Environment.Execute();
        }
        const FAudioBufferReadRef WetLeft = EnvironmentOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Left"));
        const FAudioBufferReadRef WetRight = EnvironmentOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Right"));
        OutSource.SetNumUninitialized(IMReferenceGraphFrames * 2);
        OutWet.SetNumUninitialized(IMReferenceGraphFrames * 2);
        OutSend.SetNumUninitialized(IMReferenceGraphFrames);
        for (int32 I = 0; I < IMReferenceGraphFrames; ++I)
        {
            OutSource[2 * I] = SourceLeft->GetData()[I];
            OutSource[2 * I + 1] = SourceRight->GetData()[I];
            OutWet[2 * I] = WetLeft->GetData()[I];
            OutWet[2 * I + 1] = WetRight->GetData()[I];
            OutSend[I] = Send->GetData()[I];
        }
        return true;
    };

    // Identity negative control: a frozen response from A must not be accepted
    // after the source/world/voice identity has changed on the path to B.  The
    // source operator is the consumer-side authority, so inject one malformed
    // result at a time and require a degraded block without a rendered block.
    auto RunIdentityNegative = [&](const TCHAR* Label, uint64 ResultAudioId,
        uint64 ResultWorldGeneration, bool bWrongVoiceGeneration, FString& OutError) -> bool
    {
        OutError.Reset();
        Context->Device->RenderRoutes.store(7, std::memory_order_release);
        const uint64 RejectedBefore = Context->Device->RejectedBlocks.load(std::memory_order_acquire);
        const uint64 RenderedBefore = Context->Device->RenderedBlocks.load(std::memory_order_acquire);
        const uint64 ExpectedGeneration = Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire) + 1;

        IM_AcousticVoiceResult Mismatched;
        Mismatched.Frame = FrozenFrame;
        Mismatched.Frame.Generation = bWrongVoiceGeneration ? ExpectedGeneration + 1 : ExpectedGeneration;
        Mismatched.Frame.Sequence = 9000 + ExpectedGeneration;
        Mismatched.AudioComponentId = ResultAudioId;
        Mismatched.WorldGeneration = ResultWorldGeneration;
        Mismatched.PublishedSeconds = FPlatformTime::Seconds();
        if (!Context->Device->Voices[0]->Results.Push(Mismatched))
        {
            OutError = FString::Printf(TEXT("%s result ring was not empty before identity injection"), Label);
            return false;
        }

        FMetasoundEnvironment IdentityEnvironment;
        IdentityEnvironment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
        IM_AcousticSourceNode IdentityNode(FName(*FString::Printf(TEXT("SDKIdentityNegative_%s"), Label)), FGuid::NewGuid());
        FInputVertexInterfaceData IdentityInputs(IM_AcousticSourceOperator::GetVertexInterface().GetInputInterface());
        auto IdentityMono = FAudioBufferWriteRef::CreateNew(Settings);
        for (int32 I = 0; I < IMReferenceGraphFrames; ++I) IdentityMono->GetData()[I] = Mono[I];
        auto IdentityVoice = FInt32WriteRef::CreateNew(0);
        const FBuildOperatorParams IdentityParams(IdentityNode, Settings, IdentityInputs, IdentityEnvironment);
        IM_AcousticSourceOperator IdentitySource(IdentityParams, IdentityMono, IdentityVoice);
        FOutputVertexInterfaceData IdentityOutputs(IM_AcousticSourceOperator::GetVertexInterface().GetOutputInterface());
        IdentitySource.BindOutputs(IdentityOutputs);
        const FAudioBufferReadRef IdentityLeft = IdentityOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Left"));
        IdentitySource.Execute();

        const uint64 RejectedAfter = Context->Device->RejectedBlocks.load(std::memory_order_acquire);
        const uint64 RenderedAfter = Context->Device->RenderedBlocks.load(std::memory_order_acquire);
        const bool Rejected = RejectedAfter == RejectedBefore + 1 && RenderedAfter == RenderedBefore;
        bool FiniteOutput = true;
        if (FiniteOutput)
        {
            for (int32 I = 0; I < IdentityLeft->Num(); ++I)
            {
                if (!FMath::IsFinite(IdentityLeft->GetData()[I])) { FiniteOutput = false; break; }
            }
        }
        UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKIdentityNegative label=%s rejected=%d rendered_delta=%llu finite_output=%d expected_generation=%llu"),
            Label, Rejected ? 1 : 0, RenderedAfter - RenderedBefore, FiniteOutput ? 1 : 0, ExpectedGeneration);
        if (!Rejected || !FiniteOutput)
        {
            OutError = FString::Printf(TEXT("%s identity mismatch was not fail-closed (rejected_delta=%llu rendered_delta=%llu finite_output=%d)"),
                Label, RejectedAfter - RejectedBefore, RenderedAfter - RenderedBefore, FiniteOutput ? 1 : 0);
            return false;
        }
        return true;
    };

    TArray<float> GraphDirect, GraphPath, GraphSourceFull, GraphWet, GraphSend;
    TArray<float> IgnoredWet, IgnoredSend;
    FString GraphError;
    if (!RunGraph(1, GraphDirect, IgnoredWet, IgnoredSend, GraphError)
        || !RunGraph(2, GraphPath, IgnoredWet, IgnoredSend, GraphError)
        || !RunGraph(7, GraphSourceFull, GraphWet, GraphSend, GraphError))
    {
        IM_StopAcousticMetaSoundContext(Context);
        return Fail(FString::Printf(TEXT("Current graph reference fixture failed: %s"), *GraphError));
    }

    IM_AcousticAudioFrame ReferenceFrame = FrozenFrame;
    ReferenceFrame.Generation = 1;
    TArray<float> ReferenceDirect, ReferencePath, ReferenceSourceFull, ReferenceWet;
    if (!RenderReferenceSource(ReferenceFrame, 1, ReferenceDirect)
        || !RenderReferenceSource(ReferenceFrame, 2, ReferencePath)
        || !RenderReferenceSource(ReferenceFrame, 7, ReferenceSourceFull)
        || !RenderReferenceWet(ReferenceWet))
    {
        IM_StopAcousticMetaSoundContext(Context);
        return Fail(TEXT("SDK-only reference renderer could not render all frozen stems."));
    }

    const IMReferenceComparison DirectComparison = IMCompareStereo(GraphDirect, ReferenceDirect);
    const IMReferenceComparison PathComparison = IMCompareStereo(GraphPath, ReferencePath);
    const IMReferenceComparison WetComparison = IMCompareStereo(GraphWet, ReferenceWet);
    TArray<float> GraphFullMix = GraphSourceFull;
    TArray<float> ReferenceFullMix = ReferenceSourceFull;
    for (int32 I = 0; I < GraphFullMix.Num(); ++I)
    {
        GraphFullMix[I] += GraphWet[I];
        ReferenceFullMix[I] += ReferenceWet[I];
    }
    const IMReferenceComparison FullComparison = IMCompareStereo(GraphFullMix, ReferenceFullMix);
    const IMReferenceComparison SendComparison = IMCompareStereo(GraphSend, ExpectedSend);
    UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKReference energy graph_direct=%g graph_path=%g graph_wet=%g graph_full=%g ref_direct=%g ref_path=%g ref_wet=%g ref_full=%g stereo_asym=%g"),
        IMStereoEnergy(GraphDirect), IMStereoEnergy(GraphPath), IMStereoEnergy(GraphWet), IMStereoEnergy(GraphFullMix),
        IMStereoEnergy(ReferenceDirect), IMStereoEnergy(ReferencePath), IMStereoEnergy(ReferenceWet), IMStereoEnergy(ReferenceFullMix),
        IMStereoChannelAsymmetry(ReferenceFullMix));
    const bool PositivePass = DirectComparison.Equal && PathComparison.Equal && WetComparison.Equal
        && FullComparison.Equal && SendComparison.Equal;
    TestTrue(TEXT("Frozen direct stem matches SDK-only renderer"), DirectComparison.Equal);
    TestTrue(TEXT("Frozen path stem matches SDK-only renderer"), PathComparison.Equal);
    TestTrue(TEXT("Frozen wet stem matches SDK-only renderer"), WetComparison.Equal);
    TestTrue(TEXT("Frozen full mix matches SDK-only renderer"), FullComparison.Equal);
    TestTrue(TEXT("Frozen environmental send uses the declared gain"), SendComparison.Equal);
    UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKReference stems direct[%s] path[%s] wet[%s] full[%s] send[%s]"),
        *DirectComparison.Summary, *PathComparison.Summary, *WetComparison.Summary,
        *FullComparison.Summary, *SendComparison.Summary);

    TArray<float> WrongGainMix = ReferenceFullMix;
    TArray<float> SwappedMix = ReferenceFullMix;
    TArray<float> DuplicateWetMix = ReferenceSourceFull;
    for (int32 I = 0; I < IMReferenceGraphFrames; ++I)
    {
        WrongGainMix[2 * I] *= 0.5f; WrongGainMix[2 * I + 1] *= 0.5f;
        SwappedMix[2 * I] = ReferenceFullMix[2 * I + 1];
        SwappedMix[2 * I + 1] = ReferenceFullMix[2 * I];
        DuplicateWetMix[2 * I] += 2.0f * ReferenceWet[2 * I];
        DuplicateWetMix[2 * I + 1] += 2.0f * ReferenceWet[2 * I + 1];
    }
    const bool WrongGainRejected = !IMCompareStereo(GraphFullMix, WrongGainMix).Equal;
    const bool ChannelsRejected = !IMCompareStereo(GraphFullMix, SwappedMix).Equal;
    const bool DuplicateWetRejected = !IMCompareStereo(GraphFullMix, DuplicateWetMix).Equal;
    TestTrue(TEXT("Known wrong gain is rejected"), WrongGainRejected);
    TestTrue(TEXT("Known left/right swap is rejected"), ChannelsRejected);
    TestTrue(TEXT("Known duplicated wet is rejected"), DuplicateWetRejected);
    TestTrue(TEXT("Wet negative control is discriminating"), IMStereoEnergy(ReferenceWet) > 1.0e-10f);
    TestTrue(TEXT("Stereo negative control is discriminating"), IMStereoChannelAsymmetry(ReferenceFullMix) > 1.0e-5f);

    FString IdentityError;
    const bool AudioIdMismatchRejected = RunIdentityNegative(TEXT("audio_id"), TestAudioId + 1, TestEpoch, false, IdentityError);
    const bool WorldMismatchRejected = RunIdentityNegative(TEXT("world_generation"), TestAudioId, TestEpoch + 1, false, IdentityError);
    const bool VoiceGenerationMismatchRejected = RunIdentityNegative(TEXT("voice_generation"), TestAudioId, TestEpoch, true, IdentityError);
    const bool IdentityNegativePass = AudioIdMismatchRejected && WorldMismatchRejected && VoiceGenerationMismatchRejected;
    TestTrue(TEXT("Mismatched audio-component identity is rejected"), AudioIdMismatchRejected);
    TestTrue(TEXT("Mismatched world identity is rejected"), WorldMismatchRejected);
    TestTrue(TEXT("Mismatched voice generation is rejected"), VoiceGenerationMismatchRejected);
    if (!IdentityNegativePass && !IdentityError.IsEmpty()) AddError(IdentityError);

    FString EvidenceJSON;
    auto Writer = TJsonWriterFactory<>::Create(&EvidenceJSON);
    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("status"), PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass
        ? TEXT("PASS_S2_SDK_REFERENCE") : TEXT("FAIL_S2_SDK_REFERENCE"));
    Writer->WriteValue(TEXT("sample_rate_hz"), IMReferenceSampleRate);
    Writer->WriteValue(TEXT("graph_block_frames"), IMReferenceGraphFrames);
    Writer->WriteValue(TEXT("source_to_environment_lag_frames"), int32(IMReferenceSourceToEnvironmentLagFrames));
    Writer->WriteValue(TEXT("source_to_environment_lag_ms"), 1000.0 * IMReferenceSourceToEnvironmentLagFrames / IMReferenceSampleRate);
    Writer->WriteValue(TEXT("channels"), TEXT("stereo_interleaved_LR"));
    Writer->WriteValue(TEXT("input_gain"), IMReferenceInputGain);
    Writer->WriteValue(TEXT("send_gain"), ExpectedSendGain);
    Writer->WriteValue(TEXT("reverb_send_distance_gain"), FrozenReverbSendDistanceGain);
    Writer->WriteValue(TEXT("reverb_send_near_distance_m"), IM_AcousticRecipe::ReverbSendNearDistanceM);
    Writer->WriteValue(TEXT("reverb_send_far_distance_m"), IM_AcousticRecipe::ReverbSendFarDistanceM);
    Writer->WriteValue(TEXT("wet_gain"), IMReferenceWetGain);
    Writer->WriteValue(TEXT("absolute_tolerance"), IMReferenceAbsoluteTolerance);
    Writer->WriteValue(TEXT("relative_tolerance"), IMReferenceRelativeTolerance);
    Writer->WriteValue(TEXT("bake_asset"), BakePath);
    Writer->WriteValue(TEXT("pcm_input"), PCMPath);
    Writer->WriteValue(TEXT("pcm_offset_frames"), int32(IMReferenceSourceToEnvironmentLagFrames));
    Writer->WriteValue(TEXT("listener_candidate"), SelectedListenerName);
    Writer->WriteObjectStart(TEXT("source_ue_cm"));
    Writer->WriteValue(TEXT("x"), SourceUE.X); Writer->WriteValue(TEXT("y"), SourceUE.Y); Writer->WriteValue(TEXT("z"), SourceUE.Z);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("listener_ue_cm"));
    Writer->WriteValue(TEXT("x"), ListenerUE.X); Writer->WriteValue(TEXT("y"), ListenerUE.Y); Writer->WriteValue(TEXT("z"), ListenerUE.Z);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("listener_sdk_m"));
    Writer->WriteValue(TEXT("x"), ListenerSpace.origin.x); Writer->WriteValue(TEXT("y"), ListenerSpace.origin.y); Writer->WriteValue(TEXT("z"), ListenerSpace.origin.z);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("positive_controls"));
    Writer->WriteValue(TEXT("direct"), DirectComparison.Equal);
    Writer->WriteValue(TEXT("path"), PathComparison.Equal);
    Writer->WriteValue(TEXT("wet"), WetComparison.Equal);
    Writer->WriteValue(TEXT("full_mix"), FullComparison.Equal);
    Writer->WriteValue(TEXT("send"), SendComparison.Equal);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("negative_controls"));
    Writer->WriteValue(TEXT("wrong_gain_rejected"), WrongGainRejected);
    Writer->WriteValue(TEXT("left_right_swap_rejected"), ChannelsRejected);
    Writer->WriteValue(TEXT("duplicated_wet_rejected"), DuplicateWetRejected);
    Writer->WriteObjectStart(TEXT("identity_mismatch"));
    Writer->WriteValue(TEXT("audio_component_id_rejected"), AudioIdMismatchRejected);
    Writer->WriteValue(TEXT("world_generation_rejected"), WorldMismatchRejected);
    Writer->WriteValue(TEXT("voice_generation_rejected"), VoiceGenerationMismatchRejected);
    Writer->WriteValue(TEXT("frozen_response_fail_closed"), IdentityNegativePass);
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();
    Writer->Close();
    const FString EvidencePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundClosure/sdk-reference-comparison.json"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(EvidencePath), true);
    if (!FFileHelper::SaveStringToFile(EvidenceJSON, *EvidencePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        AddError(FString::Printf(TEXT("Could not save SDK reference evidence: %s"), *EvidencePath));

    IM_StopAcousticMetaSoundContext(Context);
    UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound SDK reference comparison"),
        (PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass) ? TEXT("PASS") : TEXT("FAIL"));
    UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSoundSDKReference %s"),
        (PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass) ? TEXT("PASS") : TEXT("FAIL"));
    return !HasAnyErrors() && PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass;
}
#endif
