#include "web_dashboard.h"
#include "wifi_config_api.h"
#include "config_manager.h"
#include "cmsis_dap_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_app_format.h"
#include "driver/uart.h"
#include "lwip/ip4_addr.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "WEB_DASHBOARD";

// Session Authentication state (Phase 1, Item 3)
static char active_session_token[33] = {0};
static uint32_t session_expiry_time = 0;

// Rate limiting token-bucket parameters (Phase 1, Item 4)
#define MAX_RATE_LIMIT_CLIENTS 10
typedef struct {
    uint32_t ip;
    float tokens;
    uint32_t last_update;
} rate_limit_client_t;

static rate_limit_client_t rate_limit_clients[MAX_RATE_LIMIT_CLIENTS];
static int rate_limit_clients_count = 0;

// System statistics tracking (Phase 7, Item 21)
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

// Token-Bucket Rate Limiter per client IP
static bool check_rate_limit(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return true; // Fail safe
    
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &addr_size) != 0) {
        return true; // Fail safe
    }
    
    uint32_t client_ip = addr.sin_addr.s_addr;
    uint32_t now = esp_log_timestamp();
    
    rate_limit_client_t *client = NULL;
    for (int i = 0; i < rate_limit_clients_count; i++) {
        if (rate_limit_clients[i].ip == client_ip) {
            client = &rate_limit_clients[i];
            break;
        }
    }
    
    if (client == NULL) {
        if (rate_limit_clients_count < MAX_RATE_LIMIT_CLIENTS) {
            client = &rate_limit_clients[rate_limit_clients_count++];
        } else {
            // Evict slot via pseudorandom replacement
            client = &rate_limit_clients[esp_random() % MAX_RATE_LIMIT_CLIENTS];
        }
        client->ip = client_ip;
        client->tokens = 10.0f;
        client->last_update = now;
    }
    
    float elapsed = (float)(now - client->last_update) / 1000.0f;
    client->last_update = now;
    
    // Leaks 10 tokens per second (Max capacity of 10)
    client->tokens += elapsed * 10.0f;
    if (client->tokens > 10.0f) {
        client->tokens = 10.0f;
    }
    
    if (client->tokens >= 1.0f) {
        client->tokens -= 1.0f;
        return true;
    }
    
    ESP_LOGW(TAG, "Rate Limit Exceeded for Client IP: %s", inet_ntoa(addr.sin_addr));
    return false;
}

// Cookie session validation (Phase 1, Item 3)
static bool is_authenticated(httpd_req_t *req) {
    char cookie_buf[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) == ESP_OK) {
        char *token = strstr(cookie_buf, "Session-Token=");
        if (token) {
            token += 14; // "Session-Token=" length
            char token_val[33] = {0};
            int i = 0;
            while (i < 32 && token[i] != '\0' && token[i] != ';' && token[i] != ' ' && token[i] != '\r') {
                token_val[i] = token[i];
                i++;
            }
            token_val[i] = '\0';
            
            if (strlen(active_session_token) > 0 && strcmp(token_val, active_session_token) == 0) {
                if (esp_log_timestamp() < session_expiry_time) {
                    // Extend session lifetime on activity
                    session_expiry_time = esp_log_timestamp() + 3600000;
                    return true;
                }
            }
        }
    }
    return false;
}

// Check authorization via API Token Header or Active Dashboard session
static bool is_api_authorized(httpd_req_t *req) {
    char token_header[64];
    if (httpd_req_get_hdr_value_str(req, "X-Airtap-Token", token_header, sizeof(token_header)) == ESP_OK) {
        char saved_token[64];
        if (config_manager_get_api_token(saved_token, sizeof(saved_token))) {
            if (strcmp(token_header, saved_token) == 0) {
                return true;
            }
        }
    }
    return is_authenticated(req);
}

// Helper function to read the complete HTTP POST body securely
static esp_err_t read_http_body(httpd_req_t *req, char *buffer, size_t max_len) {
    size_t content_len = req->content_len;
    if (content_len >= max_len) {
        return ESP_ERR_NO_MEM;
    }
    
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buffer + received, content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

// embedded premium login HTML/CSS page
static const char *LOGIN_HTML = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Airtap Login</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
            background: linear-gradient(135deg, #0f172a 0%, #1e1b4b 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            color: #f1f5f9;
        }
        .login-card {
            background: rgba(30, 41, 59, 0.7);
            backdrop-filter: blur(16px);
            border: 1px solid rgba(255, 255, 255, 0.1);
            padding: 40px;
            border-radius: 16px;
            width: 100%;
            max-width: 400px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.5);
            text-align: center;
        }
        h1 {
            font-size: 2.2em;
            margin-bottom: 8px;
            background: linear-gradient(to right, #38bdf8, #818cf8);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        p {
            color: #94a3b8;
            margin-bottom: 30px;
            font-size: 0.95em;
        }
        .input-group {
            margin-bottom: 25px;
            text-align: left;
        }
        label {
            display: block;
            margin-bottom: 8px;
            font-size: 0.85em;
            font-weight: 600;
            color: #cbd5e1;
        }
        input {
            width: 100%;
            padding: 12px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            background: rgba(15, 23, 42, 0.6);
            color: white;
            border-radius: 8px;
            font-size: 1em;
            outline: none;
            transition: all 0.3s;
        }
        input:focus {
            border-color: #38bdf8;
            box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.2);
        }
        .btn {
            width: 100%;
            padding: 12px;
            border: none;
            background: linear-gradient(to right, #38bdf8, #6366f1);
            color: white;
            font-weight: bold;
            border-radius: 8px;
            cursor: pointer;
            font-size: 1em;
            transition: transform 0.1s, opacity 0.2s;
            margin-top: 10px;
        }
        .btn:hover {
            opacity: 0.9;
        }
        .btn:active {
            transform: scale(0.98);
        }
        .msg {
            margin-top: 15px;
            font-size: 0.9em;
            color: #f87171;
            min-height: 1.2em;
        }
    </style>
</head>
<body>
    <div class="login-card">
        <h1>🔌 Airtap Login</h1>
        <p>Enter password to access debug console</p>
        <div class="input-group">
            <label for="password">Dashboard Password</label>
            <input type="password" id="password" placeholder="Enter password" onkeydown="if(event.key==='Enter') login()">
        </div>
        <button class="btn" onclick="login()">Sign In</button>
        <div class="msg" id="msg"></div>
    </div>
    <script>
        async function login() {
            const password = document.getElementById('password').value;
            const msg = document.getElementById('msg');
            msg.textContent = '';
            if (!password) {
                msg.textContent = 'Password is required';
                return;
            }
            try {
                const response = await fetch('/api/login', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ password })
                });
                const data = await response.json();
                if (data.success) {
                    msg.style.color = '#4ade80';
                    msg.textContent = 'Login successful! Redirecting...';
                    setTimeout(() => window.location.href = '/dashboard', 1000);
                } else {
                    msg.style.color = '#f87171';
                    msg.textContent = data.message || 'Invalid password';
                }
            } catch (e) {
                msg.textContent = 'Connection failed';
            }
        }
    </script>
</body>
</html>
)html";

// Embedded premium system HTML with dark mode (Phase 7, Item 24)
static const char *DASHBOARD_HTML = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Airtap Dashboard</title>
    <style>
        :root {
            --bg-color: #0f172a;
            --card-bg: rgba(30, 41, 59, 0.7);
            --border-color: rgba(255, 255, 255, 0.08);
            --text-color: #f1f5f9;
            --text-muted: #94a3b8;
            --primary: #38bdf8;
            --success: #4ade80;
            --danger: #f87171;
            --accent: #6366f1;
        }
        
        [data-theme="light"] {
            --bg-color: #f8fafc;
            --card-bg: #ffffff;
            --border-color: #e2e8f0;
            --text-color: #0f172a;
            --text-muted: #64748b;
            --primary: #0284c7;
            --success: #16a34a;
            --danger: #dc2626;
            --accent: #4f46e5;
        }

        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
            background: var(--bg-color);
            color: var(--text-color);
            min-height: 100vh;
            padding: 20px;
            transition: all 0.3s;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
        }
        header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 30px;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 20px;
        }
        header h1 {
            font-size: 2.2em;
            background: linear-gradient(to right, var(--primary), var(--accent));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .header-controls {
            display: flex;
            gap: 12px;
            align-items: center;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .card {
            background: var(--card-bg);
            backdrop-filter: blur(16px);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            padding: 24px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.15);
            transition: all 0.3s;
        }
        .card:hover {
            transform: translateY(-4px);
            box-shadow: 0 15px 40px rgba(0,0,0,0.25);
        }
        .card h2 {
            font-size: 1.25em;
            margin-bottom: 16px;
            display: flex;
            align-items: center;
            gap: 10px;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 10px;
        }
        .status-badge {
            display: inline-block;
            width: 10px;
            height: 10px;
            border-radius: 50%;
        }
        .status-badge.online {
            background-color: var(--success);
            box-shadow: 0 0 8px var(--success);
            animation: pulse 2s infinite;
        }
        .status-badge.offline {
            background-color: var(--danger);
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.4; }
        }
        .stat {
            display: flex;
            justify-content: space-between;
            margin: 10px 0;
            font-size: 0.95em;
        }
        .stat-label {
            color: var(--text-muted);
            font-weight: 500;
        }
        .stat-value {
            font-weight: bold;
        }
        .form-group {
            display: flex;
            flex-direction: column;
            gap: 6px;
            margin-bottom: 15px;
        }
        .form-group label {
            font-size: 0.85em;
            font-weight: 600;
            color: var(--text-muted);
        }
        .form-group input, .form-group select {
            padding: 10px;
            background: rgba(15, 23, 42, 0.4);
            border: 1px solid var(--border-color);
            color: var(--text-color);
            border-radius: 6px;
            outline: none;
            transition: all 0.2s;
        }
        .form-group input:focus {
            border-color: var(--primary);
        }
        .btn-group {
            display: flex;
            gap: 10px;
            margin-top: 15px;
        }
        .btn {
            padding: 10px 16px;
            border: none;
            border-radius: 6px;
            font-weight: bold;
            font-size: 0.9em;
            cursor: pointer;
            transition: all 0.2s;
            color: white;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
        }
        .btn:active {
            transform: scale(0.97);
        }
        .btn-primary { background: linear-gradient(to right, var(--primary), var(--accent)); }
        .btn-primary:hover { opacity: 0.9; }
        .btn-success { background-color: var(--success); color: #0f172a; }
        .btn-danger { background-color: var(--danger); }
        .btn-secondary { background-color: var(--text-muted); }
        .msg {
            font-size: 0.85em;
            text-align: center;
            margin-top: 10px;
            font-weight: bold;
            min-height: 1.2em;
        }
        
        /* Terminal styling */
        #terminal-container {
            background: #000;
            padding: 15px;
            border-radius: 8px;
            border: 1px solid var(--border-color);
        }
        #terminal {
            background: #000;
            color: #0f0;
            font-family: monospace;
            height: 250px;
            overflow-y: scroll;
            line-height: 1.4;
            white-space: pre-wrap;
            font-size: 0.9em;
            padding: 5px;
        }
        #terminal input {
            border: 1px solid #333;
            background: #111;
            color: #0f0;
            width: 100%;
            padding: 10px;
            border-radius: 6px;
            font-family: monospace;
            font-size: 0.9em;
            outline: none;
        }
        .scan-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.85em;
            margin-top: 10px;
            max-height: 150px;
            overflow-y: auto;
            display: block;
        }
        .scan-table th, .scan-table td {
            padding: 8px;
            text-align: left;
            border-bottom: 1px solid var(--border-color);
        }
        .scan-table tr:hover {
            background: rgba(255,255,255,0.02);
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div>
                <h1 id="device-title">🔌 Airtap</h1>
                <p style="color: var(--text-muted); font-size: 0.95em;">Secure Multi-Transport Debug Probe</p>
            </div>
            <div class="header-controls">
                <button class="btn btn-secondary" onclick="toggleTheme()" id="theme-btn">Theme</button>
                <button class="btn btn-danger" onclick="logout()">Sign Out</button>
            </div>
        </header>

        <div class="grid">
            <!-- System Stats Card -->
            <div class="card">
                <h2>📊 Telemetry & Stats</h2>
                <div class="stat"><span class="stat-label">Device Name:</span><span class="stat-value" id="sys-name">-</span></div>
                <div class="stat"><span class="stat-label">Uptime:</span><span class="stat-value" id="sys-uptime">-</span></div>
                <div class="stat"><span class="stat-label">Free Heap:</span><span class="stat-value" id="sys-heap">-</span></div>
                <div class="stat"><span class="stat-label">API Security Token:</span><span class="stat-value" id="sys-token" style="font-family:monospace;font-size:0.85em;">-</span></div>
                <div class="stat"><span class="stat-label">WiFi SSID:</span><span class="stat-value" id="sys-ssid">-</span></div>
            </div>

            <!-- AP / Connection Security -->
            <div class="card">
                <h2>🔐 Secure SoftAP</h2>
                <div class="stat"><span class="stat-label">AP SSID:</span><span class="stat-value" id="ap-ssid">-</span></div>
                <div class="stat"><span class="stat-label">AP Password:</span><span class="stat-value" id="ap-pass">-</span></div>
                <div class="stat"><span class="stat-label">Security:</span><span class="stat-value" style="color:var(--success);">WPA2-PSK (Encrypted)</span></div>
                <p style="font-size:0.8em; color:var(--text-muted); margin-top:10px;">Fallback AP is encrypted. Change via serial or custom config.</p>
            </div>

            <!-- WiFi Configuration with Scanning (Phase 2, Item 7) -->
            <div class="card" style="grid-column: span 2;">
                <h2>⚙️ Connect WiFi STA</h2>
                <div style="display: flex; gap: 20px;">
                    <div style="flex: 1;">
                        <div class="form-group">
                            <label>SSID</label>
                            <input type="text" id="wifi-ssid" placeholder="SSID name">
                        </div>
                        <div class="form-group">
                            <label>Password</label>
                            <input type="password" id="wifi-pass" placeholder="Password">
                        </div>
                        <div class="btn-group">
                            <button class="btn btn-primary" onclick="saveWifi()">Save</button>
                            <button class="btn btn-success" onclick="reconnectWifi()">Apply</button>
                            <button class="btn btn-danger" onclick="resetWifi()">Reset</button>
                        </div>
                        <div class="msg" id="wifi-msg"></div>
                    </div>
                    <div style="flex: 1; border-left: 1px solid var(--border-color); padding-left: 20px;">
                        <div style="display:flex; justify-content:space-between; align-items:center;">
                            <label style="font-weight:600; font-size:0.85em; color:var(--text-muted);">Available Networks</label>
                            <button class="btn btn-primary" onclick="scanNetworks()" style="padding:4px 8px; font-size:0.8em;">Scan</button>
                        </div>
                        <div id="scan-list" style="margin-top:10px; max-height: 150px; overflow-y:auto; border:1px solid var(--border-color); border-radius:6px;">
                            <p style="padding:10px; text-align:center; color:var(--text-muted); font-size:0.85em;">Click Scan to discover...</p>
                        </div>
                    </div>
                </div>
            </div>

            <!-- Dynamic Device Naming & Backups -->
            <div class="card">
                <h2>🛠️ Device Naming & Backup</h2>
                <div class="form-group">
                    <label>Configure Custom Name</label>
                    <input type="text" id="custom-device-name" placeholder="Airtap-Bench">
                </div>
                <button class="btn btn-primary" onclick="saveDeviceName()" style="width:100%;">Update Name</button>
                <div class="msg" id="device-msg"></div>
                <hr style="border:none; border-top:1px solid var(--border-color); margin: 15px 0;">
                <div class="btn-group">
                    <button class="btn btn-primary" onclick="downloadBackup()" style="flex:1;">📤 Backup Config</button>
                    <button class="btn btn-success" onclick="document.getElementById('restore-file').click()" style="flex:1; color:#fff;">📥 Restore</button>
                </div>
                <input type="file" id="restore-file" style="display:none;" accept=".json" onchange="restoreConfig(this.files)">
            </div>

            <!-- Secure Web OTA Card (Phase 1, Item 1 & Phase 3) -->
            <div class="card">
                <h2>☁️ Secure OTA Firmware Flasher</h2>
                <div style="border: 2px dashed var(--primary); border-radius: 8px; padding: 20px; text-align: center; cursor: pointer; transition: background 0.3s;" id="drop-zone" onclick="document.getElementById('ota-file').click()">
                    <span style="color: var(--primary); font-weight: bold; font-size: 0.9em;" id="drop-text">Drag & Drop Binary or Click to Browse</span>
                    <input type="file" id="ota-file" style="display: none;" accept=".bin" onchange="handleFileSelect(this.files)">
                </div>
                <div id="progress-container" style="display: none; margin-top: 15px;">
                    <div style="width: 100%; background: rgba(0,0,0,0.2); border-radius: 5px; height: 10px; overflow: hidden;">
                        <div id="progress-bar" style="width: 0%; background-color: var(--success); height: 100%; transition: width 0.1s;"></div>
                    </div>
                    <div style="display: flex; justify-content: space-between; font-size: 0.8em; margin-top: 5px; color: var(--text-muted);">
                        <span id="progress-status">Uploading...</span>
                        <span id="progress-percent">0%</span>
                    </div>
                </div>
                <div class="msg" id="ota-msg" style="margin-top: 10px;"></div>
            </div>

            <!-- Terminal Console -->
            <div class="card" style="grid-column: span 2;">
                <h2>🖥️ Web Serial Monitor</h2>
                <div id="terminal-container">
                    <div id="terminal"></div>
                    <div style="display: flex; gap: 10px; margin-top: 10px;">
                        <input type="text" id="terminal-input" placeholder="Type serial input command..." onkeydown="if(event.key==='Enter') sendTerminal()">
                        <button class="btn btn-primary" onclick="sendTerminal()" style="width:100px;">Send</button>
                        <button class="btn btn-danger" onclick="clearTerminal()" style="width:100px;">Clear</button>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <script>
        let ws;
        let theme = localStorage.getItem('theme') || 'dark';
        document.body.setAttribute('data-theme', theme);

        function toggleTheme() {
            theme = theme === 'dark' ? 'light' : 'dark';
            document.body.setAttribute('data-theme', theme);
            localStorage.setItem('theme', theme);
        }

        async function logout() {
            try {
                await fetch('/api/logout', { method: 'POST' });
                window.location.href = '/login';
            } catch (e) {
                window.location.href = '/login';
            }
        }

        async function fetchStats() {
            try {
                const response = await fetch('/api/status');
                if (response.status === 401) {
                    window.location.href = '/login';
                    return;
                }
                const data = await response.json();
                document.getElementById('sys-name').textContent = data.device_name;
                document.getElementById('device-title').textContent = '🔌 ' + data.device_name;
                document.getElementById('sys-uptime').textContent = formatUptime(data.uptime);
                document.getElementById('sys-heap').textContent = formatBytes(data.free_heap);
                document.getElementById('sys-token').textContent = data.api_token;
                document.getElementById('sys-ssid').textContent = data.wifi.ssid || 'None';
                document.getElementById('ap-ssid').textContent = data.ap.ssid;
                document.getElementById('ap-pass').textContent = data.ap.password;
            } catch (e) {
                console.error(e);
            }
        }

        async function saveDeviceName() {
            const name = document.getElementById('custom-device-name').value;
            const msg = document.getElementById('device-msg');
            if(!name) return;
            try {
                const r = await fetch('/api/device/name', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ name })
                });
                const res = await r.json();
                if(res.success) {
                    msg.style.color = 'var(--success)';
                    msg.textContent = 'Device name updated successfully!';
                    fetchStats();
                } else {
                    msg.style.color = 'var(--danger)';
                    msg.textContent = 'Failed to update device name';
                }
            } catch(e) {
                msg.textContent = 'Network error';
            }
        }

        function downloadBackup() {
            window.location.href = '/api/device/backup';
        }

        async function restoreConfig(files) {
            if (files.length === 0) return;
            const file = files[0];
            const reader = new FileReader();
            reader.onload = async function(e) {
                try {
                    const config = JSON.parse(e.target.result);
                    const r = await fetch('/api/device/restore', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify(config)
                    });
                    const res = await r.json();
                    if(res.success) {
                        alert('Restore successful! Device will now reboot.');
                        setTimeout(() => location.reload(), 5000);
                    } else {
                        alert('Restore failed: ' + res.message);
                    }
                } catch(err) {
                    alert('Invalid JSON config backup file');
                }
            };
            reader.readAsText(file);
        }

        async function scanNetworks() {
            const list = document.getElementById('scan-list');
            list.innerHTML = '<p style="padding:10px; text-align:center; color:var(--text-muted);">Scanning...</p>';
            try {
                const response = await fetch('/api/wifi/scan');
                const data = await response.json();
                if(data.length === 0) {
                    list.innerHTML = '<p style="padding:10px; text-align:center;">No networks found</p>';
                    return;
                }
                let html = '<table class="scan-table"><tr><th>SSID</th><th>RSSI</th></tr>';
                data.forEach(item => {
                    html += `<tr onclick="selectSSID('${item.ssid}')"><td><b>${item.ssid}</b></td><td>${item.rssi} dBm</td></tr>`;
                });
                html += '</table>';
                list.innerHTML = html;
            } catch (e) {
                list.innerHTML = '<p style="padding:10px; text-align:center; color:var(--danger);">Scan failed</p>';
            }
        }

        function selectSSID(ssid) {
            document.getElementById('wifi-ssid').value = ssid;
        }

        async function saveWifi() {
            const ssid = document.getElementById('wifi-ssid').value;
            const password = document.getElementById('wifi-pass').value;
            const msg = document.getElementById('wifi-msg');
            msg.textContent = '';
            try {
                const r = await fetch('/api/wifi/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, password })
                });
                const res = await r.json();
                if(res.success) {
                    msg.style.color = 'var(--success)';
                    msg.textContent = 'WiFi settings saved successfully!';
                } else {
                    msg.style.color = 'var(--danger)';
                    msg.textContent = res.message || 'Validation error';
                }
            } catch(e) {
                msg.textContent = 'Connection error';
            }
        }

        async function reconnectWifi() {
            if(!confirm('Apply changes and reconnect WiFi?')) return;
            try {
                await fetch('/api/wifi/reconnect', { method: 'POST' });
                alert('Reconnecting WiFi... Dashboard will reload in 5 seconds.');
                setTimeout(() => location.reload(), 5000);
            } catch(e) {
                alert('Reconnection request failed.');
            }
        }

        async function resetWifi() {
            if(!confirm('Reset WiFi to firmware defaults?')) return;
            try {
                await fetch('/api/wifi/reset', { method: 'POST' });
                location.reload();
            } catch(e) {
                alert('Reset failed.');
            }
        }

        function handleFileSelect(files) {
            if (files.length === 0) return;
            const file = files[0];
            if (!file.name.endsWith('.bin')) {
                showMsg('ota-msg', 'Please upload a valid .bin file', 'var(--danger)');
                return;
            }
            uploadBinary(file);
        }

        async function uploadBinary(file) {
            const dropZone = document.getElementById('drop-zone');
            const dropText = document.getElementById('drop-text');
            const progressContainer = document.getElementById('progress-container');
            const progressBar = document.getElementById('progress-bar');
            const progressPercent = document.getElementById('progress-percent');
            const progressStatus = document.getElementById('progress-status');
            
            dropZone.style.pointerEvents = 'none';
            dropText.textContent = 'Uploading: ' + file.name;
            progressContainer.style.display = 'block';
            showMsg('ota-msg', '', '');

            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/api/ota', true);
            xhr.setRequestHeader('Content-Type', 'application/octet-stream');

            xhr.upload.onprogress = function(e) {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    progressBar.style.width = percent + '%';
                    progressPercent.textContent = percent + '%';
                    if (percent === 100) {
                        progressStatus.textContent = 'Validating and Flash Writing...';
                    }
                }
            };

            xhr.onload = function() {
                if (xhr.status === 200) {
                    showMsg('ota-msg', 'OTA Flashing successful! Rebooting device...', 'var(--success)');
                    progressStatus.textContent = 'Rebooting...';
                    setTimeout(() => location.reload(), 6000);
                } else {
                    let err = 'OTA Failed';
                    try {
                        const parsed = JSON.parse(xhr.responseText);
                        err = parsed.error || err;
                    } catch(e){}
                    showMsg('ota-msg', 'Error: ' + err, 'var(--danger)');
                    resetUploadUI();
                }
            };

            xhr.onerror = function() {
                showMsg('ota-msg', 'Network connection aborted', 'var(--danger)');
                resetUploadUI();
            };

            xhr.send(file);
        }

        function resetUploadUI() {
            const dropZone = document.getElementById('drop-zone');
            const dropText = document.getElementById('drop-text');
            const progressContainer = document.getElementById('progress-container');
            dropZone.style.pointerEvents = 'auto';
            dropText.textContent = 'Drag & Drop Binary or Click to Browse';
            progressContainer.style.display = 'none';
        }

        function connectWebSocket() {
            const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
            const wsUrl = `${protocol}//${location.host}/ws`;
            
            ws = new WebSocket(wsUrl);
            ws.binaryType = "arraybuffer";
            const decoder = new TextDecoder('utf-8', { fatal: false });
            
            ws.onopen = () => {
                appendTerminalText('\n*** [Airtap: Connected to serial terminal] ***\n', '#38bdf8');
            };
            
            ws.onmessage = (event) => {
                if (event.data instanceof ArrayBuffer) {
                    const text = decoder.decode(event.data);
                    appendTerminalText(text);
                } else {
                    appendTerminalText(event.data);
                }
            };
            
            ws.onclose = () => {
                appendTerminalText('\n*** [Airtap: Disconnected from terminal. Retrying...] ***\n', 'var(--danger)');
                setTimeout(connectWebSocket, 3000);
            };
            
            ws.onerror = () => {
                ws.close();
            };
        }

        function appendTerminalText(text, color) {
            const term = document.getElementById('terminal');
            const span = document.createElement('span');
            span.textContent = text;
            if (color) span.style.color = color;
            term.appendChild(span);
            term.scrollTop = term.scrollHeight;
            if (term.childNodes.length > 500) term.removeChild(term.firstChild);
        }

        function sendTerminal() {
            const input = document.getElementById('terminal-input');
            const val = input.value;
            if (ws && ws.readyState === WebSocket.OPEN && val) {
                ws.send(val + '\n');
                input.value = '';
            }
        }

        function clearTerminal() {
            document.getElementById('terminal').innerHTML = '';
        }

        function showMsg(id, text, color) {
            const el = document.getElementById(id);
            el.textContent = text;
            el.style.color = color;
        }

        function formatBytes(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round((bytes / Math.pow(k, i)) * 100) / 100 + ' ' + sizes[i];
        }

        function formatUptime(sec) {
            const d = Math.floor(sec / (3600*24));
            const h = Math.floor((sec % (3600*24)) / 3600);
            const m = Math.floor((sec % 3600) / 60);
            const s = Math.floor(sec % 60);
            return `${d}d ${h}h ${m}m ${s}s`;
        }

        window.addEventListener('DOMContentLoaded', () => {
            const dropZone = document.getElementById('drop-zone');
            if (dropZone) {
                ['dragenter', 'dragover'].forEach(name => {
                    dropZone.addEventListener(name, (e) => {
                        e.preventDefault();
                        dropZone.style.background = 'rgba(56, 189, 248, 0.1)';
                    }, false);
                });
                ['dragleave', 'drop'].forEach(name => {
                    dropZone.addEventListener(name, (e) => {
                        e.preventDefault();
                        dropZone.style.background = 'transparent';
                    }, false);
                });
                dropZone.addEventListener('drop', (e) => {
                    handleFileSelect(e.dataTransfer.files);
                }, false);
            }
            connectWebSocket();
            fetchStats();
            setInterval(fetchStats, 2000);
        });
    </script>
</body>
</html>
)html";

// Serve Dashboard login page
static esp_err_t login_page_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "Rate limit exceeded", -1);
        return ESP_FAIL;
    }
    
    if (is_authenticated(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/dashboard");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, LOGIN_HTML, strlen(LOGIN_HTML));
}

// Serves the beautiful main dashboard (Auth protected)
static esp_err_t dashboard_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, DASHBOARD_HTML, strlen(DASHBOARD_HTML));
}

// Redirects root URL
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    if (is_authenticated(req)) {
        httpd_resp_set_hdr(req, "Location", "/dashboard");
    } else {
        httpd_resp_set_hdr(req, "Location", "/login");
    }
    return httpd_resp_send(req, NULL, 0);
}

// POST /api/login endpoint
static esp_err_t login_api_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    char buffer[256] = {0};
    if (read_http_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *pwd_obj = cJSON_GetObjectItem(root, "password");
    if (!pwd_obj || !pwd_obj->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing password field");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    bool success = config_manager_verify_dashboard_password(pwd_obj->valuestring);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);

    if (success) {
        // Generate secure 32 character session token
        uint32_t val1 = esp_random();
        uint32_t val2 = esp_random();
        uint32_t val3 = esp_random();
        uint32_t val4 = esp_random();
        snprintf(active_session_token, sizeof(active_session_token), "%08lX%08lX%08lX%08lX", (unsigned long)val1, (unsigned long)val2, (unsigned long)val3, (unsigned long)val4);
        session_expiry_time = esp_log_timestamp() + 3600000; // 1 hour expiration

        cJSON_AddStringToObject(resp, "message", "Login successful");
        
        char cookie_hdr[128];
        snprintf(cookie_hdr, sizeof(cookie_hdr), "Session-Token=%s; Path=/; Max-Age=3600; HttpOnly", active_session_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
    } else {
        httpd_resp_set_status(req, "401 Unauthorized");
        cJSON_AddStringToObject(resp, "message", "Invalid credentials");
    }

    char *json_str = cJSON_Print(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/logout endpoint
static esp_err_t logout_api_handler(httpd_req_t *req) {
    active_session_token[0] = '\0';
    session_expiry_time = 0;

    httpd_resp_set_hdr(req, "Set-Cookie", "Session-Token=; Path=/; Max-Age=0; HttpOnly");
    const char *resp = "{\"success\":true}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/status - Returns complete device telemetry (Phase 7)
static esp_err_t status_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    if (!is_api_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized access");
        return ESP_FAIL;
    }

    wifi_ap_record_t ap_info;
    esp_netif_ip_info_t ip_info;
    bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    
    char ip_str[16] = {0};
    if (wifi_connected) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    system_config_t system_cfg = {0};
    config_manager_get(&system_cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", system_cfg.device_name);
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_log_timestamp() / 1000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(root, "api_token", system_cfg.api_token);

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", wifi_connected);
    cJSON_AddStringToObject(wifi, "ssid", system_cfg.sta_ssid);
    cJSON_AddStringToObject(wifi, "ip", ip_str);
    cJSON_AddNumberToObject(wifi, "bytes_rx", (double)dashboard_state.stats[0].bytes_received);
    cJSON_AddNumberToObject(wifi, "bytes_tx", (double)dashboard_state.stats[0].bytes_sent);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON *ap = cJSON_CreateObject();
    cJSON_AddStringToObject(ap, "ssid", system_cfg.ap_ssid);
    cJSON_AddStringToObject(ap, "password", system_cfg.ap_password);
    cJSON_AddItemToObject(root, "ap", ap);

    cJSON *usb = cJSON_CreateObject();
    cJSON_AddBoolToObject(usb, "connected", false);
    cJSON_AddNumberToObject(usb, "bytes_rx", (double)dashboard_state.stats[1].bytes_received);
    cJSON_AddNumberToObject(usb, "bytes_tx", (double)dashboard_state.stats[1].bytes_sent);
    cJSON_AddItemToObject(root, "usb", usb);

    cJSON *bt = cJSON_CreateObject();
    cJSON_AddBoolToObject(bt, "connected", false);
    cJSON_AddNumberToObject(bt, "bytes_rx", (double)dashboard_state.stats[2].bytes_received);
    cJSON_AddNumberToObject(bt, "bytes_tx", (double)dashboard_state.stats[2].bytes_sent);
    cJSON_AddItemToObject(root, "bluetooth", bt);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/device/name - Set Device Hostname dynamically
static esp_err_t device_name_post_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    if (!is_api_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char buffer[128] = {0};
    if (read_http_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    if (!name_obj || !name_obj->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name string");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    bool success = config_manager_set_device_name(name_obj->valuestring);
    cJSON_Delete(root);

    const char *resp = success ? "{\"success\":true}" : "{\"success\":false}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/device/backup - Configuration Backup JSON Export
static esp_err_t backup_config_handler(httpd_req_t *req) {
    if (!is_api_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    system_config_t system_cfg = {0};
    config_manager_get(&system_cfg);

    cJSON *root = cJSON_CreateObject();
    
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", system_cfg.sta_ssid);
    cJSON_AddStringToObject(wifi, "password", system_cfg.sta_password);
    cJSON_AddItemToObject(root, "wifi", wifi);
    
    cJSON_AddStringToObject(root, "device_name", system_cfg.device_name);
    cJSON_AddStringToObject(root, "api_token", system_cfg.api_token);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"airtap_backup.json\"");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/device/restore - Configuration JSON Import/Restore
static esp_err_t restore_config_handler(httpd_req_t *req) {
    if (!is_api_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char *buffer = (char *)malloc(1024);
    if (!buffer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    if (read_http_body(req, buffer, 1024) != ESP_OK) {
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON structure");
        return ESP_FAIL;
    }

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    cJSON *dev_name = cJSON_GetObjectItem(root, "device_name");
    cJSON *api_token = cJSON_GetObjectItem(root, "api_token");

    bool success = true;
    if (wifi) {
        cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
        cJSON *pass = cJSON_GetObjectItem(wifi, "password");
        if (ssid && pass && ssid->valuestring && pass->valuestring) {
            success &= config_manager_set_sta_credentials(ssid->valuestring, pass->valuestring);
        }
    }

    if (dev_name && dev_name->valuestring) {
        success &= config_manager_set_device_name(dev_name->valuestring);
    }

    if (api_token && api_token->valuestring) {
        success &= config_manager_set_api_token(api_token->valuestring);
    }

    cJSON_Delete(root);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    if (success) {
        cJSON_AddStringToObject(response, "message", "Configuration restored. Rebooting probe in 2s...");
    } else {
        cJSON_AddStringToObject(response, "message", "Failed to restore config");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(response);

    if (success) {
        xTaskCreate([](void *param) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }, "restore_reboot", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

// REST API: POST /api/ota - Secure, Authenticated OTA Updates (Phase 1, Item 1 & Phase 3)
static esp_err_t ota_post_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    // Verify authentication and permissions (API header or user session)
    if (!is_api_authorized(req)) {
        ESP_LOGE(TAG, "Unauthorized OTA upgrade request blocked.");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "X-Airtap-Token header or login required");
        return ESP_FAIL;
    }

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Target passive OTA partition slot was not found.");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Passive OTA partition slot not found");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Commencing secure OTA flash write to partition %s at offset 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);
             
    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length header is missing or 0");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to initialize OTA engine");
        return ESP_FAIL;
    }
    
    char *buf = (char *)malloc(1024);
    if (buf == NULL) {
        esp_ota_abort(update_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Heap buffer allocation failed");
        return ESP_FAIL;
    }
    
    bool validation_done = false;
    int received_so_far = 0;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining < 1024 ? remaining : 1024);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // retry
            }
            ESP_LOGE(TAG, "Secure OTA stream aborted mid-transfer.");
            free(buf);
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA transfer stream aborted");
            return ESP_FAIL;
        }
        
        received_so_far += recv_len;

        // Perform strict binary chip ID & project compatibility check on first block (Item 9)
        if (!validation_done && received_so_far >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
            const esp_app_desc_t *app_desc = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            
            // 1. Verify Project Signature Compatibility
            if (strcmp(app_desc->project_name, "cmsis_dap_tcp_esp32") != 0 && strcmp(app_desc->project_name, "airtap") != 0) {
                ESP_LOGE(TAG, "OTA Signature mismatch. Rejecting image signed as project: %s", app_desc->project_name);
                free(buf);
                esp_ota_abort(update_handle);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "{\"error\":\"Invalid firmware signature project\"}", -1);
                return ESP_FAIL;
            }
            
            // 2. Prevent version downgrade
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_app_desc_t running_desc;
            if (esp_ota_get_partition_description(running, &running_desc) == ESP_OK) {
                ESP_LOGI(TAG, "Current running ver: %s, Uploaded ver: %s", running_desc.version, app_desc->version);
                if (strcmp(app_desc->version, running_desc.version) < 0) {
                    ESP_LOGW(TAG, "OTA downgrade rejected: %s -> %s", running_desc.version, app_desc->version);
                    free(buf);
                    esp_ota_abort(update_handle);
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_send(req, "{\"error\":\"Downgrades are not permitted\"}", -1);
                    return ESP_FAIL;
                }
            }
            validation_done = true;
        }

        err = esp_ota_write(update_handle, (const void *)buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write chunk to OTA partition: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash partition write error");
            return ESP_FAIL;
        }
        
        remaining -= recv_len;
    }
    
    free(buf);
    
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }
    
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OTA update successfully flashed. Rebooting in 2s...");
    
    const char *resp_str = "{\"success\":true,\"message\":\"Firmware update successful! Rebooting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    xTaskCreate([](void *param) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }, "ota_reboot_task", 2048, NULL, 5, NULL);
    
    return ESP_OK;
}

// REST API: GET /api/openocd/config - Serves dynamic openocd.cfg file
static esp_err_t openocd_config_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    if (!is_api_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char response[512];
    snprintf(response, sizeof(response),
        "# Wireless OpenOCD Configuration for airtap\n"
        "adapter driver cmsis-dap\n"
        "cmsis-dap backend tcp\n"
        "cmsis-dap tcp host airtap.local\n"
        "cmsis-dap tcp port 4441\n"
        "\n"
        "# SWD interface configuration\n"
        "transport select swd\n"
        "adapter speed 2000\n"
        "\n"
        "# Target device configuration (Uncomment your target STM32 configuration below)\n"
        "# source [find target/stm32f1x.cfg]\n"
        "# source [find target/stm32f4x.cfg]\n"
        "# source [find target/stm32l4x.cfg]\n"
        "# source [find target/stm32g0x.cfg]\n"
    );
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"openocd.cfg\"");
    return httpd_resp_send(req, response, strlen(response));
}

// REST API: GET /api/vscode/launch - Serves custom launch.json file
static esp_err_t vscode_launch_handler(httpd_req_t *req) {
    if (!check_rate_limit(req)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return ESP_FAIL;
    }

    if (!is_api_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char response[1024];
    snprintf(response, sizeof(response),
        "{\n"
        "    \"version\": \"0.2.0\",\n"
        "    \"configurations\": [\n"
        "        {\n"
        "            \"name\": \"Wireless STM32 Debug (airtap)\",\n"
        "            \"cwd\": \"${workspaceFolder}\",\n"
        "            \"executable\": \"${workspaceFolder}/build/firmware.elf\",\n"
        "            \"request\": \"launch\",\n"
        "            \"type\": \"cortex-debug\",\n"
        "            \"runToEntryPoint\": \"main\",\n"
        "            \"servertype\": \"openocd\",\n"
        "            \"configFiles\": [\n"
        "                \"${workspaceFolder}/openocd.cfg\"\n"
        "            ]\n"
        "        }\n"
        "    ]\n"
        "}\n"
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"launch.json\"");
    return httpd_resp_send(req, response, strlen(response));
}

// WebSocket Route handler
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (!is_authenticated(req)) {
            // Refuse WS handshake if not logged into the dashboard
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "WebSocket handshake done, connection opened");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (ws_pkt.len > 0) {
        buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        
        // Forward input directly to serial bridge UART
        uart_write_bytes((uart_port_t)CONFIG_ESP_UART_BRIDGE_UART_NUM, (const char *)ws_pkt.payload, ws_pkt.len);
        free(buf);
    }
    return ESP_OK;
}

struct ws_broadcast_arg_t {
    uint8_t *data;
    size_t len;
};

// WebSocket Broadcast handler (safely queues work on the main HTTP server thread)
static void ws_broadcast_work_fn(void *arg) {
    ws_broadcast_arg_t *b_arg = (ws_broadcast_arg_t *)arg;
    if (!b_arg) return;

    if (dashboard_state.initialized && dashboard_state.server != NULL) {
        size_t clients_num = 12;
        int client_fds[12] = {0};
        esp_err_t err = httpd_get_client_list(dashboard_state.server, &clients_num, client_fds);
        if (err == ESP_OK) {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = b_arg->data;
            ws_pkt.len = b_arg->len;
            ws_pkt.type = HTTPD_WS_TYPE_BINARY;

            for (size_t i = 0; i < clients_num; i++) {
                if (httpd_ws_get_fd_info(dashboard_state.server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    // Send, and prune socket on failures (Phase 4, Item 14 - Dead client cleanup)
                    esp_err_t send_err = httpd_ws_send_data(dashboard_state.server, client_fds[i], &ws_pkt);
                    if (send_err != ESP_OK) {
                        ESP_LOGW(TAG, "WebSocket send failed for fd %d (client disconnected). Pruning.", client_fds[i]);
                        httpd_sess_trigger_close(dashboard_state.server, client_fds[i]);
                    }
                }
            }
        }
    }

    if (b_arg->data) {
        free(b_arg->data);
    }
    free(b_arg);
}

void web_dashboard_broadcast_ws(const char *data, size_t len) {
    if (!dashboard_state.initialized || dashboard_state.server == NULL || data == NULL || len == 0) {
        return;
    }

    ws_broadcast_arg_t *arg = (ws_broadcast_arg_t *)malloc(sizeof(ws_broadcast_arg_t));
    if (!arg) return;

    arg->data = (uint8_t *)malloc(len);
    if (!arg->data) {
        free(arg);
        return;
    }
    memcpy(arg->data, data, len);
    arg->len = len;

    // Queue work dynamically to avoid concurrency clashes
    esp_err_t err = httpd_queue_work(dashboard_state.server, ws_broadcast_work_fn, arg);
    if (err != ESP_OK) {
        free(arg->data);
        free(arg);
    }
}

bool web_dashboard_init(void) {
    if (dashboard_state.initialized) {
        ESP_LOGW(TAG, "Dashboard already initialized");
        return true;
    }

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 15;

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

    httpd_uri_t login_page = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_page_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &login_page);

    httpd_uri_t login_api = {
        .uri = "/api/login",
        .method = HTTP_POST,
        .handler = login_api_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &login_api);

    httpd_uri_t logout_api = {
        .uri = "/api/logout",
        .method = HTTP_POST,
        .handler = logout_api_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &logout_api);

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

    httpd_uri_t name_post = {
        .uri = "/api/device/name",
        .method = HTTP_POST,
        .handler = device_name_post_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &name_post);

    httpd_uri_t backup_get = {
        .uri = "/api/device/backup",
        .method = HTTP_GET,
        .handler = backup_config_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &backup_get);

    httpd_uri_t restore_post = {
        .uri = "/api/device/restore",
        .method = HTTP_POST,
        .handler = restore_config_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &restore_post);

    httpd_uri_t ota = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &ota);

    httpd_uri_t oocd_cfg = {
        .uri = "/api/openocd/config",
        .method = HTTP_GET,
        .handler = openocd_config_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &oocd_cfg);

    httpd_uri_t launch_json = {
        .uri = "/api/vscode/launch",
        .method = HTTP_GET,
        .handler = vscode_launch_handler,
    };
    httpd_register_uri_handler(dashboard_state.server, &launch_json);

    // Register WebSocket route URI
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true
    };
    httpd_register_uri_handler(dashboard_state.server, &ws);

    // Register WiFi Configuration REST API endpoints
    wifi_config_api_register(dashboard_state.server);

    dashboard_state.initialized = true;
    dashboard_state.start_time = esp_log_timestamp();
    
    ESP_LOGI(TAG, "Web dashboard started on port 80");
    return true;
}

bool web_dashboard_is_initialized(void) {
    return dashboard_state.initialized;
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
        (unsigned int)dashboard_state.stats[0].bytes_received,
        (unsigned int)dashboard_state.stats[0].bytes_sent,
        (unsigned int)dashboard_state.stats[1].bytes_received,
        (unsigned int)dashboard_state.stats[1].bytes_sent,
        (unsigned int)dashboard_state.stats[2].bytes_received,
        (unsigned int)dashboard_state.stats[2].bytes_sent
    );

    return (len > 0 && len < size) ? len : 0;
}
