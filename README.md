# Wi-Fi Metadata Scanner (Windows)

This project scans nearby Wi-Fi **metadata** using Windows `netsh` output.

It does **not** capture packets, decrypt traffic, or read private payloads.

## What it collects

- SSID (network name)
- BSSID (access point MAC)
- Signal strength
- Radio type
- Band
- Channel
- Authentication / encryption mode

## Usage

Run from PowerShell:

```powershell
python .\wifi_metadata_scanner.py
```

Custom output paths:

```powershell
python .\wifi_metadata_scanner.py --json-out .\data\scan.json --csv-out .\data\scan.csv
```

Print JSON payload to terminal too:

```powershell
python .\wifi_metadata_scanner.py --print
```

## Output files

- `wifi_scan.json` (nested SSID/BSSID structure)
- `wifi_scan.csv` (flat rows, easy for art/data pipelines)

## Notes

- Requires Windows and a Wi-Fi adapter.
- `netsh` must be available (standard on Windows).
- Run where you have permission to write output files.
