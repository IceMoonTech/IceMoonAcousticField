#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticCoordinates.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticCoordinateMapping,"IceMoon.AcousticField.W0.CoordinateMapping",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticCoordinateMapping::RunTest(const FString&)
{
    bool Success=true;
    const auto CheckSDKVector=[this,&Success](const TCHAR* Name,const IPLVector3& Actual,const IPLVector3& Expected)
    {
        const bool Pass=FMath::IsNearlyEqual(Actual.x,Expected.x,1.0e-6f)
            &&FMath::IsNearlyEqual(Actual.y,Expected.y,1.0e-6f)
            &&FMath::IsNearlyEqual(Actual.z,Expected.z,1.0e-6f);
        Success=Pass&&Success;
        if(!Pass)AddError(FString::Printf(TEXT("%s expected=(%g,%g,%g) actual=(%g,%g,%g)"),Name,
            Expected.x,Expected.y,Expected.z,Actual.x,Actual.y,Actual.z));
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticCoordinateMapping %s expected=(%g,%g,%g) actual=(%g,%g,%g) %s"),Name,
            Expected.x,Expected.y,Expected.z,Actual.x,Actual.y,Actual.z,Pass?TEXT("PASS"):TEXT("FAIL"));
    };
    CheckSDKVector(TEXT("axis_x_to_sdk"),ToSDKDirection(FVector(1,0,0)),IPLVector3(0,0,-1));
    CheckSDKVector(TEXT("axis_y_to_sdk"),ToSDKDirection(FVector(0,1,0)),IPLVector3(1,0,0));
    CheckSDKVector(TEXT("axis_z_to_sdk"),ToSDKDirection(FVector(0,0,1)),IPLVector3(0,1,0));

    const FVector Origin(1000,2000,300);
    const FVector P=Origin+FVector(100,200,-400);
    // (P-Origin)*0.01 = (1,2,-4)m; IMToSDKDirection maps (x,y,z) to (y,z,-x), hence (2,-4,-1)m.
    const IPLVector3 Position=ToSDKPosition(P,Origin);
    CheckSDKVector(TEXT("position_cm_to_m"),Position,IPLVector3(2,-4,-1));

    // UE FVector is double-based while IPLVector3 is float-based; never memcpy across these layouts.
    // 0.05 cm covers the worst float round-trip quantization for these kilometre-scale points.
    constexpr double RoundTripToleranceCm=0.05;
    const TArray<FVector> Points={
        FVector::ZeroVector,
        FVector(100,-200,300),
        FVector(100000,-200000,300000),
        FVector(-1234.567,7890.123,-3456.789)};
    for(int32 Index=0;Index<Points.Num();++Index)
    {
        const FVector RoundTrip=FromSDKPosition(ToSDKPosition(Points[Index],FVector::ZeroVector),FVector::ZeroVector);
        const double Error=(RoundTrip-Points[Index]).GetAbsMax();
        const bool Pass=Error<=RoundTripToleranceCm;Success=Pass&&Success;
        if(!Pass)AddError(FString::Printf(TEXT("roundtrip[%d] error_cm=%g tolerance_cm=%g"),Index,Error,RoundTripToleranceCm));
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticCoordinateMapping roundtrip[%d] input=(%g,%g,%g) output=(%g,%g,%g) error_cm=%g tolerance_cm=%g %s"),
            Index,Points[Index].X,Points[Index].Y,Points[Index].Z,RoundTrip.X,RoundTrip.Y,RoundTrip.Z,Error,RoundTripToleranceCm,
            Pass?TEXT("PASS"):TEXT("FAIL"));
    }
    UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticCoordinateMapping %s"),Success?TEXT("PASS"):TEXT("FAIL"));
    UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticCoordinateMapping %s"),Success?TEXT("PASS"):TEXT("FAIL"));
    UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));
    return Success;
}
#endif
