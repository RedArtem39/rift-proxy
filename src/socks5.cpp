#include "socks5.hpp"
#include "socks5_udp.hpp"
#include "socket_utils.hpp"
#include "upstream.hpp"
#include "logger.hpp"
#include <vector>
#include <ws2tcpip.h>
#include <chrono>

namespace proxy {

bool Socks5Handler::authenticate(uint64_t conn_id, SOCKET client_sock, const Config& cfg) {
    net::ConnectionTracker::update_stage(conn_id, "AUTH_HANDSHAKE");

    char ver = 0;
    if (net::recv_with_timeout(client_sock, &ver, 1, cfg.timeout_seconds) != 1 || ver != 0x01) {
        return false;
    }

    char ulen = 0;
    if (net::recv_with_timeout(client_sock, &ulen, 1, cfg.timeout_seconds) != 1) {
        return false;
    }
    int ulen_val = static_cast<unsigned char>(ulen);
    std::string uname(ulen_val, '\0');
    if (net::recv_with_timeout(client_sock, &uname[0], ulen_val, cfg.timeout_seconds) != ulen_val) {
        return false;
    }

    char plen = 0;
    if (net::recv_with_timeout(client_sock, &plen, 1, cfg.timeout_seconds) != 1) {
        return false;
    }
    int plen_val = static_cast<unsigned char>(plen);
    std::string passwd(plen_val, '\0');
    if (net::recv_with_timeout(client_sock, &passwd[0], plen_val, cfg.timeout_seconds) != plen_val) {
        return false;
    }

    bool ok = (uname == cfg.local_username && passwd == cfg.local_password);
    char resp[2] = { 0x01, static_cast<char>(ok ? 0x00 : 0x01) };
    net::send_all(client_sock, resp, 2);

    return ok;
}

bool Socks5Handler::handle_client(
    uint64_t conn_id,
    SOCKET client_sock,
    const std::string& client_ip,
    const Config& cfg,
    const Router& router,
    UpstreamPool& pool,
    const std::string& initial_data
) {
    auto session_start = std::chrono::steady_clock::now();
    net::ConnectionTracker::update_stage(conn_id, "SOCKS5_GREETING");

    std::vector<char> buffer;
    buffer.insert(buffer.end(), initial_data.begin(), initial_data.end());

    while (buffer.size() < 2) {
        char tmp[128];
        int n = net::recv_with_timeout(client_sock, tmp, sizeof(tmp), cfg.timeout_seconds);
        if (n <= 0) return false;
        buffer.insert(buffer.end(), tmp, tmp + n);
    }

    if (buffer[0] != 0x05) {
        Logger::debug("[#" + std::to_string(conn_id) + "] SOCKS5 invalid version: " + std::to_string((int)buffer[0]));
        return false;
    }

    int nmethods = static_cast<unsigned char>(buffer[1]);
    while (buffer.size() < static_cast<size_t>(2 + nmethods)) {
        char tmp[128];
        int n = net::recv_with_timeout(client_sock, tmp, sizeof(tmp), cfg.timeout_seconds);
        if (n <= 0) return false;
        buffer.insert(buffer.end(), tmp, tmp + n);
    }

    bool client_supports_no_auth = false;
    bool client_supports_user_pass = false;
    for (int i = 0; i < nmethods; ++i) {
        unsigned char m = static_cast<unsigned char>(buffer[2 + i]);
        if (m == 0x00) client_supports_no_auth = true;
        if (m == 0x02) client_supports_user_pass = true;
    }

    char chosen_method = static_cast<char>(0xFF);
    if (cfg.local_auth_enabled) {
        if (client_supports_user_pass) {
            chosen_method = 0x02;
        }
    } else {
        if (client_supports_no_auth) {
            chosen_method = 0x00;
        }
    }

    char handshake_resp[2] = { 0x05, chosen_method };
    net::send_all(client_sock, handshake_resp, 2);

    if (static_cast<unsigned char>(chosen_method) == 0xFF) {
        Logger::warn("[#" + std::to_string(conn_id) + "] SOCKS5: No acceptable auth methods from client " + client_ip);
        return false;
    }

    if (chosen_method == 0x02) {
        if (!authenticate(conn_id, client_sock, cfg)) {
            Logger::warn("[#" + std::to_string(conn_id) + "] SOCKS5: Authentication failed for client " + client_ip);
            return false;
        }
    }

    net::ConnectionTracker::update_stage(conn_id, "READING_TARGET_ADDR");

    char req_hdr[4];
    if (net::recv_with_timeout(client_sock, req_hdr, 4, cfg.timeout_seconds) != 4) {
        return false;
    }

    if (req_hdr[0] != 0x05) return false;
    if (req_hdr[1] == 0x03) { // UDP ASSOCIATE
        Socks5UdpRelay udp_relay;
        int alloc_port = 0;
        if (!udp_relay.init(cfg.local_host, alloc_port)) {
            char rep[10] = { 0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
            net::send_all(client_sock, rep, 10);
            return false;
        }

        in_addr bind_addr{};
        inet_pton(AF_INET, cfg.local_host.c_str(), &bind_addr);
        uint16_t net_port = htons(static_cast<uint16_t>(alloc_port));

        std::vector<char> success_rep = { 0x05, 0x00, 0x00, 0x01 };
        const char* p_ip = reinterpret_cast<const char*>(&bind_addr);
        success_rep.insert(success_rep.end(), p_ip, p_ip + 4);
        const char* p_pt = reinterpret_cast<const char*>(&net_port);
        success_rep.insert(success_rep.end(), p_pt, p_pt + 2);

        net::send_all(client_sock, success_rep.data(), (int)success_rep.size());
        Logger::conn("[#" + std::to_string(conn_id) + "] SOCKS5 UDP ASSOCIATE bound on " + cfg.local_host + ":" + std::to_string(alloc_port));

        udp_relay.start(client_sock);

        // Keep TCP control socket open until client disconnects
        char ch;
        while (recv(client_sock, &ch, 1, 0) > 0) {}

        udp_relay.stop();
        Logger::conn("[#" + std::to_string(conn_id) + "] SOCKS5 UDP ASSOCIATE closed");
        return true;
    }

    if (req_hdr[1] != 0x01) { // CONNECT
        Logger::warn("[#" + std::to_string(conn_id) + "] SOCKS5: Unsupported command 0x0" + std::to_string((int)req_hdr[1]));
        char rep[10] = { 0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
        net::send_all(client_sock, rep, 10);
        return false;
    }

    std::string target_host;
    unsigned char atyp = static_cast<unsigned char>(req_hdr[3]);

    if (atyp == 0x01) { // IPv4
        char ip4[4];
        if (net::recv_with_timeout(client_sock, ip4, 4, cfg.timeout_seconds) != 4) return false;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, ip4, ip_str, sizeof(ip_str));
        target_host = ip_str;
    } else if (atyp == 0x03) { // Domain name (Anti-DNS leak)
        char dlen = 0;
        if (net::recv_with_timeout(client_sock, &dlen, 1, cfg.timeout_seconds) != 1) return false;
        int domain_len = static_cast<unsigned char>(dlen);
        std::string domain(domain_len, '\0');
        if (net::recv_with_timeout(client_sock, &domain[0], domain_len, cfg.timeout_seconds) != domain_len) return false;
        target_host = domain;
    } else if (atyp == 0x04) { // IPv6
        char ip6[16];
        if (net::recv_with_timeout(client_sock, ip6, 16, cfg.timeout_seconds) != 16) return false;
        char ip_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, ip6, ip_str, sizeof(ip_str));
        target_host = ip_str;
    } else {
        Logger::warn("[#" + std::to_string(conn_id) + "] SOCKS5: Unsupported ATYP 0x0" + std::to_string((int)atyp));
        return false;
    }

    uint16_t net_port = 0;
    if (net::recv_with_timeout(client_sock, reinterpret_cast<char*>(&net_port), 2, cfg.timeout_seconds) != 2) {
        return false;
    }
    int target_port = ntohs(net_port);
    std::string target_str = target_host + ":" + std::to_string(target_port);

    net::ConnectionTracker::update_target(conn_id, target_str);

    std::string err_msg;
    int64_t connect_ms = 0;
    std::string route_info;
    SOCKET target_sock = UpstreamHandler::connect_target(conn_id, cfg, router, pool, target_host, target_port, err_msg, connect_ms, route_info);
    
    if (target_sock == INVALID_SOCKET) {
        Logger::timeout("[#" + std::to_string(conn_id) + "] SOCKS5 " + target_str + " [" + route_info + "] failed after " + std::to_string(connect_ms) + "ms: " + err_msg);
        char rep[10] = { 0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
        net::send_all(client_sock, rep, 10);
        return false;
    }

    char success_rep[10] = { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    if (net::send_all(client_sock, success_rep, 10) != 10) {
        net::close_socket(target_sock);
        return false;
    }

    net::Stats::active_connections++;
    net::Stats::total_connections++;
    net::ConnectionTracker::update_stage(conn_id, "ACTIVE_TUNNEL");

    Logger::conn("[#" + std::to_string(conn_id) + "] SOCKS5 " + client_ip + " -> " + target_str + " [" + route_info + "] established in " + std::to_string(connect_ms) + "ms");

    std::string reason;
    auto [up, down] = net::relay_traffic(conn_id, client_sock, target_sock, cfg.buffer_size, cfg.timeout_seconds, reason);

    net::Stats::active_connections--;
    net::close_socket(target_sock);

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - session_start).count();
    Logger::conn("[#" + std::to_string(conn_id) + "] SOCKS5 session closed (" + net::format_duration(total_ms) + "): " + target_str +
                 " [Tx: " + net::format_bytes(up) + ", Rx: " + net::format_bytes(down) + "] Reason: " + reason);

    return true;
}

} // namespace proxy
