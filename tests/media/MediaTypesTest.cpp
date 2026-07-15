#include "media/MediaTypes.h"

#include "core/Timebase.h"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

namespace {

using creator::media::AudioBlock;
using creator::media::ColorSpace;
using creator::media::PixelFormat;
using creator::media::VideoFrame;

// These types carry no behaviour, so there is nothing to test at runtime that
// the compiler does not already guarantee. What is worth pinning down is the
// design decisions behind them - if one of these breaks, the build should stop.

// Frames move from the capture thread into the encoder queue on every single
// frame. A throwing move would put an allocation on that path, and at 60fps for
// two hours that is 432000 chances to fail in the hot loop.
static_assert(std::is_nothrow_move_constructible_v<VideoFrame>,
              "VideoFrame must move without throwing: it is moved per frame");
static_assert(std::is_nothrow_move_assignable_v<VideoFrame>);
static_assert(std::is_nothrow_move_constructible_v<AudioBlock>,
              "AudioBlock must move without throwing: audio is the master stream");
static_assert(std::is_nothrow_move_assignable_v<AudioBlock>);

// platformHandle owns a GPU texture. A raw pointer here would mean manual
// lifetime management on the frame path, which CLAUDE.md 4 forbids outright.
static_assert(std::is_same_v<decltype(VideoFrame::platformHandle), std::shared_ptr<void>>,
              "platformHandle must own through a smart pointer, not a raw one");
static_assert(!std::is_pointer_v<decltype(VideoFrame::platformHandle)>);
static_assert(std::is_same_v<decltype(AudioBlock::samples), std::shared_ptr<const float[]>>,
              "audio samples must be owned through a smart pointer and immutable");

// Frames are handed to both the preview and the encoder, so they must be
// copyable - shared_ptr makes that cheap and keeps the texture alive for both.
static_assert(std::is_copy_constructible_v<VideoFrame>);

// Timestamps on the project timebase, never a bare integer (CLAUDE.md 4).
static_assert(std::is_same_v<decltype(VideoFrame::timestamp), creator::core::TimestampNs>,
              "frame timestamps must be on the project timebase");
static_assert(std::is_same_v<decltype(AudioBlock::timestamp), creator::core::TimestampNs>);

// Defaulting to a real format would let an unset frame silently look valid.
// This cannot be a static_assert: VideoFrame{} is a temporary, and it is
// destroyed at the end of this full-expression. VideoFrame's destructor is
// implicitly defined, and it is only constexpr if every subobject's destructor
// is - but std::shared_ptr<void>::~shared_ptr() is not constexpr (confirmed:
// MSVC rejects this with C2131, "failed due to a call to a function that
// is undefined or not declared 'constexpr'", naming VideoFrame::~VideoFrame).
// So the check below lives in DefaultFrameIsEmpty instead, where the frame is
// a real runtime object and destruction is not part of a constant expression.

// ARCHITECTURE.md 8.3: SDR Rec.709 end to end for the first release. HDR needs
// colour management across capture, preview, edit and export together, so it is
// a separate release rather than an extra enumerator here.
static_assert(static_cast<int>(ColorSpace::Rec709Sdr) == 0);

// The suite needs one runtime test or the file links nothing into cs_tests.
TEST(MediaTypesTest, DefaultFrameIsEmpty) {
    const VideoFrame frame;

    EXPECT_EQ(frame.width, 0u);
    EXPECT_EQ(frame.height, 0u);
    EXPECT_EQ(frame.pixelFormat, PixelFormat::Unknown)
        << "an unset frame must not claim a real pixel format";
    EXPECT_EQ(frame.platformHandle, nullptr);
    EXPECT_EQ(frame.timestamp.time_since_epoch().count(), 0);
}

TEST(MediaTypesTest, DefaultAudioBlockMatchesCaptureDefaults) {
    const AudioBlock block;

    EXPECT_EQ(block.sampleRate, 48000u);
    EXPECT_EQ(block.channels, 2u);
    EXPECT_EQ(block.frameCount, 0u);
    EXPECT_EQ(block.samples, nullptr);
}

}  // namespace
