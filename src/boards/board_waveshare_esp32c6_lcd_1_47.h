// src/boards/board_waveshare_esp32c6_lcd_1_47.h
#pragma once

// Waveshare ESP32-C6-LCD-1.47.
// ESP32-C6FH4 (RISC-V single-core 160 MHz, 4 MB flash, NO PSRAM).
// 1.47" 172×320 ST7789 IPS LCD over SPI, no touch, single BOOT key (GPIO9).
// Canvas is 86×105 logical pixels, 2× upscaled per-row to 172×210 panel pixels,
// centred in the 320-tall panel with ~55 px black bars top + bottom. The 2×
// upscale matches the canvas-pixel-density of the AMOLED boards (which also
// upscale ×2) so u8g2 fonts stay legible on the tiny 1.47" panel — at native
// 1× the 8 px font rows came out ~0.5 mm and were unreadable.
//
// The BOOT key carries the entire UI: a tap = BtnA short (cycle), a
// medium hold (>600 ms) = BtnA long (open/close menu), a very long
// hold (>2 s) = BtnB short (confirm/deny/pet). See input.cpp's
// scanBootKey() under BOARD_INPUT_BOOT_ONLY.

#define LCD_W_PHYS  172
#define LCD_H_PHYS  320

#define BOARD_HW_W        86
#define BOARD_HW_H        160
#define BOARD_SAFE_INSET  3

// SPI to ST7789. Pin map from Waveshare ESP32-C6-LCD-1.47 wiki + AndroidCrypto
// reference setup. SD card shares MOSI=6, SCLK=7 (separate CS=4, MISO=5).
#define PIN_LCD_MOSI   6
#define PIN_LCD_SCLK   7
#define PIN_LCD_CS    14
#define PIN_LCD_DC    15
#define PIN_LCD_RESET 21
#define PIN_LCD_BL    22   // backlight enable (PWM-capable via LEDC)

// ST7789 172×320 IPS uses a 240-wide GRAM offset by 34 columns (no row offset).
#define BOARD_ST7789_COL_OFFSET  34
#define BOARD_ST7789_ROW_OFFSET  0
#define BOARD_ST7789_IPS         1

// Single physical key — ESP32-C6 standard BOOT pin.
#define PIN_KEY_BOOT  9

// No I2C peripherals — define safe dummy SDA/SCL so power.cpp / etc., which
// take these via Wire.begin() in hw.cpp, can still compile. hw.cpp will skip
// Wire init when there's no I2C-bus consumer.
#define PIN_I2C_SDA  (-1)
#define PIN_I2C_SCL  (-1)

// Capability flags
#define BOARD_HAS_PSRAM            0
#define BOARD_HAS_TCA9554          0
#define BOARD_HAS_PCF85063         0
#define BOARD_HAS_PA_CTRL          0
#define BOARD_HAS_AXP2101          0
#define BOARD_HAS_TOUCH            0
#define BOARD_HAS_AUDIO            0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_KEY1             0
#define BOARD_HAS_KEY2             0
#define BOARD_BTN_THIRD            1
#define BOARD_INPUT_BOOT_ONLY      1   // BOOT key carries 3-tier press semantics
#define BOARD_KEY1_ACTIVE_HIGH     0   // unused (no KEY1)
#define BOARD_BTN_SWAP_AB          0

// Display path
#define BOARD_DISPLAY_ST7789       1
#define BOARD_DISPLAY_SH8601_VENDOR_INIT  0
#define BOARD_DISPLAY_CO5300       0
#define BOARD_DISPLAY_LETTERBOX    0
#define BOARD_DISPLAY_PUSH_STREAMED  0
#define BOARD_DISPLAY_SCALE        2
#define BOARD_DISPLAY_OFFSET_X     0
#define BOARD_DISPLAY_OFFSET_Y     ((LCD_H_PHYS - BOARD_HW_H * BOARD_DISPLAY_SCALE) / 2)   // = 55
#define BOARD_DISPLAY_ROTATION     0
#define BOARD_DISPLAY_HAS_BL_PWM   1   // BL is a direct GPIO; PWM via LEDC

// PMU / reset gating — all off on this board
#define BOARD_LCD_RST_VIA_PMU      0
#define BOARD_AXP_PWRON_4S_OFF     0
#define BOARD_AXP_ENABLE_AUX_LDOS  0

// Irrelevant on this board but defined for build (referenced in display.cpp
// branches that are #if-skipped here).
#define BOARD_CO5300_COL_OFFSET    0
#define BOARD_CO5300_MADCTL        0

// Credits-page hardware identification (two short lines).
#define BOARD_MODEL_LINE1  "Waveshare ESP32-C6"
#define BOARD_MODEL_LINE2  "LCD 1.47\""
