#ifndef WIFI_CONFIG_NVS_H
#define WIFI_CONFIG_NVS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

/**
 * @brief WiFi configuration structure
 */
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    char password[WIFI_PASSWORD_MAX_LEN];
    bool configured;
} wifi_config_t;

/**
 * @brief Initialize NVS and load WiFi configuration
 * 
 * Initializes the NVS flash storage and loads saved WiFi credentials.
 * If no credentials are saved, uses defaults from menuconfig.
 * 
 * @return true if successful, false otherwise
 */
bool wifi_config_init(void);

/**
 * @brief Get current WiFi configuration
 * 
 * @param config Output buffer for configuration
 * @return true if successful, false otherwise
 */
bool wifi_config_get(wifi_config_t *config);

/**
 * @brief Save WiFi configuration to NVS
 * 
 * Saves SSID and password to non-volatile storage.
 * Changes take effect on next WiFi reconnection.
 * 
 * @param ssid WiFi network SSID (max WIFI_SSID_MAX_LEN bytes)
 * @param password WiFi network password (max WIFI_PASSWORD_MAX_LEN bytes)
 * @return true if successful, false otherwise
 */
bool wifi_config_set(const char *ssid, const char *password);

/**
 * @brief Reset WiFi configuration to defaults
 * 
 * Clears NVS storage and reverts to menuconfig defaults.
 * 
 * @return true if successful, false otherwise
 */
bool wifi_config_reset(void);

/**
 * @brief Trigger WiFi reconnection with new credentials
 * 
 * Disconnects from current WiFi and reconnects with stored credentials.
 * Non-blocking - returns immediately, connection happens asynchronously.
 * 
 * @return true if reconnection started, false otherwise
 */
bool wifi_config_reconnect(void);

/**
 * @brief Check if WiFi configuration has been customized
 * 
 * @return true if custom config exists in NVS, false if using defaults
 */
bool wifi_config_is_custom(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_CONFIG_NVS_H
