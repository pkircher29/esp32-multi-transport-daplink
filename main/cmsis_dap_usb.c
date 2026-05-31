/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2025 GitHub Copilot
 *
 * USB CDC (Communications Device Class) transport for CMSIS-DAP
 * Provides CMSIS-DAP protocol over USB, making the ESP32 appear as a
 * serial port with CMSIS-DAP endpoints.
 *
 * This transport uses the same packet framing as TCP transport:
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
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include "DAP.h"

#define TAG "USB_CDC"

// USB CDC packet buffer sizes
#define USB_BUFFER_SIZE                1024
#define USB_RX_BUFFER_SIZE             (USB_BUFFER_SIZE + 16)
#define USB_TX_BUFFER_SIZE             (USB_BUFFER_SIZE + 16)
#define USB_RING_BUFFER_SIZE           4096

// CMSIS-DAP packet framing
#define DAP_PACKET_HEADER_SIZE         8
#define DAP_PACKET_SIGNATURE           0x00504144  // "DAP\0" in little-endian

// DAP packet structure
typedef struct {
    uint32_t signature;        // 0x00504144 ("DAP\0")
    uint16_t length;           // Packet length (little-endian)
    uint8_t  packet_type;      // Reserved for future use
    uint8_t  reserved;
} dap_packet_header_t;

typedef struct {
    dap_packet_header_t header;
    uint8_t data[USB_BUFFER_SIZE];
} dap_packet_t;

// Global state
static bool usb_initialized = false;
static uint8_t usb_rx_buffer[USB_RX_BUFFER_SIZE];
static uint8_t usb_tx_buffer[USB_TX_BUFFER_SIZE];
static RingbufHandle_t usb_rx_ringbuf = NULL;
static uint8_t dap_request_buffer[1024];
static uint8_t dap_response_buffer[1024];
static volatile bool usb_connected = false;

// Forward declaration
static void usb_cdc_rx_callback(int itf, cdcacm_event_t *event);
static void usb_cdc_tx_callback(int itf, cdcacm_event_t *event);

// USB CDC callbacks
static void usb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    // Data received on CDC interface
    size_t rx_size = 0;
    uint8_t *rx_data = tusb_cdc_get_rx_buffer(itf);
    
    if (rx_data != NULL) {
        rx_size = tusb_cdc_get_rx_data_size(itf);
        
        // Write to ring buffer for processing
        if (usb_rx_ringbuf != NULL && rx_size > 0) {
            size_t written = xRingbufferSend(usb_rx_ringbuf, rx_data, rx_size, 0);
            if (written != rx_size) {
                ESP_LOGW(TAG, "USB RX ring buffer overflow: wrote %d of %d bytes", 
                        written, rx_size);
            }
        }
        
        // Clear the CDC RX buffer
        tusb_cdc_read_flush(itf);
    }
}

static void usb_cdc_tx_callback(int itf, cdcacm_event_t *event)
{
    // TX complete callback - can be used for flow control if needed
}

static void usb_cdc_line_state_changed(int itf, cdcacm_event_t *event)
{
    // Track DTR (Data Terminal Ready) to know when host is connected
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    
    usb_connected = dtr;  // Host is "connected" when DTR is asserted
    
    ESP_LOGI(TAG, "USB CDC line state changed: DTR=%d, RTS=%d", dtr, rts);
}

/**
 * Initialize USB CDC transport
 * @return 0 on success, -1 on failure
 */
int cmsis_dap_usb_init(void)
{
    if (usb_initialized) {
        return 0;
    }

    ESP_LOGI(TAG, "Initializing USB CDC transport");

#ifdef CONFIG_ESP_USB_CDC_ENABLED
    // Create ring buffer for USB RX data
    usb_rx_ringbuf = xRingbufferCreate(USB_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (usb_rx_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create RX ring buffer");
        return -1;
    }

    // Configure USB CDC
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %d", ret);
        return -1;
    }

    // Register CDC ACM callbacks
    cdcacm_event_callback_t cdc_cb = {
        .rx_wanted_size = 0,
        .rx_wanted_size_callback = NULL,
        .rx_callback = usb_cdc_rx_callback,
        .tx_callback = usb_cdc_tx_callback,
        .line_state_changed_callback = usb_cdc_line_state_changed,
    };

    ret = cdcacm_register_callback(0, &cdc_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register CDC callbacks: %d", ret);
        return -1;
    }

    usb_initialized = true;
    usb_connected = false;
    
    ESP_LOGI(TAG, "USB CDC transport initialized");
    return 0;
#else
    ESP_LOGI(TAG, "USB CDC disabled in configuration");
    return 0;
#endif
}

/**
 * Parse incoming USB data as CMSIS-DAP packets
 * Reads packet header and routes to DAP_ProcessCommand
 */
static int usb_process_packet(const uint8_t *data, size_t len)
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
    if (pkt_len == 0 || pkt_len > USB_BUFFER_SIZE) {
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

    // Send response back via USB
    // Build response packet with same header format
    dap_packet_t response_pkt;
    response_pkt.header.signature = DAP_PACKET_SIGNATURE;
    response_pkt.header.length = dap_response_len;
    response_pkt.header.packet_type = 0;
    response_pkt.header.reserved = 0;

    memcpy(response_pkt.data, dap_response_buffer, dap_response_len);

    // Calculate total response size
    size_t total_response_size = DAP_PACKET_HEADER_SIZE + dap_response_len;

    // Send response via USB CDC
    if (cdcacm_write_queue(0, (uint8_t *)&response_pkt, total_response_size) > 0) {
        cdcacm_write_flush(0, 0);
    } else {
        ESP_LOGW(TAG, "Failed to write USB response");
    }

    // Return number of bytes consumed
    return DAP_PACKET_HEADER_SIZE + dap_response_len;
}

/**
 * USB CDC transport task
 * Reads data from USB, processes DAP packets, and sends responses
 */
void cmsis_dap_usb_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CMSIS-DAP USB CDC task started");

    // Initialize USB CDC
    if (cmsis_dap_usb_init() != 0) {
        ESP_LOGE(TAG, "Failed to initialize USB CDC");
        vTaskDelete(NULL);
        return;
    }

    uint8_t accumulated_data[USB_RX_BUFFER_SIZE * 2];
    size_t accumulated_len = 0;

    while (1) {
        if (usb_rx_ringbuf == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Read data from ring buffer
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(
            usb_rx_ringbuf, 
            &item_size, 
            pdMS_TO_TICKS(100),
            USB_RX_BUFFER_SIZE
        );

        if (item != NULL && item_size > 0) {
            // Append to accumulated buffer
            if (accumulated_len + item_size <= sizeof(accumulated_data)) {
                memcpy(accumulated_data + accumulated_len, item, item_size);
                accumulated_len += item_size;
            } else {
                ESP_LOGW(TAG, "Accumulated buffer overflow");
                accumulated_len = 0;
            }

            vRingbufferReturnItem(usb_rx_ringbuf, (void *)item);

            // Try to process complete packets
            size_t offset = 0;
            while (offset < accumulated_len) {
                int consumed = usb_process_packet(
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
        } else {
            // No data received, just yield
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/**
 * Get USB CDC connection status
 * @return true if connected, false otherwise
 */
bool cmsis_dap_usb_is_connected(void)
{
    return usb_connected;
}

/**
 * Get USB CDC initialization status
 * @return true if initialized, false otherwise
 */
bool cmsis_dap_usb_is_initialized(void)
{
    return usb_initialized;
}
