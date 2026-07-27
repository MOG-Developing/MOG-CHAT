#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <Arduino.h>
#include <limits.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define VERSION "MOG-CHAT V1.3"

// GPIO for boards with RF switch between PCB/IPEX antennas. -1 to skip.
#define ANTENNA_GPIO -1
#define ANTENNA_SELECT 1

#define DEFAULT_SSID "NoWifiHere"           // make the ssid different for every ESP.
#define DEFAULT_PASSWORD "mogchat123"
#define DEFAULT_CHANNEL 1
#define DEFAULT_ADMIN_USER "admin1"
#define DEFAULT_ADMIN_PASS "admin"
#define DEFAULT_NODE_PREFIX "MOG-"
#define DEFAULT_TX_POWER WIFI_POWER_13dBm
#define DEFAULT_DISC_INTERVAL 30
#define DEFAULT_PAIRING_KEY "ElectricBlanket1376891376SKFRHPZJE"
#define MAX_MESSAGES 64
#define MAX_MSG_LEN_SYNC 180
#define MAX_MSG_LEN_LOCAL 512
#define MAX_USER_LEN 20
#define MAX_NODE_ID_LEN 16
#define MAX_PEERS 6
#define MAX_DISCOVERED 16
#define ESPNOW_QUEUE_SIZE 16
#define MAX_AUTH_FAILS 5
#define AUTH_LOCKOUT_SECS 30
#define RATE_LIMIT_SLOTS 16

struct __attribute__((packed)) Message {
  char nodeId[MAX_NODE_ID_LEN + 1];
  char user[MAX_USER_LEN + 1];
  char msg[MAX_MSG_LEN_SYNC + 1];
  unsigned long time;
};

struct Peer {
  uint8_t mac[6];
  char nodeId[MAX_NODE_ID_LEN + 1];
  uint32_t challenge;
  uint32_t lastSeq;
  unsigned long lastSeen;
  bool paired;
  bool awaitingAccept;
  bool online;
  int8_t rssi;
};

struct DiscoveredNode {
  uint8_t mac[6];
  char nodeId[MAX_NODE_ID_LEN + 1];
  uint32_t challenge;
  unsigned long lastSeen;
  int8_t rssi;
};


struct __attribute__((packed)) EspNowPacket {
  uint8_t type;
  char senderNodeId[MAX_NODE_ID_LEN + 1];
  uint32_t seq;
  uint16_t dataLen;
  uint8_t data[226];
};

enum PacketType {
  PKT_DISCOVERY = 0,
  PKT_PAIR_REQUEST = 1,
  PKT_PAIR_ACCEPT = 2,
  PKT_PAIR_REJECT = 3,
  PKT_UNPAIR = 4,
  PKT_MESSAGE = 5,
  PKT_PING = 6,
  PKT_PONG = 7
};

struct IncomingPacket {
  uint8_t mac[6];
  int8_t rssi;
  EspNowPacket pkt;
};

static bool parseMac(const char *str, uint8_t mac[6]) {
  return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}


static void deriveSaltedKey(const char *salt, const char *keyStr, uint8_t output[16]) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)salt, strlen(salt));
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)keyStr, strlen(keyStr));
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  for (int i = 0; i < 5000; i++) {
    mbedtls_sha256_ret(hash, 32, hash, 0);
  }
  memcpy(output, hash, 16);
}

static void deriveLmk(const char *keyStr, uint8_t lmk[16]) {
  deriveSaltedKey("lmk", keyStr, lmk);
}

static void deriveXorKey(const char *keyStr, uint32_t &xorKey) {
  uint8_t hash[32];
  mbedtls_sha256_ret((const unsigned char *)keyStr, strlen(keyStr), hash, 0);
  memcpy(&xorKey, hash, 4);
}

WebServer server(80);
Preferences prefs;

char nodeId[MAX_NODE_ID_LEN + 1];
char apSsid[32];
char apPassword[64];
char adminUser[32];
char adminPass[64];
uint8_t wifiChannel;
int8_t txPower;
uint16_t discInterval;
uint32_t pairingXorKey;
char pairingKey[64];
char csrfToken[17];

static void generateCsrfToken() {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  snprintf(csrfToken, sizeof(csrfToken), "%08X%08X", r1, r2);
}

static bool checkCsrf() {
  if (!server.hasArg("_csrf")) { server.send(403, "text/plain", "ERR_CSRF"); return false; }
  if (server.arg("_csrf").equals(String(csrfToken))) return true;
  server.send(403, "text/plain", "ERR_CSRF");
  return false;
}

static void addEncryptedPeer(const uint8_t *mac) {
  uint8_t lmk[16];
  deriveLmk(pairingKey, lmk);
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = wifiChannel;
  peerInfo.ifidx = WIFI_IF_AP;
  peerInfo.encrypt = true;
  memcpy(peerInfo.lmk, lmk, 16);
  if (esp_now_is_peer_exist(mac)) {
    esp_now_mod_peer(&peerInfo);
  } else {
    esp_now_add_peer(&peerInfo);
  }
}


Message messages[MAX_MESSAGES];
byte msgIndex = 0;
byte msgTotal = 0;

Peer peers[MAX_PEERS];
byte peerCount = 0;

DiscoveredNode discovered[MAX_DISCOVERED];
byte discoveredCount = 0;

QueueHandle_t pktQueue;

uint32_t pktSeqCounter = 0;
unsigned long lastDiscoveryTime = 0;
unsigned long lastPeerCheckTime = 0;
unsigned long bootTime = 0;
unsigned long lastConfigSave = 0;
uint8_t authFailCount = 0;
unsigned long authLockoutUntil = 0;

typedef struct {
  char user[MAX_USER_LEN + 1];
  unsigned long lastTime;
} RateLimitEntry;
RateLimitEntry rateLimitSlots[RATE_LIMIT_SLOTS];

bool configChanged = false;
bool adminAuthEnabled = true;


void loadConfig();
void saveConfig();
String generateNodeId();
void initESPNow();
void broadcastDiscovery();
void processPacket(const uint8_t *mac, const EspNowPacket *pkt);
void sendEspNowPacket(const uint8_t *destMac, uint8_t type, const uint8_t *data, uint16_t dataLen);
void addMessage(const char *nodeId, const char *user, const char *msg, unsigned long time);
void syncMessageToPeers(const char *user, const char *msg);
String generateMessagesHtml();
bool checkAdminAuth();
void handleRoot();
void handleMessages();
void handleSend();
void handleSettings();
void handleSettingsSave();
void handleAdmin();
void handlePeers();
void handlePeersPair();
void handlePeersUnpair();
void handleClearMessages();
void handleReboot();
void handleNotFound();
int findPeerByMac(const uint8_t *mac);
int findDiscoveredByMac(const uint8_t *mac);
int findFreePeerSlot();
int findFreeDiscoveredSlot();
void removePeer(int idx);


void loadConfig() {
  prefs.begin("mogchat", false);

  String sid = prefs.getString("node_id", "");
  if (sid.length() == 0) {
    sid = generateNodeId();
    prefs.putString("node_id", sid);
  }
  sid.toCharArray(nodeId, sizeof(nodeId));
  nodeId[sizeof(nodeId) - 1] = '\0';

  String ss = prefs.getString("ssid", DEFAULT_SSID);
  ss.toCharArray(apSsid, sizeof(apSsid));
  apSsid[sizeof(apSsid) - 1] = '\0';

  String sp = prefs.getString("password", DEFAULT_PASSWORD);
  sp.toCharArray(apPassword, sizeof(apPassword));
  apPassword[sizeof(apPassword) - 1] = '\0';

  wifiChannel = prefs.getUChar("channel", DEFAULT_CHANNEL);
  if (wifiChannel < 1 || wifiChannel > 13) wifiChannel = DEFAULT_CHANNEL;

  String au = prefs.getString("admin_user", DEFAULT_ADMIN_USER);
  au.toCharArray(adminUser, sizeof(adminUser));
  adminUser[sizeof(adminUser) - 1] = '\0';

  String apw = prefs.getString("admin_pass", DEFAULT_ADMIN_PASS);
  apw.toCharArray(adminPass, sizeof(adminPass));
  adminPass[sizeof(adminPass) - 1] = '\0';

  txPower = prefs.getChar("tx_power", DEFAULT_TX_POWER);

  discInterval = prefs.getUShort("disc_int", DEFAULT_DISC_INTERVAL);
  if (discInterval < 5) discInterval = 5;

  String pk = prefs.getString("pair_key", "");
  if (pk.length() == 0) {
    pk = DEFAULT_PAIRING_KEY;
  }
  pk.toCharArray(pairingKey, sizeof(pairingKey));
  pairingKey[sizeof(pairingKey) - 1] = '\0';
  deriveXorKey(pairingKey, pairingXorKey);

  
  peerCount = prefs.getUChar("peer_count", 0);
  if (peerCount > MAX_PEERS) peerCount = MAX_PEERS;
  for (byte i = 0; i < peerCount; i++) {
    String macStr = prefs.getString(("peer_mac_" + String(i)).c_str(), "");
    String idStr = prefs.getString(("peer_id_" + String(i)).c_str(), "");
    if (macStr.length() > 0 && idStr.length() > 0) {
      
      parseMac(macStr.c_str(), peers[i].mac);
      idStr.toCharArray(peers[i].nodeId, sizeof(peers[i].nodeId));
      peers[i].nodeId[sizeof(peers[i].nodeId) - 1] = '\0';
      peers[i].paired = true;
      peers[i].online = false;
      peers[i].lastSeen = 0;
      peers[i].lastSeq = 0;
    }
  }

  authFailCount = prefs.getUChar("auth_fails", 0);
  authLockoutUntil = prefs.getULong("auth_lockout", 0);
  if (authLockoutUntil != 0) {
    // Check if lockout has expired (using signed subtraction to handle wrap)
    if ((long)(millis() - authLockoutUntil) >= 0) {
      // Lockout expired
      authFailCount = 0;
    }
    // else still locked, keep authFailCount as read
  } else {
    // No lockout data stored
    authFailCount = 0;
  }

  prefs.end();
}

static void saveAuthState() {
  static unsigned long lastAuthSave = 0;
  if (millis() - lastAuthSave < 3000) return;
  lastAuthSave = millis();
  prefs.begin("mogchat", false);
  prefs.putUChar("auth_fails", authFailCount);
  prefs.putULong("auth_lockout", authLockoutUntil);
  prefs.end();
}

void saveConfig() {
  if (millis() - lastConfigSave < 3000) return;
  lastConfigSave = millis();
  prefs.begin("mogchat", false);
  prefs.putString("node_id", String(nodeId));
  prefs.putString("ssid", String(apSsid));
  prefs.putString("password", String(apPassword));
  prefs.putUChar("channel", wifiChannel);
  prefs.putString("admin_user", String(adminUser));
  prefs.putString("admin_pass", String(adminPass));
  prefs.putChar("tx_power", txPower);
  prefs.putUShort("disc_int", discInterval);
  prefs.putString("pair_key", String(pairingKey));

  // Save paired peers
  prefs.putUChar("peer_count", peerCount);
  for (byte i = 0; i < peerCount; i++) {
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
            peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
    prefs.putString(("peer_mac_" + String(i)).c_str(), String(macStr));
    prefs.putString(("peer_id_" + String(i)).c_str(), String(peers[i].nodeId));
  }

  prefs.end();
}

String generateNodeId() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char buf[16];
  sprintf(buf, "MOG-%02X%02X", mac[4], mac[5]);
  return String(buf);
}


bool checkAdminAuth() {
  if (!adminAuthEnabled) return true;

  
  if (authFailCount >= MAX_AUTH_FAILS && ((long)(millis() - authLockoutUntil) < 0)) {
    server.send(429, "text/plain", "Too many attempts. Try again later.");
    return false;
  }

  if (!server.hasHeader("Authorization")) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
  }

  String auth = server.header("Authorization");
  if (!auth.startsWith("Basic ")) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
  }

  String encoded = auth.substring(6);
  int input_len = encoded.length();
  if (input_len == 0) {
    authFailCount++;
    authLockoutUntil = millis() + ((unsigned long)AUTH_LOCKOUT_SECS * 1000);
    saveAuthState();
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
  }
  uint8_t buffer[96];
  size_t olen = 0;
  int ret = mbedtls_base64_decode(buffer, 100, &olen, (const unsigned char*)encoded.c_str(), input_len);
  if (ret != 0) {
    authFailCount++;
    authLockoutUntil = millis() + ((unsigned long)AUTH_LOCKOUT_SECS * 1000);
    saveAuthState();
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
  }
  buffer[olen] = '\0';
  String decoded = (char*)buffer;

  if (decoded.length() == 0) {
    authFailCount++;
    authLockoutUntil = millis() + ((unsigned long)AUTH_LOCKOUT_SECS * 1000);
    saveAuthState();
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
  }

  int colon = decoded.indexOf(':');
  if (colon < 0) {
    authFailCount++;
    authLockoutUntil = millis() + ((unsigned long)AUTH_LOCKOUT_SECS * 1000);
    saveAuthState();
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
  }

  String user = decoded.substring(0, colon);
  String pass = decoded.substring(colon + 1);

  if (user.equals(String(adminUser)) && pass.equals(String(adminPass))) {
    authFailCount = 0;
    authLockoutUntil = 0;
    return true;
  }

  authFailCount++;
  authLockoutUntil = millis() + AUTH_LOCKOUT_SECS * 1000;
  saveAuthState();
  server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Admin\"");
  server.send(401, "text/plain", "Unauthorized");
  return false;
}


void initESPNow() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  // Create queue for incoming packets
  pktQueue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(IncomingPacket));
  if (pktQueue == NULL) {
    Serial.println("Failed to create packet queue");
    return;
  }

  uint8_t pmk[16];
  deriveSaltedKey("pmk", pairingKey, pmk);
  esp_now_set_pmk(pmk);

  esp_now_register_send_cb([](const uint8_t *mac_addr, esp_now_send_status_t status) {
    // Optional: track send status
  });

  esp_now_register_recv_cb([](const uint8_t *mac, const uint8_t *data, int len) {
    IncomingPacket pkt;
    if (len > (int)sizeof(EspNowPacket)) len = sizeof(EspNowPacket);
    memcpy(pkt.mac, mac, 6);
    memcpy(&pkt.pkt, data, len);
    pkt.rssi = WiFi.RSSI();
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(pktQueue, &pkt, &higherPriorityTaskWoken) != pdTRUE) {
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
  });

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  peerInfo.channel = wifiChannel;
  peerInfo.ifidx = WIFI_IF_AP;
  peerInfo.encrypt = false;
  for (int i = 0; i < 6; i++) peerInfo.peer_addr[i] = 0xFF;
  esp_now_add_peer(&peerInfo);
}

void sendEspNowPacket(const uint8_t *destMac, uint8_t type, const uint8_t *data, uint16_t dataLen) {
  EspNowPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = type;
  strncpy(pkt.senderNodeId, nodeId, MAX_NODE_ID_LEN);
  pkt.senderNodeId[MAX_NODE_ID_LEN] = 0;
  pkt.seq = pktSeqCounter++;
  if (dataLen > 226) dataLen = 226;
  pkt.dataLen = dataLen;
  if (data != NULL && dataLen > 0) {
    memcpy(pkt.data, data, dataLen);
  }

  uint16_t totalLen = sizeof(pkt.type) + sizeof(pkt.senderNodeId) + sizeof(pkt.seq) + sizeof(pkt.dataLen) + dataLen;
  if (totalLen > 250) totalLen = 250;

  esp_now_send(destMac, (uint8_t *)&pkt, totalLen);
}

void broadcastDiscovery() {
  uint8_t data[12];
  uint32_t challenge = esp_random();
  memcpy(data, &challenge, 4);
  data[4] = wifiChannel;
  strncpy((char *)data + 5, VERSION, 7);

  uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  sendEspNowPacket(broadcast, PKT_DISCOVERY, data, sizeof(data));
}

void sendPairRequest(const uint8_t *mac, uint32_t theirChallenge, uint32_t myChallenge) {
  uint8_t data[8];
  uint32_t resp = theirChallenge ^ pairingXorKey;
  memcpy(data, &resp, 4);
  memcpy(data + 4, &myChallenge, 4);
  sendEspNowPacket(mac, PKT_PAIR_REQUEST, data, sizeof(data));
}

void sendPairAccept(const uint8_t *mac, uint32_t theirChallenge) {
  uint8_t data[4];
  uint32_t resp = theirChallenge ^ pairingXorKey;
  memcpy(data, &resp, 4);
  sendEspNowPacket(mac, PKT_PAIR_ACCEPT, data, sizeof(data));
}

void sendPairReject(const uint8_t *mac) {
  sendEspNowPacket(mac, PKT_PAIR_REJECT, NULL, 0);
}

void syncMessageToPeers(const char *user, const char *msg) {
  uint8_t data[sizeof(Message)];
  Message m;
  memset(&m, 0, sizeof(m));
  strncpy(m.nodeId, nodeId, MAX_NODE_ID_LEN);
  m.nodeId[MAX_NODE_ID_LEN] = 0;
  strncpy(m.user, user, MAX_USER_LEN);
  m.user[MAX_USER_LEN] = 0;
  strncpy(m.msg, msg, MAX_MSG_LEN_SYNC);
  m.msg[MAX_MSG_LEN_SYNC] = 0;
  m.time = millis();
  memcpy(data, &m, sizeof(m));

  for (byte i = 0; i < peerCount; i++) {
    if (peers[i].paired && peers[i].online) {
      sendEspNowPacket(peers[i].mac, PKT_MESSAGE, data, sizeof(Message));
    }
  }
}

void sendPing(const uint8_t *mac) {
  uint8_t data[4];
  uint32_t t = millis();
  memcpy(data, &t, 4);
  sendEspNowPacket(mac, PKT_PING, data, 4);
}

void sendPong(const uint8_t *mac) {
  uint8_t data[4];
  uint32_t t = millis();
  memcpy(data, &t, 4);
  sendEspNowPacket(mac, PKT_PONG, data, 4);
}


void processDiscovery(const uint8_t *mac, const EspNowPacket *pkt) {
  uint32_t challenge;
  memcpy(&challenge, pkt->data, 4);
  uint8_t channel = pkt->data[4];

  int idx = findDiscoveredByMac(mac);
  if (idx < 0) {
    idx = findFreeDiscoveredSlot();
    if (idx >= 0) {
      memcpy(discovered[idx].mac, mac, 6);
      discoveredCount++;
    }
  }
  if (idx >= 0) {
    strncpy(discovered[idx].nodeId, pkt->senderNodeId, MAX_NODE_ID_LEN);
    discovered[idx].nodeId[MAX_NODE_ID_LEN] = 0;
    discovered[idx].challenge = challenge;
    discovered[idx].lastSeen = millis();
    
    discovered[idx].rssi = WiFi.RSSI();
  }

  
  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0 && peers[peerIdx].paired) {
    peers[peerIdx].online = true;
    peers[peerIdx].lastSeen = millis();
    peers[peerIdx].challenge = challenge;
  }
}

void processPairRequest(const uint8_t *mac, const EspNowPacket *pkt) {
  uint32_t resp, theirChallenge;
  memcpy(&resp, pkt->data, 4);
  memcpy(&theirChallenge, pkt->data + 4, 4);

  
  int discIdx = findDiscoveredByMac(mac);
  if (discIdx < 0) return;

  
  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0 && peers[peerIdx].paired) {
    sendPairAccept(mac, theirChallenge);
    return;
  }

  // Add to peers as awaiting accept
  if (peerIdx < 0) {
    peerIdx = findFreePeerSlot();
    if (peerIdx < 0) return;
    memcpy(peers[peerIdx].mac, mac, 6);
    strncpy(peers[peerIdx].nodeId, pkt->senderNodeId, MAX_NODE_ID_LEN);
    peers[peerIdx].nodeId[MAX_NODE_ID_LEN] = 0;
    peerCount++;
  }

  peers[peerIdx].awaitingAccept = true;
  peers[peerIdx].challenge = theirChallenge;
  peers[peerIdx].online = true;
  peers[peerIdx].lastSeen = millis();
}

void processPairAccept(const uint8_t *mac, const EspNowPacket *pkt) {
  uint32_t resp;
  memcpy(&resp, pkt->data, 4);

  int peerIdx = findPeerByMac(mac);
  if (peerIdx < 0) return;

  
  uint32_t expected = peers[peerIdx].challenge ^ pairingXorKey;
  if (resp != expected) return;

  peers[peerIdx].paired = true;
  peers[peerIdx].awaitingAccept = false;
  peers[peerIdx].online = true;
  peers[peerIdx].lastSeen = millis();
  peers[peerIdx].lastSeq = 0;

  addEncryptedPeer(mac);

  saveConfig();
}

void processPairReject(const uint8_t *mac, const EspNowPacket *pkt) {
  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0 && !peers[peerIdx].paired) {
    removePeer(peerIdx);
  }
}

void processUnpair(const uint8_t *mac, const EspNowPacket *pkt) {
  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0) {
    removePeer(peerIdx);
    esp_now_del_peer(mac);
  }
}


static bool validUsername(const char *u) {
  size_t l = strlen(u);
  if (l < 1 || l > MAX_USER_LEN) return false;
  for (size_t i = 0; i < l; i++) {
    char c = u[i];
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

static bool validNodeId(const char *n) {
  size_t l = strlen(n);
  if (l < 1 || l > MAX_NODE_ID_LEN) return false;
  for (size_t i = 0; i < l; i++) {
    char c = n[i];
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

void processMessage(const uint8_t *mac, const EspNowPacket *pkt) {
  int peerIdx = findPeerByMac(mac);
  if (peerIdx < 0 || !peers[peerIdx].paired) {
    sendEspNowPacket(mac, PKT_UNPAIR, NULL, 0);
    return;
  }

  if (pkt->seq <= peers[peerIdx].lastSeq) {
    // If the difference is huge, it's likely a reboot, not a replay.
    // Reset the counter and accept the new packet.
    if (peers[peerIdx].lastSeq > 1000 && pkt->seq < 100) {
      peers[peerIdx].lastSeq = pkt->seq;
    } else {
      return; // Reject actual replayed packet
    }
  } else {
    peers[peerIdx].lastSeq = pkt->seq;
  }
  peers[peerIdx].online = true;
  peers[peerIdx].lastSeen = millis();

  if (pkt->dataLen >= sizeof(Message) && pkt->dataLen <= sizeof(pkt->data)) {
    Message m;
    memcpy(&m, pkt->data, sizeof(Message));
    m.nodeId[MAX_NODE_ID_LEN] = 0;
    m.user[MAX_USER_LEN] = 0;
    m.msg[MAX_MSG_LEN_SYNC] = 0;
    if (!validUsername(m.user) || !validNodeId(m.nodeId)) return;
    addMessage(m.nodeId, m.user, m.msg, m.time);
  }
}

void processPing(const uint8_t *mac, const EspNowPacket *pkt) {
  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0 && peers[peerIdx].paired) {
    peers[peerIdx].online = true;
    peers[peerIdx].lastSeen = millis();
    sendPong(mac);
  } else {
    sendEspNowPacket(mac, PKT_UNPAIR, NULL, 0);
  }
}

void processPong(const uint8_t *mac, const EspNowPacket *pkt) {
  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0 && peers[peerIdx].paired) {
    peers[peerIdx].online = true;
    peers[peerIdx].lastSeen = millis();
  } else {
    sendEspNowPacket(mac, PKT_UNPAIR, NULL, 0);
  }
}

void processPacket(const uint8_t *mac, const EspNowPacket *pkt) {
  
  if (strcmp(pkt->senderNodeId, nodeId) == 0) return;

  switch (pkt->type) {
    case PKT_DISCOVERY: processDiscovery(mac, pkt); break;
    case PKT_PAIR_REQUEST: processPairRequest(mac, pkt); break;
    case PKT_PAIR_ACCEPT: processPairAccept(mac, pkt); break;
    case PKT_PAIR_REJECT: processPairReject(mac, pkt); break;
    case PKT_MESSAGE: processMessage(mac, pkt); break;
    case PKT_PING: processPing(mac, pkt); break;
    case PKT_PONG: processPong(mac, pkt); break;
    case PKT_UNPAIR: processUnpair(mac, pkt); break;
  }
}

void processPacketQueue() {
  IncomingPacket pkt;
  while (xQueueReceive(pktQueue, &pkt, 0) == pdTRUE) {
    processPacket(pkt.mac, &pkt.pkt);
    int peerIdx = findPeerByMac(pkt.mac);
    if (peerIdx >= 0) peers[peerIdx].rssi = pkt.rssi;
  }
}

int findPeerByMac(const uint8_t *mac) {
  for (byte i = 0; i < peerCount; i++) {
    if (memcmp(peers[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

int findDiscoveredByMac(const uint8_t *mac) {
  for (byte i = 0; i < discoveredCount; i++) {
    if (memcmp(discovered[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

int findFreePeerSlot() {
  if (peerCount >= MAX_PEERS) return -1;
  return peerCount;
}

int findFreeDiscoveredSlot() {
  if (discoveredCount >= MAX_DISCOVERED) return -1;
  return discoveredCount;
}

void removePeer(int idx) {
  if (idx < 0 || idx >= peerCount) return;
  for (byte i = idx; i < peerCount - 1; i++) {
    memmove(&peers[i], &peers[i + 1], sizeof(Peer));
  }
  peerCount--;
  memset(&peers[peerCount], 0, sizeof(Peer));
  saveConfig();
}

void checkPeersOnline() {
  unsigned long now = millis();
  for (byte i = 0; i < peerCount; i++) {
    if (peers[i].paired && peers[i].online) {
      if (now - peers[i].lastSeen > discInterval * 2000) {
        peers[i].online = false;
      }
    }
  }
  // Clean up old discovered nodes
  for (byte i = 0; i < discoveredCount; i++) {
    if (now - discovered[i].lastSeen > discInterval * 4000) {
      for (byte j = i; j < discoveredCount - 1; j++) {
        memmove(&discovered[j], &discovered[j + 1], sizeof(DiscoveredNode));
      }
      discoveredCount--;
      i--;
    }
  }
}


void addMessage(const char *nodeId, const char *user, const char *msg, unsigned long time) {
  strncpy(messages[msgIndex].nodeId, nodeId, MAX_NODE_ID_LEN);
  messages[msgIndex].nodeId[MAX_NODE_ID_LEN] = 0;
  strncpy(messages[msgIndex].user, user, MAX_USER_LEN);
  messages[msgIndex].user[MAX_USER_LEN] = 0;
  strncpy(messages[msgIndex].msg, msg, MAX_MSG_LEN_SYNC);
  messages[msgIndex].msg[MAX_MSG_LEN_SYNC] = 0;
  messages[msgIndex].time = time;

  msgIndex = (msgIndex + 1) % MAX_MESSAGES;
  if (msgTotal < MAX_MESSAGES) msgTotal++;
}


String generateMessagesHtml() {
  String html;
  byte count = msgTotal < MAX_MESSAGES ? msgTotal : MAX_MESSAGES;
  byte start = msgTotal < MAX_MESSAGES ? 0 : msgIndex;

  for (byte i = 0; i < count; i++) {
    byte idx = (start + i) % MAX_MESSAGES;
    String messageHtml = "<div class=\"message\"><div class=\"message-header\">";
    messageHtml += "<span class=\"username\">";
    messageHtml += String(messages[idx].user);
    messageHtml += "</span>";

    if (strcmp(messages[idx].nodeId, nodeId) != 0 && strlen(messages[idx].nodeId) > 0) {
      messageHtml += "<span class=\"source-tag\">";
      messageHtml += String(messages[idx].nodeId);
      messageHtml += "</span>";
    }

    messageHtml += "</div><div class=\"message-content\">";

    String msg = String(messages[idx].msg);
    msg.replace("&", "&amp;");
    msg.replace("<", "&lt;");
    msg.replace(">", "&gt;");
    msg.replace("\"", "&quot;");
    messageHtml += msg;

    messageHtml += "</div></div>";
    html += messageHtml;
  }

  html += "<span id=\"_pc\" style=\"display:none\">";
  html += String(peerCount);
  html += "</span>";

  return html;
}

String navbarHtml() {
  return "<div class=\"navbar\"><a href=\"/\" class=\"nav-link\">Chat</a><a href=\"/settings\" class=\"nav-link\">Settings</a><a href=\"/peers\" class=\"nav-link\">Peers</a><a href=\"/admin\" class=\"nav-link\">Admin</a></div>";
}

String pageHeader(const char *title) {
  return "<!DOCTYPE html><html><head><title>" + String(title) + " - " + VERSION + "</title>"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<style>"
         "*{box-sizing:border-box}"
         "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;"
         "background:#36393f;color:#dcddde;margin:0;padding:0}"
         ".navbar{background:#2f3136;padding:8px 12px;border-bottom:1px solid #202225;"
         "display:flex;gap:8px;flex-wrap:wrap}"
         ".nav-link{color:#b9bbbe;text-decoration:none;padding:6px 12px;border-radius:4px;font-size:14px}"
         ".nav-link:hover{background:#40444b;color:#fff}"
         ".nav-link.active{background:#40444b;color:#fff}"
         ".container{max-width:800px;margin:0 auto;padding:16px}"
         "h1{color:#fff;font-size:20px;margin:0 0 16px 0}"
         ".card{background:#2f3136;border-radius:8px;padding:16px;margin-bottom:12px}"
         ".label{color:#72767d;font-size:11px;text-transform:uppercase;margin-bottom:4px}"
         ".value{color:#fff;font-size:15px;margin-bottom:12px}"
         ".btn{background:#5865f2;color:#fff;border:none;border-radius:4px;padding:8px 16px;"
         "cursor:pointer;font-size:13px;font-weight:500}"
         ".btn:hover{background:#4752c4}"
         ".btn-danger{background:#ed4245}"
         ".btn-danger:hover{background:#c03537}"
         ".btn-success{background:#3ba55d}"
         ".btn-success:hover{background:#2d7d46}"
         ".btn-small{padding:4px 10px;font-size:12px}"
         ".input-field{background:#40444b;border:1px solid #202225;border-radius:4px;"
         "color:#dcddde;padding:8px 12px;font-size:14px;width:100%;margin-bottom:8px}"
         ".input-field:focus{outline:none;border-color:#5865f2}"
         "select.input-field{cursor:pointer}"
         ".info-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
         ".msg-success{background:#3ba55d33;color:#3ba55d;padding:8px;border-radius:4px;margin-bottom:8px}"
         ".msg-error{background:#ed424533;color:#ed4245;padding:8px;border-radius:4px;margin-bottom:8px}"
         ".peer-item{display:flex;justify-content:space-between;align-items:center;"
         "padding:8px;background:#40444b;border-radius:4px;margin-bottom:6px}"
         ".peer-name{font-weight:500;color:#fff}"
         ".peer-status{font-size:11px;padding:2px 6px;border-radius:3px}"
         ".status-online{background:#3ba55d;color:#fff}"
         ".status-offline{background:#72767d;color:#fff}"
         ".status-pending{background:#faa61a;color:#fff}"
         ".peer-mac{color:#72767d;font-size:11px}"
         ".chat-container{max-width:800px;margin:0 auto;height:100vh;display:flex;flex-direction:column}"
         ".header{background:#2f3136;padding:12px;border-bottom:1px solid #202225;text-align:center}"
         ".header h1{margin:4px 0;font-size:18px}"
         ".header .sub{color:#72767d;font-size:11px}"
         ".chat-area{flex:1;overflow-y:auto;padding:12px;background:#36393f}"
         ".message{margin-bottom:10px;padding:6px 12px}"
         ".message:hover{background:#32353b;border-radius:4px}"
         ".message-header{display:flex;align-items:center;margin-bottom:2px;flex-wrap:wrap;gap:4px}"
         ".username{font-weight:500;color:#fff;font-size:14px}"
          ".source-tag{background:#5865f2;color:#fff;font-size:10px;padding:1px 5px;border-radius:3px}"
          ".message-content{color:#dcddde;word-wrap:break-word;font-size:14px;line-height:1.4}"
         ".input-area{background:#40444b;padding:12px;border-top:1px solid #202225}"
         ".input-row{display:flex;gap:8px}"
         "#messageInput{flex:1;background:#484c52;border:none;border-radius:6px;"
         "color:#dcddde;font-size:14px;padding:10px 12px;outline:none}"
         "#sendButton{background:#5865f2;color:#fff;border:none;border-radius:6px;"
         "padding:10px 20px;cursor:pointer;font-weight:500;font-size:14px}"
         "#sendButton:hover{background:#4752c4}#sendButton:disabled{opacity:0.6;cursor:not-allowed}"
         ".node-badge{display:inline-block;background:#202225;color:#b9bbbe;font-size:10px;"
         "padding:2px 8px;border-radius:10px;margin-top:4px}"
          "@media(max-width:600px){.info-grid{grid-template-columns:1fr}}"
          ".char-counter{color:#72767d;font-size:10px;margin-top:4px}"
          ".char-counter.warn{color:#faa61a}"
          ".char-counter.over{color:#ed4245}"
         "</style></head><body>";
}

String pageFooter() {
  return "</body></html>";
}


void handleRoot() {
  String html = pageHeader("Chat");
  html.reserve(8192);
  html += "<div class=\"chat-container\">";
  html += "<div class=\"header\">";
  html += "<h1>" + String(VERSION) + "</h1>";
  html += "<div class=\"sub\">" + String(nodeId) + " &middot; by MOG-Developing</div>";
  html += navbarHtml();
  html += "</div>";
  html += "<div class=\"chat-area\" id=\"chatArea\"></div>";
  html += "<div class=\"input-area\">";
  html += "<div class=\"input-row\">";
  html += "<input type=\"text\" id=\"messageInput\" placeholder=\"Send a message...\" maxlength=\"" + String(MAX_MSG_LEN_SYNC) + "\" oninput=\"updateCharCount()\">";
  html += "<button id=\"sendButton\">Send</button>";
  html += "</div>";
  html += "<div style=\"display:flex;justify-content:space-between;margin-top:4px\">";
  html += "<span class=\"char-counter\" id=\"charCount\">0/" + String(MAX_MSG_LEN_SYNC) + "</span>";
  html += "<span class=\"node-badge\">Connected to: " + String(nodeId) + "</span>";
  html += "<span class=\"node-badge\" id=\"peerCount\">Peers: 0</span>";
  html += "</div></div></div>";
  html += "<script>";
  html += "const chatArea=document.getElementById('chatArea'),msgInput=document.getElementById('messageInput'),";
  html += "sendBtn=document.getElementById('sendButton'),peerCountEl=document.getElementById('peerCount');";
  html += "let username='User'+Math.floor(Math.random()*10000),isSending=false;";
  html += "function updateCharCount(){const len=msgInput.value.length;const el=document.getElementById('charCount');";
  html += "el.textContent=len+'/"+String(MAX_MSG_LEN_SYNC)+"';";
  html += "el.className='char-counter'+(len>"+String(MAX_MSG_LEN_SYNC-20)+"?'.warn':'')+(len>="+String(MAX_MSG_LEN_SYNC)+"?'.over':'')}";
  html += "async function sendMessage(){";
  html += "if(isSending)return;const m=msgInput.value.trim();if(!m)return;";
  html += "isSending=true;sendBtn.disabled=true;sendBtn.textContent='Sending...';";
  html += "try{const fd=new FormData;fd.append('user',username);fd.append('message',m);";
  html += "const r=await fetch('/send',{method:'POST',body:fd});";
  html += "if(r.ok){msgInput.value='';await loadMessages()}}";
  html += "catch(e){console.error(e)}";
  html += "finally{isSending=false;sendBtn.disabled=false;sendBtn.textContent='Send'}}";
  html += "async function loadMessages(){";
  html += "try{const r=await fetch('/messages');if(r.ok){const m=await r.text();";
  html += "if(m&&m.trim()){chatArea.innerHTML=m;chatArea.scrollTop=chatArea.scrollHeight;";
  html += "const pc=document.getElementById('_pc');if(pc)peerCountEl.textContent='Peers: '+pc.textContent}}}";
  html += "catch(e){console.error(e)}}";
  html += "sendBtn.addEventListener('click',sendMessage);";
  html += "msgInput.addEventListener('keypress',e=>{if(e.key==='Enter')sendMessage()});";
  html += "setInterval(loadMessages,1500);";
  html += "loadMessages();updateCharCount();";
  html += "</script>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleMessages() {
  String html = generateMessagesHtml();
  server.send(200, "text/html", html);
}

void handleSend() {
  if (server.hasArg("user") && server.hasArg("message")) {
    String user = server.arg("user");
    String message = server.arg("message");

    // Validate
    if (user.length() < 1 || user.length() > MAX_USER_LEN) { server.send(400, "text/plain", "ERR_USER"); return; }
    for (size_t i = 0; i < user.length(); i++) {
      char c = user.charAt(i);
      if (!isalnum(c) && c != '_' && c != '-') { server.send(400, "text/plain", "ERR_USER"); return; }
    }
    if (message.length() < 1 || message.length() > MAX_MSG_LEN_LOCAL) { server.send(400, "text/plain", "ERR_MSG"); return; }

    int rlSlot = -1;
    int rlFree = -1;
    int rlOldest = -1;
    unsigned long oldestTime = ULONG_MAX;
    for (int i = 0; i < RATE_LIMIT_SLOTS; i++) {
      if (rateLimitSlots[i].user[0] == '\0') {
        if (rlFree < 0) rlFree = i;
      } else if (strcmp(rateLimitSlots[i].user, user.c_str()) == 0) {
        rlSlot = i;
        break;
      }
      // Track the oldest entry for LRU replacement (only consider used slots)
      if (rateLimitSlots[i].user[0] != '\0') {
        if (rateLimitSlots[i].lastTime < oldestTime) {
          oldestTime = rateLimitSlots[i].lastTime;
          rlOldest = i;
        }
      }
    }
    if (rlSlot < 0) {
      if (rlFree >= 0) {
        rlSlot = rlFree;
      } else {
        // Table full, use the oldest slot (LRU)
        rlSlot = rlOldest;
      }
    }
    if (rlSlot >= 0) {
      bool isExisting = (rateLimitSlots[rlSlot].user[0] != '\0' && strcmp(rateLimitSlots[rlSlot].user, user.c_str()) == 0);
      if (isExisting && millis() - rateLimitSlots[rlSlot].lastTime < 1000) {
        server.send(429, "text/plain", "ERR_RATE"); return;
      }
      strncpy(rateLimitSlots[rlSlot].user, user.c_str(), MAX_USER_LEN);
      rateLimitSlots[rlSlot].user[MAX_USER_LEN] = 0;
      rateLimitSlots[rlSlot].lastTime = millis();
    }

    String rawMsg = message;
    if (rawMsg.length() > MAX_MSG_LEN_SYNC) rawMsg = rawMsg.substring(0, MAX_MSG_LEN_SYNC);

    addMessage(nodeId, user.c_str(), rawMsg.c_str(), millis());
    syncMessageToPeers(user.c_str(), rawMsg.c_str());

    server.send(200, "text/plain", "OK");
    return;
  }
  server.send(400, "text/plain", "ERR_ARGS");
}

void handleSettings() {
  if (!checkAdminAuth()) return;

  String html = pageHeader("Settings");
  html.reserve(8192);
  html += "<div class=\"container\">";
  html += navbarHtml();
  html += "<h1>Settings</h1>";

  if (configChanged) {
    html += "<div class=\"msg-success\">Settings saved! <form action=\"/reboot\" method=\"post\" style=\"display:inline\"><input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\"><button class=\"btn\" type=\"submit\" style=\"color:#fff;background:transparent;border:none;cursor:pointer;text-decoration:underline;padding:0;font:inherit\">Reboot to apply</button></form></div>";
    configChanged = false;
  }

  html += "<form action=\"/settings/save\" method=\"post\">";
  html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
  html += "<div class=\"card\">";
  html += "<div class=\"label\">Node ID</div>";
  html += "<input class=\"input-field\" type=\"text\" name=\"node_id\" value=\"" + String(nodeId) + "\" maxlength=\"16\">";
  html += "<div class=\"label\">AP SSID</div>";
  html += "<input class=\"input-field\" type=\"text\" name=\"ssid\" value=\"" + String(apSsid) + "\" maxlength=\"31\">";
  html += "<div class=\"label\">AP Password</div>";
  html += "<input class=\"input-field\" type=\"password\" name=\"password\" value=\"" + String(apPassword) + "\" maxlength=\"63\">";
  html += "</div>";

  html += "<div class=\"card\">";
  html += "<div class=\"label\">WiFi Channel (1-13)</div>";
  html += "<select class=\"input-field\" name=\"channel\">";
  for (int ch = 1; ch <= 13; ch++) {
    html += "<option value=\"" + String(ch) + "\"" + (ch == wifiChannel ? " selected" : "") + ">" + String(ch) + "</option>";
  }
  html += "</select>";
  html += "<div class=\"label\">TX Power</div>";
  html += "<select class=\"input-field\" name=\"tx_power\">";
  int powerVals[] = {WIFI_POWER_2dBm, WIFI_POWER_7dBm, WIFI_POWER_11dBm, WIFI_POWER_13dBm, WIFI_POWER_17dBm, WIFI_POWER_19_5dBm};
  const char *powerLabels[] = {"2 dBm (low)", "7 dBm", "11 dBm", "13 dBm", "17 dBm", "19.5 dBm (max)"};
  for (int i = 0; i < 6; i++) {
    html += "<option value=\"" + String(powerVals[i]) + "\"" + (txPower == powerVals[i] ? " selected" : "") + ">" + String(powerLabels[i]) + "</option>";
  }
  html += "</select>";
  html += "<div class=\"label\">Discovery Interval (seconds)</div>";
  html += "<input class=\"input-field\" type=\"number\" name=\"disc_interval\" value=\"" + String(discInterval) + "\" min=\"5\" max=\"300\">";
  html += "</div>";

  html += "<div class=\"card\">";
  html += "<div class=\"label\">Admin Username</div>";
  html += "<input class=\"input-field\" type=\"text\" name=\"admin_user\" value=\"" + String(adminUser) + "\" maxlength=\"31\">";
  html += "<div class=\"label\">Admin Password</div>";
  html += "<input class=\"input-field\" type=\"password\" name=\"admin_pass\" value=\"" + String(adminPass) + "\" maxlength=\"63\">";
  html += "<div class=\"label\">Pairing Key (used for ESP-to-ESP auth)</div>";
  html += "<input class=\"input-field\" type=\"text\" name=\"pairing_key\" value=\"" + String(pairingKey) + "\" maxlength=\"63\">";
  html += "</div>";

  html += "<button class=\"btn\" type=\"submit\">Save Settings</button>";
  html += "</form>";
  html += "</div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleSettingsSave() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;

  if (server.hasArg("node_id")) {
    String val = server.arg("node_id");
    if (val.length() > 0 && val.length() <= MAX_NODE_ID_LEN && validNodeId(val.c_str())) {
      val.toCharArray(nodeId, sizeof(nodeId));
    } else {
      server.send(400, "text/plain", "Invalid Node ID");
      return;
    }
  }
  if (server.hasArg("ssid")) {
    server.arg("ssid").toCharArray(apSsid, sizeof(apSsid));
    apSsid[sizeof(apSsid) - 1] = '\0';
  }
  if (server.hasArg("password")) {
    server.arg("password").toCharArray(apPassword, sizeof(apPassword));
    apPassword[sizeof(apPassword) - 1] = '\0';
  }
  if (server.hasArg("channel")) {
    int ch = server.arg("channel").toInt();
    if (ch >= 1 && ch <= 13) wifiChannel = ch;
  }
  if (server.hasArg("tx_power")) {
    int p = server.arg("tx_power").toInt();
    int pwrVals[] = {WIFI_POWER_2dBm, WIFI_POWER_7dBm, WIFI_POWER_11dBm, WIFI_POWER_13dBm, WIFI_POWER_17dBm, WIFI_POWER_19_5dBm};
    for (int i = 0; i < 6; i++) {
      if (p == pwrVals[i]) { txPower = p; break; }
    }
  }
  if (server.hasArg("disc_interval")) {
    int di = server.arg("disc_interval").toInt();
    if (di >= 5 && di <= 300) discInterval = di;
  }
  if (server.hasArg("admin_user")) {
    server.arg("admin_user").toCharArray(adminUser, sizeof(adminUser));
    adminUser[sizeof(adminUser) - 1] = '\0';
  }
  if (server.hasArg("admin_pass")) {
    server.arg("admin_pass").toCharArray(adminPass, sizeof(adminPass));
    adminPass[sizeof(adminPass) - 1] = '\0';
  }
  if (server.hasArg("pairing_key")) {
    String pk = server.arg("pairing_key");
    if (pk.length() > 0) {
      pk.toCharArray(pairingKey, sizeof(pairingKey));
      pairingKey[sizeof(pairingKey) - 1] = '\0';
      deriveXorKey(pairingKey, pairingXorKey);
    }
  }

  saveConfig();
  configChanged = true;
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "");
}

void handleAdmin() {
  if (!checkAdminAuth()) return;

  String html = pageHeader("Admin");
  html.reserve(8192);
  html += "<div class=\"container\">";
  html += navbarHtml();
  html += "<h1>Admin Panel</h1>";

  html += "<div class=\"card\">";
  html += "<h1 style=\"font-size:16px;margin-bottom:12px\">System Info</h1>";
  html += "<div class=\"info-grid\">";
  html += "<div><div class=\"label\">Firmware</div><div class=\"value\">" + String(VERSION) + "</div></div>";
  html += "<div><div class=\"label\">Node ID</div><div class=\"value\">" + String(nodeId) + "</div></div>";
  html += "<div><div class=\"label\">SSID</div><div class=\"value\">" + String(apSsid) + "</div></div>";
  html += "<div><div class=\"label\">Channel</div><div class=\"value\">" + String(wifiChannel) + "</div></div>";
  html += "<div><div class=\"label\">AP IP</div><div class=\"value\">" + WiFi.softAPIP().toString() + "</div></div>";
  html += "<div><div class=\"label\">AP Clients</div><div class=\"value\">" + String(WiFi.softAPgetStationNum()) + "</div></div>";
  html += "<div><div class=\"label\">Paired Peers</div><div class=\"value\">" + String(peerCount) + "</div></div>";
  html += "<div><div class=\"label\">Messages</div><div class=\"value\">" + String(msgTotal) + "/" + String(MAX_MESSAGES) + "</div></div>";
  html += "<div><div class=\"label\">Uptime</div><div class=\"value\">" + String(millis() / 1000) + "s</div></div>";
  html += "<div><div class=\"label\">Free Heap</div><div class=\"value\">" + String(ESP.getFreeHeap()) + " bytes</div></div>";
  html += "</div></div>";

  html += "<div class=\"card\">";
  html += "<h1 style=\"font-size:16px;margin-bottom:12px\">Actions</h1>";
  html += "<form action=\"/clear\" method=\"post\" style=\"display:inline\">";
  html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
  html += "<button class=\"btn btn-danger\" type=\"submit\">Clear All Messages</button></form> ";
  html += "<form action=\"/reboot\" method=\"post\" style=\"display:inline\">";
  html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
  html += "<button class=\"btn\" type=\"submit\">Reboot ESP</button></form>";
  html += "</div>";

  html += "<div class=\"card\">";
  html += "<h1 style=\"font-size:16px;margin-bottom:12px\">Paired ESPs</h1>";
  if (peerCount == 0) {
    html += "<div class=\"value\">No paired peers. Go to <a href=\"/peers\" style=\"color:#5865f2\">Peers</a> to discover and pair.</div>";
  } else {
    for (byte i = 0; i < peerCount; i++) {
      html += "<div class=\"peer-item\">";
      html += "<div><div class=\"peer-name\">" + String(peers[i].nodeId) + "</div>";
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
              peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
              peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
      html += "<div class=\"peer-mac\">" + String(macStr) + "</div>";
      if (peers[i].online) {
        html += "<div style=\"color:#72767d;font-size:11px\">RSSI: " + String(peers[i].rssi) + " dBm";
        float dist = calculateDistance(peers[i].rssi);
        if (dist > 0) {
          html += " | ~" + String(dist, 1) + "m";
        }
        html += "</div>";
      }
      html += "</div><span class=\"peer-status " + String(peers[i].online ? "status-online" : "status-offline") + "\">" + String(peers[i].online ? "Online" : "Offline") + "</span>";
      html += "</div>";
    }
  }
  html += "</div></div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

float calculateDistance(int8_t rssi) {
  if (rssi == 0) return -1.0;
  return pow(10.0, (float)(-43 - rssi) / (10.0 * 2.0));
}

void handlePeers() {
  if (!checkAdminAuth()) return;

  String html = pageHeader("Peers");
  html.reserve(12288);
  html += "<div class=\"container\">";
  html += navbarHtml();
  html += "<h1>Peer Management</h1>";

  // Discovered nodes
  html += "<div class=\"card\">";
  html += "<h1 style=\"font-size:16px;margin-bottom:12px\">Discovered Nodes</h1>";
  if (discoveredCount == 0) {
    html += "<div class=\"value\">No ESPs discovered yet. Make sure other MOG-CHAT nodes are powered on and on the same channel.</div>";
  } else {
    for (byte i = 0; i < discoveredCount; i++) {
      // Skip if already paired
      if (findPeerByMac(discovered[i].mac) >= 0) continue;

      html += "<div class=\"peer-item\">";
      html += "<div><div class=\"peer-name\">" + String(discovered[i].nodeId) + "</div>";
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
              discovered[i].mac[0], discovered[i].mac[1], discovered[i].mac[2],
              discovered[i].mac[3], discovered[i].mac[4], discovered[i].mac[5]);
      html += "<div class=\"peer-mac\">" + String(macStr) + " | RSSI: " + String(discovered[i].rssi) + " dBm</div></div>";
      html += "<form action=\"/peers/pair\" method=\"post\" style=\"display:inline\">";
      html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
      html += "<input type=\"hidden\" name=\"mac\" value=\"" + String(macStr) + "\">";
      html += "<button class=\"btn btn-success btn-small\" type=\"submit\">Pair</button></form>";
      html += "</div>";
    }
  }
  html += "</div>";

  // Paired nodes
  html += "<div class=\"card\">";
  html += "<h1 style=\"font-size:16px;margin-bottom:12px\">Paired Nodes</h1>";
  if (peerCount == 0) {
    html += "<div class=\"value\">No paired nodes.</div>";
  } else {
    for (byte i = 0; i < peerCount; i++) {
      html += "<div class=\"peer-item\">";
      html += "<div><div class=\"peer-name\">" + String(peers[i].nodeId) + "</div>";
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
              peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
              peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
      html += "<div class=\"peer-mac\">" + String(macStr) + "";
      if (peers[i].online) {
        html += " | RSSI: " + String(peers[i].rssi) + " dBm";
        float dist = calculateDistance(peers[i].rssi);
        if (dist > 0) html += " | ~" + String(dist, 1) + "m";
      }
      html += "</div></div>";
      html += "<div style=\"display:flex;align-items:center;gap:8px\">";
      if (peers[i].awaitingAccept && !peers[i].paired) {
        html += "<span class=\"peer-status status-pending\">Pending</span>";
        html += "<form action=\"/peers/accept\" method=\"post\" style=\"display:inline\">";
        html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
        html += "<input type=\"hidden\" name=\"mac\" value=\"" + String(macStr) + "\">";
        html += "<button class=\"btn btn-success btn-small\" type=\"submit\">Accept</button></form>";
        html += "<form action=\"/peers/reject\" method=\"post\" style=\"display:inline\">";
        html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
        html += "<input type=\"hidden\" name=\"mac\" value=\"" + String(macStr) + "\">";
        html += "<button class=\"btn btn-danger btn-small\" type=\"submit\">Reject</button></form>";
      } else if (peers[i].paired) {
        html += "<span class=\"peer-status " + String(peers[i].online ? "status-online" : "status-offline") + "\">" + String(peers[i].online ? "Online" : "Offline") + "</span>";
        html += "<form action=\"/peers/unpair\" method=\"post\" style=\"display:inline\">";
        html += "<input type=\"hidden\" name=\"_csrf\" value=\"" + String(csrfToken) + "\">";
        html += "<input type=\"hidden\" name=\"mac\" value=\"" + String(macStr) + "\">";
        html += "<button class=\"btn btn-danger btn-small\" type=\"submit\">Unpair</button></form>";
      }
      html += "</div></div>";
    }
  }
  html += "</div></div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handlePeersPair() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;
  if (!server.hasArg("mac")) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  String macStr = server.arg("mac");
  uint8_t mac[6];
  if (!parseMac(macStr.c_str(), mac)) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  int discIdx = findDiscoveredByMac(mac);
  if (discIdx < 0) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  int peerIdx = findPeerByMac(mac);
  if (peerIdx < 0) {
    peerIdx = findFreePeerSlot();
    if (peerIdx < 0) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }
    memcpy(peers[peerIdx].mac, mac, 6);
    strncpy(peers[peerIdx].nodeId, discovered[discIdx].nodeId, MAX_NODE_ID_LEN);
    peers[peerIdx].nodeId[MAX_NODE_ID_LEN] = 0;
    peerCount++;
  }

  // Send pair request with our own challenge
  // Register peer as unencrypted so esp_now_send() can reach it
  esp_now_peer_info_t pairPeer;
  memset(&pairPeer, 0, sizeof(pairPeer));
  memcpy(pairPeer.peer_addr, mac, 6);
  pairPeer.channel = wifiChannel;
  pairPeer.ifidx = WIFI_IF_AP;
  pairPeer.encrypt = false;
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_add_peer(&pairPeer);
  }

  uint32_t myChallenge = esp_random();
  sendPairRequest(mac, discovered[discIdx].challenge, myChallenge);
  peers[peerIdx].awaitingAccept = true;
  peers[peerIdx].challenge = myChallenge; // Store OUR challenge so we can verify their response

  server.sendHeader("Location", "/peers");
  server.send(303, "", "");
}

void handlePeersAccept() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;
  if (!server.hasArg("mac")) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  String macStr = server.arg("mac");
  uint8_t mac[6];
  if (!parseMac(macStr.c_str(), mac)) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  int peerIdx = findPeerByMac(mac);
  if (peerIdx < 0) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  // Register peer for ESP-NOW before sending (will be upgraded to encrypted below)
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t acceptPeer;
    memset(&acceptPeer, 0, sizeof(acceptPeer));
    memcpy(acceptPeer.peer_addr, mac, 6);
    acceptPeer.channel = wifiChannel;
    acceptPeer.ifidx = WIFI_IF_AP;
    acceptPeer.encrypt = false;
    esp_now_add_peer(&acceptPeer);
  }

  sendPairAccept(mac, peers[peerIdx].challenge);
  peers[peerIdx].paired = true;
  peers[peerIdx].awaitingAccept = false;
  peers[peerIdx].online = true;
  peers[peerIdx].lastSeen = millis();
  peers[peerIdx].lastSeq = 0;

  addEncryptedPeer(mac);

  saveConfig();
  server.sendHeader("Location", "/peers");
  server.send(303, "", "");
}

void handlePeersReject() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;
  if (!server.hasArg("mac")) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  String macStr = server.arg("mac");
  uint8_t mac[6];
  if (!parseMac(macStr.c_str(), mac)) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  sendPairReject(mac);

  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0) removePeer(peerIdx);

  server.sendHeader("Location", "/peers");
  server.send(303, "", "");
}

void handlePeersUnpair() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;
  if (!server.hasArg("mac")) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  String macStr = server.arg("mac");
  uint8_t mac[6];
  if (!parseMac(macStr.c_str(), mac)) { server.sendHeader("Location", "/peers"); server.send(303, "", ""); return; }

  for (int r = 0; r < 5; r++) {
    sendEspNowPacket(mac, PKT_UNPAIR, NULL, 0);
    delay(50);
  }

  int peerIdx = findPeerByMac(mac);
  if (peerIdx >= 0) removePeer(peerIdx);
  esp_now_del_peer(mac);

  server.sendHeader("Location", "/peers");
  server.send(303, "", "");
}

void handleClearMessages() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;
  msgIndex = 0;
  msgTotal = 0;
  memset(messages, 0, sizeof(messages));
  server.sendHeader("Location", "/admin");
  server.send(303, "", "");
}

void handleReboot() {
  if (!checkAdminAuth()) return;
  if (!checkCsrf()) return;
  String html = pageHeader("Rebooting");
  html += "<div class=\"container\" style=\"text-align:center;padding-top:40px\">";
  html += "<h1>Rebooting...</h1>";
  html += "<div class=\"value\">The ESP32 will restart in 2 seconds.</div>";
  html += "</div>";
  html += pageFooter();
  server.send(200, "text/html", html);
  server.handleClient();
  delay(2000);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/plain", "404 Not Found");
}


void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(VERSION);
  Serial.println("by MOG-Developing");
  Serial.println("Booting...");

  
  loadConfig();
  Serial.print("Node ID: ");
  Serial.println(nodeId);
  Serial.print("Channel: ");
  Serial.println(wifiChannel);

  
  
#if ANTENNA_GPIO >= 0
  gpio_reset_pin((gpio_num_t)ANTENNA_GPIO);
  gpio_set_direction((gpio_num_t)ANTENNA_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)ANTENNA_GPIO, ANTENNA_SELECT);
  Serial.print("Antenna GPIO ");
  Serial.print(ANTENNA_GPIO);
  Serial.println(ANTENNA_SELECT ? " -> IPEX" : " -> PCB");
#endif

  WiFi.setSleep(false);
  WiFi.setTxPower((wifi_power_t)txPower);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, apPassword, wifiChannel, 0, 4);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("AP SSID: ");
  Serial.println(apSsid);

  
  initESPNow();
  Serial.println("ESP-NOW initialized");

  generateCsrfToken();

  // Collect Authorization header for auth checks
  const char* headerKeys[] = {"Authorization"};
  server.collectHeaders(headerKeys, 1);


  server.on("/", handleRoot);
  server.on("/messages", handleMessages);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/settings", handleSettings);
  server.on("/settings/save", HTTP_POST, handleSettingsSave);
  server.on("/admin", handleAdmin);
  server.on("/peers", handlePeers);
  server.on("/peers/pair", HTTP_POST, handlePeersPair);
  server.on("/peers/accept", HTTP_POST, handlePeersAccept);
  server.on("/peers/reject", HTTP_POST, handlePeersReject);
  server.on("/peers/unpair", HTTP_POST, handlePeersUnpair);
  server.on("/clear", HTTP_POST, handleClearMessages);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");

  bootTime = millis();
  lastDiscoveryTime = millis();
  lastPeerCheckTime = millis();

  Serial.println("Ready!");
  Serial.print("Connect to WiFi: ");
  Serial.print(apSsid);
  Serial.print(" then visit: http://");
  Serial.println(WiFi.softAPIP().toString());
}


void loop() {
  server.handleClient();
  processPacketQueue();

  unsigned long now = millis();

  
  if (now - lastDiscoveryTime > discInterval * 1000) {
    broadcastDiscovery();
    lastDiscoveryTime = now;
  }

  
  if (now - lastPeerCheckTime > 10000) {
    checkPeersOnline();
    
    for (byte i = 0; i < peerCount; i++) {
      if (peers[i].paired) {
        sendPing(peers[i].mac);
      }
    }
    lastPeerCheckTime = now;
  }

  yield(); // Give ESP32 some breathing room
}
