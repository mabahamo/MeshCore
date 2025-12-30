#include "EmergencyMesh.h"
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

  uint32_t est_timeout;
  int result = sendMessage(*_admin_contact, getRTCClock()->getCurrentTime(), 0, "ping", _expected_ack, est_timeout);

  if (result == MSG_SEND_FAILED) {
    Serial.println("ERROR: Failed to send ping");
    return false;
  }

  Serial.printf("Ping sent (%s), waiting for ACK...\n", result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
  _ack_received = false;
  return true;
}

bool EmergencyMesh::sendAlarm() {
  if (!_admin_contact) {
    Serial.println("ERROR: No admin contact configured");
    return false;
  }

  uint32_t est_timeout;
  int result = sendMessage(*_admin_contact, getRTCClock()->getCurrentTime(), 0, "alarm", _expected_ack, est_timeout);

  if (result == MSG_SEND_FAILED) {
    Serial.println("ERROR: Failed to send alarm");
    return false;
  }

  Serial.printf("ALARM sent (%s)!\n", result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
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

ContactInfo* EmergencyMesh::processAck(const uint8_t *data) {
  if (memcmp(data, &_expected_ack, 4) == 0) {
    Serial.println("ACK received!");
    _ack_received = true;
    _expected_ack = 0;
    return NULL;
  }
  return NULL;
}
