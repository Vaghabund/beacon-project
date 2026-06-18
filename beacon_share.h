#pragma once

// Forward declaration — keeps this header light; the .cpp pulls in Arduino_GFX.
class Arduino_GFX;

// ─────────────────────────────────────────────────────────────────────────────
//  SHARE mode — "scan to download"
//
//  Brings up an open captive-portal SoftAP, draws a WiFi-join QR on the display,
//  and serves the saved /beacon_*.bmp files over HTTP. Scan the QR with a phone:
//  it joins the AP, the captive portal pops the gallery, tap an image to download.
//
//  Blocks until `button_pin` is pressed, then stops the server, shuts the AP
//  down, and returns with WiFi off. Nothing here runs during the capture
//  pipeline, so it never contends with the camera/PSRAM path.
// ─────────────────────────────────────────────────────────────────────────────
void run_share_mode(Arduino_GFX* gfx, int button_pin);
