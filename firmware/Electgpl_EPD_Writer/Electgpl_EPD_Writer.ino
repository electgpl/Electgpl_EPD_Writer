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
 *  Sketch files: this .ino + spleen_fonts.h + wallpaper_builtin.h (same folder)
 *
 *  DESKTOP (boot screen)
 *    PDA-style menu over a wallpaper: Continue writing (Read document without
 *    keyboard), New document, Documents, File transfer, Settings, Help and
 *    Private: unlock/lock. Arrows + Enter, keys 1-7, or the side buttons.
 *    Esc in the editor returns to the desktop.
 *
 *  STORAGE
 *    Internal flash (LittleFS, "spiffs" partition) or microSD card (FAT),
 *    selectable in Settings; folder /docs, UTF-8. Autosave 3 s after the last
 *    keystroke. The last document and cursor are restored per volume.
 *
 *  SETTINGS
 *    Password, auto-hide of private documents, keyboard layout, storage volume,
 *    copy all documents to the other volume, wallpaper, forget keyboard.
 *
 *  PRIVATE DOCUMENTS (Casio organizer style "secret area")
 *    Documents moved to /secret with P in the file list are only listed after
 *    the password is entered (desktop item 7); public ones never ask for it.
 *    Privacy only (salted SHA-256 in NVS), NOT encryption. Recovery: hold
 *    MENU + EXIT while powering up to clear the password.
 *
 *  KEYBOARD SHORTCUTS (Ctrl+H shows them on screen)
 *    Ctrl+S save              Ctrl+O file list           Ctrl+N new document
 *    Ctrl+W file transfer     Ctrl+R anti-ghost refresh  Ctrl+H help
 *    Ctrl+L hide private      Ctrl+P preview / view      Ctrl+Home/End  doc ends
 *    Esc    desktop (or cancel a pending dead key)
 *  FILE LIST
 *    Up/Down select  Enter open  N new  R rename  D/Del delete  P private  Esc back
 *  SIDE BUTTONS (keyboard-less "pocket PDA" viewer)
 *    UP/DOWN move in lists, page in the editor/viewer (auto-repeat when held)
 *    OK select (in the editor: save)   EXIT back   BOOT desktop   MENU refresh
 *    EXIT held 3 s = delete bonds (pair the keyboard again). Documents opened
 *    with a button, or with no keyboard linked, open in the read-only viewer.
 *
 *  FILE TRANSFER (Ctrl+W)
 *    The ESP32-S3 becomes a WiFi hotspot "Electgpl-Writer" (pass electgpl1234).
 *    Join it from the PC and browse http://192.168.4.1 to download, upload
 *    or delete notes, or upload a .bmp wallpaper. Esc or Ctrl+W closes it.
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
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include "spleen_fonts.h"
#include "wallpaper_builtin.h"
#include <mbedtls/sha256.h>
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
enum { UI_EDIT = 0, UI_FILES, UI_HELP, UI_XFER, UI_DESK, UI_SETTINGS, UI_INPUT, UI_PREVIEW };
enum { MD_TEXT = 0, MD_H1, MD_H2, MD_H3, MD_CODE, MD_PRE, MD_QUOTE, MD_HR, MD_GAP };
typedef struct {             // one display line of the Markdown preview
  uint32_t c0;               // first cell in mdCells
  uint32_t src;              // source position (paragraph start)
  uint16_t n;                // number of cells
  uint8_t  kind, indent, depth;
} mdline_t;
enum { WALL_BUILTIN = 0, WALL_FILE = 1, WALL_NONE = 2 };
enum { IN_PW_OLD = 0, IN_PW_NEW1, IN_PW_NEW2, IN_UNLOCK };
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

// loop() runs the web server, the filesystems and the editor: give it 16 kB
// of stack instead of the default 8 kB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

/*===========================================================================
 *  SECTION 1 - USER CONFIGURATION
 *=========================================================================*/
#define KBD_LAYOUT          LAYOUT_LATAM  // default; changeable in Settings
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

#define PW_MIN_LEN          4             // minimum password length
#define PW_HASH_ROUNDS      2000          // SHA-256 iterations for the stored hash
#define WALL_FILE_PATH      "/wallpaper.bmp"

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
#define BTN_BOOT  0       // BOOT button, usable as a normal input after reset

#define SD_SCK    39      // microSD on its own SPI bus (SPI3/HSPI), per Elecrow's 5.79_TF example
#define SD_MISO   13
#define SD_MOSI   40
#define SD_CS     10
#define SD_PWR    42      // TF card 3.3 V enable
#define SD_SPI_HZ 20000000UL

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
static bool     fileSecret[24];              // entry lives in /secret (private)
static bool     docSecret = false;           // the open document is private
static uint8_t  nFiles = 0, fileSel = 0;
static uint8_t  fileAction = FA_NONE;        // pending action in the file list
static char     renameBuf[40];
static uint8_t  renameLen = 0;
static uint64_t fsUsed = 0, fsTotal = 0;     // cached by listFiles()
static uint8_t  prevMode = UI_DESK;          // where Esc / Help / Files return to
static uint8_t  kbdLayout = KBD_LAYOUT;      // runtime keyboard layout
// Desktop, wallpaper, settings, lock
static uint8_t *wallBuf = nullptr;           // 792x272 1-bit (PSRAM), 1 = black
static uint8_t  wallMode = WALL_BUILTIN;
static bool     wallFileOk = false;
static volatile bool wallReload = false;
static uint8_t  deskSel = 0, setSel = 0;
static bool     setConfirm = false;          // "press Y" pending in Settings
static bool     privOpen = false;            // private documents visible this session
static bool     btnEvent = false;            // current key event comes from a board button
static bool     pairDismissed = false;       // pairing screen skipped with a board button
static uint8_t  viewReturn = UI_EDIT;        // where the viewer returns to
static uint8_t  autoLockMin = 0;             // 0 = off
static uint8_t  lockFails = 0;
static uint32_t lockUntil = 0;
static char     inBuf[40];                   // shared text input (password dialogs, lock)
static uint8_t  inLen = 0;
static uint8_t  inPurpose = IN_PW_NEW1;
static char     pwFirst[40];
// Storage volumes: internal flash (LittleFS) or microSD (FAT)
static fs::FS   *docFS = &LittleFS;
static bool      sdMounted = false;
static SPIClass  sdSPI(HSPI);

// BLE
static volatile uint8_t  bleState = BLE_IDLE;
static volatile bool     passkeyActive = false;
static volatile uint32_t passkeyVal = 0;
static volatile int      kbBattery = -1;
static volatile bool     reqUnpair = false;
static volatile bool     kbLost = false;          // link dropped: cancel key repeat
static volatile int      numBonds = 0;
static hid_field_t hidFields[8];
static uint8_t     nHidFields = 0;
static uint16_t    rptHandle[8];
static uint8_t     rptId[8];
static uint8_t     nRpt = 0;
static uint8_t     prevKeys[4][16], prevN[4], prevRid[4], nPrev = 0;

// Network
static WebServer server(80);
static volatile uint8_t wifiState = WF_OFF;     // read by the BLE task
static char      wifiIp[20] = "";
static char      wifiErr[128] = "";
static volatile uint8_t apClients = 0;
static File      upFile;
static uint32_t  upBytes = 0;
static bool      upOk = false, upIsWall = false;
static char      upName[40] = "";
static char      upMsg[80] = "";                 // last upload result, shown on the device
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
/* Documents live in /docs (public) or /secret (private, Casio-style "secret
 * area": visible only after the password is entered in this session). */
static const char *dirOf(bool sec) { return sec ? "/secret" : "/docs"; }
static void docPath(const char *name, bool sec, char *out, size_t n) { snprintf(out, n, "%s/%s", dirOf(sec), name); }
static bool pwIsSet(void);
static bool privVisible(void) { return !pwIsSet() || privOpen; }
static bool isMarkdown(const char *n) { size_t l = strlen(n); return l > 3 && !strcasecmp(n + l - 3, ".md"); }

static inline bool volIsSD(void) { return docFS == (fs::FS *)&SD; }
static const char *volName(void) { return volIsSD() ? "SD card" : "Internal flash"; }
static const char *prefDocKey(void) { return volIsSD() ? "docS" : "doc"; }
static const char *prefCurKey(void) { return volIsSD() ? "curS" : "cur"; }
static const char *prefSecKey(void) { return volIsSD() ? "secS" : "sec"; }

// Human-readable size: B, kB, MB or GB
static void fmtSize(uint64_t b, char *out, size_t n) {
  if (b < 1024ULL)                 snprintf(out, n, "%u B", (unsigned)b);
  else if (b < 1024ULL * 1024)     snprintf(out, n, "%u kB", (unsigned)(b / 1024));
  else if (b < 1024ULL * 1024 * 1024) snprintf(out, n, "%.1f MB", b / 1048576.0);
  else                             snprintf(out, n, "%.1f GB", b / 1073741824.0);
}

/* Replace 'dst' with 'tmp'. littlefs rename() atomically overwrites; FAT does
 * not, so on the SD card the old file is first moved to a hidden backup that
 * loadDoc() can recover if power fails in between. */
static bool fsReplace(const char *tmp, const char *dst, const char *name, const char *dir) {
  if (!volIsSD()) return LittleFS.rename(tmp, dst);
  char bak[64]; snprintf(bak, sizeof(bak), "%s/.bak_%s", dir, name);
  if (SD.exists(bak)) SD.remove(bak);
  if (SD.exists(dst) && !SD.rename(dst, bak)) return false;
  if (!SD.rename(tmp, dst)) { SD.rename(bak, dst); return false; }
  SD.remove(bak);
  return true;
}

static bool mountSD(void) {
  pinMode(SD_PWR, OUTPUT);
  digitalWrite(SD_PWR, HIGH);
  delay(20);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdMounted = SD.begin(SD_CS, sdSPI, SD_SPI_HZ, "/sd", 5) && SD.cardType() != CARD_NONE;
  if (sdMounted && !SD.exists("/docs"))   SD.mkdir("/docs");
  if (sdMounted && !SD.exists("/secret")) SD.mkdir("/secret");
  DBG("[SD] %s\n", sdMounted ? "mounted" : "not present");
  return sdMounted;
}

static bool saveDoc(void) {
  uint32_t t0 = millis();
  const char *dir = dirOf(docSecret);
  char path[64], tmp[64];
  docPath(docName, docSecret, path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s/.tmp", dir);
  File f = docFS->open(tmp, "w");
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
  bool ok = fsReplace(tmp, path, docName, dir);
  if (ok) {
    docDirty = false;
    prefs.putString(prefDocKey(), docName);
    prefs.putBool(prefSecKey(), docSecret);
    prefs.putUInt(prefCurKey(), cursorPos);
  }
  DBG("[FS] saved %s (%lu B) %s in %lu ms\n", path, (unsigned long)len, ok ? "OK" : "ERROR",
      (unsigned long)(millis() - t0));
  return ok;
}

static void loadDoc(const char *name, bool sec) {
  char path[64]; docPath(name, sec, path, sizeof(path));
  xSemaphoreTake(docMutex, portMAX_DELAY);
  docClear();
  strlcpy(docName, name, sizeof(docName));
  docSecret = sec;
  char bak[72]; snprintf(bak, sizeof(bak), "%s/.bak_%s", dirOf(sec), name);
  if (!docFS->exists(path) && docFS->exists(bak)) docFS->rename(bak, path);   // recover
  File f = docFS->open(path, "r");
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
  prefs.putString(prefDocKey(), docName);
  prefs.putBool(prefSecKey(), docSecret);
  DBG("[FS] opened %s:%s (%lu B)\n", volName(), path, (unsigned long)docLen());
}

static bool nameTaken(const char *name) {              // names are unique across both folders
  char a[64], b[64];
  docPath(name, false, a, sizeof(a)); docPath(name, true, b, sizeof(b));
  return docFS->exists(a) || docFS->exists(b);
}

static void newDocName(char *out, size_t n) {
  for (int i = 1; i < 100; i++) {
    snprintf(out, n, "note%02d.txt", i);
    if (!nameTaken(out)) return;
  }
  snprintf(out, n, "note%08lx.txt", (unsigned long)esp_random());
}

static void listDir(bool sec) {
  File d = docFS->open(dirOf(sec));
  if (!d) return;
  File e;
  while ((e = d.openNextFile()) && nFiles < 24) {
    const char *nm = e.name();
    const char *b = strrchr(nm, '/'); b = b ? b + 1 : nm;
    if (b[0] != '.' && !e.isDirectory()) {
      fileSizes[nFiles] = e.size(); fileSecret[nFiles] = sec;
      strlcpy(fileNames[nFiles++], b, 40);
    }
    e.close();
  }
  d.close();
}

static void listFiles(void) {
  nFiles = 0;
  listDir(false);
  if (privVisible()) listDir(true);
  fsUsed  = volIsSD() ? SD.usedBytes()  : LittleFS.usedBytes();
  fsTotal = volIsSD() ? SD.totalBytes() : LittleFS.totalBytes();
  if (fileSel >= nFiles) fileSel = nFiles ? nFiles - 1 : 0;
}

// Opens the first public document, or creates one (used when the open one disappears)
static void newDoc(void);
static void openFirstPublic(void) {
  bool saved = privOpen; privOpen = false;   // list public entries only
  uint8_t sel = fileSel;
  listFiles();
  privOpen = saved;
  bool found = false;
  for (uint8_t i = 0; i < nFiles; i++) if (!fileSecret[i]) { loadDoc(fileNames[i], false); found = true; break; }
  if (!found) { uint8_t m = uiMode; newDoc(); uiMode = m; }
  fileSel = sel;
  listFiles();
}

/*===========================================================================
 *  SECTION 7b - WALLPAPER (BMP decoder with Floyd-Steinberg dithering)
 *=========================================================================*/
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* Decodes an uncompressed BMP (1/4/8/24/32 bpp, bottom-up or top-down) into
 * the 792x272 1-bit wallpaper buffer. Larger images are centre-cropped and
 * smaller ones centred; grey levels are converted with Floyd-Steinberg error
 * diffusion. Returns false if the file is missing or unsupported. */
static bool loadBmp(fs::FS &fs, const char *path, uint8_t *dst) {
  File f = fs.open(path, "r");
  if (!f) return false;
  uint8_t h[54];
  if (f.read(h, 54) != 54 || h[0] != 'B' || h[1] != 'M') { f.close(); return false; }
  uint32_t off = rd32(h + 10), dib = rd32(h + 14);
  int32_t  w = (int32_t)rd32(h + 18), ht = (int32_t)rd32(h + 22);
  uint16_t bpp = rd16(h + 28);
  uint32_t comp = rd32(h + 30), ncol = rd32(h + 46);
  bool topDown = ht < 0; if (topDown) ht = -ht;
  if (w <= 0 || ht <= 0 || w > 4096 || ht > 4096 ||
      !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32) ||
      !(comp == 0 || (comp == 3 && bpp == 32))) { f.close(); return false; }
  uint8_t pal[256];                                    // palette luminance
  if (bpp <= 8) {
    if (!ncol) ncol = 1u << bpp;
    f.seek(14 + dib);
    for (uint32_t i = 0; i < ncol && i < 256; i++) {
      uint8_t q[4]; f.read(q, 4);
      pal[i] = (q[2] * 77 + q[1] * 150 + q[0] * 29) >> 8;
    }
  }
  uint32_t rowSize = ((uint32_t)bpp * w + 31) / 32 * 4;
  uint8_t *row = (uint8_t *)ps_malloc(rowSize);
  int16_t *e0 = (int16_t *)ps_calloc(SCR_W + 2, sizeof(int16_t));
  int16_t *e1 = (int16_t *)ps_calloc(SCR_W + 2, sizeof(int16_t));
  if (!row || !e0 || !e1) { free(row); free(e0); free(e1); f.close(); return false; }
  memset(dst, 0, SCR_W / 8 * SCR_H);
  int sx0 = w > SCR_W ? (w - SCR_W) / 2 : 0,  dx0 = w < SCR_W ? (SCR_W - w) / 2 : 0;
  int sy0 = ht > SCR_H ? (ht - SCR_H) / 2 : 0, dy0 = ht < SCR_H ? (SCR_H - ht) / 2 : 0;
  int nx = w < SCR_W ? w : SCR_W, ny = ht < SCR_H ? ht : SCR_H;
  for (int y = 0; y < ny; y++) {
    int sr = sy0 + y;
    f.seek(off + (uint32_t)(topDown ? sr : (ht - 1 - sr)) * rowSize);
    f.read(row, rowSize);
    memset(e1, 0, (SCR_W + 2) * sizeof(int16_t));
    for (int x = 0; x < nx; x++) {
      int sx = sx0 + x, g;
      switch (bpp) {
        case 1:  g = pal[(row[sx >> 3] >> (7 - (sx & 7))) & 1]; break;
        case 4:  g = pal[(row[sx >> 1] >> ((sx & 1) ? 0 : 4)) & 0x0F]; break;
        case 8:  g = pal[row[sx]]; break;
        case 24: { const uint8_t *p = row + sx * 3; g = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8; } break;
        default: { const uint8_t *p = row + sx * 4; g = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8; } break;
      }
      int v = g + e0[x + 1];
      int o = v < 128 ? 0 : 255;
      int e = v - o;
      if (!o) { int X = dx0 + x, Y = dy0 + y; dst[Y * (SCR_W / 8) + (X >> 3)] |= 0x80 >> (X & 7); }
      e0[x + 2] += e * 7 / 16; e1[x] += e * 3 / 16; e1[x + 1] += e * 5 / 16; e1[x + 2] += e / 16;
    }
    int16_t *t = e0; e0 = e1; e1 = t;
  }
  free(row); free(e0); free(e1); f.close();
  DBG("[WALL] %s %ldx%ld %u bpp loaded\n", path, (long)w, (long)ht, bpp);
  return true;
}

// Looks for the wallpaper file on the active volume first, then on the other one.
static void loadWallpaper(void) {
  if (!wallBuf) return;
  fs::FS &other = volIsSD() ? (fs::FS &)LittleFS : (fs::FS &)SD;
  wallFileOk = loadBmp(*docFS, WALL_FILE_PATH, wallBuf) ||
               ((volIsSD() || sdMounted) && loadBmp(other, WALL_FILE_PATH, wallBuf));
  if (!wallFileOk) memcpy(wallBuf, wallpaper_builtin, sizeof(wallpaper_builtin));
}

static void drawWallpaper(void) {
  if (wallMode == WALL_NONE) return;
  const uint8_t *src = (wallMode == WALL_FILE && wallFileOk && wallBuf) ? wallBuf : wallpaper_builtin;
  for (int y = 0; y < SCR_H; y++)
    for (int xb = 0; xb < SCR_W / 8; xb++) {
      uint8_t b = src[y * (SCR_W / 8) + xb];
      if (b) for (int k = 0; k < 8; k++) if (b & (0x80 >> k)) px(xb * 8 + k, y, true);
    }
}

/*===========================================================================
 *  SECTION 7c - PASSWORD (privacy lock, salted iterated SHA-256 in NVS)
 *=========================================================================*/
static void pwHash(const char *pw, const uint8_t salt[16], uint8_t out[32]) {
  uint8_t buf[16 + 40];
  size_t n = strlen(pw);
  memcpy(buf, salt, 16); memcpy(buf + 16, pw, n);
  mbedtls_sha256(buf, 16 + n, out, 0);
  uint8_t b2[32 + 16];
  for (int i = 1; i < PW_HASH_ROUNDS; i++) {
    memcpy(b2, out, 32); memcpy(b2 + 32, salt, 16);
    mbedtls_sha256(b2, sizeof(b2), out, 0);
  }
}

static bool pwIsSet(void) { return prefs.isKey("pwh"); }

static void pwStore(const char *pw) {
  uint8_t salt[16], h[32];
  esp_fill_random(salt, sizeof(salt));
  pwHash(pw, salt, h);
  prefs.putBytes("pws", salt, 16);
  prefs.putBytes("pwh", h, 32);
}

static bool pwCheck(const char *pw) {
  uint8_t salt[16], h[32], ref[32];
  if (prefs.getBytes("pws", salt, 16) != 16 || prefs.getBytes("pwh", ref, 32) != 32) return true;
  pwHash(pw, salt, h);
  uint8_t d = 0;
  for (int i = 0; i < 32; i++) d |= h[i] ^ ref[i];       // constant-time compare
  return d == 0;
}

static void pwClear(void) { prefs.remove("pwh"); prefs.remove("pws"); }

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
  char left[80], right[96];
  fillRect(0, 0, SCR_W, HDR_H, true);

  const char *bt = (bleState == BLE_READY) ? "BT OK" : "BT --";
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

  const char *pp = docSecret ? "[P] " : "";
  snprintf(left, sizeof(left), " ELECTGPL WRITER  %s%s%s%s", pp, docName, docDirty ? " *" : "",
           (pwIsSet() && privOpen) ? "   PRIVATE OPEN" : "   Ctrl+H help");
  if (strLen8(left) + strLen8(right) > SCR_W / 8 - 1)          // drop the hint if it does not fit
    snprintf(left, sizeof(left), " ELECTGPL WRITER  %s%s%s", pp, docName, docDirty ? " *" : "");
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
    snprintf(hint, sizeof(hint), "Enter: open  N: new  R: rename  D: delete  P: private  Esc: back");
  text16(TXT_X + 84, TXT_Y + 5, hint, true);
  hLine(TXT_X, SCR_W - TXT_X, TXT_Y + 27, true);

  const int perPage = 8, y0 = TXT_Y + 32;
  int first = (fileSel / perPage) * perPage;
  for (int i = 0; i < perPage && first + i < nFiles; i++) {
    int idx = first + i, y = y0 + i * 26;
    char line[72];
    bool open = !strcmp(fileNames[idx], docName) && fileSecret[idx] == docSecret;
    if (idx == fileSel && fileAction == FA_RENAME)
      snprintf(line, sizeof(line), "%s_", renameBuf);
    else
      snprintf(line, sizeof(line), "%s%-32s %8lu B %s", fileSecret[idx] ? "[P] " : "    ", fileNames[idx],
               (unsigned long)fileSizes[idx], open ? "(open)" : "");
    if (idx == fileSel) { fillRect(TXT_X - 4, y - 1, SCR_W - 2 * TXT_X + 8, 25, true); text24(TXT_X, y, line, false); }
    else text24(TXT_X, y, line, true);
  }
  if (!nFiles) text24C(y0 + 60, "(no files)");
  if (pwIsSet() && !privOpen) text16(TXT_X, SCR_H - 37, "Private documents hidden (desktop item 7 to unlock)", true);

  char foot[112], u[16], t[16];
  fmtSize(fsUsed, u, sizeof(u)); fmtSize(fsTotal, t, sizeof(t));
  snprintf(foot, sizeof(foot), "Open doc: %lu of %lu kB max   |   %s: %s used of %s",
           (unsigned long)((docLen() + 1023) / 1024), (unsigned long)(gbCap / 1024), volName(), u, t);
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
    "Ctrl+L      Hide private documents",
    "Ctrl+P      Markdown preview (read-only)",
    "Ctrl+H      This help",
    "Ctrl+Home/End  Start / end of document",
    "Esc         Desktop (or cancel dead key)",
    "",
    "Press any key to return",
  };
  static const char *R[] = {
    "DESKTOP: arrows + Enter, or keys 1-7",
    "FILE LIST (Ctrl+O)",
    "Up/Down Select  Enter Open  N New",
    "R Rename  D Delete  P Private (password)",
    "VIEW: arrows PgUp/PgDn Space Home/End",
    "",
    "TRANSFER: join WiFi " AP_SSID,
    "  password " AP_PASS,
    "  then browse http://192.168.4.1",
    "",
    "BUTTONS: UP/DN move/page  OK select",
    "  EXIT back  BOOT desktop  MENU refresh",
    "  EXIT held 3 s: forget keyboard",
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
    text16(TXT_X, y0 + 146, "Download, upload (.txt/.md, .bmp wallpaper) or delete from the web page.", true);
    char lu[96]; snprintf(lu, sizeof(lu), "Last upload: %s", upMsg[0] ? upMsg : "-");
    text16(TXT_X, y0 + 164, lu, true);
    text16(TXT_X, y0 + 182, bleState == BLE_READY
             ? "Keyboard link relaxed while WiFi is on. Esc / Ctrl+W: close."
             : "Keyboard reconnection paused while WiFi is on. EXIT button: close.", true);
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

// White panel with a 2 px border and a 4 px drop shadow, drawn over the wallpaper
static void panel(int x, int y, int w, int h) {
  fillRect(x + 4, y + 4, w, h, true);
  fillRect(x, y, w, h, false);
  fillRect(x, y, w, 2, true); fillRect(x, y + h - 2, w, 2, true);
  fillRect(x, y, 2, h, true); fillRect(x + w - 2, y, 2, h, true);
}

static const char *DESK_ITEMS[] = { "Continue writing", "New document", "Documents",
                                    "File transfer", "Settings", "Help", "Private: unlock" };
static uint8_t deskCount(void) { return pwIsSet() ? 7 : 6; }

static void renderDesk(void) {
  if (deskSel >= deskCount()) deskSel = 0;
  drawWallpaper();
  drawHeader(wordCount());
  const int px0 = 20, py0 = 30, pw = 300, ph = 232;
  panel(px0, py0, pw, ph);
  fillRect(px0, py0, pw, 24, true);
  text16(px0 + 10, py0 + 4, "MENU", false);
  char line[48];
  for (uint8_t i = 0; i < deskCount(); i++) {
    int y = py0 + 30 + i * 25;
    const char *it = (i == 0 && bleState != BLE_READY) ? "Read document" :
                     (i == 6 && privOpen) ? "Private: lock" : DESK_ITEMS[i];
    snprintf(line, sizeof(line), "%u  %s", i + 1, it);
    if (i == deskSel) { fillRect(px0 + 6, y - 1, pw - 12, 24, true); text24(px0 + 12, y, line, false); }
    else text24(px0 + 12, y, line, true);
  }
  char sz[16]; fmtSize(docLen(), sz, sizeof(sz));
  snprintf(line, sizeof(line), "Last: %s%.20s (%s)", docSecret ? "[P] " : "", docName, sz);
  text16(px0 + 10, py0 + ph - 20, line, true);
}

static const char *LAYOUT_NAMES[] = { "Latin America", "Spain", "US" };
static const char *WALL_NAMES[]   = { "Built-in", "File " WALL_FILE_PATH, "None" };

static void renderSettings(void) {
  drawHeader(wordCount());
  text24(TXT_X, TXT_Y, "SETTINGS", true);
  text16(TXT_X + 120, TXT_Y + 5,
         setConfirm ? "Press Y or OK to confirm, any other key to cancel"
                    : "Up/Down: select   Enter: change   Esc: back", true);
  hLine(TXT_X, SCR_W - TXT_X, TXT_Y + 27, true);
  char val[48], line[80];
  for (uint8_t i = 0; i < 7; i++) {
    const char *lab = "";
    switch (i) {
      case 0: lab = "Password";        snprintf(val, sizeof(val), "%s", pwIsSet() ? "set (Enter: change/remove)" : "not set (Enter: set)"); break;
      case 1: lab = "Auto-hide priv.";       if (autoLockMin) snprintf(val, sizeof(val), "%u min", autoLockMin); else snprintf(val, sizeof(val), "off"); break;
      case 2: lab = "Keyboard layout"; snprintf(val, sizeof(val), "%s", LAYOUT_NAMES[kbdLayout]); break;
      case 3: lab = "Storage";         snprintf(val, sizeof(val), "%s%s", volName(), sdMounted ? "" : " (no SD card)"); break;
      case 4: lab = "Copy documents";  snprintf(val, sizeof(val), sdMounted ? "all to %s" : "needs an SD card", volIsSD() ? "internal flash" : "SD card"); break;
      case 5: lab = "Wallpaper";       snprintf(val, sizeof(val), "%s%s", WALL_NAMES[wallMode], (wallMode == WALL_FILE && !wallFileOk) ? " (missing)" : ""); break;
      case 6: lab = "Forget keyboard"; snprintf(val, sizeof(val), "pair again"); break;
    }
    snprintf(line, sizeof(line), "%-16s %s", lab, val);
    int y = TXT_Y + 32 + i * 30;
    if (i == setSel) { fillRect(TXT_X - 4, y - 1, SCR_W - 2 * TXT_X + 8, 25, true); text24(TXT_X, y, line, false); }
    else text24(TXT_X, y, line, true);
  }
}

static void drawMaskedField(int x, int y, int w) {
  fillRect(x, y, w, 30, false);
  hLine(x, x + w, y + 30, true);
  char shown[42];
  for (uint8_t i = 0; i < inLen; i++) shown[i] = '*';
  shown[inLen] = '_'; shown[inLen + 1] = 0;
  text24(x + 6, y + 3, shown, true);
}

static void renderInput(void) {
  drawWallpaper();
  drawHeader(wordCount());
  panel(146, 64, 500, 150);
  const char *t = inPurpose == IN_UNLOCK ? "Password for private documents" :
                  inPurpose == IN_PW_OLD ? "Current password" :
                  inPurpose == IN_PW_NEW1 ? "New password (empty = remove)" : "Repeat new password";
  text24(166, 80, t, true);
  drawMaskedField(166, 120, 460);
  text16(166, 180, "Enter: accept    Esc: cancel", true);
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
    text16((SCR_W - 52 * 8) / 2, 245, "No keyboard? Press any side button to read documents", true);
    text24C(210, bleState == BLE_CONNECTING ? "Connecting..." : "");
  }
}

/*===========================================================================
 *  SECTION 8b - MARKDOWN PREVIEW (read-only, Ctrl+P)
 *  Block level: # ## ### headings, paragraphs (soft breaks joined, two
 *  trailing spaces = hard break), - * + and 1. lists with nesting, > quotes,
 *  ``` fenced code, | tables (verbatim), --- rules.
 *  Inline: **bold**, *italic*, ***both***, ~~strike~~, `code`, [link](url),
 *  ![image](src), \ escapes. Each character cell carries its attributes.
 *=========================================================================*/
#define A_BOLD 0x01
#define A_ITAL 0x02
#define A_CODE 0x04
#define A_UNDR 0x08
#define A_STRK 0x10
#define A_BULL 0x20
#define MD_W   (COLS - 1)           // columns available (last one for the scroll bar)

static uint16_t *mdCells = nullptr; static uint32_t mdCellsN = 0, mdCellsCap = 0;
static mdline_t *mdLines = nullptr; static uint32_t mdLinesN = 0, mdLinesCap = 0;
static uint16_t *mdPara  = nullptr; static uint32_t mdParaN  = 0, mdParaCap  = 0;
static char     *mdRaw   = nullptr; static uint32_t mdRawN   = 0, mdRawCap   = 0;
static uint32_t  mdTop = 0;
static bool      mdOom = false;
// open paragraph context
static bool     pOpen = false, pHard = false;
static uint8_t  pKind = MD_TEXT, pIndent = 0, pDepth = 0;
static uint16_t pMarker[8];
static uint8_t  pMarkerN = 0;
static uint32_t pSrc = 0;

static bool mdGrow(void **p, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap) return true;
  uint32_t nc = *cap ? *cap : 1024;
  while (nc < need) nc *= 2;
  void *q = ps_realloc(*p, (size_t)nc * elem);
  if (!q) { mdOom = true; return false; }
  *p = q; *cap = nc;
  return true;
}

static void rawPush(char c) {
  if (mdGrow((void **)&mdRaw, &mdRawCap, mdRawN + 1, 1)) mdRaw[mdRawN++] = c;
}
static void paraPush(uint8_t c, uint8_t at) {
  if (mdGrow((void **)&mdPara, &mdParaCap, mdParaN + 1, 2)) mdPara[mdParaN++] = c | (at << 8);
}

static void mdEmit(uint8_t kind, uint8_t indent, uint8_t depth, uint32_t src,
                   const uint16_t *c, uint32_t n) {
  if (!mdGrow((void **)&mdLines, &mdLinesCap, mdLinesN + 1, sizeof(mdline_t))) return;
  if (!mdGrow((void **)&mdCells, &mdCellsCap, mdCellsN + n + 1, 2)) return;
  mdline_t &L = mdLines[mdLinesN++];
  L.c0 = mdCellsN; L.n = n; L.kind = kind; L.indent = indent; L.depth = depth; L.src = src;
  memcpy(mdCells + mdCellsN, c, n * 2);
  mdCellsN += n;
}

static void mdGap(uint32_t src) {
  if (mdLinesN && mdLines[mdLinesN - 1].kind != MD_GAP) mdEmit(MD_GAP, 0, 0, src, nullptr, 0);
}

// Word-wraps a cell run: the first line starts with 'pre' (bullet or number),
// continuation lines get a hanging indent of the same width.
static void mdWrap(uint8_t kind, uint8_t indent, uint8_t depth, uint32_t src,
                   const uint16_t *pre, uint8_t preN, const uint16_t *c, uint32_t n, int width) {
  uint16_t line[MD_W + 8];
  uint32_t i = 0;
  bool first = true;
  do {
    int ln = 0;
    if (first) for (uint8_t k = 0; k < preN; k++) line[ln++] = pre[k];
    int avail = width - preN;
    if (avail < 8) avail = 8;
    while (i < n && (c[i] & 0xFF) == ' ') i++;                   // no leading spaces
    uint32_t start = i, lastSp = UINT32_MAX;
    while (i < n && (int)(i - start) < avail) {
      uint8_t ch = c[i] & 0xFF;
      if (ch == '\n') break;
      if (ch == ' ') lastSp = i;
      i++;
    }
    uint32_t end = i;
    if (i < n && (c[i] & 0xFF) != '\n' && (c[i] & 0xFF) != ' ' && lastSp != UINT32_MAX && lastSp > start)
      end = i = lastSp;                                           // break at the last space
    for (uint32_t k = start; k < end; k++) line[ln++] = c[k];
    if (i < n && (c[i] & 0xFF) == '\n') i++;                      // hard break consumed
    mdEmit(kind, first ? indent : indent + preN, depth, src, line, ln);
    first = false;
  } while (i < n);
}

static bool isPunct(char c) { return c && strchr("\\`*_{}[]()#+-.!|~>", c); }

// Inline markup -> attributed cells (mdPara)
static void mdInline(const char *s, uint32_t n, uint8_t base) {
  mdParaN = 0;
  uint8_t at = base;
  uint32_t i = 0;
  while (i < n) {
    char c = s[i];
    if (c == '\\' && i + 1 < n && isPunct(s[i + 1])) { paraPush(s[i + 1], at); i += 2; continue; }
    if (at & A_CODE) {
      if (c == '`') { at &= ~A_CODE; i++; } else { paraPush(c, at); i++; }
      continue;
    }
    if (c == '`' && memchr(s + i + 1, '`', n - i - 1)) { at |= A_CODE; i++; continue; }
    if (c == '*' || c == '_') {
      bool dbl = (i + 1 < n && s[i + 1] == c);
      uint8_t flag = dbl ? A_BOLD : A_ITAL;
      uint32_t ml = dbl ? 2 : 1;
      bool prevAl = i > 0 && isalnum((unsigned char)s[i - 1]);
      bool nextAl = i + ml < n && isalnum((unsigned char)s[i + ml]);
      if (c == '_' && prevAl && nextAl) { paraPush(c, at); i++; continue; }   // snake_case
      if (at & flag) { at &= ~flag; i += ml; continue; }                     // closing
      bool nextSp = (i + ml >= n) || s[i + ml] == ' ';
      bool closes = false;
      for (uint32_t j = i + ml + 1; j + ml <= n; j++)
        if (s[j] == c && (dbl ? (j + 1 < n && s[j + 1] == c) : !(j + 1 < n && s[j + 1] == c && !(at & A_BOLD)))) { closes = true; break; }
      if (!nextSp && closes) { at |= flag; i += ml; continue; }
      paraPush(c, at); i++; continue;
    }
    if (c == '~' && i + 1 < n && s[i + 1] == '~') {
      if (at & A_STRK) { at &= ~A_STRK; i += 2; continue; }
      bool closes = false;
      for (uint32_t j = i + 2; j + 1 < n; j++) if (s[j] == '~' && s[j + 1] == '~') { closes = true; break; }
      if (closes) { at |= A_STRK; i += 2; continue; }
    }
    if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
      bool img = (c == '!');
      uint32_t o = i + (img ? 2 : 1), cb = o;
      while (cb < n && s[cb] != ']') cb++;
      if (cb + 1 < n && s[cb + 1] == '(') {
        uint32_t cp = cb + 2;
        while (cp < n && s[cp] != ')') cp++;
        if (cp < n) {
          if (img) { const char *t = "[image: "; while (*t) paraPush(*t++, at); }
          for (uint32_t k = o; k < cb; k++) paraPush(s[k], img ? at : (at | A_UNDR));
          if (img) paraPush(']', at);
          i = cp + 1;
          continue;
        }
      }
    }
    paraPush(c, at); i++;
  }
}

static void mdFlush(void) {
  if (!pOpen) return;
  mdInline(mdRaw, mdRawN, 0);
  int width = MD_W - pIndent - (pKind == MD_QUOTE ? 2 * pDepth : 0);
  mdWrap(pKind, pIndent, pDepth, pSrc, pMarker, pMarkerN, mdPara, mdParaN, width);
  pOpen = false; pHard = false; mdRawN = 0; pMarkerN = 0;
}

static void mdOpen(uint8_t kind, uint8_t indent, uint8_t depth, uint32_t src) {
  mdFlush();
  pOpen = true; pKind = kind; pIndent = indent; pDepth = depth; pSrc = src;
  mdRawN = 0; pMarkerN = 0; pHard = false;
}

static void mdAppend(const char *t, int n) {
  while (n > 0 && t[n - 1] == ' ') { if (n >= 2 && t[n - 2] == ' ') pHard = true; n--; }
  if (mdRawN) rawPush(' ');
  for (int k = 0; k < n; k++) rawPush(t[k]);
  if (pHard) { rawPush('\n'); pHard = false; }
}

static void mdVerbatim(uint8_t kind, uint32_t src, const char *t, int n) {
  int width = MD_W - (kind == MD_CODE ? 2 : 0);
  uint16_t line[MD_W + 8];
  int i = 0;
  do {
    int ln = 0;
    while (i < n && ln < width) line[ln++] = (uint8_t)t[i++];
    mdEmit(kind, 0, 0, src, line, ln);
  } while (i < n);
}

static void mdLine(const char *line, int ll, uint32_t src, bool *fence) {
  int sp = 0;
  while (sp < ll && line[sp] == ' ') sp++;
  const char *t = line + sp;
  int tl = ll - sp;
  if (*fence) {
    if (tl >= 3 && !strncmp(t, "```", 3)) { *fence = false; mdGap(src); }
    else mdVerbatim(MD_CODE, src, line, ll);
    return;
  }
  if (tl >= 3 && !strncmp(t, "```", 3)) { mdFlush(); *fence = true; return; }
  if (tl == 0) { mdFlush(); mdGap(src); return; }
  if (t[0] == '#') {                                              // heading
    int h = 0; while (h < tl && t[h] == '#') h++;
    if (h <= 6 && (h == tl || t[h] == ' ')) {
      mdFlush();
      const char *x = t + h; int xl = tl - h;
      while (xl && *x == ' ') { x++; xl--; }
      while (xl && (x[xl - 1] == '#' || x[xl - 1] == ' ')) xl--;
      uint8_t kind = h == 1 ? MD_H1 : h == 2 ? MD_H2 : MD_H3;
      mdInline(x, xl, A_BOLD);
      mdWrap(kind, 0, 0, src, nullptr, 0, mdPara, mdParaN, kind == MD_H1 ? MD_W / 2 : MD_W);
      return;
    }
  }
  if (tl >= 3 && (t[0] == '-' || t[0] == '*' || t[0] == '_')) {  // horizontal rule
    int cnt = 0; bool ok = true;
    for (int k = 0; k < tl; k++) { if (t[k] == t[0]) cnt++; else if (t[k] != ' ') { ok = false; break; } }
    if (ok && cnt >= 3) { mdFlush(); mdEmit(MD_HR, 0, 0, src, nullptr, 0); return; }
  }
  if (t[0] == '|') { mdFlush(); mdVerbatim(MD_PRE, src, t, tl); return; }     // table row
  if (t[0] == '>') {                                              // block quote
    int d = 0, k = 0;
    while (k < tl && (t[k] == '>' || t[k] == ' ')) { if (t[k] == '>') d++; k++; }
    if (d > 4) d = 4;
    if (!pOpen || pKind != MD_QUOTE || pDepth != d) mdOpen(MD_QUOTE, 0, d, src);
    mdAppend(t + k, tl - k);
    return;
  }
  uint8_t lvl = sp / 2; if (lvl > 6) lvl = 6;
  if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && tl > 1 && t[1] == ' ') {   // bullet
    mdOpen(MD_TEXT, lvl * 2, 0, src);
    pMarker[0] = ' ' | (A_BULL << 8); pMarker[1] = ' '; pMarkerN = 2;
    mdAppend(t + 2, tl - 2);
    return;
  }
  int d = 0;
  while (d < tl && d < 4 && isdigit((unsigned char)t[d])) d++;
  if (d && d + 1 < tl && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {     // numbered
    mdOpen(MD_TEXT, lvl * 2, 0, src);
    for (int k = 0; k <= d; k++) pMarker[pMarkerN++] = (uint8_t)t[k];
    pMarker[pMarkerN++] = ' ';
    mdAppend(t + d + 2, tl - d - 2);
    return;
  }
  if (!pOpen) mdOpen(MD_TEXT, 0, 0, src);                        // paragraph / lazy continuation
  mdAppend(t, tl);
}

// Plain-text view (non-.md files): every source line is word-wrapped on its own,
// blank lines and leading indentation are kept.
static void mdPlainLine(const char *line, int ll, uint32_t src) {
  if (!ll) { mdEmit(MD_TEXT, 0, 0, src, nullptr, 0); return; }
  mdParaN = 0;
  bool lead = true;
  for (int k = 0; k < ll; k++) {
    uint8_t c = (uint8_t)line[k];
    if (lead && c == ' ') c = 0xA0; else lead = false;         // no-break space keeps indentation
    paraPush(c, 0);
  }
  mdWrap(MD_TEXT, 0, 0, src, nullptr, 0, mdPara, mdParaN, MD_W);
}

static void mdBuild(void) {
  bool markdown = isMarkdown(docName);
  mdCellsN = mdLinesN = 0; mdRawN = 0; mdOom = false;
  pOpen = false; pHard = false; pMarkerN = 0;
  static char line[1024];
  uint32_t len = docLen(), pos = 0;
  bool fence = false;
  for (;;) {
    uint32_t e = pos;
    while (e < len && docAt(e) != '\n') e++;
    int ll = (e - pos) < sizeof(line) - 1 ? (int)(e - pos) : (int)sizeof(line) - 1;
    for (int k = 0; k < ll; k++) line[k] = docAt(pos + k);
    line[ll] = 0;
    if (markdown) mdLine(line, ll, pos, &fence); else mdPlainLine(line, ll, pos);
    if (e >= len) break;
    pos = e + 1;
  }
  mdFlush();
  while (mdLinesN && mdLines[mdLinesN - 1].kind == MD_GAP) mdLinesN--;
  DBG("[MD] %lu display lines, %lu cells%s\n", (unsigned long)mdLinesN,
      (unsigned long)mdCellsN, mdOom ? " (OUT OF MEMORY, truncated)" : "");
}

static int mdHeight(uint8_t k) {
  switch (k) {
    case MD_H1: return 54;  case MD_H2: return 32; case MD_H3: return 28;
    case MD_HR: return 14;  case MD_GAP: return 10; default: return 24;
  }
}

static const int MD_Y0 = HDR_H + 6;

static uint32_t mdMaxTop(void) {
  int h = 0;
  uint32_t i = mdLinesN;
  while (i > 0 && h + mdHeight(mdLines[i - 1].kind) <= SCR_H - MD_Y0) { h += mdHeight(mdLines[i - 1].kind); i--; }
  return i;
}

static void mdGlyph(int x, int y, uint16_t cell, uint8_t scale) {
  uint8_t c = cell & 0xFF, at = cell >> 8;
  int w = CELL_W * scale, h = CELL_H * scale;
  if (at & A_BULL) {                                              // bullet: filled disc
    int cx = x + w / 2, cy = y + h / 2 + scale, r = 3 * scale;
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) px(cx + dx, cy + dy, true);
    return;
  }
  bool inv = at & A_CODE;
  if (inv) fillRect(x, y + scale, w, h - 2 * scale, true);
  const uint16_t *g = spleen12x24[glyphIdx(c)];
  for (int r = 0; r < CELL_H; r++) {
    uint16_t bits = g[r];
    if (!bits) continue;
    int sh = (at & A_ITAL) ? (CELL_H - 1 - r) / 8 : 0;             // italic shear, 0..2 px
    for (int col = 0; col < CELL_W; col++) {
      if (!(bits & (0x800 >> col))) continue;
      int X = x + (col + sh) * scale, Y = y + r * scale;
      fillRect(X, Y, scale, scale, !inv);
      if (at & A_BOLD) fillRect(X + 1, Y, scale, scale, !inv);    // double strike
    }
  }
  if (at & A_UNDR) hLine(x, x + w - 1, y + h - 2 * scale, !inv);
  if (at & A_STRK) hLine(x, x + w - 1, y + h / 2 + scale, !inv);
}

static void renderPreview(void) {
  fillRect(0, 0, SCR_W, HDR_H, true);
  char l[64], r[64];
  snprintf(l, sizeof(l), " %s  %s%s", isMarkdown(docName) ? "PREVIEW" : "VIEW",
           docSecret ? "[P] " : "", docName);
  text16(0, 1, l, false);
  uint32_t mt = mdMaxTop();
  snprintf(r, sizeof(r), "%u%%   UP/DN: page   EXIT/Esc: back ",
           (unsigned)(mt ? (uint64_t)mdTop * 100 / mt : 100));
  text16(SCR_W - strLen8(r) * 8, 1, r, false);

  int y = MD_Y0;
  for (uint32_t i = mdTop; i < mdLinesN; i++) {
    const mdline_t &L = mdLines[i];
    int h = mdHeight(L.kind);
    if (y + h > SCR_H) break;
    int x = TXT_X + L.indent * CELL_W;
    uint8_t scale = 1, force = 0;
    switch (L.kind) {
      case MD_H1: scale = 2; force = A_BOLD; fillRect(TXT_X, y + 50, SCR_W - 2 * TXT_X, 2, true); break;
      case MD_H2: force = A_BOLD; hLine(TXT_X, SCR_W - TXT_X, y + 28, true); break;
      case MD_H3: force = A_BOLD; break;
      case MD_QUOTE:
        for (int d = 0; d < L.depth; d++) fillRect(TXT_X + d * 2 * CELL_W + 2, y, 3, h, true);
        x += L.depth * 2 * CELL_W; break;
      case MD_CODE: fillRect(TXT_X + 2, y, 3, h, true); x = TXT_X + 2 * CELL_W; break;
      case MD_HR:   fillRect(TXT_X, y + 6, SCR_W - 2 * TXT_X - 8, 2, true); break;
      default: break;
    }
    for (uint16_t k = 0; k < L.n; k++)
      mdGlyph(x + k * CELL_W * scale, y, mdCells[L.c0 + k] | (force << 8), scale);
    y += h;
  }
  if (mdLinesN == 0) text24C(120, "(empty document)");
  // scroll bar
  const int sx = SCR_W - 6, sy0 = MD_Y0, sh = SCR_H - MD_Y0 - 4;
  for (int yy = sy0; yy < sy0 + sh; yy += 4) px(sx + 1, yy, true);
  if (mt > 0) {
    int ty = sy0 + (int)((uint64_t)(sh - 16) * mdTop / mt);
    fillRect(sx, ty, 3, 16, true);
  }
}

static void startPreview(uint8_t ret) {
  viewReturn = ret;
  xSemaphoreTake(docMutex, portMAX_DELAY);
  mdBuild();
  mdTop = 0;                                           // open at the cursor's paragraph
  for (uint32_t i = 0; i < mdLinesN; i++) if (mdLines[i].src <= cursorPos) mdTop = i; else break;
  uint32_t mt = mdMaxTop();
  if (mdTop > mt) mdTop = mt;
  uiMode = UI_PREVIEW;
  xSemaphoreGive(docMutex);
  if (mdOom) setStatus("Preview truncated: out of memory", 4000);
}

static void handlePreviewKey(uint8_t u, bool ctrl) {
  uint32_t mt = mdMaxTop();
  int area = SCR_H - MD_Y0;
  switch (u) {
    case 0x29: uiMode = viewReturn;                                    // Esc
               if (viewReturn == UI_FILES) listFiles();
               return;
    case 0x13: if (ctrl && bleState == BLE_READY) uiMode = UI_EDIT; return;   // Ctrl+P: edit
    case 0x51: if (mdTop < mt) mdTop++; return;                        // down
    case 0x52: if (mdTop) mdTop--; return;                             // up
    case 0x4E: case 0x2C: case 0x28: {                                 // PgDn / Space / Enter (OK)
      int h = 0;
      while (mdTop < mt && h + mdHeight(mdLines[mdTop].kind) <= area - 24) h += mdHeight(mdLines[mdTop++].kind);
      return;
    }
    case 0x4B: {                                                       // PgUp
      int h = 0;
      while (mdTop > 0 && h + mdHeight(mdLines[mdTop - 1].kind) <= area - 24) h += mdHeight(mdLines[--mdTop].kind);
      return;
    }
    case 0x4A: mdTop = 0; return;                                      // Home
    case 0x4D: mdTop = mt; return;                                     // End
  }
}

static bool pairShown = false;               // last frame was the pairing screen

static void renderFrame(void) {
  paintClear();
  bool needPair = passkeyActive ||
                  (bleState != BLE_READY && numBonds == 0 && !pairDismissed);
  pairShown = needPair;
  if (needPair)               renderPair();
  else if (uiMode == UI_DESK)  renderDesk();
  else if (uiMode == UI_SETTINGS) renderSettings();
  else if (uiMode == UI_INPUT) renderInput();
  else if (uiMode == UI_PREVIEW) renderPreview();
  else if (uiMode == UI_FILES) renderFiles();
  else if (uiMode == UI_HELP)  renderHelp();
  else if (uiMode == UI_XFER)  renderXfer();
  else                        renderEdit();
}

// Display task: natural coalescing. While the panel refreshes, keystrokes
// pile up; when BUSY is released the most recent state gets drawn.
static void displayTask(void *) {
  uint32_t done = 0, partials = 0;
  bool idleCleaned = false;              // the 60 s idle refresh runs once per idle period
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    uint32_t now  = millis();
    uint32_t idle = now - lastInputMs;
    if (idle < CLEAN_IDLE_MS) idleCleaned = false;
    bool want  = (viewSeq != done);
    bool idleClean = (partials > 0 && idle >= CLEAN_IDLE_MS && !idleCleaned);
    bool clean = cleanRequest || idleClean ||
                 (partials >= CLEAN_HARD_PARTIALS) ||
                 (partials >= CLEAN_SOFT_PARTIALS && idle >= CLEAN_PAUSE_MS);
    if (!want && !clean) continue;

    uint32_t seq = viewSeq;
    uint32_t t0 = millis();
    bool wasPair = pairShown;
    xSemaphoreTake(docMutex, portMAX_DELAY);
    renderFrame();
    xSemaphoreGive(docMutex);
    uint32_t t1 = millis();
    // Entering or leaving the pairing screen replaces almost every pixel:
    // do a full refresh so no ghost of the previous layout remains.
    if (wasPair != pairShown && done != 0) clean = true;

    if (clean) { cleanRequest = false; epdFullClear(); partials = 0; if (idleClean) idleCleaned = true; }
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
    if (altgr) return (kbdLayout == LAYOUT_LATAM && u == 0x14) ? '@' : 0;    // AltGr+Q
    char c = 'a' + (u - 0x04);
    return (shift ^ capsLock) ? (c - 32) : c;
  }
  if (u == 0x2C) return ' ';
  const keymap_t *km; size_t n;
  if (kbdLayout == LAYOUT_ES)       { km = KM_ES;    n = sizeof(KM_ES) / sizeof(km[0]); }
  else if (kbdLayout == LAYOUT_US)  { km = KM_US;    n = sizeof(KM_US) / sizeof(km[0]); }
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
  for (uint8_t i = 0; i < nFiles; i++) if (!strcmp(fileNames[i], docName) && fileSecret[i] == docSecret) fileSel = i;
  if (uiMode != UI_FILES) prevMode = uiMode;
  uiMode = UI_FILES;
}

static void newDoc(void) {
  if (docDirty) saveDoc();
  char nm[40]; newDocName(nm, sizeof(nm));
  xSemaphoreTake(docMutex, portMAX_DELAY);
  docClear();
  strlcpy(docName, nm, sizeof(docName));
  docSecret = false;
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
  bool sec = fileSecret[fileSel];
  char oldP[64], newP[64];
  docPath(fileNames[fileSel], sec, oldP, sizeof(oldP));
  docPath(nn.c_str(), sec, newP, sizeof(newP));
  if (!strcmp(oldP, newP)) { fileAction = FA_NONE; return; }
  if (nameTaken(nn.c_str())) { setStatus("Name already in use"); return; }
  bool isOpen = !strcmp(fileNames[fileSel], docName) && sec == docSecret;
  if (isOpen && docDirty) saveDoc();
  if (!docFS->rename(oldP, newP)) { setStatus("ERROR renaming"); return; }
  if (isOpen) { strlcpy(docName, nn.c_str(), sizeof(docName)); prefs.putString(prefDocKey(), docName); }
  listFiles();
  for (uint8_t i = 0; i < nFiles; i++) if (nn == fileNames[i]) fileSel = i;
  fileAction = FA_NONE;
  setStatus("Renamed");
}

// P in the file list: move the entry between /docs and /secret
static void toggleSecret(void) {
  if (!nFiles) return;
  if (!pwIsSet()) { setStatus("Set a password in Settings first", 4000); return; }
  if (!privOpen)  { setStatus("Unlock private documents first", 4000); return; }
  bool sec = fileSecret[fileSel];
  char a[64], b[64];
  docPath(fileNames[fileSel], sec, a, sizeof(a));
  docPath(fileNames[fileSel], !sec, b, sizeof(b));
  bool isOpen = !strcmp(fileNames[fileSel], docName) && sec == docSecret;
  if (isOpen && docDirty) saveDoc();
  if (!docFS->rename(a, b)) { setStatus("ERROR moving the file"); return; }
  if (isOpen) { docSecret = !sec; prefs.putBool(prefSecKey(), docSecret); }
  String nm = fileNames[fileSel];
  listFiles();
  for (uint8_t i = 0; i < nFiles; i++) if (nm == fileNames[i]) fileSel = i;
  setStatus(sec ? "Now public" : "Now private");
}

static void doDelete(void) {
  if (!nFiles) return;
  bool sec = fileSecret[fileSel];
  char path[64]; docPath(fileNames[fileSel], sec, path, sizeof(path));
  bool isOpen = !strcmp(fileNames[fileSel], docName) && sec == docSecret;
  if (isOpen) docDirty = false;                 // do not let autosave recreate it
  docFS->remove(path);
  listFiles();
  if (isOpen) openFirstPublic();
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
      if (!nFiles) break;
      if (docDirty) saveDoc();
      loadDoc(fileNames[fileSel], fileSecret[fileSel]);
      // Without a keyboard (or from a board button) open the read-only viewer
      if (btnEvent || bleState != BLE_READY) startPreview(UI_FILES);
      else uiMode = UI_EDIT;
      break;
    case 0x29: uiMode = prevMode; break;                                        // Esc
    case 0x11: newDoc(); setStatus("New document"); break;                      // N
    case 0x15:                                                                  // R
      if (nFiles) {
        strlcpy(renameBuf, fileNames[fileSel], sizeof(renameBuf));
        renameLen = strlen(renameBuf);
        fileAction = FA_RENAME;
      }
      break;
    case 0x07: case 0x4C: if (nFiles) fileAction = FA_DELETE; break;           // D / Del
    case 0x13: toggleSecret(); break;                                           // P
  }
}

static void wifiOff(void);

static void privLock(void) {
  if (!privOpen) return;
  bool wasSecret = docSecret;
  if (docDirty) saveDoc();
  privOpen = false;
  if (wasSecret) openFirstPublic();                  // never leave a private text on screen
  if (uiMode == UI_FILES) { fileAction = FA_NONE; listFiles(); }
  if (wasSecret && (uiMode == UI_EDIT || uiMode == UI_PREVIEW)) uiMode = UI_DESK;
  setStatus("Private documents locked");
  DBG("[LOCK] private documents locked\n");
}

// ASCII-only line entry used by the password dialogs and the lock screen
static void inputKey(uint8_t u, uint8_t mods) {
  bool shift = mods & 0x22;
  bool altgr = (mods & 0x40) || ((mods & 0x01) && (mods & 0x04));
  if (u == 0x2A) { if (inLen) inBuf[--inLen] = 0; return; }
  uint8_t c = translate(u, shift, altgr);
  if (c == DK_GRAVE) c = '`';
  if (c == DK_CIRC)  c = '^';
  if (c >= 0x20 && c <= 0x7E && inLen < sizeof(inBuf) - 1) { inBuf[inLen++] = c; inBuf[inLen] = 0; }
}

static void clearInput(void) { memset(inBuf, 0, sizeof(inBuf)); inLen = 0; }


static void handleInputKey(uint8_t u, uint8_t mods) {
  if (u == 0x29) { clearInput(); memset(pwFirst, 0, sizeof(pwFirst));
                   uiMode = inPurpose == IN_UNLOCK ? UI_DESK : UI_SETTINGS; return; }
  if (!(u == 0x28 || u == 0x58)) { inputKey(u, mods); return; }
  switch (inPurpose) {
    case IN_UNLOCK:
      if (millis() < lockUntil) { setStatus("Too many attempts, wait"); break; }
      if (pwCheck(inBuf)) {
        privOpen = true; lockFails = 0; uiMode = UI_DESK;
        setStatus("Private documents visible");
        DBG("[LOCK] private documents unlocked\n");
      } else {
        lockFails++;
        if (lockFails >= 5) lockUntil = millis() + 30000UL * (lockFails - 4);
        char m[48]; snprintf(m, sizeof(m), "Wrong password (%u)", lockFails);
        setStatus(m);
      }
      break;
    case IN_PW_OLD:
      if (!pwCheck(inBuf)) { setStatus("Wrong password"); clearInput(); uiMode = UI_SETTINGS; return; }
      privOpen = true;
      inPurpose = IN_PW_NEW1; break;
    case IN_PW_NEW1:
      if (!inLen) { pwClear(); autoLockMin = 0; prefs.putUChar("alock", 0);
                    setStatus("Password removed"); uiMode = UI_SETTINGS; break; }
      if (inLen < PW_MIN_LEN) { setStatus("Password too short"); break; }
      strlcpy(pwFirst, inBuf, sizeof(pwFirst)); inPurpose = IN_PW_NEW2; break;
    case IN_PW_NEW2:
      if (strcmp(pwFirst, inBuf)) { setStatus("Passwords do not match"); inPurpose = IN_PW_NEW1; }
      else { pwStore(inBuf); privOpen = true; setStatus("Password set"); uiMode = UI_SETTINGS; }
      memset(pwFirst, 0, sizeof(pwFirst));
      break;
  }
  clearInput();
}

static void newDoc(void);
static void startFiles(void);
static void toggleWiFi(void);

static bool needKeyboard(void) {
  if (bleState == BLE_READY) return false;
  setStatus("Connect the keyboard for this", 3000);
  return true;
}

static void deskActivate(uint8_t i) {
  switch (i) {
    case 0:                                                      // continue / read
      if (btnEvent || bleState != BLE_READY) startPreview(UI_DESK); else uiMode = UI_EDIT;
      break;
    case 1: if (!needKeyboard()) { newDoc(); setStatus("New document"); } break;
    case 2: startFiles(); break;
    case 3: prevMode = UI_DESK; toggleWiFi(); break;
    case 4: setSel = 0; setConfirm = false; uiMode = UI_SETTINGS; break;
    case 5: prevMode = UI_DESK; uiMode = UI_HELP; break;
    case 6:
      if (privOpen) privLock();
      else if (!needKeyboard()) { clearInput(); inPurpose = IN_UNLOCK; uiMode = UI_INPUT; }
      break;
  }
}

static void handleDeskKey(uint8_t u) {
  uint8_t n = deskCount();
  if (deskSel >= n) deskSel = 0;
  if (u == 0x52) deskSel = deskSel ? deskSel - 1 : n - 1;
  else if (u == 0x51) deskSel = (deskSel + 1) % n;
  else if (u == 0x28 || u == 0x58) deskActivate(deskSel);
  else if (u >= 0x1E && u < 0x1E + n) { deskSel = u - 0x1E; deskActivate(deskSel); }   // digits 1..n
}

// Switch the document volume (internal flash <-> SD card)
static void switchVolume(bool toSD) {
  if (toSD && !sdMounted) { setStatus("No SD card"); return; }
  if (docDirty) saveDoc();
  docFS = toSD ? (fs::FS *)&SD : (fs::FS *)&LittleFS;
  prefs.putUChar("vol", toSD ? 1 : 0);
  String last = prefs.getString(prefDocKey(), "");
  bool sec = prefs.getBool(prefSecKey(), false) && privVisible();
  if (last.length() && docFS->exists(String(dirOf(sec)) + "/" + last)) loadDoc(last.c_str(), sec);
  else openFirstPublic();
  listFiles();
  setStatus(toSD ? "Now using the SD card" : "Now using internal flash");
}

// Copy every document of the active volume to the other one (existing files are kept)
static void copyDir(fs::FS &src, fs::FS &dst, const char *dir, unsigned &copied, unsigned &skipped) {
  static uint8_t buf[2048];
  if (!dst.exists(dir)) dst.mkdir(dir);
  File d = src.open(dir);
  File e;
  while (d && (e = d.openNextFile())) {
    const char *nm = e.name();
    const char *b = strrchr(nm, '/'); b = b ? b + 1 : nm;
    if (b[0] == '.' || e.isDirectory()) { e.close(); continue; }
    String p = String(dir) + "/" + b;
    if (dst.exists(p)) { skipped++; e.close(); continue; }
    File o = dst.open(p, "w");
    if (o) { size_t n; while ((n = e.read(buf, sizeof(buf))) > 0) o.write(buf, n); o.close(); copied++; }
    e.close();
  }
  if (d) d.close();
}

// Copy every document of the active volume to the other one (existing files are kept)
static void copyAllDocs(void) {
  if (!sdMounted) { setStatus("No SD card"); return; }
  if (docDirty) saveDoc();
  fs::FS &src = *docFS;
  fs::FS &dst = volIsSD() ? (fs::FS &)LittleFS : (fs::FS &)SD;
  unsigned copied = 0, skipped = 0;
  copyDir(src, dst, "/docs", copied, skipped);
  if (privVisible()) copyDir(src, dst, "/secret", copied, skipped);
  char m[64]; snprintf(m, sizeof(m), "Copied %u, skipped %u (already there)", copied, skipped);
  setStatus(m, 5000);
}

static void handleSettingsKey(uint8_t u) {
  if (setConfirm) {
    setConfirm = false;
    if (u == 0x1C || u == 0x28) { reqUnpair = true; setStatus("Pairing deleted"); } // Y or Enter/OK
    return;
  }
  switch (u) {
    case 0x29: uiMode = UI_DESK; return;                                             // Esc
    case 0x52: setSel = setSel ? setSel - 1 : 6; return;
    case 0x51: setSel = (setSel + 1) % 7; return;
    case 0x28: case 0x58: break;
    default: return;
  }
  switch (setSel) {
    case 0: if (needKeyboard()) break;
            clearInput(); inPurpose = pwIsSet() ? IN_PW_OLD : IN_PW_NEW1; uiMode = UI_INPUT; break;
    case 1:
      if (!pwIsSet()) { setStatus("Set a password first"); break; }
      autoLockMin = autoLockMin == 0 ? 5 : autoLockMin == 5 ? 15 : autoLockMin == 15 ? 30 : 0;
      prefs.putUChar("alock", autoLockMin); break;
    case 2: kbdLayout = (kbdLayout + 1) % 3; prefs.putUChar("layout", kbdLayout); break;
    case 3: switchVolume(!volIsSD()); break;
    case 4: copyAllDocs(); break;
    case 5:
      wallMode = (wallMode + 1) % 3; prefs.putUChar("wall", wallMode);
      if (wallMode == WALL_FILE) loadWallpaper();
      break;
    case 6: setConfirm = true; break;
  }
}

static void handlePress(uint8_t u, uint8_t mods, bool repeat) {
  bool ctrl  = mods & 0x11;
  bool shift = mods & 0x22;
  bool altgr = (mods & 0x40) || ((mods & 0x01) && (mods & 0x04));
  if (altgr) ctrl = false;

  if (u == 0x39) { if (!repeat) capsLock = !capsLock; requestRender(); return; }

  if (uiMode == UI_DESK)     { if (!repeat) handleDeskKey(u); requestRender(); return; }
  if (uiMode == UI_SETTINGS) { if (!repeat) handleSettingsKey(u); requestRender(); return; }
  if (uiMode == UI_INPUT)    { handleInputKey(u, mods); requestRender(); return; }
  if (uiMode == UI_HELP) { if (!repeat) uiMode = prevMode; requestRender(); return; }
  if (uiMode == UI_PREVIEW)  { handlePreviewKey(u, ctrl); requestRender(); return; }
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
      case 0x0B: prevMode = uiMode; uiMode = UI_HELP; break;                       // H
      case 0x0F: privLock(); break;                                                // L
      case 0x13: startPreview(UI_EDIT); break;                                     // P
      case 0x1A: prevMode = uiMode; toggleWiFi(); break;                           // W
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
    case 0x29:                                                     // Esc
      if (deadKey) { deadKey = 0; break; }
      xSemaphoreGive(docMutex);
      if (docDirty) saveDoc();
      uiMode = UI_DESK;
      requestRender();
      return;
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
  if (kbLost) { kbLost = false; heldUsage = 0; }
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
/* BLE state shown to the user. While a keyboard is bonded, only "linked" vs
 * "not linked" is visible, so scan/connect retries must not redraw the panel:
 * with the keyboard switched to another host (K380s Easy-Switch) the retry
 * loop would otherwise refresh the E-Paper every few seconds. */
static void setBleState(uint8_t s) {
  uint8_t old = bleState;
  bleState = s;
  bool visibleChange = (numBonds == 0) ? (old != s) : ((old == BLE_READY) != (s == BLE_READY));
  if (visibleChange) requestRender();
}

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    DBG("[BLE] disconnected, reason=%d\n", reason);
    passkeyActive = false;
    kbLost = true;
    setBleState(BLE_IDLE);
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
  // 30 % duty cycle (window 30 ms every 100 ms). A 100 % BLE scan leaves almost
  // no airtime to WiFi on the shared 2.4 GHz radio; keyboards advertise every
  // few tens of ms when reconnecting, so 30 % still finds them quickly.
  scan->setInterval(100);
  scan->setWindow(30);

  NimBLEClient *cl = NimBLEDevice::createClient();
  cl->setClientCallbacks(&ccb, false);
  // 7.5-30 ms, latency 0, timeout 4 s; connection-initiation scan 30 ms every 100 ms
  cl->setConnectionParams(6, 24, 0, 400, 160, 48);
  cl->setConnectTimeout(5000);
  bool relaxed = false;

  for (;;) {
    int nb = NimBLEDevice::getNumBonds();
    if (nb != numBonds) { numBonds = nb; requestRender(); }
    if (reqUnpair) {
      reqUnpair = false;
      if (cl->isConnected()) cl->disconnect();
      NimBLEDevice::deleteAllBonds();
      kbBattery = -1;
      DBG("[BLE] bonds deleted\n");
      requestRender();
    }
    bool wifiOn = (wifiState != WF_OFF);
    if (cl->isConnected()) {
      // During file transfer ask the keyboard for a relaxed link (30-50 ms,
      // slave latency 4) so WiFi gets more airtime; restore it afterwards.
      if (wifiOn && !relaxed) {
        relaxed = true;
        DBG("[BLE] relaxed conn params for WiFi: %d\n", cl->updateConnParams(24, 40, 4, 600));
      } else if (!wifiOn && relaxed) {
        relaxed = false;
        DBG("[BLE] normal conn params: %d\n", cl->updateConnParams(6, 24, 0, 400));
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    relaxed = false;
    // No keyboard and WiFi on: do not scan or initiate at all, the radio is
    // left entirely to the hotspot until the transfer is closed.
    if (wifiOn) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    // 1) Scan in 5 s windows
    setBleState(BLE_SCAN);
    candFound = false;
    scan->start(5000, false, true);
    uint32_t t0 = millis();
    while (!candFound && millis() - t0 < 5200 && !reqUnpair && wifiState == WF_OFF) vTaskDelay(pdMS_TO_TICKS(50));
    if (wifiState != WF_OFF && !candFound) { if (scan->isScanning()) scan->stop(); continue; }
    if (scan->isScanning()) scan->stop();

    NimBLEAddress target;
    if (candFound) target = candAddr;
    else if (numBonds > 0) {
      // 2) Direct attempt to the bonded keyboard (covers directed advertising)
      target = NimBLEDevice::getBondedAddress(0);
      DBG("[BLE] direct attempt to %s\n", target.toString().c_str());
    } else continue;

    setBleState(BLE_CONNECTING);
    if (!cl->connect(target, true)) { setBleState(BLE_IDLE); continue; }
    DBG("[BLE] connected to %s, securing link...\n", target.toString().c_str());

    if (!secureLink(cl)) {
      DBG("[BLE] security failure\n");
      if (cl->isConnected()) cl->disconnect();
      setBleState(BLE_IDLE);
      continue;
    }
    if (!setupHid(cl)) {
      DBG("[BLE] no keyboard report found\n");
      cl->disconnect(); setBleState(BLE_IDLE);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    setBleState(BLE_READY);
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
               "<h2>ELECTGPL WRITER</h2>");
  h += "<p>Storage: " + String(volName()) + "</p><table>";
  for (uint8_t i = 0; i < nFiles; i++) {
    String q = String(fileNames[i]) + (fileSecret[i] ? "&s=1" : "");
    h += "<tr><td>" + String(fileSecret[i] ? "[P] " : "") + String(fileNames[i]) + "</td><td>" + String(fileSizes[i]) +
         " B</td><td><a href='/dl?f=" + q +
         "'>download</a></td><td><a href='/del?f=" + q +
         "' onclick=\"return confirm('Delete?')\">delete</a></td></tr>";
  }
  h += F("</table>"
         "<h3>Upload a note (.txt / .md)</h3><input type='file' id='fd' accept='.txt,.md'> "
         "<button onclick=\"up('doc','fd')\">Upload note</button>"
         "<h3>Wallpaper (.bmp, 792x272 recommended)</h3><input type='file' id='fw' accept='.bmp'> "
         "<button onclick=\"up('wall','fw')\">Upload wallpaper</button>"
         "<p><progress id='p' max='100' value='0' style='width:100%'></progress><br><span id='s'></span></p>"
         "<script>"
         "function up(k,id){var i=document.getElementById(id),s=document.getElementById('s'),"
         "p=document.getElementById('p');if(!i.files.length){s.textContent='Choose a file first';return;}"
         "var f=i.files[0],x=new XMLHttpRequest();"
         "x.open('POST','/upload');"
         "x.setRequestHeader('Content-Type','application/octet-stream');"
         "x.setRequestHeader('X-Kind',k);x.setRequestHeader('X-Name',encodeURIComponent(f.name));"
         "x.upload.onprogress=function(e){if(e.lengthComputable){p.value=100*e.loaded/e.total;"
         "s.textContent='Uploading '+f.name+': '+Math.round(100*e.loaded/e.total)+' %';}};"
         "x.onload=function(){s.textContent=x.responseText;if(x.status==200)setTimeout(function(){location.reload();},1500);};"
         "x.onerror=function(){s.textContent='Network error: no answer from the device';};"
         "s.textContent='Uploading '+f.name+'...';x.send(f);}"
         "</script></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void webDownload(void) {
  String n = server.arg("f");
  if (!safeName(n)) { server.send(400, "text/plain", "invalid name"); return; }
  bool sec = server.arg("s") == "1";
  if (sec && !privVisible()) { server.send(403, "text/plain", "private documents are locked"); return; }
  File f = docFS->open(String(dirOf(sec)) + "/" + n, "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + n + "\"");
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

static void webDelete(void) {
  String n = server.arg("f");
  bool sec = server.arg("s") == "1";
  if (safeName(n) && !(n == docName && sec == docSecret) && (!sec || privVisible()))
    docFS->remove(String(dirOf(sec)) + "/" + n);
  server.sendHeader("Location", "/"); server.send(303);
}

/* Uploads arrive as a raw request body (application/octet-stream) sent by the
 * page's JavaScript, with the file name in the X-Name header. This avoids the
 * multipart/form-data parser and lets the browser show real upload progress. */
static void webUploadRaw(void) {
  HTTPRaw &r = server.raw();
  if (r.status == RAW_START) {
    upBytes = 0; upOk = false; upMsg[0] = 0;
    upIsWall = server.header("X-Kind") == "wall";
    String n = server.header("X-Name");               // encodeURIComponent keeps [A-Za-z0-9._-]
    strlcpy(upName, n.c_str(), sizeof(upName));
    if (upIsWall) upFile = docFS->open(WALL_FILE_PATH, "w");
    else if (safeName(n) && n != docName && !docFS->exists("/secret/" + n)) upFile = docFS->open("/docs/" + n, "w");
    else upFile = File();
    DBG("[WEB] upload start '%s' (%s) -> %s\n", upName, upIsWall ? "wallpaper" : "note",
        upFile ? "open" : "REJECTED");
  } else if (r.status == RAW_WRITE) {
    if (upFile && upFile.write(r.buf, r.currentSize) != r.currentSize) {
      upFile.close(); upFile = File();                 // storage full or write error
      snprintf(upMsg, sizeof(upMsg), "ERROR writing %s (storage full?)", upName);
    }
    upBytes += r.currentSize;
  } else if (r.status == RAW_END) {
    if (upFile) { upFile.close(); upOk = true; }
    DBG("[WEB] upload end '%s' %lu B %s\n", upName, (unsigned long)upBytes, upOk ? "OK" : "FAILED");
  } else if (r.status == RAW_ABORTED) {
    if (upFile) upFile.close();
    upOk = false;
    DBG("[WEB] upload aborted '%s' after %lu B\n", upName, (unsigned long)upBytes);
  }
}

static void webUploadDone(void) {
  char sz[16]; fmtSize(upBytes, sz, sizeof(sz));
  if (upOk) {
    snprintf(upMsg, sizeof(upMsg), "OK: %s (%s)%s", upName, sz, upIsWall ? " set as wallpaper" : "");
    if (upIsWall) { wallMode = WALL_FILE; prefs.putUChar("wall", wallMode); wallReload = true; }
    server.send(200, "text/plain", upMsg);
  } else {
    if (!upMsg[0] || strncmp(upMsg, "ERROR", 5))
      snprintf(upMsg, sizeof(upMsg), "ERROR: %s rejected (name: a-z 0-9 . _ - , .txt/.md) or incomplete", upName);
    server.send(400, "text/plain", upMsg);
  }
  requestRender();                                      // show the result on the device too
}

/* WiFi and BLE run at the same time/* WiFi and BLE run at the same time: the ESP32-S3 has a single 2.4 GHz radio
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
    uiMode = (prevMode == UI_XFER) ? (uint8_t)UI_DESK : prevMode;
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
static void buttonKey(uint8_t u) {
  lastInputMs = millis();
  btnEvent = true;
  handlePress(u, 0, false);
  btnEvent = false;
}

static void goHome(void) {
  if (wifiState != WF_OFF) wifiOff();
  if (docDirty) saveDoc();
  fileAction = FA_NONE; setConfirm = false;
  uiMode = UI_DESK;
  requestRender();
}

/* Side buttons for keyboard-less use ("pocket PDA" viewer):
 *   UP / DOWN : move (lists) or page (editor / viewer), auto-repeat when held
 *   OK        : select (Enter); in the editor: save
 *   EXIT      : back (Esc); held 3 s: forget the keyboard
 *   BOOT      : desktop          MENU : full refresh */
static void handleButtons(void) {
  static const uint8_t pins[6] = { BTN_UP, BTN_DOWN, BTN_OK, BTN_MENU, BTN_EXIT, BTN_BOOT };
  static uint8_t  last[6] = {1, 1, 1, 1, 1, 1};
  static uint32_t tDown[6], tRep[6], lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 25) return;                            // debounce
  lastMs = now;
  bool pairScreen = !passkeyActive && bleState != BLE_READY && numBonds == 0 && !pairDismissed;
  for (uint8_t i = 0; i < 6; i++) {
    uint8_t v = digitalRead(pins[i]);
    bool pressed = (v == LOW && last[i] == HIGH), released = (v == HIGH && last[i] == LOW);
    last[i] = v;
    if (pressed) { tDown[i] = now; tRep[i] = now + 600; }
    if (pairScreen) {                                       // any button: skip pairing, read docs
      if (released) { pairDismissed = true; lastInputMs = now; requestRender(); }
      continue;
    }
    if (passkeyActive) continue;
    bool page = (uiMode == UI_EDIT || uiMode == UI_PREVIEW);
    if (pins[i] == BTN_UP || pins[i] == BTN_DOWN) {
      bool rep = (v == LOW && !pressed && (int32_t)(now - tRep[i]) >= 0);
      if (pressed || rep) {
        if (rep) tRep[i] = now + 350;
        uint8_t k = pins[i] == BTN_UP ? (page ? 0x4B : 0x52) : (page ? 0x4E : 0x51);
        buttonKey(k);
      }
      continue;
    }
    if (!released) continue;
    uint32_t held = now - tDown[i];
    switch (pins[i]) {
      case BTN_OK:
        if (uiMode == UI_EDIT) { lastInputMs = now; setStatus(saveDoc() ? "Saved" : "ERROR saving"); }
        else buttonKey(0x28);
        break;
      case BTN_MENU: cleanRequest = true; requestRender(); break;
      case BTN_EXIT:
        if (held >= 3000) { reqUnpair = true; pairDismissed = false; setStatus("Pairing deleted"); }
        else buttonKey(0x29);
        break;
      case BTN_BOOT: lastInputMs = now; goHome(); break;
    }
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
  pinMode(BTN_BOOT, INPUT_PULLUP);

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
  if (!LittleFS.exists("/docs"))   LittleFS.mkdir("/docs");
  if (!LittleFS.exists("/secret")) LittleFS.mkdir("/secret");
  mountSD();

  prefs.begin("writer", false);
  kbdLayout   = prefs.getUChar("layout", KBD_LAYOUT) % 3;
  wallMode    = prefs.getUChar("wall", WALL_BUILTIN) % 3;
  autoLockMin = prefs.getUChar("alock", 0);
  // Password recovery: hold MENU + EXIT while powering up (privacy lock, not encryption)
  if (digitalRead(BTN_MENU) == LOW && digitalRead(BTN_EXIT) == LOW && pwIsSet()) {
    pwClear(); autoLockMin = 0; prefs.putUChar("alock", 0);
    DBG("[LOCK] password cleared by button recovery\n");
  }
  wallBuf = (uint8_t *)ps_malloc(SCR_W / 8 * SCR_H);
  loadWallpaper();
  docFS = (sdMounted && prefs.getUChar("vol", 0) == 1) ? (fs::FS *)&SD : (fs::FS *)&LittleFS;
  String last = prefs.getString(prefDocKey(), "");
  bool lastSec = prefs.getBool(prefSecKey(), false);
  // A private document is not reopened at boot while a password is set
  if (last.length() && !(lastSec && pwIsSet()) &&
      docFS->exists(String(dirOf(lastSec)) + "/" + last)) {
    loadDoc(last.c_str(), lastSec);
    uint32_t c = prefs.getUInt(prefCurKey(), docLen());
    cursorPos = c > docLen() ? docLen() : c;
    fixViewport();
  } else {
    openFirstPublic();
  }

  server.on("/", HTTP_GET, webRoot);
  server.on("/dl", HTTP_GET, webDownload);
  server.on("/del", HTTP_GET, webDelete);
  server.on("/upload", HTTP_POST, webUploadDone, webUploadRaw);
  static const char *hdrKeys[] = { "X-Name", "X-Kind" };
  server.collectHeaders(hdrKeys, 2);

  NimBLEDevice::init("Electgpl-Writer");
  NimBLEDevice::setSecurityAuth(true, true, true);          // bonding, MITM, LE SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // we display the passkey
  numBonds = NimBLEDevice::getNumBonds();   // known before the first frame: no false pairing screen

  if (!psramFound()) setStatus("NO PSRAM: Tools > PSRAM > OPI PSRAM", 60000);
  uiMode = UI_DESK;
  privOpen = false;
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

  if (privOpen && autoLockMin && pwIsSet() &&
      millis() - lastInputMs > (uint32_t)autoLockMin * 60000UL) { DBG("[LOCK] auto-hide\n"); privLock(); }

  if (wallReload) { wallReload = false; loadWallpaper(); requestRender(); }


  static bool statusShown = false;                        // redraw when the status message expires
  bool active = statusMsg[0] && millis() < statusUntil;
  if (statusShown && !active) { statusMsg[0] = 0; requestRender(); }
  statusShown = active;

  delay(2);
}
