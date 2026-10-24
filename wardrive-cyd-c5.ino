/*
  WARDRIVE CYD-C5 (NM-CYD-C5, ST7789 2.8", XPT2046 touch, SD card)
  GPS: GPS+BDS Beidou dual-mode module (ATGM336H-compatible, NEO-M8N replacement), NMEA at 9600 baud
  - Calibration runs automatically on first boot. Start it again with the [CAL] button in the header.
  - Wi-Fi scanning on 2.4 GHz, 5 GHz or both. Results are logged to the SD card in WiGLE CSV format.
  - No web server and no Wi-Fi access point. Everything is shown on the LCD.

  Arduino IDE 2.x, ESP32 by Espressif Systems v3.3.x (board: ESP32C5 Dev Module)
  Libraries: GFX Library for Arduino (Arduino_GFX_Library.h), XPT2046_Touchscreen
  Tools: USB CDC On Boot = Enabled, PSRAM = Disabled, Flash Size = 16MB
*/

#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "esp_wifi.h"

// ===================== PINS (NM-CYD-C5) =====================
#define PIN_SCK      6
#define PIN_MISO     2
#define PIN_MOSI     7
#define LCD_CS      23
#define LCD_DC      24
#define LCD_RST     -1
#define LCD_BL      25
#define TOUCH_CS     1
#define SD_CS       10
#define GPS_RX_PIN   8      // GPS module TX -> GPIO8 (header pin next to 3.3V and GND)
#define GPS_TX_PIN  -1
#define GPS_BAUD   9600
#define GPS_DEBUG_RAW 0     // 1 = print every raw NMEA sentence to Serial (set 0 when done testing)

// ===================== DISPLAY =====================
#define DISPLAY_IPS      0  // try 0 first, use 1 if colors are wrong
#define DISPLAY_INVERT   0
#define DISPLAY_ROTATION 1  // 1 = landscape, 3 = landscape upside down

// ===================== TOUCH =====================
#define TOUCH_Z_MIN  1000   // minimum touch pressure (z). Shown on the calibration screen. Adjust if needed.

// ===================== SCANNING & LOGGING =====================
#define ACTIVE_SCAN        1
#define SCAN_MS_PER_CH   100   // ~3.8 s per full scan. 200 = more hits, 60 = fastest
#define LOG_ALL_SIGHTINGS  0
#define RECENT_MAX         8
#define RECENT_SHOW        3
#define DEDUP_SLOTS     4096
#define DEDUP_MAX       3000

// Colors (RGB565) - green on black
#define C_BG      0x0000
#define C_GREEN   0x07E0   // main color
#define C_GDIM    0x0320   // dimmed green text
#define C_GFAINT  0x0180   // frames
#define C_AMBER   0xFD20   // warnings / messages
#define C_RED     0xF800
#define C_OFF     0x0120   // inactive bars

// Layout (320 x 240, landscape)
#define Y_HEADER  0
#define Y_GPS     32
#define Y_BANDS   68
#define Y_START  110
#define Y_STATS  152
#define Y_MSG    188
#define Y_RECENT 196

enum { BAND_24 = 0, BAND_5 = 1, BAND_BOTH = 2 };

// Calibration targets: 0 = top left, 1 = top right, 2 = bottom right, 3 = bottom left
static const int CAL_X[4] = {20, 300, 300, 20};
static const int CAL_Y[4] = {20, 20, 220, 220};

// SPI: one shared bus for display, touch and SD card
SPIClass sharedSpi(FSPI);
Arduino_HWSPI bus(LCD_DC, LCD_CS, PIN_SCK, PIN_MOSI, PIN_MISO, &sharedSpi, true);
Arduino_ST7789 gfx(&bus, LCD_RST, DISPLAY_ROTATION, DISPLAY_IPS);
XPT2046_Touchscreen ts(TOUCH_CS);
Preferences prefs;

// ---------- Calibration ----------
struct Cal {
  bool  ok = false;
  bool  swap = false;   // true if screen X corresponds to raw Y
  float ax = 1, bx = 0; // screenX = raw * ax + bx
  float ay = 1, by = 0; // screenY = raw * ay + by
} cal;

static void calSave() {
  prefs.putBool("calOk", true);
  prefs.putBool("calSwap", cal.swap);
  prefs.putFloat("calAx", cal.ax);
  prefs.putFloat("calBx", cal.bx);
  prefs.putFloat("calAy", cal.ay);
  prefs.putFloat("calBy", cal.by);
}

static bool calLoad() {
  if (!prefs.getBool("calOk", false)) return false;
  cal.swap = prefs.getBool("calSwap", false);
  cal.ax = prefs.getFloat("calAx", 1);
  cal.bx = prefs.getFloat("calBx", 0);
  cal.ay = prefs.getFloat("calAy", 1);
  cal.by = prefs.getFloat("calBy", 0);
  cal.ok = true;
  return true;
}

// rx/ry = averaged raw values for the four corners in order TL, TR, BR, BL
static bool calCompute(const float rx[4], const float ry[4]) {
  float xL = (rx[0] + rx[3]) / 2, xR = (rx[1] + rx[2]) / 2;   // raw X for left/right edge
  float yT = (ry[0] + ry[1]) / 2, yB = (ry[2] + ry[3]) / 2;   // raw Y for top/bottom edge
  float yL = (ry[0] + ry[3]) / 2, yR = (ry[1] + ry[2]) / 2;   // raw Y for left/right edge
  float xT = (rx[0] + rx[1]) / 2, xB = (rx[2] + rx[3]) / 2;   // raw X for top/bottom edge

  float dNo = fabsf(xR - xL);
  float dSw = fabsf(yR - yL);
  if (dNo < 150 && dSw < 150) return false;   // no clear pattern, taps were wrong

  cal.swap = dSw > dNo;
  float sxL, sxR, syT, syB;
  if (!cal.swap) { sxL = xL; sxR = xR; syT = yT; syB = yB; }
  else           { sxL = yL; sxR = yR; syT = xT; syB = xB; }
  if (fabsf(sxR - sxL) < 150 || fabsf(syB - syT) < 150) return false;

  // Left edge = screen x 20, right = 300, top = y 20, bottom = y 220
  cal.ax = 280.0f / (sxR - sxL);
  cal.bx = 20.0f - sxL * cal.ax;
  cal.ay = 200.0f / (syB - syT);
  cal.by = 20.0f - syT * cal.ay;
  cal.ok = true;
  return true;
}

static void mapTouch(int rawX, int rawY, int &sx, int &sy) {
  float a = cal.swap ? rawY : rawX;
  float b = cal.swap ? rawX : rawY;
  int ix = (int)(a * cal.ax + cal.bx);
  int iy = (int)(b * cal.ay + cal.by);
  sx = ix < 0 ? 0 : (ix > 319 ? 319 : ix);
  sy = iy < 0 ? 0 : (iy > 239 ? 239 : iy);
}

// ---------- GPS ----------
struct GpsState {
  bool     fix = false;
  int      fixQuality = 0;
  int      sats = 0;
  float    hdop = 0;
  bool     hdopValid = false;
  float    alt = 0;
  bool     altValid = false;
  float    speedKmh = 0;
  double   lat = 0, lon = 0;
  bool     timeValid = false;
  int      year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  uint32_t lastSentenceMs = 0;
  uint32_t sentences = 0;
  uint32_t badChecksums = 0;
} gps;

static char nmeaBuf[128];
static uint8_t nmeaLen = 0;

static int twoDigits(const char *p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

static double nmeaToDeg(const char *v, const char *hemi) {
  if (!v || !*v) return NAN;
  double raw = atof(v);
  int deg = (int)(raw / 100.0);
  double minutes = raw - deg * 100.0;
  double d = deg + minutes / 60.0;
  if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
  return d;
}

static bool nmeaChecksumOk(const char *s) {
  const char *star = strchr(s, '*');
  if (!star || star - s < 2) return false;
  uint8_t x = 0;
  for (const char *p = s + 1; p < star; p++) x ^= (uint8_t)*p;
  return strtol(star + 1, nullptr, 16) == x;
}

static int nmeaSplit(char *s, char *f[], int maxF) {
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < maxF; p++) {
    if (*p == ',' || *p == '*') {
      *p = 0;
      f[n++] = p + 1;
    }
  }
  return n;
}

static void parseNmea(char *s) {
  if (!nmeaChecksumOk(s)) {
    gps.badChecksums++;
    return;
  }
  char *f[20];
  int n = nmeaSplit(s, f, 20);
  if (n < 2) return;
  gps.sentences++;
  gps.lastSentenceMs = millis();

  if (strncmp(f[0] + 3, "GGA", 3) == 0 && n >= 10) {
    gps.fixQuality = atoi(f[6]);
    gps.fix = (gps.fixQuality == 1 || gps.fixQuality == 2);
    gps.sats = atoi(f[7]);
    gps.hdopValid = *f[8] != 0;
    gps.hdop = atof(f[8]);
    gps.altValid = *f[9] != 0;
    gps.alt = atof(f[9]);
    if (gps.fix) {
      double la = nmeaToDeg(f[2], f[3]);
      double lo = nmeaToDeg(f[4], f[5]);
      if (!isnan(la) && !isnan(lo)) {
        gps.lat = la;
        gps.lon = lo;
      }
    }
  } else if (strncmp(f[0] + 3, "GSA", 3) == 0 && n >= 17) {
    // Satellites used and HDOP. Only the combined GN sentence, so GPS and BDS are not counted twice.
    if (f[0][1] == 'G' && f[0][2] == 'N') {
      int used = 0;
      for (int i = 3; i <= 14; i++) if (*f[i]) used++;
      if (used > 0) gps.sats = used;
      if (*f[16]) {
        gps.hdop = atof(f[16]);
        gps.hdopValid = true;
      }
    }
  } else if (strncmp(f[0] + 3, "RMC", 3) == 0 && n >= 10) {
    gps.speedKmh = atof(f[7]) * 1.852f;
    // RMC gives position and fix even when GGA is not sent (the GP-02 may send only RMC by default).
    if (f[2][0] == 'A' && *f[3] && *f[5]) {
      double la = nmeaToDeg(f[3], f[4]);
      double lo = nmeaToDeg(f[5], f[6]);
      if (!isnan(la) && !isnan(lo)) {
        gps.fix = true;
        gps.lat = la;
        gps.lon = lo;
      }
    } else if (f[2][0] == 'V') {
      gps.fix = false;
    }
    if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
      gps.hour = twoDigits(f[1]);
      gps.minute = twoDigits(f[1] + 2);
      gps.second = twoDigits(f[1] + 4);
      gps.day = twoDigits(f[9]);
      gps.month = twoDigits(f[9] + 2);
      gps.year = 2000 + twoDigits(f[9] + 4);
      gps.timeValid = (f[2][0] == 'A');
    }
  }
}

static void gpsFeed(char c) {
  if (c == '$') nmeaLen = 0;
  if (c == '\r') return;
  if (c == '\n') {
    nmeaBuf[nmeaLen] = 0;
    if (GPS_DEBUG_RAW && nmeaLen > 0) Serial.println(nmeaBuf);
    if (nmeaLen > 6 && nmeaBuf[0] == '$') parseNmea(nmeaBuf);
    nmeaLen = 0;
    return;
  }
  if (nmeaLen < sizeof(nmeaBuf) - 1) nmeaBuf[nmeaLen++] = c;
}

static const char *gpsStateStr() {
  if (gps.sentences == 0 || millis() - gps.lastSentenceMs > 10000) return "nodata";
  if (!gps.fix) return "searching";
  return "fix";
}

// ---------- Wi-Fi & logging ----------
int      bandWanted = BAND_BOTH;
int      bandApplied = -1;
bool     sessionActive = false;
bool     scanRunning = false;
bool     sdOk = false;
uint32_t lastScanStartMs = 0;
uint32_t lastSdTryMs = 0;
uint32_t lastFlushMs = 0;
uint32_t scansDone = 0;
int      lastScan24 = 0, lastScan5 = 0;
uint32_t uniqueSession = 0, rowsWritten = 0;
File     logFile;
String   logName;

char     msgText[48] = "";
uint32_t msgUntilMs = 0;

static uint64_t dedup[DEDUP_SLOTS];
static uint32_t dedupCount = 0;

struct RecentHit {
  char ssid[33];
  int8_t rssi;
  bool is5;
};
static RecentHit recent[RECENT_MAX];
static int recentHead = 0, recentCount = 0;

static uint64_t macKey(const uint8_t *m) {
  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | m[i];
  return k | (1ULL << 48);
}

static bool dedupInsert(uint64_t key) {
  if (dedupCount >= DEDUP_MAX) {
    memset(dedup, 0, sizeof(dedup));
    dedupCount = 0;
  }
  uint32_t h = (uint32_t)(key ^ (key >> 29)) * 2654435761u;
  for (uint32_t n = 0; n < DEDUP_SLOTS; n++) {
    uint32_t idx = (h + n) & (DEDUP_SLOTS - 1);
    if (dedup[idx] == 0) {
      dedup[idx] = key;
      dedupCount++;
      return true;
    }
    if (dedup[idx] == key) return false;
  }
  return false;
}

static int chanToMhz(int ch, bool is5) {
  if (is5) return 5000 + ch * 5;
  if (ch == 14) return 2484;
  return 2407 + ch * 5;
}

static const char *authStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "[ESS]";
    case WIFI_AUTH_WEP:             return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK:         return "[WPA-PSK][ESS]";
    case WIFI_AUTH_WPA2_PSK:        return "[WPA2-PSK][CCMP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA-PSK][WPA2-PSK][CCMP][ESS]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP][CCMP][ESS]";
    case WIFI_AUTH_WPA3_PSK:        return "[WPA3-SAE][CCMP][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2-PSK][WPA3-SAE][CCMP][ESS]";
    default:                        return "[ESS]";
  }
}

static void setMsg(const char *t) {
  strncpy(msgText, t, sizeof(msgText) - 1);
  msgText[sizeof(msgText) - 1] = 0;
  msgUntilMs = millis() + 3000;
}

static void applyBand() {
  if (bandApplied == bandWanted) return;
  wifi_band_mode_t mode = WIFI_BAND_MODE_AUTO;
  if (bandWanted == BAND_24) mode = WIFI_BAND_MODE_2G_ONLY;
  else if (bandWanted == BAND_5) mode = WIFI_BAND_MODE_5G_ONLY;
  esp_err_t err = esp_wifi_set_band_mode(mode);
  if (err == ESP_OK) {
    bandApplied = bandWanted;
    Serial.printf("Band set: %d\n", bandWanted);
  } else {
    Serial.printf("esp_wifi_set_band_mode error: %d\n", err);
  }
}

static void pushRecent(const char *ssid, int rssi, bool is5) {
  RecentHit &r = recent[recentHead];
  strncpy(r.ssid, ssid, sizeof(r.ssid) - 1);
  r.ssid[sizeof(r.ssid) - 1] = 0;
  r.rssi = (int8_t)rssi;
  r.is5 = is5;
  recentHead = (recentHead + 1) % RECENT_MAX;
  if (recentCount < RECENT_MAX) recentCount++;
}

static void writeRow(const uint8_t *mac, String ssid, int ch, bool is5, int rssi, wifi_auth_mode_t enc) {
  char macS[18];
  snprintf(macS, sizeof(macS), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  ssid.replace(',', '_');
  ssid.replace('"', '\'');
  ssid.replace('\r', ' ');
  ssid.replace('\n', ' ');

  char firstSeen[24] = "";
  if (gps.timeValid) {
    snprintf(firstSeen, sizeof(firstSeen), "%04d-%02d-%02d %02d:%02d:%02d",
             gps.year, gps.month, gps.day, gps.hour, gps.minute, gps.second);
  }

  char lat[16] = "", lon[16] = "", alt[12] = "", acc[12] = "";
  if (gps.fix) {
    snprintf(lat, sizeof(lat), "%.7f", gps.lat);
    snprintf(lon, sizeof(lon), "%.7f", gps.lon);
    if (gps.altValid) snprintf(alt, sizeof(alt), "%.1f", gps.alt);
    if (gps.hdopValid) snprintf(acc, sizeof(acc), "%.1f", gps.hdop * 5.0f);
  }

  char line[320];
  snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%d,%d,%s,%s,%s,%s,WIFI",
           macS, ssid.c_str(), authStr(enc), firstSeen, ch, chanToMhz(ch, is5), rssi,
           lat, lon, alt, acc);
  logFile.println(line);
  rowsWritten++;
  if (rowsWritten % 10 == 0) logFile.flush();
}

static void processScan(int n) {
  int c24 = 0, c5 = 0;
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    bool is5 = ch > 14;
    if (is5) c5++; else c24++;

    const uint8_t *mac = WiFi.BSSID(i);
    if (!mac) continue;
    bool isNew = dedupInsert(macKey(mac));
    if (!isNew && !LOG_ALL_SIGHTINGS) continue;
    if (isNew) uniqueSession++;

    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    if (sessionActive && logFile) {
      writeRow(mac, ssid, ch, is5, rssi, WiFi.encryptionType(i));
    }
    if (isNew) pushRecent(ssid.c_str(), rssi, is5);
  }
  lastScan24 = c24;
  lastScan5 = c5;
  scansDone++;
}

static void startScan() {
  applyBand();
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true, true, !ACTIVE_SCAN, SCAN_MS_PER_CH);
  lastScanStartMs = millis();
  if (r == WIFI_SCAN_RUNNING) {
    scanRunning = true;
  } else if (r >= 0) {
    processScan(r);
    WiFi.scanDelete();
  }
}

static bool startSession() {
  if (!sdOk) {
    setMsg("SD CARD MISSING");
    return false;
  }
  if (sessionActive) return true;

  char name[40];
  if (gps.timeValid) {
    snprintf(name, sizeof(name), "/wd_%04d%02d%02d_%02d%02d%02d.csv",
             gps.year, gps.month, gps.day, gps.hour, gps.minute, gps.second);
  } else {
    snprintf(name, sizeof(name), "/wd_boot%010lu.csv", (unsigned long)(millis() / 1000));
  }

  logFile = SD.open(name, FILE_WRITE);
  if (!logFile) {
    setMsg("COULD NOT CREATE FILE");
    return false;
  }
  logName = name;
  logFile.println("WigleWifi-1.4,appRelease=WARDRIVE-CYD-C5,model=NM-CYD-C5,release=1.0,device=WARDRIVE-CYD-C5,display=ST7789,board=ESP32-C5,brand=DIY");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
  logFile.flush();

  memset(dedup, 0, sizeof(dedup));
  dedupCount = 0;
  uniqueSession = 0;
  rowsWritten = 0;
  sessionActive = true;
  Serial.printf("Logging started: %s\n", name);
  return true;
}

static void stopSession() {
  if (logFile) {
    logFile.flush();
    logFile.close();
  }
  sessionActive = false;
  Serial.println("Logging stopped");
}

// ---------- Drawing (hacker style: green on black) ----------
static void printPad(int x, int y, const char *s, int width, uint16_t fg, uint16_t bg, uint8_t size) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%-*s", width, s);
  gfx.setTextSize(size);
  gfx.setTextColor(fg, bg);
  gfx.setCursor(x, y);
  gfx.print(buf);
}

static int cBand = -1, cSession = -1, cGpsState = -1, cBars = -1;

static void invalidateCache() {
  cBand = -1;
  cSession = -1;
  cGpsState = -1;
  cBars = -1;
}

static void drawBandButtons() {
  if (cBand == bandWanted) return;
  cBand = bandWanted;
  const char *labels[3] = {"2.4G", "5G", "2.4+5G"};
  for (int i = 0; i < 3; i++) {
    int bx = 6 + i * 106;
    bool sel = (bandWanted == i);
    gfx.fillRect(bx, Y_BANDS, 96, 36, C_BG);
    gfx.drawRect(bx, Y_BANDS, 96, 36, sel ? C_GREEN : C_GDIM);
    if (sel) gfx.fillRect(bx + 1, Y_BANDS + 1, 94, 34, C_GREEN);
    gfx.setTextSize(2);
    gfx.setTextColor(sel ? C_BG : C_GDIM, sel ? C_GREEN : C_BG);
    int tw = strlen(labels[i]) * 12;
    gfx.setCursor(bx + (96 - tw) / 2, Y_BANDS + 10);
    gfx.print(labels[i]);
  }
}

static void drawStartButton() {
  if (cSession == (int)sessionActive) return;
  cSession = sessionActive;
  uint16_t bg = sessionActive ? C_RED : C_GREEN;
  gfx.fillRect(6, Y_START, 308, 36, C_BG);
  gfx.drawRect(6, Y_START, 308, 36, bg);
  gfx.fillRect(7, Y_START + 1, 306, 34, bg);
  gfx.setTextSize(2);
  gfx.setTextColor(C_BG, bg);
  const char *t = sessionActive ? "> STOP <" : "> START <";
  int tw = strlen(t) * 12;
  gfx.setCursor(6 + (308 - tw) / 2, Y_START + 10);
  gfx.print(t);
}

static void drawGpsBar() {
  const char *st = gpsStateStr();
  bool fix = !strcmp(st, "fix");
  int state = fix ? 2 : (!strcmp(st, "searching") ? 1 : 0);
  if (cGpsState != state) {
    cGpsState = state;
    uint16_t col = fix ? C_GREEN : (state == 1 ? C_AMBER : C_RED);
    gfx.fillCircle(16, Y_GPS + 16, 5, col);
  }

  const char *label = fix ? "GPS FIX" : (state == 1 ? "SEARCHING SATS" : "NO NMEA");
  char hd[10] = "-";
  if (gps.hdopValid) snprintf(hd, sizeof(hd), "%.1f", gps.hdop);
  char line1[64];
  snprintf(line1, sizeof(line1), "%s  SAT %d  HDOP %s", label, gps.sats, hd);

  char line2[64] = "";
  if (fix) {
    snprintf(line2, sizeof(line2), "%.5f %.5f", gps.lat, gps.lon);
    if (gps.altValid) {
      char alt[16];
      snprintf(alt, sizeof(alt), "  ALT %.0fm", gps.alt);
      strncat(line2, alt, sizeof(line2) - strlen(line2) - 1);
    }
  } else if (state == 0) {
    snprintf(line2, sizeof(line2), "Check GPS port wiring");
  }

  printPad(28, Y_GPS + 6, line1, 40, fix ? C_GREEN : C_GDIM, C_BG, 1);
  printPad(28, Y_GPS + 19, line2, 40, C_GDIM, C_BG, 1);

  int bars = gps.sats >= 8 ? 5 : gps.sats >= 6 ? 4 : gps.sats >= 4 ? 3 : gps.sats >= 2 ? 2 : gps.sats >= 1 ? 1 : 0;
  int key = bars * 2 + (fix ? 1 : 0);
  if (cBars != key) {
    cBars = key;
    for (int i = 0; i < 5; i++) {
      int h = 4 + i * 3;
      uint16_t c = i < bars ? (fix ? C_GREEN : C_AMBER) : C_OFF;
      gfx.fillRect(280 + i * 6, Y_GPS + 24 - h, 4, h, c);
    }
  }
}

static void drawStats() {
  char tm[8];
  if (gps.timeValid) snprintf(tm, sizeof(tm), "%02d:%02d", gps.hour, gps.minute);
  else snprintf(tm, sizeof(tm), "--:--");
  char l1[64], l2[64];
  snprintf(l1, sizeof(l1), "UNIQ %-6lu ROWS %-6lu UTC %s",
           (unsigned long)uniqueSession, (unsigned long)rowsWritten, tm);
  snprintf(l2, sizeof(l2), "24G %-3d 5G %-3d SCAN %-5lu SD %s",
           lastScan24, lastScan5, (unsigned long)scansDone, sdOk ? "OK" : "NO");
  printPad(10, Y_STATS + 6, l1, 50, C_GREEN, C_BG, 1);
  printPad(10, Y_STATS + 20, l2, 50, C_GDIM, C_BG, 1);
}

static void drawMsg() {
  char b[56] = "";
  if (msgText[0] && millis() < msgUntilMs) snprintf(b, sizeof(b), "> %s", msgText);
  printPad(8, Y_MSG, b, 50, C_AMBER, C_BG, 1);
}

static void drawRecent() {
  for (int k = 0; k < RECENT_SHOW; k++) {
    char line[64] = "";
    if (k < recentCount) {
      int idx = (recentHead - 1 - k + RECENT_MAX) % RECENT_MAX;
      const RecentHit &h = recent[idx];
      snprintf(line, sizeof(line), "%-26.26s %4d dBm  %s",
               h.ssid[0] ? h.ssid : "<hidden>", (int)h.rssi, h.is5 ? "5G" : "2.4G");
    }
    printPad(8, Y_RECENT + 14 + k * 10, line, 42, C_GREEN, C_BG, 1);
  }
}

static void drawDynamic() {
  drawGpsBar();
  drawStats();
  drawMsg();
  drawRecent();
}

static void drawStaticLayout() {
  gfx.fillScreen(C_BG);

  // Header: green bar with black text
  gfx.fillRect(0, Y_HEADER, 320, 28, C_GREEN);
  gfx.setTextSize(2);
  gfx.setTextColor(C_BG, C_GREEN);
  gfx.setCursor(8, 6);
  gfx.print("WARDRIVE CYD-C5");

  // CAL button
  gfx.fillRect(256, 5, 58, 18, C_BG);
  gfx.drawRect(256, 5, 58, 18, C_BG);
  gfx.setTextSize(1);
  gfx.setTextColor(C_BG, C_GREEN);
  gfx.setCursor(268, 10);
  gfx.print("[CAL]");

  // Panel frames
  gfx.drawRect(4, Y_GPS, 312, 32, C_GFAINT);
  gfx.drawRect(4, Y_STATS, 312, 34, C_GFAINT);

  gfx.setTextSize(1);
  gfx.setTextColor(C_GDIM, C_BG);
  gfx.setCursor(8, Y_RECENT + 2);
  gfx.print("// LATEST NEW SSIDs");
  invalidateCache();
}

// ---------- Touch & calibration ----------
static TS_Point readTouch() {
  return ts.getPoint();
}

static void waitForPress(int idx) {
  uint32_t lastShow = 0;
  while (true) {
    TS_Point p = readTouch();
    if (millis() - lastShow > 150) {
      char b[24];
      snprintf(b, sizeof(b), "z=%d     ", p.z);
      gfx.setTextSize(1);
      gfx.setTextColor(C_GDIM, C_BG);
      gfx.setCursor(120, 220);
      gfx.print(b);
      lastShow = millis();
    }
    if (p.z >= TOUCH_Z_MIN) return;
    delay(15);
  }
}

static bool sampleCorner(float &rx, float &ry) {
  long sx = 0, sy = 0;
  int n = 0;
  for (int k = 0; k < 15; k++) {
    TS_Point p = readTouch();
    if (p.z >= TOUCH_Z_MIN) {
      sx += p.x;
      sy += p.y;
      n++;
    }
    delay(20);
  }
  if (n < 10) return false;
  rx = sx / (float)n;
  ry = sy / (float)n;
  return true;
}

static void waitRelease() {
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    if (readTouch().z < TOUCH_Z_MIN) break;
    delay(20);
  }
  delay(300);
}

static void drawCalTarget(int idx) {
  gfx.fillScreen(C_BG);
  gfx.setTextSize(2);
  gfx.setTextColor(C_GREEN, C_BG);
  gfx.setCursor(40, 70);
  gfx.print("> CALIBRATION");
  gfx.setTextSize(1);
  gfx.setTextColor(C_GDIM, C_BG);
  char b[40];
  snprintf(b, sizeof(b), "TAP THE CENTER OF THE CROSS (%d/4)", idx + 1);
  gfx.setCursor(70, 105);
  gfx.print(b);
  int x = CAL_X[idx], y = CAL_Y[idx];
  gfx.drawLine(x - 12, y, x + 12, y, C_AMBER);
  gfx.drawLine(x, y - 12, x, y + 12, C_AMBER);
  gfx.drawCircle(x, y, 6, C_AMBER);
}

static void showCalMessage(const char *t, uint32_t ms) {
  gfx.fillRect(0, 140, 320, 16, C_BG);
  gfx.setTextSize(1);
  gfx.setTextColor(C_AMBER, C_BG);
  gfx.setCursor(84, 146);
  gfx.print(t);
  delay(ms);
}

static void runCalibration() {
  while (true) {
    float rx[4], ry[4];
    int i = 0;
    while (i < 4) {
      drawCalTarget(i);
      waitForPress(i);
      float x, y;
      if (!sampleCorner(x, y)) {
        showCalMessage("TOO WEAK PRESS, TRY AGAIN", 800);
        drawCalTarget(i);
        waitRelease();
        continue;
      }
      rx[i] = x;
      ry[i] = y;
      Serial.printf("Cal %d: raw x=%.0f y=%.0f\n", i, x, y);
      waitRelease();
      i++;
    }
    if (calCompute(rx, ry)) {
      calSave();
      Serial.printf("Calibrated: swap=%d ax=%.4f bx=%.2f ay=%.4f by=%.2f\n",
                    cal.swap, cal.ax, cal.bx, cal.ay, cal.by);
      drawCalTarget(3);
      showCalMessage("CALIBRATED", 1200);
      break;
    }
    showCalMessage("CALIBRATION FAILED, RETRY", 2000);
  }
  drawStaticLayout();
  drawDynamic();
  drawBandButtons();
  drawStartButton();
}

static void setBand(int b) {
  if (b == bandWanted) return;
  bandWanted = b;
  prefs.putUChar("band", (uint8_t)b);
  const char *names[] = {"2.4 GHz", "5 GHz", "2.4 + 5 GHz"};
  char m[48];
  snprintf(m, sizeof(m), "BAND: %s", names[b]);
  setMsg(m);
}

static void toggleSession() {
  if (sessionActive) {
    stopSession();
    setMsg("LOGGING STOPPED");
  } else if (startSession()) {
    setMsg("LOGGING STARTED");
  }
}

static void handleTouch(int x, int y) {
  // [CAL] in the header
  if (y < 28 && x >= 256) {
    runCalibration();
    return;
  }
  // Band buttons
  if (y >= Y_BANDS && y < Y_BANDS + 36) {
    for (int i = 0; i < 3; i++) {
      int bx = 6 + i * 106;
      if (x >= bx && x < bx + 96) {
        setBand(i);
        return;
      }
    }
  }
  // Start/stop
  if (y >= Y_START && y < Y_START + 36 && x >= 6 && x < 314) {
    toggleSession();
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nWARDRIVE CYD-C5 starting");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  sharedSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  gfx.begin();
  gfx.invertDisplay(DISPLAY_INVERT);

  ts.begin(sharedSpi);

  // Keep the other devices on the bus deselected before initializing the SD card
  pinMode(LCD_CS, OUTPUT);   digitalWrite(LCD_CS, HIGH);
  pinMode(TOUCH_CS, OUTPUT); digitalWrite(TOUCH_CS, HIGH);

  sdOk = SD.begin(SD_CS, sharedSpi, 400000);
  Serial.println(sdOk ? "SD card OK" : "SD card missing (check card, FAT32 and SD pins)");

  prefs.begin("wardrive", false);
  bandWanted = prefs.getUChar("band", BAND_BOTH);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  applyBand();

  drawStaticLayout();
  if (!calLoad()) {
    runCalibration();   // first boot: calibrate
  } else {
    Serial.println("Calibration loaded from flash");
  }
  drawDynamic();
  drawBandButtons();
  drawStartButton();
}

void loop() {
  while (Serial1.available()) gpsFeed((char)Serial1.read());

  // Touch with debounce: the press must be held for 40 ms before it counts
  static uint32_t pressStart = 0;
  static bool fired = false;
  TS_Point p = readTouch();
  bool pressed = p.z >= TOUCH_Z_MIN;
  if (!pressed) {
    pressStart = 0;
    fired = false;
  } else {
    if (pressStart == 0) pressStart = millis();
    if (!fired && millis() - pressStart >= 40) {
      fired = true;
      int sx, sy;
      mapTouch(p.x, p.y, sx, sy);
      Serial.printf("Touch raw x=%d y=%d z=%d -> %d,%d\n", p.x, p.y, p.z, sx, sy);
      handleTouch(sx, sy);
    }
  }

  uint32_t now = millis();

  // GPS status in Serial every 5 s
  static uint32_t lastGpsPrint = 0;
  if (now - lastGpsPrint > 5000) {
    lastGpsPrint = now;
    if (gps.sentences == 0) {
      Serial.println("GPS: NO DATA. Check module TX -> GPIO8, 3.3V, GND and 9600 baud.");
    } else {
      Serial.printf("GPS: sentences=%lu badChecksum=%lu fix=%d sats=%d hdop=%.1f\n",
                    (unsigned long)gps.sentences, (unsigned long)gps.badChecksums,
                    gps.fix ? 1 : 0, gps.sats, gps.hdop);
    }
  }

  if (scanRunning) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      processScan(n);
      WiFi.scanDelete();
      scanRunning = false;
    } else if (n != WIFI_SCAN_RUNNING) {
      scanRunning = false;
    }
  }

  if (sessionActive && !scanRunning && now - lastScanStartMs > 500) {
    startScan();
  }

  if (!sdOk && now - lastSdTryMs > 10000) {
    lastSdTryMs = now;
    sdOk = SD.begin(SD_CS, sharedSpi, 400000);
  }

  if (sessionActive && logFile && now - lastFlushMs > 5000) {
    lastFlushMs = now;
    logFile.flush();
  }

  static uint32_t lastDraw = 0;
  if (now - lastDraw > 500) {
    lastDraw = now;
    drawDynamic();
    drawBandButtons();
    drawStartButton();
  }
}
