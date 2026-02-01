/*
 * WiFi Metrics - Track WiFi signal strength and connection success rate
 * Stores 5-minute averages over 24 hours for charting
 */

#ifndef WIFI_METRICS_H
#define WIFI_METRICS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Number of 5-minute buckets for 24 hours: (24 * 60) / 5 = 288
#define WIFI_METRICS_BUCKET_COUNT 288

// Metrics bucket structure
typedef struct {
    int8_t avg_rssi;            // Average RSSI in dBm (-127 to 0, 0 = no data)
    uint8_t connection_pct;     // Percentage of time connected (0-100)
    uint8_t sample_count;       // Number of RSSI samples in this bucket
    bool valid;                 // Bucket contains data
} wifi_metrics_bucket_t;

// Current metrics summary
typedef struct {
    int8_t current_rssi;        // Current RSSI reading
    bool connected;             // Currently connected
    uint32_t total_connected_sec;   // Total connected time since boot
    uint32_t total_disconnected_sec; // Total disconnected time since boot
    int current_bucket_index;   // Current bucket being filled
    bool time_synced;           // NTP time is synchronized
    int64_t boot_time_utc;      // UTC timestamp at boot (0 if not synced)
} wifi_metrics_summary_t;

/**
 * Initialize WiFi metrics tracking
 * @param event_group Event group containing WiFi status bits
 * @param wifi_connected_bit Bit indicating WiFi is connected
 * @param eth_netif Ethernet interface for NTP (uses ethernet for internet access)
 */
void wifi_metrics_init(EventGroupHandle_t event_group, EventBits_t wifi_connected_bit,
                       esp_netif_t *eth_netif);

/**
 * Start WiFi metrics collection (call after initialization)
 * Starts the sampling task
 */
void wifi_metrics_start(void);

/**
 * Get metrics history (thread-safe copy)
 * @param buckets Array to fill (must be WIFI_METRICS_BUCKET_COUNT elements)
 * @param current_index Output: index of the current bucket
 */
void wifi_metrics_get_history(wifi_metrics_bucket_t *buckets, int *current_index);

/**
 * Get current metrics summary
 */
void wifi_metrics_get_summary(wifi_metrics_summary_t *summary);

/**
 * Check if NTP time is synchronized
 */
bool wifi_metrics_time_synced(void);

/**
 * Get current UTC time (returns 0 if not synced)
 */
int64_t wifi_metrics_get_utc_time(void);

#endif // WIFI_METRICS_H
