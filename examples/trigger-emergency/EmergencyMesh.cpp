#include "EmergencyMesh.h"
#include "ConfigStore.h"
#include <helpers/IdentityStore.h>
#include <helpers/ArduinoHelpers.h>

void EmergencyMesh::begin(fs::FS& fs) {
  _fs = &fs;
  BaseChatMesh::begin();

#if defined(NRF52_PLATFORM)
  IdentityStore store(fs, "");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(fs, "/identity");
  store.begin();
#else
  IdentityStore store(fs, "/identity");
#endif

  if (!store.load("_main", self_id)) {
    Serial.println("Generating new keypair...");
    ((StdRNG *)getRNG())->begin(millis());
    self_id = mesh::LocalIdentity(getRNG());

    int count = 0;
    while (count < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
      self_id = mesh::LocalIdentity(getRNG());
      count++;
    }
    store.save("_main", self_id);
  }

  Serial.print("Emergency Node ID: ");
  mesh::Utils::printHex(Serial, self_id.pub_key, PUB_KEY_SIZE);
  Serial.println();
}

void EmergencyMesh::onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  if (_waiting_for_admin) {
    // First advertisement received - save as admin contact
    Serial.print("Admin node discovered: ");
    Serial.println(contact.name);
    Serial.print("  Public key: ");
    mesh::Utils::printHex(Serial, contact.id.pub_key, PUB_KEY_SIZE);
    Serial.println();
    Serial.printf("  Path length: %d\n", contact.out_path_len);

    _admin_contact = lookupContactByPubKey(contact.id.pub_key, PUB_KEY_SIZE);
    _admin_advert_received = true;
    _waiting_for_admin = false;
  }
}

void EmergencyMesh::restoreAdminContact(const uint8_t* pub_key) {
  _admin_contact = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (_admin_contact) {
    Serial.println("Admin contact restored");
  } else {
    Serial.println("WARNING: Failed to restore admin contact");
  }
}

void EmergencyMesh::sendSelfAdvertisement(const char* device_name) {
  Serial.print("Sending advertisement as: ");
  Serial.println(device_name);
  auto pkt = createSelfAdvert(device_name);
  if (pkt) {
    sendFlood(pkt, 500);  // small delay
    Serial.println("Advertisement sent");
  }
}

bool EmergencyMesh::sendPing() {
  if (!_admin_contact) {
    Serial.println("ERROR: No admin contact configured");
    return false;
  }

  _send_attempt = 0;  // Reset attempt counter
  _is_alarm = false;
  _last_message = "ping";

  uint32_t est_timeout;
  int result = sendMessage(*_admin_contact, getRTCClock()->getCurrentTime(), _send_attempt, "ping", _expected_ack, est_timeout);

  if (result == MSG_SEND_FAILED) {
    Serial.println("ERROR: Failed to send ping");
    return false;
  }

  Serial.printf("Ping sent (attempt %d, %s), waiting for ACK...\n", _send_attempt, result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
  _ack_received = false;
  return true;
}

bool EmergencyMesh::sendAlarm() {
  if (!_admin_contact) {
    Serial.println("ERROR: No admin contact configured");
    return false;
  }

  _send_attempt = 0;  // Reset attempt counter
  _is_alarm = true;
  _last_message = "alarm";

  uint32_t est_timeout;
  int result = sendMessage(*_admin_contact, getRTCClock()->getCurrentTime(), _send_attempt, "alarm", _expected_ack, est_timeout);

  if (result == MSG_SEND_FAILED) {
    Serial.println("ERROR: Failed to send alarm");
    return false;
  }

  Serial.printf("ALARM sent (attempt %d, %s)!\n", _send_attempt, result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
  _ack_received = false;
  return true;
}

bool EmergencyMesh::sendBatteryStatus(uint16_t millivolts) {
  if (!_admin_contact) {
    Serial.println("ERROR: No admin contact configured");
    return false;
  }

  char status_msg[32];
  sprintf(status_msg, "status:%d", millivolts);

  uint32_t dummy_ack = 0;
  uint32_t est_timeout;
  int result = sendMessage(*_admin_contact, getRTCClock()->getCurrentTime(), 0, status_msg, dummy_ack, est_timeout);

  if (result == MSG_SEND_FAILED) {
    Serial.println("ERROR: Failed to send battery status");
    return false;
  }

  Serial.printf("Battery status sent: %dmV (%s)\n", millivolts, result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
  return true;
}

bool EmergencyMesh::retrySend() {
  if (!_admin_contact || !_last_message) {
    return false;  // Can't retry
  }

  _send_attempt++;

  // After 2 failed attempts with path, reset path and try FLOOD
  if (_send_attempt == 2 && _admin_contact->out_path_len >= 0) {
    Serial.println("Path failed after 2 attempts, resetting to FLOOD");
    _admin_contact->out_path_len = -1;  // Reset path to force FLOOD
  }

  // Give up after 3 total attempts
  if (_send_attempt >= 3) {
    Serial.println("Send failed after 3 attempts");
    return false;
  }

  uint32_t est_timeout;
  int result = sendMessage(*_admin_contact, getRTCClock()->getCurrentTime(), _send_attempt, _last_message, _expected_ack, est_timeout);

  if (result == MSG_SEND_FAILED) {
    Serial.printf("ERROR: Retry %d failed to send\n", _send_attempt);
    return false;
  }

  Serial.printf("Retry %d sent (%s), waiting for ACK...\n", _send_attempt, result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
  _ack_received = false;
  return true;
}

void EmergencyMesh::onContactPathUpdated(const ContactInfo& contact) {
  // Check if this is the admin contact
  if (_admin_contact && contact.id.matches(_admin_contact->id)) {
    Serial.printf("Admin path updated: len=%d\n", contact.out_path_len);

    // Save updated path to config
    if (_config_store) {
      ConfigStore* store = (ConfigStore*)_config_store;
      EmergencyConfig config;
      store->load(config);

      // Update path info
      config.admin_path_len = contact.out_path_len;
      if (contact.out_path_len > 0 && contact.out_path_len <= 64) {
        memcpy(config.admin_path, contact.out_path, contact.out_path_len);
        Serial.println("Saving admin path to config");
        store->save(config);
      }
    }
  }
}

ContactInfo* EmergencyMesh::processAck(const uint8_t *data) {
  if (memcmp(data, &_expected_ack, 4) == 0) {
    Serial.println("ACK received!");
    _ack_received = true;
    _expected_ack = 0;
    _send_attempt = 0;  // Reset on success
    return NULL;
  }
  return NULL;
}
