// src/hw/pins.h
#pragma once

#if defined(BOARD_WAVESHARE_ESP32S3_TOUCH_AMOLED_1_8)
  #include "../boards/board_waveshare_esp32s3_touch_amoled_1_8.h"
#elif defined(BOARD_WAVESHARE_ESP32S3_TOUCH_AMOLED_1_75C)
  #include "../boards/board_waveshare_esp32s3_touch_amoled_1_75c.h"
#elif defined(BOARD_WAVESHARE_ESP32C6_TOUCH_AMOLED_2_16)
  #include "../boards/board_waveshare_esp32c6_touch_amoled_2_16.h"
#elif defined(BOARD_WAVESHARE_ESP32S3_TOUCH_AMOLED_2_16)
  #include "../boards/board_waveshare_esp32s3_touch_amoled_2_16.h"
#elif defined(BOARD_WAVESHARE_ESP32C6_LCD_1_47)
  #include "../boards/board_waveshare_esp32c6_lcd_1_47.h"
#else
  #error "No board defined. Add -DBOARD_WAVESHARE_ESP32S3_TOUCH_AMOLED_1_8, _1_75C, _ESP32C6_TOUCH_AMOLED_2_16, _ESP32S3_TOUCH_AMOLED_2_16, or _ESP32C6_LCD_1_47 to build_flags in platformio.ini."
#endif

// ── Capability-flag defaults ───────────────────────────────────────────────
// New flags introduced when porting the C6-LCD-1.47 (no touch / no audio /
// no IMU / no PMU). Defaults preserve the four AMOLED boards' behaviour
// (they all have these peripherals), so only boards that *lack* a feature
// need to set the flag to 0 in their header.
#ifndef BOARD_HAS_TOUCH
  #define BOARD_HAS_TOUCH  1
#endif
#ifndef BOARD_HAS_AUDIO
  #define BOARD_HAS_AUDIO  1
#endif
#ifndef BOARD_HAS_IMU
  #define BOARD_HAS_IMU    1
#endif
#ifndef BOARD_HAS_KEY1
  #define BOARD_HAS_KEY1   1
#endif
#ifndef BOARD_INPUT_BOOT_ONLY
  #define BOARD_INPUT_BOOT_ONLY  0
#endif
#ifndef BOARD_DISPLAY_ST7789
  #define BOARD_DISPLAY_ST7789   0
#endif
#ifndef BOARD_DISPLAY_HAS_BL_PWM
  #define BOARD_DISPLAY_HAS_BL_PWM  0
#endif

// Derived flag: is the I2C bus used by any peripheral on this board?
// hw.cpp uses this to skip Wire.begin() when nothing is attached.
#define BOARD_HAS_I2C  (BOARD_HAS_TOUCH || BOARD_HAS_AXP2101 || \
                        BOARD_HAS_PCF85063 || BOARD_HAS_TCA9554 || \
                        BOARD_HAS_IMU || BOARD_HAS_AUDIO)
