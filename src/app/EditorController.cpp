#include "app/EditorController.h"

#include "app/EditorEngineWorker.h"
#include "app/EditorSessionWorker.h"
#include "app/RecentProjectRegistry.h"

#include <QMetaObject>

#include <algorithm>
#include <utility>

namespace creator::app {
namespace {

QString clipKindName(domain::ClipKind kind) {
    switch (kind) {
        case domain::ClipKind::Asset:
            return QStringLiteral("asset");
        case domain::ClipKind::Title:
            return QStringLiteral("title");
        case domain::ClipKind::Caption:
            return QStringLiteral("caption");
    }
    return {};
}

QString alignmentName(domain::TextAlignment alignment) {
    switch (alignment) {
        case domain::TextAlignment::Left:
            return QStringLiteral("left");
        case domain::TextAlignment::Center:
            return QStringLiteral("center");
        case domain::TextAlignment::Right:
            return QStringLiteral("right");
    }
    return {};
}

QString pipPresetName(domain::PipPreset preset) {
    switch (preset) {
        case domain::PipPreset::FullFrame:
            return QStringLiteral("fullFrame");
        case domain::PipPreset::TopLeft:
            return QStringLiteral("topLeft");
        case domain::PipPreset::TopRight:
            return QStringLiteral("topRight");
        case domain::PipPreset::BottomLeft:
            return QStringLiteral("bottomLeft");
        case domain::PipPreset::BottomRight:
            return QStringLiteral("bottomRight");
        case domain::PipPreset::Custom:
            return QStringLiteral("custom");
    }
    return {};
}

std::optional<domain::PipPreset> parsePipPreset(const QString& value) {
    if (value == QStringLiteral("fullFrame")) {
        return domain::PipPreset::FullFrame;
    }
    if (value == QStringLiteral("topLeft")) return domain::PipPreset::TopLeft;
    if (value == QStringLiteral("topRight")) return domain::PipPreset::TopRight;
    if (value == QStringLiteral("bottomLeft")) {
        return domain::PipPreset::BottomLeft;
    }
    if (value == QStringLiteral("bottomRight")) {
        return domain::PipPreset::BottomRight;
    }
    return std::nullopt;
}

std::optional<domain::TextAlignment> parseAlignment(const QString& value) {
    if (value == QStringLiteral("left")) return domain::TextAlignment::Left;
    if (value == QStringLiteral("center")) {
        return domain::TextAlignment::Center;
    }
    if (value == QStringLiteral("right")) return domain::TextAlignment::Right;
    return std::nullopt;
}

double sourceAspect(const edit_engine::TimelineSnapshot& snapshot,
                    const domain::Clip& clip) {
    double result =
        static_cast<double>(snapshot.canvasWidth) / snapshot.canvasHeight;
    if (!clip.assetId().has_value()) return result;
    const auto found = std::find_if(
        snapshot.assets.begin(), snapshot.assets.end(),
        [&clip](const domain::MediaAsset& asset) {
            return asset.id() == *clip.assetId();
        });
    if (found != snapshot.assets.end() && found->video().has_value()) {
        result = static_cast<double>(found->video()->width) /
                 found->video()->height;
    }
    return result;
}

std::int64_t timelineFrameCount(const domain::Timeline& timeline) {
    core::TimestampNs end{};
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(end, clip.timelineRange().end());
        }
    }
    return core::timestampToFrame(end, timeline.frameRate());
}

}  // namespace

EditorController::EditorController(
    std::unique_ptr<edit_engine::IEditEngine> engine, QObject* parent)
    : QObject(parent),
      worker_(new EditorEngineWorker{std::move(engine)}),
      sessionWorker_(new EditorSessionWorker) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &EditorEngineWorker::completed, this,
            &EditorController::handleCompleted);
    connect(worker_, &EditorEngineWorker::frameCompleted, this,
            &EditorController::handleFrameCompleted);
    sessionWorker_->moveToThread(&sessionThread_);
    connect(&sessionThread_, &QThread::finished, sessionWorker_,
            &QObject::deleteLater);
    connect(sessionWorker_, &EditorSessionWorker::opened, this,
            &EditorController::handleSessionOpened);
    connect(sessionWorker_, &EditorSessionWorker::edited, this,
            &EditorController::handleSessionEdited);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_, &QTimer::timeout, this,
            &EditorController::requestPlaybackFrame);
    workerThread_.start();
    sessionThread_.start();
}

EditorController::~EditorController() {
    playbackTimer_.stop();
    QMetaObject::invokeMethod(sessionWorker_, [] {},
                              Qt::BlockingQueuedConnection);
    disconnect(sessionWorker_, nullptr, this, nullptr);
    sessionThread_.quit();
    sessionThread_.wait();
    while (!queuedCommands_.empty()) {
        auto command = std::move(queuedCommands_.front());
        queuedCommands_.pop_front();
        postToWorker(std::move(command));
    }
    QMetaObject::invokeMethod(worker_, [] {}, Qt::BlockingQueuedConnection);
    disconnect(worker_, nullptr, this, nullptr);
    workerThread_.quit();
    workerThread_.wait();
}

qlonglong EditorController::timelineRevision() const noexcept {
    return snapshot_.has_value() ? snapshot_->revision.value() : -1;
}

qlonglong EditorController::timelineDurationNs() const noexcept {
    if (!snapshot_.has_value()) return 0;
    core::TimestampNs end{};
    for (const auto& track : snapshot_->timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(end, clip.timelineRange().end());
        }
    }
    return end.time_since_epoch().count();
}

const domain::Clip* EditorController::selectedClip() const noexcept {
    if (!snapshot_.has_value() || selectedTrackId_.isEmpty() ||
        selectedClipId_.isEmpty()) {
        return nullptr;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) return nullptr;
    return snapshot_->timeline.clip(trackId.value(), clipId.value());
}

const domain::Track* EditorController::selectedTrack() const noexcept {
    if (!snapshot_.has_value() || selectedTrackId_.isEmpty()) return nullptr;
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    return trackId.hasValue() ? snapshot_->timeline.track(trackId.value())
                              : nullptr;
}

qlonglong EditorController::selectedClipStartNs() const noexcept {
    const auto* clip = selectedClip();
    return clip == nullptr
               ? -1
               : clip->timelineRange().start().time_since_epoch().count();
}

qlonglong EditorController::selectedClipEndNs() const noexcept {
    const auto* clip = selectedClip();
    return clip == nullptr
               ? -1
               : clip->timelineRange().end().time_since_epoch().count();
}

QString EditorController::selectedClipKind() const {
    const auto* clip = selectedClip();
    return clip == nullptr ? QString{} : clipKindName(clip->kind());
}

bool EditorController::selectedVisualCompatible() const noexcept {
    const auto* clip = selectedClip();
    return clip != nullptr && clip->mediaKind() != domain::MediaKind::Audio;
}

bool EditorController::selectedAudioCompatible() const noexcept {
    const auto* clip = selectedClip();
    return clip != nullptr && clip->hasAudio();
}

QVariantMap EditorController::selectedVisualTransform() const {
    const auto* clip = selectedClip();
    if (clip == nullptr || !clip->visualTransform().has_value()) return {};
    const auto& value = *clip->visualTransform();
    return {{QStringLiteral("x"), value.x()},
            {QStringLiteral("y"), value.y()},
            {QStringLiteral("width"), value.width()},
            {QStringLiteral("height"), value.height()},
            {QStringLiteral("scaleX"), value.scaleX()},
            {QStringLiteral("scaleY"), value.scaleY()},
            {QStringLiteral("rotationDegrees"), value.rotationDegrees()},
            {QStringLiteral("cropLeft"), value.cropLeft()},
            {QStringLiteral("cropTop"), value.cropTop()},
            {QStringLiteral("cropRight"), value.cropRight()},
            {QStringLiteral("cropBottom"), value.cropBottom()},
            {QStringLiteral("opacity"), value.opacity()},
            {QStringLiteral("zOrder"), value.zOrder()}};
}

QVariantMap EditorController::selectedAudioEnvelope() const {
    const auto* clip = selectedClip();
    if (clip == nullptr || !clip->audioEnvelope().has_value()) return {};
    const auto& value = *clip->audioEnvelope();
    return {{QStringLiteral("gainDb"), value.gainDb()},
            {QStringLiteral("fadeInNs"),
             QVariant::fromValue<qint64>(value.fadeIn().count())},
            {QStringLiteral("fadeOutNs"),
             QVariant::fromValue<qint64>(value.fadeOut().count())}};
}

QVariantMap EditorController::selectedTitlePayload() const {
    const auto* clip = selectedClip();
    if (clip == nullptr || !clip->titlePayload().has_value()) return {};
    const auto& value = *clip->titlePayload();
    return {{QStringLiteral("text"), QString::fromUtf8(value.text())},
            {QStringLiteral("fontFamily"),
             QString::fromUtf8(value.fontFamily())},
            {QStringLiteral("x"), value.x()},
            {QStringLiteral("y"), value.y()},
            {QStringLiteral("foreground"),
             QString::fromStdString(value.foreground().toString())},
            {QStringLiteral("background"),
             QString::fromStdString(value.background().toString())},
            {QStringLiteral("alignment"), alignmentName(value.alignment())}};
}

QVariantList EditorController::selectedCaptionCues() const {
    const auto* clip = selectedClip();
    if (clip == nullptr || clip->kind() != domain::ClipKind::Caption) return {};
    QVariantList result;
    result.reserve(static_cast<qsizetype>(clip->captionCues().size()));
    for (const auto& cue : clip->captionCues()) {
        result.push_back(QVariantMap{
            {QStringLiteral("cueId"),
             QString::fromStdString(cue.id().value())},
            {QStringLiteral("startOffsetNs"),
             QVariant::fromValue<qint64>(cue.startOffset().count())},
            {QStringLiteral("durationNs"),
             QVariant::fromValue<qint64>(cue.duration().count())},
            {QStringLiteral("text"), QString::fromUtf8(cue.text())}});
    }
    return result;
}

QString EditorController::selectedPipPreset() const {
    const auto* clip = selectedClip();
    if (clip == nullptr || !snapshot_.has_value() ||
        clip->mediaKind() == domain::MediaKind::Audio) {
        return {};
    }
    if (!clip->visualTransform().has_value()) {
        return pipPresetName(domain::PipPreset::FullFrame);
    }
    const double canvasAspect =
        static_cast<double>(snapshot_->canvasWidth) / snapshot_->canvasHeight;
    return pipPresetName(domain::identifyPipPreset(
        *clip->visualTransform(), sourceAspect(*snapshot_, *clip),
        canvasAspect));
}

QString EditorController::selectedResolvedFontFamily() const {
    const auto* clip = selectedClip();
    if (clip == nullptr || !snapshot_.has_value()) return {};
    const auto found = std::find_if(
        snapshot_->generatedOverlays.begin(), snapshot_->generatedOverlays.end(),
        [clip](const edit_engine::GeneratedOverlayDescriptor& descriptor) {
            return descriptor.ownerClipId() == clip->id();
        });
    return found == snapshot_->generatedOverlays.end()
               ? QString{}
               : QString::fromUtf8(found->resolvedFontFamily());
}

void EditorController::openSession(
    std::vector<domain::MediaAsset> assets,
    edit_engine::TimelineSnapshot snapshot) {
    ++generation_;
    derivedFailureRecoveryPending_ = false;
    snapshot.assets = assets;
    mediaBinModel_.setAssets(std::move(assets));
    timelineTrackModel_.setTimeline(snapshot.timeline);
    snapshot_ = snapshot;
    if (!selectedTrackId_.isEmpty() || !selectedClipId_.isEmpty()) {
        selectedTrackId_.clear();
        selectedClipId_.clear();
        emit selectionChanged();
    }
    if (rangeIn_.has_value() || rangeOut_.has_value()) {
        rangeIn_.reset();
        rangeOut_.reset();
        emit markedRangeChanged();
    }
    if (canUndo_ || canRedo_ || !clean_) {
        canUndo_ = false;
        canRedo_ = false;
        clean_ = true;
        emit editStateChanged();
    }
    setPlaying(false);
    if (playhead_ != core::TimestampNs{}) {
        playhead_ = core::TimestampNs{};
        emit playheadChanged();
    }
    setStatus({});
    if (!previewImage_.isNull()) {
        previewImage_ = {};
        emit previewImageChanged();
    }
    setPreviewStale(true);
    emit timelineChanged();
    queueLoad(std::move(snapshot));
}

void EditorController::commitTimeline(edit_engine::TimelineChangeSet change) {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    if (change.baseRevision() != snapshot_->revision ||
        change.target().timeline.id() != snapshot_->timeline.id()) {
        setStatus(QStringLiteral("Timeline update is stale or changes session identity"));
        return;
    }
    timelineTrackModel_.setTimeline(change.target().timeline);
    snapshot_ = change.target();
    setPreviewStale(true);
    emit timelineChanged();
    queueUpdate(std::move(change));
}

void EditorController::play() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    queueSimple(EditorEngineOperation::Play);
}

void EditorController::pause() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    playbackTimer_.stop();
    queueSimple(EditorEngineOperation::Pause);
}

void EditorController::seek(qlonglong positionNs) {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    if (positionNs < 0) {
        setStatus(QStringLiteral("Editor position must not be negative"));
        return;
    }
    queueSimple(EditorEngineOperation::Seek,
                core::TimestampNs{core::DurationNs{positionNs}});
}

void EditorController::selectClip(QString trackId, QString clipId) {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    const auto track = domain::TrackId::create(
        trackId.toUtf8().toStdString());
    const auto clip = domain::ClipId::create(clipId.toUtf8().toStdString());
    if (!track.hasValue() || !clip.hasValue() ||
        snapshot_->timeline.clip(track.value(), clip.value()) == nullptr) {
        setStatus(QStringLiteral("Selected timeline clip does not exist"));
        return;
    }
    if (selectedTrackId_ == trackId && selectedClipId_ == clipId) return;
    selectedTrackId_ = std::move(trackId);
    selectedClipId_ = std::move(clipId);
    setStatus({});
    emit selectionChanged();
}

void EditorController::markRangeIn() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    core::TimestampNs in = playhead_;
    if (hasMarkedRange() && in > *rangeOut_) {
        rangeIn_ = in;
        rangeOut_.reset();
    } else if (rangeOut_.has_value() && in > *rangeOut_) {
        rangeIn_ = *rangeOut_;
        rangeOut_ = in;
    } else {
        rangeIn_ = in;
    }
    emit markedRangeChanged();
}

void EditorController::markRangeOut() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    core::TimestampNs out = playhead_;
    if (rangeIn_.has_value() && out < *rangeIn_) {
        rangeOut_ = *rangeIn_;
        rangeIn_ = out;
    } else {
        rangeOut_ = out;
    }
    emit markedRangeChanged();
}

void EditorController::openProject(QUrl projectUrl) {
    if (!projectUrl.isLocalFile() || projectUrl.toLocalFile().isEmpty()) {
        setStatus(QStringLiteral("Editor project URL must be a local file"));
        return;
    }
    ++sessionGeneration_;
    ++generation_;
    activeSessionCommand_.reset();
    durableSessionReady_ = false;
    derivedFailureRecoveryPending_ = false;
    setPlaying(false);
    if (!selectedTrackId_.isEmpty() || !selectedClipId_.isEmpty()) {
        selectedTrackId_.clear();
        selectedClipId_.clear();
        emit selectionChanged();
    }
    if (rangeIn_.has_value() || rangeOut_.has_value()) {
        rangeIn_.reset();
        rangeOut_.reset();
        emit markedRangeChanged();
    }
    if (!previewImage_.isNull()) {
        previewImage_ = {};
        emit previewImageChanged();
    }
    setPreviewStale(true);
    setSessionBusy(true);
    setStatus({});
    const auto path = pathFromQString(projectUrl.toLocalFile());
    const quint64 generation = sessionGeneration_;
    QMetaObject::invokeMethod(
        sessionWorker_,
        [worker = sessionWorker_, generation, path] {
            worker->openProject(generation, path);
        },
        Qt::QueuedConnection);
}

void EditorController::splitSelected() {
    if (selectedTrackId_.isEmpty() || selectedClipId_.isEmpty()) {
        setStatus(QStringLiteral("Select a clip before splitting"));
        return;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Split,
                                       .trackId = trackId.value(),
                                       .clipId = clipId.value(),
                                       .position = playhead_});
}

void EditorController::trimSelectedStart() {
    if (selectedTrackId_.isEmpty() || selectedClipId_.isEmpty()) {
        setStatus(QStringLiteral("Select a clip before trimming"));
        return;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::TrimLeading,
                                       .trackId = trackId.value(),
                                       .clipId = clipId.value(),
                                       .position = playhead_});
}

void EditorController::trimSelectedEnd() {
    if (selectedTrackId_.isEmpty() || selectedClipId_.isEmpty()) {
        setStatus(QStringLiteral("Select a clip before trimming"));
        return;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::TrimTrailing,
                                       .trackId = trackId.value(),
                                       .clipId = clipId.value(),
                                       .position = playhead_});
}

void EditorController::deleteMarkedRange(bool ripple) {
    if (!hasMarkedRange()) {
        setStatus(QStringLiteral("Mark a non-empty range before deleting"));
        return;
    }
    auto range = domain::TimeRange::create(*rangeIn_, *rangeOut_ - *rangeIn_);
    if (!range.hasValue()) {
        setStatus(QString::fromStdString(range.error().message()));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::DeleteRange,
                                       .range = range.value(),
                                       .ripple = ripple});
}

void EditorController::applySelectedVisualTransform(
    double x, double y, double width, double height, double scaleX,
    double scaleY, double rotationDegrees, double cropLeft, double cropTop,
    double cropRight, double cropBottom, double opacity, int zOrder) {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr) {
        setStatus(QStringLiteral("Select a visual clip before applying a visual transform"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    if (!selectedVisualCompatible()) {
        setStatus(QStringLiteral("The selected clip does not support a visual transform"));
        return;
    }
    auto value = domain::VisualTransform::create(
        x, y, width, height, scaleX, scaleY, rotationDegrees, cropLeft,
        cropTop, cropRight, cropBottom, opacity,
        static_cast<std::int32_t>(zOrder));
    if (!value.hasValue()) {
        setStatus(QStringLiteral("Visual transform: ") +
                  QString::fromStdString(value.error().message()));
        return;
    }
    auto trackId = domain::TrackId::create(selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::SetVisualTransform,
        .trackId = std::move(trackId).value(),
        .clipId = std::move(clipId).value(),
        .visualTransform = std::move(value).value()});
}

void EditorController::applySelectedPipPreset(QString preset) {
    const auto parsed = parsePipPreset(preset);
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (!parsed.has_value()) {
        setStatus(QStringLiteral("PIP preset is invalid"));
        return;
    }
    if (clip == nullptr || track == nullptr || !snapshot_.has_value() ||
        !selectedVisualCompatible()) {
        setStatus(QStringLiteral("Select a visual clip before applying a PIP preset"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    const std::int32_t zOrder = clip->visualTransform().has_value()
                                    ? clip->visualTransform()->zOrder()
                                    : 0;
    const double canvasAspect =
        static_cast<double>(snapshot_->canvasWidth) / snapshot_->canvasHeight;
    auto value = domain::visualTransformForPipPreset(
        *parsed, sourceAspect(*snapshot_, *clip), canvasAspect, zOrder);
    if (!value.hasValue()) {
        setStatus(QStringLiteral("PIP preset: ") +
                  QString::fromStdString(value.error().message()));
        return;
    }
    applySelectedVisualTransform(
        value.value().x(), value.value().y(), value.value().width(),
        value.value().height(), value.value().scaleX(), value.value().scaleY(),
        value.value().rotationDegrees(), value.value().cropLeft(),
        value.value().cropTop(), value.value().cropRight(),
        value.value().cropBottom(), value.value().opacity(),
        value.value().zOrder());
}

void EditorController::resetSelectedVisualTransform() {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr || !selectedVisualCompatible()) {
        setStatus(QStringLiteral("Select a visual clip before resetting its transform"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    auto trackId = domain::TrackId::create(selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::SetVisualTransform,
        .trackId = std::move(trackId).value(),
        .clipId = std::move(clipId).value()});
}

void EditorController::applySelectedAudioEnvelope(
    double gainDb, qlonglong fadeInNs, qlonglong fadeOutNs) {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr || !selectedAudioCompatible()) {
        setStatus(QStringLiteral("Select a clip with audio before applying an audio envelope"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    auto value = domain::AudioEnvelope::create(
        gainDb, core::DurationNs{fadeInNs}, core::DurationNs{fadeOutNs},
        clip->timelineRange().duration());
    if (!value.hasValue()) {
        setStatus(QStringLiteral("Audio envelope: ") +
                  QString::fromStdString(value.error().message()));
        return;
    }
    auto trackId = domain::TrackId::create(selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::SetAudioEnvelope,
        .trackId = std::move(trackId).value(),
        .clipId = std::move(clipId).value(),
        .audioEnvelope = std::move(value).value()});
}

void EditorController::resetSelectedAudioEnvelope() {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr || !selectedAudioCompatible()) {
        setStatus(QStringLiteral("Select a clip with audio before resetting its envelope"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    auto trackId = domain::TrackId::create(selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::SetAudioEnvelope,
        .trackId = std::move(trackId).value(),
        .clipId = std::move(clipId).value()});
}

void EditorController::addTitle(
    QString text, QString fontFamily, double x, double y, QString foreground,
    QString background, QString alignment) {
    if (snapshot_.has_value()) {
        const auto stableId = domain::TrackId::create("title-1").value();
        const auto* stableTrack = snapshot_->timeline.track(stableId);
        if (stableTrack != nullptr && stableTrack->locked()) {
            setStatus(QStringLiteral("The title track is locked"));
            return;
        }
    }
    const auto foregroundValue =
        domain::RgbaColor::parse(foreground.toUtf8().toStdString());
    if (!foregroundValue.hasValue()) {
        setStatus(QStringLiteral("Title foreground: ") +
                  QString::fromStdString(foregroundValue.error().message()));
        return;
    }
    const auto backgroundValue =
        domain::RgbaColor::parse(background.toUtf8().toStdString());
    if (!backgroundValue.hasValue()) {
        setStatus(QStringLiteral("Title background: ") +
                  QString::fromStdString(backgroundValue.error().message()));
        return;
    }
    const auto alignmentValue = parseAlignment(alignment);
    if (!alignmentValue.has_value()) {
        setStatus(QStringLiteral("Title alignment is invalid"));
        return;
    }
    auto payload = domain::TitlePayload::create(
        text.toUtf8().toStdString(), fontFamily.toUtf8().toStdString(), x, y,
        foregroundValue.value(), backgroundValue.value(), *alignmentValue);
    if (!payload.hasValue()) {
        setStatus(QStringLiteral("Title payload: ") +
                  QString::fromStdString(payload.error().message()));
        return;
    }
    auto range = domain::TimeRange::create(playhead_, core::DurationNs{3'000'000'000});
    if (!range.hasValue()) {
        setStatus(QStringLiteral("Title range: ") +
                  QString::fromStdString(range.error().message()));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::AddTitle,
                                       .range = range.value(),
                                       .titlePayload = std::move(payload).value()});
}

void EditorController::editSelectedTitle(
    QString text, QString fontFamily, double x, double y, QString foreground,
    QString background, QString alignment) {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr ||
        clip->kind() != domain::ClipKind::Title) {
        setStatus(QStringLiteral("Select a title clip before editing it"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    const auto foregroundValue =
        domain::RgbaColor::parse(foreground.toUtf8().toStdString());
    if (!foregroundValue.hasValue()) {
        setStatus(QStringLiteral("Title foreground: ") +
                  QString::fromStdString(foregroundValue.error().message()));
        return;
    }
    const auto backgroundValue =
        domain::RgbaColor::parse(background.toUtf8().toStdString());
    if (!backgroundValue.hasValue()) {
        setStatus(QStringLiteral("Title background: ") +
                  QString::fromStdString(backgroundValue.error().message()));
        return;
    }
    const auto alignmentValue = parseAlignment(alignment);
    if (!alignmentValue.has_value()) {
        setStatus(QStringLiteral("Title alignment is invalid"));
        return;
    }
    auto payload = domain::TitlePayload::create(
        text.toUtf8().toStdString(), fontFamily.toUtf8().toStdString(), x, y,
        foregroundValue.value(), backgroundValue.value(), *alignmentValue);
    if (!payload.hasValue()) {
        setStatus(QStringLiteral("Title payload: ") +
                  QString::fromStdString(payload.error().message()));
        return;
    }
    auto trackId = domain::TrackId::create(selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected title identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::EditTitle,
        .trackId = std::move(trackId).value(),
        .clipId = std::move(clipId).value(),
        .titlePayload = std::move(payload).value()});
}

void EditorController::removeSelectedTitle() {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr ||
        clip->kind() != domain::ClipKind::Title) {
        setStatus(QStringLiteral("Select a title clip before removing it"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    auto trackId = domain::TrackId::create(selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected title identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::RemoveGeneratedClip,
        .trackId = std::move(trackId).value(),
        .clipId = std::move(clipId).value()});
}

void EditorController::addCaptionCue(
    qlonglong startOffsetNs, qlonglong durationNs, QString text) {
    const domain::Clip* captionClip = selectedClip();
    const domain::Track* captionTrack = selectedTrack();
    if (captionClip == nullptr || captionTrack == nullptr ||
        captionClip->kind() != domain::ClipKind::Caption) {
        captionClip = nullptr;
        captionTrack = nullptr;
    }
    if (snapshot_.has_value() && captionClip == nullptr) {
        const auto stableId = domain::TrackId::create("caption-1").value();
        const auto* stableTrack = snapshot_->timeline.track(stableId);
        if (stableTrack != nullptr) {
            captionTrack = stableTrack;
            const auto found = std::find_if(
                stableTrack->clips().begin(), stableTrack->clips().end(),
                [this](const domain::Clip& candidate) {
                    return candidate.timelineRange().start() <= playhead_ &&
                           playhead_ < candidate.timelineRange().end();
                });
            if (found != stableTrack->clips().end()) captionClip = &*found;
        }
    }
    if (captionTrack != nullptr && captionTrack->locked()) {
        setStatus(QStringLiteral("The caption track is locked"));
        return;
    }
    auto validationId = domain::CueId::create("inspector-validation");
    auto cue = domain::CaptionCue::create(
        std::move(validationId).value(), core::DurationNs{startOffsetNs},
        core::DurationNs{durationNs}, text.toUtf8().toStdString());
    if (!cue.hasValue()) {
        setStatus(QStringLiteral("Caption cue: ") +
                  QString::fromStdString(cue.error().message()));
        return;
    }
    EditorEditRequest request{
        .kind = EditorEditKind::AddCaptionCue,
        .captionCue = CaptionCueDraft{core::DurationNs{startOffsetNs},
                                      core::DurationNs{durationNs},
                                      text.toUtf8().toStdString()}};
    if (captionClip != nullptr && captionTrack != nullptr) {
        auto cues = captionClip->captionCues();
        cues.push_back(cue.value());
        auto validated = captionClip->withCaptionCues(std::move(cues));
        if (!validated.hasValue()) {
            setStatus(QStringLiteral("Caption cue overlap or range: ") +
                      QString::fromStdString(validated.error().message()));
            return;
        }
        request.trackId = captionTrack->id();
        request.clipId = captionClip->id();
    } else {
        const auto requestedEnd = core::DurationNs{startOffsetNs} +
                                  core::DurationNs{durationNs};
        const auto clipDuration =
            std::max(core::DurationNs{2'000'000'000}, requestedEnd);
        auto range = domain::TimeRange::create(playhead_, clipDuration);
        if (!range.hasValue()) {
            setStatus(QStringLiteral("Caption clip range: ") +
                      QString::fromStdString(range.error().message()));
            return;
        }
        request.range = range.value();
    }
    queueSessionEdit(std::move(request));
}

void EditorController::editCaptionCue(
    QString cueId, qlonglong startOffsetNs, qlonglong durationNs,
    QString text) {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr ||
        clip->kind() != domain::ClipKind::Caption) {
        setStatus(QStringLiteral("Select a caption clip before editing a cue"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    auto id = domain::CueId::create(cueId.toUtf8().toStdString());
    if (!id.hasValue()) {
        setStatus(QStringLiteral("Caption cue identity is invalid"));
        return;
    }
    auto replacement = domain::CaptionCue::create(
        id.value(), core::DurationNs{startOffsetNs},
        core::DurationNs{durationNs}, text.toUtf8().toStdString());
    if (!replacement.hasValue()) {
        setStatus(QStringLiteral("Caption cue: ") +
                  QString::fromStdString(replacement.error().message()));
        return;
    }
    auto cues = clip->captionCues();
    const auto found = std::find_if(
        cues.begin(), cues.end(), [&id](const domain::CaptionCue& candidate) {
            return candidate.id() == id.value();
        });
    if (found == cues.end()) {
        setStatus(QStringLiteral("Selected caption cue does not exist"));
        return;
    }
    *found = replacement.value();
    auto validated = clip->withCaptionCues(std::move(cues));
    if (!validated.hasValue()) {
        setStatus(QStringLiteral("Caption cue overlap or range: ") +
                  QString::fromStdString(validated.error().message()));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::EditCaptionCue,
        .trackId = domain::TrackId::create(
                       selectedTrackId_.toUtf8().toStdString())
                       .value(),
        .clipId = domain::ClipId::create(
                      selectedClipId_.toUtf8().toStdString())
                      .value(),
        .cueId = std::move(id).value(),
        .captionCue = CaptionCueDraft{core::DurationNs{startOffsetNs},
                                      core::DurationNs{durationNs},
                                      text.toUtf8().toStdString()}});
}

void EditorController::removeCaptionCue(QString cueId) {
    const auto* clip = selectedClip();
    const auto* track = selectedTrack();
    if (clip == nullptr || track == nullptr ||
        clip->kind() != domain::ClipKind::Caption) {
        setStatus(QStringLiteral("Select a caption clip before removing a cue"));
        return;
    }
    if (track->locked()) {
        setStatus(QStringLiteral("The selected track is locked"));
        return;
    }
    auto id = domain::CueId::create(cueId.toUtf8().toStdString());
    if (!id.hasValue() ||
        std::none_of(clip->captionCues().begin(), clip->captionCues().end(),
                     [&id](const domain::CaptionCue& cue) {
                         return id.hasValue() && cue.id() == id.value();
                     })) {
        setStatus(QStringLiteral("Selected caption cue does not exist"));
        return;
    }
    queueSessionEdit(EditorEditRequest{
        .kind = EditorEditKind::RemoveCaptionCue,
        .trackId = domain::TrackId::create(
                       selectedTrackId_.toUtf8().toStdString())
                       .value(),
        .clipId = domain::ClipId::create(
                      selectedClipId_.toUtf8().toStdString())
                      .value(),
        .cueId = std::move(id).value()});
}

void EditorController::undo() {
    if (!canUndo_) {
        setStatus(QStringLiteral("There is no edit to undo"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Undo});
}

void EditorController::redo() {
    if (!canRedo_) {
        setStatus(QStringLiteral("There is no edit to redo"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Redo});
}

void EditorController::save() {
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Save});
}

void EditorController::queueSessionEdit(EditorEditRequest request) {
    if (!durableSessionReady_) {
        setStatus(QStringLiteral("No durable editor session is open"));
        return;
    }
    if (sessionBusy_) return;
    if (busy()) {
        setStatus(QStringLiteral("Wait for the preview update to finish"));
        return;
    }
    if (playing_) {
        setPlaying(false);
        queueSimple(EditorEngineOperation::Pause);
    }
    const quint64 commandId = nextSessionCommandId_++;
    activeSessionCommand_ = commandId;
    setSessionBusy(true);
    setStatus({});
    const quint64 generation = sessionGeneration_;
    QMetaObject::invokeMethod(
        sessionWorker_,
        [worker = sessionWorker_, generation, commandId,
         request = std::move(request)]() mutable {
            worker->edit(generation, commandId, std::move(request));
        },
        Qt::QueuedConnection);
}

void EditorController::handleSessionOpened(quint64 generation,
                                           EditorSessionResultPtr result) {
    if (generation != sessionGeneration_) return;
    if (!result || !result->hasValue()) {
        durableSessionReady_ = false;
        setStatus(result ? QString::fromStdString(result->error().message())
                         : QStringLiteral("Editor session returned no result"));
        setSessionBusy(false);
        return;
    }
    durableSessionReady_ = true;
    const auto& state = result->value().state;
    openSession(state.assets, state.snapshot);
    canUndo_ = state.canUndo;
    canRedo_ = state.canRedo;
    clean_ = state.clean;
    emit editStateChanged();
    if (result->value().derivedWorkDiagnostic.has_value()) {
        derivedFailureRecoveryPending_ = true;
        setStatus(QString::fromStdString(
            result->value().derivedWorkDiagnostic->message()));
        setPreviewStale(true);
    }
    setSessionBusy(false);
}

void EditorController::handleSessionEdited(quint64 generation,
                                           quint64 commandId,
                                           EditorSessionResultPtr result) {
    if (generation != sessionGeneration_ ||
        !activeSessionCommand_.has_value() ||
        commandId != *activeSessionCommand_) {
        return;
    }
    activeSessionCommand_.reset();
    if (!result || !result->hasValue()) {
        setStatus(result ? QString::fromStdString(result->error().message())
                         : QStringLiteral("Editor command returned no result"));
        setSessionBusy(false);
        return;
    }
    const auto& update = result->value();
    publishSessionState(update.state);
    if (update.derivedWorkDiagnostic.has_value()) {
        setStatus(QString::fromStdString(
            update.derivedWorkDiagnostic->message()));
        setPreviewStale(true);
        derivedFailureRecoveryPending_ = true;
        queueLoad(update.state.snapshot, true);
    } else if (update.change.has_value()) {
        setPreviewStale(true);
        queueUpdate(*update.change);
    }
    setSessionBusy(false);
}

void EditorController::publishSessionState(const EditorSessionState& state) {
    const bool revisionChanged =
        !snapshot_.has_value() || snapshot_->revision != state.snapshot.revision;
    mediaBinModel_.setAssets(state.assets);
    timelineTrackModel_.setTimeline(state.snapshot.timeline);
    snapshot_ = state.snapshot;
    const bool editStateChangedValue = canUndo_ != state.canUndo ||
                                       canRedo_ != state.canRedo ||
                                       clean_ != state.clean;
    canUndo_ = state.canUndo;
    canRedo_ = state.canRedo;
    clean_ = state.clean;
    if (editStateChangedValue) emit editStateChanged();

    if (!selectedTrackId_.isEmpty() && !selectedClipId_.isEmpty()) {
        auto trackId = domain::TrackId::create(
            selectedTrackId_.toUtf8().toStdString());
        auto clipId = domain::ClipId::create(
            selectedClipId_.toUtf8().toStdString());
        if (!trackId.hasValue() || !clipId.hasValue() ||
            state.snapshot.timeline.clip(trackId.value(), clipId.value()) ==
                nullptr) {
            selectedTrackId_.clear();
            selectedClipId_.clear();
            emit selectionChanged();
        } else if (revisionChanged) {
            emit selectionChanged();
        }
    }
    emit timelineChanged();
}

void EditorController::setSessionBusy(bool value) {
    if (sessionBusy_ == value) return;
    sessionBusy_ = value;
    emit sessionBusyChanged();
}

void EditorController::queueLoad(edit_engine::TimelineSnapshot snapshot,
                                 bool recoveryPriority) {
    const quint64 commandId = beginCommand(EditorEngineOperation::Load, std::nullopt);
    QueuedCommand command{generation_, commandId, EditorEngineOperation::Load,
                          std::nullopt, std::move(snapshot), std::nullopt};
    if (recoveryPriority) {
        queuedCommands_.push_front(std::move(command));
    } else {
        queuedCommands_.push_back(std::move(command));
    }
    dispatchNext();
}

void EditorController::queueUpdate(edit_engine::TimelineChangeSet change) {
    const quint64 commandId =
        beginCommand(EditorEngineOperation::Update, std::nullopt);
    queuedCommands_.push_back(
        QueuedCommand{generation_, commandId, EditorEngineOperation::Update,
                      std::nullopt, std::nullopt, std::move(change)});
    dispatchNext();
}

void EditorController::queueSimple(
    EditorEngineOperation operation,
    std::optional<core::TimestampNs> position) {
    const quint64 commandId = beginCommand(operation, position);
    queuedCommands_.push_back(QueuedCommand{generation_, commandId, operation,
                                             position, std::nullopt,
                                             std::nullopt});
    dispatchNext();
}

void EditorController::queueFrame(core::TimestampNs position) {
    if (!snapshot_.has_value() || previewStale_ || frameRequestInFlight_) return;
    const quint64 commandId =
        beginCommand(EditorEngineOperation::Frame, position, false,
                     snapshot_->revision.value());
    frameRequestInFlight_ = true;
    queuedCommands_.push_back(QueuedCommand{
        generation_, commandId, EditorEngineOperation::Frame, position,
        std::nullopt, std::nullopt});
    dispatchNext();
}

void EditorController::dispatchNext() {
    if (workerCommandActive_ || queuedCommands_.empty()) return;
    workerCommandActive_ = true;
    auto command = std::move(queuedCommands_.front());
    queuedCommands_.pop_front();
    postToWorker(std::move(command));
}

void EditorController::postToWorker(QueuedCommand command) {
    const quint64 generation = command.generation;
    const quint64 commandId = command.commandId;
    switch (command.operation) {
        case EditorEngineOperation::Play:
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId] {
                    worker->play(generation, commandId);
                },
                Qt::QueuedConnection);
            break;
        case EditorEngineOperation::Pause:
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId] {
                    worker->pause(generation, commandId);
                },
                Qt::QueuedConnection);
            break;
        case EditorEngineOperation::Seek: {
            const core::TimestampNs position = *command.position;
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId, position] {
                    worker->seek(generation, commandId, position);
                },
                Qt::QueuedConnection);
            break;
        }
        case EditorEngineOperation::Frame: {
            const core::TimestampNs position = *command.position;
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId, position] {
                    worker->requestFrame(generation, commandId, position);
                },
                Qt::QueuedConnection);
            break;
        }
        case EditorEngineOperation::Load: {
            auto snapshot = std::move(*command.snapshot);
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId,
                 snapshot = std::move(snapshot)]() mutable {
                    worker->load(generation, commandId, std::move(snapshot));
                },
                Qt::QueuedConnection);
            break;
        }
        case EditorEngineOperation::Update: {
            auto change = std::move(*command.change);
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId,
                 change = std::move(change)]() mutable {
                    worker->update(generation, commandId, std::move(change));
                },
                Qt::QueuedConnection);
            break;
        }
    }
}

quint64 EditorController::beginCommand(
    EditorEngineOperation operation,
    std::optional<core::TimestampNs> position, bool countsAsBusy,
    std::optional<std::int64_t> expectedRevision) {
    const bool wasBusy = busy();
    const quint64 commandId = nextCommandId_++;
    commands_.emplace(commandId,
                      PendingCommand{generation_, operation, position,
                                     countsAsBusy, expectedRevision});
    if (countsAsBusy) {
        ++pendingCommands_;
        if (!wasBusy) emit busyChanged();
    }
    return commandId;
}

void EditorController::handleCompleted(quint64 generation, quint64 commandId,
                                       int operationValue, bool success,
                                       const QString& errorMessage) {
    const auto found = commands_.find(commandId);
    if (found == commands_.end()) return;
    const PendingCommand command = found->second;
    const auto operation = static_cast<EditorEngineOperation>(operationValue);
    const bool current = generation == generation_ &&
                         command.generation == generation_ &&
                         command.operation == operation;
    commands_.erase(found);
    if (command.countsAsBusy) --pendingCommands_;
    workerCommandActive_ = false;

    if (current) {
        if (!success) {
            if (operation == EditorEngineOperation::Load &&
                derivedFailureRecoveryPending_) {
                derivedFailureRecoveryPending_ = false;
            }
            setStatus(errorMessage);
            if (operation == EditorEngineOperation::Load ||
                operation == EditorEngineOperation::Update) {
                setPreviewStale(true);
            }
            if (operation == EditorEngineOperation::Update &&
                snapshot_.has_value()) {
                queueLoad(*snapshot_, true);
            }
        } else {
            switch (operation) {
                case EditorEngineOperation::Load:
                    if (derivedFailureRecoveryPending_) {
                        derivedFailureRecoveryPending_ = false;
                        setPreviewStale(true);
                    } else {
                        setPreviewStale(false);
                        queueFrame(playhead_);
                    }
                    break;
                case EditorEngineOperation::Play:
                    setPlaying(true);
                    break;
                case EditorEngineOperation::Pause:
                    setPlaying(false);
                    break;
                case EditorEngineOperation::Seek:
                    playhead_ = *command.position;
                    emit playheadChanged();
                    if (playing_) {
                        playbackStart_ = playhead_;
                        playbackClock_.restart();
                    }
                    queueFrame(playhead_);
                    break;
                case EditorEngineOperation::Update:
                    setPreviewStale(false);
                    queueFrame(playhead_);
                    break;
                case EditorEngineOperation::Frame:
                    break;
            }
        }
    }
    if (!busy()) emit busyChanged();
    dispatchNext();
}

void EditorController::handleFrameCompleted(
    quint64 generation, quint64 commandId, bool success,
    const QString& errorMessage, qlonglong revision, qlonglong positionNs,
    QImage image) {
    const auto found = commands_.find(commandId);
    if (found == commands_.end()) return;
    const PendingCommand command = found->second;
    const bool current = generation == generation_ &&
                         command.generation == generation_ &&
                         command.operation == EditorEngineOperation::Frame;
    commands_.erase(found);
    frameRequestInFlight_ = false;
    workerCommandActive_ = false;

    if (current) {
        const bool commandBecameStale =
            !snapshot_.has_value() || !command.expectedRevision.has_value() ||
            *command.expectedRevision != snapshot_->revision.value();
        const bool returnedExpectedRevision = command.expectedRevision.has_value() &&
                                              revision == *command.expectedRevision;
        const bool returnedExpectedPosition =
            command.position.has_value() &&
            positionNs == command.position->time_since_epoch().count();
        if (commandBecameStale) {
            // The durable timeline changed while the worker decoded this frame.
            // The queued update/load will request a frame for the new revision.
        } else if (!success || !returnedExpectedRevision ||
                   !returnedExpectedPosition || image.isNull()) {
            if (success && !returnedExpectedRevision) {
                setStatus(QStringLiteral(
                    "Edit engine returned the wrong preview revision"));
            } else if (success && !returnedExpectedPosition) {
                setStatus(QStringLiteral(
                    "Edit engine returned the wrong preview position"));
            } else {
                setStatus(errorMessage);
            }
            setPreviewStale(true);
            setPlaying(false);
        } else {
            previewImage_ = std::move(image);
            playhead_ = core::TimestampNs{core::DurationNs{positionNs}};
            emit previewImageChanged();
            emit playheadChanged();
        }
    }
    dispatchNext();
}

void EditorController::requestPlaybackFrame() {
    if (!playing_ || !snapshot_.has_value() || previewStale_ ||
        frameRequestInFlight_) {
        return;
    }
    const auto elapsed = core::DurationNs{playbackClock_.nsecsElapsed()};
    const auto unquantized = playbackStart_ + elapsed;
    const auto rate = snapshot_->timeline.frameRate();
    const auto frameIndex = core::timestampToFrame(unquantized, rate);
    const auto frameCount = timelineFrameCount(snapshot_->timeline);
    if (frameCount <= 0) {
        setPlaying(false);
        queueSimple(EditorEngineOperation::Pause);
        return;
    }
    if (frameIndex >= frameCount) {
        const auto lastPosition = core::frameToTimestamp(frameCount - 1, rate);
        if (lastPosition > playhead_) queueFrame(lastPosition);
        setPlaying(false);
        queueSimple(EditorEngineOperation::Pause);
        return;
    }
    const auto position = core::frameToTimestamp(frameIndex, rate);
    if (position <= playhead_) return;
    queueFrame(position);
}

void EditorController::setPreviewStale(bool value) {
    if (previewStale_ == value) return;
    previewStale_ = value;
    emit previewStaleChanged();
}

void EditorController::setPlaying(bool value) {
    if (playing_ == value) return;
    playing_ = value;
    if (playing_) {
        playbackStart_ = playhead_;
        playbackClock_.restart();
        const auto rate = snapshot_->timeline.frameRate();
        const auto interval = std::max<std::int64_t>(
            1, (1000 * rate.denominator()) / rate.numerator());
        playbackTimer_.start(static_cast<int>(interval));
    } else {
        playbackTimer_.stop();
    }
    emit playingChanged();
}

void EditorController::setStatus(QString value) {
    if (statusMessage_ == value) return;
    statusMessage_ = std::move(value);
    emit statusMessageChanged();
}

}  // namespace creator::app
