

#include "IMAcousticWavePlayerConsumerTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "HAL/IConsoleManager.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "DSP/BufferVectorOperations.h"
#include "DSP/MultichannelBuffer.h"
#include "MetasoundBuildError.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundEngineNodesNames.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundLog.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundParamHelper.h"
#include "MetasoundPrimitives.h"
#include "MetasoundRenderCost.h"
#include "MetasoundTrace.h"
#include "MetasoundTrigger.h"
#include "MetasoundVertex.h"
#include "MetasoundWave.h"
#include "Sound/SoundWaveProxyPlayer.h"

PRAGMA_DISABLE_INTERNAL_WARNINGS
#define LOCTEXT_NAMESPACE "IMAcousticWavePlayerConsumerTest"

namespace Metasound
{
namespace IMAcousticWavePlayerConsumerTestPrivate
{
		METASOUND_PARAM(InputTriggerPlay, "Play", "Play the wave player.")
		METASOUND_PARAM(InputTriggerStop, "Stop", "Stop the wave player.")
		METASOUND_PARAM(InputWaveAsset, "Wave Asset", "The wave asset to be real-time decoded.")
		METASOUND_PARAM(InputStartTime, "Start Time", "Time into the wave asset to start (seek) the wave asset.")
		METASOUND_PARAM(InputPitchShift, "Pitch Shift", "The pitch shift to use for the wave asset in semitones.")
		METASOUND_PARAM(InputLoop, "Loop", "Whether or not to loop between the start and specified end times.")
		METASOUND_PARAM(InputLoopStart, "Loop Start", "When to start the loop.")
		METASOUND_PARAM(InputLoopDuration, "Loop Duration", "The duration of the loop when wave player is enabled for looping. A negative value will loop the whole wave asset.")
		METASOUND_PARAM(InputMaintainAudioSync, "Maintain Audio Sync", "Force audio to seek to maintain sample accurate playback even if the source audio is latent")

		METASOUND_PARAM(OutputTriggerOnPlay, "On Play", "Triggers when Play is triggered.")
		METASOUND_PARAM(OutputTriggerOnDone, "On Finished", "Triggers when the wave played has finished playing or Stop is triggered.")
		METASOUND_PARAM(OutputTriggerOnNearlyDone, "On Nearly Finished", "Triggers when the wave played has almost finished playing (the block before it finishes). Allows time for logic to trigger different variations to play seamlessly.")
		METASOUND_PARAM(OutputTriggerOnLooped, "On Looped", "Triggers when the wave player has looped.")
		METASOUND_PARAM(OutputTriggerOnCuePoint, "On Cue Point", "Triggers when a wave cue point was hit during playback.")
		METASOUND_PARAM(OutputCuePointID, "Cue Point ID", "The cue point ID that was triggered.")
		METASOUND_PARAM(OutputCuePointLabel, "Cue Point Label", "The cue point label that was triggered (if there was a label parsed in the imported .wav file).")
		METASOUND_PARAM(OutputLoopRatio, "Loop Percent", "Returns the current playback location as a ratio of the loop (0-1) if looping is enabled.")
		METASOUND_PARAM(OutputPlaybackLocation, "Playback Location", "Returns the absolute position of the wave playback as a ratio of wave duration (0-1).")
		METASOUND_PARAM(OutputPlaybackTime, "Playback Time", "Returns the current absolute playback time of the wave.")
		METASOUND_PARAM(OutputAudioMono, "Out Mono", "The mono channel audio output.")
		METASOUND_PARAM(OutputAudioLeft, "Out Left", "The left channel audio output.")
		METASOUND_PARAM(OutputAudioRight, "Out Right", "The right channel audio output.")
		METASOUND_PARAM(OutputAudioFrontRight, "Out Front Right", "The front right channel audio output.")
		METASOUND_PARAM(OutputAudioFrontLeft, "Out Front Left", "The front left channel audio output.")
		METASOUND_PARAM(OutputAudioFrontCenter, "Out Front Center", "The front center channel audio output.")
		METASOUND_PARAM(OutputAudioLowFrequency, "Out Low Frequency", "The low frequency channel audio output.")
		METASOUND_PARAM(OutputAudioSideRight, "Out Side Right", "The side right channel audio output.")
		METASOUND_PARAM(OutputAudioSideLeft, "Out Side Left", "The side left channel audio output.")
		METASOUND_PARAM(OutputAudioBackRight, "Out Back Right", "The back right channel audio output.")
		METASOUND_PARAM(OutputAudioBackLeft, "Out Back Left", "The back left channel audio output.")


	static int32 IM_GetStockMaxDecodeSizeInFrames()
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.WavePlayer.MaxDecodeSizeInFrames"));
		return Variable ? Variable->GetInt() : 1024;
	}

		template<typename AudioChannelConfigurationInfoType>
		FVertexInterface GetVertexInterface()
		{


			FDataVertexMetadata InputLoopStartMetaData = METASOUND_GET_PARAM_METADATA(InputLoopStart);
			InputLoopStartMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata InputLoopDurationMetaData = METASOUND_GET_PARAM_METADATA(InputLoopDuration);
			InputLoopDurationMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata InputMaintainAudioSyncMetaData = METASOUND_GET_PARAM_METADATA(InputMaintainAudioSync);
			InputMaintainAudioSyncMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata OutputTriggerOnNearlyDoneMetaData = METASOUND_GET_PARAM_METADATA(OutputTriggerOnNearlyDone);
			OutputTriggerOnNearlyDoneMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata OutputTriggerOnCuePointMetaData = METASOUND_GET_PARAM_METADATA(OutputTriggerOnCuePoint);
			OutputTriggerOnCuePointMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata OutputCuePointIDMetaData = METASOUND_GET_PARAM_METADATA(OutputCuePointID);
			OutputCuePointIDMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata OutputCuePointLabelMetaData = METASOUND_GET_PARAM_METADATA(OutputCuePointLabel);
			OutputCuePointLabelMetaData.bIsAdvancedDisplay = true;

			FDataVertexMetadata OutputPlaybackLocationMetaData = METASOUND_GET_PARAM_METADATA(OutputPlaybackLocation);
			OutputPlaybackLocationMetaData.bIsAdvancedDisplay = true;


			FDataVertexMetadata OutputLoopRatioMetadata
			{
				METASOUND_GET_PARAM_TT(OutputLoopRatio),
				METASOUND_LOCTEXT("OutputLoopRatioNotPercentDisplayName", "Loop Ratio"),
				true
			};

			FVertexInterface VertexInterface(
				FInputVertexInterface(
					TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerPlay)),
					TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerStop)),
					TInputDataVertex<FWaveAsset>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputWaveAsset)),
					TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputStartTime), 0.0f),
					TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputPitchShift), 0.0f),
					TInputDataVertex<bool>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputLoop), false),
					TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME(InputLoopStart), InputLoopStartMetaData, 0.0f),
					TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME(InputLoopDuration), InputLoopDurationMetaData, -1.0f),
					TInputConstructorVertex<bool>(METASOUND_GET_PARAM_NAME(InputMaintainAudioSync), InputMaintainAudioSyncMetaData, false)
				),
				FOutputVertexInterface(
					TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnPlay)),
					TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnDone)),
					TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME(OutputTriggerOnNearlyDone), OutputTriggerOnNearlyDoneMetaData),
					TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnLooped)),
					TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME(OutputTriggerOnCuePoint), OutputTriggerOnCuePointMetaData),
					TOutputDataVertex<int32>(METASOUND_GET_PARAM_NAME(OutputCuePointID), OutputCuePointIDMetaData),
					TOutputDataVertex<FString>(METASOUND_GET_PARAM_NAME(OutputCuePointLabel), OutputCuePointLabelMetaData),
					TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME(OutputLoopRatio), OutputLoopRatioMetadata),
					TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME(OutputPlaybackLocation), OutputPlaybackLocationMetaData),
					TOutputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputPlaybackTime))
				)
			);


			for (const FOutputDataVertex& OutputDataVertex : AudioChannelConfigurationInfoType::GetAudioOutputs())
			{
				VertexInterface.GetOutputInterface().Add(OutputDataVertex);
			}

			return VertexInterface;
		}

		template<typename AudioChannelConfigurationInfoType>
		FNodeClassMetadata GetNodeInfo()
		{
			FNodeClassMetadata Info;
			Info.ClassName = { TEXT("IM"), TEXT("Acoustic Wave Consumer Trace"), AudioChannelConfigurationInfoType::GetVariantName() };
			Info.MajorVersion = 1;
			Info.MinorVersion = 1;
			Info.DisplayName = AudioChannelConfigurationInfoType::GetNodeDisplayName();
			Info.Description = METASOUND_LOCTEXT("Metasound_WavePlayerNodeDescription", "Plays a wave asset. The wave's channel configurations will be up or down mixed to match the wave players audio channel format.");
			Info.Author = PluginAuthor;
			Info.PromptIfMissing = PluginNodeMissingPrompt;
			Info.DefaultInterface = GetVertexInterface<AudioChannelConfigurationInfoType>();
			Info.Keywords = { METASOUND_LOCTEXT("WavePlayerSoundKeyword", "Sound"),
				                METASOUND_LOCTEXT("WavePlayerCueKeyword", "Cue")
			};

			return Info;
		}

	struct FIMWavePlayerOpArgs
	{
		FOperatorSettings Settings;
		TArray<FOutputDataVertex> OutputAudioVertices;
		FTriggerReadRef PlayTrigger;
		FTriggerReadRef StopTrigger;
		FWaveAssetReadRef WaveAsset;
		FTimeReadRef StartTime;
		FFloatReadRef PitchShift;
		FBoolReadRef bLoop;
		FTimeReadRef LoopStartTime;
		FTimeReadRef LoopDuration;
		bool bMaintainAudioSync;
		FNodeRenderCost CostReporter;
		uint64 TraceInstanceID = 0;
		uint32 TraceDeviceID = MAX_uint32;
	};


	class FIMWavePlayerOperator : public TExecutableOperator<FIMWavePlayerOperator>
	{
	public:


		static constexpr float MaxAbsPitchShiftInOctaves = 6.0f;

		FIMWavePlayerOperator(const FIMWavePlayerOpArgs& InArgs)
			: OperatorSettings(InArgs.Settings)
			, CostReporter(InArgs.CostReporter)
			, PlayTrigger(InArgs.PlayTrigger)
			, StopTrigger(InArgs.StopTrigger)
			, WaveAsset(InArgs.WaveAsset)
			, StartTime(InArgs.StartTime)
			, PitchShift(InArgs.PitchShift)
			, bLoop(InArgs.bLoop)
			, LoopStartTime(InArgs.LoopStartTime)
			, LoopDuration(InArgs.LoopDuration)
			, bMaintainAudioSync(InArgs.bMaintainAudioSync)
			, TriggerOnDone(FTriggerWriteRef::CreateNew(InArgs.Settings))
			, TriggerOnNearlyDone(FTriggerWriteRef::CreateNew(InArgs.Settings))
			, TriggerOnLooped(FTriggerWriteRef::CreateNew(InArgs.Settings))
			, TriggerOnCuePoint(FTriggerWriteRef::CreateNew(InArgs.Settings))
			, CuePointID(FInt32WriteRef::CreateNew(0))
			, CuePointLabel(FStringWriteRef::CreateNew(TEXT("")))
			, LoopPercent(FFloatWriteRef::CreateNew(0.0f))
			, PlaybackLocation(FFloatWriteRef::CreateNew(0.0f))
			, PlaybackTime(FTimeWriteRef::CreateNew(0.0))
		{
			for (const FOutputDataVertex& OutputAudioVertex : InArgs.OutputAudioVertices)
			{
				OutputAudioBufferVertexNames.Add(OutputAudioVertex.VertexName);

				FAudioBufferWriteRef AudioBuffer = FAudioBufferWriteRef::CreateNew(InArgs.Settings);
				OutputAudioBuffers.Add(AudioBuffer);



				MultiChannelOutputView.Emplace(AudioBuffer->GetData(), AudioBuffer->Num());
			}

			const float SampleRate = InArgs.Settings.GetSampleRate();
			const int32 NumChannels = OutputAudioBuffers.Num();
			FSoundWaveProxyPlayer::FSettings PlayerSettings(SampleRate, NumChannels);
			const int32 StockDecodeFrames = IM_GetStockMaxDecodeSizeInFrames();
			PlayerSettings.MaxDecodeSizeFrames = FMath::IsPowerOfTwo(StockDecodeFrames) ?
				StockDecodeFrames : FMath::RoundUpToPowerOfTwo(StockDecodeFrames);
			PlayerSettings.bMaintainAudioSync = bMaintainAudioSync;
			ProxyPlayer = FSoundWaveProxyPlayer::Create(PlayerSettings);
			TraceInstanceID = InArgs.TraceInstanceID;
			TraceDeviceID = InArgs.TraceDeviceID;
			TraceSink = IM_AcquireWaveConsumerSink();
			if (TraceSink) TraceSink->Operators.fetch_add(1);

		}

		~FIMWavePlayerOperator() override
		{
			if (TraceSink) TraceSink->Operators.fetch_sub(1, std::memory_order_release);
		}

		virtual void BindInputs(FInputVertexInterfaceData& InOutVertexData) override
		{

			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerPlay), PlayTrigger);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerStop), StopTrigger);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputWaveAsset), WaveAsset);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputStartTime), StartTime);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputPitchShift), PitchShift);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputLoop), bLoop);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputLoopStart), LoopStartTime);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputLoopDuration), LoopDuration);
			InOutVertexData.SetValue(METASOUND_GET_PARAM_NAME(InputMaintainAudioSync), bMaintainAudioSync);
		}

		virtual void BindOutputs(FOutputVertexInterfaceData& InOutVertexData) override
		{

			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), PlayTrigger);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnDone), TriggerOnDone);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnNearlyDone), TriggerOnNearlyDone);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnLooped), TriggerOnLooped);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnCuePoint), TriggerOnCuePoint);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputCuePointID), CuePointID);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputCuePointLabel), CuePointLabel);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputLoopRatio), LoopPercent);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputPlaybackLocation), PlaybackLocation);
			InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(OutputPlaybackTime), PlaybackTime);

			check(OutputAudioBuffers.Num() == OutputAudioBufferVertexNames.Num());

			for (int32 ChannelIndex = 0; ChannelIndex < OutputAudioBuffers.Num(); ChannelIndex++)
			{
				InOutVertexData.BindReadVertex(OutputAudioBufferVertexNames[ChannelIndex], OutputAudioBuffers[ChannelIndex]);
			}
		}

		void Execute()
		{
			METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(Metasound::FIMWavePlayerOperator::Execute);


			TriggerOnDone->AdvanceBlock();
			TriggerOnNearlyDone->AdvanceBlock();
			TriggerOnCuePoint->AdvanceBlock();
			TriggerOnLooped->AdvanceBlock();


			bDidWaveRenderThisBlock = false;

#if UE_USOUNDWAVE_HOT_RELOADING_ENABLED




			if (bIsPlaying && WaveAsset->IsSoundWaveValid() && CurrentSoundWaveData.IsValid() && ProxyPlayer.IsValid() && !ProxyPlayer->IsFinished())
			{
				const TSharedPtr<const FSoundWaveProxy> Proxy = WaveAsset->GetWaveProxy();
				if (Proxy.IsValid() && !Proxy->IsLatestData(CurrentSoundWaveData.Get()))
				{
					const float ResumeTimeSeconds = ProxyPlayer->GetCurrentPlaybackTimeSeconds();
					const TSharedRef<const FSoundWaveData> NewData = Proxy->GetSoundWaveDataRef();



					if (ProxyPlayer->SetSoundWave(NewData))
					{
						CurrentSoundWaveData = NewData;
						ProxyPlayer->SeekToTime(ResumeTimeSeconds);
					}
					else
					{
						bIsPlaying = false;
					}
				}
			}
#endif

			if (ProxyPlayer.IsValid())
			{
				ProxyPlayer->SetSpeed(CalculateSpeedFromPitchShift());
				ProxyPlayer->SetLoop(*bLoop, LoopStartTime->GetSeconds(), LoopDuration->GetSeconds());
				ExecuteSubblocks();
				*LoopPercent = ProxyPlayer->GetLoopProgress();
				*PlaybackLocation = ProxyPlayer->GetPlaybackProgress();
				*PlaybackTime = FTime::FromSeconds(ProxyPlayer->GetCurrentPlaybackTimeSeconds());
			}
			else
			{
				*LoopPercent = 0.0f;
				*PlaybackLocation = 0.0f;
				*PlaybackTime = FTime::FromSeconds(0.0f);
				ZeroFrameRange(0, OperatorSettings.GetNumFramesPerBlock());
			}

			if (bDidWaveRenderThisBlock)
			{
				CostReporter.SetRenderCost(1.f);
			}
			++TraceBlock;
		}

		void Reset(const IOperator::FResetParams& ResetParams)
		{
			ensure(ProxyPlayer->GetOutputSampleRate() == ResetParams.OperatorSettings.GetSampleRate());
			ensure(ProxyPlayer->GetOutputNumChannels() == OutputAudioBuffers.Num());
			OperatorSettings = ResetParams.OperatorSettings;

			TriggerOnDone->Reset();
			TriggerOnNearlyDone->Reset();
			TriggerOnLooped->Reset();
			TriggerOnCuePoint->Reset();

			*CuePointID = 0;
			*CuePointLabel = TEXT("");
			*LoopPercent = 0.0f;
			*PlaybackLocation = 0.0f;
			*PlaybackTime = FTime(0.0);
			for (const FAudioBufferWriteRef& BufferRef : OutputAudioBuffers)
			{
				BufferRef->Zero();
			}

			const float SampleRate = ResetParams.OperatorSettings.GetSampleRate();
			const int32 NumChannels = OutputAudioBuffers.Num();
			FSoundWaveProxyPlayer::FSettings PlayerSettings(SampleRate, NumChannels);
			const int32 StockDecodeFrames = IM_GetStockMaxDecodeSizeInFrames();
			PlayerSettings.MaxDecodeSizeFrames = FMath::IsPowerOfTwo(StockDecodeFrames) ?
				StockDecodeFrames : FMath::RoundUpToPowerOfTwo(StockDecodeFrames);
			PlayerSettings.bMaintainAudioSync = bMaintainAudioSync;
			ProxyPlayer->Reset(PlayerSettings);

			++TraceGraphEpoch;
			TraceBlock = 0;
			TraceTriggerOrdinal = 0;
			TraceFirstRenderPending = false;
			CurrentWaveAsset = FWaveAsset();

			CurrentSoundWaveData.Reset();
			bIsPlaying = false;
			bDidWaveRenderThisBlock = false;
		}

	private:

		void ExecuteSubblocks()
		{





			int32 PlayTrigIndex = 0;
			int32 NextPlayFrame = 0;
			const int32 NumPlayTrigs = PlayTrigger->NumTriggeredInBlock();

			int32 StopTrigIndex = 0;
			int32 NextStopFrame = 0;
			const int32 NumStopTrigs = StopTrigger->NumTriggeredInBlock();

			int32 CurrAudioFrame = 0;
			int32 NextAudioFrame = 0;
			const int32 LastAudioFrame = OperatorSettings.GetNumFramesPerBlock() - 1;
			const int32 NoTrigger = OperatorSettings.GetNumFramesPerBlock() << 1;

			while (NextAudioFrame < LastAudioFrame)
			{


				if (PlayTrigIndex < NumPlayTrigs)
				{
					NextPlayFrame = (*PlayTrigger)[PlayTrigIndex];
				}
				else
				{
					NextPlayFrame = NoTrigger;
				}


				if (StopTrigIndex < NumStopTrigs)
				{
					NextStopFrame = (*StopTrigger)[StopTrigIndex];
				}
				else
				{
					NextStopFrame = NoTrigger;
				}


				NextAudioFrame = FMath::Min(NextPlayFrame, NextStopFrame);


				if (NextAudioFrame == NoTrigger)
				{
					NextAudioFrame = OperatorSettings.GetNumFramesPerBlock();
				}


				while (CurrAudioFrame != NextAudioFrame)
				{
					if (bIsPlaying)
					{
						RenderFrameRange(CurrAudioFrame, NextAudioFrame);
					}
					else
					{
						ZeroFrameRange(CurrAudioFrame, NextAudioFrame);
					}
					CurrAudioFrame = NextAudioFrame;
				}


				if (CurrAudioFrame == NextPlayFrame)
				{
					const bool bStarted = StartPlaying();
					++TraceTriggerOrdinal;
					TraceFirstRenderPending = bStarted;
					if (TraceSink)
					{
						FIMWaveConsumerRow Row = TraceRow(EIMWaveConsumerRowKind::Accept, CurrAudioFrame, CurrAudioFrame);
						TraceSink->Append(Row);
					}
					if (!bStarted)
					{
						TriggerOnDone->TriggerFrame(CurrAudioFrame);
					}

					++PlayTrigIndex;
				}

				if (CurrAudioFrame == NextStopFrame)
				{
					bIsPlaying = false;
					TriggerOnDone->TriggerFrame(CurrAudioFrame);
					++StopTrigIndex;
				}
			}
		}

		void RenderFrameRange(int32 StartFrame, int32 EndFrame)
		{
			using namespace Audio;

			METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(Metasound::FIMWavePlayerOperator::RenderFrameRange);

			check(ProxyPlayer.IsValid());
			check(ProxyPlayer->IsPlayerValid());
			check(bIsPlaying);
			const int32 NumFramesToGenerate = EndFrame - StartFrame;


			bDidWaveRenderThisBlock = true;

			TArray<FSoundWaveProxyPlayer::FSourceEvent> SourceEvents;
			if (NumFramesToGenerate > 0)
			{
				Audio::FMultichannelBufferView OutputSlice = Audio::SliceMultichannelBufferView(MultiChannelOutputView, StartFrame, NumFramesToGenerate);
				ProxyPlayer->RenderMultiChannelAudio(StartFrame, OutputSlice, SourceEvents);
				bIsPlaying = !ProxyPlayer->IsFinished();
				if (TraceSink)
				{
					FIMWaveConsumerRow Row = TraceRow(EIMWaveConsumerRowKind::Render, StartFrame, EndFrame);
					Row.FirstRender = TraceFirstRenderPending;
					TraceSink->Append(Row, OutputAudioBuffers[0]->GetData() + StartFrame, NumFramesToGenerate);
				}
				TraceFirstRenderPending = false;
			}

			for (const FSoundWaveProxyPlayer::FSourceEvent& SourceEvent : SourceEvents)
			{
				switch (SourceEvent.Type)
				{
				case FSoundWaveProxyPlayer::FSourceEvent::Loop:
					TriggerOnLooped->TriggerFrame(SourceEvent.OutputFrameIndex);
					break;
				case FSoundWaveProxyPlayer::FSourceEvent::OnNearlyFinished:
					TriggerOnNearlyDone->TriggerFrame(SourceEvent.OutputFrameIndex);
					break;
				case FSoundWaveProxyPlayer::FSourceEvent::OnFinished:
					TriggerOnDone->TriggerFrame(SourceEvent.OutputFrameIndex);
					break;
				case FSoundWaveProxyPlayer::FSourceEvent::CuePoint:
					{
						check(ProxyPlayer->GetCuePoints().IsValidIndex(SourceEvent.CuePointIndex));
						const FSoundWaveCuePoint& CuePoint = ProxyPlayer->GetCuePoints()[SourceEvent.CuePointIndex];
						*CuePointID = CuePoint.CuePointID;
						*CuePointLabel = CuePoint.Label;
						TriggerOnCuePoint->TriggerFrame(SourceEvent.OutputFrameIndex);
					}
					break;
				}
			}
		}

		void ZeroFrameRange(int32 StartFrame, int32 EndFrame)
		{
			const int32 NumFramesToZero = EndFrame - StartFrame;
			if (NumFramesToZero > 0)
			{
				Audio::FMultichannelBufferView OutputSlice = Audio::SliceMultichannelBufferView(MultiChannelOutputView, StartFrame, NumFramesToZero);
				for (TArrayView<float>& ChannelView : OutputSlice)
				{
					FMemory::Memset(ChannelView.GetData(), 0, ChannelView.Num() * ChannelView.GetTypeSize());
				}
			}
		}

		float GetPitchShiftClamped() const
		{
			return FMath::Clamp(*PitchShift, -12.0f * MaxAbsPitchShiftInOctaves, 12.0f * MaxAbsPitchShiftInOctaves);
		}

		float CalculateSpeedFromPitchShift() const
		{
			return FMath::Pow(2.0f, GetPitchShiftClamped() / 12.0f);
		}

		bool StartPlaying()
		{
			check(ProxyPlayer.IsValid());
			TraceSetSoundWaveReturned = false;
			CurrentWaveAsset = *WaveAsset;



			const TSharedPtr<const FSoundWaveProxy> Proxy = CurrentWaveAsset.GetWaveProxy();
			if (!Proxy.IsValid())
			{
				bIsPlaying = false;
				return false;
			}
			const TSharedRef<const FSoundWaveData> NewSoundWaveData = Proxy->GetSoundWaveDataRef();
			CurrentSoundWaveData = NewSoundWaveData;
			TraceSetSoundWaveReturned = ProxyPlayer->SetSoundWave(NewSoundWaveData);
			if (TraceSetSoundWaveReturned)
			{
				ProxyPlayer->SetSpeed(CalculateSpeedFromPitchShift());
				ProxyPlayer->SetLoop(*bLoop, LoopStartTime->GetSeconds(), LoopDuration->GetSeconds());
				ProxyPlayer->SeekToTime(StartTime->GetSeconds());
			}

			bIsPlaying = ProxyPlayer.IsValid() && ProxyPlayer->IsPlayerValid() && !ProxyPlayer->IsFinished();
			return bIsPlaying;
		}

		FIMWaveConsumerRow TraceRow(EIMWaveConsumerRowKind Kind, int32 Start, int32 End) const
		{
			FIMWaveConsumerRow Row;
			Row.Kind = Kind;
			Row.InstanceID = TraceInstanceID;
			Row.DeviceID = TraceDeviceID;
			Row.OperatorID = reinterpret_cast<uint64>(this);
			Row.GraphEpoch = TraceGraphEpoch;
			Row.Block = TraceBlock;
			Row.TriggerOrdinal = TraceTriggerOrdinal;
			Row.Start = Start;
			Row.End = End;
			Row.Seconds = FPlatformTime::Seconds();
			Row.Cycles = FPlatformTime::Cycles64();
			Row.Pitch = *PitchShift;
			Row.SetSoundWaveReturned = TraceSetSoundWaveReturned;
			Row.PlayerValid = ProxyPlayer.IsValid() && ProxyPlayer->IsPlayerValid();
			Row.Playing = bIsPlaying;
			Row.OutputReference = reinterpret_cast<uint64>(GetDataReferenceID(OutputAudioBuffers[0]));
			const TSharedPtr<const FSoundWaveProxy> Proxy = CurrentWaveAsset.GetWaveProxy();
			Row.ProxyPointer = reinterpret_cast<uint64>(Proxy.Get());
			if (Proxy.IsValid() && CurrentSoundWaveData.IsValid())
			{
				Row.DataPointer = reinterpret_cast<uint64>(CurrentSoundWaveData.Get());
				Row.DataGUID = CurrentSoundWaveData->GetGUID();
				Row.WaveName = CurrentSoundWaveData->GetFName();
				Row.Package = CurrentSoundWaveData->GetPackageName();
				Row.Format = CurrentSoundWaveData->GetRuntimeFormat();
				Row.SourceRate = CurrentSoundWaveData->GetSampleRate();
				Row.SourceChannels = CurrentSoundWaveData->GetNumChannels();
				Row.SourceFrames = CurrentSoundWaveData->GetNumFrames();
			}
			return Row;
		}

		TSharedPtr<FIMWaveConsumerSink, ESPMode::ThreadSafe> TraceSink;
		uint64 TraceInstanceID = 0, TraceGraphEpoch = 1, TraceBlock = 0, TraceTriggerOrdinal = 0;
		uint32 TraceDeviceID = MAX_uint32;
		bool TraceSetSoundWaveReturned = false, TraceFirstRenderPending = false;

		FOperatorSettings OperatorSettings;
		FNodeRenderCost CostReporter;


		FTriggerReadRef PlayTrigger;
		FTriggerReadRef StopTrigger;
		FWaveAssetReadRef WaveAsset;
		FTimeReadRef StartTime;
		FFloatReadRef PitchShift;
		FBoolReadRef bLoop;
		FTimeReadRef LoopStartTime;
		FTimeReadRef LoopDuration;
		const bool bMaintainAudioSync;


		FTriggerWriteRef TriggerOnDone;
		FTriggerWriteRef TriggerOnNearlyDone;
		FTriggerWriteRef TriggerOnLooped;
		FTriggerWriteRef TriggerOnCuePoint;
		FInt32WriteRef CuePointID;
		FStringWriteRef CuePointLabel;
		FFloatWriteRef LoopPercent;
		FFloatWriteRef PlaybackLocation;
		FTimeWriteRef PlaybackTime;
		TArray<FAudioBufferWriteRef> OutputAudioBuffers;
		TArray<FName> OutputAudioBufferVertexNames;


		TSharedPtr<const FSoundWaveData> CurrentSoundWaveData;
		bool bIsPlaying = false;
		bool bDidWaveRenderThisBlock = false;
		FWaveAsset CurrentWaveAsset;
		TUniquePtr<FSoundWaveProxyPlayer> ProxyPlayer;
		Audio::FMultichannelBufferView MultiChannelOutputView;
	};

	class FIMWavePlayerOperatorFactory : public IOperatorFactory
	{
	public:
		FIMWavePlayerOperatorFactory(const TArray<FOutputDataVertex>& InOutputAudioVertices)
		: OutputAudioVertices(InOutputAudioVertices)
		{
		}

		virtual TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& InParams, FBuildResults& OutResults) override
		{

			const FInputVertexInterfaceData& Inputs = InParams.InputData;

			FIMWavePlayerOpArgs Args =
			{
				InParams.OperatorSettings,
				OutputAudioVertices,
				Inputs.GetOrCreateDefaultDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerPlay), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerStop), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<FWaveAsset>(METASOUND_GET_PARAM_NAME(InputWaveAsset), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<FTime>(METASOUND_GET_PARAM_NAME(InputStartTime), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InputPitchShift), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<bool>(METASOUND_GET_PARAM_NAME(InputLoop), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<FTime>(METASOUND_GET_PARAM_NAME(InputLoopStart), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultDataReadReference<FTime>(METASOUND_GET_PARAM_NAME(InputLoopDuration), InParams.OperatorSettings),
				Inputs.GetOrCreateDefaultValue<bool>(METASOUND_GET_PARAM_NAME(InputMaintainAudioSync), InParams.OperatorSettings),
				FNodeRenderCost(InParams.GraphRenderCost ? InParams.GraphRenderCost->AddNode(InParams.Node.GetInstanceID(), InParams.Environment) : FNodeRenderCost{}),
				InParams.Environment.Contains<uint64>(CoreInterface::Environment::InstanceID) ? InParams.Environment.GetValue<uint64>(CoreInterface::Environment::InstanceID) : 0,
				InParams.Environment.Contains<uint32>(Frontend::SourceInterface::Environment::DeviceID) ? InParams.Environment.GetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID) : MAX_uint32
			};

			return MakeUnique<FIMWavePlayerOperator>(Args);
		}
	private:
		TArray<FOutputDataVertex> OutputAudioVertices;
	};

	template<typename AudioChannelConfigurationInfoType>
	class TIMWavePlayerNode : public FBasicNode
	{

	public:
		TIMWavePlayerNode(const FNodeInitData& InInitData)
			: TIMWavePlayerNode(FNodeData(InInitData.InstanceName, InInitData.InstanceID, IMAcousticWavePlayerConsumerTestPrivate::GetVertexInterface<AudioChannelConfigurationInfoType>()), MakeShared<const FNodeClassMetadata>(CreateNodeClassMetadata()))
		{
		}

		TIMWavePlayerNode(FNodeData InNodeData, TSharedRef<const FNodeClassMetadata> InClassMetadata)
			: FBasicNode(MoveTemp(InNodeData), MoveTemp(InClassMetadata))
			, Factory(MakeOperatorFactoryRef<FIMWavePlayerOperatorFactory>(AudioChannelConfigurationInfoType::GetAudioOutputs()))
		{
		}


		virtual ~TIMWavePlayerNode() = default;

		static FNodeClassMetadata CreateNodeClassMetadata()
		{
			return IMAcousticWavePlayerConsumerTestPrivate::GetNodeInfo<AudioChannelConfigurationInfoType>();
		}

		virtual FOperatorFactorySharedRef GetDefaultOperatorFactory() const override
		{
			return Factory;
		}

	private:
		FOperatorFactorySharedRef Factory;
	};

	struct FIMMonoAudioChannelConfigurationInfo
	{
		static FText GetNodeDisplayName() { return METASOUND_LOCTEXT("Metasound_WavePlayerMonoNodeDisplayName", "Wave Player (1.0, Mono)"); }
		static FName GetVariantName() { return Metasound::EngineNodes::MonoVariant; }

		static TArray<FOutputDataVertex> GetAudioOutputs()
		{
			return {
				TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputAudioMono))
			};
		}
	};
	using FMonoWavePlayerNode = TIMWavePlayerNode<FIMMonoAudioChannelConfigurationInfo>;
	METASOUND_REGISTER_NODE(FMonoWavePlayerNode);

}
}
#undef LOCTEXT_NAMESPACE
PRAGMA_ENABLE_INTERNAL_WARNINGS
#endif
