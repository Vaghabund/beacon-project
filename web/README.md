# beacon web — image decoder (MVP)

Drop a beacon `.bmp` and get the hidden WiFi scan back as an OSINT dashboard —
all decoded **locally in the browser**, nothing uploaded.

## Run

```
cd web
python -m http.server 8000
# open http://localhost:8000  (must be http://, not file:// — ES modules)
```

Then drag a capture onto the page. To make a test capture without hardware:

```
python tools/test_data_roundtrip.py     # writes _roundtrip.bmp in the repo root
```

…and drop that `_roundtrip.bmp` onto the page.

## What it shows
- decode status (WBCN version, CRC, payload size)
- summary tiles (network count, bands, strongest signal, channels, hidden SSIDs)
- channel occupancy
- networks ranked by signal: SSID, BSSID, vendor (OUI), dBm + bar, channel/band, rough distance
- geolocation panel — **placeholder** for the WiGLE/wardriving lookup (phase 2: needs internet + an API key, server-side)
- raw decoded frame (hex)

## Files
- `decode.js` — faithful port of `tools/decode_beacon.py` (parses BMP bytes directly, byte-exact; no canvas)
- `index.html` — the dashboard UI
- `test_decode.js` — Node check: `node web/test_decode.js _roundtrip.bmp`

## Notes
- Input must stay **lossless** (BMP/PNG). A JPEG re-save destroys the LSB payload.
- The OUI vendor table in `index.html` is a small starter set — extend as needed.
