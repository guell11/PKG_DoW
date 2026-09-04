#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_WAIT_REGISTRY_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_WAIT_REGISTRY_H_

#include "common/common.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// Tracks the guest memory labels that suspended PM4 submissions are waiting on
// (WAIT_REG_MEM / WAIT_REG_MEM_64). Producers that write those labels (EOP
// writes, write_data, dma_data, flips, direct CPU writes) record the value here,
// which wakes the GPU thread immediately instead of waiting for the blocked
// re-poll timeout. The producer value is also latched: a guest fence label is
// frequently reset to zero right after the producer writes it, so re-reading
// guest memory at wake time can miss the transient satisfied window.
class WaitRegistry final {
public:
	struct Waiter {
		uint64_t address = 0;   // guest physical address of the label
		uint64_t reference = 0; // masked compare reference
		uint64_t mask = 0;      // compare mask
		uint32_t function = 0;  // compare function (TestWaitRegMemValue semantics)
		bool     is64 = false;  // 64-bit label
		bool     latched = false; // a producer already satisfied this waiter
		uint64_t registered_tick = 0; // host tick at registration (deadlock breaker)
		uint64_t last_produced = 0;   // last value any producer wrote (0 with
		                              // produced_valid=false when never written)
		bool     produced_valid = false;
		uint64_t produced_frame = 0;
	};

	// Registers a waiter for the given label. Called with the scheduler
	// blocked; the waiter is re-checked every GPU pass and removed once
	// satisfied.
	static void Register(const Waiter& waiter);

	// Removes and returns the waiters whose label now satisfies their
	// condition, either from the producer-latched value or from a fresh
	// read of guest memory.
	static std::vector<Waiter> CollectSatisfied();

	// Records a producer write and latches any already-registered waiter it
	// satisfies. Returns true when at least one waiter latched, meaning the
	// caller should wake the GPU thread.
	static bool RecordProduced(uint64_t address, uint64_t value, bool is64);

	// Removes and returns waiters stuck longer than the deadline whose
	// condition is satisfied by the last value a real producer wrote. The
	// serial command processor cannot model two GPU queues running
	// concurrently, so a label written, reset and re-waited across queues can
	// cycle forever even though a real producer did signal it. Waiters are
	// only released when an actual producer wrote a satisfying value at least
	// once — no value is ever fabricated.
	static std::vector<Waiter> CollectDeadlockBroken(uint64_t now_tick, uint64_t min_age_ticks);

	// Frame boundary bookkeeping (flip): labels written in a previous frame
	// must not satisfy waits of the current frame through the latch path.
	static void AdvanceFrame();

	static void Clear();

	[[nodiscard]] static bool Empty();
	[[nodiscard]] static size_t Count();

	// Host tick source for age-based policies.
	static uint64_t Now();

private:
	// Slot for a registered waiter plus the frame-guard state.
	struct Impl;
};

// Wakes the guest GPU thread immediately (after a producer wrote a waited
// label or a waiter became releasable). Cheap when nothing is parked.
void GpuWakeNudge();

// Monotonic generation bumped by every nudge; the GPU thread compares it to
// detect wakes without consuming them.
uint64_t GpuWakeGeneration();

// Deadlock-breaker cadence: producer latches are honored instantly; this only
// decides when to retry waiters that no producer ever satisfied.
constexpr uint64_t kWakeNudgeMinAgeMicros = 5000;

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_WAIT_REGISTRY_H_
