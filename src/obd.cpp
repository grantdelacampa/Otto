#include "obd.h"
#include "settings.h"
#include "brain.h"
#include "audio.h"
#include "motion.h"
#include "commands.h"
#include <NimBLEDevice.h>
#include <Preferences.h>

ObdState g_obd;

// ---------------------------------------------------------------- tuning --
static const uint32_t SCAN_EVERY_MS  = 20000;  // look for the adapter this often while not linked
static const uint32_t SCAN_SECS      = 5;
static const uint32_t SEARCH_EVERY   = 6000;   // car not answering (ignition off): retry 0100
static const uint32_t FRESH_MS       = 2000;   // rpm / speed this old still counts
static const uint32_t STATUS_EVERY   = 30000;  // re-read lamp + code count (0101)
static const float    WARM_C         = 75;     // "warm" once coolant reaches this

// ------------------------------------------------------------- transport --
// One ELM327 command at a time: send, collect text until the '>' prompt, parse, next.
static bool s_sim = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_rx[600];
static volatile size_t s_rxLen = 0;
static volatile bool s_prompt = false;

static void rxAppend(const char* d, size_t n) {
  portENTER_CRITICAL(&s_mux);
  for (size_t i = 0; i < n; i++) {
    char c = d[i];
    if (c == '>') { s_prompt = true; continue; }
    if (c == 0) continue;
    if (s_rxLen < sizeof(s_rx) - 1) s_rx[s_rxLen++] = c;
  }
  portEXIT_CRITICAL(&s_mux);
}

static void rxClear() {
  portENTER_CRITICAL(&s_mux);
  s_rxLen = 0; s_prompt = false;
  portEXIT_CRITICAL(&s_mux);
}

// ---- BLE central ----
static NimBLEClient* s_client = nullptr;
static NimBLERemoteCharacteristic* s_tx = nullptr;   // we write commands here
static volatile bool s_bleDown = false;
static volatile bool s_found = false;
static NimBLEAddress s_foundAddr;
static char s_foundName[24] = "";
static bool s_scanning = false;
static uint32_t s_lastScan = 0;
static char s_savedAddr[24] = "";                     // "aa:bb:cc:dd:ee:ff"
static uint8_t s_savedType = 0;                     // address type (public / random), kept for reference

static void onNotify(NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) { rxAppend((const char*)d, n); }

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*) override { s_bleDown = true; }
};
static ClientCB s_clientCb;

static bool nameLooksObd(const std::string& nm) {
  String n = String(nm.c_str());
  n.toUpperCase();
  static const char* const kHints[] = {"OBD", "ELM", "VLINK", "V-LINK", "VGATE", "ICAR", "KONNWEI", "VEEPEAK", "CARISTA"};
  for (const char* h : kHints) if (n.indexOf(h) >= 0) return true;
  return false;
}

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    if (s_found) return;
    std::string addr = d->getAddress().toString();
    bool match;
    if (s_savedAddr[0]) match = !strcasecmp(addr.c_str(), s_savedAddr);   // paired: only our adapter
    else match = nameLooksObd(d->getName()) || d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFF0)) ||
                 d->isAdvertisingService(NimBLEUUID((uint16_t)0x18F0));
    if (!match) return;
    s_foundAddr = d->getAddress();
    strlcpy(s_foundName, d->getName().empty() ? "OBD" : d->getName().c_str(), sizeof(s_foundName));
    s_found = true;
    NimBLEDevice::getScan()->stop();
  }
};
static ScanCB s_scanCb;
static void scanDone(NimBLEScanResults) { s_scanning = false; }

static void saveAddr(const NimBLEAddress& a) {
  strlcpy(s_savedAddr, a.toString().c_str(), sizeof(s_savedAddr));
  s_savedType = a.getType();
  Preferences p;
  p.begin("otto", false);
  p.putString("obdAddr", s_savedAddr);
  p.putUChar("obdType", s_savedType);
  p.end();
}

// Find a notify characteristic (replies) and a write one (commands). Common BLE ELM327s use
// FFF0 (FFF1 notify / FFF2 write) or FFE0 (FFE1 both); anything else that has both also works.
static bool bleConnect() {
  if (!s_client) {
    s_client = NimBLEDevice::createClient();
    s_client->setClientCallbacks(&s_clientCb, false);
    s_client->setConnectTimeout(5);
  }
  Serial.printf("obd: connecting to %s (%s)\n", s_foundName, s_foundAddr.toString().c_str());
  if (!s_client->connect(s_foundAddr)) { Serial.println("obd: connect failed"); return false; }
  NimBLERemoteCharacteristic *rx = nullptr, *tx = nullptr;
  std::vector<NimBLERemoteService*>* svcs = s_client->getServices(true);
  for (NimBLERemoteService* sv : *svcs) {
    NimBLEUUID u = sv->getUUID();
    if (u == NimBLEUUID((uint16_t)0x1800) || u == NimBLEUUID((uint16_t)0x1801) || u == NimBLEUUID((uint16_t)0x180A)) continue;
    NimBLERemoteCharacteristic *r = nullptr, *w = nullptr;
    for (NimBLERemoteCharacteristic* c : *sv->getCharacteristics(true)) {
      if (!r && (c->canNotify() || c->canIndicate())) r = c;
      if (!w && (c->canWrite() || c->canWriteNoResponse())) w = c;
    }
    if (r && w) { rx = r; tx = w; break; }
  }
  if (!rx || !tx) { Serial.println("obd: no serial characteristics on that device"); s_client->disconnect(); return false; }
  if (!rx->subscribe(rx->canNotify(), onNotify)) { Serial.println("obd: subscribe failed"); s_client->disconnect(); return false; }
  s_tx = tx;
  s_bleDown = false;
  if (!s_savedAddr[0]) saveAddr(s_foundAddr);
  strlcpy(g_obd.name, s_foundName, sizeof(g_obd.name));
  Serial.printf("obd: linked to %s\n", g_obd.name);
  return true;
}

static void rawSend(const char* cmd) {
  rxClear();
  if (s_sim) { Serial.printf("obd> %s\n", cmd); return; }
  if (!s_tx) return;
  char b[40];
  int n = snprintf(b, sizeof(b), "%s\r", cmd);
  s_tx->writeValue((const uint8_t*)b, n, s_tx->canWrite() && !s_tx->canWriteNoResponse());
}

// ------------------------------------------------------------ ELM327 FSM --
enum class St : uint8_t { Off, Init, Search, Poll };
static St s_st = St::Off;
static int s_initStep = 0;
static bool s_waiting = false;
static char s_cmd[24] = "";
static uint32_t s_sentAt = 0, s_timeout = 0, s_nextSearch = 0, s_lastStatus = 0;
static int s_timeouts = 0, s_noData = 0, s_slot = 0;
static bool s_hadEcu = false;                 // the car answered at some point on this link
static char s_rawPending[24] = "";           // `obd raw` command waiting to go out
static bool s_rawActive = false;

static const char* const kInit[] = {"ATZ", "ATE0", "ATL0", "ATS1", "ATH0", "ATAT1", "ATSP0"};
static const int N_INIT = sizeof(kInit) / sizeof(kInit[0]);

static void send(const char* cmd, uint32_t timeoutMs) {
  strlcpy(s_cmd, cmd, sizeof(s_cmd));
  s_waiting = true;
  s_sentAt = millis();
  s_timeout = timeoutMs;
  rawSend(cmd);
}

static void resetData() {
  g_obd.ecu = false;
  g_obd.rpm = -1; g_obd.kmh = -1; g_obd.coolantC = -100; g_obd.throttle = -1; g_obd.maf = -1;
  g_obd.rpmAt = g_obd.speedAt = g_obd.coolAt = 0;
}

static void startInit() {
  s_st = St::Init; s_initStep = 0; s_waiting = false; s_timeouts = 0;
  resetData();
}

static bool pidSupported(uint8_t pid) {
  if (pid == 0 || pid > 0x40) return false;
  int w = (pid - 1) / 32, b = 31 - (pid - 1) % 32;
  return (g_obd.pids[w] >> b) & 1;
}

// Hex bytes of the first reply line starting with `prefix` (e.g. 0x41 0x0C). Spaces optional.
static int findReply(const char* text, uint8_t mode, int pid, uint8_t* out, int maxOut, int lineSkip = 0) {
  const char* p = text;
  int seen = 0;
  while (*p) {
    const char* e = p;
    while (*e && *e != '\r' && *e != '\n') e++;
    uint8_t b[16]; int n = 0; bool hexLine = e > p;
    int nib = -1;
    for (const char* q = p; q < e; q++) {
      char c = toupper(*q);
      if (c == ' ') continue;
      int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (v < 0) { hexLine = false; break; }
      if (nib < 0) nib = v; else { if (n < 16) b[n++] = (uint8_t)(nib << 4 | v); nib = -1; }
    }
    if (hexLine && n >= 1 && b[0] == mode && (pid < 0 || (n >= 2 && b[1] == pid))) {
      if (seen++ >= lineSkip) {
        int start = pid < 0 ? 1 : 2, k = 0;
        for (int i = start; i < n && k < maxOut; i++) out[k++] = b[i];
        return k;
      }
    }
    p = *e ? e + 1 : e;
  }
  return -1;
}

static bool textHas(const char* t, const char* s) {
  String u = String(t); u.toUpperCase(); u.replace(" ", "");
  String v = String(s); v.replace(" ", "");
  return u.indexOf(v) >= 0;
}

static void decodeCodes(const char* text) {
  g_obd.nCodes = 0;
  for (int line = 0; line < 4; line++) {
    uint8_t b[14];
    int n = findReply(text, 0x43, -1, b, sizeof(b), line);
    if (n < 0) break;
    int i = (n % 2) ? 1 : 0;                     // CAN replies start with a count byte
    for (; i + 1 < n; i += 2) {
      uint8_t A = b[i], B = b[i + 1];
      if (!A && !B) continue;
      if (g_obd.nCodes >= 8) break;
      snprintf(g_obd.codes[g_obd.nCodes++], 6, "%c%X%X%X%X", "PCBU"[A >> 6], (A >> 4) & 3, A & 0xF, B >> 4, B & 0xF);
    }
  }
}

// ---- reactions fed from the data ----
static uint32_t s_engineOnAt = 0, s_lastRev = 0, s_lastHot = 0, s_warmAt = 0, s_codesAt = 0;
static float s_idleRpm = 0;
static bool s_inRev = false, s_wasCold = false, s_warmShown = false, s_engine = false, s_codesShown = false;
static int s_codeIdx = -1;
static uint32_t s_codeNext = 0;

static void onRpm(uint32_t now) {
  bool running = g_obd.rpm > 300;
  if (running && !s_engine) {                    // engine start: queue warm-up + codes cards
    s_engineOnAt = now; s_wasCold = false; s_warmShown = false; s_codesShown = false;
    s_warmAt = now + 6500;                       // after ENGINE ON + the greeting
    s_codesAt = now + 11000;
  }
  s_engine = running;
  if (!running) { s_inRev = false; s_idleRpm = 0; return; }
  // revs while parked: rpm well above the learned idle
  bool parked = g_motion.stoppedSecs > 2.0f && (g_obd.kmh < 0 || g_obd.kmh < 2);
  if (!parked) { s_inRev = false; return; }
  if (!s_inRev && g_obd.rpm < 1300) s_idleRpm = s_idleRpm ? s_idleRpm + (g_obd.rpm - s_idleRpm) * 0.1f : g_obd.rpm;
  float idle = s_idleRpm ? s_idleRpm : 700;
  if (!s_inRev && g_obd.rpm > idle + 900 && g_obd.rpm > idle * 1.6f && now - s_lastRev > 400) {
    s_inRev = true; s_lastRev = now;
    if (g_set.revOn) brainEvent(Expr::Rev, 1100);
  }
  if (s_inRev && g_obd.rpm < idle + 400) s_inRev = false;
}

static void parse(const char* text, uint32_t now) {
  uint8_t b[8];
  // Init / search replies
  if (s_st == St::Init) return;
  if (!strcmp(s_cmd, "0100") || !strcmp(s_cmd, "0120")) {
    int n = findReply(text, 0x41, s_cmd[2] == '0' && s_cmd[3] == '0' ? 0x00 : 0x20, b, 4);
    if (n == 4) {
      g_obd.pids[s_cmd[2] == '2' ? 1 : 0] = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
      if (!strcmp(s_cmd, "0100")) {
        g_obd.ecu = true; s_noData = 0; s_hadEcu = true;
        Serial.printf("obd: car answers, PIDs 01-20 %08X\n", (unsigned)g_obd.pids[0]);
      }
    } else if (!strcmp(s_cmd, "0100")) {
      g_obd.ecu = false;
      s_nextSearch = now + SEARCH_EVERY;
    }
    return;
  }
  if (!strcmp(s_cmd, "ATDP")) {
    const char* p = text;
    while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    strlcpy(g_obd.proto, p, sizeof(g_obd.proto));
    for (char* q = g_obd.proto; *q; q++) if (*q == '\r' || *q == '\n') { *q = 0; break; }
    Serial.printf("obd: protocol %s\n", g_obd.proto);
    return;
  }
  if (!strcmp(s_cmd, "03")) { decodeCodes(text); Serial.printf("obd: %d code(s) read\n", g_obd.nCodes); return; }

  uint8_t pid = (uint8_t)strtol(s_cmd + 2, nullptr, 16);
  int n = findReply(text, 0x41, pid, b, 4);
  if (n < 0) {
    if (pid == 0x0C) {                           // the car went quiet: ignition off?
      if (textHas(text, "NODATA") || textHas(text, "UNABLETOCONNECT") || textHas(text, "ERROR") || textHas(text, "STOPPED")) {
        if (++s_noData >= 3) {
          Serial.println("obd: car stopped answering");
          resetData();
          g_obd.rpm = 0; g_obd.rpmAt = now;      // a linked adapter + silent car = engine off
          onRpm(now);
          s_st = St::Search; s_nextSearch = now + SEARCH_EVERY;
        }
      }
    }
    return;
  }
  switch (pid) {
    case 0x0C: if (n >= 2) { g_obd.rpm = (b[0] * 256 + b[1]) / 4.0f; g_obd.rpmAt = now; s_noData = 0; onRpm(now); } break;
    case 0x0D:
      g_obd.kmh = b[0]; g_obd.speedAt = now;
      g_speed.mph = b[0] * 0.621371f; g_speed.at = now;   // launch timer + trip miles
      break;
    case 0x05: g_obd.coolantC = b[0] - 40.0f; g_obd.coolAt = now; break;
    case 0x11: g_obd.throttle = b[0] * 100.0f / 255.0f; break;
    case 0x10: if (n >= 2) g_obd.maf = (b[0] * 256 + b[1]) / 100.0f; break;
    case 0x01: g_obd.mil = b[0] & 0x80; g_obd.nDtc = b[0] & 0x7F; break;
  }
}

// What to ask next while polling: rpm + speed every round, then one slower item in turn.
static void pollNext(uint32_t now) {
  static const uint8_t kSlow[] = {0x05, 0x11, 0x10, 0x05};
  static int phase = 0;
  char c[8];
  if (s_rawPending[0]) { send(s_rawPending, 4000); s_rawActive = true; s_rawPending[0] = 0; return; }
  if (phase == 0) { send("010C", 1500); phase = 1; return; }
  if (phase == 1) { send("010D", 1500); phase = 2; return; }
  phase = 0;
  if (!s_lastStatus || now - s_lastStatus > STATUS_EVERY) { s_lastStatus = now; send("0101", 1500); return; }
  if ((g_obd.mil || g_obd.nDtc) && g_obd.nCodes == 0 && g_obd.nDtc) { g_obd.nCodes = 0xFF; send("03", 3000); return; }
  for (int k = 0; k < 4; k++) {
    uint8_t pid = kSlow[s_slot++ % 4];
    if (pid == 0x10 && !pidSupported(0x10)) continue;
    snprintf(c, sizeof(c), "01%02X", pid);
    send(c, 1500);
    return;
  }
  send("010C", 1500);
}

static void fsm(uint32_t now) {
  if (s_waiting) {
    if (s_prompt) {
      char text[sizeof(s_rx)];
      portENTER_CRITICAL(&s_mux);
      memcpy(text, s_rx, s_rxLen); text[s_rxLen] = 0;
      portEXIT_CRITICAL(&s_mux);
      s_waiting = false; s_timeouts = 0;
      if (s_rawActive) {
        s_rawActive = false;
        String t = String(text); t.replace("\r", " | "); t.replace("\n", " ");
        Serial.printf("obd raw %s: %s\n", s_cmd, t.c_str());
      }
      if (s_st == St::Init) {
        if (++s_initStep >= N_INIT) { s_st = St::Search; s_nextSearch = 0; }
      } else {
        parse(text, now);
        if (s_st == St::Search && g_obd.ecu) {
          s_st = St::Poll;
          s_lastStatus = 0;
          if (pidSupported(0x20)) send("0120", 2000);
          else send("ATDP", 1500);
          return;
        }
        if (s_st == St::Poll && !strcmp(s_cmd, "0120")) { send("ATDP", 1500); return; }
      }
    } else if (now - s_sentAt > s_timeout) {
      s_waiting = false;
      s_rawActive = false;
      if (!strcmp(s_cmd, "03")) g_obd.nCodes = 0;   // try reading the codes again later
      if (++s_timeouts >= 3) { Serial.println("obd: adapter not answering, resetting it"); startInit(); return; }
      if (s_st == St::Init) s_initStep++;        // some clones never answer one of the AT commands
      if (s_st == St::Search) s_nextSearch = now + SEARCH_EVERY;
    } else return;
  }
  switch (s_st) {
    case St::Init:
      if (s_initStep < N_INIT) send(kInit[s_initStep], s_initStep == 0 ? 3000 : 1500);
      else { s_st = St::Search; s_nextSearch = 0; }
      break;
    case St::Search:
      if (s_rawPending[0]) { send(s_rawPending, 4000); s_rawActive = true; s_rawPending[0] = 0; break; }
      if ((int32_t)(now - s_nextSearch) >= 0) send("0100", 12000);   // first contact: 5-baud init can take a while
      break;
    case St::Poll: pollNext(now); break;
    default: break;
  }
}

// --------------------------------------------------------------- reactions --
static void cards(uint32_t now) {
  if (!g_obd.ecu || !s_engine) return;
  // warm-up: cold engine right after start (once), then "warm" when it gets there
  if (s_warmAt && (int32_t)(now - s_warmAt) >= 0 && g_obd.coolAt) {
    s_warmAt = 0;
    if (g_obd.coolantC < g_set.coldCoolC) { s_wasCold = true; brainEvent(Expr::WarmUp, 4500); audioPlayClip("nochkalt"); }
  }
  if (s_wasCold && !s_warmShown && g_obd.coolantC >= WARM_C) {
    s_warmShown = true;
    brainEvent(Expr::Warm, 3500);
    audioPlayClip("warm");
  }
  // running hot: repeat every minute until it cools a little
  if (g_obd.coolAt && g_obd.coolantC >= g_set.hotCoolC && now - s_lastHot > 60000) {
    s_lastHot = now;
    brainEvent(Expr::EngineHot, 6000);
    audioChirp(900, 400, 300);
  }
  // check-engine: once per engine start when the car has stored codes
  if (!s_codesShown && s_codesAt && (int32_t)(now - s_codesAt) >= 0 && g_obd.nCodes && g_obd.nCodes != 0xFF) {
    s_codesShown = true;
    obdShowCodes();
    audioChirp(500, 300, 200);
  }
}

static void showCode(int i) {
  char sub[28];
  if (g_obd.nCodes == 0 || g_obd.nCodes == 0xFF) {
    brainSetAnswer(g_obd.ecu ? "CHECK ENGINE" : "NO OBD", g_obd.ecu ? "OK" : "--", g_obd.ecu ? "NO CODES STORED" : "ADAPTER NOT LINKED");
    return;
  }
  snprintf(sub, sizeof(sub), "CODE %d OF %d%s", i + 1, g_obd.nCodes, g_obd.mil ? " - LAMP ON" : "");
  brainSetAnswer("CHECK ENGINE", g_obd.codes[i], sub);
}

void obdShowCodes() {
  int n = (g_obd.nCodes == 0xFF) ? 0 : g_obd.nCodes;
  s_codeIdx = 0;
  s_codeNext = millis() + 2500;
  showCode(0);
  brainEvent(Expr::CheckEngine, n > 1 ? 2600 * n : 4000);
  if (n <= 1) s_codeIdx = -1;
}

// ---------------------------------------------------------------- public --
void obdInit() {
  Preferences p;
  p.begin("otto", true);
  String a = p.getString("obdAddr", "");
  s_savedType = p.getUChar("obdType", 0);
  p.end();
  strlcpy(s_savedAddr, a.c_str(), sizeof(s_savedAddr));
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setAdvertisedDeviceCallbacks(&s_scanCb, false);
  sc->setActiveScan(true);
  sc->setInterval(100);
  sc->setWindow(50);
}

void obdUpdate(uint32_t now) {
  // cycle through several codes on the check-engine card
  if (s_codeIdx >= 0 && brainCurrent() == Expr::CheckEngine && (int32_t)(now - s_codeNext) >= 0) {
    s_codeIdx = (s_codeIdx + 1) % g_obd.nCodes;
    s_codeNext = now + 2500;
    showCode(s_codeIdx);
  } else if (s_codeIdx >= 0 && brainCurrent() != Expr::CheckEngine && now - s_codeNext > 3000) s_codeIdx = -1;

  if (s_sim) { if (s_st == St::Off) startInit(); fsm(now); cards(now); return; }

  if (s_client && s_tx && s_bleDown) {           // adapter went away (car off, out of range)
    Serial.println("obd: adapter disconnected");
    s_tx = nullptr; s_bleDown = false;
    g_obd.adapter = false; s_st = St::Off; resetData();
    s_engine = false; s_hadEcu = false;
  }
  if (!g_set.obdOn) {
    if (g_obd.adapter) obdDisconnect();
    return;
  }
  if (!g_obd.adapter) {
    if (s_found && !s_scanning) {
      s_found = false;
      if (bleConnect()) { g_obd.adapter = true; startInit(); }
      s_lastScan = millis();
      return;
    }
    if (!s_scanning && (!s_lastScan || now - s_lastScan > SCAN_EVERY_MS)) {
      s_lastScan = now;
      s_scanning = NimBLEDevice::getScan()->start(SCAN_SECS, scanDone, false);
    }
    return;
  }
  fsm(now);
  cards(now);
}

void obdDisconnect() {
  if (s_scanning) { NimBLEDevice::getScan()->stop(); s_scanning = false; }
  if (s_client && s_client->isConnected()) s_client->disconnect();
  s_tx = nullptr;
  g_obd.adapter = false; s_st = St::Off; resetData();
  s_engine = false; s_hadEcu = false;
}

int obdEngineState() {
  if (!g_obd.adapter) return -1;
  if (g_obd.rpmAt && millis() - g_obd.rpmAt < 5000 && g_obd.rpm >= 0) return g_obd.rpm > 300 ? 1 : 0;
  if (s_hadEcu && !g_obd.ecu) return 0;          // adapter still linked, car went silent: ignition off
  return -1;
}

bool obdRpmFresh() { return g_obd.rpmAt && millis() - g_obd.rpmAt < FRESH_MS && g_obd.rpm >= 0; }
float obdRpm() { return obdRpmFresh() ? g_obd.rpm : 0; }

float obdFuelGps() {
  if (g_obd.maf < 0 || !obdRpmFresh()) return -1;
  return g_obd.maf / 14.7f;                      // stoichiometric petrol
}

void obdSimRx(const char* t) {
  // one reply per line, '|' separates the adapter's lines; the prompt is implied
  String s = String(t);
  s.replace("|", "\r");
  s += "\r>";
  rxAppend(s.c_str(), s.length());
}

void obdCommand(const char* a, char* r, size_t n) {
  while (*a == ' ') a++;
  if (!*a) {
    snprintf(r, n, "obd: %s%s, adapter %s%s%s, car %s%s%s - rpm %.0f, %.0f km/h, coolant %.0fC, throttle %.0f%%, lamp %s, codes %d",
             g_set.obdOn ? "on" : "off", s_sim ? " (SIM)" : "",
             g_obd.adapter ? "linked " : (s_scanning ? "scanning" : "not linked"), g_obd.adapter ? g_obd.name : "",
             s_savedAddr[0] && !g_obd.adapter ? (String(" (paired ") + s_savedAddr + ")").c_str() : "",
             g_obd.ecu ? "answering" : "silent", g_obd.proto[0] ? " / " : "", g_obd.proto,
             g_obd.rpm, g_obd.kmh, g_obd.coolantC, g_obd.throttle, g_obd.mil ? "ON" : "off",
             g_obd.nCodes == 0xFF ? 0 : g_obd.nCodes);
  } else if (!strcasecmp(a, "on") || !strcasecmp(a, "off")) {
    g_set.obdOn = !strcasecmp(a, "on");
    settingsSave();
    snprintf(r, n, "obd %s", g_set.obdOn ? "on" : "off");
  } else if (!strcasecmp(a, "forget")) {
    obdDisconnect();
    s_savedAddr[0] = 0;
    Preferences p;
    p.begin("otto", false);
    p.remove("obdAddr"); p.remove("obdType");
    p.end();
    snprintf(r, n, "obd: forgot the paired adapter; the next one found pairs");
  } else if (!strcasecmp(a, "sim on")) {
    obdDisconnect();
    s_sim = true; g_obd.adapter = true; strlcpy(g_obd.name, "SIM", sizeof(g_obd.name));
    startInit();
    snprintf(r, n, "obd: bench sim on - run tools/fake_obd.py");
  } else if (!strcasecmp(a, "sim off")) {
    s_sim = false; g_obd.adapter = false; s_st = St::Off; resetData(); s_engine = false; s_hadEcu = false;
    g_obd.mil = false; g_obd.nDtc = 0; g_obd.nCodes = 0; g_obd.proto[0] = 0;   // forget the fake car
    snprintf(r, n, "obd: sim off");
  } else if (!strcasecmp(a, "codes")) {
    obdShowCodes();
    snprintf(r, n, "obd: %d code(s)", g_obd.nCodes == 0xFF ? 0 : g_obd.nCodes);
  } else if (!strncasecmp(a, "raw ", 4)) {
    if (!g_obd.adapter) { snprintf(r, n, "obd: no adapter linked"); return; }
    strlcpy(s_rawPending, a + 4, sizeof(s_rawPending));
    snprintf(r, n, "obd: sending %s (reply on serial)", s_rawPending);
  } else {
    snprintf(r, n, "obd [on|off|forget|codes|raw <cmd>|sim on|sim off]");
  }
}
