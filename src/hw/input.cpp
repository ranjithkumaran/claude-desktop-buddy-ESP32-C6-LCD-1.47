#include "hw/input.h"
#include "hw/pins.h"
#include "hw/expander.h"
#include "hw/power.h"
#include <Arduino.h>

#if BOARD_HAS_TOUCH
  #if BOARD_TOUCH_CST92XX
    #include <Wire.h>
    #include "TouchDrvCSTXXX.hpp"
  #else
    #include <Arduino_DriveBus_Library.h>
  #endif
#endif

static HwBtn   s_a, s_b;
static HwTouch s_tp;
static uint8_t s_axpEvt = 0;

#if BOARD_HAS_TOUCH
  #if BOARD_TOUCH_CST92XX
  static TouchDrvCST92xx s_cst;
  #else
  static std::shared_ptr<Arduino_IIC_DriveBus> s_iicBus;
  static std::unique_ptr<Arduino_IIC>          s_ft3168;
  #endif
  static volatile bool                          s_tpIrqFlag = false;
  static void IRAM_ATTR onTouchIrq() { s_tpIrqFlag = true; }
#else
  // No touch hardware — s_tp stays default-constructed (down=false), and the
  // IRQ-pending peek always returns false. main.cpp's hwTouch()/hwTouchIrqPending()
  // callers will see an always-released finger.
  static const bool s_tpIrqFlag = false;
#endif

bool HwBtn::pressedFor(uint32_t ms) {
  return isPressed && (millis() - pressedAt) >= ms;
}

bool hwInputInit() {
#if BOARD_HAS_KEY1
  pinMode(PIN_KEY1, INPUT_PULLUP);   // GPIO0 has external pullup; INPUT_PULLUP is harmless
#endif
#if BOARD_HAS_KEY2
  pinMode(PIN_KEY2, INPUT_PULLUP);   // External R18 10K already pulls high; INPUT_PULLUP is harmless
#endif
#if BOARD_BTN_THIRD
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);   // External R8 10K already pulls high; INPUT_PULLUP is harmless
#endif

#if !BOARD_HAS_TOUCH
  return true;
#elif BOARD_TOUCH_CST92XX
  // CST92xx @ 0x5A via SensorLib. Reset is handled by hwExpanderResetSequence()
  // (TP_RST is shared with LCD_RESET on 1.75C), so pass rstPin=-1 to skip the
  // driver's internal reset — otherwise it would also re-reset the display.
  s_cst.setPins(-1, PIN_TP_INT);
  if (!s_cst.begin(Wire, 0x5A, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwInput: CST92xx init failed");
    return false;
  }
  Serial.printf("hwInput: CST92xx model=%s\n", s_cst.getModelName());
  s_cst.setMaxCoordinates(LCD_W_PHYS, LCD_H_PHYS);
  s_cst.setMirrorXY(true, true);
  pinMode(PIN_TP_INT, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_TP_INT), onTouchIrq, FALLING);
  return true;
#else
  s_iicBus = std::make_shared<Arduino_HWIIC>(PIN_I2C_SDA, PIN_I2C_SCL, &Wire);
  s_ft3168.reset(new Arduino_FT3x68(s_iicBus, FT3168_DEVICE_ADDRESS,
                                    DRIVEBUS_DEFAULT_VALUE, PIN_TP_INT, onTouchIrq));
  for (int i = 0; i < 5; i++) {
    if (s_ft3168->begin()) return true;
    delay(100);
  }
  Serial.println("hwInput: FT3168 init failed");
  return false;
#endif
}

#if BOARD_HAS_KEY1
static void scanKey1() {
  uint32_t now = millis();
#if BOARD_KEY1_ACTIVE_HIGH
  bool pressed = digitalRead(PIN_KEY1) == HIGH;
#else
  bool pressed = digitalRead(PIN_KEY1) == LOW;
#endif
  s_a.wasPressed  = pressed && !s_a.isPressed;
  s_a.wasReleased = !pressed && s_a.isPressed;
  if (s_a.wasPressed) s_a.pressedAt = now;
  s_a.isPressed = pressed;
}
#endif

#if BOARD_HAS_KEY2
static void scanKey2() {
  uint32_t now = millis();
  bool pressed = digitalRead(PIN_KEY2) == LOW;
  s_b.wasPressed  = pressed && !s_b.isPressed;
  s_b.wasReleased = !pressed && s_b.isPressed;
  if (s_b.wasPressed) s_b.pressedAt = now;
  s_b.isPressed = pressed;
}
#endif

#if BOARD_BTN_THIRD
// BOOT-key handling has two flavours.
//
// BOARD_INPUT_BOOT_ONLY=0 (existing C6-2.16 / S3-2.16):
//   BOOT is bonus — a short tap synthesises BtnA-long (legacy behaviour;
//   real BtnA / BtnB handle the rest). scanKey1() runs every frame and
//   re-derives s_a from the real PWR key, so the synthesised edges don't
//   persist beyond the synth frame.
//
// BOARD_INPUT_BOOT_ONLY=1 (C6-LCD-1.47, single button):
//   BOOT IS BtnA. We mirror the physical button into s_a directly:
//     - press edge   → isPressed=true, wasPressed=true, pressedAt=now
//     - held         → isPressed stays true, pressedAt unchanged (so main.cpp's
//                      pressedFor(600) fires live at 600 ms → menu opens
//                      DURING the hold, same UX as real BtnA on AMOLED boards)
//     - release edge → isPressed=false, wasReleased=true
//   BtnB doesn't exist physically; menu CONFIRM is reached by overriding
//   main.cpp's BtnA-long-when-panel-open behaviour to confirm instead of
//   close (guarded by BOARD_INPUT_BOOT_ONLY in main.cpp).
//
// Critical: without a separate scanKey1() running every frame to clear edges,
// scanBootKey() must clear s_a/s_b edge flags itself on EVERY scan, otherwise
// a one-tap event would persist and main.cpp would re-fire its release handler
// every frame (= pig flashing, menu thrashing — observed bug 2025-11).
static uint32_t s_bootPressedAt = 0;
static void scanBootKey() {
  bool pressed = digitalRead(PIN_KEY_BOOT) == LOW;
  uint32_t now = millis();

#if BOARD_INPUT_BOOT_ONLY
  // Single-frame edge pulses — clear every scan, set only on the event frame.
  s_a.wasPressed  = false;
  s_a.wasReleased = false;
  s_b.wasPressed  = false;
  s_b.wasReleased = false;

  if (pressed && !s_bootPressedAt) {
    // Press edge.
    s_bootPressedAt = now;
    s_a.isPressed   = true;
    s_a.wasPressed  = true;
    s_a.pressedAt   = now;
  } else if (pressed && s_bootPressedAt) {
    // Held — keep isPressed=true so pressedFor(600) accumulates naturally
    // from the original pressedAt. main.cpp opens the menu mid-hold at 600ms.
    s_a.isPressed = true;
  } else if (!pressed && s_bootPressedAt) {
    // Release edge.
    uint32_t held = now - s_bootPressedAt;
    s_bootPressedAt = 0;
    s_a.isPressed   = false;
    if (held >= 30) {
      // Above debounce floor — publish the release. main.cpp decides whether
      // the short-release action runs based on btnALong (set if pressedFor(600)
      // already fired mid-hold).
      s_a.wasReleased = true;
    }
  }
  // (!pressed && !s_bootPressedAt): idle — leave isPressed=false, no edges
#else
  // Legacy: BOOT short tap synthesises BtnA long-press only. scanKey1 will
  // clear these edges on the next frame.
  if (pressed && !s_bootPressedAt) {
    s_bootPressedAt = now;
  } else if (!pressed && s_bootPressedAt) {
    uint32_t held = now - s_bootPressedAt;
    s_bootPressedAt = 0;
    if (held > 30 && held < 1000) {
      s_a.wasPressed  = true;
      s_a.wasReleased = true;
      s_a.pressedAt   = now - 1500;
      s_a.isPressed   = false;
    }
  }
#endif
}
#endif

#if !BOARD_HAS_KEY2
static void scanAxp() {
#if BOARD_HAS_AXP2101
  if (hwExpanderAxpIrqLow()) {
    if (hwAxpPekeyShortPress()) s_axpEvt = 0x02;
    if (hwAxpPekeyLongPress())  s_axpEvt = 0x04;
  }
  // Route 0x02 to BtnB pulse for one frame:
  bool pressed = (s_axpEvt == 0x02);
  s_b.wasPressed  = pressed;
  s_b.wasReleased = pressed;
  s_b.isPressed   = false;
  if (pressed) s_axpEvt = 0;
  // 0x04 stays in s_axpEvt until consumed by hwAxpBtnEvent()
#else
  // No PMU: BtnB only sourced from scanBootKey() under BOARD_INPUT_BOOT_ONLY,
  // or simply never fires.
#endif
}
#endif

#if BOARD_HAS_TOUCH
static void scanTouch() {
  // Poll when IRQ fires OR when a finger was down last frame — both FT3168
  // and CST92xx only reliably IRQ on state edges, so a drag wouldn't advance
  // x/y without this.
  bool shouldPoll = s_tpIrqFlag || s_tp.down;
  s_tpIrqFlag = false;

  if (!shouldPoll) {
    s_tp.justPressed  = false;
    s_tp.justReleased = false;
    return;
  }

#if BOARD_TOUCH_CST92XX
  int16_t x[2] = {0}, y[2] = {0};
  uint8_t n = s_cst.getPoint(x, y, s_cst.getSupportTouchPoint());
  if (n > 0) {
    s_tp.justPressed  = !s_tp.down;
    s_tp.justReleased = false;
    // Mirror of hwDisplayPush's letterbox scale: reverse (physical → canvas).
    // Use BOARD_* (raw macros from board header) since display.h's constexpr
    // wrappers aren't visible here.
    #if BOARD_DISPLAY_LETTERBOX
      constexpr int OFF_X  = (LCD_W_PHYS - BOARD_DISPLAY_DEST_W) / 2;
      constexpr int OFF_Y  = (LCD_H_PHYS - BOARD_DISPLAY_DEST_H) / 2;
      int dx = x[0] - OFF_X;
      int dy = y[0] - OFF_Y;
      int tx = (dx * BOARD_HW_W) / BOARD_DISPLAY_DEST_W;
      int ty = (dy * BOARD_HW_H) / BOARD_DISPLAY_DEST_H;
      if (tx < 0) tx = 0; else if (tx >= BOARD_HW_W) tx = BOARD_HW_W - 1;
      if (ty < 0) ty = 0; else if (ty >= BOARD_HW_H) ty = BOARD_HW_H - 1;
      s_tp.x = tx;
      s_tp.y = ty;
    #else
      // Non-letterbox: physical → canvas via OFFSET subtract + scale downscale.
      // OFFSET is 0 on 1.8 (full-fill), 148/128 on 2.16 (centred 184×224 in 480×480).
      int dx = x[0] - BOARD_DISPLAY_OFFSET_X;
      int dy = y[0] - BOARD_DISPLAY_OFFSET_Y;
      int tx = dx / BOARD_DISPLAY_SCALE;
      int ty = dy / BOARD_DISPLAY_SCALE;
      if (tx < 0) tx = 0; else if (tx >= BOARD_HW_W) tx = BOARD_HW_W - 1;
      if (ty < 0) ty = 0; else if (ty >= BOARD_HW_H) ty = BOARD_HW_H - 1;
      s_tp.x = tx;
      s_tp.y = ty;
    #endif
    s_tp.down = true;
  } else {
    s_tp.justReleased = s_tp.down;
    s_tp.down = false;
    s_tp.justPressed  = false;
  }
#else
  uint8_t fingers = (uint8_t)s_ft3168->IIC_Read_Device_Value(
      Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  if (fingers > 0) {
    int rx = (int)s_ft3168->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int ry = (int)s_ft3168->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    s_tp.justPressed  = !s_tp.down;
    s_tp.justReleased = false;
    int dx = rx - BOARD_DISPLAY_OFFSET_X;
    int dy = ry - BOARD_DISPLAY_OFFSET_Y;
    int tx = dx / BOARD_DISPLAY_SCALE;
    int ty = dy / BOARD_DISPLAY_SCALE;
    if (tx < 0) tx = 0; else if (tx >= BOARD_HW_W) tx = BOARD_HW_W - 1;
    if (ty < 0) ty = 0; else if (ty >= BOARD_HW_H) ty = BOARD_HW_H - 1;
    s_tp.x = tx;
    s_tp.y = ty;
    s_tp.down = true;
  } else {
    s_tp.justReleased = s_tp.down;
    s_tp.down = false;
    s_tp.justPressed  = false;
  }
#endif
}
#else  // !BOARD_HAS_TOUCH
static inline void scanTouch() {
  // No touch hardware. Clear pulse flags so consumers (gesture/swipe
  // classifier in main.cpp) see a steady "not touching" state.
  s_tp.justPressed  = false;
  s_tp.justReleased = false;
  s_tp.down         = false;
}
#endif

void hwInputUpdate() {
#if BOARD_HAS_KEY1
  scanKey1();
#endif
#if BOARD_HAS_KEY2
  scanKey2();
#else
  scanAxp();
#endif
#if BOARD_BTN_THIRD
  scanBootKey();
#endif
  scanTouch();
}

#if BOARD_BTN_SWAP_AB
HwBtn& hwBtnA() { return s_b; }
HwBtn& hwBtnB() { return s_a; }
#else
HwBtn& hwBtnA() { return s_a; }
HwBtn& hwBtnB() { return s_b; }
#endif

uint8_t hwAxpBtnEvent() {
  uint8_t e = s_axpEvt;
  if (e == 0x04) s_axpEvt = 0;   // 0x02 already cleared by scanAxp
  return e;
}

const HwTouch& hwTouch() { return s_tp; }
bool hwTouchIrqPending() { return s_tpIrqFlag; }
