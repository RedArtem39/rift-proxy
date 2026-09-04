#pragma once

#include <string>
#include <vector>

enum class ProtocolMode {
    DUAL,
    SOCKS5,
    HTTP
};

enum class UpstreamType {
    DIRECT,
    SOCKS5,
    HTTP
};

struct UpstreamNodeConfig {
    std::string name;
    UpstreamType type = UpstreamType::SOCKS5;
    std::string host;
    int port = 0;
    std::string username;
    std::string password;
};

struct Config {
    std::string local_host = "127.0.0.1";
    int local_port = 1080;
    ProtocolMode mode = ProtocolMode::DUAL;

    bool local_auth_enabled = false;
    std::string local_username = "";
    std::string local_password = "";

    // Upstream proxy settings
    UpstreamType upstream_type = UpstreamType::DIRECT;
    std::string upstream_host = "";
    int upstream_port = 0;
    std::string upstream_username = "";
    std::string upstream_password = "";

    // Proxy Pool & Health Checker
    std::vector<UpstreamNodeConfig> upstream_nodes;
    std::string pool_strategy = "failover"; // "failover", "best_latency", "round_robin"
    int health_check_interval = 30;

    // Smart Routing Rules
    bool smart_routing_enabled = true;
    std::string default_route = "PROXY"; // "PROXY", "DIRECT", "REJECT"
    std::vector<std::string> rules;

    // Kill switch (prevents real IP leak if proxy drops)
    bool kill_switch = false;

    // Windows System Proxy
    bool enable_system_proxy = false;
    std::string system_proxy_bypass = "localhost;127.*;10.*;192.168.*;<local>";

    int buffer_size = 65536;
    int timeout_seconds = 30;
    bool debug_log = false;

    static Config load_from_file(const std::string& path);
    bool save_to_file(const std::string& path) const;
    std::string to_json_string() const;
    static Config from_json_string(const std::string& json_str);
};
