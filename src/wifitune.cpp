#include "wifitune.h"
#include "commands.h"
#include "settings.h"
#include "brain.h"
#include "audio.h"
#include "voice.h"
#include "recorder.h"
#include "ota.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_mac.h>

// The tuning page, built into the firmware (platformio.ini: board_build.embed_txtfiles)
extern const char tuning_html_start[] asm("_binary_tools_tuning_html_start");
extern const char tuning_html_end[] asm("_binary_tools_tuning_html_end");

static const uint32_t IDLE_OFF_MS = 10UL * 60 * 1000;   // no page request for this long -> off
static WebServer* s_srv = nullptr;
static bool s_on = false, s_voiceWas = false;
static uint32_t s_lastReq = 0, s_startedAt = 0;
static bool s_clientSeen = false;
static char s_ssid[16] = "", s_pass[16] = "";
static String s_reply;                                   // replies captured from handleCommand
static uint32_t s_rebootAt = 0;
void requestQuietReboot();                               // main.cpp

void wifiTuneReply(const char* msg) {                    // commands.cpp: reply() for SRC_HTTP
  if (s_reply.length() < 6000) { s_reply += msg; s_reply += '\n'; }
}

static void touchReq() { s_lastReq = millis(); s_clientSeen = true; }

static void sendNoCache(int code, const char* type, const String& body) {
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(code, type, body);
}

static void onPage() {
  touchReq();
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send_P(200, "text/html", tuning_html_start, tuning_html_end - tuning_html_start - 1);   // -1: embed adds a NUL
}

static void onCmd() {
  touchReq();
  String c = s_srv->arg("plain");
  c.trim();
  s_reply = "";
  if (c.length()) handleCommand(c.c_str(), SRC_HTTP);
  sendNoCache(200, "text/plain", s_reply);
  s_reply = "";
}

static void onStat() {
  touchReq();
  char b[260];
  buildTelemetry(b, sizeof(b));
  sendNoCache(200, "application/json", b);
}

static void onCfgGet() { touchReq(); sendNoCache(200, "application/json", settingsToJson()); }

static void onCfgPost() {
  touchReq();
  bool ok = settingsApplyJson(s_srv->arg("plain").c_str());
  sendNoCache(ok ? 200 : 400, "text/plain", ok ? "saved" : "bad json");
}

static void onLog() {                                    // drive log: [[brake,accel,lat,yaw,bump],...]
  touchReq();
  size_t n = recorderCount();
  s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(200, "application/json", "");
  String chunk = "[";
  for (size_t i = 0; i < n; i++) {
    LogRec r = recorderGet(i);
    char b[64];
    snprintf(b, sizeof(b), "%s[%.3f,%.3f,%.3f,%.1f,%.3f]", i ? "," : "", r.brake / 1000.0f, r.accel / 1000.0f,
             r.lat / 1000.0f, r.yaw / 10.0f, r.bump / 1000.0f);
    chunk += b;
    if (chunk.length() > 1200) { s_srv->sendContent(chunk); chunk = ""; }
  }
  chunk += "]";
  s_srv->sendContent(chunk);
  s_srv->sendContent("");
}

// Firmware update: multipart upload of firmware.bin, ?size=<bytes> so the image size is known up front
static bool s_otaOk = false;
static void onOtaDone() {
  touchReq();
  bool ok = s_otaOk && otaEnd();
  sendNoCache(ok ? 200 : 500, "text/plain", ok ? "ota ok" : String("ota fail ") + otaError());
}
static void onOtaData() {
  touchReq();
  HTTPUpload& up = s_srv->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_otaOk = otaBegin((size_t)s_srv->arg("size").toInt());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_otaOk) s_otaOk = otaWrite(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    otaAbort(); s_otaOk = false;
  }
}

void wifiTuneStart() {
  if (s_on) return;
  // Voice off while tuning: the Wi-Fi stack shares core 0 with the speech engine's audio feed,
  // and both want the chip's internal RAM. It comes back on wifiTuneStop() if VOICE is on.
  s_voiceWas = voiceRunning();
  if (s_voiceWas) voiceSetEnabled(false);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(s_ssid, sizeof(s_ssid), "OTTO-%02X%02X", mac[4], mac[5]);
  snprintf(s_pass, sizeof(s_pass), "otto%02x%02x", mac[3] ^ 0x5A, mac[5] ^ 0xA5);   // 8 chars, per board
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(s_ssid, s_pass, 6, 0, 2)) {
    Serial.println("wifi: could not start the access point");
    WiFi.mode(WIFI_OFF);
    if (s_voiceWas) voiceSetEnabled(true);
    return;
  }
  WiFi.setTxPower(WIFI_POWER_11dBm);                     // it's for a phone in the same car
  s_srv = new WebServer(80);
  s_srv->on("/", HTTP_GET, onPage);
  s_srv->on("/tuning.html", HTTP_GET, onPage);
  s_srv->on("/api/cmd", HTTP_POST, onCmd);
  s_srv->on("/api/stat", HTTP_GET, onStat);
  s_srv->on("/api/cfg", HTTP_GET, onCfgGet);
  s_srv->on("/api/cfg", HTTP_POST, onCfgPost);
  s_srv->on("/api/log", HTTP_GET, onLog);
  s_srv->on("/api/ota", HTTP_POST, onOtaDone, onOtaData);
  s_srv->onNotFound([]() { s_srv->sendHeader("Location", "http://192.168.4.1/"); s_srv->send(302, "text/plain", ""); });
  s_srv->begin();
  s_on = true; s_clientSeen = false;
  s_lastReq = s_startedAt = millis();
  Serial.printf("wifi: tuning on - join %s (password %s), open http://%s/\n", s_ssid, s_pass, WiFi.softAPIP().toString().c_str());
  audioChirp(900, 1500, 120);
}

void wifiTuneStop() {
  if (!s_on) return;
  s_srv->stop();
  delete s_srv; s_srv = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_on = false;
  Serial.println("wifi: tuning off");
  brainEvent(Expr::Saved, 1500);
  // Wi-Fi keeps part of the chip's internal RAM even when it's off, and voice then can't start
  // ("Failed create audio detect task"). A quick restart (no boot show) gives it all back.
  // Settings are already saved.
  if (s_voiceWas && g_set.voiceOn) s_rebootAt = millis() + 1500;
}

void wifiTuneUpdate(uint32_t now) {
  if (s_rebootAt && (int32_t)(now - s_rebootAt) >= 0) { s_rebootAt = 0; requestQuietReboot(); }
  if (!s_on) return;
  s_srv->handleClient();
  if (!otaActive() && (int32_t)(now - s_lastReq) > (int32_t)IDLE_OFF_MS) { Serial.println("wifi: 10 min without the page, turning off"); wifiTuneStop(); return; }
  // Until the page is open, show how to get there; afterwards Otto goes back to his faces
  bool recent = s_clientSeen && (int32_t)(now - s_lastReq) < 15000;
  if (!recent) {
    char label[24];
    snprintf(label, sizeof(label), "WI-FI %s", s_ssid);
    brainSetAnswer(label, s_pass, "THEN OPEN 192.168.4.1");
    brainEvent(Expr::WifiTune, 1200);
  }
}

bool wifiTuneActive() { return s_on; }
const char* wifiTuneSsid() { return s_ssid; }
const char* wifiTunePass() { return s_pass; }

void wifiTuneInfo(char* b, size_t n) {
  if (!s_on) { snprintf(b, n, "wifi: off"); return; }
  snprintf(b, n, "wifi: on %lu s - %s / %s, %d phone(s) joined, last page request %lu s ago", (millis() - s_startedAt) / 1000,
           s_ssid, s_pass, WiFi.softAPgetStationNum(), (millis() - s_lastReq) / 1000);
}
