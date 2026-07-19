#include "avatar/openseeface/OpenSeeFaceProcessSupervisor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>

namespace {

using creator::avatar::openseeface::OpenSeeFaceProcessConfig;
using creator::avatar::openseeface::OpenSeeFaceProcessSupervisor;
using creator::core::ErrorCode;

#ifdef _WIN32
std::filesystem::path commandShellPath() {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "ComSpec") == 0 && value != nullptr) {
        const std::filesystem::path result{value};
        free(value);
        return result;
    }
    return std::filesystem::path{R"(C:\Windows\System32\cmd.exe)"};
}
#endif

OpenSeeFaceProcessConfig shortLivedProcess() {
#ifdef _WIN32
    return OpenSeeFaceProcessConfig{commandShellPath(), {"/C", "exit 7"}, {}};
#else
    return OpenSeeFaceProcessConfig{"/bin/sh", {"-c", "exit 7"}, {}};
#endif
}

TEST(OpenSeeFaceProcessSupervisorTest, RejectsEmptyExecutable) {
    OpenSeeFaceProcessSupervisor supervisor;
    const auto result = supervisor.start(OpenSeeFaceProcessConfig{});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(supervisor.running());
}

TEST(OpenSeeFaceProcessSupervisorTest, LaunchesAndReapsWorkerExitCode) {
    OpenSeeFaceProcessSupervisor supervisor;
    ASSERT_TRUE(supervisor.start(shortLivedProcess()).hasValue());

    std::optional<int> exitCode;
    for (int attempt = 0; attempt < 200 && !exitCode.has_value(); ++attempt) {
        auto result = supervisor.pollExitCode();
        ASSERT_TRUE(result.hasValue()) << result.error().message();
        exitCode = result.value();
        if (!exitCode.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    ASSERT_TRUE(exitCode.has_value());
    EXPECT_EQ(*exitCode, 7);
    EXPECT_FALSE(supervisor.running());

    ASSERT_TRUE(supervisor.start(shortLivedProcess()).hasValue());
    supervisor.stop();
    EXPECT_FALSE(supervisor.running());
}

TEST(OpenSeeFaceProcessSupervisorTest, RejectsConcurrentStart) {
    OpenSeeFaceProcessSupervisor supervisor;
#ifdef _WIN32
    const auto executable = commandShellPath();
    const OpenSeeFaceProcessConfig config{executable, {"/C", "ping -n 3 127.0.0.1 > nul"}, {}};
#else
    const OpenSeeFaceProcessConfig config{"/bin/sh", {"-c", "sleep 1"}, {}};
#endif
    ASSERT_TRUE(supervisor.start(config).hasValue());
    const auto secondStart = supervisor.start(config);
    ASSERT_FALSE(secondStart.hasValue());
    EXPECT_EQ(secondStart.error().code(), ErrorCode::InvalidState);
    supervisor.stop();
}

}  // namespace
