# Multi-Transport DAPLink Implementation Plan

## Project Goal
Convert the cmsis_dap_tcp_esp32 into a fully-featured ESP32 DAPLink supporting **USB**, **WiFi/TCP**, and **Bluetooth** simultaneously.

## Current Architecture
- **TCP/IPv4/IPv6** transport via lwip (cmsis_dap_tcp.c)
- **DAP Protocol Layer** (DAP.c) - transport-agnostic command processor
- **Hardware Drivers** (SW_DP.c, JTAG_DP.c) - GPIO-level SWD/JTAG implementation
- Clear separation allows new transports to be added without modifying DAP.c

## Implementation Strategy

### Phase 1: USB Support (Priority 1)
**Why First**: USB is the most common debugger interface and doesn't require WiFi setup.

**Implementation**:
1. Add TinyUSB component dependency (esp_tinyusb)
2. Create `main/cmsis_dap_usb.c` - USB CDC endpoint handler
3. Implement USB packet framing (same protocol as TCP)
4. USB task calls `DAP_ProcessCommand()` like TCP task does
5. Add USB menuconfig options (enable/disable, product name, etc.)

**Components**:
- `cmsis_dap_usb.c` (~300 lines) - USB device initialization + CDC rx/tx loops
- Update `CMakeLists.txt` - add esp_tinyusb dependency
- Update `Kconfig.projbuild` - USB configuration options
- `DAP_config.h` - no changes needed (GPIO pins same)

**Benefits**:
- Zero setup required (plug-and-play)
- High-speed transport (12 Mbps Full-Speed)
- Power delivery possible
- Works on all platforms (Linux, Windows, macOS)

---

### Phase 2: Bluetooth Support (Priority 2)
**Why Second**: Adds wireless debugging without WiFi network requirement.

**Two Options**:
- **Bluetooth Classic (SPP)**: Better range, compatible with older devices
- **Bluetooth LE**: Lower power, modern, smaller payload (need fragmentation)

**Recommendation**: Implement **Bluetooth Classic (SPP)** first (simpler protocol):

**Implementation**:
1. Add `esp_bt` and `esp_bluedroid` components
2. Create `main/cmsis_dap_bt.c` - Bluetooth SPP server
3. Same DAP packet framing over SPP
4. Handle multiple SPP connections (or priority model)
5. Add Bluetooth menuconfig (enable/disable, device name, PIN)

**Components**:
- `cmsis_dap_bt.c` (~350 lines) - SPP server + GAP/SDP setup
- Update `CMakeLists.txt` - add esp_bt dependency
- Update `Kconfig.projbuild` - Bluetooth settings (device name, SPP channel)
- `main.c` - BT initialization task

**Benefits**:
- No WiFi setup needed
- Wireless debugging within ~10m
- Compatible with any Bluetooth adapter
- Optional battery-powered portable debugger

---

### Phase 3: Multi-Transport Orchestration (Priority 3)
**Goal**: Handle simultaneous connections across all three transports.

**Design**:
1. Create transport abstraction layer (`transport.h`)
2. Each transport (USB, WiFi, BT) registers handlers
3. Command dispatch routed through unified interface
4. Configuration bits for enabling each transport
5. Optional transport switching/bridging

**Components**:
- `main/transport.h` - Common interface for all transports
- Update `main.c` - Initialize all enabled transports
- Menuconfig - Enable/disable each transport independently

**Conflict Resolution**:
- By default, all transports can be active simultaneously
- OpenOCD connects via any available interface
- Consider mutual exclusion if needed (one client at a time)

---

## File Changes Summary

### New Files
```
main/cmsis_dap_usb.c          (~300 lines) - USB CDC transport
main/cmsis_dap_bt.c           (~350 lines) - Bluetooth SPP transport
main/transport.h              (~80 lines)  - Common transport interface (optional)
```

### Modified Files
```
main/CMakeLists.txt           - Add esp_tinyusb, esp_bt dependencies
main/Kconfig.projbuild        - USB/Bluetooth configuration options
main/main.c                   - Initialize USB/BT tasks
main/cmsis_dap_tcp.c          - Minor refactor (optional, extract common code)
```

### No Changes Required
```
main/DAP.c                    - Already transport-agnostic ✓
main/DAP.h                    - No changes needed ✓
main/SW_DP.c, JTAG_DP.c      - GPIO layer unchanged ✓
CMakeLists.txt (root)         - No changes ✓
```

---

## Implementation Phases

### Phase 1: USB (Week 1)
- [ ] Add TinyUSB to CMakeLists.txt
- [ ] Implement cmsis_dap_usb.c with USB CDC
- [ ] Add USB menuconfig options
- [ ] Update main.c to initialize USB task
- [ ] Test USB enumeration on Windows/Linux
- [ ] Test DAP commands via USB

### Phase 2: Bluetooth Classic (Week 2)
- [ ] Add Bluetooth components to CMakeLists.txt
- [ ] Implement cmsis_dap_bt.c with SPP
- [ ] Add Bluetooth menuconfig options
- [ ] Update main.c to initialize BT task
- [ ] Test BLE device pairing
- [ ] Test DAP commands via Bluetooth

### Phase 3: Multi-Transport Integration (Week 3)
- [ ] Create transport abstraction layer
- [ ] Refactor common packet handling code
- [ ] Test simultaneous USB + WiFi connections
- [ ] Test simultaneous WiFi + Bluetooth connections
- [ ] Test all three transports active
- [ ] Performance testing and optimization

---

## Technical Details

### USB CDC Packet Format
Uses same framing as TCP:
```
Byte 0-3:   "DAP\0" (signature)
Byte 4-5:   Packet length (little-endian)
Byte 6:     Packet type
Byte 7:     Reserved
Byte 8+:    DAP command/response data
```

### Bluetooth SPP Packet Format
Same framing; SPP provides reliable stream just like TCP.

### Memory Considerations
- ESP32-S3-N16R8: 16 MB flash, 8 MB PSRAM available
- Estimated additional footprint:
  - USB stack: ~50 KB
  - Bluetooth stack: ~150 KB
  - Application code: ~20 KB
  - **Total headroom**: Plenty (well under 1 MB)

### Performance Targets
- **USB**: ~100+ KB/s throughput
- **Bluetooth Classic**: ~80+ KB/s (SPP MTU ~990 bytes)
- **WiFi/TCP**: ~200 KB/s (same as current)

---

## Testing Strategy

1. **Unit Tests**
   - Each transport initializes correctly
   - Packet framing/deframing works
   - DAP command dispatch succeeds

2. **Integration Tests**
   - OpenOCD connects via USB → program target
   - OpenOCD connects via Bluetooth → program target
   - OpenOCD connects via WiFi → program target

3. **Stress Tests**
   - Multiple transports simultaneously
   - Large firmware images (512 KB+)
   - Connection drop/reconnect handling

4. **Real-world Tests**
   - Program STM32 target via each transport
   - Verify flashing speed and correctness
   - Test with OpenOCD's -f interface/cmsis-dap.cfg

---

## Menuconfig Structure
```
CMSIS-DAP Configuration
├─ Debug Output
├─ CPU Usage Reporting
├─ WiFi Configuration
│  └─ TCP Server (existing)
├─ USB Configuration (NEW)
│  ├─ Enable USB CDC
│  ├─ USB Product ID
│  └─ USB Serial Number
├─ Bluetooth Configuration (NEW)
│  ├─ Enable Bluetooth SPP
│  ├─ Bluetooth Device Name
│  ├─ Bluetooth PIN Code
│  └─ Bluetooth TX Power
├─ GPIO Number Assignments
└─ UART to TCP/IP Bridge
```

---

## Success Criteria
✓ All three transports work simultaneously  
✓ USB plug-and-play debugging (no configuration)  
✓ Bluetooth wireless debugging (with pairing)  
✓ WiFi/TCP support maintained  
✓ OpenOCD compatible via all transports  
✓ No regression in existing functionality  
✓ Binary size < 1.5 MB (fits in partition)  

---

## Next Steps
1. Start with Phase 1 (USB) - quickest win
2. Verify USB functionality with real hardware
3. Proceed to Bluetooth implementation
4. Optimize and integrate
