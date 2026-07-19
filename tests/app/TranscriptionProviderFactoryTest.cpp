#include "app/TranscriptionProviderFactory.h"

#include "transcription/AudioInput.h"
#include "transcription/ITranscriptionProvider.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using creator::app::makeTranscriptionProvider;
using creator::app::TranscriptionProviderOptions;
using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::transcription::AudioInput;
using creator::transcription::ITranscriptionProvider;
using creator::transcription::TranscriptionOptions;

TEST(TranscriptionProviderFactoryTest, ReturnsAProvider) {
    TranscriptionProviderOptions options;
    std::unique_ptr<ITranscriptionProvider> provider =
        makeTranscriptionProvider(options);
    ASSERT_NE(provider, nullptr);
}

#if !defined(CS_APP_ENABLE_WHISPER)
// Default build (whisper gate OFF): the factory yields the Unavailable provider,
// which returns the documented "not built" error instead of pretending to work.
TEST(TranscriptionProviderFactoryTest, UnavailableWhenWhisperNotBuilt) {
    TranscriptionProviderOptions options;
    auto provider = makeTranscriptionProvider(options);
    ASSERT_NE(provider, nullptr);

    std::vector<float> samples(16000, 0.1f);
    const auto audio = AudioInput::create(samples, 16000, 1).value();
    const TranscriptionOptions request{SourceId::create("cam-1").value(), "en"};

    const auto result = provider->transcribe(audio, request);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}
#endif

}  // namespace
