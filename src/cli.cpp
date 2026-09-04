#include "cli.hpp"
#include "socket_utils.hpp"
#include "system_proxy.hpp"
#include "upstream.hpp"
#include "logger.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>

namespace cli {

static std::string to_lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

CommandLineInterface::CommandLineInterface(proxy::TunnelServer& srv, std::string cfg_path)
    : server(srv), config_path(std::move(cfg_path)) {}

void CommandLineInterface::print_banner() {
    std::cout << "\nProxyClient v2.0 [Professional Smart Routing & Failover Service]\n"
              << "Type 'help' for command list or 'help <command>' for detailed documentation.\n\n";
}

void CommandLineInterface::print_help(const std::string& specific_cmd) {
    if (!specific_cmd.empty()) {
        if (specific_cmd == "all") {
            print_all_docs();
        } else {
            print_command_doc(specific_cmd);
        }
        return;
    }

    std::cout << "\n================================================================================\n"
              << "                        PROXYCLIENT COMMAND INDEX                               \n"
              << "================================================================================\n\n"
              << "1. INSPECTION & TELEMETRY:\n"
              << "  status                 Display service runtime configuration, state, and policies\n"
              << "  stats                  Display global bandwidth (Tx/Rx) and total connection metrics\n"
              << "  conns                  Display real-time table of active connections with timers\n"
              << "  nodes / pool           Display upstream proxy pool status, latency, and fail counts\n"
              << "  rules                  Display smart routing classification rules and default action\n\n"
              << "2. POOL & ROUTING MANAGEMENT:\n"
              << "  check                  Trigger immediate ping / health check on all upstream nodes\n"
              << "  switch <name|#|auto>   Manually switch active upstream proxy or revert to auto\n"
              << "  strategy <name>        Change pool strategy (failover | best_latency | round_robin)\n"
              << "  test <host> [port]     Probe network path, latency, and routing for a target endpoint\n\n"
              << "3. SYSTEM & RUNTIME CONTROLS:\n"
              << "  sysproxy <on|off>      Toggle Windows System Proxy (WinINet) for system-wide routing\n"
              << "  debug <on|off>         Toggle verbose debug logging in console\n"
              << "  reload                 Hot-reload configuration from config.json without restart\n"
              << "  clear / cls            Clear the terminal screen buffer\n"
              << "  help [cmd|all]         Display this index or full documentation for a specific command\n"
              << "  quit / exit            Stop proxy service, restore Windows settings, and exit\n\n"
              << "Tip: Type 'help <command>' (e.g. 'help switch', 'help rules', 'help test') for detailed docs.\n"
              << "     Type 'help all' to read the complete reference manual.\n\n";
}

void CommandLineInterface::print_command_doc(const std::string& raw_cmd) {
    std::string cmd = to_lower_str(raw_cmd);

    if (cmd == "status") {
        std::cout << "\n--- COMMAND DOCUMENTATION: status ---\n"
                  << "SYNTAX:        status\n"
                  << "DESCRIPTION:   Displays the complete operational status of the proxy server.\n"
                  << "OUTPUT FIELDS: - Service State: Whether the core listeners are RUNNING or STOPPED.\n"
                  << "               - Listen Address: IP and Port where the local server accepts connections.\n"
                  << "               - Local Mode: DUAL (HTTP + SOCKS5), SOCKS5, or HTTP.\n"
                  << "               - Local Auth: Local username/password authentication requirement.\n"
                  << "               - Smart Routing: Enabled state and default routing action (PROXY/DIRECT).\n"
                  << "               - Pool Strategy: Active upstream selection strategy (FAILOVER, BEST_LATENCY, etc.).\n"
                  << "               - Kill Switch: Whether non-proxied leak protection is active.\n"
                  << "               - System Proxy: Whether Windows WinINet system-wide routing is enabled.\n"
                  << "REST API:      GET /api/status\n\n";
    } else if (cmd == "stats") {
        std::cout << "\n--- COMMAND DOCUMENTATION: stats ---\n"
                  << "SYNTAX:        stats\n"
                  << "DESCRIPTION:   Displays real-time cumulative traffic and throughput metrics.\n"
                  << "OUTPUT FIELDS: - Active Connections: Number of client sockets currently open and relaying.\n"
                  << "               - Total Handled: Lifetime count of client connections processed.\n"
                  << "               - Transmitted (Tx): Total volume of data sent to upstream targets.\n"
                  << "               - Received (Rx): Total volume of data received from upstream targets.\n"
                  << "REST API:      GET /api/status (metrics section)\n\n";
    } else if (cmd == "conns" || cmd == "connections") {
        std::cout << "\n--- COMMAND DOCUMENTATION: conns ---\n"
                  << "SYNTAX:        conns\n"
                  << "ALIAS:         connections, list\n"
                  << "DESCRIPTION:   Renders an interactive tabular view of all currently active TCP/UDP\n"
                  << "               connections passing through the proxy pipeline.\n"
                  << "COLUMNS:       - ID: Unique monotonically increasing connection identifier.\n"
                  << "               - PROTO: Client protocol (HTTP CONNECT, HTTP Plain, SOCKS5 TCP, SOCKS5 UDP).\n"
                  << "               - CLIENT: Source IP address and ephemeral source port.\n"
                  << "               - TARGET: Destination hostname/IP and destination port.\n"
                  << "               - STAGE: Real-time pipeline stage (e.g. CONNECTING_UPSTREAM, ACTIVE_TUNNEL).\n"
                  << "               - DURATION: Total elapsed wall-clock time since socket acceptance.\n"
                  << "               - IDLE: Elapsed time since the last packet was relayed.\n"
                  << "               - TX / RX: Transferred payload volume for this specific session.\n\n";
    } else if (cmd == "nodes" || cmd == "pool") {
        std::cout << "\n--- COMMAND DOCUMENTATION: nodes ---\n"
                  << "SYNTAX:        nodes\n"
                  << "ALIAS:         pool\n"
                  << "DESCRIPTION:   Displays all upstream proxy servers configured in the proxy pool.\n"
                  << "COLUMNS:       - #: Numerical index (used as a shorthand for 'switch <#>' command).\n"
                  << "               - NAME: Custom human-readable node name specified in config.json.\n"
                  << "               - TYPE: Protocol type of the upstream proxy (SOCKS5 or HTTP).\n"
                  << "               - ENDPOINT: Remote host and port of the proxy.\n"
                  << "               - STATUS: Health check status (ONLINE if reachable, OFFLINE if unresponsive).\n"
                  << "               - LATENCY: Round-trip handshake ping in milliseconds measured by Health Check.\n"
                  << "               - FAILS: Consecutive connection failure counter (triggers auto-failover at 2).\n"
                  << "REST API:      GET /api/nodes\n\n";
    } else if (cmd == "rules") {
        std::cout << "\n--- COMMAND DOCUMENTATION: rules ---\n"
                  << "SYNTAX:        rules\n"
                  << "DESCRIPTION:   Lists all active Smart Routing traffic classification rules in evaluation order.\n"
                  << "RULE TYPES:    - DOMAIN-SUFFIX: Matches exact domain and all subdomains (e.g. 'google.com').\n"
                  << "               - DOMAIN-KEYWORD: Substring match anywhere in hostname (e.g. 'bank').\n"
                  << "               - DOMAIN: Strict exact match of hostname.\n"
                  << "               - IP-CIDR: IPv4 subnet range matching (e.g. '192.168.0.0/16', '10.0.0.0/8').\n"
                  << "               - FINAL: Default fallback match when no earlier rule matches.\n"
                  << "ACTIONS:       - DIRECT: Outbound TCP/UDP connection is made directly bypassing proxy.\n"
                  << "               - PROXY: Outbound traffic is forwarded through the upstream proxy pool.\n"
                  << "               - REJECT: Connection is immediately blocked and dropped.\n"
                  << "REST API:      GET /api/rules\n\n";
    } else if (cmd == "check") {
        std::cout << "\n--- COMMAND DOCUMENTATION: check ---\n"
                  << "SYNTAX:        check\n"
                  << "DESCRIPTION:   Immediately triggers an asynchronous health check and latency probe across\n"
                  << "               every node in the upstream proxy pool without waiting for the background timer.\n"
                  << "BEHAVIOR:      - Attempts TCP handshake and protocol handshake with each node.\n"
                  << "               - Updates 'LATENCY' and 'STATUS' (ONLINE/OFFLINE) columns in real time.\n"
                  << "               - Resets failure counters for nodes that successfully respond.\n"
                  << "REST API:      POST /api/check\n\n";
    } else if (cmd == "switch") {
        std::cout << "\n--- COMMAND DOCUMENTATION: switch ---\n"
                  << "SYNTAX:        switch <node_name | node_index | auto>\n"
                  << "DESCRIPTION:   Manually pins outgoing proxy traffic to a specific upstream node or returns\n"
                  << "               to automatic pool strategy (Failover / Best Latency / Round Robin).\n"
                  << "PARAMETERS:    - <node_name>: Exact name of the node (e.g. 'Canada-Montreal', 'US-East').\n"
                  << "               - <node_index>: Numerical index from 'nodes' table (e.g. 'switch 1').\n"
                  << "               - auto: Clears manual override and resumes automatic pool strategy.\n"
                  << "EXAMPLES:      switch Canada-Montreal   -> Locks outbound routing to Canada node\n"
                  << "               switch 2                 -> Switches to node #2 in the pool\n"
                  << "               switch auto              -> Restores automatic balancing\n"
                  << "REST API:      POST /api/switch (Body: {\"node\":\"Canada-Montreal\"} or {\"node\":\"auto\"})\n\n";
    } else if (cmd == "strategy") {
        std::cout << "\n--- COMMAND DOCUMENTATION: strategy ---\n"
                  << "SYNTAX:        strategy <failover | best_latency | round_robin>\n"
                  << "DESCRIPTION:   Sets the active routing strategy for selecting nodes from the proxy pool.\n"
                  << "STRATEGIES:    - failover: Always routes through the primary node; automatically shifts\n"
                  << "                 to the backup node if the primary fails 2 consecutive connections.\n"
                  << "               - best_latency: Dynamically routes connections through the proxy node with\n"
                  << "                 the lowest measured ping latency based on periodic Health Checks.\n"
                  << "               - round_robin: Cycles through healthy nodes sequentially for each new request.\n"
                  << "EXAMPLES:      strategy best_latency    -> Prioritizes fastest available proxy\n"
                  << "               strategy failover        -> Strict primary/secondary redundancy\n"
                  << "REST API:      POST /api/strategy (Body: {\"strategy\":\"best_latency\"})\n\n";
    } else if (cmd == "test") {
        std::cout << "\n--- COMMAND DOCUMENTATION: test ---\n"
                  << "SYNTAX:        test <host> [port]\n"
                  << "DESCRIPTION:   Performs an active network route and latency probe to a target endpoint\n"
                  << "               through the full Smart Routing and Proxy Pool pipeline.\n"
                  << "PARAMETERS:    - <host>: Domain name or IP address to probe (e.g. 'google.com', '1.1.1.1').\n"
                  << "               - [port]: TCP port (optional, defaults to 443 if omitted).\n"
                  << "OUTPUT:        Reports whether connection succeeded or failed, the matched routing rule,\n"
                  << "               upstream node used, and exact connection duration in milliseconds.\n"
                  << "EXAMPLES:      test google.com 443\n"
                  << "               test 1.1.1.1 53\n"
                  << "               test internal.corp.net 8080\n\n";
    } else if (cmd == "sysproxy") {
        std::cout << "\n--- COMMAND DOCUMENTATION: sysproxy ---\n"
                  << "SYNTAX:        sysproxy <on | off>\n"
                  << "DESCRIPTION:   Controls the global Windows Internet Settings (WinINet) system proxy.\n"
                  << "BEHAVIOR:      - 'sysproxy on': Writes proxy settings to HKCU\\Software\\Microsoft\\Windows\\\n"
                  << "                 CurrentVersion\\Internet Settings and broadcasts INTERNET_OPTION_SETTINGS_CHANGED.\n"
                  << "                 All Windows web browsers (Chrome, Edge, Firefox), system apps, and curl\n"
                  << "                 immediately route their traffic through ProxyClient without rebooting.\n"
                  << "               - 'sysproxy off': Disables system proxy and restores direct network routing.\n"
                  << "SAFETY:        Original Windows proxy settings are backed up on startup and automatically\n"
                  << "               restored when ProxyClient terminates or receives Ctrl+C.\n"
                  << "REST API:      POST /api/sysproxy (Body: {\"enable\":true} or {\"enable\":false})\n\n";
    } else if (cmd == "debug") {
        std::cout << "\n--- COMMAND DOCUMENTATION: debug ---\n"
                  << "SYNTAX:        debug <on | off>\n"
                  << "DESCRIPTION:   Controls verbose packet-level and diagnostic logging in the console.\n"
                  << "BEHAVIOR:      - 'debug on': Displays detailed internal logs, health check probes,\n"
                  << "                 SOCKS5 handshake byte exchanges, and fine-grained session durations.\n"
                  << "               - 'debug off': Filters out debug entries and shows only essential\n"
                  << "                 connection lifecycle and error events.\n\n";
    } else if (cmd == "reload") {
        std::cout << "\n--- COMMAND DOCUMENTATION: reload ---\n"
                  << "SYNTAX:        reload\n"
                  << "DESCRIPTION:   Hot-reloads configuration from config.json without restarting the process.\n"
                  << "UPDATES:       - Re-reads proxy pool nodes, credentials, and endpoints.\n"
                  << "               - Re-loads and compiles Smart Routing rules.\n"
                  << "               - Updates pool balancing strategy and timeout thresholds.\n"
                  << "               - Active open connections continue running without interruption.\n\n";
    } else if (cmd == "clear" || cmd == "cls") {
        std::cout << "\n--- COMMAND DOCUMENTATION: clear ---\n"
                  << "SYNTAX:        clear\n"
                  << "ALIAS:         cls\n"
                  << "DESCRIPTION:   Clears the terminal screen buffer and re-displays the startup banner.\n\n";
    } else if (cmd == "quit" || cmd == "exit") {
        std::cout << "\n--- COMMAND DOCUMENTATION: quit ---\n"
                  << "SYNTAX:        quit\n"
                  << "ALIAS:         exit\n"
                  << "DESCRIPTION:   Gracefully shuts down the ProxyClient service.\n"
                  << "SHUTDOWN STEPS:- Restores original Windows System Proxy settings in registry.\n"
                  << "               - Stops background Health Checker thread.\n"
                  << "               - Terminates REST API server.\n"
                  << "               - Closes local listening sockets and shuts down active worker threads.\n\n";
    } else {
        std::cout << "\nUnknown command: '" << raw_cmd << "'. Type 'help' for a full list of available commands.\n\n";
    }
}

void CommandLineInterface::print_all_docs() {
    std::cout << "\n================================================================================\n"
              << "                   PROXYCLIENT FULL REFERENCE MANUAL                            \n"
              << "================================================================================\n";
    print_command_doc("status");
    print_command_doc("stats");
    print_command_doc("conns");
    print_command_doc("nodes");
    print_command_doc("rules");
    print_command_doc("check");
    print_command_doc("switch");
    print_command_doc("strategy");
    print_command_doc("test");
    print_command_doc("sysproxy");
    print_command_doc("debug");
    print_command_doc("reload");
    print_command_doc("clear");
    print_command_doc("quit");
    std::cout << "================================================================================\n"
              << "                        END OF REFERENCE MANUAL                                 \n"
              << "================================================================================\n\n";
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

void CommandLineInterface::handle_command(const std::string& raw_line) {
    std::string line = raw_line;
    // Strip UTF-8 BOM if present
    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
        line = line.substr(3);
    }
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd.empty()) return;

    if (cmd == "help" || cmd == "?") {
        std::string sub;
        iss >> sub;
        print_help(sub);
    } else if (cmd == "status") {
        print_status();
    } else if (cmd == "stats") {
        print_stats();
    } else if (cmd == "conns" || cmd == "connections" || cmd == "list") {
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
                std::cout << "Node '" << target << "' not found in pool. Type 'nodes' to list available nodes.\n";
            }
        } else {
            std::cout << "Usage: switch <node_name | node_number | auto>\n"
                      << "Type 'help switch' for detailed documentation.\n";
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
            std::cout << "Usage: strategy <failover|best_latency|round_robin>\n"
                      << "Type 'help strategy' for detailed documentation.\n";
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
            std::cout << "Usage: sysproxy on|off\n"
                      << "Type 'help sysproxy' for detailed documentation.\n";
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
            std::cout << "Usage: debug on|off\n"
                      << "Type 'help debug' for detailed documentation.\n";
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
