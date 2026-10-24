#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundSource.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace IMAcousticQ5ConnectionProbePrivate
{
constexpr TCHAR IMQ5SourceObjectPath[] =
	TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticQ5Impulse.MS_AcousticQ5Impulse");
constexpr TCHAR IMQ5WaveObjectPath[] =
	TEXT("/IceMoonAcousticField/Tests/Audio/SW_AcousticQ5Impulse.SW_AcousticQ5Impulse");
constexpr TCHAR IMQ5EnvironmentObjectPath[] =
	TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticEnvironment.MS_AcousticEnvironment");

constexpr TCHAR IMQ5TargetNodeIdText[] = TEXT("E92D18304242039CD6001382C7DA918E");
constexpr TCHAR IMQ5TargetVertexIdText[] = TEXT("E488C09152673EB8F2DC89B960A6AF04");
constexpr TCHAR IMQ5AcousticSourceNodeIdText[] = TEXT("18A1C96B45BDEDDFE7E96BB28641A2D4");
constexpr TCHAR IMQ5AcousticSourceVoiceVertexIdText[] = TEXT("D8A15D1E3A2BF63FE2275AED593515FA");
constexpr TCHAR IMQ5AudioBusWriterNodeIdText[] = TEXT("E0FEE3E44A1B08339537678210DE63A7");
constexpr TCHAR IMQ5AudioBusWriterBusVertexIdText[] = TEXT("10C257F9714647525CFD4E715E16F0C8");
constexpr TCHAR IMQ5PinnedOnPlayNodeIdText[] = TEXT("29C47C8B4A51E049052C56AD61BA333A");
constexpr TCHAR IMQ5PinnedOnPlayVertexIdText[] = TEXT("CC02F2799674D5DA72ABBEB51D1DC820");
constexpr TCHAR IMQ5PinnedPlayTemplateNodeIdText[] = TEXT("6A79A662460F5E02B7E118845024D629");
constexpr TCHAR IMQ5PinnedPlayTemplateVertexIdText[] = TEXT("8F91DB654B651C24B42803BC698D18BD");
constexpr TCHAR IMQ5PinnedFinishNodeIdText[] = TEXT("899DB84545ECB0090260749A38E6DAE7");
constexpr TCHAR IMQ5PinnedFinishVertexIdText[] = TEXT("DC13176B76D7263C9A5AC6AF2436C9B4");
constexpr TCHAR IMQ5PinnedWaveFinishedVertexIdText[] = TEXT("8D9B132C6AF67AD91C16DE102B0D8769");

constexpr TCHAR IMQ5ExpectedSourceSha1[] = TEXT("6dd1c837a04230771ad19b2b6802d38834247da7");
constexpr TCHAR IMQ5ExpectedWaveSha1[] = TEXT("3f720a2536e69ddf7599c4c7413b693ff1300882");
constexpr TCHAR IMQ5ExpectedEnvironmentSha1[] = TEXT("560d3f3c7b5d3306338e778479ee314aee877d89");

struct FIMQ5AssetState
{
	FString Name;
	FString ObjectPath;
	FString DiskPath;
	FString ExpectedSha1;
	FString Sha1Before;
	FString Sha1After;
	UObject* Object = nullptr;
	UPackage* Package = nullptr;
	bool bPackageDirtyBefore = false;
	bool bPackageDirtyAfter = false;
};

struct FIMQ5EditorState
{
	UWorld* World = nullptr;
	UPackage* MapPackage = nullptr;
	FString WorldPath;
	FString MapPackagePath;
	bool bMapDirty = false;
};

struct FIMQ5InputTemplateChainSpec
{
	FString Name;
	FString TargetNodeIdText;
	FString TargetVertexIdText;
	FName TargetPinName;
	FName GraphInputName;
	FName ExpectedDataType;
};

struct FIMQ5InputTemplateChainObservation
{
	FString Name;
	FString Error;
	FName GraphInputName;
	FName ExpectedDataType;
	FGuid TargetNodeId;
	FGuid TargetVertexId;
	FName TargetPinName;
	FName TargetPinType;
	bool bTargetInputOccupied = false;
	bool bTemplateSourceResolved = false;
	FGuid TemplateNodeId;
	FString TemplateNodeName;
	FGuid TemplateOutputVertexId;
	FName TemplateOutputName;
	FName TemplateOutputType;
	bool bTemplateDependencyFound = false;
	bool bExpectedTemplateDependency = false;
	FName TemplateDependencyNamespace;
	FName TemplateDependencyName;
	FName TemplateDependencyVariant;
	FGuid TemplateInputVertexId;
	FName TemplateInputName;
	FName TemplateInputType;
	bool bTemplateInputOccupied = false;
	bool bUpstreamGraphInputResolved = false;
	FGuid UpstreamNodeId;
	FString UpstreamNodeName;
	FGuid UpstreamVertexId;
	FName UpstreamPinName;
	FName UpstreamPinType;
	bool bGraphInputNodeFound = false;
	FGuid GraphInputNodeId;
	FGuid GraphInputVertexId;
	FName GraphInputOutputName;
	FName GraphInputOutputType;
	bool bGraphInputIdentityMatches = false;
	bool bGraphInputToTemplateInputExact = false;
	bool bTemplateOutputToTargetExact = false;
	bool bChainExact = false;
};

struct FIMQ5ConnectionObservation
{
	FString Classification = TEXT("NOT_OBSERVED");
	FGuid TargetNodeId;
	FGuid TargetVertexId;
	FName TargetPinName;
	FName TargetPinType;
	bool bTargetInputOccupied = false;
	bool bIncomingSourceResolved = false;
	FGuid SourceNodeId;
	FGuid SourceVertexId;
	FString SourceNodeName;
	FName SourcePinName;
	FName SourcePinType;
	bool bExactEdgeConfirmed = false;
	bool bMatchesPinnedOnPlay = false;
	bool bGraphOnPlayInputFound = false;
	FGuid GraphOnPlayNodeId;
	FGuid GraphOnPlayVertexId;
	bool bMatchesGraphOnPlayInput = false;
	bool bAllThreeChainsExact = false;
	bool bPlayDelayExact = false;
	bool bFinishDelayExact = false;
	FGuid PlayDelayNodeId;
	FGuid FinishDelayNodeId;
	TArray<FIMQ5InputTemplateChainObservation> InputTemplateChains;
	bool bBuilderCleanedUp = false;
};

bool IMQ5ReadSha1(const FString& FilePath, FString& OutSha1, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!IFileManager::Get().FileExists(*FilePath)
		|| !FFileHelper::LoadFileToArray(Bytes, *FilePath)
		|| Bytes.IsEmpty()
		|| Bytes.Num() > static_cast<int64>(MAX_uint32))
	{
		OutError = FString::Printf(TEXT("Cannot read a non-empty asset package within the SHA-1 API size limit: %s"), *FilePath);
		return false;
	}

	OutSha1 = FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).ToString().ToLower();
	return true;
}

bool IMQ5CaptureEditorState(FIMQ5EditorState& OutState, FString& OutError)
{
	if (!GEditor || !GEngine)
	{
		OutError = TEXT("Editor or Engine singleton is unavailable.");
		return false;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			OutError = TEXT("PIE is active; the Q5 connection probe requires an idle Editor world.");
			return false;
		}
	}

	OutState.World = GEditor->GetEditorWorldContext().World();
	if (!OutState.World)
	{
		OutError = TEXT("The Editor world is unavailable.");
		return false;
	}

	OutState.MapPackage = OutState.World->GetPackage();
	if (!OutState.MapPackage)
	{
		OutError = TEXT("The Editor world has no map package.");
		return false;
	}

	OutState.WorldPath = OutState.World->GetPathName();
	OutState.MapPackagePath = OutState.MapPackage->GetName();
	OutState.bMapDirty = OutState.MapPackage->IsDirty();
	return true;
}

bool IMQ5WriteReceipt(const FString& FilePath, const TSharedRef<FJsonObject>& Receipt)
{
	FString JsonText;
	const auto Writer = TJsonWriterFactory<>::Create(&JsonText);
	return FJsonSerializer::Serialize(Receipt, Writer)
		&& FFileHelper::SaveStringToFile(
			JsonText,
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString IMQ5Guid(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::Digits).ToUpper();
}

TSharedRef<FJsonObject> IMQ5MakeAssetReceipt(const FIMQ5AssetState& Asset)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("object_path"), Asset.ObjectPath);
	Object->SetStringField(TEXT("disk_path"), Asset.DiskPath);
	Object->SetStringField(TEXT("expected_sha1"), Asset.ExpectedSha1);
	Object->SetStringField(TEXT("sha1_before"), Asset.Sha1Before);
	Object->SetStringField(TEXT("sha1_after"), Asset.Sha1After);
	Object->SetBoolField(TEXT("before_matches_expected"), Asset.Sha1Before == Asset.ExpectedSha1);
	Object->SetBoolField(TEXT("after_matches_before"), !Asset.Sha1Before.IsEmpty() && Asset.Sha1After == Asset.Sha1Before);
	Object->SetBoolField(TEXT("package_loaded"), Asset.Object != nullptr && Asset.Package != nullptr);
	Object->SetStringField(TEXT("package_path"), Asset.Package ? Asset.Package->GetName() : FString());
	Object->SetBoolField(TEXT("package_dirty_before"), Asset.bPackageDirtyBefore);
	Object->SetBoolField(TEXT("package_dirty_after"), Asset.bPackageDirtyAfter);
	Object->SetBoolField(TEXT("package_dirty_unchanged"), Asset.Object != nullptr && Asset.bPackageDirtyBefore == Asset.bPackageDirtyAfter);
	return Object;
}

TSharedRef<FJsonObject> IMQ5MakeEditorStateReceipt(
	const FIMQ5EditorState& Before,
	const FIMQ5EditorState& After,
	bool bHaveBefore,
	bool bHaveAfter)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("captured_before"), bHaveBefore);
	Object->SetBoolField(TEXT("captured_after"), bHaveAfter);
	Object->SetStringField(TEXT("world_path_before"), Before.WorldPath);
	Object->SetStringField(TEXT("world_path_after"), After.WorldPath);
	Object->SetStringField(TEXT("map_package_before"), Before.MapPackagePath);
	Object->SetStringField(TEXT("map_package_after"), After.MapPackagePath);
	Object->SetBoolField(TEXT("map_dirty_before"), Before.bMapDirty);
	Object->SetBoolField(TEXT("map_dirty_after"), After.bMapDirty);
	Object->SetBoolField(
		TEXT("map_and_world_unchanged"),
		bHaveBefore && bHaveAfter
			&& Before.World == After.World
			&& Before.MapPackage == After.MapPackage
			&& Before.WorldPath == After.WorldPath
			&& Before.MapPackagePath == After.MapPackagePath
			&& Before.bMapDirty == After.bMapDirty);
	return Object;
}

struct FIMScopedQ5BuilderCleanup
{
	Metasound::Engine::FDocumentBuilderRegistry& Registry;
	FMetasoundFrontendClassName ClassName;
	FTopLevelAssetPath AssetPath;
	bool bAttempted = false;
	bool bRemoved = false;

	FIMScopedQ5BuilderCleanup(
		Metasound::Engine::FDocumentBuilderRegistry& InRegistry,
		const FMetasoundFrontendClassName& InClassName,
		const FTopLevelAssetPath& InAssetPath)
		: Registry(InRegistry)
		, ClassName(InClassName)
		, AssetPath(InAssetPath)
	{
	}

	bool Finish()
	{
		if (!bAttempted)
		{
			bAttempted = true;
			bRemoved = Registry.FinishBuilding(ClassName, AssetPath);
		}
		return bRemoved;
	}

	~FIMScopedQ5BuilderCleanup()
	{
		if (!bAttempted)
		{
			Finish();
		}
	}
};

bool IMQ5TraceInputTemplateChain(
	UMetaSoundBuilderBase& Builder,
	const FIMQ5InputTemplateChainSpec& Spec,
	FIMQ5InputTemplateChainObservation& OutChain)
{
	OutChain.Name = Spec.Name;
	OutChain.GraphInputName = Spec.GraphInputName;
	OutChain.ExpectedDataType = Spec.ExpectedDataType;

	const auto Fail = [&OutChain](const FString& Reason)
	{
		OutChain.Error = Reason;
		return false;
	};

	FGuid ExpectedTargetNodeId;
	FGuid ExpectedTargetVertexId;
	if (!FGuid::ParseExact(Spec.TargetNodeIdText, EGuidFormats::Digits, ExpectedTargetNodeId)
		|| !FGuid::ParseExact(Spec.TargetVertexIdText, EGuidFormats::Digits, ExpectedTargetVertexId))
	{
		return Fail(TEXT("Pinned target NodeID or VertexID is malformed."));
	}

	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	const FMetaSoundBuilderNodeInputHandle TargetInput =
		Builder.FindNodeInputByName(
			FMetaSoundNodeHandle(ExpectedTargetNodeId),
			Spec.TargetPinName,
			Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| !TargetInput.IsSet()
		|| TargetInput.NodeID != ExpectedTargetNodeId
		|| TargetInput.VertexID != ExpectedTargetVertexId)
	{
		return Fail(TEXT("Pinned target input handle did not resolve exactly."));
	}

	OutChain.TargetNodeId = TargetInput.NodeID;
	OutChain.TargetVertexId = TargetInput.VertexID;
	Builder.GetNodeInputData(TargetInput, OutChain.TargetPinName, OutChain.TargetPinType, Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| OutChain.TargetPinName != Spec.TargetPinName
		|| OutChain.TargetPinType != Spec.ExpectedDataType)
	{
		return Fail(TEXT("Target input pin name/type differs from the pinned specification."));
	}

	OutChain.bTargetInputOccupied = Builder.NodeInputIsConnected(TargetInput);
	const FMetasoundFrontendNode* TemplateNode = nullptr;
	const FMetasoundFrontendVertex* TemplateOutputVertex =
		Builder.GetConstBuilder().FindNodeOutputConnectedToNodeInput(
			TargetInput.NodeID,
			TargetInput.VertexID,
			&TemplateNode);
	if (!TemplateNode || !TemplateOutputVertex)
	{
		return Fail(OutChain.bTargetInputOccupied
			? TEXT("Target input reports occupied, but its first-hop source vertex did not resolve.")
			: TEXT("Target input is not occupied and has no first-hop source vertex."));
	}

	OutChain.bTemplateSourceResolved = true;
	OutChain.TemplateNodeId = TemplateNode->GetID();
	OutChain.TemplateNodeName = TemplateNode->Name.ToString();
	OutChain.TemplateOutputVertexId = TemplateOutputVertex->VertexID;
	const FMetaSoundBuilderNodeOutputHandle TemplateOutput(
		OutChain.TemplateNodeId,
		OutChain.TemplateOutputVertexId);
	Builder.GetNodeOutputData(
		TemplateOutput,
		OutChain.TemplateOutputName,
		OutChain.TemplateOutputType,
		Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| OutChain.TemplateOutputName != FName(TEXT("Value"))
		|| OutChain.TemplateOutputType != Spec.ExpectedDataType)
	{
		return Fail(TEXT("First-hop output did not read back as the expected Input Template Value pin."));
	}

	const FMetasoundFrontendClass* TemplateDependency =
		Builder.GetConstBuilder().FindDependency(TemplateNode->ClassID);
	if (!TemplateDependency)
	{
		return Fail(TEXT("The first-hop node's class dependency could not be read."));
	}

	OutChain.bTemplateDependencyFound = true;
	const FMetasoundFrontendClassName& TemplateClassName =
		TemplateDependency->Metadata.GetClassName();
	OutChain.TemplateDependencyNamespace = TemplateClassName.Namespace;
	OutChain.TemplateDependencyName = TemplateClassName.Name;
	OutChain.TemplateDependencyVariant = TemplateClassName.Variant;
	OutChain.bExpectedTemplateDependency =
		TemplateClassName.Namespace == FName(TEXT("UE"))
		&& TemplateClassName.Name == FName(TEXT("Input"))
		&& TemplateClassName.Variant == FName(TEXT("Template"));
	if (!OutChain.bExpectedTemplateDependency)
	{
		return Fail(TEXT("First-hop node is not the UE.Input.Template dependency."));
	}

	if (TemplateNode->Interface.Inputs.Num() != 1
		|| TemplateNode->Interface.Outputs.Num() != 1
		|| TemplateNode->Interface.Inputs[0].Name != FName(TEXT("Value"))
		|| TemplateNode->Interface.Outputs[0].Name != FName(TEXT("Value"))
		|| TemplateNode->Interface.Inputs[0].TypeName != Spec.ExpectedDataType
		|| TemplateNode->Interface.Outputs[0].TypeName != Spec.ExpectedDataType
		|| TemplateNode->Interface.Outputs[0].VertexID != OutChain.TemplateOutputVertexId)
	{
		return Fail(TEXT("Input Template must expose one Value input/output of the expected type."));
	}

	Result = EMetaSoundBuilderResult::Failed;
	const FMetaSoundBuilderNodeInputHandle TemplateInput =
		Builder.FindNodeInputByName(
			FMetaSoundNodeHandle(OutChain.TemplateNodeId),
			FName(TEXT("Value")),
			Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| !TemplateInput.IsSet()
		|| TemplateInput.NodeID != OutChain.TemplateNodeId
		|| TemplateInput.VertexID != TemplateNode->Interface.Inputs[0].VertexID
		|| !Builder.ContainsNodeInput(TemplateInput))
	{
		return Fail(TEXT("Input Template Value input handle did not resolve exactly."));
	}

	OutChain.TemplateInputVertexId = TemplateInput.VertexID;
	Builder.GetNodeInputData(
		TemplateInput,
		OutChain.TemplateInputName,
		OutChain.TemplateInputType,
		Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| OutChain.TemplateInputName != FName(TEXT("Value"))
		|| OutChain.TemplateInputType != Spec.ExpectedDataType)
	{
		return Fail(TEXT("Input Template Value input name/type differs from its generated interface."));
	}

	OutChain.bTemplateInputOccupied = Builder.NodeInputIsConnected(TemplateInput);
	const FMetasoundFrontendNode* UpstreamNode = nullptr;
	const FMetasoundFrontendVertex* UpstreamVertex =
		Builder.GetConstBuilder().FindNodeOutputConnectedToNodeInput(
			TemplateInput.NodeID,
			TemplateInput.VertexID,
			&UpstreamNode);
	if (!UpstreamNode || !UpstreamVertex)
	{
		return Fail(TEXT("Input Template.Value has no resolvable upstream graph input output."));
	}

	OutChain.bUpstreamGraphInputResolved = true;
	OutChain.UpstreamNodeId = UpstreamNode->GetID();
	OutChain.UpstreamNodeName = UpstreamNode->Name.ToString();
	OutChain.UpstreamVertexId = UpstreamVertex->VertexID;
	const FMetaSoundBuilderNodeOutputHandle UpstreamOutput(
		OutChain.UpstreamNodeId,
		OutChain.UpstreamVertexId);
	Builder.GetNodeOutputData(
		UpstreamOutput,
		OutChain.UpstreamPinName,
		OutChain.UpstreamPinType,
		Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| OutChain.UpstreamPinType != Spec.ExpectedDataType)
	{
		return Fail(TEXT("Upstream graph-input output type differs from the expected input type."));
	}

	const FMetasoundFrontendNode* GraphInputNode =
		Builder.GetConstBuilder().FindGraphInputNode(Spec.GraphInputName);
	if (!GraphInputNode || GraphInputNode->Interface.Outputs.Num() != 1)
	{
		return Fail(TEXT("Expected graph input node is missing or does not expose exactly one output."));
	}

	OutChain.bGraphInputNodeFound = true;
	OutChain.GraphInputNodeId = GraphInputNode->GetID();
	OutChain.GraphInputVertexId = GraphInputNode->Interface.Outputs[0].VertexID;
	OutChain.bGraphInputIdentityMatches =
		OutChain.UpstreamNodeId == OutChain.GraphInputNodeId
		&& OutChain.UpstreamVertexId == OutChain.GraphInputVertexId;
	const FMetaSoundBuilderNodeOutputHandle GraphInputOutput(
		OutChain.GraphInputNodeId,
		OutChain.GraphInputVertexId);
	Builder.GetNodeOutputData(
		GraphInputOutput,
		OutChain.GraphInputOutputName,
		OutChain.GraphInputOutputType,
		Result);
	if (Result != EMetaSoundBuilderResult::Succeeded
		|| OutChain.GraphInputOutputType != Spec.ExpectedDataType)
	{
		return Fail(TEXT("FindGraphInputNode output type differs from the expected graph input type."));
	}

	OutChain.bGraphInputToTemplateInputExact =
		Builder.NodesAreConnected(GraphInputOutput, TemplateInput);
	OutChain.bTemplateOutputToTargetExact =
		Builder.NodesAreConnected(TemplateOutput, TargetInput);

	if (!OutChain.bTargetInputOccupied
		|| !OutChain.bTemplateInputOccupied
		|| !OutChain.bGraphInputIdentityMatches
		|| !OutChain.bGraphInputToTemplateInputExact
		|| !OutChain.bTemplateOutputToTargetExact
		|| OutChain.TargetPinType != Spec.ExpectedDataType
		|| OutChain.TemplateOutputType != Spec.ExpectedDataType
		|| OutChain.TemplateInputType != Spec.ExpectedDataType
		|| OutChain.UpstreamPinType != Spec.ExpectedDataType
		|| OutChain.GraphInputOutputType != Spec.ExpectedDataType)
	{
		return Fail(TEXT("Two-hop chain occupancy, identity, exact-edge, or type checks failed."));
	}

	OutChain.bChainExact = true;
	return true;
}

bool IMQ5TraceTriggerDelay(
	UMetaSoundBuilderBase& Builder,
	const TCHAR* TargetNodeText,
	const TCHAR* TargetVertexText,
	FName TargetPinName,
	const TCHAR* ExpectedUpstreamNodeText,
	const TCHAR* ExpectedUpstreamVertexText,
	FName ExpectedUpstreamPinName,
	FGuid& OutDelayNodeId,
	FGuid& OutDelayInputVertexId,
	FString& OutError)
{
	FGuid TargetNodeId, TargetVertexId, ExpectedUpstreamNodeId, ExpectedUpstreamVertexId;
	if (!FGuid::ParseExact(TargetNodeText, EGuidFormats::Digits, TargetNodeId)
		|| !FGuid::ParseExact(TargetVertexText, EGuidFormats::Digits, TargetVertexId)
		|| !FGuid::ParseExact(ExpectedUpstreamNodeText, EGuidFormats::Digits, ExpectedUpstreamNodeId)
		|| !FGuid::ParseExact(ExpectedUpstreamVertexText, EGuidFormats::Digits, ExpectedUpstreamVertexId))
	{
		OutError = TEXT("Pinned delayed-trigger NodeID or VertexID is malformed.");
		return false;
	}
	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	const FMetaSoundBuilderNodeInputHandle Target = Builder.FindNodeInputByName(
		FMetaSoundNodeHandle(TargetNodeId), TargetPinName, Result);
	if (Result != EMetaSoundBuilderResult::Succeeded || Target.VertexID != TargetVertexId)
	{
		OutError = TEXT("Pinned delayed-trigger target input did not resolve exactly.");
		return false;
	}
	const FMetasoundFrontendNode* DelayNode = nullptr;
	const FMetasoundFrontendVertex* DelayOut = Builder.GetConstBuilder().FindNodeOutputConnectedToNodeInput(
		Target.NodeID, Target.VertexID, &DelayNode);
	if (!DelayNode || !DelayOut || DelayOut->Name != FName(TEXT("Out"))
		|| DelayOut->TypeName != FName(TEXT("Trigger")))
	{
		OutError = TEXT("Delayed-trigger target is not fed by a Trigger Out vertex.");
		return false;
	}
	const FMetasoundFrontendClass* DelayClass = Builder.GetConstBuilder().FindDependency(DelayNode->ClassID);
	if (!DelayClass || DelayClass->Metadata.GetClassName().Namespace != FName(TEXT("UE"))
		|| DelayClass->Metadata.GetClassName().Name != FName(TEXT("Trigger Delay"))
		|| DelayClass->Metadata.GetClassName().Variant != NAME_None)
	{
		OutError = TEXT("Delayed-trigger target is not fed by the UE.Trigger Delay standard node.");
		return false;
	}
	OutDelayNodeId = DelayNode->GetID();
	const FMetaSoundBuilderNodeInputHandle DelayIn = Builder.FindNodeInputByName(
		FMetaSoundNodeHandle(OutDelayNodeId), FName(TEXT("In")), Result);
	if (Result != EMetaSoundBuilderResult::Succeeded || !DelayIn.IsSet())
	{
		OutError = TEXT("UE.Trigger Delay.In did not resolve.");
		return false;
	}
	OutDelayInputVertexId = DelayIn.VertexID;
	const FMetasoundFrontendNode* UpstreamNode = nullptr;
	const FMetasoundFrontendVertex* UpstreamOut = Builder.GetConstBuilder().FindNodeOutputConnectedToNodeInput(
		DelayIn.NodeID, DelayIn.VertexID, &UpstreamNode);
	if (!UpstreamNode || !UpstreamOut || UpstreamNode->GetID() != ExpectedUpstreamNodeId
		|| UpstreamOut->VertexID != ExpectedUpstreamVertexId
		|| UpstreamOut->Name != ExpectedUpstreamPinName
		|| UpstreamOut->TypeName != FName(TEXT("Trigger")))
	{
		OutError = TEXT("UE.Trigger Delay.In does not have the pinned upstream Trigger edge.");
		return false;
	}
	return true;
}

bool IMQ5ObserveIncomingEdge(
	UMetaSoundSource* SourceAsset,
	FIMQ5ConnectionObservation& OutObservation,
	FString& OutError)
{
	if (!SourceAsset)
	{
		OutError = TEXT("The pinned Q5 source MetaSound failed to load.");
		return false;
	}

	Metasound::Engine::FDocumentBuilderRegistry& Registry =
		Metasound::Engine::FDocumentBuilderRegistry::GetChecked();
	TScriptInterface<IMetaSoundDocumentInterface> DocumentInterface = SourceAsset;
	if (!DocumentInterface.GetObject()
		|| !DocumentInterface.GetInterface()
		|| Registry.FindBuilder(DocumentInterface) != nullptr)
	{
		OutError = TEXT("The source document interface is invalid or already has a registered builder.");
		return false;
	}

	const FMetasoundFrontendClassName ClassName =
		SourceAsset->GetConstDocument().RootGraph.Metadata.GetClassName();
	if (!ClassName.IsValid())
	{
		OutError = TEXT("The loaded source graph has no valid MetaSound class name.");
		return false;
	}

	const FTopLevelAssetPath AssetPath = FSoftObjectPath(SourceAsset).GetAssetPath();
	UMetaSoundBuilderBase& Builder = Registry.FindOrBeginBuilding(*SourceAsset);
	FIMScopedQ5BuilderCleanup Cleanup(Registry, ClassName, AssetPath);

	FString PlayDelayError, FinishDelayError;
	FGuid PlayDelayInputVertexId, FinishDelayInputVertexId;
	OutObservation.bPlayDelayExact = IMQ5TraceTriggerDelay(
		Builder, IMQ5TargetNodeIdText, IMQ5TargetVertexIdText,
		FName(TEXT("Play")), IMQ5PinnedPlayTemplateNodeIdText,
		IMQ5PinnedPlayTemplateVertexIdText, FName(TEXT("Value")),
		OutObservation.PlayDelayNodeId, PlayDelayInputVertexId, PlayDelayError);
	OutObservation.bFinishDelayExact = IMQ5TraceTriggerDelay(
		Builder, IMQ5PinnedFinishNodeIdText, IMQ5PinnedFinishVertexIdText,
		FName(TEXT("UE.Source.OneShot.OnFinished")), IMQ5TargetNodeIdText,
		IMQ5PinnedWaveFinishedVertexIdText, FName(TEXT("On Finished")),
		OutObservation.FinishDelayNodeId, FinishDelayInputVertexId, FinishDelayError);

	const FIMQ5InputTemplateChainSpec ChainSpecs[] = {
		{
			TEXT("wave_player_play"),
			IMQ5Guid(OutObservation.PlayDelayNodeId),
			IMQ5Guid(PlayDelayInputVertexId),
			FName(TEXT("In")),
			FName(TEXT("UE.Source.OnPlay")),
			FName(TEXT("Trigger"))
		},
		{
			TEXT("acoustic_source_voice"),
			IMQ5AcousticSourceNodeIdText,
			IMQ5AcousticSourceVoiceVertexIdText,
			FName(TEXT("Voice")),
			FName(TEXT("Acoustic Voice")),
			FName(TEXT("Int32"))
		},
		{
			TEXT("audio_bus_writer_audio_bus"),
			IMQ5AudioBusWriterNodeIdText,
			IMQ5AudioBusWriterBusVertexIdText,
			FName(TEXT("Audio Bus")),
			FName(TEXT("Acoustic Bus")),
			FName(TEXT("AudioBusAsset"))
		}
	};

	bool bAllThreeChainsExact = true;
	FString FirstChainError;
	for (const FIMQ5InputTemplateChainSpec& Spec : ChainSpecs)
	{
		FIMQ5InputTemplateChainObservation Chain;
		if (!IMQ5TraceInputTemplateChain(Builder, Spec, Chain))
		{
			bAllThreeChainsExact = false;
			if (FirstChainError.IsEmpty())
			{
				FirstChainError = Chain.Name + TEXT(": ") + Chain.Error;
			}
		}
		OutObservation.InputTemplateChains.Add(MoveTemp(Chain));
	}

	OutObservation.bAllThreeChainsExact =
		bAllThreeChainsExact && OutObservation.InputTemplateChains.Num() == UE_ARRAY_COUNT(ChainSpecs);
	if (OutObservation.InputTemplateChains.Num() == UE_ARRAY_COUNT(ChainSpecs))
	{
		const FIMQ5InputTemplateChainObservation& PlayChain = OutObservation.InputTemplateChains[0];
		OutObservation.TargetNodeId = PlayChain.TargetNodeId;
		OutObservation.TargetVertexId = PlayChain.TargetVertexId;
		OutObservation.TargetPinName = PlayChain.TargetPinName;
		OutObservation.TargetPinType = PlayChain.TargetPinType;
		OutObservation.bTargetInputOccupied = PlayChain.bTargetInputOccupied;
		OutObservation.bIncomingSourceResolved = PlayChain.bTemplateSourceResolved;
		OutObservation.SourceNodeId = PlayChain.TemplateNodeId;
		OutObservation.SourceVertexId = PlayChain.TemplateOutputVertexId;
		OutObservation.SourceNodeName = PlayChain.TemplateNodeName;
		OutObservation.SourcePinName = PlayChain.TemplateOutputName;
		OutObservation.SourcePinType = PlayChain.TemplateOutputType;
		OutObservation.bExactEdgeConfirmed = PlayChain.bTemplateOutputToTargetExact;
		OutObservation.bGraphOnPlayInputFound = PlayChain.bGraphInputNodeFound;
		OutObservation.GraphOnPlayNodeId = PlayChain.GraphInputNodeId;
		OutObservation.GraphOnPlayVertexId = PlayChain.GraphInputVertexId;
		OutObservation.bMatchesGraphOnPlayInput = PlayChain.bGraphInputIdentityMatches;
	}

	FGuid ExpectedOnPlayNodeId;
	FGuid ExpectedOnPlayVertexId;
	const bool bPinnedOnPlayIdsValid =
		FGuid::ParseExact(IMQ5PinnedOnPlayNodeIdText, EGuidFormats::Digits, ExpectedOnPlayNodeId)
		&& FGuid::ParseExact(IMQ5PinnedOnPlayVertexIdText, EGuidFormats::Digits, ExpectedOnPlayVertexId);
	const FIMQ5InputTemplateChainObservation* PlayChain =
		OutObservation.InputTemplateChains.IsEmpty() ? nullptr : &OutObservation.InputTemplateChains[0];
	OutObservation.bMatchesPinnedOnPlay =
		bPinnedOnPlayIdsValid
		&& PlayChain
		&& PlayChain->bGraphInputNodeFound
		&& PlayChain->bGraphInputIdentityMatches
		&& PlayChain->GraphInputNodeId == ExpectedOnPlayNodeId
		&& PlayChain->GraphInputVertexId == ExpectedOnPlayVertexId
		&& PlayChain->GraphInputOutputType == FName(TEXT("Trigger"));
	if (!OutObservation.bAllThreeChainsExact)
	{
		OutError = FirstChainError.IsEmpty()
			? TEXT("One or more of the three input-template chains did not resolve exactly.")
			: FirstChainError;
	}
	else if (!bPinnedOnPlayIdsValid)
	{
		OutError = TEXT("Pinned OnPlay graph-input NodeID or VertexID is malformed.");
	}
	else if (!OutObservation.bMatchesPinnedOnPlay)
	{
		OutError = TEXT("UE.Source.OnPlay resolved to a graph-input node/output that differs from the pinned NodeID or VertexID.");
	}
	else if (!OutObservation.bPlayDelayExact || !OutObservation.bFinishDelayExact
		|| OutObservation.PlayDelayNodeId == OutObservation.FinishDelayNodeId)
	{
		OutError = !OutObservation.bPlayDelayExact ? PlayDelayError
			: !OutObservation.bFinishDelayExact ? FinishDelayError
			: TEXT("Play and completion delay chains unexpectedly share one node.");
	}
	OutObservation.Classification = !OutObservation.bAllThreeChainsExact
		? TEXT("INPUT_TEMPLATE_CHAIN_FAILURE")
		: !OutObservation.bMatchesPinnedOnPlay
			? TEXT("PINNED_ON_PLAY_IDENTITY_MISMATCH")
			: OutObservation.bPlayDelayExact && OutObservation.bFinishDelayExact
				&& OutObservation.PlayDelayNodeId != OutObservation.FinishDelayNodeId
				? TEXT("THREE_INPUT_CHAINS_AND_TWO_TRIGGER_DELAYS_EXACT")
				: TEXT("TRIGGER_DELAY_CHAIN_FAILURE");
	const bool bQuerySucceeded =
		OutObservation.bAllThreeChainsExact && OutObservation.bMatchesPinnedOnPlay
		&& OutObservation.bPlayDelayExact && OutObservation.bFinishDelayExact
		&& OutObservation.PlayDelayNodeId != OutObservation.FinishDelayNodeId;

	OutObservation.bBuilderCleanedUp = Cleanup.Finish();
	if (!OutObservation.bBuilderCleanedUp)
	{
		OutError = OutError.IsEmpty()
			? TEXT("The transient MetaSound builder could not be removed from the document builder registry.")
			: OutError + TEXT(" Builder registry cleanup also failed.");
		return false;
	}

	return bQuerySucceeded;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	IM_AcousticQ5ConnectionProbe,
	"IceMoon.AcousticField.Q5.ConnectionSourceReadOnlyProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool IM_AcousticQ5ConnectionProbe::RunTest(const FString&)
{
	const FDateTime StartedUtc = FDateTime::UtcNow();
	const FString Nonce = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString RunId = FString::Printf(
		TEXT("%04d%02d%02dT%02d%02d%02dZ_%s"),
		StartedUtc.GetYear(),
		StartedUtc.GetMonth(),
		StartedUtc.GetDay(),
		StartedUtc.GetHour(),
		StartedUtc.GetMinute(),
		StartedUtc.GetSecond(),
		*Nonce.Left(12));
	const FString ReceiptDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/Q5"));
	const FString ReceiptPath = FPaths::Combine(ReceiptDirectory, TEXT("connection_source_probe_") + RunId + TEXT(".json"));

	TArray<IMAcousticQ5ConnectionProbePrivate::FIMQ5AssetState> Assets;
	Assets.SetNum(3);
	Assets[0].Name = TEXT("source");
	Assets[0].ObjectPath = IMAcousticQ5ConnectionProbePrivate::IMQ5SourceObjectPath;
	Assets[0].ExpectedSha1 = IMAcousticQ5ConnectionProbePrivate::IMQ5ExpectedSourceSha1;
	Assets[1].Name = TEXT("wave");
	Assets[1].ObjectPath = IMAcousticQ5ConnectionProbePrivate::IMQ5WaveObjectPath;
	Assets[1].ExpectedSha1 = IMAcousticQ5ConnectionProbePrivate::IMQ5ExpectedWaveSha1;
	Assets[2].Name = TEXT("environment");
	Assets[2].ObjectPath = IMAcousticQ5ConnectionProbePrivate::IMQ5EnvironmentObjectPath;
	Assets[2].ExpectedSha1 = IMAcousticQ5ConnectionProbePrivate::IMQ5ExpectedEnvironmentSha1;

	const FString AudioContentDirectory = FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins/IceMoonAcousticField/Content/Tests/Audio"));
	Assets[0].DiskPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(AudioContentDirectory, TEXT("MS_AcousticQ5Impulse.uasset")));
	Assets[1].DiskPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(AudioContentDirectory, TEXT("SW_AcousticQ5Impulse.uasset")));
	Assets[2].DiskPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(AudioContentDirectory, TEXT("MS_AcousticEnvironment.uasset")));

	FString Failure;
	IMAcousticQ5ConnectionProbePrivate::FIMQ5EditorState EditorBefore;
	IMAcousticQ5ConnectionProbePrivate::FIMQ5EditorState EditorAfter;
	const bool bHaveEditorBefore = IMAcousticQ5ConnectionProbePrivate::IMQ5CaptureEditorState(EditorBefore, Failure);
	bool bAssetFilesReadable = true;
	for (IMAcousticQ5ConnectionProbePrivate::FIMQ5AssetState& Asset : Assets)
	{
		FString HashError;
		if (!IMAcousticQ5ConnectionProbePrivate::IMQ5ReadSha1(Asset.DiskPath, Asset.Sha1Before, HashError))
		{
			bAssetFilesReadable = false;
			if (Failure.IsEmpty())
			{
				Failure = HashError;
			}
		}
		else if (Asset.Sha1Before != Asset.ExpectedSha1)
		{
			bAssetFilesReadable = false;
			if (Failure.IsEmpty())
			{
				Failure = FString::Printf(TEXT("Pinned %s package SHA-1 changed before the read-only probe."), *Asset.Name);
			}
		}
	}

	if (bAssetFilesReadable)
	{
		Assets[0].Object = LoadObject<UMetaSoundSource>(nullptr, *Assets[0].ObjectPath);
		Assets[1].Object = LoadObject<USoundWave>(nullptr, *Assets[1].ObjectPath);
		Assets[2].Object = LoadObject<UMetaSoundSource>(nullptr, *Assets[2].ObjectPath);
		for (IMAcousticQ5ConnectionProbePrivate::FIMQ5AssetState& Asset : Assets)
		{
			if (!Asset.Object || Asset.Object->GetPathName() != Asset.ObjectPath)
			{
				if (Failure.IsEmpty())
				{
					Failure = FString::Printf(TEXT("Pinned asset failed exact-path load: %s"), *Asset.ObjectPath);
				}
				break;
			}

			Asset.Package = Asset.Object->GetPackage();
			if (!Asset.Package)
			{
				if (Failure.IsEmpty())
				{
					Failure = FString::Printf(TEXT("Pinned asset has no package: %s"), *Asset.ObjectPath);
				}
				break;
			}
			Asset.bPackageDirtyBefore = Asset.Package->IsDirty();
			if (Asset.bPackageDirtyBefore && Failure.IsEmpty())
			{
				Failure = FString::Printf(TEXT("Pinned %s package was already dirty before observation."), *Asset.Name);
			}
		}
	}

	IMAcousticQ5ConnectionProbePrivate::FIMQ5ConnectionObservation Observation;
	bool bObservationComplete = false;
	if (Failure.IsEmpty())
	{
		bObservationComplete = IMAcousticQ5ConnectionProbePrivate::IMQ5ObserveIncomingEdge(
			Cast<UMetaSoundSource>(Assets[0].Object),
			Observation,
			Failure);
	}

	FString AfterStateError;
	const bool bHaveEditorAfter = IMAcousticQ5ConnectionProbePrivate::IMQ5CaptureEditorState(EditorAfter, AfterStateError);
	if (!bHaveEditorAfter && Failure.IsEmpty())
	{
		Failure = AfterStateError;
	}
	if (bHaveEditorBefore
		&& bHaveEditorAfter
		&& (EditorBefore.World != EditorAfter.World
			|| EditorBefore.MapPackage != EditorAfter.MapPackage
			|| EditorBefore.WorldPath != EditorAfter.WorldPath
			|| EditorBefore.MapPackagePath != EditorAfter.MapPackagePath
			|| EditorBefore.bMapDirty != EditorAfter.bMapDirty)
		&& Failure.IsEmpty())
	{
		Failure = TEXT("The active Editor world, map package, or map dirty state changed during observation.");
	}

	bool bAssetHashesUnchanged = true;
	bool bAssetPackagesUnchanged = true;
	for (IMAcousticQ5ConnectionProbePrivate::FIMQ5AssetState& Asset : Assets)
	{
		FString HashError;
		if (!IMAcousticQ5ConnectionProbePrivate::IMQ5ReadSha1(Asset.DiskPath, Asset.Sha1After, HashError))
		{
			bAssetHashesUnchanged = false;
			if (Failure.IsEmpty())
			{
				Failure = HashError;
			}
		}
		else if (Asset.Sha1After != Asset.Sha1Before
			|| Asset.Sha1After != Asset.ExpectedSha1)
		{
			bAssetHashesUnchanged = false;
			if (Failure.IsEmpty())
			{
				Failure = FString::Printf(TEXT("Pinned %s package bytes changed during observation."), *Asset.Name);
			}
		}

		if (Asset.Package)
		{
			Asset.bPackageDirtyAfter = Asset.Package->IsDirty();
			if (Asset.bPackageDirtyAfter != Asset.bPackageDirtyBefore)
			{
				bAssetPackagesUnchanged = false;
				if (Failure.IsEmpty())
				{
					Failure = FString::Printf(TEXT("Pinned %s package dirty state changed during observation."), *Asset.Name);
				}
			}
		}
		else
		{
			bAssetPackagesUnchanged = false;
		}
	}

	const bool bMapUnchanged =
		bHaveEditorBefore
		&& bHaveEditorAfter
		&& EditorBefore.World == EditorAfter.World
		&& EditorBefore.MapPackage == EditorAfter.MapPackage
		&& EditorBefore.WorldPath == EditorAfter.WorldPath
		&& EditorBefore.MapPackagePath == EditorAfter.MapPackagePath
		&& EditorBefore.bMapDirty == EditorAfter.bMapDirty;
	const bool bComplete =
		Failure.IsEmpty()
		&& bObservationComplete
		&& Observation.bAllThreeChainsExact
		&& Observation.bMatchesPinnedOnPlay
		&& Observation.bPlayDelayExact
		&& Observation.bFinishDelayExact
		&& Observation.PlayDelayNodeId != Observation.FinishDelayNodeId
		&& Observation.InputTemplateChains.Num() == 3
		&& Observation.bBuilderCleanedUp
		&& bAssetHashesUnchanged
		&& bAssetPackagesUnchanged
		&& bMapUnchanged;

	TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5ConnectionSourceProbe.v1"));
	Receipt->SetStringField(TEXT("run_id"), RunId);
	Receipt->SetStringField(TEXT("started_utc"), StartedUtc.ToIso8601());
	Receipt->SetStringField(TEXT("completed_utc"), FDateTime::UtcNow().ToIso8601());
	Receipt->SetStringField(TEXT("status"), bComplete ? TEXT("OBSERVATION_COMPLETE") : TEXT("FAIL_CLOSED"));
	Receipt->SetStringField(TEXT("q5_graph_status"), TEXT("FAIL_UNACCEPTED"));
	Receipt->SetStringField(TEXT("classification"), Observation.Classification);
	Receipt->SetStringField(TEXT("error"), Failure);
	Receipt->SetBoolField(TEXT("read_only_probe"), true);
	Receipt->SetBoolField(TEXT("build_called"), false);
	Receipt->SetBoolField(TEXT("save_called"), false);
	Receipt->SetBoolField(TEXT("connect_or_disconnect_called"), false);
	Receipt->SetBoolField(TEXT("default_or_import_mutation_called"), false);
	Receipt->SetBoolField(TEXT("source_graph_accepted"), false);
	Receipt->SetBoolField(TEXT("observation_complete"), bObservationComplete);
	Receipt->SetBoolField(TEXT("all_three_chains_exact"), Observation.bAllThreeChainsExact);
	Receipt->SetBoolField(TEXT("play_delay_chain_exact"), Observation.bPlayDelayExact);
	Receipt->SetBoolField(TEXT("completion_delay_chain_exact"), Observation.bFinishDelayExact);
	Receipt->SetStringField(TEXT("play_delay_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.PlayDelayNodeId));
	Receipt->SetStringField(TEXT("completion_delay_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.FinishDelayNodeId));
	Receipt->SetBoolField(TEXT("asset_hashes_unchanged"), bAssetHashesUnchanged);
	Receipt->SetBoolField(TEXT("asset_package_dirty_states_unchanged"), bAssetPackagesUnchanged);
	Receipt->SetBoolField(TEXT("map_and_editor_world_unchanged"), bMapUnchanged);

	TSharedRef<FJsonObject> AssetReceipts = MakeShared<FJsonObject>();
	for (const IMAcousticQ5ConnectionProbePrivate::FIMQ5AssetState& Asset : Assets)
	{
		AssetReceipts->SetObjectField(Asset.Name, IMAcousticQ5ConnectionProbePrivate::IMQ5MakeAssetReceipt(Asset));
	}
	Receipt->SetObjectField(TEXT("assets"), AssetReceipts);
	Receipt->SetObjectField(
		TEXT("editor_state"),
		IMAcousticQ5ConnectionProbePrivate::IMQ5MakeEditorStateReceipt(EditorBefore, EditorAfter, bHaveEditorBefore, bHaveEditorAfter));

	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.TargetNodeId));
	Target->SetStringField(TEXT("vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.TargetVertexId));
	Target->SetStringField(TEXT("pin_name"), Observation.TargetPinName.ToString());
	Target->SetStringField(TEXT("pin_type"), Observation.TargetPinType.ToString());
	Target->SetBoolField(TEXT("input_occupied"), Observation.bTargetInputOccupied);
	Receipt->SetObjectField(TEXT("target_input"), Target);

	TSharedRef<FJsonObject> Incoming = MakeShared<FJsonObject>();
	Incoming->SetBoolField(TEXT("source_resolved"), Observation.bIncomingSourceResolved);
	Incoming->SetStringField(TEXT("source_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.SourceNodeId));
	Incoming->SetStringField(TEXT("source_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.SourceVertexId));
	Incoming->SetStringField(TEXT("source_node_name"), Observation.SourceNodeName);
	Incoming->SetStringField(TEXT("source_pin_name"), Observation.SourcePinName.ToString());
	Incoming->SetStringField(TEXT("source_pin_type"), Observation.SourcePinType.ToString());
	Incoming->SetBoolField(TEXT("exact_edge_confirmed"), Observation.bExactEdgeConfirmed);
	Incoming->SetBoolField(TEXT("matches_pinned_on_play"), Observation.bMatchesPinnedOnPlay);
	Incoming->SetBoolField(TEXT("graph_on_play_input_found"), Observation.bGraphOnPlayInputFound);
	Incoming->SetStringField(TEXT("graph_on_play_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.GraphOnPlayNodeId));
	Incoming->SetStringField(TEXT("graph_on_play_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Observation.GraphOnPlayVertexId));
	Incoming->SetBoolField(TEXT("matches_graph_on_play_input"), Observation.bMatchesGraphOnPlayInput);
	Incoming->SetBoolField(TEXT("builder_cleaned_up"), Observation.bBuilderCleanedUp);
	Incoming->SetStringField(TEXT("builder_query"), TEXT("GetConstBuilder().FindNodeOutputConnectedToNodeInput"));
	Incoming->SetStringField(TEXT("exact_edge_query"), TEXT("NodesAreConnected(source output handle, target input handle)"));
	Receipt->SetObjectField(TEXT("incoming_edge"), Incoming);

	TArray<TSharedPtr<FJsonValue>> ChainReceipts;
	for (const IMAcousticQ5ConnectionProbePrivate::FIMQ5InputTemplateChainObservation& Chain : Observation.InputTemplateChains)
	{
		TSharedRef<FJsonObject> ChainReceipt = MakeShared<FJsonObject>();
		ChainReceipt->SetStringField(TEXT("name"), Chain.Name);
		ChainReceipt->SetStringField(TEXT("expected_data_type"), Chain.ExpectedDataType.ToString());
		ChainReceipt->SetStringField(TEXT("target_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.TargetNodeId));
		ChainReceipt->SetStringField(TEXT("target_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.TargetVertexId));
		ChainReceipt->SetStringField(TEXT("target_pin_name"), Chain.TargetPinName.ToString());
		ChainReceipt->SetStringField(TEXT("target_pin_type"), Chain.TargetPinType.ToString());
		ChainReceipt->SetBoolField(TEXT("target_input_occupied"), Chain.bTargetInputOccupied);
		ChainReceipt->SetStringField(TEXT("template_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.TemplateNodeId));
		ChainReceipt->SetStringField(TEXT("template_node_name"), Chain.TemplateNodeName);
		ChainReceipt->SetStringField(TEXT("template_output_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.TemplateOutputVertexId));
		ChainReceipt->SetStringField(TEXT("template_output_name"), Chain.TemplateOutputName.ToString());
		ChainReceipt->SetStringField(TEXT("template_output_type"), Chain.TemplateOutputType.ToString());
		ChainReceipt->SetBoolField(TEXT("template_source_resolved"), Chain.bTemplateSourceResolved);
		ChainReceipt->SetBoolField(TEXT("template_dependency_found"), Chain.bTemplateDependencyFound);
		ChainReceipt->SetBoolField(TEXT("expected_input_template_dependency"), Chain.bExpectedTemplateDependency);
		ChainReceipt->SetStringField(TEXT("template_dependency_namespace"), Chain.TemplateDependencyNamespace.ToString());
		ChainReceipt->SetStringField(TEXT("template_dependency_name"), Chain.TemplateDependencyName.ToString());
		ChainReceipt->SetStringField(TEXT("template_dependency_variant"), Chain.TemplateDependencyVariant.ToString());
		ChainReceipt->SetStringField(TEXT("template_input_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.TemplateNodeId));
		ChainReceipt->SetStringField(TEXT("template_input_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.TemplateInputVertexId));
		ChainReceipt->SetStringField(TEXT("template_input_name"), Chain.TemplateInputName.ToString());
		ChainReceipt->SetStringField(TEXT("template_input_type"), Chain.TemplateInputType.ToString());
		ChainReceipt->SetBoolField(TEXT("template_input_occupied"), Chain.bTemplateInputOccupied);
		ChainReceipt->SetStringField(TEXT("upstream_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.UpstreamNodeId));
		ChainReceipt->SetStringField(TEXT("upstream_node_name"), Chain.UpstreamNodeName);
		ChainReceipt->SetStringField(TEXT("upstream_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.UpstreamVertexId));
		ChainReceipt->SetStringField(TEXT("upstream_pin_name"), Chain.UpstreamPinName.ToString());
		ChainReceipt->SetStringField(TEXT("upstream_pin_type"), Chain.UpstreamPinType.ToString());
		ChainReceipt->SetBoolField(TEXT("upstream_graph_input_resolved"), Chain.bUpstreamGraphInputResolved);
		ChainReceipt->SetBoolField(TEXT("graph_input_node_found"), Chain.bGraphInputNodeFound);
		ChainReceipt->SetStringField(TEXT("graph_input_name"), Chain.GraphInputName.ToString());
		ChainReceipt->SetStringField(TEXT("graph_input_node_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.GraphInputNodeId));
		ChainReceipt->SetStringField(TEXT("graph_input_vertex_id"), IMAcousticQ5ConnectionProbePrivate::IMQ5Guid(Chain.GraphInputVertexId));
		ChainReceipt->SetStringField(TEXT("graph_input_output_name"), Chain.GraphInputOutputName.ToString());
		ChainReceipt->SetStringField(TEXT("graph_input_output_type"), Chain.GraphInputOutputType.ToString());
		ChainReceipt->SetBoolField(TEXT("graph_input_identity_matches_upstream"), Chain.bGraphInputIdentityMatches);
		ChainReceipt->SetBoolField(TEXT("graph_input_to_template_input_exact"), Chain.bGraphInputToTemplateInputExact);
		ChainReceipt->SetBoolField(TEXT("template_output_to_target_exact"), Chain.bTemplateOutputToTargetExact);
		ChainReceipt->SetBoolField(TEXT("chain_exact"), Chain.bChainExact);
		ChainReceipt->SetStringField(TEXT("error"), Chain.Error);
		ChainReceipts.Add(MakeShared<FJsonValueObject>(ChainReceipt));
	}
	Receipt->SetArrayField(TEXT("input_template_chains"), ChainReceipts);

	IFileManager::Get().MakeDirectory(*ReceiptDirectory, true);
	const bool bReceiptWritten = IMAcousticQ5ConnectionProbePrivate::IMQ5WriteReceipt(ReceiptPath, Receipt);
	if (!bReceiptWritten)
	{
		const FString Message = FString::Printf(TEXT("Could not write Q5 probe receipt: %s"), *ReceiptPath);
		AddError(Message);
		UE_LOG(LogTemp, Error, TEXT("IMLogs AcousticQ5ConnectionProbe FAIL run_id=%s receipt=UNAVAILABLE q5_graph_status=FAIL_UNACCEPTED reason=%s"),
			*RunId, *Message);
		return false;
	}

	if (bComplete)
	{
		const FString Message = FString::Printf(
			TEXT("Read-only incoming-edge observation recorded as %s; Q5 graph remains unaccepted."),
			*Observation.Classification);
		AddInfo(Message);
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticQ5ConnectionProbe OBSERVATION_COMPLETE run_id=%s receipt=%s classification=%s"),
			*RunId, *ReceiptPath, *Observation.Classification);
		return true;
	}

	const FString ErrorMessage = Failure.IsEmpty()
		? TEXT("The read-only Q5 connection observation did not satisfy all fail-closed checks.")
		: Failure;
	AddError(ErrorMessage);
	UE_LOG(LogTemp, Error, TEXT("IMLogs AcousticQ5ConnectionProbe FAIL run_id=%s receipt=%s classification=%s q5_graph_status=FAIL_UNACCEPTED reason=%s"),
		*RunId, *ReceiptPath, *Observation.Classification, *ErrorMessage);
	return false;
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
