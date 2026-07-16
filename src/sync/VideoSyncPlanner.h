#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "media/MediaTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace creator::synchronization {

struct VideoSyncBatch final {
    std::vector<media::VideoFrame> frames;
};

struct VideoSyncSnapshot final {
    std::uint64_t framesPassed{0};
    std::uint64_t framesDropped{0};
    std::uint64_t framesDuplicated{0};
    std::uint64_t gridIntervalsSkipped{0};
};

class VideoSyncPlanner final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<VideoSyncPlanner>> create(
        core::DurationNs framePeriod, std::size_t maximumDuplicatesPerInput = 2);

    VideoSyncPlanner(const VideoSyncPlanner&) = delete;
    VideoSyncPlanner& operator=(const VideoSyncPlanner&) = delete;
    ~VideoSyncPlanner();

    [[nodiscard]] core::Result<VideoSyncBatch> plan(
        media::VideoFrame frame, core::TimestampNs correctedTimestamp);
    [[nodiscard]] VideoSyncSnapshot snapshot() const noexcept;

private:
    class Impl;
    explicit VideoSyncPlanner(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::synchronization
