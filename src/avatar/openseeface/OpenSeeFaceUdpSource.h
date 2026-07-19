#pragma once

#include "avatar/ITrackingSource.h"

#include <cstdint>
#include <memory>

namespace creator::avatar::openseeface {

/// Receives OpenSeeFace UDP datagrams without owning a thread or clock.
///
/// The socket is non-blocking and each poll consumes at most one bounded UDP
/// datagram. Malformed packets are returned as InvalidArgument; an empty poll
/// is a successful empty batch. The source binds all interfaces so the
/// OpenSeeFace process may use its documented loopback/default configuration.
class OpenSeeFaceUdpSource final : public ITrackingSource {
public:
    OpenSeeFaceUdpSource();
    ~OpenSeeFaceUdpSource() override;

    OpenSeeFaceUdpSource(const OpenSeeFaceUdpSource&) = delete;
    OpenSeeFaceUdpSource& operator=(const OpenSeeFaceUdpSource&) = delete;
    OpenSeeFaceUdpSource(OpenSeeFaceUdpSource&&) = delete;
    OpenSeeFaceUdpSource& operator=(OpenSeeFaceUdpSource&&) = delete;

    [[nodiscard]] core::Result<void> start(std::uint16_t port) override;
    [[nodiscard]] core::Result<std::vector<TrackingResult>> poll(
        core::TimestampNs projectTime) override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] std::uint16_t boundPort() const noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::avatar::openseeface
