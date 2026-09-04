#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

class DescriptorHeap {
public:
	struct CachedCommit {
		vk::DescriptorSet set          = nullptr;
		bool              needs_update = true;
	};

	DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore);
	~DescriptorHeap();
	KYTY_CLASS_NO_COPY(DescriptorHeap);

	[[nodiscard]] vk::DescriptorSet Commit(vk::DescriptorSetLayout layout);
	[[nodiscard]] CachedCommit      CommitCached(vk::DescriptorSetLayout layout,
	                                             std::span<const vk::WriteDescriptorSet> writes);

private:
	static constexpr uint32_t DescriptorSetBatch = 32;
	static constexpr size_t   DescriptorCacheHardMaxEntries = 16384;

	struct Batch {
		std::array<vk::DescriptorSet, DescriptorSetBatch> sets {};
		uint32_t                                          size       = 0;
		uint32_t                                          allocation = DescriptorSetBatch;
	};

	struct DescriptorAtom {
		uint32_t binding       = 0;
		uint32_t array_element = 0;
		uint32_t type          = 0;
		uint32_t image_layout  = 0;
		uint64_t object0       = 0;
		uint64_t object1       = 0;
		uint64_t offset        = 0;
		uint64_t range         = 0;

		[[nodiscard]] bool operator==(const DescriptorAtom& rhs) const = default;
	};

	struct DescriptorCacheEntry {
		vk::DescriptorSetLayout        layout = nullptr;
		std::vector<DescriptorAtom>     atoms;
		vk::DescriptorSet               set = nullptr;
	};

	[[nodiscard]] bool Allocate(vk::DescriptorSetLayout layout, Batch& batch);
	void               CreatePool();
	void               ResetTransientCache(uint64_t tick);
	[[nodiscard]] std::vector<DescriptorAtom> MaterializeDescriptorKey(
	    std::span<const vk::WriteDescriptorSet> writes) const;
	[[nodiscard]] size_t HashDescriptorKey(vk::DescriptorSetLayout layout,
	                                       std::span<const DescriptorAtom> atoms) const;

	GraphicContext&                                     m_graphics;
	MasterSemaphore&                                    m_master_semaphore;
	vk::DescriptorPool                                  m_current_pool = nullptr;
	std::deque<std::pair<vk::DescriptorPool, uint64_t>> m_pending_pools;
	std::unordered_map<vk::DescriptorSetLayout, Batch>  m_sets;
	std::unordered_map<size_t, std::vector<DescriptorCacheEntry>> m_descriptor_cache;
	uint64_t                                             m_descriptor_cache_tick = 0;
	uint64_t                                             m_descriptor_cache_hits = 0;
	uint64_t                                             m_descriptor_cache_misses = 0;
	size_t                                               m_descriptor_cache_entries = 0;
	size_t                                               m_descriptor_cache_limit = 4096;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
