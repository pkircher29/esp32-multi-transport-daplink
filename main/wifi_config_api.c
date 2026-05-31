#include "wifi_config_api.h"
#include "wifi_config_nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "WIFI_CONFIG_API";

// GET /api/wifi/config - Get current configuration
static esp_err_t wifi_config_get_handler(httpd_req_t *req) {
    wifi_config_t config = {0};
    if (!wifi_config_get(&config)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get config");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", config.ssid);
    cJSON_AddBoolToObject(root, "configured", config.configured);
    cJSON_AddNumberToObject(root, "password_length", (double)strlen(config.password));

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/wifi/config - Set WiFi credentials
static esp_err_t wifi_config_set_handler(httpd_req_t *req) {
    char buffer[256] = {0};
    int received = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
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

    bool success = wifi_config_set(ssid_obj->valuestring, password_obj->valuestring);
    
    // Send response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    if (success) {
        cJSON_AddStringToObject(response, "message", "WiFi config saved. Call /api/wifi/reconnect to apply.");
    } else {
        cJSON_AddStringToObject(response, "message", "Failed to save WiFi config");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    
    return success ? ESP_OK : ESP_FAIL;
}

// POST /api/wifi/reconnect - Reconnect with stored credentials
static esp_err_t wifi_config_reconnect_handler(httpd_req_t *req) {
    bool success = wifi_config_reconnect();

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

// POST /api/wifi/reset - Reset to defaults
static esp_err_t wifi_config_reset_handler(httpd_req_t *req) {
    bool success = wifi_config_reset();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    if (success) {
        cJSON_AddStringToObject(response, "message", "WiFi config reset to defaults. Call /api/wifi/reconnect to apply.");
    } else {
        cJSON_AddStringToObject(response, "message", "Failed to reset config");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(response);
    
    return success ? ESP_OK : ESP_FAIL;
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

    ESP_LOGI(TAG, "WiFi configuration API registered");
    return ESP_OK;
}
