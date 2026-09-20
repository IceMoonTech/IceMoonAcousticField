#include "IMAcousticSDKContext.h"

namespace
{
struct IM_AcousticSDKContextState
{
    IPLContext Context = nullptr;
    IM_AcousticSDKContextState()
    {
        IPLContextSettings Settings{};
        Settings.version = STEAMAUDIO_VERSION;
        // SDK 4.8.1 Context construction rewrites global ippSetCpuFeatures and
        // allocator/log state. Multiple contexts with differing SIMD caps invalidated
        // a live HRTF FFT in the native W1 test. Create exactly once, before effects.
        Settings.simdLevel = IPL_SIMDLEVEL_AVX2;
        iplContextCreate(&Settings, &Context);
    }
    ~IM_AcousticSDKContextState() { if (Context) { iplContextRelease(&Context); } }
};
}

IPLContext IM_GetAcousticSDKContext()
{
    // C++ static initialization serializes the first caller. Subsequent consumers
    // only retain the existing context; no SDK global state is reconfigured.
    static IM_AcousticSDKContextState State;
    return State.Context;
}
