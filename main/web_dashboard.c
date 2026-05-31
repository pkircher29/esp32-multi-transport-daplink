#include "web_dashboard.h"
#include "cmsis_dap_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WEB_DASHBOARD";

// Statistics tracking
typedef struct {
    uint32_t bytes_received;
    uint32_t bytes_sent;
    uint32_t packet_count;
} transport_stats_t;

typedef struct {
    httpd_handle_t server;
    bool initialized;
    transport_stats_t stats[3];  // 0=WiFi, 1=USB, 2=Bluetooth
    uint32_t start_time;
} web_dashboard_state_t;

static web_dashboard_state_t dashboard_state = {0};

// Embedded HTML/CSS/JavaScript dashboard
static const char *DASHBOARD_HTML = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 CMSIS-DAP Dashboard</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
        }
        header {
            color: white;
            margin-bottom: 30px;
        }
        header h1 {
            font-size: 2.5em;
            margin-bottom: 10px;
        }
        header p {
            opacity: 0.9;
            font-size: 1.1em;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .card {
            background: white;
            border-radius: 10px;
            padding: 25px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
            transition: transform 0.3s, box-shadow 0.3s;
        }
        .card:hover {
            transform: translateY(-5px);
            box-shadow: 0 15px 50px rgba(0,0,0,0.3);
        }
        .card h2 {
            color: #333;
            font-size: 1.3em;
            margin-bottom: 15px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .status-badge {
            display: inline-block;
            width: 12px;
            height: 12px;
            border-radius: 50%;
            margin-right: 8px;
        }
        .status-badge.online {
            background-color: #4CAF50;
            animation: pulse 2s infinite;
        }
        .status-badge.offline {
            background-color: #f44336;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .stat {
            margin: 10px 0;
            color: #555;
            font-size: 0.95em;
        }
        .stat-label {
            font-weight: 600;
            color: #333;
        }
        .stat-value {
            color: #667eea;
            font-weight: bold;
            font-size: 1.1em;
        }
        .footer {
            color: white;
            text-align: center;
            opacity: 0.8;
            margin-top: 50px;
        }
        .loading {
            display: inline-block;
            width: 8px;
            height: 8px;
            background: #667eea;
            border-radius: 50%;
            animation: loading 1.4s infinite;
        }
        @keyframes loading {
            0%, 100% { transform: scale(0.8); opacity: 0.5; }
            50% { transform: scale(1); opacity: 1; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>🔌 CMSIS-DAP Dashboard</h1>
            <p>Multi-transport ESP32 Debugger Status Monitor</p>
        </header>

        <div class="grid">
            <!-- WiFi Card -->
            <div class="card">
                <h2>📡 WiFi / TCP</h2>
                <div class="stat">
                    <span class="status-badge" id="wifi-status"></span>
                    <span class="stat-label">Status:</span>
                    <span class="stat-value" id="wifi-status-text">Connecting...</span>
                </div>
                <div class="stat">
                    <span class="stat-label">IP Address:</span>
                    <span class="stat-value" id="wifi-ip">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Port:</span>
                    <span class="stat-value">4441</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Rx Bytes:</span>
                    <span class="stat-value" id="wifi-rx">0</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Tx Bytes:</span>
                    <span class="stat-value" id="wifi-tx">0</span>
                </div>
            </div>

            <!-- USB Card -->
            <div class="card">
                <h2>🔌 USB CDC</h2>
                <div class="stat">
                    <span class="status-badge" id="usb-status"></span>
                    <span class="stat-label">Status:</span>
                    <span class="stat-value" id="usb-status-text">Disconnected</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Product:</span>
                    <span class="stat-value">ESP32 CMSIS-DAP</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Rx Bytes:</span>
                    <span class="stat-value" id="usb-rx">0</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Tx Bytes:</span>
                    <span class="stat-value" id="usb-tx">0</span>
                </div>
            </div>

            <!-- Bluetooth Card -->
            <div class="card">
                <h2>📱 Bluetooth SPP</h2>
                <div class="stat">
                    <span class="status-badge" id="bt-status"></span>
                    <span class="stat-label">Status:</span>
                    <span class="stat-value" id="bt-status-text">Ready</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Device Name:</span>
                    <span class="stat-value">ESP32-CMSIS-DAP</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Rx Bytes:</span>
                    <span class="stat-value" id="bt-rx">0</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Tx Bytes:</span>
                    <span class="stat-value" id="bt-tx">0</span>
                </div>
            </div>
        </div>

        <footer class="footer">
            <p>Updating <span class="loading"></span></p>
        </footer>
    </div>

    <script>
        async function updateStatus() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();

                // WiFi
                const wifiStatus = document.getElementById('wifi-status');
                if (data.wifi.connected) {
                    wifiStatus.className = 'status-badge online';
                    document.getElementById('wifi-status-text').textContent = 'Connected';
                    document.getElementById('wifi-ip').textContent = data.wifi.ip || '-';
                } else {
                    wifiStatus.className = 'status-badge offline';
                    document.getElementById('wifi-status-text').textContent = 'Disconnected';
                    document.getElementById('wifi-ip').textContent = '-';
                }
                document.getElementById('wifi-rx').textContent = formatBytes(data.wifi.bytes_rx);
                document.getElementById('wifi-tx').textContent = formatBytes(data.wifi.bytes_tx);

                // USB
                const usbStatus = document.getElementById('usb-status');
                if (data.usb.connected) {
                    usbStatus.className = 'status-badge online';
                    document.getElementById('usb-status-text').textContent = 'Connected';
                } else {
                    usbStatus.className = 'status-badge offline';
                    document.getElementById('usb-status-text').textContent = 'Disconnected';
                }
                document.getElementById('usb-rx').textContent = formatBytes(data.usb.bytes_rx);
                document.getElementById('usb-tx').textContent = formatBytes(data.usb.bytes_tx);

                // Bluetooth
                const btStatus = document.getElementById('bt-status');
                if (data.bluetooth.connected) {
                    btStatus.className = 'status-badge online';
                    document.getElementById('bt-status-text').textContent = 'Connected';
                } else {
                    btStatus.className = 'status-badge offline';
                    document.getElementById('bt-status-text').textContent = 'Ready';
                }
                document.getElementById('bt-rx').textContent = formatBytes(data.bluetooth.bytes_rx);
                document.getElementById('bt-tx').textContent = formatBytes(data.bluetooth.bytes_tx);
            } catch (e) {
                console.error('Status update failed:', e);
            }
        }

        function formatBytes(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round((bytes / Math.pow(k, i)) * 100) / 100 + ' ' + sizes[i];
        }

        // Update status every 1 second
        updateStatus();
        setInterval(updateStatus, 1000);
    </script>
</body>
</html>
)";

// REST API: GET /api/status
static esp_err_t status_handler(httpd_req_t *req) {
    // Get WiFi status
    wifi_ap_record_t ap_info;
    esp_netif_ip_info_t ip_info;
    bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    
    char ip_str[16] = {0};
    if (wifi_connected) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    // Build JSON response
    char response[512] = {0};
    snprintf(response, sizeof(response),
        "{"
        "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"bytes_rx\":%u,\"bytes_tx\":%u},"
        "\"usb\":{\"connected\":false,\"bytes_rx\":%u,\"bytes_tx\":%u},"
        "\"bluetooth\":{\"connected\":false,\"bytes_rx\":%u,\"bytes_tx\":%u}"
        "}",
        wifi_connected ? "true" : "false",
        ip_str,
        dashboard_state.stats[0].bytes_received,
        dashboard_state.stats[0].bytes_sent,
        dashboard_state.stats[1].bytes_received,
        dashboard_state.stats[1].bytes_sent,
        dashboard_state.stats[2].bytes_received,
        dashboard_state.stats[2].bytes_sent
    );

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

// Serve dashboard HTML
static esp_err_t dashboard_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, DASHBOARD_HTML, strlen(DASHBOARD_HTML));
}

// Redirect root to dashboard
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/dashboard");
    return httpd_resp_send(req, NULL, 0);
}

bool web_dashboard_init(void) {
    if (dashboard_state.initialized) {
        ESP_LOGW(TAG, "Dashboard already initialized");
        return true;
    }

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;

    // Start HTTP server
    esp_err_t ret = httpd_start(&dashboard_state.server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }

    // Register URI handlers
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &root);

    httpd_uri_t dashboard = {
        .uri = "/dashboard",
        .method = HTTP_GET,
        .handler = dashboard_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &dashboard);

    httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &status);

    dashboard_state.initialized = true;
    dashboard_state.start_time = esp_log_timestamp();
    
    ESP_LOGI(TAG, "Web dashboard started on port 80");
    ESP_LOGI(TAG, "Access at http://ESP32_IP/dashboard");
    
    return true;
}

bool web_dashboard_is_initialized(void) {
    return dashboard_state.initialized;
}

uint16_t web_dashboard_get_status_json(char *buffer, uint16_t size) {
    if (!buffer || size == 0) return 0;

    wifi_ap_record_t ap_info;
    esp_netif_ip_info_t ip_info;
    bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    
    char ip_str[16] = {0};
    if (wifi_connected) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    int len = snprintf(buffer, size,
        "{"
        "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"bytes_rx\":%u,\"bytes_tx\":%u},"
        "\"usb\":{\"connected\":false,\"bytes_rx\":%u,\"bytes_tx\":%u},"
        "\"bluetooth\":{\"connected\":false,\"bytes_rx\":%u,\"bytes_tx\":%u}"
        "}",
        wifi_connected ? "true" : "false",
        ip_str,
        dashboard_state.stats[0].bytes_received,
        dashboard_state.stats[0].bytes_sent,
        dashboard_state.stats[1].bytes_received,
        dashboard_state.stats[1].bytes_sent,
        dashboard_state.stats[2].bytes_received,
        dashboard_state.stats[2].bytes_sent
    );

    return (len > 0 && len < size) ? len : 0;
}

void web_dashboard_update_stats(uint8_t transport_id, uint32_t bytes_received, uint32_t bytes_sent) {
    if (transport_id < 3) {
        dashboard_state.stats[transport_id].bytes_received += bytes_received;
        dashboard_state.stats[transport_id].bytes_sent += bytes_sent;
        dashboard_state.stats[transport_id].packet_count++;
    }
}

void web_dashboard_reset_stats(void) {
    memset(&dashboard_state.stats, 0, sizeof(dashboard_state.stats));
}
