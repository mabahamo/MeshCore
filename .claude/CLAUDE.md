# MeshCore Project - Claude Instructions

## Project Overview
MeshCore is a LoRa mesh networking firmware for embedded devices, with primary support for the T1000-E tracker hardware. The project implements encrypted peer-to-peer messaging, group channels, and mesh routing protocols.

## Build System
- **Platform**: PlatformIO-based build system
- **Primary build target**: `make t100e` (builds `env:t1000e_companion_radio_ble`)
- **Firmware output**: `.pio/build/t1000e_companion_radio_ble/firmware.uf2`
- **Flashing**: Copy UF2 file to device in bootloader mode (double-press reset button)

## Architecture

### Key Components
1. **Mesh Core** (`src/`)
   - `Dispatcher.cpp/h`: Radio packet handling, receive/transmit scheduling
   - `Mesh.cpp/h`: Core mesh protocol, packet routing, encryption/decryption
   - `Packet.h`: Packet structure definitions
   - `Identity.cpp/h`: Cryptographic identity management (Ed25519)

2. **Radio Layer** (`src/helpers/radiolib/`)
   - Abstraction over RadioLib for LoRa radios (LR1110, SX126x, etc.)
   - `RadioLibWrappers.cpp`: Hardware-specific radio implementations

3. **Application Layer** (`examples/companion_radio/`)
   - `MyMesh.cpp/h`: Application-level mesh features (contacts, channels, messaging)
   - `DataStore.cpp/h`: Persistent storage for contacts, channels, settings
   - `ui-orig/UITask.cpp/h`: User interface (display, button handling, buzzer)

### Message Flow
- **Channel messages**: Encrypted with shared channel secret, no contact lookup required
- **Direct messages**: Encrypted with per-contact shared secret, sender MUST be in receiver's contact list
- Messages are queued in `offline_queue` when companion app is disconnected
- Messages trigger notifications via buzzer (RTTTL tones)

## Important Patterns

### Debugging
- Use `MESH_DEBUG_PRINTLN()` for debug logging (requires `-D MESH_DEBUG=1` in platformio.ini)
- For production, comment out debug flags with `;` prefix in platformio.ini
- Serial monitoring: `screen /dev/tty.usbmodem* 115200` (or `cu`/`miniterm`)

### Timing
- Use `millis()` for Arduino timing (32-bit millisecond counter with wraparound)
- Helper: `millisHasNowPassed(timestamp)` handles wraparound correctly
- Helper: `futureMillis(delay_ms)` calculates future timestamp

### Memory Management
- Packets use static pool allocation (see `StaticPoolPacketManager`)
- ALWAYS call `releasePacket()` or `_mgr->free()` to return packets to pool
- Contact/channel storage uses fixed-size arrays (MAX_CONTACTS, MAX_GROUP_CHANNELS)

### UI/Buzzer Integration
- Buzzer plays RTTTL (Ring Tone Text Transfer Language) melodies non-blocking
- Check `buzzer.isPlaying()` before starting new tone
- Notifications: `notify(UIEventType::...)` in UITask.cpp

## Common Tasks

### Adding New Features
1. State variables go in class headers (MyMesh.h, UITask.h, etc.)
2. Initialize in constructors
3. For persistent state, add to `NodePrefs` and call `savePrefs()`
4. For UI features, coordinate between MyMesh and UITask via public methods

### Modifying Packet Handling
- **Receive path**: `Dispatcher::checkRecv()` → `Mesh::onRecvPacket()` → `MyMesh::onMessageRecv()` / `onChannelMessageRecv()`
- **Send path**: Create packet → `sendFlood()` / `sendDirect()` → `Dispatcher::checkSend()`
- Always check `hasSeen()` before processing/retransmitting packets

### Button Handling
- Button events defined in `ui-orig/Button.cpp`
- Callbacks registered in `UITask::begin()`
- Short/double/triple/quad/long press patterns available
- `handleButtonAnyPress()` fires first (for wake-up), then specific handler

## Hardware Variants
- **T1000-E**: Nordic nRF52840 + LR1110 radio, buzzer on pin 25, LED on pin 24
- Config in `variants/t1000-e/platformio.ini` and `variants/t1000-e/target.cpp`
- Board-specific code in `src/helpers/nrf52/T1000eBoard.cpp`

## Testing
- Physical testing requires two devices on same LoRa settings
- Default channel is "Public" (shared across all users)
- Direct messaging requires sender in receiver's contacts
- Monitor with serial terminal for debug output

## Git Workflow
- Never commit to main branch directly
- Create feature branches for all changes
- Use conventional commit messages
- PRs should target the `main` branch
