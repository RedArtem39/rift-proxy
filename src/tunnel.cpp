#include "tunnel.hpp"
#include "socks5.hpp"
#include "http_proxy.hpp"
#include "socket_utils.hpp"
#include "system_proxy.hpp"
#include "logger.hpp"
#include <chrono>
#include <ws2tcpip.h>

namespace proxy {

TunnelServer::TunnelServer(Config cfg) : config(std::move(cfg)) {}

TunnelServer::~TunnelServer() {
    stop();
}

void TunnelServer::setup_pool() {
    std::vector<UpstreamNode> nodes;

    // Add nodes from proxy_pool list
    for (const auto& n : config.upstream_nodes) {
        UpstreamNode node;
        node.name = n.name;
        node.type = n.type;
        node.host = n.host;
        node.port = n.port;
        node.username = n.username;
        node.password = n.password;
        node.is_direct = false;
        nodes.push_back(node);
    }

    // If pool list is empty but single upstream is configured
    if (nodes.empty() && config.upstream_type != UpstreamType::DIRECT && !config.upstream_host.empty()) {
        UpstreamNode primary;
        primary.name = "Primary-Upstream";
        primary.type = config.upstream_type;
        primary.host = config.upstream_host;
        primary.port = config.upstream_port;
        primary.username = config.upstream_username;
        primary.password = config.upstream_password;
        primary.is_direct = false;
        nodes.push_back(primary);
    }

    PoolStrategy strat = PoolStrategy::FAILOVER;
    if (config.pool_strategy == "best_latency") strat = PoolStrategy::BEST_LATENCY;
    else if (config.pool_strategy == "round_robin") strat = PoolStrategy::ROUND_ROBIN;

    pool.init(nodes, strat, config.health_check_interval);

    // Setup router
    RouteAction def_act = (config.default_route == "DIRECT") ? RouteAction::DIRECT : RouteAction::PROXY;
    router.load_rules(config.rules, def_act);
}

bool TunnelServer::start() {
    if (running.load()) return true;

    Logger::set_debug(config.debug_log);
    setup_pool();

    listen_socket = net::create_listen_socket(config.local_host, config.local_port);
    if (listen_socket == INVALID_SOCKET) {
        Logger::error("Failed to bind and listen on " + config.local_host + ":" + std::to_string(config.local_port));
        return false;
    }

    running.store(true);

    if (config.enable_system_proxy) {
        sys::SystemProxyManager::enable(config.local_host, config.local_port, config.system_proxy_bypass);
    }

    pool.start_health_checker();
    accept_thread = std::thread(&TunnelServer::accept_loop, this);

    std::string mode_str = (config.mode == ProtocolMode::DUAL ? "DUAL (HTTP + SOCKS5)" : (config.mode == ProtocolMode::SOCKS5 ? "SOCKS5" : "HTTP"));
    Logger::info("Proxy server listening on " + config.local_host + ":" + std::to_string(config.local_port) + " [" + mode_str + "]");
    Logger::info("Routing policy: " + config.default_route + " (" + std::to_string(router.get_rules().size()) + " active rules)");
    Logger::info("Pool strategy: " + pool.get_active_strategy_name());

    return true;
}

void TunnelServer::stop() {
    if (!running.load()) return;
    running.store(false);

    pool.stop_health_checker();

    if (config.enable_system_proxy) {
        sys::SystemProxyManager::disable();
    }

    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
    }

    if (accept_thread.joinable()) {
        accept_thread.join();
    }

    Logger::info("Proxy server stopped.");
}

void TunnelServer::update_config(const Config& new_cfg) {
    config = new_cfg;
    Logger::set_debug(config.debug_log);
    setup_pool();
}

void TunnelServer::accept_loop() {
    while (running.load()) {
        WSAPOLLFD pfd{};
        pfd.fd = listen_socket;
        pfd.events = POLLIN;

        int ret = WSAPoll(&pfd, 1, 500);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            sockaddr_storage client_addr{};
            int addr_len = sizeof(client_addr);
            SOCKET client_sock = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
            if (client_sock != INVALID_SOCKET) {
                net::set_tcp_nodelay(client_sock, true);

                char client_ip_buf[INET6_ADDRSTRLEN] = "unknown";
                int client_port = 0;
                if (client_addr.ss_family == AF_INET) {
                    auto* s = reinterpret_cast<sockaddr_in*>(&client_addr);
                    inet_ntop(AF_INET, &s->sin_addr, client_ip_buf, sizeof(client_ip_buf));
                    client_port = ntohs(s->sin_port);
                } else if (client_addr.ss_family == AF_INET6) {
                    auto* s = reinterpret_cast<sockaddr_in6*>(&client_addr);
                    inet_ntop(AF_INET6, &s->sin6_addr, client_ip_buf, sizeof(client_ip_buf));
                    client_port = ntohs(s->sin6_port);
                }
                std::string client_str = std::string(client_ip_buf) + ":" + std::to_string(client_port);

                std::thread([this, client_sock, client_str]() {
                    this->process_client(client_sock, client_str);
                }).detach();
            }
        }
    }
}

void TunnelServer::process_client(SOCKET client_sock, const std::string& client_ip) {
    if (config.mode == ProtocolMode::SOCKS5) {
        uint64_t cid = net::ConnectionTracker::register_conn("SOCKS5", client_ip, "pending");
        Socks5Handler::handle_client(cid, client_sock, client_ip, config, router, pool);
        net::ConnectionTracker::unregister_conn(cid);
    } else if (config.mode == ProtocolMode::HTTP) {
        uint64_t cid = net::ConnectionTracker::register_conn("HTTP", client_ip, "pending");
        HttpProxyHandler::handle_client(cid, client_sock, client_ip, config, router, pool);
        net::ConnectionTracker::unregister_conn(cid);
    } else {
        char peek_buf[4] = {0};
        int bytes_peeked = recv(client_sock, peek_buf, sizeof(peek_buf), MSG_PEEK);
        if (bytes_peeked <= 0) {
            net::close_socket(client_sock);
            return;
        }

        if (peek_buf[0] == 0x05) {
            uint64_t cid = net::ConnectionTracker::register_conn("SOCKS5", client_ip, "pending");
            Socks5Handler::handle_client(cid, client_sock, client_ip, config, router, pool);
            net::ConnectionTracker::unregister_conn(cid);
        } else {
            uint64_t cid = net::ConnectionTracker::register_conn("HTTP", client_ip, "pending");
            HttpProxyHandler::handle_client(cid, client_sock, client_ip, config, router, pool);
            net::ConnectionTracker::unregister_conn(cid);
        }
    }

    net::close_socket(client_sock);
}

} // namespace proxy
