#include "project_store/ProjectPackageStore.h"

#include "core/Utc.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef CS_CRASH_FIXTURE_PATH
#error CS_CRASH_FIXTURE_PATH must name the crash fixture executable
#endif

namespace {

namespace fs = std::filesystem;

using creator::core::Utc;
using creator::project_store::ProjectPackageStore;

int runFixture(const fs::path& packagePath) {
    const fs::path executable{CS_CRASH_FIXTURE_PATH};
#ifdef _WIN32
    std::wstring command = L"\"" + executable.wstring() + L"\" \"" +
                           packagePath.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) {
        return -1;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = static_cast<DWORD>(-1);
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
#else
    const pid_t child = ::fork();
    if (child == 0) {
        const std::string executableText = executable.string();
        const std::string packageText = packagePath.string();
        ::execl(executableText.c_str(), executableText.c_str(), packageText.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (child < 0) return -1;
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

using FileSnapshot = std::vector<std::pair<fs::path, std::string>>;

FileSnapshot snapshotFixtureFiles(const fs::path& packagePath) {
    FileSnapshot result;
    const fs::path media = packagePath / "media";
    std::error_code ec;
    for (fs::recursive_directory_iterator it{media, ec};
         !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        std::ifstream in{it->path(), std::ios::binary};
        result.emplace_back(fs::relative(it->path(), packagePath, ec),
                            std::string{std::istreambuf_iterator<char>{in},
                                        std::istreambuf_iterator<char>{}});
    }
    std::sort(result.begin(), result.end());
    return result;
}

class CrashRecoveryIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        packagePath_ = fs::temp_directory_path() /
                       ("cs_crash_" + std::string{info->name()} + ".cstudio");
        std::error_code ec;
        fs::remove_all(packagePath_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(packagePath_, ec);
    }

    fs::path packagePath_;
};

TEST_F(CrashRecoveryIntegrationTest, ReopensAndRecoversAfterExitWithoutDestructors) {
    ASSERT_EQ(runFixture(packagePath_), 73);

    ProjectPackageStore store;
    const auto opened = store.open(packagePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    ASSERT_EQ(opened.value().recoveryCandidates.size(), 1u);

    const auto candidate = opened.value().recoveryCandidates.front();
    const auto before = snapshotFixtureFiles(packagePath_);
    ASSERT_EQ(before.size(), 1u);
    const fs::path interruptedPart =
        packagePath_ / ".tmp/media/screen-1/writing.mkv.part";
    ASSERT_TRUE(fs::is_regular_file(interruptedPart));
    const auto recovered = store.recover(packagePath_, candidate.sessionId, Utc::now());
    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(recovered.value().readySegments, 1u);
    EXPECT_EQ(recovered.value().failedSegments, 1u);
    EXPECT_EQ(recovered.value().quarantinedParts, 1u);
    EXPECT_EQ(recovered.value().orphanParts, 0u);
    EXPECT_EQ(snapshotFixtureFiles(packagePath_), before);
    EXPECT_FALSE(fs::exists(interruptedPart));
    EXPECT_TRUE(fs::is_regular_file(
        packagePath_ /
        "recovery/quarantine/session-crash/media/screen-1/writing.mkv.part"));
    const auto reopened = store.open(packagePath_);
    ASSERT_TRUE(reopened.hasValue());
    EXPECT_TRUE(reopened.value().recoveryCandidates.empty());
}

}  // namespace
