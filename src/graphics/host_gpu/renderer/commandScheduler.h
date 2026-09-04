#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <condition_variable>
#include <mutex>

#include <queue>

#include <thread>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler {
public:
	// Graphics scheduler: owns the master timeline and submits on the graphics
	// family.
	CommandScheduler(RenderContext& context, GraphicContext& graphics);
	// Compute scheduler: shares the graphics scheduler's master timeline so
	// ticks stay globally ordered across both queues, and submits on the
	// dedicated async-compute family (the graphics family when the device has
	// no separate compute family).
	CommandScheduler(RenderContext& context, GraphicContext& graphics,
	                 MasterSemaphore& master, bool compute_family);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           BeginRendering(const RenderState& state);
	void           EndRendering();
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           FlushAndWait();
	void           Finish();
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool             Active() const noexcept { return m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	// The scheduler whose submission is currently being recorded. The render
	// executor takes scheduler-side effects on behalf of the current command
	// buffer (e.g. utility-buffer uploads); routing those through the
	// recording scheduler keeps compute submissions off the graphics queue.
	[[nodiscard]] static CommandScheduler& Recording();
	[[nodiscard]] static bool              HasRecording() noexcept;
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master, uint32_t family);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BindCurrent();
	void BeginNext();
	void PopPendingOperations(bool refresh_gpu_tick);
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);

	// Master timeline: owned by the graphics scheduler, borrowed by the
	// compute scheduler. A single timeline keeps deferred-destroy ticks and
	// cross-queue waits globally ordered.
	std::unique_ptr<MasterSemaphore> m_owned_master;
	MasterSemaphore&                 m_master;
	RenderContext&                   m_context;
	GraphicContext&                  m_graphics;
	CommandPool                      m_command_pool;
	vk::Queue                        m_submit_queue = nullptr;
	CommandBuffer                    m_command;
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
	HW::Context*                 m_registers            = nullptr;
	HW::UserConfig*              m_user_config          = nullptr;
	HW::Shader*                  m_shaders              = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
