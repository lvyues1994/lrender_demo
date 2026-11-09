#pragma once
#include "SessionTypes.h"
#include <array>
namespace demo {
// Serialized owner-thread scheduling state only. Worker completions must hop to
// that owner. No callbacks, clocks, World, targets, delta or history storage.
// A reservation counts conservatively BEFORE assembly/submit, so inline external
// completions can retire an already-published ticket without later reinsertion.
class FramePump {
public:
 explicit FramePump(unsigned maxInFlight);
 FramePump(FramePump const&)=delete;
 FramePump& operator=(FramePump const&)=delete;
 void markDirty();
 void clearDirty(); // test/probe: drop dirty without extract
 void setContinuous(bool);
 void setPaused(bool); // preserves dirty and outstanding work
 void stop(); // irreversible; no new tickets, existing work must still complete
 // Empty means DO NOT extract a delta. Caller keeps any previously extracted
 // pending delta until acceptance. Admitted contexts require valid view/nonzero epoch.
 std::optional<FrameStamp> reserve(lr::core::ViewId,SessionGeneration);
 // All CONFIRMED settled outcomes use the SAME ticket, including old epochs.
 // Abandoning/cancelling a result waiter is NOT proof that queued work settled.
 // Unknown/duplicate/forged receipt is ignored. Completion NEVER retries a frame,
 // marks dirty, clears history or decides whether a result may be displayed.
 void complete(FrameStamp);
 unsigned inFlight() const;
private:
 unsigned const capacity_;
 unsigned count_=0;
 bool dirty_=false,continuous_=false,paused_=false,stopped_=false;
 std::uint64_t sequence_=0;
 std::array<std::optional<FrameStamp>,2> tickets_;
};
// Caller boundary: known PRE-accept assembly failure -> retain pending delta,
// complete(ticket), markDirty (unless closing). Ambiguous submit exception ->
// latch an independent SESSION FAULT (not just soft pause); do NOT free/replay
// its ticket until acceptance/settlement is established. User resume can clear
// soft pause while capacity remains; the pump itself has no fault-recovery API.
// Once submit returned its sender, work is already queued even before sender start;
// attach an independent terminal observer, never treat sender cancellation as undo.
// History reset is acknowledged by Session only after state applied + submit
// accepted in the same owner turn. Pump has no authority to acknowledge it.
}
