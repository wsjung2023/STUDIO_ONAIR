#include "avatar/openseeface/OpenSeeFaceUdpSource.h"

#include "avatar/openseeface/OpenSeeFaceParser.h"
#include "core/AppError.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace creator::avatar::openseeface {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::size_t kMaximumFacesPerDatagram = 32;
constexpr std::size_t kMaximumDatagramBytes =
    kFaceRecordSizeBytes * kMaximumFacesPerDatagram;

AppError socketError(const char* action) {
#ifdef _WIN32
    return AppError{ErrorCode::IoFailure,
                    std::string(action) + " (Winsock " +
                        std::to_string(WSAGetLastError()) + ")"};
#else
    return AppError{ErrorCode::IoFailure,
                    std::string(action) + ": " + std::strerror(errno)};
#endif
}

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

void closeSocket(Socket socket) noexcept {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool wouldBlock() noexcept {
#ifdef _WIN32
    const auto error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

}  // namespace

class OpenSeeFaceUdpSource::Impl final {
public:
    ~Impl() { stop(); }

    Result<void> start(std::uint16_t requestedPort) {
        if (socket_ != kInvalidSocket) {
            return AppError{ErrorCode::InvalidState,
                            "OpenSeeFace UDP source is already running"};
        }
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return socketError("OpenSeeFace Winsock initialization failed");
        }
        winsockStarted_ = true;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket) {
            stop();
            return socketError("OpenSeeFace UDP socket creation failed");
        }
        int reuse = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse)) < 0) {
            stop();
            return socketError("OpenSeeFace UDP socket option failed");
        }
#ifdef _WIN32
        u_long nonBlocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonBlocking) != 0) {
#else
        const int flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
#endif
            stop();
            return socketError("OpenSeeFace UDP non-blocking setup failed");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(requestedPort);
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) < 0) {
            stop();
            return socketError("OpenSeeFace UDP bind failed");
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int length = sizeof(bound);
#else
        socklen_t length = sizeof(bound);
#endif
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &length) < 0) {
            stop();
            return socketError("OpenSeeFace UDP bound-port query failed");
        }
        boundPort_ = ntohs(bound.sin_port);
        return core::ok();
    }

    Result<std::vector<TrackingResult>> poll(core::TimestampNs projectTime) {
        if (socket_ == kInvalidSocket) {
            return AppError{ErrorCode::InvalidState,
                            "OpenSeeFace UDP source is not running"};
        }
        std::array<std::byte, kMaximumDatagramBytes> datagram{};
        const auto received = recvfrom(
            socket_, reinterpret_cast<char*>(datagram.data()),
            static_cast<int>(datagram.size()), 0, nullptr, nullptr);
        if (received < 0) {
            if (wouldBlock()) return std::vector<TrackingResult>{};
            return socketError("OpenSeeFace UDP receive failed");
        }
        if (received == 0) return std::vector<TrackingResult>{};
        return parseDatagram(
            std::span<const std::byte>{datagram.data(),
                                       static_cast<std::size_t>(received)},
            projectTime);
    }

    void stop() noexcept {
        closeSocket(socket_);
        socket_ = kInvalidSocket;
        boundPort_ = 0;
#ifdef _WIN32
        if (winsockStarted_) {
            WSACleanup();
            winsockStarted_ = false;
        }
#endif
    }

    [[nodiscard]] bool running() const noexcept {
        return socket_ != kInvalidSocket;
    }
    [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }

private:
    Socket socket_{kInvalidSocket};
    std::uint16_t boundPort_{0};
#ifdef _WIN32
    bool winsockStarted_{false};
#endif
};

OpenSeeFaceUdpSource::OpenSeeFaceUdpSource() : impl_(std::make_unique<Impl>()) {}
OpenSeeFaceUdpSource::~OpenSeeFaceUdpSource() = default;

Result<void> OpenSeeFaceUdpSource::start(std::uint16_t port) {
    return impl_->start(port);
}

Result<std::vector<TrackingResult>> OpenSeeFaceUdpSource::poll(
    core::TimestampNs projectTime) {
    return impl_->poll(projectTime);
}

void OpenSeeFaceUdpSource::stop() noexcept { impl_->stop(); }
bool OpenSeeFaceUdpSource::running() const noexcept { return impl_->running(); }
std::uint16_t OpenSeeFaceUdpSource::boundPort() const noexcept {
    return impl_->boundPort();
}

}  // namespace creator::avatar::openseeface
