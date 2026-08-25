/*
 * Proxy Server - SSL/TLS passthrough to Powerwall
 */

#ifndef PROXY_H
#define PROXY_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"

// Request log entry for status display
#define REQUEST_LOG_SIZE 20

typedef struct {
    int64_t timestamp;      // Seconds since boot
    uint32_t source_ip;     // Client IP address
    uint32_t bytes_in;      // Bytes received from client
    uint32_t bytes_out;     // Bytes sent to client
    uint16_t ttfb_ms;       // Time to first byte (ms)
    uint16_t ttlb_ms;       // Time to last byte (ms)
    uint8_t result;         // 0=success, 1=timeout, 2=error
    bool valid;             // Entry contains valid data
} request_log_entry_t;

// Proxy statistics
typedef struct {
    uint64_t total_bytes_in;
    uint64_t total_bytes_out;
    uint32_t total_requests;
    uint32_t successful_requests;
    uint32_t failed_requests;
    uint32_t avg_ttfb_ms;
} proxy_stats_t;

/**
 * Initialize proxy subsystem
 * @param event_group Event group for synchronization
 * @param eth_got_ip_bit Bit to wait for before starting server
 * @param watchdog_timestamp Pointer to watchdog timestamp (updated on successful connections)
 * @param eth Ethernet netif — listen socket is bound to this IP only (not WiFi)
 */
void proxy_init(EventGroupHandle_t event_group, EventBits_t eth_got_ip_bit,
                volatile int64_t *watchdog_timestamp, esp_netif_t *eth);

/**
 * Start the proxy server (call after WiFi connects)
 * This initializes the buffer pool and starts the TCP server task
 */
void proxy_start(void);

/**
 * Get current proxy statistics (thread-safe copy)
 */
void proxy_get_stats(proxy_stats_t *stats);

/**
 * Get request log entries (thread-safe copy)
 * @param entries Array to fill with log entries
 * @param count Output: number of valid entries returned
 * @param max_entries Maximum entries to return
 */
void proxy_get_request_log(request_log_entry_t *entries, int *count, int max_entries);

/**
 * Get average TTFB in milliseconds
 */
uint32_t proxy_get_avg_ttfb(void);

#endif // PROXY_H
