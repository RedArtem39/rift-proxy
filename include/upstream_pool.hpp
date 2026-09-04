#pragma once

#include "config.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

struct UpstreamNode {
    std::string name;
    UpstreamType type = UpstreamType::SOCKS5;
    std::string host;
    int port = 0;
    std::string username;
    std::string password;

    bool is_direct = false;
    std::atomic<bool> alive{true};
    std::atomic<int64_t> latency_ms{0};
    std::atomic<int> failure_count{0};
    std::chrono::system_clock::time_point last_check;

    UpstreamNode() = default;
    UpstreamNode(const UpstreamNode& other)
        : name(other.name), type(other.type), host(other.host), port(other.port),
          username(other.username), password(other.password), is_direct(other.is_direct),
          alive(other.alive.load()), latency_ms(other.latency_ms.load()),
          failure_count(other.failure_count.load()), last_check(other.last_check) {}

    UpstreamNode& operator=(const UpstreamNode& other) {
        if (this != &other) {
            name = other.name;
            type = other.type;
            host = other.host;
            port = other.port;
            username = other.username;
            password = other.password;
            is_direct = other.is_direct;
            alive.store(other.alive.load());
            latency_ms.store(other.latency_ms.load());
            failure_count.store(other.failure_count.load());
            last_check = other.last_check;
        }
        return *this;
    }
};

enum class PoolStrategy {
    FAILOVER,
    BEST_LATENCY,
    ROUND_ROBIN
};

class UpstreamPool {
public:
    UpstreamPool() = default;
    ~UpstreamPool();

    void init(const std::vector<UpstreamNode>& initial_nodes, PoolStrategy strat, int health_check_interval_sec);
    void start_health_checker();
    void stop_health_checker();

    std::shared_ptr<UpstreamNode> select_node();
    void mark_node_failure(const std::string& name);
    void mark_node_success(const std::string& name, int64_t latency);

    void run_health_check_all();
    std::vector<UpstreamNode> get_snapshot();
    bool manual_select(const std::string& name_or_index);
    std::string get_active_strategy_name() const;
    void set_strategy(PoolStrategy strat);

private:
    std::vector<std::shared_ptr<UpstreamNode>> nodes;
    mutable std::mutex pool_mutex;
    PoolStrategy strategy = PoolStrategy::FAILOVER;
    int check_interval = 30;
    std::atomic<size_t> round_robin_idx{0};
    std::string manual_override_name;

    std::atomic<bool> checker_running{false};
    std::thread checker_thread;

    static int64_t probe_node(const UpstreamNode& node, int timeout_sec);
};
