/*
 * Remote OTA - Firmware updates from GitHub
 */

#ifndef REMOTE_OTA_H
#define REMOTE_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "esp_netif.h"

// Remote OTA state (read-only access for status display)
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
    char last_error[80];
} remote_ota_state_t;

/**
 * Initialize remote OTA subsystem
 * @param netif The network interface to use for HTTP requests (typically Ethernet)
 */
void remote_ota_init(esp_netif_t *netif);

/** Snapshot Ethernet nameservers (DHCP or static). Ignores Tesla AP DNS. */
bool remote_ota_remember_eth_dns(uint32_t dns_main, uint32_t dns_backup);

/** Push remembered Ethernet DNS into lwIP (call after WiFi DHCP). */
void remote_ota_apply_eth_dns(void);

/**
 * Get current remote OTA state (thread-safe copy)
 */
void remote_ota_get_state(remote_ota_state_t *state);

/**
 * HTTP handler: GET /api/update - Returns update status JSON
 */
esp_err_t api_update_status_handler(httpd_req_t *req);

/**
 * HTTP handler: POST /api/check-update - Triggers update check
 */
esp_err_t api_check_update_handler(httpd_req_t *req);

/**
 * HTTP handler: POST /api/install-update - Installs available update
 */
esp_err_t api_install_update_handler(httpd_req_t *req);

/**
 * HTTP handler: POST /api/revert - Reverts to previous version
 */
esp_err_t api_revert_handler(httpd_req_t *req);

#endif // REMOTE_OTA_H
