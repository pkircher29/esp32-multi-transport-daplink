#include "sdkconfig.h"
#include "wifi_config_nvs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WIFI_CONFIG_NVS";
static const char *NVS_NAMESPACE = "wifi_config";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASSWORD = "password";

static custom_wifi_config_t current_config = {0};
static bool nvs_initialized = false;

bool wifi_config_init(void) {
    if (nvs_initialized) {
        return true;
    }

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition full or version mismatch, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Try to load saved config from NVS
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        // Try to read SSID
        size_t ssid_len = sizeof(current_config.ssid);
        ret = nvs_get_str(nvs_handle, NVS_KEY_SSID, current_config.ssid, &ssid_len);
        
        if (ret == ESP_OK) {
            // SSID found, try to read password
            size_t pwd_len = sizeof(current_config.password);
            ret = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, current_config.password, &pwd_len);
            
            if (ret == ESP_OK) {
                // Both found - use saved config
                current_config.configured = true;
                ESP_LOGI(TAG, "Loaded WiFi config from NVS: %s", current_config.ssid);
                nvs_close(nvs_handle);
                nvs_initialized = true;
                return true;
            }
        }
        nvs_close(nvs_handle);
    }

    // No valid config in NVS, use defaults from menuconfig
    strlcpy(current_config.ssid, CONFIG_ESP_WIFI_SSID, sizeof(current_config.ssid));
    strlcpy(current_config.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(current_config.password));
    current_config.configured = false;  // Indicates using defaults, not custom
    
    ESP_LOGI(TAG, "Using default WiFi config from menuconfig: %s", current_config.ssid);
    
    nvs_initialized = true;
    return true;
}

bool wifi_config_get(custom_wifi_config_t *config) {
    if (!config || !nvs_initialized) {
        return false;
    }

    memcpy(config, &current_config, sizeof(custom_wifi_config_t));
    return true;
}

bool wifi_config_set(const char *ssid, const char *password) {
    if (!ssid || !password || !nvs_initialized) {
        return false;
    }

    // Validate lengths
    if (strlen(ssid) >= WIFI_SSID_MAX_LEN || strlen(password) >= WIFI_PASSWORD_MAX_LEN) {
        ESP_LOGE(TAG, "SSID or password too long");
        return false;
    }

    // Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return false;
    }

    // Write SSID
    ret = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return false;
    }

    // Write password
    ret = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return false;
    }

    // Commit changes to NVS
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return false;
    }

    // Update in-memory config
    strlcpy(current_config.ssid, ssid, sizeof(current_config.ssid));
    strlcpy(current_config.password, password, sizeof(current_config.password));
    current_config.configured = true;

    ESP_LOGI(TAG, "WiFi config saved to NVS: %s", ssid);
    return true;
}

bool wifi_config_reset(void) {
    if (!nvs_initialized) {
        return false;
    }

    // Open and erase NVS namespace
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    // Reset to defaults
    strlcpy(current_config.ssid, CONFIG_ESP_WIFI_SSID, sizeof(current_config.ssid));
    strlcpy(current_config.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(current_config.password));
    current_config.configured = false;

    ESP_LOGI(TAG, "WiFi config reset to defaults");
    return true;
}

bool wifi_config_reconnect(void) {
    if (!nvs_initialized) {
        return false;
    }

    // Prepare WiFi config
    wifi_sta_config_t sta_config = {
        .ssid = {0},
        .password = {0},
    };
    
    strlcpy((char *)sta_config.ssid, current_config.ssid, sizeof(sta_config.ssid));
    strlcpy((char *)sta_config.password, current_config.password, sizeof(sta_config.password));

    // Apply new config
    wifi_config_t wifi_config = {
        .sta = sta_config,
    };
    
    // Stop WiFi, change mode to STA, apply config, start and connect
    esp_wifi_stop();
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode to STA: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return false;
    }

    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi mode switched to STA and reconnection initiated with SSID: %s", current_config.ssid);
    return true;
}

bool wifi_config_is_custom(void) {
    return current_config.configured;
}
