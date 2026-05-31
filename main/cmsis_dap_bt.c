/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2025 GitHub Copilot
 *
 * Bluetooth SPP (Serial Port Profile) transport for CMSIS-DAP
 * Provides CMSIS-DAP protocol over Bluetooth Classic, making the ESP32 discoverable
 * as a wireless CMSIS-DAP debugger.
 *
 * Uses the same packet framing as TCP and USB transports:
 *   - 8-byte header with signature, length, and packet type
 *   - Followed by DAP command/response data
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#include "DAP.h"

#define TAG "BT_SPP"

// SPP packet buffer sizes
#define BT_BUFFER_SIZE                1024
#define BT_RX_QUEUE_LEN              16
#define SPP_SERVER_NAME              "CMSIS-DAP"
#define SPP_DEVICE_NAME              "ESP32-CMSIS-DAP"

// CMSIS-DAP packet framing (same as TCP/USB)
#define DAP_PACKET_HEADER_SIZE       8
#define DAP_PACKET_SIGNATURE         0x00504144  // "DAP\0" in little-endian

// DAP packet structure
typedef struct {
    uint32_t signature;        // 0x00504144 ("DAP\0")
    uint16_t length;           // Packet length (little-endian)
    uint8_t  packet_type;      // Reserved for future use
    uint8_t  reserved;
} dap_packet_header_t;

// RX queue message
typedef struct {
    uint8_t data[BT_BUFFER_SIZE];
    size_t len;
} bt_rx_msg_t;

// Global state
static bool bt_initialized = false;
static bool spp_connected = false;
static uint32_t spp_handle = 0;
static QueueHandle_t bt_rx_queue = NULL;
static uint8_t dap_request_buffer[1024];
static uint8_t dap_response_buffer[1024];
static SemaphoreHandle_t bt_cmd_semaphore = NULL;
static volatile uint32_t spp_mode = 0;

// Forward declarations
static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t param);

/**
 * SPP callback handler for connection and data events
 */
static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(TAG, "SPP initialized");
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            break;

        case ESP_SPP_DISCOVERY_COMP_EVT:
            ESP_LOGI(TAG, "SPP discovery completed");
            break;

        case ESP_SPP_OPEN_EVT:
            ESP_LOGI(TAG, "SPP connection opened, handle: %u", param->open.handle);
            spp_handle = param->open.handle;
            spp_connected = true;
            break;

        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI(TAG, "SPP connection closed, handle: %u", param->close.handle);
            spp_connected = false;
            spp_handle = 0;
            break;

        case ESP_SPP_START_EVT:
            ESP_LOGI(TAG, "SPP server started");
            break;

        case ESP_SPP_CL_INIT_EVT:
            ESP_LOGI(TAG, "SPP client initialized");
            break;

        case ESP_SPP_DATA_IND_EVT:
            ESP_LOGV(TAG, "SPP data received, len: %d", param->data_ind.len);
            
            // Queue received data for processing
            if (bt_rx_queue != NULL && param->data_ind.len > 0) {
                bt_rx_msg_t msg;
                msg.len = (param->data_ind.len < BT_BUFFER_SIZE) ? 
                          param->data_ind.len : BT_BUFFER_SIZE;
                memcpy(msg.data, param->data_ind.data, msg.len);
                
                if (xQueueSend(bt_rx_queue, &msg, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "BT RX queue overflow");
                }
            }
            break;

        case ESP_SPP_CONG_EVT:
            ESP_LOGI(TAG, "SPP congestion event, handle: %u, cong: %d",
                    param->cong.handle, param->cong.cong);
            break;

        case ESP_SPP_WRITE_EVT:
            ESP_LOGV(TAG, "SPP write completed");
            break;

        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI(TAG, "SPP server connection opened, handle: %u",
                    param->srv_open.handle);
            spp_handle = param->srv_open.handle;
            spp_connected = true;
            break;

        default:
            ESP_LOGI(TAG, "SPP event: %d", event);
            break;
    }
}

/**
 * GAP callback handler for device discovery and pairing
 */
static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t param)
{
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param.auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication successful");
            } else {
                ESP_LOGE(TAG, "Authentication failed, status: 0x%x", param.auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(TAG, "PIN code request");
            esp_bt_pin_code_t pin_code;
            memset(pin_code, '0', ESP_BT_PIN_CODE_LEN);
            esp_bt_gap_pin_reply(param.pin_req.bda, true, 4, pin_code);
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "Confirmation request");
            esp_bt_gap_ssp_confirm_reply(param.cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "Passkey notification: %u", param.key_notif.passkey);
            break;

        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI(TAG, "Passkey request");
            break;

        default:
            ESP_LOGD(TAG, "GAP event: %d", event);
            break;
    }
}

/**
 * Process a complete CMSIS-DAP packet and send response
 */
static int bt_process_packet(const uint8_t *data, size_t len)
{
    if (len < DAP_PACKET_HEADER_SIZE) {
        return -1;  // Not enough data for header
    }

    dap_packet_header_t *header = (dap_packet_header_t *)data;

    // Verify packet signature
    if (header->signature != DAP_PACKET_SIGNATURE) {
        ESP_LOGW(TAG, "Invalid packet signature: 0x%08x", header->signature);
        return -1;
    }

    // Extract packet length
    uint16_t pkt_len = header->length;
    
    // Validate length
    if (pkt_len == 0 || pkt_len > BT_BUFFER_SIZE) {
        ESP_LOGW(TAG, "Invalid packet length: %u", pkt_len);
        return -1;
    }

    // Check if we have the complete packet
    if (len < (DAP_PACKET_HEADER_SIZE + pkt_len)) {
        return 0;  // Incomplete packet, wait for more data
    }

    // Extract DAP command from packet data
    const uint8_t *dap_cmd = data + DAP_PACKET_HEADER_SIZE;
    size_t dap_cmd_len = pkt_len;

    // Process DAP command
    uint32_t dap_response_len = sizeof(dap_response_buffer);
    DAP_ProcessCommand(dap_cmd, dap_cmd_len, dap_response_buffer, &dap_response_len);

    // Build response packet with same header format
    uint8_t response_pkt[BT_BUFFER_SIZE + DAP_PACKET_HEADER_SIZE];
    dap_packet_header_t *resp_header = (dap_packet_header_t *)response_pkt;
    resp_header->signature = DAP_PACKET_SIGNATURE;
    resp_header->length = dap_response_len;
    resp_header->packet_type = 0;
    resp_header->reserved = 0;

    memcpy(response_pkt + DAP_PACKET_HEADER_SIZE, dap_response_buffer, dap_response_len);

    // Calculate total response size
    size_t total_response_size = DAP_PACKET_HEADER_SIZE + dap_response_len;

    // Send response via SPP
    if (spp_connected && spp_handle > 0) {
        esp_spp_write(spp_handle, total_response_size, response_pkt);
    }

    // Return number of bytes consumed
    return DAP_PACKET_HEADER_SIZE + dap_response_len;
}

/**
 * Initialize Bluetooth Classic and SPP server
 * @return 0 on success, -1 on failure
 */
int cmsis_dap_bt_init(void)
{
    if (bt_initialized) {
        return 0;
    }

    ESP_LOGI(TAG, "Initializing Bluetooth Classic and SPP");

#ifdef CONFIG_ESP_BLUETOOTH_ENABLED
    // Initialize Bluetooth
    esp_err_t ret = esp_bt_controller_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth controller: %d", ret);
        return -1;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable Bluetooth: %d", ret);
        return -1;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Bluedroid: %d", ret);
        return -1;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable Bluedroid: %d", ret);
        return -1;
    }

    // Register GAP callback
    ret = esp_bt_gap_register_callback(gap_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GAP callback: %d", ret);
        return -1;
    }

    // Register SPP callback and initialize SPP
    ret = esp_spp_register_callback(spp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register SPP callback: %d", ret);
        return -1;
    }

    ret = esp_spp_init(ESP_SPP_MODE_CB);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPP: %d", ret);
        return -1;
    }

    // Set device name
    esp_bt_dev_set_device_name(SPP_DEVICE_NAME);

    // Set discoverable and connectable mode
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // Create RX queue for data buffering
    bt_rx_queue = xQueueCreate(BT_RX_QUEUE_LEN, sizeof(bt_rx_msg_t));
    if (bt_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RX queue");
        return -1;
    }

    bt_initialized = true;
    ESP_LOGI(TAG, "Bluetooth Classic and SPP initialized, device name: %s", SPP_DEVICE_NAME);
    return 0;

#else
    ESP_LOGI(TAG, "Bluetooth disabled in configuration");
    return 0;
#endif
}

/**
 * Bluetooth SPP transport task
 * Reads data from Bluetooth, processes DAP packets, and sends responses
 */
void cmsis_dap_bt_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CMSIS-DAP Bluetooth SPP task started");

    // Initialize Bluetooth
    if (cmsis_dap_bt_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth");
        vTaskDelete(NULL);
        return;
    }

    uint8_t accumulated_data[BT_BUFFER_SIZE * 2];
    size_t accumulated_len = 0;
    bt_rx_msg_t rx_msg;

    while (1) {
        // Wait for data from Bluetooth queue
        if (xQueueReceive(bt_rx_queue, &rx_msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Append to accumulated buffer
            if (accumulated_len + rx_msg.len <= sizeof(accumulated_data)) {
                memcpy(accumulated_data + accumulated_len, rx_msg.data, rx_msg.len);
                accumulated_len += rx_msg.len;
            } else {
                ESP_LOGW(TAG, "Accumulated buffer overflow");
                accumulated_len = 0;
            }

            // Try to process complete packets
            size_t offset = 0;
            while (offset < accumulated_len) {
                int consumed = bt_process_packet(
                    accumulated_data + offset,
                    accumulated_len - offset
                );

                if (consumed > 0) {
                    offset += consumed;
                } else {
                    // Incomplete packet, wait for more data
                    break;
                }
            }

            // Shift accumulated buffer
            if (offset > 0 && offset < accumulated_len) {
                memmove(accumulated_data, accumulated_data + offset, accumulated_len - offset);
                accumulated_len -= offset;
            } else if (offset > 0) {
                accumulated_len = 0;
            }
        }
    }
}

/**
 * Get Bluetooth SPP connection status
 * @return true if connected, false otherwise
 */
bool cmsis_dap_bt_is_connected(void)
{
    return spp_connected;
}

/**
 * Get Bluetooth initialization status
 * @return true if initialized, false otherwise
 */
bool cmsis_dap_bt_is_initialized(void)
{
    return bt_initialized;
}
