#include "config.hpp"
#include "socket_utils.hpp"
#include "socks5.hpp"
#include "json.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace configtool {

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

bool parse_proxy_line(const std::string& raw_input, UpstreamNodeConfig& out_node) {
    std::string input = trim(raw_input);
    if (input.empty()) return false;

    // Default values
    out_node.type = UpstreamType::SOCKS5;
    out_node.username = "";
    out_node.password = "";

    // Check scheme if present
    if (input.rfind("socks5://", 0) == 0) {
        out_node.type = UpstreamType::SOCKS5;
        input = input.substr(9);
    } else if (input.rfind("http://", 0) == 0) {
        out_node.type = UpstreamType::HTTP;
        input = input.substr(7);
    }

    // Check user:pass@host:port format
    size_t at_pos = input.find('@');
    if (at_pos != std::string::npos) {
        std::string auth_part = input.substr(0, at_pos);
        std::string host_part = input.substr(at_pos + 1);

        size_t colon_auth = auth_part.find(':');
        if (colon_auth != std::string::npos) {
            out_node.username = auth_part.substr(0, colon_auth);
            out_node.password = auth_part.substr(colon_auth + 1);
        } else {
            out_node.username = auth_part;
        }

        size_t colon_host = host_part.find(':');
        if (colon_host != std::string::npos) {
            out_node.host = host_part.substr(0, colon_host);
            out_node.port = std::stoi(host_part.substr(colon_host + 1));
        } else {
            out_node.host = host_part;
            out_node.port = 1080;
        }
    } else {
        // Formats: host:port:user:pass or host:port
        std::vector<std::string> parts;
        std::stringstream ss(input);
        std::string item;
        while (std::getline(ss, item, ':')) {
            parts.push_back(item);
        }

        if (parts.size() == 2) {
            out_node.host = parts[0];
            out_node.port = std::stoi(parts[1]);
        } else if (parts.size() == 4) {
            out_node.host = parts[0];
            out_node.port = std::stoi(parts[1]);
            out_node.username = parts[2];
            out_node.password = parts[3];
        } else {
            return false;
        }
    }

    if (out_node.name.empty()) {
        out_node.name = "Node-" + out_node.host;
    }
    return !out_node.host.empty() && out_node.port > 0;
}

bool test_node(const UpstreamNodeConfig& node, int64_t& out_latency_ms, std::string& out_diag) {
    auto start = std::chrono::steady_clock::now();
    SOCKET s = net::connect_to_host(node.host, node.port, 4, out_diag, out_latency_ms);
    if (s == INVALID_SOCKET) {
        return false;
    }

    if (node.type == UpstreamType::SOCKS5) {
        // Authenticate if needed
        bool has_auth = !node.username.empty();
        uint8_t handshake[4];
        handshake[0] = 0x05; // SOCKS5
        if (has_auth) {
            handshake[1] = 0x02; // 2 methods
            handshake[2] = 0x00; // No auth
            handshake[3] = 0x02; // Username/Password
            net::send_all(s, (const char*)handshake, 4);
        } else {
            handshake[1] = 0x01; // 1 method
            handshake[2] = 0x00; // No auth
            net::send_all(s, (const char*)handshake, 3);
        }

        uint8_t resp[2];
        if (net::recv_with_timeout(s, (char*)resp, 2, 4) != 2 || resp[0] != 0x05) {
            out_diag = "Invalid SOCKS5 handshake response from proxy";
            net::close_socket(s);
            return false;
        }

        if (resp[1] == 0x02) {
            // Send auth
            std::vector<uint8_t> auth_pkt;
            auth_pkt.push_back(0x01);
            auth_pkt.push_back((uint8_t)node.username.size());
            auth_pkt.insert(auth_pkt.end(), node.username.begin(), node.username.end());
            auth_pkt.push_back((uint8_t)node.password.size());
            auth_pkt.insert(auth_pkt.end(), node.password.begin(), node.password.end());
            net::send_all(s, (const char*)auth_pkt.data(), (int)auth_pkt.size());

            uint8_t auth_resp[2];
            if (net::recv_with_timeout(s, (char*)auth_resp, 2, 4) != 2 || auth_resp[1] != 0x00) {
                out_diag = "Authentication failed (wrong username/password)";
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

void apply_preset(Config& cfg, int preset_choice) {
    if (preset_choice == 1) {
        // Bypass RU & CIS
        cfg.smart_routing_enabled = true;
        cfg.default_route = "PROXY";
        cfg.rules = {
            "IP-CIDR,127.0.0.0/8,DIRECT",
            "IP-CIDR,192.168.0.0/16,DIRECT",
            "IP-CIDR,10.0.0.0/8,DIRECT",
            "IP-CIDR,172.16.0.0/12,DIRECT",
            "DOMAIN-SUFFIX,ru,DIRECT",
            "DOMAIN-SUFFIX,su,DIRECT",
            "DOMAIN-SUFFIX,by,DIRECT",
            "DOMAIN-SUFFIX,kz,DIRECT",
            "DOMAIN-SUFFIX,gosuslugi.ru,DIRECT",
            "DOMAIN-KEYWORD,sber,DIRECT",
            "DOMAIN-KEYWORD,tinkoff,DIRECT",
            "DOMAIN-KEYWORD,vtb,DIRECT",
            "DOMAIN-KEYWORD,alfa,DIRECT",
            "DOMAIN-KEYWORD,yandex,DIRECT",
            "FINAL,PROXY"
        };
        std::cout << "[SUCCESS] Applied Preset: Bypass RU/CIS & Banking DIRECT (All other traffic -> PROXY)\n";
    } else if (preset_choice == 2) {
        // Full Tunnel
        cfg.smart_routing_enabled = true;
        cfg.default_route = "PROXY";
        cfg.rules = {
            "IP-CIDR,127.0.0.0/8,DIRECT",
            "IP-CIDR,192.168.0.0/16,DIRECT",
            "FINAL,PROXY"
        };
        std::cout << "[SUCCESS] Applied Preset: Full Tunnel (100% traffic through proxy)\n";
    } else if (preset_choice == 3) {
        // Blocklist Proxy Only
        cfg.smart_routing_enabled = true;
        cfg.default_route = "DIRECT";
        cfg.rules = {
            "DOMAIN-KEYWORD,instagram,PROXY",
            "DOMAIN-KEYWORD,twitter,PROXY",
            "DOMAIN-KEYWORD,facebook,PROXY",
            "DOMAIN-KEYWORD,notion,PROXY",
            "DOMAIN-KEYWORD,chatgpt,PROXY",
            "DOMAIN-KEYWORD,openai,PROXY",
            "DOMAIN-KEYWORD,spotify,PROXY",
            "FINAL,DIRECT"
        };
        std::cout << "[SUCCESS] Applied Preset: Selective Proxy (Only blocked services -> PROXY, rest DIRECT)\n";
    }
}

void print_header(const std::string& title) {
    std::cout << "\n================================================================================\n"
              << "  " << title << "\n"
              << "================================================================================\n";
}

void menu_proxy_pool(Config& cfg) {
    while (true) {
        print_header("PROXY POOL & UPSTREAM NODES");
        std::cout << "Active Strategy: [" << cfg.pool_strategy << "] | Health Check: every " << cfg.health_check_interval << "s\n\n";

        if (cfg.upstream_nodes.empty()) {
            std::cout << "  (No upstream nodes configured. Direct routing only)\n\n";
        } else {
            std::cout << std::left
                      << std::setw(4)  << "#"
                      << std::setw(20) << "NAME"
                      << std::setw(10) << "TYPE"
                      << std::setw(26) << "ENDPOINT"
                      << std::setw(20) << "AUTH"
                      << "\n";
            std::cout << std::string(80, '-') << "\n";
            for (size_t i = 0; i < cfg.upstream_nodes.size(); ++i) {
                const auto& n = cfg.upstream_nodes[i];
                std::string ep = n.host + ":" + std::to_string(n.port);
                std::string auth_str = n.username.empty() ? "None" : (n.username + ":***");
                std::cout << std::left
                          << std::setw(4)  << (i + 1)
                          << std::setw(20) << n.name
                          << std::setw(10) << (n.type == UpstreamType::SOCKS5 ? "SOCKS5" : "HTTP")
                          << std::setw(26) << ep
                          << std::setw(20) << auth_str
                          << "\n";
            }
            std::cout << "\n";
        }

        std::cout << "Options:\n"
                  << "  [1] Add Node (Quick paste or input)\n"
                  << "  [2] Test All Nodes Latency\n"
                  << "  [3] Delete a Node\n"
                  << "  [4] Change Pool Strategy (failover / best_latency / round_robin)\n"
                  << "  [5] Change Health Check Interval\n"
                  << "  [0] Back to Main Menu\n\n"
                  << "Enter choice: ";

        std::string choice_str;
        if (!std::getline(std::cin, choice_str)) break;
        choice_str = trim(choice_str);

        if (choice_str == "0") break;

        if (choice_str == "1") {
            std::cout << "\nEnter proxy string (e.g. 152.232.171.125:8000:user:pass or host:port):\n> ";
            std::string line;
            std::getline(std::cin, line);
            UpstreamNodeConfig node;
            if (parse_proxy_line(line, node)) {
                std::cout << "Enter node name (press Enter for '" << node.name << "'): ";
                std::string custom_name;
                std::getline(std::cin, custom_name);
                if (!trim(custom_name).empty()) node.name = trim(custom_name);

                std::cout << "Testing connection to " << node.host << ":" << node.port << "...\n";
                int64_t lat = 0;
                std::string diag;
                if (test_node(node, lat, diag)) {
                    std::cout << "[SUCCESS] Node responded in " << lat << " ms! Added to pool.\n";
                } else {
                    std::cout << "[WARNING] Test connection failed: " << diag << ". Adding anyway.\n";
                }
                cfg.upstream_nodes.push_back(node);
            } else {
                std::cout << "[ERROR] Could not parse proxy format. Expected host:port[:user:pass]\n";
            }
        } else if (choice_str == "2") {
            if (cfg.upstream_nodes.empty()) {
                std::cout << "No nodes to test.\n";
            } else {
                std::cout << "\nTesting all nodes...\n";
                for (size_t i = 0; i < cfg.upstream_nodes.size(); ++i) {
                    const auto& n = cfg.upstream_nodes[i];
                    std::cout << "  #" << (i + 1) << " [" << n.name << "] " << n.host << ":" << n.port << " ... ";
                    int64_t lat = 0;
                    std::string diag;
                    if (test_node(n, lat, diag)) {
                        std::cout << "ONLINE (" << lat << " ms)\n";
                    } else {
                        std::cout << "FAILED (" << diag << ")\n";
                    }
                }
            }
        } else if (choice_str == "3") {
            if (cfg.upstream_nodes.empty()) {
                std::cout << "No nodes to delete.\n";
            } else {
                std::cout << "Enter node number to delete (1-" << cfg.upstream_nodes.size() << "): ";
                std::string idx_str;
                std::getline(std::cin, idx_str);
                try {
                    int idx = std::stoi(idx_str);
                    if (idx >= 1 && idx <= (int)cfg.upstream_nodes.size()) {
                        cfg.upstream_nodes.erase(cfg.upstream_nodes.begin() + (idx - 1));
                        std::cout << "[SUCCESS] Node removed.\n";
                    } else {
                        std::cout << "[ERROR] Invalid index.\n";
                    }
                } catch (...) {
                    std::cout << "[ERROR] Invalid number.\n";
                }
            }
        } else if (choice_str == "4") {
            std::cout << "\nSelect Strategy:\n"
                      << "  [1] failover     (Use primary, switch to backup on fail)\n"
                      << "  [2] best_latency (Automatically pick lowest ping node)\n"
                      << "  [3] round_robin  (Distribute requests evenly)\n"
                      << "Choice: ";
            std::string strat_choice;
            std::getline(std::cin, strat_choice);
            if (strat_choice == "1") cfg.pool_strategy = "failover";
            else if (strat_choice == "2") cfg.pool_strategy = "best_latency";
            else if (strat_choice == "3") cfg.pool_strategy = "round_robin";
            std::cout << "[SUCCESS] Strategy set to: " << cfg.pool_strategy << "\n";
        } else if (choice_str == "5") {
            std::cout << "Enter health check interval in seconds (default: 30): ";
            std::string int_str;
            std::getline(std::cin, int_str);
            try {
                int sec = std::stoi(int_str);
                if (sec >= 5) {
                    cfg.health_check_interval = sec;
                    std::cout << "[SUCCESS] Health check interval set to " << sec << "s.\n";
                }
            } catch (...) {}
        }
    }
}

void menu_routing_rules(Config& cfg) {
    while (true) {
        print_header("SMART ROUTING RULES");
        std::cout << "Enabled: [" << (cfg.smart_routing_enabled ? "YES" : "NO") << "] | Default Action: [" << cfg.default_route << "]\n\n";

        std::cout << std::left
                  << std::setw(4)  << "#"
                  << std::setw(18) << "RULE TYPE"
                  << std::setw(34) << "PATTERN"
                  << std::setw(12) << "ACTION"
                  << "\n";
        std::cout << std::string(68, '-') << "\n";

        for (size_t i = 0; i < cfg.rules.size(); ++i) {
            std::string r = cfg.rules[i];
            std::string type, pattern, action;
            std::stringstream ss(r);
            std::getline(ss, type, ',');
            std::getline(ss, pattern, ',');
            std::getline(ss, action, ',');
            if (action.empty() && !pattern.empty()) {
                action = pattern;
                pattern = "-";
            }
            std::cout << std::left
                      << std::setw(4)  << (i + 1)
                      << std::setw(18) << type
                      << std::setw(34) << pattern
                      << std::setw(12) << action
                      << "\n";
        }
        std::cout << "\n";

        std::cout << "Options:\n"
                  << "  [1] Add Custom Rule (e.g. DOMAIN-SUFFIX,ru,DIRECT)\n"
                  << "  [2] Delete a Rule\n"
                  << "  [3] Toggle Smart Routing (Enable/Disable)\n"
                  << "  [4] Change Default Action (PROXY / DIRECT / REJECT)\n"
                  << "  [5] Apply Routing Preset (Bypass RU / Full Tunnel / Selective)\n"
                  << "  [0] Back to Main Menu\n\n"
                  << "Enter choice: ";

        std::string choice_str;
        if (!std::getline(std::cin, choice_str)) break;
        choice_str = trim(choice_str);

        if (choice_str == "0") break;

        if (choice_str == "1") {
            std::cout << "\nEnter rule in format: TYPE,PATTERN,ACTION\n"
                      << "Types: DOMAIN-SUFFIX, DOMAIN-KEYWORD, DOMAIN, IP-CIDR, FINAL\n"
                      << "Actions: DIRECT, PROXY, REJECT\n"
                      << "Example: DOMAIN-SUFFIX,ru,DIRECT\n> ";
            std::string rule_input;
            std::getline(std::cin, rule_input);
            rule_input = trim(rule_input);
            if (!rule_input.empty()) {
                cfg.rules.push_back(rule_input);
                std::cout << "[SUCCESS] Rule added.\n";
            }
        } else if (choice_str == "2") {
            std::cout << "Enter rule number to delete (1-" << cfg.rules.size() << "): ";
            std::string idx_str;
            std::getline(std::cin, idx_str);
            try {
                int idx = std::stoi(idx_str);
                if (idx >= 1 && idx <= (int)cfg.rules.size()) {
                    cfg.rules.erase(cfg.rules.begin() + (idx - 1));
                    std::cout << "[SUCCESS] Rule deleted.\n";
                }
            } catch (...) {}
        } else if (choice_str == "3") {
            cfg.smart_routing_enabled = !cfg.smart_routing_enabled;
            std::cout << "[SUCCESS] Smart routing is now: " << (cfg.smart_routing_enabled ? "ENABLED" : "DISABLED") << "\n";
        } else if (choice_str == "4") {
            std::cout << "Select default action: [1] PROXY  [2] DIRECT  [3] REJECT : ";
            std::string a_str;
            std::getline(std::cin, a_str);
            if (a_str == "1") cfg.default_route = "PROXY";
            else if (a_str == "2") cfg.default_route = "DIRECT";
            else if (a_str == "3") cfg.default_route = "REJECT";
            std::cout << "[SUCCESS] Default route set to: " << cfg.default_route << "\n";
        } else if (choice_str == "5") {
            std::cout << "\nSelect Preset:\n"
                      << "  [1] Bypass RU/CIS & Banking (ru/su/gosuslugi/banks -> DIRECT, rest -> PROXY)\n"
                      << "  [2] Full Tunnel (100% traffic through proxy)\n"
                      << "  [3] Selective (Only blocked socials/AI -> PROXY, rest -> DIRECT)\n"
                      << "Choice: ";
            std::string p_str;
            std::getline(std::cin, p_str);
            try {
                apply_preset(cfg, std::stoi(p_str));
            } catch (...) {}
        }
    }
}

void menu_local_server(Config& cfg) {
    print_header("LOCAL SERVER & PROTOCOL");
    std::cout << "  Listen Address: " << cfg.local_host << ":" << cfg.local_port << "\n"
              << "  Protocol Mode:  " << (cfg.mode == ProtocolMode::DUAL ? "DUAL (HTTP + SOCKS5)" : (cfg.mode == ProtocolMode::SOCKS5 ? "SOCKS5" : "HTTP")) << "\n"
              << "  Local Auth:     " << (cfg.local_auth_enabled ? ("ENABLED (" + cfg.local_username + ")") : "DISABLED") << "\n\n";

    std::cout << "Options:\n"
              << "  [1] Change Local Port (Current: " << cfg.local_port << ")\n"
              << "  [2] Change Protocol Mode (dual / socks5 / http)\n"
              << "  [3] Toggle Local Authentication\n"
              << "  [0] Back\n\nChoice: ";

    std::string c_str;
    std::getline(std::cin, c_str);
    c_str = trim(c_str);

    if (c_str == "1") {
        std::cout << "Enter new local port (1-65535): ";
        std::string p_str;
        std::getline(std::cin, p_str);
        try {
            int p = std::stoi(p_str);
            if (p > 0 && p <= 65535) {
                cfg.local_port = p;
                std::cout << "[SUCCESS] Local port changed to: " << p << "\n";
            }
        } catch (...) {}
    } else if (c_str == "2") {
        std::cout << "Select mode: [1] dual (SOCKS5+HTTP)  [2] socks5  [3] http : ";
        std::string m_str;
        std::getline(std::cin, m_str);
        if (m_str == "1") cfg.mode = ProtocolMode::DUAL;
        else if (m_str == "2") cfg.mode = ProtocolMode::SOCKS5;
        else if (m_str == "3") cfg.mode = ProtocolMode::HTTP;
        std::cout << "[SUCCESS] Mode updated.\n";
    } else if (c_str == "3") {
        cfg.local_auth_enabled = !cfg.local_auth_enabled;
        if (cfg.local_auth_enabled) {
            std::cout << "Enter local username: ";
            std::getline(std::cin, cfg.local_username);
            std::cout << "Enter local password: ";
            std::getline(std::cin, cfg.local_password);
        }
        std::cout << "[SUCCESS] Local auth: " << (cfg.local_auth_enabled ? "ENABLED" : "DISABLED") << "\n";
    }
}

void menu_system_security(Config& cfg) {
    print_header("SYSTEM PROXY & SECURITY");
    std::cout << "  Auto System Proxy on Startup: " << (cfg.enable_system_proxy ? "ENABLED" : "DISABLED") << "\n"
              << "  Kill Switch (Leak Protection): " << (cfg.kill_switch ? "ENABLED" : "DISABLED") << "\n"
              << "  Verbose Debug Logging:         " << (cfg.debug_log ? "ENABLED" : "DISABLED") << "\n"
              << "  System Proxy Bypass:           " << cfg.system_proxy_bypass << "\n\n";

    std::cout << "Options:\n"
              << "  [1] Toggle Auto System Proxy on Startup\n"
              << "  [2] Toggle Kill Switch (Strict Anti-Leak)\n"
              << "  [3] Toggle Debug Logging\n"
              << "  [4] Change System Proxy Bypass List\n"
              << "  [0] Back\n\nChoice: ";

    std::string c_str;
    std::getline(std::cin, c_str);
    c_str = trim(c_str);

    if (c_str == "1") {
        cfg.enable_system_proxy = !cfg.enable_system_proxy;
        std::cout << "[SUCCESS] Auto System Proxy: " << (cfg.enable_system_proxy ? "ENABLED" : "DISABLED") << "\n";
    } else if (c_str == "2") {
        cfg.kill_switch = !cfg.kill_switch;
        std::cout << "[SUCCESS] Kill Switch: " << (cfg.kill_switch ? "ENABLED" : "DISABLED") << "\n";
    } else if (c_str == "3") {
        cfg.debug_log = !cfg.debug_log;
        std::cout << "[SUCCESS] Debug Log: " << (cfg.debug_log ? "ENABLED" : "DISABLED") << "\n";
    } else if (c_str == "4") {
        std::cout << "Enter bypass list (semicolon separated, e.g. localhost;127.*;10.*):\n> ";
        std::string bp;
        std::getline(std::cin, bp);
        if (!trim(bp).empty()) {
            cfg.system_proxy_bypass = trim(bp);
            std::cout << "[SUCCESS] Bypass list updated.\n";
        }
    }
}

} // namespace configtool

#include "doctor.hpp"

void print_configrif_usage(const char* prog) {
    std::cout << "Rift Configuration Manager (configrif)\n\n"
              << "Interactive Mode:\n"
              << "  " << prog << " [path/to/config.json]\n\n"
              << "Non-Interactive Subcommands:\n"
              << "  " << prog << " add-node <ip:port:user:pass> [name]\n"
              << "  " << prog << " set-port <port>\n"
              << "  " << prog << " set-strategy <failover|best_latency|round_robin>\n"
              << "  " << prog << " preset <bypass-ru | full-tunnel | selective>\n"
              << "  " << prog << " doctor | doktor\n"
              << "  " << prog << " test\n\n";
}

int main(int argc, char* argv[]) {
    net::WinsockScope winsock;

    std::string config_path = "config.json";

    // Non-interactive command line modes
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help" || arg1 == "help") {
            print_configrif_usage(argv[0]);
            return 0;
        }

        if (arg1 == "doctor" || arg1 == "doktor") {
            std::string target_cfg = (argc > 2 && argv[2][0] != '-') ? argv[2] : config_path;
            bool ok = diag::Doctor::run_diagnostics(target_cfg, true);
            return ok ? 0 : 1;
        } else if (arg1 == "add-node" && argc > 2) {
            Config cfg = Config::load_from_file(config_path);
            UpstreamNodeConfig node;
            if (configtool::parse_proxy_line(argv[2], node)) {
                if (argc > 3) node.name = argv[3];
                int64_t lat = 0;
                std::string diag;
                bool ok = configtool::test_node(node, lat, diag);
                cfg.upstream_nodes.push_back(node);
                cfg.save_to_file(config_path);
                std::cout << "[SUCCESS] Added node '" << node.name << "' (" << node.host << ":" << node.port << ") "
                          << (ok ? ("ONLINE: " + std::to_string(lat) + "ms") : ("WARNING: " + diag)) << "\n";
                return 0;
            } else {
                std::cout << "[ERROR] Failed to parse proxy: " << argv[2] << "\n";
                return 1;
            }
        } else if (arg1 == "set-port" && argc > 2) {
            Config cfg = Config::load_from_file(config_path);
            cfg.local_port = std::stoi(argv[2]);
            cfg.save_to_file(config_path);
            std::cout << "[SUCCESS] Local port set to " << cfg.local_port << "\n";
            return 0;
        } else if (arg1 == "set-strategy" && argc > 2) {
            Config cfg = Config::load_from_file(config_path);
            cfg.pool_strategy = argv[2];
            cfg.save_to_file(config_path);
            std::cout << "[SUCCESS] Pool strategy set to " << cfg.pool_strategy << "\n";
            return 0;
        } else if (arg1 == "preset" && argc > 2) {
            Config cfg = Config::load_from_file(config_path);
            std::string p = argv[2];
            if (p == "bypass-ru" || p == "ru") configtool::apply_preset(cfg, 1);
            else if (p == "full-tunnel" || p == "full") configtool::apply_preset(cfg, 2);
            else if (p == "selective") configtool::apply_preset(cfg, 3);
            cfg.save_to_file(config_path);
            return 0;
        } else if (arg1 == "test") {
            Config cfg = Config::load_from_file(config_path);
            std::cout << "Testing " << cfg.upstream_nodes.size() << " nodes in " << config_path << "...\n";
            for (size_t i = 0; i < cfg.upstream_nodes.size(); ++i) {
                const auto& n = cfg.upstream_nodes[i];
                int64_t lat = 0;
                std::string diag;
                bool ok = configtool::test_node(n, lat, diag);
                std::cout << "  #" << (i + 1) << " [" << n.name << "] " << n.host << ":" << n.port
                          << (ok ? (" -> ONLINE (" + std::to_string(lat) + " ms)") : (" -> FAILED (" + diag + ")")) << "\n";
            }
            return 0;
        } else if (arg1.find(".json") != std::string::npos) {
            config_path = arg1;
        }
    }

    Config cfg = Config::load_from_file(config_path);

    while (true) {
        configtool::print_header("RIFT CONFIGURATION MANAGER (configrif)");
        std::cout << "Active config file: " << config_path << "\n\n"
                  << "  [1] Proxy Pool & Nodes        (" << cfg.upstream_nodes.size() << " nodes configured, Strategy: " << cfg.pool_strategy << ")\n"
                  << "  [2] Quick Paste Proxy Node    (Paste IP:Port:User:Pass directly)\n"
                  << "  [3] Smart Routing Rules       (" << cfg.rules.size() << " rules, Default: " << cfg.default_route << ")\n"
                  << "  [4] Local Server & Protocol   (Port: " << cfg.local_port << ", Mode: " << (cfg.mode == ProtocolMode::DUAL ? "dual" : "socks5") << ")\n"
                  << "  [5] System Proxy & Security   (Auto-Sysproxy: " << (cfg.enable_system_proxy ? "ON" : "OFF") << ", KillSwitch: " << (cfg.kill_switch ? "ON" : "OFF") << ")\n"
                  << "  [6] Apply Quick Routing Preset(Bypass RU / Full Tunnel / Selective)\n"
                  << "  [7] Test All Configured Nodes\n"
                  << "  [8] Run Doctor Diagnostics    (Comprehensive Config & System Validation)\n"
                  << "  [9] View Raw JSON Config\n"
                  << "  [S] Save and Exit\n"
                  << "  [0] Exit without saving\n\n"
                  << "Select option: ";

        std::string choice;
        if (!std::getline(std::cin, choice)) break;
        choice = configtool::trim(choice);

        if (choice == "0") {
            std::cout << "Exited without saving.\n";
            break;
        } else if (choice == "1") {
            configtool::menu_proxy_pool(cfg);
        } else if (choice == "2") {
            std::cout << "\nPaste proxy string (e.g. 152.232.171.125:8000:Yz2063:zxnbKF):\n> ";
            std::string line;
            std::getline(std::cin, line);
            UpstreamNodeConfig node;
            if (configtool::parse_proxy_line(line, node)) {
                std::cout << "Enter node name (press Enter for '" << node.name << "'): ";
                std::string custom_name;
                std::getline(std::cin, custom_name);
                if (!configtool::trim(custom_name).empty()) node.name = configtool::trim(custom_name);

                int64_t lat = 0;
                std::string diag;
                std::cout << "Testing connection to " << node.host << ":" << node.port << "...\n";
                if (configtool::test_node(node, lat, diag)) {
                    std::cout << "[SUCCESS] Node responded in " << lat << " ms! Added to pool.\n";
                } else {
                    std::cout << "[WARNING] Test connection failed: " << diag << ". Adding anyway.\n";
                }
                cfg.upstream_nodes.push_back(node);
            } else {
                std::cout << "[ERROR] Invalid format.\n";
            }
        } else if (choice == "3") {
            configtool::menu_routing_rules(cfg);
        } else if (choice == "4") {
            configtool::menu_local_server(cfg);
        } else if (choice == "5") {
            configtool::menu_system_security(cfg);
        } else if (choice == "6") {
            std::cout << "\nSelect Preset:\n"
                      << "  [1] Bypass RU/CIS & Banking (ru/su/gosuslugi/banks -> DIRECT, rest -> PROXY)\n"
                      << "  [2] Full Tunnel (100% traffic through proxy)\n"
                      << "  [3] Selective (Only blocked socials/AI -> PROXY, rest -> DIRECT)\n"
                      << "Choice: ";
            std::string p_str;
            std::getline(std::cin, p_str);
            try {
                configtool::apply_preset(cfg, std::stoi(p_str));
            } catch (...) {}
        } else if (choice == "7") {
            std::cout << "\nTesting all nodes...\n";
            for (size_t i = 0; i < cfg.upstream_nodes.size(); ++i) {
                const auto& n = cfg.upstream_nodes[i];
                int64_t lat = 0;
                std::string diag;
                bool ok = configtool::test_node(n, lat, diag);
                std::cout << "  #" << (i + 1) << " [" << n.name << "] " << n.host << ":" << n.port
                          << (ok ? (" -> ONLINE (" + std::to_string(lat) + " ms)") : (" -> FAILED (" + diag + ")")) << "\n";
            }
        } else if (choice == "8" || choice == "doctor" || choice == "doktor" || choice == "d" || choice == "D") {
            diag::Doctor::run_diagnostics(config_path, true);
        } else if (choice == "9") {
            std::cout << "\n--- Current JSON Configuration ---\n"
                      << cfg.to_json_string() << "\n\n";
        } else if (choice == "s" || choice == "S" || choice == "save" || choice == "10") {
            if (cfg.save_to_file(config_path)) {
                std::cout << "[SUCCESS] Configuration saved to " << config_path << ".\n";
            } else {
                std::cout << "[ERROR] Failed to save configuration to " << config_path << ".\n";
            }
            break;
        }
    }

    return 0;
}
