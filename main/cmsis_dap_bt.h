/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2025 GitHub Copilot
 *
 * Bluetooth SPP transport header for CMSIS-DAP
 */

#ifndef CMSIS_DAP_BT_H
#define CMSIS_DAP_BT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize Bluetooth Classic and SPP server
 * @return 0 on success, -1 on failure
 */
int cmsis_dap_bt_init(void);

/**
 * Bluetooth SPP transport task
 * Should be created with xTaskCreate()
 * @param pvParameters Task parameters (unused)
 */
void cmsis_dap_bt_task(void *pvParameters);

/**
 * Get Bluetooth SPP connection status
 * @return true if connected, false otherwise
 */
bool cmsis_dap_bt_is_connected(void);

/**
 * Get Bluetooth initialization status
 * @return true if initialized, false otherwise
 */
bool cmsis_dap_bt_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif  // CMSIS_DAP_BT_H
