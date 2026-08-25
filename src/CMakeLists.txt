# Optional project-root .env (gitignored). Process environment wins over .env.
set(_dotenv "${CMAKE_CURRENT_LIST_DIR}/../.env")
if(EXISTS "${_dotenv}")
    file(STRINGS "${_dotenv}" _env_lines)
    foreach(_line IN LISTS _env_lines)
        if(_line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.*)$")
            set(_k "${CMAKE_MATCH_1}")
            set(_v "${CMAKE_MATCH_2}")
            string(REGEX REPLACE "\r$" "" _v "${_v}")
            string(REGEX REPLACE "^[\"'](.*)[\"']$" "\\1" _v "${_v}")
            if(NOT DEFINED ENV{${_k}} OR "$ENV{${_k}}" STREQUAL "")
                set(ENV{${_k}} "${_v}")
            endif()
        endif()
    endforeach()
endif()

idf_component_register(
    SRCS "main.c" "proxy.c" "remote_ota.c" "wifi_metrics.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls esp_http_server esp_https_ota mbedtls
)

if(DEFINED ENV{REMOTE_OTA_VERSION_URL} AND NOT "$ENV{REMOTE_OTA_VERSION_URL}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "REMOTE_OTA_VERSION_URL=\"$ENV{REMOTE_OTA_VERSION_URL}\"")
    message(STATUS "REMOTE_OTA_VERSION_URL=$ENV{REMOTE_OTA_VERSION_URL}")
endif()
