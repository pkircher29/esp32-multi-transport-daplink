#ifndef WEB_DASHBOARD_H
#define WEB_DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enforce HTTP Basic Auth on a request for state-changing endpoints.
 *
 * When CONFIG_ESP_WEB_DASHBOARD_AUTH_ENABLED is set, validates the request's
 * Authorization header against the configured credentials. On failure it sends
 * a 401 response (with a WWW-Authenticate challenge) and returns ESP_FAIL; the
 * caller should then return immediately. When auth is disabled this is a no-op
 * that always returns ESP_OK.
 *
 * @param req The HTTP request to authenticate
 * @return ESP_OK if authorized (or auth disabled), ESP_FAIL otherwise
 */
esp_err_t web_dashboard_require_auth(httpd_req_t *req);

/**
 * @brief Initialize the web dashboard HTTP server
 * 
 * Starts an HTTP server on port 80 with REST API endpoints
 * and a web-based status dashboard.
 * 
 * @return true if initialization successful, false otherwise
 */
bool web_dashboard_init(void);

/**
 * @brief Check if web dashboard is initialized
 * 
 * @return true if running, false otherwise
 */
bool web_dashboard_is_initialized(void);

/**
 * @brief Get current transport status as JSON
 * 
 * Returns a JSON string with status of all transports:
 * - WiFi/TCP: connected status, IP address, port
 * - USB CDC: connection status
 * - Bluetooth: connection status, device name
 * 
 * @param buffer Output buffer for JSON string
 * @param size Maximum buffer size
 * @return Number of bytes written
 */
uint16_t web_dashboard_get_status_json(char *buffer, uint16_t size);

/**
 * @brief Update statistics counters
 * 
 * Called by transport layers to update traffic statistics
 * 
 * @param transport_id 0=WiFi, 1=USB, 2=Bluetooth
 * @param bytes_received Bytes received from target device
 * @param bytes_sent Bytes sent to target device
 */
void web_dashboard_update_stats(uint8_t transport_id, uint32_t bytes_received, uint32_t bytes_sent);

/**
 * @brief Reset all statistics
 */
void web_dashboard_reset_stats(void);

/**
 * @brief Broadcast data to all active WebSocket terminal clients
 * 
 * @param data Data buffer to send
 * @param len Length of data in bytes
 */
void web_dashboard_broadcast_ws(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // WEB_DASHBOARD_H
