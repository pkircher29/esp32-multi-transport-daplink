#include "config_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CONFIG_MANAGER";
static const char *NVS_NAMESPACE = "airtap_cfg";

// NVS Keys (Max 15 characters)
static const char *NVS_KEY_STA_SSID     = "sta_ssid";
static const char *NVS_KEY_STA_PWD      = "sta_pwd";
static const char *NVS_KEY_AP_SSID      = "ap_ssid";
static const char *NVS_KEY_AP_PWD       = "ap_pwd";
static const char *NVS_KEY_API_TOKEN    = "api_token";
static const char *NVS_KEY_DASH_HASH    = "dash_hash";
static const char *NVS_KEY_DASH_SALT    = "dash_salt";
static const char *NVS_KEY_DEVICE_NAME  = "dev_name";
static const char *NVS_KEY_STA_CFG      = "sta_cfg";

static system_config_t active_config = {0};
static bool manager_initialized = false;

// Forward declarations
static void generate_secure_token(char *buf, size_t max_len);
static void compute_sha256_hash(const char *password, const char *salt, char *out_hash_hex);

bool config_manager_init(void) {
    if (manager_initialized) {
        return true;
    }

    // Initialize NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition full or mismatched version, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return false;
    }

    // Get MAC address for auto-generations
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_suffix[7];
    snprintf(mac_suffix, sizeof(mac_suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    // Load Device Name
    size_t name_len = sizeof(active_config.device_name);
    ret = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_NAME, active_config.device_name, &name_len);
    if (ret != ESP_OK) {
        snprintf(active_config.device_name, sizeof(active_config.device_name), "Airtap-%s", mac_suffix);
        nvs_set_str(nvs_handle, NVS_KEY_DEVICE_NAME, active_config.device_name);
        ESP_LOGI(TAG, "Generated default device name: %s", active_config.device_name);
    }

    // Load WiFi STA Configuration
    uint8_t is_sta_cfg = 0;
    nvs_get_u8(nvs_handle, NVS_KEY_STA_CFG, &is_sta_cfg);
    active_config.sta_configured = (is_sta_cfg != 0);

    size_t sta_ssid_len = sizeof(active_config.sta_ssid);
    ret = nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, active_config.sta_ssid, &sta_ssid_len);
    if (ret != ESP_OK) {
        strlcpy(active_config.sta_ssid, CONFIG_ESP_WIFI_SSID, sizeof(active_config.sta_ssid));
    }

    size_t sta_pwd_len = sizeof(active_config.sta_password);
    ret = nvs_get_str(nvs_handle, NVS_KEY_STA_PWD, active_config.sta_password, &sta_pwd_len);
    if (ret != ESP_OK) {
        strlcpy(active_config.sta_password, CONFIG_ESP_WIFI_PASSWORD, sizeof(active_config.sta_password));
    }

    // Load SoftAP configuration
    size_t ap_ssid_len = sizeof(active_config.ap_ssid);
    ret = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, active_config.ap_ssid, &ap_ssid_len);
    if (ret != ESP_OK) {
        snprintf(active_config.ap_ssid, sizeof(active_config.ap_ssid), "Airtap-%s", mac_suffix);
        nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, active_config.ap_ssid);
    }

    size_t ap_pwd_len = sizeof(active_config.ap_password);
    ret = nvs_get_str(nvs_handle, NVS_KEY_AP_PWD, active_config.ap_password, &ap_pwd_len);
    if (ret != ESP_OK) {
        // Generate password from MAC, e.g. "Airtap-7AF23C"
        snprintf(active_config.ap_password, sizeof(active_config.ap_password), "Airtap-%s", mac_suffix);
        nvs_set_str(nvs_handle, NVS_KEY_AP_PWD, active_config.ap_password);
        ESP_LOGI(TAG, "Generated secure SoftAP password from MAC: %s", active_config.ap_password);
    }

    // Load API Security Token
    size_t token_len = sizeof(active_config.api_token);
    ret = nvs_get_str(nvs_handle, NVS_KEY_API_TOKEN, active_config.api_token, &token_len);
    if (ret != ESP_OK) {
        generate_secure_token(active_config.api_token, sizeof(active_config.api_token));
        nvs_set_str(nvs_handle, NVS_KEY_API_TOKEN, active_config.api_token);
        ESP_LOGI(TAG, "Generated secure API security token: %s", active_config.api_token);
    }

    // Load Dashboard Password Hash & Salt
    size_t hash_len = sizeof(active_config.dash_pass_hash);
    size_t salt_len = sizeof(active_config.dash_pass_salt);
    ret = nvs_get_str(nvs_handle, NVS_KEY_DASH_HASH, active_config.dash_pass_hash, &hash_len);
    esp_err_t salt_ret = nvs_get_str(nvs_handle, NVS_KEY_DASH_SALT, active_config.dash_pass_salt, &salt_len);

    if (ret != ESP_OK || salt_ret != ESP_OK) {
        // Generate default password hash for "admin"
        ESP_LOGI(TAG, "No dashboard password found. Initializing with default: admin");
        
        // Generate a random salt
        generate_secure_token(active_config.dash_pass_salt, 17); // 16 hex chars + null
        active_config.dash_pass_salt[16] = '\0';
        nvs_set_str(nvs_handle, NVS_KEY_DASH_SALT, active_config.dash_pass_salt);
        
        // Hash default password "admin"
        compute_sha256_hash("admin", active_config.dash_pass_salt, active_config.dash_pass_hash);
        nvs_set_str(nvs_handle, NVS_KEY_DASH_HASH, active_config.dash_pass_hash);
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Config Manager initialized successfully!");
    ESP_LOGI(TAG, "--- SECURITY INFO ---");
    ESP_LOGI(TAG, "SoftAP SSID: %s", active_config.ap_ssid);
    ESP_LOGI(TAG, "SoftAP Password: %s", active_config.ap_password);
    ESP_LOGI(TAG, "API Token: %s", active_config.api_token);
    ESP_LOGI(TAG, "---------------------");

    manager_initialized = true;
    return true;
}

bool config_manager_get(system_config_t *config) {
    if (!config || !manager_initialized) {
        return false;
    }
    memcpy(config, &active_config, sizeof(system_config_t));
    return true;
}

bool config_manager_set_sta_credentials(const char *ssid, const char *password) {
    if (!ssid || !password || !manager_initialized) {
        return false;
    }

    if (strlen(ssid) >= CONFIG_SSID_MAX_LEN || strlen(password) >= CONFIG_PASSWORD_MAX_LEN) {
        ESP_LOGE(TAG, "SSID or password too long");
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return false;

    nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, ssid);
    nvs_set_str(nvs_handle, NVS_KEY_STA_PWD, password);
    nvs_set_u8(nvs_handle, NVS_KEY_STA_CFG, 1);
    
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    strlcpy(active_config.sta_ssid, ssid, sizeof(active_config.sta_ssid));
    strlcpy(active_config.sta_password, password, sizeof(active_config.sta_password));
    active_config.sta_configured = true;

    ESP_LOGI(TAG, "WiFi station configuration saved: %s", ssid);
    return true;
}

bool config_manager_get_sta_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len) {
    if (!manager_initialized) return false;
    if (ssid) strlcpy(ssid, active_config.sta_ssid, ssid_len);
    if (password) strlcpy(password, active_config.sta_password, password_len);
    return true;
}

bool config_manager_set_ap_credentials(const char *ssid, const char *password) {
    if (!ssid || !password || !manager_initialized) return false;

    if (strlen(ssid) >= CONFIG_SSID_MAX_LEN || strlen(password) >= CONFIG_PASSWORD_MAX_LEN) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return false;

    nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, ssid);
    nvs_set_str(nvs_handle, NVS_KEY_AP_PWD, password);

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    strlcpy(active_config.ap_ssid, ssid, sizeof(active_config.ap_ssid));
    strlcpy(active_config.ap_password, password, sizeof(active_config.ap_password));

    return true;
}

bool config_manager_get_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len) {
    if (!manager_initialized) return false;
    if (ssid) strlcpy(ssid, active_config.ap_ssid, ssid_len);
    if (password) strlcpy(password, active_config.ap_password, password_len);
    return true;
}

bool config_manager_set_device_name(const char *name) {
    if (!name || !manager_initialized) return false;

    if (strlen(name) >= CONFIG_NAME_MAX_LEN) return false;

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return false;

    nvs_set_str(nvs_handle, NVS_KEY_DEVICE_NAME, name);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    strlcpy(active_config.device_name, name, sizeof(active_config.device_name));
    return true;
}

bool config_manager_get_device_name(char *name, size_t name_len) {
    if (!manager_initialized) return false;
    if (name) strlcpy(name, active_config.device_name, name_len);
    return true;
}

bool config_manager_set_api_token(const char *token) {
    if (!token || !manager_initialized) return false;

    if (strlen(token) >= CONFIG_TOKEN_MAX_LEN) return false;

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return false;

    nvs_set_str(nvs_handle, NVS_KEY_API_TOKEN, token);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    strlcpy(active_config.api_token, token, sizeof(active_config.api_token));
    return true;
}

bool config_manager_get_api_token(char *token, size_t token_len) {
    if (!manager_initialized) return false;
    if (token) strlcpy(token, active_config.api_token, token_len);
    return true;
}

bool config_manager_verify_dashboard_password(const char *password) {
    if (!password || !manager_initialized) return false;

    char check_hash[CONFIG_HASH_MAX_LEN];
    compute_sha256_hash(password, active_config.dash_pass_salt, check_hash);

    return (strcmp(check_hash, active_config.dash_pass_hash) == 0);
}

bool config_manager_set_dashboard_password(const char *password) {
    if (!password || !manager_initialized) return false;

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return false;

    // Generate new secure random salt
    generate_secure_token(active_config.dash_pass_salt, 17);
    active_config.dash_pass_salt[16] = '\0';
    nvs_set_str(nvs_handle, NVS_KEY_DASH_SALT, active_config.dash_pass_salt);

    // Compute hash
    compute_sha256_hash(password, active_config.dash_pass_salt, active_config.dash_pass_hash);
    nvs_set_str(nvs_handle, NVS_KEY_DASH_HASH, active_config.dash_pass_hash);

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Dashboard password changed and hashed successfully.");
    return true;
}

bool config_manager_is_sta_custom(void) {
    return active_config.sta_configured;
}

bool config_manager_reset(void) {
    if (!manager_initialized) return false;

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    active_config.sta_configured = false;
    strlcpy(active_config.sta_ssid, CONFIG_ESP_WIFI_SSID, sizeof(active_config.sta_ssid));
    strlcpy(active_config.sta_password, CONFIG_ESP_WIFI_PASSWORD, sizeof(active_config.sta_password));

    // Force re-init to rebuild default salts, tokens, and keys
    manager_initialized = false;
    return config_manager_init();
}

bool config_manager_reconnect_wifi(void) {
    if (!manager_initialized) return false;

    // Build the configurations
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, active_config.sta_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, active_config.sta_password, sizeof(wifi_config.sta.password));

    esp_wifi_stop();
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return false;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) return false;

    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi Station reconnect initiated for SSID: %s", active_config.sta_ssid);
    return true;
}

// Private helpers
static void generate_secure_token(char *buf, size_t max_len) {
    uint8_t rand_bytes[32];
    for (size_t i = 0; i < sizeof(rand_bytes); i += 4) {
        uint32_t val = esp_random();
        memcpy(&rand_bytes[i], &val, 4);
    }
    
    size_t hex_chars = (max_len - 1) / 2;
    if (hex_chars > 32) hex_chars = 32;

    for (size_t i = 0; i < hex_chars; i++) {
        sprintf(buf + (i * 2), "%02X", rand_bytes[i]);
    }
    buf[hex_chars * 2] = '\0';
}

static void compute_sha256_hash(const char *password, const char *salt, char *out_hash_hex) {
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 indicates standard SHA-256
    
    mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    mbedtls_sha256_update(&ctx, (const unsigned char *)salt, strlen(salt));
    
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    for (int i = 0; i < 32; i++) {
        sprintf(out_hash_hex + (i * 2), "%02x", hash[i]);
    }
    out_hash_hex[64] = '\0';
}
