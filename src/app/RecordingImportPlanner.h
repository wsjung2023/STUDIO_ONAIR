#pragma once

#include "core/Result.h"
#include "domain/MediaAsset.h"
#include "domain/Segment.h"
#include "domain/StudioScene.h"
#include "domain/Timeline.h"
#include "media/IMediaProbe.h"
#include "project_store/IStudioStore.h"

#include <string>
#include <vector>

namespace creator::app {

struct RecordingSegmentProbe final {
    std::string relativePath;
    media::MediaProbeResult media;

    friend bool operator==(const RecordingSegmentProbe&,
                           const RecordingSegmentProbe&) = default;
};

struct RecordingImportRequest final {
    domain::SessionId sessionId;
    std::vector<domain::SegmentInfo> segments;
    std::vector<project_store::RecordingSourceRole> sources;
    std::vector<domain::StudioScene> scenes;
    std::vector<project_store::RecordingSceneEvent> sceneEvents;
    std::vector<project_store::RecordingMarker> markers;
    domain::Timeline timeline;
    std::vector<RecordingSegmentProbe> probes;
};

struct RecordingImportPlan final {
    domain::SessionId sessionId;
    core::TimestampNs appendBase;
    std::vector<domain::MediaAsset> assets;
    std::vector<domain::Track> tracks;
    std::vector<domain::TimelineMarker> markers;

    friend bool operator==(const RecordingImportPlan&,
                           const RecordingImportPlan&) = default;
};

[[nodiscard]] core::Result<RecordingImportPlan> planRecordingImport(
    const RecordingImportRequest& request);

}  // namespace creator::app
