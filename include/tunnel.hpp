#pragma once

#include "config.hpp"
#include "router.hpp"
#include "upstream_pool.hpp"
#include <winsock2.h>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>

namespace proxy {

class TunnelServer {
public:
    explicit TunnelServer(Config cfg);
    ~TunnelServer();

    bool start();
    void stop();
    bool is_running() const { return running.load(); }
    const Config& get_config() const { return config; }
    void update_config(const Config& new_cfg);

    UpstreamPool& get_pool() { return pool; }
    const Router& get_router() const { return router; }

private:
    Config config;
    Router router;
    UpstreamPool pool;

    std::atomic<bool> running{false};
    SOCKET listen_socket = INVALID_SOCKET;
    std::thread accept_thread;

    void accept_loop();
    void process_client(SOCKET client_sock, const std::string& client_ip);
    void setup_pool();
};

} // namespace proxy
