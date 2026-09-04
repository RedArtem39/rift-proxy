#include "oneshot_cli.hpp"
#include "socket_utils.hpp"
#include "system_proxy.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "upstream.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace cli {

static std::string extract_http_body(const std::string& resp) {
    size_t pos = resp.find("\r\n\r\n");
    if (pos != std::string::npos) {
        return resp.substr(pos + 4);
    }
    return resp;
}

std::string OneShotClient::http_get(int port, const std::string& path) {
    std::string diag;
    int64_t elapsed = 0;
    SOCKET s = net::connect_to_host("127.0.0.1", port, 2, diag, elapsed);
    if (s == INVALID_SOCKET) return "";

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    net::send_all(s, req.c_str(), (int)req.size());

    std::string resp;
    char buf[1024];
    while (true) {
        int n = net::recv_with_timeout(s, buf, sizeof(buf), 2);
        if (n <= 0) break;
        resp.append(buf, n);
    }
    net::close_socket(s);
    return extract_http_body(resp);
}

std::string OneShotClient::http_post(int port, const std::string& path, const std::string& json_body) {
    std::string diag;
    int64_t elapsed = 0;
    SOCKET s = net::connect_to_host("127.0.0.1", port, 2, diag, elapsed);
    if (s == INVALID_SOCKET) return "";

    std::ostringstream ss;
    ss << "POST " << path << " HTTP/1.1\r\n"
       << "Host: 127.0.0.1:" << port << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << json_body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << json_body;

    std::string req = ss.str();
    net::send_all(s, req.c_str(), (int)req.size());

    std::string resp;
    char buf[1024];
    while (true) {
        int n = net::recv_with_timeout(s, buf, sizeof(buf), 2);
        if (n <= 0) break;
        resp.append(buf, n);
    }
    net::close_socket(s);
    return extract_http_body(resp);
}

bool OneShotClient::is_daemon_running(int api_port) {
    std::string res = http_get(api_port, "/api/status");
    return !res.empty() && res.find("\"running\":true") != std::string::npos;
}

int OneShotClient::execute_subcommand(const std::string& cmd, const std::vector<std::string>& args, int api_port) {
    std::string lower_cmd = cmd;
    std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), [](unsigned char c){ return (char)std::tolower(c); });

    if (lower_cmd == "sysproxy") {
        if (args.empty()) {
            std::cout << "Usage: rft sysproxy <on|off>\n";
            return 1;
        }
        std::string mode = args[0];
        if (mode == "on") {
            // Check if daemon running to get port
            int local_port = 1080;
            std::string status_raw = http_get(api_port, "/api/status");
            if (!status_raw.empty()) {
                try {
                    auto parsed = json::Parser::parse(status_raw);
                    local_port = parsed["local_port"].as_int(1080);
                } catch (...) {}
            }
            sys::SystemProxyManager::enable("127.0.0.1", local_port);
            std::cout << "[SUCCESS] Windows System Proxy enabled -> 127.0.0.1:" << local_port << "\n";
            return 0;
        } else if (mode == "off") {
            sys::SystemProxyManager::disable();
            std::cout << "[SUCCESS] Windows System Proxy disabled.\n";
            return 0;
        } else {
            std::cout << "Usage: rft sysproxy <on|off>\n";
            return 1;
        }
    }

    if (lower_cmd == "test") {
        std::string host = args.empty() ? "1.1.1.1" : args[0];
        int port = (args.size() > 1) ? std::stoi(args[1]) : 443;
        std::cout << "Testing connection to " << host << ":" << port << "...\n";
        auto start_tp = std::chrono::steady_clock::now();
        std::string diag;
        int64_t elapsed = 0;
        SOCKET s = net::connect_to_host(host, port, 5, diag, elapsed);
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();
        if (s != INVALID_SOCKET) {
            std::cout << "[SUCCESS] Connected to " << host << ":" << port << " in " << total_ms << " ms\n";
            net::close_socket(s);
            return 0;
        } else {
            std::cout << "[FAILED] Target " << host << ":" << port << " unreachable: " << diag << " (" << total_ms << "ms)\n";
            return 1;
        }
    }

    // For daemon-dependent commands: status, nodes, rules, switch, strategy, check
    if (!is_daemon_running(api_port)) {
        std::cout << "[ERROR] Rift daemon is not running on port " << api_port << ".\n"
                  << "Start daemon using: rft run (or rft --sysproxy)\n";
        return 1;
    }

    if (lower_cmd == "status") {
        std::string raw = http_get(api_port, "/api/status");
        try {
            auto v = json::Parser::parse(raw);
            std::cout << "\n--- Proxy Service Status ---\n"
                      << "  State:           " << (v["running"].as_bool() ? "RUNNING" : "STOPPED") << "\n"
                      << "  Listen Address:  " << v["local_host"].as_string() << ":" << v["local_port"].as_int() << "\n"
                      << "  Mode:            " << v["mode"].as_string() << "\n"
                      << "  Smart Routing:   " << (v["smart_routing"].as_bool() ? ("ENABLED (Default: " + v["default_route"].as_string() + ")") : "DISABLED") << "\n"
                      << "  Active Strategy: " << v["active_strategy"].as_string() << "\n"
                      << "  Kill Switch:     " << (v["kill_switch"].as_bool() ? "ENABLED" : "DISABLED") << "\n"
                      << "  System Proxy:    " << (v["system_proxy_enabled"].as_bool() ? "ENABLED (Routing active)" : "DISABLED") << "\n";
            if (v.has_field("metrics")) {
                auto& m = v["metrics"];
                std::cout << "\n--- Traffic Metrics ---\n"
                          << "  Active Connections: " << m["active_connections"].as_int() << "\n"
                          << "  Total Processed:    " << m["total_connections"].as_int() << "\n"
                          << "  Transmitted (Tx):   " << net::format_bytes(m["bytes_sent"].as_int()) << "\n"
                          << "  Received (Rx):      " << net::format_bytes(m["bytes_received"].as_int()) << "\n\n";
            }
            return 0;
        } catch (...) {
            std::cout << raw << "\n";
            return 0;
        }
    } else if (lower_cmd == "nodes" || lower_cmd == "pool") {
        std::string raw = http_get(api_port, "/api/nodes");
        try {
            auto v = json::Parser::parse(raw);
            const auto& nodes = v["nodes"].as_array();
            std::cout << "\n--- Upstream Proxy Pool (" << nodes.size() << " nodes) [Strategy: " << v["strategy"].as_string() << "] ---\n";
            std::cout << std::left
                      << std::setw(4)  << "#"
                      << std::setw(20) << "NAME"
                      << std::setw(10) << "TYPE"
                      << std::setw(26) << "ENDPOINT"
                      << std::setw(12) << "STATUS"
                      << std::setw(14) << "LATENCY"
                      << std::setw(10) << "FAILS"
                      << "\n";
            std::cout << std::string(96, '-') << "\n";
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto& n = nodes[i];
                std::string status = n["alive"].as_bool() ? "ONLINE" : "OFFLINE";
                int64_t lat = n["latency_ms"].as_int();
                std::string lat_str = (lat >= 0) ? (std::to_string(lat) + " ms") : "TIMEOUT";
                std::cout << std::left
                          << std::setw(4)  << (i + 1)
                          << std::setw(20) << n["name"].as_string()
                          << std::setw(10) << n["type"].as_string()
                          << std::setw(26) << n["endpoint"].as_string()
                          << std::setw(12) << status
                          << std::setw(14) << lat_str
                          << std::setw(10) << n["failure_count"].as_int()
                          << "\n";
            }
            std::cout << "\n";
            return 0;
        } catch (...) {
            std::cout << raw << "\n";
            return 0;
        }
    } else if (lower_cmd == "rules") {
        std::string raw = http_get(api_port, "/api/rules");
        try {
            auto v = json::Parser::parse(raw);
            const auto& rules = v["rules"].as_array();
            std::cout << "\n--- Smart Routing Rules (" << rules.size() << " rules) ---\n";
            std::cout << std::left
                      << std::setw(4)  << "#"
                      << std::setw(18) << "TYPE"
                      << std::setw(32) << "PATTERN"
                      << std::setw(12) << "ACTION"
                      << "\n";
            std::cout << std::string(66, '-') << "\n";
            for (size_t i = 0; i < rules.size(); ++i) {
                const auto& r = rules[i];
                std::cout << std::left
                          << std::setw(4)  << (i + 1)
                          << std::setw(18) << r["type"].as_string()
                          << std::setw(32) << r["pattern"].as_string()
                          << std::setw(12) << r["action"].as_string()
                          << "\n";
            }
            std::cout << "Default Action: " << v["default_action"].as_string() << "\n\n";
            return 0;
        } catch (...) {
            std::cout << raw << "\n";
            return 0;
        }
    } else if (lower_cmd == "switch") {
        if (args.empty()) {
            std::cout << "Usage: rft switch <node_name | node_index | auto>\n";
            return 1;
        }
        std::string body = "{\"node\":\"" + args[0] + "\"}";
        std::string raw = http_post(api_port, "/api/switch", body);
        try {
            auto v = json::Parser::parse(raw);
            std::cout << (v["success"].as_bool() ? "[OK] " : "[ERROR] ") << v["message"].as_string() << "\n";
            return v["success"].as_bool() ? 0 : 1;
        } catch (...) {
            std::cout << raw << "\n";
            return 0;
        }
    } else if (lower_cmd == "strategy") {
        if (args.empty()) {
            std::cout << "Usage: rft strategy <failover | best_latency | round_robin>\n";
            return 1;
        }
        std::string body = "{\"strategy\":\"" + args[0] + "\"}";
        std::string raw = http_post(api_port, "/api/strategy", body);
        try {
            auto v = json::Parser::parse(raw);
            if (v["success"].as_bool()) {
                std::cout << "[OK] Strategy updated to: " << v["strategy"].as_string() << "\n";
                return 0;
            } else {
                std::cout << "[ERROR] " << v["message"].as_string() << "\n";
                return 1;
            }
        } catch (...) {
            std::cout << raw << "\n";
            return 0;
        }
    } else if (lower_cmd == "check") {
        std::cout << "Triggering health check on all nodes...\n";
        std::string raw = http_post(api_port, "/api/check", "{}");
        execute_subcommand("nodes", {}, api_port);
        return 0;
    }

    std::cout << "Unknown command: '" << cmd << "'. Type 'rft help' for list of commands.\n";
    return 1;
}

} // namespace cli
