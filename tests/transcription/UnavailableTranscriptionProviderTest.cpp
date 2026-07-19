#include "transcription/UnavailableTranscriptionProvider.h"

#include "transcription/AudioInput.h"
#include "transcription/ITranscriptionProvider.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using creator::core::AppError;
using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::transcription::AudioInput;
using creator::transcription::ITranscriptionProvider;
using creator::transcription::TranscriptionOptions;
using creator::transcription::UnavailableTranscriptionProvider;

TranscriptionOptions options() {
    return TranscriptionOptions{SourceId::create("cam-1").value(), "en"};
}

AudioInput oneSecondMono(std::vector<float>& backing) {
    backing.assign(16000, 0.1f);
    return AudioInput::create(backing, 16000, 1).value();
}

TEST(UnavailableTranscriptionProviderTest, DefaultReportsNotBuilt) {
    UnavailableTranscriptionProvider provider;
    std::vector<float> backing;
    const auto audio = oneSecondMono(backing);

    const auto result = provider.transcribe(audio, options());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
    // The message must name the missing capability, not fail silently (CLAUDE 9).
    EXPECT_NE(result.error().message().find("whisper"), std::string::npos);
    EXPECT_EQ(provider.reason().code(), ErrorCode::UnsupportedVersion);
}

TEST(UnavailableTranscriptionProviderTest, SurfacesSpecificPreflightError) {
    const AppError specific{ErrorCode::InvalidState,
                            "audited whisper model could not be loaded"};
    UnavailableTranscriptionProvider provider{specific};
    std::vector<float> backing;
    const auto audio = oneSecondMono(backing);

    const auto result = provider.transcribe(audio, options());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error(), specific);
}

// Used through the port type, it is still a valid ITranscriptionProvider that
// never throws across the boundary.
TEST(UnavailableTranscriptionProviderTest, UsableThroughPortInterface) {
    std::unique_ptr<ITranscriptionProvider> provider =
        std::make_unique<UnavailableTranscriptionProvider>();
    std::vector<float> backing;
    const auto audio = oneSecondMono(backing);

    const auto result = provider->transcribe(audio, options());
    EXPECT_FALSE(result.hasValue());
}

}  // namespace
