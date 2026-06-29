# First Flash Checklist

Target board: Waveshare ESP32-S3-Touch-LCD-2

## Before You Flash

1. Install Arduino IDE 2.x.
2. Install `esp32` by Espressif Systems, **version 3.1.3** — pinned, not the latest.
   Core 3.3.x changed `spiFrequencyToClockDiv()` and breaks the display library.
3. Install `GFX Library for Arduino` **1.5.4** from Library Manager.
4. **Do not** install any "QRCode" library — SHARE mode uses the QR encoder
   bundled inside the esp32 core (`<qrcode.h>` → `esp_qrcode_*`); a Library-Manager
   "QRCode" has the same header name and will collide.
5. Connect the board by USB-C.
6. For camera support, connect the OV5640 or OV2640 to the board's 24-pin camera socket.

## Arduino IDE Settings

Set these before uploading:

```
Board:            ESP32S3 Dev Module
USB CDC On Boot:  Enabled
PSRAM:            OPI PSRAM
Flash Size:       16MB
Partition Scheme: Custom (uses wifi_rings_esp32s3/partitions.csv, ~55 images)
                  or "Huge APP (3MB No OTA/1MB SPIFFS)" for ~4 images
CPU Frequency:    240MHz
Upload Speed:     921600 (drop to 460800 if unstable)
```

See the [README](README.md#flashing-it) for the full toolchain rationale and a
command-line flashing recipe.

## Flash Steps

1. Open the `wifi_rings_esp32s3/` folder (its `.ino`) in Arduino IDE.
2. Select the correct COM port.
3. Click Verify first.
4. Click Upload.
5. Open Serial Monitor at `115200` baud immediately after upload.

## Expected First Boot Output

You should see:

1. A boot banner.
2. PSRAM total/free values.
3. A `LittleFS ready: … KB total` line (saves go here).
4. A final `=== ready ===` line.

The screen should then enter live view if the camera is connected and detected.
**Tap** (screen touch or GPIO0) to capture; **hold ≥0.8 s** in either live or
result view for the SHARE-mode QR screen.

## If Upload Fails

1. Hold `BOOT`.
2. Press and release `RESET`.
3. Release `BOOT`.
4. Upload again.

## If Boot Fails

`no PSRAM — set Tools > PSRAM > OPI PSRAM`
: The board options are wrong.

`camera init failed`
: The camera is missing, not seated correctly, or not compatible with the selected pin map.

Blank screen but serial looks healthy
: Check that `Arduino_GFX_Library` is installed and that the upload used the correct board settings.

Serial port disappears or the board keeps resetting
: Retry with a lower upload speed and use the BOOT/RESET sequence above.
