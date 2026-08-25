idf_component_register(
    SRCS "main.c" "proxy.c" "remote_ota.c" "wifi_metrics.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls esp_http_server esp_https_ota mbedtls
)

# Tagged CI sets this so each fork's firmware OTA-checks its own GitHub Pages.
# Local USB builds use include/config.local.h instead.
if(DEFINED ENV{REMOTE_OTA_VERSION_URL} AND NOT "$ENV{REMOTE_OTA_VERSION_URL}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "REMOTE_OTA_VERSION_URL=\"$ENV{REMOTE_OTA_VERSION_URL}\"")
    message(STATUS "REMOTE_OTA_VERSION_URL=$ENV{REMOTE_OTA_VERSION_URL}")
endif()
