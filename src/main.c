/*
 * ESP32-S3 W5500 Ethernet WiFi Bridge (ESP-IDF)
 *
 * This implementation uses ESP-IDF native esp_eth driver with W5500 over SPI.
 * Implements SSL/TLS passthrough proxy without decryption.
 * The proxy forwards encrypted traffic from Ethernet to WiFi and modifies TTL to hide external origin.
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "mdns.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_app_format.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

#include "config.h"
#include "web_ui.h"

static const char *TAG = "wifi-eth-bridge";

// NVS namespace for WiFi credentials
#define NVS_WIFI_NAMESPACE "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"

// WiFi credentials (runtime, loaded from NVS)
static char wifi_ssid[33] = "";
static char wifi_password[65] = "";
static bool wifi_configured = false;  // True if credentials saved in NVS

// Powerwall connectivity status
static volatile bool powerwall_reachable = false;
static volatile int64_t last_powerwall_check = 0;

// ===== Request Log =====
// Tracks individual request/response exchanges through the proxy
#define REQUEST_LOG_SIZE 10

typedef struct {
    int64_t timestamp;      // Seconds since boot
    uint32_t source_ip;     // Source IP (network byte order)
    uint32_t bytes_in;      // Request bytes (client -> powerwall)
    uint32_t bytes_out;     // Response bytes (powerwall -> client)
    uint16_t ttfb_ms;       // Time to first byte from Powerwall
    uint16_t ttlb_ms;       // Time to last byte (full response duration)
    uint8_t result;         // 0=success, 1=timeout, 2=error
    bool valid;
} request_log_entry_t;

static request_log_entry_t request_log[REQUEST_LOG_SIZE];
static int request_log_index = 0;
static SemaphoreHandle_t request_log_mutex = NULL;

// Running average TTFB (exponential moving average)
static uint32_t avg_ttfb_ms = 0;
static uint32_t ttfb_sample_count = 0;

// Cumulative statistics (must be before log_request)
static int64_t boot_time_us = 0;
static uint64_t total_bytes_in = 0;
static uint64_t total_bytes_out = 0;
static uint32_t total_requests = 0;
static uint32_t successful_requests = 0;
static uint32_t failed_requests = 0;

/** Log a completed request/response exchange */
static void log_request(uint32_t source_ip, uint32_t bytes_in, uint32_t bytes_out, uint16_t ttfb_ms, uint16_t ttlb_ms, uint8_t result)
{
    // Update cumulative statistics (atomic-safe for single writer)
    total_requests++;
    if (result == 0) {
        successful_requests++;
    } else {
        failed_requests++;
    }

    if (!request_log_mutex) return;
    if (xSemaphoreTake(request_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        request_log_entry_t *entry = &request_log[request_log_index];
        entry->timestamp = esp_timer_get_time() / 1000000;
        entry->source_ip = source_ip;
        entry->bytes_in = bytes_in;
        entry->bytes_out = bytes_out;
        entry->ttfb_ms = ttfb_ms;
        entry->ttlb_ms = ttlb_ms;
        entry->result = result;
        entry->valid = true;
        request_log_index = (request_log_index + 1) % REQUEST_LOG_SIZE;

        // Update running average TTFB (exponential moving average, alpha=0.2)
        if (result == 0 && ttfb_ms > 0) {
            if (ttfb_sample_count == 0) {
                avg_ttfb_ms = ttfb_ms;
            } else {
                avg_ttfb_ms = (avg_ttfb_ms * 4 + ttfb_ms) / 5;
            }
            ttfb_sample_count++;
        }

        xSemaphoreGive(request_log_mutex);
    }
}

// Event group for WiFi and Ethernet status
static EventGroupHandle_t s_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define ETH_CONNECTED_BIT BIT1
#define ETH_GOT_IP_BIT BIT2

// CPU usage tracking (percentage, 0-100)
static volatile uint8_t cpu_usage_percent = 0;

// ===== Log Capture Ring Buffer =====
#define LOG_BUFFER_SIZE 50
#define LOG_MSG_MAX_LEN 120

typedef struct {
    int64_t timestamp;      // Seconds since boot
    uint8_t level;          // ESP_LOG_ERROR=1, WARN=2, INFO=3, DEBUG=4, VERBOSE=5
    char message[LOG_MSG_MAX_LEN];
} log_entry_t;

static log_entry_t log_buffer[LOG_BUFFER_SIZE];
static int log_buffer_index = 0;
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t original_vprintf = NULL;

/** Custom log handler to capture logs to ring buffer */
static int custom_log_vprintf(const char *fmt, va_list args)
{
    // Always call original handler first
    int ret = 0;
    if (original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // Try to capture log (non-blocking to avoid deadlocks)
    if (log_mutex && xSemaphoreTake(log_mutex, 0) == pdTRUE) {
        log_entry_t *entry = &log_buffer[log_buffer_index];
        entry->timestamp = esp_timer_get_time() / 1000000;

        // Parse log level from format (ESP-IDF format: "X (tag) message")
        char temp[LOG_MSG_MAX_LEN + 32];
        va_list args_copy;
        va_copy(args_copy, args);
        vsnprintf(temp, sizeof(temp), fmt, args_copy);
        va_end(args_copy);

        // Determine level from first character
        entry->level = 3;  // Default INFO
        if (temp[0] == 'E') entry->level = 1;
        else if (temp[0] == 'W') entry->level = 2;
        else if (temp[0] == 'I') entry->level = 3;
        else if (temp[0] == 'D') entry->level = 4;
        else if (temp[0] == 'V') entry->level = 5;

        // Copy message, strip trailing newline
        strncpy(entry->message, temp, LOG_MSG_MAX_LEN - 1);
        entry->message[LOG_MSG_MAX_LEN - 1] = '\0';
        size_t len = strlen(entry->message);
        if (len > 0 && entry->message[len - 1] == '\n') {
            entry->message[len - 1] = '\0';
        }

        log_buffer_index = (log_buffer_index + 1) % LOG_BUFFER_SIZE;
        xSemaphoreGive(log_mutex);
    }

    return ret;
}

/** Initialize the log capture system */
static void init_log_capture(void)
{
    log_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        log_buffer[i].timestamp = 0;
        log_buffer[i].level = 0;
        log_buffer[i].message[0] = '\0';
    }
    // Install custom log handler
    original_vprintf = esp_log_set_vprintf(custom_log_vprintf);
    ESP_LOGI(TAG, "Log capture initialized: %d entries", LOG_BUFFER_SIZE);
}

// Ethernet and WiFi handles
static esp_eth_handle_t eth_handle = NULL;
static esp_netif_t *eth_netif = NULL;
static esp_netif_t *wifi_netif = NULL;

// Server socket
static int server_socket = -1;

// OTA HTTP server handle
static httpd_handle_t web_server = NULL;

// ===== Remote OTA State =====
typedef struct {
    char available_version[32];
    char download_url[256];
    uint32_t firmware_size;
    char previous_version[32];
    char previous_url[256];
    uint32_t previous_size;
    bool update_available;
    bool previous_available;
    bool check_in_progress;
    bool install_in_progress;
    int64_t last_check_time;
} remote_ota_state_t;

static remote_ota_state_t remote_ota = {0};
static SemaphoreHandle_t remote_ota_mutex = NULL;

// ===== Buffer Pool =====
// Preallocated buffers to avoid malloc/free overhead per connection
typedef struct {
    uint8_t client_buffer[PROXY_BUFFER_SIZE];
    uint8_t powerwall_buffer[PROXY_BUFFER_SIZE];
    bool in_use;
} buffer_pair_t;

static buffer_pair_t buffer_pool[MAX_CONCURRENT_CLIENTS];
static SemaphoreHandle_t buffer_pool_mutex = NULL;

/** Initialize the buffer pool */
static void init_buffer_pool(void)
{
    buffer_pool_mutex = xSemaphoreCreateMutex();
    request_log_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
        buffer_pool[i].in_use = false;
    }
    for (int i = 0; i < REQUEST_LOG_SIZE; i++) {
        request_log[i].valid = false;
    }
    ESP_LOGI(TAG, "Buffer pool initialized: %d slots, %d bytes each",
             MAX_CONCURRENT_CLIENTS, PROXY_BUFFER_SIZE * 2);
}

/** Acquire a buffer pair from the pool. Returns index or -1 if none available */
static int acquire_buffer_pair(void)
{
    int index = -1;
    if (xSemaphoreTake(buffer_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
            if (!buffer_pool[i].in_use) {
                buffer_pool[i].in_use = true;
                index = i;
                break;
            }
        }
        xSemaphoreGive(buffer_pool_mutex);
    }
    return index;
}

/** Release a buffer pair back to the pool */
static void release_buffer_pair(int index)
{
    if (index >= 0 && index < MAX_CONCURRENT_CLIENTS) {
        if (xSemaphoreTake(buffer_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            buffer_pool[index].in_use = false;
            xSemaphoreGive(buffer_pool_mutex);
        }
    }
}

// ===== NVS WiFi Credential Storage =====

/** Load WiFi credentials from NVS - returns true if credentials exist */
static bool load_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi credentials in NVS");
        wifi_configured = false;
        return false;
    }

    size_t ssid_len = sizeof(wifi_ssid);
    size_t pass_len = sizeof(wifi_password);

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, wifi_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, wifi_password, &pass_len);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK && strlen(wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Loaded WiFi credentials from NVS: SSID=%s", wifi_ssid);
        wifi_configured = true;
        return true;
    }

    ESP_LOGI(TAG, "No valid WiFi credentials found");
    wifi_configured = false;
    return false;
}

/** Save WiFi credentials to NVS */
static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved to NVS: SSID=%s", ssid);
        // Update runtime credentials
        strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
        strncpy(wifi_password, password, sizeof(wifi_password) - 1);
        wifi_password[sizeof(wifi_password) - 1] = '\0';
    } else {
        ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
    }

    return err;
}

// ===== Powerwall Connectivity Check =====

/** Check if Powerwall is reachable (non-blocking TCP connect test) */
static void check_powerwall_connectivity(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        powerwall_reachable = false;
        return;
    }

    // Set socket to non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    inet_pton(AF_INET, POWERWALL_IP_STR, &addr.sin_addr);

    int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (result == 0) {
        powerwall_reachable = true;
    } else if (errno == EINPROGRESS) {
        // Wait for connection with timeout
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);
        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};

        if (select(sock + 1, NULL, &write_fds, NULL, &tv) > 0) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
            powerwall_reachable = (error == 0);
        } else {
            powerwall_reachable = false;
        }
    } else {
        powerwall_reachable = false;
    }

    close(sock);
    last_powerwall_check = esp_timer_get_time() / 1000;  // Convert to ms
}

// ===== OTA Update Handlers =====
// Icons, CSS, and JavaScript are defined in web_ui.h

/** OTA status page - modern dark theme with WiFi config */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    esp_ota_get_state_partition(running, &ota_state);

    // Get WiFi status
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    wifi_ap_record_t ap_info = {0};
    int rssi = 0;
    if (wifi_connected) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    // Check Powerwall connectivity (rate-limited)
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_powerwall_check > 5000 || last_powerwall_check == 0) {
        check_powerwall_connectivity();
    }

    // Get IP address
    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "N/A";
    if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    // Build response - split into chunks for memory efficiency
    httpd_resp_set_type(req, "text/html");

    // Send HTML head and CSS separately (CSS is too large for single buffer)
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32 WiFi Bridge</title><style>");
    httpd_resp_sendstr_chunk(req, DARK_CSS);
    httpd_resp_sendstr_chunk(req,
        "svg.i{width:1.125rem;height:1.125rem;vertical-align:middle;margin-right:0.25rem;fill:currentColor}"
        "</style></head><body><div class=\"container\">");

    char buf[512];

    // Status card - header
    httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h1>" ICON_ROUTER " ESP32 WiFi Bridge</h1><div class=\"grid\">");

    // WiFi status (clickable to show/hide WiFi config)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"status-item\" style=\"cursor:pointer\" onclick=\"document.getElementById('wificfg').style.display=document.getElementById('wificfg').style.display==='none'?'block':'none'\">"
        "<div class=\"label\">" ICON_WIFI " WiFi " ICON_SETTINGS "</div>");
    snprintf(buf, sizeof(buf),
        "<div class=\"value\"><span class=\"status-dot %s\"></span>%s</div></div>",
        wifi_connected ? "status-ok" : "status-err",
        wifi_connected ? "Connected" : "Disconnected");
    httpd_resp_sendstr_chunk(req, buf);

    // Signal strength (with ID for auto-refresh, colored by quality)
    if (wifi_connected) {
        const char *sig_color = rssi > -50 ? "#22c55e" : rssi > -60 ? "#84cc16" : rssi > -70 ? "#eab308" : "#ef4444";
        const char *sig_quality = rssi > -50 ? "Excellent" : rssi > -60 ? "Good" : rssi > -70 ? "Fair" : "Weak";
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">" ICON_SIGNAL " Signal</div>"
            "<div class=\"value\" id=\"sig\"><span style=\"color:%s\">%d dBm (%s)</span></div></div>",
            sig_color, rssi, sig_quality);
    } else {
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">" ICON_SIGNAL " Signal</div><div class=\"value\" id=\"sig\">-</div></div>");
    }
    httpd_resp_sendstr_chunk(req, buf);

    // Powerwall status
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">" ICON_BATTERY " Powerwall</div>"
        "<div class=\"value\"><span class=\"status-dot %s\"></span>%s</div></div>",
        powerwall_reachable ? "status-ok" : "status-err",
        powerwall_reachable ? "Reachable" : "Unreachable");
    httpd_resp_sendstr_chunk(req, buf);

    // Target IP
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">" ICON_DNS " Target</div><div class=\"value\">%s</div></div>"
        "</div></div>", POWERWALL_IP_STR);
    httpd_resp_sendstr_chunk(req, buf);

    // WiFi Configuration card (hidden by default, toggle via WiFi Status click)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\" id=\"wificfg\" style=\"display:none\"><h2>" ICON_SETTINGS " WiFi Configuration</h2>"
        "<form method=\"POST\" action=\"/wifi/save\">"
        "<div class=\"form-group\"><label class=\"label\">Network SSID</label>");

    snprintf(buf, sizeof(buf),
        "<input type=\"text\" name=\"ssid\" id=\"ssid\" value=\"%s\" placeholder=\"Enter SSID\" class=\"mt-1\">", wifi_ssid);
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req,
        "<div class=\"flex mt-1\">"
        "<button type=\"button\" class=\"btn btn-secondary\" onclick=\"scanWifi()\">" ICON_SEARCH " Scan</button>"
        "<select id=\"wl\" style=\"display:none;flex:1\" onchange=\"document.getElementById('ssid').value=this.value\"></select>"
        "</div></div>");

    httpd_resp_sendstr_chunk(req,
        "<div class=\"form-group\"><label class=\"label\">Password</label>"
        "<input type=\"password\" name=\"password\" placeholder=\"Enter password\" class=\"mt-1\"></div>");

    snprintf(buf, sizeof(buf),
        "<div class=\"text-xs text-muted\" style=\"margin-bottom:0.75rem\">Current: %s</div>"
        "<button type=\"submit\" class=\"btn btn-primary\">" ICON_SAVE " Save &amp; Reconnect</button>"
        "</form></div>", wifi_ssid);
    httpd_resp_sendstr_chunk(req, buf);

    // System info card
    httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h2>" ICON_MEMORY " System</h2><div class=\"grid\">");
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">CPU</div><div class=\"value\" id=\"cpu\">%u%%</div></div>"
        "<div class=\"status-item\"><div class=\"label\">Heap</div><div class=\"value\">%lu KB</div></div>",
        cpu_usage_percent, (unsigned long)(esp_get_free_heap_size() / 1024));
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">WiFi IP</div><div class=\"value\">%s</div></div>"
        "</div>",
        ip_str);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<hr><form id=\"rebootform\" method=\"POST\" action=\"/reboot\">"
        "<button type=\"button\" class=\"btn btn-secondary\" onclick=\"if(confirm('Reboot device?'))document.getElementById('rebootform').submit()\">"
        ICON_UPDATE " Reboot</button></form></div>");

    // Statistics card
    {
        int64_t uptime_sec = (esp_timer_get_time() - boot_time_us) / 1000000;
        int days = uptime_sec / 86400;
        int hours = (uptime_sec % 86400) / 3600;
        int mins = (uptime_sec % 3600) / 60;
        int secs = uptime_sec % 60;

        uint32_t success_rate = 0;
        if (total_requests > 0) {
            success_rate = (successful_requests * 100) / total_requests;
        }

        // Format bytes with appropriate unit
        const char *in_unit = "B", *out_unit = "B";
        double bytes_in_fmt = total_bytes_in, bytes_out_fmt = total_bytes_out;
        if (bytes_in_fmt >= 1073741824) { bytes_in_fmt /= 1073741824; in_unit = "GB"; }
        else if (bytes_in_fmt >= 1048576) { bytes_in_fmt /= 1048576; in_unit = "MB"; }
        else if (bytes_in_fmt >= 1024) { bytes_in_fmt /= 1024; in_unit = "KB"; }
        if (bytes_out_fmt >= 1073741824) { bytes_out_fmt /= 1073741824; out_unit = "GB"; }
        else if (bytes_out_fmt >= 1048576) { bytes_out_fmt /= 1048576; out_unit = "MB"; }
        else if (bytes_out_fmt >= 1024) { bytes_out_fmt /= 1024; out_unit = "KB"; }

        httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h2>" ICON_SWAP " Statistics</h2><div class=\"grid\">");
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">Uptime</div><div class=\"value\" id=\"uptime\">%dd %dh %dm %ds</div></div>"
            "<div class=\"status-item\"><div class=\"label\">Requests</div><div class=\"value\" id=\"reqcnt\">%lu</div></div>",
            days, hours, mins, secs, (unsigned long)total_requests);
        httpd_resp_sendstr_chunk(req, buf);
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">Success Rate</div><div class=\"value\" id=\"succrate\" style=\"color:%s\">%lu%%</div></div>"
            "<div class=\"status-item\"><div class=\"label\">Failed</div><div class=\"value\" id=\"failcnt\" style=\"color:#ef4444\">%lu</div></div>",
            success_rate >= 90 ? "#22c55e" : success_rate >= 70 ? "#eab308" : "#ef4444",
            (unsigned long)success_rate, (unsigned long)failed_requests);
        httpd_resp_sendstr_chunk(req, buf);
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">Bytes In</div><div class=\"value\" id=\"bytesin\">%.1f %s</div></div>"
            "<div class=\"status-item\"><div class=\"label\">Bytes Out</div><div class=\"value\" id=\"bytesout\">%.1f %s</div></div>",
            bytes_in_fmt, in_unit, bytes_out_fmt, out_unit);
        httpd_resp_sendstr_chunk(req, buf);
        httpd_resp_sendstr_chunk(req, "</div></div>");
    }

    // Recent requests card with TTFB (IDs for auto-refresh)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><h2>" ICON_SWAP " Recent Requests</h2>"
        "<div class=\"flex\" style=\"justify-content:space-between;margin-bottom:0.5rem\">");
    snprintf(buf, sizeof(buf),
        "<span class=\"text-sm text-muted\">Avg TTFB: <span id=\"avgttfb\">%lu</span> ms</span>",
        (unsigned long)avg_ttfb_ms);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<span class=\"text-xs text-muted\">Updated: <span id=\"lastref\">now</span></span></div>"
        "<table style=\"width:100%;font-size:0.875rem\">"
        "<tr style=\"color:#94a3b8\"><td>Age</td><td>Source</td><td>Req/Resp</td><td>Response</td><td>Status</td></tr>"
        "<tbody id=\"reqtbl\">");

    if (request_log_mutex && xSemaphoreTake(request_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int64_t now = esp_timer_get_time() / 1000000;
        for (int i = 0; i < REQUEST_LOG_SIZE; i++) {
            // Read in reverse order (most recent first)
            int idx = (request_log_index - 1 - i + REQUEST_LOG_SIZE) % REQUEST_LOG_SIZE;
            request_log_entry_t *e = &request_log[idx];
            if (!e->valid) continue;

            int64_t age = now - e->timestamp;
            const char *age_unit = "s";
            if (age >= 3600) { age /= 3600; age_unit = "h"; }
            else if (age >= 60) { age /= 60; age_unit = "m"; }

            const char *status = e->result == 0 ? "OK" : (e->result == 1 ? "TMO" : "ERR");
            const char *color = e->result == 0 ? "#22c55e" : (e->result == 1 ? "#eab308" : "#ef4444");

            // Format source IP
            uint8_t *ip = (uint8_t *)&e->source_ip;

            snprintf(buf, sizeof(buf),
                "<tr><td>%lld%s</td><td>%d.%d.%d.%d</td><td>%lu/%lu</td><td>%u-%ums</td><td style=\"color:%s\">%s</td></tr>",
                (long long)age, age_unit,
                ip[0], ip[1], ip[2], ip[3],
                (unsigned long)e->bytes_in, (unsigned long)e->bytes_out,
                e->ttfb_ms, e->ttlb_ms, color, status);
            httpd_resp_sendstr_chunk(req, buf);
        }
        xSemaphoreGive(request_log_mutex);
    }

    httpd_resp_sendstr_chunk(req, "</tbody></table></div>");

    // Log Viewer card
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><h2>" ICON_MEMORY " System Logs</h2>"
        "<div style=\"max-height:200px;overflow-y:auto;font-family:monospace;font-size:0.75rem;background:#0f172a;padding:0.5rem;border-radius:0.375rem\" id=\"logview\">");

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
            int idx = (log_buffer_index - 1 - i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
            log_entry_t *e = &log_buffer[idx];
            if (e->timestamp == 0 && e->message[0] == '\0') continue;

            const char *color = "#94a3b8";  // Default gray
            if (e->level == 1) color = "#ef4444";       // ERROR - red
            else if (e->level == 2) color = "#eab308";  // WARN - yellow
            else if (e->level == 3) color = "#22c55e";  // INFO - green
            else if (e->level == 4) color = "#94a3b8";  // DEBUG - gray

            // HTML-escape the message
            char escaped[LOG_MSG_MAX_LEN * 2];
            char *out = escaped;
            for (const char *in = e->message; *in && (out - escaped) < (int)sizeof(escaped) - 6; in++) {
                switch (*in) {
                    case '<': memcpy(out, "&lt;", 4); out += 4; break;
                    case '>': memcpy(out, "&gt;", 4); out += 4; break;
                    case '&': memcpy(out, "&amp;", 5); out += 5; break;
                    default: *out++ = *in; break;
                }
            }
            *out = '\0';

            snprintf(buf, sizeof(buf),
                "<div style=\"color:%s;white-space:nowrap\">%s</div>", color, escaped);
            httpd_resp_sendstr_chunk(req, buf);
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_sendstr_chunk(req, "</div></div>");

    // Firmware card (at bottom)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><h2>" ICON_UPDATE " Firmware</h2><div class=\"grid\">");

    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">Version</div><div class=\"value\">%s</div></div>"
        "<div class=\"status-item\"><div class=\"label\">Built</div><div class=\"value text-sm\">%s</div></div>",
        app_desc->version, app_desc->date);
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">Partition</div><div class=\"value\">%s</div></div>"
        "<div class=\"status-item\"><div class=\"label\">State</div><div class=\"value\">%s</div></div></div><hr>",
        running->label,
        ota_state == ESP_OTA_IMG_VALID ? "Valid" :
        ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "Pending" : "New");
    httpd_resp_sendstr_chunk(req, buf);

    // Remote update section
    httpd_resp_sendstr_chunk(req,
        "<div id=\"remote-update\" class=\"status-item\" style=\"grid-column:span 2;background:#0f172a\">"
        "<div class=\"flex\" style=\"justify-content:space-between\">"
        "<div><span class=\"label\">Remote Updates</span>"
        "<div class=\"value text-sm\" id=\"update-status\">Not checked</div></div>"
        "<button class=\"btn btn-secondary\" onclick=\"checkUpdate()\">Check for Updates</button>"
        "</div>"
        "<div id=\"update-actions\" style=\"display:none;margin-top:0.75rem\" class=\"flex\">"
        "<button class=\"btn btn-primary\" onclick=\"installUpdate()\">Install Update</button>"
        "<button class=\"btn btn-secondary\" onclick=\"revertFirmware()\" id=\"revert-btn\" style=\"display:none\">Revert to Previous</button>"
        "</div></div><hr>");

    // OTA upload form
    httpd_resp_sendstr_chunk(req,
        "<form method=\"POST\" action=\"/ota/upload\" enctype=\"multipart/form-data\">"
        "<div class=\"form-group\"><label class=\"label\">" ICON_UPLOAD " Upload Firmware (.bin)</label>"
        "<input type=\"file\" name=\"firmware\" accept=\".bin\" class=\"mt-1\"></div>"
        "<div class=\"flex\"><button type=\"submit\" class=\"btn btn-primary\">" ICON_UPLOAD " Upload</button>");
    httpd_resp_sendstr_chunk(req,
        "<button type=\"button\" class=\"btn btn-danger\" onclick=\"if(confirm('Rollback to previous partition?'))document.getElementById('rb').submit()\">" ICON_HISTORY " Rollback</button></div>"
        "</form><form id=\"rb\" method=\"POST\" action=\"/ota/rollback\"></form>"
        "<div class=\"alert alert-warn mt-2\">" ICON_WARN " Device will reboot after update</div></div>");

    // JavaScript for WiFi scanning and auto-refresh (defined in web_ui.h)
    httpd_resp_sendstr_chunk(req, WEB_UI_SCRIPT "</div></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);  // End chunked response
    return ESP_OK;
}

/** OTA firmware upload handler */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA upload started, content length: %d", req->content_len);

    if (req->content_len > OTA_MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Firmware too large: %d > %d", req->content_len, OTA_MAX_FIRMWARE_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    // Receive and write firmware in chunks
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    int total_received = 0;
    bool header_skipped = false;

    while (total_received < req->content_len) {
        int remaining = req->content_len - total_received;
        received = httpd_req_recv(req, buf, (remaining < 4096) ? remaining : 4096);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Error receiving data: %d", received);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        // Skip multipart form header on first chunk
        char *data = buf;
        int data_len = received;
        if (!header_skipped) {
            char *bin_start = memmem(buf, received, "\r\n\r\n", 4);
            if (bin_start) {
                bin_start += 4;
                data = bin_start;
                data_len = received - (bin_start - buf);
                header_skipped = true;
            }
        }

        if (header_skipped && data_len > 0) {
            // Check for multipart boundary at end (simplified: trim last boundary)
            err = esp_ota_write(ota_handle, data, data_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                return ESP_FAIL;
            }
        }

        total_received += received;
        if (total_received % 65536 < 4096) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes", total_received, req->content_len);
        }
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed - invalid image?");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting...");

    const char *response = "<!DOCTYPE html><html><head><title>OTA Success</title>"
        "<meta http-equiv='refresh' content='10;url=/'>"
        "<style>body{font-family:Arial,sans-serif;margin:40px;text-align:center;}"
        ".success{color:#4CAF50;font-size:24px;}</style></head>"
        "<body><p class='success'>&#10004; Firmware updated successfully!</p>"
        "<p>Device is rebooting... Redirecting in 10 seconds.</p></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    // Delay to allow response to be sent, then reboot
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/** WiFi scan handler - returns JSON list of networks */
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");

    // Disconnect first - scanning not allowed while connecting
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay for disconnect to complete

    // Configure scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // Blocking scan
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        // Try to reconnect even if scan failed
        esp_wifi_connect();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    wifi_ap_record_t *ap_list = NULL;
    if (ap_count > 0) {
        if (ap_count > 20) ap_count = 20;  // Limit results
        ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
        }
    }

    // Build JSON response
    httpd_resp_set_type(req, "application/json");

    char buf[64];
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");

    for (int i = 0; i < ap_count && ap_list; i++) {
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i > 0 ? "," : "",
                 (char *)ap_list[i].ssid,
                 ap_list[i].rssi);
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);

    if (ap_list) free(ap_list);

    // Reconnect to WiFi after scan
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi scan complete: %d networks found", ap_count);
    return ESP_OK;
}

/** WiFi save handler - saves new credentials and reconnects */
static esp_err_t wifi_save_handler(httpd_req_t *req)
{
    char content[256];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    content[received] = '\0';

    ESP_LOGI(TAG, "WiFi save request: %s", content);

    // Parse form data (ssid=xxx&password=xxx)
    char new_ssid[33] = {0};
    char new_password[65] = {0};

    // Find ssid=
    char *ssid_start = strstr(content, "ssid=");
    if (ssid_start) {
        ssid_start += 5;
        char *ssid_end = strchr(ssid_start, '&');
        size_t ssid_len = ssid_end ? (size_t)(ssid_end - ssid_start) : strlen(ssid_start);
        if (ssid_len > sizeof(new_ssid) - 1) ssid_len = sizeof(new_ssid) - 1;
        strncpy(new_ssid, ssid_start, ssid_len);
    }

    // Find password=
    char *pass_start = strstr(content, "password=");
    if (pass_start) {
        pass_start += 9;
        char *pass_end = strchr(pass_start, '&');
        size_t pass_len = pass_end ? (size_t)(pass_end - pass_start) : strlen(pass_start);
        if (pass_len > sizeof(new_password) - 1) pass_len = sizeof(new_password) - 1;
        strncpy(new_password, pass_start, pass_len);
    }

    // URL decode (basic: + to space, %XX)
    for (char *p = new_ssid; *p; p++) if (*p == '+') *p = ' ';
    for (char *p = new_password; *p; p++) if (*p == '+') *p = ' ';

    if (strlen(new_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving new WiFi credentials: SSID=%s", new_ssid);

    // Save to NVS
    esp_err_t err = save_wifi_credentials(new_ssid, new_password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    // Update runtime credentials
    strncpy(wifi_ssid, new_ssid, sizeof(wifi_ssid) - 1);
    strncpy(wifi_password, new_password, sizeof(wifi_password) - 1);
    wifi_configured = true;

    // Send success response before reconnecting
    const char *response =
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"10;url=/\">"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".box{background:#1e293b;padding:2rem;border-radius:0.75rem;text-align:center;border:1px solid #334155}"
        ".spinner{width:3rem;height:3rem;border:3px solid #334155;border-top:3px solid #3b82f6;border-radius:50%;animation:spin 1s linear infinite;margin:1rem auto}"
        "@keyframes spin{to{transform:rotate(360deg)}}</style></head>"
        "<body><div class=\"box\"><div class=\"spinner\"></div>"
        "<h2>Connecting to WiFi...</h2>"
        "<p>Connecting to network. Page will refresh automatically.</p>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    // Configure and connect to WiFi
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_wifi_disconnect();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, new_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, new_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi connecting to: %s", new_ssid);
    return ESP_OK;
}

/** API endpoint for status JSON */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    wifi_ap_record_t ap_info = {0};
    int rssi = 0;
    if (wifi_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    // Check Powerwall
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_powerwall_check > 5000) {
        check_powerwall_connectivity();
    }

    // Calculate uptime in seconds
    int64_t uptime_sec = (esp_timer_get_time() - boot_time_us) / 1000000;

    char response[512];
    snprintf(response, sizeof(response),
        "{\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},"
        "\"powerwall\":{\"reachable\":%s,\"ip\":\"%s\"},"
        "\"cpu\":%u,\"heap\":%lu,"
        "\"uptime\":%lld,"
        "\"total_bytes_in\":%llu,\"total_bytes_out\":%llu,"
        "\"total_requests\":%lu,\"successful_requests\":%lu,\"failed_requests\":%lu}",
        wifi_connected ? "true" : "false",
        wifi_ssid, rssi,
        powerwall_reachable ? "true" : "false",
        POWERWALL_IP_STR,
        cpu_usage_percent,
        (unsigned long)esp_get_free_heap_size(),
        (long long)uptime_sec,
        (unsigned long long)total_bytes_in, (unsigned long long)total_bytes_out,
        (unsigned long)total_requests, (unsigned long)successful_requests, (unsigned long)failed_requests);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** API endpoint for RSSI value only */
static esp_err_t api_rssi_handler(httpd_req_t *req)
{
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    int rssi = 0;
    if (wifi_connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    char response[16];
    snprintf(response, sizeof(response), "%d", rssi);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** API endpoint for recent requests */
static esp_err_t api_requests_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"avg_ttfb\":%lu,\"requests\":[", (unsigned long)avg_ttfb_ms);
    httpd_resp_sendstr_chunk(req, buf);

    if (request_log_mutex && xSemaphoreTake(request_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int64_t now = esp_timer_get_time() / 1000000;
        bool first = true;
        for (int i = 0; i < REQUEST_LOG_SIZE; i++) {
            int idx = (request_log_index - 1 - i + REQUEST_LOG_SIZE) % REQUEST_LOG_SIZE;
            request_log_entry_t *e = &request_log[idx];
            if (!e->valid) continue;

            int64_t age = now - e->timestamp;
            uint8_t *ip = (uint8_t *)&e->source_ip;

            snprintf(buf, sizeof(buf),
                "%s{\"age\":%lld,\"ip\":\"%d.%d.%d.%d\",\"in\":%lu,\"out\":%lu,\"ttfb\":%u,\"ttlb\":%u,\"ok\":%d}",
                first ? "" : ",",
                (long long)age, ip[0], ip[1], ip[2], ip[3],
                (unsigned long)e->bytes_in, (unsigned long)e->bytes_out,
                e->ttfb_ms, e->ttlb_ms, e->result == 0 ? 1 : 0);
            httpd_resp_sendstr_chunk(req, buf);
            first = false;
        }
        xSemaphoreGive(request_log_mutex);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/** API endpoint for system logs */
static esp_err_t api_logs_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"logs\":[");

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool first = true;
        char buf[320];

        for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
            // Read in reverse order (most recent first)
            int idx = (log_buffer_index - 1 - i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
            log_entry_t *e = &log_buffer[idx];
            if (e->timestamp == 0 && e->message[0] == '\0') continue;

            // Escape special JSON characters in message (limit to 100 chars to fit buffer)
            char escaped[128];
            char *out = escaped;
            for (const char *in = e->message; *in && (out - escaped) < (int)sizeof(escaped) - 6; in++) {
                switch (*in) {
                    case '"':  *out++ = '\\'; *out++ = '"'; break;
                    case '\\': *out++ = '\\'; *out++ = '\\'; break;
                    case '\n': *out++ = '\\'; *out++ = 'n'; break;
                    case '\r': *out++ = '\\'; *out++ = 'r'; break;
                    case '\t': *out++ = '\\'; *out++ = 't'; break;
                    default:
                        if ((unsigned char)*in >= 32) *out++ = *in;
                        break;
                }
            }
            *out = '\0';

            snprintf(buf, sizeof(buf),
                "%s{\"ts\":%lld,\"lvl\":%u,\"msg\":\"%s\"}",
                first ? "" : ",",
                (long long)e->timestamp, e->level, escaped);
            httpd_resp_sendstr_chunk(req, buf);
            first = false;
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/** Reboot handler */
static esp_err_t reboot_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Manual reboot requested");

    const char *response = "<!DOCTYPE html><html><head><title>Reboot</title>"
        "<meta http-equiv='refresh' content='10;url=/'>"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".box{background:#1e293b;padding:2rem;border-radius:0.75rem;text-align:center;border:1px solid #334155}"
        ".spinner{width:3rem;height:3rem;border:3px solid #334155;border-top:3px solid #3b82f6;border-radius:50%;animation:spin 1s linear infinite;margin:1rem auto}"
        "@keyframes spin{to{transform:rotate(360deg)}}</style></head>"
        "<body><div class=\"box\"><div class=\"spinner\"></div>"
        "<h2>Rebooting...</h2>"
        "<p>Device is restarting. Page will refresh automatically.</p>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

/** Manual rollback handler */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Manual rollback requested");

    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();

    if (last_invalid == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No previous partition to rollback to");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_set_boot_partition(last_invalid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rollback failed");
        return ESP_FAIL;
    }

    const char *response = "<!DOCTYPE html><html><head><title>Rollback</title>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "</head><body><p>Rolling back to previous firmware... Rebooting.</p></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

// ===== Remote OTA Functions =====

/** Simple JSON string parser - finds value for a key in JSON string */
static bool json_get_string(const char *json, const char *key, char *value, size_t value_size)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    char *key_pos = strstr(json, search_key);
    if (!key_pos) return false;

    char *colon = strchr(key_pos + strlen(search_key), ':');
    if (!colon) return false;

    // Skip whitespace and find opening quote
    char *start = colon + 1;
    while (*start == ' ' || *start == '\t') start++;
    if (*start != '"') return false;
    start++;

    // Find closing quote
    char *end = strchr(start, '"');
    if (!end) return false;

    size_t len = end - start;
    if (len >= value_size) len = value_size - 1;
    strncpy(value, start, len);
    value[len] = '\0';
    return true;
}

/** Simple JSON number parser - finds integer value for a key */
static bool json_get_int(const char *json, const char *key, uint32_t *value)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    char *key_pos = strstr(json, search_key);
    if (!key_pos) return false;

    char *colon = strchr(key_pos + strlen(search_key), ':');
    if (!colon) return false;

    *value = strtoul(colon + 1, NULL, 10);
    return true;
}

/** Compare version strings (simple: returns 1 if v1 > v2, -1 if v1 < v2, 0 if equal) */
static int version_compare(const char *v1, const char *v2)
{
    // Handle empty or "none" versions
    if (!v1 || !v2 || strlen(v1) == 0 || strlen(v2) == 0) return 0;
    if (strcmp(v1, "none") == 0 || strcmp(v2, "none") == 0) return 0;

    return strcmp(v1, v2);
}

/** Check for remote firmware updates from GitHub */
static void check_for_remote_update(void)
{
    if (!remote_ota_mutex) {
        remote_ota_mutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (remote_ota.check_in_progress || remote_ota.install_in_progress) {
        xSemaphoreGive(remote_ota_mutex);
        return;
    }

    remote_ota.check_in_progress = true;
    xSemaphoreGive(remote_ota_mutex);

    ESP_LOGI(TAG, "Checking for remote firmware update...");

    // Configure HTTP client for GitHub (with certificate verification)
    esp_http_client_config_t config = {
        .url = REMOTE_OTA_VERSION_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for update check");
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > 1024) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        goto cleanup;
    }

    char *buffer = malloc(content_length + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for version.json");
        goto cleanup;
    }

    int read_len = esp_http_client_read(client, buffer, content_length);
    if (read_len != content_length) {
        ESP_LOGE(TAG, "Failed to read version.json: %d/%d", read_len, content_length);
        free(buffer);
        goto cleanup;
    }
    buffer[read_len] = '\0';

    ESP_LOGI(TAG, "Received version.json: %s", buffer);

    // Parse version.json
    char version[32] = {0};
    char url[256] = {0};
    uint32_t size = 0;
    char prev_version[32] = {0};
    char prev_url[256] = {0};
    uint32_t prev_size = 0;

    if (!json_get_string(buffer, "version", version, sizeof(version))) {
        ESP_LOGE(TAG, "Failed to parse version from JSON");
        free(buffer);
        goto cleanup;
    }

    json_get_string(buffer, "url", url, sizeof(url));
    json_get_int(buffer, "size", &size);
    json_get_string(buffer, "previous_version", prev_version, sizeof(prev_version));
    json_get_string(buffer, "previous_url", prev_url, sizeof(prev_url));
    json_get_int(buffer, "previous_size", &prev_size);

    free(buffer);

    // Compare with current version
    const esp_app_desc_t *app_desc = esp_app_get_description();

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(remote_ota.available_version, version, sizeof(remote_ota.available_version) - 1);
        strncpy(remote_ota.download_url, url, sizeof(remote_ota.download_url) - 1);
        remote_ota.firmware_size = size;
        strncpy(remote_ota.previous_version, prev_version, sizeof(remote_ota.previous_version) - 1);
        strncpy(remote_ota.previous_url, prev_url, sizeof(remote_ota.previous_url) - 1);
        remote_ota.previous_size = prev_size;
        remote_ota.last_check_time = esp_timer_get_time() / 1000000;

        // Update available if remote version is different and newer
        remote_ota.update_available = (version_compare(version, app_desc->version) != 0);
        remote_ota.previous_available = (strlen(prev_version) > 0 && strcmp(prev_version, "none") != 0);

        ESP_LOGI(TAG, "Remote version: %s, Current: %s, Update available: %s",
                 version, app_desc->version, remote_ota.update_available ? "yes" : "no");

        xSemaphoreGive(remote_ota_mutex);
    }

cleanup:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        remote_ota.check_in_progress = false;
        xSemaphoreGive(remote_ota_mutex);
    }
}

/** Perform OTA update from remote URL */
static void perform_remote_ota_task(void *pvParameters)
{
    const char *url = (const char *)pvParameters;

    ESP_LOGI(TAG, "Starting remote OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t err = esp_https_ota(&ota_config);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Remote OTA successful! Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Remote OTA failed: %s", esp_err_to_name(err));
        if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            remote_ota.install_in_progress = false;
            xSemaphoreGive(remote_ota_mutex);
        }
    }

    vTaskDelete(NULL);
}

/** API endpoint for update status */
static esp_err_t api_update_status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    char response[512];

    if (remote_ota_mutex && xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(response, sizeof(response),
            "{\"current_version\":\"%s\","
            "\"available_version\":\"%s\","
            "\"update_available\":%s,"
            "\"firmware_size\":%lu,"
            "\"previous_version\":\"%s\","
            "\"previous_available\":%s,"
            "\"check_in_progress\":%s,"
            "\"install_in_progress\":%s,"
            "\"last_check\":%lld}",
            app_desc->version,
            remote_ota.available_version,
            remote_ota.update_available ? "true" : "false",
            (unsigned long)remote_ota.firmware_size,
            remote_ota.previous_version,
            remote_ota.previous_available ? "true" : "false",
            remote_ota.check_in_progress ? "true" : "false",
            remote_ota.install_in_progress ? "true" : "false",
            (long long)remote_ota.last_check_time);
        xSemaphoreGive(remote_ota_mutex);
    } else {
        snprintf(response, sizeof(response),
            "{\"current_version\":\"%s\",\"error\":\"mutex\"}",
            app_desc->version);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** API endpoint to trigger update check */
static esp_err_t api_check_update_handler(httpd_req_t *req)
{
    // Check in background to avoid blocking
    xTaskCreate(
        (TaskFunction_t)check_for_remote_update,
        "update_check",
        4096,
        NULL,
        3,
        NULL
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
    return ESP_OK;
}

/** API endpoint to install update */
static esp_err_t api_install_update_handler(httpd_req_t *req)
{
    if (!remote_ota_mutex) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA not initialized");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mutex error");
        return ESP_FAIL;
    }

    if (!remote_ota.update_available || strlen(remote_ota.download_url) == 0) {
        xSemaphoreGive(remote_ota_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No update available");
        return ESP_FAIL;
    }

    if (remote_ota.install_in_progress) {
        xSemaphoreGive(remote_ota_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Install already in progress");
        return ESP_FAIL;
    }

    remote_ota.install_in_progress = true;

    // Copy URL for task (static buffer since struct lives forever)
    static char url_copy[256];
    strncpy(url_copy, remote_ota.download_url, sizeof(url_copy) - 1);

    xSemaphoreGive(remote_ota_mutex);

    // Start OTA in background task
    xTaskCreate(
        perform_remote_ota_task,
        "remote_ota",
        8192,
        url_copy,
        5,
        NULL
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"installing\"}");
    return ESP_OK;
}

/** API endpoint to revert to previous version */
static esp_err_t api_revert_handler(httpd_req_t *req)
{
    if (!remote_ota_mutex) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA not initialized");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mutex error");
        return ESP_FAIL;
    }

    if (!remote_ota.previous_available || strlen(remote_ota.previous_url) == 0) {
        xSemaphoreGive(remote_ota_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No previous version available");
        return ESP_FAIL;
    }

    if (remote_ota.install_in_progress) {
        xSemaphoreGive(remote_ota_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Install already in progress");
        return ESP_FAIL;
    }

    remote_ota.install_in_progress = true;

    // Copy URL for task
    static char prev_url_copy[256];
    strncpy(prev_url_copy, remote_ota.previous_url, sizeof(prev_url_copy) - 1);

    xSemaphoreGive(remote_ota_mutex);

    ESP_LOGI(TAG, "Reverting to previous version from: %s", prev_url_copy);

    // Start OTA in background task
    xTaskCreate(
        perform_remote_ota_task,
        "revert_ota",
        8192,
        prev_url_copy,
        5,
        NULL
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"reverting\"}");
    return ESP_OK;
}

/** Start the HTTP server (port 80) - serves web UI and OTA */
static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_HTTP_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 18;

    esp_err_t err = httpd_start(&web_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    // Main status page
    httpd_uri_t status_page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ota_status_handler,
    };
    httpd_register_uri_handler(web_server, &status_page);

    // OTA upload endpoint
    httpd_uri_t ota_upload = {
        .uri = "/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
    };
    httpd_register_uri_handler(web_server, &ota_upload);

    // OTA rollback endpoint
    httpd_uri_t ota_rollback = {
        .uri = "/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
    };
    httpd_register_uri_handler(web_server, &ota_rollback);

    // Reboot endpoint
    httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
    };
    httpd_register_uri_handler(web_server, &reboot);

    // WiFi configuration endpoints
    httpd_uri_t wifi_scan = {
        .uri = "/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_handler,
    };
    httpd_register_uri_handler(web_server, &wifi_scan);

    httpd_uri_t wifi_save = {
        .uri = "/wifi/save",
        .method = HTTP_POST,
        .handler = wifi_save_handler,
    };
    httpd_register_uri_handler(web_server, &wifi_save);

    // API endpoints
    httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(web_server, &api_status);

    httpd_uri_t api_rssi = {
        .uri = "/api/rssi",
        .method = HTTP_GET,
        .handler = api_rssi_handler,
    };
    httpd_register_uri_handler(web_server, &api_rssi);

    httpd_uri_t api_requests = {
        .uri = "/api/requests",
        .method = HTTP_GET,
        .handler = api_requests_handler,
    };
    httpd_register_uri_handler(web_server, &api_requests);

    httpd_uri_t api_logs = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_handler,
    };
    httpd_register_uri_handler(web_server, &api_logs);

    // Remote OTA endpoints
    httpd_uri_t api_update = {
        .uri = "/api/update",
        .method = HTTP_GET,
        .handler = api_update_status_handler,
    };
    httpd_register_uri_handler(web_server, &api_update);

    httpd_uri_t api_check_update = {
        .uri = "/api/check-update",
        .method = HTTP_POST,
        .handler = api_check_update_handler,
    };
    httpd_register_uri_handler(web_server, &api_check_update);

    httpd_uri_t api_install_update = {
        .uri = "/api/install-update",
        .method = HTTP_POST,
        .handler = api_install_update_handler,
    };
    httpd_register_uri_handler(web_server, &api_install_update);

    httpd_uri_t api_revert = {
        .uri = "/api/revert",
        .method = HTTP_POST,
        .handler = api_revert_handler,
    };
    httpd_register_uri_handler(web_server, &api_revert);

    ESP_LOGI(TAG, "HTTP server started on port %d", WEB_HTTP_PORT);
    return ESP_OK;
}

/** Validate the running firmware (call after successful boot) */
static void validate_ota_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA - validating new firmware...");
            // Mark as valid - firmware booted successfully
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware validated successfully!");
        }
    }
}

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        xEventGroupSetBits(s_event_group, ETH_CONNECTED_BIT);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        xEventGroupClearBits(s_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        xEventGroupClearBits(s_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    xEventGroupSetBits(s_event_group, ETH_GOT_IP_BIT);
}

/** Event handler for WiFi events */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
}

/** Event handler for IP_EVENT_STA_GOT_IP */
static void wifi_got_ip_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG, "WiFi got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
}

/** Initialize W5500 Ethernet */
static esp_err_t init_ethernet(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet W5500...");

    // Create event group
    s_event_group = xEventGroupCreate();

    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default event loop for Ethernet
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = W5500_MOSI_GPIO,
        .miso_io_num = W5500_MISO_GPIO,
        .sclk_io_num = W5500_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Configure SPI device for W5500
    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,  // 20 MHz
        .spics_io_num = W5500_CS_GPIO,
        .queue_size = 20,
        .cs_ena_posttrans = 1,
    };

    // Configure W5500
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI3_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = W5500_INT_GPIO;

    // Configure MAC and PHY
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    // Clone MAC address from the built-in WiFi interface
    // W5500 doesn't have a burned-in MAC, so we derive one from the chip's base MAC
    uint8_t mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_WIFI_STA));
    // Modify locally-administered bit to indicate this is a derived address
    // This ensures the Ethernet MAC is unique but related to the WiFi MAC
    mac_addr[0] = (mac_addr[0] | 0x02) & 0xFE;  // Set local bit, clear multicast bit
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
    ESP_LOGI(TAG, "Ethernet MAC (derived from WiFi): %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    // Set hostname for DHCP (powerwall-XXXX using last 2 bytes of MAC)
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "%s-%02X%02X", MDNS_HOSTNAME, mac_addr[4], mac_addr[5]);
    ESP_ERROR_CHECK(esp_netif_set_hostname(eth_netif, hostname));
    ESP_LOGI(TAG, "DHCP hostname set to: %s", hostname);

    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Start Ethernet driver
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet initialized - waiting for connection...");
    return ESP_OK;
}

/** Initialize WiFi Station mode - only if credentials are configured */
static esp_err_t init_wifi(void)
{
    // Load saved WiFi credentials from NVS
    bool has_credentials = load_wifi_credentials();

    // Create default WiFi station (needed for scanning even without credentials)
    wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_got_ip_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (has_credentials) {
        // Configure and start WiFi with saved credentials
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi initialized - connecting to %s", wifi_ssid);
    } else {
        // Start WiFi without connecting (allows scanning)
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGW(TAG, "WiFi initialized but no credentials configured - use web UI to configure");
    }

    return ESP_OK;
}

/** Initialize mDNS with simple hostname (powerwall.local) */
static void init_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());

    // Use simple hostname for mDNS (powerwall.local) for easy discovery
    // DHCP hostname keeps the MAC suffix for unique identification
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_LOGI(TAG, "mDNS hostname set to: %s.local", MDNS_HOSTNAME);

    // Get firmware version for TXT records
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // Create TXT records with device info (using runtime wifi_ssid)
    mdns_txt_item_t txt_records[] = {
        {"wifi_ssid", wifi_ssid},
        {"target", POWERWALL_IP_STR},
        {"ota_port", "80"},
        {"version", app_desc->version},
    };

    // Add _powerwall._tcp service for proxy discovery (port 443)
    mdns_service_add("powerwall", MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT, txt_records,
                     sizeof(txt_records) / sizeof(txt_records[0]));
    ESP_LOGI(TAG, "mDNS service added: powerwall.%s.%s on port %d", MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT);

    // Add _http._tcp service for status/OTA web UI
    mdns_txt_item_t http_txt[] = {
        {"path", "/"},
        {"wifi_ssid", wifi_ssid},
        {"version", app_desc->version},
    };
    mdns_service_add("Powerwall Bridge", "_http", "_tcp", WEB_HTTP_PORT, http_txt,
                     sizeof(http_txt) / sizeof(http_txt[0]));
    ESP_LOGI(TAG, "mDNS service added: _http._tcp on port %d", WEB_HTTP_PORT);
}

/** WiFi quality monitoring task - periodically logs connection quality */
static void wifi_quality_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi quality monitoring started (interval: %d seconds)", WIFI_QUALITY_LOG_INTERVAL_SEC);
    
    while (1) {
        // Wait for the configured interval
        vTaskDelay(pdMS_TO_TICKS(WIFI_QUALITY_LOG_INTERVAL_SEC * 1000));
        
        // Check if WiFi is connected
        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            wifi_ap_record_t ap_info;
            esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
            
            if (err == ESP_OK) {
                // Log WiFi connection quality metrics
                ESP_LOGI(TAG, "WiFi Quality - RSSI: %d dBm, Channel: %d, Auth: %d", 
                         ap_info.rssi, ap_info.primary, ap_info.authmode);
                
                // Provide quality interpretation
                if (ap_info.rssi > -50) {
                    ESP_LOGI(TAG, "WiFi Signal: Excellent");
                } else if (ap_info.rssi > -60) {
                    ESP_LOGI(TAG, "WiFi Signal: Good");
                } else if (ap_info.rssi > -70) {
                    ESP_LOGI(TAG, "WiFi Signal: Fair");
                } else {
                    ESP_LOGW(TAG, "WiFi Signal: Weak");
                }
            } else {
                ESP_LOGW(TAG, "Failed to get WiFi AP info: %d", err);
            }
        } else {
            ESP_LOGW(TAG, "WiFi not connected - skipping quality check");
        }
    }
}

/** System monitoring task - periodically logs system metrics and calculates CPU usage */
static void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System monitoring started (interval: %d seconds)", SYSTEM_MONITOR_INTERVAL_SEC);

    // For CPU usage calculation - track idle task runtime
    #if configGENERATE_RUN_TIME_STATS
    TaskHandle_t idle_task_0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle_task_1 = xTaskGetIdleTaskHandleForCore(1);
    uint32_t prev_idle_0 = 0, prev_idle_1 = 0;
    int64_t prev_time_us = 0;
    TaskStatus_t task_status;
    #endif

    while (1) {
        // Wait for the configured interval
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_MONITOR_INTERVAL_SEC * 1000));

        // Calculate CPU usage from idle task runtime
        #if configGENERATE_RUN_TIME_STATS
        uint32_t idle_runtime_0 = 0, idle_runtime_1 = 0;

        // Get idle task runtimes (in microseconds when using ESP_TIMER)
        if (idle_task_0) {
            vTaskGetInfo(idle_task_0, &task_status, pdFALSE, eRunning);
            idle_runtime_0 = task_status.ulRunTimeCounter;
        }
        if (idle_task_1) {
            vTaskGetInfo(idle_task_1, &task_status, pdFALSE, eRunning);
            idle_runtime_1 = task_status.ulRunTimeCounter;
        }

        // Get current time in microseconds (same unit as runtime counters)
        int64_t current_time_us = esp_timer_get_time();

        if (prev_time_us > 0) {
            // Delta time in microseconds for this interval
            int64_t delta_time_us = current_time_us - prev_time_us;
            // Total available CPU time = delta * 2 cores (in microseconds)
            int64_t total_cpu_time_us = delta_time_us * 2;
            // Delta idle time (both cores combined)
            uint32_t delta_idle = (idle_runtime_0 - prev_idle_0) + (idle_runtime_1 - prev_idle_1);

            if (total_cpu_time_us > 0) {
                // CPU usage = 100 - idle_percentage
                int64_t idle_pct = (delta_idle * 100LL) / total_cpu_time_us;
                cpu_usage_percent = (idle_pct > 100) ? 0 : (uint8_t)(100 - idle_pct);
            }
        }

        prev_idle_0 = idle_runtime_0;
        prev_idle_1 = idle_runtime_1;
        prev_time_us = current_time_us;
        #endif

        // Get free heap size
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();

        // Log system status
        #if configGENERATE_RUN_TIME_STATS
        ESP_LOGI(TAG, "System Status - CPU: %u%%, Heap: %lu KB free, Min: %lu KB",
                 cpu_usage_percent, (unsigned long)(free_heap / 1024),
                 (unsigned long)(min_free_heap / 1024));
        #else
        ESP_LOGI(TAG, "System Status - Heap: %lu KB free, Min: %lu KB",
                 (unsigned long)(free_heap / 1024),
                 (unsigned long)(min_free_heap / 1024));
        #endif

        // Warn on low heap
        if (free_heap < 20000) {
            ESP_LOGW(TAG, "Heap Status: Critical - Low memory!");
        }
    }
}

/** SSL/TLS Passthrough Proxy task - forwards encrypted packets without decryption */
static void handle_client_task(void *pvParameters)
{
    int client_sock = (int)pvParameters;
    int buffer_index = -1;

    // Per-request tracking for TTFB/TTLB measurement
    TickType_t request_start_time = 0;
    uint32_t request_bytes_in = 0;
    uint32_t request_bytes_out = 0;
    bool awaiting_first_byte = false;
    uint16_t current_ttfb_ms = 0;
    uint16_t current_ttlb_ms = 0;
    uint8_t request_result = 0;  // 0=success, 1=timeout, 2=error

    // Get source IP
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    uint32_t source_ip = 0;
    if (getpeername(client_sock, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
        source_ip = peer_addr.sin_addr.s_addr;
    }

    ESP_LOGI(TAG, "Handling client connection (SSL passthrough mode)");

    // Acquire buffer pair from pool (avoids malloc/free overhead)
    buffer_index = acquire_buffer_pair();
    if (buffer_index < 0) {
        ESP_LOGE(TAG, "No buffers available - max concurrent clients (%d) reached", MAX_CONCURRENT_CLIENTS);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Connect to Powerwall via TCP (no TLS, just raw socket)
    struct sockaddr_in powerwall_addr;
    powerwall_addr.sin_family = AF_INET;
    powerwall_addr.sin_port = htons(443);
    inet_pton(AF_INET, POWERWALL_IP_STR, &powerwall_addr.sin_addr);

    int powerwall_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (powerwall_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket to Powerwall");
        release_buffer_pair(buffer_index);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Set TTL to hide that traffic is coming from outside the network
    // Common TTL values: 64 (Linux/Unix), 128 (Windows), 255 (Cisco)
    // Using 64 as it's the most common default
    int ttl = TTL_VALUE;
    if (setsockopt(powerwall_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
        ESP_LOGW(TAG, "Failed to set TTL on socket: %d", errno);
    } else {
        ESP_LOGI(TAG, "Set TTL to %d on outgoing connection", ttl);
    }

    // Set timeouts on both sockets
    struct timeval timeout = {.tv_sec = PROXY_TIMEOUT_MS / 1000, .tv_usec = (PROXY_TIMEOUT_MS % 1000) * 1000};
    if (setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set timeout on client socket: %d", errno);
    }
    if (setsockopt(powerwall_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set timeout on powerwall socket: %d", errno);
    }

    // Connect to Powerwall
    if (connect(powerwall_sock, (struct sockaddr *)&powerwall_addr, sizeof(powerwall_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to connect to Powerwall at %s:443 - error: %d", POWERWALL_IP_STR, errno);
        release_buffer_pair(buffer_index);
        close(powerwall_sock);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Disable Nagle's algorithm for lower latency on both sockets
    int nodelay = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    setsockopt(powerwall_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ESP_LOGI(TAG, "Connected to Powerwall at %s:443 (encrypted passthrough)", POWERWALL_IP_STR);

    // Get buffer pointers from the preallocated pool
    uint8_t *client_buffer = buffer_pool[buffer_index].client_buffer;
    uint8_t *powerwall_buffer = buffer_pool[buffer_index].powerwall_buffer;

    // Set both sockets to non-blocking mode for bidirectional forwarding
    int flags = fcntl(client_sock, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGW(TAG, "Failed to set client socket to non-blocking mode: %d", errno);
        }
    } else {
        ESP_LOGW(TAG, "Failed to get client socket flags: %d", errno);
    }
    
    flags = fcntl(powerwall_sock, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(powerwall_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGW(TAG, "Failed to set powerwall socket to non-blocking mode: %d", errno);
        }
    } else {
        ESP_LOGW(TAG, "Failed to get powerwall socket flags: %d", errno);
    }

    TickType_t last_activity = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(PROXY_TIMEOUT_MS);

    // Bidirectional forwarding loop using select() for efficient I/O multiplexing
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_sock, &read_fds);
        FD_SET(powerwall_sock, &read_fds);
        
        int max_fd = (client_sock > powerwall_sock) ? client_sock : powerwall_sock;
        
        // Use select with a small timeout for activity checking
        struct timeval select_timeout = {.tv_sec = 0, .tv_usec = 100000}; // 100ms
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &select_timeout);
        
        if (ready < 0) {
            ESP_LOGE(TAG, "select() error: %d", errno);
            break;
        } else if (ready == 0) {
            // Timeout - check for inactivity timeout
            if ((xTaskGetTickCount() - last_activity) > timeout_ticks) {
                ESP_LOGI(TAG, "Connection timeout - no activity for %d ms", PROXY_TIMEOUT_MS);
                request_result = 1;  // Timeout
                break;
            }
            continue;
        }

        // Client -> Powerwall: Forward encrypted data
        if (FD_ISSET(client_sock, &read_fds)) {
            int len = recv(client_sock, client_buffer, PROXY_BUFFER_SIZE, 0);
            if (len > 0) {
                // If we were waiting for response and got new request data,
                // log the previous request/response exchange
                if (request_bytes_out > 0 && !awaiting_first_byte) {
                    log_request(source_ip, request_bytes_in, request_bytes_out, current_ttfb_ms, current_ttlb_ms, request_result);
                    request_bytes_in = 0;
                    request_bytes_out = 0;
                    current_ttfb_ms = 0;
                    current_ttlb_ms = 0;
                    request_result = 0;
                }

                // Start timing new request
                if (!awaiting_first_byte) {
                    request_start_time = xTaskGetTickCount();
                    awaiting_first_byte = true;
                }

                // Forward encrypted data to Powerwall
                int total_sent = 0;
                while (total_sent < len) {
                    int sent = send(powerwall_sock, client_buffer + total_sent, len - total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Wait for socket to become writable using select()
                            fd_set write_fds;
                            FD_ZERO(&write_fds);
                            FD_SET(powerwall_sock, &write_fds);
                            struct timeval write_timeout = {.tv_sec = 5, .tv_usec = 0};
                            if (select(powerwall_sock + 1, NULL, &write_fds, NULL, &write_timeout) <= 0) {
                                ESP_LOGE(TAG, "Timeout waiting for Powerwall socket writability");
                                goto cleanup;
                            }
                            continue;
                        }
                        ESP_LOGE(TAG, "Error sending to Powerwall: %d", errno);
                        goto cleanup;
                    }
                    total_sent += sent;
                }
                
                last_activity = xTaskGetTickCount();
                request_bytes_in += len;
                total_bytes_in += len;

                #if DEBUG_MODE
                ESP_LOGI(TAG, "Forwarded %d bytes from client to Powerwall (encrypted)", len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, client_buffer, len < 64 ? len : 64, ESP_LOG_INFO);
                #endif
            } else if (len == 0) {
                ESP_LOGI(TAG, "Client closed connection");
                break;
            } else {
                ESP_LOGE(TAG, "Error reading from client: %d", errno);
                request_result = 2;  // Error
                break;
            }
        }

        // Powerwall -> Client: Forward encrypted data
        if (FD_ISSET(powerwall_sock, &read_fds)) {
            int len = recv(powerwall_sock, powerwall_buffer, PROXY_BUFFER_SIZE, 0);
            if (len > 0) {
                // Calculate TTFB on first response byte
                if (awaiting_first_byte) {
                    TickType_t ttfb_ticks = xTaskGetTickCount() - request_start_time;
                    uint32_t ttfb_ms = ttfb_ticks * portTICK_PERIOD_MS;
                    current_ttfb_ms = (ttfb_ms > 65535) ? 65535 : ttfb_ms;
                    awaiting_first_byte = false;
                    #if DEBUG_MODE
                    ESP_LOGI(TAG, "TTFB: %u ms", current_ttfb_ms);
                    #endif
                }
                
                // Forward encrypted data to client
                int total_sent = 0;
                while (total_sent < len) {
                    int sent = send(client_sock, powerwall_buffer + total_sent, len - total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Wait for socket to become writable using select()
                            fd_set write_fds;
                            FD_ZERO(&write_fds);
                            FD_SET(client_sock, &write_fds);
                            struct timeval write_timeout = {.tv_sec = 5, .tv_usec = 0};
                            if (select(client_sock + 1, NULL, &write_fds, NULL, &write_timeout) <= 0) {
                                ESP_LOGE(TAG, "Timeout waiting for client socket writability");
                                goto cleanup;
                            }
                            continue;
                        }
                        ESP_LOGE(TAG, "Error sending to client: %d", errno);
                        goto cleanup;
                    }
                    total_sent += sent;
                }
                
                last_activity = xTaskGetTickCount();
                request_bytes_out += len;
                total_bytes_out += len;

                // Update TTLB (time to last byte) - updated on each chunk
                TickType_t ttlb_ticks = last_activity - request_start_time;
                uint32_t ttlb_ms = ttlb_ticks * portTICK_PERIOD_MS;
                current_ttlb_ms = (ttlb_ms > 65535) ? 65535 : ttlb_ms;

                #if DEBUG_MODE
                ESP_LOGI(TAG, "Forwarded %d bytes from Powerwall to client (encrypted)", len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, powerwall_buffer, len < 64 ? len : 64, ESP_LOG_INFO);
                #endif
            } else if (len == 0) {
                ESP_LOGI(TAG, "Powerwall closed connection");
                break;
            } else {
                ESP_LOGE(TAG, "Error reading from Powerwall: %d", errno);
                request_result = 2;  // Error
                break;
            }
        }
    }

cleanup:
    // Log final request if any data was exchanged
    if (request_bytes_in > 0 || request_bytes_out > 0) {
        log_request(source_ip, request_bytes_in, request_bytes_out, current_ttfb_ms, current_ttlb_ms, request_result);
    }

    release_buffer_pair(buffer_index);
    close(powerwall_sock);
    close(client_sock);

    ESP_LOGI(TAG, "Client connection closed (passthrough mode)");
    vTaskDelete(NULL);
}

/** TCP Server task */
static void tcp_server_task(void *pvParameters)
{
    // Wait for Ethernet to get IP
    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    xEventGroupWaitBits(s_event_group, ETH_GOT_IP_BIT, false, true, portMAX_DELAY);

    // Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_socket < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PROXY_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_socket, 3) != 0) {
        ESP_LOGE(TAG, "Socket listen failed");
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP Server (SSL passthrough) listening on port %d", PROXY_PORT);
    ESP_LOGI(TAG, "Ready to forward encrypted SSL/TLS traffic to Powerwall (%s:443) with TTL modification", POWERWALL_IP_STR);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection");
            continue;
        }

        char addr_str[32];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Client connected from %s:%d", addr_str, ntohs(client_addr.sin_port));

        // Spawn a new task to handle each client connection
        // This allows multiple simultaneous connections
        BaseType_t task_created = xTaskCreate(handle_client_task, "ssl_passthrough", 
                                               SSL_PASSTHROUGH_TASK_STACK_SIZE, (void *)client_sock, 5, NULL);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client handler task");
            close(client_sock);
        }
    }

    close(server_socket);
    vTaskDelete(NULL);
}

/** Task to initialize WiFi-dependent services after connection */
static void wifi_services_task(void *pvParameters)
{
    // Wait for WiFi credentials to be configured (if not already)
    if (!wifi_configured) {
        ESP_LOGW(TAG, "No WiFi credentials configured - waiting for configuration via web UI");
        while (!wifi_configured) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "Waiting for WiFi configuration via web UI at http://<eth-ip>/");
        }
        ESP_LOGI(TAG, "WiFi credentials configured - connecting to %s", wifi_ssid);
    }

    // Wait for WiFi connection (with timeout for logging)
    ESP_LOGI(TAG, "Waiting for WiFi connection to %s...", wifi_ssid);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT,
                                                false, true, pdMS_TO_TICKS(30000));
        if (bits & WIFI_CONNECTED_BIT) {
            break;
        }
        ESP_LOGW(TAG, "WiFi not connected yet - check credentials via web UI at http://<eth-ip>/");
    }

    ESP_LOGI(TAG, "WiFi connected - starting proxy services");

    // Initialize buffer pool for proxy connections
    init_buffer_pool();

    // Start WiFi quality monitoring task
    xTaskCreate(wifi_quality_monitor_task, "wifi_monitor", 3072, NULL, 3, NULL);

    // Start TCP server task (proxy)
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Proxy services started - forwarding to %s:443", POWERWALL_IP_STR);

    vTaskDelete(NULL);
}

void app_main(void)
{
    // Record boot time for uptime calculation
    boot_time_us = esp_timer_get_time();

    // Initialize log capture early to capture startup logs
    init_log_capture();

    ESP_LOGI(TAG, "=== ESP32-S3-POE-ETH WiFi-Ethernet SSL Bridge ===");
    ESP_LOGI(TAG, "Mode: SSL Passthrough (no decryption, TTL modification)");
    ESP_LOGI(TAG, "Target: Tesla Powerwall at %s:443", POWERWALL_IP_STR);

    // Print firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware version: %s (built %s %s)", app_desc->version, app_desc->date, app_desc->time);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Ethernet first (OTA server runs on Ethernet)
    ESP_ERROR_CHECK(init_ethernet());

    // Initialize WiFi (starts connection attempt in background)
    ESP_ERROR_CHECK(init_wifi());

    // Wait for Ethernet to get IP before starting OTA server
    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    xEventGroupWaitBits(s_event_group, ETH_GOT_IP_BIT, false, true, portMAX_DELAY);

    // Start HTTP server immediately (on Ethernet interface)
    // This allows WiFi config even if WiFi credentials are wrong
    start_http_server();
    ESP_LOGI(TAG, "HTTP server started - http://<eth-ip>/");

    // Initialize mDNS on Ethernet (for device discovery, doesn't need WiFi)
    init_mdns();

    // Validate OTA image early so device doesn't rollback while user configures WiFi
    validate_ota_image();

    // Start system monitoring task
    xTaskCreate(system_monitor_task, "sys_monitor", 3072, NULL, 3, NULL);

    // Start WiFi-dependent services in background task
    // This allows OTA to remain responsive while waiting for WiFi
    xTaskCreate(wifi_services_task, "wifi_services", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "System initialized - configure WiFi via OTA UI if needed");
}
