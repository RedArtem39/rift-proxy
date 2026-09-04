#include "socks5_udp.hpp"
#include "socket_utils.hpp"
#include "logger.hpp"
#include <vector>

namespace proxy {

Socks5UdpRelay::Socks5UdpRelay() {}

Socks5UdpRelay::~Socks5UdpRelay() {
    stop();
}

bool Socks5UdpRelay::init(const std::string& bind_ip, int& allocated_port) {
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // OS picks free port
    inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);

    if (bind(udp_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(udp_sock);
        udp_sock = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound_addr{};
    int len = sizeof(bound_addr);
    if (getsockname(udp_sock, (sockaddr*)&bound_addr, &len) == 0) {
        allocated_port = ntohs(bound_addr.sin_port);
        local_port = allocated_port;
        return true;
    }

    closesocket(udp_sock);
    udp_sock = INVALID_SOCKET;
    return false;
}

void Socks5UdpRelay::start(SOCKET client_tcp_control_sock) {
    control_sock = client_tcp_control_sock;
    running.store(true);
    worker_thread = std::thread(&Socks5UdpRelay::relay_loop, this);
}

void Socks5UdpRelay::stop() {
    if (!running.load()) return;
    running.store(false);

    if (udp_sock != INVALID_SOCKET) {
        closesocket(udp_sock);
        udp_sock = INVALID_SOCKET;
    }

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void Socks5UdpRelay::relay_loop() {
    std::vector<char> buf(65536);
    sockaddr_in client_udp_addr{};
    bool client_udp_known = false;

    net::set_socket_blocking(udp_sock, false);

    WSAPOLLFD fds[2];
    fds[0].fd = control_sock;
    fds[0].events = POLLIN | POLLHUP;
    fds[1].fd = udp_sock;
    fds[1].events = POLLIN;

    while (running.load()) {
        int ret = WSAPoll(fds, 2, 500);
        if (ret <= 0) continue;

        // Check if TCP control socket disconnected -> terminate UDP relay
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char ch;
            int n = recv(control_sock, &ch, 1, MSG_PEEK);
            if (n <= 0) {
                // Client closed TCP connection
                break;
            }
        }

        // UDP packet arrived
        if (fds[1].revents & POLLIN) {
            sockaddr_in from_addr{};
            int from_len = sizeof(from_addr);
            int n = recvfrom(udp_sock, buf.data(), (int)buf.size(), 0, (sockaddr*)&from_addr, &from_len);
            if (n >= 10) {
                // Parse RFC 1928 SOCKS5 UDP header:
                // +----+------+------+----------+----------+----------+
                // |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
                // +----+------+------+----------+----------+----------+
                // | 2  |  1   |  1   | Variable |    2     | Variable |
                // +----+------+------+----------+----------+----------+
                if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00) {
                    client_udp_addr = from_addr;
                    client_udp_known = true;

                    unsigned char atyp = (unsigned char)buf[3];
                    int header_len = 0;
                    sockaddr_in target_addr{};
                    target_addr.sin_family = AF_INET;

                    if (atyp == 0x01 && n >= 10) { // IPv4
                        memcpy(&target_addr.sin_addr, &buf[4], 4);
                        memcpy(&target_addr.sin_port, &buf[8], 2);
                        header_len = 10;
                    }

                    if (header_len > 0 && n > header_len) {
                        // Forward payload directly to target UDP
                        sendto(udp_sock, buf.data() + header_len, n - header_len, 0, (sockaddr*)&target_addr, sizeof(target_addr));
                    }
                } else if (client_udp_known) {
                    // Response from remote target -> encapsulate back into SOCKS5 UDP packet
                    std::vector<char> resp(10 + n);
                    resp[0] = 0x00; resp[1] = 0x00; resp[2] = 0x00;
                    resp[3] = 0x01; // IPv4
                    memcpy(&resp[4], &from_addr.sin_addr, 4);
                    memcpy(&resp[8], &from_addr.sin_port, 2);
                    memcpy(&resp[10], buf.data(), n);

                    sendto(udp_sock, resp.data(), (int)resp.size(), 0, (sockaddr*)&client_udp_addr, sizeof(client_udp_addr));
                }
            }
        }
    }
}

} // namespace proxy
