#pragma once

#include "config.hpp"
#include "router.hpp"
#include "upstream_pool.hpp"
#include <winsock2.h>
#include <string>
#include <cstdint>

namespace proxy {

class HttpProxyHandler {
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
    static bool check_auth(const std::string& headers, const Config& cfg);
    static bool parse_request_line(const std::string& line, std::string& method, std::string& target_host, int& target_port, std::string& path_or_url);
};

} // namespace proxy
