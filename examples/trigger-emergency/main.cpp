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
  STATE_ALARM_SENT
};

State current_state = STATE_CONFIG;
unsigned long state_start_time = 0;
unsigned long last_loop_time = 0;

#define AWAKE_TIMEOUT_MS 120000  // 120 seconds (2 minutes)
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
int current_battery_pct = 0;  // Current battery percentage

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

#ifdef DISPLAY_CLASS
void showStartupLogo() {
  display.startFrame();
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.print("B9");
  display.setCursor(0, 40);
  display.print("Ingenieria");
  display.setTextSize(1);
  display.endFrame();
  delay(500);  // Show logo for 0.5 seconds
}

void showAlarmInstruction(int battery_pct) {
  display.startFrame();

  // Arrow pointing to button (small font)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("<--");

  // Battery percentage at top right (small font)
  display.setCursor(96, 0);
  char batt_str[8];
  sprintf(batt_str, "%d%%", battery_pct);
  display.print(batt_str);

  // Instruction text - large font for readability
  display.setTextSize(2);
  display.setCursor(5, 10);
  display.print("Doble");
  display.setCursor(5, 28);
  display.print("click");
  display.setCursor(5, 46);
  display.print("ALARMA");

  // Reset text size
  display.setTextSize(1);

  display.endFrame();
}

void showAlarmSending(int battery_pct, int attempt) {
  display.startFrame();

  // Battery percentage at top right (small font)
  display.setTextSize(1);
  display.setCursor(96, 0);
  char batt_str[8];
  sprintf(batt_str, "%d%%", battery_pct);
  display.print(batt_str);

  // Alarm sending message - large font for readability
  display.setTextSize(2);
  display.setCursor(5, 10);
  display.print("Enviando");
  display.setCursor(5, 28);
  display.print("Alarma");
  display.setCursor(5, 46);
  char msg[8];
  sprintf(msg, "%d/4", attempt + 1);  // attempt is 0-based, display 1-based
  display.print(msg);

  // Reset text size
  display.setTextSize(1);

  display.endFrame();
}

void showAlarmActivated(int battery_pct) {
  display.startFrame();

  // Battery percentage at top right (small font)
  display.setTextSize(1);
  display.setCursor(96, 0);
  char batt_str[8];
  sprintf(batt_str, "%d%%", battery_pct);
  display.print(batt_str);

  // Success message - large font, centered
  display.setTextSize(2);
  display.setCursor(10, 15);
  display.print("ALARMA");
  display.setCursor(0, 40);
  display.print("ACTIVADA");

  // Reset text size to default
  display.setTextSize(1);

  display.endFrame();
}
#endif

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
  showStartupLogo();  // Show B9 Ingenier\u00eda logo on startup
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

  // Pass config_store pointer to mesh for path updates
  the_mesh.setConfigStore(&config_store);

#ifdef ESP32
  // Check if we woke from deep sleep
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  // Only show logo on power-up, not after deep sleep
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
#ifdef DISPLAY_CLASS
    showStartupLogo();  // Show B9 Ingeniería logo only on power-up
#endif
  }

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

      // Get battery status
      uint16_t batt_mv = board.getBattMilliVolts();
      current_battery_pct = getBatteryPercentage(batt_mv);
      Serial.printf("Battery: %d%% (%dmV)\n", current_battery_pct, batt_mv);

      // Send advertisement so admin knows we're awake
      the_mesh.sendSelfAdvertisement(device_name);

      // Go directly to ARMED state - wait for button press
      current_state = STATE_ARMED;

#ifdef DISPLAY_CLASS
      // Show alarm activation instruction
      showAlarmInstruction(current_battery_pct);
#endif
    } else {
      current_state = STATE_ARMED;
    }

    state_start_time = millis();
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

      // Get battery status
      uint16_t batt_mv = board.getBattMilliVolts();
      current_battery_pct = getBatteryPercentage(batt_mv);
      Serial.printf("Battery: %d%% (%dmV)\n", current_battery_pct, batt_mv);

      // Send flood advertisement so admin knows we're online
      the_mesh.sendSelfAdvertisement(device_name);

      // Go directly to ARMED state - wait for button press
      current_state = STATE_ARMED;

#ifdef DISPLAY_CLASS
      // Show alarm activation instruction
      showAlarmInstruction(current_battery_pct);
#endif

      state_start_time = millis();
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

  // Handle ARMED mode (awake from deep sleep or power on)
  else if (current_state == STATE_ARMED) {
#ifdef DISPLAY_CLASS
    int btn_event = user_btn.check();

    if (btn_event == BUTTON_EVENT_DOUBLE_CLICK) {
      Serial.println("Double press - sending ALARM!");

      // Turn on orange LED immediately when alarm is triggered
      digitalWrite(P_LORA_TX_LED, HIGH);

      if (the_mesh.sendAlarm()) {
        current_state = STATE_ALARM_SENT;
        state_start_time = now;

#ifdef DISPLAY_CLASS
        showAlarmSending(current_battery_pct, 0);  // Attempt 0 (first send)
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

  // Handle ALARM_SENT mode
  else if (current_state == STATE_ALARM_SENT) {
    // Check if ACK received (alarm successful)
    if (the_mesh.isAckReceived()) {
      Serial.println("Alarm ACK received! Showing confirmation for 2 minutes...");

#ifdef DISPLAY_CLASS
      showAlarmActivated(current_battery_pct);
#endif

      // Turn LED green (ON) to show success
      digitalWrite(P_LORA_TX_LED, HIGH);

      the_mesh.resetAck();
      the_mesh.resetSendFailed();

      // Wait for 2 minutes showing the success message
      delay(120000);  // 2 minutes

      // Turn off LED before sleeping
      digitalWrite(P_LORA_TX_LED, LOW);

      Serial.println("Going back to sleep...");
      Serial.flush();
      delay(100);

#ifdef ESP32
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
      esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
      esp_deep_sleep_start();
#endif
    }

    // Check if all retries failed
    if (the_mesh.hasSendFailed()) {
      Serial.println("Alarm failed after all retries, but continuing to sleep anyway");

#ifdef DISPLAY_CLASS
      display.startFrame();
      display.print("ALARMA\nENVIADA\n(sin conf)");
      display.endFrame();
      delay(2000);
#endif

      // Turn off LED before sleeping
      digitalWrite(P_LORA_TX_LED, LOW);

      the_mesh.resetSendFailed();

      Serial.flush();
      delay(100);

#ifdef ESP32
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BTN, 0);
      esp_sleep_enable_timer_wakeup(STATUS_REPORT_INTERVAL_SECS * 1000000ULL);
      esp_deep_sleep_start();
#endif
    }

    // Update display with current retry attempt
    static uint8_t last_displayed_attempt = 255;  // Track last displayed attempt
    uint8_t current_attempt = the_mesh.getSendAttempt();
    if (current_attempt != last_displayed_attempt) {
#ifdef DISPLAY_CLASS
      showAlarmSending(current_battery_pct, current_attempt);
#endif
      last_displayed_attempt = current_attempt;
    }

    // Timeout - go to sleep even if no confirmation (2 minutes)
    if (now - state_start_time > 120000) {  // 120 seconds (2 minutes) max wait
      Serial.println("Alarm timeout - going to sleep");

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
