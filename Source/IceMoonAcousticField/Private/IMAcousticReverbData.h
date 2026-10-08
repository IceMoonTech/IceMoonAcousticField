#pragma once
#include <phonon.h>
#include <array>
#include <atomic>
#include <cstdint>

// Three bounded leases: worker writes Free->Writing, detaches the SDK source
// before publishing Ready; one audio consumer claims Reading and returns Free
// only after its last Apply using that IR. Queue pressure skips an update.
// The pool outlives worker and audio effects. Sources are never released from
// a steady callback. SDK's internal read-buffer swap belongs to that one effect.
enum class EIMAcousticIRState : unsigned char { Free, Writing, Ready, Reading };
struct FIMAcousticReverbSlot
{
    std::atomic<EIMAcousticIRState> State{EIMAcousticIRState::Free};
    IPLSource Source=nullptr;
    // Protected by the slot state lease. Only a successful SDK Apply consumes
    // its pending TripleBuffer IR. Free alone does not acknowledge that buffer.
    bool Applied=false;
    IPLReflectionEffectParams Params{};
    IPLCoordinateSpace3 Listener{};
    std::uint64_t Sequence=0;
    double CapturedSeconds=0;
    ~FIMAcousticReverbSlot(){if(Source)iplSourceRelease(&Source);}
};
struct FIMAcousticReverbPool
{
    explicit FIMAcousticReverbPool(std::uint64_t Epoch):WorldGeneration(Epoch){}
    const std::uint64_t WorldGeneration;
    std::atomic<bool> Stopped{false};
    std::array<FIMAcousticReverbSlot,3> Slots;
};
