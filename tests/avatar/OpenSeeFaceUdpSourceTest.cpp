#include "avatar/openseeface/OpenSeeFaceUdpSource.h"

#include "avatar/openseeface/OpenSeeFaceParser.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using creator::avatar::openseeface::OpenSeeFaceUdpSource;
using creator::avatar::openseeface::kFaceRecordSizeBytes;
using creator::core::DurationNs;
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

bool sendDatagram(std::uint16_t port, const std::byte* bytes,
                  std::size_t size) {
    const Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto sent = sendto(
        socket, reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closeSocket(socket);
    return sent == static_cast<int>(size);
}

TEST(OpenSeeFaceUdpSourceTest, StartsOnEphemeralPortAndStopsIdempotently) {
    OpenSeeFaceUdpSource source;
    ASSERT_TRUE(source.start(0).hasValue());
    EXPECT_TRUE(source.running());
    EXPECT_NE(source.boundPort(), 0U);
    source.stop();
    source.stop();
    EXPECT_FALSE(source.running());
    EXPECT_EQ(source.boundPort(), 0U);
    const auto polled = source.poll(TimestampNs{});
    ASSERT_FALSE(polled.hasValue());
    EXPECT_EQ(polled.error().code(), creator::core::ErrorCode::InvalidState);
}

TEST(OpenSeeFaceUdpSourceTest, ReceivesAndParsesOneLoopbackFaceDatagram) {
    OpenSeeFaceUdpSource source;
    ASSERT_TRUE(source.start(0).hasValue());
    std::array<std::byte, kFaceRecordSizeBytes> record{};
    record[28] = std::byte{1};  // success
    ASSERT_TRUE(sendDatagram(source.boundPort(), record.data(), record.size()));

    creator::core::Result<std::vector<creator::avatar::TrackingResult>> result{
        std::vector<creator::avatar::TrackingResult>{}};
    for (int attempt = 0; attempt < 1000; ++attempt) {
        result = source.poll(TimestampNs{DurationNs{123'000'000}});
        ASSERT_TRUE(result.hasValue()) << result.error().message();
        if (!result.value().empty()) break;
        std::this_thread::yield();
    }
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_TRUE(result.value().front().faceFound);
    EXPECT_FLOAT_EQ(result.value().front().confidence, 1.0F);
    EXPECT_EQ(result.value().front().timestamp,
              TimestampNs{DurationNs{123'000'000}});
}

TEST(OpenSeeFaceUdpSourceTest, ReportsMalformedDatagramWithoutKillingSource) {
    OpenSeeFaceUdpSource source;
    ASSERT_TRUE(source.start(0).hasValue());
    const std::array<std::byte, 1> malformed{std::byte{0}};
    ASSERT_TRUE(sendDatagram(source.boundPort(), malformed.data(), malformed.size()));

    creator::core::Result<std::vector<creator::avatar::TrackingResult>> result{
        std::vector<creator::avatar::TrackingResult>{}};
    for (int attempt = 0; attempt < 1000; ++attempt) {
        result = source.poll(TimestampNs{});
        if (!result.hasValue() || !result.value().empty()) break;
        std::this_thread::yield();
    }
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(source.running());
}

}  // namespace
