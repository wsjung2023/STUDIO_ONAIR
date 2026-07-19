#include "avatar/AvatarMotionNdjsonSink.h"
#include "avatar/AvatarMotionPipeline.h"
#include "avatar/AvatarTrackingSession.h"
#include "avatar/CalibrationProfile.h"
#include "avatar/openseeface/OpenSeeFaceParser.h"
#include "avatar/openseeface/OpenSeeFaceUdpSource.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using creator::avatar::AvatarMotionNdjsonSink;
using creator::avatar::AvatarMotionPipeline;
using creator::avatar::AvatarTrackingSession;
using creator::avatar::CalibrationProfile;
using creator::avatar::openseeface::OpenSeeFaceUdpSource;
using creator::avatar::openseeface::kFaceRecordSizeBytes;
using creator::core::TimestampNs;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

void closeSocket(Socket socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool sendFace(std::uint16_t port) {
    const Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) return false;
    std::array<std::byte, kFaceRecordSizeBytes> packet{};
    packet[28] = std::byte{1};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto sent = sendto(
        socket, reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()), 0,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closeSocket(socket);
    return sent == static_cast<int>(packet.size());
}

TEST(AvatarTrackingSessionTest, DrivesOpenSeeFaceSourceIntoTelemetry) {
    const auto root = fs::temp_directory_path() / "creator-studio-avatar-session";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "telemetry");
    AvatarMotionNdjsonSink sink(root / "telemetry");
    AvatarMotionPipeline pipeline{
        creator::avatar::AvatarProviderId::create("openseeface").value(),
        CalibrationProfile::identity(), sink};
    auto source = std::make_unique<OpenSeeFaceUdpSource>();
    auto* sourcePointer = source.get();
    AvatarTrackingSession session{std::move(source), pipeline};
    ASSERT_TRUE(session.start(0).hasValue());
    ASSERT_TRUE(sendFace(sourcePointer->boundPort()));
    std::optional<creator::avatar::AvatarMotionSample> sample;
    for (int attempt = 0; attempt < 1000 && !sample.has_value(); ++attempt) {
        auto polled = session.poll(TimestampNs{});
        ASSERT_TRUE(polled.hasValue()) << polled.error().message();
        sample = std::move(polled).value();
        if (!sample.has_value()) std::this_thread::yield();
    }
    ASSERT_TRUE(sample.has_value());
    EXPECT_TRUE(session.running());
    session.stop();
    EXPECT_FALSE(session.running());
    std::ifstream input(root / "telemetry" / AvatarMotionNdjsonSink::kFileName);
    std::string line;
    EXPECT_TRUE(std::getline(input, line));
    EXPECT_NE(line.find("avatar.motion"), std::string::npos);
    fs::remove_all(root, error);
}

}  // namespace
