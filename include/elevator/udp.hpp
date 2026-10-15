#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace elevator {
struct Datagram { std::string body; std::uint16_t port; };
// Sockets bind to loopback on an OS-selected port; no privileged/fixed ports.
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    std::uint16_t port() const;
    void send(std::uint16_t destination, const std::string& body);
    std::optional<Datagram> receive(int timeout_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// A sequential client: retries use the same sequence number. Server caches
// the last reply per source port so a lost ACK cannot duplicate a request.
class RpcClient {
public:
    explicit RpcClient(std::uint16_t server) : server_(server) {}
    std::string call(const std::string& body);
private:
    UdpSocket socket_;
    std::uint16_t server_;
    std::uint64_t sequence_ = 0;
};
}
