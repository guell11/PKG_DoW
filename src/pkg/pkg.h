#ifndef EMULATOR_INCLUDE_EMULATOR_PKG_PKG_H_
#define EMULATOR_INCLUDE_EMULATOR_PKG_PKG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Pkg {

enum class Status {
	Ok,
	FileOpenFailed,
	InvalidContainer,
	UnsupportedEncryptedContent,
	UnsupportedRetailSelf,
	UnsafeEntryPath,
	OutputExists,
	IoError,
	NoBootExecutable,
};

enum class ContainerKind {
	Unknown,
	Cnt,
	Ps5FinalizedImage,
};

enum class ExecutableKind {
	Missing,
	RawElf,
	SelfOrUnknown,
};

struct Entry {
	uint32_t    id = 0;
	uint32_t    flags = 0;
	uint32_t    key_flags = 0;
	uint64_t    offset = 0;
	uint64_t    size = 0;
	std::string name;
	bool        encrypted = false;
};

struct PackageInfo {
	ContainerKind       kind = ContainerKind::Unknown;
	uint64_t            container_offset = 0;
	uint64_t            container_size = 0;
	std::string         content_id;
	uint32_t            drm_type = 0;
	uint32_t            content_type = 0;
	bool                retail = false;
	bool                contains_encrypted_entries = false;
	bool                contains_pfs_image = false;
	std::vector<Entry> entries;
};

struct Result {
	Status      status = Status::Ok;
	std::string message;
	PackageInfo package;
};

struct ExtractionResult {
	Result                result;
	std::filesystem::path output_directory;
	std::filesystem::path boot_executable;
	ExecutableKind        boot_kind = ExecutableKind::Missing;
	uint32_t              extracted_files = 0;
};

// Parses public PKG headers. No key handling, decryption, or SELF conversion.
[[nodiscard]] Result Inspect(const std::filesystem::path& package_path);

// Copies only direct, unencrypted CNT entries. PFS-backed packages are rejected.
[[nodiscard]] ExtractionResult ExtractHomebrew(const std::filesystem::path& package_path,
	                                               const std::filesystem::path& output_directory);

[[nodiscard]] std::filesystem::path FindBootExecutable(const std::filesystem::path& app_root);
[[nodiscard]] ExecutableKind ClassifyExecutable(const std::filesystem::path& executable);
[[nodiscard]] const char* StatusMessage(Status status);

} // namespace Pkg

#endif // EMULATOR_INCLUDE_EMULATOR_PKG_PKG_H_
