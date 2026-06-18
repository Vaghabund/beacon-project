# wifi_rings — Waveshare ESP32-S3-Touch-LCD-2 build guide

## what this does

One-button device. Three states:

**LIVE VIEW** — camera streams to display as a lightweight viewfinder.
Press button → pipeline runs. 10s idle → light sleep. 60s idle → deep sleep.

**PIPELINE** — fresh capture → freeze frame on screen → WiFi scan (overlay shown) → encode rings → embed recoverable data → save BMP to flash → show result.
Progress bar advances across bottom of screen.

**RESULT VIEW** — encoded image held on screen.
Tap button → back to live view. **Hold button (≥0.8s) → SHARE mode.**
10s idle → light sleep. 60s idle → deep sleep.

**SHARE mode** — the device opens an open captive-portal Wi-Fi AP and shows a
**Wi-Fi-join QR** on screen. Scan it with a phone → it joins the AP → the captive
portal auto-opens a gallery of all saved images, tap one to download. Press the
button to exit (Wi-Fi off, back to live view).

Deep sleep always wakes into live view — no edge cases.

---

## hardware

| part | notes |
|------|-------|
| Waveshare ESP32-S3-Touch-LCD-2 | ESP32-S3R8, 8MB OPI PSRAM, 16MB flash, built-in ST7789T3 display |
| OV5640 or OV2640 | camera plugs into the board's 24-pin connector |
| built-in ST7789T3 | 240×320 native panel, used in landscape via rotation 1 |
| one button | GPIO0 / BOOT, active LOW, internal pullup |
| internal flash | 16MB — images saved to a LittleFS partition (default storage) |
| microSD card | onboard slot, SPI, **shares the LCD bus** — optional capacity upgrade (see below) |

---

## memory layout

```
internal SRAM (~512KB)
  Arduino + FreeRTOS runtime    ~180KB
  WiFi stack                    ~100KB
  stacks + misc                  ~30KB
  cos/sin LUT (1024 × 4B × 2)     8KB   ← hot path, must be internal
  DMA line buf A (320 × 2B)       640B   ← must be internal (DMA rule)
  DMA line buf B (320 × 2B)       640B   ← must be internal (DMA rule)
  ray_buf (stack, per call)       ~1KB
  ──────────────────────────────────────
  headroom                       ~200KB

PSRAM (8MB OPI)
  g_src  320×240×3 RGB888        230KB   ← decoded camera frame
  g_dst  320×240×3 RGB888        230KB   ← encoded output
  camera JPEG framebuf           ~50KB   ← managed by esp_camera
  ──────────────────────────────────────
  total used                    ~510KB
  headroom                      ~7.5MB
```

**The one rule you must follow:**
- Big image buffers → PSRAM (`MALLOC_CAP_SPIRAM`)
- DMA transfer buffers → internal SRAM (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`)
- Everything else → wherever malloc puts it

---

## why JPEG capture (not RGB direct)

Espressif's own driver docs warn against capturing RGB888 directly when WiFi
is active — PSRAM write bandwidth is shared and frames can be corrupted or
incomplete. The correct pattern universally adopted by the community:

```
capture JPEG  →  WiFi off  →  fmt2rgb888() decode  →  process  →  display
```

JPEG at QVGA is ~15–50KB. RGB888 at QVGA is 230KB. JPEG is ~5–10× smaller,
faster to write into PSRAM, and the decode step (`fmt2rgb888`) is fast (~10ms).

---

## two layers: artwork + recoverable data

Each capture carries the WiFi scan **twice**, in two independent layers:

1. **Artistic layer (`wifi_rings.c`)** — concentric rings warp the photo. Signal
   strength drives radial displacement; SSID bytes drive per-ray sort depth. This
   is a *one-way visualisation* — you cannot read the networks back out of it.

2. **Recoverable layer (`wifi_data.c`)** — the *exact* scan (SSID, BSSID, RSSI,
   channel) is hidden in the pixel LSBs as a framed, CRC-checked payload. A host
   decoder reads it straight back.

**Ordering is what keeps them from fighting:** the LSB embed runs **after**
`rings_encode()`, so the pixel-sort can never overwrite the payload, and a ±1 LSB
nudge is invisible against the rings.

```
rings_encode(src → dst)      artistic layer, baked first
        ↓
data_embed_lsb(dst, payload) hidden layer written last — rings can't touch it
        ↓
save_bmp(dst)                lossless BMP to LittleFS, or the LSB layer dies
```

**The hard constraint: the image must be saved losslessly.** JPEG throws away
exactly the low-order bits the payload lives in, so the device saves an
uncompressed 24-bit **BMP** (~230 KB). Never re-save the file as JPEG.

### payload frame (`wifi_data.h`)

```
magic 'WBCN' | version(1) | n_nets(1)
  per net:  ssid_len(1) | ssid | bssid[6] | rssi(int8) | channel(1)
crc16 (CCITT-FALSE, big-endian) over everything before it
```

Capacity at 320×240×3 = 230,400 LSBs ≈ 28 KB; a full 12-network payload is < 1 KB,
so 1 LSB/channel (the least visible option) is used. The device also self-extracts
the payload right after embedding and logs `self-extract OK/FAILED` over serial.

### recovering the hidden data

Images live on the device's LittleFS as `/beacon_NNNN.bmp`. Pull them off over USB
(the LittleFS uploader/downloader plugin, or a small serial/web dump), then on a
laptop:

```
pip install pillow numpy
python decode_beacon.py beacon_0001.bmp        # table
python decode_beacon.py *.bmp --json           # machine-readable
```

`test_data_roundtrip.py` mirrors the firmware in Python and decodes its own output
— run it to sanity-check the format end-to-end without hardware (it prints `PASS`).

### share mode — scan to download (`beacon_share.cpp`)

No card reader needed to get images off the device. In RESULT VIEW, **hold the
button ≥0.8s** to enter SHARE mode:

```
hold button → SoftAP "beacon-XXXX" up + DNS captive portal + HTTP server
            → display shows a WiFi-join QR
phone scans QR → joins the open AP → captive portal pops the gallery
              → tap an image → downloads the lossless BMP (LSB data intact)
press button → AP down, WiFi off, back to live view
```

- The QR encodes a standard Wi-Fi-join string (`WIFI:T:nopass;S:beacon-XXXX;;`),
  so one scan joins the network; the captive portal then opens `http://192.168.4.1`
  automatically. The SSID + URL are also printed on screen as a manual fallback.
- The AP is **open** (no password) — it's local and ephemeral, and open networks
  pop the captive portal most reliably. Downloads are served as
  `application/octet-stream` so phones save (not transcode) the file, keeping the
  hidden LSB payload byte-exact.
- Filenames are validated (`beacon_*.bmp`, no path traversal) before serving.

Dependency: the **QRCode** library by Richard Moore (Arduino Library Manager →
"QRCode"). `WiFi` / `WebServer` / `DNSServer` / `LittleFS` ship with arduino-esp32.

---

## workflow

```
[deep sleep]
      │  button press (EXT0, falling edge) → full reboot ~300ms
      ▼
[LIVE VIEW] ←─────────────────────────────────────────────────┐
      │  JPEG grab → decode → blit, ~5-8 fps                  │
      │  10s idle → light sleep (XCLK running, ~2ms wake)     │
      │  60s idle → deep sleep                                 │
      │  button press                                          │
      ▼                                                        │
[100ms settle]                                                 │
      ▼                                                        │
[fresh JPEG capture]   dedicated frame, no live-view bleed     │
      ▼                                                        │
[freeze frame on screen]  photo shown immediately              │
      │  "scanning wifi..." overlay drawn over image           │
      ▼                                                        │
[WiFi scan]            2-4s, photo visible behind overlay      │
      │                  → fills g_nets[], sorted by dBm desc  │
      ▼                                                        │
[WiFi off]             stop before encode to free PSRAM BW     │
      ▼                                                        │
[rings_encode()]       read g_src → write g_dst, pure CPU      │
      ▼                                                        │
[data_embed_lsb()]     hide scan in LSBs (after rings)         │
      ▼                                                        │
[save_bmp()]           lossless BMP → LittleFS                 │
      ▼                                                        │
[SPI blit]             ping-pong DMA, ~15ms                    │
      ▼                                                        │
[RESULT VIEW]                                                  │
      │  image held on screen, progress bar full               │
      │  tap button → live view ───────────────────────────────┤
      │  hold button → SHARE mode (AP + QR + gallery) → live ───┤
      │  10s idle → light sleep (XCLK running)                 │
      │  60s idle → deep sleep                                 │
      │  tap (after wake) ─────────────────────────────────────┘
```

Nothing in the pipeline runs simultaneously. No PSRAM bandwidth contention.

## sleep behaviour

| mode | trigger | wake time | XCLK | use |
|------|---------|-----------|------|-----|
| light sleep | 10s idle | ~2ms | running | short idle, fast resume |
| deep sleep | 60s idle | ~300ms (reboot) | off | long idle, lowest power |

Light sleep keeps camera XCLK running so the sensor maintains state.
Deep sleep always wakes into LIVE VIEW — result view is never the wake target.

---

## arduino IDE setup

```
Board:            ESP32S3 Dev Module
PSRAM:            OPI PSRAM          ← critical, must match R8 chip
Flash size:       16MB
Partition scheme: Custom            ← uses partitions.csv (~12.8MB LittleFS, ~55 images)
                  (or "Huge APP (3MB No OTA/1MB SPIFFS)" for ~4 images, no custom table)
CPU frequency:    240MHz
Arduino version:  ≥ 3.x (ESP32 core)
```

Images are saved to a LittleFS partition. The bundled `partitions.csv` gives a
~12.8MB data partition (~55 BMPs); select **Partition Scheme > Custom** to use it.
With the default 1MB SPIFFS scheme you get ~4 images.

## libraries

| library | version | notes |
|---------|---------|-------|
| Arduino_GFX_Library | latest | onboard display driver for this board |
| esp32-camera | bundled with arduino-esp32 core | no install needed |
| QRCode (ricmoo) | latest | SHARE-mode QR rendering — Library Manager: "QRCode" |
| WiFi / WebServer / DNSServer / LittleFS | bundled with arduino-esp32 core | no install needed |

## built-in display mapping

The Waveshare ESP32-S3-Touch-LCD-2 vendor demos use this LCD map:

```
SCLK 39
MOSI 38
MISO 40
DC   42
CS   45
RST  -1
BL   1
```

This repo now uses `Arduino_GFX_Library` directly with those pins.

## camera mapping

The Waveshare ESP32-S3-Touch-LCD-2 camera mapping used by this repo:

```
PWDN  17
RESET -1
XCLK   8
SIOD  21
SIOC  16
Y9     2
Y8     7
Y7    10
Y6    14
Y5    11
Y4    15
Y3    13
Y2    12
VSYNC  6
HREF   4
PCLK   9
```

## microSD mapping — verified from the schematic

The SD slot is SPI and **shares the LCD's SPI bus** (read off the
[schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2/ESP32-S3-Touch-LCD-2-SchDoc.pdf)):

```
IO38  LCD_MOSI / SD_MOSI     ← shared
IO39  LCD_SCLK / SD_SCLK     ← shared
IO40  SD_MISO                ← shared MISO line (LCD is write-only)
IO41  SD_CS                  ← dedicated chip-select
IO45  LCD_CS                 ← LCD chip-select (for reference)
IO42  LCD_DC
```

**This is a shared bus, not a second SPI port.** The LCD and SD are two devices on
*one* SPI bus, distinguished only by their CS lines (LCD_CS=45, SD_CS=41). You must
**not** spin up a separate `SPIClass` on IO38/39/40 for the SD — a second SPI
peripheral steals the pin routing from the display and breaks it, even with no card
inserted. The SD has to be mounted on the same SPI instance Arduino_GFX drives.

**Current status: SD is not used — storage is LittleFS** (internal flash), which
avoids the shared bus entirely. The SD is a future capacity upgrade. When you wire
it up (with hardware in hand to confirm live-view fps doesn't regress):

1. Drive the LCD and SD from one shared `SPIClass` — e.g. switch the LCD databus
   to `Arduino_HWSPI` on a `SPIClass` you `begin(39, 40, 38)`, then
   `SD.begin(41, thatSpi)`. Both bracket their transfers in SPI transactions, so
   they coexist on the bus, selected by CS.
2. Point `save_bmp()` at `SD` instead of `LittleFS` (one-line change — same BMP
   bytes, same lossless guarantee).

## GPIO notes

- GPIO0 is used for button input and sleep wake in this sketch.
- Use BOOT intentionally during flashing; holding it changes boot mode.
- The SD reuses the LCD's SPI pins (38/39/40); only CS (41) is its own — confirmed
  against the schematic, no pin collision with the camera.

---

## timing estimate (240MHz, OPI PSRAM @ 80MHz)

| step | estimate |
|------|----------|
| Live view frame rate | ~25–30 fps (GRAB_LATEST + quality 63 + 80MHz SPI) |
| Capture settle | 100ms |
| Camera capture (JPEG QVGA) | ~50ms |
| fmt2rgb888 decode | ~10ms |
| WiFi scan | 2–4s (hardware, unavoidable) |
| rings_encode (5 networks) | ~50ms |
| data_embed_lsb + self-extract | ~5ms |
| save BMP to LittleFS (~230KB) | ~100–400ms (flash write) |
| SPI blit ping-pong DMA | ~15ms |
| **total pipeline after press** | **~3–5s** (WiFi scan dominates) |

---

## RingConfig tuning for 320×240 @ ~150 PPI

```c
// minimum ring_thickness for visibility on physical screen: 14px
// recommended starting point:
inner_radius    = 18
ring_thickness  = 22
ring_gap        = 2
max_displace    = 36   // strong signal: pixels drag this far
min_displace    = 2    // weak signal: barely moves
sort_dir        = 0    // 0=dark-inward  1=bright-inward  2=hue  3=sat
disp_mode       = 0    // 0=radial (fastest, LUT-only)
```

## previewing on a PC (no hardware)

`wifi_rings.c` is pure C with no Arduino deps, so the **exact** device encoder
runs on a laptop. `rings_preview.py` compiles it (via `python -m ziglang cc`)
into a shared library and runs it over a photo with mock WiFi data — instant
visual tuning, no flashing.

```
pip install ziglang pillow numpy
python rings_preview.py                       # sample photo + mock_wifi_networks.json
python rings_preview.py myphoto.jpg
python rings_preview.py --ring-thickness 30 --max-displace 50
python rings_preview.py --sweep               # 2x2 grid over all four sort_dir modes
```

Output PNGs land in `output/`. The flags mirror `RingConfig`, so whatever looks
good here is what to set in `RING_CONFIG_DEFAULT`. This is a faithful test (same
C); the older `wifi_rings_per_signal.html` is a separate JS sketch and has
drifted from the current algorithm.

**Emulating the whole device (display + UI flow):** load the sketch into
[Wokwi](https://wokwi.com) (ESP32-S3 + ST7789 in the browser) to exercise the
screen layout, button taps/holds, sleep states, and the QR render. Wokwi has no
camera or real WiFi scan, so stub those to test the UI; use `rings_preview.py`
for the actual rings look.

## files

```
wifi_rings.h                    types, RingConfig, API
wifi_rings.c                    artistic encode (one-way), pure C, no Arduino deps
wifi_data.h                     payload frame format + data-layer API
wifi_data.c                     recoverable encode: framing, CRC16, LSB embed/extract
wifi_rings_esp32s3.ino          full Arduino sketch (pipeline, LittleFS save, sleep)
beacon_share.h                  SHARE-mode API (scan-to-download)
beacon_share.cpp                captive-portal SoftAP + QR + HTTP gallery
partitions.csv                  optional 16MB layout — ~12.8MB LittleFS (~55 images)
decode_beacon.py                host decoder — recover the hidden scan from a BMP
test_data_roundtrip.py          host self-test for the data layer (no hardware)
rings_preview.py                host preview — run the real encoder on a photo, emit PNG
rings_host.c                    C shim binding wifi_rings.c to rings_preview.py (ctypes)
FIRST_FLASH.md                 first-flash checklist for this board
wifi_rings_per_signal.html      browser reference — visualise the algorithm
mock_wifi_networks.json         mock scan data for the browser reference
README.md                       this file
```
