#include "ffmpeg_adapter/FfmpegMediaProbe.h"

#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"
#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"
#include "recorder/RecordingTrack.h"

extern "C" {
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::domain::SourceId;
using creator::ffmpeg_adapter::FfmpegAudioSegmentEncoder;
using creator::ffmpeg_adapter::FfmpegMediaProbe;
using creator::ffmpeg_adapter::FfmpegVideoSegmentEncoder;
using creator::ffmpeg_adapter::VideoEncoderOptions;
using creator::media::AudioBlock;
using creator::media::VideoFrame;
using creator::recorder::IVideoFrameMapper;
using creator::recorder::MappedVideoFrame;
using creator::recorder::RecordingTrack;
using creator::recorder::SegmentEncodeConfig;
using creator::recorder::TrackRole;

std::uint64_t processId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

class ScopedPathRemoval final {
public:
    explicit ScopedPathRemoval(fs::path path) : path_(std::move(path)) {}
    ScopedPathRemoval(const ScopedPathRemoval&) = delete;
    ScopedPathRemoval& operator=(const ScopedPathRemoval&) = delete;
    ~ScopedPathRemoval() {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

private:
    fs::path path_;
};

#ifdef _WIN32
bool createDirectoryJunction(const fs::path& link, const fs::path& target) {
    std::wstring command = L"cmd.exe /d /c mklink /J \"" + link.wstring() +
                           L"\" \"" + target.wstring() + L"\" >nul";
    std::vector<wchar_t> mutableCommand{command.begin(), command.end()};
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    const bool readExitCode =
        GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return readExitCode && exitCode == 0;
}
#endif

struct Pixels final {
    std::vector<std::uint8_t> bytes;
};

class Mapper final : public IVideoFrameMapper {
public:
    Result<MappedVideoFrame> map(const VideoFrame& frame) override {
        auto pixels = std::static_pointer_cast<Pixels>(frame.platformHandle);
        return MappedVideoFrame{.timestamp = frame.timestamp,
                                .width = frame.width,
                                .height = frame.height,
                                .rowBytes = frame.width * 4ULL,
                                .data = pixels->bytes.data(),
                                .owner = std::move(pixels)};
    }
};

VideoFrame frameAt(std::chrono::milliseconds timestamp, std::uint8_t value) {
    auto pixels = std::make_shared<Pixels>();
    pixels->bytes.resize(64 * 36 * 4, value);
    return VideoFrame{.timestamp = creator::core::TimestampNs{timestamp},
                      .width = 64,
                      .height = 36,
                      .visibleRect = {0, 0, 64, 36},
                      .contentWidth = 64,
                      .contentHeight = 36,
                      .pixelFormat = creator::media::PixelFormat::Bgra8,
                      .platformHandle = std::move(pixels)};
}

AudioBlock monoBlock(std::uint32_t frames = 1024) {
    auto samples = std::shared_ptr<float[]>(new float[frames]);
    for (std::uint32_t index = 0; index < frames; ++index) {
        samples[index] = static_cast<float>(static_cast<int>(index % 17) - 8) /
                         32.0F;
    }
    return AudioBlock{.timestamp = {},
                      .sampleRate = 48'000,
                      .channels = 1,
                      .frameCount = frames,
                      .samples = std::move(samples)};
}

SegmentEncodeConfig config(const fs::path& path, TrackRole role,
                           std::string source) {
    return SegmentEncodeConfig{
        .track = RecordingTrack::create(
                     SourceId::create(std::move(source)).value(), role)
                     .value(),
        .partPath = path,
        .startTime = {},
        .targetDuration = std::chrono::seconds{2},
    };
}

std::string independentSha256(const fs::path& path) {
    auto* context = av_sha_alloc();
    EXPECT_NE(context, nullptr);
    if (context == nullptr) return {};
    EXPECT_EQ(av_sha_init(context, 256), 0);
    std::ifstream input{path, std::ios::binary};
    std::array<std::uint8_t, 4096> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            av_sha_update(context, buffer.data(),
                          static_cast<unsigned int>(count));
        }
    }
    std::array<std::uint8_t, 32> digest{};
    av_sha_final(context, digest.data());
    av_free(context);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : digest) {
        output << std::setw(2) << static_cast<unsigned int>(value);
    }
    return output.str();
}

class FfmpegMediaProbeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() /
                ("creator-studio-media-probe-" + std::to_string(processId()) +
                 "-" + info->name());
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_ / "media");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        EXPECT_FALSE(fs::exists(root_));
    }

    fs::path encodeVideo(std::string name = "video.mkv") {
        const auto path = root_ / "media" / std::move(name);
        FfmpegVideoSegmentEncoder encoder{
            std::make_unique<Mapper>(),
            VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"},
                                .frameRateNumerator = 30,
                                .frameRateDenominator = 1}};
        EXPECT_TRUE(encoder.start(config(path, TrackRole::Screen, "screen")).hasValue());
        EXPECT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{0}, 10)).hasValue());
        EXPECT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{33}, 80)).hasValue());
        EXPECT_TRUE(encoder.accept(frameAt(std::chrono::milliseconds{66}, 160)).hasValue());
        EXPECT_TRUE(encoder.finish(
                                creator::core::TimestampNs{
                                    std::chrono::milliseconds{100}})
                        .hasValue());
        return path;
    }

    fs::path encodeAudio() {
        const auto path = root_ / "media" / "audio.mka";
        FfmpegAudioSegmentEncoder encoder;
        EXPECT_TRUE(encoder.start(
                                config(path, TrackRole::Microphone, "microphone"))
                        .hasValue());
        EXPECT_TRUE(encoder.accept(monoBlock()).hasValue());
        EXPECT_TRUE(encoder.finish(
                                creator::core::TimestampNs{
                                    std::chrono::milliseconds{22}})
                        .hasValue());
        return path;
    }

    fs::path root_;
};

TEST_F(FfmpegMediaProbeTest, ProbesVideoContainerAndIndependentHashExactly) {
    const auto path = encodeVideo();
    FfmpegMediaProbe probe;

    const auto result = probe.probe(root_, fs::path{"media/video.mkv"});

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_TRUE(result.value().video.has_value());
    EXPECT_FALSE(result.value().audio.has_value());
    EXPECT_EQ(result.value().video->width, 64);
    EXPECT_EQ(result.value().video->height, 36);
    EXPECT_EQ(result.value().video->frameRate,
              creator::core::FrameRate::create(30, 1).value());
    EXPECT_EQ(result.value().codecName, "mpeg4");
    EXPECT_FALSE(result.value().formatName.empty());
    EXPECT_EQ(result.value().byteSize, fs::file_size(path));
    EXPECT_EQ(result.value().sha256, independentSha256(path));
    EXPECT_LE(std::chrono::abs(result.value().duration -
                               std::chrono::milliseconds{100}),
              std::chrono::milliseconds{34});
}

TEST_F(FfmpegMediaProbeTest, EncodedSegmentEndNeverExceedsPhysicalVideoDuration) {
    const fs::path relative{"media/duration-video.mkv"};
    FfmpegVideoSegmentEncoder encoder{
        std::make_unique<Mapper>(),
        VideoEncoderOptions{.preferredEncoderNames = {"mpeg4"},
                            .frameRateNumerator = 60,
                            .frameRateDenominator = 1}};
    ASSERT_TRUE(encoder.start(
                    config(root_ / relative, TrackRole::Screen, "screen"))
                    .hasValue());
    for (std::int64_t frame = 0; frame < 60; ++frame) {
        ASSERT_TRUE(encoder.accept(frameAt(
                                   std::chrono::milliseconds{
                                       frame * 1'000LL / 30LL},
                                   static_cast<std::uint8_t>(frame)))
                        .hasValue());
    }
    const auto encoded = encoder.finish(
        creator::core::TimestampNs{std::chrono::seconds{2}});
    ASSERT_TRUE(encoded.hasValue()) << encoded.error().message();

    FfmpegMediaProbe probe;
    const auto physical = probe.probe(root_, relative);
    ASSERT_TRUE(physical.hasValue()) << physical.error().message();
    EXPECT_LE(encoded.value().endTime.time_since_epoch(),
              physical.value().duration);
}

TEST_F(FfmpegMediaProbeTest, ProbesMonoAudioContainerExactly) {
    const auto path = encodeAudio();
    FfmpegMediaProbe probe;

    const auto result = probe.probe(root_, fs::path{"media/audio.mka"});

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_FALSE(result.value().video.has_value());
    ASSERT_TRUE(result.value().audio.has_value());
    EXPECT_EQ(result.value().audio->sampleRate, 48'000);
    EXPECT_EQ(result.value().audio->channels, 1);
    EXPECT_EQ(result.value().codecName, "aac");
    EXPECT_FALSE(result.value().formatName.empty());
    EXPECT_EQ(result.value().byteSize, fs::file_size(path));
    EXPECT_EQ(result.value().sha256, independentSha256(path));
    EXPECT_LE(std::chrono::abs(result.value().duration -
                               std::chrono::milliseconds{22}),
              std::chrono::milliseconds{22});
}

TEST_F(FfmpegMediaProbeTest, ProbesUnicodePackageRelativePath) {
    const auto original = encodeVideo("unicode-source.mkv");
    const fs::path relative{u8"미디어/영상.mkv"};
    std::error_code error;
    fs::create_directories((root_ / relative).parent_path(), error);
    ASSERT_FALSE(error) << error.message();
    fs::rename(original, root_ / relative, error);
    ASSERT_FALSE(error) << error.message();
    FfmpegMediaProbe probe;

    const auto result = probe.probe(root_, relative);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_TRUE(result.value().video.has_value());
    EXPECT_EQ(result.value().video->width, 64);
    EXPECT_EQ(result.value().sha256, independentSha256(root_ / relative));
}

TEST_F(FfmpegMediaProbeTest, RejectsMissingDirectoryTraversalAndTruncatedInput) {
    FfmpegMediaProbe probe;
    const auto missing = probe.probe(root_, "media/missing.mkv");
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(probe.probe(root_, "media").hasValue());
    EXPECT_FALSE(probe.probe(root_, "../outside.mkv").hasValue());
    {
        std::ofstream empty{root_ / "media" / "empty.mkv", std::ios::binary};
    }
    EXPECT_FALSE(probe.probe(root_, "media/empty.mkv").hasValue());

    const auto valid = encodeVideo("valid.mkv");
    std::ifstream input{valid, std::ios::binary};
    std::array<char, 32> prefix{};
    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    {
        std::ofstream truncated{root_ / "media" / "truncated.mkv",
                                std::ios::binary | std::ios::trunc};
        truncated.write(prefix.data(), input.gcount());
    }
    const auto result = probe.probe(root_, "media/truncated.mkv");
    ASSERT_FALSE(result.hasValue());
    EXPECT_TRUE(result.error().code() == ErrorCode::ParseFailure ||
                result.error().code() == ErrorCode::IoFailure);
}

TEST_F(FfmpegMediaProbeTest, RejectsHardLinkedMediaFile) {
    const auto outside = fs::temp_directory_path() /
                         ("creator-studio-media-probe-outside-" +
                          std::to_string(processId()) + ".mkv");
    const ScopedPathRemoval cleanupOutside{outside};
    std::error_code error;
    fs::remove(outside, error);
    const auto encoded = encodeVideo("inside.mkv");
    fs::rename(encoded, outside, error);
    ASSERT_FALSE(error) << error.message();
    fs::create_hard_link(outside, root_ / "media" / "linked.mkv", error);
    ASSERT_FALSE(error) << error.message();
    FfmpegMediaProbe probe;

    const auto result = probe.probe(root_, "media/linked.mkv");

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    fs::remove(root_ / "media" / "linked.mkv", error);
}

#ifdef _WIN32
TEST_F(FfmpegMediaProbeTest, LocksOneFileIdentityAcrossMetadataSizeAndHash) {
    const auto path = encodeVideo("locked.mkv");
    const auto replacement = encodeVideo("replacement.mkv");
    {
        std::ofstream append{path, std::ios::binary | std::ios::app};
        std::array<char, 64 * 1024> padding{};
        for (int block = 0; block < 256; ++block) {
            append.write(padding.data(),
                         static_cast<std::streamsize>(padding.size()));
        }
    }
    const auto expectedSize = fs::file_size(path);
    const auto expectedHash = independentSha256(path);
    FfmpegMediaProbe probe;
    const auto probed = probe.probe(root_, "media/locked.mkv");

    ASSERT_TRUE(probed.hasValue()) << probed.error().message();
    EXPECT_EQ(probed.value().byteSize, expectedSize);
    EXPECT_EQ(probed.value().sha256, expectedHash);
    ASSERT_TRUE(probed.value().identityLease);
    EXPECT_TRUE(
        probed.value().identityLease->verifyCurrentIdentity().hasValue());
    HANDLE writer = CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_SHARING_VIOLATION);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    EXPECT_FALSE(MoveFileExW(replacement.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING));
    EXPECT_TRUE(fs::exists(replacement));
}
#endif

#ifndef _WIN32
TEST_F(FfmpegMediaProbeTest, RejectsConcurrentSameSizeMutation) {
    const auto path = encodeVideo("mutating.mkv");
    {
        std::ofstream append{path, std::ios::binary | std::ios::app};
        std::array<char, 64 * 1024> padding{};
        for (int block = 0; block < 1'024; ++block) {
            append.write(padding.data(),
                         static_cast<std::streamsize>(padding.size()));
        }
    }
    FfmpegMediaProbe probe;
    auto result = std::async(std::launch::async, [&] {
        return probe.probe(root_, "media/mutating.mkv");
    });
    const int writer = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(writer, 0);
    std::uint8_t value = 0;
    while (result.wait_for(std::chrono::milliseconds{0}) !=
           std::future_status::ready) {
        value ^= 0xffU;
        ASSERT_EQ(pwrite(writer, &value, 1, 4'096), 1);
    }
    close(writer);

    const auto probed = result.get();

    ASSERT_FALSE(probed.hasValue());
    EXPECT_EQ(probed.error().code(), ErrorCode::IoFailure);
}
#endif

TEST_F(FfmpegMediaProbeTest, RejectsRedirectedMediaPath) {
    const auto target = encodeVideo("target.mkv");
    std::error_code error;
#ifdef _WIN32
    const auto link = root_ / "redirected";
    ASSERT_TRUE(createDirectoryJunction(link, root_ / "media"));
    const fs::path relative{"redirected/target.mkv"};
#else
    const auto link = root_ / "media" / "link.mkv";
    fs::create_symlink(target, link, error);
    ASSERT_FALSE(error) << error.message();
    const fs::path relative{"media/link.mkv"};
#endif

    FfmpegMediaProbe probe;
    const auto result = probe.probe(root_, relative);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    ASSERT_TRUE(fs::remove(link, error));
}

}  // namespace
