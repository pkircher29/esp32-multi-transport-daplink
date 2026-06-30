set(COMPONENT_SRCS
    "DAP.c"
    "DAP_vendor.c"
    "JTAG_DP.c"
    "SWO.c"
    "SW_DP.c"
    "UART.c"
    "cmsis_dap_tcp.c"
    "config_manager.c"
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

set(COMP_REQUIRES
    driver
    lwip
    esp_event
    esp_timer
    esp_wifi
    nvs_flash
    esp_http_server
    json
    mdns
    app_update
    mbedtls
)

if(CONFIG_ESP_USB_CDC_ENABLED)
    list(APPEND COMP_REQUIRES esp_tinyusb)
endif()

if(CONFIG_ESP_CMSIS_DAP_BT_ENABLED OR CONFIG_ESP_CMSIS_DAP_BLE_ENABLED)
    list(APPEND COMP_REQUIRES bt)
endif()

idf_component_register(SRCS ${COMPONENT_SRCS}
                       PRIV_REQUIRES spi_flash
                       REQUIRES ${COMP_REQUIRES}
                       INCLUDE_DIRS "")

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-missing-field-initializers")
