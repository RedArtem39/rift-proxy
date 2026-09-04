#include "cli.hpp"
#include "socket_utils.hpp"
#include "system_proxy.hpp"
#include "upstream.hpp"
#include "logger.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace cli {

CommandLineInterface::CommandLineInterface(proxy::TunnelServer& srv, std::string cfg_path)
    : server(srv), config_path(std::move(cfg_path)) {}

void CommandLineInterface::print_banner() {
    std::cout << "\nProxyClient v2.0 [Professional Smart Routing & Failover Service]\n"
              << "Type 'help' for command list or 'quit' to exit.\n\n";
}

void CommandLineInterface::print_help() {
    std::cout << "\nCommands:\n"
              << "  status                 Show server configuration, mode and pool state\n"
              << "  stats                  Show global traffic metrics (Tx/Rx)\n"
              << "  conns                  Show active connections and timers\n"
              << "  nodes / pool           Show upstream proxy pool and latency status\n"
              << "  rules                  Show active smart routing rules\n"
              << "  check                  Trigger immediate health check on all nodes\n"
              << "  switch <name|id|auto>  Switch active proxy node or revert to auto\n"
              << "  strategy <name>        Set pool strategy (failover|best_latency|round_robin)\n"
              << "  test <host> <port>     Run latency and route probe\n"
              << "  sysproxy on|off        Toggle Windows system proxy settings\n"
              << "  debug on|off           Toggle debug output\n"
              << "  reload                 Reload config.json\n"
              << "  clear                  Clear console screen\n"
              << "  help                   Show this list\n"
              << "  quit / exit            Stop service and exit\n\n";
}

void CommandLineInterface::print_status() {
    const auto& cfg = server.get_config();
    std::cout << "\n--- Proxy Service Status ---\n"
              << "  State:           " << (server.is_running() ? "RUNNING" : "STOPPED") << "\n"
              << "  Listen Address:  " << cfg.local_host << ":" << cfg.local_port << "\n"
              << "  Mode:            " << (cfg.mode == ProtocolMode::DUAL ? "DUAL (HTTP + SOCKS5)" : (cfg.mode == ProtocolMode::SOCKS5 ? "SOCKS5" : "HTTP")) << "\n"
              << "  Local Auth:      " << (cfg.local_auth_enabled ? ("ENABLED (" + cfg.local_username + ")") : "DISABLED") << "\n"
              << "  Smart Routing:   " << (cfg.smart_routing_enabled ? ("ENABLED (Default: " + cfg.default_route + ")") : "DISABLED") << "\n"
              << "  Pool Strategy:   " << server.get_pool().get_active_strategy_name() << "\n"
              << "  Kill Switch:     " << (cfg.kill_switch ? "ENABLED" : "DISABLED") << "\n"
              << "  System Proxy:    " << (sys::SystemProxyManager::is_enabled() ? "ENABLED (Windows system routing active)" : "DISABLED") << "\n\n";
}

void CommandLineInterface::print_stats() {
    std::cout << "\n--- Traffic Metrics ---\n"
              << "  Active Connections:  " << net::Stats::active_connections.load() << "\n"
              << "  Total Handled:       " << net::Stats::total_connections.load() << "\n"
              << "  Transmitted (Tx):    " << net::format_bytes(net::Stats::total_bytes_sent.load()) << "\n"
              << "  Received (Rx):       " << net::format_bytes(net::Stats::total_bytes_received.load()) << "\n\n";
}

void CommandLineInterface::print_active_connections() {
    auto conns = net::ConnectionTracker::get_active_connections();
    std::cout << "\n--- Active Connections (" << conns.size() << ") ---\n";
    if (conns.empty()) {
        std::cout << "  No active connections.\n\n";
        return;
    }

    std::cout << std::left 
              << std::setw(6)  << "ID"
              << std::setw(8)  << "PROTO"
              << std::setw(22) << "CLIENT"
              << std::setw(28) << "TARGET"
              << std::setw(20) << "STAGE"
              << std::setw(12) << "DURATION"
              << std::setw(12) << "IDLE"
              << std::setw(12) << "TX"
              << std::setw(12) << "RX"
              << "\n";
    std::cout << std::string(120, '-') << "\n";

    for (const auto& c : conns) {
        std::cout << std::left
                  << std::setw(6)  << c.id
                  << std::setw(8)  << c.protocol
                  << std::setw(22) << c.client_ip
                  << std::setw(28) << c.target
                  << std::setw(20) << c.stage
                  << std::setw(12) << net::format_duration(c.elapsed_ms())
                  << std::setw(12) << net::format_duration(c.idle_ms())
                  << std::setw(12) << net::format_bytes(c.bytes_up)
                  << std::setw(12) << net::format_bytes(c.bytes_down)
                  << "\n";
    }
    std::cout << "\n";
}

void CommandLineInterface::print_nodes() {
    auto nodes = server.get_pool().get_snapshot();
    std::cout << "\n--- Upstream Proxy Pool (" << nodes.size() << " nodes) [Strategy: " << server.get_pool().get_active_strategy_name() << "] ---\n";
    if (nodes.empty()) {
        std::cout << "  No upstream nodes configured (DIRECT mode).\n\n";
        return;
    }

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
        std::string ep = n.is_direct ? "DIRECT" : (n.host + ":" + std::to_string(n.port));
        std::string status = n.alive.load() ? "ONLINE" : "OFFLINE";
        std::string lat = (n.latency_ms.load() >= 0) ? (std::to_string(n.latency_ms.load()) + " ms") : "TIMEOUT";
        if (n.is_direct) lat = "0 ms";

        std::string type_str = (n.type == UpstreamType::SOCKS5) ? "SOCKS5" : "HTTP";
        if (n.is_direct) type_str = "DIRECT";

        std::cout << std::left
                  << std::setw(4)  << (i + 1)
                  << std::setw(20) << n.name
                  << std::setw(10) << type_str
                  << std::setw(26) << ep
                  << std::setw(12) << status
                  << std::setw(14) << lat
                  << std::setw(10) << n.failure_count.load()
                  << "\n";
    }
    std::cout << "\n";
}

void CommandLineInterface::print_rules() {
    const auto& rules = server.get_router().get_rules();
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
        std::string type_str;
        switch (r.type) {
            case RuleType::DOMAIN_SUFFIX: type_str = "DOMAIN-SUFFIX"; break;
            case RuleType::DOMAIN_KEYWORD: type_str = "DOMAIN-KEYWORD"; break;
            case RuleType::DOMAIN_EXACT: type_str = "DOMAIN"; break;
            case RuleType::IP_CIDR: type_str = "IP-CIDR"; break;
            case RuleType::FINAL_RULE: type_str = "FINAL"; break;
        }

        std::string act_str = (r.action == RouteAction::DIRECT ? "DIRECT" : (r.action == RouteAction::REJECT ? "REJECT" : "PROXY"));

        std::cout << std::left
                  << std::setw(4)  << (i + 1)
                  << std::setw(18) << type_str
                  << std::setw(32) << r.pattern
                  << std::setw(12) << act_str
                  << "\n";
    }
    std::cout << "Default Action: " << (server.get_config().default_route) << "\n\n";
}

void CommandLineInterface::test_connectivity(const std::string& host, int port) {
    std::cout << "Probing " << host << ":" << port << " through smart routing pipeline..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();

    std::string err;
    int64_t elapsed_ms = 0;
    std::string route_info;
    SOCKET s = proxy::UpstreamHandler::connect_target(0, server.get_config(), server.get_router(), server.get_pool(), host, port, err, elapsed_ms, route_info);
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

    if (s != INVALID_SOCKET) {
        std::cout << "[SUCCESS] Target " << host << ":" << port << " [" << route_info << "] reached in " << elapsed_ms << "ms (total pipeline: " << total_ms << "ms)\n\n";
        net::close_socket(s);
    } else {
        std::cout << "[FAILED] Target " << host << ":" << port << " [" << route_info << "] unreachable: " << err << " (elapsed: " << total_ms << "ms)\n\n";
    }
}

void CommandLineInterface::handle_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd.empty()) return;

    if (cmd == "help" || cmd == "?") {
        print_help();
    } else if (cmd == "status") {
        print_status();
    } else if (cmd == "stats") {
        print_stats();
    } else if (cmd == "conns" || cmd == "connections") {
        print_active_connections();
    } else if (cmd == "nodes" || cmd == "pool") {
        print_nodes();
    } else if (cmd == "rules") {
        print_rules();
    } else if (cmd == "check") {
        std::cout << "Running health check on all upstream nodes...\n";
        server.get_pool().run_health_check_all();
        print_nodes();
    } else if (cmd == "switch") {
        std::string target;
        if (iss >> target) {
            if (server.get_pool().manual_select(target)) {
                std::cout << "Active node switched to: " << server.get_pool().get_active_strategy_name() << "\n";
            } else {
                std::cout << "Node '" << target << "' not found in pool.\n";
            }
        } else {
            std::cout << "Usage: switch <node_name | node_number | auto>\n";
        }
    } else if (cmd == "strategy") {
        std::string strat;
        if (iss >> strat) {
            if (strat == "failover") {
                server.get_pool().set_strategy(PoolStrategy::FAILOVER);
                std::cout << "Strategy set to FAILOVER\n";
            } else if (strat == "best_latency" || strat == "best_ping" || strat == "speed") {
                server.get_pool().set_strategy(PoolStrategy::BEST_LATENCY);
                std::cout << "Strategy set to BEST_LATENCY\n";
            } else if (strat == "round_robin" || strat == "rr") {
                server.get_pool().set_strategy(PoolStrategy::ROUND_ROBIN);
                std::cout << "Strategy set to ROUND_ROBIN\n";
            } else {
                std::cout << "Available strategies: failover, best_latency, round_robin\n";
            }
        } else {
            std::cout << "Usage: strategy <failover|best_latency|round_robin>\n";
        }
    } else if (cmd == "sysproxy") {
        std::string sub;
        iss >> sub;
        if (sub == "on") {
            const auto& cfg = server.get_config();
            sys::SystemProxyManager::enable(cfg.local_host, cfg.local_port, cfg.system_proxy_bypass);
        } else if (sub == "off") {
            sys::SystemProxyManager::disable();
        } else {
            std::cout << "Usage: sysproxy on|off\n";
        }
    } else if (cmd == "debug") {
        std::string sub;
        iss >> sub;
        if (sub == "on") {
            Logger::set_debug(true);
            std::cout << "Debug logging enabled.\n";
        } else if (sub == "off") {
            Logger::set_debug(false);
            std::cout << "Debug logging disabled.\n";
        } else {
            std::cout << "Usage: debug on|off\n";
        }
    } else if (cmd == "test") {
        std::string host = "1.1.1.1";
        int port = 443;
        if (iss >> host) {
            iss >> port;
        }
        test_connectivity(host, port);
    } else if (cmd == "reload") {
        Config new_cfg = Config::load_from_file(config_path);
        server.update_config(new_cfg);
        std::cout << "Configuration reloaded from " << config_path << "\n";
    } else if (cmd == "clear" || cmd == "cls") {
        system("cls");
        print_banner();
    } else if (cmd == "quit" || cmd == "exit") {
        server.stop();
    } else {
        std::cout << "Unknown command: '" << cmd << "'. Type 'help' for command list.\n";
    }
}

void CommandLineInterface::run() {
    print_banner();
    print_status();

    std::string line;
    while (server.is_running()) {
        std::cout << "proxy> ";
        if (!std::getline(std::cin, line)) {
            break;
        }
        handle_command(line);
    }
}

} // namespace cli
