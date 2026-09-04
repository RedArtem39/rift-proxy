#include "http_proxy.hpp"
#include "socket_utils.hpp"
#include "upstream.hpp"
#include "logger.hpp"
#include <sstream>
#include <algorithm>
#include <chrono>

namespace proxy {

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

bool HttpProxyHandler::check_auth(const std::string& headers, const Config& cfg) {
    if (!cfg.local_auth_enabled) return true;

    std::string expected_cred = cfg.local_username + ":" + cfg.local_password;
    std::string expected_header = "proxy-authorization: basic " + net::base64_encode(expected_cred);

    std::string lower_headers = to_lower(headers);
    return lower_headers.find(to_lower(expected_header)) != std::string::npos;
}

bool HttpProxyHandler::parse_request_line(const std::string& line, std::string& method, std::string& target_host, int& target_port, std::string& path_or_url) {
    std::istringstream iss(line);
    std::string url, version;
    if (!(iss >> method >> url >> version)) {
        return false;
    }

    if (method == "CONNECT") {
        size_t colon = url.find(':');
        if (colon != std::string::npos) {
            target_host = url.substr(0, colon);
            target_port = std::stoi(url.substr(colon + 1));
        } else {
            target_host = url;
            target_port = 443;
        }
        return true;
    }

    if (url.rfind("http://", 0) == 0) {
        std::string without_scheme = url.substr(7);
        size_t slash = without_scheme.find('/');
        std::string host_port = (slash != std::string::npos) ? without_scheme.substr(0, slash) : without_scheme;
        path_or_url = (slash != std::string::npos) ? without_scheme.substr(slash) : "/";

        size_t colon = host_port.find(':');
        if (colon != std::string::npos) {
            target_host = host_port.substr(0, colon);
            target_port = std::stoi(host_port.substr(colon + 1));
        } else {
            target_host = host_port;
            target_port = 80;
        }
        return true;
    }

    return false;
}

bool HttpProxyHandler::handle_client(
    uint64_t conn_id,
    SOCKET client_sock,
    const std::string& client_ip,
    const Config& cfg,
    const Router& router,
    UpstreamPool& pool,
    const std::string& initial_data
) {
    auto session_start = std::chrono::steady_clock::now();
    net::ConnectionTracker::update_stage(conn_id, "READING_HTTP_HEADERS");

    std::string header_data = initial_data;

    while (header_data.find("\r\n\r\n") == std::string::npos && header_data.size() < 16384) {
        char buf[512];
        int n = net::recv_with_timeout(client_sock, buf, sizeof(buf), cfg.timeout_seconds);
        if (n <= 0) return false;
        header_data.append(buf, n);
    }

    size_t header_end = header_data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    size_t first_line_end = header_data.find("\r\n");
    std::string request_line = header_data.substr(0, first_line_end);
    std::string method, target_host, path_or_url;
    int target_port = 80;

    if (!parse_request_line(request_line, method, target_host, target_port, path_or_url)) {
        std::string lower = to_lower(header_data);
        size_t host_pos = lower.find("\r\nhost: ");
        if (host_pos != std::string::npos) {
            size_t host_end = header_data.find("\r\n", host_pos + 8);
            std::string host_hdr = header_data.substr(host_pos + 8, host_end - (host_pos + 8));
            size_t colon = host_hdr.find(':');
            if (colon != std::string::npos) {
                target_host = host_hdr.substr(0, colon);
                target_port = std::stoi(host_hdr.substr(colon + 1));
            } else {
                target_host = host_hdr;
                target_port = 80;
            }
        }
    }

    if (target_host.empty()) {
        Logger::warn("[#" + std::to_string(conn_id) + "] HTTP invalid request line: " + request_line);
        return false;
    }

    std::string target_str = target_host + ":" + std::to_string(target_port);
    net::ConnectionTracker::update_target(conn_id, target_str);

    if (!check_auth(header_data, cfg)) {
        Logger::warn("[#" + std::to_string(conn_id) + "] HTTP auth required for " + client_ip);
        std::string auth_resp = 
            "HTTP/1.1 407 Proxy Authentication Required\r\n"
            "Proxy-Authenticate: Basic realm=\"ProxyClient\"\r\n"
            "Content-Length: 0\r\n\r\n";
        net::send_all(client_sock, auth_resp.c_str(), static_cast<int>(auth_resp.size()));
        return false;
    }

    std::string err_msg;
    int64_t connect_ms = 0;
    std::string route_info;
    SOCKET target_sock = UpstreamHandler::connect_target(conn_id, cfg, router, pool, target_host, target_port, err_msg, connect_ms, route_info);

    if (target_sock == INVALID_SOCKET) {
        Logger::timeout("[#" + std::to_string(conn_id) + "] HTTP connect to " + target_str + " [" + route_info + "] failed after " + std::to_string(connect_ms) + "ms: " + err_msg);
        std::string err_resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
        net::send_all(client_sock, err_resp.c_str(), static_cast<int>(err_resp.size()));
        return false;
    }

    if (method == "CONNECT") {
        std::string ok_resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (net::send_all(client_sock, ok_resp.c_str(), static_cast<int>(ok_resp.size())) <= 0) {
            net::close_socket(target_sock);
            return false;
        }

        std::string remaining_payload = header_data.substr(header_end + 4);
        if (!remaining_payload.empty()) {
            net::send_all(target_sock, remaining_payload.c_str(), static_cast<int>(remaining_payload.size()));
        }
    } else {
        net::send_all(target_sock, header_data.c_str(), static_cast<int>(header_data.size()));
    }

    net::Stats::active_connections++;
    net::Stats::total_connections++;
    net::ConnectionTracker::update_stage(conn_id, "ACTIVE_TUNNEL");

    Logger::conn("[#" + std::to_string(conn_id) + "] HTTP " + method + " " + client_ip + " -> " + target_str + " [" + route_info + "] established in " + std::to_string(connect_ms) + "ms");

    std::string reason;
    auto [up, down] = net::relay_traffic(conn_id, client_sock, target_sock, cfg.buffer_size, cfg.timeout_seconds, reason);

    net::Stats::active_connections--;
    net::close_socket(target_sock);

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - session_start).count();
    Logger::conn("[#" + std::to_string(conn_id) + "] HTTP session closed (" + net::format_duration(total_ms) + "): " + target_str +
                 " [Tx: " + net::format_bytes(up) + ", Rx: " + net::format_bytes(down) + "] Reason: " + reason);

    return true;
}

} // namespace proxy
