#include "wifi_config_api.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI_CONFIG_API";

// Helper function to read the complete HTTP POST body securely
static esp_err_t read_http_body(httpd_req_t *req, char *buffer, size_t max_len) {
    size_t content_len = req->content_len;
    if (content_len >= max_len) {
        ESP_LOGE(TAG, "Request body size (%d) exceeds max buffer size (%d)", (int)content_len, (int)max_len);
        return ESP_ERR_NO_MEM;
    }
    
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buffer + received, content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // Retry on timeout
            }
            ESP_LOGE(TAG, "Socket read error: %d", ret);
            return ESP_FAIL;
        }
        received += ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

// GET /api/wifi/config - Get current configuration status (excluding passwords)
static esp_err_t wifi_config_get_handler(httpd_req_t *req) {
    system_config_t config = {0};
    if (!config_manager_get(&config)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get config");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", config.sta_ssid);
    cJSON_AddBoolToObject(root, "configured", config.sta_configured);
    cJSON_AddNumberToObject(root, "password_length", (double)strlen(config.sta_password));

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/wifi/config - Validate and set WiFi STA credentials
static esp_err_t wifi_config_set_handler(httpd_req_t *req) {
    char buffer[512] = {0};
    if (read_http_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_obj = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_obj = cJSON_GetObjectItem(root, "password");

    if (!ssid_obj || !ssid_obj->valuestring || !password_obj || !password_obj->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID or password");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *ssid = ssid_obj->valuestring;
    const char *password = password_obj->valuestring;
    size_t ssid_len = strlen(ssid);
    size_t pwd_len = strlen(password);

    // WiFi Credential Validation (Phase 2, Item 6)
    cJSON *response = cJSON_CreateObject();
    bool valid = true;
    const char *err_msg = "";

    if (ssid_len < 1 || ssid_len > 31) {
        valid = false;
        err_msg = "SSID must be between 1 and 31 characters";
    } else if (pwd_len != 0 && (pwd_len < 8 || pwd_len > 63)) {
        // WPA2 Passwords must be between 8 and 63 characters (0 indicates open network)
        valid = false;
        err_msg = "Password must be between 8 and 63 characters";
    }

    bool success = false;
    if (valid) {
        success = config_manager_set_sta_credentials(ssid, password);
    }

    cJSON_AddBoolToObject(response, "success", success && valid);
    if (success && valid) {
        cJSON_AddStringToObject(response, "message", "WiFi config saved. Apply changes to reconnect.");
    } else {
        cJSON_AddStringToObject(response, "message", !valid ? err_msg : "Failed to save configuration");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    if (!valid || !success) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    
    return (success && valid) ? ESP_OK : ESP_FAIL;
}

// POST /api/wifi/reconnect - Trigger station reconnection
static esp_err_t wifi_config_reconnect_handler(httpd_req_t *req) {
    bool success = config_manager_reconnect_wifi();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    if (success) {
        cJSON_AddStringToObject(response, "message", "WiFi reconnection initiated");
    } else {
        cJSON_AddStringToObject(response, "message", "Failed to initiate reconnection");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    
    return success ? ESP_OK : ESP_FAIL;
}

// POST /api/wifi/reset - Revert WiFi parameters to defaults
static esp_err_t wifi_config_reset_handler(httpd_req_t *req) {
    bool success = config_manager_reset();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    if (success) {
        cJSON_AddStringToObject(response, "message", "WiFi config reset to defaults. Apply changes to reconnect.");
    } else {
        cJSON_AddStringToObject(response, "message", "Failed to reset configuration");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    
    return success ? ESP_OK : ESP_FAIL;
}

// GET /api/wifi/scan - Perform a active background network scan
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    // Start dynamic network scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 120,
    };
    
    ESP_LOGI(TAG, "Starting active WiFi scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed to start");
        return ESP_FAIL;
    }
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 30) ap_count = 30; // Cap to top 30 strongest networks
    
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        free(ap_records);
        ESP_LOGE(TAG, "Failed to get scan AP records: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to retrieve scan results");
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", (double)ap_records[i].rssi);
        cJSON_AddItemToArray(root, item);
    }
    
    free(ap_records);
    
    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t wifi_config_api_register(httpd_handle_t server) {
    if (!server) {
        return ESP_FAIL;
    }

    // GET /api/wifi/config
    httpd_uri_t wifi_get = {
        .uri = "/api/wifi/config",
        .method = HTTP_GET,
        .handler = wifi_config_get_handler,
    };
    httpd_register_uri_handler(server, &wifi_get);

    // POST /api/wifi/config
    httpd_uri_t wifi_set = {
        .uri = "/api/wifi/config",
        .method = HTTP_POST,
        .handler = wifi_config_set_handler,
    };
    httpd_register_uri_handler(server, &wifi_set);

    // POST /api/wifi/reconnect
    httpd_uri_t wifi_reconnect = {
        .uri = "/api/wifi/reconnect",
        .method = HTTP_POST,
        .handler = wifi_config_reconnect_handler,
    };
    httpd_register_uri_handler(server, &wifi_reconnect);

    // POST /api/wifi/reset
    httpd_uri_t wifi_reset = {
        .uri = "/api/wifi/reset",
        .method = HTTP_POST,
        .handler = wifi_config_reset_handler,
    };
    httpd_register_uri_handler(server, &wifi_reset);

    // GET /api/wifi/scan
    httpd_uri_t wifi_scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_handler,
    };
    httpd_register_uri_handler(server, &wifi_scan);

    ESP_LOGI(TAG, "WiFi REST API endpoints registered successfully");
    return ESP_OK;
}
