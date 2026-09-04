#include "graphics/guest_gpu/waitRegistry.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/graphicsRun.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// The registry instance is process-wide: suspended PM4 submissions and producer
// threads (kernel flip path, host writebacks) reference it from different
// threads. A single mutex serializes it; the critical sections are tiny and
// waiters number at most a few per label.
class WaitRegistryImpl {
public:
	using Waiter = WaitRegistry::Waiter;

	void Register(const Waiter& waiter) {
		for (auto& existing: m_waiters[waiter.address]) {
			if (existing.function == waiter.function &&
			    existing.reference == waiter.reference && existing.mask == waiter.mask &&
			    existing.is64 == waiter.is64) {
				// Same waiter re-suspending on a re-poll: refresh its age so
				// the deadlock breaker measures from the latest block.
				existing.latched = false;
				existing.registered_tick = Now();
				return;
			}
		}
		Waiter registered = waiter;
		registered.latched = false;
		registered.registered_tick = Now();
		registered.produced_frame = m_frame_id;
		m_waiters[waiter.address].push_back(registered);
	}

	std::vector<Waiter> CollectSatisfied() {
		std::vector<Waiter> woken;
		for (auto iterator = m_waiters.begin(); iterator != m_waiters.end();) {
			auto& waiters = iterator->second;
			for (size_t index = 0; index < waiters.size();) {
				auto& waiter = waiters[index];
				bool  satisfied = waiter.latched;
				if (!satisfied) {
					// Fresh read of the label: the producer may have written it
					// outside our knowledge (direct CPU store).
					uint64_t value = 0;
					if (waiter.is64) {
						std::memcpy(&value, reinterpret_cast<const void*>(waiter.address),
						            sizeof(uint64_t));
					} else {
						uint32_t narrow = 0;
						std::memcpy(&narrow, reinterpret_cast<const void*>(waiter.address),
						            sizeof(uint32_t));
						value = narrow;
					}
					satisfied = Compare(waiter, value);
				}
				if (!satisfied) {
					++index;
					continue;
				}
				woken.push_back(waiter);
				waiters.erase(waiters.begin() + static_cast<std::ptrdiff_t>(index));
			}
			if (waiters.empty()) {
				iterator = m_waiters.erase(iterator);
			} else {
				++iterator;
			}
		}
		return woken;
	}

	bool RecordProduced(uint64_t address, uint64_t value, bool is64) {
		auto iterator = m_waiters.find(address);
		if (iterator == m_waiters.end()) {
			return false;
		}
		bool latched_any = false;
		for (auto& waiter: iterator->second) {
			waiter.last_produced = value;
			waiter.produced_valid = true;
			waiter.produced_frame = m_frame_id;
			// Frame guard: a label produced in a previous frame must not
			// latch a waiter of the current frame through the stale value.
			const bool fresh = waiter.produced_frame == m_frame_id ||
			                   waiter.registered_tick >= Now() - 1;
			if (!waiter.latched && waiter.is64 == is64 && fresh &&
			    Compare(waiter, value)) {
				waiter.latched = true;
				latched_any = true;
			}
		}
		return latched_any;
	}

	std::vector<Waiter> CollectDeadlockBroken(uint64_t now_tick, uint64_t min_age_ticks) {
		std::vector<Waiter> broken;
		for (auto iterator = m_waiters.begin(); iterator != m_waiters.end();) {
			auto& waiters = iterator->second;
			for (size_t index = 0; index < waiters.size();) {
				auto& waiter = waiters[index];
				// Cross-queue production must not release a waiter registered
				// in a different frame than the producing write.
				if (waiter.latched || !waiter.produced_valid ||
				    waiter.produced_frame != m_frame_id ||
				    now_tick - waiter.registered_tick < min_age_ticks ||
				    !Compare(waiter, waiter.last_produced)) {
					++index;
					continue;
				}
				LOGF("WaitRegistry: breaking cross-queue wait deadlock: addr = 0x%016" PRIx64
				     ", produced = 0x%016" PRIx64 ", func = %u, ref = 0x%016" PRIx64
				     ", mask = 0x%016" PRIx64 "\n",
				     waiter.address, waiter.last_produced, waiter.function, waiter.reference,
				     waiter.mask);
				broken.push_back(waiter);
				waiters.erase(waiters.begin() + static_cast<std::ptrdiff_t>(index));
			}
			if (waiters.empty()) {
				iterator = m_waiters.erase(iterator);
			} else {
				++iterator;
			}
		}
		return broken;
	}

	void AdvanceFrame() {
		m_frame_id++;
	}

	void Clear() {
		m_waiters.clear();
	}

	[[nodiscard]] bool Empty() const { return m_waiters.empty(); }
	[[nodiscard]] size_t Count() const {
		size_t total = 0;
		for (const auto& [address, waiters]: m_waiters) {
			(void)address;
			total += waiters.size();
		}
		return total;
	}

	static uint64_t Now() {
		return static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(
		        std::chrono::steady_clock::now().time_since_epoch())
		        .count());
	}

	static bool Compare(const Waiter& waiter, uint64_t value) {
		return TestWaitRegMemValue(value, waiter.reference, waiter.mask, waiter.function);
	}

private:
	std::unordered_map<uint64_t, std::vector<Waiter>> m_waiters;
	uint64_t m_frame_id = 0;
};

namespace {

std::mutex                 g_mutex;
WaitRegistryImpl           g_impl;
std::atomic_uint64_t g_wake_sequence {0};

} // namespace

void WaitRegistry::Register(const Waiter& waiter) {
	std::lock_guard lock(g_mutex);
	g_impl.Register(waiter);
}

std::vector<WaitRegistry::Waiter> WaitRegistry::CollectSatisfied() {
	std::lock_guard lock(g_mutex);
	return g_impl.CollectSatisfied();
}

bool WaitRegistry::RecordProduced(uint64_t address, uint64_t value, bool is64) {
	std::lock_guard lock(g_mutex);
	return g_impl.RecordProduced(address, value, is64);
}

std::vector<WaitRegistry::Waiter> WaitRegistry::CollectDeadlockBroken(uint64_t now_tick,
                                                                      uint64_t min_age_ticks) {
	std::lock_guard lock(g_mutex);
	return g_impl.CollectDeadlockBroken(now_tick, min_age_ticks);
}

void WaitRegistry::AdvanceFrame() {
	std::lock_guard lock(g_mutex);
	g_impl.AdvanceFrame();
}

void WaitRegistry::Clear() {
	std::lock_guard lock(g_mutex);
	g_impl.Clear();
}

bool WaitRegistry::Empty() {
	std::lock_guard lock(g_mutex);
	return g_impl.Empty();
}

size_t WaitRegistry::Count() {
	std::lock_guard lock(g_mutex);
	return g_impl.Count();
}

uint64_t WaitRegistry::Now() {
	return WaitRegistryImpl::Now();
}

void GpuWakeNudge() {
	// Wake generation counter: the GPU thread waits on its own generation and
	// producers bump it, which short-circuits the blocked re-poll sleep.
	g_wake_sequence.fetch_add(1, std::memory_order_release);
	GuestGpu::WakeBlockedSubmissions();
}

uint64_t GpuWakeGeneration() {
	return g_wake_sequence.load(std::memory_order_acquire);
}

} // namespace Libs::Graphics
