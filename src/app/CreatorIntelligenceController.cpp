#include "app/CreatorIntelligenceController.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "core/AppError.h"
#include "cut_suggest/CutReason.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "cut_suggest/CutSuggestionAnalyzer.h"
#include "domain/Identifiers.h"
#include "transcription/AudioInput.h"

#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>

#include <exception>
#include <utility>

namespace creator::app {

CreatorIntelligenceController::CreatorIntelligenceController(
    std::unique_ptr<transcription::ITranscriptionProvider> provider,
    AudioLoader audioLoader, SnapshotProvider snapshotProvider,
    TranscriptApprover transcriptApprover, CutApprover cutApprover,
    QObject* parent)
    : QObject(parent),
      provider_(std::move(provider)),
      audioLoader_(std::move(audioLoader)),
      snapshotProvider_(std::move(snapshotProvider)),
      transcriptApprover_(std::move(transcriptApprover)),
      cutApprover_(std::move(cutApprover)) {}

CreatorIntelligenceController::~CreatorIntelligenceController() {
    ++generation_;
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

bool CreatorIntelligenceController::analyzeProject() {
    if (busy_) {
        setStatus(QStringLiteral("Local AI analysis is already running"));
        return false;
    }
    if (!provider_ || !audioLoader_ || !snapshotProvider_) {
        setStatus(QStringLiteral("Local AI is not configured"));
        return false;
    }
    auto snapshot = snapshotProvider_();
    if (!snapshot.has_value()) {
        setStatus(QStringLiteral("Open an editable project before analysis"));
        return false;
    }
    if (worker_.joinable()) worker_.join();

    clearProposal();
    cancelRequested_ = false;
    setBusy(true);
    setStatus(QStringLiteral("Analyzing project audio locally..."));
    const auto generation = ++generation_;
    const auto revision = snapshot->revision.value();
    QPointer<CreatorIntelligenceController> guard{this};
    worker_ = std::jthread(
        [this, guard, generation, revision,
         snapshot = std::move(*snapshot)](std::stop_token stop) mutable {
            std::shared_ptr<AnalysisResult> result;
            try {
                auto samples = audioLoader_(snapshot, stop);
                if (!samples.hasValue()) {
                    result = std::make_shared<AnalysisResult>(samples.error());
                } else if (stop.stop_requested()) {
                    result = std::make_shared<AnalysisResult>(core::AppError{
                        core::ErrorCode::InvalidState, "analysis cancelled"});
                } else {
                    auto input = transcription::AudioInput::create(
                        samples.value(), 16'000, 1);
                    auto sourceId = domain::SourceId::create("project-mix");
                    if (!input.hasValue()) {
                        result = std::make_shared<AnalysisResult>(input.error());
                    } else if (!sourceId.hasValue()) {
                        result = std::make_shared<AnalysisResult>(sourceId.error());
                    } else {
                        auto transcript = provider_->transcribe(
                            input.value(), transcription::TranscriptionOptions{
                                               sourceId.value(), {}});
                        if (!transcript.hasValue()) {
                            result = std::make_shared<AnalysisResult>(
                                transcript.error());
                        } else if (stop.stop_requested()) {
                            result = std::make_shared<AnalysisResult>(
                                core::AppError{core::ErrorCode::InvalidState,
                                               "analysis cancelled"});
                        } else {
                            auto format = audio_dsp::AudioFormat::create(
                                16'000, 1);
                            auto parameters =
                                cut_suggest::CutSuggestParameters::create();
                            if (!format.hasValue()) {
                                result = std::make_shared<AnalysisResult>(
                                    format.error());
                            } else if (!parameters.hasValue()) {
                                result = std::make_shared<AnalysisResult>(
                                    parameters.error());
                            } else {
                                audio_dsp::AudioBuffer view{
                                    samples.value().data(),
                                    samples.value().size(), format.value()};
                                cut_suggest::CutSuggestionAnalyzer analyzer{
                                    std::move(parameters).value()};
                                auto cuts = analyzer.analyze(
                                    view, transcript.value());
                                if (!cuts.hasValue()) {
                                    result = std::make_shared<AnalysisResult>(
                                        cuts.error());
                                } else {
                                    result = std::make_shared<AnalysisResult>(
                                        CreatorIntelligenceProposal{
                                            std::move(transcript).value(),
                                            std::move(cuts).value(), revision});
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception&) {
                result = std::make_shared<AnalysisResult>(core::AppError{
                    core::ErrorCode::InvalidState,
                    "local AI analysis failed unexpectedly"});
            } catch (...) {
                result = std::make_shared<AnalysisResult>(core::AppError{
                    core::ErrorCode::InvalidState,
                    "local AI analysis failed unexpectedly"});
            }
            const bool cancelled = stop.stop_requested();
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, result = std::move(result), cancelled] {
                    if (guard) {
                        guard->completeAnalysis(generation, result, cancelled);
                    }
                },
                Qt::QueuedConnection);
        });
    return true;
}

void CreatorIntelligenceController::cancelAnalysis() {
    if (!busy_) return;
    cancelRequested_ = true;
    if (worker_.joinable()) worker_.request_stop();
    setStatus(QStringLiteral("Cancelling local AI analysis..."));
}

void CreatorIntelligenceController::rejectProposal() {
    clearProposal();
    setStatus(QStringLiteral("Local AI proposal discarded"));
}

bool CreatorIntelligenceController::approveTranscript() {
    if (!pendingTranscript_.has_value() || !transcriptApprover_) {
        setStatus(QStringLiteral("There is no transcript proposal to approve"));
        return false;
    }
    auto approved = transcriptApprover_(*pendingTranscript_, proposalRevision_);
    if (!approved.hasValue()) {
        setStatus(QStringLiteral("Transcript approval failed: ") +
                  QString::fromStdString(approved.error().message()));
        return false;
    }
    pendingTranscript_.reset();
    transcriptProposal_.clear();
    emit proposalChanged();
    setStatus(QStringLiteral("Transcript proposal approved"));
    return true;
}

bool CreatorIntelligenceController::approveCut(int index) {
    if (index < 0 || index >= static_cast<int>(pendingCuts_.size()) ||
        !cutApprover_) {
        setStatus(QStringLiteral("There is no cut proposal at that index"));
        return false;
    }
    auto approved = cutApprover_(pendingCuts_[static_cast<std::size_t>(index)]
                                     .span(),
                                 proposalRevision_);
    if (!approved.hasValue()) {
        setStatus(QStringLiteral("Cut approval failed: ") +
                  QString::fromStdString(approved.error().message()));
        return false;
    }
    pendingCuts_.erase(pendingCuts_.begin() + index);
    cutSuggestions_.removeAt(index);
    emit proposalChanged();
    setStatus(QStringLiteral("Cut proposal approved"));
    return true;
}

void CreatorIntelligenceController::completeAnalysis(
    std::uint64_t generation, std::shared_ptr<AnalysisResult> result,
    bool cancelled) {
    if (generation != generation_) return;
    setBusy(false);
    if (cancelled || cancelRequested_) {
        clearProposal();
        setStatus(QStringLiteral("Local AI analysis cancelled"));
        return;
    }
    if (!result || !result->hasValue()) {
        clearProposal();
        setStatus(QStringLiteral("Local AI analysis failed: ") +
                  QString::fromStdString(
                      result ? result->error().message()
                             : std::string{"missing analysis result"}));
        return;
    }
    publishProposal(std::move(result->value()));
}

void CreatorIntelligenceController::clearProposal() {
    const bool changed = hasPendingProposal() || !transcriptProposal_.empty() ||
                         !cutSuggestions_.empty();
    pendingTranscript_.reset();
    pendingCuts_.clear();
    transcriptProposal_.clear();
    cutSuggestions_.clear();
    if (changed) emit proposalChanged();
}

void CreatorIntelligenceController::publishProposal(
    CreatorIntelligenceProposal proposal) {
    proposalRevision_ = proposal.timelineRevision;
    pendingTranscript_ = std::move(proposal.transcript);
    pendingCuts_ = std::move(proposal.cuts);

    transcriptProposal_.clear();
    for (const auto& segment : pendingTranscript_->segments()) {
        transcriptProposal_.push_back(QVariantMap{
            {QStringLiteral("startNs"),
             QVariant::fromValue<qint64>(
                 segment.range().start().time_since_epoch().count())},
            {QStringLiteral("durationNs"),
             QVariant::fromValue<qint64>(segment.range().duration().count())},
            {QStringLiteral("text"), QString::fromUtf8(segment.text())}});
    }
    cutSuggestions_.clear();
    for (const auto& cut : pendingCuts_) {
        QVariantMap item{
            {QStringLiteral("startNs"),
             QVariant::fromValue<qint64>(
                 cut.span().start().time_since_epoch().count())},
            {QStringLiteral("durationNs"),
             QVariant::fromValue<qint64>(cut.span().duration().count())},
            {QStringLiteral("reason"),
             QString::fromUtf8(cut_suggest::toString(cut.reason()))},
            {QStringLiteral("score"), cut.score()}};
        if (cut.label().has_value()) {
            item.insert(QStringLiteral("label"),
                        QString::fromUtf8(*cut.label()));
        }
        cutSuggestions_.push_back(std::move(item));
    }
    emit proposalChanged();
    setStatus(QStringLiteral("Local AI proposed %1 transcript segment(s) and "
                             "%2 cut(s); review before applying")
                  .arg(transcriptProposal_.size())
                  .arg(cutSuggestions_.size()));
}

void CreatorIntelligenceController::setBusy(bool value) {
    if (busy_ == value) return;
    busy_ = value;
    emit busyChanged();
}

void CreatorIntelligenceController::setStatus(QString value) {
    if (statusMessage_ == value) return;
    statusMessage_ = std::move(value);
    emit statusMessageChanged();
}

}  // namespace creator::app
