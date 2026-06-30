# BEACON

**A one-button camera that paints the invisible Wi-Fi around you into the photo it takes — and hides the real scan data inside the same image so it can be read back later.**

Point it, tap it. It freezes a frame, scans every Wi-Fi network in range, and warps the photo into concentric rings of radial pixel-sort — one ring per network, the pattern driven by each network's own identity, the smear driven by its signal strength. The exact scan (SSID, MAC, channel, RSSI) is steganographically embedded in the pixels, so a companion web tool can recover the network list from a saved image. Hold the button and the device becomes a Wi-Fi hotspot serving a little OSINT gallery of everything it has captured.

It runs entirely on one ESP32-S3 board — camera, display, touch, Wi-Fi, storage, web server — no phone app, no cloud.

> **New here? Read [docs/ENCODING.md](docs/ENCODING.md)** for the step-by-step visual walkthrough of how a Wi-Fi scan becomes the rings.

---

## what / why / how

**What it is.** A self-contained device that treats a Wi-Fi scan as both *art* and *data*. Every capture carries the same scan twice: once as a one-way visualisation (the rings — beautiful, not decodable), and once as an exact, recoverable payload hidden in the pixel LSBs.

**Why.** Wi-Fi is an invisible fingerprint of a place — the networks, their vendors, their signal strengths are a kind of ambient portrait of where you're standing. BEACON makes that fingerprint visible *and* keeps it honest: the picture isn't a random filter, it's a direct readout of the radio environment, and the ground truth travels inside the file.

**How.** A fixed pipeline runs on one button press:

```
freeze frame → Wi-Fi scan → encode rings (art) → embed scan in LSBs (data) → save lossless PNG → show result
```

Nothing in the pipeline runs concurrently, so there's no PSRAM-bandwidth contention between the camera, the radio, and the encoder.

---

## what to buy & build

| part | notes |
|------|-------|
| **Waveshare ESP32-S3-Touch-LCD-2** | the whole computer: ESP32-S3R8, 8 MB OPI PSRAM, 16 MB flash, a 240×320 ST7789T3 display, CST816 capacitive touch, and a 24-pin camera connector. This is the only board you need. |
| **OV2640 camera module** | the 24-pin FPC camera that mates with the board's connector. OV5640 also works. This is what the device "sees." |
| **USB-C cable** | power + flashing + serial. |
| *(optional)* microSD card | the slot is wired but unused today — a future capacity upgrade (see [hardware reference](#hardware-reference)). |
| *(optional)* tactile button on GPIO0 | not required — a **finger tap on the screen already acts as the button**. Solder one to GPIO0/BOOT if you want a physical click. |
| *(coming)* 3D-printed enclosure | print files will be added to this repo. |

**Assembly:** seat the camera ribbon in the 24-pin connector, plug in USB-C, flash. That's it.

---

## flashing it

The firmware is an Arduino sketch. The toolchain is **pinned** — using newer versions will break the build, so match these exactly.

### one-time setup

1. Install **Arduino IDE 2.x**.
2. In Boards Manager, install **esp32 by Espressif `3.1.3`** — **not** the latest.
   *Core 3.3.x changed `spiFrequencyToClockDiv()` to take a `spi_t*`, which breaks the display library. 3.1.3 has the signature it expects.*
3. In Library Manager, install **GFX Library for Arduino `1.5.4`**.
4. **Do not install any "QRCode" library.** The QR encoder used here is the one **bundled inside the esp32 core** (`<qrcode.h>` → `esp_qrcode_*`). Richard Moore's "QRCode" library has the same header name and will collide.
5. Connect the board over USB-C.

`WiFi`, `WebServer`, `DNSServer`, `LittleFS`, `Preferences`, and the camera + QR components all ship with the esp32 core — nothing else to install.

### IDE settings

Open the **`wifi_rings_esp32s3/`** folder as the sketch (the `.ino` plus all its `.c/.h/.cpp` live together, as Arduino requires).

```
Board:            ESP32S3 Dev Module
USB CDC On Boot:  Enabled        ← required for Serial over native USB
PSRAM:            OPI PSRAM      ← critical, must match the R8 chip
Flash Size:       16MB
Partition Scheme: Custom         ← uses wifi_rings_esp32s3/partitions.csv (~12.8 MB LittleFS, ~55 images)
CPU Frequency:    240MHz
Upload Speed:     921600         ← drop to 460800 if uploads are flaky
```

Verify, then Upload. Open Serial Monitor at **115200**. A healthy first boot prints a banner, `PSRAM: 8388608 bytes total`, `LittleFS ready: … KB total`, and `=== ready ===`, then enters live view.

### command-line flashing (what this repo is developed with)

```bash
CLI="<arduino-ide>/resources/app/lib/backend/resources/arduino-cli.exe"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,FlashSize=16M,CDCOnBoot=cdc,CPUFreq=240,UploadSpeed=921600"
"$CLI" compile --fqbn "$FQBN" wifi_rings_esp32s3
"$CLI" upload  --fqbn "$FQBN" --port COM7 wifi_rings_esp32s3
```

### if upload fails

Hold **BOOT**, tap **RESET**, release **BOOT**, upload again. If the port keeps dropping, lower the upload speed.

---

## using it

One button (a screen tap *or* GPIO0) drives three states. Press = short tap, hold = ≥ 0.8 s.

**LIVE VIEW** — the camera streams to the display as a viewfinder.
- **tap** → run the capture pipeline
- **hold** → SHARE mode (the gallery)

**RESULT VIEW** — the finished, ringed image is held on screen.
- **tap** → back to live view
- **hold** → SHARE mode

**SHARE mode** — the device brings up an open Wi-Fi AP (`beacon-XXXX`) and shows **two QR codes**. See [getting images off the device](#getting-images-off-the-device).
- **hold** → exit back to live view

While the pipeline runs, a status label cycles through `capturing → scanning wifi → encoding rings → saving`, and a progress bar tracks the stages. Both vanish the instant the finished image appears.

Captures are saved to internal flash as **`beacon_<seq>_<YYYYMMDD_HHMMSS>.png`** — a monotonic sequence number (the unique, sortable key) plus a timestamp. *The clock is anchored to firmware build time (the board has no RTC and never joins a network for time), so the date/time is approximate and resets on power-up; the sequence number is the source of truth for ordering.*

> The device naps automatically to save power: after 45 s idle the backlight and camera power down and the CPU light-sleeps (a few mA). A **double-tap** wakes it back to where it was (live view, or the held result image) — a single stray touch just re-naps. It never naps while USB-tethered, since light sleep would suspend the USB connection and break re-flashing.

---

## how the encoding works

Short version below — **the full visual, step-by-step walkthrough is in [docs/ENCODING.md](docs/ENCODING.md)**, with images generated from the real encoder.

Each capture carries the Wi-Fi scan in **two independent layers**:

### 1. the artistic layer — `wifi_rings.c` (one-way)

Concentric rings warp the frozen photo. One ring per network, **innermost = strongest signal**.

- **The pattern is a direct readout of identity, not a hash.** Each network's `SSID + BSSID + channel` is concatenated, and every byte (0–255) is split into **two halves, each a value 0–15** (e.g. the letter `F` = value 70 = 4×16+6 → halves 4 and 6). Each half sets the **sort depth of one radial spoke**, tiled around the ring. Equal bytes always make equal-looking spokes — a "truth" you can eyeball — without being machine-readable.
- **Signal strength sets the reach.** Stronger networks sort pixels deeper into their band (`reach = (dbm-window) × band width`). A spoke is a *radial pixel-sort*: it gathers pixels along a ray and reorders them by luminance, smearing the photo outward.

This is a deliberately **one-way** visualisation — you cannot reconstruct the scan from the rings.

### 2. the recoverable layer — `wifi_data.c` (exact, hidden)

The *exact* scan is hidden in the **pixel least-significant-bits** as a framed, CRC-checked payload:

```
magic 'WBCN' | version | n_nets | per-net{ ssid_len, ssid, bssid[6], rssi, channel } | crc16
```

A ±1 nudge to the low bit of each colour channel is invisible against the rings but byte-exact to a decoder.

**Ordering is what keeps the two layers from fighting:** the LSB embed runs **after** `rings_encode()`, so the pixel-sort can never overwrite the payload.

```
rings_encode(src → dst)        artistic layer, baked first
        ↓
data_embed_lsb(dst, payload)   hidden layer written last — rings can't touch it
        ↓
save_png(dst)                  lossless PNG (zlib "stored" blocks — no compression lib)
```

**The one hard rule: the image must stay lossless.** JPEG (or any lossy re-encode) throws away exactly the bits the payload lives in. The device saves PNG; never re-export as JPEG.

---

## getting images off the device

### SHARE mode (no cable, no card reader)

Hold the button in live or result view. The device opens an **open AP** and shows two QR codes:

```
            // BEACON SHARE
        ┌──────────┐    ┌──────────┐
        │   QR 1   │    │   QR 2   │
        │ join AP  │    │ open URL │
        └──────────┘    └──────────┘
           1 JOIN          2 OPEN
```

1. **Scan QR 1** → your phone joins the `beacon-XXXX` network.
2. **Scan QR 2 with the Camera app** → opens `192.168.4.1` in **real Safari/Chrome**.
3. Browse the gallery: tap a thumbnail to **preview**, **press & hold an image → Save to Photos**, tick rows to **DELETE** or **DOWNLOAD** (multiple selected → one `.zip`).

> **Why two QRs and why Safari?** iOS's captive-portal mini-browser blocks saving, downloads, and long-press. BEACON answers the OS connectivity probe as "online" so iOS *doesn't* trap you in that mini-browser — you use the real browser, where saving works natively. A `WIFI:` QR and an `http://` QR are different payload types, and you can't load the page until you're on the network, so they're inherently two scans. (First time on iOS you may need to "Forget" the network once so it re-probes.)

The gallery is styled as a small OSINT terminal and shows a live storage gauge.

> **No authentication.** The AP is open and the gallery has no login — by design, so a phone can join and browse with zero setup. The tradeoff: anyone in range while SHARE mode is up can view, download, or delete every saved capture, not just their own. SHARE mode is opt-in (long-press only) and the AP closes the moment you exit it, but don't leave it running unattended somewhere you wouldn't want a stranger rifling through your captures.

### over USB (exact bytes, guaranteed)

Pull the PNGs off LittleFS (e.g. an `esptool read_flash` + `littlefs-python` extraction, or a serial dump) and decode on a laptop:

```bash
pip install pillow numpy
python tools/decode_beacon.py beacon_0001_20260628_145312.png      # table
python tools/decode_beacon.py *.png --json                          # machine-readable
```

### the web decoder — `web/` (the OSINT view)

`web/index.html` is a local, offline **signal-intelligence dashboard**. Drop a captured PNG on it and it reads the hidden payload right in the browser — no upload, nothing leaves the machine — and renders: network table ranked by signal, vendor lookup from the MAC OUI, channel occupancy, 2.4/5 GHz bands, rough distance estimate, and the raw frame bytes. (A WiGLE wardriving-geolocation panel is stubbed for a future online phase.)

```bash
cd web && python -m http.server     # then open http://localhost:8000
```

---

## previewing the encoder without hardware

`wifi_rings.c` is pure C with no Arduino dependencies, so the **exact device encoder** runs on a laptop. `tools/rings_preview.py` compiles it (via `python -m ziglang cc`) into a shared library and runs it over any photo with mock Wi-Fi data — instant visual tuning, no flashing.

```bash
pip install ziglang pillow numpy
python tools/rings_preview.py                              # sample photo + mock networks
python tools/rings_preview.py myphoto.jpg --ring-thickness 30 --max-displace 50
python tools/rings_preview.py --sweep                      # 2×2 grid over all four sort modes
```

This is also what generates the figures in [docs/ENCODING.md](docs/ENCODING.md) — the previews are faithful because they run the same C the device runs.

---

## repo map

```
wifi_rings_esp32s3/            Arduino sketch — open THIS folder in the IDE
  wifi_rings_esp32s3.ino         pipeline, state machine, camera, LittleFS save, clock, sleep
  wifi_rings.h / .c              artistic encoder (one-way), pure C, no Arduino deps
  wifi_data.h / .c               recoverable encoder: framing, CRC-16, LSB embed/extract
  beacon_share.h / .cpp          SHARE mode: AP + two-QR screen + OSINT HTTP gallery
  partitions.csv                 16 MB layout — ~12.8 MB LittleFS (~55 images)
tools/                         host-side dev tools (run with python from repo root)
  rings_preview.py               preview the real encoder on a photo → PNG
  rings_host.c                   C shim binding wifi_rings.c into rings_preview.py
  encoding_steps.py              generate the step-by-step figures for docs/ENCODING.md
  decode_beacon.py               recover the hidden scan from a saved PNG/BMP
  test_data_roundtrip.py         data-layer self-test (no hardware) → prints PASS
  mock_wifi_networks.json        mock scan data for the preview tools
web/                           offline browser OSINT decoder (drop a PNG, read the scan)
docs/
  ENCODING.md                    step-by-step visual explainer of the whole encoding
  PORTFOLIO.md                   project narrative — the tools & ideas this combines
sample-images/                 default photo(s) for the preview tools
FIRST_FLASH.md                 first-flash checklist
PROJECT_STATE.md               living status / bring-up notes
```

---

## hardware reference

### display (ST7789T3, landscape via rotation 1)

```
SCLK 39   MOSI 38   MISO 40   DC 42   CS 45   RST -1   BL 1
```

### camera (Waveshare ESP32-S3-Touch-LCD-2 map)

```
PWDN 17  RESET -1  XCLK 8  SIOD 21  SIOC 16
Y9 2  Y8 7  Y7 10  Y6 14  Y5 11  Y4 15  Y3 13  Y2 12
VSYNC 6  HREF 4  PCLK 9
```

> The data lines map to the OV2640 DVP bus as Y2..Y9, where **Y2 = D0 (LSB)** and **Y9 = D7 (MSB)**. Getting D0..D7 in the wrong order keeps timing/geometry perfect but **scrambles every colour** — a bit-permutation that preserves brightness. That was the original bring-up bug; the map above is correct.

### touch (CST816, I²C0)

```
SDA 48   SCL 47   INT 46   addr 0x15
```

Used as a stand-in button (a tap = a press). The camera's SCCB owns I²C1, so touch deliberately uses I²C0 to avoid a port clash.

### capture vs. save format

- **Capture** (camera → RAM → live view) is **RGB565**. The OV2640 on this board produces no JPEG frames here (`esp_camera_fb_get()` times out), so RGB565 is used for the live source.
- **Save** is always **lossless PNG**, because the recoverable payload lives in the pixel LSBs. Never save lossy.

### microSD (future capacity upgrade — currently unused)

The SD slot is SPI and **shares the LCD's bus** (`IO38 MOSI`, `IO39 SCLK`, `IO40 MISO` shared; `IO41` dedicated CS). It must mount on the *same* SPI instance the display driver uses — do **not** spin up a second `SPIClass` on those pins or you'll steal the routing and break the display. Storage stays on LittleFS until this is wired up; then `save_png()` repoints from `LittleFS` to `SD` (same PNG bytes).

### memory layout

```
internal SRAM (~512 KB)        cos/sin LUT (8 KB, hot path) + DMA line buffers (must be internal)
PSRAM (8 MB OPI)               g_src 230 KB + g_dst 230 KB (RGB888 frames) + camera framebuf
```

**The one rule:** big image buffers → PSRAM (`MALLOC_CAP_SPIRAM`); DMA transfer buffers → internal SRAM (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`).

### timing (240 MHz, OPI PSRAM)

| step | estimate |
|------|----------|
| live view | ~25–30 fps |
| capture + decode | ~60 ms |
| **Wi-Fi scan** | **2–4 s** (hardware, dominates) |
| rings encode | ~50–70 ms |
| LSB embed + self-check | ~5 ms |
| save PNG (~230 KB) | ~0.1–4 s (flash write) |
| **total after press** | **~3–8 s** |

---

## license / status

Personal project, in active bring-up — see [PROJECT_STATE.md](PROJECT_STATE.md) for the current confirmed-working state and open items. Enclosure files and a few docs are still landing.
