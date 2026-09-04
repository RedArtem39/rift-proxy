#pragma once

#include <string>
#include <mutex>
#include <iostream>

enum class LogLevel {
    DEBUG_LVL,
    INFO_LVL,
    CONN_LVL,
    WARN_LVL,
    ERROR_LVL,
    TIMEOUT_LVL
};

class Logger {
public:
    static void init();
    static void set_debug(bool enabled);
    static bool is_debug();

    static void log(LogLevel level, const std::string& msg);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void conn(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);
    static void timeout(const std::string& msg);

private:
    static std::mutex s_mutex;
    static bool s_debug_enabled;
    static std::string get_timestamp();
};
