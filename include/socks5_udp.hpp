#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <atomic>
#include <thread>
#include <cstdint>

namespace proxy {

class Socks5UdpRelay {
public:
    Socks5UdpRelay();
    ~Socks5UdpRelay();

    bool init(const std::string& bind_ip, int& allocated_port);
    void start(SOCKET client_tcp_control_sock);
    void stop();

private:
    SOCKET udp_sock = INVALID_SOCKET;
    int local_port = 0;
    std::atomic<bool> running{false};
    std::thread worker_thread;
    SOCKET control_sock = INVALID_SOCKET;

    void relay_loop();
};

} // namespace proxy
