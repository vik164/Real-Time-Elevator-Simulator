#include "elevator/udp.hpp"
#include <chrono>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace elevator {
namespace {
#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle invalid_socket = INVALID_SOCKET;
using AddressLength = int;
void close_socket(Handle s) { closesocket(s); }
struct Network {
    Network() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
    ~Network() { WSACleanup(); }
};
#else
using Handle = int;
constexpr Handle invalid_socket = -1;
using AddressLength = socklen_t;
void close_socket(Handle s) { ::close(s); }
struct Network {};
#endif
sockaddr_in address(std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}
}
struct UdpSocket::Impl {
    Handle socket = invalid_socket;
    std::uint16_t port = 0;
    ~Impl() { if (socket != invalid_socket) close_socket(socket); }
};
UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {
    static Network network;
    (void)network;
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == invalid_socket) throw std::runtime_error("Cannot create UDP socket");
    auto addr = address(0);
    if (::bind(impl_->socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("Cannot bind UDP socket");
    AddressLength length = sizeof(addr);
    if (getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&addr), &length) != 0)
        throw std::runtime_error("Cannot read UDP port");
    impl_->port = ntohs(addr.sin_port);
}
UdpSocket::~UdpSocket() = default;
std::uint16_t UdpSocket::port() const { return impl_->port; }
void UdpSocket::send(std::uint16_t destination, const std::string& body) {
    if (body.size() > 4096) throw std::invalid_argument("UDP packet too large");
    const auto addr = address(destination);
    const auto sent = sendto(impl_->socket, body.data(), static_cast<int>(body.size()), 0,
        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (sent != static_cast<int>(body.size())) throw std::runtime_error("UDP send failed");
}
std::optional<Datagram> UdpSocket::receive(int timeout_ms) {
    if (timeout_ms < 0) throw std::invalid_argument("Negative socket timeout");
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(impl_->socket, &ready);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int selected = select(static_cast<int>(impl_->socket + 1), &ready, nullptr, nullptr, &timeout);
    if (selected == 0) return std::nullopt;
    if (selected < 0) {
#ifndef _WIN32
        if (errno == EINTR) return std::nullopt;
#endif
        throw std::runtime_error("UDP select failed");
    }
    char buffer[4097];
    sockaddr_in sender{};
    AddressLength length = sizeof(sender);
    const auto count = recvfrom(impl_->socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&sender), &length);
    if (count < 0) throw std::runtime_error("UDP receive failed");
    if (count > 4096) return std::nullopt;
    return Datagram{std::string(buffer, static_cast<std::size_t>(count)), ntohs(sender.sin_port)};
}
std::string RpcClient::call(const std::string& body) {
    const auto prefix = std::to_string(++sequence_) + " ";
    for (int attempt = 0; attempt < 5; ++attempt) {
        socket_.send(server_, prefix + body);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            auto reply = socket_.receive(static_cast<int>(remaining > 0 ? remaining : 0));
            if (reply && reply->port == server_ && reply->body.compare(0, prefix.size(), prefix) == 0) {
                auto result = reply->body.substr(prefix.size());
                if (result.compare(0, 5, "ERROR") == 0) throw std::runtime_error(result);
                return result;
            }
        }
    }
    throw std::runtime_error("UDP RPC timed out after five attempts");
}
}
