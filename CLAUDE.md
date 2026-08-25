# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 WiFi-Ethernet SSL bridge that forwards encrypted traffic from Ethernet to a Tesla Powerwall over WiFi. Uses SSL passthrough (no decryption) with TTL modification to appear as local traffic.

## Build Commands

```bash
pio run                    # Build firmware
pio run -t upload          # Upload via USB
pio device monitor         # Serial monitor
./deploy.sh                # Build and deploy via OTA (mDNS discovery)
./deploy.sh -a             # Deploy to ALL eligible devices
./deploy.sh -d -i <IP>     # Deploy only to specific IP
```

## Architecture

```
[Ethernet Client] <==SSL==> [ESP32 Bridge] <==SSL==> [Powerwall WiFi]
                            Port 443 proxy
                            Port 80 Web UI/OTA
```

- **Ethernet (W5500 via SPI)**: Connected to user's network with internet access
- **WiFi**: Connected to Powerwall's isolated network (192.168.91.x, no internet)
- **Proxy**: TCP passthrough on port 443, no TLS termination
- **Web UI**: Status dashboard and OTA updates on port 80. HTTP Basic Auth (username `admin`); first boot must set a password. Port 443 is unauthenticated passthrough.

## Source Files

| File | Purpose |
|------|---------|
| `src/main.c` | Core application: web server, WiFi config, OTA, event handlers, initialization |
| `src/proxy.c` | SSL passthrough proxy: buffer pool, TCP server, request logging |
| `src/remote_ota.c` | GitHub OTA: version checking, firmware download, update API handlers |
| `include/config.h` | All configuration constants (pins, ports, timeouts, URLs) |
| `include/proxy.h` | Proxy module API: stats, request log access |
| `include/remote_ota.h` | Remote OTA module API |
| `include/web_ui.h` | Web UI assets: SVG icons, CSS, JavaScript (as C string macros) |

## Module Architecture

The codebase is split into three modules with clear responsibilities:

**main.c** - Application core
- Network initialization (Ethernet W5500, WiFi station)
- HTTP server and web UI handlers
- Event handlers (connect/disconnect)
- Background tasks (system monitor, WiFi monitor, connection watchdog, Ethernet DHCP fallback, BOOT-button recovery)
- NVS storage for WiFi credentials and Ethernet static/DHCP settings

**proxy.c** - SSL passthrough proxy
- Buffer pool for connection handling
- TCP server accepting connections on port 443
- Bidirectional forwarding with select()
- Request logging with TTFB/TTLB metrics
- Statistics (bytes, requests, success/failure counts)

**remote_ota.c** - Remote firmware updates
- Fetches version.json from GitHub Pages
- Compares versions and downloads updates
- API handlers for /api/update, /api/check-update, /api/install-update, /api/revert

## Important Patterns

- **Dual network interfaces**: HTTP client must specify `if_name` to use Ethernet for internet access (WiFi has no internet)
- **FreeRTOS tasks**: Must call `vTaskDelete(NULL)` before returning or run forever
- **String escaping in web_ui.h**: Use single `%` for CSS/JS (not `%%`), only use `%%` in printf format strings
- **OTA validation**: Firmware marked valid after Ethernet IP obtained to prevent rollback during WiFi config
- **Buffer pool**: Pre-allocated buffers for proxy connections to avoid malloc per request
- **Module initialization**: Call `remote_ota_init(eth_netif)` and `proxy_init(event_group, bit, &timestamp, eth_netif)` before starting services. HTTP and :443 bind Ethernet-only.
- **Ethernet IP**: Default DHCP. Static config lives in NVS namespace `eth_config`. Apply with `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` before `esp_eth_start`. DHCP fallback is a one-shot NVS flag (`force_dhcp`) that ignores static for a single boot; saved static settings are not erased.
- **Lockout recovery**: After static apply, ICMP-ping the gateway for `ETH_DHCP_FALLBACK_SEC`. HTTP `/api/status` or a successful proxy connection cancels fallback. Hold GPIO0 (BOOT) 3 seconds to force DHCP.

## Hardware

- Board: ESP32-S3-POE-ETH (Waveshare)
- Ethernet: W5500 via SPI (MISO=12, MOSI=11, SCLK=13, CS=14, INT=10)
- Platform pinned to espressif32@6.9.0

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Status dashboard |
| `/api/status` | GET | System status JSON (includes `eth`) |
| `/api/requests` | GET | Request history with TTFB/TTLB |
| `/api/logs` | GET | System log entries |
| `/api/update` | GET | Remote OTA status |
| `/api/check-update` | POST | Trigger GitHub version check |
| `/api/install-update` | POST | Install update from GitHub |
| `/wifi/scan` | GET | Scan WiFi networks |
| `/wifi/save` | POST | Save WiFi credentials |
| `/eth/save` | POST | Save Ethernet DHCP/static settings (reboots) |
| `/ota/upload` | POST | Upload firmware binary |
| `/reboot` | POST | Trigger reboot |

## CI/CD

- GitHub Actions builds on push to main and on tags
- Tagged releases (`v*`) deploy to GitHub Pages and create releases
- ESP Web Tools flasher: GitHub Pages at `https://<owner>.github.io/esp32-wifi-bridge/`
- Remote OTA URL is injected at tagged CI builds (`REMOTE_OTA_VERSION_URL`). Local USB builds override with gitignored `include/config.local.h`.
