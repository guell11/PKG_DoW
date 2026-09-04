#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_PKG_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_PKG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Loader::Pkg {

// This reader intentionally handles only clear PKG container metadata. It never
// reads, decrypts, installs, or executes protected package payloads.
enum class Error {
	None,
	NotFound,
	NotRegularFile,
	Io,
	TooSmall,
	BadMagic,
	TooManyEntries,
	InvalidRange,
	InvalidTable,
};

enum class Platform {
	Unknown,
	Ps4,
	Ps5,
};

// A PKG header cannot prove entitlement, ownership, or whether code is
// homebrew. Declared means that its clear content id explicitly says HOMEBREW.
enum class HomebrewStatus {
	Indeterminate,
	Declared,
};

struct TableEntry {
	uint32_t id = 0;
	uint32_t filename_offset = 0;
	uint32_t flags1 = 0;
	uint32_t flags2 = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
};

struct Info {
	uint64_t file_size = 0;
	uint32_t package_type = 0;
	uint32_t file_count = 0;
	uint32_t entry_data_size = 0;
	uint64_t body_offset = 0;
	uint64_t body_size = 0;
	uint64_t content_offset = 0;
	uint64_t content_size = 0;
	std::string content_id;
	std::string title_id;
	Platform platform = Platform::Unknown;
	HomebrewStatus homebrew = HomebrewStatus::Indeterminate;
	std::vector<TableEntry> entries;
};

struct ParseResult {
	Info info;
	Error error = Error::None;

	[[nodiscard]] explicit operator bool() const { return error == Error::None; }
};

// Parses big-endian CNT headers used by PS4-style PKGs. All declared clear
// ranges are validated against the file before they are returned.
[[nodiscard]] ParseResult ParseMetadata(const std::filesystem::path& package_path);

// Searches only conventional locations in an already extracted directory. No
// package extraction occurs. eboot.bin is preferred; a regular ELF is fallback.
[[nodiscard]] std::optional<std::filesystem::path>
FindExtractedExecutable(const std::filesystem::path& extracted_root);

} // namespace Loader::Pkg

#endif // EMULATOR_INCLUDE_EMULATOR_LOADER_PKG_H_
