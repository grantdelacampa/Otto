#include "ble.h"
#include "config.h"
#include "settings.h"
#include "brain.h"
#include "audio.h"
#include "recorder.h"
#include "ota.h"
#include "commands.h"
#include <NimBLEDevice.h>
#include <Preferences.h>

void handleCommand(const char* line, uint16_t source);  // commands.cpp

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

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    s->updateConnParams(d->conn_handle, 24, 48, 0, 400);
    s_advMode = -2;                    // NimBLE stopped advertising: let updateAdvertising() decide
  }
  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    (void)s; (void)d;
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
  s_log = svc->createCharacteristic(UUID_LOG, NIMBLE_PROPERTY::NOTIFY, 240);
  NimBLECharacteristic* ota = svc->createCharacteristic(UUID_OTA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 512);
  ota->setCallbacks(new OtaCB());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_SERVICE);
  adv->setScanResponse(true);
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
  s_server = nullptr; s_tel = s_log = s_cmd = s_cfg = nullptr;
}

bool bleAnyConnected() { return s_server && s_server->getConnectedCount() > 0; }

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
    adv->start();
  }
  s_advMode = want;
}

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
  updateAdvertising(now);
}
