#include "system_proxy.hpp"
#include "logger.hpp"
#include <windows.h>
#include <wininet.h>
#include <vector>

namespace sys {

bool SystemProxyManager::s_saved_initial_state = false;
DWORD SystemProxyManager::s_initial_proxy_enable = 0;
std::wstring SystemProxyManager::s_initial_proxy_server;
std::wstring SystemProxyManager::s_initial_proxy_override;

static const wchar_t* REG_KEY_INTERNET_SETTINGS = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

static std::wstring to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size_needed);
    return wstr;
}

void SystemProxyManager::notify_system_change() {
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

bool SystemProxyManager::init() {
    if (s_saved_initial_state) return true;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_INTERNET_SETTINGS, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD dwVal = 0;
        DWORD size = sizeof(dwVal);

        if (RegQueryValueExW(hKey, L"ProxyEnable", NULL, &type, (LPBYTE)&dwVal, &size) == ERROR_SUCCESS) {
            s_initial_proxy_enable = dwVal;
        }

        wchar_t buf[1024] = {0};
        size = sizeof(buf);
        if (RegQueryValueExW(hKey, L"ProxyServer", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
            s_initial_proxy_server = buf;
        }

        memset(buf, 0, sizeof(buf));
        size = sizeof(buf);
        if (RegQueryValueExW(hKey, L"ProxyOverride", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
            s_initial_proxy_override = buf;
        }

        RegCloseKey(hKey);
        s_saved_initial_state = true;
        return true;
    }
    return false;
}

bool SystemProxyManager::enable(const std::string& host, int port, const std::string& bypass) {
    init();

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_INTERNET_SETTINGS, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        Logger::error("Failed to open registry key for writing system proxy");
        return false;
    }

    DWORD enable_val = 1;
    RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&enable_val, sizeof(enable_val));

    std::string server_str = host + ":" + std::to_string(port);
    std::wstring server_wstr = to_wstring(server_str);
    RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (const BYTE*)server_wstr.c_str(), static_cast<DWORD>((server_wstr.size() + 1) * sizeof(wchar_t)));

    std::wstring bypass_wstr = to_wstring(bypass);
    RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, (const BYTE*)bypass_wstr.c_str(), static_cast<DWORD>((bypass_wstr.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(hKey);

    notify_system_change();
    Logger::info("Windows System Proxy enabled -> " + server_str);
    return true;
}

bool SystemProxyManager::disable() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_INTERNET_SETTINGS, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD enable_val = 0;
    RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&enable_val, sizeof(enable_val));
    RegCloseKey(hKey);

    notify_system_change();
    Logger::info("Windows System Proxy disabled");
    return true;
}

bool SystemProxyManager::is_enabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_INTERNET_SETTINGS, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD dwVal = 0;
        DWORD size = sizeof(dwVal);
        if (RegQueryValueExW(hKey, L"ProxyEnable", NULL, &type, (LPBYTE)&dwVal, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return dwVal != 0;
        }
        RegCloseKey(hKey);
    }
    return false;
}

void SystemProxyManager::restore() {
    if (!s_saved_initial_state) return;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_INTERNET_SETTINGS, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&s_initial_proxy_enable, sizeof(s_initial_proxy_enable));
        
        if (!s_initial_proxy_server.empty()) {
            RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (const BYTE*)s_initial_proxy_server.c_str(), static_cast<DWORD>((s_initial_proxy_server.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"ProxyServer");
        }

        if (!s_initial_proxy_override.empty()) {
            RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, (const BYTE*)s_initial_proxy_override.c_str(), static_cast<DWORD>((s_initial_proxy_override.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"ProxyOverride");
        }

        RegCloseKey(hKey);
        notify_system_change();
    }
}

} // namespace sys
