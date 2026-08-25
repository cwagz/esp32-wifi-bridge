/*
 * Proxy Server - SSL/TLS passthrough to Powerwall
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"

#include "config.h"
#include "proxy.h"

static const char *TAG = "proxy";

// ===== Module State =====
static EventGroupHandle_t proxy_event_group = NULL;
static EventBits_t proxy_eth_got_ip_bit = 0;
static volatile int64_t *proxy_watchdog_timestamp = NULL;
static esp_netif_t *proxy_eth_netif = NULL;

// Statistics
static uint64_t total_bytes_in = 0;
static uint64_t total_bytes_out = 0;
static uint32_t total_requests = 0;
static uint32_t successful_requests = 0;
static uint32_t failed_requests = 0;

// Request logging
static request_log_entry_t request_log[REQUEST_LOG_SIZE];
static int request_log_index = 0;
static SemaphoreHandle_t request_log_mutex = NULL;

// Running average TTFB
static uint32_t avg_ttfb_ms = 0;
static uint32_t ttfb_sample_count = 0;

// Server socket
static int server_socket = -1;

// ===== Buffer Pool =====
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

// ===== Request Logging =====

/** Log a completed request/response exchange */
static void log_request(uint32_t source_ip, uint32_t bytes_in, uint32_t bytes_out,
                        uint16_t ttfb_ms, uint16_t ttlb_ms, uint8_t result)
{
    // Update cumulative statistics
    total_requests++;
    if (result == 0) {
        successful_requests++;
        if (proxy_watchdog_timestamp) {
            if (*proxy_watchdog_timestamp == 0) {
                ESP_LOGI(TAG, "Watchdog armed after first successful Powerwall proxy");
            }
            *proxy_watchdog_timestamp = esp_timer_get_time();
        }
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

// ===== Proxy Tasks =====

/** SSL/TLS Passthrough Proxy task - forwards encrypted packets without decryption */
static void handle_client_task(void *pvParameters)
{
    int client_sock = (int)(intptr_t)pvParameters;
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

    // Acquire buffer pair from pool
    buffer_index = acquire_buffer_pair();
    if (buffer_index < 0) {
        ESP_LOGE(TAG, "No buffers available - max concurrent clients (%d) reached", MAX_CONCURRENT_CLIENTS);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Connect to Powerwall via TCP
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

    // Disable Nagle's algorithm for lower latency
    int nodelay = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    setsockopt(powerwall_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ESP_LOGI(TAG, "Connected to Powerwall at %s:443 (encrypted passthrough)", POWERWALL_IP_STR);

    // Get buffer pointers from the preallocated pool
    uint8_t *client_buffer = buffer_pool[buffer_index].client_buffer;
    uint8_t *powerwall_buffer = buffer_pool[buffer_index].powerwall_buffer;

    // Set both sockets to non-blocking mode
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

    // Bidirectional forwarding loop using select()
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_sock, &read_fds);
        FD_SET(powerwall_sock, &read_fds);

        int max_fd = (client_sock > powerwall_sock) ? client_sock : powerwall_sock;

        struct timeval select_timeout = {.tv_sec = 0, .tv_usec = 100000}; // 100ms
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &select_timeout);

        if (ready < 0) {
            ESP_LOGE(TAG, "select() error: %d", errno);
            break;
        } else if (ready == 0) {
            if ((xTaskGetTickCount() - last_activity) > timeout_ticks) {
                ESP_LOGI(TAG, "Connection timeout - no activity for %d ms", PROXY_TIMEOUT_MS);
                request_result = 1;  // Timeout
                break;
            }
            continue;
        }

        // Client -> Powerwall
        if (FD_ISSET(client_sock, &read_fds)) {
            int len = recv(client_sock, client_buffer, PROXY_BUFFER_SIZE, 0);
            if (len > 0) {
                // Log previous exchange if starting new request
                if (request_bytes_out > 0 && !awaiting_first_byte) {
                    log_request(source_ip, request_bytes_in, request_bytes_out,
                               current_ttfb_ms, current_ttlb_ms, request_result);
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

                // Forward to Powerwall
                int total_sent = 0;
                while (total_sent < len) {
                    int sent = send(powerwall_sock, client_buffer + total_sent, len - total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

        // Powerwall -> Client
        if (FD_ISSET(powerwall_sock, &read_fds)) {
            int len = recv(powerwall_sock, powerwall_buffer, PROXY_BUFFER_SIZE, 0);
            if (len > 0) {
                // Calculate TTFB on first response byte
                if (awaiting_first_byte) {
                    TickType_t ttfb_ticks = xTaskGetTickCount() - request_start_time;
                    uint32_t ttfb_ms_val = ttfb_ticks * portTICK_PERIOD_MS;
                    current_ttfb_ms = (ttfb_ms_val > 65535) ? 65535 : ttfb_ms_val;
                    awaiting_first_byte = false;
                    #if DEBUG_MODE
                    ESP_LOGI(TAG, "TTFB: %u ms", current_ttfb_ms);
                    #endif
                }

                // Forward to client
                int total_sent = 0;
                while (total_sent < len) {
                    int sent = send(client_sock, powerwall_buffer + total_sent, len - total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

                // Update TTLB
                TickType_t ttlb_ticks = last_activity - request_start_time;
                uint32_t ttlb_ms_val = ttlb_ticks * portTICK_PERIOD_MS;
                current_ttlb_ms = (ttlb_ms_val > 65535) ? 65535 : ttlb_ms_val;

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
        log_request(source_ip, request_bytes_in, request_bytes_out,
                   current_ttfb_ms, current_ttlb_ms, request_result);
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
    xEventGroupWaitBits(proxy_event_group, proxy_eth_got_ip_bit, false, true, portMAX_DELAY);

    esp_netif_ip_info_t ip_info = {0};
    for (int i = 0; i < 50; i++) {
        if (proxy_eth_netif &&
            esp_netif_get_ip_info(proxy_eth_netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "No Ethernet IP — not binding :443 on all interfaces");
        vTaskDelete(NULL);
        return;
    }

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
    server_addr.sin_addr.s_addr = ip_info.ip.addr;

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed on " IPSTR ":%d", IP2STR(&ip_info.ip), PROXY_PORT);
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

    ESP_LOGI(TAG, "TCP Server (SSL passthrough) listening on " IPSTR ":%d (Ethernet only)",
             IP2STR(&ip_info.ip), PROXY_PORT);
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

        // Spawn handler task
        BaseType_t task_created = xTaskCreate(handle_client_task, "ssl_passthrough",
                                               SSL_PASSTHROUGH_TASK_STACK_SIZE,
                                               (void *)(intptr_t)client_sock, 5, NULL);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client handler task");
            close(client_sock);
        }
    }

    close(server_socket);
    vTaskDelete(NULL);
}

// ===== Public API =====

void proxy_init(EventGroupHandle_t event_group, EventBits_t eth_got_ip_bit,
                volatile int64_t *watchdog_timestamp, esp_netif_t *eth)
{
    proxy_event_group = event_group;
    proxy_eth_got_ip_bit = eth_got_ip_bit;
    proxy_watchdog_timestamp = watchdog_timestamp;
    proxy_eth_netif = eth;
    ESP_LOGI(TAG, "Proxy initialized");
}

void proxy_start(void)
{
    // Initialize buffer pool
    init_buffer_pool();

    // Start TCP server task
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Proxy services started - forwarding to %s:443", POWERWALL_IP_STR);
}

void proxy_get_stats(proxy_stats_t *stats)
{
    if (!stats) return;

    stats->total_bytes_in = total_bytes_in;
    stats->total_bytes_out = total_bytes_out;
    stats->total_requests = total_requests;
    stats->successful_requests = successful_requests;
    stats->failed_requests = failed_requests;
    stats->avg_ttfb_ms = avg_ttfb_ms;
}

void proxy_get_request_log(request_log_entry_t *entries, int *count, int max_entries)
{
    if (!entries || !count) return;
    *count = 0;

    if (!request_log_mutex) return;
    if (xSemaphoreTake(request_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    // Copy valid entries (most recent first)
    int idx = (request_log_index - 1 + REQUEST_LOG_SIZE) % REQUEST_LOG_SIZE;
    for (int i = 0; i < REQUEST_LOG_SIZE && *count < max_entries; i++) {
        if (request_log[idx].valid) {
            memcpy(&entries[*count], &request_log[idx], sizeof(request_log_entry_t));
            (*count)++;
        }
        idx = (idx - 1 + REQUEST_LOG_SIZE) % REQUEST_LOG_SIZE;
    }

    xSemaphoreGive(request_log_mutex);
}

uint32_t proxy_get_avg_ttfb(void)
{
    return avg_ttfb_ms;
}
