#pragma once
#include <phonon.h>

// Borrowed process-lifetime context; callers retain their own lease. Call only
// during lifecycle/worker setup, never from steady audio processing.
IPLContext IM_GetAcousticSDKContext();
