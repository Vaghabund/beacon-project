# TODO

## pending

- [ ] (capacity upgrade, optional) **shared-bus microSD** — SD shares the LCD SPI bus (IO38/39/40 + CS 41, verified). Drive LCD+SD from one SPIClass (LCD → Arduino_HWSPI), point save_bmp() at SD. Do this with hardware in hand to confirm live-view fps. LittleFS is the default until then.
- [ ] select Partition Scheme "Custom" (uses partitions.csv) for ~55 images, or accept ~4 on the default 1MB SPIFFS
- [ ] test and verify 25-30fps live view on hardware
- [ ] verify OV5640 XCLK stays stable through light sleep cycles
- [ ] verify fmt2rgb888 byte order matches Pixel {r,g,b} struct on target hardware
- [x] run test_data_roundtrip.py to confirm the data-layer format end-to-end (PASS)
- [ ] on-hardware: decode a real saved PNG with decode_beacon.py and confirm SSIDs match the scan
- [ ] on-hardware: verify SHARE mode end-to-end — QR scans, phone joins AP, captive portal opens gallery, PNG downloads and decodes (this is the only way images leave the device today, so it's the top item before calling this done)
- [ ] tune LONG_PRESS_MS if 800ms feels too short/long on the real button

## in progress

- [ ] hardware assembly and first flash

## done

- [x] repo cleanup — removed old/drifted visualizers (wifi_rings_per_signal.html, wifi_visualizer.py, demo_multiple_networks.py) and stale fixture (mock-data/scan-data_1); rings_preview.py replaces them
- [x] host preview — rings_preview.py compiles the real wifi_rings.c (via pip ziglang) and runs it on a photo; param flags + sort_dir sweep, PNG output. Faithful visual tuning with no hardware.
- [x] SHARE mode — long-press in result view opens captive-portal SoftAP + QR; phone scans to join and download saved BMPs (beacon_share.cpp; needs QRCode lib)
- [x] recoverable data layer — LSB stego (SSID/BSSID/RSSI/channel), framed + CRC16, embedded after rings so the sort can't clobber it
- [x] lossless BMP save to LittleFS (24-bit, bottom-up/BGR) — JPEG would destroy the LSB payload
- [x] verified microSD pinout from schematic (IO38/39/40 + CS 41, shares LCD bus) — documented for the capacity upgrade
- [x] host decoder (decode_beacon.py) + format self-test (test_data_roundtrip.py)
- [x] capture BSSID in the scan (the real per-AP signature)
- [x] consolidate repo — remove Python scanner, deprecated HTML variants, CSV mock data
- [x] landscape orientation — 320×240, no software rotation needed
- [x] extern "C" linkage fix for wifi_rings.c ↔ .ino
- [x] LUT operator precedence bug fix (wifi_rings.c)
- [x] non-ASCII SSID encoding — raw byte % 10 per byte, works for UTF-8
- [x] WiFi re-enable fix after capture failure
- [x] Pixel packing static_assert
- [x] three-state workflow: LIVE VIEW → PIPELINE → RESULT VIEW
- [x] battery-saving nap (45s idle → light sleep, screen+camera off; double-tap wakes; skipped while USB-tethered) — superseded the earlier light-sleep/deep-sleep split, see PROJECT_STATE.md
- [x] rank WiFi scan results by RSSI before truncating to MAX_NETWORKS — scanNetworks() isn't signal-sorted, so a dense area (>12 visible APs) was previously able to drop genuinely-strong networks in favor of weaker ones the scan happened to enumerate first
- [x] progress bar across pipeline stages
- [x] freeze frame + "scanning wifi..." overlay during WiFi scan
- [x] live view fps optimisation: GRAB_LATEST, fb_count=2, quality 63, 80MHz SPI
- [x] User_Setup.h added to repo
