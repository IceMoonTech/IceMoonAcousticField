#include "IMAcousticAudioRenderer.h"
#include "IMAcousticPresenceParameters.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundParamHelper.h"
#include "MetasoundPrimitives.h"
#include "MetasoundTrigger.h"
#include "Tests/IMAcousticAuditionCapture.h"
#include <cmath>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

#define LOCTEXT_NAMESPACE "IMAcousticPresence"

namespace IMAcousticPresenceMetaSoundNodePrivate
{
using namespace Metasound;

class FIMAcousticPresenceOperator final : public TExecutableOperator<FIMAcousticPresenceOperator>
{
public:
	static const FVertexInterface& GetVertexInterface()
	{
		static const FVertexInterface Interface(
			FInputVertexInterface(
				TInputDataVertex<FAudioBuffer>(FName(TEXT("Mono")), FDataVertexMetadata{}),
				TInputDataVertex<FAudioBuffer>(FName(TEXT("Wave/Pitch")), FDataVertexMetadata{}),
				TInputDataVertex<FTrigger>(FName(TEXT("Contact")), FDataVertexMetadata{}),
				TInputDataVertex<float>(FName(TEXT("Equal Loudness")), FDataVertexMetadata{}, 0.f),
				TInputDataVertex<float>(FName(TEXT("Compensation")), FDataVertexMetadata{}, 1.f),
				TInputDataVertex<float>(FName(TEXT("Enabled")), FDataVertexMetadata{}, 1.f),
				TInputDataVertex<float>(FName(TEXT("Impact")), FDataVertexMetadata{}, .35f),
				TInputDataVertex<float>(FName(TEXT("Material")), FDataVertexMetadata{}, 0.f),
				TInputDataVertex<float>(FName(TEXT("Enclosure")), FDataVertexMetadata{}, 0.f)),
			FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(FName(TEXT("Out")), FDataVertexMetadata{})));
		return Interface;
	}

	static const FNodeClassMetadata& GetNodeInfo()
	{
		static const FNodeClassMetadata Info = []
		{
			FNodeClassMetadata Metadata;
			Metadata.ClassName = {TEXT("IM"), TEXT("Acoustic Presence"), TEXT("Mono")};
			Metadata.MajorVersion = 2;
			Metadata.MinorVersion = 0;
			Metadata.DisplayName = LOCTEXT("Title", "夸张临场感");
			Metadata.Description = LOCTEXT("Description", "只保留接触事件登记与等响度补偿增益；不再改写素材音色、不做自制混响。Impact/Material/Enclosure 为兼容既有图的占位输入。");
			Metadata.Author = TEXT("IceMoon");
			Metadata.DefaultInterface = GetVertexInterface();
			return Metadata;
		}();
		return Info;
	}

	static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& Params, FBuildResults&)
	{
		return MakeUnique<FIMAcousticPresenceOperator>(Params);
	}

	explicit FIMAcousticPresenceOperator(const FBuildOperatorParams& Params)
		: Mono(Params.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Mono"), Params.OperatorSettings)),
		WavePitch(Params.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Wave/Pitch"), Params.OperatorSettings)),
		Contact(Params.InputData.GetOrCreateDefaultDataReadReference<FTrigger>(TEXT("Contact"), Params.OperatorSettings)),
		EqualLoudness(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Equal Loudness"), Params.OperatorSettings)),
		Compensation(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Compensation"), Params.OperatorSettings)),
		Enabled(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Enabled"), Params.OperatorSettings)),
		Impact(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Impact"), Params.OperatorSettings)),
		Material(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Material"), Params.OperatorSettings)),
		Enclosure(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Enclosure"), Params.OperatorSettings)),
		Output(FAudioBufferWriteRef::CreateNew(Params.OperatorSettings)),
			Uncompensated(FAudioBufferWriteRef::CreateNew(Params.OperatorSettings)),
		LegacyOutput(FAudioBufferWriteRef::CreateNew(Params.OperatorSettings))
	{
		Reset(Params);
	}

	void Reset(const IOperator::FResetParams& Params)
	{
		SampleRate = int32(Params.OperatorSettings.GetSampleRate());
		CurrentGain = 1.f;
		Output->Zero();
	}

	void BindInputs(FInputVertexInterfaceData& Data) override
	{
		Data.BindReadVertex(TEXT("Mono"), Mono);
		Data.BindReadVertex(TEXT("Wave/Pitch"), WavePitch);
		Data.BindReadVertex(TEXT("Contact"), Contact);
		Data.BindReadVertex(TEXT("Equal Loudness"), EqualLoudness);
		Data.BindReadVertex(TEXT("Compensation"), Compensation);
		Data.BindReadVertex(TEXT("Enabled"), Enabled);
		Data.BindReadVertex(TEXT("Impact"), Impact);
		Data.BindReadVertex(TEXT("Material"), Material);
		Data.BindReadVertex(TEXT("Enclosure"), Enclosure);
	}

	void BindOutputs(FOutputVertexInterfaceData& Data) override { Data.BindReadVertex(TEXT("Out"), Output); }

	void Execute()
	{
		Contact->ExecuteBlock(
			[this](int32 StartFrame, int32 EndFrame) { ProcessRange(StartFrame, EndFrame); },
			[this](int32 StartFrame, int32 EndFrame)
			{
				IMAcousticAuditionCapture::Contact(StartFrame);
				ProcessRange(StartFrame, EndFrame);
			});
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::WavePitch, WavePitch->GetData(), nullptr, WavePitch->Num(), SampleRate);
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::WaveGain, Mono->GetData(), nullptr, Mono->Num(), SampleRate);
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::LegacyPresence, LegacyOutput->GetData(), nullptr, LegacyOutput->Num(), SampleRate);
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::Presence, Output->GetData(), nullptr, Output->Num(), SampleRate);
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::UncompensatedPresence, Uncompensated->GetData(), nullptr, Uncompensated->Num(), SampleRate);
	}

private:
	void ProcessRange(int32 StartFrame, int32 EndFrame)
	{
		const float TargetGain = *Enabled > .5f && *EqualLoudness > .5f && FMath::IsFinite(*Compensation) && *Compensation > 0.f ? *Compensation : 1.f;
		for (int32 Frame = StartFrame; Frame < EndFrame; ++Frame)
		{
			CurrentGain += FMath::Clamp(TargetGain - CurrentGain, -1.f / (.04f * SampleRate), 1.f / (.04f * SampleRate));
			// Sample character comes from the recorded wave. The node's only job is the equal-loudness gain,
			// so the exaggerated and physical paths feed the same shared spatial/convolution chain.
			const float Input = FMath::IsFinite(Mono->GetData()[Frame]) ? Mono->GetData()[Frame] : 0.f;
			Uncompensated->GetData()[Frame] = Input;
			Output->GetData()[Frame] = Input * CurrentGain;
			LegacyOutput->GetData()[Frame] = Input; // The retired coloured stage is gone; the capture stage stays for tooling.
		}
	}
	FAudioBufferReadRef Mono, WavePitch;
	FTriggerReadRef Contact;
	FFloatReadRef EqualLoudness, Compensation, Enabled, Impact, Material, Enclosure;
	FAudioBufferWriteRef Output, Uncompensated, LegacyOutput;
	int32 SampleRate = 48000;
	float CurrentGain = 1.f;
};

using FIMAcousticPresenceNode = TNodeFacade<FIMAcousticPresenceOperator>;
METASOUND_REGISTER_NODE(FIMAcousticPresenceNode)

class FIMAcousticSelfNearfieldOperator final : public TExecutableOperator<FIMAcousticSelfNearfieldOperator>
{
public:
	static const FVertexInterface& GetVertexInterface()
	{
		static const FVertexInterface Interface(FInputVertexInterface(
			TInputDataVertex<FAudioBuffer>(FName(TEXT("Mono")), FDataVertexMetadata{}),
			TInputDataVertex<FAudioBuffer>(FName(TEXT("Spatial Left")), FDataVertexMetadata{}),
			TInputDataVertex<FAudioBuffer>(FName(TEXT("Spatial Right")), FDataVertexMetadata{}),
			TInputDataVertex<float>(FName(TEXT("Enabled")), FDataVertexMetadata{}, 0.f),
			TInputDataVertex<float>(FName(TEXT("Pan")), FDataVertexMetadata{}, 0.f)),
			FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(FName(TEXT("Left")), FDataVertexMetadata{}),
			TOutputDataVertex<FAudioBuffer>(FName(TEXT("Right")), FDataVertexMetadata{})));
		return Interface;
	}
	static const FNodeClassMetadata& GetNodeInfo()
	{
		static const FNodeClassMetadata Info = []
		{
			FNodeClassMetadata Metadata;
			Metadata.ClassName = {TEXT("IM"), TEXT("Acoustic Self Nearfield"), TEXT("Stereo")};
			Metadata.MajorVersion = 1;
			Metadata.MinorVersion = 0;
			Metadata.DisplayName = LOCTEXT("SelfNearfield", "自身脚步近场直达");
			Metadata.Description = LOCTEXT("SelfNearfieldDescription", "直通 HRTF 空间直达声，不再替换成近中置声像；近场只在声源节点内去掉距离衰减。Mono/Enabled/Pan 为兼容既有图的占位输入。");
			Metadata.Author = TEXT("IceMoon");
			Metadata.DefaultInterface = GetVertexInterface();
			return Metadata;
		}();
		return Info;
	}
	static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& Params, FBuildResults&)
	{
		return MakeUnique<FIMAcousticSelfNearfieldOperator>(Params);
	}
	explicit FIMAcousticSelfNearfieldOperator(const FBuildOperatorParams& Params)
		: Mono(Params.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Mono"), Params.OperatorSettings)),
		SpatialLeft(Params.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Spatial Left"), Params.OperatorSettings)),
		SpatialRight(Params.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Spatial Right"), Params.OperatorSettings)),
		Enabled(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Enabled"), Params.OperatorSettings)),
		Pan(Params.InputData.GetOrCreateDefaultDataReadReference<float>(TEXT("Pan"), Params.OperatorSettings)),
		Left(FAudioBufferWriteRef::CreateNew(Params.OperatorSettings)), Right(FAudioBufferWriteRef::CreateNew(Params.OperatorSettings))
	{
		SampleRate = int32(Params.OperatorSettings.GetSampleRate());
	}
	void BindInputs(FInputVertexInterfaceData& Data) override
	{
		Data.BindReadVertex(TEXT("Mono"), Mono);
		Data.BindReadVertex(TEXT("Spatial Left"), SpatialLeft);
		Data.BindReadVertex(TEXT("Spatial Right"), SpatialRight);
		Data.BindReadVertex(TEXT("Enabled"), Enabled);
		Data.BindReadVertex(TEXT("Pan"), Pan);
	}
	void BindOutputs(FOutputVertexInterfaceData& Data) override
	{
		Data.BindReadVertex(TEXT("Left"), Left);
		Data.BindReadVertex(TEXT("Right"), Right);
	}
	void Execute()
	{
		// The spatial direct sound is the output. A near-centre pan used to replace it here and removed
		// the left/right cue; direction now stays the HRTF solution (optionally azimuth-scaled in the source node).
		FMemory::Memcpy(Left->GetData(), SpatialLeft->GetData(), Left->Num() * sizeof(float));
		FMemory::Memcpy(Right->GetData(), SpatialRight->GetData(), Right->Num() * sizeof(float));
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::Nearfield, Left->GetData(), Right->GetData(), Left->Num(), SampleRate);
	}
private:
	FAudioBufferReadRef Mono, SpatialLeft, SpatialRight;
	FFloatReadRef Enabled, Pan;
	FAudioBufferWriteRef Left, Right;
	int32 SampleRate = 48000;
};

using FIMAcousticSelfNearfieldNode = TNodeFacade<FIMAcousticSelfNearfieldOperator>;
METASOUND_REGISTER_NODE(FIMAcousticSelfNearfieldNode)
}

#undef LOCTEXT_NAMESPACE

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPresenceDirectShapingTest, "IceMoon.AcousticField.Presence.Corrections.DirectShaping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticPresenceDirectShapingTest::RunTest(const FString&)
{
	// Strength 1 is the accepted exaggerated version; the checks below run on it, the mapping checks at the end on other strengths.
	const float Scale = FIMAcousticPresenceParameters::DirectAzimuthScaleAt(1.f);
	const float Limit = FMath::DegreesToRadians(FIMAcousticPresenceParameters::DirectAzimuthLimitDegreesAt(1.f));
	auto Direction = [](float AzimuthDegrees, float ElevationDegrees)
	{
		const float Azimuth = FMath::DegreesToRadians(AzimuthDegrees), Elevation = FMath::DegreesToRadians(ElevationDegrees);
		return IPLVector3{FMath::Cos(Elevation) * FMath::Sin(Azimuth), FMath::Sin(Elevation), -FMath::Cos(Elevation) * FMath::Cos(Azimuth)};
	};
	auto AzimuthDegrees = [](const IPLVector3& Value) { return FMath::RadiansToDegrees(FMath::Atan2(Value.x, -Value.z)); };
	const IPLVector3 Front{0.f, 0.f, -1.f};
	const float Lateral = FMath::DegreesToRadians(2.3f);
	const IPLVector3 FrontShaped = FIMAcousticDirectShaping::ScaleAzimuth(Front, Scale, 0.f, Limit);
	TestTrue(TEXT("Straight ahead stays straight ahead"), FMath::IsNearlyZero(FrontShaped.x, 1.e-6f) && FMath::IsNearlyEqual(FrontShaped.z, -1.f, 1.e-6f));
	TestTrue(TEXT("Without a lateral offset the direction is unchanged"),
		FMath::IsNearlyEqual(AzimuthDegrees(FIMAcousticDirectShaping::ScaleAzimuth(Direction(5.f, 0.f), Scale, 0.f, Limit)), 5.f, .01f));
	const IPLVector3 Right = FIMAcousticDirectShaping::ScaleAzimuth(Direction(2.3f, -10.f), Scale, Lateral, Limit);
	const IPLVector3 Left = FIMAcousticDirectShaping::ScaleAzimuth(Direction(-2.3f, -10.f), Scale, -Lateral, Limit);
	TestTrue(TEXT("The foot's lateral azimuth is multiplied by the global scale, mirrored per side"),
		FMath::IsNearlyEqual(AzimuthDegrees(Right), 2.3f * Scale, .01f) && FMath::IsNearlyEqual(AzimuthDegrees(Left), -2.3f * Scale, .01f));
	TestTrue(TEXT("A whole-direction offset (listener swing) is added once, not multiplied"),
		FMath::IsNearlyEqual(AzimuthDegrees(FIMAcousticDirectShaping::ScaleAzimuth(Direction(4.3f, 0.f), Scale, Lateral, Limit)), 2.f + 2.3f * Scale, .01f));
	TestTrue(TEXT("Elevation and unit length are preserved"),
		FMath::IsNearlyEqual(Right.y, FMath::Sin(FMath::DegreesToRadians(-10.f)), 1.e-5f)
		&& FMath::IsNearlyEqual(Right.x * Right.x + Right.y * Right.y + Right.z * Right.z, 1.f, 1.e-5f));
	TestTrue(TEXT("Scaled azimuth is limited"),
		FMath::IsNearlyEqual(AzimuthDegrees(FIMAcousticDirectShaping::ScaleAzimuth(Direction(10.f, 0.f), Scale, FMath::DegreesToRadians(10.f), Limit)),
			FIMAcousticPresenceParameters::DirectAzimuthLimitDegrees, .01f));
	TestTrue(TEXT("An already wider azimuth is never pulled back toward the front"),
		FMath::IsNearlyEqual(AzimuthDegrees(FIMAcousticDirectShaping::ScaleAzimuth(Direction(70.f, 0.f), Scale, Lateral, Limit)), 70.f, .01f));
	const IPLVector3 Behind{0.3f, 0.f, 1.f};
	const IPLVector3 BehindShaped = FIMAcousticDirectShaping::ScaleAzimuth(Behind, Scale, Lateral, Limit);
	TestTrue(TEXT("Sources behind the listener are unchanged"), BehindShaped.x == Behind.x && BehindShaped.z == Behind.z);
	const IPLVector3 Invalid{NAN, 0.f, -1.f};
	TestTrue(TEXT("Invalid input is returned unchanged"), !FMath::IsFinite(FIMAcousticDirectShaping::ScaleAzimuth(Invalid, Scale, Lateral, Limit).x));

	using FParameters = FIMAcousticPresenceParameters;
	TestTrue(TEXT("Strength 1 reproduces the accepted values exactly"),
		FParameters::DirectAzimuthScaleAt(1.f) == FParameters::DirectAzimuthScale && FParameters::DirectAzimuthLimitDegreesAt(1.f) == FParameters::DirectAzimuthLimitDegrees
		&& FParameters::WetSendExtraDbAt(1.f) == FParameters::WetSendExtraDb && FParameters::WetSideGainAt(1.f) == FParameters::WetSideGain);
	TestTrue(TEXT("Strength 0 is the physical path: scale 1, no extra wet, side gain 1"),
		FParameters::DirectAzimuthScaleAt(0.f) == 1.f && FParameters::WetSendExtraDbAt(0.f) == 0.f && FParameters::WetSideGainAt(0.f) == 1.f);
	// Strength 2 is the upper end of the accepted mapping (literals on purpose, independent of the constants): the wet send gains 6 dB per unit
	// of strength, the side gain 0.8 per unit, the azimuth scale 19 per unit, and the limit opens linearly from 60 degrees at 1 to 90 degrees at 2.
	TestTrue(TEXT("Strength 2: wet send +12 dB, wet side gain 2.6, azimuth scale 39, azimuth limit 90 degrees"),
		FMath::IsNearlyEqual(FParameters::WetSendExtraDbAt(2.f), 12.f, 1.e-4f) && FMath::IsNearlyEqual(FParameters::WetSideGainAt(2.f), 2.6f, 1.e-4f)
		&& FMath::IsNearlyEqual(FParameters::DirectAzimuthScaleAt(2.f), 39.f, 1.e-4f) && FMath::IsNearlyEqual(FParameters::DirectAzimuthLimitDegreesAt(2.f), 90.f, 1.e-4f));
	const float PhysicalScale = FParameters::DirectAzimuthScaleAt(0.f), PhysicalLimit = FMath::DegreesToRadians(FParameters::DirectAzimuthLimitDegreesAt(0.f));
	for (const float Degrees : {-80.f, -30.f, -2.3f, 0.f, 2.3f, 10.f, 45.f, 75.f})
	{
		const IPLVector3 Input = Direction(Degrees, -10.f), Output = FIMAcousticDirectShaping::ScaleAzimuth(Input, PhysicalScale, Lateral, PhysicalLimit);
		TestTrue(FString::Printf(TEXT("Strength 0 returns the direction bit for bit (%.1f degrees)"), Degrees), Output.x == Input.x && Output.y == Input.y && Output.z == Input.z);
	}
	float PreviousAzimuth = -1.f, PreviousLimit = -1.f;
	for (int32 Step = 0; Step <= 20; ++Step)
	{
		const float Strength = Step * .1f;
		const float Shaped = AzimuthDegrees(FIMAcousticDirectShaping::ScaleAzimuth(Direction(2.3f, 0.f), FParameters::DirectAzimuthScaleAt(Strength), Lateral,
			FMath::DegreesToRadians(FParameters::DirectAzimuthLimitDegreesAt(Strength))));
		TestTrue(FString::Printf(TEXT("Azimuth is monotonic in strength (%.1f)"), Strength), Shaped >= PreviousAzimuth - 1.e-4f && FParameters::DirectAzimuthLimitDegreesAt(Strength) >= PreviousLimit);
		PreviousAzimuth = Shaped;
		PreviousLimit = FParameters::DirectAzimuthLimitDegreesAt(Strength);
	}
	TestTrue(TEXT("The strength range is clamped and non-finite input falls back to 1"),
		FParameters::ClampStrength(-1.f) == 0.f && FParameters::ClampStrength(5.f) == FParameters::StrengthMax && FParameters::ClampStrength(NAN) == 1.f);
	return !HasAnyErrors();
}
#endif
