#include "config.hpp"
#include "json.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>

static std::string protocol_mode_to_string(ProtocolMode mode) {
    switch (mode) {
        case ProtocolMode::SOCKS5: return "socks5";
        case ProtocolMode::HTTP: return "http";
        case ProtocolMode::DUAL:
        default: return "dual";
    }
}

static ProtocolMode string_to_protocol_mode(const std::string& str) {
    if (str == "socks5") return ProtocolMode::SOCKS5;
    if (str == "http") return ProtocolMode::HTTP;
    return ProtocolMode::DUAL;
}

static std::string upstream_type_to_string(UpstreamType type) {
    switch (type) {
        case UpstreamType::SOCKS5: return "socks5";
        case UpstreamType::HTTP: return "http";
        case UpstreamType::DIRECT:
        default: return "direct";
    }
}

static UpstreamType string_to_upstream_type(const std::string& str) {
    if (str == "socks5") return UpstreamType::SOCKS5;
    if (str == "http") return UpstreamType::HTTP;
    return UpstreamType::DIRECT;
}

Config Config::from_json_string(const std::string& json_str) {
    Config cfg;
    try {
        json::Value root = json::Parser::parse(json_str);

        if (root.has_field("local")) {
            auto& local = root["local"];
            cfg.local_host = local["host"].as_string(cfg.local_host);
            cfg.local_port = local["port"].as_int(cfg.local_port);
            cfg.mode = string_to_protocol_mode(local["mode"].as_string("dual"));
        }

        if (root.has_field("auth")) {
            auto& auth = root["auth"];
            cfg.local_auth_enabled = auth["enabled"].as_bool(false);
            cfg.local_username = auth["username"].as_string("");
            cfg.local_password = auth["password"].as_string("");
        }

        if (root.has_field("upstream")) {
            auto& upstream = root["upstream"];
            cfg.upstream_type = string_to_upstream_type(upstream["type"].as_string("direct"));
            cfg.upstream_host = upstream["host"].as_string("");
            cfg.upstream_port = upstream["port"].as_int(0);
            cfg.upstream_username = upstream["username"].as_string("");
            cfg.upstream_password = upstream["password"].as_string("");
        }

        if (root.has_field("proxy_pool")) {
            auto& pool = root["proxy_pool"];
            cfg.pool_strategy = pool["strategy"].as_string("failover");
            cfg.health_check_interval = pool["health_check_interval"].as_int(30);

            if (pool.has_field("nodes")) {
                const auto& nodes_arr = pool["nodes"].as_array();
                cfg.upstream_nodes.clear();
                for (const auto& node_val : nodes_arr) {
                    UpstreamNodeConfig n;
                    n.name = node_val["name"].as_string("Node");
                    n.type = string_to_upstream_type(node_val["type"].as_string("socks5"));
                    n.host = node_val["host"].as_string("");
                    n.port = node_val["port"].as_int(0);
                    n.username = node_val["username"].as_string("");
                    n.password = node_val["password"].as_string("");
                    cfg.upstream_nodes.push_back(n);
                }
            }
        }

        if (root.has_field("routing")) {
            auto& routing = root["routing"];
            cfg.smart_routing_enabled = routing["enabled"].as_bool(true);
            cfg.default_route = routing["default"].as_string("PROXY");
            if (routing.has_field("rules")) {
                const auto& rules_arr = routing["rules"].as_array();
                cfg.rules.clear();
                for (const auto& r : rules_arr) {
                    cfg.rules.push_back(r.as_string());
                }
            }
        }

        if (root.has_field("security")) {
            auto& sec = root["security"];
            cfg.kill_switch = sec["kill_switch"].as_bool(false);
        }

        if (root.has_field("system_proxy")) {
            auto& sys = root["system_proxy"];
            cfg.enable_system_proxy = sys["enabled"].as_bool(false);
            cfg.system_proxy_bypass = sys["bypass"].as_string(cfg.system_proxy_bypass);
        }

        if (root.has_field("advanced")) {
            auto& adv = root["advanced"];
            cfg.buffer_size = adv["buffer_size"].as_int(65536);
            cfg.timeout_seconds = adv["timeout_seconds"].as_int(30);
            cfg.debug_log = adv["debug_log"].as_bool(false);
        }
    } catch (const std::exception& e) {
        Logger::error(std::string("Config parse error: ") + e.what());
    }
    return cfg;
}

std::string Config::to_json_string() const {
    json::Value root;
    root.type = json::Type::Object;

    json::Value local;
    local.type = json::Type::Object;
    local["host"] = local_host;
    local["port"] = local_port;
    local["mode"] = protocol_mode_to_string(mode);
    root["local"] = local;

    json::Value auth;
    auth.type = json::Type::Object;
    auth["enabled"] = local_auth_enabled;
    auth["username"] = local_username;
    auth["password"] = local_password;
    root["auth"] = auth;

    json::Value upstream;
    upstream.type = json::Type::Object;
    upstream["type"] = upstream_type_to_string(upstream_type);
    upstream["host"] = upstream_host;
    upstream["port"] = upstream_port;
    upstream["username"] = upstream_username;
    upstream["password"] = upstream_password;
    root["upstream"] = upstream;

    json::Value pool;
    pool.type = json::Type::Object;
    pool["strategy"] = pool_strategy;
    pool["health_check_interval"] = health_check_interval;

    json::Value nodes_arr;
    nodes_arr.type = json::Type::Array;
    for (const auto& n : upstream_nodes) {
        json::Value node_obj;
        node_obj.type = json::Type::Object;
        node_obj["name"] = n.name;
        node_obj["type"] = upstream_type_to_string(n.type);
        node_obj["host"] = n.host;
        node_obj["port"] = n.port;
        node_obj["username"] = n.username;
        node_obj["password"] = n.password;
        nodes_arr.arr_val.push_back(node_obj);
    }
    pool["nodes"] = nodes_arr;
    root["proxy_pool"] = pool;

    json::Value routing;
    routing.type = json::Type::Object;
    routing["enabled"] = smart_routing_enabled;
    routing["default"] = default_route;
    json::Value rules_arr;
    rules_arr.type = json::Type::Array;
    for (const auto& r : rules) {
        rules_arr.arr_val.push_back(json::Value(r));
    }
    routing["rules"] = rules_arr;
    root["routing"] = routing;

    json::Value sec;
    sec.type = json::Type::Object;
    sec["kill_switch"] = kill_switch;
    root["security"] = sec;

    json::Value sys;
    sys.type = json::Type::Object;
    sys["enabled"] = enable_system_proxy;
    sys["bypass"] = system_proxy_bypass;
    root["system_proxy"] = sys;

    json::Value adv;
    adv.type = json::Type::Object;
    adv["buffer_size"] = buffer_size;
    adv["timeout_seconds"] = timeout_seconds;
    adv["debug_log"] = debug_log;
    root["advanced"] = adv;

    return json::serialize(root, 0);
}

Config Config::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::warn("Config file not found at " + path + ", creating default config.");
        Config cfg;
        cfg.save_to_file(path);
        return cfg;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return from_json_string(buffer.str());
}

bool Config::save_to_file(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        Logger::error("Failed to open file for writing: " + path);
        return false;
    }
    file << to_json_string() << "\n";
    return true;
}
