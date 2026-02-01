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

#include "config.h"
#include "remote_ota.h"

static const char *TAG = "remote-ota";

// Module state
static remote_ota_state_t remote_ota = {0};
static SemaphoreHandle_t remote_ota_mutex = NULL;
static esp_netif_t *ota_netif = NULL;

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
    xSemaphoreGive(remote_ota_mutex);

    ESP_LOGI(TAG, "Checking for remote firmware update from GitHub via Ethernet...");

    esp_http_client_handle_t client = NULL;

    // Get interface name to force HTTP traffic through specified interface
    char if_name[8] = {0};
    if (ota_netif) {
        esp_netif_get_netif_impl_name(ota_netif, if_name);
        ESP_LOGI(TAG, "Using interface: %s", if_name);
    }

    // Configure HTTP client for GitHub (with certificate verification)
    esp_http_client_config_t config = {
        .url = REMOTE_OTA_VERSION_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .if_name = if_name[0] ? if_name : NULL,
    };

    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for update check");
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to GitHub: %s (no internet access?)", esp_err_to_name(err));
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

        // Update available if remote version is different
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

    vTaskDelete(NULL);
}

// ===== OTA Download Task =====

/** Perform OTA update from remote URL */
static void perform_remote_ota_task(void *pvParameters)
{
    const char *url = (const char *)pvParameters;

    ESP_LOGI(TAG, "Starting remote OTA from: %s", url);

    // Get interface name to force download through specified interface
    char if_name[8] = {0};
    if (ota_netif) {
        esp_netif_get_netif_impl_name(ota_netif, if_name);
        ESP_LOGI(TAG, "Downloading via interface: %s", if_name);
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .if_name = if_name[0] ? if_name : NULL,
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

// ===== Public API =====

void remote_ota_init(esp_netif_t *netif)
{
    ota_netif = netif;
    remote_ota_mutex = xSemaphoreCreateMutex();
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

esp_err_t api_check_update_handler(httpd_req_t *req)
{
    xTaskCreate(
        check_for_remote_update_task,
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
