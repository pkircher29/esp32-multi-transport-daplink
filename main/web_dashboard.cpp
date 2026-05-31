#include "web_dashboard.h"
#include "wifi_config_api.h"
#include "wifi_config_nvs.h"
#include "cmsis_dap_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "driver/uart.h"
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

// Embedded HTML/CSS/JavaScript dashboard using custom delimiter "html"
static const char *DASHBOARD_HTML = R"html(
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
        .form-group {
            display: flex;
            flex-direction: column;
            gap: 5px;
            margin-bottom: 12px;
        }
        .form-group label {
            font-size: 0.85em;
            font-weight: 600;
            color: #444;
        }
        .form-group input {
            padding: 10px;
            border: 1px solid #ddd;
            border-radius: 6px;
            font-size: 0.95em;
            outline: none;
            transition: border-color 0.2s;
        }
        .form-group input:focus {
            border-color: #667eea;
        }
        .btn-group {
            display: flex;
            gap: 8px;
            margin-top: 15px;
        }
        .btn {
            flex: 1;
            padding: 10px;
            border: none;
            border-radius: 6px;
            font-weight: bold;
            font-size: 0.9em;
            cursor: pointer;
            transition: background-color 0.2s, transform 0.1s;
            color: white;
        }
        .btn:active {
            transform: scale(0.98);
        }
        .btn-primary { background-color: #667eea; }
        .btn-primary:hover { background-color: #5a6fd6; }
        .btn-success { background-color: #4CAF50; }
        .btn-success:hover { background-color: #43a047; }
        .btn-danger { background-color: #f44336; }
        .btn-danger:hover { background-color: #e53935; }
        .msg {
            font-size: 0.85em;
            text-align: center;
            margin-top: 8px;
            font-weight: bold;
            min-height: 1.2em;
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
        #terminal-container:fullscreen {
            width: 100vw;
            height: 100vh;
            padding: 20px;
            background: #000;
            display: flex;
            flex-direction: column;
            gap: 15px;
            box-sizing: border-box;
            border: none;
            border-radius: 0;
        }
        #terminal-container:fullscreen #terminal {
            flex: 1 !important;
            height: 100% !important;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>🔌 Airtap Dashboard</h1>
            <p>Wireless Multi-transport ESP32 Debugger Status Monitor</p>
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

            <!-- WiFi Configuration Card -->
            <div class="card">
                <h2>⚙️ Configure WiFi</h2>
                <div class="form-group">
                    <label>Network SSID</label>
                    <input type="text" id="wifi-ssid" placeholder="Enter SSID">
                </div>
                <div class="form-group">
                    <label>Password</label>
                    <input type="password" id="wifi-pass" placeholder="Enter Password">
                </div>
                <div class="btn-group">
                    <button class="btn btn-primary" onclick="saveWifi()">Save</button>
                    <button class="btn btn-success" onclick="reconnectWifi()">Apply</button>
                    <button class="btn btn-danger" onclick="resetWifi()" title="Reset to default menuconfig credentials">Reset</button>
                </div>
                <div class="msg" id="wifi-msg"></div>
            </div>

            <!-- STM32 Out-Of-Box Assistant Card -->
            <div class="card" style="grid-column: span 2;">
                <h2>🚀 STM32 Out-Of-Box Assistant</h2>
                <p style="color: #666; font-size: 0.9em; margin-bottom: 15px;">Setup your STM32 wireless debug session in under 30 seconds!</p>
                <div style="display: flex; flex-wrap: wrap; gap: 20px; margin-bottom: 20px;">
                    <div style="flex: 1; min-width: 250px; background: #f9f9f9; padding: 15px; border-radius: 8px; border: 1px solid #eee;">
                        <h3 style="font-size: 1.05em; color: #333; margin-bottom: 10px;">🔌 SWD Connection Guide</h3>
                        <div class="stat" style="font-family: monospace; font-size: 0.9em; color: #444; line-height: 1.6;">
                            <div><b>Airtap Pin</b> &nbsp;&nbsp;&nbsp; ➡️ &nbsp; <b>STM32 Pin</b></div>
                            <hr style="margin: 5px 0; border: none; border-top: 1px solid #ddd;">
                            <div>GPIO 13 (TMS) ➡️ &nbsp; SWDIO / TMS</div>
                            <div>GPIO 14 (TCK) ➡️ &nbsp; SWCLK / TCK</div>
                            <div>GPIO 12 (RST) ➡️ &nbsp; nRESET (SRST)</div>
                            <div>GND &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; ➡️ &nbsp; GND</div>
                        </div>
                    </div>
                    <div style="flex: 1; min-width: 250px; display: flex; flex-direction: column; gap: 12px; justify-content: center;">
                        <button class="btn btn-primary" onclick="window.location.href='/api/openocd/config'" style="padding: 12px;">📥 Download openocd.cfg</button>
                        <button class="btn btn-success" onclick="window.location.href='/api/vscode/launch'" style="padding: 12px;">📥 Download launch.json</button>
                    </div>
                </div>
            </div>

            <!-- Web OTA Update Card -->
            <div class="card">
                <h2>☁️ Web OTA Update</h2>
                <p style="color: #666; font-size: 0.9em; margin-bottom: 15px;">Wirelessly flash a new firmware binary onto your Airtap.</p>
                <div class="form-group" style="border: 2px dashed #667eea; border-radius: 8px; padding: 20px; text-align: center; cursor: pointer; transition: background 0.3s;" id="drop-zone" onclick="document.getElementById('ota-file').click()">
                    <span style="color: #667eea; font-weight: bold; font-size: 0.95em;" id="drop-text">Drag & Drop Binary or Click to Browse</span>
                    <input type="file" id="ota-file" style="display: none;" accept=".bin" onchange="handleFileSelect(this.files)">
                </div>
                <div id="progress-container" style="display: none; margin-top: 15px;">
                    <div style="width: 100%; background-color: #ddd; border-radius: 5px; height: 10px; overflow: hidden;">
                        <div id="progress-bar" style="width: 0%; background-color: #4CAF50; height: 100%; transition: width 0.1s;"></div>
                    </div>
                    <div style="display: flex; justify-content: space-between; font-size: 0.8em; margin-top: 5px; color: #555;">
                        <span id="progress-status">Uploading...</span>
                        <span id="progress-percent">0%</span>
                    </div>
                </div>
                <div class="msg" id="ota-msg" style="margin-top: 10px;"></div>
            </div>

            <!-- Console Terminal Card -->
            <div class="card" style="grid-column: span 2;">
                <h2>🖥️ Web Serial Console</h2>
                <p style="color: #666; font-size: 0.9em; margin-bottom: 15px;">Live interactive console with the target device on UART1 (115200 baud).</p>
                <div id="terminal-container" style="background: #000; padding: 15px; border-radius: 8px; display: flex; flex-direction: column; gap: 10px; border: 1px solid #333;">
                    <div id="terminal" style="background: #000; color: #0f0; font-family: monospace; height: 300px; overflow-y: scroll; line-height: 1.4; white-space: pre-wrap; font-size: 0.9em; box-shadow: inset 0 0 10px rgba(0,255,0,0.5); padding: 5px; border: none; outline: none;"></div>
                    <div style="display: flex; gap: 10px; margin-top: 5px;">
                        <input type="text" id="terminal-input" placeholder="Type command here and press Enter..." style="flex: 1; padding: 10px; border: 1px solid #333; background: #111; color: #0f0; border-radius: 6px; font-family: monospace; outline: none; font-size: 0.9em;" onkeydown="handleTerminalKey(event)">
                        <button class="btn btn-primary" onclick="sendTerminalCommand()" style="flex: 0 0 100px;">Send</button>
                        <button class="btn btn-success" onclick="toggleFullscreen()" style="flex: 0 0 110px; background-color: #4CAF50;" id="fs-btn">Fullscreen</button>
                        <button class="btn btn-danger" onclick="clearTerminal()" style="flex: 0 0 100px; background-color: #555;">Clear</button>
                    </div>
                </div>
            </div>
        </div>

        <footer class="footer">
            <p>Updating <span class="loading"></span></p>
        </footer>
    </div>

    <script>
        let ws;

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

        async function loadWifiConfig() {
            try {
                const response = await fetch('/api/wifi/config');
                const data = await response.json();
                if (data.ssid) {
                    document.getElementById('wifi-ssid').value = data.ssid;
                    if (data.configured) {
                        showMsg('wifi-msg', 'Custom configuration loaded', '#4CAF50');
                    } else {
                        showMsg('wifi-msg', 'Using default credentials', '#ffa000');
                    }
                }
            } catch (e) {
                console.error('Failed to load WiFi config:', e);
            }
        }

        async function saveWifi() {
            const ssid = document.getElementById('wifi-ssid').value;
            const password = document.getElementById('wifi-pass').value;
            if (!ssid) {
                showMsg('wifi-msg', 'SSID is required', '#f44336');
                return;
            }
            try {
                showMsg('wifi-msg', 'Saving...', '#667eea');
                const response = await fetch('/api/wifi/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, password })
                });
                const data = await response.json();
                if (data.success) {
                    showMsg('wifi-msg', 'Configuration saved! Click Apply to reconnect.', '#4CAF50');
                } else {
                    showMsg('wifi-msg', data.message || 'Failed to save config', '#f44336');
                }
            } catch (e) {
                showMsg('wifi-msg', 'Connection failed', '#f44336');
            }
        }

        async function reconnectWifi() {
            if (!confirm('Reconnecting WiFi will temporarily interrupt your debug session. Proceed?')) return;
            try {
                showMsg('wifi-msg', 'Initiating reconnection...', '#667eea');
                const response = await fetch('/api/wifi/reconnect', { method: 'POST' });
                const data = await response.json();
                if (data.success) {
                    showMsg('wifi-msg', 'Reconnecting! Page will reload in 5 seconds...', '#4CAF50');
                    setTimeout(() => location.reload(), 5000);
                } else {
                    showMsg('wifi-msg', data.message || 'Failed to reconnect', '#f44336');
                }
            } catch (e) {
                showMsg('wifi-msg', 'Connection failed', '#f44336');
            }
        }

        async function resetWifi() {
            if (!confirm('Reset to default firmware credentials?')) return;
            try {
                showMsg('wifi-msg', 'Resetting...', '#667eea');
                const response = await fetch('/api/wifi/reset', { method: 'POST' });
                const data = await response.json();
                if (data.success) {
                    document.getElementById('wifi-pass').value = '';
                    showMsg('wifi-msg', 'Reset success! Click Apply to revert.', '#4CAF50');
                    await loadWifiConfig();
                } else {
                    showMsg('wifi-msg', data.message || 'Failed to reset', '#f44336');
                }
            } catch (e) {
                showMsg('wifi-msg', 'Connection failed', '#f44336');
            }
        }

        function handleFileSelect(files) {
            if (files.length === 0) return;
            const file = files[0];
            if (!file.name.endsWith('.bin')) {
                showMsg('ota-msg', 'Please upload a valid .bin file', '#f44336');
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
                        progressStatus.textContent = 'Writing to Flash...';
                    } else {
                        progressStatus.textContent = 'Uploading...';
                    }
                }
            };

            xhr.onload = function() {
                if (xhr.status === 200) {
                    try {
                        const res = JSON.parse(xhr.responseText);
                        showMsg('ota-msg', res.message || 'OTA Successful! Rebooting...', '#4CAF50');
                        progressStatus.textContent = 'Rebooting...';
                        setTimeout(() => location.reload(), 6000);
                    } catch (e) {
                        showMsg('ota-msg', 'OTA Successful! Rebooting device...', '#4CAF50');
                        progressStatus.textContent = 'Rebooting...';
                        setTimeout(() => location.reload(), 6000);
                    }
                } else {
                    showMsg('ota-msg', 'OTA Failed: ' + xhr.responseText, '#f44336');
                    resetUploadUI();
                }
            };

            xhr.onerror = function() {
                showMsg('ota-msg', 'Connection error occurred', '#f44336');
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
            
            ws.onopen = () => {
                appendTerminalText('\n*** [Airtap: Connected to serial terminal] ***\n', '#ffa000');
            };
            
            ws.onmessage = (event) => {
                appendTerminalText(event.data);
            };
            
            ws.onclose = () => {
                appendTerminalText('\n*** [Airtap: Disconnected from terminal. Retrying...] ***\n', '#f44336');
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
            if (color) {
                span.style.color = color;
            }
            term.appendChild(span);
            term.scrollTop = term.scrollHeight;
            
            // Limit terminal line count
            if (term.childNodes.length > 500) {
                term.removeChild(term.firstChild);
            }
        }

        function handleTerminalKey(e) {
            if (e.key === 'Enter') {
                sendTerminalCommand();
            }
        }

        function sendTerminalCommand() {
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

        function toggleFullscreen() {
            const container = document.getElementById('terminal-container');
            if (!document.fullscreenElement) {
                container.requestFullscreen().catch(err => {
                    console.error(`Error enabling fullscreen: ${err.message}`);
                });
            } else {
                document.exitFullscreen();
            }
        }

        // Listen for fullscreen change event to keep button state in sync
        document.addEventListener('fullscreenchange', () => {
            const fsBtn = document.getElementById('fs-btn');
            if (document.fullscreenElement) {
                fsBtn.textContent = 'Exit';
                fsBtn.style.backgroundColor = '#f44336';
            } else {
                fsBtn.textContent = 'Fullscreen';
                fsBtn.style.backgroundColor = '#4CAF50';
            }
        });

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

        // Add Drag and Drop listeners
        window.addEventListener('DOMContentLoaded', () => {
            const dropZone = document.getElementById('drop-zone');
            if (dropZone) {
                ['dragenter', 'dragover'].forEach(name => {
                    dropZone.addEventListener(name, (e) => {
                        e.preventDefault();
                        dropZone.style.background = '#eef2ff';
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
            
            // Connect terminal socket
            connectWebSocket();
        });

        // Update status every 1 second
        updateStatus();
        setInterval(updateStatus, 1000);

        // Load initial WiFi configuration
        loadWifiConfig();
    </script>
</body>
</html>
)html";

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

    // Build JSON response with explicit casts to unsigned int to avoid format errors
    char response[512] = {0};
    snprintf(response, sizeof(response),
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

static esp_err_t captive_portal_redirect_handler(httpd_req_t *req, httpd_err_code_t error)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/dashboard");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_FAIL;
}

// REST API: POST /api/ota - Wirelessly flash target partition
static esp_err_t ota_post_handler(httpd_req_t *req) {
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Passive OTA partition not found");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
             update_partition->subtype, (unsigned long)update_partition->address);
             
    int remaining = req->content_len;
    if (remaining <= 0) {
        ESP_LOGE(TAG, "Content-Length is missing or 0");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length is missing or 0");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }
    
    char *buf = (char *)malloc(1024);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        esp_ota_abort(update_handle);
        return ESP_FAIL;
    }
    
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining < 1024 ? remaining : 1024);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // Retry
            }
            ESP_LOGE(TAG, "OTA receive failed or connection closed");
            free(buf);
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive failed");
            return ESP_FAIL;
        }
        
        err = esp_ota_write(update_handle, (const void *)buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_FAIL;
        }
        
        remaining -= recv_len;
    }
    
    free(buf);
    
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_end failed");
        return ESP_FAIL;
    }
    
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OTA Update Succeeded! Rebooting in 2 seconds...");
    
    // Send JSON response
    const char *resp_str = "{\"success\":true,\"message\":\"Firmware update successful! Rebooting device...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    // Reboot in a background task
    xTaskCreate([](void *param) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }, "ota_reboot_task", 2048, NULL, 5, NULL);
    
    return ESP_OK;
}

// REST API: GET /api/openocd/config - Serves dynamic openocd.cfg file
static esp_err_t openocd_config_handler(httpd_req_t *req) {
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
        ESP_LOGI(TAG, "WebSocket handshake done, connection opened");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Receive frame size
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get length: 0x%x", ret);
        return ret;
    }
    
    if (ws_pkt.len > 0) {
        buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for WS payload");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: 0x%x", ret);
            free(buf);
            return ret;
        }
        
        // Write the characters directly to the target serial bridge UART
        uart_write_bytes((uart_port_t)CONFIG_ESP_UART_BRIDGE_UART_NUM, (const char *)ws_pkt.payload, ws_pkt.len);
        free(buf);
    }
    return ESP_OK;
}

void web_dashboard_broadcast_ws(const char *data, size_t len) {
    if (!dashboard_state.initialized || dashboard_state.server == NULL) {
        return;
    }
    
    // Get list of active client file descriptors
    size_t clients_num = 12;
    int client_fds[12] = {0};
    esp_err_t err = httpd_get_client_list(dashboard_state.server, &clients_num, client_fds);
    if (err != ESP_OK) {
        return;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)data;
    ws_pkt.len = len;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    for (size_t i = 0; i < clients_num; i++) {
        // Send WS data to all descriptors (non-WS client descs will gracefully fail without harm)
        httpd_ws_send_data(dashboard_state.server, client_fds[i], &ws_pkt);
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
    config.max_uri_handlers = 12;

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
        .user_ctx = NULL,
        .is_websocket = false
    };
    httpd_register_uri_handler(dashboard_state.server, &root);

    httpd_uri_t dashboard = {
        .uri = "/dashboard",
        .method = HTTP_GET,
        .handler = dashboard_handler,
        .user_ctx = NULL,
        .is_websocket = false
    };
    httpd_register_uri_handler(dashboard_state.server, &dashboard);

    httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
        .is_websocket = false
    };
    httpd_register_uri_handler(dashboard_state.server, &status);

    httpd_uri_t ota = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL,
        .is_websocket = false
    };
    httpd_register_uri_handler(dashboard_state.server, &ota);

    httpd_uri_t oocd_cfg = {
        .uri = "/api/openocd/config",
        .method = HTTP_GET,
        .handler = openocd_config_handler,
        .user_ctx = NULL,
        .is_websocket = false
    };
    httpd_register_uri_handler(dashboard_state.server, &oocd_cfg);

    httpd_uri_t launch_json = {
        .uri = "/api/vscode/launch",
        .method = HTTP_GET,
        .handler = vscode_launch_handler,
        .user_ctx = NULL,
        .is_websocket = false
    };
    httpd_register_uri_handler(dashboard_state.server, &launch_json);

    // Register WebSocket route URI
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(dashboard_state.server, &ws);

    // Register WiFi Configuration REST API endpoints
    wifi_config_api_register(dashboard_state.server);

    // Register 404 handler for Captive Portal redirects in AP mode
    httpd_register_err_handler(dashboard_state.server, HTTPD_404_NOT_FOUND, captive_portal_redirect_handler);

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

    // Build JSON response with explicit casts to unsigned int to avoid format errors
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
