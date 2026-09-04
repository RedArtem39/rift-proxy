#pragma once

#include "config.hpp"
#include <string>
#include <vector>

namespace cli {

class OneShotClient {
public:
    static bool is_daemon_running(int api_port = 9090);
    static int execute_subcommand(const std::string& cmd, const std::vector<std::string>& args, int api_port = 9090);

private:
    static std::string http_get(int port, const std::string& path);
    static std::string http_post(int port, const std::string& path, const std::string& json_body);
};

} // namespace cli
