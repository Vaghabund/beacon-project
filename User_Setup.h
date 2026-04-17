// ─────────────────────────────────────────────────────────────────────────────
//  TFT_eSPI User_Setup.h — wifi_rings ESP32-S3R8
//
//  Copy this file to:
//    ~/Arduino/libraries/TFT_eSPI/User_Setup.h
//  (replace the existing file)
//
//  Display: ST7789 320×240, landscape, SPI @ 80MHz
//  Board:   ESP32-S3R8
//  Avoid GPIO 35, 36, 37 — used by OPI PSRAM internally.
// ─────────────────────────────────────────────────────────────────────────────

#define ST7789_DRIVER

#define TFT_WIDTH  320
#define TFT_HEIGHT 240

// ── SPI pins — adjust to your wiring ─────────────────────────────────────────
#define TFT_MOSI    11
#define TFT_SCLK    12
#define TFT_CS      10
#define TFT_DC       9
#define TFT_RST      8
#define TFT_MISO    -1   // not used

// ── SPI frequency ─────────────────────────────────────────────────────────────
// 80MHz is the maximum the ESP32-S3 SPI peripheral supports and within
// ST7789 / ILI9341 spec. Halves blit time vs 40MHz (~7ms vs ~15ms per frame).
#define SPI_FREQUENCY       80000000
#define SPI_READ_FREQUENCY  20000000

// ── DMA ───────────────────────────────────────────────────────────────────────
// Required for pushImageDMA / dmaWait used in the ping-pong blit.
#define ESP32_DMA

// ── fonts — only what we use ──────────────────────────────────────────────────
#define LOAD_GLCD    // Font 1 — used by overlay_text() and fatal()
#undef LOAD_FONT2
#undef LOAD_FONT4
#undef LOAD_FONT6
#undef LOAD_FONT7
#undef LOAD_FONT8
#undef LOAD_GFXFF
#undef SMOOTH_FONT
