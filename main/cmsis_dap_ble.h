#ifndef CMSIS_DAP_BLE_H
#define CMSIS_DAP_BLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize BLE GATT server for CMSIS-DAP
 * 
 * Starts a Bluetooth Low Energy GATT server with characteristics for
 * bidirectional CMSIS-DAP packet transfer.
 * 
 * @return true if initialization successful, false otherwise
 */
bool cmsis_dap_ble_init(void);

/**
 * @brief Check if BLE is initialized
 * 
 * @return true if running, false otherwise
 */
bool cmsis_dap_ble_is_initialized(void);

/**
 * @brief Check if BLE client is connected
 * 
 * @return true if a client is connected, false otherwise
 */
bool cmsis_dap_ble_is_connected(void);

/**
 * @brief Send data over BLE
 * 
 * Sends a complete packet (with 8-byte header) to connected client.
 * Blocks until transmission completes or timeout occurs.
 * 
 * @param data Packet data (8-byte header + payload)
 * @param length Total packet length
 * @return Number of bytes sent, 0 on error
 */
uint16_t cmsis_dap_ble_write(const uint8_t *data, uint16_t length);

/**
 * @brief BLE GATT server task
 * 
 * FreeRTOS task for handling BLE GATT events and data transfer.
 * Should be created with xTaskCreate() in main.c
 * 
 * @param pvParameters Unused
 */
void cmsis_dap_ble_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // CMSIS_DAP_BLE_H
