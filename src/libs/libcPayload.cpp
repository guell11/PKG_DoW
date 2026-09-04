#include "libs/libcPayload.h"

#include "common/common.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/guestPrintf.h"
#include "libs/libs.h"
#include "libs/vaContext.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs {

namespace LibcPayload {

LIB_VERSION("LibcPayload", 1, "LibcPayload", 1, 1);

// PS5 Payload SDK ELFs import libc/libkernel entry points under their plain
// FreeBSD-style names. Every function below is registered under that plain
// name so Loader::SymbolDatabase::FindByName resolves payload imports directly
// instead of letting the runtime linker patch them to silent stubs.

using LibKernel::KernelTimespec;

// FreeBSD x86-64 open(2) flag values accepted by FileSystem::KernelOpen.
constexpr int FB_O_RDONLY    = 0x0000;
constexpr int FB_O_WRONLY    = 0x0001;
constexpr int FB_O_RDWR      = 0x0002;
constexpr int FB_O_APPEND    = 0x0008;
constexpr int FB_O_CREAT     = 0x0200;
constexpr int FB_O_TRUNC     = 0x0400;
constexpr int FB_O_EXCL      = 0x0800;
constexpr int FB_O_DIRECTORY = 0x00020000;

// Sentinel FILE* values for __stdinp/__stdoutp/__stderrp. Guest code only
// passes them back to the stdio wrappers below; they are never dereferenced.
// (kept for the docs reference; the actual objects are the GuestStdioFile
// statics registered above)

// Guest wchar_t is 4 bytes (FreeBSD x86-64); host Windows wchar_t is 2 bytes,
// so all wide-character helpers below operate on uint32_t directly.
using gwchar_t = uint32_t;

struct GuestTm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

static_assert(sizeof(GuestTm) == 36);

// ---------------------------------------------------------------- guest heap

namespace {

constexpr uint64_t kHeapChunkSize = 32u * 1024u * 1024u;
constexpr uint64_t kHeapMaxSize   = 1024ull * 1024u * 1024u;
constexpr uint64_t kHeapAlign     = 16u;
constexpr uint64_t kHeapMinSplit  = 48u;

struct HeapBlock {
	HeapBlock* prev;
	HeapBlock* next;
	uint64_t   size; // payload size in bytes, multiple of kHeapAlign
	uint32_t   used;
	uint32_t   pad;
};

static_assert(sizeof(HeapBlock) == 32);

class GuestHeap {
public:
	KYTY_CLASS_NO_COPY(GuestHeap);

	GuestHeap() = default;

	void* Alloc(uint64_t size) {
		if (size == 0) {
			size = 1;
		}
		if (size > UINT64_MAX - (kHeapAlign - 1u)) {
			*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
			return nullptr;
		}
		const auto need = (size + kHeapAlign - 1u) & ~(kHeapAlign - 1u);

		std::lock_guard lock(m_mutex);

		auto* block = FindFree(need);
		if (block == nullptr && Grow(need + sizeof(HeapBlock))) {
			block = FindFree(need);
		}
		if (block == nullptr) {
			LOGF("LibcPayload: guest heap exhausted (limit = %" PRIu64 " bytes)\n", kHeapMaxSize);
			*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
			return nullptr;
		}
		block->used = 1;
		m_allocated.insert(block + 1);
		return block + 1;
	}

	void Free(void* ptr) {
		if (ptr == nullptr) {
			return;
		}

		std::lock_guard lock(m_mutex);

		if (m_allocated.find(ptr) == m_allocated.end()) {
			LOGF("LibcPayload: free() of unknown pointer 0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(ptr));
			return;
		}

		auto* block = reinterpret_cast<HeapBlock*>(ptr) - 1;
		if (!OwnsBlock(block) || block->used == 0) {
			LOGF("LibcPayload: free() of unknown pointer 0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(ptr));
			return;
		}
		block->used = 0;
		m_allocated.erase(ptr);
		Coalesce(block);
	}

	void* Realloc(void* ptr, uint64_t size) {
		if (ptr == nullptr) {
			return Alloc(size);
		}
		if (size == 0) {
			Free(ptr);
			return nullptr;
		}
		if (size > UINT64_MAX - (kHeapAlign - 1u)) {
			*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
			return nullptr;
		}

		const auto need = (size + kHeapAlign - 1u) & ~(kHeapAlign - 1u);

		std::lock_guard lock(m_mutex);

		if (m_allocated.find(ptr) == m_allocated.end()) {
			LOGF("LibcPayload: realloc() of unknown pointer 0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(ptr));
			return nullptr;
		}

		auto* block = reinterpret_cast<HeapBlock*>(ptr) - 1;
		if (!OwnsBlock(block) || block->used == 0) {
			LOGF("LibcPayload: realloc() of unknown pointer 0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(ptr));
			return nullptr;
		}

		// Grow into the adjacent free block when possible.
		if (block->next != nullptr && !block->next->used && Adjacent(block, block->next) &&
		    block->size + sizeof(HeapBlock) + block->next->size >= need) {
			AbsorbNext(block);
		}

		if (block->size >= need) {
			if (block->size >= need + sizeof(HeapBlock) + kHeapMinSplit) {
				Split(block, need);
			}
			return block + 1;
		}

		// Allocate + copy + free with the lock already held.
		auto* fresh = FindFree(need);
		if (fresh == nullptr && Grow(need + sizeof(HeapBlock))) {
			fresh = FindFree(need);
		}
		if (fresh == nullptr) {
			*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
			return nullptr;
		}
		fresh->used = 1;
		auto* result = fresh + 1;
		std::memcpy(result, block + 1, block->size);
		block->used = 0;
		m_allocated.erase(ptr);
		m_allocated.insert(result);
		Coalesce(block);
		return result;
	}

private:
	HeapBlock* FindFree(uint64_t need) {
		for (auto* b = m_first; b != nullptr; b = b->next) {
			if (b->used || b->size < need) {
				continue;
			}
			if (b->size >= need + sizeof(HeapBlock) + kHeapMinSplit) {
				Split(b, need);
			}
			return b;
		}
		return nullptr;
	}

	[[nodiscard]] bool OwnsBlock(const HeapBlock* block) const {
		const auto addr = reinterpret_cast<uint64_t>(block);
		return std::any_of(m_chunks.begin(), m_chunks.end(), [addr](uint64_t chunk) {
			return addr >= chunk && addr <= chunk + kHeapChunkSize - sizeof(HeapBlock);
		});
	}

	static bool Adjacent(const HeapBlock* a, const HeapBlock* b) {
		return reinterpret_cast<uint64_t>(a) + sizeof(HeapBlock) + a->size ==
		       reinterpret_cast<uint64_t>(b);
	}

	void AbsorbNext(HeapBlock* b) {
		auto* n = b->next;
		if (n == nullptr) {
			return;
		}
		b->size += sizeof(HeapBlock) + n->size;
		b->next  = n->next;
		if (n->next != nullptr) {
			n->next->prev = b;
		} else {
			m_last = b;
		}
	}

	void Coalesce(HeapBlock* b) {
		if (b->next != nullptr && !b->next->used && Adjacent(b, b->next)) {
			AbsorbNext(b);
		}
		if (b->prev != nullptr && !b->prev->used && Adjacent(b->prev, b)) {
			AbsorbNext(b->prev);
		}
	}

	void Split(HeapBlock* b, uint64_t need) {
		auto* rest = reinterpret_cast<HeapBlock*>(reinterpret_cast<uint8_t*>(b + 1) + need);
		rest->prev = b;
		rest->next = b->next;
		rest->size = b->size - need - sizeof(HeapBlock);
		rest->used = 0;
		if (b->next != nullptr) {
			b->next->prev = rest;
		} else {
			m_last = rest;
		}
		b->next = rest;
		b->size = need;
	}

	bool Grow(uint64_t min_free_bytes) {
		if (m_total + kHeapChunkSize > kHeapMaxSize ||
		    kHeapChunkSize < min_free_bytes + sizeof(HeapBlock)) {
			return false;
		}

		const auto vaddr = LibKernel::Memory::AllocateRuntimeMemory(
		    0, kHeapChunkSize, Common::VirtualMemory::Mode::ReadWrite, "LibcPayloadHeap");
		if (vaddr == 0) {
			return false;
		}

		auto* block = reinterpret_cast<HeapBlock*>(vaddr);
		block->prev = m_last;
		block->next = nullptr;
		block->size = kHeapChunkSize - sizeof(HeapBlock);
		block->used = 0;

		if (m_last != nullptr) {
			m_last->next = block;
		} else {
			m_first = block;
		}
		m_last = block;

		m_chunks.push_back(vaddr);
		m_total += kHeapChunkSize;

		LOGF("LibcPayload: guest heap chunk #%d at 0x%016" PRIx64 " (total = %" PRIu64
		     " bytes)\n",
		     static_cast<int>(m_chunks.size()), vaddr, m_total);
		return true;
	}

	std::mutex              m_mutex;
	HeapBlock*              m_first = nullptr;
	HeapBlock*              m_last  = nullptr;
	std::vector<uint64_t>   m_chunks;
	std::unordered_set<void*> m_allocated;
	uint64_t                m_total = 0;
};

GuestHeap& Heap() {
	static GuestHeap heap;
	return heap;
}

char* HeapStrdup(const char* s) {
	if (s == nullptr) {
		return nullptr;
	}
	const auto len  = std::strlen(s);
	auto*      copy = static_cast<char*>(Heap().Alloc(len + 1));
	if (copy != nullptr) {
		std::memcpy(copy, s, len + 1);
	}
	return copy;
}

} // namespace

// ------------------------------------------------------------------ environ

namespace {

std::mutex&         EnvMutex() {
	static std::mutex mutex;
	return mutex;
}
std::vector<char*>& EnvBlock() {
	static std::vector<char*> block;
	return block;
}
std::once_flag&     EnvInitFlag() {
	static std::once_flag flag;
	return flag;
}

void EnvInit() {
	static const char* const defaults[] = {
	    "HOME=/app0",    "PATH=/app0:/bin", "TMPDIR=/app0", "USER=ps5",
	    "SHELL=/bin/sh", "LANG=C",          "LC_ALL=C",     "TZ=UTC",
	    "TERM=xterm",    "PWD=/app0",
	};
	for (const auto* entry: defaults) {
		EnvBlock().push_back(HeapStrdup(entry));
	}
}

void EnvEnsureInit() {
	std::call_once(EnvInitFlag(), EnvInit);
}

} // namespace

// -------------------------------------------------------------------- stdio

namespace {

constexpr size_t kFormatBufferSize = 64u * 1024u;

// Minimal FreeBSD __sFILE prefix: guest inline putc/getc macros dereference
// the FILE* directly (fp->_p at +0, fp->_w at +12, fp->_lbfsize at +0x28), so
// the standard-stream objects must be real readable structs, not sentinels.
struct GuestStdioFile {
	void*   _p;       // +0
	int32_t _r;       // +8
	int32_t _w;       // +12 (0 => every putc overflows to putc/__swbuf)
	int32_t _flags;   // +16
	int32_t _file;    // +20
	int32_t _lbfsize; // +24
	uint8_t _pad[0x38];
};

static GuestStdioFile g_stdin_file  = {};
static GuestStdioFile g_stdout_file = {};
static GuestStdioFile g_stderr_file = {};

struct GuestFile {
	uint32_t magic;
	int32_t  fd;
	int32_t  eof;
	int32_t  error;
	int32_t  ungetc; // one-deep pushback, -1 when empty
	int32_t  append;
};

constexpr uint32_t kGuestFileMagic = 0x46505455u; // 'FPTU'

bool FileIsSentinel(const void* f) {
	return f == &g_stdin_file || f == &g_stdout_file || f == &g_stderr_file;
}

bool FileIsConsole(const void* f) {
	return f == &g_stdout_file || f == &g_stderr_file;
}

int ConsoleWrite(const char* text, size_t len) {
	std::string copy(text, text + len);
	LOGF_COLOR(Log::Color::BrightMagenta, "%s", copy.c_str());
	return static_cast<int>(len);
}

int FileWriteRaw(GuestFile* f, const void* buf, size_t len) {
	if (len == 0) {
		return 0;
	}
	const auto written = LibKernel::FileSystem::KernelWrite(f->fd, buf, len);
	if (written < 0) {
		f->error = 1;
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(written));
		return -1;
	}
	return static_cast<int>(written);
}

} // namespace

namespace {
// Defined further down, next to the time helpers; declared here because the
// *scanf wrappers above are the callers.
int VsscanfCore(const char* input, const char* format, VaList* va);
} // namespace

static KYTY_SYSV_ABI int payload_printf(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return GetGuestVprintfFunc()(format, &ctx.va_list);
}

static KYTY_SYSV_ABI int payload_vprintf(const char* format, VaList* va) {
	return GetGuestVprintfFunc()(format, va);
}

static KYTY_SYSV_ABI int payload_vfprintf(void* stream, const char* format, VaList* va) {
	if (FileIsConsole(stream)) {
		return GetGuestVprintfFunc()(format, va);
	}

	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	static thread_local std::vector<char> buffer;
	buffer.clear();
	buffer.resize(kFormatBufferSize, '\0');

	const auto len = GuestVsnprintfVa(buffer.data(), buffer.size(), format, va);
	if (len <= 0) {
		return len;
	}
	const auto to_write = std::min<size_t>(static_cast<size_t>(len), buffer.size() - 1u);
	return FileWriteRaw(f, buffer.data(), to_write);
}

static KYTY_SYSV_ABI int payload_fprintf(void* stream, VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return payload_vfprintf(stream, format, &ctx.va_list);
}

static KYTY_SYSV_ABI int payload_vsnprintf(char* str, size_t size, const char* format,
                                           VaList* va) {
	return GuestVsnprintfVa(str, size, format, va);
}

static KYTY_SYSV_ABI int payload_sprintf(char* str, VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	// str is a named parameter (guest RDI), so the variadic stream starts at
	// the format (guest RSI).
	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return GuestVsnprintfVa(str, static_cast<size_t>(-1), format, &ctx.va_list);
}

static KYTY_SYSV_ABI int payload_vasprintf(char** strp, const char* format, VaList* va) {
	if (strp == nullptr || format == nullptr) {
		return -1;
	}

	static thread_local std::vector<char> buffer;
	buffer.clear();
	buffer.resize(kFormatBufferSize, '\0');

	const auto len = GuestVsnprintfVa(buffer.data(), buffer.size(), format, va);
	if (len < 0) {
		*strp = nullptr;
		return -1;
	}

	const auto total = std::min<size_t>(static_cast<size_t>(len), buffer.size() - 1u);
	auto*      copy  = static_cast<char*>(Heap().Alloc(total + 1));
	if (copy == nullptr) {
		*strp = nullptr;
		return -1;
	}
	std::memcpy(copy, buffer.data(), total);
	copy[total] = '\0';
	*strp       = copy;
	return static_cast<int>(total);
}

static KYTY_SYSV_ABI int payload_putchar(int c) {
	const char ch = static_cast<char>(c);
	ConsoleWrite(&ch, 1);
	return static_cast<unsigned char>(ch);
}

static KYTY_SYSV_ABI int payload_fputc(int c, void* stream) {
	if (FileIsConsole(stream)) {
		return payload_putchar(c);
	}

	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}
	const char ch = static_cast<char>(c);
	return FileWriteRaw(f, &ch, 1) == 1 ? static_cast<unsigned char>(ch) : -1;
}

static KYTY_SYSV_ABI int payload_fputs(const char* s, void* stream) {
	if (s == nullptr) {
		return -1;
	}
	if (FileIsConsole(stream)) {
		return ConsoleWrite(s, std::strlen(s));
	}

	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}
	const auto len = std::strlen(s);
	return FileWriteRaw(f, s, len) == static_cast<int>(len) ? 0 : -1;
}

static KYTY_SYSV_ABI int payload_putc(int c, void* stream) {
	return payload_fputc(c, stream);
}

static KYTY_SYSV_ABI int payload___swbuf(int c, void* stream) {
	return payload_fputc(c, stream);
}

static KYTY_SYSV_ABI void* payload_fopen(const char* path, const char* mode) {
	if (path == nullptr || mode == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return nullptr;
	}

	int flags  = -1;
	int append = 0;
	for (const char* m = mode; *m != '\0'; m++) {
		switch (*m) {
			case 'r': flags = FB_O_RDONLY; break;
			case 'w': flags = FB_O_WRONLY | FB_O_CREAT | FB_O_TRUNC; break;
			case 'a':
				flags  = FB_O_WRONLY | FB_O_CREAT;
				append = 1;
				break;
			case '+':
				flags = (flags == FB_O_RDONLY ? FB_O_RDWR
				                              : (FB_O_RDWR | FB_O_CREAT | FB_O_TRUNC));
				flags &= ~((append != 0) ? FB_O_TRUNC : 0);
				break;
			case 'b':
			case 'e': break;
			default: *Posix::GetErrorAddr() = Posix::POSIX_EINVAL; return nullptr;
		}
	}
	if (flags == -1) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return nullptr;
	}

	const auto fd = LibKernel::FileSystem::KernelOpen(path, flags, 0666);
	if (fd < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(fd));
		return nullptr;
	}

	auto* f = static_cast<GuestFile*>(Heap().Alloc(sizeof(GuestFile)));
	if (f == nullptr) {
		LibKernel::FileSystem::KernelClose(fd);
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return nullptr;
	}

	f->magic  = kGuestFileMagic;
	f->fd     = fd;
	f->eof    = 0;
	f->error  = 0;
	f->ungetc = -1;
	f->append = append;

	if (append != 0) {
		LibKernel::FileSystem::KernelLseek(fd, 0, 2 /* SEEK_END */);
	}

	return f;
}

static KYTY_SYSV_ABI int payload_fclose(void* stream) {
	if (FileIsSentinel(stream)) {
		// Standard streams are never closed by the guest (FreeBSD ignores it).
		return 0;
	}
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	const auto fd = f->fd;
	f->magic = 0;
	Heap().Free(f);

	const auto result = LibKernel::FileSystem::KernelClose(fd);
	if (result < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	return 0;
}

static KYTY_SYSV_ABI size_t payload_fread(void* ptr, size_t size, size_t nmemb, void* stream) {
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic || ptr == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return 0;
	}

	const auto total = size * nmemb;
	if (total == 0) {
		return 0;
	}

	auto*  out  = static_cast<uint8_t*>(ptr);
	size_t done = 0;

	// Drain the pushback byte first (out stays fixed; reads index by done).
	if (f->ungetc >= 0 && done < total) {
		out[done]  = static_cast<uint8_t>(f->ungetc);
		f->ungetc  = -1;
		done++;
	}

	while (done < total) {
		const auto n = LibKernel::FileSystem::KernelRead(f->fd, out + done, total - done);
		if (n < 0) {
			f->error = 1;
			*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(n));
			break;
		}
		if (n == 0) {
			f->eof = 1;
			break;
		}
		done += static_cast<size_t>(n);
	}

	if (size == 0) {
		return 0;
	}
	return done / size;
}

static KYTY_SYSV_ABI size_t payload_fwrite(const void* ptr, size_t size, size_t nmemb,
                                           void* stream) {
	if (FileIsConsole(stream)) {
		const auto total = size * nmemb;
		ConsoleWrite(static_cast<const char*>(ptr), total);
		return nmemb;
	}

	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic || ptr == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return 0;
	}

	const auto total = size * nmemb;
	if (total == 0) {
		return 0;
	}

	const auto written = FileWriteRaw(f, ptr, total);
	if (written < 0 || size == 0) {
		return 0;
	}
	return static_cast<size_t>(written) / size;
}

static KYTY_SYSV_ABI int payload_fseek(void* stream, int64_t offset, int whence) {
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	const auto result = LibKernel::FileSystem::KernelLseek(f->fd, offset, whence);
	if (result < 0) {
		f->error = 1;
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	f->eof    = 0;
	f->ungetc = -1;
	return 0;
}

static KYTY_SYSV_ABI int64_t payload_ftell(void* stream) {
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	int64_t base = 0;
	if (f->ungetc >= 0) {
		base = -1;
	}

	const auto result = LibKernel::FileSystem::KernelLseek(f->fd, 0, 1 /* SEEK_CUR */);
	if (result < 0) {
		f->error = 1;
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	return result + base;
}

static KYTY_SYSV_ABI int payload_fileno(void* stream) {
	if (FileIsSentinel(stream)) {
		if (stream == &g_stdin_file) {
			return 0;
		}
		return (stream == &g_stdout_file) ? 1 : 2;
	}
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}
	return f->fd;
}

static KYTY_SYSV_ABI int payload_feof(void* stream) {
	auto* f = static_cast<GuestFile*>(stream);
	return (f != nullptr && f->magic == kGuestFileMagic) ? f->eof : 0;
}

static KYTY_SYSV_ABI int payload_ferror(void* stream) {
	auto* f = static_cast<GuestFile*>(stream);
	return (f != nullptr && f->magic == kGuestFileMagic) ? f->error : 0;
}

static KYTY_SYSV_ABI int payload_fgetc(void* stream) {
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	if (f->ungetc >= 0) {
		const auto c = f->ungetc;
		f->ungetc    = -1;
		return c;
	}

	uint8_t    byte = 0;
	const auto n    = LibKernel::FileSystem::KernelRead(f->fd, &byte, 1);
	if (n < 0) {
		f->error = 1;
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(n));
		return -1;
	}
	if (n == 0) {
		f->eof = 1;
		return -1; // EOF
	}
	return byte;
}

static KYTY_SYSV_ABI int payload_ungetc(int c, void* stream) {
	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic || c == -1) {
		return -1;
	}
	if (f->ungetc >= 0) {
		return f->ungetc;
	}
	f->ungetc = static_cast<unsigned char>(c);
	f->eof    = 0;
	return static_cast<unsigned char>(c);
}

static KYTY_SYSV_ABI char* payload_fgets(char* s, int size, void* stream) {
	if (s == nullptr || size <= 0) {
		return nullptr;
	}

	auto* pos  = s;
	int   left = size - 1;
	while (left > 0) {
		const auto c = payload_fgetc(stream);
		if (c == -1) {
			break;
		}
		*pos++ = static_cast<char>(c);
		left--;
		if (c == '\n') {
			break;
		}
	}

	if (pos == s) {
		return nullptr;
	}
	*pos = '\0';
	return s;
}

static KYTY_SYSV_ABI int payload_fscanf(void* stream, VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	auto* f = static_cast<GuestFile*>(stream);
	if (f == nullptr || f->magic != kGuestFileMagic) {
		return -1;
	}

	// Reads the remaining stream into memory and scans it there; the file
	// position ends up past everything read. Payload config files are small.
	static thread_local std::vector<char> buffer;
	buffer.clear();

	bool eof = false;
	for (;;) {
		uint8_t    byte = 0;
		const auto n    = LibKernel::FileSystem::KernelRead(f->fd, &byte, 1);
		if (n <= 0) {
			eof = true;
			// Mark the guest FILE as end-of-file too: config parsers that
			// loop on feof() stop only when the flag sticks after fscanf.
			f->eof = 1;
			break;
		}
		buffer.push_back(static_cast<char>(byte));
		if (buffer.size() >= kFormatBufferSize) {
			break;
		}
	}
	buffer.push_back('\0');

	// The retail fscanf reports EOF (not "zero matches") once the stream is
	// exhausted; Doom's config parser loops until it sees exactly that, so
	// returning 0 here would spin forever on an empty remainder.
	if (eof && buffer.size() == 1) {
		return -1;
	}

	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return VsscanfCore(buffer.data(), format, &ctx.va_list);
}

static KYTY_SYSV_ABI int payload_sscanf(const char* str, VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	return VsscanfCore(str, format, &ctx.va_list);
}

static KYTY_SYSV_ABI int payload_vsscanf(const char* str, const char* format, VaList* va) {
	return VsscanfCore(str, format, va);
}

static KYTY_SYSV_ABI int payload_remove(const char* path) {
	const auto result = LibKernel::FileSystem::KernelUnlink(path);
	if (result < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	return 0;
}

static KYTY_SYSV_ABI int payload_rename(const char* from, const char* to) {
	const auto result = LibKernel::FileSystem::KernelRename(from, to);
	if (result < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	return 0;
}

static KYTY_SYSV_ABI void payload_perror(const char* prefix) {
	const int err  = *Posix::GetErrorAddr();
	const auto text = std::strerror(err);
	if (prefix != nullptr && prefix[0] != '\0') {
		LOGF_COLOR(Log::Color::BrightMagenta, "%s: %s\n", prefix, text);
	} else {
		LOGF_COLOR(Log::Color::BrightMagenta, "%s\n", text);
	}
}

// ------------------------------------------------------------------- memory

static KYTY_SYSV_ABI void* payload_malloc(size_t size) {
	return Heap().Alloc(size);
}

static KYTY_SYSV_ABI void payload_free(void* ptr) {
	Heap().Free(ptr);
}

static KYTY_SYSV_ABI void* payload_calloc(size_t nmemb, size_t size) {
	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return nullptr;
	}
	const auto total = nmemb * size;
	auto*      ptr   = Heap().Alloc(total);
	if (ptr != nullptr) {
		std::memset(ptr, 0, total);
	}
	return ptr;
}

static KYTY_SYSV_ABI void* payload_realloc(void* ptr, size_t size) {
	return Heap().Realloc(ptr, size);
}

static KYTY_SYSV_ABI char* payload_strdup(const char* s) {
	return HeapStrdup(s);
}

// ------------------------------------------------------------------ environ

static KYTY_SYSV_ABI char* payload_getenv(const char* name) {
	if (name == nullptr || name[0] == '\0') {
		return nullptr;
	}

	EnvEnsureInit();

	std::lock_guard lock(EnvMutex());

	const auto len = std::strlen(name);
	for (const auto* entry: EnvBlock()) {
		if (std::strncmp(entry, name, len) == 0 && entry[len] == '=') {
			return const_cast<char*>(entry + len + 1);
		}
	}
	return nullptr;
}

static KYTY_SYSV_ABI int payload_setenv_impl(const char* name, const char* value, int overwrite) {
	if (name == nullptr || value == nullptr || name[0] == '\0' ||
	    std::strchr(name, '=') != nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	EnvEnsureInit();

	std::lock_guard lock(EnvMutex());

	char* existing = nullptr;
	const auto len = std::strlen(name);
	for (auto* entry: EnvBlock()) {
		if (std::strncmp(entry, name, len) == 0 && entry[len] == '=') {
			existing = entry;
			break;
		}
	}

	if (existing != nullptr && overwrite == 0) {
		return 0;
	}

	const std::string composed = std::string(name) + "=" + value;
	auto*             copy     = HeapStrdup(composed.c_str());
	if (copy == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return -1;
	}

	if (existing != nullptr) {
		EnvBlock().erase(std::remove(EnvBlock().begin(), EnvBlock().end(), existing),
		                 EnvBlock().end());
	}
	EnvBlock().push_back(copy);
	return 0;
}

static KYTY_SYSV_ABI int payload_putenv(char* string) {
	if (string == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	const auto* eq = std::strchr(string, '=');
	if (eq == nullptr || eq == string) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	const std::string name(string, eq - string);
	return payload_setenv_impl(name.c_str(), eq + 1, 1);
}

// -------------------------------------------------------------------- dirent

namespace {

// FreeBSD x86-64 struct dirent layout, matching KernelGetdirentries output.
struct GuestDirent {
	uint32_t d_fileno;
	uint16_t d_reclen;
	uint8_t  d_type;
	uint8_t  d_namlen;
	char     d_name[256];
};

static_assert(sizeof(GuestDirent) == 264);

constexpr uint64_t kDirentBufferSize = 16u * 1024u;

struct GuestDir {
	uint32_t magic;
	int32_t  fd;
	uint8_t* buffer;
	uint64_t valid;
	uint64_t pos;
};

constexpr uint32_t kGuestDirMagic = 0x44524952u; // 'DRIR'

} // namespace

static KYTY_SYSV_ABI void* payload_opendir(const char* path) {
	if (path == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return nullptr;
	}

	const auto fd = LibKernel::FileSystem::KernelOpen(path, FB_O_RDONLY | FB_O_DIRECTORY, 0777);
	if (fd < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(fd));
		return nullptr;
	}

	auto* buffer = static_cast<uint8_t*>(Heap().Alloc(kDirentBufferSize));
	auto* dir    = static_cast<GuestDir*>(Heap().Alloc(sizeof(GuestDir)));
	if (buffer == nullptr || dir == nullptr) {
		Heap().Free(buffer);
		Heap().Free(dir);
		LibKernel::FileSystem::KernelClose(fd);
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return nullptr;
	}

	dir->magic  = kGuestDirMagic;
	dir->fd     = fd;
	dir->buffer = buffer;
	dir->valid  = 0;
	dir->pos    = 0;

	return dir;
}

static KYTY_SYSV_ABI GuestDirent* payload_readdir(void* dirp) {
	auto* dir = static_cast<GuestDir*>(dirp);
	if (dir == nullptr || dir->magic != kGuestDirMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return nullptr;
	}

	if (dir->pos >= dir->valid) {
		const auto n = LibKernel::FileSystem::KernelGetdirentries(
		    dir->fd, reinterpret_cast<char*>(dir->buffer), static_cast<int>(kDirentBufferSize),
		    nullptr);
		if (n <= 0) {
			if (n < 0) {
				*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(n));
			}
			return nullptr;
		}
		dir->valid = static_cast<uint64_t>(n);
		dir->pos   = 0;
	}

	auto* entry = reinterpret_cast<GuestDirent*>(dir->buffer + dir->pos);
	dir->pos += entry->d_reclen;
	return entry;
}

static KYTY_SYSV_ABI int payload_closedir(void* dirp) {
	auto* dir = static_cast<GuestDir*>(dirp);
	if (dir == nullptr || dir->magic != kGuestDirMagic) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	const auto fd = dir->fd;
	Heap().Free(dir->buffer);
	dir->magic = 0;
	Heap().Free(dir);

	const auto result = LibKernel::FileSystem::KernelClose(fd);
	if (result < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	return 0;
}

// -------------------------------------------------------------- ctype / str

#define CTYPE_FN(name, expr)                                                                        \
	static KYTY_SYSV_ABI int payload_##name(int c) {                                                \
		return (expr);                                                                              \
	}

CTYPE_FN(isalnum, std::isalnum(static_cast<unsigned char>(c)))
CTYPE_FN(isalpha, std::isalpha(static_cast<unsigned char>(c)))
CTYPE_FN(isblank, std::isblank(static_cast<unsigned char>(c)))
CTYPE_FN(iscntrl, std::iscntrl(static_cast<unsigned char>(c)))
CTYPE_FN(isgraph, std::isgraph(static_cast<unsigned char>(c)))
CTYPE_FN(islower, std::islower(static_cast<unsigned char>(c)))
CTYPE_FN(isprint, std::isprint(static_cast<unsigned char>(c)))
CTYPE_FN(ispunct, std::ispunct(static_cast<unsigned char>(c)))
CTYPE_FN(isspace, std::isspace(static_cast<unsigned char>(c)))
CTYPE_FN(isupper, std::isupper(static_cast<unsigned char>(c)))
CTYPE_FN(isxdigit, std::isxdigit(static_cast<unsigned char>(c)))
CTYPE_FN(toupper, std::toupper(static_cast<unsigned char>(c)))
CTYPE_FN(tolower, std::tolower(static_cast<unsigned char>(c)))

#undef CTYPE_FN

static KYTY_SYSV_ABI void* payload_memchr(const void* s, int c, size_t n) {
	return const_cast<void*>(::memchr(s, c, n));
}

static KYTY_SYSV_ABI double payload_atof(const char* str) {
	return ::atof(str);
}

static KYTY_SYSV_ABI double payload_strtod(const char* str, char** endptr) {
	return ::strtod(str, endptr);
}

static KYTY_SYSV_ABI float payload_strtof(const char* str, char** endptr) {
	return std::strtof(str, endptr);
}

// Guest long double is 80-bit; converted through double, which only loses
// precision on the rare %Lf/strtold paths.
static KYTY_SYSV_ABI double payload_strtold(const char* str, char** endptr) {
	return ::strtod(str, endptr);
}

static KYTY_SYSV_ABI int64_t payload_strtoll(const char* str, char** endptr, int base) {
	return ::strtoll(str, endptr, base);
}

static KYTY_SYSV_ABI uint64_t payload_strtoull(const char* str, char** endptr, int base) {
	return ::strtoull(str, endptr, base);
}

static KYTY_SYSV_ABI char* payload_strerror(int errnum) {
	return ::strerror(errnum);
}

static KYTY_SYSV_ABI int payload_strcoll(const char* a, const char* b) {
	return ::strcoll(a, b);
}

static KYTY_SYSV_ABI size_t payload_strxfrm(char* dst, const char* src, size_t n) {
	return ::strxfrm(dst, src, n);
}

static KYTY_SYSV_ABI size_t payload_strlcpy(char* dst, const char* src, size_t siz) {
	const auto len = std::strlen(src);
	if (siz != 0) {
		const auto copy = std::min(len, siz - 1u);
		std::memcpy(dst, src, copy);
		dst[copy] = '\0';
	}
	return len;
}

static KYTY_SYSV_ABI size_t payload_strnlen(const char* s, size_t maxlen) {
	size_t len = 0;
	while (len < maxlen && s[len] != '\0') {
		len++;
	}
	return len;
}

static KYTY_SYSV_ABI size_t payload_strlcat(char* dst, const char* src, size_t siz) {
	const auto dst_len = payload_strnlen(dst, siz);
	if (dst_len == siz) {
		return siz + std::strlen(src);
	}
	return dst_len + payload_strlcpy(dst + dst_len, src, siz - dst_len);
}

static KYTY_SYSV_ABI int payload_strncasecmp(const char* a, const char* b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		const auto ca = std::tolower(static_cast<unsigned char>(a[i]));
		const auto cb = std::tolower(static_cast<unsigned char>(b[i]));
		if (ca != cb || ca == 0) {
			return ca - cb;
		}
	}
	return 0;
}

static KYTY_SYSV_ABI char* payload_strtok_r(char* str, const char* delim, char** saveptr) {
	if (delim == nullptr || saveptr == nullptr) {
		return nullptr;
	}

	char* s = (str != nullptr) ? str : *saveptr;
	if (s == nullptr) {
		return nullptr;
	}

	s += std::strspn(s, delim);
	if (*s == '\0') {
		*saveptr = s;
		return nullptr;
	}

	auto* token = s;
	s           = std::strpbrk(token, delim);
	if (s == nullptr) {
		*saveptr = token + std::strlen(token);
	} else {
		*s       = '\0';
		*saveptr = s + 1;
	}
	return token;
}

static KYTY_SYSV_ABI char* payload_strtok(char* str, const char* delim) {
	thread_local char* save_ptr = nullptr;
	return payload_strtok_r(str, delim, &save_ptr);
}

// ---------------------------------------------------------------- wide chars

static KYTY_SYSV_ABI size_t payload_wcslen(const gwchar_t* s) {
	size_t len = 0;
	while (s != nullptr && s[len] != 0) {
		len++;
	}
	return len;
}

static KYTY_SYSV_ABI int payload_wcscmp(const gwchar_t* a, const gwchar_t* b) {
	if (a == nullptr || b == nullptr) {
		return (a == b) ? 0 : ((a == nullptr) ? -1 : 1);
	}
	while (*a != 0 && *a == *b) {
		a++;
		b++;
	}
	return static_cast<int>(*a) - static_cast<int>(*b);
}

static KYTY_SYSV_ABI int payload_wcsncmp(const gwchar_t* a, const gwchar_t* b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i] || a[i] == 0) {
			return static_cast<int>(a[i]) - static_cast<int>(b[i]);
		}
	}
	return 0;
}

static KYTY_SYSV_ABI const gwchar_t* payload_wcsstr(const gwchar_t* haystack,
                                                    const gwchar_t* needle) {
	if (haystack == nullptr || needle == nullptr) {
		return nullptr;
	}
	if (*needle == 0) {
		return haystack;
	}
	for (const auto* h = haystack; *h != 0; h++) {
		const auto* n = needle;
		const auto* p = h;
		while (*n != 0 && *p == *n) {
			n++;
			p++;
		}
		if (*n == 0) {
			return h;
		}
	}
	return nullptr;
}

static KYTY_SYSV_ABI int payload_wcscoll(const gwchar_t* a, const gwchar_t* b) {
	return payload_wcscmp(a, b);
}

static KYTY_SYSV_ABI size_t payload_wcsxfrm(gwchar_t* dst, const gwchar_t* src, size_t n) {
	const auto len = payload_wcslen(src);
	if (dst != nullptr && n != 0) {
		const auto copy = std::min(len, n - 1u);
		std::memcpy(dst, src, copy * sizeof(gwchar_t));
		dst[copy] = 0;
	}
	return len;
}

static KYTY_SYSV_ABI size_t payload_wcstombs(char* dst, const gwchar_t* src, size_t len) {
	size_t n = 0;
	while (src != nullptr && src[n] != 0) {
		if (n >= len) {
			return static_cast<size_t>(-1);
		}
		const auto wc = src[n];
		// C locale: only Latin-1 converts cleanly.
		dst[n] = static_cast<char>(wc > 0xffu ? '?' : wc);
		n++;
	}
	return n;
}

static KYTY_SYSV_ABI size_t payload_mbstowcs(gwchar_t* dst, const char* src, size_t len) {
	size_t n = 0;
	while (src != nullptr && src[n] != '\0') {
		if (n >= len) {
			return static_cast<size_t>(-1);
		}
		dst[n] = static_cast<unsigned char>(src[n]);
		n++;
	}
	if (n < len && dst != nullptr) {
		dst[n] = 0;
	}
	return n;
}

static KYTY_SYSV_ABI size_t payload_mbrtowc(gwchar_t* pwc, const char* s, size_t n, void*) {
	if (s == nullptr) {
		return 0;
	}
	if (n == 0) {
		return static_cast<size_t>(-2);
	}
	if (pwc != nullptr) {
		*pwc = static_cast<unsigned char>(*s);
	}
	return (*s == '\0') ? 0 : 1;
}

static KYTY_SYSV_ABI size_t payload_wcrtomb(char* s, gwchar_t wc, void*) {
	if (s == nullptr) {
		return 1;
	}
	if (wc > 0xffu) {
		*Posix::GetErrorAddr() = Posix::POSIX_EILSEQ;
		return static_cast<size_t>(-1);
	}
	*s = static_cast<char>(wc);
	return 1;
}

static KYTY_SYSV_ABI size_t payload_mbrlen(const char* s, size_t n, void*) {
	if (s == nullptr || n == 0) {
		return static_cast<size_t>(-2);
	}
	return (*s == '\0') ? 0 : 1;
}

static KYTY_SYSV_ABI int payload_mbtowc(gwchar_t* pwc, const char* s, size_t n) {
	if (s == nullptr) {
		return 0;
	}
	if (n == 0 || *s == '\0') {
		return 0;
	}
	if (pwc != nullptr) {
		*pwc = static_cast<unsigned char>(*s);
	}
	return 1;
}

static KYTY_SYSV_ABI size_t payload_mbsrtowcs(gwchar_t* dst, const char** src, size_t len,
                                              void*) {
	if (src == nullptr || *src == nullptr) {
		return 0;
	}

	const auto* s    = *src;
	size_t      n    = 0;
	bool        full = (dst == nullptr);

	while (s[n] != '\0') {
		if (dst != nullptr && n == len) {
			full = false;
			break;
		}
		if (dst != nullptr) {
			dst[n] = static_cast<unsigned char>(s[n]);
		}
		n++;
	}

	*src = full ? nullptr : s + n;
	return n;
}

// -------------------------------------------------------------- sort / search

using guest_cmp_t = KYTY_SYSV_ABI int (*)(const void*, const void*);

static void PayloadSwapBytes(uint8_t* a, uint8_t* b, size_t width) {
	while (width-- != 0) {
		const auto t = *a;
		*a++         = *b;
		*b++         = t;
	}
}

// Host qsort cannot be reused: it would call the guest comparator with the
// Windows x64 convention instead of the guest SysV one.
static void PayloadQsort(uint8_t* base, size_t num, size_t width, guest_cmp_t cmp) {
	while (num > 12) {
		const size_t mid   = num / 2;
		const size_t left  = 1;
		const size_t right = num - 1;

		// Median-of-three pivot selection.
		if (cmp(base + mid * width, base + left * width) < 0) {
			PayloadSwapBytes(base + mid * width, base + left * width, width);
		}
		if (cmp(base + right * width, base + mid * width) < 0) {
			PayloadSwapBytes(base + right * width, base + mid * width, width);
			if (cmp(base + mid * width, base + left * width) < 0) {
				PayloadSwapBytes(base + mid * width, base + left * width, width);
			}
		}

		size_t                                 j    = right;
		static thread_local std::vector<uint8_t> pivot;
		pivot.assign(base + mid * width, base + (mid + 1) * width);

		size_t i = left;
		for (;;) {
			while (i < num && cmp(base + i * width, pivot.data()) < 0) {
				i++;
			}
			while (j > 0 && cmp(base + j * width, pivot.data()) > 0) {
				j--;
			}
			if (i >= j) {
				break;
			}
			PayloadSwapBytes(base + i * width, base + j * width, width);
			i++;
			j--;
		}

		// Recurse into the smaller side, iterate on the larger one.
		if (j + 1 < num - (j + 1)) {
			PayloadQsort(base, j + 1, width, cmp);
			base += (j + 1) * width;
			num  -= (j + 1);
		} else {
			PayloadQsort(base + (j + 1) * width, num - (j + 1), width, cmp);
			num = j + 1;
		}
	}

	// Insertion sort for small ranges.
	static thread_local std::vector<uint8_t> key;
	for (size_t i = 1; i < num; i++) {
		key.assign(base + i * width, base + (i + 1) * width);
		size_t j = i;
		while (j > 0 && cmp(base + (j - 1) * width, key.data()) > 0) {
			std::memcpy(base + j * width, base + (j - 1) * width, width);
			j--;
		}
		std::memcpy(base + j * width, key.data(), width);
	}
}

static KYTY_SYSV_ABI void payload_qsort(void* base, size_t nmemb, size_t size, guest_cmp_t cmp) {
	if (base == nullptr || cmp == nullptr || size == 0 || nmemb < 2) {
		return;
	}
	PayloadQsort(static_cast<uint8_t*>(base), nmemb, size, cmp);
}

static KYTY_SYSV_ABI void* payload_bsearch(const void* key, const void* base, size_t nmemb,
                                           size_t size, guest_cmp_t cmp) {
	if (key == nullptr || base == nullptr || cmp == nullptr) {
		return nullptr;
	}

	const auto* bytes = static_cast<const uint8_t*>(base);
	size_t      lo    = 0;
	size_t      hi    = nmemb;
	while (lo < hi) {
		const auto mid   = lo + (hi - lo) / 2;
		const auto* item = bytes + mid * size;
		const auto r     = cmp(key, item);
		if (r == 0) {
			return const_cast<void*>(static_cast<const void*>(item));
		}
		if (r < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return nullptr;
}

// ------------------------------------------------------------- time / locale

static KYTY_SYSV_ABI int64_t payload_time(int64_t* timer) {
	const auto now = static_cast<int64_t>(std::time(nullptr));
	if (timer != nullptr) {
		*timer = now;
	}
	return now;
}

static GuestTm ToGuestTm(const std::tm& time) {
	return {time.tm_sec,  time.tm_min,  time.tm_hour, time.tm_mday, time.tm_mon,
	        time.tm_year, time.tm_wday, time.tm_yday, time.tm_isdst};
}

// ------------------------------------------------------------------- scanf

namespace {

// Minimal scanf engine: parses one conversion at a time with the host
// sscanf, pulling destination pointers out of the guest va_list.
int VsscanfCore(const char* input, const char* format, VaList* va) {
	if (input == nullptr || format == nullptr || va == nullptr) {
		return -1;
	}

	int        count   = 0;
	const char* in     = input;
	bool       matching = true;

	for (const char* f = format; *f != '\0' && matching; f++) {
		if (std::isspace(static_cast<unsigned char>(*f)) != 0) {
			while (std::isspace(static_cast<unsigned char>(*in)) != 0) {
				in++;
			}
			continue;
		}
		if (*f != '%') {
			if (*in == *f) {
				in++;
			} else {
				matching = false;
			}
			continue;
		}

		f++; // skip '%'
		if (*f == '\0') {
			break;
		}
		if (*f == '%') {
			if (*in == '%') {
				in++;
			} else {
				matching = false;
			}
			continue;
		}

		bool suppress = false;
		if (*f == '*') {
			suppress = true;
			f++;
		}

		int width = -1;
		if (*f >= '0' && *f <= '9') {
			width = 0;
			while (*f >= '0' && *f <= '9') {
				width = width * 10 + (*f - '0');
				f++;
			}
		}

		char length = 0; // 0, 'h', 'H' (hh), 'l', 'L' (ll / size_t)
		if (*f == 'h') {
			length = 'h';
			f++;
			if (*f == 'h') {
				length = 'H';
				f++;
			}
		} else if (*f == 'l') {
			length = 'l';
			f++;
			if (*f == 'l') {
				length = 'L';
				f++;
			}
		} else if (*f == 'z' || *f == 'j' || *f == 't') {
			length = 'L';
			f++;
		}

		const char conv = *f;
		if (conv == '\0') {
			break;
		}

		if (conv == 'n') {
			if (!suppress) {
				*VaArg_ptr<int32_t>(va) = static_cast<int32_t>(in - input);
			}
			continue;
		}

		if (conv == 'c') {
			const auto n = (width > 0) ? width : 1;
			if (!suppress) {
				std::memcpy(VaArg_ptr<char>(va), in, n);
			}
			in += n;
			if (!suppress) {
				count++;
			}
			continue;
		}

		if (conv == 's') {
			while (std::isspace(static_cast<unsigned char>(*in)) != 0) {
				in++;
			}
			const auto* start = in;
			while (*in != '\0' && std::isspace(static_cast<unsigned char>(*in)) == 0 &&
			       (width < 0 || (in - start) < width)) {
				in++;
			}
			if (in == start) {
				matching = false;
				break;
			}
			if (!suppress) {
				auto* dst = VaArg_ptr<char>(va);
				std::memcpy(dst, start, in - start);
				dst[in - start] = '\0';
				count++;
			}
			continue;
		}

		if (conv == '[') {
			f++;
			bool negate = false;
			if (*f == '^') {
				negate = true;
				f++;
			}
			bool set[256] = {false};
			if (*f == ']') {
				set[static_cast<unsigned char>(']')] = true;
				f++;
			}
			while (*f != '\0' && *f != ']') {
				unsigned c0 = static_cast<unsigned char>(*f);
				f++;
				if (*f == '-' && f[1] != '\0' && f[1] != ']') {
					const unsigned c1 = static_cast<unsigned char>(f[1]);
					f += 2;
					for (unsigned c = c0; c <= c1; c++) {
						set[c] = true;
					}
				} else {
					set[c0] = true;
				}
			}

			const auto* start = in;
			while (*in != '\0' && (width < 0 || (in - start) < width)) {
				const bool m = set[static_cast<unsigned char>(*in)];
				if (m == negate) {
					break;
				}
				in++;
			}
			if (in == start) {
				matching = false;
				break;
			}
			if (!suppress) {
				auto* dst = VaArg_ptr<char>(va);
				std::memcpy(dst, start, in - start);
				dst[in - start] = '\0';
				count++;
			}
			continue;
		}

		if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' ||
		    conv == 'o') {
			std::string spec = "%";
			if (width > 0) {
				spec += std::to_string(width);
			}
			spec += "ll";
			spec += conv;
			spec += "%n";

			int64_t value    = 0;
			int     consumed = 0;
			if (std::sscanf(in, spec.c_str(), &value, &consumed) != 1 || consumed == 0) {
				matching = false;
				break;
			}
			in += consumed;
			if (!suppress) {
				switch (length) {
					case 'H': *VaArg_ptr<int8_t>(va) = static_cast<int8_t>(value); break;
					case 'h': *VaArg_ptr<int16_t>(va) = static_cast<int16_t>(value); break;
					case 'l':
					case 'L': *VaArg_ptr<int64_t>(va) = value; break;
					default: *VaArg_ptr<int32_t>(va) = static_cast<int32_t>(value); break;
				}
				count++;
			}
			continue;
		}

		if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
			std::string spec = "%";
			if (width > 0) {
				spec += std::to_string(width);
			}
			spec += "lf";
			spec += "%n";

			double  value    = 0.0;
			int     consumed = 0;
			if (std::sscanf(in, spec.c_str(), &value, &consumed) != 1 || consumed == 0) {
				matching = false;
				break;
			}
			in += consumed;
			if (!suppress) {
				// Guest 80-bit long double is approximated with double.
				*VaArg_ptr<double>(va) = value;
				count++;
			}
			continue;
		}

		// Unknown conversion: treat as mismatch.
		matching = false;
	}

	return count;
}

} // namespace

static KYTY_SYSV_ABI GuestTm* payload_localtime(const int64_t* timer) {
	if (timer == nullptr) {
		return nullptr;
	}

	thread_local GuestTm result {};
	std::tm              host_result {};
	const auto           t = static_cast<std::time_t>(*timer);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (_localtime64_s(&host_result, &t) != 0) {
		return nullptr;
	}
#else
	if (localtime_r(&t, &host_result) == nullptr) {
		return nullptr;
	}
#endif

	result = ToGuestTm(host_result);
	return &result;
}

static KYTY_SYSV_ABI size_t payload_strftime(char* str, size_t count, const char* format,
                                             const GuestTm* timeptr) {
	if (str == nullptr || format == nullptr || timeptr == nullptr) {
		return 0;
	}

	std::tm host_time {};
	host_time.tm_sec   = timeptr->tm_sec;
	host_time.tm_min   = timeptr->tm_min;
	host_time.tm_hour  = timeptr->tm_hour;
	host_time.tm_mday  = timeptr->tm_mday;
	host_time.tm_mon   = timeptr->tm_mon;
	host_time.tm_year  = timeptr->tm_year;
	host_time.tm_wday  = timeptr->tm_wday;
	host_time.tm_yday  = timeptr->tm_yday;
	host_time.tm_isdst = timeptr->tm_isdst;

	return std::strftime(str, count, format, &host_time);
}

static KYTY_SYSV_ABI char* payload_setlocale(int, const char*) {
	return const_cast<char*>("C");
}

static KYTY_SYSV_ABI int payload_rand() {
	return ::rand();
}

static KYTY_SYSV_ABI void payload_srand(unsigned int seed) {
	::srand(seed);
}

static KYTY_SYSV_ABI long payload_random() {
	return static_cast<long>(::rand());
}

static KYTY_SYSV_ABI void payload_srandom(unsigned int seed) {
	::srand(seed);
}

static KYTY_SYSV_ABI unsigned int payload_sleep(unsigned int seconds) {
	std::this_thread::sleep_for(std::chrono::seconds(seconds));
	return 0;
}

static KYTY_SYSV_ABI int payload_usleep(uint32_t usec) {
	std::this_thread::sleep_for(std::chrono::microseconds(usec));
	return 0;
}

static KYTY_SYSV_ABI char* payload_getcwd(char* buf, size_t size) {
	static const char cwd[] = "/app0";
	if (buf == nullptr || size < sizeof(cwd)) {
		*Posix::GetErrorAddr() = Posix::POSIX_ERANGE;
		return nullptr;
	}
	std::memcpy(buf, cwd, sizeof(cwd));
	return buf;
}

// ---------------------------------------------------------- sysconf / sysctl

// FreeBSD unistd.h keys used by the payload SDK / SDL.
constexpr int SC_ARG_MAX          = 1;
constexpr int SC_CHILD_MAX        = 2;
constexpr int SC_CLK_TCK          = 3;
constexpr int SC_OPEN_MAX         = 5;
constexpr int SC_PAGESIZE         = 47;
constexpr int SC_NPROCESSORS_CONF = 57;
constexpr int SC_NPROCESSORS_ONLN = 58;

static KYTY_SYSV_ABI int64_t payload_sysconf(int name) {
	switch (name) {
		case SC_ARG_MAX: return 1ll * 1024 * 1024;
		case SC_CHILD_MAX: return 64;
		case SC_CLK_TCK: return 100;
		case SC_OPEN_MAX: return 1024;
		case SC_PAGESIZE: return 0x4000; // matches Posix::getpagesize()
		case SC_NPROCESSORS_CONF:
		case SC_NPROCESSORS_ONLN: return std::max(1u, std::thread::hardware_concurrency());
		default: break;
	}

	static std::atomic_uint32_t log_count = 0;
	if (log_count.fetch_add(1) < 8) {
		LOGF("LibcPayload: sysconf(%d) unsupported, returning -1\n", name);
	}
	*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
	return -1;
}

namespace {

int SysctlFill(const void* oldp, size_t* oldlenp, const void* data, size_t data_size) {
	if (oldlenp == nullptr) {
		return 0;
	}
	if (oldp == nullptr) {
		*oldlenp = data_size;
		return 0;
	}
	if (*oldlenp < data_size) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return -1;
	}
	std::memcpy(const_cast<void*>(oldp), data, data_size);
	*oldlenp = data_size;
	return 0;
}

int SysctlGetByName(const char* name, void* oldp, size_t* oldlenp) {
	if (std::strcmp(name, "hw.ncpu") == 0) {
		const auto ncpu = std::max(1u, std::thread::hardware_concurrency());
		return SysctlFill(oldp, oldlenp, &ncpu, sizeof(ncpu));
	}
	if (std::strcmp(name, "hw.physmem") == 0 || std::strcmp(name, "hw.usermem") == 0 ||
	    std::strcmp(name, "hw.realmem") == 0) {
		const uint64_t mem = 8ull * 1024 * 1024 * 1024;
		return SysctlFill(oldp, oldlenp, &mem, sizeof(mem));
	}
	if (std::strcmp(name, "hw.machine") == 0 || std::strcmp(name, "hw.machine_arch") == 0) {
		static const char machine[] = "amd64";
		return SysctlFill(oldp, oldlenp, machine, sizeof(machine));
	}
	if (std::strcmp(name, "kern.ostype") == 0) {
		static const char ostype[] = "FreeBSD";
		return SysctlFill(oldp, oldlenp, ostype, sizeof(ostype));
	}
	if (std::strcmp(name, "kern.osrelease") == 0) {
		static const char osrelease[] = "13.0-RELEASE";
		return SysctlFill(oldp, oldlenp, osrelease, sizeof(osrelease));
	}
	if (std::strcmp(name, "kern.osreldate") == 0) {
		const int reldate = 1300138;
		return SysctlFill(oldp, oldlenp, &reldate, sizeof(reldate));
	}
	if (std::strcmp(name, "kern.hostname") == 0) {
		static const char hostname[] = "ps5";
		return SysctlFill(oldp, oldlenp, hostname, sizeof(hostname));
	}

	static std::atomic_uint32_t log_count = 0;
	if (log_count.fetch_add(1) < 16) {
		LOGF("LibcPayload: sysctl(\"%s\") unsupported, returning ENOENT\n", name);
	}
	*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
	return -1;
}

} // namespace

static KYTY_SYSV_ABI int payload_sysctlbyname(const char* name, void* oldp, size_t* oldlenp,
                                              const void*, size_t) {
	if (name == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	return SysctlGetByName(name, oldp, oldlenp);
}

static KYTY_SYSV_ABI int payload_sysctl(const uint32_t* mib, uint32_t namelen, void* oldp,
                                        size_t* oldlenp, const void*, size_t) {
	if (mib == nullptr || namelen != 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	constexpr uint32_t CTL_KERN        = 1;
	constexpr uint32_t CTL_HW          = 6;
	constexpr uint32_t KERN_OSTYPE     = 1;
	constexpr uint32_t KERN_OSRELEASE  = 2;
	constexpr uint32_t KERN_OSRELDATE  = 24;
	constexpr uint32_t HW_MACHINE      = 1;
	constexpr uint32_t HW_NCPU         = 3;
	constexpr uint32_t HW_PHYSMEM      = 5;
	constexpr uint32_t HW_USERMEM      = 6;
	constexpr uint32_t HW_MACHINE_ARCH = 11;

	switch (mib[0]) {
		case CTL_KERN:
			switch (mib[1]) {
				case KERN_OSTYPE: return SysctlGetByName("kern.ostype", oldp, oldlenp);
				case KERN_OSRELEASE: return SysctlGetByName("kern.osrelease", oldp, oldlenp);
				case KERN_OSRELDATE: return SysctlGetByName("kern.osreldate", oldp, oldlenp);
				default: break;
			}
			break;
		case CTL_HW:
			switch (mib[1]) {
				case HW_NCPU: return SysctlGetByName("hw.ncpu", oldp, oldlenp);
				case HW_PHYSMEM: return SysctlGetByName("hw.physmem", oldp, oldlenp);
				case HW_USERMEM: return SysctlGetByName("hw.usermem", oldp, oldlenp);
				case HW_MACHINE: return SysctlGetByName("hw.machine", oldp, oldlenp);
				case HW_MACHINE_ARCH: return SysctlGetByName("hw.machine_arch", oldp, oldlenp);
				default: break;
			}
			break;
		default: break;
	}

	*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
	return -1;
}

// ---------------------------------------------------------- process control

#define FAIL_STUB(name, errno_val)                                                                  \
	static KYTY_SYSV_ABI int name {                                                                 \
		static std::atomic_uint32_t log_count = 0;                                                  \
		if (log_count.fetch_add(1) < 4) {                                                           \
			LOGF("LibcPayload: %s is not available inside the emulator\n", #name);                  \
		}                                                                                           \
		*Posix::GetErrorAddr() = errno_val;                                                         \
		return -1;                                                                                  \
	}

FAIL_STUB(payload_system(const char*), Posix::POSIX_ENOSYS)
FAIL_STUB(payload_execv(const char*, char* const*), Posix::POSIX_ENOEXEC)
FAIL_STUB(payload_execvp(const char*, char* const*), Posix::POSIX_ENOEXEC)
FAIL_STUB(payload_waitpid(int, int*, int), Posix::POSIX_ECHILD)
FAIL_STUB(payload_pipe(int*), Posix::POSIX_ENOSYS)
FAIL_STUB(payload_dup2(int, int), Posix::POSIX_EBADF)
FAIL_STUB(payload_fcntl(int, int, ...), Posix::POSIX_EINVAL)
FAIL_STUB(payload_ioctl(int, uint64_t, ...), Posix::POSIX_ENOTTY)
FAIL_STUB(payload_getifaddrs(void**), Posix::POSIX_ENXIO)

#undef FAIL_STUB

static KYTY_SYSV_ABI int payload_kill(int, int) {
	// Single guest process; signals are never delivered.
	return 0;
}

static KYTY_SYSV_ABI void payload_freeifaddrs(void*) {}

static KYTY_SYSV_ABI int payload_socketpair(int, int, int, int*) {
	*Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP;
	return -1;
}

static KYTY_SYSV_ABI int payload_munmap(void* addr, size_t len) {
	const auto result = LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(addr), len);
	if (result < 0) {
		*Posix::GetErrorAddr() = LibKernel::KernelToPosix(static_cast<int>(result));
		return -1;
	}
	return 0;
}

// Remaining sce* plain imports the payload SDK pulls in. FindByName matches
// these after stripping the leading "sce", so the C++ names below must end
// with the stripped form exactly.

static KYTY_SYSV_ABI int UserServiceGetForegroundUser(int32_t* user_id) {
	// SDK ABI: int sceUserServiceGetForegroundUser(SceUserServiceUserId*),
	// 0 = success, user id written through the pointer.
	if (user_id != nullptr) {
		*user_id = 1;
	}
	return 0;
}

static KYTY_SYSV_ABI int SystemServiceLaunchWebBrowser(int /*user_id*/, const void* /*param*/) {
	static std::atomic_uint32_t log_count = 0;
	if (log_count.fetch_add(1) < 4) {
		LOGF("LibcPayload: SystemServiceLaunchWebBrowser ignored (no browser in emulator)\n");
	}
	return 0;
}

static KYTY_SYSV_ABI int NetResolverStartAton(int /*rid*/, const char* hostname, size_t /*len*/,
                                              int /*timeout*/, int /*retry*/, int /*flags*/) {
	static std::atomic_uint32_t log_count = 0;
	if (log_count.fetch_add(1) < 4) {
		LOGF("LibcPayload: NetResolverStartAton(\"%s\") unsupported (no DNS for payload)\n",
		     (hostname != nullptr ? hostname : "<null>"));
	}
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
}

// The payload CRT treats sceSysmoduleLoadModuleInternal like the retail API:
// 0 = OK (module already loaded), nonzero = error. System modules are already
// provided by the HLE registrations, so report success.
static KYTY_SYSV_ABI int SysmoduleLoadModuleInternal(uint16_t /*id*/) {
	return 0;
}

static KYTY_SYSV_ABI int* payload_error() {
	return Posix::GetErrorAddr();
}

static KYTY_SYSV_ABI int* payload_sceNetErrnoLoc() {
	return Posix::GetErrorAddr();
}

[[noreturn]] KYTY_SYSV_ABI void ExitProcessNow(int code) {
	// Payload code runs on a guest stack while Windows TEB stack limits are
	// temporarily disabled. Entering the host CRT exit/atexit path from there
	// corrupts shutdown and races RuntimeLinker teardown with guest threads.
	Log::Flush();
	std::fflush(nullptr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
#endif
	std::_Exit(code);
}

// --------------------------------------------------------------- setjmp/ljmp

// FreeBSD x86-64 _setjmp layout: callee-saved registers plus the stack and
// return address. The kernel's pointer mangling is skipped; this pair is only
// ever used together, so the round trip stays consistent.
__attribute__((naked, returns_twice)) static KYTY_SYSV_ABI int payload_setjmp(void* env) {
	asm volatile("movq %rbx, 0(%rdi)\n"
	             "movq %rbp, 8(%rdi)\n"
	             "movq %r12, 16(%rdi)\n"
	             "movq %r13, 24(%rdi)\n"
	             "movq %r14, 32(%rdi)\n"
	             "movq %r15, 40(%rdi)\n"
	             "movq (%rsp), %rcx\n" // caller return address
	             "movq %rcx, 56(%rdi)\n"
	             "leaq 8(%rsp), %rcx\n" // rsp once setjmp returns
	             "movq %rcx, 48(%rdi)\n"
	             "xorl %eax, %eax\n"
	             "ret\n");
}

__attribute__((naked, noreturn)) static KYTY_SYSV_ABI void payload_longjmp(void* env, int val) {
	asm volatile("movq 0(%rdi), %rbx\n"
	             "movq 8(%rdi), %rbp\n"
	             "movq 16(%rdi), %r12\n"
	             "movq 24(%rdi), %r13\n"
	             "movq 32(%rdi), %r14\n"
	             "movq 40(%rdi), %r15\n"
	             "movq 56(%rdi), %rcx\n" // return address
	             "movq 48(%rdi), %rsp\n" // rsp as it was after setjmp returned
	             "movl %esi, %eax\n"
	             "testl %eax, %eax\n"
	             "jne 1f\n"
	             "movl $1, %eax\n"
	             "1:\n"
	             "jmpq *%rcx\n");
}

// ------------------------------------------------------------------- signals

using guest_sig_handler_t = KYTY_SYSV_ABI void (*)(int);

namespace {
guest_sig_handler_t g_signal_handlers[32] = {nullptr};
}

static KYTY_SYSV_ABI int payload_sigemptyset(uint8_t* set) {
	if (set == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	std::memset(set, 0, 16); // FreeBSD sizeof(sigset_t)
	return 0;
}

static KYTY_SYSV_ABI int payload_sigaddset(uint8_t* set, int signo) {
	if (set == nullptr || signo < 1 || signo > 128) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	set[(signo - 1) / 8] |= static_cast<uint8_t>(1u << ((signo - 1) % 8));
	return 0;
}

static KYTY_SYSV_ABI int payload_sigaction(int signum, const void* act, void* /*oldact*/) {
	if (signum < 1 || signum > 31) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	// Handlers are recorded but never delivered: there is no guest signal
	// delivery for payload processes in the emulator.
	if (act != nullptr) {
		g_signal_handlers[signum] = *reinterpret_cast<const guest_sig_handler_t*>(act);
	}
	return 0;
}

static KYTY_SYSV_ABI guest_sig_handler_t payload_signal(int signum, guest_sig_handler_t handler) {
	if (signum < 1 || signum > 31) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return reinterpret_cast<guest_sig_handler_t>(-1);
	}
	const auto previous       = g_signal_handlers[signum];
	g_signal_handlers[signum] = handler;
	return previous;
}

static KYTY_SYSV_ABI int payload_pthread_sigmask(int, const void*, void*) {
	return 0;
}

static KYTY_SYSV_ABI int payload_sigprocmask(int, const void*, void*) {
	return 0;
}

// ------------------------------------------------------------ pthread extras

static KYTY_SYSV_ABI int payload_pthread_cond_destroy(LibKernel::PthreadCond* cond) {
	const auto result = LibKernel::PthreadCondDestroy(cond);
	return (result != OK) ? LibKernel::KernelToPosix(result) : 0;
}

static KYTY_SYSV_ABI int payload_pthread_set_name_np(LibKernel::Pthread /*thread*/,
                                                     const char* name) {
	static std::atomic_uint32_t log_count = 0;
	if (log_count.fetch_add(1) < 4 && name != nullptr) {
		LOGF("LibcPayload: pthread_set_name_np(\"%s\") ignored\n", name);
	}
	return 0;
}

static KYTY_SYSV_ABI int payload_pthread_setcanceltype(int, int* oldtype) {
	if (oldtype != nullptr) {
		*oldtype = 0;
	}
	return 0;
}

// --------------------------------------------------------------- inet helpers

namespace {

uint32_t ParseIpv4(const char* cp, bool* ok) {
	uint32_t parts[4] = {0, 0, 0, 0};
	uint32_t value    = 0;
	int      part     = 0;
	int      digits   = 0;
	bool     done     = false;

	for (const char* p = cp; !done; p++) {
		if (*p >= '0' && *p <= '9') {
			value = value * 10u + static_cast<uint32_t>(*p - '0');
			if (++digits > 3 || value > 255) {
				*ok = false;
				return 0;
			}
			continue;
		}
		if (*p == '.') {
			if (digits == 0 || part >= 3) {
				*ok = false;
				return 0;
			}
			parts[part++] = value;
			value         = 0;
			digits        = 0;
			continue;
		}
		if (*p == '\0') {
			if (digits == 0 || part != 3) {
				*ok = false;
				return 0;
			}
			parts[part] = value;
			done        = true;
			continue;
		}
		*ok = false;
		return 0;
	}

	*ok = true;
	// Network byte order value.
	return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

} // namespace

static KYTY_SYSV_ABI uint32_t payload_inet_addr(const char* cp) {
	if (cp == nullptr) {
		return 0xffffffffu;
	}
	bool ok = false;
	const auto addr = ParseIpv4(cp, &ok);
	return ok ? addr : 0xffffffffu; // INADDR_NONE
}

static KYTY_SYSV_ABI int payload_inet_aton(const char* cp, uint32_t* addr) {
	if (cp == nullptr || addr == nullptr) {
		return 0;
	}
	bool ok = false;
	const auto value = ParseIpv4(cp, &ok);
	if (!ok) {
		return 0;
	}
	*addr = value;
	return 1;
}

static KYTY_SYSV_ABI const char* payload_inet_ntoa(uint32_t addr) {
	static thread_local char buffer[18];
	std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", (addr >> 24) & 0xffu,
	              (addr >> 16) & 0xffu, (addr >> 8) & 0xffu, addr & 0xffu);
	return buffer;
}

static KYTY_SYSV_ABI const char* payload_inet_ntop(int af, const void* src, char* dst,
                                                   size_t size) {
	if (af != 2 /* AF_INET */ || src == nullptr || dst == nullptr) {
		return nullptr;
	}
	const auto bytes = static_cast<const uint8_t*>(src);
	if (size < 16) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOSPC;
		return nullptr;
	}
	std::snprintf(dst, size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
	return dst;
}

// -------------------------------------------------------------- guest objects

static int g_isthreaded = 1;
static int g_mb_cur_max = 1;

LIB_DEFINE(InitLibcPayload_1) {
	// Memory
	LIB_FUNC("malloc", LibcPayload::payload_malloc);
	LIB_FUNC("free", LibcPayload::payload_free);
	LIB_FUNC("calloc", LibcPayload::payload_calloc);
	LIB_FUNC("realloc", LibcPayload::payload_realloc);
	LIB_FUNC("strdup", LibcPayload::payload_strdup);

	// Console / formatted output
	LIB_FUNC("printf", LibcPayload::payload_printf);
	LIB_FUNC("vprintf", LibcPayload::payload_vprintf);
	LIB_FUNC("fprintf", LibcPayload::payload_fprintf);
	LIB_FUNC("vfprintf", LibcPayload::payload_vfprintf);
	LIB_FUNC("vsnprintf", LibcPayload::payload_vsnprintf);
	LIB_FUNC("sprintf", LibcPayload::payload_sprintf);
	LIB_FUNC("vasprintf", LibcPayload::payload_vasprintf);
	LIB_FUNC("putchar", LibcPayload::payload_putchar);
	LIB_FUNC("putc", LibcPayload::payload_putc);
	LIB_FUNC("fputc", LibcPayload::payload_fputc);
	LIB_FUNC("fputs", LibcPayload::payload_fputs);
	LIB_FUNC("__swbuf", LibcPayload::payload___swbuf);
	LIB_FUNC("perror", LibcPayload::payload_perror);

	// Environment
	LIB_FUNC("getenv", LibcPayload::payload_getenv);
	LIB_FUNC("putenv", LibcPayload::payload_putenv);

	// File stdio
	LIB_FUNC("fopen", LibcPayload::payload_fopen);
	LIB_FUNC("fclose", LibcPayload::payload_fclose);
	LIB_FUNC("fread", LibcPayload::payload_fread);
	LIB_FUNC("fwrite", LibcPayload::payload_fwrite);
	LIB_FUNC("fseek", LibcPayload::payload_fseek);
	LIB_FUNC("fseeko", LibcPayload::payload_fseek);
	LIB_FUNC("ftell", LibcPayload::payload_ftell);
	LIB_FUNC("ftello", LibcPayload::payload_ftell);
	LIB_FUNC("fileno", LibcPayload::payload_fileno);
	LIB_FUNC("feof", LibcPayload::payload_feof);
	LIB_FUNC("ferror", LibcPayload::payload_ferror);
	LIB_FUNC("fgetc", LibcPayload::payload_fgetc);
	LIB_FUNC("fgets", LibcPayload::payload_fgets);
	LIB_FUNC("ungetc", LibcPayload::payload_ungetc);
	LIB_FUNC("remove", LibcPayload::payload_remove);
	LIB_FUNC("rename", LibcPayload::payload_rename);
	LIB_FUNC("fscanf", LibcPayload::payload_fscanf);
	LIB_FUNC("sscanf", LibcPayload::payload_sscanf);
	LIB_FUNC("vsscanf", LibcPayload::payload_vsscanf);

	// directory iteration
	LIB_FUNC("opendir", LibcPayload::payload_opendir);
	LIB_FUNC("closedir", LibcPayload::payload_closedir);
	LIB_FUNC("readdir", LibcPayload::payload_readdir);

	// ctype
	LIB_FUNC("isalnum", LibcPayload::payload_isalnum);
	LIB_FUNC("isalpha", LibcPayload::payload_isalpha);
	LIB_FUNC("isblank", LibcPayload::payload_isblank);
	LIB_FUNC("iscntrl", LibcPayload::payload_iscntrl);
	LIB_FUNC("isgraph", LibcPayload::payload_isgraph);
	LIB_FUNC("islower", LibcPayload::payload_islower);
	LIB_FUNC("isprint", LibcPayload::payload_isprint);
	LIB_FUNC("ispunct", LibcPayload::payload_ispunct);
	LIB_FUNC("isspace", LibcPayload::payload_isspace);
	LIB_FUNC("isupper", LibcPayload::payload_isupper);
	LIB_FUNC("isxdigit", LibcPayload::payload_isxdigit);
	LIB_FUNC("toupper", LibcPayload::payload_toupper);
	LIB_FUNC("tolower", LibcPayload::payload_tolower);

	// strings
	LIB_FUNC("memchr", LibcPayload::payload_memchr);
	LIB_FUNC("atof", LibcPayload::payload_atof);
	LIB_FUNC("strtod", LibcPayload::payload_strtod);
	LIB_FUNC("strtof", LibcPayload::payload_strtof);
	LIB_FUNC("strtold", LibcPayload::payload_strtold);
	LIB_FUNC("strtoll", LibcPayload::payload_strtoll);
	LIB_FUNC("strtoull", LibcPayload::payload_strtoull);
	LIB_FUNC("strerror", LibcPayload::payload_strerror);
	// FreeBSD payload CRTs may request the internal spelling through dlsym.
	// It has the same ABI and semantics as strerror.
	LIB_FUNC("_Strerror", LibcPayload::payload_strerror);
	LIB_FUNC("strcoll", LibcPayload::payload_strcoll);
	LIB_FUNC("strxfrm", LibcPayload::payload_strxfrm);
	LIB_FUNC("strlcpy", LibcPayload::payload_strlcpy);
	LIB_FUNC("strlcat", LibcPayload::payload_strlcat);
	LIB_FUNC("strncasecmp", LibcPayload::payload_strncasecmp);
	LIB_FUNC("strnlen", LibcPayload::payload_strnlen);
	LIB_FUNC("strtok", LibcPayload::payload_strtok);
	LIB_FUNC("strtok_r", LibcPayload::payload_strtok_r);

	// wide chars
	LIB_FUNC("wcslen", LibcPayload::payload_wcslen);
	LIB_FUNC("wcscmp", LibcPayload::payload_wcscmp);
	LIB_FUNC("wcsncmp", LibcPayload::payload_wcsncmp);
	LIB_FUNC("wcsstr", LibcPayload::payload_wcsstr);
	LIB_FUNC("wcscoll", LibcPayload::payload_wcscoll);
	LIB_FUNC("wcsxfrm", LibcPayload::payload_wcsxfrm);
	LIB_FUNC("wcstombs", LibcPayload::payload_wcstombs);
	LIB_FUNC("mbstowcs", LibcPayload::payload_mbstowcs);
	LIB_FUNC("mbrtowc", LibcPayload::payload_mbrtowc);
	LIB_FUNC("wcrtomb", LibcPayload::payload_wcrtomb);
	LIB_FUNC("mbrlen", LibcPayload::payload_mbrlen);
	LIB_FUNC("mbtowc", LibcPayload::payload_mbtowc);
	LIB_FUNC("mbsrtowcs", LibcPayload::payload_mbsrtowcs);

	// sort / search
	LIB_FUNC("qsort", LibcPayload::payload_qsort);
	LIB_FUNC("bsearch", LibcPayload::payload_bsearch);

	// time / locale / misc
	LIB_FUNC("time", LibcPayload::payload_time);
	LIB_FUNC("localtime", LibcPayload::payload_localtime);
	LIB_FUNC("strftime", LibcPayload::payload_strftime);
	LIB_FUNC("setlocale", LibcPayload::payload_setlocale);
	LIB_FUNC("rand", LibcPayload::payload_rand);
	LIB_FUNC("srand", LibcPayload::payload_srand);
	LIB_FUNC("random", LibcPayload::payload_random);
	LIB_FUNC("srandom", LibcPayload::payload_srandom);
	LIB_FUNC("sleep", LibcPayload::payload_sleep);
	LIB_FUNC("usleep", LibcPayload::payload_usleep);
	LIB_FUNC("getcwd", LibcPayload::payload_getcwd);
	LIB_FUNC("sysconf", LibcPayload::payload_sysconf);
	LIB_FUNC("sysctl", LibcPayload::payload_sysctl);
	LIB_FUNC("sysctlbyname", LibcPayload::payload_sysctlbyname);
	LIB_FUNC("munmap", LibcPayload::payload_munmap);
	LIB_FUNC("__error", LibcPayload::payload_error);
	LIB_FUNC("sceNetErrnoLoc", LibcPayload::payload_sceNetErrnoLoc);
	LIB_FUNC("exit", LibcPayload::ExitProcessNow);
	LIB_FUNC("_exit", LibcPayload::ExitProcessNow);
	LIB_FUNC("_Exit", LibcPayload::ExitProcessNow);

	// non-local exits
	LIB_FUNC("setjmp", LibcPayload::payload_setjmp);
	LIB_FUNC("longjmp", LibcPayload::payload_longjmp);

	// signals
	LIB_FUNC("sigemptyset", LibcPayload::payload_sigemptyset);
	LIB_FUNC("sigaddset", LibcPayload::payload_sigaddset);
	LIB_FUNC("sigaction", LibcPayload::payload_sigaction);
	LIB_FUNC("signal", LibcPayload::payload_signal);
	LIB_FUNC("pthread_sigmask", LibcPayload::payload_pthread_sigmask);
	LIB_FUNC("sigprocmask", LibcPayload::payload_sigprocmask);

	// pthread extras
	LIB_FUNC("pthread_cond_destroy", LibcPayload::payload_pthread_cond_destroy);
	LIB_FUNC("pthread_set_name_np", LibcPayload::payload_pthread_set_name_np);
	LIB_FUNC("pthread_setcanceltype", LibcPayload::payload_pthread_setcanceltype);

	// process control stubs
	LIB_FUNC("system", LibcPayload::payload_system);
	LIB_FUNC("execv", LibcPayload::payload_execv);
	LIB_FUNC("execvp", LibcPayload::payload_execvp);
	LIB_FUNC("kill", LibcPayload::payload_kill);
	LIB_FUNC("waitpid", LibcPayload::payload_waitpid);
	LIB_FUNC("pipe", LibcPayload::payload_pipe);
	LIB_FUNC("dup2", LibcPayload::payload_dup2);
	LIB_FUNC("fcntl", LibcPayload::payload_fcntl);
	LIB_FUNC("ioctl", LibcPayload::payload_ioctl);
	LIB_FUNC("socketpair", LibcPayload::payload_socketpair);
	LIB_FUNC("getifaddrs", LibcPayload::payload_getifaddrs);
	LIB_FUNC("freeifaddrs", LibcPayload::payload_freeifaddrs);

	// inet helpers
	LIB_FUNC("__inet_addr", LibcPayload::payload_inet_addr);
	LIB_FUNC("__inet_aton", LibcPayload::payload_inet_aton);
	LIB_FUNC("__inet_ntoa", LibcPayload::payload_inet_ntoa);
	LIB_FUNC("__inet_ntop", LibcPayload::payload_inet_ntop);

	// remaining sce* plain imports from the payload SDK
	LIB_FUNC("sceUserServiceGetForegroundUser", LibcPayload::UserServiceGetForegroundUser);
	LIB_FUNC("sceSystemServiceLaunchWebBrowser", LibcPayload::SystemServiceLaunchWebBrowser);
	LIB_FUNC("sceNetResolverStartAton", LibcPayload::NetResolverStartAton);
	LIB_FUNC("sceSysmoduleLoadModuleInternal", LibcPayload::SysmoduleLoadModuleInternal);

	// FreeBSD exposes __stdinp/__stdoutp/__stderrp as inline-dereferenced FILE
	// objects; the guest putc/getc macros read them without calling us.
	LIB_OBJECT("__stdinp", &g_stdin_file);
	LIB_OBJECT("__stdoutp", &g_stdout_file);
	LIB_OBJECT("__stderrp", &g_stderr_file);

	// __isthreaded/__mb_cur_max are variables the guest reads in place.
	LIB_OBJECT("__isthreaded", &g_isthreaded);
	LIB_OBJECT("__mb_cur_max", &g_mb_cur_max);
}

} // namespace LibcPayload

} // namespace Libs
