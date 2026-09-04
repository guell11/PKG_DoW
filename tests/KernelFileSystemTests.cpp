#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

namespace FileSystem = Libs::LibKernel::FileSystem;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "KernelFileSystemTests: failed: %s\n", text);
    std::abort();
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("kyty_kernel_file_system_" + std::to_string(unique));
    Check(std::filesystem::create_directories(m_path),
          "create temporary directory");
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  [[nodiscard]] const std::filesystem::path &Path() const { return m_path; }

  KYTY_CLASS_NO_COPY(TempDirectory);

private:
  std::filesystem::path m_path;
};

void CheckSaveRename(const std::filesystem::path &root,
                     std::string_view payload) {
  constexpr char Source[] = "/savedata0/STEMP000.DAT";
  constexpr char Target[] = "/savedata0/SDATA000.DAT";
  constexpr char Suffix[] = "-after-rename";

  const int fd = FileSystem::KernelOpen(Source, 0x601, 0777);
  Check(fd >= 3, "open temporary save file");
  Check(FileSystem::KernelWrite(fd, payload.data(), payload.size()) ==
            payload.size(),
        "write save payload");
  Check(FileSystem::KernelRename(Source, Target) == OK,
        "rename open save file");
  Check(FileSystem::KernelWrite(fd, Suffix, sizeof(Suffix) - 1) ==
            sizeof(Suffix) - 1,
        "write through renamed descriptor");
  Check(FileSystem::KernelClose(fd) == OK, "close renamed descriptor");

  Common::File result(root / "SDATA000.DAT", Common::File::Mode::Read);
  Check(!result.IsInvalid(), "open renamed save file");
  const auto data = result.ReadWholeBuffer();
  result.Close();
  const std::string expected = std::string(payload) + Suffix;
  Check(data.Size() == expected.size(), "renamed save size");
  Check(std::memcmp(data.GetData(), expected.data(), expected.size()) == 0,
        "renamed save contents");
}

void CheckRelativeGuestPaths(const std::filesystem::path &app_root) {
  constexpr char MarkerName[] = "relative-marker.txt";
  constexpr char MarkerData[] = "app0-data";

  Check(std::filesystem::create_directories(app_root), "create app0 directory");
  Common::File marker;
  Check(marker.Create(app_root / MarkerName), "create app0 marker");
  marker.Write(MarkerData, sizeof(MarkerData) - 1);
  marker.Close();
  FileSystem::Mount(app_root, "/app0");

  const int directory_fd = FileSystem::KernelOpen(".", 0x00020000, 0);
  Check(directory_fd >= 3, "open relative guest directory");

  char dirents[512] {};
  const auto dirents_size =
      FileSystem::KernelGetdirentries(directory_fd, dirents, sizeof(dirents), nullptr);
  Check(dirents_size > 0, "read relative guest directory");

  bool found_marker = false;
  for (int64_t offset = 0; offset + 8 <= dirents_size;) {
    uint16_t reclen = 0;
    std::memcpy(&reclen, dirents + offset + 4, sizeof(reclen));
    const auto namlen = static_cast<uint8_t>(dirents[offset + 7]);
    Check(reclen >= 8 && offset + reclen <= dirents_size,
          "validate relative guest directory entry");
    if (namlen == sizeof(MarkerName) - 1 &&
        std::memcmp(dirents + offset + 8, MarkerName, sizeof(MarkerName) - 1) == 0) {
      found_marker = true;
    }
    offset += reclen;
  }
  Check(found_marker, "relative guest directory lists app0 file");
  Check(FileSystem::KernelClose(directory_fd) == OK, "close relative guest directory");

  const int file_fd = FileSystem::KernelOpen("relative-marker.txt", 0, 0);
  Check(file_fd >= 3, "open relative guest file");
  Check(FileSystem::KernelClose(file_fd) == OK, "close relative guest file");

  Common::File mapped_file(app_root / MarkerName, Common::File::Mode::Read);
  Check(!mapped_file.IsInvalid(), "open mapped app0 marker");
  const auto mapped_data = mapped_file.ReadWholeBuffer();
  mapped_file.Close();
  Check(mapped_data.Size() == sizeof(MarkerData) - 1,
        "relative guest file size");
  Check(std::memcmp(mapped_data.GetData(), MarkerData, sizeof(MarkerData) - 1) == 0,
        "relative guest file contents");
}

} // namespace

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::OutputDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

  TempDirectory temporary;
  FileSystem::Initialize();
  FileSystem::Mount(temporary.Path(), "/savedata0");
  CheckRelativeGuestPaths(temporary.Path() / "app0");
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  FileSystem::Shutdown();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
