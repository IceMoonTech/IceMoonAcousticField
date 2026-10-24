#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
// Editor preparation only. Root runs this in the manifest-bound copy after its
// audio tests. Ordinary audition uses the already authored graph and selected
// full water-drop Wave; the W3 pressure tests retain their synthetic stimulus.
// First run: duplicate the existing single-level whitebox, clear its wrong-map
// bake binding, author persistent native audio, then use BakeVolume's own bake.
// Existing target: validate and exit WITHOUT saving, rebaking or overwriting.
// An interrupted first run intentionally leaves an incomplete target for explicit
// root review; rerunning cannot silently replace that map or its assets.
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInterface.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWave.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundOperatorSettings.h"
#include "MetasoundSource.h"
#include "StaticMeshResources.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "ObjectTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace IMAcousticAuditionSetupTestPrivate
{
constexpr const TCHAR* SourceMap=TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr const TCHAR* TargetMap=TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
constexpr const TCHAR* IMMetaSoundPackage=TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");
constexpr const TCHAR* WavePackagePath=TEXT("/IceMoonAcousticField/Tests/Audio/SW_WaterDropPotsi_SoftTailModerate_28s");
constexpr const TCHAR* AttenuationPackage=TEXT("/IceMoonAcousticField/Tests/IM_V2AuditionAttenuation");
// User-selected potsi Moderate PCM, 48 kHz/mono/16-bit and 28.176 seconds.
// Hash imported PCM, not the container or historical MetaSound asset name.
constexpr int32 IMSelectedFrames=1352448;
constexpr const TCHAR* IMSelectedPCMHash=TEXT("6845F1DD535FFC9F99E0FC87B97C3A8221043BEC");
constexpr const TCHAR* ReadyTag=TEXT("IMAcousticAuditionPreparedV1");
constexpr const TCHAR* SourceTag=TEXT("IMAcousticAuditionReferenceV1");
const FVector SourcePosition(550,300,150),StartPosition(750,300,150);

bool Reject(FString& Error,const FString& Why){Error=Why;return false;}
FString ObjectPath(const TCHAR* Package){return FString(Package)+TEXT(".")+FPackageName::GetLongPackageAssetName(Package);}
FString HashBytes(const uint8* Data,int32 Count)
{
	FSHA1 Hash;if(Count>0)Hash.Update(Data,Count);Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20);
}
bool FileHash(const FString& File,FString& Digest)
{
	TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*File))return false;
	Digest=HashBytes(Bytes.GetData(),Bytes.Num());return true;
}
FString TransformKey(const FTransform& T)
{
	const FVector P=T.GetLocation(),S=T.GetScale3D();const FQuat Q=T.GetRotation();
	// Preserve LWC precision; FTransform::ToString's display rounding is not a
	// geometry-preservation oracle for a copied map.
	return FString::Printf(TEXT("%.17g,%.17g,%.17g|%.17g,%.17g,%.17g,%.17g|%.17g,%.17g,%.17g"),
		P.X,P.Y,P.Z,Q.X,Q.Y,Q.Z,Q.W,S.X,S.Y,S.Z);
}
struct FIMGeometryIdentity
{
	int32 Meshes=0,WhiteboxMeshes=0,Instances=0;
	FString Hash;
	TArray<FString> Rows;
	TMap<FString,TWeakObjectPtr<UStaticMeshComponent>> Components;
	TArray<FString> NonBakedAdditions;
};
bool Geometry(UWorld* World,FIMGeometryIdentity& Out,FString& Error)
{
	Out={};
	if(!World||!World->PersistentLevel)return Reject(Error,TEXT("No persistent world for geometry identity."));
	for(TActorIterator<AActor> It(World);It;++It)
	{
		TArray<UStaticMeshComponent*> Meshes;It->GetComponents(Meshes);
		for(const auto* M:Meshes)
		{
			const UStaticMesh* Mesh=M->GetStaticMesh();
			const FStaticMeshRenderData* Data=Mesh?Mesh->GetRenderData():nullptr;
			if(!Data||Data->LODResources.Num()==0)return Reject(Error,TEXT("Cannot verify LOD0 geometry: ")+M->GetPathName());
			const auto& LOD=Data->LODResources[0];const auto Indices=LOD.IndexBuffer.GetArrayView();
			const uint32 Vertices=LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
			if(!Vertices||!Indices.Num())return Reject(Error,TEXT("Empty/unavailable CPU LOD0: ")+M->GetPathName());
			FSHA1 MeshHash;
			for(uint32 I=0;I<Vertices;++I)
			{
				const FVector3f V=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I);
				const float Values[]{V.X,V.Y,V.Z};MeshHash.Update(reinterpret_cast<const uint8*>(Values),sizeof(Values));
			}
			for(int32 I=0;I<Indices.Num();++I)
			{
				const uint32 Index=Indices[I];if(Index>=Vertices)return Reject(Error,TEXT("Invalid LOD0 index."));
				MeshHash.Update(reinterpret_cast<const uint8*>(&Index),sizeof(Index));
			}
			MeshHash.Final();uint8 Digest[20];MeshHash.GetHash(Digest);
			// Relative to PersistentLevel: map-package renaming alone must not
			// change identity. All mesh components, including visual helpers,
			// remain in this comparison even if acoustically ignored.
			FString Row=M->GetPathName(World->PersistentLevel)+TEXT("|")+M->GetClass()->GetPathName()+TEXT("|")
				+Mesh->GetPathName()+TEXT("|")+TransformKey(M->GetComponentTransform())+TEXT("|")+BytesToHex(Digest,20)
				+FString::Printf(TEXT("|mobility=%d|collision=%d|vertices=%u|indices=%d"),int32(M->Mobility),int32(M->GetCollisionEnabled()),Vertices,Indices.Num());
			for(int32 I=0;I<M->GetNumMaterials();++I)Row+=TEXT("|material=")+GetPathNameSafe(M->GetMaterial(I));
			if(const auto* ISM=Cast<UInstancedStaticMeshComponent>(M))
			{
				Row+=FString::Printf(TEXT("|instances=%d"),ISM->GetInstanceCount());
				for(int32 I=0;I<ISM->GetInstanceCount();++I)
				{
					FTransform T;if(!ISM->GetInstanceTransform(I,T,true))return Reject(Error,TEXT("Cannot read instance transform."));
					Row+=TEXT("|")+TransformKey(T);++Out.Instances;
				}
			}
			Out.Rows.Add(Row);Out.Components.Add(Row,const_cast<UStaticMeshComponent*>(M));
			++Out.Meshes;if(It->ActorHasTag(TEXT("IMAcousticWhitebox")))++Out.WhiteboxMeshes;
		}
	}
	Out.Rows.Sort();FSHA1 Hash;
	for(const FString& Row:Out.Rows)
	{
		FTCHARToUTF8 Bytes(*Row);const uint32 Length=Bytes.Length();
		Hash.Update(reinterpret_cast<const uint8*>(&Length),sizeof(Length));Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()),Length);
	}
	Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);Out.Hash=BytesToHex(Digest,20);
	return Out.Meshes>0||Reject(Error,TEXT("Source map contains no meshes."));
}
bool FindVolume(UWorld* World,AIMAcousticBakeVolume*& Out,FString& Error)
{
	Out=nullptr;
	for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It)
	{if(Out)return Reject(Error,TEXT("Expected one BakeVolume, found multiple."));Out=*It;}
	return Out!=nullptr||Reject(Error,TEXT("Existing W2 BakeVolume/material mapping is missing; run W2 preparation first."));
}
bool IMPreservesGeometry(const FIMGeometryIdentity& Baseline,FIMGeometryIdentity& Target,
	const AIMAcousticBakeVolume* Volume,FString& Error)
{
	if(!Volume||!Volume->BakeBounds||Target.WhiteboxMeshes!=Baseline.WhiteboxMeshes)
		return Reject(Error,TEXT("Audition whitebox mesh count/bake region differs."));
	for(const FString& Row:Baseline.Rows)
		if(!Target.Components.Contains(Row))return Reject(Error,TEXT("An original whitebox component changed or disappeared: ")+Row);
	const FBox Region=Volume->BakeBounds->Bounds.GetBox();
	for(const FString& Row:Target.Rows)
	{
		if(Baseline.Components.Contains(Row))continue;
		const auto* Component=Target.Components.FindChecked(Row).Get();
		if(!Component||Component->ComponentHasTag(TEXT("IMAcousticRequired")))
			return Reject(Error,TEXT("An additional component cannot be treated as a non-baked helper: ")+Row);
		// CaptureScene excludes movable/editor/ignored helpers and geometry
		// outside this exact world-space bake box. Original rows remain exact;
		// an added static component inside the region still fails preparation.
		bool bExcluded=Component->Mobility!=EComponentMobility::Static
			||Component->ComponentHasTag(TEXT("IMAcousticIgnore"))
			||!Region.Intersect(Component->CalcBounds(Component->GetComponentTransform()).GetBox());
#if WITH_EDITORONLY_DATA
		bExcluded|=Component->IsVisualizationComponent()||Component->IsEditorOnly();
#endif
		if(!bExcluded)return Reject(Error,TEXT("Additional baked geometry requires an explicit new fixture contract: ")+Row);
		Target.NonBakedAdditions.Add(Component->GetPathName(Volume->GetWorld()->PersistentLevel));
	}
	return true;
}
bool UnusedPackage(const TCHAR* Name,FString& Error)
{
	return (!FPackageName::DoesPackageExist(Name)&&!FindPackage(nullptr,Name))
		||Reject(Error,TEXT("Refusing to overwrite existing package: ")+FString(Name));
}
bool SaveNewAsset(UObject* Asset,FString& Error)
{
	const FString File=FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
	if(IFileManager::Get().FileExists(*File))return Reject(Error,TEXT("Refusing asset overwrite: ")+File);
	Asset->MarkPackageDirty();FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
	if(!UPackage::SavePackage(Asset->GetOutermost(),Asset,*File,Args))return Reject(Error,TEXT("Asset save failed: ")+File);
	FAssetRegistryModule::AssetCreated(Asset);return true;
}
bool VerifyWave(USoundWave* Wave,FString& Error)
{
	// The graph's Wave Player owns playback and the full loop. Keep the accepted
	// imported asset settings; legacy standalone-wave virtualization is unrelated.
	if(!Wave||Wave->GetPathName()!=ObjectPath(WavePackagePath)||Wave->NumChannels!=1
		||Wave->GetImportedSampleRate()!=48000)
		return Reject(Error,TEXT("Selected water-drop Wave path/mono/48k contract differs."));
	TArray<uint8> PCM;uint32 Rate=0;uint16 Channels=0;
	if(!Wave->GetImportedSoundWaveData(PCM,Rate,Channels)||Rate!=48000||Channels!=1||PCM.Num()!=IMSelectedFrames*2
		||HashBytes(PCM.GetData(),PCM.Num())!=IMSelectedPCMHash)
		return Reject(Error,TEXT("Imported water-drop PCM differs from the selected full-loop source."));
	return true;
}

const FMetasoundFrontendLiteral* IMUnconnectedDefault(const FMetasoundFrontendGraph& Graph,
	const FMetasoundFrontendNode& Node,const TCHAR* Name)
{
	const auto* Pin=Node.Interface.Inputs.FindByPredicate([&](const auto& V){return V.Name==Name;});
	if(!Pin||Graph.Edges.ContainsByPredicate([&](const auto& E)
		{return E.ToNodeID==Node.GetID()&&E.ToVertexID==Pin->VertexID;}))return nullptr;
	const auto* Literal=Node.InputLiterals.FindByPredicate([&](const auto& V){return V.VertexID==Pin->VertexID;});
	return Literal?&Literal->Value:nullptr;
}

bool IMVerifyAuditionGraph(const FMetasoundFrontendGraph& Graph,USoundWave*& Wave,FString& Error)
{
	Wave=nullptr;
	const FMetasoundFrontendNode* Player=nullptr;
	for(const auto& Node:Graph.Nodes)if(Node.Name==TEXT("Wave Player"))
	{if(Player)return Reject(Error,TEXT("Ordinary audition requires exactly one Wave Player."));Player=&Node;}
	if(!Player)return Reject(Error,TEXT("Ordinary audition Wave Player is missing."));
	const auto* Asset=IMUnconnectedDefault(Graph,*Player,TEXT("Wave Asset"));
	UObject* Object=nullptr;
	if(!Asset||!Asset->TryGet(Object)||!VerifyWave(Cast<USoundWave>(Object),Error))
		return Reject(Error,TEXT("Wave Player does not consume the selected water-drop default: ")+Error);
	const auto* Loop=IMUnconnectedDefault(Graph,*Player,TEXT("Loop"));bool Looping=false;
	if(!Loop||!Loop->TryGet(Looping)||!Looping)
		return Reject(Error,TEXT("Wave Player must consume its enabled loop default."));
	for(const auto& Expected:{TPair<const TCHAR*,float>(TEXT("Start Time"),0.f),
		TPair<const TCHAR*,float>(TEXT("Pitch Shift"),0.f),TPair<const TCHAR*,float>(TEXT("Loop Start"),0.f),
		TPair<const TCHAR*,float>(TEXT("Loop Duration"),-1.f)})
	{
		const auto* Literal=IMUnconnectedDefault(Graph,*Player,Expected.Key);float Actual=0.f;
		if(!Literal||!Literal->TryGet(Actual)||!FMath::IsNearlyEqual(Actual,Expected.Value))
			return Reject(Error,FString::Printf(TEXT("Wave Player %s differs from the unpitched full-loop contract."),Expected.Key));
	}
	Wave=CastChecked<USoundWave>(Object);return true;
}

bool IMVerifyAuditionSound(USoundBase* Sound,USoundWave*& Wave,FString& Error)
{
	Wave=nullptr;auto* Source=Cast<UMetaSoundSource>(Sound);
	if(!Source||Source->GetPathName()!=ObjectPath(IMMetaSoundPackage)
		||Source->OutputFormat!=EMetaSoundOutputAudioFormat::Stereo||Source->NumChannels!=2
		||Source->GetOperatorSettings(48000).GetNumFramesPerBlock()!=512)
		return Reject(Error,TEXT("Ordinary audition requires the current stereo acoustic MetaSound with 512-frame blocks."));
	const auto* Document=Cast<IMetaSoundDocumentInterface>(Source);
	if(!Document)return Reject(Error,TEXT("Ordinary audition source has no native MetaSound document."));
	return IMVerifyAuditionGraph(Document->GetConstDocument().RootGraph.GetConstDefaultGraph(),Wave,Error);
}

bool CreateAudio(UWorld* World,FString& Error)
{
	USoundWave* Wave=nullptr;
	auto* Sound=LoadObject<UMetaSoundSource>(nullptr,*ObjectPath(IMMetaSoundPackage));
	if(!IMVerifyAuditionSound(Sound,Wave,Error)||!UnusedPackage(AttenuationPackage,Error))return false;
	auto* Attenuation=NewObject<USoundAttenuation>(CreatePackage(AttenuationPackage),
		FName(*FPackageName::GetLongPackageAssetName(AttenuationPackage)),RF_Public|RF_Standalone);
	auto& S=Attenuation->Attenuation;
	S.bSpatialize=false;
	S.bAttenuate=false;S.bAttenuateWithLPF=false;S.bEnableOcclusion=false;S.bEnableListenerFocus=false;S.bEnableReverbSend=false;
	S.PluginSettings.SpatializationPluginSettingsArray.Reset();
	S.PluginSettings.OcclusionPluginSettingsArray.Reset();S.PluginSettings.ReverbPluginSettingsArray.Reset();S.PluginSettings.SourceDataOverridePluginSettingsArray.Reset();
	if(!SaveNewAsset(Attenuation,Error))return false;
	FActorSpawnParameters Spawn;Spawn.Name=TEXT("IM_V2AuditionSource");Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* Ambient=World->SpawnActor<AAmbientSound>(SourcePosition,FRotator::ZeroRotator,Spawn);
	if(!Ambient)return Reject(Error,TEXT("Cannot spawn persistent AAmbientSound."));
	Ambient->SetActorLabel(TEXT("IM_V2_AUDITION_SOURCE - full water-drop MetaSound"));Ambient->Tags.AddUnique(SourceTag);
	// BakeVolume starts managed playback after the source has its graph binding;
	// automatic AudioComponent activation would race that initialization.
	auto* Audio=Ambient->GetAudioComponent();Audio->bAutoActivate=false;Audio->bAutoDestroy=false;Audio->bStopWhenOwnerDestroyed=true;
	Audio->bAllowSpatialization=false;Audio->bIsUISound=false;Audio->bOverrideAttenuation=false;Audio->SetAttenuationSettings(Attenuation);
	Audio->VolumeMultiplier=.7f;Audio->PitchMultiplier=1;Audio->SetSound(Sound);
	auto* Marker=NewObject<UIMAcousticSourceComponent>(Ambient,TEXT("IM_AcousticSource"),RF_Transactional);
	Marker->AudioComponent=Audio;Ambient->AddInstanceComponent(Marker);Marker->RegisterComponent();
	if(!Marker->ValidateSource(Error))return false;
	APlayerStart* Start=nullptr;
	for(TActorIterator<APlayerStart> It(World);It;++It){if(Start)return Reject(Error,TEXT("Multiple player starts require explicit review."));Start=*It;}
	if(!Start)Start=World->SpawnActor<APlayerStart>();
	if(!Start)return Reject(Error,TEXT("Cannot create audition start."));
	Start->Modify();Start->SetActorLocationAndRotation(StartPosition,FRotator::ZeroRotator);
	Start->PlayerStartTag=TEXT("IM_V2AuditionStart");Start->SetActorLabel(TEXT("IM_V2_AUDITION_START - WASD mouse"));
	TArray<UPrimitiveComponent*> Helpers;Start->GetComponents(Helpers);
	for(auto* C:Helpers){C->Modify();C->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));}
	// Native GameModeBase supplies DefaultPawn and its built-in movement input.
	// It is free movement with collision, not a new grounded character controller.
	World->GetWorldSettings()->Modify();World->GetWorldSettings()->DefaultGameMode=AGameModeBase::StaticClass();
	return true;
}

class FIMAuditionPrepareCommand final:public IAutomationLatentCommand
{
public:
	// Automation owns Test until this latent command completes. Every UObject
	// lookup/mutation is on GT; BakeVolume exclusively owns SDK async work.
	explicit FIMAuditionPrepareCommand(FAutomationTestBase* InTest):Test(InTest),Started(FPlatformTime::Seconds()){}
	bool Update() override
	{
		check(IsInGameThread());FString Error;
		if(!GEditor||GEditor->PlayWorld)return Finish(false,TEXT("PrepareAudition requires an idle owned Editor; PIE must be stopped by root first."));
		if(FPlatformTime::Seconds()-Started>900)
		{if(bCreatedMap&&Volume.IsValid())Volume->CancelBake();return Finish(false,TEXT("Audition preparation timed out; incomplete target retained, never overwritten on rerun."));}
		if(Stage==0)
		{
			Evidence=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/AuditionPrepare"),FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*Evidence,true);
			UWorld* Current=GEditor->GetEditorWorldContext().World();
			if(Current&&Current->GetOutermost()->IsDirty())return Finish(false,TEXT("Current Editor map has unsaved changes; use a clean owned session."));
			if(!FPackageName::DoesPackageExist(SourceMap,&SourceFile)||!FileHash(SourceFile,SourceFileHash))return Finish(false,TEXT("Existing whitebox map file missing."));
			if(!FEditorFileUtils::LoadMap(SourceMap,false,true))return Finish(false,TEXT("Cannot load existing whitebox."));
			UWorld* Source=GEditor->GetEditorWorldContext().World();
			if(Source->GetWorldPartition()||Source->GetStreamingLevels().Num()!=0||Source->PersistentLevel->IsUsingExternalActors())
				return Finish(false,TEXT("This preparation supports the verified single-level whitebox; external actors/streaming require separate copy validation."));
			if(!Geometry(Source,Baseline,Error)||Baseline.WhiteboxMeshes!=60)return Finish(false,TEXT("Source geometry differs from the verified 60-mesh whitebox: ")+Error);
			FFileHelper::SaveStringArrayToFile(Baseline.Rows,*FPaths::Combine(Evidence,TEXT("source-geometry.txt")));
			AIMAcousticBakeVolume* SourceVolume=nullptr;
			if(!FindVolume(Source,SourceVolume,Error))return Finish(false,Error);
			OriginalBakePath=GetPathNameSafe(SourceVolume->BakedField.Get());
			if(FPackageName::DoesPackageExist(TargetMap))
			{
				bExisting=true;if(!FEditorFileUtils::LoadMap(TargetMap,false,true))return Finish(false,TEXT("Existing audition map cannot load; refusing replacement."));
				const bool PreparedValid=ValidatePrepared(Error);
				return Finish(PreparedValid,PreparedValid?TEXT("Existing audition map validated; no writes or rebake."):Error);
			}
			if(!UnusedPackage(TargetMap,Error))return Finish(false,Error);
			USoundWave* SelectedWave=nullptr;
			if(!IMVerifyAuditionSound(LoadObject<UMetaSoundSource>(nullptr,*ObjectPath(IMMetaSoundPackage)),SelectedWave,Error)
				||!UnusedPackage(AttenuationPackage,Error))return Finish(false,Error);
			for(TActorIterator<AActor> It(Source);It;++It)
			{
				TArray<UAudioComponent*> Audio;It->GetComponents(Audio);
				TArray<UIMAcousticSourceComponent*> Markers;It->GetComponents(Markers);
				if(Audio.Num()||Markers.Num())return Finish(false,TEXT("Source contains pre-existing audio; refusing to delete/mute it to manufacture a one-source fixture."));
			}
			ObjectTools::FPackageGroupName Name;Name.PackageName=TargetMap;Name.ObjectName=FPackageName::GetLongPackageAssetName(TargetMap);
			TSet<UPackage*> Refused;
			auto* Copy=Cast<UWorld>(ObjectTools::DuplicateSingleObject(Source,Name,Refused,false));
			if(!Copy||Copy==Source||Copy->GetOutermost()->GetName()!=TargetMap)return Finish(false,TEXT("World duplication failed or retained source package ownership."));
			if(Source->PersistentLevel->MapBuildData&&Copy->PersistentLevel->MapBuildData==Source->PersistentLevel->MapBuildData)
				return Finish(false,TEXT("World copy retained source MapBuildData; refuse saving shared build data."));
			AIMAcousticBakeVolume* CopiedVolume=nullptr;
			if(!FindVolume(Copy,CopiedVolume,Error)||!Geometry(Copy,TargetGeometry,Error)||TargetGeometry.Hash!=Baseline.Hash)
				return Finish(false,TEXT("Duplicated geometry differs before authoring: ")+Error);
			CopiedVolume->Modify();CopiedVolume->BakedField=nullptr; // Original asset is invalid for the new world package.
			CopiedVolume->Tags.Remove(ReadyTag);
			const FString TargetFile=FPackageName::LongPackageNameToFilename(TargetMap,FPackageName::GetMapPackageExtension());
			if(IFileManager::Get().FileExists(*TargetFile)||!FEditorFileUtils::SaveLevel(Copy->PersistentLevel,TargetFile))
				return Finish(false,TEXT("Initial copied-map save failed or target appeared concurrently."));
			bCreatedMap=true;Stage=1;return false;
		}
		if(Stage==1)
		{
			if(!bCreatedMap||!FEditorFileUtils::LoadMap(TargetMap,false,true))return Finish(false,TEXT("Cannot open this run's new copied map."));
			UWorld* World=GEditor->GetEditorWorldContext().World();AIMAcousticBakeVolume* Found=nullptr;
			if(!FindVolume(World,Found,Error))return Finish(false,Error);Volume=Found;
			if(Volume->BakedField)return Finish(false,TEXT("Copied map unexpectedly retained an original bake binding."));
			if(!CreateAudio(World,Error))return Finish(false,Error);
			Volume->Modify();Volume->bEnableV2=true;Volume->bDirectRoute=true;Volume->bPathRoute=true;Volume->bReverbRoute=true;Volume->ReverbWetGain=.25f;
			Volume->SetActorLabel(TEXT("IM_V2_AUDITION_FIELD - V2 and route switches"));
			if(!Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(SourcePosition)||!Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(StartPosition))
				return Finish(false,TEXT("Source/start lies outside copied bake bounds."));
			if(!Geometry(World,TargetGeometry,Error)||!IMPreservesGeometry(Baseline,TargetGeometry,Volume.Get(),Error))return Finish(false,TEXT("Audio authoring changed existing mesh geometry: ")+Error);
			Volume->GenerateProbes();
			if(Volume->GeneratedProbes<=0)return Finish(false,TEXT("Copied-map probe generation failed: ")+Volume->Status);
			Volume->Bake();if(!Volume->Status.StartsWith(TEXT("Baking.")))return Finish(false,TEXT("Copied-map bake did not start: ")+Volume->Status);
			Stage=2;return false;
		}
		if(Stage==2)
		{
			if(!Volume.IsValid())return Finish(false,TEXT("BakeVolume destroyed during its async bake."));
			if(!Volume->BakedField)
			{
				if(!Volume->Status.StartsWith(TEXT("Baking.")))return Finish(false,TEXT("Copied-map bake failed: ")+Volume->Status);
				return false; // Existing BakeVolume completion ticker publishes the new saved asset.
			}
			if(!Volume->ValidateCurrentBake(Error)||Volume->BakedField->WorldPackage!=TargetMap
				||Volume->BakedField->GetPathName()==OriginalBakePath)return Finish(false,TEXT("New map requires its own current bake asset: ")+Error);
			if(!Geometry(Volume->GetWorld(),TargetGeometry,Error)||!IMPreservesGeometry(Baseline,TargetGeometry,Volume.Get(),Error))return Finish(false,TEXT("Bake changed copied mesh geometry: ")+Error);
			Volume->Tags.AddUnique(ReadyTag);Volume->MarkPackageDirty();
			const FString File=FPackageName::LongPackageNameToFilename(TargetMap,FPackageName::GetMapPackageExtension());
			if(!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,File))return Finish(false,TEXT("Cannot save audition source and new bake binding."));
			Volume.Reset();Stage=3;return false;
		}
		if(!FEditorFileUtils::LoadMap(TargetMap,false,true))return Finish(false,TEXT("Saved audition map failed reload."));
		const bool Valid=ValidatePrepared(Error);
		return Finish(Valid,Valid?TEXT("Audition assets prepared and reloaded; PIE playback and user listening remain unverified."):Error);
	}
private:
	bool ValidatePrepared(FString& Error)
	{
		UWorld* World=GEditor->GetEditorWorldContext().World();
		if(!World||World->GetOutermost()->GetName()!=TargetMap)return Reject(Error,TEXT("Wrong audition world."));
		AIMAcousticBakeVolume* Found=nullptr;if(!FindVolume(World,Found,Error))return false;Volume=Found;
		if(!Geometry(World,TargetGeometry,Error)||!IMPreservesGeometry(Baseline,TargetGeometry,Found,Error))
			return Reject(Error,TEXT("Audition geometry differs from the preserved whitebox contract; existing map will not be overwritten: ")+Error);
		if(!Volume->ActorHasTag(ReadyTag)||!Volume->BakedField||Volume->BakedField->WorldPackage!=TargetMap
			||Volume->BakedField->GetPathName()==OriginalBakePath||!Volume->BakedField->GetPathName().StartsWith(TEXT("/IceMoonAcousticField/")))
			return Reject(Error,TEXT("Audition map is incomplete or uses a wrong-map/non-plugin bake; refusing overwrite or automatic recovery."));
		if(!Volume->ValidateCurrentBake(Error))return false;
		TArray<FVector4> Preview;FVector Origin;
		if(!Volume->BakedField->GetProbePreview(TargetMap,Volume->BakedField->SceneFingerprint,Preview,Origin,Error)||Preview.IsEmpty())return false;
		ProbeCount=Preview.Num();
		if(!Volume->bEnableV2||!Volume->bDirectRoute||!Volume->bPathRoute||!Volume->bReverbRoute||!FMath::IsNearlyEqual(Volume->ReverbWetGain,.25f))
			return Reject(Error,TEXT("Audition route switches/gain differ from prepared defaults; retained for explicit review."));
		UIMAcousticSourceComponent* Marker=nullptr;UAudioComponent* Audio=nullptr;int32 AudioCount=0,StartCount=0;
		for(TActorIterator<AActor> It(World);It;++It)
		{
			TArray<UAudioComponent*> A;It->GetComponents(A);AudioCount+=A.Num();
			TArray<UIMAcousticSourceComponent*> M;It->GetComponents(M);
			for(auto* C:M){if(Marker)return Reject(Error,TEXT("Expected exactly one persistent managed source."));Marker=C;}
			if(auto* Start=Cast<APlayerStart>(*It))
			{
				++StartCount;if(Start->PlayerStartTag!=TEXT("IM_V2AuditionStart")||!Start->GetActorLocation().Equals(StartPosition,.01))
					return Reject(Error,TEXT("Audition player start differs."));
			}
		}
		if(!Marker||AudioCount!=1||StartCount!=1||!Marker->GetOwner()->IsA<AAmbientSound>()||!Marker->GetOwner()->ActorHasTag(SourceTag)
			||Marker->CreationMethod!=EComponentCreationMethod::Instance||!Marker->GetOwner()->GetActorLocation().Equals(SourcePosition,.01))
			return Reject(Error,TEXT("Persistent source actor/component/placement is missing or differs."));
		if(!Marker->ValidateSource(Error))return false;Audio=Marker->AudioComponent;
		if(Audio->bAutoActivate||Audio->bAutoDestroy||!Audio->bStopWhenOwnerDestroyed||Audio->bIsUISound||Audio->bAllowSpatialization
			||!Audio->AttenuationSettings||Audio->AttenuationSettings->GetPathName()!=ObjectPath(AttenuationPackage)
			||!FMath::IsNearlyEqual(Audio->VolumeMultiplier,.7f)||!FMath::IsNearlyEqual(Audio->PitchMultiplier,1.f))
			return Reject(Error,TEXT("Persistent AudioComponent autoplay/attenuation/volume contract differs."));
		ActualSoundPath=GetPathNameSafe(Audio->Sound);USoundWave* SelectedWave=nullptr;
		if(!IMVerifyAuditionSound(Audio->Sound,SelectedWave,Error))return false;
		ActualWavePath=SelectedWave->GetPathName();
		if(World->GetWorldSettings()->DefaultGameMode!=AGameModeBase::StaticClass())return Reject(Error,TEXT("Native movement GameMode differs."));
		FFileHelper::SaveStringArrayToFile(TargetGeometry.Rows,*FPaths::Combine(Evidence,TEXT("audition-geometry.txt")));
		return true;
	}
	bool Finish(bool Success,FString Message)
	{
		if(!SourceFileHash.IsEmpty())
		{
			FString Current;if(!FileHash(SourceFile,Current)||Current!=SourceFileHash){Success=false;Message+=TEXT(" Source whitebox file changed during preparation.");}
		}
		if(!Success)Test->AddError(Message);
		if(!Evidence.IsEmpty())
		{
			FString Json;auto W=TJsonWriterFactory<>::Create(&Json);W->WriteObjectStart();
			W->WriteValue(TEXT("scope"),TEXT("editor asset preparation only; runtime audio/user audition NOT_VERIFIED"));
			W->WriteValue(TEXT("pass"),Success);W->WriteValue(TEXT("message"),Message);W->WriteValue(TEXT("existing_map_validation_only"),bExisting);
			W->WriteValue(TEXT("source_map"),SourceMap);W->WriteValue(TEXT("target_map"),TargetMap);W->WriteValue(TEXT("source_file_sha1"),SourceFileHash);
			W->WriteValue(TEXT("source_meshes"),Baseline.Meshes);W->WriteValue(TEXT("source_whitebox_meshes"),Baseline.WhiteboxMeshes);
			W->WriteValue(TEXT("target_meshes"),TargetGeometry.Meshes);W->WriteValue(TEXT("source_geometry_sha1"),Baseline.Hash);W->WriteValue(TEXT("target_geometry_sha1"),TargetGeometry.Hash);
			W->WriteArrayStart(TEXT("non_baked_additional_components"));for(const FString& Path:TargetGeometry.NonBakedAdditions)W->WriteValue(Path);W->WriteArrayEnd();
			W->WriteValue(TEXT("probe_count"),ProbeCount);W->WriteValue(TEXT("selected_wave_pcm_sha1"),IMSelectedPCMHash);
			W->WriteValue(TEXT("sound_asset"),ActualSoundPath);W->WriteValue(TEXT("wave_asset"),ActualWavePath);
			W->WriteValue(TEXT("selected_wave_frames"),IMSelectedFrames);W->WriteValue(TEXT("source_contract"),TEXT("full-waterdrop-stereo-metasound-v2"));
			if(Volume.IsValid()&&Volume->BakedField){W->WriteValue(TEXT("bake_asset"),Volume->BakedField->GetPathName());W->WriteValue(TEXT("world_package"),Volume->BakedField->WorldPackage);W->WriteValue(TEXT("scene_fingerprint"),Volume->BakedField->SceneFingerprint);}
			W->WriteObjectEnd();W->Close();
			if(!FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Evidence,TEXT("prepare-result.json"))))
			{Success=false;Test->AddError(TEXT("Could not persist audition preparation receipt."));}
		}
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticPrepareAudition %s evidence=%s"),*Message,*Evidence);
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticPrepareAudition %s"),Success?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));return true;
	}
	FAutomationTestBase* Test;double Started;int32 Stage=0,ProbeCount=0;bool bCreatedMap=false,bExisting=false;
	FString SourceFile,SourceFileHash,OriginalBakePath,Evidence,ActualSoundPath,ActualWavePath;
	FIMGeometryIdentity Baseline,TargetGeometry;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPrepareAudition,"IceMoon.AcousticField.W3.PrepareAudition",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticPrepareAudition::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticAuditionSetupTestPrivate::FIMAuditionPrepareCommand(this));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticAuditionSourceContract,
	"IceMoon.AcousticField.Audition.SelectedSourceContract",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticAuditionSourceContract::RunTest(const FString&)
{
	// Native document copies supply counterexamples without changing an asset
	// or a live audio operator. This validates fixture identity, not hearing.
	FString Error;USoundWave* Wave=nullptr;
	auto* Sound=LoadObject<UMetaSoundSource>(nullptr,*IMAcousticAuditionSetupTestPrivate::ObjectPath(IMAcousticAuditionSetupTestPrivate::IMMetaSoundPackage));
	if(!IMAcousticAuditionSetupTestPrivate::IMVerifyAuditionSound(Sound,Wave,Error)){AddError(Error);return false;}
	UE_LOG(LogTemp,Display,TEXT("IMLogs AuditionSelectedWave compression=%d virtualization=%d frames=%d imported_pcm_sha1=%s"),
		static_cast<int32>(Wave->GetSoundAssetCompressionTypeEnum()),static_cast<int32>(Wave->VirtualizationMode),IMAcousticAuditionSetupTestPrivate::IMSelectedFrames,IMAcousticAuditionSetupTestPrivate::IMSelectedPCMHash);
	auto* Legacy=LoadObject<USoundWave>(nullptr,
		TEXT("/IceMoonAcousticField/Tests/IM_V2AuditionReference.IM_V2AuditionReference"));
	if(!Legacy){AddError(TEXT("Legacy stimulus is needed for the source identity counterexample."));return false;}
	Error.Empty();
	if(IMAcousticAuditionSetupTestPrivate::IMVerifyAuditionSound(Legacy,Wave,Error))
	{AddError(TEXT("Legacy fixed stimulus was incorrectly accepted as ordinary water-drop MetaSound."));return false;}
	const auto* Document=Cast<IMetaSoundDocumentInterface>(Sound);
	const auto& Original=Document->GetConstDocument().RootGraph.GetConstDefaultGraph();
	for(const TCHAR* PinName:{TEXT("Wave Asset"),TEXT("Loop"),TEXT("Loop Duration")})
	{
		FMetasoundFrontendGraph Counterexample=Original;
		auto* Player=Counterexample.Nodes.FindByPredicate([](const auto& N){return N.Name==TEXT("Wave Player");});
		const auto* Pin=Player?Player->Interface.Inputs.FindByPredicate([&](const auto& V){return V.Name==PinName;}):nullptr;
		auto* Literal=Pin?Player->InputLiterals.FindByPredicate([&](const auto& V){return V.VertexID==Pin->VertexID;}):nullptr;
		if(!Literal){AddError(TEXT("Counterexample input default is missing."));return false;}
		if(FCString::Strcmp(PinName,TEXT("Wave Asset"))==0)Literal->Value.Set(static_cast<UObject*>(Legacy));
		else if(FCString::Strcmp(PinName,TEXT("Loop"))==0)Literal->Value.Set(false);
		else Literal->Value.Set(1.f);
		Error.Empty();
		if(IMAcousticAuditionSetupTestPrivate::IMVerifyAuditionGraph(Counterexample,Wave,Error))
		{AddError(FString::Printf(TEXT("Invalid %s default passed ordinary source validation."),PinName));return false;}
	}
	UE_LOG(LogTemp,Display,TEXT("IMLogs AuditionSelectedSourceContract full_source=1 rejected_legacy=1 rejected_wave_loop_duration_mutants=3"));
	UE_LOG(LogTemp,Display,TEXT("IMExitEditor PASS AuditionSelectedSourceContract"));
	return true;
}
#endif
