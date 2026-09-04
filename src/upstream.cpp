#include "upstream.hpp"
#include "socket_utils.hpp"
#include "logger.hpp"
#include <vector>
#include <sstream>
#include <chrono>

namespace proxy {

SOCKET UpstreamHandler::connect_target(
    uint64_t conn_id,
    const Config& cfg,
    const Router& router,
    UpstreamPool& pool,
    const std::string& target_host,
    int target_port,
    std::string& error_msg,
    int64_t& elapsed_ms,
    std::string& route_info_out
) {
    auto start_time = std::chrono::steady_clock::now();

    // 1. Evaluate Smart Routing Rules
    RouteAction action = RouteAction::PROXY;
    std::string rule_match;
    if (cfg.smart_routing_enabled) {
        action = router.match(target_host, target_port, rule_match);
    }

    if (action == RouteAction::REJECT) {
        elapsed_ms = 0;
        route_info_out = "REJECT (" + rule_match + ")";
        error_msg = "Connection blocked by routing rule [" + rule_match + "]";
        return INVALID_SOCKET;
    }

    if (action == RouteAction::DIRECT) {
        route_info_out = "DIRECT (" + rule_match + ")";
        net::ConnectionTracker::update_stage(conn_id, "CONNECTING_DIRECT");
        std::string diag;
        SOCKET s = net::connect_to_host(target_host, target_port, cfg.timeout_seconds, diag, elapsed_ms);
        if (s == INVALID_SOCKET) {
            error_msg = diag.empty() ? ("Direct connection to " + target_host + ":" + std::to_string(target_port) + " failed") : diag;
        }
        return s;
    }

    // 2. Route Action is PROXY -> Select from pool with auto-failover
    auto node = pool.select_node();
    if (!node) {
        if (cfg.kill_switch) {
            elapsed_ms = 0;
            route_info_out = "KILL-SWITCH (BLOCKED)";
            error_msg = "Kill Switch activated: No healthy upstream proxy available";
            return INVALID_SOCKET;
        }
        // Fallback to direct
        route_info_out = "DIRECT (FALLBACK)";
        std::string diag;
        return net::connect_to_host(target_host, target_port, cfg.timeout_seconds, diag, elapsed_ms);
    }

    route_info_out = "PROXY [" + node->name + "] (" + (rule_match.empty() ? "DEFAULT" : rule_match) + ")";
    
    // Attempt 1 with selected node
    SOCKET s = connect_via_node(conn_id, *node, cfg.timeout_seconds, target_host, target_port, error_msg, elapsed_ms);
    if (s != INVALID_SOCKET) {
        pool.mark_node_success(node->name, elapsed_ms);
        return s;
    }

    // Attempt 1 failed -> mark failure and try failover to backup node
    pool.mark_node_failure(node->name);
    auto backup_node = pool.select_node();
    if (backup_node && backup_node->name != node->name) {
        Logger::warn("[FAILOVER] Retrying " + target_host + ":" + std::to_string(target_port) + " via backup node: " + backup_node->name);
        route_info_out = "PROXY [" + backup_node->name + "] (FAILOVER)";
        int64_t retry_ms = 0;
        s = connect_via_node(conn_id, *backup_node, cfg.timeout_seconds, target_host, target_port, error_msg, retry_ms);
        if (s != INVALID_SOCKET) {
            pool.mark_node_success(backup_node->name, retry_ms);
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
            return s;
        }
        pool.mark_node_failure(backup_node->name);
    }

    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
    return INVALID_SOCKET;
}

SOCKET UpstreamHandler::connect_via_node(
    uint64_t conn_id,
    const UpstreamNode& node,
    int timeout_sec,
    const std::string& target_host,
    int target_port,
    std::string& error_msg,
    int64_t& elapsed_ms
) {
    if (node.is_direct) {
        std::string diag;
        return net::connect_to_host(target_host, target_port, timeout_sec, diag, elapsed_ms);
    }

    if (node.type == UpstreamType::SOCKS5) {
        return connect_via_socks5(conn_id, node, timeout_sec, target_host, target_port, error_msg, elapsed_ms);
    } else {
        return connect_via_http(conn_id, node, timeout_sec, target_host, target_port, error_msg, elapsed_ms);
    }
}

SOCKET UpstreamHandler::connect_via_socks5(
    uint64_t conn_id,
    const UpstreamNode& node,
    int timeout_sec,
    const std::string& target_host,
    int target_port,
    std::string& error_msg,
    int64_t& elapsed_ms
) {
    auto start_time = std::chrono::steady_clock::now();

    net::ConnectionTracker::update_stage(conn_id, "CONNECTING_UPSTREAM_SOCKS5");
    std::string diag;
    int64_t tcp_ms = 0;
    SOCKET s = net::connect_to_host(node.host, node.port, timeout_sec, diag, tcp_ms);
    if (s == INVALID_SOCKET) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Could not reach upstream SOCKS5 " + node.host + ":" + std::to_string(node.port) + " (" + diag + ")";
        return INVALID_SOCKET;
    }

    net::ConnectionTracker::update_stage(conn_id, "UPSTREAM_SOCKS5_HANDSHAKE");

    std::vector<char> handshake;
    handshake.push_back(0x05);

    bool has_auth = !node.username.empty();
    if (has_auth) {
        handshake.push_back(0x02);
        handshake.push_back(0x00);
        handshake.push_back(0x02);
    } else {
        handshake.push_back(0x01);
        handshake.push_back(0x00);
    }

    if (net::send_all(s, handshake.data(), static_cast<int>(handshake.size())) != (int)handshake.size()) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Failed to send SOCKS5 handshake to upstream (" + std::to_string(elapsed_ms) + "ms)";
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    char reply[2];
    int64_t hs_ms = 0;
    int recv_len = net::recv_with_timeout(s, reply, 2, timeout_sec, &hs_ms);
    if (recv_len != 2 || reply[0] != 0x05) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = (recv_len == -2) ? ("Upstream SOCKS5 handshake timed out after " + std::to_string(hs_ms) + "ms") : "Invalid SOCKS5 handshake response";
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    unsigned char chosen_method = static_cast<unsigned char>(reply[1]);
    if (chosen_method == 0xFF) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Upstream SOCKS5 rejected authentication methods";
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    if (chosen_method == 0x02) {
        net::ConnectionTracker::update_stage(conn_id, "UPSTREAM_SOCKS5_AUTH");
        std::vector<char> auth_req;
        auth_req.push_back(0x01);
        auth_req.push_back(static_cast<char>(node.username.size()));
        auth_req.insert(auth_req.end(), node.username.begin(), node.username.end());
        auth_req.push_back(static_cast<char>(node.password.size()));
        auth_req.insert(auth_req.end(), node.password.begin(), node.password.end());

        if (net::send_all(s, auth_req.data(), static_cast<int>(auth_req.size())) != (int)auth_req.size()) {
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
            error_msg = "Failed to send credentials to upstream SOCKS5";
            net::close_socket(s);
            return INVALID_SOCKET;
        }

        char auth_reply[2];
        int64_t auth_ms = 0;
        if (net::recv_with_timeout(s, auth_reply, 2, timeout_sec, &auth_ms) != 2 || auth_reply[1] != 0x00) {
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
            error_msg = "Upstream SOCKS5 authentication rejected";
            net::close_socket(s);
            return INVALID_SOCKET;
        }
    }

    net::ConnectionTracker::update_stage(conn_id, "UPSTREAM_SOCKS5_CONNECT_REQ");

    // Anti-DNS Leak: Send domain names directly via ATYP 0x03 to let remote proxy resolve
    std::vector<char> req;
    req.push_back(0x05);
    req.push_back(0x01);
    req.push_back(0x00);

    in_addr addr4;
    if (inet_pton(AF_INET, target_host.c_str(), &addr4) == 1) {
        req.push_back(0x01);
        const char* p = reinterpret_cast<const char*>(&addr4);
        req.insert(req.end(), p, p + 4);
    } else {
        req.push_back(0x03); // Domain name (Remote DNS resolution)
        req.push_back(static_cast<char>(target_host.size()));
        req.insert(req.end(), target_host.begin(), target_host.end());
    }

    uint16_t net_port = htons(static_cast<uint16_t>(target_port));
    const char* port_ptr = reinterpret_cast<const char*>(&net_port);
    req.insert(req.end(), port_ptr, port_ptr + 2);

    if (net::send_all(s, req.data(), static_cast<int>(req.size())) != (int)req.size()) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Failed to send connect request to upstream SOCKS5";
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    char resp_hdr[4];
    int64_t resp_ms = 0;
    if (net::recv_with_timeout(s, resp_hdr, 4, timeout_sec, &resp_ms) != 4) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Upstream SOCKS5 response timed out after " + std::to_string(resp_ms) + "ms";
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    if (resp_hdr[1] != 0x00) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Upstream SOCKS5 error code 0x0" + std::to_string((int)resp_hdr[1]);
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    unsigned char resp_atyp = static_cast<unsigned char>(resp_hdr[3]);
    int addr_len = 0;
    if (resp_atyp == 0x01) addr_len = 4;
    else if (resp_atyp == 0x04) addr_len = 16;
    else if (resp_atyp == 0x03) {
        char domain_len = 0;
        net::recv_with_timeout(s, &domain_len, 1, timeout_sec);
        addr_len = static_cast<unsigned char>(domain_len);
    }

    std::vector<char> remaining(addr_len + 2);
    net::recv_with_timeout(s, remaining.data(), static_cast<int>(remaining.size()), timeout_sec);

    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
    return s;
}

SOCKET UpstreamHandler::connect_via_http(
    uint64_t conn_id,
    const UpstreamNode& node,
    int timeout_sec,
    const std::string& target_host,
    int target_port,
    std::string& error_msg,
    int64_t& elapsed_ms
) {
    auto start_time = std::chrono::steady_clock::now();

    net::ConnectionTracker::update_stage(conn_id, "CONNECTING_UPSTREAM_HTTP");
    std::string diag;
    int64_t tcp_ms = 0;
    SOCKET s = net::connect_to_host(node.host, node.port, timeout_sec, diag, tcp_ms);
    if (s == INVALID_SOCKET) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Could not reach upstream HTTP " + node.host + ":" + std::to_string(node.port) + " (" + diag + ")";
        return INVALID_SOCKET;
    }

    net::ConnectionTracker::update_stage(conn_id, "UPSTREAM_HTTP_CONNECT_REQ");

    std::ostringstream req;
    req << "CONNECT " << target_host << ":" << target_port << " HTTP/1.1\r\n";
    req << "Host: " << target_host << ":" << target_port << "\r\n";
    req << "Proxy-Connection: keep-alive\r\n";
    req << "User-Agent: ProxyClient/1.0\r\n";

    if (!node.username.empty()) {
        std::string creds = node.username + ":" + node.password;
        req << "Proxy-Authorization: Basic " << net::base64_encode(creds) << "\r\n";
    }
    req << "\r\n";

    std::string req_str = req.str();
    if (net::send_all(s, req_str.c_str(), static_cast<int>(req_str.size())) != (int)req_str.size()) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Failed to send HTTP CONNECT request to upstream";
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    std::string resp;
    char ch;
    while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 8192) {
        int n = net::recv_with_timeout(s, &ch, 1, timeout_sec);
        if (n <= 0) {
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
            error_msg = (n == -2) ? ("Upstream HTTP proxy response timed out after " + std::to_string(elapsed_ms) + "ms") : "Connection closed by upstream HTTP proxy";
            net::close_socket(s);
            return INVALID_SOCKET;
        }
        resp.push_back(ch);
    }

    if (resp.find(" 200 ") == std::string::npos && resp.find(" 200\r\n") == std::string::npos) {
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        error_msg = "Upstream HTTP proxy rejected tunnel request: " + resp.substr(0, resp.find("\r\n"));
        net::close_socket(s);
        return INVALID_SOCKET;
    }

    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
    return s;
}

} // namespace proxy
