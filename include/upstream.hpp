#pragma once

#include "config.hpp"
#include "router.hpp"
#include "upstream_pool.hpp"
#include <winsock2.h>
#include <string>
#include <cstdint>
#include <memory>

namespace proxy {

class UpstreamHandler {
public:
    static SOCKET connect_target(
        uint64_t conn_id,
        const Config& cfg,
        const Router& router,
        UpstreamPool& pool,
        const std::string& target_host,
        int target_port,
        std::string& error_msg,
        int64_t& elapsed_ms,
        std::string& route_info_out
    );

private:
    static SOCKET connect_via_node(
        uint64_t conn_id,
        const UpstreamNode& node,
        int timeout_sec,
        const std::string& target_host,
        int target_port,
        std::string& error_msg,
        int64_t& elapsed_ms
    );

    static SOCKET connect_via_socks5(
        uint64_t conn_id,
        const UpstreamNode& node,
        int timeout_sec,
        const std::string& target_host,
        int target_port,
        std::string& error_msg,
        int64_t& elapsed_ms
    );

    static SOCKET connect_via_http(
        uint64_t conn_id,
        const UpstreamNode& node,
        int timeout_sec,
        const std::string& target_host,
        int target_port,
        std::string& error_msg,
        int64_t& elapsed_ms
    );
};

} // namespace proxy
