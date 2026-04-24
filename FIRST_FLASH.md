# First Flash Checklist

Target board: Waveshare ESP32-S3-Touch-LCD-2

## Before You Flash

1. Install Arduino IDE 2.x.
2. Install `esp32` by Espressif Systems, version 3.x or newer.
3. Install `Arduino_GFX_Library` from Library Manager.
4. Connect the board by USB-C.
5. For camera support, connect the OV5640 or OV2640 to the board's 24-pin camera socket.

## Arduino IDE Settings

Set these before uploading:

```
Board:            ESP32S3 Dev Module
USB CDC On Boot:  Enabled
PSRAM:            OPI PSRAM
Flash Size:       16MB
Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS) or larger app partition
CPU Frequency:    240MHz
Upload Speed:     921600 (drop to 460800 if unstable)
```

## Flash Steps

1. Open `wifi_rings_esp32s3.ino` in Arduino IDE.
2. Select the correct COM port.
3. Click Verify first.
4. Click Upload.
5. Open Serial Monitor at `115200` baud immediately after upload.

## Expected First Boot Output

You should see:

1. A boot banner.
2. PSRAM total/free values.
3. A final `=== ready ===` line.

The screen should then enter live view if the camera is connected and detected.

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
