#include "loader/runtimeLinker.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/platform/sysDbg.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/libcPayload.h"
#include "loader/elf.h"
#include "loader/gamePatch.h"
#include "loader/jit.h"
#include "loader/redZonePatcher.h"
#include "loader/symbolDatabase.h"
#include "loader/x64InstructionEmulator.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/uio.h>
#include <unistd.h>
#endif
#endif

namespace Libs::LibKernel {
void SetProgName(const std::string& name);
} // namespace Libs::LibKernel

namespace Loader {

Program::Program() = default;

Program::~Program() = default;

static void FreeTlsBlock(ThreadLocalStorage::Block* block) {
	if (block == nullptr || block->ptr == nullptr) {
		return;
	}

	if (block->free_func != nullptr) {
		block->free_func(block->ptr);
	} else if (block->vm_alloc) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(reinterpret_cast<uint64_t>(block->ptr),
		                                                  block->alloc_size));
	} else {
		delete[] block->ptr;
	}

	block->ptr        = nullptr;
	block->free_func  = nullptr;
	block->vm_alloc   = false;
	block->alloc_size = 0;
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
	return alignment != 0 ? (value + alignment - 1) & ~(alignment - 1) : value;
}

ThreadLocalStorage::~ThreadLocalStorage() {
	for (auto& [_, block]: tlss) {
		FreeTlsBlock(&block);
	}
}

#pragma pack(1)

struct EntryParams {
	int         argc;
	uint32_t    pad;
	const char* argv[16];
};

struct PayloadArgs {
	int (KYTY_SYSV_ABI *sys_dynlib_dlsym)(int, const char*, void*);
	int*      rwpipe;
	int*      rwpair;
	intptr_t  kpipe_addr;
	intptr_t  kdata_base_addr;
	int*      payloadout;
	uint8_t   kpipe_scratch[64];
};

#pragma pack()

using atexit_func_t = KYTY_SYSV_ABI void (*)();
using entry_func_t  = KYTY_SYSV_ABI void (*)(EntryParams* params, atexit_func_t atexit_func);
using module_ini_fini_func_t = KYTY_SYSV_ABI int (*)(size_t args, const void* argp,
                                                     module_func_t func);

static SymbolDatabase*           g_payload_symbols       = nullptr;
static uint64_t                  g_payload_dlsym_bridge  = 0;
static int                       g_payload_argc          = 1;
static const char*               g_payload_argv[16]      = {"KytyPayload"};
static char*                     g_payload_environment[] = {nullptr};
static const char                g_payload_progname[]    = "crispy-doom";
static char                       g_payload_elf_path[512] = "";
static uint64_t                   g_payload_proc_guest    = 0;
static const char*               g_payload_progname_ptr  = g_payload_progname;
static int                       g_payload_isthreaded    = 0;
static uint64_t                  g_payload_metadata_guest = 0;
// Last system path the payload CRT stat()ed: sprx_open stats a module before
// opening it, so this tracks which module the CRT is currently loading. The
// fake dynlib record's kernel-flavor node (id 1) renames itself to this path
// so kernel_dynlib_obj(1) answers with the module being opened.
static char     g_payload_last_module_path[512] = "";
static uint64_t g_payload_dynlib_name_slot      = 0;

static KYTY_SYSV_ABI int PayloadGetArgc() {
	return g_payload_argc;
}

static bool PayloadOwnsRuntimeSymbol(std::string_view symbol) {
	return symbol == "getargc" || symbol == "getargv" || symbol == "environ" ||
	       symbol == "__progname" || symbol == "__isthreaded" || symbol == "exit" ||
	       symbol == "_exit" || symbol == "_Exit";
}

static KYTY_SYSV_ABI char** PayloadGetArgv() {
	return const_cast<char**>(g_payload_argv);
}

static KYTY_SYSV_ABI int PayloadDlsym(int /*handle*/, const char* name, void* destination) {
	static std::atomic_uint32_t trace_count {0};
	if (name != nullptr && trace_count.fetch_add(1) < 64) {
		LOGF("PS5 payload dlsym trace: %s\n", name);
	}
	if (name == nullptr || destination == nullptr) {
		return -1;
	}
	auto* out = static_cast<void**>(destination);
	const std::string_view symbol {name};
	if (symbol == "sceKernelDlsym" || symbol == "getpid") {
		*out = reinterpret_cast<void*>(g_payload_dlsym_bridge);
		return 0;
	}
	if (symbol == "getargc") {
		*out = reinterpret_cast<void*>(PayloadGetArgc);
		return 0;
	}
	if (symbol == "getargv") {
		*out = reinterpret_cast<void*>(PayloadGetArgv);
		return 0;
	}
	if (symbol == "environ") {
		*out = g_payload_environment;
		return 0;
	}
	if (symbol == "__progname") {
		*out = &g_payload_progname_ptr;
		return 0;
	}
	if (symbol == "__isthreaded") {
		*out = &g_payload_isthreaded;
		return 0;
	}
	if (symbol == "exit" || symbol == "_exit" || symbol == "_Exit") {
		*out = reinterpret_cast<void*>(Libs::LibcPayload::ExitProcessNow);
		return 0;
	}
	if (g_payload_symbols != nullptr) {
		for (const auto type: {SymbolType::Func, SymbolType::Object}) {
			if (const auto* record = g_payload_symbols->FindByName(name, type); record != nullptr) {
				*out = reinterpret_cast<void*>(record->vaddr);
				return 0;
			}
		}
	}
	LOGF("PS5 payload dlsym unresolved: %s\n", name);
	return -1;
}

// The payload CRT's internal rtld reads the ELF itself through raw FreeBSD
// syscalls (open/read/lseek/close/stat/mmap). Route them to the HLE file
// system so so_open/__rtld_find_file succeed and main() is reached.

// Guest-visible fd -> HLE descriptor. The HLE numbering space (DESCRIPTOR_MIN
// and up) does not overlap with the small guest fds handed out here.
static Common::Mutex                payload_fd_mutex;
static std::unordered_map<int, int> payload_fd_map;
static int                          payload_fd_next = 0;

static uint64_t              payload_mmap_base = 0;
static uint64_t              payload_mmap_pos  = 0;
static constexpr uint64_t    payload_mmap_size = 0x1000000ull;

static bool PayloadReadGuestBytes(uint64_t guest_address, void* out, size_t size) {
	if (guest_address == 0 || out == nullptr || size == 0) {
		return false;
	}
	if (Libs::LibKernel::Memory::TryReadBacking(guest_address, out, size)) {
		return true;
	}
	// Guest stacks are ordinary fixed virtual mappings, not part of the
	// direct-memory backing alias. Raw payload CRT syscalls use both kinds.
	std::memcpy(out, reinterpret_cast<const void*>(guest_address), size);
	return true;
}

static bool PayloadWriteGuestBytes(uint64_t guest_address, const void* data, size_t size) {
	if (guest_address == 0 || data == nullptr || size == 0) {
		return false;
	}
	if (Libs::LibKernel::Memory::TryWriteBacking(guest_address, data, size)) {
		return true;
	}
	std::memcpy(reinterpret_cast<void*>(guest_address), data, size);
	return true;
}

static bool PayloadIsHleSystemModule(std::string_view path) {
	return (Common::StartsWith(path, "/system/") || Common::StartsWith(path, "/system_ex/") ||
	        Common::StartsWith(path, "/user/homebrew/")) &&
	       path.find("/lib/") != std::string_view::npos &&
	       Common::EndsWith(path, ".sprx");
}

static bool PayloadReadGuestString(uint64_t guest_address, std::string& out,
                                   size_t max_length = 4096) {
	out.clear();
	if (guest_address == 0 || max_length == 0) {
		return false;
	}

	out.reserve(std::min<size_t>(max_length, 256));
	for (size_t i = 0; i < max_length; i++) {
		char ch = '\0';
		if (guest_address > UINT64_MAX - i ||
		    !PayloadReadGuestBytes(guest_address + i, &ch, sizeof(ch))) {
			out.clear();
			return false;
		}
		if (ch == '\0') {
			return true;
		}
		out.push_back(ch);
	}

	out.clear();
	return false;
}

static uint64_t PayloadMmapBase() {
	if (payload_mmap_base == 0) {
		payload_mmap_base = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0x800200000ull, payload_mmap_size, Common::VirtualMemory::Mode::ReadWrite,
		    "payload_rtld_mmap_window");
		if (payload_mmap_base != 0) {
			payload_mmap_pos = payload_mmap_base;
		}
	}
	return payload_mmap_base;
}

static KYTY_SYSV_ABI int64_t PayloadSyscall(uint64_t number, uint64_t arg1, uint64_t arg2,
                                            uint64_t arg3, uint64_t arg4, uint64_t arg5,
                                            uint64_t arg6) {
	static std::atomic_uint32_t trace_count {0};
	if (trace_count.fetch_add(1) < 512) {
		LOGF("PS5 payload syscall trace: %" PRIu64 " (%016" PRIx64 ", %016" PRIx64
		     ", %016" PRIx64 ")\n",
		     number, arg1, arg2, arg3);
	}

	// Kernel-pipe syscall family used by the payload CRT while it discovers the
	// running kernel. The emulator has no kernel pipe; answer from host state so
	// the CRT binds its runtime and reaches the HLE libc.
	if (number == 20) { // FreeBSD SYS_getpid
		return 1;
	}
		if (number == 0x289) {
			// sys_ktrace/kern_rw style primitive: (cmd, from, to, out). The
			// payload CRT's kernel_get_proc uses it to copy 8 bytes from a guest
			// address into out (arg4). Serve any read inside the payload's own
			// address space so the crafted proc/dynlib lists are readable.
			if (arg4 == 0 || arg2 == 0) {
				return -1;
			}
			uint64_t value = 0;
			if (!PayloadReadGuestBytes(arg2, &value, sizeof(value))) {
				return -1;
			}
			if (!PayloadWriteGuestBytes(arg4, &value, sizeof(value))) {
				return -1;
			}
			return 0;
		}
	if (number == 616) { // sys_thr_get_name / payload path query
		if (arg2 == 0 || arg3 == 0) {
			return -1;
		}
		// __rtld_payload_new(pid, path, size) asks the kernel for the running
		// executable's path and later opens that file to parse its own ELF
		// headers (payload_open -> so_open -> __rtld_find_file). Report the
		// mounted /app0 path of the loaded game so the CRT can find itself.
		const char* path = g_payload_elf_path[0] != '\0' ? g_payload_elf_path : "KytyPayload";
		const auto len   = std::strlen(path) + 1;
		if (arg3 < len) {
			return -1;
		}
		if (!PayloadWriteGuestBytes(arg2, path, len)) {
			return -1;
		}
		return 0;
	}
	if (number == 601) { // sys_mdbg_service
		// CRT klog uses service 7 to emit startup diagnostics. Mirror its guest
		// string to the emulator log, then report success like the retail service.
		if (arg2 != 0) {
			char message[512] {};
			if (PayloadReadGuestBytes(arg2, message, sizeof(message) - 1)) {
				message[sizeof(message) - 1] = '\0';
				LOGF("PS5 payload klog: %s\n", message);
			}
		}
		return 0;
	}
	if (number == 5) { // FreeBSD SYS_open(path, flags, mode)
		std::string path;
		if (!PayloadReadGuestString(arg1, path)) {
			return -1;
		}
		const auto handle = Libs::LibKernel::FileSystem::KernelOpen(
		    path.c_str(), static_cast<int>(arg2 & 0xffff), static_cast<uint16_t>(arg3));
		if (handle < 0) {
			LOGF("PS5 payload syscall open: %s -> not found\n", path.c_str());
			return -1;
		}
		Common::LockGuard lock(payload_fd_mutex);
		const int guest_fd      = payload_fd_next++;
		payload_fd_map[guest_fd] = handle;
		LOGF("PS5 payload syscall open: %s -> fd %d\n", path.c_str(), guest_fd);
		return guest_fd;
	}
	if (number == 3) { // FreeBSD SYS_read(fd, buf, count)
		int handle = -1;
		{
			Common::LockGuard lock(payload_fd_mutex);
			const auto it = payload_fd_map.find(static_cast<int>(arg1));
			if (it == payload_fd_map.end()) {
				return -1;
			}
			handle = it->second;
		}
		return Libs::LibKernel::FileSystem::KernelRead(handle, reinterpret_cast<void*>(arg2),
		                                                static_cast<size_t>(arg3));
	}
	if (number == 6) { // FreeBSD SYS_close(fd)
		int handle = -1;
		{
			Common::LockGuard lock(payload_fd_mutex);
			const auto it = payload_fd_map.find(static_cast<int>(arg1));
			if (it == payload_fd_map.end()) {
				return -1;
			}
			handle = it->second;
			payload_fd_map.erase(it);
		}
		return Libs::LibKernel::FileSystem::KernelClose(handle);
	}
	if (number == 0x1DE) { // FreeBSD SYS_lseek(fd, offset, whence)
		int handle = -1;
		{
			Common::LockGuard lock(payload_fd_mutex);
			const auto it = payload_fd_map.find(static_cast<int>(arg1));
			if (it == payload_fd_map.end()) {
				return -1;
			}
			handle = it->second;
		}
		return Libs::LibKernel::FileSystem::KernelLseek(handle, static_cast<int64_t>(arg2),
		                                                 static_cast<int>(arg3));
	}
	if (number == 0xBC) { // FreeBSD SYS_stat(path, sb)
		std::string path;
		if (!PayloadReadGuestString(arg1, path) || arg2 == 0) {
			LOGF("PS5 payload syscall stat: invalid guest path=0x%016" PRIx64
			     " stat=0x%016" PRIx64 "\n",
			     arg1, arg2);
			return -1;
		}
		LOGF("PS5 payload syscall stat: %s\n", path.c_str());
		// Remember the module the CRT is probing so the fake kernel dynlib
		// record's kernel-flavor node (id 1) can rename itself for the
		// sprx_open special branch (libkernel*.sprx -> dynlib_obj(handle 1)).
		if (PayloadIsHleSystemModule(path) &&
		    path.size() < sizeof(g_payload_last_module_path)) {
			std::memcpy(g_payload_last_module_path, path.c_str(), path.size() + 1);
			if (g_payload_dynlib_name_slot != 0) {
				std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(
				                g_payload_dynlib_name_slot)),
				            g_payload_last_module_path, path.size() + 1);
			}
		}
		// Build into host storage, then copy through the guest backing API. The
		// payload ELF can use a relocatable guest mapping that is not equal to a
		// host pointer on Windows.
		Libs::LibKernel::FileSystem::FileStat stat {};
		const bool hle_module = PayloadIsHleSystemModule(path);
		if (!hle_module && Libs::LibKernel::FileSystem::KernelStat(path.c_str(), &stat) != 0) {
			return -1;
		}
		if (hle_module) {
			stat.st_mode = 0100777;
			stat.st_size = 1;
			stat.st_blksize = 512;
			stat.st_blocks = 1;
		}
		if (
		    !PayloadWriteGuestBytes(arg2, &stat, sizeof(stat))) {
			return -1;
		}
		return 0;
	}
	if (number == 0x25A) { // PS4/PS5 SYS_randomized_path
		// Retail kernels may rewrite a path to a randomized sandbox name. The
		// emulator has a stable /app0 mount, so return the caller's size unchanged
		// and let the payload CRT continue with its normal fallback search.
		if (arg3 != 0) {
			uint64_t size = 0;
			if (!PayloadReadGuestBytes(arg3, &size, sizeof(size))) {
				return -1;
			}
			if (size == 0) {
				size = 0xff;
				if (!PayloadWriteGuestBytes(arg3, &size, sizeof(size))) {
					return -1;
				}
			}
		}
		return -1;
	}
	if (number == 0x146) { // FreeBSD SYS_getcwd(buf, size)
		static constexpr char cwd[] = "/app0";
		if (arg1 == 0 || arg2 < sizeof(cwd) ||
		    !PayloadWriteGuestBytes(arg1, cwd, sizeof(cwd))) {
			return -1;
		}
		return 0;
	}
	if (number == 0x1DD) { // FreeBSD SYS_mmap(addr, len, prot, flags, fd, offset)
		const uint64_t length = arg2;
		if (length == 0) {
			return -1;
		}
		const uint64_t base = PayloadMmapBase();
		if (base == 0 || payload_mmap_pos + length > base + payload_mmap_size) {
			LOGF("PS5 payload syscall mmap: out of window (len = %" PRIu64 ")\n", length);
			return -1;
		}
		const uint64_t result = payload_mmap_pos;
		payload_mmap_pos += (length + 0x3fff) & ~0x3fffull;
		const int guest_fd = static_cast<int>(arg5);
		if (guest_fd >= 0) { // fd-backed mapping: pread the file contents
			int handle = -1;
			{
				Common::LockGuard lock(payload_fd_mutex);
				const auto it = payload_fd_map.find(guest_fd);
				if (it != payload_fd_map.end()) {
					handle = it->second;
				}
			}
			if (handle >= 0) {
				const auto read = Libs::LibKernel::FileSystem::KernelPread(
				    handle, reinterpret_cast<void*>(result), static_cast<size_t>(length),
				    static_cast<int64_t>(arg6));
				if (read < 0) {
					LOGF("PS5 payload syscall mmap: pread failed\n");
				}
			}
		}
		LOGF("PS5 payload syscall mmap: len = %" PRIu64 " -> 0x%016" PRIx64 "\n", length,
		     result);
		return static_cast<int64_t>(result);
	}
	if (number == 73) { // FreeBSD SYS_munmap: window memory stays mapped
		return 0;
	}
	if (number == 74) { // FreeBSD SYS_msync: no host writeback needed
		return 0;
	}
	LOGF("PS5 payload syscall not implemented: %" PRIu64 "\n", number);
	return -1;
}

static uint64_t CreatePayloadDlsymBridge() {
	constexpr uint64_t bridge_size = 64;
	auto bridge = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0x800000000ull, bridge_size, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    "payload_dlsym_bridge");
	if (bridge == 0) {
		return 0;
	}
	auto* code = reinterpret_cast<uint8_t*>(bridge);
	std::memset(code, 0x90, bridge_size);
	// +0: jmp [rip + 0x32] -> PayloadDlsym pointer at +56.
	const uint8_t resolver_jump[] = {0xff, 0x25, 0x32, 0x00, 0x00, 0x00};
	std::memcpy(code, resolver_jump, sizeof(resolver_jump));
	// +10: translate FreeBSD syscall register ABI then call PayloadSyscall.
	// Guest FreeBSD ABI here: rax=number rdi=a1 rsi=a2 rdx=a3 rcx=a4 r8=a5 r9=a6.
	// SysV C ABI wants: rdi=number rsi=a1 rdx=a2 rcx=a3 r8=a4 r9=a5 [rsp+8]=a6.
	const uint8_t syscall_stub[] = {
	    0x41, 0x51,                         // push r9 (a6, becomes [rsp+8] after the next push)
	    0x41, 0x51,                         // push r9 (saved shadow/alignment)
	    0x4d, 0x89, 0xc1,                   // mov r9, r8   (a5)
	    0x4d, 0x89, 0xd0,                   // mov r8, r10   (a4)
	    0x48, 0x89, 0xd1,                   // mov rcx, rdx (a3)
	    0x48, 0x89, 0xf2,                   // mov rdx, rsi (a2)
	    0x48, 0x89, 0xfe,                   // mov rsi, rdi (a1)
	    0x48, 0x89, 0xc7,                   // mov rdi, rax (number)
	    0x48, 0xb8,                         // mov rax, PayloadSyscall
	};
	std::memcpy(code + 10, syscall_stub, sizeof(syscall_stub));
	const auto dispatcher = reinterpret_cast<uint64_t>(PayloadSyscall);
	std::memcpy(code + 10 + sizeof(syscall_stub), &dispatcher, sizeof(dispatcher));
	const uint8_t tail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x10, 0xc3}; // call rax; add rsp,16; ret
	std::memcpy(code + 10 + sizeof(syscall_stub) + sizeof(dispatcher), tail, sizeof(tail));
	const auto resolver = reinterpret_cast<uint64_t>(PayloadDlsym);
	std::memcpy(code + 56, &resolver, sizeof(resolver));
	Common::VirtualMemory::FlushInstructionCache(bridge, bridge_size);
	return bridge;
}

// PS5 payload SDK CRTs (ps5-payload-sdk kernel flavor) boot through
// kernel-exploit primitives that only exist in a jailbroken kernel:
// kernel_dynlib_dlsym resolves symbols by walking the kernel's dynlib data
// through a kernel pipe, and __patch_init rewrites ucred caps. Neither can
// work in the emulator, so the guest copies are patched to host trampolines
// that answer from the HLE symbol database instead.

static KYTY_SYSV_ABI uint64_t PayloadKernelDynlibDlsym(int /*handle*/, int /*flags*/,
                                                       const char* name) {
	if (name == nullptr || g_payload_symbols == nullptr) {
		return 0;
	}
	// The CRT flags the kernel's dynlib dlsym as bound by writing 1 into the
	// returned address; hand it scratch instead of an HLE function body.
	static uint64_t g_payload_dlsym_enable_flag = 0;
	if (std::strcmp(name, "sceKernelDlsym") == 0) {
		LOGF("PS5 payload kernel_dynlib_dlsym: sceKernelDlsym -> enable flag\n");
		return reinterpret_cast<uint64_t>(&g_payload_dlsym_enable_flag);
	}

	// Payload CRT owns process startup state.  The normal libkernel exports
	// read LibC's module-start globals, which are never initialized for a
	// standalone ET_SYSV_DYN payload.  Resolve these names through the payload
	// bridge first so argc/argv/environment supplied by --guest-arg reach main().
	if (PayloadOwnsRuntimeSymbol(name)) {
		void* bridge_out = nullptr;
		if (PayloadDlsym(-1, name, &bridge_out) == 0 && bridge_out != nullptr) {
			LOGF("PS5 payload kernel_dynlib_dlsym: %s -> 0x%016" PRIx64 " (bridge)\n", name,
			     reinterpret_cast<uint64_t>(bridge_out));
			return reinterpret_cast<uint64_t>(bridge_out);
		}
	}
	for (const auto type: {Loader::SymbolType::Func, Loader::SymbolType::Object,
	                       Loader::SymbolType::NoType}) {
		if (const auto* record = g_payload_symbols->FindByName(name, type);
		    record != nullptr && record->vaddr != 0) {
			LOGF("PS5 payload kernel_dynlib_dlsym: %s -> 0x%016" PRIx64 "\n", name,
			     record->vaddr);
			return record->vaddr;
		}
	}

	// Built-ins the CRT expects from the runtime (getargc/getargv/environ/
	// __progname/exit) live in the dlsym bridge, not the symbol database.
	void* bridge_out = nullptr;
	if (PayloadDlsym(-1, name, &bridge_out) == 0 && bridge_out != nullptr) {
		LOGF("PS5 payload kernel_dynlib_dlsym: %s -> 0x%016" PRIx64 " (bridge)\n", name,
		     reinterpret_cast<uint64_t>(bridge_out));
		return reinterpret_cast<uint64_t>(bridge_out);
	}

	LOGF("PS5 payload kernel_dynlib_dlsym unresolved: %s\n", name);
	return 0;
}

// The payload rtld resolves the executable's imports through
// sprx_sym2addr(lib, name), which NID-encodes the name and walks the module
// symtab copied out of the kernel dynlib record. The NID encoding uses a
// custom-IV hash that only exists inside the guest CRT; instead of
// reproducing it, the guest sprx_sym2addr is patched to a host trampoline
// that resolves the plain name straight from the HLE database.
static KYTY_SYSV_ABI uint64_t PayloadSprxSym2Addr(const void* /*lib*/, const char* name) {
	// The rtld occasionally probes the resolver with raw symtab offsets
	// instead of resolved name pointers (and with null libraries); treat any
	// non-canonical guest pointer as "not found" instead of faulting.
	if (name == nullptr || g_payload_symbols == nullptr ||
	    reinterpret_cast<uintptr_t>(name) < 0x10000) {
		return 0;
	}
	if (PayloadOwnsRuntimeSymbol(name)) {
		void* bridge_out = nullptr;
		if (PayloadDlsym(-1, name, &bridge_out) == 0 && bridge_out != nullptr) {
			LOGF("PS5 payload sprx_sym2addr: %s -> 0x%016" PRIx64 " (bridge)\n", name,
			     reinterpret_cast<uint64_t>(bridge_out));
			return reinterpret_cast<uint64_t>(bridge_out);
		}
	}
	for (const auto type: {Loader::SymbolType::Func, Loader::SymbolType::Object,
	                       Loader::SymbolType::NoType}) {
		if (const auto* record = g_payload_symbols->FindByName(name, type);
		    record != nullptr && record->vaddr != 0) {
			LOGF("PS5 payload sprx_sym2addr: %s -> 0x%016" PRIx64 "\n", name, record->vaddr);
			return record->vaddr;
		}
	}

	void* bridge_out = nullptr;
	if (PayloadDlsym(-1, name, &bridge_out) == 0 && bridge_out != nullptr) {
		LOGF("PS5 payload sprx_sym2addr: %s -> 0x%016" PRIx64 " (bridge)\n", name,
		     reinterpret_cast<uint64_t>(bridge_out));
		return reinterpret_cast<uint64_t>(bridge_out);
	}

	LOGF("PS5 payload sprx_sym2addr unresolved: %s\n", name);
	return 0;
}

static bool PayloadWriteGuestCode(uint64_t vaddr, const void* code, size_t size) {
	constexpr uint64_t page = 4096;
	if (!Libs::LibKernel::Memory::ProtectGuestMemory(
	        vaddr, page, Common::VirtualMemory::Mode::ExecuteReadWrite)) {
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(vaddr)), code, size);
	Common::VirtualMemory::FlushInstructionCache(vaddr, page);
	return true;
}

static uint64_t CreatePayloadHostTrampoline(const char* name, uint64_t host_func) {
	auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0x800100000ull, 4096, Common::VirtualMemory::Mode::ExecuteReadWrite, name);
	if (page == 0) {
		return 0;
	}
	// movabs rax, host_func; jmp rax
	uint8_t stub[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
	std::memcpy(stub + 2, &host_func, sizeof(host_func));
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(page)), stub, sizeof(stub));
	Common::VirtualMemory::FlushInstructionCache(page, 4096);
	return page;
}

// kernel_copyout(kernel_addr, guest_buf, len) / kernel_copyin(guest_buf,
// kernel_addr, len): both are plain byte copies through the exploit pipe on a
// real console; here they can be a raw host memcpy because guest addresses are
// host addresses in this emulator.
// kernel_copyout/kernel_copyin(kernel_addr, guest_buf, len): both are plain
// byte copies through the exploit pipe on a real console; here they can be a
// raw host memcpy because guest addresses are host addresses in this
// emulator. The CRT tests the return value's sign, so always report success.
static KYTY_SYSV_ABI int PayloadKernelMemcpy(uint64_t src, uint64_t dst, uint64_t len) {
	if (src == 0 || dst == 0 || len == 0) {
		return 0;
	}
	static std::atomic_uint32_t window_trace {0};
	if (window_trace.fetch_add(1) < 2048) {
		LOGF("PS5 payload kernel copy: src = 0x%016" PRIx64 ", dst = 0x%016" PRIx64
		     ", len = %" PRIu64 "\n",
		     src, dst, len);
	}
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(dst)),
	            reinterpret_cast<const void*>(static_cast<uintptr_t>(src)),
	            static_cast<size_t>(len));
	return 0;
}

static void PatchPayloadKernelCrt(Program* program) {
	EXIT_IF(program == nullptr);

	const auto trampoline =
	    CreatePayloadHostTrampoline("payload_kernel_dlsym_trampoline",
	                                reinterpret_cast<uint64_t>(&PayloadKernelDynlibDlsym));

	if (auto sym = program->elf->FindStaticSymbolValue("kernel_dynlib_dlsym");
	    sym.has_value() && trampoline != 0) {
		const auto target = program->base_vaddr + sym.value();
		// movabs rax, trampoline; jmp rax
		uint8_t patch[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
		std::memcpy(patch + 2, &trampoline, sizeof(trampoline));
		if (PayloadWriteGuestCode(target, patch, sizeof(patch))) {
			LOGF("PS5 payload CRT patch: kernel_dynlib_dlsym at 0x%016" PRIx64
			     " -> HLE resolver\n",
			     target);
		}
	}

	// sprx_sym2addr resolves every module import by NID-encoding the name and
	// walking the module symtab; replacing it with the HLE resolver removes
	// the dependency on the CRT's custom NID hash entirely.
	if (auto sym = program->elf->FindStaticSymbolValue("sprx_sym2addr"); sym.has_value()) {
		const auto sprx_trampoline = CreatePayloadHostTrampoline(
		    "payload_sprx_sym2addr_trampoline",
		    reinterpret_cast<uint64_t>(&PayloadSprxSym2Addr));
		if (sprx_trampoline != 0) {
			const auto target = program->base_vaddr + sym.value();
			uint8_t patch[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
			std::memcpy(patch + 2, &sprx_trampoline, sizeof(sprx_trampoline));
			if (PayloadWriteGuestCode(target, patch, sizeof(patch))) {
				LOGF("PS5 payload CRT patch: sprx_sym2addr at 0x%016" PRIx64
				     " -> HLE resolver\n",
				     target);
			}
		}
	}

	if (auto sym = program->elf->FindStaticSymbolValue("__patch_init"); sym.has_value()) {
		const auto target = program->base_vaddr + sym.value();
		const uint8_t ret0[] = {0x31, 0xc0, 0xc3}; // xor eax,eax; ret
		if (PayloadWriteGuestCode(target, ret0, sizeof(ret0))) {
			LOGF("PS5 payload CRT patch: __patch_init at 0x%016" PRIx64 " -> return 0\n",
			     target);
		}
	}

	// __kernel_init walks the real kernel (version probe, KASLR tables, prison0,
	// dynlib data) through the exploit pipe; none of that exists in the
	// emulator, so bypass it entirely. The CRT treats the return code as fatal,
	// and every symbol it would have found is already provided by the HLE.
	if (auto sym = program->elf->FindStaticSymbolValue("__kernel_init"); sym.has_value()) {
		const auto target = program->base_vaddr + sym.value();
		const uint8_t ret0[] = {0x31, 0xc0, 0xc3}; // xor eax,eax; ret
		if (PayloadWriteGuestCode(target, ret0, sizeof(ret0))) {
			LOGF("PS5 payload CRT patch: __kernel_init at 0x%016" PRIx64 " -> return 0\n",
			     target);
		}
	}

	// kernel_copyout/kernel_copyin shuttle bytes through the exploit pipe
	// (socket pairs + rw syscalls 0x69/0x29/0x2e/3/4). None of that machinery
	// exists here; redirect both to a host memcpy trampoline so every kernel
	// metadata read/write from the crafted proc/dynlib record just works.
	if (auto sym = program->elf->FindStaticSymbolValue("kernel_copyout"); sym.has_value()) {
		const auto tramp = CreatePayloadHostTrampoline(
		    "payload_kernel_copyout", reinterpret_cast<uint64_t>(&PayloadKernelMemcpy));
		const auto target = program->base_vaddr + sym.value();
		uint8_t patch[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
		std::memcpy(patch + 2, &tramp, sizeof(tramp));
		if (PayloadWriteGuestCode(target, patch, sizeof(patch))) {
			LOGF("PS5 payload CRT patch: kernel_copyout at 0x%016" PRIx64
			     " -> host memcpy\n",
			     target);
		}
	}
	if (auto sym = program->elf->FindStaticSymbolValue("kernel_copyin"); sym.has_value()) {
		const auto tramp = CreatePayloadHostTrampoline(
		    "payload_kernel_copyin", reinterpret_cast<uint64_t>(&PayloadKernelMemcpy));
		const auto target = program->base_vaddr + sym.value();
		uint8_t patch[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
		std::memcpy(patch + 2, &tramp, sizeof(tramp));
		if (PayloadWriteGuestCode(target, patch, sizeof(patch))) {
			LOGF("PS5 payload CRT patch: kernel_copyin at 0x%016" PRIx64
			     " -> host memcpy\n",
			     target);
		}
	}

	// kernel_get_proc/kernel_dynlib_handle/kernel_dynlib_obj walk the kernel's
	// allproc list and dynlib metadata through the exploit pipe. The emulator
	// has none of that; patch kernel_get_proc to a payload_init-style stub that
	// returns the guest proc record prepared in Execute(). __kernel_init is
	// bypassed above, so these globals would otherwise stay null and the
	// sysmodule path of sprx_open (used for the game's DT_NEEDED system sprx)
	// would never find a handle.
	if (g_payload_proc_guest != 0) {
		if (auto sym = program->elf->FindStaticSymbolValue("kernel_get_proc"); sym.has_value()) {
			const auto target = program->base_vaddr + sym.value();
			// movabs rax, <proc>; ret  -> rax = guest proc record
			uint8_t patch[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xc3};
			const auto value = static_cast<uint64_t>(g_payload_proc_guest);
			std::memcpy(patch + 2, &value, sizeof(value));
			if (PayloadWriteGuestCode(target, patch, sizeof(patch))) {
				LOGF("PS5 payload CRT patch: kernel_get_proc at 0x%016" PRIx64
				     " -> guest proc 0x%016" PRIx64 "\n",
				     target, value);
			}
		}
	}
}

enum class BindType { Unknown, Local, Global, Weak };

struct RelocationInfo {
	bool        resolved   = false;
	BindType    bind       = BindType::Unknown;
	SymbolType  type       = SymbolType::Unknown;
	uint64_t    value      = 0;
	uint64_t    vaddr      = 0;
	uint64_t    base_vaddr = 0;
	std::string name;
	std::string dbg_name;
	bool        bind_self = false;
};

struct StubbedImportRecord {
	uint32_t    index       = 0;
	uint64_t    patch_vaddr = 0;
	uint64_t    thunk_vaddr = 0;
	std::string name;
	SymbolType  type = SymbolType::Unknown;
	BindType    bind = BindType::Unknown;
	std::string program;
};

// The structure will be passed via the stack
// since the size of an object is larger than 16 bytes
struct RelocateHandlerStack {
	uint64_t stack[3];
};

static std::vector<StubbedImportRecord> g_stubbed_imports;
static std::atomic_uint32_t             g_unresolved_stub_call_log_count {0};
static std::vector<uint64_t>            g_unresolved_stub_thunk_pages;
static uint64_t                         g_unresolved_stub_thunk_offset = 0;
static constexpr uint64_t               UNRESOLVED_STUB_PAGE_SIZE      = 4096;

static KYTY_SYSV_ABI uint64_t ResolveImportStubWithId(uint64_t record_id);

static bool PatchGuestMemory64(uint64_t vaddr, uint64_t value) {
	auto* ptr     = reinterpret_cast<uint64_t*>(vaddr);
	bool  changed = (*ptr != value);
	std::memcpy(ptr, &value, sizeof(value));
	return changed;
}

static uint64_t AllocateUnresolvedImportThunk(uint64_t record_id) {
	constexpr uint64_t thunk_size = 165;

	if (g_unresolved_stub_thunk_pages.empty() ||
	    g_unresolved_stub_thunk_offset + thunk_size > UNRESOLVED_STUB_PAGE_SIZE) {
		auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, UNRESOLVED_STUB_PAGE_SIZE, Common::VirtualMemory::Mode::ExecuteReadWrite,
		    "unresolved_import_thunk");
		EXIT_NOT_IMPLEMENTED(page == 0);
		g_unresolved_stub_thunk_pages.push_back(page);
		g_unresolved_stub_thunk_offset = 0;
	}

	auto* code = reinterpret_cast<uint8_t*>(g_unresolved_stub_thunk_pages.back() +
	                                        g_unresolved_stub_thunk_offset);
	g_unresolved_stub_thunk_offset += thunk_size;

	const auto target = reinterpret_cast<uint64_t>(ResolveImportStubWithId);
	uint8_t    bytes[thunk_size] {};
	size_t     i      = 0;
	const auto emit   = [&](uint8_t b) { bytes[i++] = b; };
	const auto emit64 = [&](uint64_t v) {
		std::memcpy(bytes + i, &v, sizeof(v));
		i += sizeof(v);
	};
	const auto emit32 = [&](uint32_t v) {
		std::memcpy(bytes + i, &v, sizeof(v));
		i += sizeof(v);
	};
	const auto save_xmm = [&](uint8_t reg, uint8_t offset) {
		emit(0xf3);
		emit(0x0f);
		emit(0x7f);
		if (offset == 0) {
			emit(static_cast<uint8_t>(0x04u | (reg << 3u)));
			emit(0x24);
		} else {
			emit(static_cast<uint8_t>(0x44u | (reg << 3u)));
			emit(0x24);
			emit(offset);
		}
	};
	const auto load_xmm = [&](uint8_t reg, uint8_t offset) {
		emit(0xf3);
		emit(0x0f);
		emit(0x6f);
		if (offset == 0) {
			emit(static_cast<uint8_t>(0x04u | (reg << 3u)));
			emit(0x24);
		} else {
			emit(static_cast<uint8_t>(0x44u | (reg << 3u)));
			emit(0x24);
			emit(offset);
		}
	};

	emit(0x50); // push rax; preserve AL for variadic SysV calls
	emit(0x57); // push rdi
	emit(0x56); // push rsi
	emit(0x52); // push rdx
	emit(0x51); // push rcx
	emit(0x41);
	emit(0x50); // push r8
	emit(0x41);
	emit(0x51); // push r9
	emit(0x48);
	emit(0x81);
	emit(0xec);
	emit32(0x80); // sub rsp, 0x80
	for (uint8_t reg = 0; reg < 8; reg++) {
		save_xmm(reg, static_cast<uint8_t>(reg * 0x10u));
	}
	emit(0x48);
	emit(0xbf);
	emit64(record_id); // mov rdi, record_id
	emit(0x48);
	emit(0xb8);
	emit64(target); // mov rax, ResolveImportStubWithId
	emit(0xff);
	emit(0xd0); // call rax
	emit(0x49);
	emit(0x89);
	emit(0xc3); // mov r11, rax
	for (uint8_t reg = 0; reg < 8; reg++) {
		load_xmm(reg, static_cast<uint8_t>(reg * 0x10u));
	}
	emit(0x48);
	emit(0x81);
	emit(0xc4);
	emit32(0x80); // add rsp, 0x80
	emit(0x41);
	emit(0x59); // pop r9
	emit(0x41);
	emit(0x58); // pop r8
	emit(0x59); // pop rcx
	emit(0x5a); // pop rdx
	emit(0x5e); // pop rsi
	emit(0x5f); // pop rdi
	emit(0x58); // pop rax
	emit(0x4d);
	emit(0x85);
	emit(0xdb); // test r11, r11
	emit(0x74);
	emit(0x03); // jz +3
	emit(0x41);
	emit(0xff);
	emit(0xe3); // jmp r11
	// Match the integer fallback for floating-point return values.
	emit(0x0f);
	emit(0x57);
	emit(0xc0); // xorps xmm0, xmm0
	emit(0x31);
	emit(0xc0); // xor eax, eax
	emit(0xc3); // ret

	EXIT_NOT_IMPLEMENTED(i != thunk_size);
	std::memcpy(code, bytes, sizeof(bytes));
	Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(code), thunk_size);
	return reinterpret_cast<uint64_t>(code);
}

static uint64_t RegisterStubbedImport(uint32_t index, const Program* program,
                                      const RelocationInfo& ri) {
	const auto program_name = program != nullptr ? Common::PathToString(program->file_name) : "";

	for (auto& record: g_stubbed_imports) {
		if (record.patch_vaddr == ri.vaddr) {
			record.index   = index;
			record.name    = ri.name;
			record.type    = ri.type;
			record.bind    = ri.bind;
			record.program = program_name;
			return record.thunk_vaddr;
		}
	}

	StubbedImportRecord record {};
	record.index       = index;
	record.patch_vaddr = ri.vaddr;
	record.name        = ri.name;
	record.type        = ri.type;
	record.bind        = ri.bind;
	record.program     = program_name;
	g_stubbed_imports.push_back(record);
	const auto record_id                     = g_stubbed_imports.size() - 1;
	const auto thunk                         = AllocateUnresolvedImportThunk(record_id);
	g_stubbed_imports[record_id].thunk_vaddr = thunk;
	return thunk;
}

static KYTY_SYSV_ABI uint64_t ResolveImportStubWithId(uint64_t record_id) {
	if (record_id < g_stubbed_imports.size()) {
		auto& record = g_stubbed_imports[record_id];
		auto  nid    = record.name;
		auto  pos    = Common::FindIndex(nid, "[");
		if (Common::IndexValid(nid, pos)) {
			nid = Common::Left(nid, pos);
		}

		SymbolRecord resolved {};
		if (!nid.empty() &&
		    Common::Singleton<RuntimeLinker>::Instance()->ResolveLoadedSymbolByNid(nid, record.type,
		                                                                           &resolved) &&
		    resolved.vaddr != 0 && resolved.vaddr != record.thunk_vaddr) {
			LOGF("Late-resolved import: %s -> %s [0x%016" PRIx64 "]\n", record.name.c_str(),
			     resolved.name.c_str(), resolved.vaddr);

			if (record.patch_vaddr != 0) {
				PatchGuestMemory64(record.patch_vaddr, resolved.vaddr);
			}

			return resolved.vaddr;
		}
	}

	const auto log_index = g_unresolved_stub_call_log_count.fetch_add(1);
	if (log_index < 1024) {
		if (record_id < g_stubbed_imports.size()) {
			const auto& record = g_stubbed_imports[record_id];
			printf("Unresolved import stub called: %s\n", record.name.c_str());
			LOGF("Unresolved import stub called [%u]: patch_vaddr=0x%016" PRIx64
			     " jmprela_index=%" PRIu32 " symbol=%s type=%s bind=%s program=%s\n",
			     log_index, record.patch_vaddr, record.index, record.name.c_str(),
			     Common::EnumName(record.type).c_str(), Common::EnumName(record.bind).c_str(),
			     record.program.c_str());
		} else {
			printf("Unresolved import stub called: <bad-record>\n");
			LOGF("Unresolved import stub called [%u]: record_id=%" PRIu64 " symbol=<bad-record>\n",
			     log_index, record_id);
		}
	}
	return 0;
}

constexpr uint64_t SYSTEM_RESERVED  = 0x800000000u;
constexpr uint64_t CODE_BASE_INCR   = 0x010000000u;
constexpr uint64_t INVALID_OFFSET   = 0x040000000u;
constexpr uint64_t CODE_BASE_OFFSET = 0x100000000u;
constexpr uint64_t INVALID_MEMORY   = SYSTEM_RESERVED + INVALID_OFFSET;

static uint64_t g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;
static uint64_t g_invalid_memory    = 0;

static Program*              g_tls_main_program        = nullptr;
static thread_local Program* g_tls_cached_main_program = nullptr;
static thread_local uint8_t* g_tls_cached_main_tcb     = nullptr;

static KYTY_SYSV_ABI void RunEntry(uint64_t addr, EntryParams* params, atexit_func_t atexit_func,
                                   void* stack_top) {
#if defined(__x86_64__) || defined(_M_X64)
	auto* func = reinterpret_cast<entry_func_t>(addr);

	if (stack_top != nullptr) {
		const auto aligned_stack_top =
		    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
		const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
		const auto guest_rbp = guest_rsp;

		auto* guest_root_frame = reinterpret_cast<uintptr_t*>(guest_rbp);
		guest_root_frame[0]    = 0;
		guest_root_frame[1]    = 0;

#if defined(__APPLE__)
		// Clang on macOS can allocate plain "r" inputs to r12/r13, which the template
		// clobbers before consuming them. Pin the inputs to registers the SysV guest
		// preserves without changing register allocation on Windows or Linux.
		register entry_func_t func_reg asm("rbx")      = func;
		register uintptr_t    guest_rsp_reg asm("r14") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r15") = guest_rbp;
#endif

#if defined(__APPLE__)
		asm volatile(
		    "pushq %%r12\n\t"
		    "pushq %%r13\n\t"
		    "movq %%rsp, %%r12\n\t"
		    "movq %%rbp, %%r13\n\t"
		    "movq %[guest_rsp], %%rsp\n\t"
		    "movq %[guest_rbp], %%rbp\n\t"
		    "callq *%[func]\n\t"
		    "movq %%r13, %%rbp\n\t"
		    "movq %%r12, %%rsp\n\t"
		    "popq %%r13\n\t"
		    "popq %%r12\n\t"
		    :
		    : [func] "r"(func_reg), "D"(params),
		      "S"(atexit_func), [guest_rsp] "r"(guest_rsp_reg), [guest_rbp] "r"(guest_rbp_reg)
		    : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
		      "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
		      "xmm13", "xmm14", "xmm15");
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Windows stack probes use the TEB stack limits during the guest stack switch.
		// bounds, which describe the host stack and are invalid while RSP is in guest memory.
		register entry_func_t func_reg asm("rbx")     = func;
		register uintptr_t    guest_rsp_reg asm("r8") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r9") = guest_rbp;
		asm volatile("pushq %%r12\n\t"
		             "pushq %%r13\n\t"
		             "pushq %%r14\n\t"
		             "pushq %%r15\n\t"
		             "movq %%gs:0x08, %%r14\n\t"
		             "movq %%gs:0x10, %%r15\n\t"
		             "xorq %%rcx, %%rcx\n\t"
		             "movq %%rcx, %%gs:0x08\n\t"
		             "movq %%rcx, %%gs:0x10\n\t"
		             "movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             "movq %%r14, %%gs:0x08\n\t"
		             "movq %%r15, %%gs:0x10\n\t"
		             "popq %%r15\n\t"
		             "popq %%r14\n\t"
		             "popq %%r13\n\t"
		             "popq %%r12\n\t"
		             : [guest_rsp] "+r"(guest_rsp_reg), [guest_rbp] "+r"(guest_rbp_reg)
		             : [func] "r"(func_reg), "D"(params), "S"(atexit_func)
		             : "cc", "memory", "rax", "rcx", "rdx", "r10", "r11", "xmm0", "xmm1", "xmm2",
		               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		               "xmm12", "xmm13", "xmm14", "xmm15");
#else
		// Clobbers prevent inputs from being allocated to r12/r13.
		asm volatile("movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             :
		             : [func] "r"(func), "D"(params),
		               "S"(atexit_func), [guest_rsp] "r"(guest_rsp), [guest_rbp] "r"(guest_rbp)
		             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13",
		               "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
		               "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
		return;
	}

	uintptr_t guest_root_frame[2] = {};

#if defined(__APPLE__)
	register entry_func_t func_reg asm("rbx")      = func;
	register uintptr_t    guest_rbp_reg asm("r14") = reinterpret_cast<uintptr_t>(guest_root_frame);
#endif

#if defined(__APPLE__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
	             :
#if defined(__APPLE__)
	             : [func] "r"(func_reg), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_rbp_reg)
#else
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
#endif
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1",
	               "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
	               "xmm12", "xmm13", "xmm14", "xmm15");
#else
	// Keep inputs out of r12.
	asm volatile("movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             :
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "xmm0",
	               "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
	               "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
#else
	(void)stack_top;
	reinterpret_cast<entry_func_t>(addr)(params, atexit_func);
#endif
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
struct MainEntryStackTestState {
	bool      called = false;
	uintptr_t rsp    = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t teb_stack_base  = UINTPTR_MAX;
	uintptr_t teb_stack_limit = UINTPTR_MAX;
#endif
};

static KYTY_SYSV_ABI void TestMainEntryStackCallback(EntryParams* params,
                                                     atexit_func_t /*atexit_func*/) {
	auto* state = reinterpret_cast<MainEntryStackTestState*>(const_cast<char*>(params->argv[0]));
	asm volatile("pushq %%r15\n\t"
	             "pushq %%r14\n\t"
	             "popq %%r14\n\t"
	             "popq %%r15\n\t"
	             :
	             :
	             : "memory");
	asm volatile("movq %%rsp, %0" : "=r"(state->rsp) : : "memory");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(state->teb_stack_base), "=r"(state->teb_stack_limit)
	             :
	             : "memory");
#endif
	state->called = true;
}

bool TestMainEntryUsesGuestStack() {
	constexpr uint64_t stack_size = 0x10000;
	const auto         stack_base = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, stack_size, Common::VirtualMemory::Mode::ReadWrite, "main_entry_stack_test");
	if (stack_base == 0) {
		return false;
	}

	MainEntryStackTestState state {};
	EntryParams             params {};
	params.argv[0] = reinterpret_cast<const char*>(&state);
	std::memset(reinterpret_cast<void*>(stack_base), 0xcd, stack_size);
	auto* root_frame = reinterpret_cast<const uintptr_t*>(stack_base + stack_size) - 2;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t original_teb_stack_base  = 0;
	uintptr_t original_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(original_teb_stack_base), "=r"(original_teb_stack_limit)
	             :
	             : "memory");
#endif

	RunEntry(reinterpret_cast<uint64_t>(TestMainEntryStackCallback), &params, nullptr,
	         reinterpret_cast<void*>(stack_base + stack_size));

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t restored_teb_stack_base  = 0;
	uintptr_t restored_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(restored_teb_stack_base), "=r"(restored_teb_stack_limit)
	             :
	             : "memory");
	const bool teb_ok = state.teb_stack_base == 0 && state.teb_stack_limit == 0 &&
	                    restored_teb_stack_base == original_teb_stack_base &&
	                    restored_teb_stack_limit == original_teb_stack_limit;
#else
	constexpr bool teb_ok = true;
#endif

	const bool rsp_ok  = state.rsp >= stack_base && state.rsp < stack_base + stack_size;
	const bool root_ok = root_frame[0] == 0 && root_frame[1] == 0;
	const bool freed   = Libs::LibKernel::Memory::FreeGuestMemory(stack_base, stack_size);
	return state.called && rsp_ok && root_ok && teb_ok && freed;
}

bool TestModuleRelocationUsesWritableHostMapping() {
	constexpr uint64_t page_size = 0x4000;
	constexpr uint64_t value     = 0x4b59545950415443;
	const auto         base      = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0, page_size, Common::VirtualMemory::Mode::ReadWrite, "host_only_patch_test");
	if (base == 0) {
		return false;
	}
	Libs::LibKernel::Memory::SetProgramMemoryProtection(base, page_size,
	                                                    Common::VirtualMemory::Mode::Read);

	Libs::LibKernel::Memory::VirtualQueryInfo before {};
	Libs::LibKernel::Memory::VirtualQueryInfo after {};
	const bool                                before_ok =
	    Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(base), 0, &before,
	                                                sizeof(before)) == 0;
	const bool changed  = PatchGuestMemory64(base, value);
	const bool after_ok = Libs::LibKernel::Memory::KernelVirtualQuery(
	                          reinterpret_cast<const void*>(base), 0, &after, sizeof(after)) == 0;
	const bool value_ok = *reinterpret_cast<const uint64_t*>(base) == value;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION mbi {};
	const bool               host_mode_ok =
	    VirtualQuery(reinterpret_cast<const void*>(base), &mbi, sizeof(mbi)) != 0 &&
	    mbi.Protect == PAGE_READWRITE;
#else
	constexpr bool host_mode_ok = true;
#endif
	const bool freed = Libs::LibKernel::Memory::FreeGuestMemory(base, page_size);

	return before_ok && after_ok && changed && value_ok && host_mode_ok && freed &&
	       before.protection == after.protection;
}
#endif

static uint64_t GetAlignedSize(const Elf64_Phdr* p) {
	return (p->p_align != 0 ? (p->p_memsz + (p->p_align - 1)) & ~(p->p_align - 1) : p->p_memsz);
}

static void DbgDumpSymbols(const std::string& folder, Elf64_Sym* symbols, uint64_t size,
                           const char* names) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + "symbols.txt");

	for (auto* sym = symbols;
	     reinterpret_cast<uint8_t*>(sym) < reinterpret_cast<uint8_t*>(symbols) + size; sym++) {
		f.Printf("----\n");
		f.Printf("st_name = %" PRIu32 ", %s\n", sym->st_name, names + sym->st_name);
		f.Printf("st_info = 0x%02" PRIx8 "\n", sym->st_info);
		f.Printf("st_other = 0x%02" PRIx8 "\n", sym->st_other);
		f.Printf("st_shndx = 0x%04" PRIx16 "\n", sym->st_shndx);
		f.Printf("st_value = 0x%016" PRIx64 "\n", sym->st_value);
		f.Printf("st_size = %" PRIu64 "\n", sym->st_size);
	}

	f.Close();
}

static void DbgDumpRela(const std::string& folder, Elf64_Rela* records, uint64_t size,
                        const char* /*names*/, const char* file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	for (auto* r = records;
	     reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++) {
		f.Printf("----\n"
		         "r_offset = 0x%016" PRIx64 "\n"
		         "r_info = 0x%016" PRIx64 "\n"
		         "r_addend = %" PRId64 "\n",
		         r->r_offset, r->r_info, r->r_addend);
	}

	f.Close();
}

static Common::VirtualMemory::Mode GetMode(Elf64_Word flags) {
	switch (flags) {
		case PF_R: return Common::VirtualMemory::Mode::Read;
		case PF_W: return Common::VirtualMemory::Mode::Write;
		case PF_R | PF_W: return Common::VirtualMemory::Mode::ReadWrite;
		case PF_X: return Common::VirtualMemory::Mode::Execute;
		case PF_X | PF_R: return Common::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_W: return Common::VirtualMemory::Mode::ExecuteWrite;
		case PF_X | PF_W | PF_R: return Common::VirtualMemory::Mode::ExecuteReadWrite;

		default: return Common::VirtualMemory::Mode::NoAccess;
	}
}

struct GuestStackFrame {
	GuestStackFrame* next;
	uintptr_t        return_address;
};

static bool IsReadableRange(uint64_t addr, uint64_t size);

static int WalkGuestStack(uint64_t rbp, uint64_t rsp, void** stack, int capacity) {
	constexpr uintptr_t STACK_SIZE = 1024u * 1024u;
	const uintptr_t     code_start = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	const uintptr_t     stack_end  = rsp + STACK_SIZE;
	if (stack == nullptr || capacity <= 0 || rsp == 0 || rbp < rsp || stack_end < rsp ||
	    g_desired_base_addr <= code_start) {
		return 0;
	}

	auto* frame = reinterpret_cast<GuestStackFrame*>(rbp);

	int depth = 0;
	while (depth < capacity) {
		const auto frame_addr = reinterpret_cast<uintptr_t>(frame);
		if (frame_addr < rsp || frame_addr > stack_end - sizeof(GuestStackFrame) ||
		    !IsReadableRange(frame_addr, sizeof(GuestStackFrame)) ||
		    frame->return_address < code_start || frame->return_address >= g_desired_base_addr) {
			break;
		}
		stack[depth++] = reinterpret_cast<void*>(frame->return_address);
		if (reinterpret_cast<uintptr_t>(frame->next) <= frame_addr) {
			break;
		}
		frame = frame->next;
	}
	return depth;
}

// Probe diagnostic ranges without raising another fault.
static bool IsReadableRange(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0) {
		return false;
	}

	const uint64_t end = addr + size;
	if (end < addr) {
		return false;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uint64_t current = addr;
	while (current < end) {
		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) == 0 ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}
		const auto region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (region_end <= current) {
			return false;
		}
		current = std::min(region_end, end);
	}
#elif defined(__APPLE__)
	// Walk the Mach regions covering the range and require read permission. The fatal
	// report dumps memory behind raw register values, and a fault inside the reporter
	// re-enters the signal handler and wedges the reporting thread.
	uint64_t current = addr;
	while (current < end) {
		mach_vm_address_t              region_addr = current;
		mach_vm_size_t                 region_size = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t                    object_name = MACH_PORT_NULL;
		if (mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
		                   reinterpret_cast<vm_region_info_t>(&info), &count,
		                   &object_name) != KERN_SUCCESS ||
		    region_addr > current || (info.protection & VM_PROT_READ) == 0) {
			return false;
		}
		current = region_addr + region_size;
	}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	const auto page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
	if (page_size == 0) {
		return false;
	}

	for (uint64_t current = addr; current < end;) {
		uint8_t probe = 0;

		iovec local {&probe, sizeof(probe)};
		iovec remote {reinterpret_cast<void*>(current), sizeof(probe)};

		if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
		    static_cast<ssize_t>(sizeof(probe))) {
			return false;
		}

		const uint64_t next = (current & ~(page_size - 1)) + page_size;
		if (next <= current) { // wrapped at the top of the address space
			break;
		}
		current = next;
	}
#else
	(void)end;
#endif
	return true;
}

static bool KytyExceptionHandler(const Common::HostException::ExceptionInfo& exception_info) {
	const auto* info = &exception_info;

	if (info->type == Common::HostException::ExceptionType::IllegalInstruction &&
	    Loader::X64InstructionEmulator::TryEmulate(info->native_context)) {
		return true;
	}

	if (info->type == Common::HostException::ExceptionType::AccessViolation) {
		using CoreAccess = Common::HostException::AccessViolationType;
		using GpuAccess  = Libs::Graphics::PageFaultAccess;
		GpuAccess access;
		switch (info->access_violation_type) {
			case CoreAccess::Read: access = GpuAccess::Read; break;
			case CoreAccess::Write: access = GpuAccess::Write; break;
			case CoreAccess::Execute: access = GpuAccess::Execute; break;
			case CoreAccess::Unknown: return false;
		}
		if (Libs::LibKernel::Memory::HandleGpuFault(access, info->access_violation_vaddr)) {
			return true;
		}
	}
	EXIT("Unhandled host exception: type=%u code=%u pc=0x%016" PRIx64
	     " access=%u address=0x%016" PRIx64 "\n"
	     " regs: rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64
	     " rsi=%016" PRIx64 " rdi=%016" PRIx64 " rbp=%016" PRIx64 " rsp=%016" PRIx64
	     " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64
	     " r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64 "\n",
	     static_cast<unsigned>(info->type), info->native_code, info->exception_address,
	     static_cast<unsigned>(info->access_violation_type), info->access_violation_vaddr, info->rax,
	     info->rbx, info->rcx, info->rdx, info->rsi, info->rdi, info->rbp, info->rsp, info->r8,
	     info->r9, info->r10, info->r11, info->r12, info->r13, info->r14, info->r15);
}

static void EncodeId64(uint16_t in_id, std::string* out_id) {
	static const char* str = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (in_id < 0x40u) {
		*out_id += str[in_id];
	} else {
		if (in_id < 0x1000u) {
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		} else {
			*out_id += str[static_cast<uint16_t>(in_id >> 12u) & 0x3fu];
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		}
	}
}

template <class T>
static void GetDynDataOs(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = elf->GetDynamicData<T>(dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynData(Elf64* elf, uint64_t base_vaddr, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = reinterpret_cast<T>(base_vaddr + dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynValue(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_val;
	}
}

template <class T>
static void GetDynValues(Elf64* elf, T* out, Elf64_Sxword tag) {
	for (const auto* dyn: elf->GetDynList(tag)) {
		out->push_back(dyn->d_un.d_val);
	}
}

template <class T>
static void GetDynPtr(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_ptr;
	}
}

static void KYTY_SYSV_ABI ProgramExitHandler() {
	Common::Singleton<RuntimeLinker>::Instance()->StopAllModules();

	LOGF("exit!!!\n");
}

template <class T>
static void GetDynModules(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		ModuleId id {};
		// id.id            = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version_major = static_cast<int>((need >> 40u) & 0xffu);
		id.version_minor = static_cast<int>((need >> 32u) & 0xffu);
		id.name          = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

template <class T>
static void GetDynLibs(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		LibraryId id {};
		// id.id      = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version = static_cast<int>((need >> 32u) & 0xffffu);
		id.name    = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

static RelocationInfo GetRelocationInfo(Elf64_Rela* r, Program* program) {
	KYTY_PROFILER_FUNCTION();

	// KYTY_PROFILER_BLOCK("1");

	RelocationInfo ret;
	// SymbolRecord   sr {};

	// KYTY_PROFILER_END_BLOCK;

	// KYTY_PROFILER_BLOCK("2");

	auto         type    = r->GetType();
	auto         symbol  = r->GetSymbol();
	Elf64_Sxword addend  = r->r_addend;
	auto*        symbols = program->dynamic_info->symbol_table;
	auto*        names   = program->dynamic_info->str_table;
	ret.base_vaddr       = program->base_vaddr;
	ret.vaddr            = ret.base_vaddr + r->r_offset;
	ret.bind_self        = false;

	// KYTY_PROFILER_END_BLOCK;

	// KYTY_PROFILER_BLOCK("3");

	switch (type) {
		case R_X86_64_GLOB_DAT:
		case R_X86_64_JUMP_SLOT: addend = 0; [[fallthrough]];
		case R_X86_64_64: {
			auto         sym          = symbols[symbol];
			auto         bind         = sym.GetBind();
			auto         sym_type     = sym.GetType();
			uint64_t     symbol_vaddr = 0;
			SymbolRecord sr {};
			switch (sym_type) {
				case STT_NOTYPE: ret.type = SymbolType::NoType; break;
				case STT_FUNC: ret.type = SymbolType::Func; break;
				case STT_OBJECT: ret.type = SymbolType::Object; break;
				default: EXIT("unknown symbol type: %d\n", (int)sym_type);
			}
			switch (bind) {
				case STB_LOCAL:
					symbol_vaddr = ret.base_vaddr + sym.st_value;
					ret.bind     = BindType::Local;
					break;
				case STB_GLOBAL: ret.bind = BindType::Global; [[fallthrough]];
				case STB_WEAK: {
					ret.bind = (ret.bind == BindType::Unknown ? BindType::Weak : ret.bind);
					ret.name = names + sym.st_name;
					program->rt->Resolve(ret.name, ret.type, program, &sr, &ret.bind_self);
					symbol_vaddr = sr.vaddr;
				} break;
				default: EXIT("unknown bind: %d\n", (int)bind);
			}
			ret.resolved = (symbol_vaddr != 0);
			ret.value    = (ret.resolved ? symbol_vaddr + addend : 0);
			ret.name     = sr.name;
			ret.dbg_name = sr.dbg_name;
		} break;
		case R_X86_64_RELATIVE:
			ret.value    = ret.base_vaddr + addend;
			ret.resolved = true;
			break;
		case R_X86_64_DTPMOD64:
			ret.value    = reinterpret_cast<uint64_t>(program);
			ret.resolved = true;
			ret.type     = SymbolType::TlsModule;
			ret.bind     = BindType::Local;
			ret.dbg_name = Common::PathToString(program->file_name);
			break;
		default: EXIT("unknown type: %d\n", (int)type);
	}

	// KYTY_PROFILER_END_BLOCK;

	return ret;
}

static void RelocateRecord(uint32_t index, Elf64_Rela* r, Program* program, bool jmprela_table,
                           bool imports_only, std::vector<std::string>* unresolved) {
	KYTY_PROFILER_FUNCTION();

	auto ri = GetRelocationInfo(r, program);

	if (imports_only &&
	    (ri.bind_self || (ri.bind != BindType::Global && ri.bind != BindType::Weak))) {
		return;
	}

	[[maybe_unused]] bool patched        = false;
	bool                  stubbed_import = false;
	bool                  stubbed_func   = false;

	// KYTY_PROFILER_BLOCK("patch");

	if (ri.resolved) {
		patched = PatchGuestMemory64(ri.vaddr, ri.value);
	} else {
		uint64_t value = 0;
		bool     weak  = (ri.bind == BindType::Weak || !program->fail_if_global_not_resolved);
		if (ri.type == SymbolType::Object && weak) {
			value = g_invalid_memory;
		} else if (ri.type == SymbolType::Func && jmprela_table && weak) {
			value          = RegisterStubbedImport(index, program, ri);
			stubbed_import = true;
			stubbed_func   = true;
		} else if (ri.type == SymbolType::Func && !jmprela_table && weak) {
			value        = RegisterStubbedImport(index, program, ri);
			stubbed_func = true;
		} else if (ri.type == SymbolType::NoType && weak) {
			value = RuntimeLinker::ReadFromElf(program, ri.vaddr) + ri.base_vaddr;
		}

		if (value != 0) {
			patched = PatchGuestMemory64(ri.vaddr, value);
		} else {
			auto dbg_str = fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, ri.value,
			                           ri.name.c_str(), Common::EnumName(ri.type).c_str(),
			                           Common::EnumName(ri.bind).c_str(), ri.dbg_name.c_str());

			if (unresolved != nullptr) {
				unresolved->push_back(dbg_str);
			} else {
				EXIT("Can't resolve: %s\n", dbg_str.c_str());
			}

			if (ri.type == SymbolType::Object) {
				value = g_invalid_memory;
			} else if (ri.type == SymbolType::Func || ri.type == SymbolType::NoType) {
				value        = RegisterStubbedImport(index, program, ri);
				stubbed_func = true;
				if (jmprela_table) {
					stubbed_import = true;
				}
			}

			if (value != 0) {
				patched = PatchGuestMemory64(ri.vaddr, value);
			}
		}
	}

	// KYTY_PROFILER_END_BLOCK;

	if (patched && stubbed_import) {
		const auto thunk = RegisterStubbedImport(index, program, ri);
		LOGF("Relocate: unresolved PLT import patched to stub [%u] [%016" PRIx64 "] <- %016" PRIx64
		     ", %s, %s, %s, %s\n",
		     index, ri.vaddr, thunk, ri.name.c_str(), Common::EnumName(ri.type).c_str(),
		     Common::EnumName(ri.bind).c_str(), Common::PathToString(program->file_name).c_str());
	} else if (patched && stubbed_func) {
		const auto thunk = RegisterStubbedImport(index, program, ri);
		LOGF("Relocate: unresolved non-PLT function patched to stub [%u] [%016" PRIx64
		     "] <- %016" PRIx64 ", %s, %s, %s, %s\n",
		     index, ri.vaddr, thunk, ri.name.c_str(), Common::EnumName(ri.type).c_str(),
		     Common::EnumName(ri.bind).c_str(), Common::PathToString(program->file_name).c_str());
	}

	if (program->dbg_print_reloc) {
		if (/* !dbg_str.ContainsStr("libc_") && */ patched && !ri.bind_self &&
		    (ri.bind == BindType::Global || ri.bind == BindType::Weak ||
		     ri.type == SymbolType::TlsModule)) {
			auto dbg_str = fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, ri.value,
			                           ri.name.c_str(), Common::EnumName(ri.type).c_str(),
			                           Common::EnumName(ri.bind).c_str(), ri.dbg_name.c_str());

			LOGF("Relocate: %s\n", dbg_str.c_str());
		}
	}
}

static void RelocateRecords(Elf64_Rela* records, uint64_t size, Program* program,
                            bool jmprela_table, bool imports_only,
                            std::vector<std::string>* unresolved) {
	KYTY_PROFILER_FUNCTION();

	uint32_t index = 0;
	for (auto* r = records;
	     reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++, index++) {
		RelocateRecord(index, r, program, jmprela_table, imports_only, unresolved);
	}
}

__attribute__((naked)) static KYTY_SYSV_ABI void RelocateHandlerReturnStub() {
	asm volatile("addq $8, %rsp\n\t"
	             "retq\n");
}

static KYTY_SYSV_ABI uint64_t RelocateHandler(RelocateHandlerStack s) {
	auto*       stack     = s.stack;
	auto*       program   = reinterpret_cast<Program*>(stack[-1]);
	auto        rel_index = stack[0];
	std::string name      = "<unknown function>";

	if (program != nullptr && program->dynamic_info != nullptr &&
	    program->dynamic_info->jmprela_table != nullptr) {
		auto ri = GetRelocationInfo(program->dynamic_info->jmprela_table + rel_index, program);

		name = ri.name.c_str();
	}

	// Restore return address (for stack trace)
	stack[-1] = reinterpret_cast<uint64_t>(RelocateHandlerReturnStub);

	LOGF("=== Stubbed function, returning OK ===\n[%d]\t%s\n", Common::Thread::GetThreadIdUnique(),
	     name.c_str());
	return 0;
}

static KYTY_MS_ABI uint8_t* TlsMainGetAddr() {
	EXIT_IF(g_tls_main_program == nullptr);

	if (g_tls_cached_main_program == g_tls_main_program && g_tls_cached_main_tcb != nullptr) {
		return g_tls_cached_main_tcb;
	}

	g_tls_cached_main_program = g_tls_main_program;
	g_tls_cached_main_tcb =
	    RuntimeLinker::TlsGetAddr(g_tls_main_program) + g_tls_main_program->tls.tcb_offset;
	return g_tls_cached_main_tcb;
}

static void PatchProgram(Program* program, uint64_t address, uint64_t size) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);

	if (size >= 12) {
		// Replace guest stack-canary/errno stores through fs:[0x28] with nops.
		// Windows x64 cannot host guest FS directly, and an unpatched shared-library access faults
		// at address 0x28.
		const uint8_t fs_store_pattern[8] = {0x64, 0xc7, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00};
		auto*         start_ptr           = reinterpret_cast<uint8_t*>(address);
		auto*         end_ptr             = start_ptr + size - 12;

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			if (memcmp(ptr, fs_store_pattern, sizeof(fs_store_pattern)) == 0) {
				LOGF("Patch fs:[0x28] store at addr: [%016" PRIx64 "]\n",
				     reinterpret_cast<uint64_t>(ptr));
				if (ptr + 16 < start_ptr + size && ptr[12] == 0xcd && ptr[13] == 0x45 &&
				    ptr[14] == 0x90 && ptr[15] == 0x0f && ptr[16] == 0x0b) {
					ptr[0] = 0x5d; // pop rbp
					ptr[1] = 0xc3; // ret
					std::memset(ptr + 2, 0x90, 15);
				} else {
					std::memset(ptr, 0x90, 12);
				}
			}
		}
	}

	if (!program->elf->IsShared() && program->tls.handler_vaddr != 0) {
		// Replace:
		//   66 66 66
		//   mov <reg>, qword ptr fs:[0x00]
		// with:
		//   call <handler>
		//   mov <reg>,rax
		//   nop ...
		const uint8_t tls_pattern[5] = {0x64, 0x48, 0x8B, 0x00, 0x25};

		EXIT_IF(Jit::Call9::GetSize() != 9);

		auto* start_ptr = reinterpret_cast<uint8_t*>(address);
		auto* end_ptr   = start_ptr + size - Jit::Call9::GetSize();

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			auto*  inst_ptr     = ptr;
			size_t prefix_count = 0;
			while (prefix_count < 3 && inst_ptr < start_ptr + size && *inst_ptr == 0x66) {
				inst_ptr++;
				prefix_count++;
			}

			if (inst_ptr + Jit::Call9::GetSize() > start_ptr + size) {
				break;
			}

			const uint8_t modrm = inst_ptr[3];
			if (memcmp(inst_ptr, tls_pattern, 3) == 0 && (modrm & 0xc7u) == 0x04u &&
			    inst_ptr[4] == tls_pattern[4] &&
			    *reinterpret_cast<const uint32_t*>(inst_ptr + 5) == 0) {
				LOGF("Patch tls at addr: [%016" PRIx64 "]\n",
				     reinterpret_cast<uint64_t>(inst_ptr));

				const auto reg = (modrm >> 3u) & 7u;
				EXIT_NOT_IMPLEMENTED(reg == 4u);

				// A raw scan can encounter a 0x66 in the preceding instruction.
				auto* code = new (inst_ptr) Jit::Call9;
				code->SetFunc(reg == 0
				                  ? program->tls.handler_vaddr
				                  : program->tls.handler_vaddr + Jit::TlsRegStub::GetOffset(reg));
				ptr += prefix_count + Jit::Call9::GetSize() - 1;
			}
		}
	}
}

uint64_t RuntimeLinker::GetEntry() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->elf->GetEntry() + p->base_vaddr;
		}
	}
	return 0;
}

uint64_t RuntimeLinker::GetProcParam() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->proc_param_vaddr;
		}
	}
	return 0;
}

void RuntimeLinker::DbgDump(const std::string& folder) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		auto folder_str = Common::FixDirectorySlash(folder);
		folder_str += Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name));

		EXIT_IF(p->elf == nullptr);

		p->elf->DbgDump(folder_str);

		if (p->dynamic_info != nullptr) {
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table_entry_size != 0 &&
			                     p->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table_entry_size != 0 &&
			                     p->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->jmprela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table == nullptr);

			if (p->dynamic_info->symbol_table != nullptr) {
				DbgDumpSymbols(folder_str, p->dynamic_info->symbol_table,
				               p->dynamic_info->symbol_table_total_size,
				               p->dynamic_info->str_table);
			}
			if (p->dynamic_info->jmprela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->jmprela_table,
				            p->dynamic_info->jmprela_table_size, p->dynamic_info->str_table,
				            "jmprela_table.txt");
			}
			if (p->dynamic_info->rela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->rela_table,
				            p->dynamic_info->rela_table_total_size, p->dynamic_info->str_table,
				            "rela_table.txt");
			}
		}

		if (p->export_symbols != nullptr) {
			p->export_symbols->DbgDump(folder_str, "export_symbols.txt");
		}
		if (p->import_symbols != nullptr) {
			p->import_symbols->DbgDump(folder_str, "import_symbols.txt");
		}
	}
}

void RuntimeLinker::RelocateAll() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		Relocate(p);
	}

	m_relocated = true;
}

void RuntimeLinker::RelocateProgram(Program* program) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(program == nullptr);
	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	Relocate(program);
	if (!GamePatch::ApplyPending(program)) {
		EXIT("Failed to apply pending game cheat\n");
	}
}

void RuntimeLinker::UnloadProgram(Program* program) {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	if (auto it = std::find(m_programs.begin(), m_programs.end(), program);
	    it != m_programs.end()) {
		DeleteProgram(*it);
		m_programs.erase(it);
	} else {
		EXIT("program not found");
	}

	if (m_relocated) {
		RelocateAll();
	}
}

RuntimeLinker::RuntimeLinker(): m_symbols(std::make_unique<SymbolDatabase>()) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RuntimeLinker::~RuntimeLinker() {
	Clear();
}

Program* RuntimeLinker::LoadProgram(const std::filesystem::path& elf_name) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	static int32_t id_seq = 0;

	LOGF("Loading: %s\n", Common::PathToString(elf_name).c_str());

	auto  program_owner = std::make_unique<Program>();
	auto* program       = program_owner.get();

	program->rt        = this;
	program->file_name = elf_name;
	program->unique_id = ++id_seq;

	program->elf = std::make_unique<Elf64>();
	program->elf->Open(elf_name);

	if (program->elf->IsValid()) {
		LoadProgramToMemory(program);
		ParseProgramDynamicInfo(program);
		CreateSymbolDatabase(program);
	} else {
		EXIT("elf is not valid: %s\n", Common::PathToString(elf_name).c_str());
	}

	m_programs.push_back(program_owner.release());

	if (!program->elf->IsShared()) {
		program->fail_if_global_not_resolved = false;
		Libs::LibKernel::SetProgName(elf_name.filename().string());
	}

	if (Common::EndsWith(Common::ToLower(Common::DirectoryWithoutFilename(
	                         Common::PathToGenericString(elf_name))),
	                     "_module/")) {
		program->fail_if_global_not_resolved = false;
	}

	return program;
}

void RuntimeLinker::SaveMainProgram(const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		EXIT_IF(p->elf == nullptr);

		if (!p->elf->IsShared()) {
			p->elf->Save(elf_name);
			break;
		}
	}
}

void RuntimeLinker::SaveProgram(Program* program, const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	if (auto it = std::find(m_programs.begin(), m_programs.end(), program);
	    it != m_programs.end()) {
		EXIT_IF((*it)->elf == nullptr);

		(*it)->elf->Save(elf_name);
	} else {
		EXIT("program not found");
	}
}

void RuntimeLinker::Execute(const std::filesystem::path& game_patch,
                            const std::vector<std::string>& guest_args) {
	KYTY_PROFILER_THREAD("Thread_Main");

	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* main_stack_top = Libs::LibKernel::PthreadCreateMainGuestStack();

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Guest code has no Windows stack probes and may jump over the guard page. Module
	// initializers execute on the host stack too, so grow it before calling any guest code.
	size_t expanded_size = 0;
	while (expanded_size < static_cast<size_t>(768) * 1024) {
		sys_dbg_stack_info_t stack {};
		SysStackUsage(stack);
		*reinterpret_cast<uint32_t*>(stack.guard_addr) = 0;
		expanded_size += stack.guard_size;
	}
#endif

	PreloadAdjacentPrograms();
	RelocateAll();

	if (!game_patch.empty()) {
		if (!GamePatch::Apply(game_patch, m_programs.empty() ? nullptr : m_programs.front(),
		                      m_programs)) {
			EXIT("Failed to apply game cheat\n");
		}
	}
	StartAllModules();

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Execute: %s\n---\n", "Main");

	if (auto entry = GetEntry(); entry != 0) {
		const bool is_payload = !m_programs.empty() && m_programs.front()->elf != nullptr &&
		                        m_programs.front()->elf->GetEhdr()->e_type == ET_SYSV_DYN;
		if (is_payload) {
			g_payload_symbols      = m_symbols.get();
			g_payload_dlsym_bridge = CreatePayloadDlsymBridge();
			EXIT_NOT_IMPLEMENTED(g_payload_dlsym_bridge == 0);
			// The payload CRT asks the kernel (syscall 616) for the running
			// executable's path and re-opens that file to parse its ELF headers;
			// hand it the /app0-mounted name of the game ELF.
			{
				const auto file_name =
				    Common::PathToString(m_programs.front()->file_name.filename());
				std::snprintf(g_payload_elf_path, sizeof(g_payload_elf_path), "/app0/%s",
				              file_name.c_str());
			}
			g_payload_argc    = 1;
			g_payload_argv[0] = "crispy-doom";
			for (size_t i = 0; i < guest_args.size() && i + 1 < std::size(g_payload_argv); i++) {
				g_payload_argv[i + 1] = guest_args[i].c_str();
				g_payload_argc++;
			}
			g_payload_argv[g_payload_argc] = nullptr;

			auto* payload_args = reinterpret_cast<PayloadArgs*>(
			    (reinterpret_cast<uintptr_t>(main_stack_top) - 0x200u) & ~static_cast<uintptr_t>(0x0f));
			// payloadout lives past the struct + scratch so __kernel_init's
			// in-place reads never collide with the payload's exit code slot.
			auto* payload_out = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(payload_args) + 0x80u);
			std::memset(payload_args, 0, sizeof(PayloadArgs));
			*payload_out = 0;
			// Keep dynlib metadata and exploit scratch separate. Both are guest
			// addresses; syscall bridges validate every access through the backing API.
			g_payload_metadata_guest = reinterpret_cast<uint64_t>(payload_args->kpipe_scratch);
			uint32_t metadata_version = 0x01000000;
			std::memcpy(payload_args->kpipe_scratch + 0x14, &metadata_version,
			            sizeof(metadata_version));
			payload_args->sys_dynlib_dlsym = reinterpret_cast<decltype(payload_args->sys_dynlib_dlsym)>(
			    g_payload_dlsym_bridge);
			payload_args->payloadout = payload_out;

			// The PS5 payload CRT dereferences rwpipe/rwpair (two int pairs) and
			// records kpipe_addr/kdata_base_addr during __kernel_init; give it
			// host-owned scratch in the guest address space instead of nulls.
			payload_args->rwpipe = reinterpret_cast<int*>(payload_args->kpipe_scratch + 0x20);
			payload_args->rwpair = reinterpret_cast<int*>(payload_args->kpipe_scratch + 0x28);
			payload_args->kpipe_addr = reinterpret_cast<intptr_t>(payload_args->kpipe_scratch + 0x20);
			payload_args->kdata_base_addr = reinterpret_cast<intptr_t>(payload_args->kpipe_scratch + 0x20);
			// Craft a fake FreeBSD proc + dynlib object in a guest window so
			// kernel_dynlib_handle/kernel_dynlib_obj (patched kernel_get_proc)
			// can answer sprx_open's sysmodule queries without the exploit pipe:
			//   proc+0x3e8 -> dynlib_object list head
			//   dynlist[0] + 0x08 -> next, +0x28 -> id, +0x2a0.. name/path/symtab
			// The 0x180-byte dynlib_object copied by kernel_dynlib_obj carries
			// the name (offset +0x00/0x10) and symtab/stroff/symcnt used by
			// sprx_sym2addr; values mirror an empty HLE-backed system module.
			if (const uint64_t window = PayloadMmapBase(); window != 0) {
				LOGF("PS5 payload fake kernel record: window = 0x%016" PRIx64 "\n", window);
				const uint64_t proc = window + 0x100000;
				auto store_u64 = [](uint64_t dst, uint64_t value) {
					std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(dst)), &value,
					            sizeof(value));
				};
				// kernel_dynlib_handle/kernel_dynlib_obj walk: *(proc+0x3e8) is a
				// container whose +0x00 is the first dynlib node; each iteration
				// first dereferences the cursor (cursor = *cursor), then checks
				// name@+0x08 and id@+0x28 of the node it lands on.
				// The handle walk compares the node name's basename segment
				// (delimited by '/') against the query, so store retail-style
				// full paths.
				static constexpr const char* kDynlibNames[] = {
				    "/system/common/lib/libSceSysmodule.sprx",
				    "/system/common/lib/libSceSystemService.sprx",
				    "/system/common/lib/libSceUserService.sprx",
				    "/system/common/lib/libScePad.sprx",
				    "/system/common/lib/libSceAudioOut.sprx",
				    "/system/common/lib/libSceVideoOut.sprx",
				    "/system/common/lib/libSceKeyboard.sprx",
				    "/system/common/lib/libSceImeDialog.sprx",
				    "/system/common/lib/libkernel_web.sprx",
				    "/system/common/lib/libSceLibcInternal.sprx",
				    "/system/common/lib/libSceNet.sprx"};
				constexpr uint64_t kNodeStride = 0x180;
				constexpr uint64_t kInfoStride = 0x120;
				const uint64_t cont  = proc + 0x1000;              // list container
				const uint64_t nodes = proc + 0x2000;              // 11 * 0x180
				const uint64_t infos = proc + 0x4000;              // 11 * 0x120
				const uint64_t names = proc + 0x6000;              // name strings
				const uint64_t strtb = proc + 0x7000;              // strtab "\0"
				const uint64_t symtb = proc + 0x7100;              // one zeroed Elf64_Sym
				std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(nodes)), 0,
				            kNodeStride * std::size(kDynlibNames));
				std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(infos)), 0,
				            kInfoStride * std::size(kDynlibNames));
				uint64_t name_pos = names;
				for (size_t i = 0; i < std::size(kDynlibNames); i++) {
					const auto  len  = std::strlen(kDynlibNames[i]) + 1;
					const auto  node = nodes + i * kNodeStride;
					const auto  info = infos + i * kInfoStride;
					std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(name_pos)),
					            kDynlibNames[i], len);
					// node: next@0, name_ptr@8, id@0x28, base@0x30, size@0x38.
					// The 12th kernel-flavor node (id 1) follows the 11 modules,
					// so the last module node links forward to it.
					store_u64(node + 0x00, nodes + (i + 1) * kNodeStride);
					store_u64(node + 0x08, name_pos);
					// module handle id: mirrors the sequential fake handles that
					// KernelLoadStartModule hands out for HLE'd system modules
					store_u64(node + 0x28, 0x12345001 + i);
					store_u64(node + 0x30, 0);    // image base
					store_u64(node + 0x38, 0);    // image size
					// node+0x148 = SymTabInfo pointer: kernel_dynlib_obj copies the
					// node into an obj buffer, and sprx_open reads the info ptr at
					// obj+0x148 (-0x190(%rbp) with obj at -0x2d8(%rbp)).
					store_u64(node + 0x148, info);
					// symtab info: symtab@+0x28 (size@+0x30), strtab@+0x38 (size@+0x40)
					store_u64(info + 0x28, symtb);
					store_u64(info + 0x30, 0x18);
					store_u64(info + 0x38, strtb);
					store_u64(info + 0x40, 1);
					name_pos += (len + 7) & ~7ull;
				}
				// Kernel-flavor node (id 1): sprx_open opens libkernel*.sprx
				// through the special branch — it resolves "sceKernelDlsym" from
				// the kernel dynlib and, when found, queries dynlib_obj with
				// handle 1. The node's name slot is rewritten on every stat of a
				// system module (g_payload_dynlib_name_slot), so the object walk
				// always answers with the module currently being opened.
				{
					const auto  node = nodes + std::size(kDynlibNames) * kNodeStride;
					const auto  info = infos + std::size(kDynlibNames) * kInfoStride;
					const auto  kernel_name =
					    names + 0x800; // dedicated mutable name buffer
					std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(kernel_name)),
					            "/system/priv/lib/libkernel_web.sprx",
					            sizeof("/system/priv/lib/libkernel_web.sprx"));
					std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 0,
					            kNodeStride);
					std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(info)), 0,
					            kInfoStride);
					store_u64(node + 0x00, 0);    // tail of the list
					store_u64(node + 0x08, kernel_name);
					store_u64(node + 0x28, 1);    // special-branch dynlib handle
					store_u64(node + 0x148, info);
					store_u64(info + 0x28, symtb);
					store_u64(info + 0x30, 0x18);
					store_u64(info + 0x38, strtb);
					store_u64(info + 0x40, 1);
					g_payload_dynlib_name_slot = kernel_name;
				}
				std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(symtb)), 0, 0x18);
				*reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(strtb)) = '\0';
				store_u64(cont + 0x00, nodes);
				store_u64(proc + 0x3e8, cont);
				g_payload_proc_guest = proc;
				LOGF("PS5 payload fake kernel record: proc = 0x%016" PRIx64
				     ", nodes = 0x%016" PRIx64 "\n",
				     proc, nodes);
			}
			LOGF("PS5 payload entry: argc=%d dlsym=0x%016" PRIx64 "\n", g_payload_argc,
			     g_payload_dlsym_bridge);
			PatchPayloadKernelCrt(m_programs.front());
			RunEntry(entry, reinterpret_cast<EntryParams*>(payload_args), ProgramExitHandler,
			         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(payload_args) - 0x1000u));
			return;
		}
		auto* params = reinterpret_cast<EntryParams*>(
		    (reinterpret_cast<uintptr_t>(main_stack_top) - 0x100u) & ~static_cast<uintptr_t>(0x0f));
		std::memset(params, 0, sizeof(EntryParams));
		params->argc    = 1;
		params->argv[0] = "KytyEmu";
		for (size_t i = 0; i < guest_args.size() && i + 1 < std::size(params->argv); i++) {
			params->argv[i + 1] = guest_args[i].c_str();
			params->argc++;
		}

		LOGF("stack_addr = %" PRIx64 "\n", reinterpret_cast<uint64_t>(params));

		RunEntry(entry, params, ProgramExitHandler,
		         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(params) - 0x1000u));
	}
}

void RuntimeLinker::Clear() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);
	GamePatch::Clear();

	for (auto* p: m_programs) {
		DeleteProgram(p);
	}
	m_programs.clear();
	for (const auto page: g_unresolved_stub_thunk_pages) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(page, UNRESOLVED_STUB_PAGE_SIZE));
	}
	g_unresolved_stub_thunk_pages.clear();
	g_unresolved_stub_thunk_offset = 0;
	g_stubbed_imports.clear();
	g_unresolved_stub_call_log_count.store(0);
	if (g_invalid_memory != 0) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(g_invalid_memory, 4096));
		g_invalid_memory = 0;
	}
	g_tls_main_program        = nullptr;
	g_tls_cached_main_program = nullptr;
	g_tls_cached_main_tcb     = nullptr;
	// Payload dlsym keeps a non-owning pointer into m_symbols. Clear it before
	// destroying the database so a later payload cannot observe freed state.
	g_payload_symbols      = nullptr;
	g_payload_dlsym_bridge = 0;
	g_payload_metadata_guest = 0;
	g_payload_proc_guest   = 0;
	g_payload_dynlib_name_slot = 0;
	g_payload_last_module_path[0] = '\0';
	payload_fd_map.clear();
	payload_fd_next        = 0;
	payload_mmap_base      = 0;
	payload_mmap_pos       = 0;
	g_desired_base_addr       = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	m_symbols.reset();
	m_relocated = false;
}

void RuntimeLinker::Resolve(const std::string& name, SymbolType type, Program* program,
                            SymbolRecord* out_info, bool* bind_self) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);

	auto ids = Common::Split(name, '#');

	if (bind_self != nullptr) {
		*bind_self = false;
	}

	if (ids.size() == 3) {
		const LibraryId* l = FindLibrary(*program, ids.at(1));
		const ModuleId*  m = FindModule(*program, ids.at(2));

		auto resolve_by_nid = [this, type](const std::string& nid, SymbolRecord* out) -> bool {
			EXIT_IF(out == nullptr);

			if (m_symbols != nullptr) {
				if (const auto* rec = m_symbols->FindByNid(nid, type); rec != nullptr) {
					*out = *rec;
					return true;
				}
			}

			for (auto* p: m_programs) {
				if (p != nullptr && p->export_symbols != nullptr) {
					if (const auto* rec = p->export_symbols->FindByNid(nid, type); rec != nullptr) {
						*out = *rec;
						return true;
					}
				}
			}

			return false;
		};

		if (l != nullptr && m != nullptr) {
			SymbolResolve sr {};
			sr.name                 = ids.at(0);
			sr.library              = l->name;
			sr.library_version      = l->version;
			sr.module               = m->name;
			sr.module_version_major = m->version_major;
			sr.module_version_minor = m->version_minor;
			sr.type                 = type;

			const SymbolRecord* rec = nullptr;

			if (m_symbols != nullptr) {
				rec = m_symbols->Find(sr);
			}

			if (rec == nullptr) {
				if (auto* p = FindProgram(*m, *l); p != nullptr && p->export_symbols != nullptr) {
					rec = p->export_symbols->Find(sr);
					if (bind_self != nullptr) {
						*bind_self = (p == program);
					}
				}
			}

			if (rec == nullptr) {
				if (resolve_by_nid(sr.name, out_info)) {
					LOGF("PS5 NID fallback: %s -> %s\n", sr.name.c_str(), out_info->name.c_str());
					return;
				}
			}

			if (rec != nullptr) {
				//*out_vaddr = rec->vaddr;
				*out_info = *rec;
			} else {
				out_info->vaddr    = 0;
				out_info->name     = SymbolDatabase::GenerateName(sr);
				out_info->dbg_name = "";
			}
		} else {
			if (resolve_by_nid(ids.at(0), out_info)) {
				LOGF("PS5 NID fallback: %s -> %s (missing lib/module metadata)\n",
				     ids.at(0).c_str(), out_info->name.c_str());
				return;
			}

			EXIT("l == nullptr || m == nullptr");
		}
	} else {
		auto resolve_plain_name = [this, type, &name](SymbolRecord* out) -> bool {
			if (m_symbols != nullptr) {
				if (const auto* rec = m_symbols->FindByName(name, type); rec != nullptr) {
					*out = *rec;
					return true;
				}
			}
			for (auto* p: m_programs) {
				if (p != nullptr && p->export_symbols != nullptr) {
					if (const auto* rec = p->export_symbols->FindByName(name, type); rec != nullptr) {
						*out = *rec;
						return true;
					}
				}
			}
			return false;
		};
		if (resolve_plain_name(out_info)) {
			LOGF("PS5 payload symbol fallback: %s -> %s\n", name.c_str(),
			     out_info->name.c_str());
		} else {
			out_info->vaddr    = 0;
			out_info->name     = name;
			out_info->dbg_name = "";
		}
	}
}

bool RuntimeLinker::ResolveLoadedSymbolByNid(const std::string& nid, SymbolType type,
                                             SymbolRecord* out_info) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);

	for (auto* p: m_programs) {
		if (p != nullptr && p->export_symbols != nullptr) {
			if (const auto* rec = p->export_symbols->FindByNid(nid, type); rec != nullptr) {
				*out_info = *rec;
				return true;
			}
		}
	}

	if (m_symbols != nullptr) {
		if (const auto* rec = m_symbols->FindByNid(nid, type); rec != nullptr) {
			*out_info = *rec;
			return true;
		}
	}

	return false;
}

uint64_t RuntimeLinker::ReadFromElf(Program* program, uint64_t vaddr) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->base_vaddr == 0 || program->base_size == 0);
	EXIT_IF(program->elf == nullptr);

	uint64_t ret = 0;

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr      = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size = phdr[i].p_filesz;

			if (vaddr >= segment_addr && vaddr < segment_addr + segment_file_size) {
				program->elf->LoadSegment(reinterpret_cast<uint64_t>(&ret),
				                          phdr[i].p_offset + vaddr - segment_addr, sizeof(ret));
				break;
			}
		}
	}

	return ret;
}

Program* RuntimeLinker::FindProgramById(int32_t id) {
	Common::LockGuard lock(m_mutex);

	// Id 0 is reserved for main program
	if (id == 0 && !m_programs.empty()) {
		return m_programs.front();
	}

	for (auto* p: m_programs) {
		if (p->unique_id == id) {
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByFileName(const std::filesystem::path& elf_name) {
	Common::LockGuard lock(m_mutex);

	auto fixed_name = Common::FixFilenameSlash(Common::PathToGenericString(elf_name));
	for (auto* p: m_programs) {
		if (Common::EqualNoCase(Common::FixFilenameSlash(Common::PathToGenericString(p->file_name)),
		                        fixed_name)) {
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByAddr(uint64_t vaddr) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		const auto* ehdr = p->elf->GetEhdr();
		const auto* phdr = p->elf->GetPhdr();

		EXIT_IF(phdr == nullptr || ehdr == nullptr);

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
			if (phdr[i].p_memsz != 0 &&
			    (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
				uint64_t segment_addr = phdr[i].p_vaddr + p->base_vaddr;
				uint64_t segment_size = GetAlignedSize(phdr + i);

				if (vaddr >= segment_addr && vaddr < segment_addr + segment_size) {
					return p;
				}
			}
		}
	}

	return nullptr;
}

void RuntimeLinker::StackTrace(uint64_t frame_ptr, uint64_t stack_ptr) {
	void* stack[20];
	int   depth = WalkGuestStack(frame_ptr, stack_ptr, stack, static_cast<int>(std::size(stack)));

	LOGF("Stack trace [thread = %d]:\n", Common::Thread::GetThreadIdUnique());

	for (int i = 0; i < depth; i++) {
		auto  vaddr = reinterpret_cast<uint64_t>(stack[i]);
		auto* p     = FindProgramByAddr(vaddr);
		LOGF("[%d] %016" PRIx64 ", off=%016" PRIx64 ", %s\n", i, vaddr,
		     (p == nullptr ? 0 : vaddr - p->base_vaddr),
		     (p == nullptr
		          ? "???"
		          : Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name))
		                .c_str()));
	}
}

static std::string GetProgramModuleName(const Program* program) {
	EXIT_IF(program == nullptr);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0') {
		return std::string(program->dynamic_info->so_name);
	}

	return Common::FilenameWithoutDirectory(Common::PathToGenericString(program->file_name));
}

static bool ModuleStartDependenciesSatisfied(const Program*               program,
                                             const std::vector<Program*>& programs,
                                             const std::vector<Program*>& started) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);

	for (const auto* needed: program->dynamic_info->needed) {
		if (needed == nullptr || needed[0] == '\0') {
			continue;
		}

		const auto needed_name = std::string(needed);

		for (auto* dependency: programs) {
			if (dependency == nullptr || dependency == program || dependency->elf == nullptr ||
			    !dependency->elf->IsShared()) {
				continue;
			}

			const auto dependency_name = GetProgramModuleName(dependency);
			if (Common::EqualNoCase(dependency_name, needed_name) ||
			    Common::EqualNoCase(Common::FilenameWithoutDirectory(
			                            Common::PathToGenericString(dependency->file_name)),
			                        needed_name)) {
				if (std::find(started.begin(), started.end(), dependency) == started.end()) {
					return false;
				}
				break;
			}
		}
	}

	return true;
}

void RuntimeLinker::StartAllModules() {
	Common::LockGuard lock(m_mutex);

	std::vector<Program*> started;

	for (;;) {
		bool progressed = false;

		for (auto* p: m_programs) {
			if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
			    std::find(started.begin(), started.end(), p) == started.end() &&
			    ModuleStartDependenciesSatisfied(p, m_programs, started)) {
				StartModule(p, 0, nullptr, nullptr);
				started.push_back(p);
				progressed = true;
			}
		}

		if (!progressed) {
			break;
		}
	}

	for (auto* p: m_programs) {
		if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
		    std::find(started.begin(), started.end(), p) == started.end()) {
			StartModule(p, 0, nullptr, nullptr);
			started.push_back(p);
		}
	}
}

void RuntimeLinker::StopAllModules() {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		if (p->elf->IsShared() && p->dynamic_info->fini_vaddr != 0) {
			StopModule(p, 0, nullptr, nullptr);
		}
	}
}

static bool IsAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return Common::EndsWith(lower, ".prx") || Common::EndsWith(lower, ".sprx");
}

static bool SkipAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return lower == "eboot.bin" || lower == "libkernel.prx" || lower == "libkernel_sys.prx";
}

void RuntimeLinker::PreloadAdjacentPrograms() {
	if (m_programs.empty()) {
		return;
	}

	std::vector<std::filesystem::path> module_paths;

	auto is_loaded = [this](const std::filesystem::path& path) {
		auto fixed_path = Common::FixFilenameSlash(Common::PathToGenericString(path));
		for (auto* program: m_programs) {
			if (Common::EqualNoCase(
			        Common::FixFilenameSlash(Common::PathToGenericString(program->file_name)),
			        fixed_path)) {
				return true;
			}
		}
		return false;
	};

	auto add_path = [&module_paths, &is_loaded](const std::filesystem::path& path) {
		if (is_loaded(path)) {
			return;
		}
		for (const auto& p: module_paths) {
			if (Common::EqualNoCase(Common::PathToGenericString(p),
			                        Common::PathToGenericString(path))) {
				return;
			}
		}
		module_paths.push_back(path);
	};

	auto add_dir = [&add_path](const std::filesystem::path& dir) {
		if (!Common::File::IsDirectoryExisting(dir)) {
			return;
		}
		for (const auto& entry: Common::File::GetDirEntries(dir)) {
			if (entry.is_file && IsAdjacentModuleFile(entry.name) &&
			    !SkipAdjacentModuleFile(entry.name)) {
				add_path(dir / entry.name);
			}
		}
	};

	auto root = m_programs.at(0)->file_name.parent_path();
	if (root.empty()) {
		return;
	}

	add_dir(root);
	add_dir(root / "sce_module");
	add_dir(root / "sce_modules");

	for (const auto& path: module_paths) {
		auto* program                        = LoadProgram(path);
		program->fail_if_global_not_resolved = false;
	}
}

int RuntimeLinker::StartModule(Program* program, size_t args, const void* argp,
                               module_func_t func) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Start module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

	return reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->init_vaddr +
	                                                program->base_vaddr)(args, argp, func);
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Stop module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

	int result = reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->fini_vaddr +
	                                                      program->base_vaddr)(args, argp, func);

	Libs::LibKernel::PthreadDeleteStaticObjects(program);

	return result;
}

uint8_t* RuntimeLinker::TlsGetAddr(Program* program) {
	EXIT_IF(program == nullptr);

	Common::LockGuard lock(program->tls.mutex);

	auto& tls = program->tls.tlss[Common::Thread::GetThreadIdUnique()];

	if (tls.ptr == nullptr) {
		constexpr uint64_t TCB_SIZE  = 0x40;
		constexpr uint64_t TCB_ALIGN = 0x20;

		const auto tcb_offset =
		    program->tls.tcb_offset != 0 ? program->tls.tcb_offset : program->tls.image_size;
		const auto alloc_size = AlignUp(tcb_offset, TCB_ALIGN) + TCB_SIZE;
		tls.ptr        = reinterpret_cast<uint8_t*>(Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, alloc_size, Common::VirtualMemory::Mode::ReadWrite, "thread_local_storage"));
		tls.free_func  = nullptr;
		tls.vm_alloc   = true;
		tls.alloc_size = alloc_size;

		EXIT_IF(tls.ptr == nullptr);

		std::memset(tls.ptr, 0, alloc_size);

		if (!program->tls.init_image.empty()) {
			std::memcpy(tls.ptr, program->tls.init_image.data(), program->tls.init_image.size());
		} else {
			std::memcpy(tls.ptr, reinterpret_cast<void*>(program->tls.image_vaddr),
			            program->tls.init_size);
		}

		auto* tcb = reinterpret_cast<uint64_t*>(tls.ptr + tcb_offset);
		tcb[0]    = reinterpret_cast<uint64_t>(tcb);
	}

	return tls.ptr;
}

void RuntimeLinker::DeleteTls(Program* program, int thread_id) {
	EXIT_IF(program == nullptr);

	if (thread_id == Common::Thread::GetThreadIdUnique() && g_tls_cached_main_program == program) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}

	Common::LockGuard lock(program->tls.mutex);

	if (auto it = program->tls.tlss.find(thread_id); it != program->tls.tlss.end()) {
		FreeTlsBlock(&it->second);
		program->tls.tlss.erase(it);
	}
}

static uint64_t CalcBaseSize(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr) {
	uint64_t base_size = 0;
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t last_addr = phdr[i].p_vaddr + GetAlignedSize(phdr + i);
			if (last_addr > base_size) {
				base_size = last_addr;
			}
		}
	}
	return base_size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RuntimeLinker::LoadProgramToMemory(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->base_vaddr != 0 || program->base_size != 0 ||
	        program->elf == nullptr);

	// static uint64_t desired_base_addr = DESIRED_BASE_ADDR;

	bool is_shared   = program->elf->IsShared();
	bool is_next_gen = program->elf->IsNextGen();
	bool is_payload  = program->elf->GetEhdr()->e_type == ET_SYSV_DYN;

	EXIT_NOT_IMPLEMENTED(!is_shared && !is_next_gen && !is_payload);

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	program->base_size                 = CalcBaseSize(ehdr, phdr);
	constexpr uint64_t GUEST_PAGE_SIZE = 0x4000;
	EXIT_IF(program->base_size > UINT64_MAX - (GUEST_PAGE_SIZE - 1));
	program->base_size_aligned = AlignUp(program->base_size, GUEST_PAGE_SIZE);

	uint64_t tls_handler_size = is_shared ? 0 : Jit::SafeCall::GetSize();
	EXIT_IF(tls_handler_size > UINT64_MAX - program->base_size_aligned);
	program->mapped_size = program->base_size_aligned + tls_handler_size;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const bool         use_red_zone_protection  = Config::RedZoneProtectionEnabled();
	constexpr uint64_t RED_ZONE_TRAMPOLINE_SIZE = 8u * 1024u * 1024u;
	if (use_red_zone_protection) {
		EXIT_IF(RED_ZONE_TRAMPOLINE_SIZE > UINT64_MAX - program->mapped_size);
		program->mapped_size += RED_ZONE_TRAMPOLINE_SIZE;
	}
#endif

	program->base_vaddr = Libs::LibKernel::Memory::AllocateProgramMemory(
	    g_desired_base_addr, program->mapped_size, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    Common::PathToString(program->file_name.filename()).c_str());
	EXIT_IF(program->base_vaddr == 0);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (use_red_zone_protection) {
		program->red_zone_trampoline_vaddr = program->base_vaddr + program->base_size_aligned;
		program->red_zone_trampoline_size  = RED_ZONE_TRAMPOLINE_SIZE;
		RegisterRedZonePatchModule(reinterpret_cast<void*>(program->base_vaddr),
		                           program->base_size_aligned,
		                           reinterpret_cast<void*>(program->red_zone_trampoline_vaddr),
		                           program->red_zone_trampoline_size);
	}
#endif
	if (!is_shared) {
		program->tls.handler_vaddr = program->base_vaddr + program->base_size_aligned;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		program->tls.handler_vaddr += program->red_zone_trampoline_size;
#endif
	}

	g_desired_base_addr += CODE_BASE_INCR * (1 + program->mapped_size / CODE_BASE_INCR);

	EXIT_IF(program->base_size_aligned < program->base_size);
	LOGF("base_vaddr             = 0x%016" PRIx64 "\n"
	     "base_size              = 0x%016" PRIx64 "\n"
	     "base_size_aligned      = 0x%016" PRIx64 "\n"
	     "mapped_size            = 0x%016" PRIx64 "\n",
	     program->base_vaddr, program->base_size, program->base_size_aligned, program->mapped_size);
	if (!is_shared) {
		LOGF("tls_handler_size       = 0x%016" PRIx64 "\n", tls_handler_size);
	}

	if (!Common::HostException::InstallHandler(KytyExceptionHandler)) {
		EXIT("Failed to install the required vectored exception handler\n");
	}

	// program->elf->SetBaseVAddr(program->base_vaddr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::vector<std::pair<uint64_t, uint64_t>> executable_segments;
	uint64_t                                   eh_frame_header_addr = 0;
	uint64_t                                   eh_frame_header_size = 0;
#endif

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr        = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size   = phdr[i].p_filesz;
			uint64_t segment_memory_size = GetAlignedSize(phdr + i);
			auto     mode                = GetMode(phdr[i].p_flags);

			LOGF("[%d] addr        = 0x%016" PRIx64 "\n"
			     "[%d] file_size   = %" PRIu64 "\n"
			     "[%d] memory_size = %" PRIu64 "\n"
			     "[%d] mode        = %s\n",
			     i, segment_addr, i, segment_file_size, i, segment_memory_size, i,
			     Common::EnumName(mode).c_str());

			program->elf->LoadSegment(segment_addr, phdr[i].p_offset, segment_file_size);

			bool skip_protect = (phdr[i].p_type == PT_LOAD && is_next_gen &&
			                     mode == Common::VirtualMemory::Mode::NoAccess);

			if (Common::VirtualMemory::IsExecute(mode)) {
				PatchProgram(program, segment_addr, segment_memory_size);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
				if (use_red_zone_protection) {
					executable_segments.emplace_back(segment_addr, segment_file_size);
				}
#endif
			}

			if (!skip_protect) {
				Libs::LibKernel::Memory::SetProgramMemoryProtection(segment_addr,
				                                                    segment_memory_size, mode);

				if (Common::VirtualMemory::IsExecute(mode)) {
					Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_memory_size);
				}
			}
		}

		if (phdr[i].p_type == PT_TLS) {
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->tls.image_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			program->tls.init_size   = std::min(phdr[i].p_filesz, GetAlignedSize(phdr + i));
			program->tls.image_size  = GetAlignedSize(phdr + i);
			program->tls.tcb_offset  = program->tls.image_size;

			LOGF("tls addr = 0x%016" PRIx64 "\n"
			     "tls init   = %" PRIu64 "\n"
			     "tls size   = %" PRIu64 "\n"
			     "tls offset = %" PRIu64 "\n",
			     program->tls.image_vaddr, program->tls.init_size, program->tls.image_size,
			     program->tls.tcb_offset);
		}

		if (phdr[i].p_type == PT_OS_PROCPARAM) {
			EXIT_IF(program->proc_param_vaddr != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->proc_param_vaddr = phdr[i].p_vaddr + program->base_vaddr;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (use_red_zone_protection && phdr[i].p_type == PT_GNU_EH_FRAME) {
			eh_frame_header_addr = phdr[i].p_vaddr + program->base_vaddr;
			eh_frame_header_size = phdr[i].p_memsz;
		}
#endif
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (use_red_zone_protection) {
		std::vector<uintptr_t> function_starts;
		if (!DecodeEhFrameFunctionStarts(eh_frame_header_addr, eh_frame_header_size,
		                                 &function_starts)) {
			LOGF("Windows guest red-zone patching could not decode function boundaries for %s\n",
			     Common::PathToString(program->file_name).c_str());
		}
		for (const auto& [segment_addr, segment_size]: executable_segments) {
			const auto result =
			    PatchRedZoneMemoryInstructions(segment_addr, segment_size, function_starts);
			LOGF("Windows guest red-zone patching: %s, functions=%" PRIu64 ", red_zone=%" PRIu64
			     ", memory=%" PRIu64 ", patched=%" PRIu64 ", short=%" PRIu64 ", stack=%" PRIu64
			     ", control=%" PRIu64 ", unrelocatable=%" PRIu64 "\n",
			     Common::PathToString(program->file_name.filename()).c_str(), result.function_count,
			     result.red_zone_function_count, result.memory_instruction_count,
			     result.patched_memory_instruction_count, result.short_memory_instruction_count,
			     result.stack_dependent_memory_instruction_count,
			     result.control_flow_memory_instruction_count,
			     result.unrelocatable_memory_instruction_count);
			Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_size);
		}
		Common::VirtualMemory::FlushInstructionCache(program->red_zone_trampoline_vaddr,
		                                             program->red_zone_trampoline_size);
	}
#endif

	if (!is_shared) {
		SetupTlsHandler(program);
	}

	LOGF("entry = 0x%016" PRIx64 "\n", program->elf->GetEntry() + program->base_vaddr);
}

void RuntimeLinker::DeleteProgram(Program* p) {
	auto program = std::unique_ptr<Program>(p);
	if (g_tls_main_program == program.get()) {
		g_tls_main_program = nullptr;
	}
	if (g_tls_cached_main_program == program.get()) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}
	for (auto& record: g_stubbed_imports) {
		if (record.patch_vaddr >= program->base_vaddr &&
		    record.patch_vaddr < program->base_vaddr + program->mapped_size) {
			record.patch_vaddr = 0;
		}
	}

	if (program->base_vaddr != 0 || program->mapped_size != 0) {
		EXIT_IF(program->base_vaddr == 0 || program->mapped_size == 0);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (program->red_zone_trampoline_size != 0) {
			UnregisterRedZonePatchModule(reinterpret_cast<void*>(program->base_vaddr));
		}
#endif
		EXIT_IF(
		    !Libs::LibKernel::Memory::FreeGuestMemory(program->base_vaddr, program->mapped_size));
	}

	if (program->custom_call_plt_vaddr != 0 || program->custom_call_plt_num != 0) {
		const auto size = Jit::CallPlt::GetSize(program->custom_call_plt_num);
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(program->custom_call_plt_vaddr, size));
	}
}

void RuntimeLinker::ParseProgramDynamicInfo(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->dynamic_info != nullptr);

	program->dynamic_info = std::make_unique<DynamicInfo>();

	auto* elf = program->elf.get();

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_HASH) && elf->HasDynValue(DT_HASH));
	GetDynDataOs(elf, &program->dynamic_info->hash_table, DT_OS_HASH);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->hash_table, DT_HASH);
	GetDynValue(elf, &program->dynamic_info->hash_table_size, DT_OS_HASHSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRTAB) && elf->HasDynValue(DT_STRTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRSZ) && elf->HasDynValue(DT_STRSZ));
	GetDynDataOs(elf, &program->dynamic_info->str_table, DT_OS_STRTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->str_table, DT_STRTAB);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_OS_STRSZ);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_STRSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMTAB) && elf->HasDynValue(DT_SYMTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMENT) && elf->HasDynValue(DT_SYMENT));
	GetDynDataOs(elf, &program->dynamic_info->symbol_table, DT_OS_SYMTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->symbol_table, DT_SYMTAB);
	GetDynValue(elf, &program->dynamic_info->symbol_table_total_size, DT_OS_SYMTABSZ);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_OS_SYMENT);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_SYMENT);

	GetDynPtr(elf, &program->dynamic_info->init_vaddr, DT_INIT);
	GetDynPtr(elf, &program->dynamic_info->fini_vaddr, DT_FINI);
	GetDynPtr(elf, &program->dynamic_info->init_array_vaddr, DT_INIT_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->fini_array_vaddr, DT_FINI_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->preinit_array_vaddr, DT_PREINIT_ARRAY);
	GetDynValue(elf, &program->dynamic_info->init_array_size, DT_INIT_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->fini_array_size, DT_FINI_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->preinit_array_size, DT_PREINIT_ARRAYSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTGOT) && elf->HasDynValue(DT_PLTGOT));
	GetDynPtr(elf, &program->dynamic_info->pltgot_vaddr, DT_OS_PLTGOT);
	GetDynPtr(elf, &program->dynamic_info->pltgot_vaddr, DT_PLTGOT);

	Elf64_Sxword jmprel_type = 0;
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTREL) && elf->HasDynValue(DT_PLTREL));
	GetDynValue(elf, &jmprel_type, DT_OS_PLTREL);
	GetDynValue(elf, &jmprel_type, DT_PLTREL);

	// Standard PS5 Payload SDK PIEs may have no PLT relocation table at all: every
	// relocation is carried by DT_RELA. Retail SELF programs still require RELA.
	EXIT_NOT_IMPLEMENTED(jmprel_type != 0 && jmprel_type != DT_RELA);
	if (jmprel_type == DT_RELA) {
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_JMPREL) && elf->HasDynValue(DT_JMPREL));
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTRELSZ) && elf->HasDynValue(DT_PLTRELSZ));
		GetDynDataOs(elf, &program->dynamic_info->jmprela_table, DT_OS_JMPREL);
		GetDynData(elf, program->base_vaddr, &program->dynamic_info->jmprela_table, DT_JMPREL);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_OS_PLTRELSZ);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_PLTRELSZ);
	}

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_RELA) && elf->HasDynValue(DT_RELA));
	GetDynDataOs(elf, &program->dynamic_info->rela_table, DT_OS_RELA);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->rela_table, DT_RELA);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_OS_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_OS_RELAENT);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_RELAENT);

	GetDynValue(elf, &program->dynamic_info->relative_count, DT_RELACOUNT);

	GetDynValue(elf, &program->dynamic_info->debug, DT_DEBUG);
	GetDynValue(elf, &program->dynamic_info->flags, DT_FLAGS);
	GetDynValue(elf, &program->dynamic_info->textrel, DT_TEXTREL);

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->debug != 0);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->textrel != 0);

	std::vector<uint64_t> needed;
	GetDynValues(elf, &needed, DT_NEEDED);
	for (auto need: needed) {
		program->dynamic_info->needed.push_back(program->dynamic_info->str_table + need);
	}

	uint64_t so_name = 0;
	GetDynValue(elf, &so_name, DT_SONAME);
	program->dynamic_info->so_name = program->dynamic_info->str_table + so_name;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_NEEDED_MODULE) &&
	                     elf->HasDynValue(DT_OS_NEEDED_MODULE_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_MODULE_INFO) &&
	                     elf->HasDynValue(DT_OS_MODULE_INFO_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_IMPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_IMPORT_LIB_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_EXPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_EXPORT_LIB_1));
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE);
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE_1);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO_1);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB_1);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB_1);
}

static void InstallRelocateHandler(Program* program) {
	KYTY_PROFILER_FUNCTION();

	uint64_t pltgot_vaddr = program->dynamic_info->pltgot_vaddr + program->base_vaddr;
	uint64_t pltgot_size  = static_cast<uint64_t>(3) * 8;
	void**   pltgot       = reinterpret_cast<void**>(pltgot_vaddr);

	Common::VirtualMemory::Mode old_mode {};
	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(
	    pltgot_vaddr, pltgot_size, Common::VirtualMemory::Mode::Write, &old_mode));

	pltgot[1] = program;
	pltgot[2] = reinterpret_cast<void*>(RelocateHandler);

	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(pltgot_vaddr, pltgot_size, old_mode));

	if (Common::VirtualMemory::IsExecute(old_mode)) {
		Common::VirtualMemory::FlushInstructionCache(pltgot_vaddr, pltgot_size);
	}

	// TODO(): check if this table already generated by compiler (sometimes it is missing)
	if (program->custom_call_plt_vaddr == 0) {
		program->custom_call_plt_num =
		    program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
		auto size                      = Jit::CallPlt::GetSize(program->custom_call_plt_num);
		program->custom_call_plt_vaddr = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    SYSTEM_RESERVED, size, Common::VirtualMemory::Mode::Write, "custom_call_plt");
		EXIT_NOT_IMPLEMENTED(program->custom_call_plt_vaddr == 0);
		auto* code = new (reinterpret_cast<void*>(program->custom_call_plt_vaddr))
		    Jit::CallPlt(program->custom_call_plt_num);
		code->SetPltGot(pltgot_vaddr);
		EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(program->custom_call_plt_vaddr, size,
		                                                     Common::VirtualMemory::Mode::Execute));
		Common::VirtualMemory::FlushInstructionCache(program->custom_call_plt_vaddr, size);
	}
}

void RuntimeLinker::Relocate(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);

	if (g_invalid_memory == 0) {
		g_invalid_memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    INVALID_MEMORY, 4096, Common::VirtualMemory::Mode::NoAccess, "invalid_memory", true);
		EXIT_NOT_IMPLEMENTED(g_invalid_memory == 0);
	}

	LOGF_COLOR(Log::Color::White, "--- Relocate program: %s ---\n",
	           Common::PathToString(program->file_name).c_str());

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table == nullptr);

	// Payload SDK PIEs can put every relocation in DT_RELA and omit both PLT and GOT.
	// The GOT relocation handler is only needed for a real DT_JMPREL table.
	if (program->dynamic_info->jmprela_table != nullptr) {
		EXIT_NOT_IMPLEMENTED(program->dynamic_info->pltgot_vaddr == 0);
		InstallRelocateHandler(program);
	}

	std::vector<std::string> unresolved;
	const bool               imports_only = program->relocated;

	RelocateRecords(program->dynamic_info->rela_table, program->dynamic_info->rela_table_total_size,
	                program, false, imports_only, &unresolved);
	if (program->dynamic_info->jmprela_table != nullptr) {
		RelocateRecords(program->dynamic_info->jmprela_table,
		                program->dynamic_info->jmprela_table_size, program, true, imports_only,
		                &unresolved);
	}
	program->relocated = true;

	if (program->tls.image_vaddr != 0 && program->tls.init_size != 0 &&
	    program->tls.init_image.empty()) {
		const auto* src = reinterpret_cast<const uint8_t*>(program->tls.image_vaddr);
		program->tls.init_image.assign(src, src + program->tls.init_size);
	}

	if (!unresolved.empty()) {
		LOGF("--- Stubbed unresolved imports: %zu ---\n", unresolved.size());
		for (const auto& symbol: unresolved) {
			LOGF("Stubbed: %s\n", symbol.c_str());
		}
	}
}

Program* RuntimeLinker::FindProgram(const ModuleId& m, const LibraryId& l) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		const auto& export_libs    = p->dynamic_info->export_libs;
		const auto& export_modules = p->dynamic_info->export_modules;

		if (std::find(export_libs.begin(), export_libs.end(), l) != export_libs.end() &&
		    std::find(export_modules.begin(), export_modules.end(), m) != export_modules.end()) {
			return p;
		}
	}
	return nullptr;
}

const ModuleId* RuntimeLinker::FindModule(const Program& program, const std::string& id) {
	const auto& import_modules = program.dynamic_info->import_modules;

	if (auto it = std::find_if(import_modules.begin(), import_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != import_modules.end()) {
		return &(*it);
	}

	const auto& export_modules = program.dynamic_info->export_modules;

	if (auto it = std::find_if(export_modules.begin(), export_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != export_modules.end()) {
		return &(*it);
	}

	return nullptr;
}

const LibraryId* RuntimeLinker::FindLibrary(const Program& program, const std::string& id) {
	const auto& import_libs = program.dynamic_info->import_libs;

	if (auto it = std::find_if(import_libs.begin(), import_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != import_libs.end()) {
		return &(*it);
	}

	const auto& export_libs = program.dynamic_info->export_libs;

	if (auto it = std::find_if(export_libs.begin(), export_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != export_libs.end()) {
		return &(*it);
	}

	return nullptr;
}

void RuntimeLinker::CreateSymbolDatabase(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->export_symbols != nullptr);
	EXIT_IF(program->import_symbols != nullptr);

	program->export_symbols = std::make_unique<SymbolDatabase>();
	program->import_symbols = std::make_unique<SymbolDatabase>();

	auto syms = [](Program* program, SymbolDatabase* symbols, bool is_export) {
		if (program->dynamic_info->symbol_table == nullptr ||
		    program->dynamic_info->str_table == nullptr) {
			return;
		}

		for (auto* sym = program->dynamic_info->symbol_table;
		     reinterpret_cast<uint8_t*>(sym) <
		     reinterpret_cast<uint8_t*>(program->dynamic_info->symbol_table) +
		         program->dynamic_info->symbol_table_total_size;
		     sym++) {
			std::string id   = std::string(program->dynamic_info->str_table + sym->st_name);
			auto        bind = sym->GetBind();
			auto        type = sym->GetType();
			auto        ids  = Common::Split(id, '#');

			if (ids.size() == 3) {
				const auto* l = FindLibrary(*program, ids.at(1));
				const auto* m = FindModule(*program, ids.at(2));

				if (l != nullptr && m != nullptr && (bind == STB_GLOBAL || bind == STB_WEAK) &&
				    (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
				    is_export == (sym->st_value != 0)) {
					SymbolResolve sr {};
					sr.name                 = ids.at(0);
					sr.library              = l->name;
					sr.library_version      = l->version;
					sr.module               = m->name;
					sr.module_version_major = m->version_major;
					sr.module_version_minor = m->version_minor;
					switch (type) {
						case STT_NOTYPE: sr.type = SymbolType::NoType; break;
						case STT_FUNC: sr.type = SymbolType::Func; break;
						case STT_OBJECT: sr.type = SymbolType::Object; break;
						default: sr.type = SymbolType::Unknown; break;
					}
					symbols->Add(sr, (is_export ? sym->st_value + program->base_vaddr : 0));
				}
			}
		}
	};

	syms(program, program->export_symbols.get(), true);
	syms(program, program->import_symbols.get(), false);
}

void RuntimeLinker::SetupTlsHandler(Program* program) {
	EXIT_IF(program == nullptr);
	EXIT_IF(g_tls_main_program != nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->elf->IsShared());
	EXIT_IF(program->tls.handler_vaddr == 0);

	g_tls_main_program = program;

	auto* code = new (reinterpret_cast<void*>(program->tls.handler_vaddr)) Jit::SafeCall;

	code->SetFunc(TlsMainGetAddr);

	for (uint8_t reg = 1; reg < 8; reg++) {
		if (reg == 4) {
			continue;
		}

		auto* stub = new (reinterpret_cast<void*>(program->tls.handler_vaddr +
		                                          Jit::TlsRegStub::GetOffset(reg))) Jit::TlsRegStub;
		stub->SetFunc(program->tls.handler_vaddr);
		stub->SetOutputReg(reg);
	}

	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(program->tls.handler_vaddr,
	                                                     Jit::SafeCall::GetSize(),
	                                                     Common::VirtualMemory::Mode::Execute));
	Common::VirtualMemory::FlushInstructionCache(program->tls.handler_vaddr,
	                                             Jit::SafeCall::GetSize());
}

void RuntimeLinker::DeleteTlss(int thread_id) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		DeleteTls(p, thread_id);
	}
}

void RuntimeLinker::SetApplicationHeapApi(void* const api[10]) {
	Common::LockGuard lock(m_mutex);

	if (api == nullptr || api[0] == nullptr || api[1] == nullptr) {
		m_application_heap_malloc         = nullptr;
		m_application_heap_free           = nullptr;
		m_application_heap_posix_memalign = nullptr;
		return;
	}

	m_application_heap_malloc = reinterpret_cast<application_heap_malloc_func_t>(api[0]);
	m_application_heap_free   = reinterpret_cast<application_heap_free_func_t>(api[1]);
	m_application_heap_posix_memalign =
	    reinterpret_cast<application_heap_posix_memalign_func_t>(api[6]);
}

void* RuntimeLinker::ApplicationHeapMemalign(uint64_t alignment, uint64_t size) {
	Common::LockGuard lock(m_mutex);

	if (m_application_heap_posix_memalign != nullptr) {
		void* ptr = nullptr;
		return m_application_heap_posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
	}

	return nullptr;
}

void* RuntimeLinker::ApplicationHeapMalloc(uint64_t size) {
	Common::LockGuard lock(m_mutex);

	return m_application_heap_malloc != nullptr ? m_application_heap_malloc(size) : nullptr;
}

} // namespace Loader
