#pragma once

#include "media/IMediaProbe.h"

#include <filesystem>
#include <map>
#include <utility>

namespace creator::fakes {

class FakeMediaProbe final : public media::IMediaProbe {
public:
    void set(std::filesystem::path relativePath,
             core::Result<media::MediaProbeResult> result) {
        results_.insert_or_assign(std::move(relativePath), std::move(result));
    }

    [[nodiscard]] core::Result<media::MediaProbeResult> probe(
        const std::filesystem::path&,
        const std::filesystem::path& relativePath) override {
        const auto found = results_.find(relativePath);
        if (found == results_.end()) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "fake media result was not configured"};
        }
        return found->second;
    }

private:
    std::map<std::filesystem::path,
             core::Result<media::MediaProbeResult>> results_;
};

}  // namespace creator::fakes
