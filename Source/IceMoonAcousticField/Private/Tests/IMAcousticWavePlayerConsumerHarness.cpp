#include "IMAcousticWavePlayerConsumerTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "IMAcousticFootstepCharacter.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "JsonObjectConverter.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundGenerator.h"
#include "MetasoundPrimitives.h"
#include "MetasoundSource.h"
#include "MetasoundTrigger.h"
#include "MetasoundVertexData.h"
#include "MetasoundWave.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "UObject/StrongObjectPtr.h"

namespace IMAcousticWavePlayerConsumerHarnessPrivate
{
std::atomic<FIMWaveConsumerSink*> IMSink{nullptr};
std::atomic<int32> IMAcquiring{0};
uint64 IMEpoch = 0;
FString OwnedOutputDirectory;
const TCHAR* IMOriginalPath = TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstep.MS_IMFootstep");
FString IMOutput()
{
	if (!OwnedOutputDirectory.IsEmpty()) return OwnedOutputDirectory;
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/ConsumerR108ControlRetry_20261008"));
}
bool IMSave(const TCHAR* Name, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	return FJsonSerializer::Serialize(Object, Writer)
		&& FFileHelper::SaveStringToFile(Text, *FPaths::Combine(IMOutput(), Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
bool IMDocument(const TCHAR* Name, const FMetasoundFrontendDocument& Document)
{
	FString Text;
	return FJsonObjectConverter::UStructToJsonObjectString(Document, Text)
		&& FFileHelper::SaveStringToFile(Text, *FPaths::Combine(IMOutput(), Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
TSharedRef<FJsonObject> IMRowJson(const FIMWaveConsumerRow& Row)
{
	auto Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("kind"), Row.Kind == EIMWaveConsumerRowKind::Accept ? TEXT("accept") : TEXT("render"));
	Object->SetStringField(TEXT("instance_id"), LexToString(Row.InstanceID));
	Object->SetStringField(TEXT("operator_id"), LexToString(Row.OperatorID));
	Object->SetStringField(TEXT("graph_epoch"), LexToString(Row.GraphEpoch));
	Object->SetStringField(TEXT("graph_block"), LexToString(Row.Block));
	Object->SetStringField(TEXT("trigger_ordinal"), LexToString(Row.TriggerOrdinal));
	Object->SetNumberField(TEXT("subframe_start"), Row.Start);
	Object->SetNumberField(TEXT("subframe_end"), Row.End);
	Object->SetStringField(TEXT("graph_local_frame"), LexToString(Row.Block * 512 + Row.Start));
	Object->SetField(TEXT("master_consumed_frame"), MakeShared<FJsonValueNull>());
	Object->SetNumberField(TEXT("device_id"), Row.DeviceID);
	Object->SetNumberField(TEXT("native_platform_seconds"), Row.Seconds);
	Object->SetStringField(TEXT("native_cycles64"), LexToString(Row.Cycles));
	Object->SetStringField(TEXT("proxy_pointer"), LexToString(Row.ProxyPointer));
	Object->SetStringField(TEXT("data_pointer"), LexToString(Row.DataPointer));
	Object->SetStringField(TEXT("data_guid"), Row.DataGUID.ToString());
	Object->SetStringField(TEXT("wave_name"), Row.WaveName.ToString());
	Object->SetStringField(TEXT("package"), Row.Package.ToString());
	Object->SetStringField(TEXT("runtime_format"), Row.Format.ToString());
	Object->SetNumberField(TEXT("source_rate"), Row.SourceRate);
	Object->SetNumberField(TEXT("source_channels"), Row.SourceChannels);
	Object->SetNumberField(TEXT("source_frames"), Row.SourceFrames);
	Object->SetNumberField(TEXT("pitch_semitones"), Row.Pitch);
	Object->SetBoolField(TEXT("set_sound_wave_returned"), Row.SetSoundWaveReturned);
	Object->SetBoolField(TEXT("player_valid"), Row.PlayerValid);
	Object->SetBoolField(TEXT("playing"), Row.Playing);
	Object->SetBoolField(TEXT("first_render"), Row.FirstRender);
	Object->SetStringField(TEXT("output_reference"), LexToString(Row.OutputReference));
	Object->SetNumberField(TEXT("source_pcm_offset"), Row.SourcePCMOffset);
	Object->SetNumberField(TEXT("source_pcm_frames"), Row.SourcePCMFrames);
	return Object;
}
bool IMPCM(const TCHAR* Name, const TArray<float>& PCM, int32 Count)
{
	return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(reinterpret_cast<const uint8*>(PCM.GetData()), Count * sizeof(float)),
		*FPaths::Combine(IMOutput(), Name));
}
struct FIMBuiltOperator
{
	TUniquePtr<Metasound::INode> Node;
	TUniquePtr<Metasound::IOperator> Operator;
	TUniquePtr<Metasound::FOutputVertexInterfaceData> Outputs;
};
bool IMBuild(const FMetasoundFrontendClassName& Name, const Metasound::FOperatorSettings& Settings,
	const Metasound::FMetasoundEnvironment& Environment,
	TFunctionRef<void(Metasound::FInputVertexInterfaceData&)> Bind, FIMBuiltOperator& Out)
{
	using namespace Metasound;
	const Frontend::FNodeClassRegistryKey Key(EMetasoundFrontendClassType::External, Name, 1, 1);
	FVertexInterface Interface;
	auto& Registry = Frontend::INodeClassRegistry::GetChecked();
	if (!Registry.FindDefaultVertexInterface(Key, Interface)) return false;
	Out.Node = Registry.CreateNode(Key, FNodeData(TEXT("IM_R108_compare"), FGuid::NewGuid(), Interface));
	if (!Out.Node) return false;
	FInputVertexInterfaceData Inputs(Interface.GetInputInterface());
	Bind(Inputs);
	FBuildResults Results;
	const FBuildOperatorParams Params(*Out.Node, Settings, Inputs, Environment);
	Out.Operator = Out.Node->GetDefaultOperatorFactory()->CreateOperator(Params, Results);
	if (!Out.Operator || !Out.Operator->GetExecuteFunction()) return false;
	Out.Outputs = MakeUnique<FOutputVertexInterfaceData>(Interface.GetOutputInterface());
	Out.Operator->BindOutputs(*Out.Outputs);
	return true;
}
void IMExecute(FIMBuiltOperator& Built)
{
	Built.Operator->GetExecuteFunction()(Built.Operator.Get());
}
bool IMIdentity(const FIMWaveConsumerRow& Row, const FSoundWaveData& Expected)
{
	return Row.Kind == EIMWaveConsumerRowKind::Accept && Row.SetSoundWaveReturned && Row.PlayerValid && Row.Playing
		&& Row.DataGUID == Expected.GetGUID() && Row.Package == Expected.GetPackageName()
		&& Row.WaveName == Expected.GetFName() && Row.SourceFrames == Expected.GetNumFrames()
		&& Row.SourceChannels == Expected.GetNumChannels() && Row.SourceRate == Expected.GetSampleRate();
}
bool IMCase(const TSharedRef<FJsonObject>& Input, int32 CaseIndex, int32 Pattern, bool bWrong, bool bInvalid,
	TArray<float>& StockPCM, TArray<float>& OffPCM, TArray<float>& OnPCM, TSharedRef<FJsonObject>& Result)
{
	using namespace Metasound;
	const FString WavePath = Input->GetStringField(TEXT("wave_gt_selected"));
	USoundWave* ExpectedWave = LoadObject<USoundWave>(nullptr, *WavePath);
	USoundWave* ActualWave = bWrong ? LoadObject<USoundWave>(nullptr,
		TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/SW_IMFootstep_wood03.SW_IMFootstep_wood03")) : ExpectedWave;
	if (!ExpectedWave || !ActualWave) return false;
	const FOperatorSettings Settings(48000, 48000.f / 512.f);
	const Audio::FProxyDataInitParams Init{TEXT("IMAcousticWaveConsumerR108")};
	const FWaveAsset ExpectedAsset(ExpectedWave->CreateProxyData(Init));
	const FWaveAsset ActualAsset = bInvalid ? FWaveAsset() : FWaveAsset(ActualWave->CreateProxyData(Init));
	if (!ExpectedAsset.GetWaveProxy().IsValid()) return false;
	const auto ExpectedData = ExpectedAsset.GetWaveProxy()->GetSoundWaveDataRef();
	auto Play = FTriggerWriteRef::CreateNew(Settings);
	auto Stop = FTriggerWriteRef::CreateNew(Settings);
	auto Wave = TDataWriteReference<FWaveAsset>::CreateNew(ActualAsset);
	auto Pitch = FFloatWriteRef::CreateNew(float(Input->GetNumberField(TEXT("pitch_semitones"))));
	auto Gain = FFloatWriteRef::CreateNew(float(Input->GetNumberField(TEXT("gain"))));
	FMetasoundEnvironment Environment;
	Environment.SetValue<uint64>(CoreInterface::Environment::InstanceID, uint64(CaseIndex + 1000));
	Environment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, MAX_uint32 - 17);
	auto Bind = [&](FInputVertexInterfaceData& Inputs)
	{
		Inputs.BindWriteVertex(TEXT("Play"), Play);
		Inputs.BindWriteVertex(TEXT("Stop"), Stop);
		Inputs.BindWriteVertex(TEXT("Wave Asset"), Wave);
		Inputs.BindWriteVertex(TEXT("Pitch Shift"), Pitch);
	};
	FIMBuiltOperator Players[3], Gains[3];
	auto Sink = MakeShared<FIMWaveConsumerSink, ESPMode::ThreadSafe>(++IMEpoch);
	IM_SetWaveConsumerSink(nullptr);
	if (!IMBuild({TEXT("UE"), TEXT("Wave Player"), TEXT("Mono")}, Settings, Environment, Bind, Players[0])
		|| !IMBuild({TEXT("IM"), TEXT("Acoustic Wave Consumer Trace"), TEXT("Mono")}, Settings, Environment, Bind, Players[1])) return false;
	IM_SetWaveConsumerSink(&Sink.Get());
	const bool bBuilt = IMBuild({TEXT("IM"), TEXT("Acoustic Wave Consumer Trace"), TEXT("Mono")}, Settings, Environment, Bind, Players[2]);
	IM_SetWaveConsumerSink(nullptr);
	if (!bBuilt) return false;
	for (int32 Variant = 0; Variant < 3; ++Variant)
	{
		auto Mono = Players[Variant].Outputs->GetDataReadReference<FAudioBuffer>(TEXT("Out Mono"));
		auto BindGain = [&](FInputVertexInterfaceData& Inputs)
		{
			Inputs.BindReadVertex(TEXT("PrimaryOperand"), Mono);
			Inputs.BindWriteVertex(TEXT("AdditionalOperands"), Gain);
		};
		if (!IMBuild({TEXT("UE"), TEXT("Multiply"), TEXT("Audio by Float")}, Settings, Environment, BindGain, Gains[Variant])) return false;
	}
	Result->SetStringField(TEXT("expected_wave"), WavePath);
	Result->SetNumberField(TEXT("gain"), *Gain);
	Result->SetNumberField(TEXT("pitch_semitones"), *Pitch);
	Result->SetNumberField(TEXT("trigger_pattern"), Pattern);
	const int32 StartPCM = StockPCM.Num();
	const TCHAR* TriggerNames[] = {TEXT("On Play"), TEXT("On Finished"), TEXT("On Nearly Finished"), TEXT("On Looped"), TEXT("On Cue Point")};
	TArray<TSharedPtr<FJsonValue>> TriggerRows;
	for (int32 Block = 0; Block < 64; ++Block)
	{
		Play->Reset(); Stop->Reset();
		if (Block == 0)
		{
			Play->TriggerFrame(Pattern == 2 ? 17 : 0);
			if (Pattern == 1) Play->TriggerFrame(128);
			if (Pattern == 2) { Stop->TriggerFrame(128); Play->TriggerFrame(511); }
		}
		if (Block == 12 && Pattern == 1) Stop->TriggerFrame(256);
		for (int32 Variant = 0; Variant < 3; ++Variant) { IMExecute(Players[Variant]); IMExecute(Gains[Variant]); }
		for (int32 Layer = 0; Layer < 2; ++Layer)
		{
			FIMBuiltOperator* Chain = Layer == 0 ? Players : Gains;
			const TCHAR* OutputName = Layer == 0 ? TEXT("Out Mono") : TEXT("Out");
			auto Stock = Chain[0].Outputs->GetDataReadReference<FAudioBuffer>(OutputName);
			for (int32 Variant = 1; Variant < 3; ++Variant)
			{
				auto Candidate = Chain[Variant].Outputs->GetDataReadReference<FAudioBuffer>(OutputName);
				if (Stock->Num() != 512 || Candidate->Num() != 512
					|| FMemory::Memcmp(Stock->GetData(), Candidate->GetData(), 512 * sizeof(float)))
				{
					Result->SetStringField(TEXT("first_mismatch"), TEXT("audio_bytes"));
					Result->SetNumberField(TEXT("first_mismatch_block"), Block);
					Result->SetNumberField(TEXT("first_mismatch_layer"), Layer);
					Result->SetNumberField(TEXT("first_mismatch_variant"), Variant);
					return false;
				}
			}
		}
		for (const TCHAR* Name : TriggerNames)
		{
			auto Stock = Players[0].Outputs->GetDataReadReference<FTrigger>(Name);
			for (int32 Variant = 1; Variant < 3; ++Variant)
			{
				auto Candidate = Players[Variant].Outputs->GetDataReadReference<FTrigger>(Name);
				if (Stock->GetTriggeredFrames() != Candidate->GetTriggeredFrames())
				{
					Result->SetStringField(TEXT("first_mismatch"), Name);
					Result->SetNumberField(TEXT("first_mismatch_block"), Block);
					return false;
				}
			}
			for (int32 Frame : Stock->GetTriggeredFrames())
			{
				auto Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("block"), Block); Row->SetNumberField(TEXT("frame"), Frame); Row->SetStringField(TEXT("pin"), Name);
				TriggerRows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		const TCHAR* FloatNames[] = {TEXT("Loop Percent"), TEXT("Playback Location")};
		for (const TCHAR* Name : FloatNames)
		{
			const auto Stock = Players[0].Outputs->GetDataReadReference<float>(Name);
			for (int32 Variant = 1; Variant < 3; ++Variant)
			{
				const auto Candidate = Players[Variant].Outputs->GetDataReadReference<float>(Name);
				if (FMemory::Memcmp(&*Stock, &*Candidate, sizeof(float)))
				{ Result->SetStringField(TEXT("first_mismatch"), Name); Result->SetNumberField(TEXT("first_mismatch_block"), Block); return false; }
			}
		}
		const auto StockTime = Players[0].Outputs->GetDataReadReference<FTime>(TEXT("Playback Time"));
		const auto StockCueID = Players[0].Outputs->GetDataReadReference<int32>(TEXT("Cue Point ID"));
		const auto StockCueLabel = Players[0].Outputs->GetDataReadReference<FString>(TEXT("Cue Point Label"));
		for (int32 Variant = 1; Variant < 3; ++Variant)
		{
			if (StockTime->GetSeconds() != Players[Variant].Outputs->GetDataReadReference<FTime>(TEXT("Playback Time"))->GetSeconds()
				|| *StockCueID != *Players[Variant].Outputs->GetDataReadReference<int32>(TEXT("Cue Point ID"))
				|| *StockCueLabel != *Players[Variant].Outputs->GetDataReadReference<FString>(TEXT("Cue Point Label")))
			{ Result->SetStringField(TEXT("first_mismatch"), TEXT("time_or_cue")); Result->SetNumberField(TEXT("first_mismatch_block"), Block); return false; }
		}
		StockPCM.Append(Gains[0].Outputs->GetDataReadReference<FAudioBuffer>(TEXT("Out"))->GetData(), 512);
		OffPCM.Append(Gains[1].Outputs->GetDataReadReference<FAudioBuffer>(TEXT("Out"))->GetData(), 512);
		OnPCM.Append(Gains[2].Outputs->GetDataReadReference<FAudioBuffer>(TEXT("Out"))->GetData(), 512);
	}
	int32 Accepts = 0, FirstRenders = 0;
	bool bIdentity = true;
	TArray<TSharedPtr<FJsonValue>> Rows;
	const int32 Count = Sink->Published.load(std::memory_order_acquire);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const auto& Row = Sink->Rows[Index];
		if (Row.Kind == EIMWaveConsumerRowKind::Accept) { ++Accepts; bIdentity &= IMIdentity(Row, *ExpectedData); }
		if (Row.FirstRender) ++FirstRenders;
		Rows.Add(MakeShared<FJsonValueObject>(IMRowJson(Row)));
	}
	Result->SetArrayField(TEXT("consumer_rows"), Rows);
	Result->SetArrayField(TEXT("trigger_rows"), TriggerRows);
	Result->SetNumberField(TEXT("pcm_offset"), StartPCM);
	Result->SetNumberField(TEXT("pcm_frames"), StockPCM.Num() - StartPCM);
	Result->SetNumberField(TEXT("accepts"), Accepts);
	Result->SetNumberField(TEXT("first_renders"), FirstRenders);
	Result->SetNumberField(TEXT("sink_failure"), Sink->Failure.load());
	Result->SetBoolField(TEXT("byte_and_trigger_equal"), true);
	Result->SetBoolField(TEXT("expected_identity_accepted"), bIdentity && Accepts > 0);
	Result->SetBoolField(TEXT("negative_control"), bWrong || bInvalid);
	const bool bIdentityContract = bWrong || bInvalid ? !bIdentity && Accepts > 0 : bIdentity && Accepts > 0 && FirstRenders == Accepts;
	return Sink->Failure.load() == 0 && bIdentityContract && (!bInvalid || FirstRenders == 0);
}
void IMCompare(const TArray<FString>& Args)
{
	if (!Args.IsEmpty() || IMSink.load() || IFileManager::Get().FileExists(*FPaths::Combine(IMOutput(), TEXT("parity_report.json")))) return;
	IFileManager::Get().MakeDirectory(*IMOutput(), true);
	FString Text;
	auto Report = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input;
	bool bGood = FFileHelper::LoadFileToString(Text, *FPaths::Combine(IMOutput(), TEXT("parity_input.json")))
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Input);
	const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
	bGood &= Input.IsValid() && Input->TryGetArrayField(TEXT("source_events"), Events) && Events && Events->Num() == 24;
	TArray<TSharedPtr<FJsonValue>> Cases;
	TArray<float> StockPCM, OffPCM, OnPCM;
	if (bGood)
	{
		for (int32 Index = 0; Index < 24 && bGood; ++Index)
		{
			auto Case = MakeShared<FJsonObject>();
			bGood = IMCase((*Events)[Index]->AsObject().ToSharedRef(), Index, 0, false, false, StockPCM, OffPCM, OnPCM, Case);
			Cases.Add(MakeShared<FJsonValueObject>(Case));
		}
		TSet<FString> Seen;
		for (int32 Index = 0; Index < 24 && bGood; ++Index)
		{
			auto Event = (*Events)[Index]->AsObject().ToSharedRef();
			FString Path = Event->GetStringField(TEXT("wave_gt_selected"));
			if (Seen.Contains(Path)) continue;
			Seen.Add(Path);
			for (int32 Pattern = 1; Pattern <= 2 && bGood; ++Pattern)
			{
				auto Case = MakeShared<FJsonObject>();
				bGood = IMCase(Event, 100 + Cases.Num(), Pattern, false, false, StockPCM, OffPCM, OnPCM, Case);
				Cases.Add(MakeShared<FJsonValueObject>(Case));
			}
		}
		for (int32 Negative = 0; Negative < 2 && bGood; ++Negative)
		{
			auto Case = MakeShared<FJsonObject>();
			bGood = IMCase((*Events)[0]->AsObject().ToSharedRef(), 200 + Negative, 0, Negative == 0, Negative == 1,
				StockPCM, OffPCM, OnPCM, Case);
			Cases.Add(MakeShared<FJsonValueObject>(Case));
		}
	}
	IM_SetWaveConsumerSink(nullptr);
	bGood &= IMPCM(TEXT("parity_stock_gain.f32"), StockPCM, StockPCM.Num())
		&& IMPCM(TEXT("parity_diag_off_gain.f32"), OffPCM, OffPCM.Num()) && IMPCM(TEXT("parity_diag_on_gain.f32"), OnPCM, OnPCM.Num());
	Report->SetBoolField(TEXT("pass"), bGood);
	Report->SetArrayField(TEXT("cases"), Cases);
	Report->SetNumberField(TEXT("sample_rate"), 48000); Report->SetNumberField(TEXT("graph_frames"), 512);
	Report->SetStringField(TEXT("execution"), TEXT("actual UE registry factories, actual Wave proxies, actual native Multiply, stock/diagnostic-OFF/diagnostic-ON"));
	IMSave(TEXT("parity_report.json"), Report);
	UE_LOG(LogTemp, Display, TEXT("IMLogs R108_CONSUMER compare_complete pass=%d cases=%d"), bGood, Cases.Num());
}
struct FIMConsumerOwner
{
	TStrongObjectPtr<UMetaSoundSource> Original, Source;
	TStrongObjectPtr<UMetaSoundSourceBuilder> Builder;
	TSharedPtr<FIMWaveConsumerSink, ESPMode::ThreadSafe> Sink;
	TWeakObjectPtr<UWorld> World;
	FAudioDeviceHandle Device;
	FDelegateHandle Initialized, Cleanup, Created, Destroyed;
	FTSTicker::FDelegateHandle Ticker;
	FString WorldPath, ActorPath, Failure;
	uint64 ComponentID = 0;
	bool Bound = false, Cleaned = false, Finishing = false;
};
TUniquePtr<FIMConsumerOwner> IMOwner;
TSharedRef<FJsonObject> IMStatus()
{
	auto Report = MakeShared<FJsonObject>();
	if (!IMOwner) { Report->SetStringField(TEXT("state"), TEXT("OFF")); return Report; }
	const auto& State = *IMOwner;
	Report->SetStringField(TEXT("state"), State.Finishing ? TEXT("DRAINING") : State.Bound ? TEXT("BOUND") : TEXT("ARMED"));
	Report->SetStringField(TEXT("failure"), State.Failure);
	Report->SetBoolField(TEXT("bound_before_begin_play"), State.Bound);
	Report->SetBoolField(TEXT("world_cleanup_observed"), State.Cleaned);
	Report->SetStringField(TEXT("world"), State.WorldPath);
	Report->SetStringField(TEXT("actor"), State.ActorPath);
	Report->SetStringField(TEXT("component_id"), LexToString(State.ComponentID));
	Report->SetNumberField(TEXT("device_id"), State.Device.GetDeviceID());
	Report->SetStringField(TEXT("record_epoch"), LexToString(State.Sink->RecordEpoch));
	Report->SetStringField(TEXT("record_epoch_semantics"), TEXT("private sink start ordinal, separate from World/device/generator generation"));
	Report->SetNumberField(TEXT("operators"), State.Sink->Operators.load());
	Report->SetNumberField(TEXT("active_generators"), State.Sink->ActiveGenerators.load());
	Report->SetNumberField(TEXT("producer_in_flight"), State.Sink->InFlight.load());
	Report->SetNumberField(TEXT("sink_failure"), State.Sink->Failure.load());
	Report->SetNumberField(TEXT("rows"), State.Sink->Published.load(std::memory_order_acquire));
	Report->SetBoolField(TEXT("acquisition_drained"), IM_WaveConsumerAcquisitionDrained());
	Report->SetField(TEXT("master_consumed_frame_mapping"), MakeShared<FJsonValueNull>());
	return Report;
}
bool IMTransient(FIMConsumerOwner& State)
{
	State.Original.Reset(LoadObject<UMetaSoundSource>(nullptr, IMOriginalPath));
	if (!State.Original || !IMDocument(TEXT("original_graph_fresh.json"), State.Original->GetConstDocument())) return false;
	TScriptInterface<IMetaSoundDocumentInterface> Reference;
	Reference.SetObject(State.Original.Get()); Reference.SetInterface(State.Original.Get());
	auto* Subsystem = GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>();
	if (!Subsystem) return false;
	EMetaSoundBuilderResult Result;
	State.Builder.Reset(Subsystem->CreateSourcePresetBuilder(TEXT("IM_R108_Consumer"), Reference, Result));
	if (!State.Builder || Result != EMetaSoundBuilderResult::Succeeded) return false;
	State.Builder->ConvertFromPreset(Result);
	if (Result != EMetaSoundBuilderResult::Succeeded) return false;
	State.Builder->GetBuilder().InitDocument(&State.Original->GetConstDocument());
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	State.Builder->ReloadCache(true);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	const auto& Before = State.Builder->GetConstBuilder().GetConstDocumentChecked();
	if (!IMDocument(TEXT("expanded_graph_before.json"), Before)) return false;
	const auto& Graph = Before.RootGraph.GetConstDefaultGraph();
	TArray<FMetasoundFrontendNode> Players;
	for (const auto& Dependency : Before.Dependencies)
		if (Dependency.Metadata.GetClassName() == FMetasoundFrontendClassName(TEXT("UE"), TEXT("Wave Player"), TEXT("Mono")))
			for (const auto& Node : Graph.Nodes) if (Node.ClassID == Dependency.ID) Players.Add(Node);
	if (Players.Num() != 1) return false;
	const auto Player = Players[0];
	const auto Edges = Graph.Edges;
	State.Builder->RemoveNode(FMetaSoundNodeHandle(Player.GetID()), Result);
	if (Result != EMetaSoundBuilderResult::Succeeded) return false;
	const auto Replacement = State.Builder->AddNodeByClassName({TEXT("IM"), TEXT("Acoustic Wave Consumer Trace"), TEXT("Mono")}, Result, 1);
	if (Result != EMetaSoundBuilderResult::Succeeded) return false;
	for (const auto& Literal : Player.InputLiterals)
	{
		const auto* Vertex = Player.Interface.Inputs.FindByPredicate([&](const auto& Input) { return Input.VertexID == Literal.VertexID; });
		if (!Vertex) return false;
		auto Handle = State.Builder->FindNodeInputByName(Replacement, Vertex->Name, Result);
		if (Result != EMetaSoundBuilderResult::Succeeded) return false;
		State.Builder->SetNodeInputDefault(Handle, Literal.Value, Result);
		if (Result != EMetaSoundBuilderResult::Succeeded) return false;
	}
	for (const auto& Edge : Edges)
	{
		if (Edge.FromNodeID != Player.GetID() && Edge.ToNodeID != Player.GetID()) continue;
		FMetaSoundBuilderNodeOutputHandle Output(Edge.FromNodeID, Edge.FromVertexID);
		FMetaSoundBuilderNodeInputHandle Input(Edge.ToNodeID, Edge.ToVertexID);
		if (Edge.FromNodeID == Player.GetID())
		{
			const auto* Vertex = Player.Interface.Outputs.FindByPredicate([&](const auto& Pin) { return Pin.VertexID == Edge.FromVertexID; });
			if (!Vertex) return false;
			Output = State.Builder->FindNodeOutputByName(Replacement, Vertex->Name, Result);
			if (Result != EMetaSoundBuilderResult::Succeeded) return false;
		}
		if (Edge.ToNodeID == Player.GetID())
		{
			const auto* Vertex = Player.Interface.Inputs.FindByPredicate([&](const auto& Pin) { return Pin.VertexID == Edge.ToVertexID; });
			if (!Vertex) return false;
			Input = State.Builder->FindNodeInputByName(Replacement, Vertex->Name, Result);
			if (Result != EMetaSoundBuilderResult::Succeeded) return false;
		}
		State.Builder->ConnectNodes(Output, Input, Result);
		if (Result != EMetaSoundBuilderResult::Succeeded) return false;
	}
	State.Builder->SetSampleRateOverride(48000);
	State.Builder->SetBlockRateOverride(48000.f / 512.f);
	const auto Built = State.Builder->BuildNewMetaSound(TEXT("IM_R108_TransientConsumerSource"));
	State.Source.Reset(Cast<UMetaSoundSource>(Built.GetObject()));
	if (!State.Source || !State.Source->HasAnyFlags(RF_Transient)) return false;
	State.Source->VirtualizationMode = State.Original->VirtualizationMode;
	State.Source->SampleRateOverride.Default = State.Original->SampleRateOverride.Default;
	State.Source->BlockRateOverride.Default = State.Original->BlockRateOverride.Default;
	const auto Settings = State.Source->GetOperatorSettings(48000);
	return Settings.GetNumFramesPerBlock() == 512 && Settings.GetSampleRate() == 48000
		&& IMDocument(TEXT("transient_graph_fresh.json"), State.Source->GetConstDocument());
}
void IMGeneratorRow(const Metasound::FGeneratorInstanceInfo& Info, bool Created,
	const TSharedRef<FIMWaveConsumerSink, ESPMode::ThreadSafe>& Sink)
{
	FIMWaveConsumerGeneratorRow Row;
	Row.ComponentID = Info.AudioComponentID; Row.InstanceID = Info.InstanceID;
	const auto Generator = Info.Generator.Pin();
	Row.GeneratorPointer = reinterpret_cast<uint64>(Generator.Get());
	Row.Created = Created; Row.Cycles = FPlatformTime::Cycles64(); Row.Seconds = FPlatformTime::Seconds();
	Sink->AddGenerator(Row);
}
void IMArm(const TArray<FString>& Args, UWorld* World)
{
	const bool NaturalAV = Args.Num() == 1 && (Args[0] == TEXT("NaturalAV=AcousticNaturalC24_20261011T053352Z")
		|| Args[0] == TEXT("NaturalAV=AcousticNaturalC25_20261011T063628Z")
		|| Args[0] == TEXT("NaturalAV=AcousticNaturalC25_20261011T063628Z_R2")
		|| Args[0] == TEXT("NaturalAV=AcousticNaturalC25_20261011T063628Z_R3")
		|| Args[0] == TEXT("NaturalAV=AcousticNaturalC26_20261011T082612Z")
		|| Args[0] == TEXT("NaturalAV=AcousticNaturalC26_20261011T082612Z_R2"));
	if ((!Args.IsEmpty() && !NaturalAV) || IMOwner || !World || World->WorldType != EWorldType::Editor) return;
	FString Text; TSharedPtr<FJsonObject> Parity;
	const FString ParityDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/ConsumerR108ControlRetry_20261008"));
	if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(ParityDirectory, TEXT("parity_report.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parity) || !Parity->GetBoolField(TEXT("pass"))) return;
	if (NaturalAV)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexRuntime"), Args[0].RightChop(10), TEXT("Consumer"));
		if (IFileManager::Get().DirectoryExists(*Directory) || !IFileManager::Get().MakeDirectory(*Directory, true)) return;
		OwnedOutputDirectory = Directory;
	}
	auto State = MakeUnique<FIMConsumerOwner>();
	State->Sink = MakeShared<FIMWaveConsumerSink, ESPMode::ThreadSafe>(++IMEpoch);
	if (!IMTransient(*State))
	{
		auto Error = MakeShared<FJsonObject>(); Error->SetStringField(TEXT("failure"), TEXT("transient_graph_build_failed"));
		IMSave(TEXT("consumer_status.json"), Error); OwnedOutputDirectory.Reset(); return;
	}
	auto Sink = State->Sink.ToSharedRef();
	State->Created = State->Source->OnGeneratorInstanceInfoCreated.AddLambda([Sink](const auto& Info) { IMGeneratorRow(Info, true, Sink); });
	State->Destroyed = State->Source->OnGeneratorInstanceInfoDestroyed.AddLambda([Sink](const auto& Info) { IMGeneratorRow(Info, false, Sink); });
	IMOwner = MoveTemp(State);
	IM_SetWaveConsumerSink(IMOwner->Sink.Get());
	IMOwner->Initialized = FWorldDelegates::OnWorldInitializedActors.AddLambda([](const UWorld::FActorsInitializedParams& Params)
	{
		if (!IMOwner || IMOwner->Bound || !Params.World || Params.World->WorldType != EWorldType::PIE) return;
		auto& State = *IMOwner;
		TArray<AIMAcousticFootstepCharacter*> Actors;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(Params.World); Actor; ++Actor) Actors.Add(*Actor);
		if (Actors.Num() != 1 || Actors[0]->HasActorBegunPlay() || Actors[0]->FootstepSound != State.Original.Get())
		{ State.Failure = TEXT("pre_beginplay_original_instance_boundary_mismatch"); return; }
		State.World = Params.World; State.WorldPath = Params.World->GetPathName(); State.ActorPath = Actors[0]->GetPathName();
		State.Device = Params.World->GetAudioDevice();
		if (!State.Device.IsValid() || !Actors[0]->GetFootstepAudio()) { State.Failure = TEXT("pre_beginplay_device_component_invalid"); return; }
		State.ComponentID = Actors[0]->GetFootstepAudio()->GetAudioComponentID();
		Actors[0]->FootstepSound = State.Source.Get(); State.Bound = true;
		UE_LOG(LogTemp, Display, TEXT("IMLogs R108_CONSUMER pre_beginplay_bound component=%llu device=%u world=%s"), State.ComponentID, State.Device.GetDeviceID(), *State.WorldPath);
	});
	IMOwner->Cleanup = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* CleanWorld, bool, bool)
	{
		if (IMOwner && IMOwner->World.Get() == CleanWorld)
		{ IMOwner->Cleaned = true; IM_SetWaveConsumerSink(nullptr); }
	});
	IMOwner->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		if (!IMOwner) return false;
		IMSave(TEXT("consumer_status.json"), IMStatus());
		return true;
	}), .1f);
	IMSave(TEXT("consumer_status.json"), IMStatus());
}
void IMFinish(const TArray<FString>& Args)
{
	if (!Args.IsEmpty() || !IMOwner) return;
	auto& State = *IMOwner;
	State.Finishing = true;
	FWorldDelegates::OnWorldInitializedActors.Remove(State.Initialized);
	IM_SetWaveConsumerSink(nullptr);
	if ((State.Bound && !State.Cleaned) || State.Sink->Operators.load(std::memory_order_acquire)
		|| State.Sink->ActiveGenerators.load() || State.Sink->InFlight.load() || !IM_WaveConsumerAcquisitionDrained())
	{ IMSave(TEXT("consumer_status.json"), IMStatus()); return; }
	State.Source->OnGeneratorInstanceInfoCreated.Remove(State.Created);
	State.Source->OnGeneratorInstanceInfoDestroyed.Remove(State.Destroyed);
	auto Report = IMStatus();
	TArray<TSharedPtr<FJsonValue>> Rows, Generators;
	const int32 Count = State.Sink->Published.load(std::memory_order_acquire);
	for (int32 Index = 0; Index < Count; ++Index) Rows.Add(MakeShared<FJsonValueObject>(IMRowJson(State.Sink->Rows[Index])));
	const int32 GeneratorCount = State.Sink->GeneratorCount.load();
	bool Ready = GeneratorCount <= FIMWaveConsumerSink::CapacityGenerators;
	for (int32 Index = 0; Index < FMath::Min(GeneratorCount, FIMWaveConsumerSink::CapacityGenerators); ++Index)
	{
		Ready &= State.Sink->GeneratorReady[Index].load(std::memory_order_acquire);
		const auto& Row = State.Sink->Generators[Index];
		auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("component_id"), LexToString(Row.ComponentID));
		Object->SetStringField(TEXT("instance_id"), LexToString(Row.InstanceID));
		Object->SetStringField(TEXT("generator_pointer"), LexToString(Row.GeneratorPointer));
		Object->SetBoolField(TEXT("created"), Row.Created);
		Object->SetNumberField(TEXT("native_platform_seconds"), Row.Seconds);
		Object->SetStringField(TEXT("native_cycles64"), LexToString(Row.Cycles));
		Generators.Add(MakeShared<FJsonValueObject>(Object));
	}
	Report->SetArrayField(TEXT("consumer_rows"), Rows); Report->SetArrayField(TEXT("generator_rows"), Generators);
	Report->SetBoolField(TEXT("complete"), Ready && State.Failure.IsEmpty() && State.Sink->Failure.load() == 0 && Count > 0 && GeneratorCount >= 2);
	Report->SetStringField(TEXT("state"), TEXT("RELEASED"));
	Report->SetBoolField(TEXT("device_handle_release_after_drain"), true);
	IMPCM(TEXT("ordinary_consumed_mono.f32"), State.Sink->PCM, State.Sink->PCMFrames);
	IMSave(TEXT("consumer_report.json"), Report);
	FTSTicker::GetCoreTicker().RemoveTicker(State.Ticker);
	FWorldDelegates::OnWorldCleanup.Remove(State.Cleanup);
	State.Device = FAudioDeviceHandle();
	IMOwner.Reset();
	IMSave(TEXT("consumer_status.json"), IMStatus());
	OwnedOutputDirectory.Reset();
	UE_LOG(LogTemp, Display, TEXT("IMLogs R108_CONSUMER released rows=%d generators=%d"), Count, GeneratorCount);
}
FAutoConsoleCommand IMCompareCommand(TEXT("IM.Acoustic.WaveConsumer.Compare"), TEXT("Private R108 native factory transparency comparison"), FConsoleCommandWithArgsDelegate::CreateStatic(&IMCompare));
FAutoConsoleCommandWithWorldAndArgs IMArmCommand(TEXT("IM.Acoustic.WaveConsumer.Arm"), TEXT("Private R108 transient ordinary PIE binding, default OFF"), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&IMArm));
FAutoConsoleCommand IMFinishCommand(TEXT("IM.Acoustic.WaveConsumer.Finish"), TEXT("Private R108 post-World drain/export"), FConsoleCommandWithArgsDelegate::CreateStatic(&IMFinish));
}

TSharedPtr<FIMWaveConsumerSink, ESPMode::ThreadSafe> IM_AcquireWaveConsumerSink()
{
	IMAcousticWavePlayerConsumerHarnessPrivate::IMAcquiring.fetch_add(1, std::memory_order_acq_rel);
	FIMWaveConsumerSink* Sink = IMAcousticWavePlayerConsumerHarnessPrivate::IMSink.load(std::memory_order_acquire);
	TSharedPtr<FIMWaveConsumerSink, ESPMode::ThreadSafe> Result;
	if (Sink) Result = Sink->AsShared();
	IMAcousticWavePlayerConsumerHarnessPrivate::IMAcquiring.fetch_sub(1, std::memory_order_release);
	return Result;
}
void IM_SetWaveConsumerSink(FIMWaveConsumerSink* Sink) { IMAcousticWavePlayerConsumerHarnessPrivate::IMSink.store(Sink, std::memory_order_release); }
bool IM_WaveConsumerAcquisitionDrained() { return IMAcousticWavePlayerConsumerHarnessPrivate::IMAcquiring.load(std::memory_order_acquire) == 0; }
#endif
