# ESP32 WiFi Bridge - Development Notes

## Project Overview

ESP32-S3 WiFi-Ethernet SSL bridge that forwards encrypted traffic from Ethernet to a Tesla Powerwall over WiFi. Uses SSL passthrough (no decryption) with TTL modification to appear as local traffic.

## Build Commands

```bash
pio run                    # Build firmware
pio run -t upload          # Upload via USB
./deploy.sh                # Build and deploy via OTA (mDNS discovery)
./deploy.sh -a             # Deploy to ALL eligible devices
./deploy.sh -d -i <IP>     # Deploy only to specific IP
./deploy.sh -b             # Build only
```

## Key Files

- `src/main.c` - Main application (proxy, web server, WiFi config, OTA)
- `include/config.h` - Configuration constants (WiFi defaults, pins, ports)
- `include/web_ui.h` - Web UI assets (SVG icons, CSS styles, JavaScript)
- `deploy.sh` - Build and OTA deployment script with mDNS discovery
- `partitions.csv` - OTA partition layout (ota_0, ota_1)
- `platformio.ini` - PlatformIO config (pinned to espressif32@6.9.0)

## Features

### Web Dashboard (Port 80)
- Real-time status page with auto-refresh (5 second interval)
- CPU usage, uptime, and WiFi signal strength monitoring
- Request history with timing metrics (TTFB, TTLB)
- System log viewer with color-coded log levels
- Cumulative statistics (bytes in/out, success rate)
- Dark mode UI with responsive design

### OTA Updates
- HTTP server on port 80 (Ethernet interface)
- Web UI for firmware upload at `http://<eth-ip>/`
- Automatic rollback if firmware crashes before validation
- OTA server starts before WiFi connects (allows recovery from bad WiFi config)

### WiFi Configuration (Web UI)
- Credentials stored in NVS (persist across reboots)
- Scan for available networks from web UI
- Change WiFi settings without reflashing
- Falls back to compiled defaults if NVS empty

### mDNS Service
- Hostname: `powerwall.local`
- Service: `_powerwall._tcp`
- TXT records: `wifi_ssid`, `target`, `ota_port`

### Deploy Script Features
- mDNS device discovery (dns-sd on macOS, avahi on Linux)
- Multi-device selection menu
- `-a/--all` flag to deploy to all eligible devices
- Progress bar during firmware upload
- Device compatibility check (requires `ota_port` TXT record)

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Status dashboard (HTML) |
| `/api/status` | GET | System status JSON (CPU, uptime, WiFi RSSI, stats) |
| `/api/rssi` | GET | WiFi signal strength |
| `/api/requests` | GET | Request history with timing metrics |
| `/api/logs` | GET | System log entries |
| `/wifi/scan` | GET | Scan for available WiFi networks |
| `/wifi/save` | POST | Save WiFi credentials |
| `/ota/upload` | POST | Upload firmware binary |
| `/ota/rollback` | POST | Rollback to previous firmware |
| `/reboot` | POST | Trigger device reboot |

## Architecture

```
[Ethernet Client] <==SSL==> [ESP32 Bridge] <==SSL==> [Powerwall WiFi]
                            Port 443 proxy
                            Port 80 Web UI/OTA
```

## Hardware

- Board: ESP32-S3-POE-ETH (Waveshare)
- Ethernet: W5500 via SPI
- Pins: MISO=12, MOSI=11, SCLK=13, CS=14, INT=10

## Configuration Defaults (config.h)

- WiFi: Stored in NVS, defaults in config.h
- Powerwall IP: 192.168.91.1
- Proxy port: 443
- Web UI port: 80
- TTL: 64 (hides external origin)
- Buffer size: 4096 bytes
- Max concurrent clients: 4
- WiFi quality log interval: 30 seconds
- System monitor interval: 30 seconds

## Notes

- Ethernet MAC derived from WiFi MAC (locally administered bit set)
- WiFi scan requires temporary disconnect (ESP32 limitation)
- OTA validation happens after Ethernet IP obtained (prevents rollback during WiFi config)
- Platform pinned to espressif32@6.9.0 to avoid toolchain issues
- Request logging tracks TTFB (time to first byte) and TTLB (time to last byte)
- Log capture uses ring buffer (50 entries, 120 chars max per entry)
