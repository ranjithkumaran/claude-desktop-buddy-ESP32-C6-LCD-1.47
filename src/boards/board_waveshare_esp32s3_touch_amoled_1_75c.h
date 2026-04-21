// src/boards/board_waveshare_esp32s3_touch_amoled_1_75c.h
#pragma once

// Display: 466×466 round AMOLED (SH8601, assumed same driver as 1.8")
// Physical 466×466 is circular; corners are outside the visible area.
// Keep name LCD_W_PHYS / LCD_H_PHYS — display.cpp uses these directly via pins.h.
#define LCD_W_PHYS  466
#define LCD_H_PHYS  466

// Logical canvas (half of physical — upscale done in hwDisplayPush).
// BOARD_ prefix required: see note in 1.8" header about constexpr self-reference.
#define BOARD_HW_W        233
#define BOARD_HW_H        233

// Safe area: rectangle inscribed in the display circle.
// Calculated: radius=116.5, inscribed square side = 116.5×√2 ≈ 164 px.
// inset = (233-164)/2 ≈ 35. Calibrate with DEBUG_SAFE_BOX on device.
#define BOARD_SAFE_INSET  35

// QSPI to SH8601
#define PIN_LCD_SDIO0  4
#define PIN_LCD_SDIO1  5
#define PIN_LCD_SDIO2  6
#define PIN_LCD_SDIO3  7
#define PIN_LCD_SCLK   38
#define PIN_LCD_CS     12

// LCD and TP reset are direct GPIOs (no TCA9554 on this board)
#define PIN_LCD_RESET  1
#define PIN_TP_RESET   2

// LCD tearing-effect output (not used yet; reserved)
#define PIN_LCD_TE     13

// I2C bus (shared: AXP2101, FT3168, ES8311, QMI8658C)
#define PIN_I2C_SDA   15
#define PIN_I2C_SCL   14

// FT3168 touch interrupt — direct to ESP32
#define PIN_TP_INT    11

// I2S to ES8311 codec (identical to 1.8")
#define PIN_I2S_MCLK  16
#define PIN_I2S_BCLK  9
#define PIN_I2S_WS    45
#define PIN_I2S_DI    10
#define PIN_I2S_DO    8
#define PIN_PA_CTRL   46

// Buttons
#define PIN_KEY1      0   // GPIO0 BOOT key, active-low

// No TCA9554 expander on this board
#define BOARD_HAS_TCA9554  0
// AXP_IRQ is not wired to any ESP32 GPIO on the 1.75C; AXP PEK events
// are detected by polling AXP registers via I2C every frame instead.
