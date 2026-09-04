#include "logger.hpp"
#include <windows.h>
#include <chrono>
#include <iomanip>
#include <sstream>

std::mutex Logger::s_mutex;
bool Logger::s_debug_enabled = false;

void Logger::init() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
    SetConsoleOutputCP(CP_UTF8);
}

void Logger::set_debug(bool enabled) {
    s_debug_enabled = enabled;
}

bool Logger::is_debug() {
    return s_debug_enabled;
}

std::string Logger::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm buf{};
    localtime_s(&buf, &in_time_t);

    std::ostringstream ss;
    ss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (level == LogLevel::DEBUG_LVL && !s_debug_enabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    std::string time_str = get_timestamp();

    std::string tag;
    std::string color;

    switch (level) {
        case LogLevel::DEBUG_LVL:
            tag = "[DEBUG]  ";
            color = "\033[90m";
            break;
        case LogLevel::INFO_LVL:
            tag = "[INFO]   ";
            color = "\033[37m";
            break;
        case LogLevel::CONN_LVL:
            tag = "[CONN]   ";
            color = "\033[36m";
            break;
        case LogLevel::WARN_LVL:
            tag = "[WARN]   ";
            color = "\033[33m";
            break;
        case LogLevel::ERROR_LVL:
            tag = "[ERROR]  ";
            color = "\033[31m";
            break;
        case LogLevel::TIMEOUT_LVL:
            tag = "[TIMEOUT]";
            color = "\033[91m";
            break;
    }

    std::cout << time_str << " " << color << tag << "\033[0m " << msg << std::endl;
}

void Logger::debug(const std::string& msg) {
    log(LogLevel::DEBUG_LVL, msg);
}

void Logger::info(const std::string& msg) {
    log(LogLevel::INFO_LVL, msg);
}

void Logger::conn(const std::string& msg) {
    log(LogLevel::CONN_LVL, msg);
}

void Logger::warn(const std::string& msg) {
    log(LogLevel::WARN_LVL, msg);
}

void Logger::error(const std::string& msg) {
    log(LogLevel::ERROR_LVL, msg);
}

void Logger::timeout(const std::string& msg) {
    log(LogLevel::TIMEOUT_LVL, msg);
}
