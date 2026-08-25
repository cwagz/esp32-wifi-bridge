#ifndef CONFIG_H
#define CONFIG_H

/*
 * Optional local overrides (not tracked in git):
 *   1. Copy include/config.local.h.example → include/config.local.h
 *   2. And/or set REMOTE_OTA_VERSION_URL in a project-root .env
 *
 * Precedence (highest wins):
 *   compile flag / CI env  >  .env  >  config.local.h  >  defaults below
 */

#if defined(__has_include)
#  if __has_include("config.local.h")
#    include "config.local.h"
#  endif
#endif

// ===== WiFi Configuration =====
// Configure these to match your Tesla Powerwall WiFi network
#ifndef WIFI_SSID
#define WIFI_SSID "TeslaPowerwall"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

// Powerwall IP address on the WiFi network
#ifndef POWERWALL_IP_ADDR1
#define POWERWALL_IP_ADDR1 192
#endif
#ifndef POWERWALL_IP_ADDR2
#define POWERWALL_IP_ADDR2 168
#endif
#ifndef POWERWALL_IP_ADDR3
#define POWERWALL_IP_ADDR3 91
#endif
#ifndef POWERWALL_IP_ADDR4
#define POWERWALL_IP_ADDR4 1
#endif
#ifndef POWERWALL_IP_STR
#define POWERWALL_IP_STR "192.168.91.1"
#endif

// ===== W5500 SPI Pin Configuration =====
// These are the correct pins for ESP32-S3-POE-ETH (Waveshare)
#ifndef W5500_INT_GPIO
#define W5500_INT_GPIO  10
#endif
#ifndef W5500_MISO_GPIO
#define W5500_MISO_GPIO 12
#endif
#ifndef W5500_MOSI_GPIO
#define W5500_MOSI_GPIO 11
#endif
#ifndef W5500_SCK_GPIO
#define W5500_SCK_GPIO  13
#endif
#ifndef W5500_CS_GPIO
#define W5500_CS_GPIO   14
#endif

// ===== Ethernet IP Configuration =====
// If a static IP is saved but the LAN looks unreachable (no HTTP/proxy traffic
// and the gateway does not answer ICMP) for this many seconds after link-up,
// reboot into DHCP. Saved static settings are kept; the dashboard shows a warning.
#ifndef ETH_DHCP_FALLBACK_SEC
#define ETH_DHCP_FALLBACK_SEC 45
#endif
#ifndef ETH_DHCP_FALLBACK_PING_INTERVAL_MS
#define ETH_DHCP_FALLBACK_PING_INTERVAL_MS 3000
#endif
// Hold BOOT (GPIO0) for 3 seconds after the firmware is running to force DHCP.
#ifndef ETH_BOOT_GPIO
#define ETH_BOOT_GPIO 0
#endif

// ===== Proxy Server Configuration =====
#ifndef PROXY_PORT
#define PROXY_PORT 443
#endif
#ifndef PROXY_TIMEOUT_MS
#define PROXY_TIMEOUT_MS 60000  // 60 seconds (increased from 30)
#endif
#ifndef PROXY_BUFFER_SIZE
#define PROXY_BUFFER_SIZE 4096  // Buffer size for forwarding encrypted data (larger = fewer syscalls)
#endif
#ifndef SSL_PASSTHROUGH_TASK_STACK_SIZE
#define SSL_PASSTHROUGH_TASK_STACK_SIZE 6144  // Stack size per client task (reduced from 8192)
#endif
#ifndef MAX_CONCURRENT_CLIENTS
#define MAX_CONCURRENT_CLIENTS 4  // Maximum simultaneous proxy connections (each uses 2 buffers)
#endif

// ===== TTL Configuration =====
// TTL (Time-To-Live) value to set on outgoing packets to hide external origin
// Common TTL values: 64 (Linux/Unix default), 128 (Windows default), 255 (Cisco default)
// Setting to 64 as it's the most common default for web servers
#ifndef TTL_VALUE
#define TTL_VALUE 64
#endif

// ===== mDNS Configuration =====
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "powerwall"
#endif
#ifndef MDNS_SERVICE
#define MDNS_SERVICE "_powerwall"
#endif
#ifndef MDNS_PROTOCOL
#define MDNS_PROTOCOL "_tcp"
#endif

// ===== WiFi Quality Monitoring =====
// Interval for logging WiFi connection quality (in seconds)
#ifndef WIFI_QUALITY_LOG_INTERVAL_SEC
#define WIFI_QUALITY_LOG_INTERVAL_SEC 30  // Log every 30 seconds
#endif

// ===== System Monitoring =====
// Interval for logging system metrics (CPU load, etc.) in seconds
#ifndef SYSTEM_MONITOR_INTERVAL_SEC
#define SYSTEM_MONITOR_INTERVAL_SEC 30  // Log every 30 seconds
#endif

// ===== Connection Watchdog =====
// Reboot device if no successful proxy connections within this time
#ifndef WATCHDOG_TIMEOUT_SEC
#define WATCHDOG_TIMEOUT_SEC 600  // 10 minutes
#endif
#ifndef WATCHDOG_CHECK_INTERVAL_SEC
#define WATCHDOG_CHECK_INTERVAL_SEC 60  // Check every minute
#endif

// ===== Debug Configuration =====
// Enable DEBUG_MODE to show encrypted packet forwarding details
#ifndef DEBUG_MODE
#define DEBUG_MODE 0  // Set to 1 to enable debug logging
#endif

// ===== NTP Configuration =====
// NTP servers for time synchronization (uses Ethernet interface)
// Using IP addresses to avoid DNS resolution issues when WiFi network has no internet
#ifndef NTP_SERVER_PRIMARY
#define NTP_SERVER_PRIMARY "216.239.35.0"    // time.google.com
#endif
#ifndef NTP_SERVER_SECONDARY
#define NTP_SERVER_SECONDARY "216.239.35.4"  // time2.google.com
#endif
#ifndef NTP_SYNC_INTERVAL_MS
#define NTP_SYNC_INTERVAL_MS (3600 * 1000)   // Re-sync every hour
#endif

// ===== WiFi Metrics Configuration =====
// Track WiFi signal strength and connection success over time
#ifndef WIFI_METRICS_BUCKET_MINUTES
#define WIFI_METRICS_BUCKET_MINUTES 5       // Each bucket covers 5 minutes
#endif
#ifndef WIFI_METRICS_HISTORY_HOURS
#define WIFI_METRICS_HISTORY_HOURS 24       // Keep 24 hours of history
#endif
#ifndef WIFI_METRICS_SAMPLE_INTERVAL_SEC
#define WIFI_METRICS_SAMPLE_INTERVAL_SEC 30 // Sample RSSI every 30 seconds
#endif
// Total buckets: (24 * 60) / 5 = 288 buckets

// ===== Remote OTA Configuration =====
// Default is the upstream project Pages site. Forks: set this in config.local.h
// or REMOTE_OTA_VERSION_URL in .env. Tagged GitHub Actions builds inject it
// automatically from the publishing repo (https://<owner>.github.io/<repo>/version.json).
#ifndef REMOTE_OTA_VERSION_URL
#define REMOTE_OTA_VERSION_URL "https://mccahan.github.io/esp32-wifi-bridge/version.json"
#endif

// ===== Admin Web UI =====
// HTTP Basic Auth on port 80 (dashboard / OTA / config). Port 443 proxy is unchanged.
#ifndef ADMIN_USERNAME
#define ADMIN_USERNAME "admin"
#endif
#ifndef ADMIN_MIN_PASSWORD_LEN
#define ADMIN_MIN_PASSWORD_LEN 8
#endif
#ifndef ADMIN_MAX_PASSWORD_LEN
#define ADMIN_MAX_PASSWORD_LEN 64
#endif
#ifndef ADMIN_AUTH_FAIL_DELAY_MS
#define ADMIN_AUTH_FAIL_DELAY_MS 200
#endif

// ===== HTTP Server Configuration =====
// Web UI port for status page, API, WiFi config
#ifndef WEB_HTTP_PORT
#define WEB_HTTP_PORT 80
#endif

// Maximum firmware size (must match partition size: 0x1C0000 = 1835008 bytes)
#ifndef OTA_MAX_FIRMWARE_SIZE
#define OTA_MAX_FIRMWARE_SIZE 0x1C0000
#endif

#endif // CONFIG_H
