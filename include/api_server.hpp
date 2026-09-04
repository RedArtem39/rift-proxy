#pragma once

#include "tunnel.hpp"
#include <winsock2.h>
#include <string>
#include <thread>
#include <atomic>

namespace api {

class RestApiServer {
public:
    explicit RestApiServer(proxy::TunnelServer& server, int port = 9090);
    ~RestApiServer();

    bool start();
    void stop();
    bool is_running() const { return running.load(); }
    int get_port() const { return port; }

private:
    proxy::TunnelServer& server;
    int port = 9090;
    std::atomic<bool> running{false};
    SOCKET listen_sock = INVALID_SOCKET;
    std::thread server_thread;

    void listen_loop();
    void handle_http_request(SOCKET client_sock);
    
    std::string handle_get_status();
    std::string handle_get_nodes();
    std::string handle_get_rules();
    std::string handle_post_switch(const std::string& body);
    std::string handle_post_sysproxy(const std::string& body);
    std::string handle_post_strategy(const std::string& body);
};

} // namespace api
