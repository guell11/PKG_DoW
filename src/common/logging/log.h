#ifndef KYTY_COMMON_LOGGING_LOG_H_
#define KYTY_COMMON_LOGGING_LOG_H_

#include "common/common.h"

#include <atomic>

#include <fmt/color.h>
#include <fmt/printf.h>
#include <string_view>

namespace Log {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name               = "Log";
	static constexpr auto        initialize         = Log::Initialize;
	static constexpr auto        shutdown           = Log::Shutdown;
	static constexpr auto        emergency_shutdown = Log::Shutdown;
};

enum class Direction { Silent, Console, File };

Direction GetDirection();
bool      IsSilent();
void      Write(std::string_view text);
void      Write(fmt::text_style style, std::string_view text);
void      WriteFatal(std::string_view text);
void      WriteFatal(fmt::text_style style, std::string_view text);
void      Flush();

namespace Color {

inline constexpr auto Default       = fmt::text_style {};
inline constexpr auto Red           = fmt::fg(fmt::terminal_color::red);
inline constexpr auto Green         = fmt::fg(fmt::terminal_color::green);
inline constexpr auto Yellow        = fmt::fg(fmt::terminal_color::yellow);
inline constexpr auto Magenta       = fmt::fg(fmt::terminal_color::magenta);
inline constexpr auto Cyan          = fmt::fg(fmt::terminal_color::cyan);
inline constexpr auto White         = fmt::fg(fmt::terminal_color::white);
inline constexpr auto BrightRed     = fmt::fg(fmt::terminal_color::bright_red);
inline constexpr auto BrightGreen   = fmt::fg(fmt::terminal_color::bright_green);
inline constexpr auto BrightYellow  = fmt::fg(fmt::terminal_color::bright_yellow);
inline constexpr auto BrightMagenta = fmt::fg(fmt::terminal_color::bright_magenta);
inline constexpr auto BrightWhite   = fmt::fg(fmt::terminal_color::bright_white);

} // namespace Color

} // namespace Log

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF(...)                                                                                  \
	do {                                                                                           \
		if (!::Log::IsSilent()) {                                                                  \
			::Log::Write(::fmt::sprintf(__VA_ARGS__));                                             \
		}                                                                                          \
	} while (false)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		if (!::Log::IsSilent()) {                                                                  \
			::Log::Write((style), ::fmt::sprintf(__VA_ARGS__));                                    \
		}                                                                                          \
	} while (false)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
// Hot-path logging: keep the first few calls for debugging, then log only
// every `period`-th call. Per-byte file I/O, vblank waits and nanosleep
// loops fire tens of thousands of times per second; formatting every
// single call dominates the guest's CPU budget.
#define KYTY_LOG_CONCAT_INNER(a, b) a##b
#define KYTY_LOG_CONCAT(a, b) KYTY_LOG_CONCAT_INNER(a, b)
#define LOGF_EVERY(period, ...)                                                                   \
	do {                                                                                          \
		if (!::Log::IsSilent()) {                                                                  \
			static std::atomic_uint32_t KYTY_LOG_CONCAT(kyty_log_count_, __LINE__) {0};            \
			const auto kyty_log_n_ =                                                                \
			    KYTY_LOG_CONCAT(kyty_log_count_, __LINE__).fetch_add(1, std::memory_order_relaxed); \
			if (kyty_log_n_ < 64 || ((kyty_log_n_ % ((period) > 0 ? (period) : 1)) == 0)) {        \
				::Log::Write(::fmt::sprintf(__VA_ARGS__));                                          \
			}                                                                                        \
		}                                                                                            \
	} while (false)

#endif /* KYTY_COMMON_LOGGING_LOG_H_ */
