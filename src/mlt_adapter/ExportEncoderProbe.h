#pragma once

#include "core/Result.h"
#include "edit_engine/EditEngineTypes.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace creator::mlt_adapter {

struct ExportEncoderCandidate final {
    std::string id;
    std::string videoCodec;
    bool hardware{};
    bool forceMediaFoundationHardware{};

    friend bool operator==(const ExportEncoderCandidate&,
                           const ExportEncoderCandidate&) = default;
};

struct ExportEncoderAttempt final {
    ExportEncoderCandidate candidate;
    bool succeeded{};
    std::string diagnostic;

    friend bool operator==(const ExportEncoderAttempt&,
                           const ExportEncoderAttempt&) = default;
};

struct ExportEncoderSelection final {
    ExportEncoderCandidate selected;
    std::vector<ExportEncoderAttempt> attempts;
};

class ExportEncoderProbe final {
public:
    using Attempt = std::function<core::Result<void>(
        const ExportEncoderCandidate&, const edit_engine::RenderPreset&)>;

    [[nodiscard]] static core::Result<ExportEncoderSelection> select(
        const edit_engine::RenderPreset& preset, const Attempt& attempt);

    [[nodiscard]] static core::Result<ExportEncoderSelection> probe(
        const std::filesystem::path& runtimeRoot,
        const std::filesystem::path& scratchDirectory,
        const edit_engine::RenderPreset& preset);
};

}  // namespace creator::mlt_adapter
