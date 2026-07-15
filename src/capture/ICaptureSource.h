#pragma once

#include "domain/Identifiers.h"

#include <cstdint>
#include <string>

namespace creator::capture {

struct CaptureConfig final {
    std::uint32_t targetWidth{1920};
    std::uint32_t targetHeight{1080};
    std::uint32_t frameRateNumerator{60};
    std::uint32_t frameRateDenominator{1};
};

struct CaptureStats final {
    std::uint64_t receivedFrames{0};
    std::uint64_t droppedFrames{0};
    double currentFps{0.0};
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    [[nodiscard]] virtual creator::domain::SourceId id() const = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;
    virtual bool start(const CaptureConfig& config) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual CaptureStats stats() const noexcept = 0;
};

}  // namespace creator::capture
