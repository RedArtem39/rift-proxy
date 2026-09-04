#include "config.hpp"
#include "logger.hpp"
#include "socket_utils.hpp"
#include "system_proxy.hpp"
#include "tunnel.hpp"
#include "cli.hpp"
#include "api_server.hpp"
#include "win_service.hpp"
#include <windows.h>
#include <iostream>
#include <memory>

static std::unique_ptr<proxy::TunnelServer> g_server;
static std::unique_ptr<api::RestApiServer> g_api_server;

BOOL WINAPI ConsoleHandlerRoutine(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            Logger::info("Signal received. Restoring system network configuration and terminating...");
            sys::SystemProxyManager::restore();
            if (g_api_server) {
                g_api_server->stop();
            }
            if (g_server) {
                g_server->stop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

#include "oneshot_cli.hpp"

void print_usage(const char* prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " <command> [args...]\n"
              << "  " << prog << " [options]\n\n"
              << "CLI Commands (Direct Execution):\n"
              << "  status                     Display daemon status, mode, and traffic metrics\n"
              << "  nodes | pool               List all upstream nodes with latency and status\n"
              << "  rules                      Show smart routing rules and default routing policy\n"
              << "  switch <node|idx|auto>     Switch active upstream proxy node\n"
              << "  strategy <name>            Set pool strategy (failover | best_latency | round_robin)\n"
              << "  check                      Trigger immediate latency health check on all nodes\n"
              << "  sysproxy <on|off>          Enable or disable Windows System Proxy\n"
              << "  test [host] [port]         Test network connectivity to target endpoint\n"
              << "  run                        Start server daemon in interactive console mode\n"
              << "  help                       Show this help message with command references\n\n"
              << "Server Daemon Options:\n"
              << "  -c, --config <file>        Path to configuration JSON file (default: config.json)\n"
              << "  -p, --port <port>          Override local listening port (default: 1080)\n"
              << "  -m, --mode <dual|socks5|http> Set local proxy protocol mode (default: dual)\n"
              << "  -s, --sysproxy             Enable Windows System Proxy automatically on startup\n"
              << "  -d, --debug                Enable verbose debug logging\n"
              << "  --api-port <port>          Set REST API port (default: 9090, 0 to disable)\n"
              << "  -h, --help                 Show this help message\n\n"
              << "Windows Service Options:\n"
              << "  --install-service          Install as a Windows Background Service\n"
              << "  --uninstall-service        Uninstall Windows Service\n"
              << "  --start-service            Start installed Windows Service\n"
              << "  --stop-service             Stop running Windows Service\n\n"
              << "Examples:\n"
              << "  px status                  Check running proxy status and traffic\n"
              << "  px nodes                   List proxy pool with live latency\n"
              << "  px switch 2                Switch to node #2\n"
              << "  px sysproxy on             Turn on Windows system proxy\n"
              << "  px run -s -p 1080          Start server on port 1080 with system proxy\n\n";
}

int main(int argc, char* argv[]) {
    std::string config_path = "config.json";
    int override_port = 0;
    std::string override_mode;
    bool enable_sysproxy_flag = false;
    bool debug_flag = false;
    int api_port = 9090;

    // Check for direct subcommands
    if (argc > 1) {
        std::string first_arg = argv[1];
        if (first_arg == "help" || first_arg == "-h" || first_arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        // Subcommands list
        static const std::vector<std::string> subcommands = {
            "status", "nodes", "pool", "rules", "switch", "strategy", "check", "sysproxy", "test"
        };

        bool is_subcommand = false;
        for (const auto& sc : subcommands) {
            if (first_arg == sc) {
                is_subcommand = true;
                break;
            }
        }

        if (is_subcommand) {
            net::WinsockScope winsock_scope;
            std::vector<std::string> sub_args;
            for (int i = 2; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--api-port" && i + 1 < argc) {
                    api_port = std::stoi(argv[++i]);
                } else {
                    sub_args.push_back(a);
                }
            }
            return cli::OneShotClient::execute_subcommand(first_arg, sub_args, api_port);
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "run") {
            // Explicit run command - continue to server start
            continue;
        } else if (arg == "--install-service") {
            return service::WindowsServiceManager::install_service() ? 0 : 1;
        } else if (arg == "--uninstall-service") {
            return service::WindowsServiceManager::uninstall_service() ? 0 : 1;
        } else if (arg == "--start-service") {
            return service::WindowsServiceManager::start_service() ? 0 : 1;
        } else if (arg == "--stop-service") {
            return service::WindowsServiceManager::stop_service() ? 0 : 1;
        } else if (arg == "--service-run") {
            service::WindowsServiceManager::run_as_service(config_path);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) config_path = argv[++i];
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) override_port = std::stoi(argv[++i]);
        } else if (arg == "-m" || arg == "--mode") {
            if (i + 1 < argc) override_mode = argv[++i];
        } else if (arg == "-s" || arg == "--sysproxy") {
            enable_sysproxy_flag = true;
        } else if (arg == "-d" || arg == "--debug") {
            debug_flag = true;
        } else if (arg == "--api-port") {
            if (i + 1 < argc) api_port = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    Logger::init();
    net::WinsockScope winsock_scope;
    if (!winsock_scope.is_valid()) {
        return 1;
    }

    sys::SystemProxyManager::init();
    SetConsoleCtrlHandler(ConsoleHandlerRoutine, TRUE);

    Config cfg = Config::load_from_file(config_path);
    if (override_port > 0) cfg.local_port = override_port;
    if (!override_mode.empty()) {
        if (override_mode == "socks5") cfg.mode = ProtocolMode::SOCKS5;
        else if (override_mode == "http") cfg.mode = ProtocolMode::HTTP;
        else cfg.mode = ProtocolMode::DUAL;
    }
    if (enable_sysproxy_flag) cfg.enable_system_proxy = true;
    if (debug_flag) cfg.debug_log = true;

    g_server = std::make_unique<proxy::TunnelServer>(cfg);
    if (!g_server->start()) {
        Logger::error("Failed to start proxy service on specified address/port.");
        sys::SystemProxyManager::restore();
        return 1;
    }

    if (api_port > 0) {
        g_api_server = std::make_unique<api::RestApiServer>(*g_server, api_port);
        g_api_server->start();
    }

    cli::CommandLineInterface cli_app(*g_server, config_path);
    cli_app.run();

    if (g_api_server) {
        g_api_server->stop();
    }
    sys::SystemProxyManager::restore();
    return 0;
}
