#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <mutex>
#include <map>

namespace net {

class WinsockScope {
public:
    WinsockScope();
    ~WinsockScope();
    bool is_valid() const { return valid; }
private:
    bool valid = false;
};

struct ConnectionInfo {
    uint64_t id = 0;
    std::string protocol;
    std::string client_ip;
    std::string target;
    std::string stage;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_activity;
    uint64_t bytes_up = 0;
    uint64_t bytes_down = 0;

    int64_t elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
    }

    int64_t idle_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_activity).count();
    }
};

class ConnectionTracker {
public:
    static uint64_t register_conn(const std::string& proto, const std::string& client_ip, const std::string& target);
    static void update_target(uint64_t id, const std::string& target);
    static void update_stage(uint64_t id, const std::string& stage);
    static void update_bytes(uint64_t id, uint64_t up, uint64_t down);
    static void unregister_conn(uint64_t id);
    static std::vector<ConnectionInfo> get_active_connections();

private:
    static std::mutex s_mutex;
    static std::atomic<uint64_t> s_next_id;
    static std::map<uint64_t, ConnectionInfo> s_connections;
};

struct Stats {
    static std::atomic<uint64_t> total_bytes_sent;
    static std::atomic<uint64_t> total_bytes_received;
    static std::atomic<uint64_t> active_connections;
    static std::atomic<uint64_t> total_connections;

    static void reset();
};

bool set_socket_blocking(SOCKET s, bool blocking);
bool set_socket_timeouts(SOCKET s, int timeout_seconds);
bool set_tcp_nodelay(SOCKET s, bool enabled);
void close_socket(SOCKET& s);

SOCKET create_listen_socket(const std::string& host, int port);
SOCKET connect_to_host(const std::string& host, int port, int timeout_seconds, std::string& diagnostic_out, int64_t& elapsed_ms_out);

int send_all(SOCKET s, const char* data, int len);
int recv_with_timeout(SOCKET s, char* buffer, int max_len, int timeout_seconds, int64_t* elapsed_ms = nullptr);

// Bi-directional stream forwarding between client_sock and target_sock
std::pair<uint64_t, uint64_t> relay_traffic(uint64_t conn_id, SOCKET client_sock, SOCKET target_sock, int buffer_size, int timeout_seconds, std::string& termination_reason);

std::string base64_encode(const std::string& in);
std::string base64_decode(const std::string& in);

std::string format_bytes(uint64_t bytes);
std::string format_duration(int64_t ms);

} // namespace net
