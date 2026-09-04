# Rift

A high-performance, lightweight, zero-dependency C++20 proxy service and smart traffic routing engine for Windows with support for HTTP/HTTPS CONNECT tunneling, SOCKS5 (with RFC 1928/1929 authentication and UDP Associate), Smart Routing Rules, Upstream Proxy Pools with Auto-Failover & Latency Balancing, Windows System Proxy integration, REST API management, and Windows Service daemon mode.

---

## Key Features

- **Zero External Dependencies**: Built entirely with standard C++20 and native Windows SDK libraries (`ws2_32.lib`, `wininet.lib`, `advapi32.lib`). No external packages or third-party DLLs required.
- **Ultra-Fast CLI & Shortcut**: Control the daemon using the ultra-concise `rft` command (`rft status`, `rft switch 2`, `rft sysproxy on`) or run as an interactive shell / Windows background service.
- **Dual Mode Inbound**: Single listening port automatically multiplexes and handles both HTTP/HTTPS CONNECT and SOCKS5 handshakes.
- **SOCKS5 Protocol**: Complete RFC 1928 and RFC 1929 implementation supporting IPv4, IPv6, Domain names, No-Auth, Username/Password authentication, and UDP Associate.
- **Smart Routing Engine**:
  - Rule-based traffic classification: `DOMAIN-SUFFIX`, `DOMAIN-KEYWORD`, `DOMAIN`, `IP-CIDR`, and `FINAL`.
  - Actions: `DIRECT`, `PROXY`, and `REJECT` (Kill Switch).
- **Upstream Proxy Pool & Health Checking**:
  - Support for multiple upstream proxies (SOCKS5 / HTTP).
  - Background asynchronous health checker probing node latency.
  - Failover strategies: `failover` (primary -> backup), `best_latency` (lowest ping), `round_robin`, and manual node switching.
- **Anti-DNS Leak**: Delegates domain name resolution to the remote proxy using SOCKS5 `ATYP 0x03` to prevent DNS queries from reaching local ISP resolvers.
- **Windows System Proxy Integration**: Instant toggle of Windows Internet Options (WinINet) system proxy settings with automatic cleanup and restoration on termination.
- **Built-in REST API Controller**: Full HTTP API on port 9090 for programmatic management, IP rotation, and integration with bots, scrapers, and anti-detect browsers.
- **Windows Service Mode**: Native integration with Windows Service Control Manager (`--install-service` / `--uninstall-service`) for silent background auto-start.
- **Precise Timing & Latency Telemetry**: Real-time per-connection timing diagnostics (DNS, TCP connect, proxy handshake, session duration, upload/download accounting).

---

## Project Structure

```
Rift/
├── CMakeLists.txt        # CMake build configuration (C++20, MSVC)
├── build.bat             # 1-click compilation script
├── config.json           # Main configuration file
├── LICENSE               # GNU GPLv3 License
├── .gitignore            # Git ignore rules
├── include/
│   ├── api_server.hpp    # REST API HTTP controller
│   ├── cli.hpp           # Interactive console interface
│   ├── config.hpp        # Configuration parser and data models
│   ├── http_proxy.hpp    # HTTP/HTTPS CONNECT tunnel handler
│   ├── json.hpp          # Single-header lightweight JSON parser
│   ├── logger.hpp        # Thread-safe ANSI console logger
│   ├── oneshot_cli.hpp   # Direct one-shot CLI commands handler
│   ├── router.hpp        # Smart routing engine (CIDR, Domain rules)
│   ├── socket_utils.hpp  # Winsock RAII, traffic relaying, bandwidth stats
│   ├── socks5.hpp        # SOCKS5 protocol engine
│   ├── socks5_udp.hpp    # SOCKS5 UDP Associate relay
│   ├── system_proxy.hpp  # Windows WinINet system proxy manager
│   ├── tunnel.hpp        # Inbound connection dispatcher & server
│   ├── upstream.hpp      # Upstream connector with failover
│   ├── upstream_pool.hpp # Multi-proxy pool and health checker
│   └── win_service.hpp   # Windows Service Control Manager wrapper
├── src/
│   ├── api_server.cpp
│   ├── cli.cpp
│   ├── config.cpp
│   ├── http_proxy.cpp
│   ├── logger.cpp
│   ├── main.cpp
│   ├── oneshot_cli.cpp
│   ├── router.cpp
│   ├── socket_utils.cpp
│   ├── socks5.cpp
│   ├── socks5_udp.cpp
│   ├── system_proxy.cpp
│   ├── tunnel.cpp
│   ├── upstream.cpp
│   ├── upstream_pool.cpp
│   ├── win_service.cpp
│   ├── resource.rc       # Windows Version & metadata resource
│   └── app.manifest      # Windows UAC & OS compatibility manifest
└── README.md
```

---

## Build Instructions

### Prerequisites
- Windows 10 / 11 / Windows Server
- Visual Studio 2022 / 2026 or MSVC Build Tools with C++20 support
- CMake 3.20+

### Option 1: Automated Script
```cmd
build.bat
```

### Option 2: CMake
```cmd
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

Executable output: `build/Release/rift.exe` (and `rft.exe` alias)

---

## Configuration (`config.json`)

```json
{
  "local": {
    "host": "127.0.0.1",
    "port": 1080,
    "mode": "dual"
  },
  "auth": {
    "enabled": false,
    "username": "user",
    "password": "password123"
  },
  "proxy_pool": {
    "strategy": "failover",
    "health_check_interval": 30,
    "nodes": [
      {
        "name": "Primary-Proxy",
        "type": "socks5",
        "host": "152.232.171.125",
        "port": 8000,
        "username": "Yz2063",
        "password": "zxnbKF"
      }
    ]
  },
  "routing": {
    "enabled": true,
    "default": "PROXY",
    "rules": [
      "IP-CIDR,127.0.0.0/8,DIRECT",
      "IP-CIDR,192.168.0.0/16,DIRECT",
      "IP-CIDR,10.0.0.0/8,DIRECT",
      "IP-CIDR,172.16.0.0/12,DIRECT",
      "DOMAIN-SUFFIX,local,DIRECT",
      "DOMAIN-SUFFIX,lan,DIRECT",
      "DOMAIN-KEYWORD,bank,DIRECT",
      "FINAL,PROXY"
    ]
  },
  "security": {
    "kill_switch": false
  },
  "system_proxy": {
    "enabled": false,
    "bypass": "localhost;127.*;10.*;192.168.*;<local>"
  },
  "advanced": {
    "buffer_size": 65536,
    "timeout_seconds": 30,
    "debug_log": false
  }
}
```

---

## REST API Reference

The client runs an embedded HTTP REST API on `http://127.0.0.1:9090` (CORS enabled):

| Method | Endpoint | Description | Example Request / Response |
|---|---|---|---|
| `GET` | `/api/status` | Current service state, metrics, routing policy | `{"running":true,"metrics":{"bytes_sent":1234}}` |
| `GET` | `/api/nodes` | List of upstream proxies and live ping latency | `{"nodes":[{"name":"Primary","latency_ms":152}]}` |
| `GET` | `/api/rules` | Active routing rules list | `{"rules":[{"type":"DOMAIN-SUFFIX","action":"DIRECT"}]}` |
| `POST` | `/api/switch` | Switch active proxy node / IP rotation | `{"node": "Primary-Proxy"}` or `{"node": "auto"}` |
| `POST` | `/api/strategy` | Change pool balancing strategy | `{"strategy": "best_latency"}` |
| `POST` | `/api/sysproxy` | Enable or disable Windows System Proxy | `{"enable": true}` |
| `POST` | `/api/check` | Trigger immediate health check on all nodes | `{"nodes": [...]}` |

---

## Direct CLI Mode (`rft` / `rift`)

You can control the running proxy server directly from any terminal window using quick subcommands without entering an interactive shell:

```cmd
rft status                 # Check proxy status, upstream node, and traffic metrics
rft nodes                  # List all proxy nodes with real-time ping latency
rft switch 2               # Switch active upstream proxy to node #2 (or node name)
rft strategy best_latency  # Switch selection strategy: failover | best_latency | round_robin
rft check                  # Trigger immediate health probe across all proxy nodes
rft rules                  # List active smart routing rules
rft sysproxy on            # Enable Windows System Proxy (redirects OS & browser traffic)
rft sysproxy off           # Disable Windows System Proxy
rft test google.com 443    # Measure direct TCP connection latency to destination
rft run                    # Launch server daemon in interactive console mode
rft help                   # View full command reference
```

---

## Command Line Options

```
Usage:
  rft <command> [args...]
  rft [options]

CLI Commands (Direct Execution):
  status                     Display daemon status, mode, and traffic metrics
  nodes | pool               List all upstream nodes with latency and status
  rules                      Show smart routing rules and default routing policy
  switch <node|idx|auto>     Switch active upstream proxy node
  strategy <name>            Set pool strategy (failover | best_latency | round_robin)
  check                      Trigger immediate latency health check on all nodes
  sysproxy <on|off>          Enable or disable Windows System Proxy
  test [host] [port]         Test network connectivity to target endpoint
  run                        Start server daemon in interactive console mode
  help                       Show this help message with command references

Server Daemon Options:
  -c, --config <file>        Path to configuration JSON file (default: config.json)
  -p, --port <port>          Override local listening port (default: 1080)
  -m, --mode <dual|socks5|http> Set local proxy protocol mode (default: dual)
  -s, --sysproxy             Enable Windows System Proxy automatically on startup
  -d, --debug                Enable verbose debug logging
  --api-port <port>          Set REST API port (default: 9090, 0 to disable)
  -h, --help                 Show help message

Windows Service Options:
  --install-service          Install as a Windows Background Service (Admin required)
  --uninstall-service        Uninstall Windows Service
  --start-service            Start installed Windows Service
  --stop-service             Stop running Windows Service
```

---

## Installation & Global PATH Setup

Run the automated installer script:
```cmd
install.bat
```
This script will build the project with MSVC, copy `rift.exe`, `rft.exe`, and `config.json` to `%LOCALAPPDATA%\Rift`, and register the folder in your User `PATH`. You can then immediately invoke `rft` or `rift` from any CMD or PowerShell terminal globally.

To uninstall and remove from PATH:
```cmd
uninstall.bat
```

---

## License

This project is licensed under the [GNU General Public License v3.0 (GPL-3.0)](LICENSE).
