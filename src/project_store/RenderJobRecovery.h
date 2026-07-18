#pragma once

#include "project_store/IRenderJobStore.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace creator::project_store {

struct RenderArtifactEvidence final {
    std::uint64_t byteSize{};
    std::string sha256;
    std::string identity;

    friend bool operator==(const RenderArtifactEvidence&,
                           const RenderArtifactEvidence&) = default;
};

struct RenderRecoveryOutcome final {
    domain::RenderJobId jobId;
    edit_engine::RenderJobState state;
    std::string diagnostic;
};

[[nodiscard]] core::Result<RenderArtifactEvidence> inspectRenderArtifact(
    const std::filesystem::path& path);

class RenderJobRecovery final {
public:
    [[nodiscard]] static core::Result<std::vector<RenderRecoveryOutcome>>
    recoverAll(IRenderJobStore& store, core::Utc now);
};

}  // namespace creator::project_store
