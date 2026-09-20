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
enum class IM_AcousticIRState : unsigned char { Free, Writing, Ready, Reading };
struct IM_AcousticReverbSlot
{
    std::atomic<IM_AcousticIRState> State{IM_AcousticIRState::Free};
    IPLSource Source=nullptr;
    // Protected by the slot state lease. Only a successful SDK Apply consumes
    // its pending TripleBuffer IR. Free alone does not acknowledge that buffer.
    bool Applied=false;
    IPLReflectionEffectParams Params{};
    IPLCoordinateSpace3 Listener{};
    std::uint64_t Sequence=0;
    double CapturedSeconds=0;
    ~IM_AcousticReverbSlot(){if(Source)iplSourceRelease(&Source);}
};
struct IM_AcousticReverbPool
{
    explicit IM_AcousticReverbPool(std::uint64_t Epoch):WorldGeneration(Epoch){}
    const std::uint64_t WorldGeneration;
    std::atomic<bool> Stopped{false};
    std::array<IM_AcousticReverbSlot,3> Slots;
};
