#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "edit_engine/IEditEngine.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace creator::fakes {

enum class FakeEditOperation : std::size_t {
    Load,
    Update,
    Play,
    Pause,
    Seek,
    RequestFrame,
    Render,
    Count,
};

struct FakeEditCall final {
    FakeEditOperation operation;
    std::optional<std::int64_t> revision;
    std::optional<core::TimestampNs> position;

    friend bool operator==(const FakeEditCall&, const FakeEditCall&) = default;
};

class FakeEditEngine final : public edit_engine::IEditEngine {
public:
    [[nodiscard]] core::Result<void> load(
        const edit_engine::TimelineSnapshot& snapshot) override;
    [[nodiscard]] core::Result<void> update(
        const edit_engine::TimelineChangeSet& change) override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> seek(core::TimestampNs position) override;
    [[nodiscard]] core::Result<edit_engine::PreviewFrame> requestFrame(
        core::TimestampNs position) override;
    [[nodiscard]] core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override;

    void failNext(FakeEditOperation operation, core::AppError error);

    [[nodiscard]] const std::optional<edit_engine::TimelineSnapshot>&
    loadedSnapshot() const noexcept {
        return loaded_;
    }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] core::TimestampNs playhead() const noexcept { return playhead_; }
    [[nodiscard]] const std::vector<FakeEditCall>& calls() const noexcept {
        return calls_;
    }

private:
    [[nodiscard]] std::optional<core::AppError> consumeFailure(
        FakeEditOperation operation);
    [[nodiscard]] core::Result<void> requireLoaded() const;
    void record(FakeEditOperation operation,
                std::optional<std::int64_t> revision = std::nullopt,
                std::optional<core::TimestampNs> position = std::nullopt);

    std::optional<edit_engine::TimelineSnapshot> loaded_;
    bool playing_{false};
    core::TimestampNs playhead_{};
    std::vector<FakeEditCall> calls_;
    std::array<std::optional<core::AppError>,
               static_cast<std::size_t>(FakeEditOperation::Count)>
        failures_;
};

}  // namespace creator::fakes
