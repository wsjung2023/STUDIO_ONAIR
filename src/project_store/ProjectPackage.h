#pragma once

#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"

#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>

namespace creator::project_store {

enum class PersistedSessionState { Recording, Completed, Recovered, Aborted };

struct RecordingSessionRecord final {
    creator::domain::SessionId id;
    PersistedSessionState state{PersistedSessionState::Recording};
    creator::core::TimestampNs startedAt{};
    std::optional<creator::core::TimestampNs> stoppedAt;
    creator::core::Utc createdAt;
    std::optional<creator::core::Utc> finishedAt;
    std::optional<std::string> failureReason;
};

struct ProjectPackage final {
    std::filesystem::path path;
    creator::domain::ProjectManifest manifest;
};

struct RecoveryCandidate final {
    std::filesystem::path packagePath;
    std::string projectName;
    creator::domain::SessionId sessionId;
    creator::core::Utc createdAt;
    std::size_t readySegments{0};
    std::size_t writingSegments{0};
};

struct RecoveryResult final {
    creator::domain::SessionId sessionId;
    creator::core::TimestampNs stoppedAt{};
    std::size_t readySegments{0};
    std::size_t failedSegments{0};
};

}  // namespace creator::project_store
