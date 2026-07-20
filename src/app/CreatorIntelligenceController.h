#pragma once

#include "core/Result.h"
#include "cut_suggest/CutSuggestion.h"
#include "edit_engine/EditEngineTypes.h"
#include "transcription/ITranscriptionProvider.h"
#include "transcription/Transcript.h"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace creator::app {

struct CreatorIntelligenceProposal final {
    transcription::Transcript transcript;
    std::vector<cut_suggest::CutSuggestion> cuts;
    std::int64_t timelineRevision{};
};

/// Local-AI review boundary. Analysis runs off the UI thread and produces only
/// proposals; the timeline and transcript store are untouched until a user
/// explicitly approves an individual artifact.
class CreatorIntelligenceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasPendingProposal READ hasPendingProposal
                   NOTIFY proposalChanged)
    Q_PROPERTY(QVariantList transcriptProposal READ transcriptProposal
                   NOTIFY proposalChanged)
    Q_PROPERTY(QVariantList cutSuggestions READ cutSuggestions
                   NOTIFY proposalChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage
                   NOTIFY statusMessageChanged)

public:
    using AudioLoader = std::function<core::Result<std::vector<float>>(
        const edit_engine::TimelineSnapshot&, std::stop_token)>;
    using SnapshotProvider = std::function<
        std::optional<edit_engine::TimelineSnapshot>()>;
    using TranscriptApprover = std::function<core::Result<void>(
        const transcription::Transcript&, std::int64_t)>;
    using CutApprover = std::function<core::Result<void>(
        const domain::TimeRange&, std::int64_t)>;

    CreatorIntelligenceController(
        std::unique_ptr<transcription::ITranscriptionProvider> provider,
        AudioLoader audioLoader, SnapshotProvider snapshotProvider,
        TranscriptApprover transcriptApprover, CutApprover cutApprover,
        QObject* parent = nullptr);
    ~CreatorIntelligenceController() override;

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool hasPendingProposal() const noexcept {
        return pendingTranscript_.has_value() || !pendingCuts_.empty();
    }
    [[nodiscard]] QVariantList transcriptProposal() const {
        return transcriptProposal_;
    }
    [[nodiscard]] QVariantList cutSuggestions() const {
        return cutSuggestions_;
    }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }

    Q_INVOKABLE bool analyzeProject();
    Q_INVOKABLE void cancelAnalysis();
    Q_INVOKABLE void rejectProposal();
    Q_INVOKABLE bool approveTranscript();
    Q_INVOKABLE bool approveCut(int index);

signals:
    void busyChanged();
    void proposalChanged();
    void statusMessageChanged();

private:
    using AnalysisResult = core::Result<CreatorIntelligenceProposal>;

    void completeAnalysis(std::uint64_t generation,
                          std::shared_ptr<AnalysisResult> result,
                          bool cancelled);
    void clearProposal();
    void publishProposal(CreatorIntelligenceProposal proposal);
    void setBusy(bool value);
    void setStatus(QString value);

    std::unique_ptr<transcription::ITranscriptionProvider> provider_;
    AudioLoader audioLoader_;
    SnapshotProvider snapshotProvider_;
    TranscriptApprover transcriptApprover_;
    CutApprover cutApprover_;
    std::jthread worker_;
    std::uint64_t generation_{0};
    bool busy_{false};
    bool cancelRequested_{false};
    std::optional<transcription::Transcript> pendingTranscript_;
    std::vector<cut_suggest::CutSuggestion> pendingCuts_;
    std::int64_t proposalRevision_{};
    QVariantList transcriptProposal_;
    QVariantList cutSuggestions_;
    QString statusMessage_;
};

}  // namespace creator::app
