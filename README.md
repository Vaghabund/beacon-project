# wifi_rings — ESP32-S3R8 build guide

## what this does

One-button device. Three states:

**LIVE VIEW** — camera streams to display as a lightweight viewfinder.
Press button → pipeline runs. 10s idle → light sleep. 60s idle → deep sleep.

**PIPELINE** — fresh capture → freeze frame on screen → WiFi scan (overlay shown) → encode rings → show result.
Progress bar advances across bottom of screen.

**RESULT VIEW** — encoded image held on screen.
Press button → back to live view. 10s idle → light sleep. 60s idle → deep sleep.

Deep sleep always wakes into live view — no edge cases.

---

## hardware

| part | notes |
|------|-------|
| ESP32-S3R8 | the `R8` suffix = 8MB OPI PSRAM, already on chip, no extra wiring |
| OV5640 | 5MP camera, capture at QVGA (320×240) as JPEG |
| ST7789 or ILI9341 | 320×240 SPI display, landscape orientation (`setRotation(1)`) |
| one button | any GPIO, active LOW, internal pullup |

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
[SPI blit]             ping-pong DMA, ~15ms                    │
      ▼                                                        │
[RESULT VIEW]                                                  │
      │  image held on screen, progress bar full               │
      │  10s idle → light sleep (XCLK running)                 │
      │  60s idle → deep sleep                                 │
      │  button press ─────────────────────────────────────────┘
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
Flash size:       8MB
Partition scheme: Huge APP (3MB No OTA/1MB SPIFFS)
CPU frequency:    240MHz
Arduino version:  ≥ 3.x (ESP32 core)
```

## libraries

| library | version | notes |
|---------|---------|-------|
| TFT_eSPI | latest | configure User_Setup.h for your panel |
| esp32-camera | bundled with arduino-esp32 core | no install needed |

## TFT_eSPI User_Setup.h

A ready-to-use `User_Setup.h` is included in this repo. Copy it into the library:

```
cp User_Setup.h ~/Arduino/libraries/TFT_eSPI/User_Setup.h
```

Key settings it sets:
- `ST7789_DRIVER`, `TFT_WIDTH 320`, `TFT_HEIGHT 240`
- `SPI_FREQUENCY 80000000` — 80MHz, halves blit time vs 40MHz
- `ESP32_DMA` — required for ping-pong DMA blit
- Only GLCD font loaded — smaller binary

Adjust the pin defines at the top to match your wiring.

## GPIO pins to avoid on ESP32-S3R8

GPIO 35, 36, 37 are used internally by the OPI PSRAM bus.
Do not assign any peripheral or camera pin to these.

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

## files

```
wifi_rings.h                    types, RingConfig, API
wifi_rings.c                    encode algorithm, pure C, no Arduino deps
wifi_rings_esp32s3.ino          full Arduino sketch
User_Setup.h                    TFT_eSPI config — copy to Arduino/libraries/TFT_eSPI/
wifi_rings_per_signal.html      browser reference — visualise the algorithm
mock_wifi_networks.json         mock scan data for the browser reference
README.md                       this file
```
