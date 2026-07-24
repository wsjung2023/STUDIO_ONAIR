#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

bool writeReady(const std::filesystem::path& path) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << "ready";
    output.close();
    return static_cast<bool>(output);
}

bool waitForRelease(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < 3'000; ++attempt) {
        if (std::filesystem::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::filesystem::path lockPath{argv[1]};
    const std::filesystem::path readyPath{argv[2]};
    const std::filesystem::path releasePath{argv[3]};
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 3;
    OVERLAPPED overlap{};
    const bool locked =
        LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0U, MAXDWORD, MAXDWORD, &overlap) != FALSE;
    if (!locked) {
        CloseHandle(handle);
        return 4;
    }
    const bool completed = writeReady(readyPath) && waitForRelease(releasePath);
    UnlockFileEx(handle, 0U, MAXDWORD, MAXDWORD, &overlap);
    CloseHandle(handle);
    return completed ? 0 : 5;
#else
    const int descriptor =
        ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
               0600);
    if (descriptor < 0) return 3;
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return 4;
    }
    const bool completed = writeReady(readyPath) && waitForRelease(releasePath);
    (void)::flock(descriptor, LOCK_UN);
    (void)::close(descriptor);
    return completed ? 0 : 5;
#endif
}
