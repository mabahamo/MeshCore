#include <Arduino.h>
#include <Mesh.h>
#include <target.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/ArduinoHelpers.h>

#include "EmergencyMesh.h"
#include "ConfigStore.h"

#ifdef DISPLAY_CLASS
  #include <helpers/ui/MomentaryButton.h>
#endif

#define FIRMWARE_VER_TEXT "Emergency Trigger v1.0"

// States
enum State {
  STATE_CONFIG,
  STATE_WAIT_ADMIN,
  STATE_ARMED,
  STATE_SEND_PING_PENDING,  // Waiting to send ping after mesh initializes
  STATE_PING_SENT,
  STATE_ALARM_SENT
};

State current_state = STATE_CONFIG;
unsigned long state_start_time = 0;
unsigned long last_loop_time = 0;
unsigned long ping_send_time = 0;  // When to send the pending ping

#define AWAKE_TIMEOUT_MS 60000  // 60 seconds
#define STATUS_REPORT_INTERVAL_SECS (24 * 60 * 60)  // 24 hours

StdRNG fast_rng;
SimpleMeshTables tables;
EmergencyMesh the_mesh(radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, *new StaticPoolPacketManager(8), tables);

EmergencyConfig config;
ConfigStore config_store(
#if defined(ESP32)
  SPIFFS
#elif defined(RP2040_PLATFORM)
  LittleFS
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS
#endif
);

char command[256];
char device_name[32];  // Generated from MAC address

void halt() {
  while (1) ;
}

void generateDeviceName() {
  uint64_t mac = ESP.getEfuseMac();
  // Use last 3 bytes (6 hex digits) of MAC address
  sprintf(device_name, "boton_%02X%02X%02X",
          (uint8_t)((mac >> 16) & 0xFF),
          (uint8_t)((mac >> 8) & 0xFF),
          (uint8_t)(mac & 0xFF));
}

int getBatteryPercentage(uint16_t millivolts) {
  // LiPo battery: 4.2V (100%) to 3.0V (0%)
  const int MIN_MV = 3000;
  const int MAX_MV = 4200;

  if (millivolts >= MAX_MV) return 100;
  if (millivolts <= MIN_MV) return 0;

  return ((millivolts - MIN_MV) * 100) / (MAX_MV - MIN_MV);
}

void showWelcome() {
  Serial.println("===================================");
  Serial.println(FIRMWARE_VER_TEXT);
  Serial.println("===================================");
  Serial.println();
}

void showConfig() {
  Serial.println("Current Configuration:");
  Serial.printf("  Frequency:    %.3f MHz\n", config.frequency);
  Serial.printf("  Bandwidth:    %.1f kHz\n", config.bandwidth);
  Serial.printf("  SF:           %d\n", config.spreading_factor);
  Serial.printf("  CR:           %d\n", config.coding_rate);
  Serial.printf("  TX Power:     %d dBm\n", config.tx_power);
  Serial.print("  Admin Node:   ");
  if (config.configured) {
    Serial.println(config.admin_name);
    Serial.print("    Key: ");
    mesh::Utils::printHex(Serial, config.admin_pub_key, PUB_KEY_SIZE);
    Serial.println();
  } else {
    Serial.println("NOT SET - waiting for advertisement");
  }
  Serial.println();
}

void showHelp() {
  Serial.println("Commands:");
  Serial.println("  set freq <MHz>         - Set frequency (e.g., 915.800)");
  Serial.println("  set bw <kHz>           - Set bandwidth (e.g., 250.0)");
  Serial.println("  set sf <value>         - Set spreading factor (7-12)");
  Serial.println("  set cr <value>         - Set coding rate (5-8)");
  Serial.println("  set tx <dBm>           - Set TX power");
  Serial.println("  show                   - Show current configuration");
  Serial.println("  save                   - Save configuration");
  Serial.println("  wait                   - Wait for admin node advertisement");
  Serial.println("  arm                    - Enter armed mode (deep sleep)");
  Serial.println("  help                   - Show this help");
  Serial.println();
}

bool handleCommand(const char* cmd) {
  while (*cmd == ' ') cmd++;  // skip leading spaces

  if (memcmp(cmd, "set freq ", 9) == 0) {
    config.frequency = atof(&cmd[9]);
    Serial.printf("Frequency set to %.3f MHz\n", config.frequency);
    return true;
  }
  else if (memcmp(cmd, "set bw ", 7) == 0) {
    config.bandwidth = atof(&cmd[7]);
    Serial.printf("Bandwidth set to %.1f kHz\n", config.bandwidth);
    return true;
  }
  else if (memcmp(cmd, "set sf ", 7) == 0) {
    int sf = atoi(&cmd[7]);
    if (sf >= 7 && sf <= 12) {
      config.spreading_factor = sf;
      Serial.printf("SF set to %d\n", config.spreading_factor);
    } else {
      Serial.println("ERROR: SF must be 7-12");
    }
    return true;
  }
  else if (memcmp(cmd, "set cr ", 7) == 0) {
    int cr = atoi(&cmd[7]);
    if (cr >= 5 && cr <= 8) {
      config.coding_rate = cr;
      Serial.printf("CR set to %d\n", config.coding_rate);
    } else {
      Serial.println("ERROR: CR must be 5-8");
    }
    return true;
  }
  else if (memcmp(cmd, "set tx ", 7) == 0) {
    config.tx_power = atoi(&cmd[7]);
    Serial.printf("TX Power set to %d dBm\n", config.tx_power);
    return true;
  }
  else if (strcmp(cmd, "show") == 0) {
    showConfig();
    return true;
  }
  else if (strcmp(cmd, "save") == 0) {
    if (config_store.save(config)) {
      Serial.println("Configuration saved");
    } else {
      Serial.println("ERROR: Failed to save configuration");
    }
    return true;
  }
  else if (strcmp(cmd, "wait") == 0) {
    // Set radio parameters
    radio_set_params(config.frequency, config.bandwidth, config.spreading_factor, config.coding_rate);
    radio_set_tx_power(config.tx_power);

    Serial.println("Waiting for admin node advertisement...");
    Serial.println("The first node to advertise will be saved as admin.");

#ifdef DISPLAY_CLASS
    display.startFrame();
    display.print("Waiting\nfor admin\nadvert...");
    display.endFrame();
#endif

    the_mesh.startWaitingForAdmin();
    current_state = STATE_WAIT_ADMIN;
    state_start_time = millis();
    return true;
  }
  else if (strcmp(cmd, "arm") == 0) {
    if (!config.configured) {
      Serial.println("ERROR: Please wait for admin node advertisement first");
      Serial.println("Use 'wait' command to start listening.");
      return true;
    }

    // Save config and switch to armed mode
    config_store.save(config);

    Serial.println("Configuration complete. Entering ARMED mode...");
    Serial.println("Press button to wake and send ping.");
    delay(1000);

    current_state = STATE_ARMED;
    state_start_time = millis();

#ifdef ESP32
    // Enter deep sleep on ESP32
    Serial.println("Entering deep sleep...");
    Serial.flush();
    delay(100);

    // Configure wake on button press and 24-hour timer
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);  // wake on LOW (button pressed)
    esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
    esp_deep_sleep_start();
#else
    Serial.println("NOTE: Deep sleep not supported on this platform, staying awake");
#endif
    return true;
  }
  else if (strcmp(cmd, "help") == 0) {
    showHelp();
    return true;
  }

  Serial.print("ERROR: Unknown command: ");
  Serial.println(cmd);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef DISPLAY_CLASS
  display.begin();
  user_btn.begin();
#endif

  // Initialize LED (pin 35 for Heltec V3)
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);  // LED off initially

  // Initialize filesystem
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
#elif defined(ESP32)
  SPIFFS.begin(true);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
#endif

  showWelcome();

  // Load configuration
  config_store.load(config);

  // Generate device name from MAC address
  generateDeviceName();
  Serial.print("Device name: ");
  Serial.println(device_name);

  if (!radio_init()) {
    Serial.println("ERROR: Radio initialization failed");
    halt();
  }

  fast_rng.begin(radio_get_rng_seed());

  // Initialize mesh
  the_mesh.begin(
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    InternalFS
#elif defined(ESP32)
    SPIFFS
#elif defined(RP2040_PLATFORM)
    LittleFS
#endif
  );

#ifdef ESP32
  // Check if we woke from deep sleep
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke from button press!");

    // Load config and set radio parameters
    config_store.load(config);
    radio_set_params(config.frequency, config.bandwidth, config.spreading_factor, config.coding_rate);
    radio_set_tx_power(config.tx_power);

    // Restore admin contact from config
    if (config.configured) {
      ContactInfo admin;
      memset(&admin, 0, sizeof(ContactInfo));
      memcpy(admin.id.pub_key, config.admin_pub_key, PUB_KEY_SIZE);
      strcpy(admin.name, config.admin_name);
      admin.out_path_len = config.admin_path_len;
      if (config.admin_path_len > 0 && config.admin_path_len <= 64) {
        memcpy(admin.out_path, config.admin_path, config.admin_path_len);
      }
      the_mesh.addContact(admin);

      // Restore the admin contact pointer in EmergencyMesh
      the_mesh.restoreAdminContact(config.admin_pub_key);

      // Send advertisement so admin knows we're awake
      the_mesh.sendSelfAdvertisement(device_name);

      // Schedule ping to be sent after mesh loop runs a few times
      current_state = STATE_SEND_PING_PENDING;
      ping_send_time = millis() + 2000;  // Send ping after 2 seconds
    } else {
      current_state = STATE_ARMED;
    }

    state_start_time = millis();

#ifdef DISPLAY_CLASS
    uint16_t batt_mv = board.getBattMilliVolts();
    int batt_pct = getBatteryPercentage(batt_mv);

    Serial.printf("Battery: %d%% (%dmV)\n", batt_pct, batt_mv);
#endif
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke from 24-hour timer - sending battery status...");

    // Load config and set radio parameters
    config_store.load(config);
    radio_set_params(config.frequency, config.bandwidth, config.spreading_factor, config.coding_rate);
    radio_set_tx_power(config.tx_power);

    // Restore admin contact from config
    if (config.configured) {
      ContactInfo admin;
      memset(&admin, 0, sizeof(ContactInfo));
      memcpy(admin.id.pub_key, config.admin_pub_key, PUB_KEY_SIZE);
      strcpy(admin.name, config.admin_name);
      admin.out_path_len = config.admin_path_len;
      if (config.admin_path_len > 0 && config.admin_path_len <= 64) {
        memcpy(admin.out_path, config.admin_path, config.admin_path_len);
      }
      the_mesh.addContact(admin);
      the_mesh.restoreAdminContact(config.admin_pub_key);

      // Get battery status and send it
      uint16_t batt_mv = board.getBattMilliVolts();
      the_mesh.sendBatteryStatus(batt_mv);

      // Wait for message to be sent
      delay(3000);
    }

    // Go back to sleep immediately
    Serial.println("Going back to sleep...");
    Serial.flush();
    delay(100);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
    esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
    esp_deep_sleep_start();
  } else {
#endif
    // First boot or other wakeup reason (power on)
    if (config.configured) {
      Serial.println("Device powered on - already configured.");
      Serial.println("Sending advertisement to admin...");

      // Set radio parameters
      radio_set_params(config.frequency, config.bandwidth, config.spreading_factor, config.coding_rate);
      radio_set_tx_power(config.tx_power);

      // Restore admin contact from config
      ContactInfo admin;
      memset(&admin, 0, sizeof(ContactInfo));
      memcpy(admin.id.pub_key, config.admin_pub_key, PUB_KEY_SIZE);
      strcpy(admin.name, config.admin_name);
      admin.out_path_len = config.admin_path_len;
      if (config.admin_path_len > 0 && config.admin_path_len <= 64) {
        memcpy(admin.out_path, config.admin_path, config.admin_path_len);
      }
      the_mesh.addContact(admin);

      // Restore the admin contact pointer in EmergencyMesh
      the_mesh.restoreAdminContact(config.admin_pub_key);

      // Send flood advertisement so admin knows we're online
      the_mesh.sendSelfAdvertisement(device_name);

      // Schedule ping to be sent after mesh loop runs a few times
      current_state = STATE_SEND_PING_PENDING;
      ping_send_time = millis() + 2000;  // Send ping after 2 seconds

      state_start_time = millis();

#ifdef DISPLAY_CLASS
      uint16_t batt_mv = board.getBattMilliVolts();
      int batt_pct = getBatteryPercentage(batt_mv);

      Serial.printf("Battery: %d%% (%dmV)\n", batt_pct, batt_mv);
#endif
    } else {
      // Not configured - automatically enter WAIT_ADMIN mode
      Serial.println("First boot - waiting for admin node advertisement...");
      Serial.println("The first node to advertise will be saved as admin.");

      // Set radio parameters with defaults
      radio_set_params(config.frequency, config.bandwidth, config.spreading_factor, config.coding_rate);
      radio_set_tx_power(config.tx_power);

#ifdef DISPLAY_CLASS
      display.startFrame();
      display.print("Waiting for\nadmin advert");
      display.endFrame();
#endif

      the_mesh.startWaitingForAdmin();
      current_state = STATE_WAIT_ADMIN;
    }
#ifdef ESP32
  }
#endif

  command[0] = 0;
  state_start_time = millis();
}

void loop() {
  unsigned long now = millis();

  // Handle serial commands in CONFIG mode
  if (current_state == STATE_CONFIG) {
    int len = strlen(command);
    while (Serial.available() && len < sizeof(command)-1) {
      char c = Serial.read();
      if (c != '\n') {
        command[len++] = c;
        command[len] = 0;
        Serial.print(c);
      }
      if (c == '\r') break;
    }

    if (len > 0 && command[len - 1] == '\r') {
      Serial.print('\n');
      command[len - 1] = 0;
      handleCommand(command);
      command[0] = 0;
    }
  }

  // Handle WAIT_ADMIN mode - waiting for admin advertisement
  else if (current_state == STATE_WAIT_ADMIN) {
    if (the_mesh.isAdminAdvertReceived()) {
      // Admin contact received, save to config
      ContactInfo* admin = the_mesh.getAdminContact();
      if (admin) {
        memcpy(config.admin_pub_key, admin->id.pub_key, PUB_KEY_SIZE);
        strncpy(config.admin_name, admin->name, sizeof(config.admin_name) - 1);
        config.admin_path_len = admin->out_path_len;
        if (admin->out_path_len > 0 && admin->out_path_len <= 64) {
          memcpy(config.admin_path, admin->out_path, admin->out_path_len);
        }
        config.configured = true;

        // Send our advertisement back so admin knows about us
        the_mesh.sendSelfAdvertisement(device_name);

        config_store.save(config);

        Serial.println("\nAdmin node configured!");
        Serial.println("Device is now ARMED.");
        Serial.println("Entering deep sleep...");

#ifdef DISPLAY_CLASS
        display.startFrame();
        display.print("Admin OK!\nARMED\nGoing to\nsleep...");
        display.endFrame();
#endif

        delay(2000);  // Show message briefly

        // Auto-arm and go to deep sleep
#ifdef ESP32
        Serial.flush();
        delay(100);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
        esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
        esp_deep_sleep_start();
#else
        Serial.println("NOTE: Deep sleep not supported, staying awake");
        current_state = STATE_ARMED;
        state_start_time = millis();
#endif
      }
    }
  }

  // Handle SEND_PING_PENDING mode - waiting to send ping after mesh initializes
  else if (current_state == STATE_SEND_PING_PENDING) {
    if (now >= ping_send_time) {
      Serial.println("Auto-sending PING...");
      if (the_mesh.sendPing()) {
        current_state = STATE_PING_SENT;

#ifdef DISPLAY_CLASS
        display.startFrame();
        display.print("PING SENT\nWaiting ACK...");
        display.endFrame();
#endif
      } else {
        Serial.println("Failed to send ping, entering ARMED mode");
        current_state = STATE_ARMED;
      }
      state_start_time = now;
    }

    // Also allow double-click during pending state
#ifdef DISPLAY_CLASS
    int btn_event = user_btn.check();
    if (btn_event == BUTTON_EVENT_DOUBLE_CLICK) {
      Serial.println("Double press - sending ALARM!");
      if (the_mesh.sendAlarm()) {
        current_state = STATE_ALARM_SENT;
        state_start_time = now;

#ifdef DISPLAY_CLASS
        display.startFrame();
        display.print("ALARM SENT!");
        display.endFrame();
#endif
      }
    }
#endif

    // Timeout - go back to sleep
    if (now - state_start_time > AWAKE_TIMEOUT_MS) {
      Serial.println("Timeout - going back to sleep...");

      // Turn off LED before sleeping
      digitalWrite(P_LORA_TX_LED, LOW);

      Serial.flush();
      delay(100);

#ifdef ESP32
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
      esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
      esp_deep_sleep_start();
#endif
    }
  }

  // Handle ARMED mode (awake from deep sleep)
  else if (current_state == STATE_ARMED) {
#ifdef DISPLAY_CLASS
    int btn_event = user_btn.check();

    if (btn_event == BUTTON_EVENT_DOUBLE_CLICK) {
      Serial.println("Double press - sending ALARM!");
      if (the_mesh.sendAlarm()) {
        current_state = STATE_ALARM_SENT;
        state_start_time = now;

#ifdef DISPLAY_CLASS
        display.startFrame();
        display.print("ALARM SENT!");
        display.endFrame();
#endif
      }
    }
#endif

    // Timeout - go back to sleep
    if (now - state_start_time > AWAKE_TIMEOUT_MS) {
      Serial.println("Timeout - going back to sleep...");

      // Turn off LED before sleeping
      digitalWrite(P_LORA_TX_LED, LOW);

      Serial.flush();
      delay(100);

#ifdef ESP32
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
      esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
      esp_deep_sleep_start();
#endif
    }
  }

  // Handle PING_SENT mode
  else if (current_state == STATE_PING_SENT) {
    if (the_mesh.isAckReceived()) {
      Serial.println("ACK received! Press button twice to send ALARM.");

      // Turn LED green (ON)
      digitalWrite(P_LORA_TX_LED, HIGH);

#ifdef DISPLAY_CLASS
      display.startFrame();
      display.print("ACK!\nDouble press\nfor ALARM");
      display.endFrame();
#endif

      the_mesh.resetAck();
      current_state = STATE_ARMED;
      state_start_time = now;
    }

#ifdef DISPLAY_CLASS
    int btn_event = user_btn.check();
    if (btn_event == BUTTON_EVENT_DOUBLE_CLICK) {
      Serial.println("Double press - sending ALARM!");
      if (the_mesh.sendAlarm()) {
        current_state = STATE_ALARM_SENT;
        state_start_time = now;

#ifdef DISPLAY_CLASS
        display.startFrame();
        display.print("ALARM SENT!");
        display.endFrame();
#endif
      }
    }
#endif

    // Timeout
    if (now - state_start_time > AWAKE_TIMEOUT_MS) {
      Serial.println("Timeout - going back to sleep...");

      // Turn off LED before sleeping
      digitalWrite(P_LORA_TX_LED, LOW);

      Serial.flush();
      delay(100);

#ifdef ESP32
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
      esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
      esp_deep_sleep_start();
#endif
    }
  }

  // Handle ALARM_SENT mode
  else if (current_state == STATE_ALARM_SENT) {
    // Wait a bit for transmission to complete, then sleep
    if (now - state_start_time > 5000) {
      Serial.println("Alarm sent. Going back to sleep...");

      // Turn off LED before sleeping
      digitalWrite(P_LORA_TX_LED, LOW);

      Serial.flush();
      delay(100);

#ifdef ESP32
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
      esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
      esp_deep_sleep_start();
#endif
    }
  }

  // Run mesh loop
  the_mesh.loop();
  rtc_clock.tick();

  // Throttle loop
  if (now - last_loop_time < 10) {
    delay(10 - (now - last_loop_time));
  }
  last_loop_time = now;
}
