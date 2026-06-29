#pragma once

// Forward declaration — keeps this header light; the .cpp pulls in Arduino_GFX.
class Arduino_GFX;

// Press poll callback: returns 0 = none, 1 = short, 2 = long.
// Supplied by the sketch so share mode shares the exact same touch+GPIO
// long-press detection as the rest of the UI (the .cpp can't see the static
// touch helpers in the .ino).
typedef int (*beacon_press_fn)(void);

// ─────────────────────────────────────────────────────────────────────────────
//  SHARE mode — "scan to download"
//
//  Brings up an open captive-portal SoftAP, draws a WiFi-join QR on the display,
//  and serves the saved /beacon_*.png files over HTTP. Scan the QR with a phone:
//  it joins the AP, the captive portal pops the gallery, tap an image to download.
//
//  Blocks until a LONG press (via the supplied poll callback — touch or GPIO0),
//  then stops the server, shuts the AP down, and returns with WiFi off. Nothing
//  here runs during the capture pipeline, so it never contends with the
//  camera/PSRAM path.
// ─────────────────────────────────────────────────────────────────────────────
void run_share_mode(Arduino_GFX* gfx, beacon_press_fn poll);
