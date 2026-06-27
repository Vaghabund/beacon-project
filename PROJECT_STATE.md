# Project State — wifi_rings (ESP32-S3 beacon)

_Last updated: 2026-06-21 — laptop bring-up session._
_Branch: `fix/laptop-bringup-touch-camera`._

## TL;DR

The firmware builds, flashes, and runs on the Waveshare ESP32-S3-Touch-LCD-2.
Live camera (correct colour), touch-as-button, the capture pipeline, repeatable
WiFi scans, and lossless **PNG** save all work. Camera colour, the failed
first-tap, and the empty-rescan bugs are fixed (see below).

## Toolchain (what actually builds it)

- Arduino IDE's **bundled arduino-cli** (no separate install).
- **esp32 core `3.1.3`** — pinned, **not** latest. Core 3.3.x changed
  `spiFrequencyToClockDiv()` to take a `spi_t*`, which breaks
  "GFX Library for Arduino" 1.5.4. 3.1.3 has the compatible one-arg signature.
- Libraries: **GFX Library for Arduino 1.5.4**. The QR code uses the core's
  **bundled ESP-IDF encoder** (`<qrcode.h>` → `esp_qrcode_*`).
  Do **NOT** install Richard Moore's "QRCode" library — its header is also
  `qrcode.h` and collides with the bundled one.
- FQBN:
  `esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,FlashSize=16M,CDCOnBoot=cdc,CPUFreq=240,UploadSpeed=921600`
  - `PSRAM=opi` + `FlashSize=16M` are required (override the Disabled/4MB
    defaults), or the board hits the no-PSRAM fatal / partition overflow.
  - `PartitionScheme=custom` picks up `wifi_rings_esp32s3/partitions.csv`.

## Confirmed working on hardware ✅

- Build + flash green; PSRAM 8 MB detected, LittleFS ~12.8 MB mounted.
- **Touch = button**: CST816D on **I2C0** (GPIO47=SCL, GPIO48=SDA, addr 0x15),
  OR'd with GPIO0 so a soldered BOOT button still works later. (Camera SCCB
  owns I2C1, so touch deliberately uses I2C0 to avoid a port clash.)
- **No-sleep dev mode** (`ENABLE_SLEEP 0`): stays awake, backlight on, USB CDC
  stays connected — makes re-flashing reliable. Set to `1` for production power
  saving. The deep-sleep behaviour caused recurring trouble; keep it off in dev.
- **Camera capture** (RGB565, correct colour) + full pipeline: WiFi scan →
  rings encode → LSB data embed → lossless PNG save.

## Fixed this session ✅

- **Camera colour** — the root cause was the `CAM_D0..D7` data-line `#define`s
  being assigned to the wrong GPIOs (a scrambled copy of the Waveshare schematic
  in README "camera mapping"). Every timing/control pin was right, so geometry +
  brightness were perfect but colour was a bit-permutation (looked like a byte
  order / white-balance problem — it wasn't). Diagnosed with the OV2640 internal
  colour-bar: crisp bars + scrambled colour + white-stays-white = data-line
  permutation. Fixed the eight defines to match the schematic. Also added the
  sensor tuning (whitebal/awb_gain/exposure/gain) and the fmt2rgb888 B,G,R→R,G,B
  swap.
- **Failed first tap** — a stray debug raw-framebuffer dump (extra `fb_get` +
  150 KB mid-pipeline flash write) was stalling the first capture; removed. Also
  `do_wifi_scan()` now retries the cold-radio first scan (was returning -2/0).
- **Empty rescans (rings only on the first capture)** — the pipeline used the
  raw `esp_wifi_stop()` behind the Arduino WiFi class's back, so the next run's
  `WiFi.mode(STA)` never restarted the radio. Now tears down with
  `WiFi.mode(WIFI_OFF)` so every capture rescans.

## Known issues / TODO (next session)

- **Touch can't exit SHARE mode.** `beacon_share.cpp` polls GPIO0 directly (it's
  a separate translation unit and can't see the touch helper). For now use
  **short taps**; a long-hold enters the QR screen and you'd need GPIO0 to leave.
  Fix: expose the touch check to share mode.
- The onboard **BOOT button (GPIO0) did not respond** when pressed by hand —
  worth checking; touch is the working input for now.
- **Minor diagnostics remain** — `camera sensor: PID=...` in `camera_init()` and
  the throttled `grab: ...` lines in `grab_frame()` (harmless, first 8 grabs).
- **Ring look** — the artistic rings tile the SSID-as-digits around each ring, so
  the motif repeats; the full recoverable data is in the LSB layer, not the rings.
  Possible cosmetic change: stretch the SSID once around the ring instead.
- **Docs are stale**: `FIRST_FLASH.md` step 4 + `README.md` still say install the
  "QRCode" library (no longer used) and "core ≥ 3.x" (should pin 3.1.3).

## Capture format vs save format — the "PNG for a reason"

Two different formats, do not conflate them:

- **Capture format** (camera → RAM → live view): changed **JPEG → RGB565**
  this session, because the OV2640 on this board produced **no JPEG frames**
  (`esp_camera_fb_get()` timed out / returned NULL). RGB565 bypasses the
  sensor's JPEG engine. This is the live source only.
- **Save format = lossless PNG** (`save_png()` in the `.ino`). The recoverable
  WiFi scan is hidden in the **pixel LSBs** (`wifi_data.c`). JPEG — or any lossy
  re-encode — destroys those bits and the entire payload. **Never save or
  re-export the result as JPEG.** See README → "two layers: artwork + recoverable
  data".
- The PNG writer uses zlib **"stored" (uncompressed) deflate blocks**, so it
  needs **no compression library** — keeps the pinned toolchain untouched. Files
  are roughly BMP-sized; swap in miniz/PNGenc later if you want real compression.

The RGB565 capture change does **not** threaten the data layer: the LSB embed
runs after `rings_encode()` and the output is written as a lossless PNG. The web
decoder (`web/decode.js`) and `tools/decode_beacon.py` both read PNG (and BMP).
The one hard rule, if formats are revisited: **the saved image stays lossless**.

## Hardware reference (Waveshare ESP32-S3-Touch-LCD-2)

- Sensor: **OV2640** (PID 0x26). Display: ST7789T3 240×320 (landscape).
  Touch: CST816D.
- Camera pins are the documented Waveshare map (SCCB 21/16, XCLK 8, …) and are
  verified — the sensor answers on SCCB. **Do not** copy pins from the
  `copilot/combine-camera-input-wifi-visualization` branch; that's a different
  board (SIOD=4/SIOC=5). It was only the source of the RGB565 capture idea.
