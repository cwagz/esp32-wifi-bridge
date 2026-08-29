/*
 * Remote OTA - Firmware updates from GitHub
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "config.h"
#include "remote_ota.h"

static const char *TAG = "remote-ota";

// Module state
static remote_ota_state_t remote_ota = {0};
static SemaphoreHandle_t remote_ota_mutex = NULL;
static esp_netif_t *ota_netif = NULL;
static uint32_t eth_dns_main = 0;
static uint32_t eth_dns_backup = 0;

// ===== JSON Parsing Utilities =====

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

static void ota_set_error(const char *msg)
{
    if (!remote_ota_mutex) {
        return;
    }
    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    strncpy(remote_ota.last_error, msg ? msg : "", sizeof(remote_ota.last_error) - 1);
    remote_ota.last_error[sizeof(remote_ota.last_error) - 1] = '\0';
    xSemaphoreGive(remote_ota_mutex);
}

static void json_escape(const char *in, char *out, size_t n)
{
    size_t j = 0;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; in[i] && j + 2 < n; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if ((unsigned char)c >= 32) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

/** Tesla WiFi DHCP overwrites lwIP DNS (and esp_netif_get_dns_info) with
 *  192.168.91.1, which cannot resolve github.io. Snapshot the Ethernet
 *  nameserver from DHCP/static and force lwIP to use it. */
static bool dns_on_powerwall_lan(uint32_t addr)
{
    esp_ip4_addr_t a = { .addr = addr };
    return esp_ip4_addr_get_byte(&a, 0) == POWERWALL_IP_ADDR1 &&
           esp_ip4_addr_get_byte(&a, 1) == POWERWALL_IP_ADDR2 &&
           esp_ip4_addr_get_byte(&a, 2) == POWERWALL_IP_ADDR3;
}

bool remote_ota_remember_eth_dns(uint32_t dns_main, uint32_t dns_backup)
{
    if (dns_main == 0 || dns_on_powerwall_lan(dns_main)) {
        ESP_LOGW(TAG, "Not remembering DNS " IPSTR " (empty or Tesla AP)",
                 IP2STR((esp_ip4_addr_t *)&dns_main));
        return false;
    }
    eth_dns_main = dns_main;
    eth_dns_backup = dns_on_powerwall_lan(dns_backup) ? 0 : dns_backup;
    ESP_LOGI(TAG, "Remembered Ethernet DNS " IPSTR, IP2STR((esp_ip4_addr_t *)&eth_dns_main));
    return true;
}

void remote_ota_apply_eth_dns(void)
{
    if (eth_dns_main == 0) {
        return;
    }
    ip_addr_t primary;
    ip4_addr_t ip4 = { .addr = eth_dns_main };
    ip_addr_copy_from_ip4(primary, ip4);
    dns_setserver(0, &primary);

    ip_addr_t secondary;
    IP_ADDR4(&secondary, 0, 0, 0, 0);
    if (eth_dns_backup != 0) {
        ip4_addr_t b4 = { .addr = eth_dns_backup };
        ip_addr_copy_from_ip4(secondary, b4);
    }
    dns_setserver(1, &secondary);
}

static bool ota_use_ethernet_dns(void)
{
    if (eth_dns_main == 0) {
        ESP_LOGE(TAG, "OTA DNS: no Ethernet nameserver remembered (set DNS in static IP, or check DHCP option 6)");
        return false;
    }
    remote_ota_apply_eth_dns();
    if (eth_dns_backup != 0) {
        ESP_LOGI(TAG, "OTA DNS " IPSTR " / " IPSTR " (Ethernet)",
                 IP2STR((esp_ip4_addr_t *)&eth_dns_main),
                 IP2STR((esp_ip4_addr_t *)&eth_dns_backup));
    } else {
        ESP_LOGI(TAG, "OTA DNS " IPSTR " (Ethernet DHCP/static)",
                 IP2STR((esp_ip4_addr_t *)&eth_dns_main));
    }
    return true;
}

static bool ota_bind_ethernet(esp_netif_t **old_out)
{
    if (old_out) {
        *old_out = NULL;
    }
    if (!ota_netif) {
        ESP_LOGE(TAG, "OTA: Ethernet not ready");
        return false;
    }
    if (old_out) {
        *old_out = esp_netif_get_default_netif();
    }
    esp_netif_set_default_netif(ota_netif);
    ESP_LOGI(TAG, "Set Ethernet as default interface");
    return ota_use_ethernet_dns();
}

// ===== Update Check Task =====

/** Check for remote firmware updates from GitHub (runs as FreeRTOS task) */
static void check_for_remote_update_task(void *pvParameters)
{
    (void)pvParameters;

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        vTaskDelete(NULL);
        return;
    }

    if (remote_ota.check_in_progress || remote_ota.install_in_progress) {
        xSemaphoreGive(remote_ota_mutex);
        vTaskDelete(NULL);
        return;
    }

    remote_ota.check_in_progress = true;
    remote_ota.last_error[0] = '\0';
    xSemaphoreGive(remote_ota_mutex);

    ESP_LOGI(TAG, "Checking for remote firmware update from GitHub via Ethernet...");

    const char *fail = NULL;
    char fail_buf[48];
    esp_http_client_handle_t client = NULL;
    esp_netif_t *old_default = NULL;
    if (!ota_bind_ethernet(&old_default)) {
        fail = "Ethernet has no nameserver";
        ESP_LOGE(TAG, "Cannot reach GitHub: Ethernet DNS not configured");
        goto cleanup;
    }

    esp_http_client_config_t config = {
        .url = REMOTE_OTA_VERSION_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .user_agent = "ESP32-WiFi-Bridge/1.0",
    };

    client = esp_http_client_init(&config);
    if (!client) {
        fail = "HTTP client init failed";
        ESP_LOGE(TAG, "Failed to init HTTP client for update check");
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GitHub connect failed (%s), retry in 2s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        client = NULL;
        vTaskDelay(pdMS_TO_TICKS(2000));
        ota_use_ethernet_dns();
        client = esp_http_client_init(&config);
        if (!client) {
            fail = "HTTP client init failed";
            ESP_LOGE(TAG, "Failed to init HTTP client for update check");
            goto cleanup;
        }
        err = esp_http_client_open(client, 0);
    }
    if (err != ESP_OK) {
        snprintf(fail_buf, sizeof(fail_buf), "Connect failed: %s", esp_err_to_name(err));
        fail = fail_buf;
        ESP_LOGE(TAG, "Failed to connect to GitHub: %s (DNS, TLS, or no socket?)", esp_err_to_name(err));
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "version.json HTTP %d, content-length %d", status, content_length);
    if (status != 200) {
        snprintf(fail_buf, sizeof(fail_buf), "GitHub HTTP %d", status);
        fail = fail_buf;
        ESP_LOGE(TAG, "Unexpected HTTP status %d fetching version.json", status);
        goto cleanup;
    }

    char *buffer = malloc(1025);
    if (!buffer) {
        fail = "Out of memory";
        ESP_LOGE(TAG, "Failed to allocate buffer for version.json");
        goto cleanup;
    }

    int remaining = (content_length > 0 && content_length <= 1024) ? content_length : 1024;
    int total = 0;
    while (total < remaining) {
        int n = esp_http_client_read(client, buffer + total, remaining - total);
        if (n < 0) {
            fail = "Read version.json failed";
            ESP_LOGE(TAG, "Failed to read version.json after %d bytes", total);
            free(buffer);
            goto cleanup;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    buffer[total] = '\0';
    if (total == 0) {
        fail = "Empty version.json";
        ESP_LOGE(TAG, "Empty version.json");
        free(buffer);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Received version.json: %s", buffer);

    // Parse version.json
    char version[32] = {0};
    char url[256] = {0};
    uint32_t size = 0;
    char prev_version[32] = {0};
    char prev_url[256] = {0};
    uint32_t prev_size = 0;

    if (!json_get_string(buffer, "version", version, sizeof(version))) {
        fail = "Bad version.json";
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

        // Update available if remote version is different
        remote_ota.update_available = (version_compare(version, app_desc->version) != 0);
        remote_ota.previous_available = (strlen(prev_version) > 0 && strcmp(prev_version, "none") != 0);
        remote_ota.last_error[0] = '\0';

        ESP_LOGI(TAG, "Remote version: %s, Current: %s, Update available: %s",
                 version, app_desc->version, remote_ota.update_available ? "yes" : "no");

        xSemaphoreGive(remote_ota_mutex);
    }

cleanup:
    if (fail) {
        ota_set_error(fail);
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    // Restore old default interface
    if (old_default) {
        esp_netif_set_default_netif(old_default);
    }

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        remote_ota.check_in_progress = false;
        xSemaphoreGive(remote_ota_mutex);
    }

    vTaskDelete(NULL);
}

// ===== OTA Download Task =====

/** Perform OTA update from remote URL using chunked download with resume */
static void perform_remote_ota_task(void *pvParameters)
{
    const char *url = (const char *)pvParameters;
    esp_netif_t *old_default = NULL;
    esp_ota_handle_t ota_handle = 0;
    esp_http_client_handle_t http_client = NULL;
    const esp_partition_t *update_partition = NULL;
    esp_err_t err;

    ESP_LOGI(TAG, "=== Remote OTA Starting ===");
    ESP_LOGI(TAG, "URL: %s", url);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // Force GitHub traffic through Ethernet (Tesla WiFi has no internet)
    if (!ota_bind_ethernet(&old_default)) {
        ESP_LOGE(TAG, "Cannot reach GitHub: Ethernet DNS not configured");
        goto cleanup;
    }
    if (ota_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ota_netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "Using Ethernet - IP: " IPSTR, IP2STR(&ip_info.ip));
        }
    }

    // Find OTA partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        goto cleanup;
    }
    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    // Allocate download buffer
    const size_t chunk_size = 4096;
    char *buffer = malloc(chunk_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        goto cleanup;
    }

    // First, get the file size with a HEAD request
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .user_agent = "Mozilla/5.0 ESP32",
    };

    http_client = esp_http_client_init(&config);
    if (!http_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(buffer);
        goto cleanup;
    }

    // Get file size
    esp_http_client_set_method(http_client, HTTP_METHOD_HEAD);
    err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HEAD request failed: %s", esp_err_to_name(err));
        free(buffer);
        goto cleanup;
    }

    int file_size = esp_http_client_get_content_length(http_client);
    ESP_LOGI(TAG, "Firmware size: %d bytes (%.1f KB)", file_size, file_size / 1024.0);
    esp_http_client_cleanup(http_client);
    http_client = NULL;

    // Begin OTA
    err = esp_ota_begin(update_partition, file_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        free(buffer);
        goto cleanup;
    }

    // Download in chunks with Range requests (allows resume on connection drop)
    int downloaded = 0;
    int retries = 0;
    const int max_retries = 10;
    const int range_chunk = 65536;  // 64KB per request

    while (downloaded < file_size && retries < max_retries) {
        int range_end = downloaded + range_chunk - 1;
        if (range_end >= file_size) range_end = file_size - 1;

        // Create new connection for each chunk
        http_client = esp_http_client_init(&config);
        if (!http_client) {
            ESP_LOGE(TAG, "Failed to init HTTP client for chunk");
            retries++;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Set Range header
        char range_header[64];
        snprintf(range_header, sizeof(range_header), "bytes=%d-%d", downloaded, range_end);
        esp_http_client_set_header(http_client, "Range", range_header);
        esp_http_client_set_method(http_client, HTTP_METHOD_GET);

        err = esp_http_client_open(http_client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open connection: %s (retry %d)", esp_err_to_name(err), retries + 1);
            esp_http_client_cleanup(http_client);
            http_client = NULL;
            retries++;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int content_length = esp_http_client_fetch_headers(http_client);
        int status = esp_http_client_get_status_code(http_client);

        if (status != 206 && status != 200) {
            ESP_LOGW(TAG, "Unexpected HTTP status: %d (retry %d)", status, retries + 1);
            esp_http_client_close(http_client);
            esp_http_client_cleanup(http_client);
            http_client = NULL;
            retries++;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Read this chunk
        int chunk_downloaded = 0;
        bool chunk_error = false;

        while (chunk_downloaded < content_length) {
            int to_read = content_length - chunk_downloaded;
            if (to_read > (int)chunk_size) to_read = chunk_size;

            int read_len = esp_http_client_read(http_client, buffer, to_read);
            if (read_len <= 0) {
                ESP_LOGW(TAG, "Read error at offset %d (got %d bytes this chunk)",
                         downloaded + chunk_downloaded, chunk_downloaded);
                chunk_error = true;
                break;
            }

            // Write to flash
            err = esp_ota_write(ota_handle, buffer, read_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
                free(buffer);
                esp_http_client_cleanup(http_client);
                goto cleanup;
            }

            chunk_downloaded += read_len;
        }

        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        http_client = NULL;

        if (chunk_error) {
            retries++;
            ESP_LOGI(TAG, "Chunk failed, retrying from offset %d (retry %d/%d)",
                     downloaded, retries, max_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Chunk succeeded
        downloaded += chunk_downloaded;
        retries = 0;  // Reset retries on success

        int progress = (file_size > 0) ? (downloaded * 100 / file_size) : 0;
        ESP_LOGI(TAG, "Progress: %d%% (%d / %d bytes)", progress, downloaded, file_size);
    }

    free(buffer);

    if (downloaded < file_size) {
        ESP_LOGE(TAG, "Download incomplete: %d / %d bytes after %d retries",
                 downloaded, file_size, max_retries);
        goto cleanup;
    }

    // Finish OTA
    ESP_LOGI(TAG, "Download complete, verifying...");
    err = esp_ota_end(ota_handle);
    ota_handle = 0;

    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Firmware validation failed - corrupted");
        } else {
            ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        }
        goto cleanup;
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA successful! Rebooting in 2 seconds...");
    if (old_default) esp_netif_set_default_netif(old_default);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

cleanup:
    if (http_client) {
        esp_http_client_cleanup(http_client);
    }
    if (ota_handle) {
        esp_ota_abort(ota_handle);
    }
    if (old_default) {
        esp_netif_set_default_netif(old_default);
    }

    if (xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        remote_ota.install_in_progress = false;
        xSemaphoreGive(remote_ota_mutex);
    }

    ESP_LOGI(TAG, "=== Remote OTA Ended ===");
    vTaskDelete(NULL);
}

// ===== Public API =====

void remote_ota_init(esp_netif_t *netif)
{
    ota_netif = netif;
    remote_ota_mutex = xSemaphoreCreateMutex();
    remote_ota_apply_eth_dns();
    ESP_LOGI(TAG, "Remote OTA initialized");
}

void remote_ota_get_state(remote_ota_state_t *state)
{
    if (remote_ota_mutex && xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(state, &remote_ota, sizeof(remote_ota_state_t));
        xSemaphoreGive(remote_ota_mutex);
    }
}

esp_err_t api_update_status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    char response[640];
    char err_esc[96];

    if (remote_ota_mutex && xSemaphoreTake(remote_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        json_escape(remote_ota.last_error, err_esc, sizeof(err_esc));
        snprintf(response, sizeof(response),
            "{\"current_version\":\"%s\","
            "\"available_version\":\"%s\","
            "\"update_available\":%s,"
            "\"firmware_size\":%lu,"
            "\"previous_version\":\"%s\","
            "\"previous_available\":%s,"
            "\"check_in_progress\":%s,"
            "\"install_in_progress\":%s,"
            "\"last_check\":%lld,"
            "\"last_error\":\"%s\"}",
            app_desc->version,
            remote_ota.available_version,
            remote_ota.update_available ? "true" : "false",
            (unsigned long)remote_ota.firmware_size,
            remote_ota.previous_version,
            remote_ota.previous_available ? "true" : "false",
            remote_ota.check_in_progress ? "true" : "false",
            remote_ota.install_in_progress ? "true" : "false",
            (long long)remote_ota.last_check_time,
            err_esc);
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

esp_err_t api_check_update_handler(httpd_req_t *req)
{
    xTaskCreate(
        check_for_remote_update_task,
        "update_check",
        8192,
        NULL,
        3,
        NULL
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
    return ESP_OK;
}

esp_err_t api_install_update_handler(httpd_req_t *req)
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

esp_err_t api_revert_handler(httpd_req_t *req)
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

    static char prev_url_copy[256];
    strncpy(prev_url_copy, remote_ota.previous_url, sizeof(prev_url_copy) - 1);

    xSemaphoreGive(remote_ota_mutex);

    ESP_LOGI(TAG, "Reverting to previous version from: %s", prev_url_copy);

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
