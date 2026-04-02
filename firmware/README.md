# Beacon Project – ESP32-S3 Firmware

C/ESP-IDF firmware for the **Waveshare ESP32-S3-LCD-1.85** glitch-art WiFi
scanner.  One button press captures a camera frame, overlays nearby WiFi
networks as concentric coloured pixel rings, displays the result on the 360×360
round LCD, and saves a binary PPM file to the MicroSD card.

---

## Hardware

| Component | Part |
|-----------|------|
| MCU board | Waveshare ESP32-S3-LCD-1.85 |
| Display   | 360×360 round LCD (GC9D01 driver) |
| Camera    | OV2640 (ribbon connector) |
| Storage   | MicroSD via SPI |
| Battery   | 3.7 V LiPo via MX1.25 port |

---

## Project layout

```
firmware/
├── CMakeLists.txt          # Top-level ESP-IDF project
├── sdkconfig.defaults      # PSRAM, WiFi, camera, OV2640 menuconfig presets
├── partitions.csv          # 8 MB flash layout (OTA + SPIFFS + core dump)
└── main/
    ├── idf_component.yml   # Pulls in espressif/esp32-camera via IDF CM
    ├── CMakeLists.txt
    ├── config.h            # ALL pin definitions and tunable constants
    ├── main.c              # app_main + button ISR + pipeline orchestration
    ├── wifi_scan.{h,c}     # ESP-IDF station-mode WiFi scan
    ├── camera.{h,c}        # OV2640 init, QVGA RGB565 capture → 360×360 RGB888
    ├── image_gen.{h,c}     # WiFi ring renderer + RGB888→RGB565 conversion
    ├── display.{h,c}       # GC9D01 SPI LCD driver + full-frame push
    ├── sd_save.{h,c}       # FAT/SD mount (SPI mode) + PPM P6 save
    └── battery.{h,c}       # ADC1 battery voltage monitoring
```

---

## Quick start

### Prerequisites

- ESP-IDF **≥ 5.0** installed and activated (`idf.py --version`).
- OV2640 camera, MicroSD card, and LiPo battery connected to the board.

### Build & flash

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The IDF Component Manager will automatically fetch `espressif/esp32-camera`
on the first build.

---

## Pin assignments

All GPIO numbers are defined in `main/config.h`.  Verify every pin against your
physical schematic before flashing.

### Display (GC9D01, SPI2)

| Signal | GPIO |
|--------|------|
| MOSI   | 13   |
| SCLK   | 12   |
| CS     | 11   |
| DC     | 10   |
| RST    | –    |
| BL     | 46   |

### OV2640 camera

| Signal | GPIO |
|--------|------|
| XCLK   | 15   |
| SDA    |  4   |
| SCL    |  5   |
| D7–D0  | 16,17,18,19,20,6,7,8 |
| VSYNC  | 21   |
| HREF   |  3   |
| PCLK   |  2   |

### SD card (SPI3)

| Signal | GPIO |
|--------|------|
| MOSI   | 38   |
| MISO   | 39   |
| SCLK   | 40   |
| CS     | 41   |

### Other

| Signal  | GPIO | Note |
|---------|------|------|
| Button  |  0   | Boot/user button, active-low |
| Bat ADC |  9   | ADC1 CH8; GPIO 35 is not ADC-capable on ESP32-S3 |

---

## Pixel encoding algorithm

Ported directly from `wifi_visualizer.py`:

1. **Sort** APs by RSSI descending (strongest → innermost ring).
2. **Ring radius** = `min_radius + index × gap` (index 0 = strongest).
3. **Hue** = `MD5(SSID) mod 360 / 360` (deterministic per SSID).
4. **Dot colour** = `HSV(hue, 0.5 + 0.5×norm, byte_val/255 × (0.6 + 0.4×norm))`  
   where `byte_val` cycles through the metadata string bytes.
5. **No anti-aliasing** – square pixel blobs, intentional glitch aesthetic.

---

## Output files

PPM files are written to `/sdcard/beacon_NNNN.ppm` (binary P6 format,
360×360 pixels, 8-bit RGB).  Open with GIMP, ImageMagick, or any PPM viewer.

Convert to PNG on a desktop:

```bash
convert /sdcard/beacon_0000.ppm beacon_0000.png
```

---

## Battery notes

- Low battery warning logged at < 3500 mV.
- SD card writes are skipped at < 3300 mV to prevent filesystem corruption.
- The ADC pin and divider ratio are configurable in `config.h`
  (`BATT_ADC_CHANNEL`, `BATT_DIVIDER_RATIO`).
