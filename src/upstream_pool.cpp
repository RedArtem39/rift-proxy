#include "upstream_pool.hpp"
#include "socket_utils.hpp"
#include "logger.hpp"
#include <algorithm>
#include <iostream>

UpstreamPool::~UpstreamPool() {
    stop_health_checker();
}

void UpstreamPool::init(const std::vector<UpstreamNode>& initial_nodes, PoolStrategy strat, int health_check_interval_sec) {
    std::lock_guard<std::mutex> lock(pool_mutex);
    nodes.clear();
    strategy = strat;
    check_interval = (health_check_interval_sec > 0) ? health_check_interval_sec : 30;

    for (const auto& n : initial_nodes) {
        auto ptr = std::make_shared<UpstreamNode>();
        ptr->name = n.name;
        ptr->type = n.type;
        ptr->host = n.host;
        ptr->port = n.port;
        ptr->username = n.username;
        ptr->password = n.password;
        ptr->is_direct = n.is_direct;
        ptr->alive.store(true);
        ptr->latency_ms.store(0);
        ptr->failure_count.store(0);
        ptr->last_check = std::chrono::system_clock::now();
        nodes.push_back(ptr);
    }
}

int64_t UpstreamPool::probe_node(const UpstreamNode& node, int timeout_sec) {
    if (node.is_direct) {
        return 0; // Direct connection always healthy
    }

    auto start_tp = std::chrono::steady_clock::now();
    std::string diag;
    int64_t connect_ms = 0;
    SOCKET s = net::connect_to_host(node.host, node.port, timeout_sec, diag, connect_ms);
    if (s == INVALID_SOCKET) {
        return -1;
    }

    if (node.type == UpstreamType::SOCKS5) {
        std::vector<char> hs = { 0x05, 0x01, 0x00 };
        if (net::send_all(s, hs.data(), static_cast<int>(hs.size())) != (int)hs.size()) {
            net::close_socket(s);
            return -1;
        }

        char rep[2];
        if (net::recv_with_timeout(s, rep, 2, timeout_sec) != 2 || rep[0] != 0x05) {
            net::close_socket(s);
            return -1;
        }
    }

    net::close_socket(s);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();
    return elapsed;
}

void UpstreamPool::run_health_check_all() {
    std::vector<std::shared_ptr<UpstreamNode>> current_nodes;
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        current_nodes = nodes;
    }

    for (auto& n : current_nodes) {
        int64_t lat = probe_node(*n, 5);
        n->last_check = std::chrono::system_clock::now();
        if (lat >= 0) {
            n->alive.store(true);
            n->latency_ms.store(lat);
            n->failure_count.store(0);
            Logger::debug("[HEALTH-CHECK] Node '" + n->name + "' ONLINE (Latency: " + std::to_string(lat) + "ms)");
        } else {
            n->alive.store(false);
            n->latency_ms.store(-1);
            n->failure_count++;
            Logger::warn("[HEALTH-CHECK] Node '" + n->name + "' (" + n->host + ":" + std::to_string(n->port) + ") OFFLINE / UNRESPONSIVE");
        }
    }
}

void UpstreamPool::start_health_checker() {
    if (checker_running.load()) return;
    checker_running.store(true);

    checker_thread = std::thread([this]() {
        // Run initial check
        this->run_health_check_all();

        while (this->checker_running.load()) {
            for (int i = 0; i < this->check_interval && this->checker_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (this->checker_running.load()) {
                this->run_health_check_all();
            }
        }
    });
}

void UpstreamPool::stop_health_checker() {
    if (!checker_running.load()) return;
    checker_running.store(false);
    if (checker_thread.joinable()) {
        checker_thread.join();
    }
}

std::shared_ptr<UpstreamNode> UpstreamPool::select_node() {
    std::lock_guard<std::mutex> lock(pool_mutex);
    if (nodes.empty()) return nullptr;

    // Manual override check
    if (!manual_override_name.empty()) {
        for (auto& n : nodes) {
            if (n->name == manual_override_name && n->alive.load()) {
                return n;
            }
        }
    }

    if (strategy == PoolStrategy::BEST_LATENCY) {
        std::shared_ptr<UpstreamNode> best_node = nullptr;
        int64_t best_lat = 999999999;
        for (auto& n : nodes) {
            if (n->alive.load()) {
                int64_t lat = n->latency_ms.load();
                if (lat >= 0 && lat < best_lat) {
                    best_lat = lat;
                    best_node = n;
                }
            }
        }
        if (best_node) return best_node;
    } else if (strategy == PoolStrategy::ROUND_ROBIN) {
        size_t total = nodes.size();
        for (size_t i = 0; i < total; ++i) {
            size_t idx = (round_robin_idx++) % total;
            if (nodes[idx]->alive.load()) {
                return nodes[idx];
            }
        }
    }

    // Default: FAILOVER (first alive node)
    for (auto& n : nodes) {
        if (n->alive.load()) {
            return n;
        }
    }

    // Fallback to first node if none reported alive
    return nodes.front();
}

void UpstreamPool::mark_node_failure(const std::string& name) {
    std::lock_guard<std::mutex> lock(pool_mutex);
    for (auto& n : nodes) {
        if (n->name == name) {
            int failures = ++n->failure_count;
            if (failures >= 2) {
                n->alive.store(false);
                Logger::warn("[FAILOVER] Node '" + name + "' failed 2 consecutive attempts. Marking OFFLINE.");
            }
            break;
        }
    }
}

void UpstreamPool::mark_node_success(const std::string& name, int64_t latency) {
    std::lock_guard<std::mutex> lock(pool_mutex);
    for (auto& n : nodes) {
        if (n->name == name) {
            n->alive.store(true);
            n->failure_count.store(0);
            n->latency_ms.store(latency);
            break;
        }
    }
}

std::vector<UpstreamNode> UpstreamPool::get_snapshot() {
    std::lock_guard<std::mutex> lock(pool_mutex);
    std::vector<UpstreamNode> list;
    for (const auto& n : nodes) {
        UpstreamNode item;
        item.name = n->name;
        item.type = n->type;
        item.host = n->host;
        item.port = n->port;
        item.username = n->username;
        item.password = n->password;
        item.is_direct = n->is_direct;
        item.alive.store(n->alive.load());
        item.latency_ms.store(n->latency_ms.load());
        item.failure_count.store(n->failure_count.load());
        item.last_check = n->last_check;
        list.push_back(item);
    }
    return list;
}

bool UpstreamPool::manual_select(const std::string& name_or_index) {
    std::lock_guard<std::mutex> lock(pool_mutex);
    if (name_or_index == "auto" || name_or_index == "default") {
        manual_override_name.clear();
        return true;
    }

    // Check if numeric index
    try {
        size_t idx = std::stoul(name_or_index);
        if (idx >= 1 && idx <= nodes.size()) {
            manual_override_name = nodes[idx - 1]->name;
            return true;
        }
    } catch (...) {}

    for (const auto& n : nodes) {
        if (n->name == name_or_index) {
            manual_override_name = n->name;
            return true;
        }
    }
    return false;
}

std::string UpstreamPool::get_active_strategy_name() const {
    if (!manual_override_name.empty()) return "MANUAL (" + manual_override_name + ")";
    switch (strategy) {
        case PoolStrategy::BEST_LATENCY: return "BEST_LATENCY";
        case PoolStrategy::ROUND_ROBIN: return "ROUND_ROBIN";
        case PoolStrategy::FAILOVER:
        default: return "FAILOVER";
    }
}

void UpstreamPool::set_strategy(PoolStrategy strat) {
    std::lock_guard<std::mutex> lock(pool_mutex);
    strategy = strat;
    manual_override_name.clear();
}
