# TODO

## pending

- [ ] image saving — write encoded result to SD card or SPIFFS after pipeline
- [ ] test and verify 25-30fps live view on hardware
- [ ] verify OV5640 XCLK stays stable through light sleep cycles
- [ ] verify fmt2rgb888 byte order matches Pixel {r,g,b} struct on target hardware

## in progress

- [ ] hardware assembly and first flash

## done

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
