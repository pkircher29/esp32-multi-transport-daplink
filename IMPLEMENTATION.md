# Multi-Transport Implementation Guide

## Architecture Overview

This document describes the technical implementation of the multi-transport CMSIS-DAP debugger for ESP32.

## Core Principles

### 1. Transport Abstraction

The core CMSIS-DAP command processor (`DAP_ProcessCommand()`) is **transport-agnostic**. Each transport layer:

1. Receives raw data from the physical interface
2. Accumulates data into a buffer
3. Detects complete packets (via 8-byte header with packet length)
4. Calls `DAP_ProcessCommand()` with the extracted DAP command
5. Wraps the response in the same packet format
6. Sends the response back via the same physical interface

### 2. Unified Packet Format

All transports use identical packet framing:

```c
typedef struct {
    uint32_t signature;           // 0x00504144 (ASCII "DAP\0")
    uint16_t length;              // Length of payload (excluding 8-byte header)
    uint8_t  packet_type;         // Reserved for future use (0x00 for current protocol)
    uint8_t  reserved;            // Must be 0x00
    uint8_t  data[MAX_DAP_CMD];   // Variable-length DAP command or response
} dap_packet_header_t;
```

**Total packet size**: 8 bytes header + N bytes payload

### 3. Modular Task Architecture

Each transport runs as a separate FreeRTOS task:

- **WiFi/TCP**: `cmsis_dap_tcp_task()` - Continuously reads from listening socket
- **USB CDC**: `cmsis_dap_usb_task()` - Processes USB CDC rx callbacks
- **Bluetooth SPP**: `cmsis_dap_bt_task()` - Dequeues SPP data from message queue

Tasks run independently and simultaneously, allowing concurrent debugging over multiple transports.

## Transport Implementation Details

### WiFi/TCP Transport (`cmsis_dap_tcp.c`)

**Purpose**: Establish a TCP server listening on port 4441 for incoming OpenOCD connections.

**Key Components**:

1. **Initialization** (`cmsis_dap_tcp_init()`)
   - Wait for WiFi connection (event-driven)
   - Log IP address
   - Create TCP server socket
   - Bind to port 4441
   - Start listening
   - Create FreeRTOS task

2. **Main Loop** (`cmsis_dap_tcp_task()`)
   ```
   While True:
       if (socket not connected):
           client_socket = accept() from server_socket
           connected = true
       
       try:
           rx_bytes = recv(client_socket, buffer, MAX_RX)
           for each byte:
               accumulated_data[accumulated_len++] = byte
               
               # Check if we have a complete packet
               if (accumulated_len >= 8):
                   header = (dap_packet_header_t *)accumulated_data
                   total_pkt_len = 8 + header.length
                   
                   if (accumulated_len >= total_pkt_len):
                       # Process complete packet
                       response_len = process_packet(accumulated_data, response_data)
                       send(client_socket, response_data, response_len)
                       
                       # Shift buffer
                       memmove(accumulated_data, accumulated_data + total_pkt_len, ...)
                       accumulated_len -= total_pkt_len
       except socket_closed:
           connected = false
           close(client_socket)
   ```

3. **Packet Processing** (`cmsis_dap_tcp_process_packet()`)
   - Verify packet signature (0x00504144)
   - Extract length from header
   - Call `DAP_ProcessCommand(data + 8, length, response, &response_len)`
   - Wrap response with 8-byte header
   - Return total bytes to send

4. **Keep-Alive**
   - Configurable keep-alive timeout (default: 10 seconds)
   - If no data received for timeout period, close connection
   - Prevents zombied connections

**Memory Usage**: ~50 KB (socket buffer + task stack)

### USB CDC Transport (`cmsis_dap_usb.c`)

**Purpose**: Implement USB Communications Device Class (CDC) for plug-and-play debugger support.

**Dependencies**: 
- TinyUSB component (optional, disabled by default)
- FreeRTOS for task scheduling

**Key Components**:

1. **Initialization** (`cmsis_dap_usb_init()`)
   - Verify TinyUSB is available
   - Configure USB device (VID: 0x303A, PID: variable)
   - Register CDC ACM interface
   - Set device name, serial number
   - Start USB stack
   - Create FreeRTOS task

2. **USB Callbacks** (Called from USB interrupt context)
   ```c
   usb_cdc_rx_callback():
       # Called when USB host sends data
       rx_len = tinyusb_cdc_read(buffer, RX_BUFFER_SIZE)
       # Queue data to task via FreeRTOS queue
       xQueueSend(usb_rx_queue, buffer, rx_len)
   
   usb_cdc_tx_callback():
       # Called when USB host ready to receive more data
       # Signals task that we can send more
       xEventGroupSetBits(usb_events, TX_READY_BIT)
   
   usb_cdc_line_state_changed():
       # Detect DTR/RTS changes for flow control
       # Not used in current implementation
   ```

3. **Main Task** (`cmsis_dap_usb_task()`)
   ```
   While True:
       # Wait for data from queue (populated by rx callback)
       xQueueReceive(usb_rx_queue, rx_data, portMAX_DELAY)
       accumulated_data[accumulated_len++] = rx_byte
       
       # Check for complete packet (same as TCP)
       if (complete_packet_detected):
           response_len = usb_process_packet(accumulated_data, response_data)
           
           # Wait for USB ready to send
           xEventGroupWaitBits(usb_events, TX_READY_BIT, pdTRUE, ...)
           
           # Send response
           tinyusb_cdc_write(response_data, response_len)
           
           # Shift buffer
           memmove(...)
   ```

4. **Ring Buffer**
   - Circular buffer for accumulated RX data (4096 bytes)
   - Prevents overflow if host sends multiple packets rapidly
   - Handles fragmented USB transfers transparently

**Memory Usage**: ~100 KB (USB stack + DMA buffers + task stack)

### Bluetooth SPP Transport (`cmsis_dap_bt.c`)

**Purpose**: Implement Bluetooth Classic Serial Port Profile (SPP) for wireless debugger support.

**Dependencies**:
- Bluedroid component (part of standard ESP-IDF)
- FreeRTOS for task scheduling

**Key Components**:

1. **Initialization** (`cmsis_dap_bt_init()`)
   - Initialize Bluetooth controller
   - Initialize Bluedroid stack
   - Register GAP (Generic Access Profile) callbacks
   - Register SPP callbacks
   - Set Bluetooth device name
   - Set GAP security mode
   - Create SPP server with UUID
   - Enable discovery/pairing mode
   - Create FreeRTOS task

2. **SPP Server Lifecycle**
   ```
   Device Boot:
       SPP Server Created and Listening
       Device in Discoverable/Pairable Mode
       
   Phone/Computer Scan:
       Sees "ESP32-CMSIS-DAP" in Bluetooth devices
       
   Pairing Request:
       PIN Code Verification (default: "0000")
       -> User confirms on host
       -> Pairing established
       
   Connection Request:
       SPP_OPEN event fired
       Client connected via SPP
       
   Data Transfer:
       Client sends: SPP data packets
       -> SPP_DATA_IND event
       -> Data queued for processing
       
   Disconnection:
       SPP_CLOSE event fired
       Ready for next connection
   ```

3. **SPP Event Handlers**
   ```c
   spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param):
       switch (event):
           case ESP_SPP_INIT_EVT:
               # SPP initialized, ready to go
               
           case ESP_SPP_OPEN_EVT:
               # Client connected
               handle = param.open.handle
               send_response = true
               
           case ESP_SPP_CLOSE_EVT:
               # Client disconnected
               send_response = false
               
           case ESP_SPP_DATA_IND_EVT:
               # Data received from client
               len = param.data_ind.len
               data = param.data_ind.data
               # Queue for task processing
               xQueueSend(bt_rx_queue, ...)
               
           case ESP_SPP_CONG_EVT:
               # Congestion (buffer full on sending)
               # Back-pressure: slow down packet generation
               
           case ESP_SPP_WRITE_EVT:
               # Previous write completed
               # OK to send next packet
               xEventGroupSetBits(bt_events, TX_DONE_BIT)
   ```

4. **Main Task** (`cmsis_dap_bt_task()`)
   ```
   While True:
       # Wait for data from queue
       xQueueReceive(bt_rx_queue, rx_data, portMAX_DELAY)
       accumulated_data[accumulated_len++] = rx_byte
       
       # Check for complete packet
       if (complete_packet_detected):
           response_len = bt_process_packet(accumulated_data, response_data)
           
           # Wait for previous write to complete
           xEventGroupWaitBits(bt_events, TX_DONE_BIT, pdFALSE, ...)
           
           # Send response via SPP
           esp_spp_write(handle, response_len, response_data)
           
           # Shift buffer
           memmove(...)
   ```

5. **GAP Security**
   ```c
   gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param):
       switch (event):
           case ESP_BT_GAP_PIN_REQ_EVT:
               # Host requesting PIN code
               esp_bt_gap_pin_reply(device, true, PIN_LEN, PIN_CODE)
               
           case ESP_BT_GAP_CFM_REQ_EVT:
               # User confirmation (for PIN)
               esp_bt_gap_pin_reply(device, true, ...)
               
           case ESP_BT_GAP_AUTH_CMPL_EVT:
               # Authentication complete
               # Connection now secure
   ```

**Memory Usage**: ~200 KB (Bluedroid stack + SPP + task stack)

## Packet Processing Flow

### Reception Phase

```
Physical Interface
    |
    +-> Byte-Level Reception
        |
        +-> Buffer Accumulation
            |
            +-> Packet Detection
                |
                +-> Complete Packet?
                    |
                    +---> YES: Extract DAP Command
                    |         |
                    |         +-> DAP_ProcessCommand()
                    |             |
                    |             +-> Generate Response
                    |                 |
                    |                 +-> Wrap in Header
                    |                     |
                    |                     +-> Queue for Transmission
                    |
                    +---> NO: Wait for More Bytes
```

### Transmission Phase

```
Response Packet (8-byte header + N-byte DAP response)
    |
    +-> Queue to Physical Interface
        |
        +-> Await Flow Control (if needed)
            |
            +-> Transmit via Interface
                |
                +-> Confirm Delivery
```

## Concurrent Operation

When all three transports are enabled:

```
FreeRTOS Scheduler (Quantum = ~10ms)
│
├─ Task 1: WiFi/TCP Handler
│   • Accept connections on port 4441
│   • Read socket data
│   • Process DAP packets
│   • Write responses to socket
│
├─ Task 2: USB CDC Handler
│   • Monitor USB CDC events
│   • Queue RX data
│   • Send TX responses
│   • Handle host suspend/resume
│
├─ Task 3: Bluetooth SPP Handler
│   • Monitor SPP server
│   • Accept connections
│   • Queue RX data
│   • Send TX responses
│
├─ Task 4: WiFi Event Task (system)
│   • Handle WiFi connection/disconnection
│   • DHCP negotiation
│
└─ Task N: Other OS Tasks
```

All tasks run concurrently on the dual-core ESP32:
- Core 0: System tasks, WiFi driver
- Core 1: Application tasks (TCP, USB, Bluetooth)

## GPIO and Hardware Interface

### SWD Interface
```c
swd_write_bit(int bit):
    if (bit):
        gpio_set_level(SWDIO_PIN, 1)
    else:
        gpio_set_level(SWDIO_PIN, 0)
    
    gpio_set_level(SWCLK_PIN, 0)   // Clock low
    esp_rom_delay_us(HALF_CLOCK_PERIOD)
    gpio_set_level(SWCLK_PIN, 1)   // Clock high
    esp_rom_delay_us(HALF_CLOCK_PERIOD)
    gpio_set_level(SWCLK_PIN, 0)   // Clock low

swd_read_bit():
    gpio_set_level(SWDIO_PIN, 1)   // Release (pull-up)
    gpio_set_level(SWCLK_PIN, 0)   // Clock low
    esp_rom_delay_us(HALF_CLOCK_PERIOD)
    bit = gpio_get_level(SWDIO_PIN)
    gpio_set_level(SWCLK_PIN, 1)   // Clock high
    esp_rom_delay_us(HALF_CLOCK_PERIOD)
    return bit
```

### JTAG Interface
```c
jtag_write_bits(uint8_t *data, int count):
    for each bit in count:
        gpio_set_level(TMS_PIN, ...)   // Set TMS
        gpio_set_level(TDI_PIN, ...)   // Set TDI
        gpio_set_level(TCK_PIN, 0)     // Clock low
        esp_rom_delay_us(HALF_CLOCK_PERIOD)
        
        TDO_value = gpio_get_level(TDO_PIN)
        
        gpio_set_level(TCK_PIN, 1)     // Clock high
        esp_rom_delay_us(HALF_CLOCK_PERIOD)
```

## Build System Integration

### CMakeLists.txt
```cmake
# Conditional compilation based on CONFIG flags
if(CONFIG_ESP_USB_CDC_ENABLED)
    list(APPEND COMPONENT_SRCS "cmsis_dap_usb.c")
endif()

if(CONFIG_ESP_CMSIS_DAP_BT_ENABLED)
    list(APPEND COMPONENT_SRCS "cmsis_dap_bt.c")
endif()

# WiFi/TCP always included (core functionality)
list(APPEND COMPONENT_SRCS "cmsis_dap_tcp.c")
```

### Kconfig.projbuild
```
menu "CMSIS-DAP Configuration"
    
    menu "WiFi Configuration"
        config ESP_WIFI_SSID
        config ESP_WIFI_PASSWORD
        ...
    endmenu
    
    menu "USB CDC Configuration"
        config ESP_USB_CDC_ENABLED
        config ESP_USB_DEVICE_PRODUCT
        ...
    endmenu
    
    menu "Bluetooth SPP Configuration"
        config ESP_CMSIS_DAP_BT_ENABLED
        config ESP_BT_DEVICE_NAME
        ...
    endmenu
endmenu
```

### main.c Integration
```c
#include "cmsis_dap_tcp.h"
#include "cmsis_dap_usb.h"
#include "cmsis_dap_bt.h"

void app_main(void) {
    // Initialize core systems
    nvs_flash_init();
    esp_event_loop_create_default();
    
    // Initialize WiFi
    esp_netif_create_default_wifi_sta();
    wifi_init_sta();  // Blocks until connected
    
    // Start TCP server
    cmsis_dap_tcp_init();
    
    // Start optional transports
    #ifdef CONFIG_ESP_USB_CDC_ENABLED
        cmsis_dap_usb_init();
        xTaskCreate(cmsis_dap_usb_task, ...);
    #endif
    
    #ifdef CONFIG_ESP_CMSIS_DAP_BT_ENABLED
        cmsis_dap_bt_init();
        xTaskCreate(cmsis_dap_bt_task, ...);
    #endif
    
    // Main WiFi/TCP task
    xTaskCreate(cmsis_dap_tcp_task, ...);
}
```

## Performance Optimization

### 1. Clock Speed Calibration
```c
// Measure actual GPIO toggle time
start = esp_timer_get_time();
for (i = 0; i < N; i++) {
    gpio_set_level(pin, 0);
    gpio_set_level(pin, 1);
}
elapsed = esp_timer_get_time() - start;
actual_freq = (N * 2) / (elapsed * 1e-6);  // Hz
```

### 2. Packet Buffer Optimization
- Use DMA where available
- Align buffers to cache line boundaries (32 bytes)
- Pre-allocate response buffers to avoid malloc overhead

### 3. Task Priority Tuning
```c
// Higher priority for time-sensitive operations
xTaskCreate(cmsis_dap_tcp_task,    "tcp",    4096, NULL, 5, NULL);   // Standard
xTaskCreate(cmsis_dap_usb_task,    "usb",    4096, NULL, 6, NULL);   // Higher
xTaskCreate(cmsis_dap_bt_task,     "bt",     4096, NULL, 5, NULL);   // Standard
```

## Future Extensions

### Adding a New Transport (e.g., SPI)

1. **Create `cmsis_dap_spi.c`**:
   ```c
   void cmsis_dap_spi_init(void) { /* init code */ }
   void cmsis_dap_spi_task(void *pvParams) { /* main loop */ }
   int cmsis_dap_spi_process_packet(...) { /* packet processing */ }
   ```

2. **Update `CMakeLists.txt`**:
   ```cmake
   if(CONFIG_ESP_CMSIS_DAP_SPI_ENABLED)
       list(APPEND COMPONENT_SRCS "cmsis_dap_spi.c")
   endif()
   ```

3. **Update `Kconfig.projbuild`**:
   ```
   menu "SPI Configuration"
       config ESP_CMSIS_DAP_SPI_ENABLED
           bool "Enable SPI Transport"
   endmenu
   ```

4. **Update `main.c`**:
   ```c
   #ifdef CONFIG_ESP_CMSIS_DAP_SPI_ENABLED
       xTaskCreate(cmsis_dap_spi_task, ...);
   #endif
   ```

---

**Last Updated**: May 30, 2026
