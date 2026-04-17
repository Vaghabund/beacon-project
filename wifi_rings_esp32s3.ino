// ─────────────────────────────────────────────────────────────────────────────
//  wifi_rings_esp32s3.ino
//
//  Hardware:
//    ESP32-S3R8 (8MB OPI PSRAM — R8 suffix)
//    OV5640 camera (or OV2640)
//    320×240 SPI display (ST7789 / ILI9341), landscape
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
//    Flash size:       8MB
//    Partition scheme: Huge APP (3MB No OTA/1MB SPIFFS)
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
#include <TFT_eSPI.h>

#include "wifi_rings.h"

// fmt2rgb888 writes raw R,G,B bytes — Pixel must be exactly 3 bytes, no padding
static_assert(sizeof(Pixel) == 3, "Pixel struct must be 3 bytes packed");

// ─── timing ───────────────────────────────────────────────────────────────────
#define IDLE_LIGHT_SLEEP_MS   10000UL   // 10s idle → light sleep
#define IDLE_DEEP_SLEEP_MS    60000UL   // 60s idle → deep sleep
#define CAPTURE_SETTLE_MS     100       // ms to let camera settle before fresh capture

// ─── pin assignments ──────────────────────────────────────────────────────────
// ESP32-S3R8: avoid GPIO 35, 36, 37 (used by OPI PSRAM internally).

#define PIN_BUTTON      0    // BOOT button = GPIO0, active LOW

// OV5640 — adjust to your module wiring
#define CAM_PWDN       -1
#define CAM_RESET      -1
#define CAM_XCLK       10
#define CAM_SIOD       40
#define CAM_SIOC       39
#define CAM_D7         48
#define CAM_D6         11
#define CAM_D5         12
#define CAM_D4         14
#define CAM_D3         16
#define CAM_D2         18
#define CAM_D1         17
#define CAM_D0         15
#define CAM_VSYNC      38
#define CAM_HREF       47
#define CAM_PCLK       13

// ─── globals ──────────────────────────────────────────────────────────────────
TFT_eSPI tft;

// PSRAM framebuffers — allocated once in setup(), never freed
Pixel*    g_src    = nullptr;   // decoded camera RGB888
Pixel*    g_dst    = nullptr;   // encoded output RGB888

// DMA line buffers — ping-pong, must stay in internal SRAM
uint16_t* g_dma_a  = nullptr;
uint16_t* g_dma_b  = nullptr;

// WiFi scan results
WifiNetwork g_nets[MAX_NETWORKS];
int         g_n_nets = 0;

// Encoder config
RingConfig g_cfg = RING_CONFIG_DEFAULT;

// ─── state ────────────────────────────────────────────────────────────────────
enum class AppState { LIVE_VIEW, PIPELINE, RESULT_VIEW };
AppState g_state = AppState::LIVE_VIEW;

// ─── progress bar ─────────────────────────────────────────────────────────────
// 4px bar at bottom edge, 4 pipeline stages (scan, capture, encode, blit)
#define PROGRESS_STAGES  4
#define PROGRESS_Y       (IMG_H - 4)
#define PROGRESS_H       4

static void progress_bar(int stage) {
    const int filled = (IMG_W * stage) / PROGRESS_STAGES;
    tft.fillRect(0,      PROGRESS_Y, filled,         PROGRESS_H, TFT_WHITE);
    tft.fillRect(filled, PROGRESS_Y, IMG_W - filled,  PROGRESS_H, TFT_DARKGREY);
}

// ─── centered text overlay on top of whatever is on screen ───────────────────
// Dark pill behind white text — readable over any image content.
static void overlay_text(const char* msg) {
    const int pad_x = 10, pad_y = 5;
    const int cw = 6, ch = 8;          // TFT_eSPI textSize 1 glyph size
    const int len = strlen(msg);
    const int tw = len * cw;
    const int bw = tw + pad_x * 2;
    const int bh = ch + pad_y * 2;
    const int bx = (IMG_W - bw) / 2;
    const int by = (IMG_H - bh) / 2;
    tft.fillRect(bx, by, bw, bh, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(bx + pad_x, by + pad_y);
    tft.print(msg);
}

// ─── fatal error — halts with red message ─────────────────────────────────────
static void fatal(const char* msg) {
    tft.fillRect(0, 0, IMG_W, 16, TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.print(msg);
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
    tft.startWrite();
    tft.setAddrWindow(0, 0, IMG_W, IMG_H);
    rings_to_rgb565_line(&src[0], g_dma_a, IMG_W);
    for (int y = 0; y < IMG_H; y++) {
        uint16_t* send = (y % 2 == 0) ? g_dma_a : g_dma_b;
        uint16_t* prep = (y % 2 == 0) ? g_dma_b : g_dma_a;
        tft.pushImageDMA(0, y, IMG_W, 1, send);
        if (y + 1 < IMG_H)
            rings_to_rgb565_line(&src[(y + 1) * IMG_W], prep, IMG_W);
        tft.dmaWait();
    }
    tft.endWrite();
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
        Serial.printf("  %s  %d dBm  ch%d\n",
            g_nets[g_n_nets].ssid, g_nets[g_n_nets].dbm, g_nets[g_n_nets].channel);
        g_n_nets++;
    }
    rings_sort_networks(g_nets, g_n_nets);
    Serial.printf("found %d networks\n", g_n_nets);
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

    // ── 3. Encode rings ──────────────────────────────────────────────────────
    progress_bar(3);
    if (g_n_nets == 0) {
        memcpy(g_dst, g_src, IMG_PIXELS * sizeof(Pixel));
    } else {
        rings_encode(g_src, g_dst, g_nets, g_n_nets, &g_cfg);
    }
    Serial.printf("[%lu ms] encode done\n", millis() - t0);

    // ── 4. Blit result ───────────────────────────────────────────────────────
    progress_bar(4);
    blit_frame(g_dst);
    // Redraw full bar over image (blit overwrites it)
    progress_bar(PROGRESS_STAGES);
    Serial.printf("[%lu ms] total\n", millis() - t0);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RESULT VIEW
//  Holds encoded image on screen.
//  Button press → return to live view.
//  10s idle → light sleep. 60s idle → deep sleep.
// ─────────────────────────────────────────────────────────────────────────────
static void run_result_view() {
    Serial.println("result view");
    unsigned long last_activity = millis();

    while (true) {
        // ── button → back to live view ───────────────────────────────────────
        if (poll_button()) {
            return;
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
    tft.init();
    tft.setRotation(1);  // landscape — matches QVGA 320×240 directly
    tft.fillScreen(TFT_BLACK);

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
    if (!g_src || !g_dst) fatal("PSRAM alloc failed");

    // ── allocate DMA line buffers — internal SRAM only ───────────────────────
    g_dma_a = (uint16_t*)heap_caps_malloc(IMG_W * sizeof(uint16_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    g_dma_b = (uint16_t*)heap_caps_malloc(IMG_W * sizeof(uint16_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_dma_a || !g_dma_b) fatal("DMA buf alloc failed");

    // ── trig LUT ─────────────────────────────────────────────────────────────
    rings_init();

    // ── camera ───────────────────────────────────────────────────────────────
    if (!camera_init()) fatal("camera init failed");

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
