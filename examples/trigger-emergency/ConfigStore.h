#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <FS.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

struct EmergencyConfig {
  float frequency;       // MHz
  float bandwidth;       // kHz
  uint8_t spreading_factor;
  uint8_t coding_rate;
  uint8_t tx_power;
  uint8_t admin_pub_key[PUB_KEY_SIZE];
  char admin_name[32];
  int8_t admin_path_len;
  uint8_t admin_path[64];
  bool configured;
  uint8_t reserved[37];   // padding for alignment (increased from 6 to include removed device_name space)
};

class ConfigStore {
  fs::FS* _fs;
  const char* _filename;

public:
  ConfigStore(fs::FS& fs, const char* filename = "/emergency_config");

  bool save(const EmergencyConfig& config);
  bool load(EmergencyConfig& config);
  bool exists();
  void setDefaults(EmergencyConfig& config);
};
