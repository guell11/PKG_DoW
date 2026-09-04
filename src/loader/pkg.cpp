#include "loader/pkg.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <string_view>

namespace Loader::Pkg {
namespace {

constexpr uint32_t kCntMagic = 0x7f434e54;
constexpr size_t kHeaderSize = 0x80;
constexpr size_t kContentIdOffset = 0x40;
constexpr size_t kContentIdSize = 0x24;
constexpr size_t kTableEntrySize = 0x20;
constexpr uint32_t kMaxTableEntries = 4096;

uint32_t ReadBe32(const uint8_t* bytes) {
	return (static_cast<uint32_t>(bytes[0]) << 24u) | (static_cast<uint32_t>(bytes[1]) << 16u) |
	       (static_cast<uint32_t>(bytes[2]) << 8u) | static_cast<uint32_t>(bytes[3]);
}

uint64_t ReadBe64(const uint8_t* bytes) {
	return (static_cast<uint64_t>(ReadBe32(bytes)) << 32u) | ReadBe32(bytes + 4);
}

bool IsRangeInside(uint64_t offset, uint64_t size, uint64_t file_size) {
	return offset <= file_size && size <= file_size - offset;
}

bool ReadAt(std::ifstream& file, uint64_t offset, void* out, size_t size) {
	if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
	    size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
		return false;
	}
	file.clear();
	file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	file.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
	return file.good();
}

std::string ReadContentId(const std::array<uint8_t, kHeaderSize>& header) {
	const auto begin = header.begin() + kContentIdOffset;
	const auto end = std::find(begin, begin + kContentIdSize, uint8_t{0});
	std::string result;
	result.reserve(static_cast<size_t>(end - begin));
	for (auto it = begin; it != end; ++it) {
		if (*it < 0x20 || *it > 0x7e) {
			return {};
		}
		result.push_back(static_cast<char>(*it));
	}
	return result;
}

std::string TitleIdFromContentId(std::string_view content_id) {
	const auto dash = content_id.find('-');
	if (dash == std::string_view::npos) {
		return {};
	}
	const auto underscore = content_id.find('_', dash + 1);
	if (underscore == std::string_view::npos || underscore - dash - 1 != 9) {
		return {};
	}
	const auto title_id = content_id.substr(dash + 1, 9);
	if (!std::all_of(title_id.begin(), title_id.begin() + 4, [](unsigned char c) { return std::isupper(c) != 0; }) ||
	    !std::all_of(title_id.begin() + 4, title_id.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
		return {};
	}
	return std::string(title_id);
}

Platform PlatformFromTitleId(std::string_view title_id) {
	if (title_id.starts_with("CUSA") || title_id.starts_with("LAPY")) {
		return Platform::Ps4;
	}
	if (title_id.starts_with("PPSA")) {
		return Platform::Ps5;
	}
	return Platform::Unknown;
}

HomebrewStatus HomebrewFromContentId(std::string_view content_id) {
	return content_id.find("HOMEBREW") == std::string_view::npos ? HomebrewStatus::Indeterminate
	                                                              : HomebrewStatus::Declared;
}

bool IsRegularFile(const std::filesystem::path& path) {
	std::error_code error;
	return std::filesystem::is_regular_file(path, error) && !error;
}

bool HasElfMagic(const std::filesystem::path& path) {
	std::array<uint8_t, 4> magic{};
	std::ifstream file(path, std::ios::binary);
	return file.is_open() && ReadAt(file, 0, magic.data(), magic.size()) && magic[0] == 0x7f && magic[1] == 'E' &&
	       magic[2] == 'L' && magic[3] == 'F';
}

std::optional<std::filesystem::path> FirstElfIn(const std::filesystem::path& directory) {
	std::error_code error;
	std::vector<std::filesystem::path> candidates;
	for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error),
	                                      end;
	     !error && it != end; it.increment(error)) {
		const auto path = it->path();
		if (IsRegularFile(path) && path.extension() == ".elf") {
			candidates.push_back(path);
		}
	}
	std::sort(candidates.begin(), candidates.end());
	for (const auto& candidate: candidates) {
		if (HasElfMagic(candidate)) {
			return candidate;
		}
	}
	return std::nullopt;
}

} // namespace

ParseResult ParseMetadata(const std::filesystem::path& package_path) {
	ParseResult result;
	std::error_code error;
	const auto status = std::filesystem::status(package_path, error);
	if (error || !std::filesystem::exists(status)) {
		result.error = Error::NotFound;
		return result;
	}
	if (!std::filesystem::is_regular_file(status)) {
		result.error = Error::NotRegularFile;
		return result;
	}
	const auto native_size = std::filesystem::file_size(package_path, error);
	if (error) {
		result.error = Error::Io;
		return result;
	}
	result.info.file_size = native_size;
	if (native_size < kHeaderSize) {
		result.error = Error::TooSmall;
		return result;
	}

	std::ifstream file(package_path, std::ios::binary);
	std::array<uint8_t, kHeaderSize> header{};
	if (!file.is_open() || !ReadAt(file, 0, header.data(), header.size())) {
		result.error = Error::Io;
		return result;
	}
	if (ReadBe32(header.data()) != kCntMagic) {
		result.error = Error::BadMagic;
		return result;
	}

	result.info.package_type = ReadBe32(header.data() + 0x04);
	result.info.file_count = ReadBe32(header.data() + 0x0c);
	const auto entry_count = ReadBe32(header.data() + 0x10);
	const auto table_offset = static_cast<uint64_t>(ReadBe32(header.data() + 0x18));
	result.info.entry_data_size = ReadBe32(header.data() + 0x1c);
	result.info.body_offset = ReadBe64(header.data() + 0x20);
	result.info.body_size = ReadBe64(header.data() + 0x28);
	result.info.content_offset = ReadBe64(header.data() + 0x30);
	result.info.content_size = ReadBe64(header.data() + 0x38);

	if (entry_count > kMaxTableEntries) {
		result.error = Error::TooManyEntries;
		return result;
	}
	const auto table_size = static_cast<uint64_t>(entry_count) * kTableEntrySize;
	if (!IsRangeInside(table_offset, table_size, native_size) ||
	    !IsRangeInside(result.info.body_offset, result.info.body_size, native_size) ||
	    !IsRangeInside(result.info.content_offset, result.info.content_size, native_size)) {
		result.error = Error::InvalidRange;
		return result;
	}

	result.info.content_id = ReadContentId(header);
	result.info.title_id = TitleIdFromContentId(result.info.content_id);
	result.info.platform = PlatformFromTitleId(result.info.title_id);
	result.info.homebrew = HomebrewFromContentId(result.info.content_id);
	result.info.entries.reserve(entry_count);
	std::array<uint8_t, kTableEntrySize> raw_entry{};
	for (uint32_t index = 0; index < entry_count; ++index) {
		const auto offset = table_offset + static_cast<uint64_t>(index) * kTableEntrySize;
		if (!ReadAt(file, offset, raw_entry.data(), raw_entry.size())) {
			result.info.entries.clear();
			result.error = Error::Io;
			return result;
		}
		TableEntry entry;
		entry.id = ReadBe32(raw_entry.data());
		entry.filename_offset = ReadBe32(raw_entry.data() + 0x04);
		entry.flags1 = ReadBe32(raw_entry.data() + 0x08);
		entry.flags2 = ReadBe32(raw_entry.data() + 0x0c);
		entry.offset = ReadBe32(raw_entry.data() + 0x10);
		entry.size = ReadBe32(raw_entry.data() + 0x14);
		if (!IsRangeInside(entry.offset, entry.size, native_size)) {
			result.info.entries.clear();
			result.error = Error::InvalidTable;
			return result;
		}
		result.info.entries.push_back(entry);
	}
	return result;
}

std::optional<std::filesystem::path> FindExtractedExecutable(const std::filesystem::path& extracted_root) {
	if (!IsRegularFile(extracted_root)) {
		std::error_code error;
		if (!std::filesystem::is_directory(extracted_root, error) || error) {
			return std::nullopt;
		}
	} else if (HasElfMagic(extracted_root)) {
		return extracted_root;
	} else {
		return std::nullopt;
	}

	const std::array candidates = {
		extracted_root / "Image0" / "sce_module" / "eboot.bin",
		extracted_root / "image0" / "sce_module" / "eboot.bin",
		extracted_root / "sce_module" / "eboot.bin",
		extracted_root / "eboot.bin",
	};
	for (const auto& candidate: candidates) {
		if (IsRegularFile(candidate)) {
			return candidate;
		}
	}

	const std::array directories = {
		extracted_root,
		extracted_root / "Image0",
		extracted_root / "image0",
		extracted_root / "sce_module",
		extracted_root / "Image0" / "sce_module",
		extracted_root / "image0" / "sce_module",
	};
	for (const auto& directory: directories) {
		if (const auto elf = FirstElfIn(directory); elf.has_value()) {
			return elf;
		}
	}
	return std::nullopt;
}

} // namespace Loader::Pkg
