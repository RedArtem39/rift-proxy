#include "api_server.hpp"
#include "socket_utils.hpp"
#include "system_proxy.hpp"
#include "json.hpp"
#include "logger.hpp"
#include <sstream>
#include <algorithm>

namespace api {

static std::string build_http_response(int status_code, const std::string& status_text, const std::string& json_body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
       << "Content-Type: application/json; charset=utf-8\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type\r\n"
       << "Content-Length: " << json_body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << json_body;
    return ss.str();
}

RestApiServer::RestApiServer(proxy::TunnelServer& srv, int p) : server(srv), port(p) {}

RestApiServer::~RestApiServer() {
    stop();
}

bool RestApiServer::start() {
    if (running.load()) return true;

    listen_sock = net::create_listen_socket("127.0.0.1", port);
    if (listen_sock == INVALID_SOCKET) {
        Logger::warn("REST API server failed to bind on 127.0.0.1:" + std::to_string(port));
        return false;
    }

    running.store(true);
    server_thread = std::thread(&RestApiServer::listen_loop, this);
    Logger::info("REST API controller listening on http://127.0.0.1:" + std::to_string(port) + "/api");
    return true;
}

void RestApiServer::stop() {
    if (!running.load()) return;
    running.store(false);

    if (listen_sock != INVALID_SOCKET) {
        closesocket(listen_sock);
        listen_sock = INVALID_SOCKET;
    }

    if (server_thread.joinable()) {
        server_thread.join();
    }
}

void RestApiServer::listen_loop() {
    while (running.load()) {
        WSAPOLLFD pfd{};
        pfd.fd = listen_sock;
        pfd.events = POLLIN;

        int ret = WSAPoll(&pfd, 1, 500);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            sockaddr_storage client_addr{};
            int addr_len = sizeof(client_addr);
            SOCKET client = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
            if (client != INVALID_SOCKET) {
                std::thread([this, client]() {
                    this->handle_http_request(client);
                }).detach();
            }
        }
    }
}

void RestApiServer::handle_http_request(SOCKET client_sock) {
    std::string req;
    char buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        int n = net::recv_with_timeout(client_sock, buf, sizeof(buf), 5);
        if (n <= 0) break;
        req.append(buf, n);
    }

    size_t header_end = req.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        net::close_socket(client_sock);
        return;
    }

    std::string header_part = req.substr(0, header_end);
    std::string body = req.substr(header_end + 4);

    // Check Content-Length to read remaining body if needed
    std::string lower_headers = header_part;
    std::transform(lower_headers.begin(), lower_headers.end(), lower_headers.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    size_t cl_pos = lower_headers.find("content-length: ");
    if (cl_pos != std::string::npos) {
        size_t cl_end = lower_headers.find("\r\n", cl_pos);
        std::string cl_str = header_part.substr(cl_pos + 16, cl_end - (cl_pos + 16));
        try {
            size_t content_length = std::stoul(cl_str);
            while (body.size() < content_length) {
                int n = net::recv_with_timeout(client_sock, buf, sizeof(buf), 2);
                if (n <= 0) break;
                body.append(buf, n);
            }
        } catch (...) {}
    }

    std::istringstream iss(header_part);
    std::string method, path, version;
    iss >> method >> path >> version;

    if (method == "OPTIONS") {
        std::string res = build_http_response(204, "No Content", "");
        net::send_all(client_sock, res.c_str(), (int)res.size());
        net::close_socket(client_sock);
        return;
    }

    std::string resp_json;
    int code = 200;
    std::string code_str = "OK";

    if (method == "GET" && (path == "/api/status" || path == "/api" || path == "/")) {
        resp_json = handle_get_status();
    } else if (method == "GET" && path == "/api/nodes") {
        resp_json = handle_get_nodes();
    } else if (method == "GET" && path == "/api/rules") {
        resp_json = handle_get_rules();
    } else if (method == "POST" && path == "/api/switch") {
        resp_json = handle_post_switch(body);
    } else if (method == "POST" && path == "/api/sysproxy") {
        resp_json = handle_post_sysproxy(body);
    } else if (method == "POST" && path == "/api/strategy") {
        resp_json = handle_post_strategy(body);
    } else if (method == "POST" && path == "/api/check") {
        server.get_pool().run_health_check_all();
        resp_json = handle_get_nodes();
    } else {
        code = 404;
        code_str = "Not Found";
        resp_json = "{\"error\":\"Endpoint not found\",\"path\":\"" + path + "\"}";
    }

    std::string http_res = build_http_response(code, code_str, resp_json);
    net::send_all(client_sock, http_res.c_str(), (int)http_res.size());
    net::close_socket(client_sock);
}

std::string RestApiServer::handle_get_status() {
    const auto& cfg = server.get_config();
    json::Value root;
    root.type = json::Type::Object;
    root["running"] = server.is_running();
    root["local_host"] = cfg.local_host;
    root["local_port"] = cfg.local_port;
    root["mode"] = (cfg.mode == ProtocolMode::DUAL ? "dual" : (cfg.mode == ProtocolMode::SOCKS5 ? "socks5" : "http"));
    root["smart_routing"] = cfg.smart_routing_enabled;
    root["default_route"] = cfg.default_route;
    root["active_strategy"] = server.get_pool().get_active_strategy_name();
    root["system_proxy_enabled"] = sys::SystemProxyManager::is_enabled();
    root["kill_switch"] = cfg.kill_switch;

    json::Value metrics;
    metrics.type = json::Type::Object;
    metrics["active_connections"] = static_cast<int64_t>(net::Stats::active_connections.load());
    metrics["total_connections"] = static_cast<int64_t>(net::Stats::total_connections.load());
    metrics["bytes_sent"] = static_cast<int64_t>(net::Stats::total_bytes_sent.load());
    metrics["bytes_received"] = static_cast<int64_t>(net::Stats::total_bytes_received.load());
    root["metrics"] = metrics;

    return json::serialize(root, 0);
}

std::string RestApiServer::handle_get_nodes() {
    auto nodes = server.get_pool().get_snapshot();
    json::Value root;
    root.type = json::Type::Object;
    root["strategy"] = server.get_pool().get_active_strategy_name();
    
    json::Value arr;
    arr.type = json::Type::Array;
    for (const auto& n : nodes) {
        json::Value obj;
        obj.type = json::Type::Object;
        obj["name"] = n.name;
        obj["type"] = (n.type == UpstreamType::SOCKS5 ? "socks5" : "http");
        obj["endpoint"] = n.is_direct ? "direct" : (n.host + ":" + std::to_string(n.port));
        obj["alive"] = n.alive.load();
        obj["latency_ms"] = static_cast<int64_t>(n.latency_ms.load());
        obj["failure_count"] = n.failure_count.load();
        arr.arr_val.push_back(obj);
    }
    root["nodes"] = arr;
    return json::serialize(root, 0);
}

std::string RestApiServer::handle_get_rules() {
    const auto& rules = server.get_router().get_rules();
    json::Value root;
    root.type = json::Type::Object;
    root["default_action"] = server.get_config().default_route;

    json::Value arr;
    arr.type = json::Type::Array;
    for (const auto& r : rules) {
        json::Value obj;
        obj.type = json::Type::Object;
        std::string type_str;
        switch (r.type) {
            case RuleType::DOMAIN_SUFFIX: type_str = "DOMAIN-SUFFIX"; break;
            case RuleType::DOMAIN_KEYWORD: type_str = "DOMAIN-KEYWORD"; break;
            case RuleType::DOMAIN_EXACT: type_str = "DOMAIN"; break;
            case RuleType::IP_CIDR: type_str = "IP-CIDR"; break;
            case RuleType::FINAL_RULE: type_str = "FINAL"; break;
        }
        obj["type"] = type_str;
        obj["pattern"] = r.pattern;
        obj["action"] = (r.action == RouteAction::DIRECT ? "DIRECT" : (r.action == RouteAction::REJECT ? "REJECT" : "PROXY"));
        arr.arr_val.push_back(obj);
    }
    root["rules"] = arr;
    return json::serialize(root, 0);
}

std::string RestApiServer::handle_post_switch(const std::string& body) {
    json::Value root;
    root.type = json::Type::Object;
    try {
        auto parsed = json::Parser::parse(body);
        std::string target = parsed["node"].as_string("");
        if (target.empty()) {
            root["success"] = false;
            root["message"] = "Field 'node' is required (e.g. {\"node\":\"Canada-Montreal\"} or {\"node\":\"auto\"})";
        } else {
            bool ok = server.get_pool().manual_select(target);
            root["success"] = ok;
            root["active_strategy"] = server.get_pool().get_active_strategy_name();
            root["message"] = ok ? ("Switched active node to: " + server.get_pool().get_active_strategy_name()) : ("Node '" + target + "' not found");
        }
    } catch (const std::exception& e) {
        root["success"] = false;
        root["message"] = std::string("JSON parse error: ") + e.what();
    }
    return json::serialize(root, 0);
}

std::string RestApiServer::handle_post_sysproxy(const std::string& body) {
    json::Value root;
    root.type = json::Type::Object;
    try {
        auto parsed = json::Parser::parse(body);
        bool enable = parsed["enable"].as_bool(false);
        const auto& cfg = server.get_config();
        if (enable) {
            sys::SystemProxyManager::enable(cfg.local_host, cfg.local_port, cfg.system_proxy_bypass);
            root["success"] = true;
            root["system_proxy_enabled"] = true;
        } else {
            sys::SystemProxyManager::disable();
            root["success"] = true;
            root["system_proxy_enabled"] = false;
        }
    } catch (const std::exception& e) {
        root["success"] = false;
        root["message"] = std::string("JSON parse error: ") + e.what();
    }
    return json::serialize(root, 0);
}

std::string RestApiServer::handle_post_strategy(const std::string& body) {
    json::Value root;
    root.type = json::Type::Object;
    try {
        auto parsed = json::Parser::parse(body);
        std::string strat = parsed["strategy"].as_string("");
        if (strat == "failover") {
            server.get_pool().set_strategy(PoolStrategy::FAILOVER);
            root["success"] = true;
            root["strategy"] = "FAILOVER";
        } else if (strat == "best_latency") {
            server.get_pool().set_strategy(PoolStrategy::BEST_LATENCY);
            root["success"] = true;
            root["strategy"] = "BEST_LATENCY";
        } else if (strat == "round_robin") {
            server.get_pool().set_strategy(PoolStrategy::ROUND_ROBIN);
            root["success"] = true;
            root["strategy"] = "ROUND_ROBIN";
        } else {
            root["success"] = false;
            root["message"] = "Invalid strategy. Available: failover, best_latency, round_robin";
        }
    } catch (const std::exception& e) {
        root["success"] = false;
        root["message"] = std::string("JSON parse error: ") + e.what();
    }
    return json::serialize(root, 0);
}

} // namespace api
