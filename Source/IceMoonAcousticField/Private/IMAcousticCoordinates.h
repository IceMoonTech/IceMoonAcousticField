#pragma once

#include "CoreMinimal.h"
#include <phonon.h>

inline IPLVector3 ToSDKDirection(const FVector& V) { return {float(V.Y),float(V.Z),float(-V.X)}; }
inline IPLVector3 ToSDKPosition(const FVector& P,const FVector& Origin) { return ToSDKDirection((P-Origin)*0.01); }
inline FVector FromSDKPosition(const IPLVector3& P,const FVector& Origin) { return Origin+FVector(-P.z,P.x,P.y)*100.0; }
inline IPLCoordinateSpace3 ToSDKSpace(const FTransform& T,const FVector& Origin)
{
    return {ToSDKDirection(T.GetUnitAxis(EAxis::Y)),ToSDKDirection(T.GetUnitAxis(EAxis::Z)),
        ToSDKDirection(T.GetUnitAxis(EAxis::X)),ToSDKPosition(T.GetLocation(),Origin)};
}
