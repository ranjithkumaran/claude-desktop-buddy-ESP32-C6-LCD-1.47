#include "hw/display.h"
#include "hw/pins.h"
#include <Arduino.h>

static Arduino_DataBus*  s_bus    = nullptr;
#if BOARD_DISPLAY_CO5300
static Arduino_CO5300*   s_gfx    = nullptr;
#else
static Arduino_SH8601*   s_gfx    = nullptr;
#endif
static Arduino_Canvas*   s_canvas = nullptr;

static const uint8_t BRIGHT_LUT[5] = { 50, 100, 150, 200, 255 };

bool hwDisplayInit() {
  s_bus = new Arduino_ESP32QSPI(
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1,
    PIN_LCD_SDIO2, PIN_LCD_SDIO3);
#if BOARD_DISPLAY_CO5300
  // CO5300 ctor: (bus, rst, rotation, w, h, col_off1, row_off1, col_off2, row_off2)
  // col_offset1=6 per Waveshare demo (round panel RAM window offset).
  // Pass PIN_LCD_RESET so the driver does its own 200 ms hardware reset —
  // the 20 ms pulse from hwExpanderResetSequence() is too short for CO5300.
  s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RESET, 0,
                             LCD_W_PHYS, LCD_H_PHYS, 6, 0, 0, 0);
#else
  s_gfx = new Arduino_SH8601(s_bus, GFX_NOT_DEFINED, 0, LCD_W_PHYS, LCD_H_PHYS);
#endif
  s_canvas = new Arduino_Canvas(HW_W, HW_H, s_gfx);
  // canvas->begin() internally calls gfx->begin() which calls bus init.
  // Calling them separately would double-init the SPI bus → ESP_ERR_INVALID_STATE.
  if (!s_canvas->begin()) { Serial.println("hwDisplay: canvas begin failed"); return false; }
#if BOARD_DISPLAY_SH8601_VENDOR_INIT
  // The 2.16 panel revision needs extra vendor init beyond what
  // Arduino_SH8601's tftInit sends. Page-select dance + gamma /
  // brightness register configuration. Sequence from the LVGL v8
  // demo at .../02_Example/Arduino-v3.3.3/08_LVGL_V8_Test/bsp_lvgl_port.cpp.
  s_bus->beginWrite();
  s_bus->writeC8D8(0xFE, 0x20);  // page select MFR
  s_bus->writeC8D8(0x19, 0x10);
  s_bus->writeC8D8(0x1C, 0xA0);
  s_bus->writeC8D8(0xFE, 0x00);  // back to USER page
  s_bus->writeC8D8(0xC4, 0x80);
  s_bus->writeC8D8(0x3A, 0x55);  // pixel format (16bpp with control flags)
  s_bus->writeC8D8(0x35, 0x00);  // tearing line
  s_bus->writeC8D8(0x36, 0x30);  // MADCTL
  s_bus->writeC8D8(0x53, 0x20);  // CABC / dimming control
  s_bus->writeC8D8(0x51, 0xFF);  // brightness max
  s_bus->writeC8D8(0x63, 0xFF);
  s_bus->endWrite();
  delay(10);
#endif
  // UTF-8 decode for u8g2 CJK fonts — without this, print() treats each
  // byte of a multi-byte codepoint as its own glyph lookup → mojibake.
  s_canvas->setUTF8Print(true);
  s_gfx->setBrightness(0);   // black first frame to avoid white flash
  delay(20);
  s_gfx->setBrightness(150);  // default mid-brightness; main may override later
  return true;
}

Arduino_Canvas* hwCanvas() { return s_canvas; }

void hwDisplayBrightness(uint8_t lvl) {
  if (lvl > 4) lvl = 4;
  s_gfx->setBrightness(BRIGHT_LUT[lvl]);
}

void hwDisplaySleep(bool off) {
  if (off) {
    s_gfx->setBrightness(0);
    s_gfx->displayOff();
  } else {
    s_gfx->displayOn();
    hwDisplayBrightness(2);   // mid; caller usually re-applies stored level
  }
}

static uint16_t s_lineBuf[LCD_W_PHYS];   // one physical row, internal RAM
static bool     s_borderAlertOn = false;

extern "C" void hwBorderAlertSetInternal(bool on);  // forward decl from border.cpp

void hwBorderAlertSetInternal(bool on) { s_borderAlertOn = on; }

static const uint16_t BORDER_RED = 0xF800;

void hwDisplayPush() {
#ifdef DEBUG_SAFE_BOX
  // 1-px green outline at the logical safe-area boundary. Used to
  // empirically tune SAFE_INSET against the physical rounded bezel.
  s_canvas->drawRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, 0x07E0);
#endif
  uint16_t* src = (uint16_t*)s_canvas->getFramebuffer();
#if BOARD_DISPLAY_LETTERBOX
  // Bilinear scale HW_W×HW_H → DEST_W×DEST_H, centred in physical frame,
  // surround black. One-shot draw16bitRGBBitmap is required on CO5300 — per-row
  // draws leave the screen black (QSPI state issue when chaining many writes).
  //
  // RGB565 trick: split R|B (mask 0xF81F) and G (mask 0x07E0) into separate
  // 32-bit accumulators so one multiply-add handles both channels in parallel.
  // Weight is 5-bit (0..32); two successive blends (horizontal then vertical)
  // divide by 32×32 = 1024.
  static uint16_t* s_frameBuf = nullptr;
  if (!s_frameBuf) {
    s_frameBuf = (uint16_t*)heap_caps_malloc(
        LCD_W_PHYS * LCD_H_PHYS * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frameBuf) { Serial.println("hwDisplay: frameBuf alloc failed"); return; }
    memset(s_frameBuf, 0, LCD_W_PHYS * LCD_H_PHYS * sizeof(uint16_t));
  }
  constexpr int DEST_W = BOARD_DISPLAY_DEST_W;
  constexpr int DEST_H = BOARD_DISPLAY_DEST_H;
  constexpr int OFF_X  = (LCD_W_PHYS - DEST_W) / 2;
  constexpr int OFF_Y  = (LCD_H_PHYS - DEST_H) / 2;
  // Fixed-point scale: top 16 bits = integer source coord, next 5 = fraction.
  constexpr uint32_t SCALE_X = ((uint32_t)HW_W << 16) / DEST_W;
  constexpr uint32_t SCALE_Y = ((uint32_t)HW_H << 16) / DEST_H;

  for (int dy = 0; dy < DEST_H; dy++) {
    uint32_t sy_fp = (uint32_t)dy * SCALE_Y;
    int y0 = sy_fp >> 16;
    int y1 = (y0 + 1 < HW_H) ? y0 + 1 : y0;
    uint32_t fy = (sy_fp >> 11) & 0x1F;
    uint32_t inv_fy = 32 - fy;

    uint16_t* row0 = src + y0 * HW_W;
    uint16_t* row1 = src + y1 * HW_W;
    uint16_t* dstRow = s_frameBuf + (dy + OFF_Y) * LCD_W_PHYS + OFF_X;

    for (int dx = 0; dx < DEST_W; dx++) {
      uint32_t sx_fp = (uint32_t)dx * SCALE_X;
      int x0 = sx_fp >> 16;
      int x1 = (x0 + 1 < HW_W) ? x0 + 1 : x0;
      uint32_t fx = (sx_fp >> 11) & 0x1F;
      uint32_t inv_fx = 32 - fx;

      uint16_t p00 = row0[x0];
      uint16_t p01 = row0[x1];
      uint16_t p10 = row1[x0];
      uint16_t p11 = row1[x1];

      uint32_t t_rb = (p00 & 0xF81F) * inv_fx + (p01 & 0xF81F) * fx;
      uint32_t t_g  = (p00 & 0x07E0) * inv_fx + (p01 & 0x07E0) * fx;
      uint32_t b_rb = (p10 & 0xF81F) * inv_fx + (p11 & 0xF81F) * fx;
      uint32_t b_g  = (p10 & 0x07E0) * inv_fx + (p11 & 0x07E0) * fx;

      uint32_t rb = (t_rb * inv_fy + b_rb * fy) >> 10;
      uint32_t g  = (t_g  * inv_fy + b_g  * fy) >> 10;

      dstRow[dx] = (uint16_t)((rb & 0xF81F) | (g & 0x07E0));
    }
  }
  s_gfx->draw16bitRGBBitmap(0, 0, s_frameBuf, LCD_W_PHYS, LCD_H_PHYS);
#else
  // 2× integer upscale, per-row, with optional centring offset.
  // OFFSET_X/Y are 0 on full-fill boards (1.8); positive on boards
  // where the canvas is smaller than the panel (2.16 → 56/16).
  for (int y = 0; y < HW_H; y++) {
    uint16_t* row = src + y * HW_W;
    for (int x = 0; x < HW_W; x++) {
      uint16_t c = row[x];
      s_lineBuf[x*2]     = c;
      s_lineBuf[x*2 + 1] = c;
    }
    int dy = y * 2 + BOARD_DISPLAY_OFFSET_Y;
    s_gfx->draw16bitRGBBitmap(BOARD_DISPLAY_OFFSET_X, dy,     s_lineBuf, HW_W*2, 1);
    s_gfx->draw16bitRGBBitmap(BOARD_DISPLAY_OFFSET_X, dy + 1, s_lineBuf, HW_W*2, 1);
  }
#endif

  // Attention indicator: small red pill centered at the top of the
  // panel. Inset well below the rounded bezel corners. Less intrusive
  // than a full frame; reads as a "notification dot".
  if (s_borderAlertOn) {
    const int BAR_W = 200;
    const int BAR_H = 8;
    const int BAR_Y = 18;
    const int BAR_X = (LCD_W_PHYS - BAR_W) / 2;
    s_gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, BORDER_RED);
  }
}
