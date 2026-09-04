#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shaderCompiler.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <limits>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	// Vulkan pipeline-cache blobs are already validated by the driver UUID/device/driver tuple.
	// Keep an emulator schema tag as well, but do not disable the cache for source-archive builds
	// where git metadata is unavailable. Those are exactly the builds that benefit from a warm cache.
	constexpr char        hex[]        = "0123456789abcdef";
	constexpr std::string_view schema  = "KytyPC2";
	const std::string_view revision = std::string_view(KYTY_GIT_REVISION) == "unknown"
	                                      ? std::string_view("archive")
	                                      : std::string_view(KYTY_GIT_REVISION);
	std::string uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("{}:{}:{:08x}:{:08x}:{:08x}:{}\n", schema, revision, properties.vendorID,
	                   properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	if (Log::GetDirection() != Log::Direction::Console) {
		std::fwrite(message.data(), 1, message.size(), stdout);
		std::fflush(stdout);
	}
	Log::Write(message);
	Log::Flush();
}

void NormalizeStaticParamsForDynamicState(PipelineStaticParameters& static_params) {
	static_params.viewport_scale[0]  = 0.5f;
	static_params.viewport_scale[1]  = 0.5f;
	static_params.viewport_scale[2]  = 1.0f;
	static_params.viewport_offset[0] = 0.5f;
	static_params.viewport_offset[1] = 0.5f;
	static_params.viewport_offset[2] = 0.0f;

	static_params.scissor_ltrb[0] = 0;
	static_params.scissor_ltrb[1] = 0;
	static_params.scissor_ltrb[2] = 1;
	static_params.scissor_ltrb[3] = 1;

	// Blend constants are core Vulkan dynamic state. Keeping them in the pipeline key created
	// needless variants whenever a game animated a constant blend factor.
	static_params.blend_color_red   = 0.0f;
	static_params.blend_color_green = 0.0f;
	static_params.blend_color_blue  = 0.0f;
	static_params.blend_color_alpha = 0.0f;

	// Canonicalize blend registers that Vulkan ignores. Besides reducing pipeline variants, this
	// prevents irrelevant/unknown guest blend factors from reaching GetBlendFactor when blending
	// is disabled. Recent PS4 emulators use the same principle when refreshing graphics keys.
	for (uint32_t i = 0; i < static_params.color_count; i++) {
		const bool effective_blend = static_params.blend_enable[i] && !static_params.blend_bypass[i];
		if (!effective_blend) {
			static_params.blend_enable[i]         = false;
			static_params.blend_bypass[i]         = false;
			static_params.color_srcblend[i]       = 0;
			static_params.color_comb_fcn[i]       = 0;
			static_params.color_destblend[i]      = 0;
			static_params.alpha_srcblend[i]       = 0;
			static_params.alpha_comb_fcn[i]       = 0;
			static_params.alpha_destblend[i]      = 0;
			static_params.separate_alpha_blend[i] = false;
		} else if (!static_params.separate_alpha_blend[i]) {
			// Alpha state aliases color state when separate-alpha blending is disabled.
			static_params.alpha_srcblend[i]  = 0;
			static_params.alpha_comb_fcn[i]  = 0;
			static_params.alpha_destblend[i] = 0;
		}
	}
}

} // namespace

struct PipelineCache::PipelineCompiler {
	// Multiple workers drain the compile queue in parallel: scene transitions
	// enqueue whole bursts of pipeline compilations, and a single worker turns
	// each burst into serial hitches. Driver-cache writes are serialized by
	// m_driver_cache_mutex, and each job owns its own pipeline objects.
	explicit PipelineCompiler(const char* /*name*/) {
		unsigned workers = 1;
		if (const char* env = std::getenv("KYTY_PIPELINE_WORKERS");
		    env != nullptr && env[0] != '\0') {
			char* end    = nullptr;
			auto  parsed = std::strtoul(env, &end, 10);
			if (end != env && parsed > 0) {
				workers = static_cast<unsigned>(std::min<unsigned long>(parsed, 16));
			}
		} else {
			workers = std::max(1u, std::thread::hardware_concurrency() / 4u);
		}
		m_threads.reserve(workers);
		for (unsigned index = 0; index < workers; index++) {
			m_threads.emplace_back([this] { Run(); });
		}
	}

	~PipelineCompiler() {
		WaitIdle();
		{
			std::lock_guard lock(m_mutex);
			m_stopping = true;
		}
		m_cv.notify_all();
		for (auto& thread: m_threads) {
			if (thread.joinable()) {
				thread.join();
			}
		}
	}

	void Enqueue(std::function<void()> job) {
		{
			std::lock_guard lock(m_mutex);
			EXIT_IF(m_stopping);
			m_outstanding++;
			m_jobs.push_back(std::move(job));
		}
		m_cv.notify_one();
	}

	void WaitIdle() {
		std::unique_lock lock(m_mutex);
		m_idle.wait(lock, [this] { return m_outstanding == 0; });
	}

private:
	void Run() {
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock lock(m_mutex);
				m_cv.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
				if (m_stopping && m_jobs.empty()) {
					return;
				}
				job = std::move(m_jobs.front());
				m_jobs.pop_front();
			}
			job();
			{
				std::lock_guard lock(m_mutex);
				if (--m_outstanding == 0) {
					m_idle.notify_all();
				}
			}
		}
	}

	std::vector<std::thread>          m_threads;
	std::mutex                        m_mutex;
	std::condition_variable           m_cv;
	std::condition_variable           m_idle;
	std::deque<std::function<void()>> m_jobs;
	// Every queued or running job holds one reference; WaitIdle blocks until
	// this drains to zero.
	int64_t                           m_outstanding = 0;
	bool                              m_stopping    = false;
};

struct PipelineCache::ProgramCache {
	struct SourceKey {
		ShaderType stage = ShaderType::Unknown;
		uint64_t   hash  = 0;

		bool operator==(const SourceKey&) const = default;
	};

	struct Permutation {
		std::vector<uint32_t>                                static_key;
		std::shared_ptr<const ShaderRecompiler::IR::Program> program;
		ShaderProgram                                        handle;
	};

	struct SourceCache {
		// One guest shader can have many resource/static specializations. Bucket them by a fast
		// hash first, then retain exact-key + MaterializeProgram checks for collision safety.
		std::unordered_map<uint64_t, std::vector<Permutation>> permutations;
		uint64_t total_permutations = 0;
	};

	struct SourceKeyHash {
		std::size_t operator()(const SourceKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			return hash;
		}
	};

	static uint64_t MixId(uint64_t hash, uint64_t value) {
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
	}

	static constexpr std::size_t MaxStaticKeyWords =
	    12 + ShaderVertexInputInfo::RES_MAX * 17;

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info) {
		constexpr ShaderType stage = [] {
			if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
				return ShaderType::Vertex;
			} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
				return ShaderType::Pixel;
			} else {
				static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
				return ShaderType::Compute;
			}
		}();

		BuildStageStaticKey(input_info, key_scratch);
		const uint64_t static_hash = key_scratch.empty()
		                                 ? 0
		                                 : XXH3_64bits(key_scratch.data(),
		                                               key_scratch.size() * sizeof(uint32_t));
		auto& source = programs[{stage, params.hash}];
		if (auto bucket = source.permutations.find(static_hash);
		    bucket != source.permutations.end()) {
			for (const auto& permutation: bucket->second) {
				if (permutation.static_key == key_scratch &&
				    MaterializeProgram(permutation.program, params, input_info)) {
					++num_cache_hits;
					return permutation.handle;
				}
			}
		}

		++num_cache_misses;
		const auto module = CompileProgram(device, params, input_info);
		EXIT_IF(module == nullptr || !input_info.stage);
		uint64_t id =
		    MixId(MixId(params.hash, static_cast<uint64_t>(stage)), source.total_permutations++);
		if (id == 0) {
			id = 1;
		}
		const ShaderProgram handle {.id = id, .module = module};
		source.permutations[static_hash].push_back({key_scratch, input_info.stage.program, handle});

		++num_compiled;
		// Shader compilation can happen in large bursts during scene transitions. Keep the
		// per-shader progress trace opt-in: synchronous stdout writes here otherwise add
		// avoidable hitching to the render thread. Aggregate cache statistics remain logged
		// when the pipeline cache is saved.
		if (graphics_debug_dump_enabled()) {
			std::printf("Num compiled %u shaders (cache hits=%" PRIu64 ", misses=%" PRIu64 ")\n",
			            num_compiled, num_cache_hits, num_cache_misses);
		}
		return handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {
		key_scratch.reserve(MaxStaticKeyWords);
		programs.reserve(2048);
	}

	std::unordered_map<SourceKey, SourceCache, SourceKeyHash> programs;
	std::vector<uint32_t> key_scratch;
	vk::Device device;
	uint32_t   num_compiled   = 0;
	uint64_t   num_cache_hits = 0;
	uint64_t   num_cache_misses = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)),
      m_pipeline_compiler(std::make_unique<PipelineCompiler>("Kyty.PipelineCompiler")) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	// Avoid rehash/allocation churn in draw-heavy titles. These are only bucket reservations;
	// pipeline objects are still created lazily on demand.
	m_graphics_pipelines.reserve(2048);
	m_compute_pipelines.reserve(512);
	m_pending_graphics_pipelines.reserve(256);
	m_pending_compute_pipelines.reserve(128);
	InitializeDriverCache();
	PipelineCacheLog("Host GPU tuning: {} / descriptor-cache={} / background-pipeline=true",
	                 m_graphics.GetHostGpuArchitectureName(),
	                 m_graphics.GetHostGpuTuningProfile().descriptor_cache_entries);
}

PipelineCache::~PipelineCache() {
	if (m_pipeline_compiler) {
		m_pipeline_compiler->WaitIdle();
	}
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	for (const auto& [key, source]: m_program_cache->programs) {
		(void)key;
		for (const auto& [static_hash, permutations]: source.permutations) {
			(void)static_hash;
			for (const auto& permutation: permutations) {
				m_graphics.device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	m_driver_cache_path = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()), &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)",
			                 path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.sType           = vk::StructureType::ePipelineCacheCreateInfo;
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 VulkanToString(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", VulkanToString(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	if (m_pipeline_compiler) {
		m_pipeline_compiler->WaitIdle();
	}
	Common::LockGuard lock(m_mutex);
	if (m_driver_cache == nullptr) {
		return;
	}

	PipelineCacheLog(
	    "Pipeline cache stats: shaders compiled={}, shader hits={}, shader misses={}, gfx pipelines={}, compute pipelines={}",
	    m_program_cache->num_compiled, m_program_cache->num_cache_hits,
	    m_program_cache->num_cache_misses, m_graphics_pipelines.size(), m_compute_pipelines.size());

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	std::lock_guard driver_cache_lock(m_driver_cache_mutex);
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 VulkanToString(result), size);
		return;
	}
	payload.resize(size);
	auto prefix = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

ShaderProgram PipelineCache::GetVertexProgram(const HW::VertexShaderInfo& regs,
                                              const HW::ShaderRegisters&  sh,
                                              const HW::Context&          context,
                                              ShaderVertexInputInfo&      input_info) {
	const auto params = PrepareProgram(regs, sh, input_info);
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = input_info.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	Common::LockGuard lock(m_mutex);
	return m_program_cache->Get(params, input_info);
}

ShaderProgram PipelineCache::GetPixelProgram(
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    const ShaderVertexInputInfo&                        vertex_info,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo&                               input_info) {
	const auto params = PrepareProgram(regs, sh, vertex_info, target_export_mapping, input_info);
	Common::LockGuard lock(m_mutex);
	return m_program_cache->Get(params, input_info);
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.needs_lds_barriers = !m_graphics.compute_wave64_supported;
	const auto params             = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	return m_program_cache->Get(params, input_info);
}

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(
    const GraphicsPipelineKey& key) const noexcept {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.rendering.stencil_format));
	PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.vs_shader_id));
	PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.ps_shader_id));
	const uint64_t static_hash = XXH3_64bits(&key.static_params, sizeof(key.static_params));
	PipelineKeyHash::Mix(hash, static_cast<std::size_t>(static_hash));
	if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
		PipelineKeyHash::Mix(hash, static_cast<std::size_t>(static_hash >> 32u));
	}
	return hash;
}

std::size_t PipelineCache::ComputePipelineKeyHash::operator()(
    const ComputePipelineKey& key) const noexcept {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.cs_shader_id));
	if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
		PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.cs_shader_id >> 32u));
	}
	return hash;
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::GraphicsPipelineTicket PipelineCache::BeginGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    const ShaderVertexInputInfo& vs_input_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const ShaderProgram& vertex_program,
    const ShaderProgram& pixel_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	auto& ctx = command.GetRegisters();

	const HW::BlendColor& bclr                                     = ctx.GetBlendColor();
	uint32_t              color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_mask[i] =
		    (colors[i].image_id ? colors[i].export_mapping.ApplyMask(render_target_mask_slot(
		                              ctx.GetRenderTargetMask(), colors[i].target_slot))
		                        : 0);
	}
	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	PipelineStaticParameters static_params {};
	GraphicsPipeline         p {};
	p.ps_shader_id = ps_id;
	p.vs_shader_id = vs_id;

	static_params.color_count = color_count;
	PipelineRenderingState rendering {};
	rendering.color_count       = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		EXIT_IF(!colors[i].image_id || colors[i].format == vk::Format::eUndefined);
		rendering.color_formats[i] = colors[i].format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].samples;
		} else if (attachment_samples != colors[i].samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].samples);
		}
	}
	const bool with_depth =
	    depth.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects = ImageViewOps::DepthAspectMask(depth.format);
		rendering.depth_format =
		    aspects & vk::ImageAspectFlagBits::eDepth ? depth.format : vk::Format::eUndefined;
		rendering.stencil_format =
		    aspects & vk::ImageAspectFlagBits::eStencil ? depth.format : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.samples;
		} else if (attachment_samples != depth.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.with_depth         = with_depth;
	static_params.depth_test_enable  = depth.depth_test_enable;
	static_params.depth_write_enable = (depth.depth_write_enable && !depth.depth_clear_enable);
	static_params.depth_compare_op   = depth.depth_compare_op;
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	static_params.stencil_test_enable      = depth.stencil_test_enable;
	static_params.stencil_front            = depth.stencil_static_front;
	static_params.stencil_back             = depth.stencil_static_back;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		static_params.color_mask[i] = color_mask[i];
	}
	const bool rect_list     = topology == vk::PrimitiveTopology::ePatchList;
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;

	for (uint32_t i = 0; i < color_count; i++) {
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[i]       = bc.color_srcblend;
		static_params.color_comb_fcn[i]       = bc.color_comb_fcn;
		static_params.color_destblend[i]      = bc.color_destblend;
		static_params.alpha_srcblend[i]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[i]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[i]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[i] = bc.separate_alpha_blend;
		static_params.blend_enable[i]         = bc.enable;
		static_params.blend_bypass[i]         = rt.info.blend_bypass;
	}
	static_params.blend_color_red   = bclr.red;
	static_params.blend_color_green = bclr.green;
	static_params.blend_color_blue  = bclr.blue;
	static_params.blend_color_alpha = bclr.alpha;

	NormalizeStaticParamsForDynamicState(static_params);

	GraphicsPipelineKey key {};
	key.rendering     = rendering;
	key.vs_shader_id  = p.vs_shader_id;
	key.ps_shader_id  = p.ps_shader_id;
	key.static_params = static_params;

	std::shared_ptr<std::promise<GraphicsPipeline*>> compile_promise;
	std::shared_future<GraphicsPipeline*>           compile_future;
	{
		Common::LockGuard lock(m_mutex);
		if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
			return {.ready = iter->second.get()};
		}
		if (auto pending = m_pending_graphics_pipelines.find(key);
		    pending != m_pending_graphics_pipelines.end()) {
			return {.pending = pending->second};
		}
		compile_promise = std::make_shared<std::promise<GraphicsPipeline*>>();
		compile_future  = compile_promise->get_future().share();
		m_pending_graphics_pipelines.emplace(key, compile_future);
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64
		     " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	// Everything captured below is immutable host-side state. The worker never touches guest
	// memory or the command buffer. This lets expensive Vulkan pipeline/driver shader compilation
	// overlap descriptor preparation, buffer uploads and render-target setup on the render thread.
	const auto vs_input_copy = vs_input_info;
	const auto ps_input_copy = ps_active ? std::optional<ShaderPixelInputInfo>(*ps_input_info)
	                                     : std::optional<ShaderPixelInputInfo> {};
	const auto vertex_module = vertex_program.module;
	const auto pixel_module  = pixel_program.module;
	m_pipeline_compiler->Enqueue(
	    [this, key, p, rendering, static_params, vs_input_copy, ps_input_copy, vertex_module,
	     pixel_module, compile_promise, vs_id, ps_id]() mutable {
		    auto cached = std::make_unique<GraphicsPipeline>(p);
		    const ShaderPixelInputInfo* ps_ptr = ps_input_copy ? &*ps_input_copy : nullptr;
		    LogPipelineTrace("CreatePipelineInternal async begin", vs_id, ps_id);
		    {
			    std::lock_guard driver_cache_lock(m_driver_cache_mutex);
			    CreatePipelineInternal(m_graphics, *cached, rendering, vs_input_copy, vertex_module,
			                           ps_ptr, pixel_module, static_params, m_driver_cache);
		    }
		    LogPipelineTrace("CreatePipelineInternal async done", vs_id, ps_id);
		    EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
		    EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

		    GraphicsPipeline* result = nullptr;
		    {
			    Common::LockGuard lock(m_mutex);
			    auto [iter, inserted] = m_graphics_pipelines.emplace(key, std::move(cached));
			    result = iter->second.get();
			    m_pending_graphics_pipelines.erase(key);
			    EXIT_IF(result == nullptr);
			    (void)inserted;
		    }
		    compile_promise->set_value(result);
	    });

	return {.pending = std::move(compile_future)};
}

PipelineCache::GraphicsPipeline& PipelineCache::CreateGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    const ShaderVertexInputInfo& vs_input_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const ShaderProgram& vertex_program,
    const ShaderProgram& pixel_program) {
	return BeginGraphicsPipeline(colors, depth, vs_input_info, command, ps_input_info, topology,
	                             primitive_restart_enable, vertex_program, pixel_program)
	    .Wait();
}

PipelineCache::ComputePipelineTicket
PipelineCache::BeginComputePipeline(const ShaderComputeInputInfo& input_info,
                                    const ShaderProgram& compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	ComputePipeline p {};
	p.cs_shader_id = compute_program.id;

	ComputePipelineKey key {};
	key.cs_shader_id = p.cs_shader_id;

	std::shared_ptr<std::promise<ComputePipeline*>> compile_promise;
	std::shared_future<ComputePipeline*>           compile_future;
	{
		Common::LockGuard lock(m_mutex);
		if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
			return {.ready = iter->second.get()};
		}
		if (auto pending = m_pending_compute_pipelines.find(key);
		    pending != m_pending_compute_pipelines.end()) {
			return {.pending = pending->second};
		}
		compile_promise = std::make_shared<std::promise<ComputePipeline*>>();
		compile_future  = compile_promise->get_future().share();
		m_pending_compute_pipelines.emplace(key, compile_future);
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	// Compile while the render thread prepares descriptors, buffer views, image transitions and
	// BDA state. The worker only reads immutable shader metadata and host Vulkan handles.
	const auto input_copy    = input_info;
	const auto compute_module = compute_program.module;
	m_pipeline_compiler->Enqueue(
	    [this, key, p, input_copy, compute_module, compile_promise]() mutable {
		    auto cached = std::make_unique<ComputePipeline>(p);
		    {
			    std::lock_guard driver_cache_lock(m_driver_cache_mutex);
			    CreatePipelineInternal(m_graphics, *cached, input_copy, compute_module,
			                           m_driver_cache);
		    }
		    EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
		    EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

		    ComputePipeline* result = nullptr;
		    {
			    Common::LockGuard lock(m_mutex);
			    auto [iter, inserted] = m_compute_pipelines.emplace(key, std::move(cached));
			    result = iter->second.get();
			    m_pending_compute_pipelines.erase(key);
			    EXIT_IF(result == nullptr);
			    (void)inserted;
		    }
		    compile_promise->set_value(result);
	    });

	return {.pending = std::move(compile_future)};
}

PipelineCache::ComputePipeline&
PipelineCache::CreateComputePipeline(const ShaderComputeInputInfo& input_info,
                                     const ShaderProgram& compute_program) {
	return BeginComputePipeline(input_info, compute_program).Wait();
}
} // namespace Libs::Graphics
