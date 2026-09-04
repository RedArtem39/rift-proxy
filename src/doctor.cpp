#include "doctor.hpp"
#include "config.hpp"
#include "json.hpp"
#include "socket_utils.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace diag {

static bool test_node_probe(const UpstreamNodeConfig& node, int64_t& out_latency_ms, std::string& out_diag) {
    auto start = std::chrono::steady_clock::now();
    SOCKET s = net::connect_to_host(node.host, node.port, 3, out_diag, out_latency_ms);
    if (s == INVALID_SOCKET) {
        return false;
    }

    if (node.type == UpstreamType::SOCKS5) {
        bool has_auth = !node.username.empty();
        uint8_t handshake[4];
        handshake[0] = 0x05;
        if (has_auth) {
            handshake[1] = 0x02;
            handshake[2] = 0x00;
            handshake[3] = 0x02;
            net::send_all(s, (const char*)handshake, 4);
        } else {
            handshake[1] = 0x01;
            handshake[2] = 0x00;
            net::send_all(s, (const char*)handshake, 3);
        }

        uint8_t resp[2];
        if (net::recv_with_timeout(s, (char*)resp, 2, 3) != 2 || resp[0] != 0x05) {
            out_diag = "Invalid SOCKS5 handshake response";
            net::close_socket(s);
            return false;
        }

        if (resp[1] == 0x02) {
            std::vector<uint8_t> auth_pkt;
            auth_pkt.push_back(0x01);
            auth_pkt.push_back((uint8_t)node.username.size());
            auth_pkt.insert(auth_pkt.end(), node.username.begin(), node.username.end());
            auth_pkt.push_back((uint8_t)node.password.size());
            auth_pkt.insert(auth_pkt.end(), node.password.begin(), node.password.end());
            net::send_all(s, (const char*)auth_pkt.data(), (int)auth_pkt.size());

            uint8_t auth_resp[2];
            if (net::recv_with_timeout(s, (char*)auth_resp, 2, 3) != 2 || auth_resp[1] != 0x00) {
                out_diag = "Authentication rejected by proxy";
                net::close_socket(s);
                return false;
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    out_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    net::close_socket(s);
    return true;
}

bool Doctor::run_diagnostics(const std::string& config_path, bool probe_nodes) {
    std::cout << "\n================================================================================\n"
              << "                      RIFT SYSTEM & CONFIG DOCTOR                               \n"
              << "================================================================================\n"
              << "Inspecting target: " << config_path << "\n\n";

    std::vector<CheckResult> results;
    int total_errors = 0;
    int total_warnings = 0;

    // 1. File existence & JSON Syntax
    std::ifstream file(config_path);
    if (!file.is_open()) {
        results.push_back({"File", "config.json presence", false, false, "File does not exist at specified path"});
        total_errors++;
    } else {
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string raw_content = buffer.str();
        file.close();

        if (raw_content.empty()) {
            results.push_back({"File", "config.json content", false, false, "File is empty"});
            total_errors++;
        } else {
            try {
                auto parsed = json::Parser::parse(raw_content);
                results.push_back({"Syntax", "JSON Parser", true, false, "Valid JSON structure"});
            } catch (const std::exception& e) {
                results.push_back({"Syntax", "JSON Parser", false, false, std::string("JSON Syntax Error: ") + e.what()});
                total_errors++;
            }
        }
    }

    // Load structured config
    Config cfg = Config::load_from_file(config_path);

    // 2. Local Listener Configuration
    if (cfg.local_port <= 0 || cfg.local_port > 65535) {
        results.push_back({"Local", "Port Range", false, false, "Invalid port number: " + std::to_string(cfg.local_port)});
        total_errors++;
    } else {
        results.push_back({"Local", "Port Range", true, false, "Port " + std::to_string(cfg.local_port) + " is valid"});
    }

    // Test socket binding availability
    SOCKET test_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (test_s != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)cfg.local_port);
        addr.sin_addr.s_addr = inet_addr(cfg.local_host.c_str());
        if (bind(test_s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEADDRINUSE) {
                results.push_back({"Local", "Port Availability", true, true, "Port is currently in use (Rift daemon may already be running)"});
                total_warnings++;
            } else {
                results.push_back({"Local", "Port Availability", false, false, "Cannot bind to " + cfg.local_host + ":" + std::to_string(cfg.local_port) + " (WSA error " + std::to_string(err) + ")"});
                total_errors++;
            }
        } else {
            results.push_back({"Local", "Port Availability", true, false, "Port is free and ready for binding"});
        }
        closesocket(test_s);
    }

    // Local Auth
    if (cfg.local_auth_enabled) {
        if (cfg.local_username.empty() || cfg.local_password.empty()) {
            results.push_back({"Auth", "Local Credentials", false, false, "Local auth is enabled but username/password is empty"});
            total_errors++;
        } else {
            results.push_back({"Auth", "Local Credentials", true, false, "Local auth enabled for user '" + cfg.local_username + "'"});
        }
    } else {
        results.push_back({"Auth", "Local Credentials", true, false, "No-Auth mode (Open local access)"});
    }

    // 3. Upstream Proxy Pool
    if (cfg.upstream_nodes.empty()) {
        results.push_back({"Pool", "Nodes Count", true, true, "No upstream nodes configured (Operating in direct forwarding mode)"});
        total_warnings++;
    } else {
        results.push_back({"Pool", "Nodes Count", true, false, std::to_string(cfg.upstream_nodes.size()) + " nodes configured"});

        if (cfg.pool_strategy != "failover" && cfg.pool_strategy != "best_latency" && cfg.pool_strategy != "round_robin") {
            results.push_back({"Pool", "Strategy", false, false, "Unknown pool strategy: '" + cfg.pool_strategy + "' (Allowed: failover, best_latency, round_robin)"});
            total_errors++;
        } else {
            results.push_back({"Pool", "Strategy", true, false, "Strategy: " + cfg.pool_strategy});
        }

        if (cfg.health_check_interval < 5) {
            results.push_back({"Pool", "Health Check Interval", false, false, "Interval is too short (< 5s): " + std::to_string(cfg.health_check_interval) + "s"});
            total_errors++;
        } else {
            results.push_back({"Pool", "Health Check Interval", true, false, "Interval: " + std::to_string(cfg.health_check_interval) + "s"});
        }

        // Probe nodes
        if (probe_nodes) {
            for (size_t i = 0; i < cfg.upstream_nodes.size(); ++i) {
                const auto& n = cfg.upstream_nodes[i];
                std::string item_name = "Node #" + std::to_string(i + 1) + " [" + n.name + "]";
                if (n.host.empty() || n.port <= 0 || n.port > 65535) {
                    results.push_back({"Node", item_name, false, false, "Invalid host/port configuration: " + n.host + ":" + std::to_string(n.port)});
                    total_errors++;
                    continue;
                }

                int64_t lat = 0;
                std::string diag;
                bool reachable = test_node_probe(n, lat, diag);
                if (reachable) {
                    results.push_back({"Node", item_name, true, false, "ONLINE (Ping: " + std::to_string(lat) + " ms, " + n.host + ":" + std::to_string(n.port) + ")"});
                } else {
                    results.push_back({"Node", item_name, true, true, "UNREACHABLE / TIMEOUT: " + diag + " (" + n.host + ":" + std::to_string(n.port) + ")"});
                    total_warnings++;
                }
            }
        }
    }

    // 4. Smart Routing Rules Validation
    if (cfg.smart_routing_enabled) {
        if (cfg.rules.empty()) {
            results.push_back({"Rules", "Rule Set", true, true, "Smart routing enabled but rules list is empty"});
            total_warnings++;
        } else {
            int valid_rules = 0;
            int invalid_rules = 0;
            for (size_t i = 0; i < cfg.rules.size(); ++i) {
                const std::string& r = cfg.rules[i];
                std::stringstream ss(r);
                std::string type, pattern, action;
                std::getline(ss, type, ',');
                std::getline(ss, pattern, ',');
                std::getline(ss, action, ',');

                if (type == "FINAL") {
                    action = pattern;
                    if (action == "DIRECT" || action == "PROXY" || action == "REJECT") {
                        valid_rules++;
                    } else {
                        invalid_rules++;
                    }
                } else if (type == "IP-CIDR" || type == "DOMAIN-SUFFIX" || type == "DOMAIN-KEYWORD" || type == "DOMAIN") {
                    if (!pattern.empty() && (action == "DIRECT" || action == "PROXY" || action == "REJECT")) {
                        valid_rules++;
                    } else {
                        invalid_rules++;
                    }
                } else {
                    invalid_rules++;
                }
            }

            if (invalid_rules > 0) {
                results.push_back({"Rules", "Syntax Validation", false, false, std::to_string(invalid_rules) + " invalid rules found out of " + std::to_string(cfg.rules.size())});
                total_errors++;
            } else {
                results.push_back({"Rules", "Syntax Validation", true, false, "All " + std::to_string(valid_rules) + " rules are syntactically valid"});
            }
        }

        if (cfg.default_route != "PROXY" && cfg.default_route != "DIRECT" && cfg.default_route != "REJECT") {
            results.push_back({"Rules", "Default Route", false, false, "Invalid default action: '" + cfg.default_route + "' (Allowed: PROXY, DIRECT, REJECT)"});
            total_errors++;
        } else {
            results.push_back({"Rules", "Default Route", true, false, "Default action: " + cfg.default_route});
        }
    } else {
        results.push_back({"Rules", "Routing Engine", true, false, "Smart routing disabled (all traffic routed via default pool)"});
    }

    // 5. System & Security
    results.push_back({"Security", "Kill Switch", true, false, cfg.kill_switch ? "ENABLED (Strict anti-leak active)" : "DISABLED"});
    results.push_back({"System", "Auto SysProxy", true, false, cfg.enable_system_proxy ? "ENABLED (Will redirect OS on start)" : "DISABLED"});

    // Render results
    std::cout << std::left
              << std::setw(12) << "CATEGORY"
              << std::setw(28) << "CHECK ITEM"
              << std::setw(10) << "STATUS"
              << std::setw(30) << "DETAILS"
              << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (const auto& r : results) {
        std::string status_tag;
        if (!r.passed) {
            status_tag = "[FAIL]";
        } else if (r.is_warning) {
            status_tag = "[WARN]";
        } else {
            status_tag = "[PASS]";
        }

        std::cout << std::left
                  << std::setw(12) << r.category
                  << std::setw(28) << r.item
                  << std::setw(10) << status_tag
                  << r.message
                  << "\n";
    }

    std::cout << std::string(80, '=') << "\n";
    if (total_errors == 0 && total_warnings == 0) {
        std::cout << "DOCTOR VERDICT: [HEALTHY] Config is 100% valid. No errors or warnings found.\n\n";
        return true;
    } else if (total_errors == 0 && total_warnings > 0) {
        std::cout << "DOCTOR VERDICT: [USABLE WITH WARNINGS] Config is valid (" << total_warnings << " non-critical warnings).\n\n";
        return true;
    } else {
        std::cout << "DOCTOR VERDICT: [CORRUPTED / INVALID] Found " << total_errors << " critical error(s). Please fix before starting.\n\n";
        return false;
    }
}

} // namespace diag
