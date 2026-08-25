idf_component_register(
    SRCS "main.c" "proxy.c" "remote_ota.c" "wifi_metrics.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls esp_http_server esp_https_ota mbedtls
)

# OTA URL, highest wins:
#   1. REMOTE_OTA_VERSION_URL env (tagged CI)
#   2. include/config.local.h (optional local override)
#   3. git origin → https://<owner>.github.io/<repo>/version.json
#   4. config.h default (upstream Pages)
set(_local_h "${CMAKE_CURRENT_LIST_DIR}/../include/config.local.h")
if(DEFINED ENV{REMOTE_OTA_VERSION_URL} AND NOT "$ENV{REMOTE_OTA_VERSION_URL}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "REMOTE_OTA_VERSION_URL=\"$ENV{REMOTE_OTA_VERSION_URL}\"")
    message(STATUS "REMOTE_OTA_VERSION_URL=$ENV{REMOTE_OTA_VERSION_URL}")
elseif(NOT EXISTS "${_local_h}")
    execute_process(
        COMMAND git remote get-url origin
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        OUTPUT_VARIABLE _git_origin
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_origin MATCHES "github.com[:/]([^/]+)/([^/]+)")
        set(_ota_owner "${CMAKE_MATCH_1}")
        set(_ota_repo "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "\\.git$" "" _ota_repo "${_ota_repo}")
        string(REGEX REPLACE "/$" "" _ota_repo "${_ota_repo}")
        set(_derived "https://${_ota_owner}.github.io/${_ota_repo}/version.json")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            "REMOTE_OTA_VERSION_URL=\"${_derived}\"")
        message(STATUS "REMOTE_OTA_VERSION_URL from git origin: ${_derived}")
    endif()
endif()
