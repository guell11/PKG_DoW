#include "pkg/pkg.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PkgParserTests: failed: %s\n", message);
		std::abort();
	}
}

class TempDirectory {
public:
	TempDirectory() {
		m_path = std::filesystem::temp_directory_path() /
		         ("kyty_pkg_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Check(std::filesystem::create_directories(m_path), "create temp dir");
	}
	~TempDirectory() {
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}
	const std::filesystem::path& Path() const { return m_path; }
private:
	std::filesystem::path m_path;
};

void PutBe32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
	data[offset] = static_cast<uint8_t>(value >> 24u);
	data[offset + 1] = static_cast<uint8_t>(value >> 16u);
	data[offset + 2] = static_cast<uint8_t>(value >> 8u);
	data[offset + 3] = static_cast<uint8_t>(value);
}

void PutBe16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
	data[offset] = static_cast<uint8_t>(value >> 8u);
	data[offset + 1] = static_cast<uint8_t>(value);
}

void PutLe64(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
	for (uint32_t i = 0; i < 8; i++) {
		data[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
	}
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	Check(static_cast<bool>(input), "open fixture");
	const auto size = static_cast<size_t>(input.tellg());
	std::vector<uint8_t> result(size);
	input.seekg(0);
	input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
	Check(static_cast<bool>(input), "read fixture");
	return result;
}

std::filesystem::path WritePlainCnt(const std::filesystem::path& path, bool encrypted, bool unsafe_name) {
	constexpr uint32_t table = 0x600;
	constexpr uint32_t names = 0x700;
	constexpr uint32_t elf = 0x740;
	const std::string name_table = unsafe_name ? std::string("\0../escape.elf\0", 15) :
	                                              std::string("\0eboot.elf\0", 11);
	std::vector<uint8_t> pkg(elf + 8, 0);
	std::memcpy(pkg.data(), "\x7f" "CNT", 4);
	PutBe32(pkg, 0x10, 2);
	PutBe16(pkg, 0x16, 2);
	PutBe32(pkg, 0x18, table);
	std::memcpy(pkg.data() + 0x40, "HB0000-PPSA00000_00-TESTHOMEBREW0000", 36);

	PutBe32(pkg, table + 0x00, 0x0200);
	PutBe32(pkg, table + 0x10, names);
	PutBe32(pkg, table + 0x14, static_cast<uint32_t>(name_table.size()));
	PutBe32(pkg, table + 0x20, 0x9000);
	PutBe32(pkg, table + 0x24, 1);
	PutBe32(pkg, table + 0x28, encrypted ? 0x80000000u : 0);
	PutBe32(pkg, table + 0x30, elf);
	PutBe32(pkg, table + 0x34, 8);
	std::memcpy(pkg.data() + names, name_table.data(), name_table.size());
	std::memcpy(pkg.data() + elf, "\x7f" "ELF\2\1\1\0", 8);
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(pkg.data()), static_cast<std::streamsize>(pkg.size()));
	Check(static_cast<bool>(file), "write fixture");
	return path;
}

void TestPlainExtraction(const std::filesystem::path& root) {
	const auto pkg = WritePlainCnt(root / "plain.pkg", false, false);
	const auto info = Pkg::Inspect(pkg);
	Check(info.status == Pkg::Status::Ok, "inspect plaintext CNT");
	Check(info.package.kind == Pkg::ContainerKind::Cnt, "CNT type");
	Check(info.package.entries.size() == 2, "entry count");
	const auto extraction = Pkg::ExtractHomebrew(pkg, root / "plain");
	Check(extraction.result.status == Pkg::Status::Ok, "extract plaintext CNT");
	Check(extraction.extracted_files == 1, "one content entry copied");
	Check(extraction.boot_kind == Pkg::ExecutableKind::RawElf, "raw ELF detected");
	Check(std::filesystem::is_regular_file(root / "plain" / "eboot.elf"), "eboot written");
}

void TestEncryptedAndUnsafeRejected(const std::filesystem::path& root) {
	const auto encrypted = Pkg::ExtractHomebrew(WritePlainCnt(root / "encrypted.pkg", true, false), root / "encrypted");
	Check(encrypted.result.status == Pkg::Status::UnsupportedEncryptedContent, "encrypted entry blocked");
	const auto unsafe = Pkg::ExtractHomebrew(WritePlainCnt(root / "unsafe.pkg", false, true), root / "unsafe");
	Check(unsafe.result.status == Pkg::Status::UnsafeEntryPath, "path traversal blocked");
}

void TestInvalidRejected(const std::filesystem::path& root) {
	std::ofstream bad(root / "bad.pkg", std::ios::binary);
	bad.write("not-a-pkg", 9);
	bad.close();
	const auto result = Pkg::Inspect(root / "bad.pkg");
	Check(result.status == Pkg::Status::InvalidContainer, "bad magic rejected");
}

void TestPs5FinalizedImageBlocked(const std::filesystem::path& root) {
	const auto cnt = ReadAll(WritePlainCnt(root / "embedded.cnt", false, false));
	constexpr uint64_t cnt_offset = 0x10000;
	std::vector<uint8_t> fih(static_cast<size_t>(cnt_offset) + cnt.size(), 0);
	std::memcpy(fih.data(), "\x7f" "FIH", 4);
	fih[5] = 0x80; // Retail finalized image marker.
	PutLe64(fih, 0x58, cnt_offset);
	PutLe64(fih, 0xa0, cnt.size());
	std::memcpy(fih.data() + cnt_offset, cnt.data(), cnt.size());
	const auto pkg = root / "retail.ps5.pkg";
	std::ofstream output(pkg, std::ios::binary);
	output.write(reinterpret_cast<const char*>(fih.data()), static_cast<std::streamsize>(fih.size()));
	Check(static_cast<bool>(output), "write FIH fixture");
	output.close();

	const auto info = Pkg::Inspect(pkg);
	Check(info.status == Pkg::Status::Ok, "inspect PS5 FIH");
	Check(info.package.kind == Pkg::ContainerKind::Ps5FinalizedImage && info.package.retail,
	      "PS5 retail marker");
	const auto extraction = Pkg::ExtractHomebrew(pkg, root / "retail");
	Check(extraction.result.status == Pkg::Status::UnsupportedEncryptedContent,
	      "PS5 finalized image blocked");
}

} // namespace

int main() {
	TempDirectory temp;
	TestPlainExtraction(temp.Path());
	TestEncryptedAndUnsafeRejected(temp.Path());
	TestInvalidRejected(temp.Path());
	TestPs5FinalizedImageBlocked(temp.Path());
	std::printf("PkgParserTests: all cases passed\n");
	return 0;
}
