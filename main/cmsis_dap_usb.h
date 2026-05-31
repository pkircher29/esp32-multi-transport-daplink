/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2025 GitHub Copilot
 *
 * USB CDC transport header for CMSIS-DAP
 */

#ifndef CMSIS_DAP_USB_H
#define CMSIS_DAP_USB_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize USB CDC transport
 * @return 0 on success, -1 on failure
 */
int cmsis_dap_usb_init(void);

/**
 * USB CDC transport task
 * Should be created with xTaskCreate()
 * @param pvParameters Task parameters (unused)
 */
void cmsis_dap_usb_task(void *pvParameters);

/**
 * Get USB CDC connection status
 * @return true if connected, false otherwise
 */
bool cmsis_dap_usb_is_connected(void);

/**
 * Get USB CDC initialization status
 * @return true if initialized, false otherwise
 */
bool cmsis_dap_usb_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif  // CMSIS_DAP_USB_H
