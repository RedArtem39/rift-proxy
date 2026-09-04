#include "socket_utils.hpp"
#include "logger.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace net {

std::atomic<uint64_t> Stats::total_bytes_sent{0};
std::atomic<uint64_t> Stats::total_bytes_received{0};
std::atomic<uint64_t> Stats::active_connections{0};
std::atomic<uint64_t> Stats::total_connections{0};

std::mutex ConnectionTracker::s_mutex;
std::atomic<uint64_t> ConnectionTracker::s_next_id{1};
std::map<uint64_t, ConnectionInfo> ConnectionTracker::s_connections;

void Stats::reset() {
    total_bytes_sent = 0;
    total_bytes_received = 0;
    active_connections = 0;
    total_connections = 0;
}

uint64_t ConnectionTracker::register_conn(const std::string& proto, const std::string& client_ip, const std::string& target) {
    uint64_t id = s_next_id++;
    auto now = std::chrono::steady_clock::now();
    ConnectionInfo info;
    info.id = id;
    info.protocol = proto;
    info.client_ip = client_ip;
    info.target = target;
    info.stage = "INIT";
    info.start_time = now;
    info.last_activity = now;

    std::lock_guard<std::mutex> lock(s_mutex);
    s_connections[id] = info;
    return id;
}

void ConnectionTracker::update_target(uint64_t id, const std::string& target) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_connections.find(id);
    if (it != s_connections.end()) {
        it->second.target = target;
        it->second.last_activity = std::chrono::steady_clock::now();
    }
}

void ConnectionTracker::update_stage(uint64_t id, const std::string& stage) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_connections.find(id);
    if (it != s_connections.end()) {
        it->second.stage = stage;
        it->second.last_activity = std::chrono::steady_clock::now();
    }
}

void ConnectionTracker::update_bytes(uint64_t id, uint64_t up, uint64_t down) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_connections.find(id);
    if (it != s_connections.end()) {
        it->second.bytes_up = up;
        it->second.bytes_down = down;
        it->second.last_activity = std::chrono::steady_clock::now();
    }
}

void ConnectionTracker::unregister_conn(uint64_t id) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_connections.erase(id);
}

std::vector<ConnectionInfo> ConnectionTracker::get_active_connections() {
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<ConnectionInfo> list;
    list.reserve(s_connections.size());
    for (const auto& [k, v] : s_connections) {
        list.push_back(v);
    }
    return list;
}

WinsockScope::WinsockScope() {
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res == 0) {
        valid = true;
    } else {
        Logger::error("WSAStartup failed with error code: " + std::to_string(res));
    }
}

WinsockScope::~WinsockScope() {
    if (valid) {
        WSACleanup();
    }
}

bool set_socket_blocking(SOCKET s, bool blocking) {
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool set_socket_timeouts(SOCKET s, int timeout_seconds) {
    DWORD timeout_ms = static_cast<DWORD>(timeout_seconds * 1000);
    int res1 = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    int res2 = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    return (res1 == 0 && res2 == 0);
}

bool set_tcp_nodelay(SOCKET s, bool enabled) {
    int flag = enabled ? 1 : 0;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag)) == 0;
}

void close_socket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

SOCKET create_listen_socket(const std::string& host, int port) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    int status = getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0) {
        Logger::error("getaddrinfo failed for " + host + ":" + port_str + ": " + std::string(gai_strerror(status)));
        return INVALID_SOCKET;
    }

    SOCKET listen_sock = INVALID_SOCKET;
    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        listen_sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (listen_sock == INVALID_SOCKET) {
            continue;
        }

        int reuse = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

        if (bind(listen_sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == SOCKET_ERROR) {
            closesocket(listen_sock);
            listen_sock = INVALID_SOCKET;
            continue;
        }

        if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listen_sock);
            listen_sock = INVALID_SOCKET;
            continue;
        }

        break;
    }

    freeaddrinfo(result);
    return listen_sock;
}

SOCKET connect_to_host(const std::string& host, int port, int timeout_seconds, std::string& diagnostic_out, int64_t& elapsed_ms_out) {
    auto start_tp = std::chrono::steady_clock::now();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);

    auto dns_start = std::chrono::steady_clock::now();
    int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    auto dns_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - dns_start).count();

    if (status != 0) {
        elapsed_ms_out = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();
        diagnostic_out = "DNS resolution failed in " + std::to_string(dns_elapsed) + "ms (" + gai_strerror(status) + ")";
        return INVALID_SOCKET;
    }

    SOCKET out_sock = INVALID_SOCKET;
    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        out_sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (out_sock == INVALID_SOCKET) {
            continue;
        }

        set_tcp_nodelay(out_sock, true);
        set_socket_blocking(out_sock, false);

        auto connect_start = std::chrono::steady_clock::now();
        int res = connect(out_sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
        if (res == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                WSAPOLLFD pfd{};
                pfd.fd = out_sock;
                pfd.events = POLLOUT;
                int poll_res = WSAPoll(&pfd, 1, timeout_seconds * 1000);
                auto conn_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connect_start).count();

                if (poll_res == 0) {
                    diagnostic_out = "TCP connect timed out after " + std::to_string(conn_elapsed) + "ms (target not responding)";
                    closesocket(out_sock);
                    out_sock = INVALID_SOCKET;
                    continue;
                } else if (poll_res < 0) {
                    diagnostic_out = "WSAPoll failed with error " + std::to_string(WSAGetLastError());
                    closesocket(out_sock);
                    out_sock = INVALID_SOCKET;
                    continue;
                }

                if (pfd.revents & POLLOUT) {
                    int opt_err = 0;
                    int opt_len = sizeof(opt_err);
                    getsockopt(out_sock, SOL_SOCKET, SO_ERROR, (char*)&opt_err, &opt_len);
                    if (opt_err == 0) {
                        set_socket_blocking(out_sock, true);
                        set_socket_timeouts(out_sock, timeout_seconds);
                        break;
                    } else {
                        diagnostic_out = "TCP connect refused by remote host (WSA error: " + std::to_string(opt_err) + ") in " + std::to_string(conn_elapsed) + "ms";
                    }
                }
            } else {
                diagnostic_out = "Immediate connect failure: WSA error " + std::to_string(err);
            }
            closesocket(out_sock);
            out_sock = INVALID_SOCKET;
        } else {
            set_socket_blocking(out_sock, true);
            set_socket_timeouts(out_sock, timeout_seconds);
            break;
        }
    }

    freeaddrinfo(result);
    elapsed_ms_out = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();

    if (out_sock == INVALID_SOCKET && diagnostic_out.empty()) {
        diagnostic_out = "Failed to establish TCP connection (all candidate addresses failed)";
    }

    return out_sock;
}

int send_all(SOCKET s, const char* data, int len) {
    int total_sent = 0;
    while (total_sent < len) {
        int sent = send(s, data + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            return -1;
        }
        total_sent += sent;
    }
    return total_sent;
}

int recv_with_timeout(SOCKET s, char* buffer, int max_len, int timeout_seconds, int64_t* elapsed_ms) {
    auto start = std::chrono::steady_clock::now();

    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLIN;

    int poll_res = WSAPoll(&pfd, 1, timeout_seconds * 1000);
    if (elapsed_ms) {
        *elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }

    if (poll_res <= 0) {
        return (poll_res == 0) ? -2 : -1; // -2 = timeout, -1 = error
    }

    if (pfd.revents & POLLIN) {
        return recv(s, buffer, max_len, 0);
    }
    return -1;
}

std::pair<uint64_t, uint64_t> relay_traffic(uint64_t conn_id, SOCKET client_sock, SOCKET target_sock, int buffer_size, int timeout_seconds, std::string& termination_reason) {
    std::vector<char> buf1(buffer_size);
    std::vector<char> buf2(buffer_size);

    set_socket_blocking(client_sock, false);
    set_socket_blocking(target_sock, false);

    uint64_t bytes_up = 0;
    uint64_t bytes_down = 0;

    WSAPOLLFD fds[2];
    fds[0].fd = client_sock;
    fds[0].events = POLLIN;
    fds[1].fd = target_sock;
    fds[1].events = POLLIN;

    auto last_io = std::chrono::steady_clock::now();

    while (true) {
        int ret = WSAPoll(fds, 2, 1000); // 1-second check interval for responsive timeout monitoring
        auto now = std::chrono::steady_clock::now();
        int64_t idle_sec = std::chrono::duration_cast<std::chrono::seconds>(now - last_io).count();

        if (ret == 0) {
            if (idle_sec >= timeout_seconds) {
                termination_reason = "Idle timeout (" + std::to_string(idle_sec) + "s inactivity limit reached)";
                break;
            }
            continue;
        }

        if (ret < 0) {
            termination_reason = "WSAPoll polling error " + std::to_string(WSAGetLastError());
            break;
        }

        bool activity = false;

        // Client -> Target
        if (fds[0].revents & POLLIN) {
            int n = recv(client_sock, buf1.data(), static_cast<int>(buf1.size()), 0);
            if (n <= 0) {
                termination_reason = (n == 0) ? "Client closed connection (FIN)" : "Client connection reset";
                break;
            }

            set_socket_blocking(target_sock, true);
            if (send_all(target_sock, buf1.data(), n) != n) {
                termination_reason = "Failed to write data to target socket";
                break;
            }
            set_socket_blocking(target_sock, false);

            bytes_up += n;
            Stats::total_bytes_sent += n;
            activity = true;
        }

        // Target -> Client
        if (fds[1].revents & POLLIN) {
            int n = recv(target_sock, buf2.data(), static_cast<int>(buf2.size()), 0);
            if (n <= 0) {
                termination_reason = (n == 0) ? "Remote server closed connection (FIN)" : "Remote server connection reset";
                break;
            }

            set_socket_blocking(client_sock, true);
            if (send_all(client_sock, buf2.data(), n) != n) {
                termination_reason = "Failed to write data to client socket";
                break;
            }
            set_socket_blocking(client_sock, false);

            bytes_down += n;
            Stats::total_bytes_received += n;
            activity = true;
        }

        if (activity) {
            last_io = now;
            ConnectionTracker::update_bytes(conn_id, bytes_up, bytes_down);
        }

        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            termination_reason = "Socket hangup / error event detected";
            break;
        }
    }

    return { bytes_up, bytes_down };
}

static const std::string base64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

std::string base64_encode(const std::string& in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64_decode(const std::string& in) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string format_bytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (bytes >= 1024ULL * 1024 * 1024) {
        oss << (double)bytes / (1024 * 1024 * 1024) << " GB";
    } else if (bytes >= 1024ULL * 1024) {
        oss << (double)bytes / (1024 * 1024) << " MB";
    } else if (bytes >= 1024ULL) {
        oss << (double)bytes / 1024 << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

std::string format_duration(int64_t ms) {
    if (ms < 1000) {
        return std::to_string(ms) + "ms";
    }
    double sec = static_cast<double>(ms) / 1000.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << sec << "s";
    return oss.str();
}

} // namespace net
