#include "app/ControllerLiveCaptureBindings.h"

#include "app/AvatarSceneController.h"
#include "app/DeviceCaptureController.h"
#include "app/ScreenCaptureController.h"
#include "core/AppError.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cstdio>
#include <utility>

namespace creator::app {

ControllerLiveCaptureBindings::ControllerLiveCaptureBindings(
    ScreenCaptureController* screen, DeviceCaptureController* devices,
    AvatarSceneController* avatar)
    : screen_(screen), devices_(devices), avatar_(avatar) {}

std::vector<LiveCaptureSource>
ControllerLiveCaptureBindings::activeSources() const {
    std::vector<LiveCaptureSource> sources;
    if (screen_) {
        if (auto id = screen_->activeSourceId()) {
            sources.push_back({std::move(*id), recorder::TrackRole::Screen});
        }
    }
    if (devices_) {
        if (auto id = devices_->activeCameraSourceId()) {
            sources.push_back({std::move(*id), recorder::TrackRole::Camera});
        }
        if (auto id = devices_->activeMicrophoneSourceId()) {
            sources.push_back({std::move(*id), recorder::TrackRole::Microphone});
        }
        if (auto id = devices_->activeSystemAudioSourceId()) {
            sources.push_back({std::move(*id), recorder::TrackRole::SystemAudio});
        } else {
            std::fprintf(stderr,
                         "[capture-bind] system audio NOT active at record "
                         "start; track will be missing\n");
        }
    }
    if (avatar_) {
        if (auto id = avatar_->activeAvatarSourceId()) {
            sources.push_back({std::move(*id), recorder::TrackRole::Avatar});
        }
    }
    return sources;
}

core::Result<void> ControllerLiveCaptureBindings::attach(
    const LiveCaptureSource& source,
    std::shared_ptr<capture::IVideoFrameSink> videoSink,
    std::shared_ptr<capture::IAudioBlockSink> audioSink) {
    switch (source.role) {
    case recorder::TrackRole::Screen:
        if (!screen_ || !videoSink || screen_->activeSourceId() != source.sourceId) break;
        screen_->setRecordingSink(std::move(videoSink));
        return core::ok();
    case recorder::TrackRole::Camera:
        if (!devices_ || !videoSink ||
            devices_->activeCameraSourceId() != source.sourceId) break;
        devices_->setCameraRecordingSink(std::move(videoSink));
        return core::ok();
    case recorder::TrackRole::Microphone:
        if (!devices_ || !audioSink ||
            devices_->activeMicrophoneSourceId() != source.sourceId) break;
        devices_->setMicrophoneRecordingSink(std::move(audioSink));
        return core::ok();
    case recorder::TrackRole::SystemAudio:
        if (!devices_ || !audioSink ||
            devices_->activeSystemAudioSourceId() != source.sourceId) {
            std::fprintf(stderr,
                         "[capture-bind] system audio attach REFUSED: "
                         "devices=%d sink=%d idMatch=%d\n",
                         devices_ != nullptr, audioSink != nullptr,
                         devices_ && devices_->activeSystemAudioSourceId() ==
                                         source.sourceId);
            break;
        }
        devices_->setSystemAudioRecordingSink(std::move(audioSink));
        std::fprintf(stderr, "[capture-bind] system audio recording sink attached\n");
        return core::ok();
    case recorder::TrackRole::Avatar:
        if (!avatar_ || !videoSink ||
            avatar_->activeAvatarSourceId() != source.sourceId) break;
        avatar_->setAvatarRecordingSink(std::move(videoSink));
        return core::ok();
    case recorder::TrackRole::CompositePreview:
        break;
    }
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "Live capture source has no compatible recording sink"};
}

void ControllerLiveCaptureBindings::detachAll() noexcept {
    if (screen_) screen_->setRecordingSink({});
    if (devices_) {
        devices_->setCameraRecordingSink({});
        devices_->setMicrophoneRecordingSink({});
        devices_->setSystemAudioRecordingSink({});
    }
    if (avatar_) avatar_->setAvatarRecordingSink({});
}

void ControllerLiveCaptureBindings::dispatch(std::function<void()> work) {
    QObject* context = screen_ ? static_cast<QObject*>(screen_)
                               : static_cast<QObject*>(devices_);
    if (!context || !work) return;
    if (context->thread() == QThread::currentThread()) {
        work();
        return;
    }
    QPointer<QObject> guarded{context};
    QMetaObject::invokeMethod(
        context,
        [guarded, work = std::move(work)]() mutable {
            if (guarded) work();
        },
        Qt::QueuedConnection);
}

}  // namespace creator::app
