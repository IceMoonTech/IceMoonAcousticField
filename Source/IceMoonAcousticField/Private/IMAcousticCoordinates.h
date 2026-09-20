#pragma once

#include "CoreMinimal.h"
#include <phonon.h>

inline IPLVector3 IMToSDKDirection(const FVector& V) { return {float(V.Y),float(V.Z),float(-V.X)}; }
inline IPLVector3 IMToSDKPosition(const FVector& P,const FVector& Origin) { return IMToSDKDirection((P-Origin)*0.01); }
inline FVector IMFromSDKPosition(const IPLVector3& P,const FVector& Origin) { return Origin+FVector(-P.z,P.x,P.y)*100.0; }
inline IPLCoordinateSpace3 IMToSDKSpace(const FTransform& T,const FVector& Origin)
{
    return {IMToSDKDirection(T.GetUnitAxis(EAxis::Y)),IMToSDKDirection(T.GetUnitAxis(EAxis::Z)),
        IMToSDKDirection(T.GetUnitAxis(EAxis::X)),IMToSDKPosition(T.GetLocation(),Origin)};
}
