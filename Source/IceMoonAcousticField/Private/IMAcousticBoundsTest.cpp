#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticSpatialization.h"
#include "Audio.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWaveProcedural.h"
#include "UnrealEdGlobals.h"

namespace IMAcousticBoundsTestPrivate
{
constexpr const TCHAR* BoundsMap=TEXT("/IceMoonAcousticField/Tests/IM_W1Door");
constexpr const TCHAR* ListenerOutsideStatusText=TEXT("V2 degraded: listener outside bake bounds.");
constexpr int32 BoundsSampleRate=48000;
constexpr double BoundsGlobalTimeout=300.0;
constexpr double BoundsPhaseTimeout=15.0;
constexpr double BoundsBaselineTimeout=90.0;
constexpr double BoundsListenerFreeze=2.0;

bool BoundsJson(const FString& File,const TSharedRef<FJsonObject>& Object)
{
	FString Text;auto Writer=TJsonWriterFactory<>::Create(&Text);
	return FJsonSerializer::Serialize(Object,Writer)&&FFileHelper::SaveStringToFile(Text,*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

UWorld* BoundsPIE()
{
	for(const auto& Context:GEngine->GetWorldContexts())
		if(Context.WorldType==EWorldType::PIE)return Context.World();
	return nullptr;
}

class FIMAcousticBoundsCommand final:public IAutomationLatentCommand
{
public:
	explicit FIMAcousticBoundsCommand(FAutomationTestBase* InTest,const FString& InDirectory,int32 InStage=0,double InStarted=0)
		:Test(InTest),Directory(InDirectory),Started(InStarted>0?InStarted:FPlatformTime::Seconds()),Stage(InStage){}

	bool Update() override
	{
		check(IsInGameThread());
		const double Now=FPlatformTime::Seconds();
		if(Now-Started>BoundsGlobalTimeout)return Finish(false,TEXT("Bounds degrade test exceeded the 300 second global timeout."));
		if(Bridge.IsValid()&&Bridge->SnapshotProbeOverflows.load(std::memory_order_acquire)>0)
			return Finish(false,TEXT("Snapshot probe overflow created an evidence gap."));
		if(Now-LastDiagnostic>=5.0)
		{
			LastDiagnostic=Now;
			FIMAcousticSnapshotProbe Latest{};uint64 LatestIndex=0;uint32 LatestSources=0;
			const bool HasLatest=LatestCompleted(Latest,LatestIndex,LatestSources,0);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsWait stage=%d pushes=%llu latest_done=%d latest_index=%llu num_sources=%u status=%s"),
				Stage,CurrentPushes(),HasLatest?1:0,LatestIndex,LatestSources,Volume.IsValid()?*Volume->Status:TEXT("<none>"));
		}

		if(Stage==0)
		{
			UWorld* EditorWorld=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
			if(!EditorWorld)return Finish(false,TEXT("Editor world unavailable before bounds setup."));
			int32 Found=0;
			for(TActorIterator<AIMAcousticBakeVolume> It(EditorWorld);It;++It)
			{
				++Found;Volume=*It;
			}
			if(Found!=1)return Finish(false,FString::Printf(TEXT("Expected exactly one editor BakeVolume, found %d."),Found));
			FString Error;
			if(!Volume->ValidateCurrentBake(Error))
			{
				PreviousAsset=Volume->BakedField;
				Volume->GenerateProbes();
				if(Volume->GeneratedProbes<=0)return Finish(false,TEXT("Existing W1 fixture probe generation failed: ")+Volume->Status);
				Volume->Bake();Stage=1;return false;
			}
			return TransitionToPIE(Error);
		}
		if(Stage==1)
		{
			if(!Volume.IsValid())return Finish(false,TEXT("BakeVolume disappeared during existing-fixture bake."));
			if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded")))
				return Finish(false,TEXT("Existing-fixture bake failed: ")+Volume->Status);
			if(!Volume->BakedField||Volume->BakedField==PreviousAsset.Get())return false;
			FString Error;if(!Volume->ValidateCurrentBake(Error))return Finish(false,Error);
			return TransitionToPIE(Error);
		}

		UWorld* PIE=BoundsPIE();
		if(!PIE)return Finish(false,TEXT("PIE ended before bounds evidence."));
		if(Stage==2)
		{
			int32 Found=0;
			for(TActorIterator<AIMAcousticBakeVolume> It(PIE);It;++It){++Found;Volume=*It;}
			Listener=PIE->GetFirstPlayerController();
			Bridge=IMAcousticTestSupport::FindBridge(PIE);
			if(Found!=1||!Volume.IsValid()||!Listener.IsValid()||!Bridge.IsValid())return false;
			if(UWorld::RemovePIEPrefix(PIE->GetOutermost()->GetName())!=BoundsMap)
				return Finish(false,TEXT("Bounds test opened a different PIE map."));
			Box=Volume->BakeBounds->Bounds.GetBox();
			InsideListener=FVector(0,200,150);
			InsideSource=FVector(0,-200,150);
			if(!Box.IsInsideOrOn(InsideListener)||!Box.IsInsideOrOn(InsideSource))
				return Finish(false,TEXT("W1 fixture listener/source baseline is outside BakeBounds."));
			Listener->SetAudioListenerOverride(nullptr,InsideListener,FRotator(0,-90,0));
			FString Error;if(!SpawnSource(PIE,Error))return Finish(false,Error);
			Bridge=IMAcousticTestSupport::FindBridge(PIE);
			BaselinePushes=CurrentPushes();Stage=3;PhaseStarted=Now;return false;
		}

		Bridge=IMAcousticTestSupport::FindBridge(PIE);
		FeedAudio();
		if(Stage==3)
		{
			FIMAcousticSnapshotProbe Latest{};uint64 Index=0;uint32 Sources=0;
			if(CurrentPushes()>=BaselinePushes+4&&LatestCompleted(Latest,Index,Sources,BaselinePushes)&&Sources>=1
				&&!Volume->Status.Contains(TEXT("outside bake bounds"),ESearchCase::IgnoreCase))
			{
				BaselineStatus=Volume->Status;BaselineLatestIndex=Index;BaselineSources=Sources;
				BaselinePushesAfter=CurrentPushes();
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsBaseline pushes0=%llu pushes1=%llu latest_index=%llu num_sources=%u status=%s PASS"),BaselinePushes,BaselinePushesAfter,Index,Sources,*BaselineStatus);
				Listener->SetAudioListenerOverride(nullptr,Box.Max+FVector(5000,0,0),FRotator(0,0,0));
				ListenerOutsidePushes0=CurrentPushes();PhaseStarted=Now;Stage=4;return false;
			}
			if(Now-PhaseStarted>BoundsBaselineTimeout)return Finish(false,TEXT("Bounds baseline did not obtain 4 pushes and a completed NumSources>=1 snapshot within 90 seconds."));
			return false;
		}
		if(Stage==4)
		{
			if(Volume->Status==ListenerOutsideStatusText)
			{
				ListenerOutsideStatus=Volume->Status;ListenerOutsidePushes0=CurrentPushes();PhaseStarted=Now;Stage=5;return false;
			}
			if(Now-PhaseStarted>BoundsPhaseTimeout)return Finish(false,TEXT("Listener-outside status did not reach the exact required degradation string within 15 seconds."));
			return false;
		}
		if(Stage==5)
		{
			if(Now-PhaseStarted<BoundsListenerFreeze)return false;
			ListenerOutsidePushes1=CurrentPushes();
			const bool Pass=ListenerOutsidePushes1==ListenerOutsidePushes0;
			ListenerOutsidePass=Pass;
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsCase listener_outside status=%s pushes0=%llu pushes1=%llu %s"),
				*ListenerOutsideStatus,ListenerOutsidePushes0,ListenerOutsidePushes1,Pass?TEXT("PASS"):TEXT("FAIL"));
			if(!Pass)return Finish(false,TEXT("Listener outside bounds continued submitting snapshots."));
			Listener->SetAudioListenerOverride(nullptr,InsideListener,FRotator(0,-90,0));
			ListenerRestorePushes0=CurrentPushes();PhaseStarted=Now;Stage=6;return false;
		}
		if(Stage==6)
		{
			FIMAcousticSnapshotProbe Latest{};uint64 Index=0;uint32 Sources=0;
			if(CurrentPushes()>ListenerRestorePushes0&&LatestCompleted(Latest,Index,Sources,ListenerRestorePushes0)&&Sources>=1)
			{
				ListenerRestoreStatus=Volume->Status;ListenerRestorePushes1=CurrentPushes();ListenerRestoreLatestIndex=Index;ListenerRestoreSources=Sources;ListenerRestorePass=true;
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsCase listener_restore pushes0=%llu pushes1=%llu latest_index=%llu num_sources=%u status=%s PASS"),ListenerRestorePushes0,ListenerRestorePushes1,Index,Sources,*ListenerRestoreStatus);
				SourceOutsidePushes0=CurrentPushes();SourceActor->SetActorLocation(Box.Max+FVector(5000,0,0));
				PhaseStarted=Now;Stage=7;return false;
			}
			if(Now-PhaseStarted>BoundsPhaseTimeout)return Finish(false,TEXT("Listener failed to recover snapshot submission within 15 seconds."));
			return false;
		}
		if(Stage==7)
		{
			FIMAcousticSnapshotProbe Latest{};uint64 Index=0;uint32 Sources=0;
			if(CurrentPushes()>SourceOutsidePushes0&&LatestCompleted(Latest,Index,Sources,SourceOutsidePushes0)&&Sources==0)
			{
				SourceOutsidePushes1=CurrentPushes();SourceOutsideLatestIndex=Index;SourceOutsideSources=Sources;SourceOutsideStatus=Volume->Status;SourceOutsidePass=true;
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsCase source_outside pushes0=%llu pushes1=%llu latest_index=%llu num_sources=%u status=%s PASS"),SourceOutsidePushes0,SourceOutsidePushes1,Index,Sources,*SourceOutsideStatus);
				SourceRestorePushes0=CurrentPushes();SourceActor->SetActorLocation(InsideSource);PhaseStarted=Now;Stage=8;return false;
			}
			if(Now-PhaseStarted>BoundsPhaseTimeout)return Finish(false,TEXT("Source outside bounds did not produce a new completed NumSources=0 snapshot within 15 seconds."));
			return false;
		}
		if(Stage==8)
		{
			FIMAcousticSnapshotProbe Latest{};uint64 Index=0;uint32 Sources=0;
			if(CurrentPushes()>SourceRestorePushes0&&LatestCompleted(Latest,Index,Sources,SourceRestorePushes0)&&Sources>=1)
			{
				SourceRestorePushes1=CurrentPushes();SourceRestoreLatestIndex=Index;SourceRestoreSources=Sources;SourceRestoreStatus=Volume->Status;SourceRestorePass=true;
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsCase source_restore_negative pushes0=%llu pushes1=%llu latest_index=%llu num_sources=%u status=%s PASS"),SourceRestorePushes0,SourceRestorePushes1,Index,Sources,*SourceRestoreStatus);
				return Finish(ListenerOutsidePass&&ListenerRestorePass&&SourceOutsidePass&&SourceRestorePass,TEXT("Bounds listener/source out-of-bounds degradation and recovery gates passed."));
			}
			if(Now-PhaseStarted>BoundsPhaseTimeout)return Finish(false,TEXT("Source failed to re-enter the completed snapshot after returning inside bounds."));
			return false;
		}
		return Finish(false,TEXT("Unknown bounds test stage."));
	}

private:
	uint64 CurrentPushes() const{return Bridge.IsValid()?Bridge->SnapshotProbePushes.load(std::memory_order_acquire):0;}
	bool LatestCompleted(FIMAcousticSnapshotProbe& Out,uint64& Index,uint32& Sources,uint64 MinimumIndex) const
	{
		if(!Bridge.IsValid())return false;
		const uint64 Pushes=CurrentPushes();
		const uint64 Limit=Pushes<FIMAcousticDeviceBridge::ProbeSnapshotCapacity?Pushes:FIMAcousticDeviceBridge::ProbeSnapshotCapacity;
		for(uint64 I=Limit;I>0;--I)
		{
			const uint64 Candidate=I-1;
			if(Candidate<MinimumIndex)break;
			if(Bridge->SnapshotDone[Candidate].load(std::memory_order_acquire)!=Candidate+1)continue;
			Out=Bridge->SnapshotProbes[Candidate];Index=Candidate;Sources=Out.NumSources;return true;
		}
		return false;
	}
	bool TransitionToPIE(FString& Error)
	{
		if(!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,FPackageName::LongPackageNameToFilename(BoundsMap,FPackageName::GetMapPackageExtension())))
			return Finish(false,TEXT("Cannot persist W1 bake binding before bounds PIE."));
		GUnrealEd->AutomationLoadMap(BoundsMap,false,&Error);
		if(!Error.IsEmpty())return Finish(false,Error);
		Stage=2;
		// AutomationLoadMap queues PIE startup. Yield this command completely;
		// a separate latent command must run behind the queued world transition.
		ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticBoundsCommand(Test,Directory,2,Started));
		return true;
	}
	bool SpawnSource(UWorld* World,FString& Error)
	{
		SourceActor=World->SpawnActor<AActor>();
		if(!SourceActor.IsValid()){Error=TEXT("Cannot spawn bounds source actor.");return false;}
		SourceActor->SetActorLocation(InsideSource);
		SourceAudio=NewObject<UAudioComponent>(SourceActor.Get());SourceActor->SetRootComponent(SourceAudio.Get());SourceActor->AddInstanceComponent(SourceAudio.Get());
		SourceAudio->bAutoActivate=false;SourceAudio->RegisterComponent();SourceAudio->SetWorldLocation(InsideSource);
		auto* Marker=NewObject<UIMAcousticSourceComponent>(SourceActor.Get());SourceActor->AddInstanceComponent(Marker);Marker->AudioComponent=SourceAudio.Get();Marker->RegisterComponent();
		if(!IMAcousticTestSupport::ConfigureGraphSource(SourceAudio.Get(),Error))return false;
		if(!Marker->ValidateSource(Error))return false;
		SourceAudio->Play();return true;
	}
	void FeedAudio()
	{
		if(!Wave.IsValid()||FeedPCM.Num()==0)return;
		static constexpr int32 ChunkSamples=BoundsSampleRate*2;
		int32 Fed=0;
		while(Wave->GetAvailableAudioByteCount()<ChunkSamples*int32(sizeof(int16))&&Fed<ChunkSamples*2)
		{
			TArray<int16> Chunk;Chunk.SetNumUninitialized(ChunkSamples);
			for(int32 I=0;I<ChunkSamples;++I)Chunk[I]=FeedPCM[(FeedCursor+I)%FeedPCM.Num()];
			Wave->QueueAudio(reinterpret_cast<const uint8*>(Chunk.GetData()),Chunk.Num()*int32(sizeof(int16)));
			FeedCursor=(FeedCursor+ChunkSamples)%FeedPCM.Num();Fed+=ChunkSamples;
		}
	}
	bool Finish(bool Pass,const FString& Message)
	{
		auto Report=MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("map"),BoundsMap);Report->SetStringField(TEXT("directory"),Directory);Report->SetNumberField(TEXT("stage"),Stage);
		Report->SetNumberField(TEXT("started_seconds"),Started);Report->SetNumberField(TEXT("finished_seconds"),FPlatformTime::Seconds());
		Report->SetStringField(TEXT("baseline_status"),BaselineStatus);Report->SetNumberField(TEXT("baseline_pushes0"),double(BaselinePushes));Report->SetNumberField(TEXT("baseline_pushes1"),double(BaselinePushesAfter));Report->SetNumberField(TEXT("baseline_latest_index"),double(BaselineLatestIndex));Report->SetNumberField(TEXT("baseline_num_sources"),BaselineSources);
		Report->SetStringField(TEXT("listener_outside_status"),ListenerOutsideStatus);Report->SetNumberField(TEXT("listener_outside_pushes0"),double(ListenerOutsidePushes0));Report->SetNumberField(TEXT("listener_outside_pushes1"),double(ListenerOutsidePushes1));Report->SetBoolField(TEXT("listener_outside_passed"),ListenerOutsidePass);
		Report->SetStringField(TEXT("listener_restore_status"),ListenerRestoreStatus);Report->SetNumberField(TEXT("listener_restore_pushes0"),double(ListenerRestorePushes0));Report->SetNumberField(TEXT("listener_restore_pushes1"),double(ListenerRestorePushes1));Report->SetNumberField(TEXT("listener_restore_latest_index"),double(ListenerRestoreLatestIndex));Report->SetNumberField(TEXT("listener_restore_num_sources"),ListenerRestoreSources);Report->SetBoolField(TEXT("listener_restore_passed"),ListenerRestorePass);
		Report->SetStringField(TEXT("source_outside_status"),SourceOutsideStatus);Report->SetNumberField(TEXT("source_outside_pushes0"),double(SourceOutsidePushes0));Report->SetNumberField(TEXT("source_outside_pushes1"),double(SourceOutsidePushes1));Report->SetNumberField(TEXT("source_outside_latest_index"),double(SourceOutsideLatestIndex));Report->SetNumberField(TEXT("source_outside_num_sources"),SourceOutsideSources);Report->SetBoolField(TEXT("source_outside_passed"),SourceOutsidePass);
		Report->SetStringField(TEXT("source_restore_status"),SourceRestoreStatus);Report->SetNumberField(TEXT("source_restore_pushes0"),double(SourceRestorePushes0));Report->SetNumberField(TEXT("source_restore_pushes1"),double(SourceRestorePushes1));Report->SetNumberField(TEXT("source_restore_latest_index"),double(SourceRestoreLatestIndex));Report->SetNumberField(TEXT("source_restore_num_sources"),SourceRestoreSources);Report->SetBoolField(TEXT("source_restore_negative_control_passed"),SourceRestorePass);
		Report->SetBoolField(TEXT("passed"),Pass);Report->SetStringField(TEXT("message"),Message);
		const bool EvidenceSaved=BoundsJson(FPaths::Combine(Directory,TEXT("bounds.json")),Report);
		auto Terminal=MakeShared<FJsonObject>();Terminal->SetBoolField(TEXT("passed"),Pass&&EvidenceSaved);Terminal->SetStringField(TEXT("message"),EvidenceSaved?Message:TEXT("Cannot save bounds.json."));Terminal->SetNumberField(TEXT("stage"),Stage);
		const bool TerminalSaved=BoundsJson(FPaths::Combine(Directory,TEXT("terminal.json")),Terminal);
		const bool FinalPass=Pass&&EvidenceSaved&&TerminalSaved;
		if(!FinalPass)Test->AddError(Message);else Test->AddInfo(Message);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsDegrade %s %s evidence=%s"),FinalPass?TEXT("PASS"):TEXT("FAIL"),*Message,*Directory);
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticBoundsDegrade %s"),FinalPass?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),FinalPass?TEXT("PASS"):TEXT("FAIL"));
		if(GEditor&&BoundsPIE())GEditor->RequestEndPlayMap();
		return true;
	}

	FAutomationTestBase* Test=nullptr;FString Directory;
	double Started=0,LastDiagnostic=0,PhaseStarted=0;
	int32 Stage=0;FBox Box;FVector InsideListener,InsideSource;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<UIMAcousticBakeAsset> PreviousAsset;
	TWeakObjectPtr<APlayerController> Listener;TWeakObjectPtr<AActor> SourceActor;TWeakObjectPtr<UAudioComponent> SourceAudio;TWeakObjectPtr<USoundWaveProcedural> Wave;
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;
	TArray<int16> FeedPCM;int32 FeedCursor=0;
	uint64 BaselinePushes=0,BaselinePushesAfter=0,BaselineLatestIndex=0;uint32 BaselineSources=0;
	uint64 ListenerOutsidePushes0=0,ListenerOutsidePushes1=0,ListenerRestorePushes0=0,ListenerRestorePushes1=0,ListenerRestoreLatestIndex=0;uint32 ListenerRestoreSources=0;
	uint64 SourceOutsidePushes0=0,SourceOutsidePushes1=0,SourceOutsideLatestIndex=0,SourceRestorePushes0=0,SourceRestorePushes1=0,SourceRestoreLatestIndex=0;uint32 SourceOutsideSources=0,SourceRestoreSources=0;
	bool ListenerOutsidePass=false,ListenerRestorePass=false,SourceOutsidePass=false,SourceRestorePass=false;
	FString BaselineStatus,ListenerOutsideStatus,ListenerRestoreStatus,SourceOutsideStatus,SourceRestoreStatus;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticBoundsDegrade,"IceMoon.AcousticField.W2.BoundsDegrade",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticBoundsDegrade::RunTest(const FString&)
{
	const FString Directory=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W2-Bounds"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Directory,true);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBoundsDegrade START evidence=%s"),*Directory);
	FString Error;if(!FEditorFileUtils::LoadMap(IMAcousticBoundsTestPrivate::BoundsMap,false,true)){Error=TEXT("Cannot load existing W1 fixture map.");AddError(Error);return false;}
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<IMAcousticBoundsTestPrivate::FIMAcousticBoundsCommand>(this,Directory));return true;
}
#endif
