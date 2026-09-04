#pragma once

#include "tunnel.hpp"
#include <string>

namespace cli {

class CommandLineInterface {
public:
    explicit CommandLineInterface(proxy::TunnelServer& server, std::string config_path);
    void run();

private:
    proxy::TunnelServer& server;
    std::string config_path;

    void print_banner();
    void print_help(const std::string& specific_cmd = "");
    void print_command_doc(const std::string& cmd);
    void print_all_docs();
    void print_status();
    void print_stats();
    void print_active_connections();
    void print_nodes();
    void print_rules();
    void test_connectivity(const std::string& host, int port);
    void handle_command(const std::string& line);
};

} // namespace cli
