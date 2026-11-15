#include "display.h"
#include "board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "esp_lcd_spd2010.h"   // bundled in lib/esp_lcd_spd2010
#include "freertos/semphr.h"

LGFX_Sprite canvas;

static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_panel_io_handle_t s_io = nullptr;
static SemaphoreHandle_t s_done = nullptr;

// Frames go out in stripes through small internal-RAM bounce buffers.
// The SPD2010 wants window edges on multiples of 4 (Waveshare's demo rounds every area that way):
// full-width rows keep x at 0..412, and 32-row stripes keep y on multiples of 4 (last stripe 28 rows).
static const int STRIPE = 32;
static uint16_t* s_bounce[2] = {nullptr, nullptr};

static float s_bl = 0, s_blTarget = 0;

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_done, &woken);
  return woken == pdTRUE;
}

bool displayInit() {
  // Backlight PWM (core 3.x API)
  ledcAttach(PIN_LCD_BL, 20000, 8);
  ledcWrite(PIN_LCD_BL, 0);

  // Hardware reset through the IO expander (same timing as Waveshare's config)
  boardExpanderWrite(EXIO_LCD_RST, false);
  delay(20);
  boardExpanderWrite(EXIO_LCD_RST, true);
  delay(120);

  s_done = xSemaphoreCreateCounting(2, 2);

  // Filled field by field: Espressif's SPD2010_PANEL_*_CONFIG macros use out-of-order
  // designated initializers, which are fine in C but rejected by the C++ compiler.
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_LCD_SCK;
  bus.data0_io_num = PIN_LCD_D0;
  bus.data1_io_num = PIN_LCD_D1;
  bus.data2_io_num = PIN_LCD_D2;
  bus.data3_io_num = PIN_LCD_D3;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = LCD_W * STRIPE * 2;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = PIN_LCD_CS;
  io_cfg.dc_gpio_num = -1;
  io_cfg.spi_mode = 3;             // SPD2010 QSPI uses mode 3
  io_cfg.pclk_hz = LCD_QSPI_HZ;
  io_cfg.trans_queue_depth = 10;
  io_cfg.on_color_trans_done = onColorDone;
  io_cfg.user_ctx = nullptr;
  io_cfg.lcd_cmd_bits = 32;        // QSPI: opcode + command packed in 32 bits
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = 1;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io) != ESP_OK) return false;

  spd2010_vendor_config_t vendor = {};   // no init_cmds: the driver's built-in vendor sequence
  vendor.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t pcfg = {};
  pcfg.reset_gpio_num = -1;  // reset is on the expander
  pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;   // if red/blue look swapped, use ..._BGR
  pcfg.bits_per_pixel = 16;
  pcfg.vendor_config = &vendor;
  if (esp_lcd_new_panel_spd2010(s_io, &pcfg, &s_panel) != ESP_OK) return false;
  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);
  esp_lcd_panel_disp_on_off(s_panel, true);

  for (int i = 0; i < 2; i++) {
    s_bounce[i] = (uint16_t*)heap_caps_malloc(LCD_W * STRIPE * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bounce[i]) return false;
  }

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(LCD_W, LCD_H)) return false;
  canvas.fillScreen(COL_BG);
  displayPush();
  return true;
}

// Send only part of the canvas (edges are multiples of 4, as the SPD2010 wants). Rows go through the
// same two DMA bounce buffers, as many per transfer as fit.
void displayPushRect(int x, int y, int w, int h) {
  int x0 = max(0, x & ~3), x1 = min(LCD_W, (x + w + 3) & ~3);
  int y0 = max(0, y & ~3), y1 = min(LCD_H, (y + h + 3) & ~3);
  if (x1 <= x0 || y1 <= y0) return;
  int bw = x1 - x0;
  int per = max(4, ((LCD_W * STRIPE) / bw) & ~3);
  const uint16_t* src = (const uint16_t*)canvas.getBuffer();
  int b = 0;
  for (int yy = y0; yy < y1; yy += per) {
    int rows = min(per, y1 - yy);
    xSemaphoreTake(s_done, portMAX_DELAY);
    for (int r = 0; r < rows; r++) memcpy(s_bounce[b] + r * bw, src + (yy + r) * LCD_W + x0, bw * 2);
    esp_lcd_panel_draw_bitmap(s_panel, x0, yy, x1, yy + rows, s_bounce[b]);
    b ^= 1;
  }
  xSemaphoreTake(s_done, portMAX_DELAY);
  xSemaphoreTake(s_done, portMAX_DELAY);
  xSemaphoreGive(s_done);
  xSemaphoreGive(s_done);
}

void displayPush() {
  // LovyanGFX stores 16-bit pixels byte-swapped (big-endian), which is what the panel expects.
  const uint16_t* src = (const uint16_t*)canvas.getBuffer();
  int b = 0;
  for (int y = 0; y < LCD_H; y += STRIPE) {
    int rows = min(STRIPE, LCD_H - y);
    xSemaphoreTake(s_done, portMAX_DELAY);  // wait until this bounce buffer is free
    memcpy(s_bounce[b], src + y * LCD_W, LCD_W * rows * 2);
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + rows, s_bounce[b]);
    b ^= 1;
  }
  // let both transfers finish before the next frame starts drawing
  xSemaphoreTake(s_done, portMAX_DELAY);
  xSemaphoreTake(s_done, portMAX_DELAY);
  xSemaphoreGive(s_done);
  xSemaphoreGive(s_done);
}

void displayBacklight(uint8_t level) { s_blTarget = level; }

void displayUpdateBacklight(float dt) {
  float k = dt * 4.0f;  // ~250 ms ease
  if (k > 1) k = 1;
  s_bl += (s_blTarget - s_bl) * k;
  ledcWrite(PIN_LCD_BL, (uint32_t)(s_bl + 0.5f));
}

void displayOff() {
  ledcWrite(PIN_LCD_BL, 0);
  if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
}
