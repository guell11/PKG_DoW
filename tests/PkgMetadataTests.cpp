#include "loader/pkg.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kPackageSize = 0x400;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PkgMetadataTests: failed: %s\n", message);
		std::abort();
	}
}

void PutBe32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
	data[offset] = static_cast<uint8_t>(value >> 24u);
	data[offset + 1] = static_cast<uint8_t>(value >> 16u);
	data[offset + 2] = static_cast<uint8_t>(value >> 8u);
	data[offset + 3] = static_cast<uint8_t>(value);
}

void PutBe64(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
	PutBe32(data, offset, static_cast<uint32_t>(value >> 32u));
	PutBe32(data, offset + 4, static_cast<uint32_t>(value));
}

std::vector<uint8_t> MakePackage(std::string_view content_id) {
	std::vector<uint8_t> data(kPackageSize);
	PutBe32(data, 0x00, 0x7f434e54);
	PutBe32(data, 0x04, 1);
	PutBe32(data, 0x0c, 1);
	PutBe32(data, 0x10, 1);
	PutBe32(data, 0x18, 0x80);
	PutBe32(data, 0x1c, 0x20);
	PutBe64(data, 0x20, 0x100);
	PutBe64(data, 0x28, 0x100);
	PutBe64(data, 0x30, 0x200);
	PutBe64(data, 0x38, 0x100);
	for (size_t index = 0; index < content_id.size() && index < 0x24; ++index) {
		data[0x40 + index] = static_cast<uint8_t>(content_id[index]);
	}
	PutBe32(data, 0x80, 0x1000);
	PutBe32(data, 0x84, 0x12);
	PutBe32(data, 0x88, 0x11223344);
	PutBe32(data, 0x8c, 0x55667788);
	PutBe32(data, 0x90, 0x100);
	PutBe32(data, 0x94, 0x20);
	return data;
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
	std::ofstream file(path, std::ios::binary);
	Check(file.is_open(), "could not create fixture");
	file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	Check(file.good(), "could not write fixture");
}

void TestValidMetadata(const std::filesystem::path& root) {
	const auto path = root / "homebrew.pkg";
	WriteFile(path, MakePackage("UP0000-LAPY20014_00-HOMEBREW00000000"));
	const auto result = Loader::Pkg::ParseMetadata(path);
	Check(static_cast<bool>(result), "valid package rejected");
	Check(result.info.package_type == 1 && result.info.file_count == 1, "header fields wrong");
	Check(result.info.title_id == "LAPY20014", "title id wrong");
	Check(result.info.platform == Loader::Pkg::Platform::Ps4, "PS4 title id not recognized");
	Check(result.info.homebrew == Loader::Pkg::HomebrewStatus::Declared, "homebrew declaration missed");
	Check(result.info.entries.size() == 1, "entry count wrong");
	const auto& entry = result.info.entries.front();
	Check(entry.id == 0x1000 && entry.offset == 0x100 && entry.size == 0x20, "entry fields wrong");
}

void TestPs5AndRejectedRanges(const std::filesystem::path& root) {
	const auto ps5 = root / "ps5.pkg";
	WriteFile(ps5, MakePackage("IP0000-PPSA12345_00-EXTRACTEDTEST000"));
	const auto ps5_result = Loader::Pkg::ParseMetadata(ps5);
	Check(ps5_result && ps5_result.info.platform == Loader::Pkg::Platform::Ps5, "PS5 title id not recognized");
	Check(ps5_result.info.homebrew == Loader::Pkg::HomebrewStatus::Indeterminate, "commercial status guessed");

	auto bad_table = MakePackage("UP0000-CUSA12345_00-TEST000000000000");
	PutBe32(bad_table, 0x18, 0x3f0);
	const auto bad_table_path = root / "bad-table.pkg";
	WriteFile(bad_table_path, bad_table);
	Check(Loader::Pkg::ParseMetadata(bad_table_path).error == Loader::Pkg::Error::InvalidRange,
	      "out-of-bounds table accepted");

	auto bad_entry = MakePackage("UP0000-CUSA12345_00-TEST000000000000");
	PutBe32(bad_entry, 0x90, 0x3ff);
	PutBe32(bad_entry, 0x94, 2);
	const auto bad_entry_path = root / "bad-entry.pkg";
	WriteFile(bad_entry_path, bad_entry);
	Check(Loader::Pkg::ParseMetadata(bad_entry_path).error == Loader::Pkg::Error::InvalidTable,
	      "out-of-bounds entry accepted");
}

void TestExtractedExecutableLookup(const std::filesystem::path& root) {
	const auto extracted = root / "extracted";
	std::filesystem::create_directories(extracted / "Image0" / "sce_module");
	const auto eboot = extracted / "Image0" / "sce_module" / "eboot.bin";
	WriteFile(eboot, {0x53, 0x45, 0x4c, 0x46});
	Check(Loader::Pkg::FindExtractedExecutable(extracted) == eboot, "Image0 eboot not found");
	std::filesystem::remove(eboot);
	const auto elf = extracted / "Image0" / "fallback.elf";
	WriteFile(elf, {0x7f, 'E', 'L', 'F'});
	Check(Loader::Pkg::FindExtractedExecutable(extracted) == elf, "ELF fallback not found");
}

} // namespace

int main() {
	const auto root = std::filesystem::temp_directory_path() / "kyty-pkg-metadata-tests";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root, error);
	Check(!error, "could not create temporary directory");
	TestValidMetadata(root);
	TestPs5AndRejectedRanges(root);
	TestExtractedExecutableLookup(root);
	std::filesystem::remove_all(root, error);
	Check(!error, "could not remove temporary directory");
	return 0;
}
