#include "IMAcousticMetaSound.h"
#include "MetasoundAudioBus.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "Sound/AudioBus.h"

// Never pop a partial block: a short read would discard timing information.
// After startup, exhaustion is an explicit failed continuity condition, not a
// reason to hide a gap with latest-audio reads or an unbounded realtime wait.
static bool IMReadAcousticBus(IM_AcousticMetaSoundContext& C, float* Out, int32 Frames, bool& Primed)
{
    FMemory::Memzero(Out, Frames * sizeof(float));
    if (!C.BusPatch || C.Stopped.load(std::memory_order_acquire)) return false;
    if (Frames != int32(IM_AcousticMetaSoundContext::Frames))
    { C.InvalidBlocks.fetch_add(1, std::memory_order_relaxed); return false; }
    const int32 Available = C.BusPatch->GetNumSamplesAvailable();
    if (!Primed)
    {
        if (Available < C.BusPrimeFrames)
        { C.BusPrimingBlocks.fetch_add(1, std::memory_order_relaxed); return false; }
        Primed = true;
    }
    C.BusMinAvailable.store(FMath::Min(C.BusMinAvailable.load(), Available), std::memory_order_relaxed);
    C.BusMaxAvailable.store(FMath::Max(C.BusMaxAvailable.load(), Available), std::memory_order_relaxed);
    if (Available < Frames || C.BusPatch->PopAudio(Out, Frames, false) != Frames)
    {
        FMemory::Memzero(Out, Frames * sizeof(float));
        C.BusUnderruns.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    C.BusReadBlocks.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace Metasound
{
class IM_AcousticBusReaderOperator final : public TExecutableOperator<IM_AcousticBusReaderOperator>
{
public:
    static const FVertexInterface& GetVertexInterface()
    {
        static const FVertexInterface V(FInputVertexInterface(TInputDataVertex<FAudioBusAsset>(FName(TEXT("Audio Bus")), FDataVertexMetadata{})),
            FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(FName(TEXT("Out 0")), FDataVertexMetadata{})));
        return V;
    }
    static const FNodeClassMetadata& GetNodeInfo()
    {
        static const FNodeClassMetadata Info = []
        {
            FNodeClassMetadata M; M.ClassName = {TEXT("IM"), TEXT("Acoustic Bus Reader"), TEXT("Mono")};
            M.MajorVersion = 1; M.MinorVersion = 0;
            M.DisplayName = FText::FromString(TEXT("Acoustic Bus Reader"));
            M.Description = FText::FromString(TEXT("Bounded shared bus input with explicit startup reserve and underrun accounting."));
            M.Author = TEXT("IceMoon"); M.DefaultInterface = GetVertexInterface(); return M;
        }();
        return Info;
    }
    static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& P, FBuildResults&)
    {
        return MakeUnique<IM_AcousticBusReaderOperator>(P,
            P.InputData.GetOrCreateDefaultDataReadReference<FAudioBusAsset>(TEXT("Audio Bus"), P.OperatorSettings));
    }
    IM_AcousticBusReaderOperator(const FBuildOperatorParams& P, FAudioBusAssetReadRef InBus)
        : Bus(InBus), Output(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)) { Reset(P); }
    ~IM_AcousticBusReaderOperator() override { Release(); }
    void Release()
    {
        if (Owned && Context) Context->BusReaderConsumer.store(false, std::memory_order_release);
        Owned = false; Primed = false; Context.Reset();
    }
    void Reset(const IOperator::FResetParams& P)
    {
        Release(); Output->Zero();
        const FName Key = Frontend::SourceInterface::Environment::DeviceID;
        if (!P.Environment.Contains<uint32>(Key)) return;
        Context = IM_FindAcousticMetaSoundContext(P.Environment.GetValue<uint32>(Key));
        if (!Context) return;
        bool Expected = false;
        Owned = Context->BusReaderConsumer.compare_exchange_strong(Expected, true, std::memory_order_acq_rel);
        if (!Owned) Context->DuplicateConsumers.fetch_add(1, std::memory_order_relaxed);
    }
    void BindInputs(FInputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Audio Bus"), Bus); }
    void BindOutputs(FOutputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Out 0"), Output); }
    void Execute()
    {
        Output->Zero();
        if (!Context || !Owned) return;
        const auto& Proxy = Bus->GetAudioBusProxy();
        if (!Proxy.IsValid() || Proxy->AudioBusId != Context->BusId || Proxy->NumChannels != 1) return;
        IMReadAcousticBus(*Context, Output->GetData(), Output->Num(), Primed);
    }
private:
    FAudioBusAssetReadRef Bus;
    FAudioBufferWriteRef Output;
    IM_AcousticMetaSoundContextPtr Context;
    bool Owned = false, Primed = false;
};
using IM_AcousticBusReaderNode = TNodeFacade<IM_AcousticBusReaderOperator>;
METASOUND_REGISTER_NODE(IM_AcousticBusReaderNode)
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticBusBufferBoundaries, "IceMoon.AcousticField.MetaSound.BusBufferBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticBusBufferBoundaries::RunTest(const FString&)
{
    IM_AcousticMetaSoundContext C;
    C.BusPatch = MakeShared<Audio::FPatchOutput, ESPMode::ThreadSafe>(16384);
    Audio::FPatchInput Producer(C.BusPatch);
    TArray<float> Input, Output; Input.SetNumUninitialized(4096); Output.SetNumZeroed(512);
    for (int32 I=0; I<Input.Num(); ++I) Input[I] = float(I) / 4096.f;
    bool Primed = false;
    Producer.PushAudio(Input.GetData(), 3072);
    TestFalse(TEXT("Short startup reserve does not consume input"), IMReadAcousticBus(C, Output.GetData(), 512, Primed));
    TestEqual(TEXT("Startup samples retained"), C.BusPatch->GetNumSamplesAvailable(), 3072);
    Producer.PushAudio(Input.GetData()+3072, 1024);
    for (int32 Block=0; Block<8; ++Block)
    {
        TestTrue(TEXT("Complete buffered block"), IMReadAcousticBus(C, Output.GetData(), 512, Primed));
        for (int32 I=0; I<512; ++I)
            if (Output[I] != Input[Block*512+I]) { AddError(TEXT("Bus block order or samples changed")); break; }
    }
    TestFalse(TEXT("Exhaustion is rejected"), IMReadAcousticBus(C, Output.GetData(), 512, Primed));
    TestEqual(TEXT("Underrun is accounted"), C.BusUnderruns.load(), uint64(1));
    Producer.PushAudio(Input.GetData(), 256);
    IMReadAcousticBus(C, Output.GetData(), 512, Primed);
    TestEqual(TEXT("Partial block is not discarded"), C.BusPatch->GetNumSamplesAvailable(), 256);
    C.Stopped.store(true);
    TestFalse(TEXT("Stopped context cannot consume"), IMReadAcousticBus(C, Output.GetData(), 512, Primed));
    UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound bus buffer boundaries"), HasAnyErrors()?TEXT("FAIL"):TEXT("PASS"));
    UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound bus buffer boundaries %s"), HasAnyErrors()?TEXT("FAIL"):TEXT("PASS"));
    return !HasAnyErrors();
}
#endif
