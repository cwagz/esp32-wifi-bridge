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
- **Web UI**: Status dashboard and OTA updates on port 80 (Ethernet only)

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | Single-file application: proxy, web server, WiFi config, OTA, monitoring |
| `include/config.h` | All configuration constants (pins, ports, timeouts, URLs) |
| `include/web_ui.h` | Web UI assets: SVG icons, CSS, JavaScript (as C string macros) |
| `partitions.csv` | OTA partition layout (ota_0/ota_1 at 1.75MB each) |
| `sdkconfig.defaults` | ESP-IDF settings (TLS, WiFi, FreeRTOS, certificate bundle) |

## Code Structure (main.c)

The application is organized in sections:
1. **State variables** - Request logs, statistics, OTA state, watchdog timers
2. **Utility functions** - Log capture, buffer pool, NVS storage, JSON parsing
3. **HTTP handlers** - Web UI, API endpoints, OTA upload, WiFi config
4. **Remote OTA** - GitHub version check and firmware download (uses Ethernet interface)
5. **Event handlers** - Ethernet/WiFi connection events
6. **Network init** - W5500 SPI setup, WiFi station mode, mDNS
7. **Background tasks** - System monitor, WiFi monitor, connection watchdog, proxy server
8. **app_main** - Initialization sequence

## Important Patterns

- **Dual network interfaces**: HTTP client must specify `if_name` to use Ethernet for internet access (WiFi has no internet)
- **FreeRTOS tasks**: Must call `vTaskDelete(NULL)` before returning or run forever
- **String escaping in web_ui.h**: Use single `%` for CSS/JS (not `%%`), only use `%%` in printf format strings
- **OTA validation**: Firmware marked valid after Ethernet IP obtained to prevent rollback during WiFi config
- **Buffer pool**: Pre-allocated buffers for proxy connections to avoid malloc per request

## Hardware

- Board: ESP32-S3-POE-ETH (Waveshare)
- Ethernet: W5500 via SPI (MISO=12, MOSI=11, SCLK=13, CS=14, INT=10)
- Platform pinned to espressif32@6.9.0

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Status dashboard |
| `/api/status` | GET | System status JSON |
| `/api/requests` | GET | Request history with TTFB/TTLB |
| `/api/logs` | GET | System log entries |
| `/api/update` | GET | Remote OTA status |
| `/api/check-update` | POST | Trigger GitHub version check |
| `/api/install-update` | POST | Install update from GitHub |
| `/wifi/scan` | GET | Scan WiFi networks |
| `/wifi/save` | POST | Save WiFi credentials |
| `/ota/upload` | POST | Upload firmware binary |
| `/reboot` | POST | Trigger reboot |

## CI/CD

- GitHub Actions builds on push to main and on tags
- Tagged releases (`v*`) deploy to GitHub Pages and create releases
- ESP Web Tools flasher at GitHub Pages URL
- Remote OTA checks `version.json` from GitHub Pages
