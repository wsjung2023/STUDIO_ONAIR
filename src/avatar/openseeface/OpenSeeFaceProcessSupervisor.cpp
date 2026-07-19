#include "avatar/openseeface/OpenSeeFaceProcessSupervisor.h"

#include "core/AppError.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace creator::avatar::openseeface {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length) <= 0) {
        return {};
    }
    return result;
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
    if (!value.empty() &&
        value.find_first_of(L" \t\r\n\"") == std::wstring::npos) {
        return value;
    }
    std::wstring quoted{L'"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

AppError processError(const char* action) {
    return AppError{ErrorCode::IoFailure,
                    std::string(action) + " (Windows error " +
                        std::to_string(GetLastError()) + ")"};
}

#else

AppError processError(const char* action) {
    return AppError{ErrorCode::IoFailure,
                    std::string(action) + ": " + std::strerror(errno)};
}

#endif

}  // namespace

class OpenSeeFaceProcessSupervisor::Impl final {
public:
    ~Impl() { stop(); }

    Result<void> start(const OpenSeeFaceProcessConfig& config) {
        if (running()) {
            return AppError{ErrorCode::InvalidState,
                            "OpenSeeFace worker is already running"};
        }
        // running() is non-blocking and does not reap an exited worker. Reap
        // that old child before replacing its handle/pid, otherwise repeated
        // sessions leak a Windows handle or leave a POSIX zombie.
        auto previousExit = pollExitCode();
        if (!previousExit.hasValue()) return previousExit.error();
        if (config.executable.empty()) {
            return AppError{ErrorCode::InvalidArgument,
                            "OpenSeeFace worker executable is empty"};
        }
#ifdef _WIN32
        const std::wstring executable = config.executable.wstring();
        std::wstring commandLine = quoteWindowsArgument(executable);
        for (const auto& argument : config.arguments) {
            const auto wideArgument = utf8ToWide(argument);
            if (!argument.empty() && wideArgument.empty()) {
                return AppError{ErrorCode::InvalidArgument,
                                "OpenSeeFace worker argument is not valid UTF-8"};
            }
            commandLine.push_back(L' ');
            commandLine += quoteWindowsArgument(wideArgument);
        }
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const std::wstring workingDirectory = config.workingDirectory.empty()
                                                   ? std::wstring{}
                                                   : config.workingDirectory.wstring();
        if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                            FALSE, CREATE_NO_WINDOW, nullptr,
                            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                            &startup, &process)) {
            return processError("OpenSeeFace worker launch failed");
        }
        CloseHandle(process.hThread);
        processHandle_ = process.hProcess;
#else
        const pid_t child = fork();
        if (child < 0) return processError("OpenSeeFace worker fork failed");
        if (child == 0) {
            if (!config.workingDirectory.empty()) {
                const auto directory = config.workingDirectory.string();
                (void)chdir(directory.c_str());
            }
            std::vector<std::string> arguments;
            arguments.reserve(config.arguments.size() + 1);
            arguments.push_back(config.executable.string());
            arguments.insert(arguments.end(), config.arguments.begin(), config.arguments.end());
            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);
            for (auto& argument : arguments) argv.push_back(argument.data());
            argv.push_back(nullptr);
            execv(argv.front(), argv.data());
            _exit(127);
        }
        processId_ = child;
#endif
        return core::ok();
    }

    Result<std::optional<int>> pollExitCode() {
#ifdef _WIN32
        if (processHandle_ == nullptr) return std::optional<int>{};
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(processHandle_, &exitCode) == 0) {
            return processError("OpenSeeFace worker exit-code query failed");
        }
        if (exitCode == STILL_ACTIVE) return std::optional<int>{};
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
        return std::optional<int>{static_cast<int>(exitCode)};
#else
        if (processId_ <= 0) return std::optional<int>{};
        int status = 0;
        const pid_t result = waitpid(processId_, &status, WNOHANG);
        if (result == 0) return std::optional<int>{};
        if (result < 0) {
            if (errno == ECHILD) {
                processId_ = -1;
                return AppError{ErrorCode::InvalidState,
                                "OpenSeeFace worker is no longer waitable"};
            }
            return processError("OpenSeeFace worker exit-code query failed");
        }
        processId_ = -1;
        if (WIFEXITED(status)) return std::optional<int>{WEXITSTATUS(status)};
        if (WIFSIGNALED(status)) return std::optional<int>{128 + WTERMSIG(status)};
        return AppError{ErrorCode::IoFailure,
                        "OpenSeeFace worker ended in an unknown state"};
#endif
    }

    void stop() noexcept {
#ifdef _WIN32
        if (processHandle_ == nullptr) return;
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(processHandle_, &exitCode) != 0 &&
            exitCode == STILL_ACTIVE) {
            (void)TerminateProcess(processHandle_, 1);
            (void)WaitForSingleObject(processHandle_, 2000);
        }
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
#else
        if (processId_ <= 0) return;
        if (kill(processId_, SIGTERM) == 0) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                int status = 0;
                const pid_t result = waitpid(processId_, &status, WNOHANG);
                if (result == processId_ || (result < 0 && errno == ECHILD)) {
                    processId_ = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            (void)kill(processId_, SIGKILL);
        }
        (void)waitpid(processId_, nullptr, 0);
        processId_ = -1;
#endif
    }

    [[nodiscard]] bool running() const noexcept {
#ifdef _WIN32
        if (processHandle_ == nullptr) return false;
        DWORD exitCode = STILL_ACTIVE;
        return GetExitCodeProcess(processHandle_, &exitCode) != 0 &&
               exitCode == STILL_ACTIVE;
#else
        if (processId_ <= 0) return false;
        int status = 0;
        const pid_t result = waitpid(processId_, &status, WNOHANG);
        return result == 0;
#endif
    }

private:
#ifdef _WIN32
    HANDLE processHandle_{nullptr};
#else
    pid_t processId_{-1};
#endif
};

OpenSeeFaceProcessSupervisor::OpenSeeFaceProcessSupervisor()
    : impl_(std::make_unique<Impl>()) {}
OpenSeeFaceProcessSupervisor::~OpenSeeFaceProcessSupervisor() = default;

Result<void> OpenSeeFaceProcessSupervisor::start(
    const OpenSeeFaceProcessConfig& config) {
    return impl_->start(config);
}

Result<std::optional<int>> OpenSeeFaceProcessSupervisor::pollExitCode() {
    return impl_->pollExitCode();
}

void OpenSeeFaceProcessSupervisor::stop() noexcept { impl_->stop(); }
bool OpenSeeFaceProcessSupervisor::running() const noexcept { return impl_->running(); }

}  // namespace creator::avatar::openseeface
