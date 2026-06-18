# TODO

## pending

- [ ] (capacity upgrade, optional) **shared-bus microSD** — SD shares the LCD SPI bus (IO38/39/40 + CS 41, verified). Drive LCD+SD from one SPIClass (LCD → Arduino_HWSPI), point save_bmp() at SD. Do this with hardware in hand to confirm live-view fps. LittleFS is the default until then.
- [ ] select Partition Scheme "Custom" (uses partitions.csv) for ~55 images, or accept ~4 on the default 1MB SPIFFS
- [ ] test and verify 25-30fps live view on hardware
- [ ] verify OV5640 XCLK stays stable through light sleep cycles
- [ ] verify fmt2rgb888 byte order matches Pixel {r,g,b} struct on target hardware
- [ ] run test_data_roundtrip.py to confirm the data-layer format end-to-end
- [ ] on-hardware: decode a real saved BMP with decode_beacon.py and confirm SSIDs match the scan

## in progress

- [ ] hardware assembly and first flash

## done

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
- [x] light sleep (10s idle, XCLK running, ~2ms wake)
- [x] deep sleep (60s idle, ~300ms wake, always resumes in live view)
- [x] progress bar across pipeline stages
- [x] freeze frame + "scanning wifi..." overlay during WiFi scan
- [x] live view fps optimisation: GRAB_LATEST, fb_count=2, quality 63, 80MHz SPI
- [x] User_Setup.h added to repo
