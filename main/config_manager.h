#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_SSID_MAX_LEN 32
#define CONFIG_PASSWORD_MAX_LEN 64
#define CONFIG_TOKEN_MAX_LEN 64
#define CONFIG_NAME_MAX_LEN 32
#define CONFIG_HASH_MAX_LEN 65 // SHA-256 hex string + null

/**
 * @brief Configuration structure representing the active system settings
 */
typedef struct {
    char sta_ssid[CONFIG_SSID_MAX_LEN];
    char sta_password[CONFIG_PASSWORD_MAX_LEN];
    char ap_ssid[CONFIG_SSID_MAX_LEN];
    char ap_password[CONFIG_PASSWORD_MAX_LEN];
    char api_token[CONFIG_TOKEN_MAX_LEN];
    char dash_pass_hash[CONFIG_HASH_MAX_LEN];
    char dash_pass_salt[33]; // 32 hex chars + null
    char device_name[CONFIG_NAME_MAX_LEN];
    bool sta_configured;
} system_config_t;

/**
 * @brief Initialize the configuration manager, NVS, and generate defaults
 * 
 * Auto-generates a strong randomized API token and SoftAP password if they 
 * do not already exist in NVS.
 * 
 * @return true if successful, false otherwise
 */
bool config_manager_init(void);

/**
 * @brief Get the complete system configuration
 * 
 * @param config Output buffer for system configuration
 * @return true if successful, false otherwise
 */
bool config_manager_get(system_config_t *config);

/**
 * @brief Set and save WiFi station credentials
 * 
 * @param ssid WiFi network SSID (max CONFIG_SSID_MAX_LEN - 1)
 * @param password WiFi network password (max CONFIG_PASSWORD_MAX_LEN - 1)
 * @return true if successful, false otherwise
 */
bool config_manager_set_sta_credentials(const char *ssid, const char *password);

/**
 * @brief Get the WiFi station credentials
 * 
 * @param ssid Output buffer for SSID
 * @param ssid_len Size of SSID buffer
 * @param password Output buffer for password
 * @param password_len Size of password buffer
 * @return true if successful, false otherwise
 */
bool config_manager_get_sta_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len);

/**
 * @brief Set and save WiFi SoftAP credentials
 * 
 * @param ssid SoftAP SSID
 * @param password SoftAP WPA2 password
 * @return true if successful, false otherwise
 */
bool config_manager_set_ap_credentials(const char *ssid, const char *password);

/**
 * @brief Get the WiFi SoftAP credentials
 * 
 * @param ssid Output buffer for AP SSID
 * @param ssid_len Size of AP SSID buffer
 * @param password Output buffer for AP password
 * @param password_len Size of AP password buffer
 * @return true if successful, false otherwise
 */
bool config_manager_get_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len);

/**
 * @brief Set and save a custom device name
 * 
 * @param name Custom device name
 * @return true if successful, false otherwise
 */
bool config_manager_set_device_name(const char *name);

/**
 * @brief Get the custom device name
 * 
 * @param name Output buffer for device name
 * @param name_len Size of device name buffer
 * @return true if successful, false otherwise
 */
bool config_manager_get_device_name(char *name, size_t name_len);

/**
 * @brief Set and save the API security token
 * 
 * @param token Secure API token
 * @return true if successful, false otherwise
 */
bool config_manager_set_api_token(const char *token);

/**
 * @brief Get the API security token
 * 
 * @param token Output buffer for API token
 * @param token_len Size of API token buffer
 * @return true if successful, false otherwise
 */
bool config_manager_get_api_token(char *token, size_t token_len);

/**
 * @brief Verify dashboard login credentials
 * 
 * Uses SHA-256 hash comparison with stored salt.
 * 
 * @param password Plaintext password to verify
 * @return true if password is correct, false otherwise
 */
bool config_manager_verify_dashboard_password(const char *password);

/**
 * @brief Set and save a new dashboard login password
 * 
 * Generates a random salt and stores the SHA-256 hash in NVS.
 * 
 * @param password New plaintext password
 * @return true if successful, false otherwise
 */
bool config_manager_set_dashboard_password(const char *password);

/**
 * @brief Check if WiFi configuration has been customized
 * 
 * @return true if custom config exists in NVS, false if using defaults
 */
bool config_manager_is_sta_custom(void);

/**
 * @brief Reset the configuration manager back to firmware defaults
 * 
 * Clears custom credentials and NVS variables.
 * 
 * @return true if successful, false otherwise
 */
bool config_manager_reset(void);

/**
 * @brief Triggers a non-blocking background task to reconnect to WiFi STA
 * 
 * @return true if reconnection started successfully, false otherwise
 */
bool config_manager_reconnect_wifi(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
