#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Board: Waveshare ESP32-S3-Touch-LCD-1.46B (412x412 round, no cover glass)
// Pins from Waveshare's 1.46B wiki + demo code. Same ESP32-S3R8 / 16 MB flash as Rotor's 1.85".
// ---------------------------------------------------------------------------
// Display: SPD2010, QSPI, 412x412
#define PIN_LCD_SCK    40
#define PIN_LCD_D0     46
#define PIN_LCD_D1     45
#define PIN_LCD_D2     42
#define PIN_LCD_D3     41
#define PIN_LCD_CS     21
#define PIN_LCD_TE     18
#define PIN_LCD_BL      5
#define LCD_W         412
#define LCD_H         412
#define LCD_QSPI_HZ   (80 * 1000 * 1000)   // Espressif default 20 MHz; 40 and 80 both clean on the owner's board. Drop back if the image glitches

// One I2C bus for everything: touch SPD2010 (0x53), IMU QMI8658 (0x6B), RTC PCF85063 (0x51),
// IO expander TCA9554 (0x20)
#define PIN_I2C_SDA    11
#define PIN_I2C_SCL    10
#define ADDR_TCA9554   0x20
#define ADDR_PCF85063  0x51
#define ADDR_SHTC3     0x70   // not on the 1.46B; the driver finds nothing and stays off
#define PIN_RTC_INT     9

// Touch: SPD2010's touch half, on the main I2C bus. Multi-touch capable, but Otto's
// gestures use the first finger only.
#define PIN_TP_INT      4   // LOW when touch data is ready
#define ADDR_TOUCH     0x53
// If touches land in the wrong place, flip these (check with the "touch" serial command)
#define TOUCH_SWAP_XY   0
#define TOUCH_MIRROR_X  0
#define TOUCH_MIRROR_Y  0

// Bump with every change to the driving / music detection that a drive should check. The drive
// log stamps it on each session ("tune N") and docs/tuning/TUNING.md says what each one changed.
#define TUNE_REV        2

// Which way the top of the screen points in the IMU's X/Y axes (Z goes out of the back): the
// gravity "up" a calibration saves when the board stands upright on the dash. Calibration flips
// the screen when the saved up points the other way (roof mount). Bench, Nov 23 2026: upright
// calibrated to up (0.95, 0.02, 0.09) = chip +X.
#define SCREEN_UP_X     1.0f
#define SCREEN_UP_Y     0.0f
// If no report comes for this long while a finger is down, the finger counts as
// lifted. Raise it if petting keeps stopping.
#define TOUCH_RELEASE_MS 250

// TCA9554 expander bits (Waveshare's EXIO1..EXIO8 = bits 0..7)
#define EXIO_TOUCH_RST  0   // EXIO1
#define EXIO_LCD_RST    1   // EXIO2
#define EXIO_SD_D3      2   // EXIO3 (TF card CS)
#define EXIO_IMU_INT2   3   // EXIO4 (input)
#define EXIO_IMU_INT1   4   // EXIO5 (input)
// Note: the IMU interrupts only reach the ESP32 through the expander, so they can't
// wake it from deep sleep directly. Sleep keeps the 4 s gyro peek for now.

// Audio out: PCM5101 DAC -> onboard amp + speaker header
#define PIN_SPK_BCK    48
#define PIN_SPK_WS     38
#define PIN_SPK_DOUT   47
// Microphone: I2S MEMS mic
#define PIN_MIC_SCK    15
#define PIN_MIC_WS      2
#define PIN_MIC_DIN    39

// Power (same layout as the 1.85": verify on the bench with "batfake off" + a battery)
#define PIN_BAT_ADC     8   // 200k / 100k divider -> x3
#define PIN_PWR_HOLD    7   // BAT_Control: must stay HIGH to stay powered on battery
#define PIN_BTN_BOOT    0   // tap = boop, hold 2 s = calibrate
#define PIN_BTN_PWR     6   // Key_BAT: hold 3 s = power off

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------
#define BLE_NAME        "OTTO"
#define UUID_SERVICE    "7f3a0001-5e7a-4c0e-9b1d-4f54544f0000"
#define UUID_CMD        "7f3a0002-5e7a-4c0e-9b1d-4f54544f0000"  // write: text commands
#define UUID_CONFIG     "7f3a0003-5e7a-4c0e-9b1d-4f54544f0000"  // read/write: settings JSON
#define UUID_TELEM      "7f3a0004-5e7a-4c0e-9b1d-4f54544f0000"  // notify: live JSON
#define UUID_LOG        "7f3a0005-5e7a-4c0e-9b1d-4f54544f0000"  // notify: drive log binary
#define UUID_OTA        "7f3a0006-5e7a-4c0e-9b1d-4f54544f0000"  // write: firmware image chunks

#define FW_VERSION      "0.1.0"

// ---------------------------------------------------------------------------
// Colors (Otto design: silver body, ice-blue eyes; pink + yellow accents shared with Rotor)
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint8_t C_ICE[3]    = {124, 200, 255};   // #7CC8FF eyes, accents
constexpr uint8_t C_SILVER[3] = {214, 222, 232};   // #D6DEE8 piston body
constexpr uint8_t C_PINK[3]   = {255, 106, 162};
constexpr uint8_t C_YELLOW[3] = {244, 220,  75};
constexpr uint8_t C_BG[3]     = { 13,  17,  23};
constexpr uint8_t C_DIM[3]    = { 76, 122, 156};

#define COL_BG       rgb565(13, 17, 23)
#define COL_ICE      rgb565(124, 200, 255)
#define COL_SILVER   rgb565(214, 222, 232)
#define COL_SLEEPY   rgb565(138, 146, 156)   // body color while asleep
#define COL_ICE_DIM  rgb565(76, 122, 156)
#define COL_PINK     rgb565(255, 106, 162)
#define COL_YELLOW   rgb565(244, 220, 75)
#define COL_HOUSING  rgb565(58, 68, 82)
#define COL_GRID     rgb565(47, 58, 71)
#define COL_GRAY     rgb565(154, 167, 182)
#define COL_PANEL    rgb565(20, 25, 32)

// Mix two RGB888 colors and return RGB565. t = 0 -> a, 1 -> b
inline uint16_t mix565(const uint8_t a[3], const uint8_t b[3], float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return rgb565(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
}
// Same, but b is already an RGB565 color (e.g. COL_PINK or 0xFFFF for white)
inline uint16_t mix565(const uint8_t a[3], uint16_t b565, float t) {
  const uint8_t b[3] = {(uint8_t)(((b565 >> 11) & 0x1F) * 255 / 31),
                        (uint8_t)(((b565 >> 5) & 0x3F) * 255 / 63),
                        (uint8_t)((b565 & 0x1F) * 255 / 31)};
  return mix565(a, b, t);
}
