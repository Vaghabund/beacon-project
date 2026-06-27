// ─────────────────────────────────────────────────────────────────────────────
//  wifi_rings_esp32s3.ino
//
//  Hardware:
//    Waveshare ESP32-S3-Touch-LCD-2 (ESP32-S3R8, 8MB OPI PSRAM, 16MB flash)
//    OV5640 / OV2640 camera on the board's 24-pin FPC connector
//    Built-in 240×320 ST7789T3 display, used in landscape via rotation(1)
//    One push button — GPIO0, active LOW, also wake source
//
//  Workflow:
//    deep sleep → wake → LIVE VIEW (always, never wakes into result)
//
//    LIVE VIEW
//      lightweight JPEG→blit loop, no processing overhead
//      button press → 100ms settle → fresh capture → PIPELINE
//      10s no press  → light sleep (XCLK running, ~2ms wake)
//      60s no press  → deep sleep  (~300ms wake, full reboot)
//
//    PIPELINE
//      freeze frame → WiFi scan → encode rings → blit result → RESULT VIEW
//
//    RESULT VIEW
//      holds encoded image on screen
//      button press → LIVE VIEW (resets timers)
//      10s no press  → light sleep (XCLK running)
//      60s no press  → deep sleep
//
//  Sleep notes:
//    Light sleep: CPU paused, PSRAM + peripherals alive, XCLK keeps running.
//      Wake source: GPIO0 falling edge. Resume is ~2ms.
//    Deep sleep: full power-off. Wake source: GPIO0 falling edge (EXT0).
//      Resume is a full reboot (~300ms), always enters LIVE VIEW.
//
//  Arduino IDE settings:
//    Board:            ESP32S3 Dev Module
//    USB CDC On Boot:  Enabled     ← required for Serial over native USB
//    PSRAM:            OPI PSRAM    ← critical, must match R8 chip
//    Flash size:       16MB
//    Partition scheme: Custom      ← uses this folder's partitions.csv (~55 images)
//                      (or "Huge APP (3MB No OTA/1MB SPIFFS)" for ~4 images)
//    CPU frequency:    240MHz
//    Core:             3.x (arduino-esp32)
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>           // CST816 capacitive touch (used as a stand-in button)

#include "wifi_rings.h"
#include "wifi_data.h"
#include "beacon_share.h"

// fmt2rgb888 writes raw R,G,B bytes — Pixel must be exactly 3 bytes, no padding.
// data_embed_lsb also relies on this: it treats the image as a flat byte array.
static_assert(sizeof(Pixel) == 3, "Pixel struct must be 3 bytes packed");

// ─── timing ───────────────────────────────────────────────────────────────────
#define IDLE_LIGHT_SLEEP_MS   10000UL   // 10s idle → light sleep
#define IDLE_DEEP_SLEEP_MS    60000UL   // 60s idle → deep sleep
#define CAPTURE_SETTLE_MS     100       // ms to let camera settle before fresh capture

// ─── sleep enable (development) ───────────────────────────────────────────────
// 0 = device stays fully awake in live/result view (no light or deep sleep).
//     Deep sleep powers the board off — backlight off, USB CDC drops, and it
//     only wakes via a full reboot, which makes development + re-flashing painful.
// 1 = production power saving (the 10s light / 60s deep behaviour above).
#define ENABLE_SLEEP  0

// ─── pin assignments ──────────────────────────────────────────────────────────
// Waveshare ESP32-S3-Touch-LCD-2 built-in display and camera mappings.

#define PIN_BUTTON      0    // BOOT button = GPIO0, active LOW

// Camera mapping — Waveshare ESP32-S3-Touch-LCD-2 schematic (see README
// "camera mapping"). The data lines map to the OV2640 DVP bus as Y2..Y9, where
// Y2 = D0 (LSB) and Y9 = D7 (MSB). Getting D0..D7 in the wrong order keeps the
// timing/geometry perfect (that's VSYNC/HREF/PCLK) but scrambles every colour —
// a bit-permutation that preserves brightness. That was the original bug.
#define CAM_PWDN       17
#define CAM_RESET      -1
#define CAM_XCLK       8
#define CAM_SIOD       21
#define CAM_SIOC       16
#define CAM_D7          2   // Y9 (MSB)
#define CAM_D6          7   // Y8
#define CAM_D5         10   // Y7
#define CAM_D4         14   // Y6
#define CAM_D3         11   // Y5
#define CAM_D2         15   // Y4
#define CAM_D1         13   // Y3
#define CAM_D0         12   // Y2 (LSB)
#define CAM_VSYNC      6
#define CAM_HREF       4
#define CAM_PCLK       9

#define LCD_SCLK       39
#define LCD_MOSI       38
#define LCD_MISO       40
#define LCD_DC         42
#define LCD_RST        -1
#define LCD_CS         45
#define LCD_BL         1
#define LCD_ROTATION   1

// ─── capacitive touch (CST816D, shared I2C with the IMU) ──────────────────────
// From the board schematic: TP_SDA=IO48, TP_SCL=IO47, TP_INT=IO46. TP_RESET is
// tied to the board reset net (no GPIO to drive). CST816D I2C address = 0x15.
// Used here as a stand-in button: a finger tap counts as a button press, so the
// device is usable before a physical button is soldered to GPIO0.
#define TP_SDA         48
#define TP_SCL         47
#define TP_INT         46
#define TP_ADDR        0x15

// ─── storage ───────────────────────────────────────────────────────────────────
//  Encoded images are saved to internal flash via LittleFS as lossless PNG
//  (see storage_init / save_png). No SPI sharing, no risk to the display.
//
//  FUTURE microSD UPGRADE (for capacity): the SD slot is SPI and SHARES the LCD
//  bus — verified from the schematic:
//    IO38 = LCD_MOSI / SD_MOSI   (shared)
//    IO39 = LCD_SCLK / SD_SCLK   (shared)
//    IO40 = SD_MISO              (shared MISO line; LCD is write-only)
//    IO41 = SD_CS                (dedicated)
//  Because it is a shared bus, the SD must mount on the SAME SPI instance
//  Arduino_GFX drives (selected by its own CS) — NOT a second SPIClass on these
//  pins, which would steal the routing and break the display. See README
//  "microSD mapping" before enabling. Until then, storage stays on LittleFS.

static constexpr uint16_t COLOR_BLACK = 0x0000;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;
static constexpr uint16_t COLOR_RED = 0xF800;
static constexpr uint16_t COLOR_DARKGREY = 0x7BEF;

// ─── globals ──────────────────────────────────────────────────────────────────
Arduino_DataBus* g_lcd_bus = new Arduino_ESP32SPI(
    LCD_DC,
    LCD_CS,
    LCD_SCLK,
    LCD_MOSI,
    LCD_MISO
);
Arduino_GFX* tft = new Arduino_ST7789(
    g_lcd_bus,
    LCD_RST,
    LCD_ROTATION,
    true,
    240,
    320
);

// PSRAM framebuffers — allocated once in setup(), never freed
Pixel*    g_src    = nullptr;   // decoded camera RGB888
Pixel*    g_dst    = nullptr;   // encoded output RGB888
uint16_t* g_frame565 = nullptr; // display staging buffer in RGB565

// WiFi scan results
WifiNetwork g_nets[MAX_NETWORKS];
int         g_n_nets = 0;

// Encoder config
RingConfig g_cfg = RING_CONFIG_DEFAULT;

// ─── storage state ──────────────────────────────────────────────────────────────
bool        g_fs_ok = false;            // set in setup(); false → save step skipped
Preferences g_prefs;                    // persists the shot counter across reboots
uint8_t     g_payload[WBCN_MAX_FRAME];  // framed scan data, embedded into LSBs

// ─── state ────────────────────────────────────────────────────────────────────
enum class AppState { LIVE_VIEW, PIPELINE, RESULT_VIEW };
AppState g_state = AppState::LIVE_VIEW;

// Defined here (not next to poll_press) so it precedes the auto-generated
// function prototypes the Arduino build inserts at the top of the sketch —
// a scoped enum used as a return type must be visible before those prototypes.
enum class Press { NONE, SHORT, LONG };

// ─── progress bar ─────────────────────────────────────────────────────────────
// 4px bar at bottom edge, 5 pipeline stages (capture, scan, encode, save, blit)
#define PROGRESS_STAGES  5
#define PROGRESS_Y       (IMG_H - 4)
#define PROGRESS_H       4

static void progress_bar(int stage) {
    const int filled = (IMG_W * stage) / PROGRESS_STAGES;
    tft->fillRect(0,      PROGRESS_Y, filled,         PROGRESS_H, COLOR_WHITE);
    tft->fillRect(filled, PROGRESS_Y, IMG_W - filled,  PROGRESS_H, COLOR_DARKGREY);
}

// ─── centered text overlay on top of whatever is on screen ───────────────────
// Dark pill behind white text — readable over any image content.
static void overlay_text(const char* msg) {
    const int pad_x = 10, pad_y = 5;
    const int cw = 6, ch = 8;
    const int len = strlen(msg);
    const int tw = len * cw;
    const int bw = tw + pad_x * 2;
    const int bh = ch + pad_y * 2;
    const int bx = (IMG_W - bw) / 2;
    const int by = (IMG_H - bh) / 2;
    tft->fillRect(bx, by, bw, bh, COLOR_BLACK);
    tft->setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft->setTextSize(1);
    tft->setCursor(bx + pad_x, by + pad_y);
    tft->print(msg);
}

// ─── fatal error — halts with red message ─────────────────────────────────────
static void fatal(const char* msg) {
    tft->fillRect(0, 0, IMG_W, 16, COLOR_BLACK);
    tft->setTextColor(COLOR_RED, COLOR_BLACK);
    tft->setTextSize(1);
    tft->setCursor(4, 4);
    tft->print(msg);
    Serial.printf("FATAL: %s\n", msg);
    while (true) delay(1000);
}

// ─── sleep helpers ────────────────────────────────────────────────────────────
#if ENABLE_SLEEP

// Light sleep: CPU pauses, PSRAM + camera XCLK stay powered.
// Returns when GPIO0 falls (button press). Wake is ~2ms.
static void enter_light_sleep() {
    Serial.println("light sleep");
    // GPIO wakeup works in light sleep (unlike EXT0 which is deep-sleep only)
    gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    // execution resumes here after wake
    gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
    Serial.println("wake (light)");
}

// Deep sleep: full power-off. Wake triggers full reboot → setup() → LIVE VIEW.
static void enter_deep_sleep() {
    Serial.println("deep sleep");
    esp_deep_sleep_start();
    // never returns
}

#endif  // ENABLE_SLEEP

// ─── capacitive touch ─────────────────────────────────────────────────────────
// Polls the CST816 over I2C; a finger on the panel reads as a button press.
static bool g_touch_ok = false;

// Touch runs on Wire (I2C port 0). The camera installs its own SCCB driver on
// I2C port 1 (its default when pin_sccb_sda is a real pin), so port 0 is free.
static void touch_init() {
    Wire.begin(TP_SDA, TP_SCL, 400000);
    pinMode(TP_INT, INPUT);
    delay(5);
    Wire.beginTransmission(TP_ADDR);
    if (Wire.endTransmission() == 0) {
        g_touch_ok = true;
        // DisAutoSleep (reg 0xFE)=1 keeps the controller responsive to polling.
        Wire.beginTransmission(TP_ADDR);
        Wire.write(0xFE);
        Wire.write(0x01);
        Wire.endTransmission();
        Serial.println("touch (CST816) ready");
    } else {
        Serial.println("touch not detected — GPIO0/BOOT still works as the button");
    }
}

// True while a finger is on the panel. Reads the CST816 finger-count register
// (0x02, low nibble); returns false on any I2C error so it degrades gracefully.
static bool touch_pressed() {
    if (!g_touch_ok) return false;
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0x02);                        // FingerNum register
    if (Wire.endTransmission(false) != 0) return false;   // repeated start
    if (Wire.requestFrom(TP_ADDR, 1) != 1) return false;
    return (Wire.read() & 0x0F) > 0;         // low nibble = touch-point count
}

// ─── button helpers ───────────────────────────────────────────────────────────

// GPIO0 (BOOT button) OR a screen touch both count as "the button".
static bool button_pressed() {
    return (digitalRead(PIN_BUTTON) == LOW) || touch_pressed();
}

// Block until button is fully released, then return.
static void wait_release() {
    while (button_pressed()) delay(5);
}

// Non-blocking check with debounce. Returns true once on a clean press.
// Call repeatedly in a loop.
static bool poll_button() {
    if (!button_pressed()) return false;
    delay(30);
    if (!button_pressed()) return false;
    wait_release();
    return true;
}

// Distinguish a short tap from a long hold. Blocks for the duration of the
// press, then returns once the button is released (or once the long-press
// threshold is crossed). Call repeatedly in a loop.
#define LONG_PRESS_MS  800
static Press poll_press() {   // Press enum is defined up in the state section
    if (!button_pressed()) return Press::NONE;
    delay(30);
    if (!button_pressed()) return Press::NONE;   // debounce bounce/noise
    unsigned long t0 = millis();
    while (button_pressed()) {
        if (millis() - t0 >= LONG_PRESS_MS) { wait_release(); return Press::LONG; }
        delay(5);
    }
    return Press::SHORT;
}

// ─── camera init ──────────────────────────────────────────────────────────────
static bool camera_init() {
    camera_config_t cfg = {};
    cfg.pin_pwdn     = CAM_PWDN;
    cfg.pin_reset    = CAM_RESET;
    cfg.pin_xclk     = CAM_XCLK;
    cfg.pin_sccb_sda = CAM_SIOD;
    cfg.pin_sccb_scl = CAM_SIOC;
    cfg.pin_d7=CAM_D7; cfg.pin_d6=CAM_D6; cfg.pin_d5=CAM_D5; cfg.pin_d4=CAM_D4;
    cfg.pin_d3=CAM_D3; cfg.pin_d2=CAM_D2; cfg.pin_d1=CAM_D1; cfg.pin_d0=CAM_D0;
    cfg.pin_vsync    = CAM_VSYNC;
    cfg.pin_href     = CAM_HREF;
    cfg.pin_pclk     = CAM_PCLK;

    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;

    // RGB565 direct capture (not JPEG). The OV2640 on this board does not produce
    // JPEG frames here — esp_camera_fb_get() times out — but RGB565 works. Capture
    // always happens with WiFi off (live view, or the pipeline before the scan),
    // so there is no PSRAM bandwidth contention to avoid.
    cfg.pixel_format = PIXFORMAT_RGB565;
    cfg.frame_size   = FRAMESIZE_QVGA;  // 320×240 matches display
    cfg.jpeg_quality = 0;               // unused in RGB565 mode
    cfg.fb_count     = 2;               // double-buffer: camera fills one while we read the other
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;  // wait for a fresh frame to be ready

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }
    // DIAGNOSTIC: report the detected sensor so we know SCCB really works.
    sensor_t* s = esp_camera_sensor_get();
    if (s) Serial.printf("camera sensor: PID=0x%02X VER=0x%02X MIDH=0x%02X MIDL=0x%02X\n",
                         s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);
    else   Serial.println("camera init OK but sensor_get() == NULL");

    // Sensor tuning. The OV2640 boots uncalibrated: no white balance, no auto
    // exposure → green-starved, oversaturated, posterised colour. These six
    // calls (matching the old working firmware) turn on AWB + auto-exposure and
    // tame saturation, which is what actually fixes the colour — the RGB565
    // decode was already correct.
    if (s) {
        s->set_brightness(s,     1);
        s->set_contrast(s,       0);
        s->set_saturation(s,     1);
        s->set_whitebal(s,       1);   // white balance on
        s->set_awb_gain(s,       1);   // auto white-balance gain on
        s->set_exposure_ctrl(s,  1);   // auto exposure on
        s->set_gain_ctrl(s,      1);   // auto gain on
    }
    return true;
}

// ─── blit one decoded RGB888 frame to display (no rings, live view) ───────────
// src must be IMG_W×IMG_H RGB888 in PSRAM.
static void blit_frame(const Pixel* src) {
    for (int y = 0; y < IMG_H; y++) {
        rings_to_rgb565_line(&src[y * IMG_W], &g_frame565[y * IMG_W], IMG_W);
    }
    tft->draw16bitRGBBitmap(0, 0, g_frame565, IMG_W, IMG_H);
}

// ─── grab one JPEG frame, decode to dst, return false on failure ──────────────
static bool grab_frame(Pixel* dst) {
    static int dbg = 0;                       // DIAGNOSTIC: log first few grabs only
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        if (dbg++ < 8) Serial.println("grab: esp_camera_fb_get() == NULL (no frame arriving)");
        return false;
    }
    bool ok = fmt2rgb888(fb->buf, fb->len, fb->format, (uint8_t*)dst);
    if (dbg < 8) {
        Serial.printf("grab: fb len=%u fmt=%d %ux%u decode=%s\n",
                      (unsigned)fb->len, (int)fb->format,
                      (unsigned)fb->width, (unsigned)fb->height, ok ? "OK" : "FAIL");
        dbg++;
    }
    esp_camera_fb_return(fb);

    // fmt2rgb888() emits RGB565 as B,G,R bytes (it targets BMP, which is BGR).
    // Our pipeline treats g_src as true RGB888 (Pixel{r,g,b}), so swap R<->B
    // back to RGB here — otherwise warm scenes render blue/purple.
    if (ok) {
        uint8_t* p = (uint8_t*)dst;
        for (int i = 0; i < IMG_PIXELS; i++, p += 3) {
            uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
        }
    }
    return ok;
}

// ─── wifi scan ────────────────────────────────────────────────────────────────
static void do_wifi_scan() {
    // The first scan after a cold radio start often returns a negative error
    // (WIFI_SCAN_FAILED = -2) or 0 — that was the "failed first scan". Retry a
    // few times, clearing stale scan state between attempts, until it succeeds.
    int found = WiFi.scanNetworks(false, true);
    for (int attempt = 0; found <= 0 && attempt < 4; attempt++) {
        Serial.printf("scan attempt %d returned %d — retrying\n", attempt + 1, found);
        WiFi.scanDelete();
        delay(200);
        found = WiFi.scanNetworks(false, true);
    }
    if (found < 0) found = 0;
    g_n_nets = 0;
    for (int i = 0; i < found && g_n_nets < MAX_NETWORKS; i++) {
        strncpy(g_nets[g_n_nets].ssid, WiFi.SSID(i).c_str(), MAX_SSID_LEN - 1);
        g_nets[g_n_nets].ssid[MAX_SSID_LEN - 1] = '\0';
        g_nets[g_n_nets].dbm     = (int8_t)WiFi.RSSI(i);
        g_nets[g_n_nets].channel = (uint8_t)WiFi.channel(i);
        const uint8_t* bssid = WiFi.BSSID(i);   // 6 bytes, valid for this index
        if (bssid) memcpy(g_nets[g_n_nets].bssid, bssid, 6);
        else       memset(g_nets[g_n_nets].bssid, 0, 6);
        Serial.printf("  %s  %02X:%02X:%02X:%02X:%02X:%02X  %d dBm  ch%d\n",
            g_nets[g_n_nets].ssid,
            g_nets[g_n_nets].bssid[0], g_nets[g_n_nets].bssid[1],
            g_nets[g_n_nets].bssid[2], g_nets[g_n_nets].bssid[3],
            g_nets[g_n_nets].bssid[4], g_nets[g_n_nets].bssid[5],
            g_nets[g_n_nets].dbm, g_nets[g_n_nets].channel);
        g_n_nets++;
    }
    rings_sort_networks(g_nets, g_n_nets);
    Serial.printf("found %d networks\n", g_n_nets);
}

// ─── storage: SD + persistent shot counter ─────────────────────────────────────

static void storage_init() {
    g_prefs.begin("beacon", false);     // RW namespace for the shot counter
    // formatOnFail = true: first boot on a fresh partition gets formatted once.
    if (LittleFS.begin(true)) {
        g_fs_ok = true;
        Serial.printf("LittleFS ready: %u KB used / %u KB total\n",
                      (unsigned)(LittleFS.usedBytes() / 1024),
                      (unsigned)(LittleFS.totalBytes() / 1024));
    } else {
        g_fs_ok = false;
        Serial.println("LittleFS mount failed — saves skipped.");
    }
}

// Monotonic shot index persisted in NVS — survives reboots and deep sleep.
static uint32_t next_seq() {
    uint32_t s = g_prefs.getUInt("seq", 0);
    g_prefs.putUInt("seq", s + 1);
    return s;
}

static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// CRC-32 (PNG chunks) and Adler-32 (zlib) — bitwise, no tables, no deps.
static uint32_t crc32_update(uint32_t crc, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc;
}
static uint32_t adler32_update(uint32_t adler, const uint8_t* d, size_t n) {
    uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}
static void png_chunk(File& f, const char* type, const uint8_t* data, uint32_t len) {
    uint8_t lt[4]; put_be32(lt, len); f.write(lt, 4);
    uint32_t crc = 0xFFFFFFFFu;
    f.write((const uint8_t*)type, 4); crc = crc32_update(crc, (const uint8_t*)type, 4);
    if (len) { f.write(data, len); crc = crc32_update(crc, data, len); }
    uint8_t cb[4]; put_be32(cb, crc ^ 0xFFFFFFFFu); f.write(cb, 4);
}

// Write img as a lossless 24-bit PNG (RGB, top-down) — preserves the LSB payload.
// Uses zlib "stored" (uncompressed) deflate blocks, so a valid PNG is produced
// with NO compression library. The host decoders read the pixel LSBs byte-exact.
// On success fills out_path. Returns false if no FS / no PSRAM.
static bool save_png(const Pixel* img, char* out_path, size_t path_len) {
    if (!g_fs_ok) return false;

    const uint32_t seq = next_seq();
    snprintf(out_path, path_len, "/beacon_%04u.png", (unsigned)seq);

    const uint32_t rowlen = 1u + (uint32_t)IMG_W * 3;       // filter byte + RGB
    const uint32_t rawlen = rowlen * (uint32_t)IMG_H;

    uint8_t* raw = (uint8_t*)heap_caps_malloc(rawlen, MALLOC_CAP_SPIRAM);
    if (!raw) { Serial.println("png: PSRAM alloc failed"); return false; }

    uint32_t k = 0;                                         // filtered scanlines, top-down
    for (int y = 0; y < IMG_H; y++) {
        raw[k++] = 0;                                       // filter: none
        const Pixel* s = &img[y * IMG_W];
        for (int x = 0; x < IMG_W; x++) { raw[k++] = s[x].r; raw[k++] = s[x].g; raw[k++] = s[x].b; }
    }

    File f = LittleFS.open(out_path, "w");
    if (!f) { heap_caps_free(raw); Serial.printf("open %s failed\n", out_path); return false; }

    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    f.write(sig, 8);

    uint8_t ihdr[13];
    put_be32(ihdr,     (uint32_t)IMG_W);
    put_be32(ihdr + 4, (uint32_t)IMG_H);
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;   // 8-bit, RGB, no interlace
    png_chunk(f, "IHDR", ihdr, 13);

    // IDAT = zlib header (0x78 0x01) + stored deflate blocks + adler32 trailer
    const uint32_t nblocks = (rawlen + 65534u) / 65535u;
    const uint32_t idatlen = 2u + nblocks * 5u + rawlen + 4u;
    uint8_t lt[4]; put_be32(lt, idatlen); f.write(lt, 4);
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t typ[4] = {'I','D','A','T'};
    f.write(typ, 4); crc = crc32_update(crc, typ, 4);
    const uint8_t zh[2] = {0x78, 0x01}; f.write(zh, 2); crc = crc32_update(crc, zh, 2);

    uint32_t adler = 1u, pos = 0;
    while (pos < rawlen) {
        uint32_t blk = rawlen - pos; if (blk > 65535u) blk = 65535u;
        const uint16_t L = (uint16_t)blk, N = (uint16_t)~L;
        uint8_t bh[5] = { (uint8_t)((pos + blk >= rawlen) ? 1 : 0),  // BFINAL on last, BTYPE=stored
                          (uint8_t)(L & 0xFF), (uint8_t)(L >> 8),
                          (uint8_t)(N & 0xFF), (uint8_t)(N >> 8) };
        f.write(bh, 5);          crc = crc32_update(crc, bh, 5);
        f.write(raw + pos, blk); crc = crc32_update(crc, raw + pos, blk);
        adler = adler32_update(adler, raw + pos, blk);
        pos += blk;
    }
    uint8_t ad[4]; put_be32(ad, adler); f.write(ad, 4); crc = crc32_update(crc, ad, 4);
    uint8_t cb[4]; put_be32(cb, crc ^ 0xFFFFFFFFu); f.write(cb, 4);   // IDAT CRC

    png_chunk(f, "IEND", nullptr, 0);

    f.close();
    heap_caps_free(raw);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LIVE VIEW
//  Streams camera to display. Returns when button is pressed.
//  Manages light sleep (10s) and deep sleep (60s) on idle.
// ─────────────────────────────────────────────────────────────────────────────
static void run_live_view() {
    Serial.println("live view");
    unsigned long last_activity = millis();
    (void)last_activity;

    while (true) {
        // ── grab + blit frame ────────────────────────────────────────────────
        if (grab_frame(g_src)) {
            blit_frame(g_src);
        }

        // ── button check ─────────────────────────────────────────────────────
        if (poll_button()) {
            return;  // caller will run pipeline
        }

#if ENABLE_SLEEP
        // ── idle timers ──────────────────────────────────────────────────────
        unsigned long idle = millis() - last_activity;

        if (idle >= IDLE_DEEP_SLEEP_MS) {
            enter_deep_sleep();  // never returns
        }

        if (idle >= IDLE_LIGHT_SLEEP_MS) {
            enter_light_sleep();  // returns on button press
            // After light sleep wake: debounce, wait release, then back to live view
            delay(30);
            wait_release();
            last_activity = millis();  // reset timer after wake
        }
#endif
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PIPELINE
//  Fresh capture → WiFi scan → encode → blit result.
//  Progress bar advances through 4 stages.
// ─────────────────────────────────────────────────────────────────────────────
static bool run_pipeline() {
    unsigned long t0 = millis();
    Serial.println("pipeline start");

    // ── 1. Fresh dedicated capture ───────────────────────────────────────────
    // Bump JPEG quality to 12 (best) for the pipeline capture, then restore
    // to 63 (fastest) so live view resumes at full speed after result view.
    sensor_t* s = esp_camera_sensor_get();
    if (s) s->set_quality(s, 12);

    delay(CAPTURE_SETTLE_MS);
    progress_bar(1);
    if (!grab_frame(g_src)) {
        if (s) s->set_quality(s, 63);
        Serial.println("capture failed — back to live view");
        return false;
    }
    if (s) s->set_quality(s, 63);  // restore live-view quality immediately
    Serial.printf("[%lu ms] capture done\n", millis() - t0);

    // Show the frozen capture immediately — holds on screen during WiFi scan.
    blit_frame(g_src);
    progress_bar(1);        // bar drawn over image
    overlay_text("scanning wifi...");  // visible for the full 2-4s scan

    // ── 2. WiFi scan ─────────────────────────────────────────────────────────
    // Camera framebuf already released inside grab_frame. Safe to start WiFi.
    progress_bar(2);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    do_wifi_scan();
    WiFi.scanDelete();
    // Tear down through the Arduino WiFi state machine (NOT raw esp_wifi_stop):
    // esp_wifi_stop() stopped the radio behind the Arduino WiFi class's back, so
    // the next run's WiFi.mode(STA) thought it was already started and never
    // restarted it — every scan after the first came back empty (no rings).
    WiFi.mode(WIFI_OFF);
    delay(50);
    Serial.printf("[%lu ms] scan done\n", millis() - t0);

    // ── 3. Encode rings (artistic layer) + embed data layer ───────────────────
    progress_bar(3);
    if (g_n_nets == 0) {
        memcpy(g_dst, g_src, IMG_PIXELS * sizeof(Pixel));
    } else {
        rings_encode(g_src, g_dst, g_nets, g_n_nets, &g_cfg);
    }

    // Embed the recoverable payload into the LSBs *after* rings_encode, so the
    // pixel-sort can never overwrite it. Runs even for 0 networks (empty frame).
    int plen = data_build_payload(g_nets, g_n_nets, g_payload, sizeof(g_payload));
    if (plen > 0) {
        if (data_embed_lsb(g_dst, IMG_PIXELS, g_payload, plen) < 0) {
            Serial.println("payload too large to embed");
        } else {
            // On-device sanity check — read it straight back out of g_dst.
            static uint8_t verify[WBCN_MAX_FRAME];
            int vlen = data_extract_lsb(g_dst, IMG_PIXELS, verify, sizeof(verify));
            Serial.printf("embed %d B, self-extract %s\n",
                          plen, (vlen == plen) ? "OK" : "FAILED");
        }
    }
    Serial.printf("[%lu ms] encode + embed done\n", millis() - t0);

    // ── 4. Save lossless PNG (JPEG would destroy the LSB payload) ──────────────
    progress_bar(4);
    char path[24];
    if (save_png(g_dst, path, sizeof(path)))
        Serial.printf("[%lu ms] saved %s\n", millis() - t0, path);
    else
        Serial.println("save skipped (no filesystem)");

    // ── 5. Blit result ───────────────────────────────────────────────────────
    progress_bar(5);
    blit_frame(g_dst);
    // Redraw full bar over image (blit overwrites it)
    progress_bar(PROGRESS_STAGES);
    Serial.printf("[%lu ms] total\n", millis() - t0);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RESULT VIEW
//  Holds encoded image on screen.
//  Short press → return to live view.  Long press (≥800ms) → SHARE mode.
//  10s idle → light sleep. 60s idle → deep sleep.
// ─────────────────────────────────────────────────────────────────────────────
static void run_result_view() {
    Serial.println("result view (tap = live view, hold = share/QR)");
    unsigned long last_activity = millis();
    (void)last_activity;

    while (true) {
        // ── button: tap → live view, hold → share mode ────────────────────────
        Press p = poll_press();
        if (p == Press::SHORT) {
            return;                       // back to live view
        }
        if (p == Press::LONG) {
            run_share_mode(tft, PIN_BUTTON);   // AP + QR + gallery; blocks until press
            return;                       // exit share → live view
        }

#if ENABLE_SLEEP
        unsigned long idle = millis() - last_activity;

        if (idle >= IDLE_DEEP_SLEEP_MS) {
            enter_deep_sleep();  // never returns
        }

        if (idle >= IDLE_LIGHT_SLEEP_MS) {
            enter_light_sleep();  // returns on button press
            delay(30);
            wait_release();
            // Button press in result view → go back to live view
            return;
        }
#endif

        delay(10);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup — runs on every boot (including deep sleep wake)
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n\n=== wifi_rings boot ===");

    // ── display ──────────────────────────────────────────────────────────────
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    if (!tft->begin()) {
        Serial.println("display init failed");
        while (true) delay(1000);
    }
    tft->fillScreen(COLOR_BLACK);

    // ── button ───────────────────────────────────────────────────────────────
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // ── touch (stand-in button until a GPIO0 button is soldered) ──────────────
    touch_init();

    // ── PSRAM check ──────────────────────────────────────────────────────────
    Serial.printf("PSRAM: %d bytes total, %d bytes free\n",
                  ESP.getPsramSize(), ESP.getFreePsram());
    if (ESP.getPsramSize() == 0)
        fatal("no PSRAM — set Tools > PSRAM > OPI PSRAM");

    // ── allocate PSRAM framebuffers ───────────────────────────────────────────
    g_src = (Pixel*)heap_caps_malloc(IMG_PIXELS * sizeof(Pixel), MALLOC_CAP_SPIRAM);
    g_dst = (Pixel*)heap_caps_malloc(IMG_PIXELS * sizeof(Pixel), MALLOC_CAP_SPIRAM);
    g_frame565 = (uint16_t*)heap_caps_malloc(IMG_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_src || !g_dst || !g_frame565) fatal("PSRAM alloc failed");

    // ── trig LUT ─────────────────────────────────────────────────────────────
    rings_init();

    // ── camera ───────────────────────────────────────────────────────────────
    if (!camera_init()) fatal("camera init failed");

    // ── storage ──────────────────────────────────────────────────────────────
    // Non-fatal: if the filesystem won't mount, the device still runs — it just
    // skips the save step in the pipeline.
    storage_init();

    // ── deep sleep wake config ────────────────────────────────────────────────
    // EXT0 wakes on GPIO0 LOW (button press). Used for deep sleep only.
    // Light sleep uses gpio_wakeup instead (configured per-sleep in helper).
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);

    Serial.printf("free heap: %d  free PSRAM: %d\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.println("=== ready ===");
}

// ─────────────────────────────────────────────────────────────────────────────
//  loop — state machine, runs after every boot (deep sleep wake = fresh boot)
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // Always start in LIVE VIEW — no edge cases from sleeping in result view
    g_state = AppState::LIVE_VIEW;

    while (true) {
        switch (g_state) {

            case AppState::LIVE_VIEW:
                run_live_view();         // blocks until button press
                g_state = AppState::PIPELINE;
                break;

            case AppState::PIPELINE:
                if (run_pipeline()) {    // returns true on success
                    g_state = AppState::RESULT_VIEW;
                } else {
                    g_state = AppState::LIVE_VIEW;  // capture failed, retry
                }
                break;

            case AppState::RESULT_VIEW:
                run_result_view();       // blocks until button press or sleep
                g_state = AppState::LIVE_VIEW;
                break;
        }
    }
}
