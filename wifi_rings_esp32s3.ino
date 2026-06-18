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
//    PSRAM:            OPI PSRAM   ← critical, must match R8 chip
//    Flash size:       16MB
//    Partition scheme: Huge APP (3MB No OTA/1MB SPIFFS) or larger app partition
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

// ─── pin assignments ──────────────────────────────────────────────────────────
// Waveshare ESP32-S3-Touch-LCD-2 built-in display and camera mappings.

#define PIN_BUTTON      0    // BOOT button = GPIO0, active LOW

// Camera mapping from Waveshare's Arduino examples
#define CAM_PWDN       17
#define CAM_RESET      -1
#define CAM_XCLK       8
#define CAM_SIOD       21
#define CAM_SIOC       16
#define CAM_D7         10
#define CAM_D6         14
#define CAM_D5         11
#define CAM_D4         15
#define CAM_D3         13
#define CAM_D2         12
#define CAM_D1         7
#define CAM_D0         2
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

// ─── storage ───────────────────────────────────────────────────────────────────
//  Encoded BMPs are saved to internal flash via LittleFS (see storage_init /
//  save_bmp). No SPI sharing, no risk to the display.
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

// ─── button helpers ───────────────────────────────────────────────────────────

static bool button_pressed() {
    return digitalRead(PIN_BUTTON) == LOW;
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
enum class Press { NONE, SHORT, LONG };
static Press poll_press() {
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

    // JPEG capture — not RGB direct (avoids PSRAM bandwidth corruption with WiFi)
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAMESIZE_QVGA;  // 320×240 matches display
    cfg.jpeg_quality = 63;              // lowest quality = smallest file = fastest live view
                                        // pipeline bumps this to 12 before capture
    cfg.fb_count     = 2;               // double-buffer: camera writes frame N+1 while we decode N
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;  // always return newest frame, never stall

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
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
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return false;
    bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, (uint8_t*)dst);
    esp_camera_fb_return(fb);
    return ok;
}

// ─── wifi scan ────────────────────────────────────────────────────────────────
static void do_wifi_scan() {
    int found = WiFi.scanNetworks(false, true);
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

static inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}
static inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);        p[1] = (uint8_t)((v >> 8)  & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Write img as a 24-bit uncompressed BMP (lossless — preserves the LSB payload).
// BMP is stored bottom-up and BGR; the host decoder reads it back top-down RGB,
// so per-pixel channel values — and their LSBs — are preserved exactly.
// On success fills out_path with the filename written. Returns false if no FS.
static bool save_bmp(const Pixel* img, char* out_path, size_t path_len) {
    if (!g_fs_ok) return false;

    const uint32_t seq = next_seq();
    snprintf(out_path, path_len, "/beacon_%04u.bmp", (unsigned)seq);

    const int      row_raw    = IMG_W * 3;
    const int      pad        = (4 - (row_raw & 3)) & 3;   // rows align to 4 bytes
    const int      row_padded = row_raw + pad;
    const uint32_t img_size   = (uint32_t)row_padded * IMG_H;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(&hdr[2],  54 + img_size);   // file size
    put_le32(&hdr[10], 54);              // pixel data offset
    put_le32(&hdr[14], 40);              // BITMAPINFOHEADER size
    put_le32(&hdr[18], IMG_W);
    put_le32(&hdr[22], IMG_H);           // positive height → bottom-up rows
    put_le16(&hdr[26], 1);               // planes
    put_le16(&hdr[28], 24);              // bits per pixel
    put_le32(&hdr[34], img_size);
    put_le32(&hdr[38], 2835);            // ~72 DPI, x
    put_le32(&hdr[42], 2835);            // ~72 DPI, y

    File f = LittleFS.open(out_path, "w");
    if (!f) { Serial.printf("open %s failed\n", out_path); return false; }
    f.write(hdr, sizeof(hdr));

    uint8_t row[IMG_W * 3 + 4];
    for (int i = row_raw; i < row_padded; i++) row[i] = 0;   // zero pad once
    for (int y = IMG_H - 1; y >= 0; y--) {                   // bottom-up
        const Pixel* s = &img[y * IMG_W];
        for (int x = 0; x < IMG_W; x++) {
            row[x * 3 + 0] = s[x].b;   // BMP byte order is B,G,R
            row[x * 3 + 1] = s[x].g;
            row[x * 3 + 2] = s[x].r;
        }
        f.write(row, row_padded);
    }
    f.close();
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
    bool light_slept = false;

    while (true) {
        // ── grab + blit frame ────────────────────────────────────────────────
        if (grab_frame(g_src)) {
            blit_frame(g_src);
        }

        // ── button check ─────────────────────────────────────────────────────
        if (poll_button()) {
            return;  // caller will run pipeline
        }

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
    esp_wifi_stop();
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

    // ── 4. Save lossless BMP (JPEG would destroy the LSB payload) ──────────────
    progress_bar(4);
    char path[24];
    if (save_bmp(g_dst, path, sizeof(path)))
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
