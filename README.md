# Airtap (cmsis_dap_tcp) - Wireless Multi-Transport Debugger 🔌🚀

> **AIRTAP**: **A**dvanced **I**nteractive **R**emote **T**est **A**ccess **P**ort.
> 
> A premium, high-performance, wireless JTAG/SWD **CMSIS-DAP debugger and programmer** featuring simultaneous multi-transport support, dynamic STM32 setup assistance, OTA wireless updates, and a live web console completely over the air!

---

## 🌟 Upgraded Airtap Features

* **📡 Local mDNS Addressing (`http://airtap.local`)**
  * Fully resolves locally at `http://airtap.local` once connected to your network. No more looking up DHCP address tables in router consoles!
* **🚀 STM32 Out-of-Box (OOB) Assistant**
  * **🔌 Visual SWD wiring maps** embedded straight inside the dashboard.
  * **📥 One-Click `openocd.cfg`** dynamic download configured with the active network hostname.
  * **📥 One-Click `.vscode/launch.json`** dynamic downloader to instantly setup wireless Cortex-Debug sessions.
* **☁️ Wireless Over-The-Air (OTA) Updates**
  * Built with a custom 2MB dual-OTA partition scheme (`partitions.csv`) splitting flash into two secure application banks.
  * Update your firmware wirelessly by dragging and dropping new `.bin` binaries directly onto the dashboard's **OTA Upload Card**!
* **🖥️ Retro Web Monospace Serial Terminal**
  * Stream live bidirectional debug prints and send command keys straight from your browser.
  * Bridged dynamically using modern WebSockets (`ws://airtap.local/ws`) directly to the target microcontroller's UART1 lines.
* **🌐 Automatic Captive Portal Redirection**
  * When in SoftAP configuration mode, connecting to the **`ESP32-DAPLink-[MAC]`** hotspot instantly launches a captive portal redirection loading the setup dashboard at `http://192.168.4.1` automatically!

---

## 📐 Supported Transports

Airtap allows simultaneous operations across all configured interfaces:
1. **WiFi / TCP** (Active by default) - Remote wireless CMSIS-DAP debugging over port `4441`.
2. **USB CDC** (Optional) - Standard plug-and-play local CMSIS-DAP debugger when docked via USB.
3. **Bluetooth SPP** (Optional) - Wireless serial/JTAG bridge when out of WiFi range.
4. **TCP Serial Bridge** - Standalone TCP socket server on port `4442` mapping target UART1 logs.

---

## 🔌 SWD Pin Mapping

Connect your target device (e.g. STM32 Blue Pill, Nucleo, or similar Cortex-M boards) to your Airtap ESP32-S3 according to this SWD scheme:

| Airtap Pin | direction | Target Pin | description |
| :--- | :---: | :--- | :--- |
| **GPIO 13** | ➡️ | **SWDIO / TMS** | SWD Data Input/Output |
| **GPIO 14** | ➡️ | **SWCLK / TCK** | SWD Clock |
| **GPIO 12** | ➡️ | **nRESET (SRST)** | Target Reset (Optional) |
| **GND** | ↔️ | **GND** | Common Ground Reference |

---

## ⚡ Quick Start

### 1. Select and Copy Board Config
```bash
# For ESP32-S3-DevKitC-1
cp sdkconfig.esp32s3_devkitc_1 sdkconfig

# For XIAO ESP32-C6
cp sdkconfig.xiao_esp32c6 sdkconfig
```

### 2. Build and Flash the Firmware
To build, compile, and write the initial custom dual-OTA partitions to your board over `COM4` (Windows):
```powershell
. C:\Espressif\idf5.5.ps1
idf.py build
idf.py -p COM4 flash
```
*Note: During the first flash, `esptool.py` will automatically repartition the 2MB flash into two symmetric 960K app slots.*

### 3. Connect & Configure
1. Connect your PC or phone to the **`ESP32-DAPLink-[MAC]`** open Wi-Fi network.
2. The browser will automatically open your **Airtap Dashboard**. (If not, navigate to `http://airtap.local` or `http://192.168.4.1/dashboard`).
3. Set your home/office Wi-Fi network SSID and Password, then click **Save** and **Apply**.
4. Once connected to your network, navigate to **`http://airtap.local`** to view your active stats and interact with the terminal!

---

## 🛠️ Out-of-Box STM32 Debugging Setup

Airtap makes setting up your wireless debug workspace incredibly painless:

### Option A: Raw OpenOCD
1. Open your browser and go to `http://airtap.local`.
2. Click **Download openocd.cfg** and save the file in your firmware project directory.
3. Run OpenOCD:
   ```bash
   openocd -f openocd.cfg -f target/stm32f4x.cfg
   ```

### Option B: VS Code Cortex-Debug
1. Open your browser and go to `http://airtap.local`.
2. Click **Download openocd.cfg** and drop it in your project root.
3. Click **Download launch.json** and place it under your project's `.vscode/` folder.
4. Open VS Code, select **Wireless STM32 Debug (airtap)** from the debug pane, and hit **F5** to start debugging completely wirelessly!

---

## 📂 Project Structure

* [main/main.c](main/main.c) - Firmware core boot sequence, captive portal DNS engine, SoftAP controller, and mDNS initialization.
* [main/web_dashboard.cpp](main/web_dashboard.cpp) - Responsive Web Dashboard HTML, glassmorphic UI, REST configuration endpoints, dynamic file generators, Web OTA, and WebSocket server router.
* [main/uart_bridge.c](main/uart_bridge.c) - Background select loop bridging serial VFS UART bytes to TCP port `4442` and active browser WebSockets.
* [partitions.csv](partitions.csv) - Custom dual-OTA partition layouts designed specifically for 2MB flash targets.
