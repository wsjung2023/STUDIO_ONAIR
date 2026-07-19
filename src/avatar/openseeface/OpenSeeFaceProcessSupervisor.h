#pragma once

#include "core/Result.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace creator::avatar::openseeface {

/// Launch configuration for an optional OpenSeeFace worker.
///
/// The supervisor deliberately accepts the complete argument list instead of
/// inventing a command-line contract for a particular OpenSeeFace build. This
/// keeps the app compatible with both the Python worker and packaged builds,
/// while still giving the session an owned process whose lifetime it can
/// close deterministically.
struct OpenSeeFaceProcessConfig final {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
};

/// Owns one externally launched tracking worker.
///
/// This class does not read tracking packets and does not own a socket; pair
/// it with OpenSeeFaceUdpSource and AvatarTrackingSession. It is intentionally
/// synchronous and thread-free: start/stop are explicit, running() is a
/// non-blocking liveness check, and pollExitCode() reaps a completed worker.
class OpenSeeFaceProcessSupervisor final {
public:
    OpenSeeFaceProcessSupervisor();
    ~OpenSeeFaceProcessSupervisor();

    OpenSeeFaceProcessSupervisor(const OpenSeeFaceProcessSupervisor&) = delete;
    OpenSeeFaceProcessSupervisor& operator=(const OpenSeeFaceProcessSupervisor&) = delete;
    OpenSeeFaceProcessSupervisor(OpenSeeFaceProcessSupervisor&&) = delete;
    OpenSeeFaceProcessSupervisor& operator=(OpenSeeFaceProcessSupervisor&&) = delete;

    [[nodiscard]] core::Result<void> start(const OpenSeeFaceProcessConfig& config);
    /// Returns the exit code once the worker has exited, otherwise no value.
    [[nodiscard]] core::Result<std::optional<int>> pollExitCode();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::avatar::openseeface
