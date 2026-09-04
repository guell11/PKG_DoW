#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vma.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <string_view>

namespace Libs::Graphics {

namespace {

struct MemoryStats {
	std::atomic_uint64_t allocated[VK_MAX_MEMORY_TYPES] {};
	std::atomic_uint64_t count[VK_MAX_MEMORY_TYPES] {};
};

MemoryStats g_memory_stats;

void TrackAllocationImpl(const VulkanMemory& memory) {
	g_memory_stats.allocated[memory.type] += memory.requirements.size;
	g_memory_stats.count[memory.type]++;
}

void UntrackAllocationImpl(const VulkanMemory& memory) {
	EXIT_IF(g_memory_stats.allocated[memory.type] < memory.requirements.size);
	EXIT_IF(g_memory_stats.count[memory.type] == 0);
	g_memory_stats.allocated[memory.type] -= memory.requirements.size;
	g_memory_stats.count[memory.type]--;
}

} // namespace

void VulkanTrackAllocation(const VulkanMemory& memory) {
	TrackAllocationImpl(memory);
}

void VulkanUntrackAllocation(const VulkanMemory& memory) {
	UntrackAllocationImpl(memory);
}

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", VulkanToString(result).c_str());
		return false;
	}
	const auto budget = GetAdaptiveDeviceMemoryBudget();
	LOGF("Host GPU auto tuning: %s, local=%" PRIu64 " MiB, budget=%" PRIu64
	     " MiB, target=%" PRIu64 " MiB, pressure=%" PRIu64 " MiB, critical=%" PRIu64
	     " MiB, VK_EXT_memory_budget=%s\n",
	     GetHostGpuArchitectureName(), budget.physical_local / (1024 * 1024),
	     budget.budget / (1024 * 1024), budget.target / (1024 * 1024),
	     budget.pressure / (1024 * 1024), budget.critical / (1024 * 1024),
	     memory_budget_ext_enabled ? "yes" : "no");
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

uint64_t VulkanNextMemoryUniqueId() {
	static std::atomic_uint64_t sequence = 0;
	return ++sequence;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	return GetAdaptiveDeviceMemoryBudget().usage;
}

HostGpuTuningProfile GraphicContext::GetHostGpuTuningProfile() const {
	HostGpuTuningProfile profile {};
	const auto vendor = physical_device_properties.vendorID;
	const std::string_view name {physical_device_properties.deviceName.data()};

	// Vulkan is still the translation target. These profiles tune policy for the host GPU family;
	// they intentionally do not hard-code NVIDIA native/SASS instructions, which belong to the
	// driver and change across driver generations.
	if (vendor == 0x10DE) {
		profile.descriptor_cache_entries = 8192;
		profile.pipeline_worker_count    = 1;
		if (name.find("RTX 50") != std::string_view::npos) {
			profile.architecture = HostGpuArchitecture::NvidiaBlackwell;
		} else if (name.find("RTX 40") != std::string_view::npos) {
			profile.architecture = HostGpuArchitecture::NvidiaAda;
		} else if (name.find("RTX 30") != std::string_view::npos) {
			profile.architecture = HostGpuArchitecture::NvidiaAmpere;
		} else if (name.find("RTX 20") != std::string_view::npos ||
		           name.find("GTX 16") != std::string_view::npos) {
			profile.architecture = HostGpuArchitecture::NvidiaTuring;
		}
	}

	uint64_t physical_local = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; ++heap) {
		if (static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                          vk::MemoryHeapFlagBits::eDeviceLocal)) {
			physical_local += physical_device_memory_properties.memoryHeaps[heap].size;
		}
	}
	constexpr uint64_t GiB = 1024ull * 1024 * 1024;
	// Capacity matters more than marketing generation for emulation memory residency. 8 GiB cards
	// need breathing room for the compositor/driver; large cards can keep more of the guest working
	// set device-local before eviction starts. Integrated GPUs share system RAM, so reserving nearly
	// the entire advertised heap would simply move the bottleneck from VRAM to OS paging.
	if (physical_device_properties.deviceType != vk::PhysicalDeviceType::eDiscreteGpu) {
		profile.vram_target_ratio   = 0.70f;
		profile.vram_pressure_ratio = 0.82f;
		profile.vram_critical_ratio = 0.90f;
	} else if (physical_local <= 6 * GiB) {
		profile.vram_target_ratio   = 0.82f;
		profile.vram_pressure_ratio = 0.89f;
		profile.vram_critical_ratio = 0.95f;
	} else if (physical_local <= 8 * GiB) {
		profile.vram_target_ratio   = 0.86f;
		profile.vram_pressure_ratio = 0.92f;
		profile.vram_critical_ratio = 0.96f;
	} else if (physical_local <= 12 * GiB) {
		profile.vram_target_ratio   = 0.90f;
		profile.vram_pressure_ratio = 0.94f;
		profile.vram_critical_ratio = 0.97f;
	} else if (physical_local <= 16 * GiB) {
		profile.vram_target_ratio   = 0.91f;
		profile.vram_pressure_ratio = 0.95f;
		profile.vram_critical_ratio = 0.975f;
	} else {
		profile.vram_target_ratio   = 0.92f;
		profile.vram_pressure_ratio = 0.96f;
		profile.vram_critical_ratio = 0.98f;
	}
	return profile;
}

const char* GraphicContext::GetHostGpuArchitectureName() const {
	switch (GetHostGpuTuningProfile().architecture) {
		case HostGpuArchitecture::NvidiaTuring: return "NVIDIA Turing";
		case HostGpuArchitecture::NvidiaAmpere: return "NVIDIA Ampere";
		case HostGpuArchitecture::NvidiaAda: return "NVIDIA Ada Lovelace";
		case HostGpuArchitecture::NvidiaBlackwell: return "NVIDIA Blackwell";
		default: return "Generic Vulkan";
	}
}

DeviceMemoryBudget GraphicContext::GetAdaptiveDeviceMemoryBudget() const {
	DeviceMemoryBudget result {};
	if (allocator == nullptr) {
		return result;
	}

	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; ++heap) {
		const auto& props = physical_device_memory_properties.memoryHeaps[heap];
		const bool device_local = static_cast<bool>(props.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			result.physical_local += props.size;
		}
		if (!discrete || device_local) {
			result.budget += CanReportMemoryUsage() ? budgets[heap].budget : props.size;
			result.usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (result.budget == 0) {
		return result;
	}

	const auto profile = GetHostGpuTuningProfile();
	const auto ratio_bytes = [&](float ratio) -> uint64_t {
		return static_cast<uint64_t>(static_cast<long double>(result.budget) * ratio);
	};
	constexpr uint64_t MiB = 1024ull * 1024;
	constexpr uint64_t GiB = 1024ull * MiB;
	uint64_t reserve = 512ull * MiB;
	if (!discrete) {
		reserve = 2ull * GiB;
	} else if (result.physical_local <= 6 * GiB) {
		reserve = 768ull * MiB;
	} else if (result.physical_local <= 8 * GiB) {
		reserve = 640ull * MiB;
	} else if (result.physical_local <= 12 * GiB) {
		reserve = 768ull * MiB;
	} else if (result.physical_local <= 16 * GiB) {
		reserve = 1024ull * MiB;
	} else {
		reserve = 1280ull * MiB;
	}
	result.reserve = std::min(reserve, result.budget / 4);

	const auto reserve_limited = result.budget > result.reserve ? result.budget - result.reserve : 0;
	result.target   = std::min(ratio_bytes(profile.vram_target_ratio), reserve_limited);
	result.pressure = ratio_bytes(profile.vram_pressure_ratio);
	result.critical = ratio_bytes(profile.vram_critical_ratio);

	// Keep thresholds monotonic even on tiny/virtual heaps. Budget itself is dynamic when
	// VK_EXT_memory_budget is active, so other applications stealing VRAM automatically lowers the
	// emulator's targets on the next GC pass instead of waiting for paging/stutter.
	result.target   = std::min(result.target, result.budget);
	result.pressure = std::clamp(result.pressure, result.target, result.budget);
	result.critical = std::clamp(result.critical, result.pressure, result.budget);
	return result;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	return GetAdaptiveDeviceMemoryBudget().target;
}

void GraphicContext::CreateBuffer(uint64_t size, VulkanBuffer& buffer) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || buffer.buffer != nullptr ||
	        buffer.memory.allocation != nullptr || size == 0);

	vk::BufferCreateInfo buffer_info {};
	buffer_info.sType       = vk::StructureType::eBufferCreateInfo;
	buffer_info.size        = size;
	buffer_info.usage       = buffer.usage;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(buffer.memory.property);
	alloc_info.preferredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(buffer.memory.preferred_property);

	vk::Buffer::CType native_buffer = VK_NULL_HANDLE;
	const auto        result        = static_cast<vk::Result>(vmaCreateBuffer(
	    allocator, static_cast<const vk::BufferCreateInfo::NativeType*>(buffer_info), &alloc_info,
	    &native_buffer, &buffer.memory.allocation, &buffer.memory.allocation_info));
	buffer.buffer                   = native_buffer;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	device.getBufferMemoryRequirements(buffer.buffer, &buffer.memory.requirements);
	buffer.memory.type      = buffer.memory.allocation_info.memoryType;
	buffer.memory.memory    = buffer.memory.allocation_info.deviceMemory;
	buffer.memory.offset    = buffer.memory.allocation_info.offset;
	buffer.memory.unique_id = VulkanNextMemoryUniqueId();
	buffer.buffer_size      = size;
	VulkanTrackAllocation(buffer.memory);
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.memory.allocation != nullptr);

	auto&                   memory = image.memory;
	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = static_cast<vk::MemoryPropertyFlags::MaskType>(memory.property);
	alloc_info.preferredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(memory.preferred_property);

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto       result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &memory.allocation, &memory.allocation_info));
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	device.getImageMemoryRequirements(image.image, &memory.requirements);
	memory.type      = memory.allocation_info.memoryType;
	memory.memory    = memory.allocation_info.deviceMemory;
	memory.offset    = memory.allocation_info.offset;
	memory.unique_id = VulkanNextMemoryUniqueId();
	VulkanTrackAllocation(memory);
	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.memory.allocation == nullptr);

	auto& memory = image.memory;
	VulkanUntrackAllocation(memory);
	vmaDestroyImage(allocator, image.image, memory.allocation);
	image.image            = nullptr;
	memory.memory          = nullptr;
	memory.allocation      = nullptr;
	memory.allocation_info = {};
	memory.offset          = 0;
}

void GraphicContext::MapMemory(VulkanMemory& memory, void*& data) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || memory.allocation == nullptr);
	EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(vmaMapMemory(allocator, memory.allocation,
	                                                          &data)) != vk::Result::eSuccess);
}

void GraphicContext::UnmapMemory(VulkanMemory& memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || memory.allocation == nullptr);
	vmaUnmapMemory(allocator, memory.allocation);
}

} // namespace Libs::Graphics
