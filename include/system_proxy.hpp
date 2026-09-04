#pragma once

#include <windows.h>
#include <string>

namespace sys {

class SystemProxyManager {
public:
    static bool init();
    static bool enable(const std::string& host, int port, const std::string& bypass = "localhost;127.*;10.*;192.168.*;<local>");
    static bool disable();
    static bool is_enabled();
    static void restore();

private:
    static bool s_saved_initial_state;
    static DWORD s_initial_proxy_enable;
    static std::wstring s_initial_proxy_server;
    static std::wstring s_initial_proxy_override;

    static void notify_system_change();
};

} // namespace sys
