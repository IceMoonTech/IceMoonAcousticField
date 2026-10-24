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
#include "Editor.h"
#include "Engine/World.h"
#include "MetasoundAssetManager.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundFrontendTransform.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

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

#if WITH_EDITORONLY_DATA
namespace IMAcousticMetaSoundGraphReadbackTestPrivate
{
constexpr TCHAR IMQ5FinishObject[] = TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticQ5Impulse.MS_AcousticQ5Impulse");
constexpr TCHAR IMQ5FinishOriginalSha1[] = TEXT("0fd9ec8da4705c72bb6a2ba7baf8f60daacaf991");

template <typename T>
bool IMQ5FinishSameStruct(const T& A, const T& B)
{
	const auto JA = StructObject(A);
	const auto JB = StructObject(B);
	return JA->GetStringField(TEXT("_serialization_status")) == TEXT("PASS")
		&& JB->GetStringField(TEXT("_serialization_status")) == TEXT("PASS")
		&& SerializeJson(JA) == SerializeJson(JB);
}

const FMetasoundFrontendNode* IMQ5FinishNode(const FMetasoundFrontendDocument& D, const FGuid& Id)
{
	return D.RootGraph.GetConstDefaultGraph().Nodes.FindByPredicate([&](const auto& N) { return N.GetID() == Id; });
}

const FMetasoundFrontendVertex* IMQ5FinishMapVertex(const FMetasoundFrontendDocument& Before,
	const FMetasoundFrontendDocument& After, const FGuid& Node, const FGuid& Vertex, bool bInput)
{
	const auto* OldNode = IMQ5FinishNode(Before, Node);
	const auto* NewNode = IMQ5FinishNode(After, Node);
	if (!OldNode || !NewNode) return nullptr;
	const auto& OldPins = bInput ? OldNode->Interface.Inputs : OldNode->Interface.Outputs;
	const auto& NewPins = bInput ? NewNode->Interface.Inputs : NewNode->Interface.Outputs;
	const auto* OldPin = OldPins.FindByPredicate([&](const auto& P) { return P.VertexID == Vertex; });
	if (!OldPin) return nullptr;
	const auto Matches = NewPins.FilterByPredicate([&](const auto& P) { return P.Name == OldPin->Name && P.TypeName == OldPin->TypeName; });
	return Matches.Num() == 1 ? NewPins.FindByPredicate([&](const auto& P) { return P.Name == OldPin->Name && P.TypeName == OldPin->TypeName; }) : nullptr;
}

bool IMQ5FinishPreserved(const FMetasoundFrontendDocument& Before, const FMetasoundFrontendDocument& After,
	const FGuid& SourceId, const FMetasoundFrontendEdge* Added, FString& Error)
{
	auto Fail = [&](const TCHAR* Why) { Error = Why; return false; };
	if (Before.RootGraph.GetConstGraphPages().Num() != 1 || After.RootGraph.GetConstGraphPages().Num() != 1)
		return Fail(TEXT("Q5 fixture must retain its one graph page."));
	const auto& BG = Before.RootGraph.GetConstDefaultGraph();
	const auto& AG = After.RootGraph.GetConstDefaultGraph();
	if (BG.PageID != AG.PageID || BG.Nodes.Num() != AG.Nodes.Num() || AG.Edges.Num() != BG.Edges.Num() + (Added ? 1 : 0))
		return Fail(TEXT("Page, node count or exact edge delta differs."));
	const auto* BS = IMQ5FinishNode(Before, SourceId);
	const auto* AS = IMQ5FinishNode(After, SourceId);
	if (!BS || !AS || AS->Interface.Inputs.Num() != BS->Interface.Inputs.Num() + 1
		|| AS->Interface.Outputs.Num() != BS->Interface.Outputs.Num()) return Fail(TEXT("Source interface delta differs."));
	const auto* BC = Before.Dependencies.FindByPredicate([&](const auto& C) { return C.ID == BS->ClassID; });
	const auto* AC = After.Dependencies.FindByPredicate([&](const auto& C) { return C.ID == AS->ClassID; });
	if (!BC || !AC || BC->Metadata.GetClassName().Namespace != TEXT("IM")
		|| BC->Metadata.GetClassName().Name != TEXT("Acoustic Source") || BC->Metadata.GetClassName().Variant != TEXT("Stereo")
		|| AC->Metadata.GetClassName() != BC->Metadata.GetClassName()
		|| BC->Metadata.GetVersion().Major != 1 || BC->Metadata.GetVersion().Minor != 0
		|| AC->Metadata.GetVersion().Major != 1 || AC->Metadata.GetVersion().Minor != 1)
		return Fail(TEXT("Only the pinned Source 1.0 to1.1 transition is allowed."));
	const auto* Finished = AS->Interface.Inputs.FindByPredicate([](const auto& P) { return P.Name == TEXT("Finished") && P.TypeName == TEXT("Trigger"); });
	if (!Finished) return Fail(TEXT("Finished Trigger is absent."));
	for (const auto& OldNode : BG.Nodes)
	{
		const auto* NewNode = IMQ5FinishNode(After, OldNode.GetID());
		if (!NewNode) return Fail(TEXT("A persistent node disappeared."));
		if (OldNode.GetID() != SourceId)
		{
			if (!IMQ5FinishSameStruct(OldNode, *NewNode)) return Fail(TEXT("A non-target node changed."));
			continue;
		}
		FMetasoundFrontendNode Normalized = *NewNode;
		Normalized.ClassID = OldNode.ClassID;
		Normalized.Style.bMessageNodeUpdated = OldNode.Style.bMessageNodeUpdated;
		Normalized.Interface.Inputs.Reset();
		Normalized.Interface.Outputs.Reset();
		for (bool bInput : {true, false})
		{
			const auto& OldPins = bInput ? OldNode.Interface.Inputs : OldNode.Interface.Outputs;
			auto& Pins = bInput ? Normalized.Interface.Inputs : Normalized.Interface.Outputs;
			for (const auto& P : OldPins)
			{
				const auto* Mapped = IMQ5FinishMapVertex(Before, After, SourceId, P.VertexID, bInput);
				if (!Mapped) return Fail(TEXT("A prior Source pin name/type is missing or duplicated."));
				auto Copy = *Mapped;
				Copy.VertexID = P.VertexID;
				Pins.Add(Copy);
			}
		}
		if (OldNode.InputLiterals.Num() != NewNode->InputLiterals.Num()) return Fail(TEXT("Explicit Source literal count changed."));
		Normalized.InputLiterals.Reset();
		for (const auto& L : OldNode.InputLiterals)
		{
			const auto* P = IMQ5FinishMapVertex(Before, After, SourceId, L.VertexID, true);
			const auto* NL = P ? NewNode->InputLiterals.FindByPredicate([&](const auto& V) { return V.VertexID == P->VertexID; }) : nullptr;
			if (!NL || !L.Value.IsEqual(NL->Value)) return Fail(TEXT("A manual Source default changed."));
			auto Copy = *NL;
			Copy.VertexID = L.VertexID;
			Normalized.InputLiterals.Add(Copy);
		}
		if (!IMQ5FinishSameStruct(OldNode, Normalized)) return Fail(TEXT("Source manual/configuration/style data changed beyond the minor interface."));
	}
	for (const auto& Edge : BG.Edges)
	{
		const auto* From = IMQ5FinishMapVertex(Before, After, Edge.FromNodeID, Edge.FromVertexID, false);
		const auto* To = IMQ5FinishMapVertex(Before, After, Edge.ToNodeID, Edge.ToVertexID, true);
		if (!From || !To) return Fail(TEXT("Prior edge endpoint cannot be mapped uniquely."));
		auto Mapped = Edge;
		Mapped.FromVertexID = From->VertexID;
		Mapped.ToVertexID = To->VertexID;
		if (!AG.Edges.Contains(Mapped)) return Fail(TEXT("An old edge was dropped or retargeted."));
	}
	if (Added && !AG.Edges.Contains(*Added)) return Fail(TEXT("The sole intended Finished edge is missing."));
	for (const auto& C : Before.Dependencies)
	{
		if (C.ID == BS->ClassID) continue;
		const auto* NC = After.Dependencies.FindByPredicate([&](const auto& N) { return N.ID == C.ID; });
		if (!NC || !IMQ5FinishSameStruct(C, *NC)) return Fail(TEXT("A non-target dependency changed."));
	}
	for (const auto& C : After.Dependencies)
		if (C.ID != AC->ID && !Before.Dependencies.ContainsByPredicate([&](const auto& N) { return N.ID == C.ID; }))
			return Fail(TEXT("An unrelated dependency was added."));
	FMetasoundFrontendDocument Normalized = After;
	Normalized.Dependencies = Before.Dependencies;
	Normalized.RootGraph.GetDefaultGraph().Nodes = BG.Nodes;
	Normalized.RootGraph.GetDefaultGraph().Edges = BG.Edges;
	if (!IMQ5FinishSameStruct(Before, Normalized)) return Fail(TEXT("Graph interface, document metadata, variables, subgraphs or other page data changed."));
	return true;
}

FString IMQ5FinishDiskSha1(const FString& Path)
{
	TArray<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path) && !Bytes.IsEmpty()
		? FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).ToString().ToLower() : FString();
}

bool IMQ5FinishExport(const FMetasoundFrontendDocument& D, const FString& Path)
{
	const auto Doc = StructObject(D);
	return Doc->GetStringField(TEXT("_serialization_status")) == TEXT("PASS")
		&& FFileHelper::SaveStringToFile(SerializeJson(Doc), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticQ5FinishGraphAuthor,
	"IceMoon.AcousticField.MetaSound.Q5FinishGraphAuthor", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticQ5FinishGraphAuthor::RunTest(const FString&)
{
	FString RunId;
	FGuid RunGuid;
	if (!FParse::Value(FCommandLine::Get(), TEXT("IMQ5FinishAuthor="), RunId) || !FGuid::ParseExact(RunId, EGuidFormats::Digits, RunGuid))
	{ AddError(TEXT("Explicit task author RunId is required; general Automation cannot mutate this asset.")); return false; }
	if (!GEditor || GEditor->PlayWorld || !GEditor->GetEditorWorldContext().World()
		|| GEditor->GetEditorWorldContext().World()->GetPackage()->IsDirty())
	{ AddError(TEXT("Q5 graph author requires an idle clean task Editor map.")); return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/Q5FinishGraph"), RunId);
	const FString Filename = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("IceMoonAcousticField/Content/Tests/Audio/MS_AcousticQ5Impulse.uasset"));
	if (IFileManager::Get().DirectoryExists(*Directory) || IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishDiskSha1(Filename) != IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishOriginalSha1)
	{ AddError(TEXT("Author artifact or exact original package drift.")); return false; }
	IFileManager::Get().MakeDirectory(*Directory, true);
	UMetaSoundSource* Source = LoadObject<UMetaSoundSource>(nullptr, IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishObject);
	if (!Source || Source->GetPackage()->IsDirty() || Source->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || Source->NumChannels != 2)
	{ AddError(TEXT("Pinned source did not load cleanly as stereo.")); return false; }
	const FMetasoundFrontendDocument Before = Source->GetConstDocument();
	if (!IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishExport(Before, FPaths::Combine(Directory, TEXT("before.json"))))
	{ AddError(TEXT("Full reflected pre-document export failed.")); return false; }
	FGuid SourceId, FinishId;
	FGuid::ParseExact(TEXT("18A1C96B45BDEDDFE7E96BB28641A2D4"), EGuidFormats::Digits, SourceId);
	FGuid::ParseExact(TEXT("899DB84545ECB0090260749A38E6DAE7"), EGuidFormats::Digits, FinishId);
	auto& Registry = Metasound::Engine::FDocumentBuilderRegistry::GetChecked();
	UMetaSoundBuilderBase& Builder = Registry.FindOrBeginBuilding(*Source);
	const auto ClassName = Before.RootGraph.Metadata.GetClassName();
	const FTopLevelAssetPath AssetPath = FSoftObjectPath(Source).GetAssetPath();
	ON_SCOPE_EXIT { Registry.FinishBuilding(ClassName, AssetPath); };
	auto& Frontend = Builder.GetBuilder();
	Metasound::Frontend::FAutoUpdateRootGraph Update(FSoftObjectPath(Source).ToString(), true);
	FString Error;
	if (!Update.Transform(Frontend) || !IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishPreserved(Before, Source->GetConstDocument(), SourceId, nullptr, Error))
	{ AddError(TEXT("Minor upgrade rejected: ") + Error); return false; }
	EMetaSoundBuilderResult Result;
	const auto OneShot = Builder.FindNodeInputByName(FMetaSoundNodeHandle(FinishId), TEXT("UE.Source.OneShot.OnFinished"), Result);
	const FMetasoundFrontendNode* DelayNode = nullptr;
	const auto* DelayOut = Frontend.FindNodeOutputConnectedToNodeInput(OneShot.NodeID, OneShot.VertexID, &DelayNode);
	const auto* DelayClass = DelayNode ? Frontend.FindDependency(DelayNode->ClassID) : nullptr;
	if (Result != EMetaSoundBuilderResult::Succeeded || !DelayOut || !DelayClass
		|| DelayClass->Metadata.GetClassName().Namespace != TEXT("UE") || DelayClass->Metadata.GetClassName().Name != TEXT("Trigger Delay")
		|| DelayOut->Name != TEXT("Out") || DelayOut->TypeName != TEXT("Trigger"))
	{ AddError(TEXT("Original delayed graph OnFinished chain did not resolve.")); return false; }
	const FMetaSoundBuilderNodeOutputHandle Out(DelayNode->GetID(), DelayOut->VertexID);
	const auto Finished = Builder.FindNodeInputByName(FMetaSoundNodeHandle(SourceId), TEXT("Finished"), Result);
	if (Result != EMetaSoundBuilderResult::Succeeded || Frontend.FindNodeOutputConnectedToNodeInput(Finished.NodeID, Finished.VertexID))
	{ AddError(TEXT("Finished input missing or already occupied.")); return false; }
	Builder.ConnectNodes(Out, Finished, Result);
	FMetasoundFrontendEdge Added;
	Added.FromNodeID = Out.NodeID; Added.FromVertexID = Out.VertexID;
	Added.ToNodeID = Finished.NodeID; Added.ToVertexID = Finished.VertexID;
	if (Result != EMetaSoundBuilderResult::Succeeded || !Builder.NodesAreConnected(Out, Finished)
		|| !Builder.NodesAreConnected(Out, OneShot) || !IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishPreserved(Before, Source->GetConstDocument(), SourceId, &Added, Error))
	{ AddError(TEXT("Single edge patch rejected: ") + Error); return false; }
	Metasound::Frontend::FMetaSoundAssetRegistrationOptions Options;
	Options.bForceReregister = true;
	Options.bAutoUpdate = false;
	Source->UpdateAndRegisterForExecution(Options);
	auto& NodeRegistry = Metasound::Frontend::INodeClassRegistry::GetChecked();
	const auto Key = Source->GetGraphRegistryKey();
	NodeRegistry.WaitForAsyncGraphRegistration(Key);
	if (!NodeRegistry.IsGraphRegistered(Key) || !NodeRegistry.GetGraph(Key).IsValid()
		|| !IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishPreserved(Before, Source->GetConstDocument(), SourceId, &Added, Error)
		|| !IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishExport(Source->GetConstDocument(), FPaths::Combine(Directory, TEXT("after.json"))))
	{ AddError(TEXT("Frontend graph construction or final document preservation failed: ") + Error); return false; }
	Source->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Source->GetPackage(), Source, *Filename, Args))
	{ AddError(TEXT("Exact existing Q5 package save failed; task runner must recover backup after process exit.")); return false; }
	const FString NewSha1 = IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishDiskSha1(Filename);
	const auto Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("schema"), TEXT("IMQ5FinishGraphAuthor.v1"));
	Receipt->SetStringField(TEXT("run_id"), RunId);
	Receipt->SetStringField(TEXT("asset_path"), Source->GetPathName());
	Receipt->SetStringField(TEXT("sha1_before"), IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishOriginalSha1);
	Receipt->SetStringField(TEXT("sha1_after"), NewSha1);
	Receipt->SetBoolField(TEXT("frontend_graph_nonnull"), true);
	Receipt->SetBoolField(TEXT("audio_operator_execution_proven"), false);
	Receipt->SetBoolField(TEXT("fresh_process_readback_proven"), false);
	const bool bSaved = !NewSha1.IsEmpty() && NewSha1 != IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishOriginalSha1 && !Source->GetPackage()->IsDirty()
		&& FFileHelper::SaveStringToFile(IMAcousticMetaSoundGraphReadbackTestPrivate::SerializeJson(Receipt), *FPaths::Combine(Directory, TEXT("author.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s Q5 Finished single-edge author; fresh readback and Q5 execution pending"), bSaved ? TEXT("PASS") : TEXT("FAIL"));
	return bSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticQ5FinishGraphReadback,
	"IceMoon.AcousticField.MetaSound.Q5FinishGraphReadback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticQ5FinishGraphReadback::RunTest(const FString&)
{
	FString RunId, ExpectedSha1;
	FGuid RunGuid;
	if (!FParse::Value(FCommandLine::Get(), TEXT("IMQ5FinishReadback="), RunId)
		|| !FGuid::ParseExact(RunId, EGuidFormats::Digits, RunGuid)
		|| !FParse::Value(FCommandLine::Get(), TEXT("IMQ5FinishExpectedSHA1="), ExpectedSha1)
		|| ExpectedSha1.Len() != 40 || ExpectedSha1 == IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishOriginalSha1)
	{ AddError(TEXT("Explicit fresh readback RunId and changed package SHA-1 are required.")); return false; }
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/Q5FinishGraph"), RunId);
	const FString Filename = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("IceMoonAcousticField/Content/Tests/Audio/MS_AcousticQ5Impulse.uasset"));
	const FString Fresh = FPaths::Combine(Directory, TEXT("fresh.json"));
	if (!IFileManager::Get().FileExists(*FPaths::Combine(Directory, TEXT("author.json")))
		|| IFileManager::Get().FileExists(*Fresh) || IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishDiskSha1(Filename) != ExpectedSha1.ToLower())
	{ AddError(TEXT("Fresh package/artifact binding differs.")); return false; }
	UMetaSoundSource* Source = LoadObject<UMetaSoundSource>(nullptr, IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishObject);
	if (!Source || Source->GetPackage()->IsDirty() || Source->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || Source->NumChannels != 2)
	{ AddError(TEXT("Saved Q5 source did not reopen cleanly as stereo.")); return false; }
	Metasound::Frontend::FMetaSoundAssetRegistrationOptions Options;
	Options.bForceReregister = true;
	Options.bAutoUpdate = false;
	Source->UpdateAndRegisterForExecution(Options);
	auto& Registry = Metasound::Frontend::INodeClassRegistry::GetChecked();
	const auto Key = Source->GetGraphRegistryKey();
	Registry.WaitForAsyncGraphRegistration(Key);
	const bool bOk = Registry.IsGraphRegistered(Key) && Registry.GetGraph(Key).IsValid()
		&& !Source->GetPackage()->IsDirty() && IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishDiskSha1(Filename) == ExpectedSha1.ToLower()
		&& IMAcousticMetaSoundGraphReadbackTestPrivate::IMQ5FinishExport(Source->GetConstDocument(), Fresh);
	const auto Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("schema"), TEXT("IMQ5FinishGraphReadback.v1"));
	Receipt->SetStringField(TEXT("run_id"), RunId);
	Receipt->SetStringField(TEXT("expected_sha1"), ExpectedSha1.ToLower());
	Receipt->SetBoolField(TEXT("frontend_graph_nonnull"), bOk);
	Receipt->SetBoolField(TEXT("audio_operator_execution_proven"), false);
	const bool bWritten = FFileHelper::SaveStringToFile(IMAcousticMetaSoundGraphReadbackTestPrivate::SerializeJson(Receipt),
		*FPaths::Combine(Directory, TEXT("readback.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestTrue(TEXT("Fresh frontend construction, immutable disk package and full document export"), bOk && bWritten);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s Q5 Finished fresh readback; Root document comparison and Q5 execution pending"), bOk && bWritten ? TEXT("PASS") : TEXT("FAIL"));
	return bOk && bWritten;
}
#endif // WITH_EDITORONLY_DATA

#endif
