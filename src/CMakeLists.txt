idf_component_register(
    SRCS "main.c" "proxy.c" "remote_ota.c" "wifi_metrics.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls esp_http_server esp_https_ota mbedtls
)
