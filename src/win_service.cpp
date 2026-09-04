#include "win_service.hpp"
#include "tunnel.hpp"
#include "api_server.hpp"
#include "config.hpp"
#include "socket_utils.hpp"
#include "logger.hpp"
#include <iostream>
#include <memory>

namespace service {

SERVICE_STATUS WindowsServiceManager::s_status{};
SERVICE_STATUS_HANDLE WindowsServiceManager::s_status_handle = nullptr;
HANDLE WindowsServiceManager::s_stop_event = INVALID_HANDLE_VALUE;
std::string WindowsServiceManager::s_config_path = "config.json";

static std::unique_ptr<proxy::TunnelServer> g_service_tunnel;
static std::unique_ptr<api::RestApiServer> g_service_api;

void WindowsServiceManager::set_service_status(DWORD current_state, DWORD win32_exit_code, DWORD wait_hint) {
    static DWORD check_point = 1;
    s_status.dwCurrentState = current_state;
    s_status.dwWin32ExitCode = win32_exit_code;
    s_status.dwWaitHint = wait_hint;

    if (current_state == SERVICE_START_PENDING) {
        s_status.dwControlsAccepted = 0;
    } else {
        s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (current_state == SERVICE_RUNNING || current_state == SERVICE_STOPPED) {
        s_status.dwCheckPoint = 0;
    } else {
        s_status.dwCheckPoint = check_point++;
    }

    SetServiceStatus(s_status_handle, &s_status);
}

void WINAPI WindowsServiceManager::service_ctrl_handler(DWORD ctrl_code) {
    switch (ctrl_code) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            set_service_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (s_stop_event != INVALID_HANDLE_VALUE) {
                SetEvent(s_stop_event);
            }
            break;
        default:
            break;
    }
}

void WINAPI WindowsServiceManager::service_main(DWORD argc, LPWSTR* argv) {
    (void)argc; (void)argv;

    s_status_handle = RegisterServiceCtrlHandlerW(L"RiftService", service_ctrl_handler);
    if (!s_status_handle) return;

    s_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s_status.dwServiceSpecificExitCode = 0;
    set_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    s_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (s_stop_event == NULL) {
        set_service_status(SERVICE_STOPPED, GetLastError());
        return;
    }

    net::WinsockScope winsock;
    Config cfg = Config::load_from_file(s_config_path);

    g_service_tunnel = std::make_unique<proxy::TunnelServer>(cfg);
    if (!g_service_tunnel->start()) {
        set_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    g_service_api = std::make_unique<api::RestApiServer>(*g_service_tunnel, 9090);
    g_service_api->start();

    set_service_status(SERVICE_RUNNING);

    WaitForSingleObject(s_stop_event, INFINITE);

    if (g_service_api) g_service_api->stop();
    if (g_service_tunnel) g_service_tunnel->stop();

    CloseHandle(s_stop_event);
    set_service_status(SERVICE_STOPPED);
}

void WindowsServiceManager::run_as_service(const std::string& config_path) {
    s_config_path = config_path;
    SERVICE_TABLE_ENTRYW service_table[] = {
        { (LPWSTR)L"RiftService", (LPSERVICE_MAIN_FUNCTIONW)service_main },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(service_table);
}

bool WindowsServiceManager::install_service(const std::wstring& service_name, const std::wstring& display_name) {
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe_path, MAX_PATH) == 0) {
        std::cerr << "Failed to get executable path\n";
        return false;
    }

    std::wstring binary_path = L"\"" + std::wstring(exe_path) + L"\" --service-run";

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        std::cerr << "Failed to open Service Control Manager (Run as Administrator required)\n";
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        service_name.c_str(),
        display_name.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        binary_path.c_str(),
        NULL, NULL, NULL, NULL, NULL
    );

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) {
            std::cout << "Service is already installed.\n";
            return true;
        }
        std::cerr << "Failed to create Windows service (Error: " << err << ")\n";
        return false;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::cout << "Windows service installed successfully (Auto-start enabled).\n";
    return true;
}

bool WindowsServiceManager::uninstall_service(const std::wstring& service_name) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        std::cerr << "Failed to open Service Control Manager (Run as Administrator required)\n";
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, service_name.c_str(), SERVICE_STOP | DELETE);
    if (!svc) {
        CloseServiceHandle(scm);
        std::cout << "Service not found or already uninstalled.\n";
        return true;
    }

    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    if (!DeleteService(svc)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        std::cerr << "Failed to delete service (Error: " << err << ")\n";
        return false;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::cout << "Windows service uninstalled successfully.\n";
    return true;
}

bool WindowsServiceManager::start_service(const std::wstring& service_name) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, service_name.c_str(), SERVICE_START);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    bool ok = StartServiceW(svc, 0, NULL);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool WindowsServiceManager::stop_service(const std::wstring& service_name) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, service_name.c_str(), SERVICE_STOP);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status;
    bool ok = ControlService(svc, SERVICE_CONTROL_STOP, &status);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

} // namespace service
