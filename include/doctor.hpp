#pragma once

#include <string>
#include <vector>

namespace diag {

struct CheckResult {
    std::string category;
    std::string item;
    bool passed = true;
    bool is_warning = false;
    std::string message;
};

class Doctor {
public:
    static bool run_diagnostics(const std::string& config_path = "config.json", bool probe_nodes = true);
};

} // namespace diag
