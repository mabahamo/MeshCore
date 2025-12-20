# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MeshCore is a lightweight, portable C++ library for multi-hop packet routing using LoRa and other packet radios. It's designed for embedded projects targeting ESP32, NRF52, RP2040, and STM32 platforms. The project is built using PlatformIO and follows embedded development best practices with no dynamic memory allocation (except during initialization).

## Build System

MeshCore uses **PlatformIO** as its build system. The main configuration is in `platformio.ini` with variant-specific configs in `variants/*/platformio.ini`.

### Common Build Commands

```bash
# Build a specific environment/variant
pio run -e <environment_name>

# Examples:
pio run -e RAK_4631_repeater
pio run -e Heltec_v3_companion_radio_ble
pio run -e t1000e_companion_radio_ble

# Upload to device
pio run -e <environment_name> -t upload

# Build using the build script (creates firmwares in out/ directory)
# Requires FIRMWARE_VERSION environment variable
export FIRMWARE_VERSION=v1.0.0
sh build.sh build-firmware <target>
sh build.sh build-companion-firmwares
sh build.sh build-repeater-firmwares
sh build.sh build-room-server-firmwares

# List all available environments
pio project config | grep 'env:'

# Makefile shortcut (example for t1000e)
make t100e
```

### Finding Environments

Environments are defined across:
- Main `platformio.ini` (base configurations)
- `variants/*/platformio.ini` (device-specific builds)

Common patterns:
- `*_companion_radio_ble` - BLE companion radios
- `*_companion_radio_usb` - USB companion radios
- `*_repeater` - Repeater firmware
- `*_room_server` - Room server firmware

## Code Architecture

### Core Layer (src/)

The codebase follows a layered architecture from low-level packet handling to high-level mesh operations:

1. **Packet** (`Packet.h/cpp`) - Low-level packet structure and serialization
2. **Dispatcher** (`Dispatcher.h/cpp`) - Radio abstraction, packet queuing, and transmission scheduling
3. **Mesh** (`Mesh.h/cpp`) - Routing logic, packet forwarding, encryption handling
4. **BaseChatMesh** (`helpers/BaseChatMesh.h/cpp`) - Chat-specific mesh features: contacts, channels, messaging

Each layer extends the previous one, adding functionality while maintaining separation of concerns.

### Key Components

- **Identity** (`Identity.h/cpp`) - Node identity and cryptographic key management
- **Utils** (`Utils.h/cpp`) - Utility functions for the mesh network
- **MeshTables** - Abstract interface for routing tables and duplicate detection (implement in application)
- **PacketManager** - Memory management for packet allocation (use `StaticPoolPacketManager` in helpers/)

### Helper Modules (src/helpers/)

Platform-agnostic helper code:
- **radiolib/** - RadioLib radio driver wrappers
- **bridges/** - Bridge implementations for different transport types
- **ui/** - UI components for displays
- **esp32/**, **nrf52/**, **stm32/** - Platform-specific code
- **sensors/** - Sensor integration code
- **CommonCLI** - Command-line interface for repeater/room server configuration

### Example Applications (examples/)

The examples demonstrate how to build complete applications:
- **companion_radio/** - For connecting to apps via BLE/USB/WiFi
- **simple_repeater/** - Network extender
- **simple_room_server/** - BBS-style message server
- **simple_secure_chat/** - Terminal-based secure chat
- **simple_sensor/** - Sensor node implementation

Each example has:
- `main.cpp` - Application entry point
- `MyMesh.h/cpp` - Application-specific Mesh subclass
- Platform-specific configuration

### Hardware Variants (variants/)

Each hardware variant contains:
- `platformio.ini` - Build configuration for specific board
- Board-specific pin definitions and settings
- LoRa radio configuration

## Development Guidelines

### Code Style

- Use the `.clang-format` file for formatting
- Follow existing brace and indenting style in core modules
- **Do NOT retroactively reformat existing code** - creates unnecessary diffs
- Think embedded: keep code concise, avoid unnecessary abstraction layers
- Prefer simple, direct implementations over complex patterns

### Memory Management

- **No dynamic memory allocation** except in `setup()`/`begin()` functions
- Use static buffers and fixed-size arrays
- Pre-allocate all resources during initialization

### Platform Abstractions

Different platforms use different filesystems:
- **ESP32**: SPIFFS
- **NRF52/STM32**: InternalFS (+ optional QSPI flash or ExtraFS)
- **RP2040**: LittleFS

Platform selection is via preprocessor defines: `ESP32`, `NRF52_PLATFORM`, `RP2040_PLATFORM`, `STM32_PLATFORM`

### Build Flags

Key build flags in `platformio.ini`:
- `LORA_FREQ`, `LORA_BW`, `LORA_SF` - LoRa radio parameters
- `ENABLE_PRIVATE_KEY_IMPORT/EXPORT` - Security features (comment out for production)
- `RADIOLIB_EXCLUDE_*` - Exclude unused radio modules to reduce binary size
- `DISPLAY_CLASS` - Enable display support
- `BLE_PIN_CODE`, `WIFI_SSID` - Interface configuration

## Testing and Flashing

### Web Flasher

Users typically flash firmware using https://flasher.meshcore.co.uk

### Manual Flashing

For ESP32 devices:
```bash
# Non-merged binary (preserves BLE pairing)
esptool.py -p /dev/ttyUSB0 --chip esp32-s3 write_flash 0x10000 firmware.bin

# Merged binary (fresh install)
esptool.py -p /dev/ttyUSB0 --chip esp32-s3 write_flash 0x00000 firmware-merged.bin
```

For NRF52 devices:
```bash
# Use adafruit-nrfutil with .zip firmware
adafruit-nrfutil --verbose dfu serial --package firmware.zip -p /dev/ttyACM0 -b 115200 --singlebank --touch 1200
```

## Project Structure Notes

- `/arch` - Platform-specific architecture code and libraries
- `/boards` - Board definition files for PlatformIO
- `/bin` - Build utilities (e.g., uf2conv)
- `/lib` - External libraries bundled with the project
- `/docs` - Documentation including FAQ

## Contributing

- Submit PRs using **'dev'** as the base branch (not 'main')
- Open an issue first for impactful changes
- Keep implementations simple and embedded-friendly
- Respect the existing architecture and code style
