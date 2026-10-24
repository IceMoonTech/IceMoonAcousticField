#if WITH_EDITOR
#include "IMAcousticFootstepCharacter.h"
#include "Tests/IMAcousticAuditionCapture.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "HAL/IConsoleManager.h"
#include "JsonObjectConverter.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/DataValidation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Sound/SoundWave.h"

DEFINE_LOG_CATEGORY_STATIC(LogIMAcousticPresence, Log, All);

namespace IMAcousticPresencePreviewPrivate
{
const TCHAR* PhysicalPath = TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstep.MS_IMFootstep");
const TCHAR* PresencePackage = TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstepPresenceNearfield");

FString EvidencePath(const TCHAR* Name)
{
	return FPaths::Combine(IMAcousticAuditionCapture::OutputDirectory(), Name);
}

bool SaveDocument(UMetaSoundSource* Source, const TCHAR* Name)
{
	FString Document;
	return Source && FJsonObjectConverter::UStructToJsonObjectString(Source->GetConstDocument(), Document)
		&& FFileHelper::SaveStringToFile(Document, *EvidencePath(Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void BuildPreview()
{
	check(IsInGameThread());
	UMetaSoundSource* Physical = LoadObject<UMetaSoundSource>(nullptr, PhysicalPath);
	if (!Physical || GEditor->PlayWorld)
	{
		UE_LOG(LogIMAcousticPresence, Error, TEXT("Presence graph requires the saved physical source and no active Play."));
		return;
	}
	const FString AssetFile = FPackageName::LongPackageNameToFilename(PresencePackage, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(AssetFile))
	{
		UE_LOG(LogIMAcousticPresence, Error, TEXT("Presence candidate already exists; refusing destructive rebuild."));
		return;
	}
	if (!SaveDocument(Physical, TEXT("IMPhysicalGraphBefore.json"))) return;
	const FString DraftPackageName = FString(PresencePackage) + TEXT("Draft");
	if (FPaths::FileExists(FPackageName::LongPackageNameToFilename(DraftPackageName, FPackageName::GetAssetPackageExtension()))) return;
	UPackage* DraftPackage = CreatePackage(*DraftPackageName);
	UMetaSoundSource* Candidate = DuplicateObject<UMetaSoundSource>(Physical, DraftPackage, TEXT("MS_IMFootstepPresenceNearfieldDraft"));
	Candidate->SetFlags(RF_Public | RF_Standalone);
	TScriptInterface<IMetaSoundDocumentInterface> Interface = Candidate;
	UMetaSoundSourceBuilder* Builder = NewObject<UMetaSoundSourceBuilder>();
	Builder->Initialize(Interface);
	const auto& Graph = Candidate->GetConstDocument().RootGraph.GetConstDefaultGraph();
	const FMetasoundFrontendNode* Spatial = Graph.Nodes.FindByPredicate([](const auto& Node)
		{ return Node.Name == TEXT("Acoustic Source"); });
	if (!Spatial) return;
	const auto* MonoInput = Spatial->Interface.Inputs.FindByPredicate([](const auto& Pin) { return Pin.Name == TEXT("Mono"); });
	if (!MonoInput) return;
	const auto* OriginalEdge = Graph.Edges.FindByPredicate([&](const auto& Edge)
		{ return Edge.ToNodeID == Spatial->GetID() && Edge.ToVertexID == MonoInput->VertexID; });
	if (!OriginalEdge) return;
	const FMetaSoundBuilderNodeOutputHandle OriginalOutput(OriginalEdge->FromNodeID, OriginalEdge->FromVertexID);
	const FMetaSoundBuilderNodeInputHandle SpatialInput(Spatial->GetID(), MonoInput->VertexID);
	FMetaSoundBuilderNodeOutputHandle SpatialLeft, SpatialRight;
	TArray<TPair<FName, FMetaSoundBuilderNodeInputHandle>> DirectDestinations;
	for (FName Name : {FName(TEXT("Left")), FName(TEXT("Right"))})
	{
		const auto* Pin = Spatial->Interface.Outputs.FindByPredicate([&](const auto& Vertex) { return Vertex.Name == Name; });
		if (!Pin) return;
		const FMetaSoundBuilderNodeOutputHandle Handle(Spatial->GetID(), Pin->VertexID);
		if (Name == TEXT("Left")) SpatialLeft = Handle; else SpatialRight = Handle;
		for (const auto& Edge : Graph.Edges)
			if (Edge.FromNodeID == Spatial->GetID() && Edge.FromVertexID == Pin->VertexID)
				DirectDestinations.Emplace(Name, FMetaSoundBuilderNodeInputHandle(Edge.ToNodeID, Edge.ToVertexID));
	}
	EMetaSoundBuilderResult Result;
	bool Good = true;
	const auto Presence = Builder->AddNodeByClassName({TEXT("IM"), TEXT("Acoustic Presence"), TEXT("Mono")}, Result, 2);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	auto Connect = [&](const FMetaSoundBuilderNodeOutputHandle& Output, const FMetaSoundNodeHandle& Node, const TCHAR* Name)
	{
		const auto Input = Builder->FindNodeInputByName(Node, Name, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		Builder->ConnectNodes(Output, Input, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
	};
	Builder->DisconnectNodeInput(SpatialInput, Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	Connect(OriginalOutput, Presence, TEXT("Mono"));
	const FMetasoundFrontendNode* Player = Graph.Nodes.FindByPredicate([](const auto& Node) { return Node.Name == TEXT("Wave Player"); });
	if (!Player) return;
	const auto* PlayerOutput = Player->Interface.Outputs.FindByPredicate([](const auto& Pin) { return Pin.Name == TEXT("Out Mono"); });
	if (!PlayerOutput) return;
	Connect(FMetaSoundBuilderNodeOutputHandle(Player->GetID(), PlayerOutput->VertexID), Presence, TEXT("Wave/Pitch"));
	FName ContactType;
	FMetaSoundBuilderNodeOutputHandle Contact;
	Builder->FindGraphInputNode(TEXT("Footstep Contact"), ContactType, Contact, Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	Connect(Contact, Presence, TEXT("Contact"));
	for (const auto& Pair : {TPair<FName, FName>(TEXT("Presence Enabled"), TEXT("Enabled")),
		TPair<FName, FName>(TEXT("Presence Impact"), TEXT("Impact")),
		TPair<FName, FName>(TEXT("Presence Material"), TEXT("Material")),
		TPair<FName, FName>(TEXT("Presence Equal Loudness"), TEXT("Equal Loudness")),
		TPair<FName, FName>(TEXT("Presence Compensation"), TEXT("Compensation")),
		TPair<FName, FName>(TEXT("Presence Enclosure"), TEXT("Enclosure"))})
	{
		FMetasoundFrontendLiteral Default;
		Default.Set(Pair.Key == TEXT("Presence Enabled") || Pair.Key == TEXT("Presence Compensation") ? 1.f : 0.f);
		const auto Input = Builder->AddGraphInputNode(Pair.Key, TEXT("Float"), Default, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		Connect(Input, Presence, *Pair.Value.ToString());
	}
	const auto Output = Builder->FindNodeOutputByName(Presence, TEXT("Out"), Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	Builder->ConnectNodes(Output, SpatialInput, Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	const auto Nearfield = Builder->AddNodeByClassName({TEXT("IM"), TEXT("Acoustic Self Nearfield"), TEXT("Stereo")}, Result, 1);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	Connect(Output, Nearfield, TEXT("Mono"));
	Connect(SpatialLeft, Nearfield, TEXT("Spatial Left"));
	Connect(SpatialRight, Nearfield, TEXT("Spatial Right"));
	for (const auto& Pair : {TPair<FName, FName>(TEXT("Presence Near Field"), TEXT("Enabled")),
		TPair<FName, FName>(TEXT("Presence Pan"), TEXT("Pan"))})
	{
		FMetasoundFrontendLiteral Default;
		Default.Set(0.f);
		const auto Input = Builder->AddGraphInputNode(Pair.Key, TEXT("Float"), Default, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		Connect(Input, Nearfield, *Pair.Value.ToString());
	}
	for (const auto& Destination : DirectDestinations)
	{
		const auto NearOutput = Builder->FindNodeOutputByName(Nearfield, Destination.Key, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		Builder->DisconnectNodeInput(Destination.Value, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		Builder->ConnectNodes(NearOutput, Destination.Value, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
	}
	if (!Good)
	{
		UE_LOG(LogIMAcousticPresence, Error, TEXT("Presence incremental graph patch failed; candidate not saved."));
		return;
	}
	auto Built = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()->BuildToAsset(Builder, TEXT("IceMoon"),
		TEXT("MS_IMFootstepPresenceNearfield"), FPackageName::GetLongPackagePath(PresencePackage), Result, Physical);
	Candidate = Cast<UMetaSoundSource>(Built.GetObject());
	if (Result != EMetaSoundBuilderResult::Succeeded || !Candidate) return;
	Candidate->QualitySetting = Physical->QualitySetting;
	Candidate->QualitySettingGuid = Physical->QualitySettingGuid;
	Candidate->BlockRateOverride = Physical->BlockRateOverride;
	Candidate->SampleRateOverride = Physical->SampleRateOverride;
	UPackage* Package = Candidate->GetOutermost();
	FDataValidationContext Validation;
	if (Candidate->IsDataValid(Validation) != EDataValidationResult::Valid)
	{
		UE_LOG(LogIMAcousticPresence, Error, TEXT("Presence MetaSound validation failed; candidate not saved."));
		return;
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	Candidate->MarkPackageDirty();
	if (!UPackage::SavePackage(Package, Candidate, *AssetFile, SaveArgs)
		|| !SaveDocument(Candidate, TEXT("IMPresenceGraphSaved.json"))) return;
	Builder = nullptr;
	Interface = nullptr;
	Built = nullptr;
	Candidate = nullptr;
	FText UnloadError;
	if (!UPackageTools::UnloadPackages({Package, DraftPackage}, UnloadError, true))
	{
		UE_LOG(LogIMAcousticPresence, Error, TEXT("Presence package reopen failed: %s"), *UnloadError.ToString());
		return;
	}
	const FString ObjectPath = FString(PresencePackage) + TEXT(".MS_IMFootstepPresenceNearfield");
	UMetaSoundSource* Reopened = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	if (!Reopened || !SaveDocument(Reopened, TEXT("IMPresenceGraphReopened.json"))
		|| !SaveDocument(Physical, TEXT("IMPhysicalGraphAfter.json"))) return;
	FFileHelper::SaveStringToFile(TEXT("saved-reopened"), *EvidencePath(TEXT("IMGraphReady.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogIMAcousticPresence, Display, TEXT("IMLogs PRESENCE_GRAPH_READY B=toggle source=%s"), *Reopened->GetPathName());
}

void ExportSourceWaves()
{
	auto Report = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Rows;
	TSet<USoundWave*> Waves;
	for (TActorIterator<AIMAcousticFootstepCharacter> Actor(GEditor->GetEditorWorldContext().World()); Actor; ++Actor)
	{
		for (auto Wave : Actor->WoodSteps) Waves.Add(Wave.Get());
		for (auto Wave : Actor->StoneSteps) Waves.Add(Wave.Get());
	}
	for (const TCHAR* Material : {TEXT("Wood"), TEXT("Stone")})
		for (const TCHAR* Variant : {TEXT("A"), TEXT("B")})
		{
			const FString Name = FString::Printf(TEXT("SW_IMFootstepPresence%s%s"), Material, Variant);
			Waves.Add(LoadObject<USoundWave>(nullptr, *(TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/") + Name + TEXT(".") + Name)));
		}
	for (auto* Wave : Waves)
	{
		TArray<uint8> PCM;
		uint32 Rate = 0;
		uint16 Channels = 0;
		if (!Wave || !Wave->GetImportedSoundWaveData(PCM, Rate, Channels)) return;
		const FString File = EvidencePath(*(Wave->GetName() + TEXT(".s16")));
		if (!FFileHelper::SaveArrayToFile(PCM, *File)) return;
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset"), Wave->GetPathName());
		Row->SetStringField(TEXT("file"), File);
		Row->SetNumberField(TEXT("sample_rate"), Rate);
		Row->SetNumberField(TEXT("channels"), Channels);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("waves"), Rows);
	FString Text;
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text));
	FFileHelper::SaveStringToFile(Text, *EvidencePath(TEXT("IMSourceWaves.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FAutoConsoleCommand BuildCommand(TEXT("IM.AcousticField.BuildListeningCorrections"), TEXT("Create and reopen the isolated equal-loudness audition graph."), FConsoleCommandDelegate::CreateStatic(&BuildPreview));
FAutoConsoleCommand ExportCommand(TEXT("IM.AcousticField.ExportListeningSources"), TEXT("Export the current native imported PCM for spectrum diagnosis."), FConsoleCommandDelegate::CreateStatic(&ExportSourceWaves));

FAutoConsoleCommandWithWorldAndArgs ModeCommand(TEXT("IM.AcousticField.PresenceMode"), TEXT("0 physical, 1 exaggerated; B uses the same live setter."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.Num() != 1) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor)
			Actor->SetExaggeratedPresence(Arguments[0] == TEXT("1"));
	}));

FAutoConsoleCommandWithWorldAndArgs SamplesCommand(TEXT("IM.AcousticField.PresenceSamples"), TEXT("0 recorded footstep waves (default), 1 synthesized SW_IMFootstepPresence waves; exaggerated pass only."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.Num() != 1) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor)
			Actor->SetSynthesizedPresenceSamples(Arguments[0] == TEXT("1"));
	}));

FAutoConsoleCommandWithWorldAndArgs EqualCommand(TEXT("IM.AcousticField.EqualLoudness"), TEXT("0 original loudness, 1 measured equal loudness; N uses this setter."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.Num() != 1) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor) Actor->SetEqualLoudness(Arguments[0] == TEXT("1"));
	}));

FAutoConsoleCommandWithWorldAndArgs KeyCommand(TEXT("IM.AcousticField.PresenceKey"), TEXT("B/N press/release through the owned player's native input bindings."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.Num() != 2 || (Arguments[0] != TEXT("B") && Arguments[0] != TEXT("N"))) return;
		if (auto* Controller = World->GetFirstPlayerController())
		{
			const bool Pressed = Arguments[1] == TEXT("press");
			Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Arguments[0] == TEXT("B") ? EKeys::B : EKeys::N,
				Pressed ? IE_Pressed : IE_Released, Pressed ? 1.f : 0.f));
		}
	}));

uint64 ParameterSequence = 0;
FAutoConsoleCommandWithWorldAndArgs ParameterCommand(TEXT("IM.AcousticField.PresenceParameters"), TEXT("Export the actual owned audio component parameter cache through its public C++ interface."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		check(IsInGameThread());
		if (!World || !Arguments.IsEmpty()) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor)
		{
			auto* Audio = Actor->GetFootstepAudio();
			if (!Audio) return;
			auto Values = MakeShared<FJsonObject>();
			for (const FAudioParameter& Parameter : Audio->GetInstanceParameters())
			{
				const FString Name = Parameter.ParamName.ToString();
				if (Name.StartsWith(TEXT("Presence "))) Values->SetNumberField(Name, Parameter.FloatParam);
			}
			auto Report = MakeShared<FJsonObject>();
			Report->SetObjectField(TEXT("parameters"), Values);
			Report->SetStringField(TEXT("component"), Audio->GetPathName());
			Report->SetStringField(TEXT("world"), World->GetPathName());
			Report->SetNumberField(TEXT("sequence"), ++ParameterSequence);
			FString Text;
			if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text))
				|| !FFileHelper::SaveStringToFile(Text, *EvidencePath(TEXT("IMAudioParameters.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				UE_LOG(LogIMAcousticPresence, Error, TEXT("Owned audio parameter export failed."));
			return;
		}
	}));

FAutoConsoleCommandWithWorldAndArgs PassCommand(TEXT("IM.AcousticField.ListeningPass"), TEXT("record/replay label, pause, end: capture the same canonical gameplay sequence."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.IsEmpty()) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor)
		{
			if (Arguments[0] == TEXT("prepare")) { Actor->SetAutoWalking(false); Actor->bAutoLandingDemo = false; }
			else if (Arguments[0] == TEXT("pause")) Actor->PauseAuditionPass();
			else if (Arguments[0] == TEXT("end"))
			{
				Actor->EndAuditionPass();
				if (!IMAcousticAuditionCapture::End()) UE_LOG(LogIMAcousticPresence, Error, TEXT("Listening stage capture failed."));
			}
			else if (Arguments.Num() == 2 && IMAcousticAuditionCapture::Begin(Arguments[1]))
				Actor->BeginAuditionPass(Arguments[0] == TEXT("record"), Arguments[1]);
		}
	}));

#if WITH_DEV_AUTOMATION_TESTS
class FIMAcousticPresencePythonStart final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticPresencePythonStart(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
			const FString Command = FString::Printf(TEXT("py %s"), *EvidencePath(TEXT("IMSpatialEntry.py")));
		Test->TestTrue(TEXT("Python preview entry dispatched"), GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command));
		return true;
	}
private:
	FAutomationTestBase* Test;
};

class FIMAcousticPresencePreviewAwait final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticPresencePreviewAwait(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
		const FString Path = EvidencePath(TEXT("IMPreviewRuntime.json"));
		if (!FPaths::FileExists(Path)) return false;
		FString Text;
		TSharedPtr<FJsonObject> Report;
		const bool Parsed = FFileHelper::LoadFileToString(Text, *Path)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Report) && Report.IsValid();
		Test->TestTrue(TEXT("One-Play preview receipt is valid"), Parsed);
		if (Parsed) Test->TestTrue(TEXT("Both recording modes and ordinary Stop completed"), Report->GetBoolField(TEXT("success")));
		return true;
	}
private:
	FAutomationTestBase* Test;
};
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPresencePreviewTest, "IceMoon.AcousticField.Presence.Corrections.Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticPresencePreviewTest::RunTest(const FString&)
{
	using namespace IMAcousticPresencePreviewPrivate;
	if (!TestTrue(TEXT("No stale preview result is consumed"), !FPaths::FileExists(EvidencePath(TEXT("IMPreviewRuntime.json"))))) return false;
	// The preview itself is driven by IMSpatialEntry.py, a harness script that lives in the evidence folder and is not part of the plugin. Without it
	// the latent wait below never ends, so a missing script fails the test at once instead of hanging the whole automation run.
	if (!TestTrue(TEXT("Preview entry script IMSpatialEntry.py exists in the evidence folder"), FPaths::FileExists(EvidencePath(TEXT("IMSpatialEntry.py"))))) return false;
	if (!TestTrue(TEXT("Saved audition map opens"), FEditorFileUtils::LoadMap(TEXT("/IceMoonAcousticField/Tests/IM_FootstepAudition"), false, true))) return false;
	ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticPresencePythonStart(this));
	ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticPresencePreviewAwait(this));
	return true;
}
#endif
#endif
