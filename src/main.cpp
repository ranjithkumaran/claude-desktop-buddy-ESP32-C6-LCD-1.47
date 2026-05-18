// Arduino_GFX's font headers reference an undefined U8G2_FONT_SECTION
// macro — provide a no-op so the const array compiles. The font symbol
// itself is gated on U8G2_USE_LARGE_FONTS (set in build_flags).
#define U8G2_FONT_SECTION(name)
#include <Arduino_GFX_Library.h>

#include "hw/hw.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_mac.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

// TFT_eSPI used to define these named colors; Arduino_GFX uses
// RGB565_*. Keep the names so existing UI code compiles unchanged.
#define GREEN  0x07E0
#define RED    0xF800
#define BLUE   0x001F
#define YELLOW 0xFFE0
#define WHITE  0xFFFF
#define BLACK  0x0000

// spr is a thin alias for hwCanvas() — keeps existing UI code unchanged
#define spr (*hwCanvas())

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = HW_W;
const int H = HW_H;

// Narrow-canvas boards (1.47" LCD) use the chill7 u8g2 font as the default
// everywhere — same font that drawHud's "No Claude connected" line uses, which
// reads cleanly on the small 172×320 panel where the default Adafruit GFX
// 6×8 font (rendered ~16 px tall after 2× upscale) is too chunky. Wider
// AMOLED boards keep the Adafruit default. Replace bare setFont(NULL) calls
// with this helper so the global font choice survives every draw pass.
static inline void setDefaultFont() {
#if BOARD_HW_W < 120
  spr.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);
#else
  spr.setFont((const GFXfont*)NULL);
#endif
}
const int CX = W / 2;
const int CY_BASE = 120;
// LED replaced by AMOLED border-flash via hwBorderAlert() — no GPIO LED.

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
// 0..4 → ScreenBreath 20..100. Default to brightest on AMOLED boards, dimmest
// on the 1.47" LCD — its backlight + LDO run noticeably hot at full PWM and the
// panel is already overbright in a normal indoor setting at level 0.
#if BOARD_HW_W < 120
uint8_t brightLevel = 0;
#else
uint8_t brightLevel = 4;
#endif
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

static void prevPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → last species
    buddyMode = true;
    buddySetSpeciesIdx(n - 1);
    speciesIdxSave(n - 1);
  } else if (buddySpeciesIdx() == 0 && gifAvailable) {      // first species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyPrevSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS    = 30UL * 1000UL;        // 30s on battery, non-clock idle
const uint32_t CLOCK_OFF_MS_BAT = 5UL  * 60UL * 1000UL; // 5min on battery, clock visible

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  hwImuAccel(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { hwDisplayBrightness(brightLevel); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    hwDisplaySleep(false);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) hwBeep(freq, dur);
}

// Touch hit-test helper (additive: keys still work, touch is a 2nd path).
// Returns true on a fresh tap-down inside the rect; releases don't fire.
static bool tap(int x, int y, int w, int h) {
  const HwTouch& t = hwTouch();
  return t.justPressed && t.x >= x && t.y >= y && t.x < x+w && t.y < y+h;
}

// Press-start snapshot for gesture classification (swipe). Updated on every
// justPressed; read on justReleased to compute Δx/Δy/Δt.
static int16_t  _tpStartX = 0, _tpStartY = 0;
static uint32_t _tpStartMs = 0;

// Rect hit-test against the press-START position. Use on justReleased after
// a gesture has been classified as a stationary tap — so a tap with minor
// finger drift still targets the region the user pressed on.
static bool tappedFrom(int x, int y, int w, int h) {
  return _tpStartX >= x && _tpStartY >= y &&
         _tpStartX <  x + w && _tpStartY <  y + h;
}

// After a user interaction in clock mode (pet tap or species swipe), keep the
// buddy awake for this long — otherwise the time-of-day logic snaps back to
// P_SLEEP the instant the one-shot animation expires.
static const uint32_t PLAYFUL_MS = 3UL * 60UL * 1000UL;
static uint32_t _playfulUntil = 0;

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillScreen(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

// Swipe cycles through all 9 pages as a flat list:
//   Normal → Pet 1/2 → Pet 2/2 → Info 1/6 → … → Info 6/6 → (wrap to Normal)
// Key1 short-press keeps the coarser 3-mode cycle; these helpers are only
// wired into the release-based gesture classifier below.
// applyDisplayMode() fires on mode transitions and Pet sub-page (matches
// existing BtnB behaviour). Info sub-page skips it because drawInfo() clears
// its own region — also matches existing BtnB behaviour.
static void swipeNextPage() {
  if (displayMode == DISP_NORMAL) {
    displayMode = DISP_PET; petPage = 0;                  applyDisplayMode();
  } else if (displayMode == DISP_PET) {
    if (petPage + 1 < PET_PAGES)    { petPage++;          applyDisplayMode(); }
    else { displayMode = DISP_INFO; infoPage = 0;         applyDisplayMode(); }
  } else { /* DISP_INFO */
    if (infoPage + 1 < INFO_PAGES)  { infoPage++; }
    else { displayMode = DISP_NORMAL;                     applyDisplayMode(); }
  }
}

static void swipePrevPage() {
  if (displayMode == DISP_NORMAL) {
    displayMode = DISP_INFO; infoPage = INFO_PAGES - 1;   applyDisplayMode();
  } else if (displayMode == DISP_PET) {
    if (petPage > 0)                { petPage--;          applyDisplayMode(); }
    else { displayMode = DISP_NORMAL;                     applyDisplayMode(); }
  } else { /* DISP_INFO */
    if (infoPage > 0)               { infoPage--; }
    else { displayMode = DISP_PET; petPage = PET_PAGES - 1; applyDisplayMode(); }
  }
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  // Narrow boards: panel must fit inside HW_W (86 on 1.47" LCD) or it extends
  // off-canvas to the left, clipping the leading "> S" of each row. Row step
  // also tightens 14→12 so 10 settings rows + header + hints fit inside the
  // 160-tall canvas (would otherwise clip top/bottom).
  const int STEP = (W < 120) ? 12 : 14;
  int mw = (W < 120) ? W - 6 : 118;
  int mh = 16 + SETTINGS_N * STEP + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * STEP);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * STEP);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  const int STEP = (W < 120) ? 12 : 14;
  int mw = (W < 120) ? W - 6 : 118;
  int mh = 16 + RESET_N * STEP + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * STEP);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: hwPowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  const int STEP = (W < 120) ? 12 : 14;
  int mw = (W < 120) ? W - 6 : 118;
  int mh = 16 + MENU_N * STEP + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * STEP);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Portrait-only clock on AMOLED port (landscape removed — 368×448 is
// near-square; rotating doesn't change the layout meaningfully).
static HwTime  _clkTm;
uint32_t       _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool    _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = hwBattery().usbPresent;
  hwRtcRead(&_clkTm);
}

// Clock face: shown when charging on USB with nothing else going on.
// Paints the upper ~110px to the canvas; pet renders below.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkTm.dow % 7; }
// Manual centered-text helper (Arduino_GFX has no setTextDatum). Uses
// getTextBounds so the centering math is correct regardless of which font
// is active — important on narrow boards where chill7 is the global default
// and the old strlen()*6 width formula (assuming the Adafruit 6×8 font) is
// wrong. With chill7's narrower proportional glyphs, strings that overran
// an 86-px canvas (e.g. "a buddy appears" = 90 px at 6/char) now fit.
static void drawCenteredText(const char* s, int cx, int cy, int sz, uint16_t fg, uint16_t bg) {
  spr.setTextSize(sz);
  spr.setTextColor(fg, bg);
  int16_t bx, by; uint16_t bw, bh;
  spr.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  // getTextBounds returns (bx, by) as the offset from the cursor to the top-
  // left of the rendered glyph cluster, so subtract those to land the cluster
  // centred on (cx, cy).
  spr.setCursor(cx - bw/2 - bx, cy - bh/2 - by);
  spr.print(s);
  spr.setTextSize(1);
}
static void drawClock() {
  const Palette& p = characterPalette();
  char hms[12]; snprintf(hms, sizeof(hms), "%02u:%02u:%02u", _clkTm.H, _clkTm.M, _clkTm.S);
  uint8_t mi = (_clkTm.Mo >= 1 && _clkTm.Mo <= 12) ? _clkTm.Mo - 1 : 0;
  char dl[16]; snprintf(dl, sizeof(dl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkTm.D);

  // Compact clock: single-line HH:MM:SS plus date below. Clears only
  // y >= 140 so the buddy at full home scale (reaches y≈126) fits
  // entirely above. Wider canvas + portrait orientation has plenty of
  // horizontal room for HH:MM:SS at size 3 (8 chars × 18 = 144 px).
#if BOARD_HW_W < 120
  // Narrow canvas (86×160): the buddy GIF sits at y≈45–94. The original
  // y=160 / SAFE_B-21 layout puts HH:MM:SS off the bottom and lets the date
  // overdraw uncleared space above the fill rect. Re-position both centres
  // safely inside the 160 px canvas and clear the whole region below the
  // buddy so stale pixels don't bleed through.
  spr.fillRect(0, 100, W, H - 100, p.bg);
  drawCenteredText(hms, CX, 118, 1, p.text,    p.bg);
  drawCenteredText(dl,  CX, 140, 1, p.textDim, p.bg);
#else
  spr.fillRect(0, 140, W, H - 140, p.bg);
  drawCenteredText(hms, CX, 160, 3, p.text,    p.bg);
  drawCenteredText(dl,  CX, SAFE_B - 21, 1, p.textDim, p.bg);
#endif
  spr.setTextSize(1);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  hwImuAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
//
// On narrow boards drawInfo() scrolls the body by passing a pre-decremented
// y; the header is part of the same scrolled flow, so it has to honour the
// same "below TOP only" clip as drawInfo's ln() — otherwise the title row
// would scroll up into the pig area as the page advances.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
#if BOARD_HW_W < 120
  const int INFO_CLIP_TOP = 70 + 7;   // mirrors drawInfo's TOP + chill7 ascent
#else
  const int INFO_CLIP_TOP = -1000000;   // effectively no clip
#endif
  if (y >= INFO_CLIP_TOP) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(SAFE_L, y); spr.print("Info");
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(SAFE_R - 24, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  }
  y += 12;
  if (y >= INFO_CLIP_TOP) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(SAFE_L, y); spr.print(section);
  }
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillScreen(p.bg);
  spr.setTextSize(1);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
#if BOARD_HW_W < 120
  // Narrow canvas (86 px): 6 digits at default-font size 3 = 108 px wide;
  // the leading digit clipped off the left. Drop to size 2 (72 px) and
  // route labels through drawCenteredText so chill7 widths land centered.
  drawCenteredText("BLUETOOTH PAIRING", W/2, 24,    1, p.textDim, p.bg);
  drawCenteredText("enter on desktop:",  W/2, H - 30, 1, p.textDim, p.bg);
  // Default Adafruit font for the digits so setTextSize(2) actually scales
  // them; chill7 ignores textSize, which would have kept the digits tiny.
  spr.setFont((const GFXfont*)NULL);
  drawCenteredText(b, W/2, H/2, 2, p.text, p.bg);
  setDefaultFont();
#else
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(SAFE_L, SAFE_B - 32); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
#endif
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);

  // Auto-scroll: needed on narrow canvases where info pages run longer than
  // the visible window (90 px on the 1.47" LCD). Hold at the top for ~1.5 s,
  // scroll down at ~17 px/s, hold at the bottom ~1.5 s, restart. On wider
  // canvases the offset stays at 0 because content fits.
  static int     s_infoMaxY[INFO_PAGES] = {0};
  static uint8_t s_infoLastPage         = 0xFF;
  static uint32_t s_infoScrollT0        = 0;
  if (infoPage != s_infoLastPage) {
    s_infoScrollT0 = millis();
    s_infoLastPage = infoPage;
  }
  const int viewH      = H - TOP;
  const int contentH   = s_infoMaxY[infoPage];
  const int DWELL_PX   = 24;     // ~1.4 s at 60 ms/px
  int scroll = 0;
  if (contentH > viewH) {
    int overflow = contentH - viewH + 8;
    int cycle    = overflow + DWELL_PX * 2;
    int t        = (int)((millis() - s_infoScrollT0) / 60) % cycle;
    if (t < DWELL_PX)                   scroll = 0;
    else if (t > DWELL_PX + overflow)   scroll = overflow;
    else                                scroll = t - DWELL_PX;
  }
  // Start one chill7 line height below TOP so the first frame's header sits
  // fully inside the scroll region (baseline at TOP+8 → glyph spans TOP+1..
  // TOP+8, no overlap with the pig area). The strict y ≥ TOP+7 clip below
  // matches _infoHeader's INFO_CLIP_TOP so headers and body share the same
  // "fully below pig" rule as the page scrolls.
  int y = TOP + 8 - scroll;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (y >= TOP + 7 && y < H) { spr.setCursor(SAFE_L, y); spr.print(b); }
    y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

#if BOARD_HAS_AXP2101
    HwBattery hb = hwBattery();
    int vBat_mV  = hb.mV;
    int iBat_mA  = hb.mA;       // always 0 on AXP2101 (current not exposed)
    int vBus_mV  = hb.usbPresent ? 5000 : 0;
    int pct      = hb.pct;
    bool usb     = hb.usbPresent;
    bool charging = hb.charging;
    bool full    = usb && vBat_mV > 4100 && !charging;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(SAFE_L, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;
#else
    // No PMU on this board (e.g. 1.47" LCD). Skip the battery readouts —
    // hwBattery() returns all zeros, so "0%" and "0.00V" would be misleading.
    spr.setTextColor(p.textDim, p.bg);
    ln("  power    USB");
    y += 8;
#endif

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
#if BOARD_HAS_AXP2101
    ln("  temp     %dC", (int)hwBattery().tempC);
#endif

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    ln("%s", linked ? "linked" : (settings().bt ? "discover" : "off"));
    y += 4;

    spr.setTextColor(p.text, p.bg);
    ln("%s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("%02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 4;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln("Open Claude desktop");
      ln("> Developer");
      ln("> Hardware Buddy");
      y += 4;
      ln("auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware adaptation");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("yadong");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln(BOARD_MODEL_LINE1);
    ln(BOARD_MODEL_LINE2);
  }
  // Record total content height so the next frame can decide whether scroll
  // is needed (and how far). y is currently the cursor after the last line,
  // expressed in scrolled-canvas coords; add scroll back to recover absolute.
  s_infoMaxY[infoPage] = (y + scroll) - TOP;
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
// UTF-8 continuation byte = 0b10xxxxxx. Pull `take` back so we never
// land mid-codepoint when hard-breaking long Chinese sentences.
static uint8_t _utf8SafeTake(const char* w, uint8_t take, uint8_t wlen) {
  if (take == 0 || take >= wlen) return take;
  while (take > 0 && ((uint8_t)w[take] & 0xC0) == 0x80) take--;
  return take;
}

static uint8_t wrapInto(const char* in, char out[][48], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit, on UTF-8 char boundaries
    while (wlen > width - col) {
      uint8_t take = _utf8SafeTake(w, width - col, wlen);
      if (take == 0) take = 1;                 // safety: avoid infinite loop
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(SAFE_L, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(SAFE_L, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(SAFE_L, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(SAFE_L, SAFE_B - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(SAFE_L, SAFE_B - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(SAFE_R - 48, SAFE_B - 12);
    spr.print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  // Narrow-canvas layout (1.47" LCD): TOP is pushed up + line steps tightened
  // so the full 9-element stats column (mood / fed / energy / level / approved /
  // denied / napped / tokens / today) fits in the 160-tall canvas instead of
  // running off the bottom. AMOLED boards (HW_W ≥ 120) keep TOP=70 + step=20.
  //
  // TY = baseline-vs-top-left offset for the active text font: u8g2 fonts
  // (chill7 on narrow) anchor by glyph baseline, Adafruit GFX default font
  // anchors by top-left. Adding TY to text setCursor y values keeps the same
  // visual position regardless of which font is active.
#if BOARD_HW_W >= 120
  const int TOP = 70, STEP = 20, LVL_STEP = 24, SUB = 10, TY = 0;
#else
  const int TOP = 24, STEP = 14, LVL_STEP = 18, SUB = 9, TY = 7;
  spr.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);
#endif
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  // Narrow-canvas HUD has tighter indicator spacing. The wide layout positions
  // (54/+16 hearts, 38/+9 fed dots, 54/+13 energy bars) assume the AMOLED
  // boards' 184-wide canvas; on the 1.47" LCD (canvas 86 wide) hearts 2/3
  // and the right half of fed/energy fall off the edge.
#if BOARD_HW_W >= 120
  constexpr int HUD_MOOD_X   = 54, HUD_MOOD_STEP = 16;
  constexpr int HUD_FED_X    = 38, HUD_FED_STEP  = 9;
  constexpr int HUD_EN_X     = 54, HUD_EN_STEP   = 13;
  constexpr int HUD_EN_RECT_W = 9;
#else
  constexpr int HUD_MOOD_X   = 30, HUD_MOOD_STEP = 11;
  constexpr int HUD_FED_X    = 22, HUD_FED_STEP  = 6;
  constexpr int HUD_EN_X     = 36, HUD_EN_STEP   = 9;
  constexpr int HUD_EN_RECT_W = 6;
#endif

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, y - 2 + TY); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(HUD_MOOD_X + i * HUD_MOOD_STEP, y + 2, i < mood, moodCol);

  y += STEP;
  spr.setCursor(SAFE_L, y - 2 + TY); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = HUD_FED_X + i * HUD_FED_STEP;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += STEP;
  spr.setCursor(SAFE_L, y - 2 + TY); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = HUD_EN_X + i * HUD_EN_STEP;
    if (i < en) spr.fillRect(px, y - 2, HUD_EN_RECT_W, 6, enCol);
    else spr.drawRect(px, y - 2, HUD_EN_RECT_W, 6, p.textDim);
  }

  y += LVL_STEP;
  spr.fillRoundRect(SAFE_L, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(SAFE_L + 5, y + 1 + TY); spr.printf("Lv %u", stats().level);

  y += STEP;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, y + TY);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(SAFE_L, y + SUB + TY);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(SAFE_L, y + SUB * 2 + TY);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(SAFE_L, yPx + TY);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + SUB * 3);
  tokFmt("today    ", tama.tokensToday, y + SUB * 4);

#if BOARD_HW_W < 120
  setDefaultFont();   // restore default font for subsequent drawers
#endif
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(SAFE_L, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  // Header y matches drawPetStats' TOP so the title/page-counter line sits at
  // the top of the cleared stats region (above the indicator rows).
#if BOARD_HW_W >= 120
  int y = 70;
#else
  int y = 24;
#endif

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right.
  // Narrow boards use chill7 to match the body font; add 7 to setCursor y to
  // compensate for chill7's baseline anchoring vs default font's top-left.
  spr.setTextSize(1);
#if BOARD_HW_W < 120
  spr.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);
  const int HDR_TY = 7;
#else
  const int HDR_TY = 0;
#endif
  // Build the title once so we can measure + place the counter dynamically.
  char title[40];
  if (ownerName()[0]) snprintf(title, sizeof(title), "%s's %s", ownerName(), petName());
  else                snprintf(title, sizeof(title), "%s", petName());

  // Counter is right-aligned via getTextBounds so it always sits at SAFE_R
  // regardless of font. The old fixed SAFE_R-24 offset assumed default-font
  // widths and the AMOLED 184-px canvas, so on narrow chill7 boards the
  // counter landed inside the title text — "Ranjith's Buddy2/2" with no gap.
  char ctr[12]; snprintf(ctr, sizeof(ctr), "%u/%u", petPage + 1, PET_PAGES);
  int16_t bx, by; uint16_t cw, ch;
  spr.getTextBounds(ctr, 0, 0, &bx, &by, &cw, &ch);
  int counterX = SAFE_R - (int)cw;

  // Truncate the title so it can't overrun the counter. Drop chars from the
  // end one at a time until the rendered width fits, leaving a small gap.
  uint16_t tw, th;
  spr.getTextBounds(title, 0, 0, &bx, &by, &tw, &th);
  int maxTitleW = counterX - SAFE_L - 4;
  while ((int)tw > maxTitleW && strlen(title) > 1) {
    title[strlen(title) - 1] = 0;
    spr.getTextBounds(title, 0, 0, &bx, &by, &tw, &th);
  }

  spr.setTextColor(p.text, p.bg);
  spr.setCursor(SAFE_L, y + 2 + HDR_TY);
  spr.print(title);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(counterX, y + 2 + HDR_TY);
  spr.print(ctr);
#if BOARD_HW_W < 120
  setDefaultFont();
#endif
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  // chill7 font: glyphs ~7 px tall but baseline-positioned (setCursor
  // is the baseline, not the top). Allow ~10 px line spacing, ~22 byte
  // budget per line — Chinese chars are ~7 px wide, ASCII ~5 px, so a
  // mixed line of 22 bytes (~7 Chinese OR 22 ASCII) fits W=184.
  const int SHOW = 3, LH = 10, WIDTH = 22;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);

  // Menu/settings/reset should hide the HUD strip underneath — panels are
  // centered and don't cover the bottom 34 px on their own.
  if (menuOpen || settingsOpen || resetOpen) return;

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  // buddy/character ticks leave textsize at 2 (home scale); without
  // pinning it here the CJK font alternates between 1× and 2× every tick.
  spr.setTextSize(1);
  spr.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(SAFE_L, SAFE_B - 4);
    spr.print(tama.msg);
    setDefaultFont();
    return;
  }

  static char disp[32][48];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(SAFE_L, H - AREA + 8 + i * LH);   // +8 = baseline offset for 7-px font
    spr.print(disp[row]);
  }

  setDefaultFont();

  if (msgScroll > 0) {
    spr.setTextSize(1);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(SAFE_R - 18, SAFE_B - 10);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  hwInit();                  // Wire + expander + display + power + input + IMU + RTC + audio
  setDefaultFont();          // narrow boards switch to chill7; AMOLEDs keep Adafruit default
  startBt();                 // BLE stays always-on
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  characterInit(nullptr);    // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF).
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      drawCenteredText(line,      W/2, H/2 - 12, 2, p.text, p.bg);
      drawCenteredText(petName(), W/2, H/2 + 12, 2, p.body, p.bg);
    } else {
      drawCenteredText("Hello!",          W/2, H/2 - 12, 2, p.body,    p.bg);
      drawCenteredText("a buddy appears", W/2, H/2 + 12, 1, p.textDim, p.bg);
    }
    spr.setTextSize(1);
    hwDisplayPush();
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  hwInputUpdate();
  ;
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Attention indicator: AMOLED red border flash (replaces M5StickC LED).
  hwBorderAlert(activeState == P_ATTENTION && settings().led
                && (now / 400) % 2 == 0);

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (hwBtnA().isPressed || hwBtnB().isPressed) {
    if (screenOff) {
      if (hwBtnA().isPressed) swallowBtnA = true;
      if (hwBtnB().isPressed) swallowBtnB = true;
    }
    wake();
  }

  // Key3 long-press (~1s, AXP IRQ 0x04) toggles screen off — replaces
  // M5StickC's PWR short-press behaviour (we only have 2 buttons; short
  // press of Key3 is BtnB, so screen-toggle moves to long-press).
  // Very-long-press (6s) still powers off via AXP hardware.
  if (hwAxpBtnEvent() == 0x04) {
    if (screenOff) {
      wake();
    } else {
      hwDisplaySleep(true);
      screenOff = true;
    }
  }

  if (hwBtnA().pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
#if BOARD_INPUT_BOOT_ONLY
    // Single-button board: BtnA-long inside a panel CONFIRMS the current
    // selection (the role that BtnB-short plays on the other boards). Outside
    // any panel it still opens the menu. Without this, holding the only
    // physical button to confirm would instead close the menu.
    //
    // Also: hold-on-prompt = DENY. There's no BtnB to reach the deny path
    // otherwise, so a long press while a permission prompt is up sends the
    // deny decision (mirrors the BtnB-short handler in the wide-board path).
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    }
    else if (resetOpen)    applyReset(resetSel);
    else if (settingsOpen) applySetting(settingsSel);
    else if (menuOpen)     menuConfirm();
    else {
      menuOpen = true;
      menuSel  = 0;
    }
#else
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
#endif
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (hwBtnA().wasReleased) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
#if BOARD_INPUT_BOOT_ONLY
        // Single-button board has no BtnB to advance pages within a mode
        // (info 1/6 → 2/6, pet 1/2 → 2/2). swipeNextPage already implements
        // the flat 9-page cycle that covers everything, so a short BtnA tap
        // walks through all of it.
        swipeNextPage();
#else
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
#endif
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (hwBtnB().wasPressed) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // ─── Touch (additive — buttons above already handled) ──────────────
  // Clocking = idle home screen with RTC synced; drives gesture routing:
  // HUD (!clocking) gets tap-to-pet, clocking gets horizontal-swipe-to-switch-species.
  bool tpClocking = displayMode == DISP_NORMAL
                 && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
                 && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
                 && dataRtcValid();

  const HwTouch& tp = hwTouch();
  if (tp.justPressed) { _tpStartX = tp.x; _tpStartY = tp.y; _tpStartMs = millis(); }

  // Approval: tap upper half of the approval area = approve,
  //           tap lower half = deny.
  if (inPrompt) {
    const int APPROVAL_TOP = H - 78;
    if (tap(0, APPROVAL_TOP,      W, 39)) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    }
    if (tap(0, APPROVAL_TOP + 39, W, 39)) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    }
  } else if (menuOpen || settingsOpen || resetOpen) {
    // Tap a menu row → directly select + confirm. Reuses the layout
    // constants from drawMenu/drawSettings/drawReset.
    int n      = menuOpen ? MENU_N : settingsOpen ? SETTINGS_N : RESET_N;
    int hint   = MENU_HINT_H;
    int mw     = 118;
    int mh     = 16 + n * 14 + hint;
    int mx     = (W - mw) / 2;
    int my     = (H - mh) / 2;
    int rowH   = 14;
    int rowsTop = my + 8;
    const HwTouch& t = hwTouch();
    if (t.justPressed && t.x >= mx && t.x < mx + mw &&
        t.y >= rowsTop && t.y < rowsTop + n * rowH) {
      int hit = (t.y - rowsTop) / rowH;
      if (hit >= 0 && hit < n) {
        beep(2400, 30);
        if (menuOpen)         { menuSel     = hit; menuConfirm(); }
        else if (settingsOpen){ settingsSel = hit; applySetting(hit); }
        else /* resetOpen */  { resetSel    = hit; applyReset(hit); }
      }
    }
  }
  // END of press-based approval/overlay taps. Below: release-based classifier
  // for DISP_NORMAL / DISP_PET / DISP_INFO (vertical swipe cycles mode,
  // horizontal swipe in clock mode cycles species, stationary tap routes
  // to region-specific actions). Approval and overlay menus are excluded
  // so an accidental drag can't mis-decide.

  if (tp.justReleased
      && !inPrompt && !menuOpen && !settingsOpen && !resetOpen
      && !napping && !screenOff) {
    int dx = (int)tp.x - _tpStartX;
    int dy = (int)tp.y - _tpStartY;
    uint32_t dt = millis() - _tpStartMs;

    if (abs(dy) >= 40 && abs(dy) > abs(dx) * 2 && dt < 500) {
      // Vertical swipe → advance one step in the flat 9-page cycle
      // (up = next, down = previous). Key1 keeps the coarser 3-mode cycle.
      beep(1800, 30);
      if (dy < 0) swipeNextPage();
      else        swipePrevPage();
    }
    else if (tpClocking && abs(dx) >= 40 && abs(dx) > abs(dy) * 2 && dt < 500) {
      // Horizontal swipe in clock mode → cycle species (unchanged behaviour).
      beep(2400, 30);
      if (dx > 0) nextPet(); else prevPet();
      _playfulUntil = millis() + PLAYFUL_MS;
    }
    else if (abs(dx) < 12 && abs(dy) < 12 && dt < 800) {
      // Stationary tap → route by press-start position.
      if (displayMode == DISP_INFO && tappedFrom(W - 60, 0, 60, 70)) {
        beep(2400, 30);
        infoPage = (infoPage + 1) % INFO_PAGES;
      }
      else if (displayMode == DISP_PET && tappedFrom(W - 60, 0, 60, 70)) {
        beep(2400, 30);
        petPage = (petPage + 1) % PET_PAGES;
        applyDisplayMode();
      }
      else if (displayMode == DISP_NORMAL && !tpClocking && tappedFrom(12, 20, W - 24, 110)) {
        // Tap buddy body → heart reaction (HUD; clock mode uses the block below).
        triggerOneShot(P_HEART, 2000);
        _playfulUntil = millis() + PLAYFUL_MS;
        characterInvalidate();
        if (buddyMode) buddyInvalidate();
        beep(2400, 50);
      }
      else if (displayMode == DISP_NORMAL && !tpClocking && tappedFrom(0, H - 32, W, 32)) {
        // Bottom strip → scroll transcript back (mirrors BtnB short-press).
        beep(2400, 30);
        msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
      }
      else if (tpClocking && _tpStartY < 130) {
        // Clock mode upper half = buddy region (lower half is clock digits).
        triggerOneShot(P_HEART, 2000);
        _playfulUntil = millis() + PLAYFUL_MS;
        characterInvalidate();
        if (buddyMode) buddyInvalidate();
        beep(2400, 50);
      }
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  // Clock shows when Claude is idle and the RTC is synced — regardless
  // of USB power. On battery the screen still auto-offs after a longer
  // timeout (CLOCK_OFF_MS_BAT) so it doesn't drain forever.
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid();
  // Portrait-only clock on AMOLED port; landscape was removed.
  static bool wasClocking = false;
  if (clocking != wasClocking) {
    if (clocking) {
      // GIFs are tall (up to 140 px) — must shrink to fit above clock.
      // ASCII buddy at scale 2 reaches y≈126; clock starts at y=140
      // (compact single-line layout) so peek isn't needed and the pet
      // gets to keep its full size.
      characterSetPeek(true);
      buddySetPeek(false);
      // Clear the full canvas once on entry: buddy/clock both update
      // partial regions every frame, so any stale ink left behind from
      // the previous mode would persist forever.
      const Palette& cp = characterPalette();
      spr.fillScreen(cp.bg);
    } else {
      applyDisplayMode();
    }
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  // Skip the time-of-day mood logic while a one-shot animation
  // (shake → dizzy, level-up → celebrate, fast-approve → heart) is
  // active — otherwise it would overwrite activeState immediately.
  if (clocking && (int32_t)(now - oneShotUntil) >= 0) {
    if ((int32_t)(now - _playfulUntil) < 0) {
      // Recently interacted with (pet tap / species swipe) — rotate through
      // awake animations instead of falling back to the time-of-day logic
      // that mostly picks P_SLEEP. Decays to normal after PLAYFUL_MS.
      static const PersonaState PLAYFUL[] = {
        P_IDLE, P_IDLE, P_HEART, P_IDLE, P_CELEBRATE, P_IDLE
      };
      activeState = PLAYFUL[(now / 5000) % 6];
    } else {
      // Ambient rhythm is SLEEP↔IDLE only. Emotional states (HEART, CELEBRATE,
      // DIZZY) are reactions — they fire from real events (shake, fast-approve,
      // level-up, pet tap, species swipe) via triggerOneShot / playful window,
      // never spontaneously from wall-clock mood.
      uint8_t h = _clkTm.H;
      if (h < 7 || h >= 22) activeState = (now/15000 % 8 == 0) ? P_IDLE  : P_SLEEP;
      else                  activeState = (now/12000 % 6 == 0) ? P_SLEEP : P_IDLE;
    }
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff) {
    // skip canvas render — face-down or powered off
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(SAFE_L, 90);
      spr.print("installing");
      spr.setCursor(SAFE_L, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(SAFE_L, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(SAFE_L + 1, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(SAFE_L, 100);
      spr.print("no character loaded");
    }
  }
  if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    hwDisplayPush();
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    hwDisplayBrightness(0);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // Auto-off rules:
  //   USB plugged: never (clock can stay visible indefinitely)
  //   Battery + clock visible: 5 min (CLOCK_OFF_MS_BAT)
  //   Battery + non-clock idle: 30 s (SCREEN_OFF_MS)
  if (!screenOff && !inPrompt && !_onUsb) {
    uint32_t idleMs    = millis() - lastInteractMs;
    uint32_t threshold = clocking ? CLOCK_OFF_MS_BAT : SCREEN_OFF_MS;
    if (idleMs > threshold) {
      hwDisplaySleep(true);
      screenOff = true;
    }
  }

  // AMOLED burn-in mitigation: every 5 min force a full canvas redraw.
  // OLED pixels degrade where they stay lit at constant value; redrawing
  // (rather than incremental updates) at least exercises every pixel for
  // a frame. A more aggressive 1-px shimmy could shift the whole canvas
  // each cycle, but this minimum is a safe baseline.
  static uint32_t lastShimmy = 0;
  if (millis() - lastShimmy > 5UL * 60UL * 1000UL) {
    lastShimmy = millis();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }

  // LTPO-lite: vary loop cadence by what's happening. Animations tick on
  // wall-clock (buddy.cpp TICK_MS=200) and redraws are gated, so slowing the
  // loop during ambient SLEEP↔IDLE costs no frames — just fewer MCU wakes.
  // Fast rate only where latency is felt: input, interactive UI, one-shots,
  // nap-exit, transfer progress, BLE pairing.
  uint32_t loopMs;
  if (screenOff) {
    loopMs = 200;
  } else if (napping
          || hwTouch().down
          || hwBtnA().isPressed || hwBtnB().isPressed
          || inPrompt || menuOpen || settingsOpen || resetOpen
          || (int32_t)(now - oneShotUntil) < 0
          || xferActive()
          || blePasskey()
          || displayMode == DISP_INFO) {   // keep auto-scroll smooth
    loopMs = 16;
  } else {
    loopMs = 100;
  }
  // Slice the idle sleep so a touch-down IRQ (edge-triggered) or a button
  // press breaks out within ~8ms instead of waiting the full loopMs. Without
  // this, first-tap latency during idle felt sluggish.
  if (loopMs <= 16) {
    delay(loopMs);
  } else {
    uint32_t slept = 0;
    while (slept < loopMs) {
      uint32_t slice = (loopMs - slept > 8) ? 8 : (loopMs - slept);
      delay(slice);
      slept += slice;
      if (hwTouchIrqPending()) break;
#if BOARD_HAS_KEY1
      if (digitalRead(PIN_KEY1) == LOW) break;
#elif BOARD_BTN_THIRD
      // No KEY1 — use BOOT key for the wake-from-idle break.
      if (digitalRead(PIN_KEY_BOOT) == LOW) break;
#endif
    }
  }
}
