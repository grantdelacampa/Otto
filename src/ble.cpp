#include "ble.h"
#include "config.h"
#include "settings.h"
#include "brain.h"
#include "audio.h"
#include "recorder.h"
#include "ota.h"
#include "commands.h"
#include "board.h"
#include "power.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

void handleCommand(const char* line, uint16_t source);  // commands.cpp
static NimBLEHIDDevice* s_hid = nullptr;

static NimBLEServer* s_server = nullptr;
static NimBLECharacteristic *s_cmd, *s_cfg, *s_tel, *s_log;
static QueueHandle_t s_q;

struct CmdMsg { uint16_t conn; char text[240]; };

static const int MAXC = 4;
// Advertise fast for 30 s after a wake / disconnect / menu open (quick to find), slowly otherwise.
// Advertising itself was too small to measure (pwrtest, Nov 19 2026). g_set.wirelessOn = false:
// never advertise (and no stack at all unless OBD needs it).
static const uint32_t ADV_WINDOW_MS = 30000;
static volatile uint32_t s_advUntil = 0;
static volatile int s_advMode = -2;   // what's running: 0 off, 1 slow, 2 fast (-2 = unknown, recheck)
static int s_testAdv = -1;            // pwrtest: -1 normal, else force 0 off / 1 slow / 2 fast
static bool s_inited = false;

// log streaming state
static bool s_streaming = false;
static size_t s_logPos = 0;

static void enqueue(uint16_t conn, const std::string& v) {
  CmdMsg m;
  m.conn = conn;
  size_t n = min(v.size(), sizeof(m.text) - 1);
  memcpy(m.text, v.data(), n);
  m.text[n] = 0;
  xQueueSend(s_q, &m, 0);
}

static void phoneConnected(ble_gap_conn_desc* d); // phone clock (below)
static void phoneDisconnected(uint16_t conn);
static void phoneEncrypted(ble_gap_conn_desc* d);
static void phoneSlow(uint16_t conn);

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    s->updateConnParams(d->conn_handle, 24, 48, 0, 400);
    s_advMode = -2;                    // NimBLE stopped advertising: let updateAdvertising() decide
    phoneConnected(d);
  }
  void onAuthenticationComplete(ble_gap_conn_desc* d) override { phoneEncrypted(d); }
  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    (void)s;
    phoneDisconnected(d->conn_handle);
    s_advUntil = millis() + ADV_WINDOW_MS;   // a moment to reconnect (page reload, out of range)
    s_advMode = -2;
  }
};

// OTA control is handled right here in the Bluetooth task so image data can't
// arrive before "ota begin" has been processed.
static bool handleOtaCmd(const std::string& s) {
  if (s.rfind("ota ", 0) != 0) return false;
  if (s.rfind("ota begin ", 0) == 0) {
    size_t size = strtoul(s.c_str() + 10, nullptr, 10);
    bleReply(0, otaBegin(size) ? "ota ready" : (std::string("ota fail ") + otaError()).c_str());
  } else if (s == "ota end") {
    bleReply(0, otaEnd() ? "ota ok" : (std::string("ota fail ") + otaError()).c_str());
  } else if (s == "ota abort") {
    otaAbort();
    bleReply(0, "ota aborted");
  }
  return true;
}

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* d) override {
    NimBLEAttValue v = c->getValue();
    std::string s((const char*)v.data(), v.length());
    if (handleOtaCmd(s)) return;
    enqueue(d->conn_handle, s);
  }
};

class OtaCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc*) override {
    NimBLEAttValue v = c->getValue();
    otaWrite(v.data(), v.length());
  }
};

class CfgCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, ble_gap_conn_desc*) override {
    String j = settingsToJson();
    c->setValue((const uint8_t*)j.c_str(), j.length());
  }
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* d) override {
    NimBLEAttValue v = c->getValue();
    enqueue(d->conn_handle, "cfg " + std::string((const char*)v.data(), v.length()));
  }
};

// Tuning page subscribed: a quick link (live data, OTA). A phone that only lent its clock idles slowly.
class TelCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, ble_gap_conn_desc* d, uint16_t sub) override {
    (void)c;
    if (sub) NimBLEDevice::getServer()->updateConnParams(d->conn_handle, 24, 48, 0, 400);
    else phoneSlow(d->conn_handle);
  }
};


void bleInit() {
  if (s_inited) return;
  // Nothing needs the radio: don't start the stack (bleUpdate starts it if a setting turns on)
  if (!g_set.wirelessOn && !g_set.obdOn) { Serial.println("ble: off (wireless off)"); return; }
  s_inited = true;
  s_q = xQueueCreate(8, sizeof(CmdMsg));
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCB());
  NimBLEService* svc = s_server->createService(UUID_SERVICE);
  s_cmd = svc->createCharacteristic(UUID_CMD, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 240);
  s_cmd->setCallbacks(new CmdCB());
  s_cfg = svc->createCharacteristic(UUID_CONFIG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 512);
  s_cfg->setCallbacks(new CfgCB());
  s_tel = svc->createCharacteristic(UUID_TELEM, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ, 240);
  s_tel->setCallbacks(new TelCB());
  s_log = svc->createCharacteristic(UUID_LOG, NIMBLE_PROPERTY::NOTIFY, 240);
  NimBLECharacteristic* ota = svc->createCharacteristic(UUID_OTA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 512);
  ota->setCallbacks(new OtaCB());
  svc->start();

  // Media-remote face for iOS (owner, Nov 22 2026): iOS's Settings > Bluetooth lists only devices
  // offering a service it uses itself (keyboards, mice, game controllers, remotes); a plain BLE
  // gadget never shows. So Otto also offers HID as a consumer-control remote (volume up / down
  // bits, no keyboard keys) that NEVER sends a report: it only gets him listed, paired (the input
  // report needs encryption) and reconnected by iOS on its own. The battery service reports his cell.
  s_hid = new NimBLEHIDDevice(s_server);
  s_hid->manufacturer("OTTO");
  s_hid->pnp(0x02, 0xE420, 0x0210, 0x0100);       // USB-IF style ids: made-up (no real vendor)
  s_hid->hidInfo(0x00, 0x00);
  static uint8_t kRemoteMap[] = {
    0x05, 0x0C,        // usage page: consumer
    0x09, 0x01,        // usage: consumer control
    0xA1, 0x01,        // collection (application)
    0x85, 0x01,        //   report id 1
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
    0x09, 0xE9,        //   volume up
    0x09, 0xEA,        //   volume down
    0x81, 0x02,        //   input (data, var, abs)
    0x95, 0x06, 0x81, 0x03,   // 6 bits padding
    0xC0};
  s_hid->reportMap(kRemoteMap, sizeof(kRemoteMap));
  s_hid->inputReport(1);
  s_hid->startServices();
  s_hid->setBatteryLevel((uint8_t)constrain(powerPct(), 0, 100));

  // Phone clock: bond (Just Works, no PIN: Otto has no keypad) so an iPhone can share its time.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  // Advertising (setAdvData): flags, appearance "remote control", the HID service and a Current Time
  // Service solicitation; name + Otto's service UUID go in the scan response (31-byte limit), so the
  // tuning page's filter (service or "OTTO") still sees both.
  // (set again before every start in updateAdvertising: see setAdvData)
  s_advUntil = millis() + ADV_WINDOW_MS;
  s_advMode = -2;
}

bool bleReady() { return s_inited; }
void bleOpenWindow() { s_advUntil = millis() + ADV_WINDOW_MS; }
void bleTestAdvertising(int mode) { s_testAdv = mode; s_advMode = -2; }
bool bleAdvertising() { return s_inited && NimBLEDevice::getAdvertising()->isAdvertising(); }

void bleShutdown() {                  // pwrtest only: the whole stack off (a restart brings it back)
  if (!s_inited) return;
  NimBLEDevice::deinit(true);
  s_inited = false;
  s_server = nullptr; s_tel = s_log = s_cmd = s_cfg = nullptr; s_hid = nullptr;
}

bool bleAnyConnected() { return s_server && s_server->getConnectedCount() > 0; }
// The tuning page subscribes to telemetry; a paired phone that only lends its clock doesn't, and
// must not keep Otto awake (awake ~250 mA would flatten the 500 mAh cell in ~2 h).
bool bleTunerConnected() { return s_tel && s_tel->getSubscribedCount() > 0; }

void bleNotifyTelemetry(const char* json) {
  if (!s_tel || s_tel->getSubscribedCount() == 0) return;
  s_tel->setValue((const uint8_t*)json, strlen(json));
  s_tel->notify();
}

void bleReply(uint16_t conn, const char* msg) {
  (void)conn;
  if (msg[0] == '{') { bleNotifyTelemetry(msg); return; }  // already JSON
  char b[200];
  snprintf(b, sizeof(b), "{\"msg\":\"%s\"}", msg);
  bleNotifyTelemetry(b);
}

void bleStartLogStream() {
  if (!s_log || s_log->getSubscribedCount() == 0) return;
  s_streaming = true;
  s_logPos = 0;
  uint8_t hdr[8] = {'L', 'O', 'G', 1};
  uint32_t n = recorderCount();
  memcpy(hdr + 4, &n, 4);
  s_log->setValue(hdr, 8);
  s_log->notify();
}

static void streamLog() {
  const size_t PER = 18;  // 18 x 10 bytes = 180 bytes per notification
  for (int k = 0; k < 3 && s_streaming; k++) {
    size_t n = recorderCount();
    if (s_logPos >= n) {
      s_log->setValue((const uint8_t*)"END", 3);
      s_log->notify();
      s_streaming = false;
      return;
    }
    LogRec buf[PER];
    size_t m = 0;
    while (m < PER && s_logPos < n) buf[m++] = recorderGet(s_logPos++);
    s_log->setValue((const uint8_t*)buf, m * sizeof(LogRec));
    s_log->notify();
  }
}

// Raw payloads, handed to the stack right before each start: set once in bleInit they never went
// out (Nov 22 2026: no advertising at all; the host may not have been synced yet, and a host reset
// drops them).
static int s_advRc = 0, s_rspRc = 0;
static bool s_advStartOk = false;
static void setAdvData() {
  // (the ANCS solicitation didn't get Otto listed on iOS and no longer fits next to HID: dropped)
  static const uint8_t ad[] = {
    2, 0x01, BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,                       // flags
    3, 0x19, 0x80, 0x01,                                                              // appearance: remote control
    3, 0x03, 0x12, 0x18,                                                              // services: HID 0x1812
    3, 0x14, 0x05, 0x18};                                                             // solicit CTS 0x1805
  uint8_t sr[31]; size_t n = 0;
  const size_t nl = strlen(BLE_NAME);
  sr[n++] = nl + 1; sr[n++] = 0x09; memcpy(sr + n, BLE_NAME, nl); n += nl;          // complete name
  NimBLEUUID u(UUID_SERVICE);
  const ble_uuid128_t* u128 = (const ble_uuid128_t*)u.getNative();
  sr[n++] = 17; sr[n++] = 0x07; memcpy(sr + n, u128->value, 16); n += 16;            // Otto's service
  // through NimBLE's setters (they mark the data as custom, or start() builds its own over it);
  // the raw calls first only to learn the stack's return codes
  s_advRc = ble_gap_adv_set_data(ad, sizeof(ad));
  s_rspRc = ble_gap_adv_rsp_set_data(sr, n);
  NimBLEAdvertisementData a, r;
  a.addData((char*)ad, sizeof(ad));
  r.addData((char*)sr, n);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(a);
  adv->setScanResponseData(r);
}
bool bleAdvertisingOk() { return s_advStartOk && !s_advRc && !s_rspRc; }
int bleAdvRc(int which) { return which ? s_rspRc : s_advRc; }

static void updateAdvertising(uint32_t now) {
  int want;
  if (s_testAdv >= 0) want = s_testAdv;
  else {
    bool window = (int32_t)(s_advUntil - now) > 0;
    // a second central (tuning page + OTA tab) can still join inside the window
    // slow advertising (every 1-2 s) outside the window: pwrtest couldn't see it, and the page
    // can still find him any time (owner, Nov 19 2026)
    want = !g_set.wirelessOn || s_server->getConnectedCount() >= MAXC - 1 ? 0 : window ? 2 : 1;
  }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  bool on = adv->isAdvertising();
  if (want == s_advMode && (want > 0) == on) return;
  if (on) adv->stop();
  if (want > 0) {
    static const uint16_t mins[] = {0, 1600, 32}, maxs[] = {0, 3200, 64};  // x 0.625 ms
    adv->setMinInterval(mins[want]);
    adv->setMaxInterval(maxs[want]);
    setAdvData();
    s_advStartOk = adv->start();
    if (!s_advStartOk || s_advRc || s_rspRc)
      Serial.printf("ble: advertising start %s (data rc %d, scan response rc %d)\n", s_advStartOk ? "ok" : "FAILED", s_advRc, s_rspRc);
  }
  s_advMode = want;
}

// ------------------------------------------------------------ phone clock --
// A paired iPhone (or any phone with the standard Current Time Service, 0x1805) lends Otto its
// clock each time it connects: the RTC has no backup cell, so every power-off loses the time.
// Otto is the GATT *client* on the phone's own connection (raw NimBLE calls: NimBLEClient can't
// take over a connection the phone opened). iOS only answers once the link is encrypted: the first
// read fails with "insufficient authentication", Otto asks to pair (iPhone: "Pair" prompt the first
// time, silent after). Otto is not an audio device: the S3 only does Bluetooth LE.
enum class Pc : uint8_t { None, Wait, Disc, Chrs, ReadTz, ReadTime, Secure, Done };
static volatile Pc s_pc = Pc::None;
static volatile uint16_t s_pcConn = 0xFFFF;
static uint32_t s_pcAt = 0;
static uint16_t s_pcStart, s_pcEnd, s_pcTimeH, s_pcTzH;
static bool s_pcSecured = false;
static int8_t s_pcTz = -128;
static uint8_t s_pcDst = 255;
static uint8_t s_pcRaw[10];
static volatile bool s_pcGot = false;          // a reading for bleUpdate (main loop) to apply
static char s_pcLast[64] = "phone: no reading yet";

static bool s_pcPrivate = false;               // peer uses a private (resolvable) address: phones do
static void phoneConnected(ble_gap_conn_desc* d) {
  if (s_pc != Pc::None && s_pc != Pc::Done) return;   // one at a time
  s_pcConn = d->conn_handle;
  s_pcPrivate = d->peer_ota_addr.type == BLE_ADDR_RANDOM && (d->peer_ota_addr.val[5] & 0xC0) == 0x40 &&
                !d->sec_state.encrypted; s_pcSecured = false; s_pcTimeH = s_pcTzH = 0;
  s_pcAt = millis() + 1500;                    // let the phone finish its own discovery first
  s_pc = Pc::Wait;
}
static void phoneDisconnected(uint16_t conn) {
  if (conn == s_pcConn) { s_pc = Pc::None; s_pcConn = 0xFFFF; }
}
static void phoneSlow(uint16_t conn) {          // idle link: 0.5-1 s interval (6 s timeout)
  NimBLEServer* sv = NimBLEDevice::getServer();
  if (sv && conn != 0xFFFF) sv->updateConnParams(conn, 400, 800, 0, 600);
}
static void phoneFinish(const char* why) {
  snprintf(s_pcLast, sizeof(s_pcLast), "phone: %s", why);
  s_pc = Pc::Done;
  if (!bleTunerConnected()) phoneSlow(s_pcConn);
}
static void phoneEncrypted(ble_gap_conn_desc* d) {
  if (d->conn_handle != s_pcConn || s_pc != Pc::Secure) return;
  if (d->sec_state.encrypted) { s_pcAt = millis() + 300; s_pc = Pc::Wait; }   // handles known: read again
  else phoneFinish("pairing failed");
}
static void phoneSecure() {
  if (s_pcSecured) { phoneFinish("time refused"); return; }
  s_pcSecured = true; s_pc = Pc::Secure;
  if (NimBLEDevice::startSecurity(s_pcConn) != 0) phoneFinish("pairing refused");
}
static bool authErr(int st) {
  return st == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_AUTHEN ||
         st == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_ENC;
}

static int onTime(uint16_t conn, const ble_gatt_error* e, ble_gatt_attr* a, void*) {
  if (conn != s_pcConn) return 0;
  if (e->status == 0 && a && OS_MBUF_PKTLEN(a->om) >= 7) {
    memset(s_pcRaw, 0, sizeof(s_pcRaw));
    os_mbuf_copydata(a->om, 0, min((int)OS_MBUF_PKTLEN(a->om), 10), s_pcRaw);
    s_pcGot = true;
    phoneFinish("time read");
  } else if (authErr(e->status)) phoneSecure();
  else phoneFinish("time read failed");
  return 0;
}
static void readTime() {
  s_pc = Pc::ReadTime;
  if (ble_gattc_read(s_pcConn, s_pcTimeH, onTime, nullptr) != 0) phoneFinish("time read failed");
}
static int onTz(uint16_t conn, const ble_gatt_error* e, ble_gatt_attr* a, void*) {
  if (conn != s_pcConn) return 0;
  if (e->status == 0 && a && OS_MBUF_PKTLEN(a->om) >= 2) {
    uint8_t b[2];
    os_mbuf_copydata(a->om, 0, 2, b);
    s_pcTz = (int8_t)b[0]; s_pcDst = b[1];
  } else if (authErr(e->status)) { phoneSecure(); return 0; }
  readTime();
  return 0;
}
static void readNext() {
  if (!s_pcTimeH) { phoneFinish("no clock on this device"); return; }
  if (s_pcTzH) {
    s_pc = Pc::ReadTz; s_pcTz = -128; s_pcDst = 255;
    if (ble_gattc_read(s_pcConn, s_pcTzH, onTz, nullptr) != 0) readTime();
  } else readTime();
}
static int onChr(uint16_t conn, const ble_gatt_error* e, const ble_gatt_chr* c, void*) {
  if (conn != s_pcConn) return 0;
  if (e->status == 0 && c) {
    uint16_t u = ble_uuid_u16(&c->uuid.u);
    if (u == 0x2A2B) s_pcTimeH = c->val_handle;       // Current Time
    if (u == 0x2A0F) s_pcTzH = c->val_handle;         // Local Time Information
  } else if (e->status == BLE_HS_EDONE) readNext();
  else phoneFinish("discovery failed");
  return 0;
}
static int onSvc(uint16_t conn, const ble_gatt_error* e, const ble_gatt_svc* sv, void*) {
  if (conn != s_pcConn) return 0;
  if (e->status == 0 && sv) { s_pcStart = sv->start_handle; s_pcEnd = sv->end_handle; s_pc = Pc::Chrs; }
  else if (e->status == BLE_HS_EDONE || e->status == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_ATTR_NOT_FOUND) {
    if (s_pc != Pc::Chrs) {
      // iOS may show its clock only to a paired device: ask a phone-like peer once, then look again.
      // PCs (tuning page) use a fixed address and never get a pairing prompt.
      if (s_pcPrivate && !s_pcSecured) phoneSecure();
      else phoneFinish("no clock on this device");
      return 0;
    }
    if (ble_gattc_disc_all_chrs(s_pcConn, s_pcStart, s_pcEnd, onChr, nullptr) != 0) phoneFinish("discovery failed");
  } else phoneFinish("discovery failed");
  return 0;
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {   // days since 1970-01-01 (Hinnant)
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void phoneApply() {                     // main loop: the RTC shares the I2C bus
  s_pcGot = false;
  const uint8_t* r = s_pcRaw;
  int year = r[0] | (r[1] << 8), mon = r[2], day = r[3], hh = r[4], mm = r[5], ss = r[6];
  if (year < 2024 || mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 59) {
    snprintf(s_pcLast, sizeof(s_pcLast), "phone: bad time %d-%d-%d", year, mon, day);
    return;
  }
  int tz = tzOffsetMin();
  if (s_pcTz != -128) tz = (s_pcTz + (s_pcDst != 255 ? s_pcDst : 0)) * 15;   // both in 15 min steps
  int64_t local = daysFromCivil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
  time_t utc = (time_t)(local - (int64_t)tz * 60);
  time_t before = rtcNowUtc();
  rtcSetUtc(utc);
  if (tz != tzOffsetMin()) setTzOffsetMin(tz);
  snprintf(s_pcLast, sizeof(s_pcLast), "phone: %04d-%02d-%02d %02d:%02d:%02d tz %+d min (clock was %s)",
           year, mon, day, hh, mm, ss, tz, before ? "set" : "unset");
  Serial.println(s_pcLast);
}

static void phoneUpdate(uint32_t now) {
  if (s_pcGot) phoneApply();
  if (s_pc == Pc::Wait && (int32_t)(now - s_pcAt) >= 0) {
    if (s_pcTimeH) { readNext(); return; }      // after pairing: handles already found
    s_pc = Pc::Disc;
    static const ble_uuid16_t cts = BLE_UUID16_INIT(0x1805);
    if (ble_gattc_disc_svc_by_uuid(s_pcConn, &cts.u, onSvc, nullptr) != 0) phoneFinish("discovery failed");
  }
}

const char* blePhoneStatus() { return s_pcLast; }

static void dropPeers() {             // wireless switched off: let go of the tuning page too
  if (!s_server) return;
  for (uint16_t h : s_server->getPeerDevices()) s_server->disconnect(h);
}

void bleUpdate(uint32_t now) {
  if (!s_inited) {
    if (s_testAdv < 0 && (g_set.wirelessOn || g_set.obdOn)) bleInit();   // turned on in the menu
    if (!s_inited) return;
  }
  static bool s_wasOn = g_set.wirelessOn;
  if (s_wasOn != g_set.wirelessOn) {
    s_wasOn = g_set.wirelessOn;
    if (!s_wasOn) dropPeers(); else bleOpenWindow();
  }
  CmdMsg m;
  while (xQueueReceive(s_q, &m, 0) == pdTRUE) handleCommand(m.text, m.conn);
  if (s_streaming) streamLog();
  phoneUpdate(now);
  static uint32_t s_batAt = 0;                   // the remote's battery level (iOS battery widget)
  if (s_hid && now - s_batAt > 60000) { s_batAt = now; s_hid->setBatteryLevel((uint8_t)constrain(powerPct(), 0, 100)); }
  updateAdvertising(now);
}
