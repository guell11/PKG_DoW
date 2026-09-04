#include "pkg/pkg.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>

namespace Pkg {

namespace {

constexpr uint64_t CntHeaderSize = 0x5a0;
constexpr uint64_t CntEntrySize = 0x20;
constexpr uint32_t EntryNamesId = 0x0200;
constexpr uint32_t ParamSfoId = 0x1000;
constexpr uint32_t ParamJsonId = 0x1008;
constexpr uint32_t Icon0Id = 0x1200;
constexpr uint32_t Pic0Id = 0x1220;
constexpr uint32_t EntryEncryptedFlag = 0x80000000u;
constexpr uint32_t MaxEntries = 16384;
constexpr uint64_t MaxNameTableSize = 16ull * 1024ull * 1024ull;
constexpr uint64_t CopyChunkSize = 1024ull * 1024ull;

[[nodiscard]] bool InRange(uint64_t offset, uint64_t size, uint64_t limit) {
	return offset <= limit && size <= limit - offset;
}

class Reader {
public:
	explicit Reader(const std::filesystem::path& path): m_file(path, std::ios::binary | std::ios::ate) {
		if (m_file) {
			m_size = static_cast<uint64_t>(m_file.tellg());
			m_file.seekg(0);
		}
	}

	[[nodiscard]] bool IsOpen() const { return static_cast<bool>(m_file); }
	[[nodiscard]] uint64_t Size() const { return m_size; }

	bool ReadAt(uint64_t offset, void* data, uint64_t size) {
		if (!InRange(offset, size, m_size) || size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
			return false;
		}
		m_file.clear();
		m_file.seekg(static_cast<std::streamoff>(offset));
		m_file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
		return static_cast<bool>(m_file);
	}

private:
	std::ifstream m_file;
	uint64_t      m_size = 0;
};

[[nodiscard]] uint16_t ReadBe16(const uint8_t* value) {
	return (static_cast<uint16_t>(value[0]) << 8u) | value[1];
}

[[nodiscard]] uint32_t ReadBe32(const uint8_t* value) {
	return (static_cast<uint32_t>(value[0]) << 24u) | (static_cast<uint32_t>(value[1]) << 16u) |
	       (static_cast<uint32_t>(value[2]) << 8u) | value[3];
}

[[nodiscard]] uint64_t ReadLe64(const uint8_t* value) {
	uint64_t result = 0;
	for (uint32_t i = 0; i < 8; i++) {
		result |= static_cast<uint64_t>(value[i]) << (i * 8u);
	}
	return result;
}

[[nodiscard]] std::string ReadCString(const uint8_t* value, size_t size) {
	const auto end = std::find(value, value + size, uint8_t {0});
	return {reinterpret_cast<const char*>(value), static_cast<size_t>(end - value)};
}

[[nodiscard]] std::string KnownEntryName(uint32_t id) {
	switch (id) {
		case ParamSfoId: return "sce_sys/param.sfo";
		case ParamJsonId: return "sce_sys/param.json";
		case Icon0Id: return "sce_sys/icon0.png";
		case Pic0Id: return "sce_sys/pic0.png";
		default: return {};
	}
}

[[nodiscard]] bool IsSafeRelativePath(std::string_view value) {
	if (value.empty() || value.size() > 240 || value.front() == '/' || value.front() == '\\' ||
	    value.find(':') != std::string_view::npos || value.find('\0') != std::string_view::npos) {
		return false;
	}
	std::string normal(value);
	std::replace(normal.begin(), normal.end(), '\\', '/');
	for (size_t begin = 0; begin < normal.size();) {
		const size_t end = normal.find('/', begin);
		const std::string_view component(normal.data() + begin,
		                                 (end == std::string::npos ? normal.size() : end) - begin);
		if (component.empty() || component == "." || component == "..") {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

[[nodiscard]] std::string NormalizeName(std::string value) {
	std::replace(value.begin(), value.end(), '\\', '/');
	return value;
}

void SetError(Result& result, Status status, std::string message) {
	result.status = status;
	result.message = std::move(message);
}

[[nodiscard]] Result ParseCnt(Reader& reader, uint64_t base, uint64_t limit,
	                             ContainerKind kind, bool retail) {
	Result result {};
	result.package.kind = kind;
	result.package.container_offset = base;
	result.package.container_size = limit;
	result.package.retail = retail;
	if (!InRange(base, CntHeaderSize, reader.Size()) || limit < CntHeaderSize) {
		SetError(result, Status::InvalidContainer, "CNT header truncated");
		return result;
	}

	std::array<uint8_t, CntHeaderSize> header {};
	if (!reader.ReadAt(base, header.data(), header.size()) ||
	    std::string_view(reinterpret_cast<const char*>(header.data()), 4) != "\x7f" "CNT") {
		SetError(result, Status::InvalidContainer, "CNT magic missing");
		return result;
	}

	const uint32_t entry_count = ReadBe32(header.data() + 0x10);
	const uint16_t entry_count_copy = ReadBe16(header.data() + 0x16);
	const uint64_t table_offset = ReadBe32(header.data() + 0x18);
	const uint64_t pfs_size_be = (static_cast<uint64_t>(ReadBe32(header.data() + 0x418)) << 32u) |
	                             ReadBe32(header.data() + 0x41c);
	result.package.content_id = ReadCString(header.data() + 0x40, 0x30);
	result.package.drm_type = ReadBe32(header.data() + 0x70);
	result.package.content_type = ReadBe32(header.data() + 0x74);
	result.package.contains_pfs_image = pfs_size_be != 0;

	if (entry_count == 0 || entry_count > MaxEntries || entry_count_copy != entry_count ||
	    !InRange(table_offset, static_cast<uint64_t>(entry_count) * CntEntrySize, limit)) {
		SetError(result, Status::InvalidContainer, "CNT entry table invalid");
		return result;
	}

	std::vector<uint8_t> table(static_cast<size_t>(entry_count) * CntEntrySize);
	if (!reader.ReadAt(base + table_offset, table.data(), table.size())) {
		SetError(result, Status::IoError, "CNT entry table read failed");
		return result;
	}

	result.package.entries.reserve(entry_count);
	for (uint32_t index = 0; index < entry_count; index++) {
		const uint8_t* raw = table.data() + static_cast<size_t>(index) * CntEntrySize;
		Entry entry {};
		entry.id = ReadBe32(raw);
		entry.flags = ReadBe32(raw + 8);
		entry.key_flags = ReadBe32(raw + 12);
		entry.offset = ReadBe32(raw + 16);
		entry.size = ReadBe32(raw + 20);
		entry.encrypted = (entry.flags & EntryEncryptedFlag) != 0;
		if (!InRange(entry.offset, entry.size, limit)) {
			SetError(result, Status::InvalidContainer, "CNT entry range invalid");
			return result;
		}
		result.package.contains_encrypted_entries |= entry.encrypted;
		result.package.entries.push_back(std::move(entry));
	}

	auto names = std::find_if(result.package.entries.begin(), result.package.entries.end(),
	                          [](const Entry& entry) { return entry.id == EntryNamesId; });
	std::vector<uint8_t> name_table;
	if (names != result.package.entries.end()) {
		if (names->encrypted || names->size > MaxNameTableSize) {
			SetError(result, Status::UnsupportedEncryptedContent, "CNT entry-name table unavailable");
			return result;
		}
		name_table.resize(static_cast<size_t>(names->size));
		if (!reader.ReadAt(base + names->offset, name_table.data(), name_table.size())) {
			SetError(result, Status::IoError, "CNT entry-name table read failed");
			return result;
		}
	}

	for (uint32_t index = 0; index < entry_count; index++) {
		const uint8_t* raw = table.data() + static_cast<size_t>(index) * CntEntrySize;
		const uint32_t name_offset = ReadBe32(raw + 4);
		auto& entry = result.package.entries[index];
		entry.name = KnownEntryName(entry.id);
		if (name_offset != 0 && name_offset < name_table.size()) {
			entry.name = ReadCString(name_table.data() + name_offset, name_table.size() - name_offset);
		}
		entry.name = NormalizeName(std::move(entry.name));
	}
	return result;
}

[[nodiscard]] Result InspectReader(Reader& reader) {
	Result result {};
	if (reader.Size() < 4) {
		SetError(result, Status::InvalidContainer, "PKG too small");
		return result;
	}
	std::array<uint8_t, 0x100> prefix {};
	const uint64_t prefix_size = std::min<uint64_t>(reader.Size(), prefix.size());
	if (!reader.ReadAt(0, prefix.data(), prefix_size)) {
		SetError(result, Status::IoError, "PKG header read failed");
		return result;
	}
	const std::string_view magic(reinterpret_cast<const char*>(prefix.data()), 4);
	if (magic == "\x7f" "CNT") {
		return ParseCnt(reader, 0, reader.Size(), ContainerKind::Cnt, false);
	}
	if (magic != "\x7f" "FIH" || reader.Size() < 0x10000) {
		SetError(result, Status::InvalidContainer, "unsupported PKG magic");
		return result;
	}
	const bool retail = prefix[5] == 0x80;
	const uint64_t cnt_offset = ReadLe64(prefix.data() + 0x58);
	const uint64_t cnt_size = ReadLe64(prefix.data() + 0xa0);
	if (!InRange(cnt_offset, cnt_size, reader.Size())) {
		SetError(result, Status::InvalidContainer, "PS5 FIH embedded CNT range invalid");
		return result;
	}
	return ParseCnt(reader, cnt_offset, cnt_size, ContainerKind::Ps5FinalizedImage, retail);
}

[[nodiscard]] bool CopyEntry(Reader& reader, uint64_t offset, uint64_t size,
	                             const std::filesystem::path& destination) {
	std::ofstream output(destination, std::ios::binary | std::ios::trunc);
	if (!output) {
		return false;
	}
	std::vector<uint8_t> buffer(static_cast<size_t>(CopyChunkSize));
	while (size != 0) {
		const uint64_t current = std::min<uint64_t>(size, buffer.size());
		if (!reader.ReadAt(offset, buffer.data(), current)) {
			return false;
		}
		output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(current));
		if (!output) {
			return false;
		}
		offset += current;
		size -= current;
	}
	return true;
}

} // namespace

const char* StatusMessage(Status status) {
	switch (status) {
		case Status::Ok: return "ok";
		case Status::FileOpenFailed: return "cannot open PKG";
		case Status::InvalidContainer: return "invalid or unsupported PKG container";
		case Status::UnsupportedEncryptedContent: return "encrypted PFS or entry blocked; no keys or decrypt support";
		case Status::UnsupportedRetailSelf: return "retail SELF blocked; import decrypted legal homebrew ELF";
		case Status::UnsafeEntryPath: return "unsafe PKG entry path";
		case Status::OutputExists: return "output file exists";
		case Status::IoError: return "I/O failure";
		case Status::NoBootExecutable: return "no raw ELF or eboot found";
	}
	return "unknown PKG status";
}

Result Inspect(const std::filesystem::path& package_path) {
	Reader reader(package_path);
	if (!reader.IsOpen()) {
		Result result {};
		SetError(result, Status::FileOpenFailed, StatusMessage(Status::FileOpenFailed));
		return result;
	}
	return InspectReader(reader);
}

std::filesystem::path FindBootExecutable(const std::filesystem::path& app_root) {
	static constexpr std::array<std::string_view, 3> names {"eboot.bin", "eboot.elf", "main.elf"};
	for (const auto name: names) {
		const auto candidate = app_root / name;
		if (std::filesystem::is_regular_file(candidate)) {
			return candidate;
		}
	}
	std::error_code error;
	for (const auto& entry: std::filesystem::recursive_directory_iterator(app_root, error)) {
		if (error) {
			return {};
		}
		if (!entry.is_regular_file()) {
			continue;
		}
		std::string extension = entry.path().extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
		               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		if (entry.path().filename() == "eboot.bin" || extension == ".elf") {
			return entry.path();
		}
	}
	return {};
}

ExecutableKind ClassifyExecutable(const std::filesystem::path& executable) {
	std::array<uint8_t, 4> magic {};
	Reader reader(executable);
	if (!reader.IsOpen() || !reader.ReadAt(0, magic.data(), magic.size())) {
		return ExecutableKind::Missing;
	}
	if (std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()) == "\x7f" "ELF") {
		return ExecutableKind::RawElf;
	}
	return ExecutableKind::SelfOrUnknown;
}

ExtractionResult ExtractHomebrew(const std::filesystem::path& package_path,
	                               const std::filesystem::path& output_directory) {
	ExtractionResult extracted {};
	Reader reader(package_path);
	if (!reader.IsOpen()) {
		SetError(extracted.result, Status::FileOpenFailed, StatusMessage(Status::FileOpenFailed));
		return extracted;
	}
	extracted.result = InspectReader(reader);
	if (extracted.result.status != Status::Ok) {
		return extracted;
	}
	const auto& package = extracted.result.package;
	if (package.kind == ContainerKind::Ps5FinalizedImage || package.retail ||
	    package.contains_encrypted_entries || package.contains_pfs_image) {
		SetError(extracted.result, Status::UnsupportedEncryptedContent,
		         "PKG has PFS/encrypted content; parser only extracts direct plaintext CNT entries");
		return extracted;
	}

	std::error_code error;
	std::filesystem::create_directories(output_directory, error);
	if (error || !std::filesystem::is_directory(output_directory)) {
		SetError(extracted.result, Status::IoError, "cannot create output directory");
		return extracted;
	}
	std::set<std::filesystem::path> targets;
	for (const auto& entry: package.entries) {
		if (entry.id == EntryNamesId || entry.name.empty()) {
			continue;
		}
		if (entry.encrypted) {
			SetError(extracted.result, Status::UnsupportedEncryptedContent,
			         StatusMessage(Status::UnsupportedEncryptedContent));
			return extracted;
		}
		if (!IsSafeRelativePath(entry.name)) {
			SetError(extracted.result, Status::UnsafeEntryPath, "unsafe CNT entry: " + entry.name);
			return extracted;
		}
		const auto target = output_directory / std::filesystem::path(entry.name);
		if (std::filesystem::exists(target) || !targets.insert(target.lexically_normal()).second) {
			SetError(extracted.result, Status::OutputExists, "refusing overwrite: " + target.string());
			return extracted;
		}
	}
	for (const auto& entry: package.entries) {
		if (entry.id == EntryNamesId || entry.name.empty()) {
			continue;
		}
		const auto target = output_directory / std::filesystem::path(entry.name);
		std::filesystem::create_directories(target.parent_path(), error);
		if (error || !CopyEntry(reader, package.container_offset + entry.offset, entry.size, target)) {
			SetError(extracted.result, Status::IoError, "cannot extract: " + entry.name);
			return extracted;
		}
		extracted.extracted_files++;
	}
	extracted.output_directory = output_directory;
	extracted.boot_executable = FindBootExecutable(output_directory);
	extracted.boot_kind = ClassifyExecutable(extracted.boot_executable);
	if (extracted.boot_kind == ExecutableKind::Missing) {
		SetError(extracted.result, Status::NoBootExecutable, StatusMessage(Status::NoBootExecutable));
	} else if (extracted.boot_kind != ExecutableKind::RawElf) {
		SetError(extracted.result, Status::UnsupportedRetailSelf, StatusMessage(Status::UnsupportedRetailSelf));
	}
	return extracted;
}

} // namespace Pkg
