#include "media/MediaTypes.h"

#include "core/Timebase.h"

#include <memory>
#include <type_traits>

namespace {

using creator::media::AudioBlock;
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
static_assert(std::is_same_v<decltype(AudioBlock::samples), std::shared_ptr<const float[]>>,
              "audio samples must be owned through a smart pointer and immutable");

// Frames are handed to both the preview and the encoder, so they must be
// copyable - shared_ptr makes that cheap and keeps the texture alive for both.
static_assert(std::is_copy_constructible_v<VideoFrame>);
static_assert(std::is_copy_constructible_v<AudioBlock>);

// Timestamps on the project timebase, never a bare integer (CLAUDE.md 4).
// The regression this guards is real and named in core/Timebase.h: the starter
// pack aliased TimestampNs to a duration, which let timestamp + timestamp
// compile.
static_assert(std::is_same_v<decltype(VideoFrame::timestamp), creator::core::TimestampNs>,
              "frame timestamps must be on the project timebase");
static_assert(std::is_same_v<decltype(AudioBlock::timestamp), creator::core::TimestampNs>);

// This file deliberately has no TEST(). Everything worth checking here is a
// compile-time property, and the assertions above run at build time - a failing
// one breaks the build, which is a louder signal than a red test.
//
// Two things this file must NOT do, both of which earlier drafts did:
//
// 1. `static_assert(VideoFrame{}.pixelFormat == PixelFormat::Unknown)`. It does
//    not compile. VideoFrame{} is a temporary destroyed inside the constant
//    evaluation, and an implicit destructor is constexpr only if every
//    subobject's is - std::shared_ptr's is not, so VideoFrame is not a literal
//    type. MSVC says C2131. The same trap applies to any AudioBlock{}.field.
//
// 2. Runtime tests that read back default member initialisers - EXPECT_EQ on
//    width == 0, sampleRate == 48000. Those assert that the compiler applied
//    the initialiser written three lines away in the header. They look like
//    coverage and are worth nothing.
//
// The object file links into cs_tests either way: MediaTypesTest.cpp is listed
// directly on add_executable, so it is compiled and linked whether or not it
// defines a TEST. (Archive-member pruning is a static-library concern, not this.)

}  // namespace
