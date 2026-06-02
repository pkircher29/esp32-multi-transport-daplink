/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32 app supporting the OpenOCD 'CMSIS-DAP over TCP/IP' protocol.
 *
 * Allows an ESP32 to act as a JTAG/SWD programmer for a target device such as
 * an ARM microcontroller. The host connects to this programmer using OpenOCD.
 *
 * Refer to the OpenOCD file src/jtag/drivers/cmsis_dap_tcp.c for the host
 * implementation.
 *
 * Build using the ESP32-IDF tools. Tested on ESP32-C6. To support another
 * board / CPU, dap/DAP_config.h will need to be modified.
 *
 * The CMSIS-DAP commands and responses are sent over a TCP socket rather than
 * USB. Use the OpenOCD cmsis_dap_tcp driver to connect. OpenOCD must be
 * configured with settings like these:
 *
 *     adapter driver cmsis-dap
 *     cmsis-dap backend tcp
 *     cmsis-dap tcp host 192.168.1.4
 *     cmsis-dap tcp port 4441
 *
 * Programming a target can then be done using something like this:
 *
 *     openocd --search tcl \
 *         -f tcl/interface/cmsis_dap_tcp.cfg \
 *         -f tcl/target/stm32f1x.cfg \
 *         -c "transport select swd" \
 *         -c "adapter speed 2000" \
 *         -c "program firmware.elf verify reset exit"
 *
 * Status:
 * - Supports SWD, JTAG, NRESET, TRST.
 * - Provides a UART-to-TCP/IP bridge for the target's serial console.
 * - SWO trace port currently unsupported.
 *
 * Some parts of this code were adapted from:
 *     esp-idf/examples/get-started/hello_world
 *     esp-idf/examples/wifi/getting_started/station
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "cpu_usage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/dhcp6.h"
#include "lwip/prot/dhcp6.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "esp_netif_net_stack.h"

#include "DAP.h"
#include "cmsis_dap_tcp.h"
#include "cmsis_dap_usb.h"
#include "cmsis_dap_bt.h"
#include "cmsis_dap_ble.h"
#include "web_dashboard.h"
#include "wifi_config_nvs.h"
#include "uart_bridge.h"
#include "mdns.h"

#define WIFI_SSID               CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD           CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAXIMUM_RETRIES    CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""

#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID

#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN

#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP

#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK

#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* Use an event group to signal two WiFi related events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static uint8_t mac_addr[6];
static char mac_addr_str[16];
static int wifi_retry_num;
static EventGroupHandle_t wifi_event_group;
static bool cmsis_dap_tcp_initialized;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif = NULL;
static TaskHandle_t dns_task_handle = NULL;

static void reboot(void)
{
    fflush(stdout);
    fflush(stderr);
    vTaskDelay(1000);
    esp_restart();      // Does not return.
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            printf("Attempting to connect to WiFi SSID: '%s'\n", WIFI_SSID);
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            int rssi;
            esp_wifi_sta_get_rssi(&rssi);
            printf("Connected to WiFi SSID: '%s'. RSSI: %d dBm\n", WIFI_SSID,
                    rssi);
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (cmsis_dap_tcp_initialized) {
                /* If connection is lost after we have initialized the server,
                 * any connected sockets and the server socket have been lost.
                 * The server socket must be reinitialized. Just reboot to
                 * reinitialize everything.
                 */
                printf("Lost connection to WiFi SSID: '%s'. Rebooting...\n",
                        WIFI_SSID);
                reboot();       // Does not return.
            }
            if (wifi_retry_num < WIFI_MAXIMUM_RETRIES) {
                printf("Retrying connection to WiFi SSID: '%s'\n", WIFI_SSID);
                esp_wifi_connect();
                wifi_retry_num++;
            }
            else {
                printf("Failed to connect to WiFi SSID: '%s'.\n", WIFI_SSID);
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("IP address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        wifi_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
#ifdef CONFIG_LWIP_IPV6
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
        esp_ip6_addr_type_t ipv6_type =
            esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        printf("IPv6 address (%s): " IPV6STR "\n",
                (ipv6_type == ESP_IP6_ADDR_IS_LINK_LOCAL) ? "link-local" :
                "global", IPV62STR(event->ip6_info.ip));
    }
#endif
}

static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        printf("DNS_SERVER: Unable to create socket\n");
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53); // DNS UDP port 53

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("DNS_SERVER: Socket unable to bind to port 53\n");
        close(sock);
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    printf("DNS redirect server started on port 53\n");

    while (1) {
        // Self-terminate if we are no longer in AP mode (switched back to STA)
        wifi_mode_t current_mode;
        if (esp_wifi_get_mode(&current_mode) == ESP_OK && current_mode != WIFI_MODE_AP) {
            printf("DNS_SERVER: WiFi mode changed, stopping DNS server\n");
            break;
        }

        // Set socket timeout so it doesn't block indefinitely (allowing quick exit)
        struct timeval tv = {
            .tv_sec = 2,
            .tv_usec = 0
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 12) {
            continue;
        }

        // DNS Header
        uint16_t id = *(uint16_t *)&rx_buffer[0];
        uint16_t flags = *(uint16_t *)&rx_buffer[2];
        uint16_t qdcount = ntohs(*(uint16_t *)&rx_buffer[4]);

        // Standard query response logic
        if ((flags & 0x8000) == 0 && qdcount > 0) {
            memset(tx_buffer, 0, sizeof(tx_buffer));
            
            // Response header
            *(uint16_t *)&tx_buffer[0] = id;
            *(uint16_t *)&tx_buffer[2] = htons(0x8180); // Standard Query Response, No Error
            *(uint16_t *)&tx_buffer[4] = htons(1);      // 1 Question
            *(uint16_t *)&tx_buffer[6] = htons(1);      // 1 Answer
            
            // Skip query name
            int offset = 12;
            while (offset < len && rx_buffer[offset] != 0) {
                offset += rx_buffer[offset] + 1;
            }
            offset += 5; // NULL byte, QTYPE (2 bytes), QCLASS (2 bytes)
            
            if (offset <= len && offset < 250) {
                // Copy question section
                memcpy(tx_buffer + 12, rx_buffer + 12, offset - 12);
                
                // Construct answer
                int ans_offset = offset;
                tx_buffer[ans_offset] = 0xc0;     // Pointer to question name
                tx_buffer[ans_offset + 1] = 0x0c; // at offset 12
                
                *(uint16_t *)&tx_buffer[ans_offset + 2] = htons(1);  // Type A (IPv4)
                *(uint16_t *)&tx_buffer[ans_offset + 4] = htons(1);  // Class IN
                *(uint32_t *)&tx_buffer[ans_offset + 6] = htonl(60); // TTL = 60s
                *(uint16_t *)&tx_buffer[ans_offset + 10] = htons(4); // RDLENGTH = 4
                
                // RDATA: 192.168.4.1 (ESP32 SoftAP IP)
                tx_buffer[ans_offset + 12] = 192;
                tx_buffer[ans_offset + 13] = 168;
                tx_buffer[ans_offset + 14] = 4;
                tx_buffer[ans_offset + 15] = 1;
                
                sendto(sock, tx_buffer, ans_offset + 16, 0, (struct sockaddr *)&client_addr, client_addr_len);
            }
        }
    }

    close(sock);
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

static void initialize_mdns(void)
{
    static bool mdns_initialized = false;
    if (mdns_initialized) {
        return;
    }
    
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        printf("mDNS Init failed: %d\n", err);
        return;
    }
    
    mdns_hostname_set("airtap");
    mdns_instance_name_set("ESP32 CMSIS-DAP Debugger (Airtap)");
    
#if defined(CONFIG_ESP_WEB_DASHBOARD_ENABLED) && defined(CONFIG_ESP_WEB_DASHBOARD_USE_HTTPS)
    mdns_service_add(NULL, "_https", "_tcp", 443, NULL, 0);
#else
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
#endif
    mdns_service_add(NULL, "_cmsis-dap", "_tcp", 4441, NULL, 0);
    
    mdns_initialized = true;
    printf("mDNS responder started! Device discoverable at http://airtap.local/\n");
}

static void wifi_start_ap(void)
{
    printf("Starting Access Point (SoftAP) fallback mode...\n");
    
    // Stop WiFi to change configuration
    esp_wifi_stop();
    
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = {0},
            .ssid_len = 0,
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    // Create custom SSID using device MAC suffix so multiple devices don't collide
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "ESP32-DAPLink-%s", mac_addr_str + 6);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);

    // Secure the fallback AP with WPA2 when a password (>= 8 chars) is
    // configured. An open AP would expose the OTA endpoint, the WiFi config
    // API and the target debug bridge to anyone in range.
    const char *ap_pass = CONFIG_ESP_WIFI_AP_PASSWORD;
    if (strlen(ap_pass) >= 8) {
        strlcpy((char *)ap_config.ap.password, ap_pass,
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        printf("Fallback AP secured with WPA2.\n");
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        printf("WARNING: fallback AP is OPEN (no password). Set "
               "CONFIG_ESP_WIFI_AP_PASSWORD (>= 8 chars) to enable WPA2.\n");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    printf("Access Point started! SSID: '%s'\n", ap_config.ap.ssid);
#if defined(CONFIG_ESP_WEB_DASHBOARD_ENABLED) && defined(CONFIG_ESP_WEB_DASHBOARD_USE_HTTPS)
    printf("Connect to this network and navigate to: https://192.168.4.1/dashboard\n");
#else
    printf("Connect to this network and navigate to: http://192.168.4.1/dashboard\n");
#endif

    // Start DNS server task for captive portal redirection
    if (dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_server_task", 4096, NULL, 4, &dns_task_handle);
    }

    initialize_mdns();
}

int wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                &event_handler,
                NULL,
                &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &event_handler,
                NULL,
                &instance_got_ip));
#ifdef CONFIG_LWIP_IPV6
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_GOT_IP6,
                &event_handler,
                NULL,
                &instance_got_ip));
#endif
    custom_wifi_config_t nvs_cfg;
    wifi_config_get(&nvs_cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = {0},
            .password = {0},
            /* Authmode threshold resets to WPA2 as default if password matches
             * WPA2 standards (password len => 8).  If you want to connect the
             * device to deprecated WEP/WPA networks, Please set the threshold
             * value to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password
             * with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, nvs_cfg.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, nvs_cfg.password, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait until either the connection is established (WIFI_CONNECTED_BIT)
     * or connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
     * The bits are set by event_handler() above.
     */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
#ifdef CONFIG_ESP_DAP_DISABLE_WIFI_POWER_SAVE
        // Disable power-save to improve WiFi performance.
        // https://github.com/espressif/arduino-esp32/issues/1484
        printf("Disabling WiFi power savings to improve performance.\n");
        esp_wifi_set_ps(WIFI_PS_NONE);
#endif
#ifdef CONFIG_LWIP_IPV6
        // Trigger IPv6 SLAAC auto-configuration, after IPv4 is up.
        esp_err_t err = esp_netif_create_ip6_linklocal(sta_netif);
        if (err != ESP_OK)
            perror("Failed to create IPv6 link local address");
#endif
        return 0;   // Success.
    }
    return -1;      // Failure.
}

void app_main(void)
{
    // Initialize the JTAG/SWD port pins.
    DAP_Setup();

    printf("CMSIS-DAP TCP running on ESP32\n");
    printf("ESP-IDF version: %s\n", IDF_VER);

    // Print chip information.
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("Hardware version: %s with %d CPU core(s), %s%s%s%s, ",
        CONFIG_IDF_TARGET,
        chip_info.cores,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
        (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
        (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
        (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 "
            "(Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }
    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" :
           "external");
    printf("Minimum free heap size: %" PRIu32 " bytes\n",
            esp_get_minimum_free_heap_size());

    // MAC address is unique for every ESP32 device. Use it as a UID
    // when reporing data.
    esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
    snprintf(mac_addr_str, sizeof(mac_addr_str), "%02X%02X%02X%02X%02X%02X",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4],
           mac_addr[5]);
    uint32_t serial_number = 0;
    serial_number |= mac_addr[2]; serial_number <<= 8;
    serial_number |= mac_addr[3]; serial_number <<= 8;
    serial_number |= mac_addr[4]; serial_number <<= 8;
    serial_number |= mac_addr[5];
    printf("MAC address: %s\n", mac_addr_str);

    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize custom wifi NVS configuration
    wifi_config_init();

    /* Initialize WiFi and connect to AP. If unable to connect after retries,
     * fall back to Access Point mode for setup.
     */
    if (wifi_init() != 0) {
        wifi_start_ap();
    } else {
        printf("WiFi Station connected successfully.\n");
        initialize_mdns();
    }

#ifdef CONFIG_ESP_UART_BRIDGE_ENABLED
    xTaskCreate(uart_bridge_task, "uart_bridge_task", 4096, NULL, 5, NULL);
#endif

#ifdef CONFIG_ESP_USB_CDC_ENABLED
    xTaskCreate(cmsis_dap_usb_task, "cmsis_dap_usb_task", 4096, NULL, 5, NULL);
#endif

#ifdef CONFIG_ESP_CMSIS_DAP_BT_ENABLED
    xTaskCreate(cmsis_dap_bt_task, "cmsis_dap_bt_task", 4096, NULL, 5, NULL);
#endif

#ifdef CONFIG_ESP_CMSIS_DAP_BLE_ENABLED
    cmsis_dap_ble_init();
#endif

#ifdef CONFIG_ESP_WEB_DASHBOARD_ENABLED
    web_dashboard_init();
#endif

    xTaskCreate(cmsis_dap_tcp_task, "cmsis_dap_tcp_task", 4096, NULL, 5, NULL);
    cmsis_dap_tcp_initialized = true;

#ifdef CONFIG_ESP_PRINT_CPU_USAGE
    xTaskCreatePinnedToCore(cpu_usage_task, "cpu_usage", 4096, NULL,
            CPU_USAGE_TASK_PRIO, NULL, tskNO_AFFINITY);
#endif
}
