#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <type_traits>

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 1024},
};

template <typename Handle>
[[nodiscard]] uint64_t HandleBits(Handle handle) {
	using CType = typename Handle::CType;
	const auto native = static_cast<CType>(handle);
	if constexpr (std::is_pointer_v<CType>) {
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native));
	} else {
		return static_cast<uint64_t>(native);
	}
}

[[nodiscard]] size_t MixHash(size_t hash, uint64_t value) {
	// 64-bit hash combine. Exact atom comparison is still performed, so collisions are harmless.
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
	value ^= value >> 31U;
	return hash ^ (static_cast<size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6U) +
	               (hash >> 2U));
}

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	m_descriptor_cache_limit = std::min<size_t>(graphics.GetHostGpuTuningProfile().descriptor_cache_entries,
	                                           DescriptorCacheHardMaxEntries);
	// A descriptor-cache entry owns one descriptor set. A pool cannot hold more than
	// DescriptorHeapCount sets, so reserving up to that bound prevents the cache map from
	// repeatedly rehashing on descriptor-heavy submissions without over-allocating for the
	// configured cache limit.
	m_descriptor_cache.reserve(std::min<size_t>(m_descriptor_cache_limit, DescriptorHeapCount));
	CreatePool();
	ResetTransientCache(m_master_semaphore.CurrentTick());
}

DescriptorHeap::~DescriptorHeap() {
	if (m_descriptor_cache_hits + m_descriptor_cache_misses != 0) {
		std::printf("Descriptor cache: hits = %" PRIu64 ", misses = %" PRIu64 "\n",
		            m_descriptor_cache_hits, m_descriptor_cache_misses);
	}
	m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool, nullptr);
	}
}

void DescriptorHeap::ResetTransientCache(uint64_t tick) {
	m_descriptor_cache.clear();
	m_descriptor_cache_entries = 0;
	m_descriptor_cache_tick    = tick;
}

std::vector<DescriptorHeap::DescriptorAtom> DescriptorHeap::MaterializeDescriptorKey(
    std::span<const vk::WriteDescriptorSet> writes) const {
	std::vector<DescriptorAtom> atoms;
	size_t total = 0;
	for (const auto& write: writes) {
		total += write.descriptorCount;
	}
	atoms.reserve(total);

	for (const auto& write: writes) {
		for (uint32_t i = 0; i < write.descriptorCount; ++i) {
			DescriptorAtom atom {};
			atom.binding       = write.dstBinding;
			atom.array_element = write.dstArrayElement + i;
			atom.type          = static_cast<uint32_t>(write.descriptorType);
			if (write.pBufferInfo != nullptr) {
				const auto& info = write.pBufferInfo[i];
				atom.object0 = HandleBits(info.buffer);
				atom.offset  = info.offset;
				atom.range   = info.range;
			} else if (write.pImageInfo != nullptr) {
				const auto& info = write.pImageInfo[i];
				atom.object0      = HandleBits(info.sampler);
				atom.object1      = HandleBits(info.imageView);
				atom.image_layout = static_cast<uint32_t>(info.imageLayout);
			} else if (write.pTexelBufferView != nullptr) {
				atom.object0 = HandleBits(write.pTexelBufferView[i]);
			}
			atoms.push_back(atom);
		}
	}
	return atoms;
}

size_t DescriptorHeap::HashDescriptorKey(vk::DescriptorSetLayout layout,
                                         std::span<const DescriptorAtom> atoms) const {
	size_t hash = MixHash(0, HandleBits(layout));
	for (const auto& atom: atoms) {
		hash = MixHash(hash, atom.binding);
		hash = MixHash(hash, atom.array_element);
		hash = MixHash(hash, atom.type);
		hash = MixHash(hash, atom.image_layout);
		hash = MixHash(hash, atom.object0);
		hash = MixHash(hash, atom.object1);
		hash = MixHash(hash, atom.offset);
		hash = MixHash(hash, atom.range);
	}
	return hash;
}

DescriptorHeap::CachedCommit DescriptorHeap::CommitCached(
    vk::DescriptorSetLayout layout, std::span<const vk::WriteDescriptorSet> writes) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(layout == nullptr);

	const auto tick = m_master_semaphore.CurrentTick();
	if (tick != m_descriptor_cache_tick) {
		// Sets can remain alive in pending pools, but the resources they point at may be recycled by
		// the emulator after a submission. Keep the cache submission-local for correctness.
		ResetTransientCache(tick);
	}

	auto atoms = MaterializeDescriptorKey(writes);
	const auto hash = HashDescriptorKey(layout, atoms);
	if (const auto it = m_descriptor_cache.find(hash); it != m_descriptor_cache.end()) {
		for (const auto& entry: it->second) {
			if (entry.layout == layout && entry.atoms == atoms) {
				++m_descriptor_cache_hits;
				return {.set = entry.set, .needs_update = false};
			}
		}
	}

	++m_descriptor_cache_misses;
	const auto set = Commit(layout);
	if (m_descriptor_cache_entries < m_descriptor_cache_limit) {
		m_descriptor_cache[hash].push_back({.layout = layout, .atoms = std::move(atoms), .set = set});
		++m_descriptor_cache_entries;
	}
	return {.set = set, .needs_update = true};
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(layout == nullptr);

	auto& batch = m_sets[layout];
	if (batch.size != 0) {
		return batch.sets[--batch.size];
	}
	if (Allocate(layout, batch)) {
		return batch.sets[--batch.size];
	}

	m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());
	if (const auto& [pool, tick] = m_pending_pools.front(); m_master_semaphore.IsFree(tick)) {
		m_current_pool = pool;
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool, {}) != vk::Result::eSuccess);
	} else {
		CreatePool();
	}

	m_sets.clear();
	// The new allocation can come from a different/recycled pool. Do not keep cached handles from
	// the previous pool even when this happens inside the same submission.
	ResetTransientCache(m_master_semaphore.CurrentTick());
	auto& fresh_batch = m_sets[layout];
	EXIT_IF(!Allocate(layout, fresh_batch));
	return fresh_batch.sets[--fresh_batch.size];
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.sType          = vk::StructureType::eDescriptorSetAllocateInfo;
	allocate.descriptorPool = m_current_pool;
	allocate.pSetLayouts    = layouts.data();

	for (;;) {
		allocate.descriptorSetCount = batch.allocation;
		const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
		if (result == vk::Result::eSuccess) {
			batch.size = batch.allocation;
			return true;
		}
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
		if (batch.allocation == 1) {
			return false;
		}
		batch.allocation /= 2;
	}
}

void DescriptorHeap::CreatePool() {
	vk::DescriptorPoolCreateInfo create {};
	create.sType         = vk::StructureType::eDescriptorPoolCreateInfo;
	create.flags         = {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(DescriptorPoolSizes.size());
	create.pPoolSizes    = DescriptorPoolSizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool) !=
	        vk::Result::eSuccess);
}

} // namespace Libs::Graphics
