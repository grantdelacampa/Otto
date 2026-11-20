#include "board.h"
#include <esp_system.h>
#include "config.h"
#include <Wire.h>
#include <Preferences.h>
#include <sys/time.h>
#include "driver/gpio.h"

static uint8_t s_exio = 0xFF;
static bool s_rtcValid = false;
static int s_tzMin = 0;

bool boardI2CRead(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool boardI2CWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void boardExpanderWrite(uint8_t bit, bool high) {
  if (high) s_exio |= (1 << bit); else s_exio &= ~(1 << bit);
  boardI2CWrite(ADDR_TCA9554, 0x01, s_exio);
}

void boardEarlyInit() {
  // Keep ourselves powered when running on battery
  gpio_hold_dis((gpio_num_t)PIN_PWR_HOLD);
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BTN_PWR, INPUT_PULLUP);
  analogReadResolution(12);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

  // TCA9554: bits 0..2 outputs (touch reset, LCD reset, SD D3). After a deep sleep the display /
  // touch chip stays held in reset (low): a sleep peek mustn't wake it, and displayInit() /
  // touchInit() pulse their own resets anyway on a full boot.
  s_exio = esp_reset_reason() == ESP_RST_DEEPSLEEP ? 0xFC : 0xFF;
  boardI2CWrite(ADDR_TCA9554, 0x01, s_exio);
  boardI2CWrite(ADDR_TCA9554, 0x03, 0xF8);

  Preferences p;
  p.begin("otto", true);
  s_tzMin = p.getInt("tz", 0);
  p.end();
}

float boardBatteryVolts() {
  return analogReadMilliVolts(PIN_BAT_ADC) * 3.0f / 1000.0f;
}

void boardPowerOff() {
  gpio_hold_dis((gpio_num_t)PIN_PWR_HOLD);
  digitalWrite(PIN_PWR_HOLD, LOW);  // on USB power the board simply stays on
}

void boardHoldResetsForSleep() {     // display + touch (one SPD2010) held in reset while asleep
  boardExpanderWrite(EXIO_TOUCH_RST, false);
  boardExpanderWrite(EXIO_LCD_RST, false);
}

void boardHoldPinsForSleep() {
  digitalWrite(PIN_PWR_HOLD, HIGH);
  gpio_hold_en((gpio_num_t)PIN_PWR_HOLD);
  ledcDetach(PIN_LCD_BL);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en((gpio_num_t)PIN_LCD_BL);
  gpio_deep_sleep_hold_en();
}

void boardReleasePinHolds() {
  gpio_hold_dis((gpio_num_t)PIN_PWR_HOLD);
  gpio_hold_dis((gpio_num_t)PIN_LCD_BL);
}

bool boardBootPressed() { return digitalRead(PIN_BTN_BOOT) == LOW; }
bool boardPwrPressed()  { return digitalRead(PIN_BTN_PWR) == LOW; }

// ----------------------------------------------------------------- RTC ----
static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static time_t timegmPortable(struct tm* t) {
  // days from civil (Howard Hinnant), avoids depending on TZ env
  int y = t->tm_year + 1900, m = t->tm_mon + 1, d = t->tm_mday;
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

bool rtcInit() {
  uint8_t r[7];
  if (!boardI2CRead(ADDR_PCF85063, 0x04, r, 7)) { s_rtcValid = false; return false; }
  s_rtcValid = !(r[0] & 0x80);  // OS flag: oscillator stopped -> time invalid
  if (s_rtcValid) {
    time_t t = rtcNowUtc();
    if (t < 1700000000) s_rtcValid = false;
    else { struct timeval tv = {t, 0}; settimeofday(&tv, nullptr); }
  }
  return s_rtcValid;
}

bool rtcValid() { return s_rtcValid; }

// ---------------------------------------------------------------- SHTC3 ---
// Sensirion SHTC3: 16-bit commands, no register address. Wake -> measure (T first,
// normal mode, no clock stretching, ~12 ms) -> read 6 bytes (T, CRC, RH, CRC) -> sleep.
static bool s_shtOk = false, s_shtPending = false, s_shtPresent = false;
static float s_shtT = 0, s_shtRH = 0;
static uint32_t s_shtT0 = 0, s_shtLast = 0;

static bool shtCmd(uint16_t c) {
  Wire.beginTransmission(ADDR_SHTC3);
  Wire.write(c >> 8);
  Wire.write(c & 0xFF);
  return Wire.endTransmission() == 0;
}

static uint8_t shtCrc(const uint8_t* d) {     // CRC-8, poly 0x31, init 0xFF
  uint8_t crc = 0xFF;
  for (int i = 0; i < 2; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

bool shtc3Init() {
  shtCmd(0x3517);                             // wake
  delayMicroseconds(300);
  s_shtPresent = shtCmd(0xEFC8);              // read ID
  if (s_shtPresent) {
    uint8_t id[3] = {0};
    if (Wire.requestFrom((int)ADDR_SHTC3, 3) == 3) for (int i = 0; i < 3; i++) id[i] = Wire.read();
    s_shtPresent = ((id[0] << 8 | id[1]) & 0x083F) == 0x0807;   // SHTC3 product code bits
  }
  shtCmd(0xB098);                             // sleep
  return s_shtPresent;
}

void shtc3Update(uint32_t now) {
  if (!s_shtPresent) return;
  if (!s_shtPending) {
    if (s_shtLast && now - s_shtLast < 5000) return;
    shtCmd(0x3517);
    delayMicroseconds(300);
    s_shtPending = shtCmd(0x7866);
    s_shtT0 = now;
    s_shtLast = now;
    return;
  }
  if (now - s_shtT0 < 15) return;
  s_shtPending = false;
  uint8_t d[6];
  bool ok = Wire.requestFrom((int)ADDR_SHTC3, 6) == 6;
  if (ok) for (int i = 0; i < 6; i++) d[i] = Wire.read();
  shtCmd(0xB098);
  if (!ok || shtCrc(d) != d[2] || shtCrc(d + 3) != d[5]) return;
  s_shtT = -45.0f + 175.0f * (d[0] << 8 | d[1]) / 65536.0f;
  s_shtRH = 100.0f * (d[3] << 8 | d[4]) / 65536.0f;
  s_shtOk = true;
}

bool shtc3Ok() { return s_shtOk; }
float shtc3TempC() { return s_shtT; }
float shtc3Humidity() { return s_shtRH; }

time_t rtcNowUtc() {
  uint8_t r[7];
  if (!boardI2CRead(ADDR_PCF85063, 0x04, r, 7)) return 0;
  struct tm t = {};
  t.tm_sec  = bcd2bin(r[0] & 0x7F);
  t.tm_min  = bcd2bin(r[1] & 0x7F);
  t.tm_hour = bcd2bin(r[2] & 0x3F);
  t.tm_mday = bcd2bin(r[3] & 0x3F);
  t.tm_mon  = bcd2bin(r[5] & 0x1F) - 1;
  t.tm_year = bcd2bin(r[6]) + 100;
  return timegmPortable(&t);
}

void rtcSetUtc(time_t now) {
  struct tm t;
  gmtime_r(&now, &t);
  Wire.beginTransmission(ADDR_PCF85063);
  Wire.write(0x04);
  Wire.write(bin2bcd(t.tm_sec));  // also clears the OS flag
  Wire.write(bin2bcd(t.tm_min));
  Wire.write(bin2bcd(t.tm_hour));
  Wire.write(bin2bcd(t.tm_mday));
  Wire.write(t.tm_wday);
  Wire.write(bin2bcd(t.tm_mon + 1));
  Wire.write(bin2bcd(t.tm_year - 100));
  Wire.endTransmission();
  struct timeval tv = {now, 0};
  settimeofday(&tv, nullptr);
  s_rtcValid = true;
}

int localHour() {
  if (!s_rtcValid) return -1;
  time_t now = time(nullptr) + s_tzMin * 60;
  struct tm t;
  gmtime_r(&now, &t);
  return t.tm_hour;
}

void setTzOffsetMin(int m) {
  s_tzMin = m;
  Preferences p;
  p.begin("otto", false);
  p.putInt("tz", m);
  p.end();
}

int tzOffsetMin() { return s_tzMin; }
