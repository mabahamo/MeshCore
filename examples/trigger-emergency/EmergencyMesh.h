#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/BaseChatMesh.h>
#include <FS.h>

#define MAX_CONTACTS 5

class EmergencyMesh : public BaseChatMesh {
  fs::FS* _fs;
  ContactInfo* _admin_contact;
  uint32_t _expected_ack;
  bool _ack_received;
  bool _admin_advert_received;
  bool _waiting_for_admin;
  uint8_t _send_attempt;
  bool _is_alarm;  // true if current send is alarm, false if ping
  const char* _last_message;  // "ping" or "alarm"
  void* _config_store;  // Pointer to ConfigStore for saving path updates
  bool _send_failed;  // true if all retries exhausted

public:
  EmergencyMesh(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
                mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
     : BaseChatMesh(radio, ms, rng, rtc, mgr, tables)
  {
    _admin_contact = NULL;
    _expected_ack = 0;
    _ack_received = false;
    _admin_advert_received = false;
    _waiting_for_admin = false;
    _send_attempt = 0;
    _is_alarm = false;
    _last_message = NULL;
    _config_store = NULL;
    _send_failed = false;
  }

  void begin(fs::FS& fs);
  void startWaitingForAdmin() { _waiting_for_admin = true; }
  bool hasAdminContact() const { return _admin_contact != NULL; }
  bool isAdminAdvertReceived() const { return _admin_advert_received; }
  ContactInfo* getAdminContact() { return _admin_contact; }
  void restoreAdminContact(const uint8_t* pub_key);
  void sendSelfAdvertisement(const char* device_name);
  bool sendPing();
  bool sendAlarm();
  bool sendBatteryStatus(uint16_t millivolts);
  bool isAckReceived() const { return _ack_received; }
  void resetAck() { _ack_received = false; _expected_ack = 0; }
  bool retrySend();  // Retry current send with fallback logic
  void setConfigStore(void* config_store_ptr) { _config_store = config_store_ptr; }
  bool isWaitingForAck() const { return _expected_ack != 0; }
  uint8_t getSendAttempt() const { return _send_attempt; }
  bool hasSendFailed() const { return _send_failed; }
  void resetSendFailed() { _send_failed = false; }

protected:
  // BaseChatMesh callbacks
  float getAirtimeBudgetFactor() const override { return 2.0; }

  int calcRxDelay(float score, uint32_t air_time) const override {
    return 0;  // no delay
  }

  bool allowPacketForward(const mesh::Packet* packet) override {
    return false;  // emergency node doesn't forward packets
  }

  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override;

  void onContactPathUpdated(const ContactInfo& contact) override;

  ContactInfo* processAck(const uint8_t *data) override;

  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp, const char *text) override {
    // Not used in emergency node
  }

  void onCommandDataRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp, const char *text) override {
    // Not used in emergency node
  }

  void onSignedMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp, const uint8_t *sender_prefix, const char *text) override {
    // Not used in emergency node
  }

  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t timestamp, const char *text) override {
    // Not used in emergency node
  }

  uint8_t onContactRequest(const ContactInfo& contact, uint32_t sender_timestamp, const uint8_t* data, uint8_t len, uint8_t* reply) override {
    return 0;  // unknown
  }

  void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override {
    // Not used in emergency node
  }

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
    return 500 + (16 * pkt_airtime_millis);
  }

  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override {
    return 500 + ((pkt_airtime_millis * 6 + 250) * (path_len + 1));
  }

  void onSendTimeout() override {
    Serial.println("Send timeout, no ACK");
    if (!retrySend()) {
      _send_failed = true;  // All retries exhausted
    }
  }
};
