#include "ConfigStore.h"

ConfigStore::ConfigStore(fs::FS& fs, const char* filename) {
  _fs = &fs;
  _filename = filename;
}

void ConfigStore::setDefaults(EmergencyConfig& config) {
  config.frequency = 915.800;
  config.bandwidth = 250.0;
  config.spreading_factor = 12;
  config.coding_rate = 8;
  config.tx_power = 22;
  memset(config.admin_pub_key, 0, PUB_KEY_SIZE);
  memset(config.admin_name, 0, sizeof(config.admin_name));
  config.admin_path_len = -1;
  memset(config.admin_path, 0, sizeof(config.admin_path));
  config.configured = false;
  memset(config.reserved, 0, sizeof(config.reserved));
}

bool ConfigStore::save(const EmergencyConfig& config) {
#if defined(NRF52_PLATFORM)
  _fs->remove(_filename);
  File file = _fs->open(_filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = _fs->open(_filename, "w");
#else
  File file = _fs->open(_filename, "w", true);
#endif

  if (!file) {
    Serial.println("ERROR: Failed to open config file for writing");
    return false;
  }

  size_t written = file.write((const uint8_t*)&config, sizeof(EmergencyConfig));
  file.close();

  if (written != sizeof(EmergencyConfig)) {
    Serial.println("ERROR: Failed to write complete config");
    return false;
  }

  Serial.println("Configuration saved");
  return true;
}

bool ConfigStore::load(EmergencyConfig& config) {
  if (!exists()) {
    Serial.println("No config file found, using defaults");
    setDefaults(config);
    return false;
  }

#if defined(RP2040_PLATFORM)
  File file = _fs->open(_filename, "r");
#else
  File file = _fs->open(_filename);
#endif

  if (!file) {
    Serial.println("ERROR: Failed to open config file for reading");
    setDefaults(config);
    return false;
  }

  size_t read_bytes = file.read((uint8_t*)&config, sizeof(EmergencyConfig));
  file.close();

  if (read_bytes != sizeof(EmergencyConfig)) {
    Serial.println("ERROR: Config file corrupted, using defaults");
    setDefaults(config);
    return false;
  }

  Serial.println("Configuration loaded");
  return true;
}

bool ConfigStore::exists() {
  return _fs->exists(_filename);
}
