#ifndef CONFIG_H
#define CONFIG_H

// ===== WiFi Configuration =====
// Configure these to match your Tesla Powerwall WiFi network
#define WIFI_SSID "TeslaPowerwall"
#define WIFI_PASSWORD ""

// Powerwall IP address on the WiFi network
#define POWERWALL_IP_ADDR1 192
#define POWERWALL_IP_ADDR2 168
#define POWERWALL_IP_ADDR3 91
#define POWERWALL_IP_ADDR4 1
#define POWERWALL_IP_STR "192.168.91.1"

// ===== W5500 SPI Pin Configuration =====
// These are the correct pins for ESP32-S3-POE-ETH (Waveshare)
#define W5500_INT_GPIO  10
#define W5500_MISO_GPIO 12
#define W5500_MOSI_GPIO 11
#define W5500_SCK_GPIO  13
#define W5500_CS_GPIO   14

// ===== Ethernet IP Configuration =====
// If a static IP is saved but the LAN looks unreachable (no HTTP/proxy traffic
// and the gateway does not answer ICMP) for this many seconds after link-up,
// reboot into DHCP. Saved static settings are kept; the dashboard shows a warning.
#define ETH_DHCP_FALLBACK_SEC 45
#define ETH_DHCP_FALLBACK_PING_INTERVAL_MS 3000
// Hold BOOT (GPIO0) for 3 seconds after the firmware is running to force DHCP.
#define ETH_BOOT_GPIO 0

// ===== Proxy Server Configuration =====
#define PROXY_PORT 443
#define PROXY_TIMEOUT_MS 60000  // 60 seconds (increased from 30)
#define PROXY_BUFFER_SIZE 4096  // Buffer size for forwarding encrypted data (larger = fewer syscalls)
#define SSL_PASSTHROUGH_TASK_STACK_SIZE 6144  // Stack size per client task (reduced from 8192)
#define MAX_CONCURRENT_CLIENTS 4  // Maximum simultaneous proxy connections (each uses 2 buffers)

// ===== TTL Configuration =====
// TTL (Time-To-Live) value to set on outgoing packets to hide external origin
// Common TTL values: 64 (Linux/Unix default), 128 (Windows default), 255 (Cisco default)
// Setting to 64 as it's the most common default for web servers
#define TTL_VALUE 64

// ===== mDNS Configuration =====
#define MDNS_HOSTNAME "powerwall"
#define MDNS_SERVICE "_powerwall"
#define MDNS_PROTOCOL "_tcp"

// ===== WiFi Quality Monitoring =====
// Interval for logging WiFi connection quality (in seconds)
#define WIFI_QUALITY_LOG_INTERVAL_SEC 30  // Log every 30 seconds

// ===== System Monitoring =====
// Interval for logging system metrics (CPU load, etc.) in seconds
#define SYSTEM_MONITOR_INTERVAL_SEC 30  // Log every 30 seconds

// ===== Connection Watchdog =====
// Reboot device if no successful proxy connections within this time
#define WATCHDOG_TIMEOUT_SEC 600  // 10 minutes
#define WATCHDOG_CHECK_INTERVAL_SEC 60  // Check every minute

// ===== Debug Configuration =====
// Enable DEBUG_MODE to show encrypted packet forwarding details
#define DEBUG_MODE 0  // Set to 1 to enable debug logging

// ===== NTP Configuration =====
// NTP servers for time synchronization (uses Ethernet interface)
// Using IP addresses to avoid DNS resolution issues when WiFi network has no internet
#define NTP_SERVER_PRIMARY "216.239.35.0"    // time.google.com
#define NTP_SERVER_SECONDARY "216.239.35.4"  // time2.google.com
#define NTP_SYNC_INTERVAL_MS (3600 * 1000)   // Re-sync every hour

// ===== WiFi Metrics Configuration =====
// Track WiFi signal strength and connection success over time
#define WIFI_METRICS_BUCKET_MINUTES 5       // Each bucket covers 5 minutes
#define WIFI_METRICS_HISTORY_HOURS 24       // Keep 24 hours of history
#define WIFI_METRICS_SAMPLE_INTERVAL_SEC 30 // Sample RSSI every 30 seconds
// Total buckets: (24 * 60) / 5 = 288 buckets

// ===== Remote OTA Configuration =====
// URL to check for firmware updates (GitHub Pages hosted version.json)
#define REMOTE_OTA_VERSION_URL "https://cwagz.github.io/esp32-wifi-bridge/version.json"

// ===== HTTP Server Configuration =====
// Web UI port for status page, API, WiFi config
#define WEB_HTTP_PORT 80

// Maximum firmware size (must match partition size: 0x1C0000 = 1835008 bytes)
#define OTA_MAX_FIRMWARE_SIZE 0x1C0000

#endif // CONFIG_H
