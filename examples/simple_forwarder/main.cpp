#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    halt();
  }

  fast_rng.begin(radio_get_rng_seed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Forwarder ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef ESP32
  // Initialize WiFi with AP name based on last 4 hex digits of device ID
  char ap_name[32];
  char id_suffix[5];
  mesh::Utils::toHex(id_suffix, &the_mesh.self_id.pub_key[PUB_KEY_SIZE - 2], 2);
  sprintf(ap_name, "Forwarder_%s", id_suffix);
  the_mesh.initWiFi(ap_name);
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // NOTE: Forwarder does NOT advertise itself to the mesh network
}

#ifdef ESP32
// Factory reset button handling
static unsigned long button_press_start = 0;
static bool button_was_pressed = false;
static bool factory_reset_triggered = false;
#endif

void loop() {
#ifdef ESP32
  // Check for factory reset button (4 second hold)
  #ifdef PIN_USER_BTN
  bool button_pressed = (digitalRead(PIN_USER_BTN) == LOW);

  if (button_pressed && !button_was_pressed) {
    // Button just pressed
    button_press_start = millis();
    button_was_pressed = true;
  } else if (!button_pressed && button_was_pressed) {
    // Button released
    button_was_pressed = false;
  } else if (button_pressed && button_was_pressed) {
    // Button is being held
    if ((millis() - button_press_start > 4000) && !factory_reset_triggered) {
      // Factory reset!
      factory_reset_triggered = true;
      Serial.println("\n\n=== FACTORY RESET ===");
      Serial.println("Clearing WiFi credentials...");

      WiFiManager wm;
      wm.resetSettings();

      Serial.println("Restarting...");
      delay(1000);
      ESP.restart();
    }
  }
  #endif
#endif

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
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
}
