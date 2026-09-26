/******************************************************************************
 *  ELECTGPL EPD WRITER - Text editor on a 5.79" E-Paper display
 *  CrowPanel ESP32-S3-WROOM-1-N8R8 + 792x272 panel (2x SSD1683, 800x272 buffer)
 *  Keyboard: Logitech K380s (or any BLE HOGP keyboard), ESP32 acts as BLE host.
 *
 *  Author: ELECTGPL
 *  EPD layer: controller sequences taken from the Message Board (validated on
 *  hardware); only the bit-banged SPI was replaced by hardware SPI.
 *
 *  Arduino IDE board: "ESP32S3 Dev Module"  (arduino-esp32 core 3.x)
 *    Flash Size 8MB | PSRAM: "OPI PSRAM" | Partition: "8M with spiffs"
 *  Libraries: NimBLE-Arduino 2.x (h2zero); builds with 2.1.0 to 2.5.1
 *  Sketch files: this .ino + spleen_fonts.h (same folder)
 *
 *  STORAGE
 *    Documents live in the internal flash (LittleFS, "spiffs" partition),
 *    folder /docs, UTF-8 encoded. Autosave 3 s after the last keystroke.
 *    The last open document and cursor position are restored at boot.
 *
 *  KEYBOARD SHORTCUTS (Ctrl+H shows them on screen)
 *    Ctrl+S save              Ctrl+O file list           Ctrl+N new document
 *    Ctrl+W file transfer     Ctrl+R anti-ghost refresh  Ctrl+H help
 *    Esc    cancel dead key   Ctrl+Home / Ctrl+End  start / end of document
 *  FILE LIST
 *    Up/Down select   Enter open   N new   R rename   D or Del delete   Esc back
 *  BOARD BUTTONS
 *    MENU = full refresh   OK = save   UP/DOWN = page up/down
 *    EXIT held 3 s = delete bonds (pair the keyboard again)
 *
 *  FILE TRANSFER (Ctrl+W)
 *    The ESP32-S3 becomes a WiFi hotspot "Electgpl-Writer" (pass electgpl1234).
 *    Join it from the PC and browse http://192.168.4.1 to download, upload
 *    or delete notes. Esc or Ctrl+W closes the hotspot. BLE stays connected.
 *
 *  PAIRING THE K380s
 *    Hold an Easy-Switch key for 3 s until its LED blinks fast.
 *    A 6-digit code shows up on the display: type it on the keyboard
 *    and press Enter (Passkey Entry, LE Secure Connections).
 ******************************************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include "spleen_fonts.h"
#include <esp_wifi.h>

/* The ESP32-S3-WROOM-1-N8R8 has 8 MB of OPI PSRAM, but the Arduino default for
 * "ESP32S3 Dev Module" is PSRAM = Disabled. Without PSRAM the document buffer,
 * the SPI stream buffer and every malloc() > 4 kB land in internal RAM, and the
 * WiFi driver can no longer initialise next to the BLE stack (ESP_ERR_NO_MEM). */
#ifndef BOARD_HAS_PSRAM
  #error "Select Tools > PSRAM > OPI PSRAM (the N8R8 module has 8 MB of octal PSRAM)"
#endif


/*===========================================================================
 *  SECTION 0 - TYPES (must precede the first function: the Arduino
 *  preprocessor inserts prototypes right before the first definition)
 *=========================================================================*/
typedef struct {
  uint8_t type;       // 1 = press, 0 = release
  uint8_t usage;      // HID Usage (page 0x07)
  uint8_t mods;       // modifier byte at event time
} key_evt_t;

typedef struct {
  uint8_t  reportId;
  uint8_t  isArray;   // 0 = variable field (bitmap), 1 = array field
  uint16_t bitOff;    // bit offset inside the report (without the ID byte)
  uint8_t  size;      // Report Size
  uint8_t  count;     // Report Count
  uint16_t usageMin;
  int32_t  logMin;
} hid_field_t;

typedef struct {
  uint32_t curLine, curCol, totalLines;
} locate_t;

enum { LAYOUT_LATAM = 0, LAYOUT_ES = 1, LAYOUT_US = 2 };
enum { UI_EDIT = 0, UI_FILES = 1, UI_HELP = 2, UI_XFER = 3 };
enum { WF_OFF = 0, WF_STARTING = 1, WF_AP = 2, WF_FAIL = 3 };
enum { FA_NONE = 0, FA_RENAME = 1, FA_DELETE = 2 };
enum { BLE_IDLE = 0, BLE_SCAN, BLE_CONNECTING, BLE_READY };

/* arduino-esp32 3.3.x: initArduino() releases the RAM reserved for the
 * Bluetooth controller (~36 kB) unless a library declares BLE usage by
 * including esp32-hal-alloc-ble-mem.h. NimBLE-Arduino does not, so that RAM
 * goes to the heap and esp_bt_controller_init() overwrites it while setting up
 * its .bss/.data ("btdm: bss start ..." lines) -> TLSF assert on next malloc.
 * A strong btInUse() definition prevents the release on every 2.x/3.x core
 * (on 3.3.x it is detected as "userOverriddenBtInUse"). */
extern "C" bool btInUse(void) { return true; }

/*===========================================================================
 *  SECTION 1 - USER CONFIGURATION
 *=========================================================================*/
#define KBD_LAYOUT          LAYOUT_LATAM
#define KBD_NAME_FILTER     ""          // "" = any BLE keyboard; e.g. "K380"

#define EPD_SPI_HZ          10000000UL  // SSD1683: check tSCYCW in its datasheet
#define CLEAN_SOFT_PARTIALS 30          // partials before a full refresh at the next pause
#define CLEAN_HARD_PARTIALS 250         // partials before a forced full refresh while typing
#define CLEAN_PAUSE_MS      1500        // minimum pause to run the "soft" full refresh
#define CLEAN_IDLE_MS       60000UL     // final full refresh after typing stops

#define AUTOSAVE_MS         3000UL      // save after this much inactivity
#define REPEAT_DELAY_MS     500         // key repeat (generated by the host, not the keyboard)
#define REPEAT_RATE_MS      40
#define TAB_SPACES          2

#define DOC_MAX             (512UL * 1024UL)   // document bytes (in PSRAM)
#define AP_SSID             "Electgpl-Writer"
#define AP_PASS             "electgpl1234"     // WPA2: 8 characters minimum
#define AP_CHANNEL          6
#define AP_MAX_CLIENTS      2

#define DEBUG_SERIAL        1

/*===========================================================================
 *  SECTION 2 - PINS AND GEOMETRY
 *=========================================================================*/
#define EPD_SCK   12      // FSPICLK (IOMUX)
#define EPD_MOSI  11      // FSPID   (IOMUX)
#define EPD_RES   47
#define EPD_DC    46
#define EPD_CS    45
#define EPD_BUSY  48
#define EPD_PWR   7

#define BTN_EXIT  1
#define BTN_MENU  2
#define BTN_DOWN  4
#define BTN_OK    5
#define BTN_UP    6

#define EPD_W 800
#define EPD_H 272
#define Source_BYTES    (400/8)
#define Gate_BITS       272
#define ALLSCREEN_BYTES (Source_BYTES*Gate_BITS)

#define SCR_W   792
#define SCR_H   272
#define HDR_H   18
#define TXT_X   12
#define TXT_Y   24
#define CELL_W  SPLEEN12X24_W
#define CELL_H  SPLEEN12X24_H
#define ROWS    10                  // 24 + 10*24 = 264 px
#define COLS    64                  // 12 + 64*12 = 780 px
#define WRAP_W  (COLS - 1)          // last column is reserved for the cursor

#if DEBUG_SERIAL
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
#endif

/*===========================================================================
 *  SECTION 3 - GLOBAL STATE
 *=========================================================================*/
static uint8_t  ImageBW[EPD_W / 8 * EPD_H];      // 27200 B, internal RAM
static uint8_t *spiStream = nullptr;             // 13600 B, transmit order (PSRAM)

// Document (gap buffer)
static char    *gb = nullptr;
static uint32_t gbCap = 0, gStart = 0, gEnd = 0;
static uint32_t cursorPos = 0, topLine = 0, prefCol = UINT32_MAX;
static char     docName[40] = "note01.txt";
static bool     docDirty = false;

// Input
static QueueHandle_t qKeys;
static bool     capsLock = false;
static uint8_t  deadKey = 0;
static uint8_t  heldUsage = 0, heldMods = 0;
static uint32_t nextRepeatMs = 0;
static volatile uint32_t lastInputMs = 0;

// UI / display
static SemaphoreHandle_t docMutex;
static TaskHandle_t dispTaskH = nullptr;
static volatile uint32_t viewSeq = 1;
static volatile bool cleanRequest = false;
static uint8_t  uiMode = UI_EDIT;
static char     statusMsg[64] = "";
static uint32_t statusUntil = 0;
static char     fileNames[24][40];
static uint32_t fileSizes[24];
static uint8_t  nFiles = 0, fileSel = 0;
static uint8_t  fileAction = FA_NONE;        // pending action in the file list
static char     renameBuf[40];
static uint8_t  renameLen = 0;
static uint32_t fsUsed = 0, fsTotal = 0;     // cached by listFiles()

// BLE
static volatile uint8_t  bleState = BLE_IDLE;
static volatile bool     passkeyActive = false;
static volatile uint32_t passkeyVal = 0;
static volatile int      kbBattery = -1;
static volatile bool     reqUnpair = false;
static volatile int      numBonds = 0;
static hid_field_t hidFields[8];
static uint8_t     nHidFields = 0;
static uint16_t    rptHandle[8];
static uint8_t     rptId[8];
static uint8_t     nRpt = 0;
static uint8_t     prevKeys[4][16], prevN[4], prevRid[4], nPrev = 0;

// Network
static WebServer server(80);
static uint8_t   wifiState = WF_OFF;
static char      wifiIp[20] = "";
static char      wifiErr[128] = "";
static volatile uint8_t apClients = 0;
static File      upFile;
static Preferences prefs;

/*===========================================================================
 *  SECTION 4 - HARDWARE SPI AND SSD1683 SEQUENCES
 *=========================================================================*/
static inline void spiBegin(void) {
  SPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
}

void EPD_GPIOInit(void) {
  pinMode(EPD_RES, OUTPUT);
  pinMode(EPD_DC,  OUTPUT);
  pinMode(EPD_CS,  OUTPUT);
  pinMode(EPD_BUSY, INPUT);
  digitalWrite(EPD_CS, HIGH);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, -1);   // manual CS
}

void EPD_WR_REG(uint8_t reg) {
  spiBegin();
  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  SPI.transfer(reg);
  digitalWrite(EPD_CS, HIGH);
  digitalWrite(EPD_DC, HIGH);
  SPI.endTransaction();
}

void EPD_WR_DATA8(uint8_t dat) {
  spiBegin();
  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  SPI.transfer(dat);
  digitalWrite(EPD_CS, HIGH);
  SPI.endTransaction();
}

// Data burst with CS held low
static void EPD_WR_DATABUF(const uint8_t *p, size_t n) {
  spiBegin();
  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  SPI.writeBytes(p, n);
  digitalWrite(EPD_CS, HIGH);
  SPI.endTransaction();
}

static void EPD_WR_FILL(uint8_t v, size_t n) {
  memset(spiStream, v, ALLSCREEN_BYTES);
  while (n) {
    size_t k = n > ALLSCREEN_BYTES ? ALLSCREEN_BYTES : n;
    EPD_WR_DATABUF(spiStream, k);
    n -= k;
  }
}

// BUSY high = busy. Yields the CPU while waiting.
void EPD_READBUSY(void) {
  while (digitalRead(EPD_BUSY)) vTaskDelay(1);
}

void EPD_HW_RESET(void) {
  delay(100);
  digitalWrite(EPD_RES, LOW);
  delay(10);
  digitalWrite(EPD_RES, HIGH);
  delay(10);
  EPD_READBUSY();
}

void EPD_Update(void) {
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xF7);
  EPD_WR_REG(0x20);
  EPD_READBUSY();
}

void EPD_PartUpdate(void) {
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0xDC);
  EPD_WR_REG(0x20);
  EPD_READBUSY();
}

void EPD_FastMode1Init(void) {
  EPD_HW_RESET();
  EPD_READBUSY();
  EPD_WR_REG(0x12);           // SWRESET
  EPD_READBUSY();
  EPD_WR_REG(0x18);           // internal temperature sensor
  EPD_WR_DATA8(0x80);
  EPD_WR_REG(0x22);           // load temperature value
  EPD_WR_DATA8(0xB1);
  EPD_WR_REG(0x20);
  EPD_READBUSY();
  EPD_WR_REG(0x1A);           // forced temperature -> fast LUT
  EPD_WR_DATA8(0x64);
  EPD_WR_DATA8(0x00);
  EPD_WR_REG(0x22);
  EPD_WR_DATA8(0x91);
  EPD_WR_REG(0x20);
  EPD_READBUSY();
  EPD_WR_REG(0x3C);
  EPD_WR_DATA8(0x3);
  EPD_READBUSY();
}

void EPD_SetRAMMP(void) {
  EPD_WR_REG(0x11); EPD_WR_DATA8(0x05);
  EPD_WR_REG(0x44); EPD_WR_DATA8(0x00); EPD_WR_DATA8(0x31);
  EPD_WR_REG(0x45); EPD_WR_DATA8(0x0f); EPD_WR_DATA8(0x01);
                    EPD_WR_DATA8(0x00); EPD_WR_DATA8(0x00);
}
void EPD_SetRAMMA(void) {
  EPD_WR_REG(0x4e); EPD_WR_DATA8(0x00);
  EPD_WR_REG(0x4f); EPD_WR_DATA8(0x0f); EPD_WR_DATA8(0x01);
}
void EPD_SetRAMSP(void) {
  EPD_WR_REG(0x91); EPD_WR_DATA8(0x04);
  EPD_WR_REG(0xc4); EPD_WR_DATA8(0x31); EPD_WR_DATA8(0x00);
  EPD_WR_REG(0xc5); EPD_WR_DATA8(0x0f); EPD_WR_DATA8(0x01);
                    EPD_WR_DATA8(0x00); EPD_WR_DATA8(0x00);
}
void EPD_SetRAMSA(void) {
  EPD_WR_REG(0xce); EPD_WR_DATA8(0x31);
  EPD_WR_REG(0xcf); EPD_WR_DATA8(0x0f); EPD_WR_DATA8(0x01);
}

void EPD_Clear_R26A6H(void) {
  EPD_SetRAMMA();  EPD_WR_REG(0x26); EPD_WR_FILL(0xFF, ALLSCREEN_BYTES);
  EPD_SetRAMSA();  EPD_WR_REG(0xA6); EPD_WR_FILL(0xFF, ALLSCREEN_BYTES);
}

void EPD_Display_Clear(void) {
  EPD_SetRAMMP();
  EPD_SetRAMMA(); EPD_WR_REG(0x24); EPD_WR_FILL(0xFF, ALLSCREEN_BYTES);
  EPD_SetRAMMA(); EPD_WR_REG(0x26); EPD_WR_FILL(0x00, ALLSCREEN_BYTES);
  EPD_SetRAMSP();
  EPD_SetRAMSA(); EPD_WR_REG(0xA4); EPD_WR_FILL(0xFF, ALLSCREEN_BYTES);
  EPD_SetRAMSA(); EPD_WR_REG(0xA6); EPD_WR_FILL(0x00, ALLSCREEN_BYTES);
}

// Same column-major walk as the original driver: master gets byte
// columns 0..49, slave gets 50..99.
static void buildStream(uint8_t col0) {
  uint32_t k = 0;
  for (uint8_t c = col0; c < col0 + Source_BYTES; c++)
    for (uint16_t l = 0; l < Gate_BITS; l++)
      spiStream[k++] = ImageBW[l * Source_BYTES * 2 + c];
}

void EPD_Display(void) {
  EPD_SetRAMMP();
  EPD_SetRAMMA();
  EPD_WR_REG(0x24);
  buildStream(0);
  EPD_WR_DATABUF(spiStream, ALLSCREEN_BYTES);

  EPD_SetRAMSP();
  EPD_SetRAMSA();
  EPD_WR_REG(0xA4);
  buildStream(Source_BYTES);
  EPD_WR_DATABUF(spiStream, ALLSCREEN_BYTES);
}

static void epdFullClear(void) {      // anti-ghosting full refresh (original sequence)
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();
}

static void epdPush(void) {           // partial refresh (original sequence)
  EPD_Display();
  EPD_PartUpdate();
}

/*===========================================================================
 *  SECTION 5 - DRAWING PRIMITIVES (180 deg rotation, 8 px seam at x=396)
 *=========================================================================*/
static inline void px(int x, int y, bool black) {
  if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
  int X = (x >= 396) ? x + 8 : x;
  X = EPD_W - X - 1;
  int Y = EPD_H - y - 1;
  uint32_t a = (uint32_t)(X >> 3) + (uint32_t)Y * (EPD_W / 8);
  uint8_t  m = 0x80 >> (X & 7);
  if (black) ImageBW[a] &= ~m; else ImageBW[a] |= m;
}

static void paintClear(void) { memset(ImageBW, 0xFF, sizeof(ImageBW)); }

static void fillRect(int x, int y, int w, int h, bool black) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) px(i, j, black);
}

static void hLine(int x0, int x1, int y, bool black) {
  for (int x = x0; x <= x1; x++) px(x, y, black);
}

static inline uint8_t glyphIdx(uint8_t c) {
  if (c >= 32 && c < 127) return c - 32;
  if (c >= 160)           return c - 160 + 95;
  return '?' - 32;
}

// Draws only the glyph's set pixels, using color 'fg'
static void glyph24(int x, int y, uint8_t c, bool fg, uint8_t scale = 1) {
  const uint16_t *g = spleen12x24[glyphIdx(c)];
  for (int r = 0; r < CELL_H; r++) {
    uint16_t bits = g[r];
    if (!bits) continue;
    for (int col = 0; col < CELL_W; col++)
      if (bits & (0x800 >> col)) {
        if (scale == 1) px(x + col, y + r, fg);
        else fillRect(x + col * scale, y + r * scale, scale, scale, fg);
      }
  }
}

static void glyph16(int x, int y, uint8_t c, bool fg) {
  const uint8_t *g = spleen8x16[glyphIdx(c)];
  for (int r = 0; r < SPLEEN8X16_H; r++) {
    uint8_t bits = g[r];
    for (int col = 0; col < 8; col++)
      if (bits & (0x80 >> col)) px(x + col, y + r, fg);
  }
}

// Decodes UTF-8 to ISO-8859-1 (anything that does not fit becomes '?')
static int utf8Next(const char *&p) {
  uint8_t c = (uint8_t)*p++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0 && ((uint8_t)*p & 0xC0) == 0x80) {
    int cp = ((c & 0x1F) << 6) | ((uint8_t)*p++ & 0x3F);
    return cp < 256 ? cp : '?';
  }
  while (((uint8_t)*p & 0xC0) == 0x80) p++;
  return '?';
}

static int strLen8(const char *s) { int n = 0; while (*s) { utf8Next(s); n++; } return n; }

static void text16(int x, int y, const char *s, bool fg) {
  while (*s) { glyph16(x, y, (uint8_t)utf8Next(s), fg); x += 8; }
}

static void text24(int x, int y, const char *s, bool fg, uint8_t scale = 1) {
  while (*s) { glyph24(x, y, (uint8_t)utf8Next(s), fg, scale); x += CELL_W * scale; }
}

static void text24C(int y, const char *s, uint8_t scale = 1) {
  text24((SCR_W - strLen8(s) * CELL_W * scale) / 2, y, s, true, scale);
}

/*===========================================================================
 *  SECTION 6 - DOCUMENT: GAP BUFFER AND WORD-WRAP LAYOUT
 *=========================================================================*/
static inline uint32_t docLen(void) { return gbCap - (gEnd - gStart); }
static inline char docAt(uint32_t i) { return i < gStart ? gb[i] : gb[i + (gEnd - gStart)]; }

static void gapMove(uint32_t pos) {
  if (pos < gStart) {
    uint32_t n = gStart - pos;
    memmove(gb + gEnd - n, gb + pos, n);
    gStart -= n; gEnd -= n;
  } else if (pos > gStart) {
    uint32_t n = pos - gStart;
    memmove(gb + gStart, gb + gEnd, n);
    gStart += n; gEnd += n;
  }
}

static bool docInsert(uint32_t pos, char c) {
  if (gStart == gEnd) return false;
  gapMove(pos);
  gb[gStart++] = c;
  return true;
}

static void docDelete(uint32_t pos) {
  if (pos >= docLen()) return;
  gapMove(pos);
  gEnd++;
}

static void docClear(void) { gStart = 0; gEnd = gbCap; cursorPos = 0; topLine = 0; }

// Computes the visible end of the line starting at 'pos'.
// end  = first index NOT drawn; next = start of the following line.
// next = len+1 marks the last line.
static void lineEnd(uint32_t pos, uint32_t len, uint32_t *end, uint32_t *next) {
  uint32_t lastSpace = UINT32_MAX;
  for (uint32_t k = pos;; k++) {
    if (k == len)  { *end = len; *next = len + 1; return; }
    char c = docAt(k);
    if (c == '\n') { *end = k; *next = k + 1; return; }
    if (k - pos == WRAP_W) {
      if (c == ' ')                            { *end = k; *next = k + 1; return; }
      if (lastSpace != UINT32_MAX && lastSpace > pos) { *end = lastSpace; *next = lastSpace + 1; return; }
      *end = k; *next = k; return;          // word longer than the line
    }
    if (c == ' ') lastSpace = k;
  }
}

static void locate(uint32_t cur, locate_t *L) {
  uint32_t len = docLen(), pos = 0, line = 0, end, next;
  bool found = false;
  for (;;) {
    lineEnd(pos, len, &end, &next);
    if (!found && cur >= pos && cur < next) { L->curLine = line; L->curCol = cur - pos; found = true; }
    if (next > len) break;
    pos = next; line++;
  }
  L->totalLines = line + 1;
  if (!found) { L->curLine = line; L->curCol = 0; }
}

static bool lineAt(uint32_t n, uint32_t *start, uint32_t *end) {
  uint32_t len = docLen(), pos = 0, e, next;
  for (uint32_t line = 0;; line++) {
    lineEnd(pos, len, &e, &next);
    if (line == n) { *start = pos; *end = e; return true; }
    if (next > len) return false;
    pos = next;
  }
}

static void fixViewport(void) {
  locate_t L; locate(cursorPos, &L);
  if (L.curLine < topLine)
    topLine = (L.curLine > ROWS / 2) ? L.curLine - ROWS / 2 : 0;
  else if (L.curLine >= topLine + ROWS)
    topLine = L.curLine - ROWS / 2;      // half-page jump (typewriter style)
  if (topLine >= L.totalLines) topLine = L.totalLines ? L.totalLines - 1 : 0;
}

static uint32_t wordCount(void) {
  uint32_t n = 0, len = docLen(); bool in = false;
  for (uint32_t i = 0; i < len; i++) {
    char c = docAt(i);
    bool ws = (c == ' ' || c == '\n');
    if (!ws && !in) n++;
    in = !ws;
  }
  return n;
}

/*===========================================================================
 *  SECTION 7 - FILES (LittleFS, UTF-8 on disk, Latin-1 in memory)
 *=========================================================================*/
static void docPath(const char *name, char *out, size_t n) { snprintf(out, n, "/docs/%s", name); }

static bool saveDoc(void) {
  uint32_t t0 = millis();
  char path[56]; docPath(docName, path, sizeof(path));
  File f = LittleFS.open("/docs/.tmp", "w");
  if (!f) return false;
  uint8_t buf[512]; size_t k = 0;
  uint32_t len = docLen();
  for (uint32_t i = 0; i < len; i++) {
    uint8_t c = (uint8_t)docAt(i);
    if (k > sizeof(buf) - 2) { f.write(buf, k); k = 0; }
    if (c < 0x80) buf[k++] = c;
    else { buf[k++] = 0xC0 | (c >> 6); buf[k++] = 0x80 | (c & 0x3F); }
  }
  if (k) f.write(buf, k);
  f.close();
  bool ok = LittleFS.rename("/docs/.tmp", path);   // littlefs rename is atomic
  if (ok) {
    docDirty = false;
    prefs.putString("doc", docName);
    prefs.putUInt("cur", cursorPos);
  }
  DBG("[FS] saved %s (%lu B) %s in %lu ms\n", path, (unsigned long)len, ok ? "OK" : "ERROR",
      (unsigned long)(millis() - t0));
  return ok;
}

static void loadDoc(const char *name) {
  char path[56]; docPath(name, path, sizeof(path));
  xSemaphoreTake(docMutex, portMAX_DELAY);
  docClear();
  strlcpy(docName, name, sizeof(docName));
  File f = LittleFS.open(path, "r");
  if (f) {
    uint8_t lead = 0;
    while (f.available()) {
      uint8_t c = f.read();
      int cp = -1;
      if (lead) { cp = ((lead & 0x1F) << 6) | (c & 0x3F); lead = 0; if (cp > 255) cp = '?'; }
      else if (c < 0x80) cp = c;
      else if ((c & 0xE0) == 0xC0) lead = c;
      else if ((c & 0xC0) == 0x80) continue;         // 3/4-byte continuation
      else cp = '?';
      if (cp < 0 || cp == '\r') continue;
      if (cp == '\t') cp = ' ';
      if (!docInsert(docLen(), (char)cp)) break;
    }
    f.close();
  }
  cursorPos = docLen();
  docDirty = false;
  fixViewport();
  xSemaphoreGive(docMutex);
  prefs.putString("doc", docName);
  DBG("[FS] opened %s (%lu B)\n", path, (unsigned long)docLen());
}

static void newDocName(char *out, size_t n) {
  char path[56];
  for (int i = 1; i < 100; i++) {
    snprintf(out, n, "note%02d.txt", i);
    docPath(out, path, sizeof(path));
    if (!LittleFS.exists(path)) return;
  }
  snprintf(out, n, "note%08lx.txt", (unsigned long)esp_random());
}

static void listFiles(void) {
  nFiles = 0;
  File d = LittleFS.open("/docs");
  if (!d) return;
  File e;
  while ((e = d.openNextFile()) && nFiles < 24) {
    const char *nm = e.name();
    const char *b = strrchr(nm, '/'); b = b ? b + 1 : nm;
    if (b[0] != '.') { fileSizes[nFiles] = e.size(); strlcpy(fileNames[nFiles++], b, 40); }
    e.close();
  }
  d.close();
  fsUsed  = LittleFS.usedBytes();
  fsTotal = LittleFS.totalBytes();
  if (fileSel >= nFiles) fileSel = nFiles ? nFiles - 1 : 0;
}

/*===========================================================================
 *  SECTION 8 - RENDER
 *=========================================================================*/
static void requestRender(void) {
  viewSeq = viewSeq + 1;
  if (dispTaskH) xTaskNotifyGive(dispTaskH);
}

static void setStatus(const char *s, uint32_t ms = 2500) {
  strlcpy(statusMsg, s, sizeof(statusMsg));
  statusUntil = millis() + ms;
  requestRender();
}

static void drawHeader(uint32_t words) {
  char left[64], right[96];
  fillRect(0, 0, SCR_W, HDR_H, true);

  const char *bt = (bleState == BLE_READY) ? "BT OK" :
                   (bleState == BLE_CONNECTING) ? "BT ..." : "BT --";
  char bat[16] = "";
  if (kbBattery >= 0) snprintf(bat, sizeof(bat), "  KB %d%%", kbBattery);
  if (statusMsg[0] && millis() < statusUntil)
    snprintf(right, sizeof(right), "%s  |  %s%s ", statusMsg, bt, bat);
  else if (wifiState == WF_AP)
    snprintf(right, sizeof(right), "AP %s  |  %lu words  |  %s%s ", wifiIp, (unsigned long)words, bt, bat);
  else
    snprintf(right, sizeof(right), "%lu words  |  %s%s%s ",
             (unsigned long)words, capsLock ? "CAPS  |  " : "", bt, bat);
  text16(SCR_W - strLen8(right) * 8, 1, right, false);

  snprintf(left, sizeof(left), " ELECTGPL WRITER  %s%s   Ctrl+H help", docName, docDirty ? " *" : "");
  if (strLen8(left) + strLen8(right) > SCR_W / 8 - 1)          // drop the hint if it does not fit
    snprintf(left, sizeof(left), " ELECTGPL WRITER  %s%s", docName, docDirty ? " *" : "");
  text16(0, 1, left, false);
}

static void renderEdit(void) {
  uint32_t len = docLen(), pos = 0, end, next;
  locate_t L; locate(cursorPos, &L);

  drawHeader(wordCount());

  // Skip ahead to topLine
  for (uint32_t l = 0; l < topLine; l++) {
    lineEnd(pos, len, &end, &next);
    if (next > len) break;
    pos = next;
  }
  for (int r = 0; r < ROWS; r++) {
    if (pos > len) break;
    lineEnd(pos, len, &end, &next);
    int y = TXT_Y + r * CELL_H;
    for (uint32_t i = pos; i < end; i++)
      glyph24(TXT_X + (i - pos) * CELL_W, y, (uint8_t)docAt(i), true);
    if (cursorPos >= pos && cursorPos < next) {            // block cursor
      int cx = TXT_X + (cursorPos - pos) * CELL_W;
      fillRect(cx, y, CELL_W, CELL_H, true);
      if (cursorPos < end) glyph24(cx, y, (uint8_t)docAt(cursorPos), false);
    }
    if (next > len) break;
    pos = next;
  }

  // Scroll bar
  const int sx = SCR_W - 6, sy0 = TXT_Y, sh = ROWS * CELL_H;
  for (int y = sy0; y < sy0 + sh; y += 4) px(sx + 1, y, true);
  if (L.totalLines > ROWS) {
    int th = sh * ROWS / L.totalLines; if (th < 8) th = 8;
    int ty = sy0 + (sh - th) * topLine / (L.totalLines - ROWS > 0 ? L.totalLines - ROWS : 1);
    if (ty > sy0 + sh - th) ty = sy0 + sh - th;
    fillRect(sx, ty, 3, th, true);
  }
}

static void renderFiles(void) {
  drawHeader(wordCount());
  text24(TXT_X, TXT_Y, "FILES", true);
  char hint[96];
  if (fileAction == FA_RENAME)
    snprintf(hint, sizeof(hint), "Type the new name   Enter: confirm   Esc: cancel");
  else if (fileAction == FA_DELETE && nFiles)
    snprintf(hint, sizeof(hint), "Delete %s ?   Y: yes   any other key: no", fileNames[fileSel]);
  else
    snprintf(hint, sizeof(hint), "Enter: open   N: new   R: rename   D/Del: delete   Esc: back");
  text16(TXT_X + 84, TXT_Y + 5, hint, true);
  hLine(TXT_X, SCR_W - TXT_X, TXT_Y + 27, true);

  const int perPage = 8, y0 = TXT_Y + 32;
  int first = (fileSel / perPage) * perPage;
  for (int i = 0; i < perPage && first + i < nFiles; i++) {
    int idx = first + i, y = y0 + i * 26;
    char line[72];
    bool open = !strcmp(fileNames[idx], docName);
    if (idx == fileSel && fileAction == FA_RENAME)
      snprintf(line, sizeof(line), "%s_", renameBuf);
    else
      snprintf(line, sizeof(line), "%-36s %8lu B %s", fileNames[idx],
               (unsigned long)fileSizes[idx], open ? "(open)" : "");
    if (idx == fileSel) { fillRect(TXT_X - 4, y - 1, SCR_W - 2 * TXT_X + 8, 25, true); text24(TXT_X, y, line, false); }
    else text24(TXT_X, y, line, true);
  }
  if (!nFiles) text24C(y0 + 60, "(no files)");

  char foot[96];
  snprintf(foot, sizeof(foot), "Open doc: %lu of %lu kB max   |   Flash: %lu kB used of %lu kB",
           (unsigned long)((docLen() + 1023) / 1024), (unsigned long)(gbCap / 1024),
           (unsigned long)(fsUsed / 1024), (unsigned long)(fsTotal / 1024));
  text16(SCR_W - TXT_X - strLen8(foot) * 8, SCR_H - 17, foot, true);
}

static void renderHelp(void) {
  drawHeader(wordCount());
  static const char *L[] = {
    "EDITOR",
    "Ctrl+S      Save now (autosave: 3 s idle)",
    "Ctrl+O      File list",
    "Ctrl+N      New document",
    "Ctrl+W      File transfer (WiFi hotspot)",
    "Ctrl+R      Full refresh (anti-ghosting)",
    "Ctrl+H      This help",
    "Ctrl+Home   Start of document",
    "Ctrl+End    End of document",
    "PgUp/PgDn   Page up / down",
    "Esc         Cancel pending dead key",
    "",
    "Press any key to return",
  };
  static const char *R[] = {
    "FILE LIST (Ctrl+O)",
    "Up/Down     Select      Enter  Open",
    "N           New         R      Rename",
    "D / Del     Delete (Y confirms)",
    "Esc         Back to the editor",
    "",
    "STORAGE: internal flash, LittleFS /docs, UTF-8",
    "TRANSFER: join WiFi " AP_SSID,
    "  password " AP_PASS,
    "  then browse http://192.168.4.1",
    "",
    "BUTTONS: MENU refresh  OK save  UP/DN page",
    "         EXIT held 3 s: forget keyboard",
  };
  for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++)
    text16(TXT_X, TXT_Y + i * 18, L[i], true);
  for (unsigned i = 0; i < sizeof(R) / sizeof(R[0]); i++)
    text16(SCR_W / 2 + 8, TXT_Y + i * 18, R[i], true);
  for (int y = TXT_Y; y < SCR_H - 8; y += 3) px(SCR_W / 2 - 6, y, true);
}

static void renderXfer(void) {
  drawHeader(wordCount());
  text24(TXT_X, TXT_Y, "FILE TRANSFER", true);
  text16(TXT_X + 180, TXT_Y + 5, "Esc or Ctrl+W: close and turn WiFi off", true);
  hLine(TXT_X, SCR_W - TXT_X, TXT_Y + 27, true);
  const int y0 = TXT_Y + 38;
  if (wifiState == WF_AP) {
    text24(TXT_X, y0,       "1. On the PC, join WiFi:", true);
    text24(TXT_X + 27 * CELL_W, y0, AP_SSID, true);
    text24(TXT_X, y0 + 32,  "   Password:", true);
    text24(TXT_X + 27 * CELL_W, y0 + 32, AP_PASS, true);
    text24(TXT_X, y0 + 72,  "2. Open in a browser:", true);
    char url[40]; snprintf(url, sizeof(url), "http://%s", wifiIp);
    fillRect(TXT_X + 27 * CELL_W - 6, y0 + 69, (strlen(url) * CELL_W) + 12, 30, true);
    text24(TXT_X + 27 * CELL_W, y0 + 72, url, false);
    char st[64];
    snprintf(st, sizeof(st), "Hotspot up on channel %d   |   PCs connected: %u", AP_CHANNEL, apClients);
    text16(TXT_X, y0 + 124, st, true);
    text16(TXT_X, y0 + 146, "Download, upload (.txt/.md) or delete notes from the web page.", true);
    text16(TXT_X, y0 + 164, "The keyboard stays connected: WiFi and BLE share the radio.", true);
  } else if (wifiState == WF_STARTING || wifiState == WF_OFF) {
    text24C(y0 + 50, "Starting WiFi hotspot...");
  } else {
    text24C(y0 + 20, "The WiFi hotspot could not start.");
    char e1[128]; strlcpy(e1, wifiErr, sizeof(e1));
    char *bar = strchr(e1, '|');
    if (bar) { *bar = 0; text24C(y0 + 55, e1); text16((SCR_W - strLen8(bar + 2) * 8) / 2, y0 + 90, bar + 2, true); }
    else text24C(y0 + 55, e1);
    text16(TXT_X, y0 + 130, "Press Esc and try Ctrl+W again. If it persists, send the [WIFI] serial log lines.", true);
  }
}

static void renderPair(void) {
  fillRect(0, 0, SCR_W, HDR_H, true);
  text16(0, 1, " ELECTGPL WRITER  -  BLUETOOTH PAIRING", false);
  if (passkeyActive) {
    char pk[8]; snprintf(pk, sizeof(pk), "%06lu", (unsigned long)passkeyVal);
    text24C(40, "Type this code on the keyboard and press Enter:");
    text24C(90, pk, 3);                                   // 36x72 px digits
    text24C(200, "(LE Secure Connections pairing, Passkey Entry)");
  } else {
    text24C(60,  "Searching for a BLE keyboard...");
    text24C(120, "On the K380s: hold an Easy-Switch key for 3 s");
    text24C(150, "until its LED blinks fast.");
    text24C(210, bleState == BLE_CONNECTING ? "Connecting..." : "");
  }
}

static void renderFrame(void) {
  paintClear();
  bool needPair = passkeyActive ||
                  (bleState != BLE_READY && numBonds == 0);
  if (needPair)               renderPair();
  else if (uiMode == UI_FILES) renderFiles();
  else if (uiMode == UI_HELP)  renderHelp();
  else if (uiMode == UI_XFER)  renderXfer();
  else                        renderEdit();
}

// Display task: natural coalescing. While the panel refreshes, keystrokes
// pile up; when BUSY is released the most recent state gets drawn.
static void displayTask(void *) {
  uint32_t done = 0, partials = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    uint32_t now  = millis();
    uint32_t idle = now - lastInputMs;
    bool want  = (viewSeq != done);
    bool clean = cleanRequest ||
                 (partials >= CLEAN_HARD_PARTIALS) ||
                 (partials >= CLEAN_SOFT_PARTIALS && idle >= CLEAN_PAUSE_MS) ||
                 (partials > 0 && idle >= CLEAN_IDLE_MS);
    if (!want && !clean) continue;

    uint32_t seq = viewSeq;
    uint32_t t0 = millis();
    xSemaphoreTake(docMutex, portMAX_DELAY);
    renderFrame();
    xSemaphoreGive(docMutex);
    uint32_t t1 = millis();

    if (clean) { cleanRequest = false; epdFullClear(); partials = 0; }
    epdPush();
    if (!clean) partials++;
    done = seq;
    DBG("[EPD] %s render=%lums total=%lums partials=%lu\n", clean ? "FULL   " : "partial",
        (unsigned long)(t1 - t0), (unsigned long)(millis() - t0), (unsigned long)partials);
  }
}

/*===========================================================================
 *  SECTION 9 - KEYBOARD LAYOUT (HID usage -> ISO-8859-1)
 *=========================================================================*/
#define DK_ACUTE 1
#define DK_DIAER 2
#define DK_GRAVE 3
#define DK_CIRC  4

typedef struct { uint8_t u, n, s, a; } keymap_t;

static const keymap_t KM_LATAM[] = {
  {0x1E,'1','!',0},{0x1F,'2','"',0},{0x20,'3','#',0},{0x21,'4','$',0},{0x22,'5','%',0},
  {0x23,'6','&',0},{0x24,'7','/',0},{0x25,'8','(',0},{0x26,'9',')',0},{0x27,'0','=',0},
  {0x2D,'\'','?','\\'},{0x2E,0xBF,0xA1,0},{0x2F,DK_ACUTE,DK_DIAER,0},{0x30,'+','*','~'},
  {0x31,'}',']','`'},{0x32,'}',']','`'},{0x33,0xF1,0xD1,0},{0x34,'{','[','^'},
  {0x35,'|',0xB0,0xAC},{0x36,',',';',0},{0x37,'.',':',0},{0x38,'-','_',0},{0x64,'<','>',0},
};
static const keymap_t KM_ES[] = {
  {0x1E,'1','!','|'},{0x1F,'2','"','@'},{0x20,'3',0xB7,'#'},{0x21,'4','$','~'},{0x22,'5','%',0},
  {0x23,'6','&',0xAC},{0x24,'7','/',0},{0x25,'8','(',0},{0x26,'9',')',0},{0x27,'0','=',0},
  {0x2D,'\'','?',0},{0x2E,0xA1,0xBF,0},{0x2F,DK_GRAVE,DK_CIRC,'['},{0x30,'+','*',']'},
  {0x31,0xE7,0xC7,'}'},{0x32,0xE7,0xC7,'}'},{0x33,0xF1,0xD1,0},{0x34,DK_ACUTE,DK_DIAER,'{'},
  {0x35,0xBA,0xAA,'\\'},{0x36,',',';',0},{0x37,'.',':',0},{0x38,'-','_',0},{0x64,'<','>',0},
};
static const keymap_t KM_US[] = {
  {0x1E,'1','!',0},{0x1F,'2','@',0},{0x20,'3','#',0},{0x21,'4','$',0},{0x22,'5','%',0},
  {0x23,'6','^',0},{0x24,'7','&',0},{0x25,'8','*',0},{0x26,'9','(',0},{0x27,'0',')',0},
  {0x2D,'-','_',0},{0x2E,'=','+',0},{0x2F,'[','{',0},{0x30,']','}',0},{0x31,'\\','|',0},
  {0x32,'\\','|',0},{0x33,';',':',0},{0x34,'\'','"',0},{0x35,'`','~',0},{0x36,',','<',0},
  {0x37,'.','>',0},{0x38,'/','?',0},{0x64,'\\','|',0},
};

static uint8_t composeDead(uint8_t dk, uint8_t c) {
  static const char base[] = "aeiouAEIOU";
  static const uint8_t ac[] = {0xE1,0xE9,0xED,0xF3,0xFA,0xC1,0xC9,0xCD,0xD3,0xDA};
  static const uint8_t di[] = {0xE4,0xEB,0xEF,0xF6,0xFC,0xC4,0xCB,0xCF,0xD6,0xDC};
  static const uint8_t gr[] = {0xE0,0xE8,0xEC,0xF2,0xF9,0xC0,0xC8,0xCC,0xD2,0xD9};
  static const uint8_t ci[] = {0xE2,0xEA,0xEE,0xF4,0xFB,0xC2,0xCA,0xCE,0xD4,0xDB};
  const char *p = strchr(base, c);
  if (!p || !c) {
    if (dk == DK_ACUTE && c == 'y') return 0xFD;
    if (dk == DK_DIAER && c == 'y') return 0xFF;
    return 0;
  }
  int i = p - base;
  switch (dk) { case DK_ACUTE: return ac[i]; case DK_DIAER: return di[i];
                case DK_GRAVE: return gr[i]; case DK_CIRC:  return ci[i]; }
  return 0;
}

static uint8_t deadChar(uint8_t dk) {
  switch (dk) { case DK_ACUTE: return 0xB4; case DK_DIAER: return 0xA8;
                case DK_GRAVE: return '`';  case DK_CIRC:  return '^'; }
  return 0;
}

// Returns the Latin-1 char or a dead-key code (1..4); 0 = nothing
static uint8_t translate(uint8_t u, bool shift, bool altgr) {
  if (u >= 0x04 && u <= 0x1D) {
    if (altgr) return (KBD_LAYOUT == LAYOUT_LATAM && u == 0x14) ? '@' : 0;   // AltGr+Q
    char c = 'a' + (u - 0x04);
    return (shift ^ capsLock) ? (c - 32) : c;
  }
  if (u == 0x2C) return ' ';
  const keymap_t *km; size_t n;
  if (KBD_LAYOUT == LAYOUT_ES)      { km = KM_ES;    n = sizeof(KM_ES) / sizeof(km[0]); }
  else if (KBD_LAYOUT == LAYOUT_US) { km = KM_US;    n = sizeof(KM_US) / sizeof(km[0]); }
  else                              { km = KM_LATAM; n = sizeof(KM_LATAM) / sizeof(km[0]); }
  for (size_t i = 0; i < n; i++) {
    if (km[i].u != u) continue;
    if (altgr) return km[i].a;
    uint8_t c = shift ? km[i].s : km[i].n;
    if (capsLock && !shift && (c == 0xF1 || c == 0xE7)) c -= 0x20;  // ñ->Ñ, ç->Ç
    return c;
  }
  return 0;
}

/*===========================================================================
 *  SECTION 10 - EDITOR (keyboard actions)
 *=========================================================================*/
static void editInsert(uint8_t c) {
  if (!docInsert(cursorPos, (char)c)) { setStatus("Document full"); return; }
  cursorPos++;
  docDirty = true;
}

static void moveVertical(int d) {
  locate_t L; locate(cursorPos, &L);
  if (prefCol == UINT32_MAX) prefCol = L.curCol;
  int64_t t = (int64_t)L.curLine + d;
  if (t < 0) t = 0;
  if (t >= (int64_t)L.totalLines) t = L.totalLines - 1;
  uint32_t s, e;
  if (lineAt((uint32_t)t, &s, &e)) cursorPos = s + ((e - s) < prefCol ? (e - s) : prefCol);
}

static void startFiles(void) {
  if (docDirty) saveDoc();
  listFiles();
  for (uint8_t i = 0; i < nFiles; i++) if (!strcmp(fileNames[i], docName)) fileSel = i;
  uiMode = UI_FILES;
}

static void newDoc(void) {
  if (docDirty) saveDoc();
  char nm[40]; newDocName(nm, sizeof(nm));
  xSemaphoreTake(docMutex, portMAX_DELAY);
  docClear();
  strlcpy(docName, nm, sizeof(docName));
  docDirty = true;
  uiMode = UI_EDIT;
  xSemaphoreGive(docMutex);
  saveDoc();
}

static void toggleWiFi(void);


static bool safeName(const String &n);

static void doRename(void) {
  String nn = renameBuf;
  if (nn.indexOf('.') < 0) nn += ".txt";
  if (!safeName(nn)) { setStatus("Invalid name (a-z 0-9 . _ -, .txt/.md)"); return; }
  char oldP[56], newP[56];
  docPath(fileNames[fileSel], oldP, sizeof(oldP));
  docPath(nn.c_str(), newP, sizeof(newP));
  if (!strcmp(oldP, newP)) { fileAction = FA_NONE; return; }
  if (LittleFS.exists(newP)) { setStatus("Name already in use"); return; }
  bool isOpen = !strcmp(fileNames[fileSel], docName);
  if (isOpen && docDirty) saveDoc();
  if (!LittleFS.rename(oldP, newP)) { setStatus("ERROR renaming"); return; }
  if (isOpen) { strlcpy(docName, nn.c_str(), sizeof(docName)); prefs.putString("doc", docName); }
  listFiles();
  for (uint8_t i = 0; i < nFiles; i++) if (nn == fileNames[i]) fileSel = i;
  fileAction = FA_NONE;
  setStatus("Renamed");
}

static void doDelete(void) {
  if (!nFiles) return;
  char path[56]; docPath(fileNames[fileSel], path, sizeof(path));
  bool isOpen = !strcmp(fileNames[fileSel], docName);
  if (isOpen) docDirty = false;                 // do not let autosave recreate it
  LittleFS.remove(path);
  listFiles();
  if (isOpen) {
    if (nFiles) loadDoc(fileNames[0]);
    else { newDoc(); listFiles(); }
  }
  uiMode = UI_FILES;
  setStatus("Deleted");
}

static void handleFilesKey(uint8_t u, uint8_t mods) {
  bool shift = mods & 0x22;
  if (fileAction == FA_RENAME) {
    if (u == 0x29) { fileAction = FA_NONE; return; }                           // Esc
    if (u == 0x2A) { if (renameLen) renameBuf[--renameLen] = 0; return; }      // Backspace
    if (u == 0x28 || u == 0x58) { doRename(); return; }                        // Enter
    uint8_t c = translate(u, shift, false);
    if (c > DK_CIRC && c < 0x80 && (isalnum(c) || c == '.' || c == '_' || c == '-') &&
        renameLen < 32) { renameBuf[renameLen++] = c; renameBuf[renameLen] = 0; }
    return;
  }
  if (fileAction == FA_DELETE) {
    if (u == 0x1C) doDelete();                                                  // Y
    fileAction = FA_NONE;
    return;
  }
  switch (u) {
    case 0x52: if (fileSel) fileSel--; break;                                   // up
    case 0x51: if (fileSel + 1 < nFiles) fileSel++; break;                      // down
    case 0x28: case 0x58:                                                       // Enter
      if (nFiles) loadDoc(fileNames[fileSel]);
      uiMode = UI_EDIT; break;
    case 0x29: uiMode = UI_EDIT; break;                                         // Esc
    case 0x11: newDoc(); setStatus("New document"); break;                      // N
    case 0x15:                                                                  // R
      if (nFiles) {
        strlcpy(renameBuf, fileNames[fileSel], sizeof(renameBuf));
        renameLen = strlen(renameBuf);
        fileAction = FA_RENAME;
      }
      break;
    case 0x07: case 0x4C: if (nFiles) fileAction = FA_DELETE; break;           // D / Del
  }
}

static void handlePress(uint8_t u, uint8_t mods, bool repeat) {
  bool ctrl  = mods & 0x11;
  bool shift = mods & 0x22;
  bool altgr = (mods & 0x40) || ((mods & 0x01) && (mods & 0x04));
  if (altgr) ctrl = false;

  if (u == 0x39) { if (!repeat) capsLock = !capsLock; requestRender(); return; }

  if (uiMode == UI_HELP) { if (!repeat) uiMode = UI_EDIT; requestRender(); return; }
  if (uiMode == UI_FILES) { handleFilesKey(u, mods); requestRender(); return; }
  if (uiMode == UI_XFER) {                                   // transfer screen: only exit keys
    if (!repeat && (u == 0x29 || (ctrl && u == 0x1A))) toggleWiFi();
    requestRender();
    return;
  }

  if (ctrl) {
    if (repeat) return;
    switch (u) {
      case 0x16: setStatus(saveDoc() ? "Saved" : "ERROR saving"); break;  // S
      case 0x12: startFiles(); break;                                              // O
      case 0x11: newDoc(); setStatus("New document"); break;                       // N
      case 0x0B: uiMode = UI_HELP; break;                                          // H
      case 0x1A: toggleWiFi(); break;                                              // W
      case 0x15: cleanRequest = true; break;                                       // R
      case 0x4A: case 0x4D:                                                        // Ctrl+Home/End
        xSemaphoreTake(docMutex, portMAX_DELAY);
        cursorPos = (u == 0x4A) ? 0 : docLen(); prefCol = UINT32_MAX; fixViewport();
        xSemaphoreGive(docMutex); break;
    }
    requestRender();
    return;
  }

  xSemaphoreTake(docMutex, portMAX_DELAY);
  uint32_t len = docLen();
  bool vertical = false;
  switch (u) {
    case 0x28: case 0x58: deadKey = 0; editInsert('\n'); break;
    case 0x2A:                                                     // Backspace
      if (deadKey) { deadKey = 0; break; }
      if (cursorPos) { cursorPos--; docDelete(cursorPos); docDirty = true; }
      break;
    case 0x4C: if (cursorPos < len) { docDelete(cursorPos); docDirty = true; } break;  // Delete
    case 0x2B: for (int i = 0; i < TAB_SPACES; i++) editInsert(' '); break;
    case 0x29: deadKey = 0; break;                                 // Esc
    case 0x4F: if (cursorPos < len) cursorPos++; break;            // right
    case 0x50: if (cursorPos) cursorPos--; break;                  // left
    case 0x51: moveVertical(+1); vertical = true; break;           // down
    case 0x52: moveVertical(-1); vertical = true; break;           // up
    case 0x4E: moveVertical(+(ROWS - 1)); vertical = true; break;  // PgDn
    case 0x4B: moveVertical(-(ROWS - 1)); vertical = true; break;  // PgUp
    case 0x4A: case 0x4D: {                                        // Home / End
      locate_t L; locate(cursorPos, &L);
      uint32_t s, e;
      if (lineAt(L.curLine, &s, &e)) cursorPos = (u == 0x4A) ? s : e;
    } break;
    default: {
      uint8_t c = translate(u, shift, altgr);
      if (!c) break;
      if (c <= DK_CIRC) {                                           // dead key
        if (deadKey) { editInsert(deadChar(deadKey)); deadKey = (deadKey == c) ? 0 : c; }
        else deadKey = c;
        break;
      }
      if (deadKey) {                          // ´ + vowel -> accented vowel
        uint8_t dk = deadKey, k = composeDead(dk, c);
        deadKey = 0;
        if (k) c = k;
        else { editInsert(deadChar(dk)); if (c == ' ') break; }   // ´ + space -> ´
      }
      editInsert(c);
    }
  }
  if (!vertical) prefCol = UINT32_MAX;
  fixViewport();
  xSemaphoreGive(docMutex);
  requestRender();
}

static bool repeatable(uint8_t u) {
  return (u >= 0x04 && u <= 0x38 && u != 0x28 && u != 0x29 && u != 0x39) ||
         u == 0x4C || (u >= 0x4F && u <= 0x52) || u == 0x64;
}

static void processKeys(void) {
  key_evt_t e;
  while (xQueueReceive(qKeys, &e, 0) == pdTRUE) {
    lastInputMs = millis();
    if (e.type) {
      handlePress(e.usage, e.mods, false);
      if (repeatable(e.usage) && !(e.mods & 0x11)) {
        heldUsage = e.usage; heldMods = e.mods;
        nextRepeatMs = millis() + REPEAT_DELAY_MS;
      } else heldUsage = 0;
    } else if (e.usage == heldUsage) heldUsage = 0;
  }
  if (heldUsage && (int32_t)(millis() - nextRepeatMs) >= 0) {
    lastInputMs = millis();
    handlePress(heldUsage, heldMods, true);
    nextRepeatMs += REPEAT_RATE_MS;
  }
}

/*===========================================================================
 *  SECTION 11 - HID: REPORT MAP PARSER AND REPORT DECODING
 *=========================================================================*/
static uint32_t getBits(const uint8_t *d, size_t n, uint32_t off, uint8_t size) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < size && i < 32; i++) {
    uint32_t b = off + i;
    if ((b >> 3) >= n) break;
    if (d[b >> 3] & (1 << (b & 7))) v |= 1UL << i;
  }
  return v;
}

// Minimal HID descriptor parser (HID 1.11, sec. 6.2.2): extracts the Input
// fields of the Keyboard/Keypad page (0x07) with their bit offsets.
static uint8_t parseReportMap(const uint8_t *p, size_t n) {
  struct { uint16_t page; uint32_t size, count; uint8_t rid; int32_t logMin; } g = {0, 0, 0, 0, 0}, stk[4];
  uint8_t sp = 0;
  uint16_t usages[16]; uint8_t nUs = 0;
  uint16_t uMin = 0; bool haveMin = false;
  struct { uint8_t rid; uint16_t off; } offs[8]; uint8_t nOffs = 0;
  nHidFields = 0;

  size_t i = 0;
  while (i < n) {
    uint8_t pre = p[i++];
    if (pre == 0xFE) { if (i + 1 >= n) break; i += 2 + p[i]; continue; }   // long item
    uint8_t sz = pre & 3; if (sz == 3) sz = 4;
    uint8_t type = (pre >> 2) & 3, tag = pre >> 4;
    if (i + sz > n) break;
    uint32_t u = 0;
    for (uint8_t k = 0; k < sz; k++) u |= (uint32_t)p[i + k] << (8 * k);
    int32_t s = (int32_t)u;
    if (sz == 1) s = (int8_t)u; else if (sz == 2) s = (int16_t)u;
    i += sz;

    if (type == 0) {                                  // MAIN
      if (tag == 0x8) {                               // Input
        uint16_t *off = nullptr;
        for (uint8_t k = 0; k < nOffs; k++) if (offs[k].rid == g.rid) off = &offs[k].off;
        if (!off && nOffs < 8) { offs[nOffs] = {g.rid, 0}; off = &offs[nOffs++].off; }
        if (off && g.page == 0x07 && !(u & 0x01) && nHidFields < 8) {
          hid_field_t &f = hidFields[nHidFields++];
          f.reportId = g.rid; f.isArray = !(u & 0x02);
          f.bitOff = *off; f.size = g.size; f.count = g.count;
          f.usageMin = haveMin ? uMin : (nUs ? usages[0] : 0);
          f.logMin = g.logMin;
        }
        if (off) *off += g.size * g.count;
      }
      nUs = 0; haveMin = false;                       // locals are cleared after a main item
    } else if (type == 1) {                           // GLOBAL
      switch (tag) {
        case 0x0: g.page = u; break;
        case 0x1: g.logMin = s; break;
        case 0x7: g.size = u; break;
        case 0x8: g.rid = u; break;
        case 0x9: g.count = u; break;
        case 0xA: if (sp < 4) stk[sp++] = g; break;   // Push
        case 0xB: if (sp) g = stk[--sp]; break;       // Pop
      }
    } else if (type == 2) {                           // LOCAL
      if (tag == 0x0 && nUs < 16) usages[nUs++] = u & 0xFFFF;
      else if (tag == 0x1) { uMin = u & 0xFFFF; haveMin = true; }
    }
  }
  for (uint8_t k = 0; k < nHidFields; k++)
    DBG("[HID] field rid=%u %s off=%u size=%u count=%u umin=0x%02X\n",
        hidFields[k].reportId, hidFields[k].isArray ? "ARRAY" : "VAR",
        hidFields[k].bitOff, hidFields[k].size, hidFields[k].count, hidFields[k].usageMin);
  return nHidFields;
}

static bool ridIsKeyboard(uint8_t rid) {
  for (uint8_t k = 0; k < nHidFields; k++) if (hidFields[k].reportId == rid) return true;
  return false;
}

static void pushKey(uint8_t type, uint8_t u, uint8_t mods) {
  key_evt_t e = {type, u, mods};
  xQueueSend(qKeys, &e, 0);
}

// Runs in the NimBLE host task: decode and enqueue only.
static void processReport(uint8_t rid, const uint8_t *d, size_t n) {
  uint8_t mods = 0, keys[16], nk = 0;
  bool any = false;
  for (uint8_t k = 0; k < nHidFields; k++) {
    const hid_field_t &f = hidFields[k];
    if (f.reportId != rid) continue;
    any = true;
    for (uint8_t j = 0; j < f.count; j++) {
      uint32_t v = getBits(d, n, f.bitOff + (uint32_t)j * f.size, f.size);
      uint16_t usage;
      if (f.isArray) {
        if (v == 0) continue;
        usage = f.usageMin + (uint16_t)(v - f.logMin);
        if (usage == 0x01) return;                   // ErrorRollOver: invalid report
      } else {
        if (!v) continue;
        usage = f.usageMin + j;
      }
      if (usage >= 0xE0 && usage <= 0xE7) mods |= 1 << (usage - 0xE0);
      else if (usage >= 0x04 && nk < 16) keys[nk++] = (uint8_t)usage;
    }
  }
  if (!any) return;

  uint8_t slot = 0xFF;
  for (uint8_t s = 0; s < nPrev; s++) if (prevRid[s] == rid) slot = s;
  if (slot == 0xFF) { if (nPrev >= 4) return; slot = nPrev++; prevRid[slot] = rid; prevN[slot] = 0; }

  for (uint8_t a = 0; a < prevN[slot]; a++) {         // releases
    bool still = false;
    for (uint8_t b = 0; b < nk; b++) if (keys[b] == prevKeys[slot][a]) still = true;
    if (!still) pushKey(0, prevKeys[slot][a], mods);
  }
  for (uint8_t b = 0; b < nk; b++) {                  // presses
    bool was = false;
    for (uint8_t a = 0; a < prevN[slot]; a++) if (prevKeys[slot][a] == keys[b]) was = true;
    if (!was) pushKey(1, keys[b], mods);
  }
  memcpy(prevKeys[slot], keys, nk);
  prevN[slot] = nk;
}

static void onHidNotify(NimBLERemoteCharacteristic *c, uint8_t *d, size_t n, bool) {
  uint16_t h = c->getHandle();
  for (uint8_t k = 0; k < nRpt; k++)
    if (rptHandle[k] == h) { processReport(rptId[k], d, n); return; }
}

static void onBattNotify(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (n) { kbBattery = d[0]; requestRender(); }
}

/*===========================================================================
 *  SECTION 12 - BLE CENTRAL (HOGP host)
 *=========================================================================*/
class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    DBG("[BLE] disconnected, reason=%d\n", reason);
    bleState = BLE_IDLE; passkeyActive = false;
    requestRender();
  }
  void onConfirmPasskey(NimBLEConnInfo &ci, uint32_t) override {
    NimBLEDevice::injectConfirmPasskey(ci, true);    // should not happen with DisplayOnly
  }
  void onAuthenticationComplete(NimBLEConnInfo &ci) override {
    passkeyActive = false;
    DBG("[BLE] security: encrypted=%d authenticated=%d bonded=%d\n",
        ci.isEncrypted(), ci.isAuthenticated(), ci.isBonded());
    requestRender();
  }
};

static volatile bool   candFound = false;
static NimBLEAddress   candAddr;

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (candFound) return;
    bool bonded = NimBLEDevice::isBonded(dev->getAddress());
    bool hid    = dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1812));
    bool kbApp  = dev->haveAppearance() &&
                  (dev->getAppearance() == 0x03C1 || dev->getAppearance() == 0x03C0);
    bool nameOk = (strlen(KBD_NAME_FILTER) == 0) ||
                  (dev->getName().find(KBD_NAME_FILTER) != std::string::npos);
    if (bonded || ((hid || kbApp) && nameOk)) {
      candAddr = dev->getAddress();
      candFound = true;
      DBG("[BLE] candidate %s '%s' bonded=%d hid=%d app=0x%04X rssi=%d\n",
          candAddr.toString().c_str(), dev->getName().c_str(), bonded, hid,
          dev->haveAppearance() ? dev->getAppearance() : 0, dev->getRSSI());
    }
  }
};

static bool setupHid(NimBLEClient *cl) {
  NimBLERemoteService *hid = cl->getService(NimBLEUUID((uint16_t)0x1812));
  if (!hid) { DBG("[HID] no 0x1812 service\n"); return false; }

  NimBLERemoteCharacteristic *map = hid->getCharacteristic(NimBLEUUID((uint16_t)0x2A4B));
  if (map) {
    NimBLEAttValue mv = map->readValue();
    DBG("[HID] Report Map: %u bytes\n", (unsigned)mv.length());
    parseReportMap(mv.data(), mv.length());
  }

  nRpt = 0; nPrev = 0;
  if (nHidFields) {                                      // Report Protocol (default)
    for (auto *c : hid->getCharacteristics(true)) {
      if (!(c->getUUID() == NimBLEUUID((uint16_t)0x2A4D)) || !c->canNotify()) continue;
      NimBLERemoteDescriptor *rr = c->getDescriptor(NimBLEUUID((uint16_t)0x2908));
      if (!rr) continue;
      NimBLEAttValue v = rr->readValue();
      if (v.length() < 2) continue;
      uint8_t rid = v.data()[0], typ = v.data()[1];
      DBG("[HID] Report handle=%u id=%u type=%u\n", c->getHandle(), rid, typ);
      if (typ != 1 || !ridIsKeyboard(rid) || nRpt >= 8) continue;
      if (c->subscribe(true, onHidNotify)) { rptHandle[nRpt] = c->getHandle(); rptId[nRpt++] = rid; }
    }
  }
  if (!nRpt) {                                           // fallback: Boot Protocol
    NimBLERemoteCharacteristic *bk = hid->getCharacteristic(NimBLEUUID((uint16_t)0x2A22));
    NimBLERemoteCharacteristic *pm = hid->getCharacteristic(NimBLEUUID((uint16_t)0x2A4E));
    if (bk && pm) {
      uint8_t boot = 0x00;
      pm->writeValue(&boot, 1, false);
      hidFields[0] = {0xFE, 0, 0, 1, 8, 0xE0, 0};        // modifiers
      hidFields[1] = {0xFE, 1, 16, 8, 6, 0x00, 0};       // 6 keys
      nHidFields = 2;
      if (bk->subscribe(true, onHidNotify)) { rptHandle[0] = bk->getHandle(); rptId[0] = 0xFE; nRpt = 1; }
      DBG("[HID] using Boot Protocol\n");
    }
  }

  NimBLERemoteService *bat = cl->getService(NimBLEUUID((uint16_t)0x180F));
  if (bat) {
    NimBLERemoteCharacteristic *lvl = bat->getCharacteristic(NimBLEUUID((uint16_t)0x2A19));
    if (lvl) {
      NimBLEAttValue v = lvl->readValue();
      if (v.length()) kbBattery = v.data()[0];
      if (lvl->canNotify()) lvl->subscribe(true, onBattNotify);
    }
  }
  return nRpt > 0;
}

/* Pairing that works with the whole NimBLE-Arduino 2.x series.
 * NimBLE-Arduino < 2.4.0 ignores the BLE_SM_IOACT_DISP action (the client has
 * no onPassKeyDisplay) and Passkey Entry never progresses. Here the passkey is
 * injected straight into the NimBLE host Security Manager:
 * ble_sm_inject_io() returns EINVAL/ENOENT until the procedure waits for I/O,
 * accepts exactly one injection and then returns EALREADY.
 * On >= 2.4.0 the wrapper injects NimBLEDevice::getSecurityPasskey() (same
 * value), and the resulting EALREADY also means Passkey Entry took place. */
static bool secureLink(NimBLEClient *cl) {
  uint32_t pk = esp_random() % 1000000UL;
  NimBLEDevice::setSecurityPasskey(pk);
  passkeyVal = pk;
  if (!cl->secureConnection(true)) return false;        // asynchronous

  uint32_t t0 = millis();
  bool shown = false;
  while (cl->isConnected() && !cl->getConnInfo().isEncrypted() &&
         millis() - t0 < 60000UL) {
    struct ble_sm_io io;
    memset(&io, 0, sizeof(io));
    io.action  = BLE_SM_IOACT_DISP;
    io.passkey = pk;
    int rc = ble_sm_inject_io(cl->getConnHandle(), &io);
    if (!shown && (rc == 0 || rc == BLE_HS_EALREADY)) {
      shown = true;
      passkeyActive = true;
      DBG("[BLE] passkey to type on the keyboard: %06lu\n", (unsigned long)pk);
      requestRender();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  bool ok = cl->isConnected() && cl->getConnInfo().isEncrypted();
  if (passkeyActive) { passkeyActive = false; requestRender(); }
  return ok;
}

static void bleTask(void *) {
  static ClientCB ccb;
  static ScanCB   scb;
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scb, false);
  scan->setActiveScan(true);                 // the scan response carries the name
  scan->setInterval(100);
  scan->setWindow(100);

  NimBLEClient *cl = NimBLEDevice::createClient();
  cl->setClientCallbacks(&ccb, false);
  cl->setConnectionParams(6, 24, 0, 400);    // 7.5-30 ms, latency 0, timeout 4 s
  cl->setConnectTimeout(5000);

  for (;;) {
    numBonds = NimBLEDevice::getNumBonds();
    if (reqUnpair) {
      reqUnpair = false;
      if (cl->isConnected()) cl->disconnect();
      NimBLEDevice::deleteAllBonds();
      kbBattery = -1;
      DBG("[BLE] bonds deleted\n");
      requestRender();
    }
    if (cl->isConnected()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

    // 1) Scan in 5 s windows
    bleState = BLE_SCAN;
    candFound = false;
    scan->start(5000, false, true);
    uint32_t t0 = millis();
    while (!candFound && millis() - t0 < 5200 && !reqUnpair) vTaskDelay(pdMS_TO_TICKS(50));
    if (scan->isScanning()) scan->stop();

    NimBLEAddress target;
    if (candFound) target = candAddr;
    else if (numBonds > 0) {
      // 2) Direct attempt to the bonded keyboard (covers directed advertising)
      target = NimBLEDevice::getBondedAddress(0);
      DBG("[BLE] direct attempt to %s\n", target.toString().c_str());
    } else continue;

    bleState = BLE_CONNECTING; requestRender();
    if (!cl->connect(target, true)) { bleState = BLE_IDLE; requestRender(); continue; }
    DBG("[BLE] connected to %s, securing link...\n", target.toString().c_str());

    if (!secureLink(cl)) {
      DBG("[BLE] security failure\n");
      if (cl->isConnected()) cl->disconnect();
      bleState = BLE_IDLE; requestRender();
      continue;
    }
    if (!setupHid(cl)) {
      DBG("[BLE] no keyboard report found\n");
      cl->disconnect(); bleState = BLE_IDLE; requestRender();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    bleState = BLE_READY;
    setStatus("Keyboard connected");
  }
}

/*===========================================================================
 *  SECTION 13 - WIFI AP TO EXPORT / IMPORT FILES
 *=========================================================================*/
static bool safeName(const String &n) {
  if (n.length() == 0 || n.length() > 36 || n[0] == '.') return false;
  for (char c : n) if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
  return n.endsWith(".txt") || n.endsWith(".md");
}

static void webRoot(void) {
  if (docDirty) saveDoc();
  listFiles();
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Electgpl Writer</title><style>"
               "body{font-family:ui-monospace,Consolas,monospace;max-width:680px;margin:2em auto;"
               "padding:0 1em;background:#f4f1ea;color:#222}"
               "h2{background:#222;color:#f4f1ea;padding:.4em .6em;font-weight:normal}"
               "table{width:100%;border-collapse:collapse}td{padding:6px 10px;border-bottom:1px solid #ccc}"
               "a{color:#222}</style></head><body>"
               "<h2>ELECTGPL WRITER</h2><table>");
  for (uint8_t i = 0; i < nFiles; i++) {
    h += "<tr><td>" + String(fileNames[i]) + "</td><td>" + String(fileSizes[i]) +
         " B</td><td><a href='/dl?f=" + fileNames[i] +
         "'>download</a></td><td><a href='/del?f=" + fileNames[i] +
         "' onclick=\"return confirm('Delete?')\">delete</a></td></tr>";
  }
  h += F("</table><h3>Upload .txt / .md</h3><form method='POST' action='/up' "
         "enctype='multipart/form-data'><input type='file' name='f'> "
         "<input type='submit' value='Upload'></form></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void webDownload(void) {
  String n = server.arg("f");
  if (!safeName(n)) { server.send(400, "text/plain", "invalid name"); return; }
  File f = LittleFS.open("/docs/" + n, "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + n + "\"");
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

static void webDelete(void) {
  String n = server.arg("f");
  if (safeName(n) && n != docName) LittleFS.remove("/docs/" + n);
  server.sendHeader("Location", "/"); server.send(303);
}

static void webUpload(void) {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String n = up.filename;
    upFile = safeName(n) && n != docName ? LittleFS.open("/docs/" + n, "w") : File();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (upFile) upFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (upFile) upFile.close();
  }
}

/* WiFi and BLE run at the same time: the ESP32-S3 has a single 2.4 GHz radio
 * and ESP-IDF time-shares it (software coexistence, CONFIG_ESP_COEX_SW_COEXIST
 * _ENABLE=1 in the arduino-esp32 libs). Rule: never call WiFi.setSleep(false)
 * while BLE is up; with coexistence WiFi must stay in modem-sleep.
 * Only SoftAP mode is used: the device creates its own network and the PC
 * joins it, so no router credentials are needed. */
static bool startAP(void) {
  wifiErr[0] = 0;
  WiFi.persistent(false);                                 // do not write WiFi config to NVS
  if (!WiFi.mode(WIFI_AP)) {
    snprintf(wifiErr, sizeof(wifiErr), "WiFi.mode(AP) failed");
  } else if (!WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
    snprintf(wifiErr, sizeof(wifiErr), "WiFi.softAP() failed");
  } else {
    // The AP netif gets its IP asynchronously after AP_START: wait for it.
    IPAddress ip = WiFi.softAPIP();
    for (int i = 0; ip == IPAddress(0, 0, 0, 0) && i < 60; i++) {   // up to 3 s
      vTaskDelay(pdMS_TO_TICKS(50));
      ip = WiFi.softAPIP();
    }
    if (ip == IPAddress(0, 0, 0, 0)) snprintf(wifiErr, sizeof(wifiErr), "hotspot has no IP after 3 s");
    else {
      strlcpy(wifiIp, ip.toString().c_str(), sizeof(wifiIp));
      server.begin();
      wifiState = WF_AP;
      apClients = 0;
      DBG("[WIFI] AP '%s' ch %d at %s, free internal RAM %lu\n", AP_SSID, AP_CHANNEL, wifiIp,
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      return true;
    }
  }
  // Arduino's WiFi class hides the esp_wifi_init() error code: repeat the call
  // with the same buffer settings the core uses, only to get the reason.
  {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_tx_buf_num = 0;  cfg.dynamic_tx_buf_num = 32; cfg.tx_buf_type = 1;
    cfg.cache_tx_buf_num  = 4;  cfg.static_rx_buf_num  = 4;  cfg.dynamic_rx_buf_num = 32;
    uint32_t freeK = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    uint32_t bigK  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024;
    esp_err_t e = (wifiState == WF_STARTING && WiFi.getMode() == WIFI_OFF) ? esp_wifi_init(&cfg) : ESP_OK;
    if (e == ESP_OK && WiFi.getMode() == WIFI_OFF) esp_wifi_deinit();
    char more[64];
    snprintf(more, sizeof(more), " | %s, RAM %lu/%lu kB%s", esp_err_to_name(e),
             (unsigned long)freeK, (unsigned long)bigK, psramFound() ? "" : ", NO PSRAM");
    strlcat(wifiErr, more, sizeof(wifiErr));
  }
  DBG("[WIFI] %s\n", wifiErr);
  WiFi.mode(WIFI_OFF);
  wifiState = WF_FAIL;
  return false;
}

static void wifiOff(void) {
  if (wifiState == WF_AP) server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiState = WF_OFF;
  wifiIp[0] = 0;
}

static void toggleWiFi(void) {
  if (uiMode == UI_XFER) {                                 // close transfer mode
    wifiOff();
    uiMode = UI_EDIT;
    listFiles();
    setStatus("WiFi off");
    return;
  }
  if (docDirty) saveDoc();                                 // the PC must see the latest text
  uiMode = UI_XFER;
  wifiState = WF_STARTING;
  requestRender();
  startAP();
  requestRender();
}

static void wifiPoll(void) {
  if (wifiState != WF_AP) return;
  server.handleClient();
  static uint32_t tPoll = 0;
  if (millis() - tPoll > 1000) {
    tPoll = millis();
    uint8_t n = WiFi.softAPgetStationNum();
    if (n != apClients) {
      apClients = n;
      DBG("[WIFI] clients: %u\n", n);
      requestRender();
    }
  }
}

/*===========================================================================
 *  SECTION 14 - BOARD BUTTONS
 *=========================================================================*/
static void handleButtons(void) {
  static const uint8_t pins[5] = { BTN_UP, BTN_DOWN, BTN_OK, BTN_MENU, BTN_EXIT };
  static uint8_t  last[5] = {1, 1, 1, 1, 1};
  static uint32_t tDown[5], lastMs = 0;
  if (millis() - lastMs < 30) return;
  lastMs = millis();
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t v = digitalRead(pins[i]);
    if (v == LOW && last[i] == HIGH) tDown[i] = millis();
    if (v == HIGH && last[i] == LOW) {
      uint32_t held = millis() - tDown[i];
      switch (pins[i]) {
        case BTN_UP:   handlePress(0x4B, 0, false); break;
        case BTN_DOWN: handlePress(0x4E, 0, false); break;
        case BTN_OK:   setStatus(saveDoc() ? "Saved" : "ERROR saving"); break;
        case BTN_MENU: cleanRequest = true; requestRender(); break;
        case BTN_EXIT:
          if (held >= 3000) { reqUnpair = true; setStatus("Pairing deleted"); }
          break;
      }
    }
    last[i] = v;
  }
}

/*===========================================================================
 *  SECTION 15 - SETUP / LOOP
 *=========================================================================*/
void setup() {
  Serial.begin(115200);
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  pinMode(BTN_EXIT, INPUT_PULLUP);
  pinMode(BTN_MENU, INPUT_PULLUP);
  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);

  docMutex = xSemaphoreCreateMutex();
  qKeys    = xQueueCreate(64, sizeof(key_evt_t));

  gb = (char *)ps_malloc(DOC_MAX);
  gbCap = DOC_MAX;
  if (!gb) { gbCap = 32 * 1024; gb = (char *)malloc(gbCap); DBG("[MEM] no PSRAM: document limited to 32 kB\n"); }
  docClear();

  spiStream = (uint8_t *)heap_caps_malloc(ALLSCREEN_BYTES, MALLOC_CAP_SPIRAM);
  if (!spiStream) spiStream = (uint8_t *)malloc(ALLSCREEN_BYTES);
  EPD_GPIOInit();
  paintClear();
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();

  if (!LittleFS.begin(true)) DBG("[FS] ERROR mounting LittleFS\n");
  if (!LittleFS.exists("/docs")) LittleFS.mkdir("/docs");

  prefs.begin("writer", false);
  String last = prefs.getString("doc", "");
  if (last.length() && LittleFS.exists("/docs/" + last)) {
    loadDoc(last.c_str());
    uint32_t c = prefs.getUInt("cur", docLen());
    cursorPos = c > docLen() ? docLen() : c;
    fixViewport();
  } else {
    char nm[40]; newDocName(nm, sizeof(nm));
    strlcpy(docName, nm, sizeof(docName));
    saveDoc();
  }

  server.on("/", HTTP_GET, webRoot);
  server.on("/dl", HTTP_GET, webDownload);
  server.on("/del", HTTP_GET, webDelete);
  server.on("/up", HTTP_POST, []() { server.sendHeader("Location", "/"); server.send(303); }, webUpload);

  NimBLEDevice::init("Electgpl-Writer");
  NimBLEDevice::setSecurityAuth(true, true, true);          // bonding, MITM, LE SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // we display the passkey

  if (!psramFound()) setStatus("NO PSRAM: Tools > PSRAM > OPI PSRAM", 60000);
  lastInputMs = millis();
  xTaskCreatePinnedToCore(displayTask, "epd", 8192, NULL, 2, &dispTaskH, 1);
  xTaskCreatePinnedToCore(bleTask,     "ble", 8192, NULL, 3, NULL, 0);
  requestRender();
  DBG("[OK] internal free=%lu largest=%lu | heap=%lu psram=%lu bonds=%d\n",
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getFreePsram(), NimBLEDevice::getNumBonds());
}

void loop() {
  processKeys();
  handleButtons();
  wifiPoll();

  if (docDirty && (millis() - lastInputMs) > AUTOSAVE_MS) saveDoc();

  static bool statusShown = false;                        // redraw when the status message expires
  bool active = statusMsg[0] && millis() < statusUntil;
  if (statusShown && !active) { statusMsg[0] = 0; requestRender(); }
  statusShown = active;

  delay(2);
}
