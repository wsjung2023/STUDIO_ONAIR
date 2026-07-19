#include "cursor/CursorEventPump.h"

#include "cursor/CursorButton.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorNormalizer.h"

#include <utility>
#include <type_traits>

namespace creator::cursor {
namespace {

core::Result<CursorButton> buttonFromOrdinal(std::uint8_t ordinal) {
    switch (ordinal) {
        case 0: return CursorButton::Left;
        case 1: return CursorButton::Right;
        case 2: return CursorButton::Middle;
        default:
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "cursor source returned an unknown button"};
    }
}

}  // namespace

core::Result<std::unique_ptr<CursorEventPump>> CursorEventPump::create(
    std::unique_ptr<ICursorSource> source,
    std::unique_ptr<CursorNdjsonSink> sink,
    domain::SourceId sourceId) {
    if (!source) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "cursor event pump requires a source"};
    }
    if (!sink) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "cursor event pump requires a sink"};
    }
    return std::unique_ptr<CursorEventPump>(
        new CursorEventPump(std::move(source), std::move(sink), std::move(sourceId)));
}

core::Result<void> CursorEventPump::drain(std::size_t maxSamples) {
    if (error_.has_value()) return *error_;
    if (maxSamples == 0) return core::ok();
    if (const auto sourceError = source_->error(); sourceError.has_value()) {
        return fail(*sourceError);
    }

    for (std::size_t i = 0; i < maxSamples; ++i) {
        auto sample = source_->poll();
        if (!sample.has_value()) break;
        ++stats_.polled;
        if (auto result = consume(*sample); !result.hasValue()) return result;
    }
    if (const auto sourceError = source_->error(); sourceError.has_value()) {
        return fail(*sourceError);
    }
    return core::ok();
}

core::Result<void> CursorEventPump::consume(const RawCursorSample& sample) {
    return std::visit(
        [this](const auto& raw) -> core::Result<void> {
            auto point = CursorNormalizer::normalize(raw.x, raw.y, raw.sourceWidth,
                                                      raw.sourceHeight);
            if (!point.hasValue()) {
                ++stats_.invalid;
                return fail(point.error());
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(raw)>, RawCursorMoveSample>) {
                auto event = CursorMoveEvent::create(raw.tNs, point.value(), sourceId_);
                if (!event.hasValue()) {
                    ++stats_.invalid;
                    return fail(event.error());
                }
                if (auto written = sink_->write(event.value()); !written.hasValue()) {
                    return fail(written.error(), true);
                }
                ++stats_.moves;
            } else {
                auto button = buttonFromOrdinal(raw.button);
                if (!button.hasValue()) {
                    ++stats_.invalid;
                    return fail(button.error());
                }
                auto event = CursorClickEvent::create(raw.tNs, point.value(), button.value());
                if (!event.hasValue()) {
                    ++stats_.invalid;
                    return fail(event.error());
                }
                if (auto written = sink_->write(event.value()); !written.hasValue()) {
                    return fail(written.error(), true);
                }
                ++stats_.clicks;
            }
            return core::ok();
        },
        sample);
}

core::Result<void> CursorEventPump::fail(core::AppError error, bool writeFailure) {
    if (writeFailure) ++stats_.writeFailures;
    if (!error_.has_value()) error_ = error;
    return error;
}

}  // namespace creator::cursor
