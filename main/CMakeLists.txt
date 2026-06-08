set(COMPONENT_SRCS
    "DAP.c"
    "DAP_vendor.c"
    "JTAG_DP.c"
    "SWO.c"
    "SW_DP.c"
    "UART.c"
    "cmsis_dap_tcp.c"
    "wifi_config_nvs.c"
    "main.c")

if(CONFIG_ESP_UART_BRIDGE_ENABLED)
    list(APPEND COMPONENT_SRCS "uart_bridge.c")
endif()

if(CONFIG_ESP_USB_CDC_ENABLED)
    list(APPEND COMPONENT_SRCS "cmsis_dap_usb.c")
endif()

if(CONFIG_ESP_CMSIS_DAP_BT_ENABLED)
    list(APPEND COMPONENT_SRCS "cmsis_dap_bt.c")
endif()

if(CONFIG_ESP_CMSIS_DAP_BLE_ENABLED)
    list(APPEND COMPONENT_SRCS "cmsis_dap_ble.c")
endif()

if(CONFIG_ESP_WEB_DASHBOARD_ENABLED)
    list(APPEND COMPONENT_SRCS "web_dashboard.cpp" "wifi_config_api.c")
endif()

if(CONFIG_ESP_PRINT_CPU_USAGE)
    list(APPEND COMPONENT_SRCS "cpu_usage.c")
endif()

set(EMBED_CERTS "")

# Generate (once) and embed a self-signed TLS certificate for the HTTPS
# dashboard. The generated key/cert are written to certs/ which is gitignored,
# so the private key is never committed.
if(CONFIG_ESP_WEB_DASHBOARD_ENABLED AND CONFIG_ESP_WEB_DASHBOARD_USE_HTTPS)
    set(cert_dir "${CMAKE_CURRENT_SOURCE_DIR}/certs")
    set(cert_pem "${cert_dir}/servercert.pem")
    set(key_pem  "${cert_dir}/prvtkey.pem")

    if(NOT EXISTS "${cert_pem}" OR NOT EXISTS "${key_pem}")
        find_program(OPENSSL_EXE openssl)
        if(NOT OPENSSL_EXE)
            message(FATAL_ERROR
                "openssl not found but CONFIG_ESP_WEB_DASHBOARD_USE_HTTPS is "
                "enabled. Install openssl (and re-run), or disable HTTPS in "
                "menuconfig (Web Dashboard configuration).")
        endif()
        file(MAKE_DIRECTORY "${cert_dir}")
        message(STATUS "Generating self-signed dashboard TLS certificate...")
        execute_process(
            COMMAND "${OPENSSL_EXE}" req -x509 -newkey rsa:2048 -nodes
                    -keyout "${key_pem}" -out "${cert_pem}"
                    -days 3650 -subj "/CN=airtap.local"
            RESULT_VARIABLE openssl_rv
            ERROR_VARIABLE  openssl_err)
        if(NOT openssl_rv EQUAL 0)
            message(FATAL_ERROR "openssl certificate generation failed: ${openssl_err}")
        endif()
    endif()

    list(APPEND EMBED_CERTS "${cert_pem}" "${key_pem}")
endif()

idf_component_register(SRCS ${COMPONENT_SRCS}
                       PRIV_REQUIRES spi_flash
                       PRIV_REQUIRES esp_https_server
                       PRIV_REQUIRES mbedtls
                       REQUIRES driver
                       REQUIRES lwip
                       REQUIRES esp_event
                       REQUIRES esp_timer
                       REQUIRES esp_wifi
                       REQUIRES nvs_flash
                       REQUIRES esp_tinyusb
                       REQUIRES bt
                       REQUIRES json
                       REQUIRES esp_http_server
                       REQUIRES mdns
                       REQUIRES app_update
                       EMBED_TXTFILES ${EMBED_CERTS}
                       INCLUDE_DIRS "")
