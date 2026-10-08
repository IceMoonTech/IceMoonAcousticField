#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundOperatorSettings.h"
#include "MetasoundSource.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/Engine.h"

namespace IMAcousticMetaSoundGraphReadbackTestPrivate
{
constexpr const TCHAR* NativeGraphReadbackMetaSound = TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");
constexpr int32 ExpectedSampleRate = 48000;

constexpr EPropertyFlags SkipPropertyFlags = CPF_Transient | CPF_DuplicateTransient | CPF_TextExportTransient;

TSharedRef<FJsonObject> StructObject(const UStruct* Struct, const void* Data)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Struct || !Data || !FJsonObjectConverter::UStructToJsonObject(Struct, Data, Result, 0, SkipPropertyFlags))
	{
		Result->SetStringField(TEXT("_serialization_status"), TEXT("FAIL"));
		return Result;
	}
	Result->SetStringField(TEXT("_serialization_status"), TEXT("PASS"));
	return Result;
}

template <typename StructType>
TSharedRef<FJsonObject> StructObject(const StructType& Value)
{
	return StructObject(StructType::StaticStruct(), &Value);
}

template <typename StructType>
TArray<TSharedPtr<FJsonValue>> StructArray(const TArray<StructType>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const StructType& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueObject>(StructObject(Value)));
	}
	return Result;
}

TSharedRef<FJsonObject> GraphPageObject(const FMetasoundFrontendGraph& Page)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("page_id"), Page.PageID.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetNumberField(TEXT("node_count"), Page.Nodes.Num());
	Result->SetNumberField(TEXT("edge_count"), Page.Edges.Num());
	Result->SetArrayField(TEXT("nodes"), StructArray(Page.Nodes));
	Result->SetArrayField(TEXT("edges"), StructArray(Page.Edges));
	Result->SetArrayField(TEXT("variables"), StructArray(Page.Variables));
#if WITH_EDITORONLY_DATA
	Result->SetObjectField(TEXT("style"), StructObject(Page.Style));
#endif
	return Result;
}

FString SerializeJson(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return FString();
	}
	return Text;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticMetaSoundNativeGraphReadback,
	"IceMoon.AcousticField.MetaSound.NativeGraphReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace IMAcousticMetaSoundGraphReadbackTestPrivate
{
bool ReadAcousticGraph(FAutomationTestBase& Test, const TCHAR* AssetPath, const TCHAR* OutputName)
{
	UMetaSoundSource* Source = LoadObject<UMetaSoundSource>(nullptr, AssetPath);
	if (!Source)
	{
		Test.AddError(FString::Printf(TEXT("MetaSound asset was not found: %s"), AssetPath));
		return false;
	}

	const IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(Source);
	if (!DocumentInterface)
	{
		Test.AddError(TEXT("MetaSoundSource does not expose IMetaSoundDocumentInterface."));
		return false;
	}

	const FMetasoundFrontendDocument& Document = DocumentInterface->GetConstDocument();
	const FMetasoundFrontendGraph& RootGraph = Document.RootGraph.GetConstDefaultGraph();
	if (RootGraph.Nodes.IsEmpty() || RootGraph.Edges.IsEmpty())
	{
		Test.AddError(FString::Printf(TEXT("Native graph is empty: nodes=%d edges=%d."), RootGraph.Nodes.Num(), RootGraph.Edges.Num()));
		return false;
	}

	const Metasound::FOperatorSettings OperatorSettings = Source->GetOperatorSettings(ExpectedSampleRate);
	const FString EvidenceDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundWaterDrop"));
	IFileManager::Get().MakeDirectory(*EvidenceDirectory, true);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("IMMetaSoundGraphDoc.v1"));
	Root->SetStringField(TEXT("status"), TEXT("PASS_NATIVE_DOCUMENT_READBACK"));
	Root->SetStringField(TEXT("asset_path"), Source->GetPathName());
	Root->SetStringField(TEXT("asset_class"), Source->GetClass()->GetPathName());
	const TCHAR* OutputFormat = Source->OutputFormat == EMetaSoundOutputAudioFormat::Mono
		? TEXT("Mono")
		: (Source->OutputFormat == EMetaSoundOutputAudioFormat::Stereo ? TEXT("Stereo") : TEXT("Other"));
	Root->SetStringField(TEXT("output_format"), OutputFormat);
	Root->SetNumberField(TEXT("num_channels"), Source->NumChannels);
	Root->SetNumberField(TEXT("document_interface_count"), Document.Interfaces.Num());
	Root->SetNumberField(TEXT("root_node_count"), RootGraph.Nodes.Num());
	Root->SetNumberField(TEXT("root_edge_count"), RootGraph.Edges.Num());

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	Interfaces.Reserve(Document.Interfaces.Num());
	for (const FMetasoundFrontendVersion& Interface : Document.Interfaces)
	{
		Interfaces.Add(MakeShared<FJsonValueString>(Interface.ToString()));
	}
	Root->SetArrayField(TEXT("interfaces"), Interfaces);
	Root->SetObjectField(TEXT("root_graph"), GraphPageObject(RootGraph));
	Root->SetArrayField(TEXT("dependencies"), StructArray(Document.Dependencies));
	Root->SetObjectField(TEXT("root_graph_interface"), StructObject(Document.RootGraph.GetDefaultInterface()));

	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetNumberField(TEXT("requested_sample_rate"), ExpectedSampleRate);
	Settings->SetNumberField(TEXT("operator_sample_rate"), OperatorSettings.GetSampleRate());
	Settings->SetNumberField(TEXT("operator_num_frames_per_block"), OperatorSettings.GetNumFramesPerBlock());
	Settings->SetNumberField(TEXT("operator_actual_block_rate"), OperatorSettings.GetActualBlockRate());
	Root->SetObjectField(TEXT("operator_settings"), Settings);

	const FString Json = SerializeJson(Root);
	const FString EvidencePath = FPaths::Combine(EvidenceDirectory, OutputName);
	if (Json.IsEmpty() || !FFileHelper::SaveStringToFile(Json, *EvidencePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Test.AddError(FString::Printf(TEXT("Failed to save native graph evidence: %s"), *EvidencePath));
		return false;
	}

	UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundNativeGraphReadback PASS asset=%s nodes=%d edges=%d interfaces=%d sample_rate=%g block_frames=%d block_rate=%g evidence=%s"),
		AssetPath,
		RootGraph.Nodes.Num(),
		RootGraph.Edges.Num(),
		Document.Interfaces.Num(),
		OperatorSettings.GetSampleRate(),
		OperatorSettings.GetNumFramesPerBlock(),
		OperatorSettings.GetActualBlockRate(),
		*EvidencePath);
	if (Source->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || Source->NumChannels != 2 || OperatorSettings.GetNumFramesPerBlock() != 512)
	{
		Test.AddError(TEXT("Acoustic graph requires matching stereo UObject format and 512-frame operator settings."));
		return false;
	}
	// UObject output-format conformance can recreate interface output nodes.
	// Require the persistent graph to retain both DSP-to-output connections.
	const FMetasoundFrontendNode* DSP = RootGraph.Nodes.FindByPredicate([](const auto& N)
		{ return N.Name == TEXT("Acoustic Source") || N.Name == TEXT("Acoustic Environment"); });
	if (!DSP) { Test.AddError(TEXT("Acoustic DSP node is missing.")); return false; }
	for (int32 Channel = 0; Channel < 2; ++Channel)
	{
		const FName OutputNodeName(*FString::Printf(TEXT("UE.OutputFormat.Stereo.Audio:%d"), Channel));
		const auto* Output = RootGraph.Nodes.FindByPredicate([&](const auto& N) { return N.Name == OutputNodeName; });
		const FName PinName = Channel == 0 ? TEXT("Left") : TEXT("Right");
		const auto* Pin = DSP->Interface.Outputs.FindByPredicate([&](const auto& V) { return V.Name == PinName; });
		if (!Output || !Pin || !RootGraph.Edges.ContainsByPredicate([&](const auto& E)
			{ return E.FromNodeID == DSP->GetID() && E.FromVertexID == Pin->VertexID && E.ToNodeID == Output->GetID(); }))
		{ Test.AddError(TEXT("Acoustic DSP stereo output edge is missing.")); return false; }
	}
	return true;
}
}


bool FIMAcousticMetaSoundNativeGraphReadback::RunTest(const FString&)
{
	const bool Source = IMAcousticMetaSoundGraphReadbackTestPrivate::ReadAcousticGraph(*this, IMAcousticMetaSoundGraphReadbackTestPrivate::NativeGraphReadbackMetaSound, TEXT("native-graph-readback.json"));
	const bool Environment = IMAcousticMetaSoundGraphReadbackTestPrivate::ReadAcousticGraph(*this, TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticEnvironment"), TEXT("environment-graph-readback.json"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound native graph readback"), Source && Environment ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound native graph readback %s"), Source && Environment ? TEXT("PASS") : TEXT("FAIL"));
	return Source && Environment;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticMetaSoundAuthor, "IceMoon.AcousticField.MetaSound.AuthorClosure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticMetaSoundAuthor::RunTest(const FString&)
{
	const FString Script = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexRuntime/disposable-project/IM_AuthorWaterDropMetaSound.py")));
	const FString Receipt = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundClosure/authoring.json"));
	const FDateTime Before = IFileManager::Get().GetTimeStamp(*Receipt);
	const bool Handled = GEngine->Exec(nullptr, *FString::Printf(TEXT("py \"%s\""), *Script));
	const bool Saved = IFileManager::Get().GetTimeStamp(*Receipt) > Before;
	TestTrue(TEXT("Native Python authoring completed"), Handled && Saved);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound authoring"), Handled && Saved ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound authoring %s"), Handled && Saved ? TEXT("PASS") : TEXT("FAIL"));
	return Handled && Saved;
}

#endif
