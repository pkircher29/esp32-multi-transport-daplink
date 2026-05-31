#include "cmsis_dap_ble.h"
#include "DAP.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <string.h>

static const char *TAG = "CMSIS_DAP_BLE";

// CMSIS-DAP BLE UUIDs
#define CMSIS_DAP_BLE_SERVICE_UUID          0x6e7f, 0x1c99, 0x4b7d, 0x95, 0x7a, 0xeb, 0x3f, 0x1d, 0x0b, 0x28, 0xa4
#define CMSIS_DAP_BLE_RX_CHAR_UUID          0x6e7f, 0x1c99, 0x4b7e, 0x95, 0x7a, 0xeb, 0x3f, 0x1d, 0x0b, 0x28, 0xa4
#define CMSIS_DAP_BLE_TX_CHAR_UUID          0x6e7f, 0x1c99, 0x4b7f, 0x95, 0x7a, 0xeb, 0x3f, 0x1d, 0x0b, 0x28, 0xa4

// BLE TX/RX data structures
#define BLE_RX_QUEUE_LEN 16
#define BLE_BUFFER_SIZE 1024
#define BLE_MTU_SIZE 512

typedef struct {
    uint8_t data[BLE_BUFFER_SIZE];
    uint16_t length;
} ble_rx_msg_t;

typedef struct {
    bool initialized;
    bool connected;
    uint16_t conn_handle;
    QueueHandle_t rx_queue;
    EventGroupHandle_t events;
    uint32_t bytes_received;
    uint32_t bytes_sent;
} ble_state_t;

static ble_state_t ble_state = {0};

// GATT characteristics
static uint16_t ble_rx_char_handle;
static uint16_t ble_tx_char_handle;

#define BLE_TX_READY_BIT (1 << 0)

// GATT server callback
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ble_state.connected = true;
            ble_state.conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE client connected, conn_handle=%d", event->connect.conn_handle);
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ble_state.connected = false;
            ble_state.conn_handle = -1;
            ESP_LOGI(TAG, "BLE client disconnected");
            break;
            
        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "BLE SUBSCRIBE event, conn_handle=%d char_handle=%d",
                     event->subscribe.conn_handle, event->subscribe.attr_handle);
            break;
            
        default:
            break;
    }
    return 0;
}

// GATT write callback (RX)
static int ble_write_callback(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ble_rx_msg_t msg = {0};
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        
        if (len > BLE_BUFFER_SIZE) {
            len = BLE_BUFFER_SIZE;
        }
        
        // Copy data from mbuf chain
        int rc = ble_hs_mbuf_to_flat(ctxt->om, msg.data, len, &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        msg.length = len;
        ble_state.bytes_received += len;
        
        // Queue for processing
        xQueueSend(ble_state.rx_queue, &msg, 0);
    }
    return 0;
}

// GATT read callback (TX)
static int ble_read_callback(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Return empty on read (notification is used for TX)
        os_mbuf_append(ctxt->om, (uint8_t *)"", 0);
    }
    return 0;
}

// GATT profile definition
static const struct ble_gatt_svc_def gatt_svc_def[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(CMSIS_DAP_BLE_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // RX Characteristic (write)
                .uuid = BLE_UUID128_DECLARE(CMSIS_DAP_BLE_RX_CHAR_UUID),
                .access_cb = ble_write_callback,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &ble_rx_char_handle,
            },
            {
                // TX Characteristic (notify)
                .uuid = BLE_UUID128_DECLARE(CMSIS_DAP_BLE_TX_CHAR_UUID),
                .access_cb = ble_read_callback,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &ble_tx_char_handle,
            },
            {0}  // End of characteristics
        }
    },
    {0}  // End of services
};

// BLE host complete callback
static void ble_host_complete_callback(void) {
    ESP_LOGI(TAG, "BLE host synced");
}

// Main task for processing BLE data
void cmsis_dap_ble_task(void *pvParameters) {
    if (!ble_state.initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        vTaskDelete(NULL);
        return;
    }

    uint8_t accumulated_data[BLE_BUFFER_SIZE];
    uint16_t accumulated_len = 0;
    ble_rx_msg_t rx_msg = {0};

    ESP_LOGI(TAG, "BLE task started");

    while (1) {
        // Wait for RX data
        if (xQueueReceive(ble_state.rx_queue, &rx_msg, pdMS_TO_TICKS(100))) {
            // Accumulate data
            if (accumulated_len + rx_msg.length <= BLE_BUFFER_SIZE) {
                memcpy(accumulated_data + accumulated_len, rx_msg.data, rx_msg.length);
                accumulated_len += rx_msg.length;

                // Check for complete packet
                if (accumulated_len >= 8) {
                    uint32_t signature = *(uint32_t *)accumulated_data;
                    uint16_t pkt_len = *(uint16_t *)(accumulated_data + 4);
                    uint16_t total_len = 8 + pkt_len;

                    if (total_len <= BLE_BUFFER_SIZE && accumulated_len >= total_len) {
                        if (signature == 0x00504144) {  // "DAP\0"
                            // Valid packet - process it
                            uint8_t response[BLE_BUFFER_SIZE];
                            uint16_t response_len = sizeof(response) - 8;

                            // Call DAP processor
                            DAP_ProcessCommand(accumulated_data + 8, pkt_len, response + 8, &response_len);

                            // Build response packet
                            *(uint32_t *)response = 0x00504144;
                            *(uint16_t *)(response + 4) = response_len;
                            response[6] = 0;
                            response[7] = 0;

                            // Send via BLE
                            if (ble_state.connected) {
                                struct os_mbuf *om = ble_hs_mbuf_from_flat(response, 8 + response_len);
                                ble_gattc_notify_custom(ble_state.conn_handle, ble_tx_char_handle, om);
                                ble_state.bytes_sent += (8 + response_len);
                            }

                            // Shift buffer
                            accumulated_len -= total_len;
                            memmove(accumulated_data, accumulated_data + total_len, accumulated_len);
                        } else {
                            // Invalid signature, skip this byte
                            accumulated_len--;
                            memmove(accumulated_data, accumulated_data + 1, accumulated_len);
                        }
                    }
                }
            } else {
                // Buffer overflow, reset
                ESP_LOGW(TAG, "BLE buffer overflow");
                accumulated_len = 0;
            }
        }
    }
}

bool cmsis_dap_ble_init(void) {
    if (ble_state.initialized) {
        ESP_LOGW(TAG, "BLE already initialized");
        return true;
    }

    // Create queues and events
    ble_state.rx_queue = xQueueCreate(BLE_RX_QUEUE_LEN, sizeof(ble_rx_msg_t));
    ble_state.events = xEventGroupCreate();
    
    if (!ble_state.rx_queue || !ble_state.events) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS objects");
        return false;
    }

    // Initialize NimBLE
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble: %d", ret);
        return false;
    }

    // Configure GAP
    ble_hs_cfg.reset_cb = ble_gap_event_handler;
    ble_hs_cfg.sync_cb = ble_host_complete_callback;
    ble_hs_cfg.gatts_register_cb = NULL;

    // Register GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_add_svcs(gatt_svc_def);

    // Set device name
    ble_svc_gap_device_name_set("ESP32-CMSIS-DAP-BLE");

    // Start NimBLE
    nimble_port_freertos_init(cmsis_dap_ble_task);

    // Start advertising
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    ret = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, 30000, &adv_params);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: %d", ret);
        return false;
    }

    ble_state.initialized = true;
    ESP_LOGI(TAG, "BLE initialized and advertising as 'ESP32-CMSIS-DAP-BLE'");
    
    return true;
}

bool cmsis_dap_ble_is_initialized(void) {
    return ble_state.initialized;
}

bool cmsis_dap_ble_is_connected(void) {
    return ble_state.connected;
}

uint16_t cmsis_dap_ble_write(const uint8_t *data, uint16_t length) {
    if (!ble_state.initialized || !ble_state.connected) {
        return 0;
    }

    if (length > BLE_MTU_SIZE) {
        length = BLE_MTU_SIZE;
    }

    // Send via BLE notification
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    int ret = ble_gattc_notify_custom(ble_state.conn_handle, ble_tx_char_handle, om);
    
    if (ret == 0) {
        ble_state.bytes_sent += length;
        return length;
    }
    
    return 0;
}
