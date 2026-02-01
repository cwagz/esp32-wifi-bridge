/*
 * WiFi Metrics - Track WiFi signal strength and connection success rate
 * Also handles NTP time synchronization using the Ethernet interface
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_netif.h"

#include "config.h"
#include "wifi_metrics.h"

static const char *TAG = "wifi_metrics";

// ===== Module State =====
static EventGroupHandle_t metrics_event_group = NULL;
static EventBits_t metrics_wifi_connected_bit = 0;
static esp_netif_t *metrics_eth_netif = NULL;

// Metrics storage
static wifi_metrics_bucket_t metrics_buckets[WIFI_METRICS_BUCKET_COUNT];
static int current_bucket_index = 0;
static SemaphoreHandle_t metrics_mutex = NULL;

// Current bucket accumulator (before averaging)
static int32_t bucket_rssi_sum = 0;
static uint32_t bucket_rssi_count = 0;
static uint32_t bucket_connected_samples = 0;
static uint32_t bucket_total_samples = 0;

// Running totals
static uint32_t total_connected_sec = 0;
static uint32_t total_disconnected_sec = 0;
static int8_t last_rssi = 0;
static bool last_connected = false;

// NTP state
static volatile bool ntp_synced = false;
static int64_t boot_time_utc = 0;  // UTC seconds at boot time

// ===== NTP Time Synchronization =====

/** NTP sync notification callback */
static void ntp_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronized");
    ntp_synced = true;

    // Calculate boot time in UTC
    time_t now = time(NULL);
    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    boot_time_utc = now - uptime_sec;

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s (UTC)", time_str);
}

/** Initialize NTP using the Ethernet interface (non-blocking) */
static void init_ntp(void)
{
    ESP_LOGI(TAG, "Initializing NTP time sync (non-blocking)");

    // Set timezone to UTC
    setenv("TZ", "UTC0", 1);
    tzset();

    // IMPORTANT: Set ethernet as the default netif so SNTP uses it
    // WiFi network (Powerwall) has no internet access
    if (metrics_eth_netif) {
        esp_netif_set_default_netif(metrics_eth_netif);
        ESP_LOGI(TAG, "Set Ethernet as default interface for NTP");
    }

    // Configure SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_PRIMARY);
    esp_sntp_setservername(1, NTP_SERVER_SECONDARY);

    // Set sync notification callback
    sntp_set_time_sync_notification_cb(ntp_sync_notification_cb);

    // Set sync interval
    esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);

    // Initialize SNTP (non-blocking - runs in background)
    esp_sntp_init();

    ESP_LOGI(TAG, "NTP initialized - sync will happen in background");
    ESP_LOGI(TAG, "Primary NTP server: %s", NTP_SERVER_PRIMARY);
    ESP_LOGI(TAG, "Secondary NTP server: %s", NTP_SERVER_SECONDARY);
}

// ===== Metrics Collection =====

/** Finalize current bucket and advance to next */
static void advance_bucket(void)
{
    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    // Calculate averages for current bucket
    wifi_metrics_bucket_t *bucket = &metrics_buckets[current_bucket_index];

    if (bucket_rssi_count > 0) {
        bucket->avg_rssi = (int8_t)(bucket_rssi_sum / (int32_t)bucket_rssi_count);
    } else {
        bucket->avg_rssi = 0;  // No data
    }

    if (bucket_total_samples > 0) {
        bucket->connection_pct = (uint8_t)((bucket_connected_samples * 100) / bucket_total_samples);
    } else {
        bucket->connection_pct = 0;
    }

    bucket->sample_count = (bucket_rssi_count > 255) ? 255 : (uint8_t)bucket_rssi_count;
    bucket->valid = (bucket_total_samples > 0);

    ESP_LOGI(TAG, "Bucket %d complete: RSSI=%d dBm, connected=%u%%, samples=%u",
             current_bucket_index, bucket->avg_rssi, bucket->connection_pct, bucket->sample_count);

    // Advance to next bucket
    current_bucket_index = (current_bucket_index + 1) % WIFI_METRICS_BUCKET_COUNT;

    // Reset accumulator for new bucket
    bucket_rssi_sum = 0;
    bucket_rssi_count = 0;
    bucket_connected_samples = 0;
    bucket_total_samples = 0;

    // Clear next bucket (it will be overwritten)
    metrics_buckets[current_bucket_index].valid = false;
    metrics_buckets[current_bucket_index].avg_rssi = 0;
    metrics_buckets[current_bucket_index].connection_pct = 0;
    metrics_buckets[current_bucket_index].sample_count = 0;

    xSemaphoreGive(metrics_mutex);
}

/** Sample current WiFi metrics */
static void sample_wifi_metrics(void)
{
    EventBits_t bits = xEventGroupGetBits(metrics_event_group);
    bool connected = (bits & metrics_wifi_connected_bit) != 0;
    int8_t rssi = 0;

    if (connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    // Update running totals
    if (connected) {
        total_connected_sec += WIFI_METRICS_SAMPLE_INTERVAL_SEC;
    } else {
        total_disconnected_sec += WIFI_METRICS_SAMPLE_INTERVAL_SEC;
    }

    // Store last values
    last_rssi = rssi;
    last_connected = connected;

    // Accumulate into current bucket
    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (connected && rssi != 0) {
            bucket_rssi_sum += rssi;
            bucket_rssi_count++;
        }
        if (connected) {
            bucket_connected_samples++;
        }
        bucket_total_samples++;
        xSemaphoreGive(metrics_mutex);
    }
}

/** WiFi metrics collection task */
static void wifi_metrics_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi metrics collection started");
    ESP_LOGI(TAG, "Bucket size: %d minutes, History: %d hours (%d buckets)",
             WIFI_METRICS_BUCKET_MINUTES, WIFI_METRICS_HISTORY_HOURS, WIFI_METRICS_BUCKET_COUNT);

    // Calculate samples per bucket
    const int samples_per_bucket = (WIFI_METRICS_BUCKET_MINUTES * 60) / WIFI_METRICS_SAMPLE_INTERVAL_SEC;
    int sample_count = 0;

    while (1) {
        // Wait for sample interval
        vTaskDelay(pdMS_TO_TICKS(WIFI_METRICS_SAMPLE_INTERVAL_SEC * 1000));

        // Sample metrics
        sample_wifi_metrics();
        sample_count++;

        // Check if bucket is complete
        if (sample_count >= samples_per_bucket) {
            advance_bucket();
            sample_count = 0;
        }
    }
}

// ===== Public API =====

void wifi_metrics_init(EventGroupHandle_t event_group, EventBits_t wifi_connected_bit,
                       esp_netif_t *eth_netif)
{
    metrics_event_group = event_group;
    metrics_wifi_connected_bit = wifi_connected_bit;
    metrics_eth_netif = eth_netif;

    // Create mutex
    metrics_mutex = xSemaphoreCreateMutex();

    // Initialize buckets
    for (int i = 0; i < WIFI_METRICS_BUCKET_COUNT; i++) {
        metrics_buckets[i].valid = false;
        metrics_buckets[i].avg_rssi = 0;
        metrics_buckets[i].connection_pct = 0;
        metrics_buckets[i].sample_count = 0;
    }

    // Initialize NTP (uses ethernet interface for internet access)
    init_ntp();

    ESP_LOGI(TAG, "WiFi metrics initialized");
}

void wifi_metrics_start(void)
{
    // Start metrics collection task
    xTaskCreate(wifi_metrics_task, "wifi_metrics", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "WiFi metrics collection task started");
}

void wifi_metrics_get_history(wifi_metrics_bucket_t *buckets, int *current_index)
{
    if (!buckets || !current_index) return;

    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(buckets, metrics_buckets, sizeof(metrics_buckets));
        *current_index = current_bucket_index;
        xSemaphoreGive(metrics_mutex);
    } else {
        // Return empty data on timeout
        memset(buckets, 0, sizeof(wifi_metrics_bucket_t) * WIFI_METRICS_BUCKET_COUNT);
        *current_index = 0;
    }
}

void wifi_metrics_get_summary(wifi_metrics_summary_t *summary)
{
    if (!summary) return;

    summary->current_rssi = last_rssi;
    summary->connected = last_connected;
    summary->total_connected_sec = total_connected_sec;
    summary->total_disconnected_sec = total_disconnected_sec;
    summary->current_bucket_index = current_bucket_index;
    summary->time_synced = ntp_synced;
    summary->boot_time_utc = boot_time_utc;
}

bool wifi_metrics_time_synced(void)
{
    return ntp_synced;
}

int64_t wifi_metrics_get_utc_time(void)
{
    if (!ntp_synced) {
        return 0;
    }
    return (int64_t)time(NULL);
}
