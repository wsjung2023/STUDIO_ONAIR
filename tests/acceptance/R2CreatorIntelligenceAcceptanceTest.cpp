// R2-07 automated acceptance test for the "creator intelligence" surface.
//
// This exercises the R2 intelligence features end to end using only the FAKE
// providers and the DEFAULT (all gates OFF) build — no real MLT, whisper.cpp, or
// RNNoise is required — and asserts the cross-cutting contract:
//   * transcription: FakeTranscriptionProvider -> Transcript -> TranscriptStore
//     round-trips and the serialized document validates against its schema;
//   * cursor: FakeCursorSource -> CursorNormalizer -> events -> AutoZoomAnalyzer
//     yields candidates and EmphasisPlanner yields a plan, both schema-valid;
//   * audio: the denoise-omitted cleanup chain (compressor -> true-peak limiter)
//     runs a synthetic program, and ExportLoudnessAnalyzer's decision, when
//     applied (LoudnessNormalizer), drives the program to target LUFS;
//   * invariants: every artifact carries project-timebase timestamps, the
//     loudness decision reaches target within tolerance, and every serialized
//     artifact validates against its committed schema.
//
// OUT OF SCOPE (documented, not silently skipped):
//   * The physical R2-07 gate — a real 30-min capture -> edit -> export whose
//     rendered file is checked for A/V sync + integrated loudness on target — can
//     only run in an ENABLED preset (CS_ENABLE_MLT/FFMPEG/WHISPER/RNNOISE) on a
//     real machine. MLT export atomicity / cancel / retry likewise need the real
//     engine. Those are the enabled-preset + real-machine step, asserted by the
//     R1*/enabled acceptance suites, and are deliberately not reachable here.
//   * cut-suggest (CutSuggestionAnalyzer) is not present in this worktree's tree,
//     so it is not linked or asserted here; when it lands it should be added to
//     this suite over the synthetic audio + transcript built below.

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioCleanupChain.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/ExportLoudnessAnalysis.h"
#include "audio_dsp/LoudnessNormalizer.h"
#include "autozoom/AutoZoomAnalyzer.h"
#include "autozoom/AutoZoomParameters.h"
#include "autozoom/ZoomCandidate.h"
#include "autozoom/ZoomCandidateSerializer.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorNormalizer.h"
#include "cursor/CursorPoint.h"
#include "cursor/ICursorSource.h"
#include "cursor_emphasis/EmphasisPlan.h"
#include "cursor_emphasis/EmphasisPlanParameters.h"
#include "cursor_emphasis/EmphasisPlanSerializer.h"
#include "cursor_emphasis/EmphasisPlanner.h"
#include "domain/Identifiers.h"
#include "fakes/FakeCursorSource.h"
#include "transcription/AudioInput.h"
#include "transcription/FakeTranscriptionProvider.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptSerializer.h"
#include "transcription/TranscriptStore.h"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace creator {
namespace {

using core::DurationNs;
using core::TimestampNs;

constexpr std::int64_t msToNs(std::int64_t ms) {
    return std::chrono::duration_cast<DurationNs>(std::chrono::milliseconds{ms})
        .count();
}
TimestampNs at(std::int64_t ns) { return TimestampNs{DurationNs{ns}}; }

// --- schema validation (mirrors tests/audio_dsp/support/EventSchemaValidator) -

// Validates `document` against the JSON schema at `schemaPath`. Returns true on
// success; on failure fills `whyNot` with a pointer + message for the log. The
// schema files are the ones the product ships (paths injected by the build), so
// this proves the serialized artifacts match the committed contract.
bool validatesAgainstSchema(const std::string& schemaPath,
                            const nlohmann::json& document, std::string* whyNot) {
    std::ifstream file(schemaPath);
    if (!file.is_open()) {
        if (whyNot != nullptr) *whyNot = "cannot open schema: " + schemaPath;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    nlohmann::json_schema::json_validator validator{
        nullptr, nlohmann::json_schema::default_string_format_check};
    validator.set_root_schema(nlohmann::json::parse(buffer.str()));

    class Handler final : public nlohmann::json_schema::basic_error_handler {
    public:
        void error(const nlohmann::json::json_pointer& pointer,
                   const nlohmann::json& instance,
                   const std::string& message) override {
            nlohmann::json_schema::basic_error_handler::error(pointer, instance,
                                                              message);
            if (!failed) {
                failed = true;
                why = "at '" + pointer.to_string() + "': " + message;
            }
        }
        bool failed{false};
        std::string why;
    } handler;

    validator.validate(document, handler);
    if (handler.failed && whyNot != nullptr) *whyNot = handler.why;
    return !handler.failed;
}

// --- fixtures ----------------------------------------------------------------

domain::SourceId screenSource() {
    return domain::SourceId::create("screen-1").value();
}

// A synthetic mono program long enough for the fake to script several segments.
std::vector<float> makeSpeechLikeMono(int seconds, std::int32_t sampleRate) {
    std::vector<float> samples(static_cast<std::size_t>(seconds) *
                                   static_cast<std::size_t>(sampleRate),
                               0.0F);
    // A quiet 200 Hz tone: finite, non-empty, deterministic. The fake keys the
    // transcript off DURATION, not sample values, so the waveform only has to be
    // a valid non-silent buffer.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        samples[i] = static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979 *
                                                       200.0 * t));
    }
    return samples;
}

// Interleaved stereo 1 kHz sine at a given dBFS — the canonical loudness fixture.
std::vector<float> makeStereoSine(std::size_t frames, double levelDbfs) {
    const double amp = std::pow(10.0, levelDbfs / 20.0);
    std::vector<float> s(frames * 2, 0.0F);
    for (std::size_t f = 0; f < frames; ++f) {
        const double v =
            amp * std::sin(2.0 * 3.14159265358979 * 1000.0 * f / 48'000.0);
        s[f * 2] = static_cast<float>(v);
        s[f * 2 + 1] = static_cast<float>(v);
    }
    return s;
}

// =============================================================================
// Transcription: fake provider -> Transcript -> store round-trip + schema.
// =============================================================================
TEST(R2CreatorIntelligenceAcceptance, TranscriptionRoundTripsAndValidates) {
    const std::vector<float> mono = makeSpeechLikeMono(5, 16'000);
    auto audio = transcription::AudioInput::create(mono, 16'000, 1);
    ASSERT_TRUE(audio.hasValue());

    transcription::FakeTranscriptionProvider provider;
    transcription::TranscriptionOptions options{screenSource(), "en"};
    auto transcript = provider.transcribe(audio.value(), options);
    ASSERT_TRUE(transcript.hasValue());
    ASSERT_FALSE(transcript.value().segments().empty())
        << "5 s of audio should script at least one segment";
    EXPECT_EQ(transcript.value().sourceId(), screenSource());

    // Invariant: segments are strictly ordered and non-negative on the project
    // timebase (Transcript::create already enforces this; assert it explicitly).
    TimestampNs previousEnd = at(0);
    for (const auto& seg : transcript.value().segments()) {
        EXPECT_GE(seg.range().start().time_since_epoch().count(), 0);
        EXPECT_GE(seg.range().start(), previousEnd);
        previousEnd = seg.range().end();
    }

    // Serialized document validates against the committed transcript schema.
    const nlohmann::json doc =
        transcription::TranscriptSerializer::toJson(transcript.value());
    std::string why;
    EXPECT_TRUE(validatesAgainstSchema(CS_TRANSCRIPT_SCHEMA_PATH, doc, &why))
        << why;

    // Durable store round-trip: write then read yields an equal Transcript, and
    // read re-validates through the serializer (a corrupt file would be rejected).
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("r2accept_tx_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    transcription::TranscriptStore store{dir};
    auto written = store.write("captions", transcript.value());
    ASSERT_TRUE(written.hasValue()) << written.error().message();
    auto readBack = store.read(written.value());
    ASSERT_TRUE(readBack.hasValue()) << readBack.error().message();
    EXPECT_EQ(readBack.value(), transcript.value());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// =============================================================================
// Cursor: FakeCursorSource -> normalize -> events -> autozoom + emphasis.
// =============================================================================
TEST(R2CreatorIntelligenceAcceptance, CursorDrivesAutoZoomAndEmphasis) {
    // Script a tight dwell (~2 s) at the frame centre with two clicks — enough
    // for AutoZoom to raise a candidate and for the planner to emit click
    // directives and an idle hide span.
    constexpr std::uint32_t kW = 1920;
    constexpr std::uint32_t kH = 1080;
    std::vector<cursor::RawCursorSample> script;
    for (std::int64_t ms = 0; ms <= 2400; ms += 50) {
        script.push_back(cursor::RawCursorMoveSample{
            at(msToNs(ms)), 960, 540, kW, kH});
    }
    script.push_back(cursor::RawCursorClickSample{at(msToNs(800)), 960, 540, kW,
                                                  kH, /*left*/ 0});
    script.push_back(cursor::RawCursorClickSample{at(msToNs(900)), 960, 540, kW,
                                                  kH, /*left*/ 0});

    // Drive the fake source, NORMALIZING each raw sample into a domain event —
    // exactly the raw -> normalized -> event path the real backend feeds.
    fakes::FakeCursorSource source{script};
    std::vector<cursor::CursorMoveEvent> moves;
    std::vector<cursor::CursorClickEvent> clicks;
    while (auto raw = source.poll()) {
        std::visit(
            [&](auto&& s) {
                using T = std::decay_t<decltype(s)>;
                auto point = cursor::CursorNormalizer::normalize(
                    s.x, s.y, s.sourceWidth, s.sourceHeight);
                ASSERT_TRUE(point.hasValue());
                if constexpr (std::is_same_v<T, cursor::RawCursorMoveSample>) {
                    auto ev = cursor::CursorMoveEvent::create(s.tNs, point.value(),
                                                              screenSource());
                    ASSERT_TRUE(ev.hasValue());
                    moves.push_back(std::move(ev).value());
                } else {
                    auto ev = cursor::CursorClickEvent::create(
                        s.tNs, point.value(), cursor::CursorButton::Left);
                    ASSERT_TRUE(ev.hasValue());
                    clicks.push_back(std::move(ev).value());
                }
            },
            *raw);
    }
    EXPECT_TRUE(source.exhausted());
    ASSERT_FALSE(moves.empty());
    ASSERT_EQ(clicks.size(), 2U);

    // AutoZoom produces at least one candidate; it carries a project-timebase
    // range and a real score, and its serialized form validates against schema.
    autozoom::AutoZoomAnalyzer analyzer{
        autozoom::AutoZoomParameters::create().value()};
    auto candidates = analyzer.analyze(moves, clicks);
    ASSERT_TRUE(candidates.hasValue()) << candidates.error().message();
    ASSERT_FALSE(candidates.value().empty())
        << "a 2 s tight dwell with clicks must yield a zoom candidate";
    for (const auto& c : candidates.value()) {
        EXPECT_GE(c.span().start().time_since_epoch().count(), 0);
        EXPECT_GT(c.span().duration().count(), 0);
        EXPECT_GT(c.score(), 0.0);
        auto json = autozoom::ZoomCandidateSerializer::toJson(c);
        ASSERT_TRUE(json.hasValue());
        std::string why;
        EXPECT_TRUE(validatesAgainstSchema(CS_ZOOM_CANDIDATE_SCHEMA_PATH,
                                           json.value(), &why))
            << why;
    }

    // EmphasisPlanner produces one directive per click, each on the timebase, and
    // the plan validates against schema.
    cursor_emphasis::EmphasisPlanner planner{
        cursor_emphasis::EmphasisPlanParameters::create().value()};
    auto plan = planner.plan(moves, clicks);
    ASSERT_TRUE(plan.hasValue()) << plan.error().message();
    EXPECT_EQ(plan.value().clicks().size(), clicks.size());
    for (const auto& directive : plan.value().clicks()) {
        EXPECT_GE(directive.startNs().time_since_epoch().count(), 0);
        EXPECT_GT(directive.duration().count(), 0);
    }
    auto planJson = cursor_emphasis::EmphasisPlanSerializer::toJson(plan.value());
    ASSERT_TRUE(planJson.hasValue());
    std::string why;
    EXPECT_TRUE(validatesAgainstSchema(CS_EMPHASIS_PLAN_SCHEMA_PATH,
                                       planJson.value(), &why))
        << why;
}

// =============================================================================
// Audio: cleanup chain runs; ExportLoudnessAnalyzer's decision drives to target.
// =============================================================================
TEST(R2CreatorIntelligenceAcceptance, CleanupChainAndLoudnessReachTarget) {
    const audio_dsp::AudioFormat fmt = audio_dsp::AudioFormat::create(48'000, 2)
                                           .value();
    constexpr std::size_t kFrames = 48'000 * 2;  // 2 s

    // Default (gate-OFF) build: no denoise node — compressor -> true-peak limiter.
    auto chain = audio_dsp::makeAudioCleanupChain(fmt, nullptr);
    ASSERT_TRUE(chain.hasValue());
    EXPECT_EQ(chain.value()->size(), 2U);

    {
        // The cleanup chain processes a program without error and reports its
        // limiter look-ahead latency for A/V-sync compensation.
        std::vector<float> program = makeStereoSine(kFrames, -12.0);
        audio_dsp::AudioBuffer view{program.data(), kFrames, fmt};
        ASSERT_TRUE(chain.value()->process(view).hasValue());
        EXPECT_GT(chain.value()->latencyFrames(), 0U);
    }

    // Loudness: a -23 LUFS program with a -14 target must be DECIDED for ~+9 dB
    // by the two-pass analyzer, and applying that decision (LoudnessNormalizer,
    // which uses the identical gain) must land the program on target.
    audio_dsp::ExportLoudnessAnalyzer::Parameters params;
    params.targetLufs = -14.0;
    params.truePeakCeilingDbtp = -1.0;
    auto analyzer = audio_dsp::ExportLoudnessAnalyzer::create(params);
    ASSERT_TRUE(analyzer.hasValue());

    std::vector<float> program = makeStereoSine(kFrames, -23.0);
    auto decision = analyzer.value().analyze(program, fmt);
    ASSERT_TRUE(decision.hasValue());
    EXPECT_TRUE(decision.value().shouldNormalize);
    EXPECT_NEAR(decision.value().gainDb, 9.0, 1.5);

    audio_dsp::LoudnessNormalizer::Parameters np;
    np.targetLufs = params.targetLufs;
    np.truePeakCeilingDbtp = params.truePeakCeilingDbtp;
    auto normalizer = audio_dsp::LoudnessNormalizer::create(np);
    ASSERT_TRUE(normalizer.hasValue());
    auto applied = normalizer.value().normalize(program, fmt);
    ASSERT_TRUE(applied.hasValue());

    // The analyzer's pass-1 decision equals the gain actually applied, and the
    // program is driven to target within tolerance with the ceiling respected.
    EXPECT_NEAR(decision.value().gainDb, applied.value().appliedGainDb, 1e-6);
    EXPECT_NEAR(applied.value().achievedLufsAfter, params.targetLufs, 0.5);
    EXPECT_LE(applied.value().truePeakAfterDbtp,
              params.truePeakCeilingDbtp + 0.15);
}

}  // namespace
}  // namespace creator
