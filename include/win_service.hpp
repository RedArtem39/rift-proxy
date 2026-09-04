#pragma once

#include <windows.h>
#include <string>

namespace service {

class WindowsServiceManager {
public:
    static bool install_service(const std::wstring& service_name = L"RiftService", const std::wstring& display_name = L"Rift Proxy Core Service");
    static bool uninstall_service(const std::wstring& service_name = L"RiftService");
    static bool start_service(const std::wstring& service_name = L"RiftService");
    static bool stop_service(const std::wstring& service_name = L"RiftService");
    static void run_as_service(const std::string& config_path);

private:
    static void WINAPI service_main(DWORD argc, LPWSTR* argv);
    static void WINAPI service_ctrl_handler(DWORD ctrl_code);
    static void set_service_status(DWORD current_state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0);

    static SERVICE_STATUS s_status;
    static SERVICE_STATUS_HANDLE s_status_handle;
    static HANDLE s_stop_event;
    static std::string s_config_path;
};

} // namespace service
