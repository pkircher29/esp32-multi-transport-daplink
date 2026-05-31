#ifndef WIFI_CONFIG_API_H
#define WIFI_CONFIG_API_H

#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register WiFi configuration REST API endpoints
 * 
 * Registers the following endpoints:
 * - POST /api/wifi/config - Set WiFi SSID and password
 * - POST /api/wifi/reconnect - Reconnect with stored credentials
 * - POST /api/wifi/reset - Reset to default credentials
 * - GET /api/wifi/config - Get current WiFi configuration
 * 
 * @param server HTTP server handle
 * @return esp_err_t ESP_OK if successful
 */
esp_err_t wifi_config_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // WIFI_CONFIG_API_H
