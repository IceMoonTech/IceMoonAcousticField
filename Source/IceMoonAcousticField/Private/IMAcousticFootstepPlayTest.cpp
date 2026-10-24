#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "AudioDevice.h"
#include "Audio.h"
#include "Async/Async.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticReverbPreset.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "ImageUtils.h"
#include "IMAcousticStallDiagnostics.h"
#include "Misc/CoreDelegates.h"
#include "IMAcousticFootstepCharacter.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/TraceScreenshot.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "JsonObjectConverter.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"
#include <atomic>
#include "MetasoundSource.h"
#include "MetasoundFrontendDocument.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"
#include "UnrealEdGlobals.h"

namespace IMAcousticFootstepPlayTestPrivate
{
constexpr const TCHAR* MapPackage = TEXT("/IceMoonAcousticField/Tests/IM_FootstepAudition");
constexpr const TCHAR* EvidenceRelative = TEXT("AcousticV2/GameFootstepR89_20261007/Runtime");
constexpr const TCHAR* MasterStem = TEXT("footstep_master");
constexpr const TCHAR* CauseEvidenceRelative = TEXT("AcousticV2/GameFootstepR89_20261007/ReadyPublicationClock_20261007_0808/Runtime");
constexpr const TCHAR* LandingEvidenceRelative = TEXT("AcousticV2/GameLandingViewportR94_20261007/Runtime");
constexpr const TCHAR* BusBranchEvidenceRelative = TEXT("AcousticV2/GameBusBranchR96_20261007/Runtime");
constexpr const TCHAR* PostSourceHostEvidenceRelative = TEXT("AcousticV2/GamePostSourceHostR99_20261007/Runtime");
constexpr const TCHAR* StallEvidenceRelative = TEXT("AcousticV2/GameFootstepR89_20261007/StallCausePhases_20261007_0848/Runtime");
constexpr double PIEStartupTimeoutSeconds = 30.0;
constexpr double ScreenshotTimeoutSeconds = 8.0;
constexpr double AudioFileTimeoutSeconds = 20.0;
constexpr double AudioFileStableSeconds = 0.75;
constexpr double TotalWallTimeoutSeconds = 120.0;
constexpr double WalkWindowSeconds = 12.0;
constexpr double StopWindowSeconds = 2.0;
constexpr double ResumeTimeoutSeconds = 15.0;
constexpr double MinimumWalkPathCm = 100.0;
constexpr int32 ScreenshotPNGQuality = 0;
constexpr int32 ScreenshotReadbackCapacity = 3;

struct FIMObservedState
{
	uint64 Frame = 0;
	double WallSeconds = 0.0, MonotonicSeconds = 0.0;
	double WorldSeconds = 0.0;
	FString WorldPackage;
	FString CharacterPath;
	FString AudioPath;
	FString ListenerViewTarget;
	int32 Contacts = 0;
	int32 WoodContacts = 0;
	int32 StoneContacts = 0;
	int32 UnknownContacts = 0;
	int32 Landings = 0, WoodLandings = 0, StoneLandings = 0;
	int32 ManualJumpPresses = 0, ManualJumpReleases = 0, ManualJumpStarts = 0;
	bool bManualJumpStrong = false;
	bool bObserverFrameApplied = false;
	float ManualJumpStartSpeedCmS = 0.f;
	float LandingImpactCmS = 0.f, LandingGain = 0.f, LandingPitch = 0.f;
	double LandingSeconds = -1000.0;
	bool bAirborne = false;
	double VerticalSpeedCmS = 0.0;
	float AppVolume = 0.f;
	bool bAllowBackgroundAudio = false;
	uint64 SourceBlocks = 0, EnvironmentBlocks = 0, InvalidBlocks = 0;
	uint64 SourceFrames = 0, NoIRBlocks = 0, BusUnderruns = 0;
	uint64 ReverbNotFresh = 0, ReverbRejected = 0;
	uint32 EnvironmentTraceBlocks = 0;
	uint64 ContextGeneration = 0;
	bool bContextRegistryMatches = false;
	bool bWorkerProfilingEnabled = false;
	int32 GraphFramesPerBlock = 0;
	int32 EditorMarkerCount = 0, VisibleEditorMarkerCount = 0;
	double CharacterSpeedCmS = 0.0;
	bool bAudioPlaying = false;
	bool bListenerTransformValid = false;
	FVector CharacterLocation = FVector::ZeroVector;
	FVector AudioLocation = FVector::ZeroVector;
	FVector ListenerLocation = FVector::ZeroVector;
};

struct FIMPhaseEvidence
{
	FString Name;
	FIMObservedState Observed;
};

struct FIMScreenshotWriteResult
{
	bool bEncoded = false, bSaved = false, bWorkerThread = false;
	uint32 WorkerThreadId = 0;
	int64 EncodedBytes = 0;
	double EncodeBeginSeconds = 0.0, EncodeEndSeconds = 0.0, WriteEndSeconds = 0.0;
};

struct FIMViewportReadbackRequest
{
	FIntPoint Size = FIntPoint::ZeroValue;
	uint64 SourceDrawFrame = 0, CopyRenderFrame = 0, ReadyRenderFrame = 0;
	uint32 CopyRenderThreadId = 0;
	EPixelFormat Format = PF_Unknown;
	FString TextureName, Failure;
	int32 RowPitchPixels = 0, BufferHeight = 0;
	double CopyBeginSeconds = 0.0, CopyEndSeconds = 0.0, ReadySeconds = 0.0;
	double MapBeginSeconds = 0.0, MapEndSeconds = 0.0;
	bool bCopySubmitted = false, bPixelsCopied = false;
	FTextureRHIRef SourceTexture;
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	TArray<FColor> Pixels;
	std::atomic<bool> Complete{false};
};

struct FIMViewportReadbackQueue : TSharedFromThis<FIMViewportReadbackQueue, ESPMode::ThreadSafe>
{
	TArray<TSharedPtr<FIMViewportReadbackRequest, ESPMode::ThreadSafe>> Requests;
	FDelegateHandle EndFrameHandle;
	std::atomic<int32> InFlight{0};
	std::atomic<bool> Released{false};
	bool bStopping = false;
	uint64 LastSourceDrawFrame = MAX_uint64;
	int32 Submitted = 0, Mapped = 0;

	void Install_RT()
	{
		check(IsInRenderingThread());
		const auto Queue = AsShared();
		EndFrameHandle = FCoreDelegates::OnEndFrameRT.AddLambda([Queue]() { Queue->BeginFrame_RT(); });
	}

	void CopyPixels_RT(FIMViewportReadbackRequest& Request, const uint8* Data)
	{
		check(IsInRenderingThread());
		Request.Pixels.SetNumUninitialized(Request.Size.X * Request.Size.Y);
		for (int32 Row = 0; Row < Request.Size.Y; ++Row)
		{
			const uint8* Source = Data + int64(Row) * Request.RowPitchPixels * sizeof(FColor);
			FColor* Destination = Request.Pixels.GetData() + int64(Row) * Request.Size.X;
			for (int32 Column = 0; Column < Request.Size.X; ++Column)
			{
				const uint8* Pixel = Source + int64(Column) * sizeof(FColor);
				if (Request.Format == PF_A2B10G10R10)
				{
					uint32 Packed;
					FMemory::Memcpy(&Packed, Pixel, sizeof(Packed));
					Destination[Column] = FColor::MakeRequantizeFrom1010102(
						Packed & 1023u, (Packed >> 10) & 1023u, (Packed >> 20) & 1023u, Packed >> 30);
				}
				else if (Request.Format == PF_R8G8B8A8)
					Destination[Column] = FColor(Pixel[0], Pixel[1], Pixel[2], 255);
				else
					FMemory::Memcpy(Destination + Column, Pixel, sizeof(FColor));
				Destination[Column].A = 255;
			}
		}
	}

	void GTToRT(FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FIMViewportReadbackRequest, ESPMode::ThreadSafe>& Request, FTextureRHIRef Texture)
	{
		check(IsInRenderingThread());
		Request->CopyRenderFrame = GFrameCounterRenderThread;
		Request->CopyRenderThreadId = FPlatformTLS::GetCurrentThreadId();
		Request->CopyBeginSeconds = FPlatformTime::Seconds();
		const bool bCurrentFrame = Request->SourceDrawFrame == GFrameCounterRenderThread
			&& Request->SourceDrawFrame != LastSourceDrawFrame;
		if (bStopping || !bCurrentFrame || !Texture || !GRHISupportsMultithreading
			|| Texture->GetDesc().Extent != Request->Size || Texture->IsMultisampled()
			|| (Texture->GetFormat() != PF_B8G8R8A8 && Texture->GetFormat() != PF_R8G8B8A8
				&& Texture->GetFormat() != PF_A2B10G10R10))
		{
			Request->Failure = TEXT("Owned native draw frame/texture or nonblocking SDR readback contract failed on RT.");
			Request->Complete.store(true, std::memory_order_release);
			InFlight.fetch_sub(1, std::memory_order_relaxed);
			return;
		}
		LastSourceDrawFrame = Request->SourceDrawFrame;
		Request->Format = Texture->GetFormat();
		Request->TextureName = Texture->GetName().ToString();
		Request->SourceTexture = MoveTemp(Texture);
		Request->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("IMAcousticNativeViewportReadback"));
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(IMAcousticViewportPNG_CopySubmit_RT);
			RHICmdList.Transition(FRHITransitionInfo(Request->SourceTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			Request->Readback->EnqueueCopy(RHICmdList, Request->SourceTexture);
			RHICmdList.Transition(FRHITransitionInfo(Request->SourceTexture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		}
		Request->CopyEndSeconds = FPlatformTime::Seconds();
		Request->bCopySubmitted = true;
		Requests.Add(Request);
		++Submitted;
	}

	void BeginFrame_RT()
	{
		check(IsInRenderingThread());
		const auto KeepAlive = AsShared();
		for (int32 Index = Requests.Num() - 1; Index >= 0; --Index)
		{
			auto Request = Requests[Index];
			if (!Request->Readback->IsReady()) continue;
			Request->ReadySeconds = FPlatformTime::Seconds();
			Request->ReadyRenderFrame = GFrameCounterRenderThread;
			Request->MapBeginSeconds = FPlatformTime::Seconds();
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(IMAcousticViewportPNG_ReadyMap_RT);
				const auto* Data = static_cast<const uint8*>(
					Request->Readback->Lock(Request->RowPitchPixels, &Request->BufferHeight));
				if (Data && Request->RowPitchPixels >= Request->Size.X && Request->BufferHeight >= Request->Size.Y)
				{
					CopyPixels_RT(*Request, Data);
					Request->bPixelsCopied = true;
					++Mapped;
				}
				else
					Request->Failure = TEXT("Ready native viewport staging dimensions or mapped pixels are invalid.");
				if (Data) Request->Readback->Unlock();
			}
			Request->MapEndSeconds = FPlatformTime::Seconds();
			Request->Readback.Reset();
			Request->SourceTexture.SafeRelease();
			Request->Complete.store(true, std::memory_order_release);
			InFlight.fetch_sub(1, std::memory_order_relaxed);
			Requests.RemoveAtSwap(Index, EAllowShrinking::No);
		}
		if (bStopping && Requests.IsEmpty())
		{
			FCoreDelegates::OnEndFrameRT.Remove(EndFrameHandle);
			EndFrameHandle.Reset();
			Released.store(true, std::memory_order_release);
		}
	}

	void Destroy_RT()
	{
		check(IsInRenderingThread());
		bStopping = true;
		BeginFrame_RT();
	}
};

struct FIMScreenshotEvidence
{
	FString Name;
	FString Path;
	FIMObservedState Observed;
	uint64 FileObservedFrame = 0;
	int64 FileBytes = -1;
	bool bRequested = false;
	bool bReady = false;
	double RequestSeconds = 0.0, ProcessedSeconds = 0.0;
	uint64 RequestFrame = 0, ProcessedFrame = 0;
	int32 Width = 0, Height = 0;
	uint32 CaptureThreadId = 0;
	double PixelCopyBeginSeconds = 0.0, PixelCopyEndSeconds = 0.0;
	double ReadbackBeginSeconds = 0.0, ReadbackEndSeconds = 0.0;
	uint64 CaptureFrame = 0;
	FString CaptureViewportClient;
	bool bPixelsCaptured = false;
	bool bReadbackQueued = false, bReadbackConsumed = false;
	double ReadbackConsumedSeconds = 0.0;
	uint64 ReadbackConsumedFrame = 0;
	TSharedPtr<FIMViewportReadbackRequest, ESPMode::ThreadSafe> ReadbackRequest;
	TFuture<FIMScreenshotWriteResult> WriteTask;
	FIMScreenshotWriteResult WriteResult;
};

void AddScreenshotReadbackFields(const TSharedRef<FJsonObject>& Object, const FIMScreenshotEvidence& Shot)
{
	Object->SetStringField(TEXT("readback_measurement"), TEXT("GT nonblocking enqueue; producer RT frame and fence/map completion are separate"));
	Object->SetBoolField(TEXT("gpu_readback_queued"), Shot.bReadbackQueued);
	Object->SetBoolField(TEXT("gpu_readback_consumed"), Shot.bReadbackConsumed);
	Object->SetNumberField(TEXT("readback_consumed_gt_frame"), double(Shot.ReadbackConsumedFrame));
	Object->SetNumberField(TEXT("readback_consumed_seconds"), Shot.ReadbackConsumedSeconds);
	if (!Shot.ReadbackRequest || !Shot.ReadbackRequest->Complete.load(std::memory_order_acquire)) return;
	const auto& Request = *Shot.ReadbackRequest;
	Object->SetNumberField(TEXT("source_draw_gt_frame"), double(Request.SourceDrawFrame));
	Object->SetNumberField(TEXT("copy_rt_frame"), double(Request.CopyRenderFrame));
	Object->SetNumberField(TEXT("fence_ready_rt_frame"), double(Request.ReadyRenderFrame));
	Object->SetNumberField(TEXT("copy_rt_thread_id"), Request.CopyRenderThreadId);
	Object->SetNumberField(TEXT("copy_submit_rt_begin_seconds"), Request.CopyBeginSeconds);
	Object->SetNumberField(TEXT("copy_submit_rt_end_seconds"), Request.CopyEndSeconds);
	Object->SetNumberField(TEXT("fence_ready_seconds"), Request.ReadySeconds);
	Object->SetNumberField(TEXT("ready_map_rt_begin_seconds"), Request.MapBeginSeconds);
	Object->SetNumberField(TEXT("ready_map_rt_end_seconds"), Request.MapEndSeconds);
	Object->SetNumberField(TEXT("row_pitch_pixels"), Request.RowPitchPixels);
	Object->SetNumberField(TEXT("staging_buffer_height"), Request.BufferHeight);
	Object->SetStringField(TEXT("source_texture_name"), Request.TextureName);
	Object->SetStringField(TEXT("source_texture_format"), GPixelFormats[Request.Format].Name);
	Object->SetBoolField(TEXT("copy_submitted"), Request.bCopySubmitted);
	Object->SetBoolField(TEXT("ready_pixels_copied"), Request.bPixelsCopied);
	Object->SetStringField(TEXT("readback_failure"), Request.Failure);
}

struct FIMStallFrameProbe
{
	uint64 BeginFrame = 0, EndFrame = 0;
	FIMAcousticStallThreadClock Before, After;
	uint32 ScreenshotsProcessed = 0;
	bool bPartialBegin = false, bClosed = false, bAbandonedAtRestore = false;
};

struct FIMRunState
{
	explicit FIMRunState(FAutomationTestBase* InTest, int32 InPIEOrdinal = 1)
		: Test(InTest), PIEOrdinal(InPIEOrdinal)
	{
		StartedWallSeconds = FPlatformTime::Seconds();
		bPostSourceHostWindow = FParse::Param(FCommandLine::Get(), TEXT("IMFootstepPostSourceHostWindow"));
		bBusBranchWindow = bPostSourceHostWindow || FParse::Param(FCommandLine::Get(), TEXT("IMFootstepBusBranchWindow"));
		bLandingWindow = bBusBranchWindow || FParse::Param(FCommandLine::Get(), TEXT("IMFootstepLandingWindow"));
		// 新产品路径沿用现有定容CPU/IR观测；不复用旧窗口目录或nonce。
		bStallCauseWindow = bLandingWindow || FParse::Param(FCommandLine::Get(), TEXT("IMFootstepStallCauseWindow"));
		bCauseWindow = bStallCauseWindow || FParse::Param(FCommandLine::Get(), TEXT("IMFootstepIRCauseWindow"));
		if (bStallCauseWindow) StallFrames.SetNum(4096); // GT, before PIE; fixed capacity
		EvidenceDirectory = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), bPostSourceHostWindow ? PostSourceHostEvidenceRelative : (bBusBranchWindow ? BusBranchEvidenceRelative : (bLandingWindow ? LandingEvidenceRelative : (bStallCauseWindow ? StallEvidenceRelative : (bCauseWindow ? CauseEvidenceRelative : EvidenceRelative))))));
		if (PIEOrdinal == 2) EvidenceDirectory = FPaths::Combine(EvidenceDirectory, TEXT("Reentry"));
		MasterFile = FPaths::Combine(EvidenceDirectory, FString(MasterStem) + TEXT(".wav"));
		ReceiptFile = FPaths::Combine(EvidenceDirectory, TEXT("footstep_play_saved.json"));
	}

		~FIMRunState()
		{
			ShutdownScreenshotWriter();
			if (ScreenshotCaptureHandle.IsValid()) FCoreDelegates::OnEndFrame.Remove(ScreenshotCaptureHandle);
			RestoreCauseWorkerProfiling();
		}

		bool ConfigureNativeViewportResolution(UGameViewportClient* Client)
		{
			check(IsInGameThread());
			if (RequiredNativeViewportSize == FIntPoint::ZeroValue || bNativeViewportResolutionApplied) return true;
			UWorld* World = Client ? Client->GetWorld() : nullptr;
			FViewport* Viewport = Client ? Client->Viewport : nullptr;
			FSceneViewport* SceneViewport = Viewport ? Viewport->AsSceneViewport() : nullptr;
			if (!World || World->WorldType != EWorldType::PIE || !SceneViewport || Viewport->GetClient() != Client) return false;
			PreviousNativeViewportSize = SceneViewport->GetSizeXY();
			bPreviousNativeViewportFixed = SceneViewport->HasFixedSize();
			NativeResolutionPrepareBeginSeconds = FPlatformTime::Seconds();
			bNativeResolutionPreparedBeforeBeginPlay = !World->HasBegunPlay();
			SceneViewport->SetFixedViewportSize(RequiredNativeViewportSize.X, RequiredNativeViewportSize.Y);
			NativeResolutionPrepareEndSeconds = FPlatformTime::Seconds();
			if (SceneViewport->GetSizeXY() != RequiredNativeViewportSize)
			{
				Failure = TEXT("The owned native viewport could not retain the exact baseline render resolution.");
				return false;
			}
			bNativeViewportResolutionApplied = true;
			return true;
		}

		bool InstallScreenshotWriter(const TWeakPtr<FIMRunState>& WeakState)
	{
		check(IsInGameThread());
		if (bScreenshotWriterActive) return true;
		UWorld* World = PIEWorld.Get();
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		if (!Client || !Client->Viewport || Client->GetWorld() != World
			|| Client->Viewport->GetClient() != Client || !FModuleManager::Get().LoadModule(TEXT("ImageWrapper"))) return false;
			if (!ConfigureNativeViewportResolution(Client)) return false;
		ScreenshotViewportClient = Client;
			ScreenshotReadbackQueue = MakeShared<FIMViewportReadbackQueue, ESPMode::ThreadSafe>();
			ENQUEUE_RENDER_COMMAND(IMAcousticViewportReadbackInit_GTToRT)(
				[Queue = ScreenshotReadbackQueue](FRHICommandListImmediate&) { Queue->Install_RT(); });
		ScreenshotDrawHandle = Client->OnDrawn().AddLambda([WeakState]()
			{
				if (const auto Pinned = WeakState.Pin()) Pinned->NativeViewportDrawFrame = GFrameCounter;
			});
		ScreenshotCaptureHandle = FCoreDelegates::OnEndFrame.AddLambda([WeakState]()
			{
				if (const auto Pinned = WeakState.Pin()) Pinned->CaptureOwnedViewport();
			});
		bScreenshotWriterActive = bScreenshotWriterEverInstalled = ScreenshotCaptureHandle.IsValid() && ScreenshotDrawHandle.IsValid();
		return bScreenshotWriterActive;
	}

	void CaptureOwnedViewport()
	{
		check(IsInGameThread());
			PollScreenshotWrites(FPlatformTime::Seconds());
			if (bScreenshotStopRequested) { ShutdownScreenshotWriter(); return; }
			if (!bScreenshotWriterActive || !Screenshots.IsValidIndex(PendingScreenshotIndex)) return;
		UGameViewportClient* Client = ScreenshotViewportClient.Get();
		UWorld* World = PIEWorld.Get();
		FViewport* Viewport = Client ? Client->Viewport : nullptr;
		if (!Viewport || Client->GetWorld() != World || Viewport->GetClient() != Client)
		{
			Failure = TEXT("The owned native PIE viewport identity changed before pixel readback.");
			return;
		}
		auto& Shot = Screenshots[PendingScreenshotIndex];
			if (Shot.bReadbackQueued) return;
			if (NativeViewportDrawFrame != GFrameCounter || Shot.RequestFrame != GFrameCounter)
			{
				Failure = TEXT("No naturally drawn owned viewport exists in the actual event request frame; delayed frames are not relabeled.");
				PendingScreenshotIndex = INDEX_NONE;
				return;
			}
		const FIntPoint Size = Viewport->GetSizeXY();
			FTextureRHIRef Texture = Viewport->GetRenderTargetTexture();
			int32 Outstanding = 0;
			for (const auto& Existing : Screenshots)
				if (Existing.bReadbackQueued && !Existing.bReady) ++Outstanding;
			if (!Texture || Size.X <= 0 || Size.Y <= 0 || int64(Size.X) * Size.Y > MAX_int32
				|| (RequiredNativeViewportSize != FIntPoint::ZeroValue && Size != RequiredNativeViewportSize)
				|| Outstanding >= ScreenshotReadbackCapacity)
			{
				Failure = TEXT("The owned full-resolution viewport is unavailable or its bounded readback/PNG queue is full.");
				PendingScreenshotIndex = INDEX_NONE;
				return;
			}
			Shot.ReadbackRequest = MakeShared<FIMViewportReadbackRequest, ESPMode::ThreadSafe>();
			Shot.ReadbackRequest->Size = Size;
			Shot.ReadbackRequest->SourceDrawFrame = GFrameCounter;
		Shot.ReadbackBeginSeconds = FPlatformTime::Seconds();
		{
				TRACE_CPUPROFILER_EVENT_SCOPE(IMAcousticViewportPNG_Enqueue_GT);
				ScreenshotReadbackQueue->InFlight.fetch_add(1, std::memory_order_relaxed);
				ENQUEUE_RENDER_COMMAND(IMAcousticViewportReadback_GTToRT)(
					[Queue = ScreenshotReadbackQueue, Request = Shot.ReadbackRequest, Texture = MoveTemp(Texture)]
					(FRHICommandListImmediate& RHICmdList) { Queue->GTToRT(RHICmdList, Request, Texture); });
		}
		Shot.ReadbackEndSeconds = FPlatformTime::Seconds();
		Shot.CaptureViewportClient = Client->GetPathName();
			Shot.bReadbackQueued = true;
			ScreenshotPeakOutstanding = FMath::Max(ScreenshotPeakOutstanding, Outstanding + 1);
			PendingScreenshotIndex = INDEX_NONE;
	}

		void CaptureScreenshotPixels(int32 ShotIndex, int32 Width, int32 Height, TArray<FColor>&& Pixels)
	{
		check(IsInGameThread());
			if (!Screenshots.IsValidIndex(ShotIndex))
		{
			Failure = TEXT("A screenshot callback has no owned pending viewport request.");
			return;
		}
			auto& Shot = Screenshots[ShotIndex];
		if (Shot.bPixelsCaptured || Width <= 0 || Height <= 0 || int64(Width) * Height != Pixels.Num())
		{
			Failure = TEXT("Viewport pixel snapshot identity or full-resolution dimensions differ from the owned request.");
			return;
		}
		Shot.PixelCopyBeginSeconds = FPlatformTime::Seconds();
		Shot.Width = Width;
		Shot.Height = Height;
		Shot.CaptureThreadId = FPlatformTLS::GetCurrentThreadId();
		Shot.bPixelsCaptured = true;
		const uint64 SnapshotCycles = FPlatformTime::Cycles64();
		Shot.WriteTask = Async(EAsyncExecution::ThreadPool,
			[Pixels = MoveTemp(Pixels), Path = Shot.Path, Name = Shot.Name, Width, Height, SnapshotCycles]()
			{
				FIMScreenshotWriteResult Result;
				Result.WorkerThreadId = FPlatformTLS::GetCurrentThreadId();
				Result.bWorkerThread = !IsInGameThread();
				if (!Result.bWorkerThread) return Result;
				TArray64<uint8> Encoded;
				const FImageView Image(static_cast<const FColor*>(Pixels.GetData()), Width, Height);
				Result.EncodeBeginSeconds = FPlatformTime::Seconds();
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(IMAcousticViewportPNG_Encode);
					Result.bEncoded = FImageUtils::CompressImage(Encoded, TEXT(".png"), Image, ScreenshotPNGQuality);
				}
				Result.EncodeEndSeconds = FPlatformTime::Seconds();
				Result.EncodedBytes = Encoded.Num();
#if UE_SCREENSHOT_TRACE_ENABLED
				if (Result.bEncoded && SHOULD_TRACE_SCREENSHOT())
					TRACE_SCREENSHOT(*Name, SnapshotCycles, Width, Height, Encoded);
#endif
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(IMAcousticViewportPNG_Write);
					Result.bSaved = Result.bEncoded && FFileHelper::SaveArrayToFile(Encoded, *Path);
				}
				Result.WriteEndSeconds = FPlatformTime::Seconds();
				return Result;
			});
		Shot.PixelCopyEndSeconds = FPlatformTime::Seconds();
	}

		void ConsumeScreenshotReadback(int32 ShotIndex)
		{
			auto& Shot = Screenshots[ShotIndex];
			if (!Shot.ReadbackRequest || Shot.bReadbackConsumed
				|| !Shot.ReadbackRequest->Complete.load(std::memory_order_acquire)) return;
			auto& Request = *Shot.ReadbackRequest;
			Shot.bReadbackConsumed = true;
			Shot.ReadbackConsumedSeconds = FPlatformTime::Seconds();
			Shot.ReadbackConsumedFrame = GFrameCounter;
			Shot.CaptureFrame = Request.SourceDrawFrame;
			if (!Request.bPixelsCopied)
			{
				if (Failure.IsEmpty()) Failure = Request.Failure;
				return;
			}
			CaptureScreenshotPixels(ShotIndex, Request.Size.X, Request.Size.Y, MoveTemp(Request.Pixels));
			Shot.ProcessedSeconds = Shot.ReadbackConsumedSeconds;
			Shot.ProcessedFrame = GFrameCounter;
			if (bStallFrameHooksActive)
			{
				++StallScreenshotsProcessed;
				if (StallOpenFrame != INDEX_NONE) ++StallFrames[StallOpenFrame].ScreenshotsProcessed;
			}
		}

	void ConsumeScreenshotWrite(FIMScreenshotEvidence& Shot)
	{
		check(IsInGameThread());
		if (!Shot.WriteTask.IsValid() || !Shot.WriteTask.IsReady()) return;
		Shot.WriteResult = Shot.WriteTask.Get();
		Shot.WriteTask = TFuture<FIMScreenshotWriteResult>();
		Shot.FileBytes = IFileManager::Get().FileSize(*Shot.Path);
		Shot.FileObservedFrame = GFrameCounter;
		Shot.bReady = Shot.WriteResult.bWorkerThread && Shot.WriteResult.bSaved
			&& Shot.WriteResult.WorkerThreadId != Shot.CaptureThreadId
			&& Shot.FileBytes > 0 && Shot.FileBytes == Shot.WriteResult.EncodedBytes;
		if (!Shot.bReady && Failure.IsEmpty())
			Failure = TEXT("The owned off-GT PNG encoding/write did not produce its complete full-resolution file.");
		UE_LOG(LogTemp, Display,
			TEXT("IMLogs FootstepDemo screenshot_async_write name=%s ready=%d size=%dx%d quality=%d capture_tid=%u worker_tid=%u copy_ms=%.3f encode_wall_ms=%.3f write_wall_ms=%.3f bytes=%lld"),
			*Shot.Name, Shot.bReady, Shot.Width, Shot.Height, ScreenshotPNGQuality,
			Shot.CaptureThreadId, Shot.WriteResult.WorkerThreadId,
			(Shot.PixelCopyEndSeconds - Shot.PixelCopyBeginSeconds) * 1000.0,
			(Shot.WriteResult.EncodeEndSeconds - Shot.WriteResult.EncodeBeginSeconds) * 1000.0,
			(Shot.WriteResult.WriteEndSeconds - Shot.WriteResult.EncodeEndSeconds) * 1000.0, Shot.FileBytes);
	}

	void PollScreenshotWrites(const double Now)
	{
		check(IsInGameThread());
			for (int32 Index = 0; Index < Screenshots.Num(); ++Index)
		{
				ConsumeScreenshotReadback(Index);
				auto& Shot = Screenshots[Index];
			ConsumeScreenshotWrite(Shot);
			if (!Shot.bReady && Now > Shot.RequestSeconds + ScreenshotTimeoutSeconds && Failure.IsEmpty())
				Failure = TEXT("An owned asynchronous viewport PNG exceeded its original screenshot deadline.");
		}
	}

	void ShutdownScreenshotWriter()
	{
		check(IsInGameThread());
			if (!bScreenshotStopRequested && bScreenshotWriterEverInstalled)
		{
				if (auto* Client = ScreenshotViewportClient.Get())
				{
					Client->OnDrawn().Remove(ScreenshotDrawHandle);
					if (bNativeViewportResolutionApplied && Client->Viewport && Client->Viewport->GetClient() == Client)
					{
						if (FSceneViewport* SceneViewport = Client->Viewport->AsSceneViewport())
						{
							SceneViewport->SetFixedViewportSize(bPreviousNativeViewportFixed ? PreviousNativeViewportSize.X : 0,
								bPreviousNativeViewportFixed ? PreviousNativeViewportSize.Y : 0);
							bNativeViewportResolutionRestored = SceneViewport->HasFixedSize() == bPreviousNativeViewportFixed;
						}
					}
				}
			ScreenshotDrawHandle.Reset();
			ScreenshotViewportClient.Reset();
			bScreenshotWriterActive = false;
				bScreenshotStopRequested = true;
				PendingScreenshotIndex = INDEX_NONE;
				ENQUEUE_RENDER_COMMAND(IMAcousticViewportReadbackDestroy_GTToRT)(
					[Queue = ScreenshotReadbackQueue](FRHICommandListImmediate&) { Queue->Destroy_RT(); });
		}
			PollScreenshotWrites(FPlatformTime::Seconds());
			if (ScreenshotReadbackQueue && !ScreenshotReadbackQueue->Released.load(std::memory_order_acquire)) return;
			for (const auto& Shot : Screenshots)
		{
				if (Shot.ReadbackRequest && !Shot.bReadbackConsumed) return;
				if (Shot.WriteTask.IsValid()) return;
		}
			if (ScreenshotReadbackQueue)
			{
				ScreenshotGPUCopiesSubmitted = ScreenshotReadbackQueue->Submitted;
				ScreenshotGPUCopiesMapped = ScreenshotReadbackQueue->Mapped;
				ScreenshotReadbackQueue.Reset();
				bScreenshotGPUReadbacksDrained = true;
			}
			FCoreDelegates::OnEndFrame.Remove(ScreenshotCaptureHandle);
			ScreenshotCaptureHandle.Reset();
			bScreenshotDelegateRestored = true;
			bScreenshotWriterRemoved = bScreenshotWriterEverInstalled;
		bScreenshotWritesDrained = true;
	}

	void BeginStallFrame(const bool bPartialBegin = false)
	{
		check(IsInGameThread());
		if (!bStallFrameHooksActive) return;
		if (StallOpenFrame != INDEX_NONE) { ++StallFrameUnclosed; return; }
		if (StallFrameCount >= StallFrames.Num()) { ++StallFrameOverflow; return; }
		StallOpenFrame = StallFrameCount++;
		auto& Row = StallFrames[StallOpenFrame];
		Row.BeginFrame = GFrameCounter;
		Row.Before = IM_AcousticReadStallThreadClock();
		Row.bPartialBegin = bPartialBegin;
	}

	void EndStallFrame(const bool bClosed = true)
	{
		check(IsInGameThread());
		if (StallOpenFrame == INDEX_NONE) return;
		auto& Row = StallFrames[StallOpenFrame];
		Row.After = IM_AcousticReadStallThreadClock();
		Row.EndFrame = GFrameCounter;
		Row.ScreenshotsProcessed = StallScreenshotsProcessed;
		Row.bClosed = bClosed;
		Row.bAbandonedAtRestore = !bClosed;
		StallOpenFrame = INDEX_NONE;
	}

	void MarkStallScreenshotProcessed()
	{
		check(IsInGameThread());
		if (!bStallFrameHooksActive) return;
		if (!Screenshots.IsValidIndex(PendingScreenshotIndex))
		{ ++StallUnexpectedScreenshotCallbacks; return; }
		auto& Shot = Screenshots[PendingScreenshotIndex];
		Shot.ProcessedSeconds = FPlatformTime::Seconds();
		Shot.ProcessedFrame = GFrameCounter;
		++StallScreenshotsProcessed;
	}

	bool InstallStallFrameProbes(const TWeakPtr<FIMRunState>& WeakState)
	{
		check(IsInGameThread());
		if (!bStallCauseWindow) return true;
		if (bStallFrameHooksActive) return true;
		// These delegates, their only strong pins, and the latent test state are
		// all GT-owned. Weak captures avoid a cycle or a stale callback at EndPIE.
		StallBeginHandle = FCoreDelegates::OnBeginFrame.AddLambda([WeakState]()
		{ if (const auto Pinned = WeakState.Pin()) Pinned->BeginStallFrame(); });
		StallEndHandle = FCoreDelegates::OnEndFrame.AddLambda([WeakState]()
		{ if (const auto Pinned = WeakState.Pin()) Pinned->EndStallFrame(); });
		bStallFrameHooksActive = bStallFrameHooksEverInstalled = true;
		BeginStallFrame(true); // enable occurs part way through this engine frame
		return StallBeginHandle.IsValid() && StallEndHandle.IsValid();
	}

	void RemoveStallFrameProbes()
	{
		if (!bStallFrameHooksActive) return;
		check(IsInGameThread());
		FCoreDelegates::OnBeginFrame.Remove(StallBeginHandle);
		FCoreDelegates::OnEndFrame.Remove(StallEndHandle);
		EndStallFrame(false); // common exit can occur before OnEndFrame
		StallBeginHandle.Reset(); StallEndHandle.Reset();
		bStallFrameHooksActive = false;
		bStallFrameHooksRemoved = true;
	}

	bool EnableCauseWorkerProfiling()
	{
		check(IsInGameThread());
		if (!bCauseWindow || !CauseContext || !CauseContext->Device) return false;
		if (bCauseProfilingActive) return true;
		CauseProfilingEnableBeforeSeconds = FPlatformTime::Seconds();
		bCauseProfilingPrevious = CauseContext->Device->ProfilingEnabled.exchange(true, std::memory_order_acq_rel);
		if (bStallCauseWindow)
			bStallTimingPrevious = CauseContext->Device->StallTimingEnabled.exchange(true, std::memory_order_acq_rel);
		bCauseProfilingActive = bCauseProfilingEverEnabled = true;
		CauseProfilingEnableAfterSeconds = FPlatformTime::Seconds();
		return CauseContext->Device->ProfilingEnabled.load(std::memory_order_acquire)
			&& (!bStallCauseWindow || CauseContext->Device->StallTimingEnabled.load(std::memory_order_acquire));
	}

	void RestoreCauseWorkerProfiling()
	{
		RemoveStallFrameProbes();
		if (!bCauseProfilingActive) return;
		// The GT-owned latent state removes its weak delegates and restores the
		// switches on common exit; its GT destructor is cancellation fallback. Retaining this exact Context keeps the bridge alive and avoids
		// restoring an old value onto a replacement device/generation.
		CauseProfilingRestoreBeforeSeconds = FPlatformTime::Seconds();
		if (CauseContext && CauseContext->Device)
		{
			CauseContext->Device->ProfilingEnabled.store(bCauseProfilingPrevious, std::memory_order_release);
			bCauseProfilingRestored = CauseContext->Device->ProfilingEnabled.load(std::memory_order_acquire) == bCauseProfilingPrevious;
			if (bStallCauseWindow)
			{
				CauseContext->Device->StallTimingEnabled.store(bStallTimingPrevious, std::memory_order_release);
				bStallTimingRestored = CauseContext->Device->StallTimingEnabled.load(std::memory_order_acquire) == bStallTimingPrevious;
			}
		}
		CauseProfilingRestoreAfterSeconds = FPlatformTime::Seconds();
		bCauseProfilingActive = false;
	}

	FAutomationTestBase* Test = nullptr;
	int32 PIEOrdinal = 1;
	uint64 AudioComponentId = 0;
	uint64 PreviousAudioComponentId = 0;
	FString EvidenceDirectory;
	FString MasterFile;
	FString ReceiptFile;
	FString Failure;
	FString IRFailure, PreviousIRFailure;
	FString LoadedEditorMap;
	FString LoadedPIEMap;
	TWeakObjectPtr<UWorld> PIEWorld;
	TWeakObjectPtr<AIMAcousticFootstepCharacter> Character;
	TWeakObjectPtr<UAudioComponent> Audio;
	TArray<FIMPhaseEvidence> Phases;
	TArray<FIMScreenshotEvidence> Screenshots;
	double StartedWallSeconds = 0.0;
	double PIEStartedWallSeconds = 0.0;
	double WalkStartedWorldSeconds = 0.0;
	double StopStartedWorldSeconds = 0.0;
	double ResumeStartedWorldSeconds = 0.0;
	double ScreenshotDeadlineSeconds = 0.0;
	double AudioFileDeadlineSeconds = 0.0;
	double AudioFileStableSinceSeconds = 0.0;
	double WalkElapsedAtLastSnapshot = 0.0;
	FVector WalkStartLocation = FVector::ZeroVector;
	FVector PreviousWalkLocation = FVector::ZeroVector;
	double WalkPathCm = 0.0;
	double ResumeMaxSpeedCmS = 0.0;
	bool bRunInputSent = false;
	uint64 WarmEnvironmentBlocks = 0, WarmNoIRBlocks = 0;
	bool bSteadyIRBaselineCaptured = false;
	int32 WalkStartContacts = 0;
	int32 WalkStartWood = 0;
	int32 WalkStartStone = 0;
	int32 StopStartContacts = 0;
	int32 ResumeStartWood = 0;
	int32 ResumeStartStone = 0;
	int32 PendingScreenshotIndex = INDEX_NONE;
	int32 NextWalkScreenshot = 0;
	int64 PreviousAudioFileBytes = -1;
	int32 PIEWorldCount = 0;
	bool bPIEStartRequested = false;
	bool bRecordingStarted = false;
	bool bRecordingStopped = false;
	bool bAudioFileReady = false;
	bool bPIEEndQueued = false;
	bool bPIEEnded = false;
	bool bPassed = false;
	bool bCauseWindow = false, bStallCauseWindow = false, bLandingWindow = false, bBusBranchWindow = false;
	bool bWasAirborne = false, bJumpInputReleased = false;
	bool bManualJumpActionMappingHandled = false;
	int32 ManualJumpPressBaseline = 0, ManualJumpReleaseBaseline = 0, ManualJumpStartBaseline = 0;
	int32 ObservedLandings = 0, ObservedFootsteps = 0, ObservedJumpRises = 0, StopStartLandings = 0;
	double PreviousVerticalSpeed = 0.0;
	float MinLandingGain = 100.f, MaxLandingGain = 0.f;
	TArray<FIMStallFrameProbe> StallFrames;
	int32 StallFrameCount = 0, StallOpenFrame = INDEX_NONE;
	uint32 StallFrameOverflow = 0, StallFrameUnclosed = 0;
	uint32 StallScreenshotsProcessed = 0, StallUnexpectedScreenshotCallbacks = 0;
	FDelegateHandle ScreenshotCaptureHandle, ScreenshotDrawHandle;
		TSharedPtr<FIMViewportReadbackQueue, ESPMode::ThreadSafe> ScreenshotReadbackQueue;
	TWeakObjectPtr<UGameViewportClient> ScreenshotViewportClient;
	uint64 NativeViewportDrawFrame = MAX_uint64;
		FIntPoint RequiredNativeViewportSize = FIntPoint::ZeroValue, PreviousNativeViewportSize = FIntPoint::ZeroValue;
		bool bPreviousNativeViewportFixed = false, bNativeViewportResolutionApplied = false, bNativeViewportResolutionRestored = false;
		bool bNativeResolutionPreparedBeforeBeginPlay = false;
		double NativeResolutionPrepareBeginSeconds = 0.0, NativeResolutionPrepareEndSeconds = 0.0;
		int32 ScreenshotPeakOutstanding = 0, ScreenshotGPUCopiesSubmitted = 0, ScreenshotGPUCopiesMapped = 0;
		bool bScreenshotStopRequested = false, bScreenshotGPUReadbacksDrained = false;
	bool bScreenshotWriterActive = false, bScreenshotWriterEverInstalled = false;
	bool bScreenshotWriterRemoved = false, bScreenshotDelegateRestored = false, bScreenshotWritesDrained = false;
	FDelegateHandle StallBeginHandle, StallEndHandle;
	bool bStallFrameHooksActive = false, bStallFrameHooksEverInstalled = false, bStallFrameHooksRemoved = false;
	bool bStallTimingPrevious = false, bStallTimingRestored = false;
	FIMAcousticMetaSoundContextPtr CauseContext;
	TSharedPtr<FJsonObject> CauseSummary;
	TSharedPtr<FJsonObject> BusBranchSummary;
	TSharedPtr<FJsonObject> PostSourceHostSummary;
	TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe> EnvironmentHost;
	TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe> PreviousEnvironmentHost;
	FIMAcousticMetaSoundContextPtr PreviousEnvironmentContext;
	bool bPostSourceHostWindow = false;
	uint64 PreviousEnvironmentWorld = 0, PreviousHostInstance = 0, EnvironmentHostInstance = 0;
	uint32 CauseBeginTrace = 0, CauseEndTrace = 0;
	uint64 CauseEndNoIR = 0;
	bool bCauseProfilingPrevious = false, bCauseProfilingActive = false;
	bool bCauseProfilingEverEnabled = false, bCauseProfilingRestored = false;
	double CauseProfilingEnableBeforeSeconds = 0.0, CauseProfilingEnableAfterSeconds = 0.0;
	double CauseProfilingRestoreBeforeSeconds = 0.0, CauseProfilingRestoreAfterSeconds = 0.0;
};

enum class EIMStage : uint8
{
	WaitForPIE,
	Walk,
	Stop,
	Resume,
	WaitForAudioFile,
	WaitForPIEExit
};

int32 CountPIEWorlds(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	int32 Count = 0;
	if (!GEngine)
	{
		return Count;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			++Count;
			OutWorld = Context.World();
		}
	}
	return Count;
}

bool IsTargetPIEMap(const UWorld* World)
{
	if (!World)
	{
		return false;
	}
	const FString Package = World->GetOutermost()->GetName();
	if (Package == MapPackage)
	{
		return true;
	}
	const FString ExpectedDirectory = TEXT("/IceMoonAcousticField/Tests/");
	const FString ExpectedPIELeaf = TEXT("_IM_FootstepAudition");
	if (!Package.StartsWith(ExpectedDirectory) || !Package.EndsWith(ExpectedPIELeaf))
	{
		return false;
	}
	const FString Leaf = Package.RightChop(ExpectedDirectory.Len());
	return Leaf.StartsWith(TEXT("UEDPIE_"));
}

FIMObservedState Observe(FIMRunState& State, UWorld* World)
{
	FIMObservedState Result;
	Result.Frame = GFrameCounter;
	Result.MonotonicSeconds = FPlatformTime::Seconds();
	Result.WallSeconds = Result.MonotonicSeconds - State.StartedWallSeconds;
	Result.WorldSeconds = World ? World->GetTimeSeconds() : 0.0;
	Result.WorldPackage = World ? World->GetOutermost()->GetName() : FString();
	Result.AppVolume = FApp::GetVolumeMultiplier();
	Result.bAllowBackgroundAudio = GetDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;

	AIMAcousticFootstepCharacter* Character = State.Character.Get();
	UAudioComponent* Audio = State.Audio.Get();
	if (Character)
	{
		Result.CharacterPath = Character->GetPathName();
		Result.CharacterLocation = Character->GetActorLocation();
		Result.Landings = Character->GetLandingCount();
		Result.ManualJumpPresses = Character->GetManualJumpPressCount();
		Result.ManualJumpReleases = Character->GetManualJumpReleaseCount();
		Result.ManualJumpStarts = Character->GetManualJumpStartCount();
		Result.bManualJumpStrong = Character->WasLastManualJumpStrong();
		Result.ManualJumpStartSpeedCmS = Character->GetLastManualJumpStartSpeedCmS();
		Result.bObserverFrameApplied = Character->WasObserverFrameApplied();
		Result.WoodLandings = Character->GetWoodLandingCount();
		Result.StoneLandings = Character->GetStoneLandingCount();
		Result.LandingImpactCmS = Character->GetLastLandingImpactCmS();
		Result.LandingGain = Character->GetLastLandingGain();
		Result.LandingPitch = Character->GetLastLandingPitch();
		Result.LandingSeconds = Character->GetLastLandingSeconds();
		Result.bAirborne = Character->GetCharacterMovement()->IsFalling();
		Result.VerticalSpeedCmS = Character->GetVelocity().Z;
		Result.Contacts = Character->GetContactCount();
		Result.WoodContacts = Character->GetWoodContactCount();
		Result.StoneContacts = Character->GetStoneContactCount();
		Result.UnknownContacts = Character->GetUnknownContactCount();
		Result.CharacterSpeedCmS = Character->GetVelocity().Size2D();
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Character);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			const FString Name = Primitive->GetName();
			if (!Name.StartsWith(TEXT("IM_AcousticSourceIcon")) && !Name.StartsWith(TEXT("IM_AcousticSourceWave_"))) continue;
			++Result.EditorMarkerCount;
			if (Primitive->IsVisible() && !Primitive->bHiddenInGame) ++Result.VisibleEditorMarkerCount;
		}
	}
	if (Audio)
	{
		Result.AudioPath = Audio->GetPathName();
		Result.AudioLocation = Audio->GetComponentLocation();
		Result.bAudioPlaying = Audio->IsPlaying();
	}
	if (World)
	{
		if (APlayerController* Controller = World->GetFirstPlayerController())
		{
			if (AActor* ViewTarget = Controller->GetViewTarget())
			{
				Result.ListenerViewTarget = ViewTarget->GetPathName();
			}
		}
		if (FAudioDevice* Device = World->GetAudioDeviceRaw())
		{
			if (auto* Graph = Audio ? Cast<UMetaSoundSource>(Audio->Sound) : nullptr)
				Result.GraphFramesPerBlock = Graph->GetOperatorSettings(int32(Device->GetSampleRate())).GetNumFramesPerBlock();
			const auto RegisteredContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID);
			const auto Context = State.bCauseWindow ? State.CauseContext : RegisteredContext;
			Result.bContextRegistryMatches = Context && Context.Get() == RegisteredContext.Get();
			Result.ContextGeneration = Context && Context->Pool ? Context->Pool->WorldGeneration : 0;
			Result.bWorkerProfilingEnabled = Context && Context->Device
				&& Context->Device->ProfilingEnabled.load(std::memory_order_acquire);
			if (Context)
			{
				Result.SourceBlocks = Context->SourceBlocks.load(std::memory_order_acquire);
				Result.EnvironmentBlocks = Context->EnvironmentBlocks.load(std::memory_order_acquire);
				Result.InvalidBlocks = Context->InvalidBlocks.load(std::memory_order_acquire);
				Result.SourceFrames = Context->SourceFrames.load(std::memory_order_acquire);
				Result.NoIRBlocks = Context->NoIRBlocks.load(std::memory_order_acquire);
				Result.BusUnderruns = Context->BusUnderruns.load(std::memory_order_acquire);
				Result.EnvironmentTraceBlocks = Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
				Result.ReverbNotFresh = Context->Device->ReverbNotFreshBlocks.load(std::memory_order_acquire);
				Result.ReverbRejected = Context->Device->ReverbRejectedBlocks.load(std::memory_order_acquire);
			}
			FTransform ListenerTransform;
			if (Device->GetListenerTransform(0, ListenerTransform))
			{
				Result.bListenerTransformValid = true;
				Result.ListenerLocation = ListenerTransform.GetLocation();
			}
		}
	}
	return Result;
}

void AddObservedFields(TSharedRef<FJsonObject> Object, const FIMObservedState& Observed)
{
	Object->SetNumberField(TEXT("frame"), static_cast<double>(Observed.Frame));
	Object->SetNumberField(TEXT("wall_elapsed_seconds"), Observed.WallSeconds);
	Object->SetNumberField(TEXT("monotonic_seconds"), Observed.MonotonicSeconds);
	Object->SetNumberField(TEXT("world_time_seconds"), Observed.WorldSeconds);
	Object->SetStringField(TEXT("world_package"), Observed.WorldPackage);
	Object->SetStringField(TEXT("character_path"), Observed.CharacterPath);
	Object->SetStringField(TEXT("audio_component_path"), Observed.AudioPath);
	Object->SetStringField(TEXT("listener_view_target"), Observed.ListenerViewTarget);
	Object->SetNumberField(TEXT("contacts"), Observed.Contacts);
	Object->SetNumberField(TEXT("wood_contacts"), Observed.WoodContacts);
	Object->SetNumberField(TEXT("stone_contacts"), Observed.StoneContacts);
	Object->SetNumberField(TEXT("unknown_contacts"), Observed.UnknownContacts);
	Object->SetNumberField(TEXT("landings"), Observed.Landings);
	Object->SetNumberField(TEXT("manual_jump_presses"), Observed.ManualJumpPresses);
	Object->SetNumberField(TEXT("manual_jump_releases"), Observed.ManualJumpReleases);
	Object->SetNumberField(TEXT("manual_jump_starts"), Observed.ManualJumpStarts);
	Object->SetBoolField(TEXT("manual_jump_strong"), Observed.bManualJumpStrong);
	Object->SetNumberField(TEXT("manual_jump_start_speed_cm_s"), Observed.ManualJumpStartSpeedCmS);
	Object->SetBoolField(TEXT("observer_frame_applied"), Observed.bObserverFrameApplied);
	Object->SetNumberField(TEXT("wood_landings"), Observed.WoodLandings);
	Object->SetNumberField(TEXT("stone_landings"), Observed.StoneLandings);
	Object->SetNumberField(TEXT("landing_impact_cm_s"), Observed.LandingImpactCmS);
	Object->SetNumberField(TEXT("landing_gain"), Observed.LandingGain);
	Object->SetNumberField(TEXT("landing_pitch_st"), Observed.LandingPitch);
	Object->SetNumberField(TEXT("landing_event_world_seconds"), Observed.LandingSeconds);
	Object->SetBoolField(TEXT("airborne"), Observed.bAirborne);
	Object->SetNumberField(TEXT("vertical_speed_cm_s"), Observed.VerticalSpeedCmS);
	Object->SetNumberField(TEXT("app_volume"), Observed.AppVolume);
	Object->SetBoolField(TEXT("allow_background_audio"), Observed.bAllowBackgroundAudio);
	Object->SetNumberField(TEXT("source_blocks"), double(Observed.SourceBlocks));
	Object->SetNumberField(TEXT("environment_blocks"), double(Observed.EnvironmentBlocks));
	Object->SetNumberField(TEXT("invalid_blocks"), double(Observed.InvalidBlocks));
	Object->SetNumberField(TEXT("source_frames"), double(Observed.SourceFrames));
	Object->SetNumberField(TEXT("no_ir_blocks"), double(Observed.NoIRBlocks));
	Object->SetNumberField(TEXT("bus_underruns"), double(Observed.BusUnderruns));
	Object->SetNumberField(TEXT("environment_trace_blocks"), Observed.EnvironmentTraceBlocks);
	Object->SetNumberField(TEXT("context_generation"), double(Observed.ContextGeneration));
	Object->SetBoolField(TEXT("context_registry_matches"), Observed.bContextRegistryMatches);
	Object->SetBoolField(TEXT("worker_profiling_enabled"), Observed.bWorkerProfilingEnabled);
	Object->SetNumberField(TEXT("reverb_not_fresh"), double(Observed.ReverbNotFresh));
	Object->SetNumberField(TEXT("reverb_renderer_rejected"), double(Observed.ReverbRejected));
	Object->SetNumberField(TEXT("graph_frames_per_block"), Observed.GraphFramesPerBlock);
	Object->SetNumberField(TEXT("editor_marker_components"), Observed.EditorMarkerCount);
	Object->SetNumberField(TEXT("visible_editor_markers_in_game"), Observed.VisibleEditorMarkerCount);
	Object->SetNumberField(TEXT("character_speed_cm_s"), Observed.CharacterSpeedCmS);
	Object->SetBoolField(TEXT("audio_component_playing"), Observed.bAudioPlaying);
	Object->SetBoolField(TEXT("listener_transform_valid"), Observed.bListenerTransformValid);
	Object->SetStringField(TEXT("character_location_cm"), Observed.CharacterLocation.ToString());
	Object->SetStringField(TEXT("audio_location_cm"), Observed.AudioLocation.ToString());
	Object->SetStringField(TEXT("listener_location_cm"), Observed.ListenerLocation.ToString());
}

void RecordPhase(FIMRunState& State, UWorld* World, const TCHAR* Name)
{
	FIMPhaseEvidence& Phase = State.Phases.AddDefaulted_GetRef();
	Phase.Name = Name;
	Phase.Observed = Observe(State, World);
	if (State.bCauseWindow && !Phase.Observed.bContextRegistryMatches && State.Failure.IsEmpty())
		State.Failure = TEXT("IR cause phase registry no longer names the retained capture Context.");
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo preview_health phase=%s marker_components=%d visible_game_markers=%d speed_cm_s=%.3f"),
		Name, Phase.Observed.EditorMarkerCount, Phase.Observed.VisibleEditorMarkerCount, Phase.Observed.CharacterSpeedCmS);
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo audio_health phase=%s app_volume=%.3f background_audio=%d graph_frames=%d source_blocks=%llu environment_blocks=%llu invalid_blocks=%llu no_ir=%llu bus_underruns=%llu"),
		Name, Phase.Observed.AppVolume, Phase.Observed.bAllowBackgroundAudio, Phase.Observed.GraphFramesPerBlock,
		Phase.Observed.SourceBlocks, Phase.Observed.EnvironmentBlocks, Phase.Observed.InvalidBlocks, Phase.Observed.NoIRBlocks, Phase.Observed.BusUnderruns);
	UE_LOG(LogTemp, Display,
		TEXT("IMLogs FootstepDemo PlaySaved phase=%s frame=%llu world_s=%.3f contacts=%d wood=%d stone=%d unknown=%d character=%s source=%s listener=%s audio_playing=%d"),
		Name,
		static_cast<unsigned long long>(Phase.Observed.Frame),
		Phase.Observed.WorldSeconds,
		Phase.Observed.Contacts,
		Phase.Observed.WoodContacts,
		Phase.Observed.StoneContacts,
		Phase.Observed.UnknownContacts,
		*Phase.Observed.CharacterLocation.ToString(),
		*Phase.Observed.AudioLocation.ToString(),
		*Phase.Observed.ListenerLocation.ToString(),
		Phase.Observed.bAudioPlaying ? 1 : 0);
}

bool ExportStallFrameEvidence(FIMRunState& State, FJsonObject& Summary)
{
	if (!State.bStallCauseWindow) return true;
	FString Frames = TEXT("index,begin_frame,end_frame,before_seconds,after_seconds,thread_id_before,thread_id_after,cpu100ns_before,cpu100ns_after,cpu_cycles_before,cpu_cycles_after,valid_before,valid_after,partial_begin,closed,abandoned_at_restore,screenshots_processed\n");
	uint32 Closed = 0, Invalid = 0, Partial = 0, Abandoned = 0;
	for (int32 I = 0; I < State.StallFrameCount; ++I)
	{
		const auto& R = State.StallFrames[I];
		Closed += R.bClosed; Partial += R.bPartialBegin; Abandoned += R.bAbandonedAtRestore;
		if (!IM_AcousticStallClockPairValid(R.Before, R.After)) ++Invalid;
		Frames += FString::Printf(TEXT("%d,%llu,%llu,%.17g,%.17g,%u,%u,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u\n"),
			I, R.BeginFrame, R.EndFrame, R.Before.Seconds, R.After.Seconds,
			R.Before.ThreadId, R.After.ThreadId, R.Before.Cpu100ns, R.After.Cpu100ns,
			R.Before.CpuCycles, R.After.CpuCycles, uint32(R.Before.Valid), uint32(R.After.Valid),
			uint32(R.bPartialBegin), uint32(R.bClosed), uint32(R.bAbandonedAtRestore), R.ScreenshotsProcessed);
	}
	Summary.SetBoolField(TEXT("stall_cause_window"), true);
	Summary.SetBoolField(TEXT("stall_timing_previous"), State.bStallTimingPrevious);
	Summary.SetBoolField(TEXT("stall_timing_restored"), State.bStallTimingRestored);
	Summary.SetBoolField(TEXT("stall_timing_terminal"), State.CauseContext->Device->StallTimingEnabled.load(std::memory_order_acquire));
	Summary.SetBoolField(TEXT("gt_frame_hooks_removed"), State.bStallFrameHooksRemoved);
	Summary.SetNumberField(TEXT("gt_stall_rows"), State.StallFrameCount);
	Summary.SetNumberField(TEXT("gt_stall_closed_rows"), Closed);
	Summary.SetNumberField(TEXT("gt_stall_partial_begin_rows"), Partial);
	Summary.SetNumberField(TEXT("gt_stall_abandoned_restore_rows"), Abandoned);
	Summary.SetNumberField(TEXT("gt_stall_invalid_clock_pairs"), Invalid);
	Summary.SetNumberField(TEXT("gt_stall_unclosed_rows"), State.StallFrameUnclosed);
	Summary.SetNumberField(TEXT("gt_stall_overflows"), State.StallFrameOverflow);
	Summary.SetNumberField(TEXT("screenshot_processed_callbacks"), State.StallScreenshotsProcessed);
	Summary.SetNumberField(TEXT("unexpected_screenshot_callbacks"), State.StallUnexpectedScreenshotCallbacks);
	const bool bSaved = FFileHelper::SaveStringToFile(Frames,
		*FPaths::Combine(State.EvidenceDirectory, TEXT("gt_stall_frames.csv")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool bComplete = bSaved && State.bStallFrameHooksEverInstalled && State.bStallFrameHooksRemoved
		&& State.bStallTimingRestored && Closed > 0 && !Invalid && !State.StallFrameUnclosed
		&& !State.StallFrameOverflow && !State.StallUnexpectedScreenshotCallbacks
		&& State.StallScreenshotsProcessed == uint32(State.Screenshots.Num());
	Summary.SetBoolField(TEXT("gt_stall_frame_evidence_complete"), bComplete);
	return bComplete;
}

bool ExportBusBranchEvidence(FIMRunState& State)
{
	if (!State.bBusBranchWindow) return true;
	check(IsInGameThread());
	const auto Context = State.CauseContext;
	State.BusBranchSummary = MakeShared<FJsonObject>();
	auto& Summary = *State.BusBranchSummary;
	Summary.SetStringField(TEXT("schema"), TEXT("IMAcousticBusBranchCapture.v1"));
	Summary.SetStringField(TEXT("clock"), TEXT("FPlatformTime::Seconds"));
	if (!Context || !Context->Device || !Context->Pool || !State.bPIEEnded
		|| !Context->Stopped.load(std::memory_order_acquire))
	{
		Summary.SetBoolField(TEXT("export_preconditions_complete"), false);
		return false;
	}
	Summary.SetBoolField(TEXT("export_preconditions_complete"), true);
	const uint32 BusCount = Context->CapturedBusReadCount.load(std::memory_order_acquire);
	const uint32 BusLimit = FMath::Min(BusCount, uint32(Context->CapturedBusReads.Num()));
	const uint32 OrderClaims = Context->CapturedTransportOrderClaims.load(std::memory_order_acquire);
	const uint32 OrderLimit = FMath::Min(OrderClaims, uint32(Context->CapturedTransportOrder.Num()));
	const uint64 WorldGeneration = Context->Pool->WorldGeneration;
	const uint32 SampleRate = Context->Device->SampleRate;
	uint32 OrderPrefix = 0, OrderIncomplete = 0, OrderReady = 0;
	for (uint32 Index = 0; Index < OrderLimit; ++Index)
	{
		const uint32 Word = Index / 64u;
		const bool bReady = Context->CapturedTransportOrderReady && Word < Context->CapturedTransportOrderReadyWordCount
			&& (Context->CapturedTransportOrderReady[Word].load(std::memory_order_acquire) & (uint64(1) << (Index % 64u))) != 0;
		if (!bReady) { ++OrderIncomplete; continue; }
		++OrderReady;
		if (Index == OrderPrefix) ++OrderPrefix;
	}
	FString Bus = TEXT("ordinal,device_id,sample_rate,world_generation,preparation_id,frames,prime_frames,capacity_frames,input_count,active_count,min_available,max_available,blocking_slot,blocking_available,blocking_queue_after,min_popped,blocking_audio_id,blocking_push_blocks,underruns_before,underruns_after,read_blocks,priming_blocks,silent_blocks,push_blocks,push_failures,begin_seconds,end_seconds,result,primed_before,primed_after,transport_failed_before,transport_failed_after,blocking_active,blocking_active_after,returned_fresh\n");
	int32 FirstFailure = INDEX_NONE;
	uint64 AccountedUnderruns = 0, PreviousUnderruns = 0;
	bool bBusRowsValid = BusCount == BusLimit && BusCount > 0;
	for (uint32 Index = 0; Index < BusLimit; ++Index)
	{
		const auto& Row = Context->CapturedBusReads[int32(Index)];
		Bus += FString::Printf(TEXT("%u,%u,%u,%llu,%llu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%u,%u,%u,%u,%u,%u,%u,%u\n"),
			Row.Ordinal, Context->DeviceId, SampleRate, WorldGeneration, Context->TestCapturePreparationId,
			Row.Frames, Row.PrimeFrames, Row.CapacityFrames, Row.InputCount, Row.ActiveCount,
			Row.MinAvailable, Row.MaxAvailable, Row.BlockingSlot, Row.BlockingAvailable, Row.BlockingQueueAfter, Row.MinPopped,
			Row.BlockingAudioId, Row.BlockingPushBlocks, Row.UnderrunsBefore, Row.UnderrunsAfter,
			Row.ReadBlocks, Row.PrimingBlocks, Row.SilentBlocks, Row.PushBlocks, Row.PushFailures,
			Row.BeginSeconds, Row.EndSeconds, uint32(Row.Result), uint32(Row.PrimedBefore), uint32(Row.PrimedAfter),
			uint32(Row.TransportFailedBefore), uint32(Row.TransportFailedAfter), uint32(Row.BlockingActive),
			uint32(Row.BlockingActiveAfter), uint32(Row.ReturnedFresh));
		bBusRowsValid = bBusRowsValid && Row.Ordinal == Index && Row.Frames == int32(FIMAcousticMetaSoundContext::Frames)
			&& Row.BeginSeconds > 0.0 && Row.EndSeconds >= Row.BeginSeconds
			&& Row.UnderrunsBefore == PreviousUnderruns && Row.UnderrunsAfter >= Row.UnderrunsBefore;
		if (Row.UnderrunsAfter > Row.UnderrunsBefore)
		{
			if (FirstFailure == INDEX_NONE) FirstFailure = int32(Index);
			AccountedUnderruns += Row.UnderrunsAfter - Row.UnderrunsBefore;
			bBusRowsValid = bBusRowsValid && Row.UnderrunsAfter - Row.UnderrunsBefore == 1 && !Row.ReturnedFresh
				&& (Row.Result == EIMAcousticBusReadResult::LatchedFault || Row.Result == EIMAcousticBusReadResult::ShortAvailable
					|| Row.Result == EIMAcousticBusReadResult::ShortPop);
		}
		PreviousUnderruns = Row.UnderrunsAfter;
	}
	FString Order = TEXT("index,group,reader_ordinal,source_trace_index,thread_id,device_id,sample_rate,slot,audio_id,world_generation,source_generation,seconds,push_blocks,underruns,frames,available,returned,active,phase,reason,finished,fresh,work_flags,work_cycles,thread_cpu_cycles\n");
	bool bReaderBegin = false, bReaderDecision = false, bReaderEnd = false, bOrderRowsValid = OrderPrefix > 0;
	for (uint32 Index = 0; Index < OrderPrefix; ++Index)
	{
		const auto& Event = Context->CapturedTransportOrder[int32(Index)];
		Order += FString::Printf(TEXT("%u,%u,%u,%u,%u,%u,%u,%d,%llu,%llu,%llu,%.17g,%llu,%llu,%d,%d,%d,%d,%u,%u,%u,%u,%u,%llu,%llu\n"),
			Event.Index, Event.Group, Event.ReaderOrdinal, Event.SourceTraceIndex, Event.ThreadId, Event.DeviceId, Event.SampleRate,
			Event.Slot, Event.AudioId, Event.WorldGeneration, Event.SourceGeneration, Event.Seconds, Event.PushBlocks, Event.Underruns,
			Event.Frames, Event.Available, Event.Returned, Event.Active, uint32(Event.Phase), uint32(Event.Reason),
			uint32(Event.Finished), uint32(Event.Fresh), Event.WorkFlags, Event.WorkCycles, Event.ThreadCpuCycles);
		bOrderRowsValid = bOrderRowsValid && Event.Index == Index && Event.DeviceId == Context->DeviceId
			&& Event.SampleRate == SampleRate && Event.WorldGeneration == WorldGeneration && Event.Seconds > 0.0;
		if (FirstFailure != INDEX_NONE && Event.ReaderOrdinal == uint32(FirstFailure))
		{
			bReaderBegin |= Event.Phase == EIMAcousticTransportOrderPhase::ReaderBegin;
			bReaderDecision |= Event.Phase == EIMAcousticTransportOrderPhase::ReaderDecision
				&& Event.Returned == int32(Context->CapturedBusReads[FirstFailure].Result) && !Event.Fresh;
			bReaderEnd |= Event.Phase == EIMAcousticTransportOrderPhase::ReaderEnd
				&& Event.Returned == int32(Context->CapturedBusReads[FirstFailure].Result) && !Event.Fresh;
		}
	}
	const uint64 TerminalUnderruns = Context->BusUnderruns.load(std::memory_order_acquire);
	const bool bBusOverflow = Context->CapturedBusReadOverflow.load(std::memory_order_acquire);
	const bool bOrderOverflow = Context->CapturedTransportOrderOverflow.load(std::memory_order_acquire);
	const bool bCountsStable = BusCount == Context->CapturedBusReadCount.load(std::memory_order_acquire)
		&& OrderClaims == Context->CapturedTransportOrderClaims.load(std::memory_order_acquire);
	const bool bBusComplete = bBusRowsValid && !bBusOverflow && AccountedUnderruns == TerminalUnderruns
		&& PreviousUnderruns == TerminalUnderruns;
	const bool bFirstFailureCovered = FirstFailure != INDEX_NONE && bReaderBegin && bReaderDecision && bReaderEnd;
	auto Save = [&State](const TCHAR* Name, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FPaths::Combine(State.EvidenceDirectory, Name),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	const bool bBusSaved = Save(TEXT("bus_reads.csv"), Bus);
	const bool bOrderSaved = Save(TEXT("transport_order.csv"), Order);
	Summary.SetNumberField(TEXT("device_id"), Context->DeviceId);
	Summary.SetNumberField(TEXT("sample_rate"), SampleRate);
	Summary.SetNumberField(TEXT("world_generation"), double(WorldGeneration));
	Summary.SetStringField(TEXT("preparation_id"), FString::Printf(TEXT("%llu"), Context->TestCapturePreparationId));
	Summary.SetStringField(TEXT("capture_allocation_mode"), Context->TestCapturePreparationId == 0 ? TEXT("gt_context_create") : TEXT("gt_prepare_transfer"));
	Summary.SetNumberField(TEXT("bus_rows"), BusCount);
	Summary.SetNumberField(TEXT("bus_capacity"), Context->CapturedBusReads.Num());
	Summary.SetNumberField(TEXT("bus_row_bytes"), sizeof(FIMAcousticBusReadBlock));
	Summary.SetBoolField(TEXT("bus_overflow"), bBusOverflow);
	Summary.SetBoolField(TEXT("bus_rows_complete"), bBusComplete);
	Summary.SetNumberField(TEXT("accounted_underruns"), double(AccountedUnderruns));
	Summary.SetNumberField(TEXT("terminal_underruns"), double(TerminalUnderruns));
	Summary.SetNumberField(TEXT("terminal_push_failures"), double(Context->SourceSendPushFailures.load(std::memory_order_acquire)));
	Summary.SetNumberField(TEXT("first_failure_ordinal"), FirstFailure);
	Summary.SetNumberField(TEXT("transport_claims"), OrderClaims);
	Summary.SetNumberField(TEXT("transport_capacity"), Context->CapturedTransportOrder.Num());
	Summary.SetNumberField(TEXT("transport_row_bytes"), sizeof(FIMAcousticTransportOrderEvent));
	Summary.SetNumberField(TEXT("transport_ready_words"), Context->CapturedTransportOrderReadyWordCount);
	Summary.SetNumberField(TEXT("transport_ready_rows"), OrderReady);
	Summary.SetNumberField(TEXT("transport_prefix_rows"), OrderPrefix);
	Summary.SetNumberField(TEXT("transport_incomplete_rows"), OrderIncomplete);
	Summary.SetNumberField(TEXT("transport_ready_rows_after_hole_not_exported"), OrderReady - OrderPrefix);
	Summary.SetBoolField(TEXT("transport_overflow"), bOrderOverflow);
	Summary.SetBoolField(TEXT("transport_rows_valid"), bOrderRowsValid);
	Summary.SetBoolField(TEXT("first_failure_reader_bracket_complete"), bFirstFailureCovered);
	Summary.SetBoolField(TEXT("published_counts_stable"), bCountsStable);
	const bool bWindowComplete = State.bSteadyIRBaselineCaptured && State.Phases.ContainsByPredicate(
		[](const FIMPhaseEvidence& Phase) { return Phase.Name == TEXT("walk_end"); });
	Summary.SetBoolField(TEXT("bus_window_complete"), bWindowComplete);
	Summary.SetBoolField(TEXT("terminal_underrun_counter_zero"), TerminalUnderruns == 0);
	Summary.SetBoolField(TEXT("bus_continuity_passed"), bWindowComplete && bBusComplete && TerminalUnderruns == 0);
	Summary.SetBoolField(TEXT("bus_csv_saved"), bBusSaved);
	Summary.SetBoolField(TEXT("transport_csv_saved"), bOrderSaved);
	const bool bHostPassed = !State.bPostSourceHostWindow || (State.PostSourceHostSummary && State.PostSourceHostSummary->GetBoolField(TEXT("passed")));
	const bool bRequiredBranch = State.bPostSourceHostWindow ? (bWindowComplete && TerminalUnderruns == 0 && bHostPassed) : bFirstFailureCovered;
	const bool bComplete = bBusComplete && bOrderRowsValid && bRequiredBranch && !OrderIncomplete
		&& bCountsStable && bBusSaved && bOrderSaved;
	Summary.SetBoolField(TEXT("first_branch_evidence_complete"), bComplete);
	UE_LOG(LogTemp, Display, TEXT("IMDEBUG-R95BusBranch bus_rows=%u first_failure=%d underruns=%llu order_prefix=%u claims=%u overflow=%d complete=%d"),
		BusCount, FirstFailure, TerminalUnderruns, OrderPrefix, OrderClaims, bOrderOverflow ? 1 : 0, bComplete ? 1 : 0);
	return bComplete;
}

bool ExportIRCauseEvidence(FIMRunState& State)
{
	if (!State.bCauseWindow) return true;
	const auto Context = State.CauseContext;
	if (!Context || !Context->Device || !Context->Pool
		|| !Context->Stopped.load(std::memory_order_acquire))
		return false;
	const auto Bridge = Context->Device;
	const uint32 Count = Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
	if (Count > uint32(Context->CapturedBlocks.Num()) || State.CauseEndTrace > Count
		|| State.CauseEndTrace <= State.CauseBeginTrace)
		return false;
	auto Save = [&State](const TCHAR* Name, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FPaths::Combine(State.EvidenceDirectory, Name),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	FString Environment = TEXT("index,frame,seconds,captured_seconds,input_version,expected_version,current_version,sequence,device_generation,pool_generation,has_current,delay_priming,enabled,routes,fresh,render_attempted,rendered,listener_x_m,listener_y_m,listener_z_m\n");
	uint32 NoIRInWindow = 0, NotFreshInWindow = 0, RejectedInWindow = 0;
	uint64 NoIROrdinal = 0;
	uint32 CounterWindowNoIR = 0, CounterWindowNotFresh = 0, CounterWindowRejected = 0;
	for (uint32 I = 0; I < Count; ++I)
	{
		const auto& R = Context->CapturedBlocks[int32(I)];
		Environment += FString::Printf(TEXT("%u,%llu,%.17g,%.17g,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%.6f,%.6f,%.6f\n"),
			I, R.Frame, R.Seconds, R.CapturedSeconds, R.InputSpatialVersion,
			R.ExpectedSpatialVersion, R.SpatialVersion, R.Sequence,
			R.DeviceWorldGeneration, R.PoolWorldGeneration,
			uint32(R.HasCurrent), uint32(R.DelayPriming), uint32(R.Enabled), R.RenderRoutes,
			uint32(R.Fresh), uint32(R.RenderAttempted), uint32(R.Rendered),
			R.ListenerX, R.ListenerY, R.ListenerZ);
		// A fresh Context starts this single-owner counter at zero. Every
		// failed valid environment execution writes one row and increments NoIR
		// once. Its ordinal binds the original phase counter window exactly,
		// including a callback whose prefix release straddled a GT phase read.
		if (!R.Rendered)
		{
			++NoIROrdinal;
			if (NoIROrdinal > State.WarmNoIRBlocks && NoIROrdinal <= State.CauseEndNoIR)
			{
				++CounterWindowNoIR;
				if (R.Fresh) ++CounterWindowRejected; else ++CounterWindowNotFresh;
			}
		}
		if (I >= State.CauseBeginTrace && I < State.CauseEndTrace && !R.Rendered)
		{
			++NoIRInWindow;
			if (R.Fresh) ++RejectedInWindow; else ++NotFreshInWindow;
		}
	}
	const uint64 SnapshotClaims = Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
	const uint64 WorkerClaims = Bridge->WorkerProbePushes.load(std::memory_order_acquire);
	const uint32 SnapshotCount = uint32(FMath::Min<uint64>(SnapshotClaims, Bridge->ProbeSnapshotCapacity));
	const uint32 WorkerCount = uint32(FMath::Min<uint64>(WorkerClaims, Bridge->ProbeWorkerCapacity));
	uint32 SnapshotIncomplete = 0, WorkerIncomplete = 0;
	FString Snapshots = TEXT("index,world_generation,spatial_version,captured_seconds,submit_seconds,submitted,fail_code,num_sources,source0_audio_id,source0_x_m,source0_y_m,source0_z_m,listener_x_cm,listener_y_cm,listener_z_cm\n");
	for (uint32 I = 0; I < SnapshotCount; ++I)
	{
		if (Bridge->SnapshotDone[I].load(std::memory_order_acquire) != uint64(I) + 1)
		{ ++SnapshotIncomplete; continue; }
		const auto& R = Bridge->SnapshotProbes[I];
		Snapshots += FString::Printf(TEXT("%u,%llu,%llu,%.17g,%.17g,%u,%u,%u,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
			I, R.WorldGeneration, R.SpatialVersion, R.CapturedSeconds, R.SubmitSeconds,
			uint32(R.Submitted), uint32(R.FailCode), R.NumSources, R.Source0AudioId,
			R.Source0X, R.Source0Y, R.Source0Z, R.ListenerUEX, R.ListenerUEY, R.ListenerUEZ);
	}
	FString Workers = TEXT("index,world_generation,loop_start_seconds,snapshot_captured,snap_valid,snap_reason,num_inputs,results_published,eval_start_seconds,eval_end_seconds,eval_ok,reverb_attempt,reverb_ok,reverb_start_seconds,reverb_end_seconds,reverb_sequence,reverb_captured,reverb_version,reverb_slot,ready_before_seconds,ready_after_seconds,push_seconds,wait_end_seconds,first_audio_id,first_voice_generation,first_sequence,cpu_wall_before_seconds,cpu_wall_after_seconds,cpu100ns_before,cpu100ns_after,cpu_cycles_before,cpu_cycles_after,cpu_thread_id_before,cpu_thread_id_after,cpu_valid_before,cpu_valid_after\n");
	uint32 ProfilingEligibleIR = 0, ProfilingMissingIR = 0, ProfilingClockOrderInvalid = 0;
	uint32 StallEligibleIR = 0, StallInvalidIR = 0;
	for (uint32 I = 0; I < WorkerCount; ++I)
	{
		if (Bridge->WorkerDone[I].load(std::memory_order_acquire) != uint64(I) + 1)
		{ ++WorkerIncomplete; continue; }
		const auto& R = Bridge->WorkerProbes[I];
		if (R.ReverbOk && R.WorldGeneration == Context->Pool->WorldGeneration
			&& R.LoopStartSeconds >= State.CauseProfilingEnableAfterSeconds
			&& R.LoopStartSeconds < State.CauseProfilingRestoreBeforeSeconds)
		{
			++ProfilingEligibleIR;
			if (R.ReverbReadyBeforeSeconds <= 0.0 || R.ReverbReadyAfterSeconds <= 0.0)
				++ProfilingMissingIR;
			else if (R.ReverbEndSeconds > R.ReverbReadyBeforeSeconds
				|| R.ReverbReadyBeforeSeconds > R.ReverbReadyAfterSeconds
				|| R.ReverbReadyAfterSeconds > R.WaitEndSeconds)
				++ProfilingClockOrderInvalid;
		}
		if (State.bStallCauseWindow && R.ReverbAttempt
			&& R.LoopStartSeconds >= State.CauseProfilingEnableAfterSeconds
			&& R.ReverbEndSeconds < State.CauseProfilingRestoreBeforeSeconds)
		{
			++StallEligibleIR;
			const FIMAcousticStallThreadClock Before{R.ReverbCpuWallBeforeSeconds, R.ReverbCpu100nsBefore,
				R.ReverbCpuCyclesBefore, R.ReverbCpuThreadIdBefore, R.ReverbCpuValidBefore};
			const FIMAcousticStallThreadClock After{R.ReverbCpuWallAfterSeconds, R.ReverbCpu100nsAfter,
				R.ReverbCpuCyclesAfter, R.ReverbCpuThreadIdAfter, R.ReverbCpuValidAfter};
			if (!IM_AcousticStallClockPairValid(Before, After)) ++StallInvalidIR;
		}
		Workers += FString::Printf(TEXT("%u,%llu,%.17g,%.17g,%u,%u,%u,%u,%.17g,%.17g,%u,%u,%u,%.17g,%.17g,%llu,%.17g,%llu,%u,%.17g,%.17g,%.17g,%.17g,%llu,%llu,%llu,%.17g,%.17g,%llu,%llu,%llu,%llu,%u,%u,%u,%u\n"),
			I, R.WorldGeneration, R.LoopStartSeconds, R.SnapshotCaptured, uint32(R.SnapValid),
			uint32(R.SnapReason), uint32(R.NumInputs), uint32(R.ResultsPublished),
			R.EvalStartSeconds, R.EvalEndSeconds, uint32(R.EvalOk),
			uint32(R.ReverbAttempt), uint32(R.ReverbOk), R.ReverbStartSeconds, R.ReverbEndSeconds,
			R.ReverbSequence, R.ReverbCaptured, R.ReverbSpatialVersion, R.ReverbSlot,
			R.ReverbReadyBeforeSeconds, R.ReverbReadyAfterSeconds, R.PushSeconds, R.WaitEndSeconds,
			R.FirstAudioId, R.FirstVoiceGen, R.FirstSeq, R.ReverbCpuWallBeforeSeconds, R.ReverbCpuWallAfterSeconds,
			R.ReverbCpu100nsBefore, R.ReverbCpu100nsAfter, R.ReverbCpuCyclesBefore, R.ReverbCpuCyclesAfter,
			R.ReverbCpuThreadIdBefore, R.ReverbCpuThreadIdAfter, uint32(R.ReverbCpuValidBefore), uint32(R.ReverbCpuValidAfter));
	}
	const uint32 SourceClaims = Context->CapturedSourceBlockClaimCount.load(std::memory_order_acquire);
	const uint32 SourceCount = FMath::Min<uint32>(SourceClaims, uint32(Context->CapturedSourceBlocks.Num()));
	uint32 SourceIncomplete = 0;
	FString Sources = TEXT("index,audio_id,world_generation,voice_generation,sequence,input_version,output_version,result_version,captured_seconds,consumed_seconds,age_ms,reject,reset_reason\n");
	for (uint32 I = 0; I < SourceCount; ++I)
	{
		const uint32 Word = I / 64u;
		if (!Context->CapturedSourceBlockReady || Word >= Context->CapturedSourceBlockReadyWordCount
			|| !(Context->CapturedSourceBlockReady[Word].load(std::memory_order_acquire) & (uint64(1) << (I % 64u))))
		{ ++SourceIncomplete; continue; }
		const auto& R = Context->CapturedSourceBlocks[int32(I)];
		Sources += FString::Printf(TEXT("%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%.6f,%u,%u\n"),
			I, R.CallbackAudioComponentId, R.ResultWorldGeneration, R.ResultVoiceGeneration,
			R.ResultSequence, R.InputSpatialVersion, R.OutputSpatialVersion, R.ResultSpatialVersion,
			R.SnapshotCaptured, R.ConsumedSeconds, R.AgeMs, uint32(R.Reject), uint32(R.ResetReason));
	}
	State.CauseSummary = MakeShared<FJsonObject>();
	auto& J = *State.CauseSummary;
	J.SetStringField(TEXT("schema"), TEXT("IMAcousticFootstepIRCause.v1"));
	J.SetNumberField(TEXT("begin_trace_index"), State.CauseBeginTrace);
	J.SetNumberField(TEXT("end_trace_index_exclusive"), State.CauseEndTrace);
	J.SetNumberField(TEXT("environment_prefix_rows"), Count);
	J.SetNumberField(TEXT("environment_capacity_rows"), Context->CapturedBlocks.Num());
	J.SetNumberField(TEXT("environment_row_bytes"), sizeof(FIMAcousticMetaSoundBlock));
	J.SetNumberField(TEXT("source_capacity_rows"), Context->CapturedSourceBlocks.Num());
	J.SetNumberField(TEXT("source_row_bytes"), sizeof(FIMAcousticBlockProbe));
	J.SetNumberField(TEXT("context_generation"), double(Context->Pool->WorldGeneration));
	J.SetNumberField(TEXT("native_no_ir_begin"), double(State.WarmNoIRBlocks));
	J.SetNumberField(TEXT("native_no_ir_end"), double(State.CauseEndNoIR));
	J.SetNumberField(TEXT("no_ir_prefix_total"), double(NoIROrdinal));
	J.SetNumberField(TEXT("window_no_ir_rows"), CounterWindowNoIR);
	J.SetNumberField(TEXT("window_not_fresh_rows"), CounterWindowNotFresh);
	J.SetNumberField(TEXT("window_renderer_rejected_rows"), CounterWindowRejected);
	J.SetNumberField(TEXT("trace_prefix_window_no_ir_rows"), NoIRInWindow);
	J.SetNumberField(TEXT("trace_prefix_window_not_fresh_rows"), NotFreshInWindow);
	J.SetNumberField(TEXT("trace_prefix_window_renderer_rejected_rows"), RejectedInWindow);
	J.SetNumberField(TEXT("snapshot_claims"), double(SnapshotClaims));
	J.SetNumberField(TEXT("worker_claims"), double(WorkerClaims));
	J.SetNumberField(TEXT("source_claims"), SourceClaims);
	J.SetNumberField(TEXT("snapshot_incomplete"), SnapshotIncomplete);
	J.SetNumberField(TEXT("worker_incomplete"), WorkerIncomplete);
	J.SetNumberField(TEXT("source_incomplete"), SourceIncomplete);
	J.SetNumberField(TEXT("snapshot_overflows"), double(Bridge->SnapshotProbeOverflows.load(std::memory_order_acquire)));
	J.SetNumberField(TEXT("worker_overflows"), double(Bridge->WorkerProbeOverflows.load(std::memory_order_acquire)));
	J.SetNumberField(TEXT("terminal_no_ir_counter"), double(Context->NoIRBlocks.load(std::memory_order_acquire)));
	J.SetNumberField(TEXT("terminal_not_fresh_counter"), double(Bridge->ReverbNotFreshBlocks.load(std::memory_order_acquire)));
	J.SetNumberField(TEXT("terminal_rejected_counter"), double(Bridge->ReverbRejectedBlocks.load(std::memory_order_acquire)));
	J.SetBoolField(TEXT("context_stopped"), Context->Stopped.load(std::memory_order_acquire));
	const bool bCounterWindowComplete = State.CauseEndNoIR >= State.WarmNoIRBlocks
		&& NoIROrdinal >= State.CauseEndNoIR
		&& uint64(CounterWindowNoIR) == State.CauseEndNoIR - State.WarmNoIRBlocks;
	J.SetBoolField(TEXT("counter_window_complete"), bCounterWindowComplete);
	J.SetBoolField(TEXT("worker_profiling_previous"), State.bCauseProfilingPrevious);
	J.SetBoolField(TEXT("worker_profiling_enabled_once"), State.bCauseProfilingEverEnabled);
	J.SetBoolField(TEXT("worker_profiling_restored"), State.bCauseProfilingRestored);
	J.SetBoolField(TEXT("worker_profiling_terminal"), Bridge->ProfilingEnabled.load(std::memory_order_acquire));
	J.SetNumberField(TEXT("profiling_enable_before_seconds"), State.CauseProfilingEnableBeforeSeconds);
	J.SetNumberField(TEXT("profiling_enable_confirmed_seconds"), State.CauseProfilingEnableAfterSeconds);
	J.SetNumberField(TEXT("profiling_restore_before_seconds"), State.CauseProfilingRestoreBeforeSeconds);
	J.SetNumberField(TEXT("profiling_restore_confirmed_seconds"), State.CauseProfilingRestoreAfterSeconds);
	J.SetNumberField(TEXT("worker_profiled_successful_ir_rows"), ProfilingEligibleIR);
	J.SetNumberField(TEXT("worker_profiled_missing_ready_clock_rows"), ProfilingMissingIR);
	J.SetNumberField(TEXT("worker_profiled_clock_order_invalid_rows"), ProfilingClockOrderInvalid);
	const bool bProfilingEvidenceComplete = State.bCauseProfilingEverEnabled && State.bCauseProfilingRestored
		&& Bridge->ProfilingEnabled.load(std::memory_order_acquire) == State.bCauseProfilingPrevious
		&& State.CauseProfilingEnableBeforeSeconds <= State.CauseProfilingEnableAfterSeconds
		&& State.CauseProfilingEnableAfterSeconds < State.CauseProfilingRestoreBeforeSeconds
		&& State.CauseProfilingRestoreBeforeSeconds <= State.CauseProfilingRestoreAfterSeconds
		&& ProfilingEligibleIR > 0 && !ProfilingMissingIR && !ProfilingClockOrderInvalid;
	J.SetBoolField(TEXT("worker_publication_clock_complete"), bProfilingEvidenceComplete);
	const bool bStallFramesComplete = ExportStallFrameEvidence(State, J);
	J.SetNumberField(TEXT("worker_stall_cpu_eligible_ir_rows"), StallEligibleIR);
	J.SetNumberField(TEXT("worker_stall_cpu_invalid_ir_rows"), StallInvalidIR);
	const bool bStallEvidenceComplete = !State.bStallCauseWindow || (bStallFramesComplete && StallEligibleIR > 0 && !StallInvalidIR);
	J.SetBoolField(TEXT("stall_phase_evidence_complete"), bStallEvidenceComplete);
	const bool bComplete = bStallEvidenceComplete && bProfilingEvidenceComplete && bCounterWindowComplete && !SnapshotIncomplete && !WorkerIncomplete && !SourceIncomplete
		&& SnapshotClaims <= Bridge->ProbeSnapshotCapacity && WorkerClaims <= Bridge->ProbeWorkerCapacity
		&& SourceClaims <= uint32(Context->CapturedSourceBlocks.Num())
		&& Count < uint32(Context->CapturedBlocks.Num());
	const bool bSaved = Save(TEXT("environment_ir.csv"), Environment) && Save(TEXT("gt_snapshots.csv"), Snapshots)
		&& Save(TEXT("worker_ir.csv"), Workers) && Save(TEXT("source_blocks.csv"), Sources);
	J.SetBoolField(TEXT("complete_prefix"), bComplete);
	J.SetBoolField(TEXT("csv_saved"), bSaved);
	UE_LOG(LogTemp, Display, TEXT("IMDEBUG-R89IRCause window=[%u,%u) no_ir_rows=%u notfresh=%u renderer_rejected=%u prefix_rows=%u complete=%d saved=%d"),
		State.CauseBeginTrace, State.CauseEndTrace, NoIRInWindow, NotFreshInWindow, RejectedInWindow, Count,
		bComplete ? 1 : 0, bSaved ? 1 : 0);
	return bComplete && bSaved;
}

bool WriteReceipt(FIMRunState& State)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("IMAcousticFootstepPlaySaved.v1"));
	Root->SetNumberField(TEXT("pie_ordinal"), State.PIEOrdinal);
	Root->SetStringField(TEXT("audio_component_id"), FString::Printf(TEXT("%llu"), State.AudioComponentId));
	Root->SetStringField(TEXT("previous_audio_component_id"), FString::Printf(TEXT("%llu"), State.PreviousAudioComponentId));
	const bool bIRWindowComplete = State.bSteadyIRBaselineCaptured && State.Phases.ContainsByPredicate(
		[](const FIMPhaseEvidence& Phase) { return Phase.Name == TEXT("walk_end"); });
	const bool bIRPassed = bIRWindowComplete && State.IRFailure.IsEmpty() && State.PreviousIRFailure.IsEmpty();
	Root->SetStringField(TEXT("status"), State.bPassed ? (bIRPassed ? TEXT("CAPTURED_LISTEN_REVIEW_PENDING") : TEXT("FUNCTIONAL_CAPTURED_IR_FAILED")) : TEXT("AUTOMATION_FAILED"));
	const bool bDiagnosticOnly = State.bBusBranchWindow || (State.bCauseWindow && !State.bLandingWindow);
	Root->SetBoolField(TEXT("functional_controls_passed"), State.bPassed && !bDiagnosticOnly);
	Root->SetBoolField(TEXT("diagnostic_window_only"), bDiagnosticOnly);
	Root->SetStringField(TEXT("capture_mode"), State.bPostSourceHostWindow ? TEXT("ordinary_game_post_source_host") : (State.bBusBranchWindow ? TEXT("ordinary_game_bus_branch_window") : (State.bLandingWindow ? TEXT("ordinary_game_landing") : (bDiagnosticOnly ? TEXT("ir_cause_window") : TEXT("ordinary_game_footstep")))));
	Root->SetBoolField(TEXT("complete_two_pie_functional_suite_passed"), !bDiagnosticOnly && State.bPassed && State.PIEOrdinal == 2);
	Root->SetBoolField(TEXT("cause_window_capture_passed"), State.bCauseWindow && State.bPassed);
	if (bDiagnosticOnly)
		Root->SetStringField(TEXT("status"), State.bPassed ? (bIRPassed ? TEXT("IR_CAUSE_WINDOW_CAPTURED") : TEXT("IR_CAUSE_WINDOW_CAPTURED_QUALITY_FAILED")) : TEXT("AUTOMATION_FAILED"));
	if (State.CauseSummary) Root->SetObjectField(TEXT("ir_cause_capture"), State.CauseSummary);
	if (State.BusBranchSummary) Root->SetObjectField(TEXT("bus_branch_capture"), State.BusBranchSummary);
	if (State.PostSourceHostSummary) Root->SetObjectField(TEXT("post_source_host"), State.PostSourceHostSummary);
	if (State.bBusBranchWindow && !State.bPostSourceHostWindow && State.bPassed)
		Root->SetStringField(TEXT("status"), TEXT("BUS_BRANCH_WINDOW_CAPTURED_CONTINUITY_FAILED"));
	if (State.bPostSourceHostWindow && State.bPassed)
		Root->SetStringField(TEXT("status"), bIRPassed ? TEXT("POST_SOURCE_HOST_WALK_CAPTURED_LISTEN_REVIEW_PENDING") : TEXT("POST_SOURCE_HOST_WALK_CAPTURED_IR_FAILED"));
	Root->SetBoolField(TEXT("post_source_two_world_walk_and_cleanup_passed"), State.bPostSourceHostWindow
		&& State.bPassed && State.PIEOrdinal == 2 && State.PostSourceHostSummary && State.PostSourceHostSummary->GetBoolField(TEXT("passed")));
	Root->SetBoolField(TEXT("steady_ir_passed"), bIRPassed);
	Root->SetBoolField(TEXT("steady_ir_window_complete"), bIRWindowComplete);
	Root->SetStringField(TEXT("steady_ir_failure"), State.IRFailure);
	Root->SetStringField(TEXT("previous_pie_steady_ir_failure"), State.PreviousIRFailure);
	Root->SetStringField(TEXT("map_package"), MapPackage);
	Root->SetStringField(TEXT("loaded_editor_map"), State.LoadedEditorMap);
	Root->SetStringField(TEXT("loaded_pie_map"), State.LoadedPIEMap);
	Root->SetNumberField(TEXT("pie_world_count"), State.PIEWorldCount);
	Root->SetStringField(TEXT("master_wav"), State.MasterFile);
	Root->SetNumberField(TEXT("master_wav_bytes"), static_cast<double>(IFileManager::Get().FileSize(*State.MasterFile)));
	Root->SetBoolField(TEXT("recording_start_requested"), State.bRecordingStarted);
	Root->SetBoolField(TEXT("recording_stop_requested"), State.bRecordingStopped);
	Root->SetBoolField(TEXT("master_wav_stable_before_end_pie"), State.bAudioFileReady);
	Root->SetStringField(TEXT("audio_quality_verdict"), TEXT("PENDING_ROOT_LISTENING_REVIEW"));
	Root->SetBoolField(TEXT("pie_ended"), State.bPIEEnded);
	Root->SetStringField(TEXT("failure"), State.Failure);
	Root->SetBoolField(TEXT("screenshot_writer_installed"), State.bScreenshotWriterEverInstalled);
	Root->SetBoolField(TEXT("screenshot_writer_removed"), State.bScreenshotWriterRemoved);
	Root->SetBoolField(TEXT("owned_native_viewport_delegate_removed"), State.bScreenshotDelegateRestored);
	Root->SetBoolField(TEXT("screenshot_cvar_mutated"), false);
	Root->SetStringField(TEXT("screenshot_capture_owner"), TEXT("PIEWorld.GameViewport.OnDrawn_then_OnEndFrame_full_resolution_readback"));
	Root->SetBoolField(TEXT("owned_screenshot_writes_drained"), State.bScreenshotWritesDrained);
	Root->SetNumberField(TEXT("png_quality"), ScreenshotPNGQuality);

	TArray<TSharedPtr<FJsonValue>> PhaseValues;
	for (const FIMPhaseEvidence& Phase : State.Phases)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("phase"), Phase.Name);
		AddObservedFields(Object, Phase.Observed);
		PhaseValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(TEXT("phases"), PhaseValues);

	TArray<TSharedPtr<FJsonValue>> ScreenshotValues;
	for (const FIMScreenshotEvidence& Screenshot : State.Screenshots)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Screenshot.Name);
		Object->SetStringField(TEXT("path"), Screenshot.Path);
		Object->SetBoolField(TEXT("request_issued"), Screenshot.bRequested);
		if (State.bStallCauseWindow)
		{
			Object->SetNumberField(TEXT("request_seconds"), Screenshot.RequestSeconds);
			Object->SetNumberField(TEXT("processed_seconds"), Screenshot.ProcessedSeconds);
			Object->SetNumberField(TEXT("request_frame"), double(Screenshot.RequestFrame));
			Object->SetNumberField(TEXT("processed_frame"), double(Screenshot.ProcessedFrame));
		}
		Object->SetBoolField(TEXT("file_ready"), Screenshot.bReady);
		Object->SetBoolField(TEXT("immutable_pixels_captured"), Screenshot.bPixelsCaptured);
		Object->SetNumberField(TEXT("width"), Screenshot.Width);
		Object->SetNumberField(TEXT("height"), Screenshot.Height);
		Object->SetNumberField(TEXT("capture_thread_id"), Screenshot.CaptureThreadId);
		Object->SetNumberField(TEXT("capture_frame"), double(Screenshot.CaptureFrame));
		Object->SetStringField(TEXT("native_viewport_client"), Screenshot.CaptureViewportClient);
		Object->SetNumberField(TEXT("readback_begin_seconds"), Screenshot.ReadbackBeginSeconds);
		Object->SetNumberField(TEXT("readback_end_seconds"), Screenshot.ReadbackEndSeconds);
			AddScreenshotReadbackFields(Object, Screenshot);
		Object->SetNumberField(TEXT("worker_thread_id"), Screenshot.WriteResult.WorkerThreadId);
		Object->SetBoolField(TEXT("encoded_off_game_thread"), Screenshot.WriteResult.bWorkerThread);
		Object->SetNumberField(TEXT("pixel_copy_begin_seconds"), Screenshot.PixelCopyBeginSeconds);
		Object->SetNumberField(TEXT("pixel_copy_end_seconds"), Screenshot.PixelCopyEndSeconds);
		Object->SetNumberField(TEXT("encode_begin_seconds"), Screenshot.WriteResult.EncodeBeginSeconds);
		Object->SetNumberField(TEXT("encode_end_seconds"), Screenshot.WriteResult.EncodeEndSeconds);
		Object->SetNumberField(TEXT("write_end_seconds"), Screenshot.WriteResult.WriteEndSeconds);
		Object->SetNumberField(TEXT("file_bytes"), static_cast<double>(Screenshot.FileBytes));
		Object->SetNumberField(TEXT("file_observed_frame"), static_cast<double>(Screenshot.FileObservedFrame));
		AddObservedFields(Object, Screenshot.Observed);
		ScreenshotValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(TEXT("screenshots"), ScreenshotValues);
	Root->SetNumberField(TEXT("walk_path_cm"), State.WalkPathCm);
	Root->SetBoolField(TEXT("native_shift_input_sent"), State.bRunInputSent);
	Root->SetBoolField(TEXT("native_J_action_mapping_handled"), State.bManualJumpActionMappingHandled);
	Root->SetStringField(TEXT("native_J_verification"), TEXT("bound key press/release callbacks and actual OnJumped velocity; InputKey action-mapping return is not the oracle"));
	Root->SetNumberField(TEXT("resume_max_speed_cm_s"), State.ResumeMaxSpeedCmS);

	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	const bool bSerialized = FJsonSerializer::Serialize(Root, Writer);
	Writer->Close();
	return bSerialized && FFileHelper::SaveStringToFile(
		Json, *State.ReceiptFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool SaveNaturalStockIdentity(FIMRunState& State, UWorld* World, const TCHAR* Phase)
{
	TArray<AIMAcousticFootstepCharacter*> Actors;
	for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor) Actors.Add(*Actor);
	if (Actors.Num() != 1) return false;
	auto* Source = Cast<UMetaSoundSource>(Actors[0]->FootstepSound);
	if (!Source || Source->GetPathName() != TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstep.MS_IMFootstep")
		|| Source->HasAnyFlags(RF_Transient) || Source->GetOutermost()->IsDirty()) return false;
	FString Document;
	if (!FJsonObjectConverter::UStructToJsonObjectString(Source->GetConstDocument(), Document)
		|| !FFileHelper::SaveStringToFile(Document, *FPaths::Combine(State.EvidenceDirectory,
			FString::Printf(TEXT("stock_graph_%s.json"), Phase)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	auto Identity = MakeShared<FJsonObject>();
	Identity->SetStringField(TEXT("phase"), Phase);
	Identity->SetStringField(TEXT("world"), World->GetPathName());
	Identity->SetStringField(TEXT("actor"), Actors[0]->GetPathName());
	Identity->SetStringField(TEXT("source_path"), Source->GetPathName());
	Identity->SetBoolField(TEXT("source_transient"), Source->HasAnyFlags(RF_Transient));
	Identity->SetBoolField(TEXT("package_dirty"), Source->GetOutermost()->IsDirty());
	Identity->SetBoolField(TEXT("master_recording_start_requested"), State.bRecordingStarted);
	Identity->SetBoolField(TEXT("master_recording_stop_requested"), State.bRecordingStopped);
	Identity->SetNumberField(TEXT("native_platform_seconds"), FPlatformTime::Seconds());
	Identity->SetNumberField(TEXT("native_gt_frame"), double(GFrameCounter));
	auto* RendererSetting = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DFShadowAsyncCompute"));
	Identity->SetNumberField(TEXT("DFShadowAsyncCompute"), RendererSetting ? RendererSetting->GetInt() : -1);
	Identity->SetNumberField(TEXT("DFShadowAsyncCompute_flags"), RendererSetting ? RendererSetting->GetFlags() : 0);
	TArray<TSharedPtr<FJsonValue>> Waves;
	if (FCString::Strcmp(Phase, TEXT("editor")) == 0)
	{
		TArray<USoundWave*> Originals;
		for (auto Wave : Actors[0]->WoodSteps) Originals.AddUnique(Wave);
		for (auto Wave : Actors[0]->StoneSteps) Originals.AddUnique(Wave);
		for (auto* Wave : Originals)
		{
			TArray<uint8> PCM, Wav;
			uint32 Rate = 0;
			uint16 Channels = 0;
			if (!Wave || !Wave->GetImportedSoundWaveData(PCM, Rate, Channels) || !Rate || !Channels || PCM.IsEmpty()) return false;
			SerializeWaveFile(Wav, PCM.GetData(), PCM.Num(), Channels, Rate);
			const FString File = FPaths::Combine(State.EvidenceDirectory, Wave->GetName() + TEXT(".wav"));
			if (!FFileHelper::SaveArrayToFile(Wav, *File)) return false;
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset"), Wave->GetPathName());
			Row->SetStringField(TEXT("imported_pcm_wav"), File);
			Row->SetNumberField(TEXT("sample_rate"), Rate);
			Row->SetNumberField(TEXT("channels"), Channels);
			Row->SetNumberField(TEXT("frames"), PCM.Num() / (2 * Channels));
			Waves.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Identity->SetArrayField(TEXT("original_imported_waves"), Waves);
	FString Text;
	return FJsonSerializer::Serialize(Identity, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *FPaths::Combine(State.EvidenceDirectory,
			FString::Printf(TEXT("stock_identity_%s.json"), Phase)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

struct FIMNaturalViewportOwner
{
	TSharedPtr<FIMRunState> State;
		FDelegateHandle Initialized, Cleanup, PostActorTick, ViewportCreated;
	FIMObservedState Previous;
	TArray<TSharedPtr<FJsonValue>> Events;
	bool Bound = false, Cleaned = false;
	bool WoodStepFrameRequested = false, StoneStepFrameRequested = false;
	bool SavedStockSource = false, StockStopReadbackWritten = false;
	double MasterRecordingStartSeconds = 0.0, MasterRecordingStopSeconds = 0.0;
};

TUniquePtr<FIMNaturalViewportOwner> NaturalViewportOwner;

void StopNaturalMasterRecording(FIMNaturalViewportOwner& Owner, UWorld* World)
{
	auto& State = *Owner.State;
	if (!Owner.SavedStockSource || !State.bRecordingStarted || State.bRecordingStopped) return;
	UAudioMixerBlueprintLibrary::StopRecordingOutput(World, EAudioRecordingExportType::WavFile,
		TEXT("IMNaturalMaster"), FPaths::ConvertRelativePathToFull(FPaths::GetPath(State.EvidenceDirectory)), nullptr, nullptr);
	State.bRecordingStopped = true;
	Owner.MasterRecordingStopSeconds = FPlatformTime::Seconds();
}

TSharedRef<FJsonObject> ObserveNaturalPhysicalMaterial(FIMNaturalViewportOwner& Owner, UWorld* World, const FIMObservedState& Observed)
{
	check(IsInGameThread());
	auto Result = MakeShared<FJsonObject>();
	const auto* Character = Owner.State->Character.Get();
	const FVector Start = Observed.AudioLocation + FVector(0, 0, 35);
	const FVector End = Observed.AudioLocation - FVector(0, 0, 55);
	Result->SetStringField(TEXT("semantics"), TEXT("fresh_GT_collision_query_from_observed_audio_world_cm_not_product_counter_or_TraceSole"));
	Result->SetStringField(TEXT("world"), GetPathNameSafe(World));
	Result->SetNumberField(TEXT("frame"), double(GFrameCounter));
	Result->SetStringField(TEXT("trace_start_cm"), Start.ToString());
	Result->SetStringField(TEXT("trace_end_cm"), End.ToString());
	Result->SetNumberField(TEXT("previous_tick_vertical_speed_cm_s"), Owner.Previous.VerticalSpeedCmS);
	Result->SetBoolField(TEXT("previous_tick_airborne"), Owner.Previous.bAirborne);
	Result->SetBoolField(TEXT("query_issued"), World && Character);
	if (!World || !Character) return Result;
	FCollisionQueryParams Query(FName(TEXT("IMAcousticNaturalPhysicalMaterial")), false, Character);
	Query.bReturnPhysicalMaterial = true;
	FHitResult Hit;
	const bool Blocking = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Query) && Hit.IsValidBlockingHit();
	Result->SetBoolField(TEXT("blocking_hit"), Blocking);
	Result->SetBoolField(TEXT("physical_material_valid"), Hit.PhysMaterial.IsValid());
	Result->SetStringField(TEXT("physical_material"), GetPathNameSafe(Hit.PhysMaterial.Get()));
	Result->SetStringField(TEXT("hit_actor"), GetPathNameSafe(Hit.GetActor()));
	Result->SetStringField(TEXT("hit_component"), GetPathNameSafe(Hit.GetComponent()));
	Result->SetStringField(TEXT("impact_point_cm"), Hit.ImpactPoint.ToString());
	Result->SetStringField(TEXT("impact_normal"), Hit.ImpactNormal.ToString());
	return Result;
}

void RecordNaturalViewportEvent(FIMNaturalViewportOwner& Owner, UWorld* World, const FString& Name, bool Capture)
{
	auto& State = *Owner.State;
	const auto Observed = Observe(State, World);
	if (State.EvidenceDirectory.Contains(TEXT("AcousticNaturalC26_20261011T082612Z")) || Owner.SavedStockSource)
	{
		const FString TraceName = FString::Printf(TEXT("IMAcousticClockGT frame=%llu native=%.9f"), GFrameCounter, FPlatformTime::Seconds());
		TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*TraceName);
	}
	auto Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("event"), Name);
	AddObservedFields(Event, Observed);
	if (State.EvidenceDirectory.Contains(TEXT("AcousticCrossC32_20261016")) && Name.StartsWith(TEXT("landing_")))
		Event->SetObjectField(TEXT("independent_collision_material"), ObserveNaturalPhysicalMaterial(Owner, World, Observed));
	const bool Request = Capture && !State.bScreenshotStopRequested && State.PendingScreenshotIndex == INDEX_NONE;
	Event->SetBoolField(TEXT("viewport_request_issued"), Request);
	Owner.Events.Add(MakeShared<FJsonValueObject>(Event));
	if (!Request) return;
	auto& Shot = State.Screenshots.AddDefaulted_GetRef();
	Shot.Name = Name;
	Shot.Path = FPaths::Combine(State.EvidenceDirectory, Name + TEXT(".png"));
	Shot.Observed = Observed;
	Shot.bRequested = true;
	Shot.RequestSeconds = FPlatformTime::Seconds();
	Shot.RequestFrame = GFrameCounter;
	State.PendingScreenshotIndex = State.Screenshots.Num() - 1;
	State.ScreenshotDeadlineSeconds = Shot.RequestSeconds + ScreenshotTimeoutSeconds;
}

void ObserveNaturalViewport(UWorld* World, ELevelTick, float)
{
	check(IsInGameThread());
	if (!NaturalViewportOwner || !NaturalViewportOwner->Bound || NaturalViewportOwner->Cleaned
		|| NaturalViewportOwner->State->PIEWorld.Get() != World) return;
	auto& Owner = *NaturalViewportOwner;
	auto& State = *Owner.State;
	if (!State.bScreenshotWriterActive && !State.bScreenshotStopRequested && !State.InstallScreenshotWriter(Owner.State)) return;
	State.PollScreenshotWrites(FPlatformTime::Seconds());
	if (State.Screenshots.IsValidIndex(State.PendingScreenshotIndex)
		&& State.Screenshots[State.PendingScreenshotIndex].bPixelsCaptured) State.PendingScreenshotIndex = INDEX_NONE;
	const auto Current = Observe(State, World);
	if (!Owner.Previous.bAirborne && Current.bAirborne)
		RecordNaturalViewportEvent(Owner, World, FString::Printf(TEXT("jump_%02d_rise"), Current.Landings + 1), true);
	if (Current.bAirborne && Owner.Previous.VerticalSpeedCmS > 0.0 && Current.VerticalSpeedCmS <= 0.0)
		RecordNaturalViewportEvent(Owner, World, FString::Printf(TEXT("jump_%02d_apex"), Current.Landings + 1), true);
	if (Current.Landings != Owner.Previous.Landings)
		RecordNaturalViewportEvent(Owner, World, FString::Printf(TEXT("landing_%02d_contact"), Current.Landings), true);
	if (Current.Contacts != Owner.Previous.Contacts)
	{
		const bool Wood = Current.WoodContacts != Owner.Previous.WoodContacts;
		const bool Stone = Current.StoneContacts != Owner.Previous.StoneContacts;
		const bool Capture = (Wood && !Owner.WoodStepFrameRequested) || (Stone && !Owner.StoneStepFrameRequested);
		RecordNaturalViewportEvent(Owner, World, FString::Printf(TEXT("step_%02d_contact"), Current.Contacts), Capture);
		Owner.WoodStepFrameRequested |= Capture && Wood;
		Owner.StoneStepFrameRequested |= Capture && Stone;
	}
	Owner.Previous = Current;
}

void ArmNaturalViewport(const TArray<FString>& Args, UWorld* World)
{
	check(IsInGameThread());
	if (Args.Num() != 1 || (Args[0] != TEXT("AcousticNaturalC24_20261011T053352Z")
		&& Args[0] != TEXT("AcousticNaturalC25_20261011T063628Z")
		&& Args[0] != TEXT("AcousticNaturalC25_20261011T063628Z_R2")
		&& Args[0] != TEXT("AcousticNaturalC25_20261011T063628Z_R3")
		&& Args[0] != TEXT("AcousticNaturalC26_20261011T082612Z")
		&& Args[0] != TEXT("AcousticNaturalC26_20261011T082612Z_R2")
		&& Args[0] != TEXT("AcousticNaturalC27_20261011T101233Z")
		&& Args[0] != TEXT("AcousticNaturalC27_20261011T101233Z_R2")
		&& Args[0] != TEXT("AcousticNaturalC27_20261011T101233Z_R3")
		&& Args[0] != TEXT("AcousticListenerC29_20261011T135339Z")
		&& Args[0] != TEXT("AcousticListenerC30_20261011T155840Z")
		&& Args[0] != TEXT("AcousticOrientationC31_20261012T023600Z")
		&& Args[0] != TEXT("AcousticVisibleC32Native_20261016")
		&& Args[0] != TEXT("AcousticCrossC32_20261016"))
		|| NaturalViewportOwner || !World || World->WorldType != EWorldType::Editor) return;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexRuntime"), Args[0], TEXT("Viewport"));
	if (IFileManager::Get().DirectoryExists(*Directory) || !IFileManager::Get().MakeDirectory(*Directory, true)) return;
	auto Owner = MakeUnique<FIMNaturalViewportOwner>();
	Owner->State = MakeShared<FIMRunState>(nullptr);
	Owner->State->EvidenceDirectory = Directory;
	Owner->SavedStockSource = Args[0] == TEXT("AcousticNaturalC27_20261011T101233Z")
		|| Args[0] == TEXT("AcousticNaturalC27_20261011T101233Z_R2")
		|| Args[0] == TEXT("AcousticNaturalC27_20261011T101233Z_R3")
		|| Args[0] == TEXT("AcousticListenerC29_20261011T135339Z")
		|| Args[0] == TEXT("AcousticListenerC30_20261011T155840Z")
		|| Args[0] == TEXT("AcousticOrientationC31_20261012T023600Z")
		|| Args[0] == TEXT("AcousticVisibleC32Native_20261016")
		|| Args[0] == TEXT("AcousticCrossC32_20261016");
	if (Owner->SavedStockSource && !SaveNaturalStockIdentity(*Owner->State, World, TEXT("editor"))) return;
	if (Args[0] == TEXT("AcousticNaturalC25_20261011T063628Z_R2") || Args[0] == TEXT("AcousticNaturalC25_20261011T063628Z_R3")
		|| Args[0] == TEXT("AcousticNaturalC26_20261011T082612Z") || Args[0] == TEXT("AcousticNaturalC26_20261011T082612Z_R2")
		|| Owner->SavedStockSource)
		Owner->State->RequiredNativeViewportSize = FIntPoint(2235, 1413);
	NaturalViewportOwner = MoveTemp(Owner);
	NaturalViewportOwner->ViewportCreated = UGameViewportClient::OnViewportCreated().AddLambda([]()
	{
		if (!NaturalViewportOwner || NaturalViewportOwner->Bound || !GEngine || !GEngine->GameViewport) return;
		UGameViewportClient* Client = GEngine->GameViewport;
		auto* World = Client->GetWorld();
		if (!World || World->WorldType != EWorldType::PIE || World->HasBegunPlay()
			|| !World->GetPackage()->GetName().Contains(TEXT("IM_FootstepAudition"))) return;
		NaturalViewportOwner->State->ConfigureNativeViewportResolution(Client);
	});
	NaturalViewportOwner->Initialized = FWorldDelegates::OnWorldInitializedActors.AddLambda([](const UWorld::FActorsInitializedParams& Params)
	{
		if (!NaturalViewportOwner || NaturalViewportOwner->Bound || !Params.World || Params.World->WorldType != EWorldType::PIE) return;
		TArray<AIMAcousticFootstepCharacter*> Actors;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(Params.World); Actor; ++Actor) Actors.Add(*Actor);
		if (Actors.Num() != 1 || Actors[0]->HasActorBegunPlay()) return;
		auto& Owner = *NaturalViewportOwner;
		Owner.State->PIEWorld = Params.World;
		Owner.State->Character = Actors[0];
		Owner.State->Audio = Actors[0]->GetFootstepAudio();
		if (Owner.SavedStockSource)
		{
			UAudioMixerBlueprintLibrary::StartRecordingOutput(Params.World, 40.0f, nullptr);
			Owner.State->bRecordingStarted = true;
			Owner.MasterRecordingStartSeconds = FPlatformTime::Seconds();
		}
		if (Owner.SavedStockSource && !SaveNaturalStockIdentity(*Owner.State, Params.World, TEXT("pie")))
			Owner.State->Failure = TEXT("current_saved_stock_source_readback_failed");
		Owner.Bound = Owner.State->Audio.IsValid();
	});
	NaturalViewportOwner->PostActorTick = FWorldDelegates::OnWorldPostActorTick.AddStatic(&ObserveNaturalViewport);
	NaturalViewportOwner->Cleanup = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* CleanWorld, bool, bool)
	{
		if (!NaturalViewportOwner || NaturalViewportOwner->State->PIEWorld.Get() != CleanWorld) return;
		NaturalViewportOwner->Cleaned = true;
		StopNaturalMasterRecording(*NaturalViewportOwner, CleanWorld);
		NaturalViewportOwner->State->ShutdownScreenshotWriter();
	});
	UE_LOG(LogTemp, Display, TEXT("IMLogs C25_NATIVE_VIEWPORT armed default_off_nonblocking_observation_only"));
}

void PrepareNaturalViewportStop(const TArray<FString>& Args, UWorld* World)
{
	check(IsInGameThread());
	if (!Args.IsEmpty() || !NaturalViewportOwner || NaturalViewportOwner->Cleaned
		|| NaturalViewportOwner->State->PIEWorld.Get() != World) return;
	auto& State = *NaturalViewportOwner->State;
	StopNaturalMasterRecording(*NaturalViewportOwner, World);
	if (NaturalViewportOwner->SavedStockSource && !NaturalViewportOwner->StockStopReadbackWritten)
	{
		NaturalViewportOwner->StockStopReadbackWritten = SaveNaturalStockIdentity(State, World, TEXT("stop"));
		if (!NaturalViewportOwner->StockStopReadbackWritten) State.Failure = TEXT("stock_source_stop_readback_failed");
	}
	State.ShutdownScreenshotWriter();
	if (!State.bScreenshotWritesDrained || !State.bScreenshotGPUReadbacksDrained) return;
	auto Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("ready_for_ordinary_stop"), true);
	Status->SetBoolField(TEXT("original_native_resolution_mode_restored"), State.bNativeViewportResolutionRestored);
	FString Text;
	FJsonSerializer::Serialize(Status, TJsonWriterFactory<>::Create(&Text));
	FFileHelper::SaveStringToFile(Text, *FPaths::Combine(State.EvidenceDirectory, TEXT("native_viewport_stop.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FinishNaturalViewport(const TArray<FString>& Args)
{
	check(IsInGameThread());
	if (!Args.IsEmpty() || !NaturalViewportOwner || !NaturalViewportOwner->Cleaned) return;
	auto& Owner = *NaturalViewportOwner;
	auto& State = *Owner.State;
	State.ShutdownScreenshotWriter();
	if (!State.bScreenshotWritesDrained || !State.bScreenshotGPUReadbacksDrained) return;
	auto Report = MakeShared<FJsonObject>();
	Report->SetArrayField(TEXT("native_events"), Owner.Events);
	TArray<TSharedPtr<FJsonValue>> Shots;
	for (const auto& Shot : State.Screenshots)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Shot.Name);
		Row->SetStringField(TEXT("path"), Shot.Path);
		AddObservedFields(Row, Shot.Observed);
		Row->SetNumberField(TEXT("request_seconds"), Shot.RequestSeconds);
		Row->SetNumberField(TEXT("request_frame"), double(Shot.RequestFrame));
		Row->SetNumberField(TEXT("capture_frame"), double(Shot.CaptureFrame));
		Row->SetStringField(TEXT("native_viewport_client"), Shot.CaptureViewportClient);
		Row->SetNumberField(TEXT("readback_begin_seconds"), Shot.ReadbackBeginSeconds);
		Row->SetNumberField(TEXT("readback_end_seconds"), Shot.ReadbackEndSeconds);
		AddScreenshotReadbackFields(Row, Shot);
		Row->SetNumberField(TEXT("width"), Shot.Width);
		Row->SetNumberField(TEXT("height"), Shot.Height);
		Row->SetNumberField(TEXT("capture_thread_id"), Shot.CaptureThreadId);
		Row->SetNumberField(TEXT("worker_thread_id"), Shot.WriteResult.WorkerThreadId);
		Row->SetBoolField(TEXT("encoded_off_game_thread"), Shot.WriteResult.bWorkerThread);
		Row->SetBoolField(TEXT("file_ready"), Shot.bReady);
		Row->SetNumberField(TEXT("file_bytes"), double(Shot.FileBytes));
		Shots.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("screenshots"), Shots);
	Report->SetStringField(TEXT("failure"), State.Failure);
	Report->SetBoolField(TEXT("world_cleanup_observed"), Owner.Cleaned);
	Report->SetBoolField(TEXT("writes_drained"), State.bScreenshotWritesDrained);
	Report->SetBoolField(TEXT("gpu_readbacks_drained"), State.bScreenshotGPUReadbacksDrained);
	Report->SetBoolField(TEXT("readback_rt_delegate_removed"), State.bScreenshotGPUReadbacksDrained);
	Report->SetNumberField(TEXT("readback_queue_capacity"), ScreenshotReadbackCapacity);
	Report->SetNumberField(TEXT("peak_outstanding_readback_and_png"), State.ScreenshotPeakOutstanding);
	Report->SetNumberField(TEXT("gpu_copies_submitted"), State.ScreenshotGPUCopiesSubmitted);
	Report->SetNumberField(TEXT("gpu_copies_mapped"), State.ScreenshotGPUCopiesMapped);
	Report->SetNumberField(TEXT("png_quality"), ScreenshotPNGQuality);
	Report->SetBoolField(TEXT("baseline_native_resolution_fixed"), State.bNativeViewportResolutionApplied);
	Report->SetBoolField(TEXT("original_native_resolution_mode_restored"), State.bNativeViewportResolutionRestored);
	Report->SetBoolField(TEXT("original_native_resolution_was_fixed"), State.bPreviousNativeViewportFixed);
	Report->SetNumberField(TEXT("original_native_viewport_width"), State.PreviousNativeViewportSize.X);
	Report->SetNumberField(TEXT("original_native_viewport_height"), State.PreviousNativeViewportSize.Y);
	Report->SetBoolField(TEXT("native_resolution_prepared_before_BeginPlay"), State.bNativeResolutionPreparedBeforeBeginPlay);
	Report->SetNumberField(TEXT("native_resolution_prepare_begin_seconds"), State.NativeResolutionPrepareBeginSeconds);
	Report->SetNumberField(TEXT("native_resolution_prepare_end_seconds"), State.NativeResolutionPrepareEndSeconds);
	Report->SetBoolField(TEXT("native_viewport_delegates_removed"), State.bScreenshotWriterRemoved);
	Report->SetBoolField(TEXT("product_accepted"), false);
	Report->SetBoolField(TEXT("saved_stock_source_not_cloned_or_tapped"), Owner.SavedStockSource);
	Report->SetBoolField(TEXT("stock_stop_graph_readback_written"), Owner.StockStopReadbackWritten);
	Report->SetBoolField(TEXT("native_master_recording_start_requested"), State.bRecordingStarted);
	Report->SetBoolField(TEXT("native_master_recording_stop_requested"), State.bRecordingStopped);
	Report->SetNumberField(TEXT("native_master_recording_start_seconds"), Owner.MasterRecordingStartSeconds);
	Report->SetNumberField(TEXT("native_master_recording_stop_seconds"), Owner.MasterRecordingStopSeconds);
	Report->SetStringField(TEXT("native_master_file"), FPaths::Combine(FPaths::GetPath(State.EvidenceDirectory), TEXT("IMNaturalMaster.wav")));
	FWorldDelegates::OnWorldInitializedActors.Remove(Owner.Initialized);
	FWorldDelegates::OnWorldPostActorTick.Remove(Owner.PostActorTick);
	FWorldDelegates::OnWorldCleanup.Remove(Owner.Cleanup);
	UGameViewportClient::OnViewportCreated().Remove(Owner.ViewportCreated);
	Report->SetBoolField(TEXT("world_delegates_removed"), true);
	FString Text;
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text));
	FFileHelper::SaveStringToFile(Text, *FPaths::Combine(State.EvidenceDirectory, TEXT("native_viewport_report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	NaturalViewportOwner.Reset();
	UE_LOG(LogTemp, Display, TEXT("IMLogs C25_NATIVE_VIEWPORT released"));
}

FAutoConsoleCommandWithWorldAndArgs NaturalViewportArmCommand(TEXT("IM.Acoustic.NaturalViewport.Arm"),
	TEXT("Default-OFF current natural-event viewport witness; reuses the owned off-GT PNG writer."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ArmNaturalViewport));
FAutoConsoleCommandWithWorldAndArgs NaturalViewportPrepareStopCommand(TEXT("IM.Acoustic.NaturalViewport.PrepareStop"),
	TEXT("Drain the owned native viewport and restore its mode before ordinary Stop."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PrepareNaturalViewportStop));
FAutoConsoleCommand NaturalViewportFinishCommand(TEXT("IM.Acoustic.NaturalViewport.Finish"),
	TEXT("Drain natural viewport evidence after ordinary Stop."), FConsoleCommandWithArgsDelegate::CreateStatic(&FinishNaturalViewport));

void EmitRunnerTerminalMarkers(const bool bPassed, const bool bEditorExitReady)
{
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] Footstep.PlaySaved %s"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	if (bEditorExitReady)
	{
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor Footstep.PlaySaved"));
	}
}

bool AbortBeforePIE(FAutomationTestBase& Test, const TCHAR* Reason)
{
	Test.AddError(Reason);
	EmitRunnerTerminalMarkers(false, true);
	return false;
}

class FIMPlaySavedCommand final : public IAutomationLatentCommand
{
public:
	FIMPlaySavedCommand(FAutomationTestBase* InTest, TSharedRef<FIMRunState> InState, EIMStage InStage)
		: Test(InTest), State(InState), Stage(InStage)
	{
	}

	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		State->PollScreenshotWrites(Now);
		if (State->PendingScreenshotIndex != INDEX_NONE) PollScreenshot(Now);
		if (!State->Failure.IsEmpty() && Stage != EIMStage::WaitForAudioFile && Stage != EIMStage::WaitForPIEExit)
			return FailAndFinish(State->Failure);
		if (Now - State->StartedWallSeconds > TotalWallTimeoutSeconds && Stage != EIMStage::WaitForPIEExit)
		{
			return FailAndFinish(TEXT("The complete saved-map PIE observation exceeded its 120 second wall limit."));
		}

		switch (Stage)
		{
		case EIMStage::WaitForPIE:
			return WaitForPIE(Now);
		case EIMStage::Walk:
			return RunWalk(Now);
		case EIMStage::Stop:
			return RunStop(Now);
		case EIMStage::Resume:
			return RunResume(Now);
		case EIMStage::WaitForAudioFile:
			return WaitForAudioFile(Now);
		case EIMStage::WaitForPIEExit:
			return WaitForPIEExit(Now);
		default:
			return FailAndFinish(TEXT("Unknown footstep test state."));
		}
	}

private:
	static constexpr double WalkScreenshotOffsets[] = {0.0, 2.0, 4.0, 6.0, 8.0, 10.0};

	UWorld* CurrentPIEWorld()
	{
		UWorld* World = nullptr;
		State->PIEWorldCount = CountPIEWorlds(World);
		if (State->PIEWorldCount == 1)
		{
			State->PIEWorld = World;
			return World;
		}
		return nullptr;
	}

	bool WaitForPIE(const double Now)
	{
		UWorld* World = nullptr;
		State->PIEWorldCount = CountPIEWorlds(World);
		if (State->PIEWorldCount > 1)
		{
			return FailAndFinish(TEXT("Expected exactly one ordinary PIE world."));
		}
		if (!World)
		{
			return Now - State->PIEStartedWallSeconds > PIEStartupTimeoutSeconds
				? FailAndFinish(TEXT("The ordinary PIE world did not become available within 30 seconds."))
				: false;
		}
		if (!IsTargetPIEMap(World))
		{
			return FailAndFinish(TEXT("PIE loaded a world other than the saved IM_FootstepAudition map."));
		}
		State->PIEWorld = World;
		State->LoadedPIEMap = World->GetOutermost()->GetName();

		AIMAcousticFootstepCharacter* FoundCharacter = nullptr;
		int32 CharacterCount = 0;
		for (TActorIterator<AIMAcousticFootstepCharacter> It(World); It; ++It)
		{
			++CharacterCount;
			FoundCharacter = *It;
		}
		if (CharacterCount > 1)
		{
			return FailAndFinish(TEXT("Saved PIE map contains more than one footstep character."));
		}
		if (!FoundCharacter)
		{
			return Now - State->PIEStartedWallSeconds > PIEStartupTimeoutSeconds
				? FailAndFinish(TEXT("The saved PIE map did not provide its footstep character within 30 seconds."))
				: false;
		}

		UAudioComponent* Audio = FoundCharacter->GetFootstepAudio();
		APlayerController* Controller = World->GetFirstPlayerController();
		FTransform ListenerTransform;
		FAudioDevice* Device = World->GetAudioDeviceRaw();
		if (!Audio || !Controller || Controller->GetPawn() != FoundCharacter || !Device
			|| !Device->GetListenerTransform(0, ListenerTransform) || !Audio->IsPlaying())
		{
			return Now - State->PIEStartedWallSeconds > PIEStartupTimeoutSeconds
				? FailAndFinish(TEXT("The saved character, listener, or persistent AudioComponent did not become ready in PIE."))
				: false;
		}
		if (!FoundCharacter->bAutoWalk)
		{
			return FailAndFinish(TEXT("The saved character did not start in its ordinary automatic-walk state."));
		}
		if (State->bLandingWindow && !FoundCharacter->WasObserverFrameApplied())
			return FailAndFinish(TEXT("The ordinary landing observer did not apply its visible-character patrol and strong-jump framing."));

		if (!State->InstallScreenshotWriter(TWeakPtr<FIMRunState>(State)))
			return FailAndFinish(TEXT("The saved-map PIE world could not bind its own rendered native GameViewport for asynchronous screenshots."));
		if (State->bCauseWindow && !State->CauseContext)
		{
			State->CauseContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID);
			IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
			if (!State->CauseContext || State->CauseContext->CapturedBlocks.Num() != 4096
				|| State->CauseContext->CapturedSourceBlocks.Num() != (State->bLandingWindow ? 4096 : 2048))
				return FailAndFinish(TEXT("IR cause window did not receive its fresh preallocated capture context."));
			if (State->bBusBranchWindow && (State->CauseContext->CapturedBusReads.Num() != 4096
				|| State->CauseContext->CapturedTransportOrder.Num() != int32(FIMAcousticMetaSoundContext::TransportOrderEventCapacity)
				|| !State->CauseContext->CapturedTransportOrderReady
				|| State->CauseContext->CapturedTransportOrderReadyWordCount != (FIMAcousticMetaSoundContext::TransportOrderEventCapacity + 63u) / 64u))
				return FailAndFinish(TEXT("Bus branch window did not receive its fresh GT-prepared fixed bus/order storage."));
			if (!State->EnableCauseWorkerProfiling())
				return FailAndFinish(TEXT("IR cause window could not enable existing worker profiling on its retained Context."));
			if (!State->InstallStallFrameProbes(TWeakPtr<FIMRunState>(State)))
				return FailAndFinish(TEXT("IMDEBUG-AC0848 could not install GT-only stall phase probes."));
		}
		State->AudioComponentId = Audio->GetAudioComponentID();
		if (State->bPostSourceHostWindow && !State->EnvironmentHost)
		{
			UIMAcousticReverbPreset* Preset = nullptr;
			int32 HostOwners = 0;
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
			{
				if (auto* Candidate = It->IM_GetEnvironmentPresetForTest())
				{ Preset = Candidate; ++HostOwners; }
			}
			if (HostOwners != 1 || !Preset || !Preset->GetEnvironmentHostForTest())
				return FailAndFinish(TEXT("Ordinary Play did not expose exactly one world-owned environment graph host."));
			State->EnvironmentHost = Preset->GetEnvironmentHostForTest();
			if (State->PIEOrdinal == 2)
			{
				const auto Previous = State->PreviousEnvironmentContext;
				if (!Previous || !State->PreviousEnvironmentHost || Previous.Get() == State->CauseContext.Get()
					|| State->PreviousEnvironmentHost.Get() == State->EnvironmentHost.Get()
					|| !Previous->Stopped.load(std::memory_order_acquire)
					|| Previous->EnvironmentConsumer.load(std::memory_order_acquire)
					|| Previous->BusReaderConsumer.load(std::memory_order_acquire)
					|| State->CauseContext->Pool->WorldGeneration == State->PreviousEnvironmentWorld)
					return FailAndFinish(TEXT("Second PIE reused a live old environment context/host/world owner."));
				for (const auto& Consumer : Previous->SourceConsumers)
					if (Consumer.load(std::memory_order_acquire))
						return FailAndFinish(TEXT("Old world source consumer remains owned before the second world's ordinary walk."));
			}
		}
		if (State->PIEOrdinal == 2 && (!State->PreviousAudioComponentId
			|| State->AudioComponentId == State->PreviousAudioComponentId))
			return FailAndFinish(TEXT("PIE re-entry reused the old footstep audio component identity."));
		State->Character = FoundCharacter;
		State->Audio = Audio;
		State->WalkStartedWorldSeconds = World->GetTimeSeconds();
		State->WalkStartContacts = FoundCharacter->GetContactCount();
		State->WalkStartWood = FoundCharacter->GetWoodContactCount();
		State->WalkStartStone = FoundCharacter->GetStoneContactCount();
		State->WalkStartLocation = FoundCharacter->GetActorLocation();
		State->PreviousWalkLocation = State->WalkStartLocation;
		State->NextWalkScreenshot = 1;
		RecordPhase(*State, World, TEXT("walk_begin"));

		UAudioMixerBlueprintLibrary::StartRecordingOutput(World, 40.0f, nullptr);
		State->bRecordingStarted = true;
		State->WalkStartedWorldSeconds = World->GetTimeSeconds();
		State->WalkStartContacts = FoundCharacter->GetContactCount();
		State->WalkStartWood = FoundCharacter->GetWoodContactCount();
		State->WalkStartStone = FoundCharacter->GetStoneContactCount();
		State->WalkStartLocation = FoundCharacter->GetActorLocation();
		State->PreviousWalkLocation = State->WalkStartLocation;
		Stage = EIMStage::Walk;
		RequestScreenshot(World, TEXT("walk_00"));
		return false;
	}

	bool CaptureLandingFrame(UWorld* World)
	{
		if (!State->bLandingWindow) return false;
		auto* Character = State->Character.Get();
		const bool bAirborne = Character->GetCharacterMovement()->IsFalling();
		const double VerticalSpeed = Character->GetVelocity().Z;
		FString Event;
		if (State->bWasAirborne && bAirborne && Character->GetContactCount() != State->ObservedFootsteps)
			State->Failure = TEXT("Ordinary footsteps triggered during consecutive airborne frames.");
		if (!State->bWasAirborne && bAirborne && VerticalSpeed > 100.0)
		{
			++State->ObservedJumpRises;
			Event = FString::Printf(TEXT("jump_%02d_rise"), State->ObservedJumpRises);
		}
		else if (bAirborne && State->PreviousVerticalSpeed > 0.0 && VerticalSpeed <= 0.0)
			Event = FString::Printf(TEXT("jump_%02d_apex"), State->ObservedJumpRises);
		if (Character->GetLandingCount() != State->ObservedLandings)
		{
			State->ObservedLandings = Character->GetLandingCount();
			State->MinLandingGain = FMath::Min(State->MinLandingGain, Character->GetLastLandingGain());
			State->MaxLandingGain = FMath::Max(State->MaxLandingGain, Character->GetLastLandingGain());
			Event = FString::Printf(TEXT("landing_%02d_contact"), State->ObservedLandings);
		}
		State->bWasAirborne = bAirborne;
		State->PreviousVerticalSpeed = VerticalSpeed;
		State->ObservedFootsteps = Character->GetContactCount();
		if (Event.IsEmpty()) return false;
		RecordPhase(*State, World, *Event);
		if (State->PendingScreenshotIndex != INDEX_NONE) return false;
		RequestScreenshot(World, *Event);
		return true;
	}

	bool RunWalk(const double Now)
	{
		UWorld* World = CurrentPIEWorld();
		AIMAcousticFootstepCharacter* Character = State->Character.Get();
		UAudioComponent* Audio = State->Audio.Get();
		if (!World || !IsTargetPIEMap(World) || !Character || !Audio)
		{
			return FailAndFinish(TEXT("Saved PIE world, character, or AudioComponent was lost during the walk window."));
		}
		if (State->bCauseWindow)
		{
			FAudioDevice* Device = World->GetAudioDeviceRaw();
			const auto Registered = Device ? IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID) : nullptr;
			if (!State->CauseContext || Registered.Get() != State->CauseContext.Get())
				return FailAndFinish(TEXT("IR cause window Context identity changed during ordinary walking."));
			if (!State->CauseContext->Device->ProfilingEnabled.load(std::memory_order_acquire))
				return FailAndFinish(TEXT("IR cause window lost its owned worker profiling switch during walking."));
			if (State->bStallCauseWindow && !State->CauseContext->Device->StallTimingEnabled.load(std::memory_order_acquire))
				return FailAndFinish(TEXT("IMDEBUG-AC0848 lost its owned stall timing switch during walking."));
		}
		const FVector CurrentLocation = Character->GetActorLocation();
		State->WalkPathCm += FVector::Distance(State->PreviousWalkLocation, CurrentLocation);
		State->PreviousWalkLocation = CurrentLocation;
		const double Elapsed = World->GetTimeSeconds() - State->WalkStartedWorldSeconds;
		if (Elapsed >= 2.0 && !State->bSteadyIRBaselineCaptured)
		{
			RecordPhase(*State, World, TEXT("steady_ir_begin"));
			const FIMObservedState& Warm = State->Phases.Last().Observed;
			State->WarmEnvironmentBlocks = Warm.EnvironmentBlocks;
			State->WarmNoIRBlocks = Warm.NoIRBlocks;
			State->CauseBeginTrace = Warm.EnvironmentTraceBlocks;
			State->bSteadyIRBaselineCaptured = true;
		}

		if (CaptureLandingFrame(World)) return false;
		if (!State->Failure.IsEmpty()) return FailAndFinish(State->Failure);
		if (!PollScreenshot(Now))
		{
			return !State->Failure.IsEmpty() ? FailAndFinish(State->Failure) : false;
		}
		if (State->NextWalkScreenshot < UE_ARRAY_COUNT(WalkScreenshotOffsets)
			&& Elapsed >= WalkScreenshotOffsets[State->NextWalkScreenshot])
		{
			if (Elapsed - WalkScreenshotOffsets[State->NextWalkScreenshot] > 1.0)
			{
				return FailAndFinish(TEXT("A normal-walk screenshot missed its scheduled time-series window."));
			}
			const FString Name = FString::Printf(TEXT("walk_%02d"), State->NextWalkScreenshot * 2);
			++State->NextWalkScreenshot;
			RequestScreenshot(World, *Name);
			return false;
		}
		if (Elapsed < WalkWindowSeconds)
		{
			if (!Audio->IsPlaying())
			{
				return FailAndFinish(TEXT("The persistent footstep AudioComponent stopped during ordinary walking."));
			}
			return false;
		}
		if (State->PendingScreenshotIndex != INDEX_NONE
			|| State->NextWalkScreenshot != UE_ARRAY_COUNT(WalkScreenshotOffsets))
		{
			return false;
		}

		RecordPhase(*State, World, TEXT("walk_end"));
		const FIMObservedState& End = State->Phases.Last().Observed;
		State->CauseEndTrace = End.EnvironmentTraceBlocks;
		State->CauseEndNoIR = End.NoIRBlocks;
		if (!State->bSteadyIRBaselineCaptured || End.EnvironmentBlocks <= State->WarmEnvironmentBlocks
			|| End.NoIRBlocks != State->WarmNoIRBlocks)
		{
			State->IRFailure = FString::Printf(TEXT("Steady environment IR failed after warm-up: env_before=%llu env_after=%llu no_ir_before=%llu no_ir_after=%llu."),
				State->WarmEnvironmentBlocks, End.EnvironmentBlocks, State->WarmNoIRBlocks, End.NoIRBlocks);
			// Preserve the strict quality failure, while finishing independent pause/run/lifecycle evidence.
			// The final automation verdict still fails; continuing capture never converts it into PASS.
			UE_LOG(LogTemp, Warning, TEXT("IMLogs FootstepDemo steady_ir_failure=%s"), *State->IRFailure);
		}
		if (End.VisibleEditorMarkerCount != 0)
		{
			return FailAndFinish(TEXT("Editor-only source marker meshes are visible in ordinary Play."));
		}
		if (Character->GetWoodContactCount() <= State->WalkStartWood
			|| Character->GetStoneContactCount() <= State->WalkStartStone
			|| Character->GetUnknownContactCount() != 0
			|| Character->GetContactCount() <= State->WalkStartContacts
			|| State->WalkPathCm < MinimumWalkPathCm)
		{
			return FailAndFinish(TEXT("The 12 second walk did not prove both mapped surfaces, zero unknown contacts, and actual character travel."));
		}

		if (State->bLandingWindow && (Character->GetLandingCount() < 3
			|| Character->GetWoodLandingCount() == 0 || Character->GetStoneLandingCount() == 0
			|| State->ObservedJumpRises < 3 || State->MinLandingGain > 1.1f || State->MaxLandingGain < 1.25f))
			return FailAndFinish(TEXT("Normal Play did not prove visible light/strong landings on both actual physical materials."));
		if (State->bBusBranchWindow || (State->bCauseWindow && !State->bLandingWindow))
		{
			// Stop the same ordinary walk after its unchanged 2s+10s window.
			// Preserve strict IR failure, recording and all six walk screenshots.
			Character->SetAutoWalking(false);
			return BeginAudioFinalization(World);
		}
		Character->SetAutoWalking(false);
		State->StopStartedWorldSeconds = World->GetTimeSeconds();
		State->StopStartContacts = Character->GetContactCount();
		State->StopStartLandings = Character->GetLandingCount();
		RecordPhase(*State, World, TEXT("stop_begin"));
		Stage = EIMStage::Stop;
		RequestScreenshot(World, TEXT("stopped_00"));
		return false;
	}

	bool RunStop(const double Now)
	{
		UWorld* World = CurrentPIEWorld();
		AIMAcousticFootstepCharacter* Character = State->Character.Get();
		if (!World || !Character || !State->Audio.IsValid())
		{
			return FailAndFinish(TEXT("Saved PIE objects were lost during the stationary negative-control window."));
		}
		const double Elapsed = World->GetTimeSeconds() - State->StopStartedWorldSeconds;
		if (!PollScreenshot(Now))
		{
			return !State->Failure.IsEmpty() ? FailAndFinish(State->Failure) : false;
		}
		if (Elapsed >= 1.0 && State->Screenshots.Num() > 0
			&& State->Screenshots.Last().Name == TEXT("stopped_00"))
		{
			RequestScreenshot(World, TEXT("stopped_01"));
			return false;
		}
		if (Elapsed < StopWindowSeconds)
		{
			return false;
		}
		if (State->PendingScreenshotIndex != INDEX_NONE)
		{
			return false;
		}
		RecordPhase(*State, World, TEXT("stop_end"));
		if (Character->GetContactCount() != State->StopStartContacts
			|| (State->bLandingWindow && Character->GetLandingCount() != State->StopStartLandings))
		{
			return FailAndFinish(TEXT("Footstep contacts increased during the 2 second SetAutoWalking(false) negative control."));
		}

		Character->SetAutoWalking(true);
		if (State->PIEOrdinal == 2)
		{
			APlayerController* Controller = World->GetFirstPlayerController();
			if (!Controller || !Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::LeftShift, IE_Pressed, 1.0f)))
			{
				return FailAndFinish(TEXT("Native controller did not accept the running key press."));
			}
			State->bRunInputSent = true;
			if (State->bLandingWindow)
			{
				State->ManualJumpPressBaseline = Character->GetManualJumpPressCount();
				State->ManualJumpReleaseBaseline = Character->GetManualJumpReleaseCount();
				State->ManualJumpStartBaseline = Character->GetManualJumpStartCount();
				State->bManualJumpActionMappingHandled = Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::J, IE_Pressed, 1.0f));
				UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo native_input key=J event=Pressed action_mapping_handled=%d"), State->bManualJumpActionMappingHandled);
			}
			UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo native_input key=LeftShift event=Pressed"));
		}
		State->ResumeStartedWorldSeconds = World->GetTimeSeconds();
		State->ResumeStartWood = Character->GetWoodContactCount();
		State->ResumeStartStone = Character->GetStoneContactCount();
		RecordPhase(*State, World, TEXT("resume_begin"));
		Stage = EIMStage::Resume;
		RequestScreenshot(World, TEXT("resume_00"));
		return false;
	}

	bool RunResume(const double Now)
	{
		UWorld* World = CurrentPIEWorld();
		AIMAcousticFootstepCharacter* Character = State->Character.Get();
		UAudioComponent* Audio = State->Audio.Get();
		if (!World || !Character || !Audio)
		{
			return FailAndFinish(TEXT("Saved PIE objects were lost after automatic walking resumed."));
		}
		const double Elapsed = World->GetTimeSeconds() - State->ResumeStartedWorldSeconds;
		if (State->bLandingWindow && State->bRunInputSent && !State->bJumpInputReleased && Elapsed >= .1)
		{
			World->GetFirstPlayerController()->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::J, IE_Released, 0.0f));
			State->bJumpInputReleased = true;
			UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo native_input key=J event=Released"));
		}
		State->ResumeMaxSpeedCmS = FMath::Max(State->ResumeMaxSpeedCmS, double(Character->GetVelocity().Size2D()));
		if (CaptureLandingFrame(World)) return false;
		if (!State->Failure.IsEmpty()) return FailAndFinish(State->Failure);
		if (!PollScreenshot(Now))
		{
			return !State->Failure.IsEmpty() ? FailAndFinish(State->Failure) : false;
		}
		if (Elapsed >= 2.0 && State->Screenshots.Num() > 0
			&& State->Screenshots.Last().Name == TEXT("resume_00"))
		{
			RequestScreenshot(World, TEXT("resume_02"));
			return false;
		}
		if (Character->GetUnknownContactCount() != 0 || !Audio->IsPlaying())
		{
			return FailAndFinish(TEXT("Unknown material contacts appeared or the persistent AudioComponent stopped after resume."));
		}
		const bool bBothMaterialsResumed = Character->GetWoodContactCount() > State->ResumeStartWood
			&& Character->GetStoneContactCount() > State->ResumeStartStone;
		const double MinimumResumeSeconds = State->PIEOrdinal == 2 ? 4.0 : 2.0;
		if (Elapsed >= MinimumResumeSeconds && bBothMaterialsResumed && State->PendingScreenshotIndex == INDEX_NONE)
		{
			RecordPhase(*State, World, TEXT("resume_end"));
			if (State->PIEOrdinal == 2)
			{
				if (State->ResumeMaxSpeedCmS < 170.0)
					return FailAndFinish(TEXT("Shift did not produce actual character acceleration from walk to run."));
				if (State->bLandingWindow && (Character->GetManualJumpPressCount() != State->ManualJumpPressBaseline + 1
					|| Character->GetManualJumpReleaseCount() != State->ManualJumpReleaseBaseline + 1
					|| Character->GetManualJumpStartCount() != State->ManualJumpStartBaseline + 1
					|| !Character->WasLastManualJumpStrong() || Character->GetLastManualJumpStartSpeedCmS() < 400.f))
					return FailAndFinish(TEXT("Native Shift+J did not reach its bound press/release callbacks and start the actual strong character jump."));
				World->GetFirstPlayerController()->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::LeftShift, IE_Released, 0.0f));
				UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo native_input key=LeftShift event=Released max_speed_cm_s=%.3f"), State->ResumeMaxSpeedCmS);
			}
			State->bPassed = true;
			return BeginAudioFinalization(World);
		}
		if (Elapsed > ResumeTimeoutSeconds)
		{
			return FailAndFinish(TEXT("Both physical-material footstep counters did not resume within 15 seconds."));
		}
		return false;
	}

	void RequestScreenshot(UWorld* World, const TCHAR* Name)
	{
		if (State->PendingScreenshotIndex != INDEX_NONE || FScreenshotRequest::IsScreenshotRequested())
		{
			State->Failure = TEXT("Refusing to replace an already pending viewport screenshot request.");
			return;
		}
		FIMScreenshotEvidence& Screenshot = State->Screenshots.AddDefaulted_GetRef();
		Screenshot.Name = Name;
		Screenshot.Path = FPaths::Combine(State->EvidenceDirectory, Screenshot.Name + TEXT(".png"));
		Screenshot.Observed = Observe(*State, World);
		IFileManager::Get().Delete(*Screenshot.Path, false, true, true);
		Screenshot.bRequested = true;
		State->PendingScreenshotIndex = State->Screenshots.Num() - 1;
		State->ScreenshotDeadlineSeconds = FPlatformTime::Seconds() + ScreenshotTimeoutSeconds;
		{ Screenshot.RequestSeconds = FPlatformTime::Seconds(); Screenshot.RequestFrame = GFrameCounter; }
		UE_LOG(LogTemp, Display,
			TEXT("IMLogs FootstepDemo PlaySaved screenshot_request=%s frame=%llu world_s=%.3f path=%s"),
			*Screenshot.Name,
			static_cast<unsigned long long>(Screenshot.Observed.Frame),
			Screenshot.Observed.WorldSeconds,
			*Screenshot.Path);
	}

	bool PollScreenshot(const double Now)
	{
		if (State->PendingScreenshotIndex == INDEX_NONE)
		{
			return true;
		}
		FIMScreenshotEvidence& Screenshot = State->Screenshots[State->PendingScreenshotIndex];
		if (Screenshot.bPixelsCaptured)
		{
			UE_LOG(LogTemp, Display,
				TEXT("IMLogs FootstepDemo PlaySaved screenshot_pixels_ready=%s frame=%llu size=%dx%d path=%s"),
				*Screenshot.Name,
				static_cast<unsigned long long>(GFrameCounter), Screenshot.Width, Screenshot.Height,
				*Screenshot.Path);
			State->PendingScreenshotIndex = INDEX_NONE;
			return true;
		}
		if (Now > State->ScreenshotDeadlineSeconds)
		{
			State->Failure = TEXT("A requested real PIE viewport screenshot was not written before its deadline.");
		}
		return false;
	}

	bool BeginAudioFinalization(UWorld* World)
	{
		if (State->PendingScreenshotIndex != INDEX_NONE)
		{
			return false;
		}
		if (State->bRecordingStarted && !State->bRecordingStopped && World)
		{
			UAudioMixerBlueprintLibrary::StopRecordingOutput(
				World,
				EAudioRecordingExportType::WavFile,
				MasterStem,
				State->EvidenceDirectory,
				nullptr,
				nullptr);
			State->bRecordingStopped = true;
			State->AudioFileDeadlineSeconds = FPlatformTime::Seconds() + AudioFileTimeoutSeconds;
			State->PreviousAudioFileBytes = -1;
			State->AudioFileStableSinceSeconds = 0.0;
			RecordPhase(*State, World, TEXT("master_recording_stop_requested"));
			UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo PlaySaved master_stop_requested path=%s"), *State->MasterFile);
			Stage = EIMStage::WaitForAudioFile;
			return false;
		}
		QueueEndPIE();
		return true;
	}

	bool WaitForAudioFile(const double Now)
	{
		UWorld* World = CurrentPIEWorld();
		if (State->PendingScreenshotIndex != INDEX_NONE && !PollScreenshot(Now))
		{
			return false;
		}
		const int64 Size = IFileManager::Get().FileSize(*State->MasterFile);
		if (Size > 44)
		{
			if (Size != State->PreviousAudioFileBytes)
			{
				State->PreviousAudioFileBytes = Size;
				State->AudioFileStableSinceSeconds = Now;
				return false;
			}
			if (Now - State->AudioFileStableSinceSeconds >= AudioFileStableSeconds)
			{
				State->bAudioFileReady = true;
				TArray<uint8> Wav;
				FWaveModInfo Info;
				bool bNonzeroPCM = false;
				if (FFileHelper::LoadFileToArray(Wav, *State->MasterFile)
					&& Info.ReadWaveInfo(Wav.GetData(), Wav.Num()))
					for (uint32 Byte = 0; Byte < Info.SampleDataSize; ++Byte)
						if (Info.SampleDataStart[Byte] != 0) { bNonzeroPCM = true; break; }
				if (!bNonzeroPCM && State->Failure.IsEmpty())
					State->Failure = TEXT("Actual master WAV contains no nonzero PCM; contact counters cannot prove audible output.");
				if (World)
				{
					RecordPhase(*State, World, TEXT("master_recording_file_ready"));
				}
				UE_LOG(LogTemp, Display,
					TEXT("IMLogs FootstepDemo PlaySaved master_file_ready bytes=%lld path=%s; audio quality remains for root listening review"),
					static_cast<long long>(Size), *State->MasterFile);
				QueueEndPIE();
				return true;
			}
		}
		if (Now > State->AudioFileDeadlineSeconds)
		{
			if (State->Failure.IsEmpty())
			{
				State->Failure = TEXT("The asynchronous master WAV export did not become stable before PIE cleanup.");
			}
			QueueEndPIE();
			return true;
		}
		return false;
	}

	bool FailAndFinish(const FString& Reason)
	{
		if (State->Failure.IsEmpty())
		{
			State->Failure = Reason;
		}
		State->bPassed = false;
		// A failed screenshot must not keep the recorder/PIE cleanup waiting on its own failed request.
		if (State->PendingScreenshotIndex != INDEX_NONE)
		{
			State->PendingScreenshotIndex = INDEX_NONE;
		}
		UWorld* World = CurrentPIEWorld();
		if (State->bRecordingStarted && !State->bRecordingStopped && World)
		{
			return BeginAudioFinalization(World);
		}
		QueueEndPIE();
		return true;
	}

	void QueueEndPIE()
	{
		State->ShutdownScreenshotWriter();
		State->RestoreCauseWorkerProfiling();
		if (State->bPIEEndQueued)
		{
			return;
		}
		State->bPIEEndQueued = true;
		if (State->bCauseWindow) IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		Stage = EIMStage::WaitForPIEExit;
		ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
		ADD_LATENT_AUTOMATION_COMMAND(FIMPlaySavedCommand(Test, State, EIMStage::WaitForPIEExit));
	}

	bool WaitForPIEExit(const double Now)
	{
		UWorld* World = nullptr;
		State->PIEWorldCount = CountPIEWorlds(World);
		if (State->PIEWorldCount == 0)
		{
			State->bPIEEnded = true;
			if (const auto* OldAudio = State->Audio.Get(); OldAudio && OldAudio->IsPlaying())
				State->Failure = TEXT("Owned footstep audio remained playing after its PIE world ended.");
			int32 ReadyScreenshots = 0;
			for (const auto& Entry : State->Screenshots) ReadyScreenshots += Entry.bReady ? 1 : 0;
			if (!ExportIRCauseEvidence(*State) && State->Failure.IsEmpty())
				State->Failure = TEXT("IR cause export has missing, overflowing or invalid prefix evidence.");
			if (State->bPostSourceHostWindow)
			{
				State->PostSourceHostSummary = IM_ExportAcousticEnvironmentHostForTest(State->EnvironmentHost, State->EvidenceDirectory);
				State->EnvironmentHostInstance = uint64(State->PostSourceHostSummary->GetNumberField(TEXT("generator_instance_id")));
				if (State->PIEOrdinal == 2 && State->EnvironmentHostInstance == State->PreviousHostInstance && State->Failure.IsEmpty())
					State->Failure = TEXT("Second world reused the previous graph's actual initialization InstanceID.");
				if (!State->PostSourceHostSummary->GetBoolField(TEXT("passed")) && State->Failure.IsEmpty())
					State->Failure = TEXT("Post-source native block ordering/output or detach/quiescence/graph ownership cleanup failed; see native_host_blocks.csv and post_source_host receipt.");
			}
			if (!ExportBusBranchEvidence(*State) && State->Failure.IsEmpty())
				State->Failure = TEXT("Bus branch export has missing actual failure, counter mismatch, incomplete prefix or uncovered first failure.");
			State->bPassed = State->Failure.IsEmpty()
				&& State->bAudioFileReady
				&& State->bScreenshotWriterRemoved && State->bScreenshotDelegateRestored && State->bScreenshotWritesDrained
				&& ReadyScreenshots >= 6
				&& State->Phases.Num() >= (State->bCauseWindow ? 5 : 6);
			const bool bReceiptWritten = WriteReceipt(*State);
			if (!bReceiptWritten && State->Failure.IsEmpty())
			{
				State->Failure = TEXT("The runtime evidence receipt could not be written after PIE exit.");
			}
			const bool bTerminalPass = State->bPassed && bReceiptWritten && State->bPIEEnded;
			const bool bFullPass = bTerminalPass && State->IRFailure.IsEmpty() && State->PreviousIRFailure.IsEmpty();
			Test->TestTrue(TEXT("saved footstep PlaySaved oracle, direct evidence, and receipt completed"), bTerminalPass);
			Test->TestTrue(TEXT("machine-readable runtime receipt was written"), bReceiptWritten);
			if (!State->Failure.IsEmpty())
			{
				Test->AddError(State->Failure);
			}
			UE_LOG(LogTemp, Display,
				TEXT("IMLogs FootstepDemo PlaySaved status=%s screenshots=%d wav_ready=%d pie_ended=1 receipt=%s"),
				bTerminalPass ? (State->bBusBranchWindow && !State->bPostSourceHostWindow ? TEXT("BUS_BRANCH_WINDOW_CAPTURED_CONTINUITY_FAILED") : (State->bCauseWindow && !State->bLandingWindow
					? (bFullPass ? TEXT("IR_CAUSE_WINDOW_CAPTURED") : TEXT("IR_CAUSE_WINDOW_CAPTURED_QUALITY_FAILED"))
					: (bFullPass ? TEXT("CAPTURED_LISTEN_REVIEW_PENDING") : TEXT("FUNCTIONAL_CAPTURED_IR_FAILED")))) : TEXT("FAIL"),
				ReadyScreenshots,
				State->bAudioFileReady ? 1 : 0,
				*State->ReceiptFile);
			if ((!State->bBusBranchWindow || State->bPostSourceHostWindow) && bTerminalPass && State->PIEOrdinal == 1 && (!State->bCauseWindow || (State->bLandingWindow && State->IRFailure.IsEmpty())))
			{
				const TSharedRef<FIMRunState> Reentry = MakeShared<FIMRunState>(Test, 2);
				Reentry->LoadedEditorMap = State->LoadedEditorMap;
				Reentry->PreviousAudioComponentId = State->AudioComponentId;
				Reentry->PreviousIRFailure = State->IRFailure;
				Reentry->PreviousEnvironmentWorld = State->CauseContext ? State->CauseContext->Pool->WorldGeneration : 0;
				Reentry->PreviousHostInstance = State->EnvironmentHostInstance;
				Reentry->PreviousEnvironmentHost = State->EnvironmentHost;
				Reentry->PreviousEnvironmentContext = State->CauseContext;
				IFileManager::Get().MakeDirectory(*Reentry->EvidenceDirectory, true);
				IFileManager::Get().Delete(*Reentry->MasterFile, false, true, true);
				IFileManager::Get().Delete(*Reentry->ReceiptFile, false, true, true);
				Reentry->bPIEStartRequested = true;
				Reentry->PIEStartedWallSeconds = FPlatformTime::Seconds();
				UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo PlaySaved reentry_requested previous_audio=%llu"), State->AudioComponentId);
				if (Reentry->bCauseWindow)
					IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, Reentry->bLandingWindow ? 4096 : 2048, 4096, false, Reentry->bBusBranchWindow, Reentry->bBusBranchWindow);
				ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
				ADD_LATENT_AUTOMATION_COMMAND(FIMPlaySavedCommand(Test, Reentry, EIMStage::WaitForPIE));
				return true;
			}
			Test->TestTrue(State->bBusBranchWindow || (State->bCauseWindow && !State->bLandingWindow)
				? TEXT("strict steady environment IR quality holds in the single cause window")
				: TEXT("strict steady environment IR quality holds across both PIE generations"), bFullPass);
			if (!State->PreviousIRFailure.IsEmpty()) Test->AddError(State->PreviousIRFailure);
			if (!State->IRFailure.IsEmpty()) Test->AddError(State->IRFailure);
			EmitRunnerTerminalMarkers(bFullPass, State->bPIEEnded);
			return true;
		}
		if (Now - State->StartedWallSeconds > TotalWallTimeoutSeconds + PIEStartupTimeoutSeconds)
		{
			if (State->Failure.IsEmpty())
			{
				State->Failure = TEXT("The owned ordinary PIE session did not exit within its cleanup limit.");
			}
			State->bPassed = false;
			const bool bReceiptWritten = WriteReceipt(*State);
			Test->TestTrue(TEXT("machine-readable runtime receipt was written after PIE exit timeout"), bReceiptWritten);
			Test->AddError(State->Failure);
			EmitRunnerTerminalMarkers(false, false);
			return true;
		}
		return false;
	}

	FAutomationTestBase* Test = nullptr;
	TSharedRef<FIMRunState> State;
	EIMStage Stage;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticFootstepPlaySavedTest,
	"IceMoon.AcousticField.Footstep.PlaySaved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticFootstepPlaySavedTest::RunTest(const FString&)
{
	if (!GEditor || !GUnrealEd || !GEngine)
	{
		return AbortBeforePIE(*this, TEXT("Footstep PlaySaved requires the Unreal Editor automation context."));
	}

	UWorld* ExistingPIEWorld = nullptr;
	const int32 ExistingPIEWorldCount = CountPIEWorlds(ExistingPIEWorld);
	if (ExistingPIEWorldCount != 0)
	{
		return AbortBeforePIE(*this, TEXT("Footstep PlaySaved refuses to take ownership of an already-running PIE session."));
	}

	const TSharedRef<FIMRunState> State = MakeShared<FIMRunState>(this);
	IFileManager::Get().MakeDirectory(*State->EvidenceDirectory, true);
	if (!IFileManager::Get().DirectoryExists(*State->EvidenceDirectory))
	{
		return AbortBeforePIE(*this, TEXT("Cannot create the owned footstep evidence directory."));
	}
	IFileManager::Get().Delete(*State->MasterFile, false, true, true);
	IFileManager::Get().Delete(*State->ReceiptFile, false, true, true);

	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld)
	{
		return AbortBeforePIE(*this, TEXT("The editor world is unavailable before loading the saved footstep map."));
	}
	const FString CurrentMap = EditorWorld->GetOutermost()->GetName();
	if (EditorWorld->GetOutermost()->IsDirty())
	{
		return AbortBeforePIE(*this, TEXT("Refusing to change maps while the current editor map package is dirty."));
	}
	if (CurrentMap != MapPackage && !FEditorFileUtils::LoadMap(MapPackage, false, false))
	{
		return AbortBeforePIE(*this, TEXT("Could not open the required saved IM_FootstepAudition map."));
	}
	EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld || EditorWorld->GetOutermost()->GetName() != MapPackage)
	{
		return AbortBeforePIE(*this, TEXT("The editor did not retain the exact saved IM_FootstepAudition map."));
	}
	State->LoadedEditorMap = EditorWorld->GetOutermost()->GetName();
	// Export the complete graph document from this fresh editor load before ordinary Play.
	int32 SavedCharacterCount = 0;
	for (TActorIterator<AIMAcousticFootstepCharacter> It(EditorWorld); It; ++It)
	{
		++SavedCharacterCount;
		auto* SavedGraph = Cast<UMetaSoundSource>(It->FootstepSound);
		FString Document;
		const FString GraphReadbackPath = State->bCauseWindow
			? FPaths::Combine(State->EvidenceDirectory, TEXT("graph_reopened.json"))
			: FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/GameFootstepR89_20261007/graph_reopened.json"));
		if (!SavedGraph || !FJsonObjectConverter::UStructToJsonObjectString(SavedGraph->GetConstDocument(), Document)
			|| !FFileHelper::SaveStringToFile(Document, *GraphReadbackPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			return AbortBeforePIE(*this, TEXT("Fresh saved footstep graph document readback failed."));
	}
	if (SavedCharacterCount != 1)
		return AbortBeforePIE(*this, TEXT("Saved footstep map character cardinality differs."));

	UE_LOG(LogTemp, Display,
		TEXT("IMLogs FootstepDemo PlaySaved start map=%s evidence=%s"),
		*State->LoadedEditorMap, *State->EvidenceDirectory);
	if (State->bCauseWindow)
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, State->bLandingWindow ? 4096 : 2048, 4096, false, State->bBusBranchWindow, State->bBusBranchWindow);
	State->bPIEStartRequested = true;
	State->PIEStartedWallSeconds = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FIMPlaySavedCommand(this, State, EIMStage::WaitForPIE));
	return true;
}

} // namespace IMAcousticFootstepPlaySaved

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
