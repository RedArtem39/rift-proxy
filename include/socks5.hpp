#pragma once

#include "config.hpp"
#include "router.hpp"
#include "upstream_pool.hpp"
#include <winsock2.h>
#include <string>
#include <cstdint>

namespace proxy {

class Socks5Handler {
public:
    static bool handle_client(
        uint64_t conn_id,
        SOCKET client_sock,
        const std::string& client_ip,
        const Config& cfg,
        const Router& router,
        UpstreamPool& pool,
        const std::string& initial_data = ""
    );

private:
    static bool authenticate(uint64_t conn_id, SOCKET client_sock, const Config& cfg);
};

} // namespace proxy
