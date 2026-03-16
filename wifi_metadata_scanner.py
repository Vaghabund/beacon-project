#!/usr/bin/env python3
"""
Windows Wi-Fi metadata scanner.

This script collects nearby network metadata from:
    netsh wlan show networks mode=bssid

It does NOT capture packets or read private traffic.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


def run_netsh() -> str:
    """Run netsh and return raw output text."""
    cmd = ["netsh", "wlan", "show", "networks", "mode=bssid"]
    try:
        completed = subprocess.run(
            cmd,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except FileNotFoundError as exc:
        raise RuntimeError("netsh was not found. This script is for Windows.") from exc

    if completed.returncode != 0:
        stderr = (completed.stderr or "").strip()
        stdout = (completed.stdout or "").strip()
        details = stderr or stdout or "Unknown error"
        raise RuntimeError(
            "netsh failed. On recent Windows versions you may need location access "
            "enabled and/or an elevated terminal. Details: "
            f"{details}"
        )

    return completed.stdout


def parse_netsh_output(raw_text: str) -> dict[str, Any]:
    """Parse netsh wlan output into structured data."""
    networks: list[dict[str, Any]] = []

    current_ssid: dict[str, Any] | None = None
    current_bssid: dict[str, Any] | None = None

    # Matches lines like: "SSID 1 : MyNetwork"
    ssid_re = re.compile(r"^\s*SSID\s+\d+\s*:\s*(.*)$", re.IGNORECASE)

    for raw_line in raw_text.splitlines():
        line = raw_line.rstrip()
        if not line:
            continue

        ssid_match = ssid_re.match(line)
        if ssid_match:
            current_ssid = {
                "ssid": ssid_match.group(1).strip(),
                "authentication": None,
                "encryption": None,
                "bssids": [],
            }
            networks.append(current_ssid)
            current_bssid = None
            continue

        if current_ssid is None:
            continue

        stripped = line.strip()

        if stripped.lower().startswith("authentication") and ":" in stripped:
            current_ssid["authentication"] = stripped.split(":", 1)[1].strip()
            continue

        if stripped.lower().startswith("encryption") and ":" in stripped:
            current_ssid["encryption"] = stripped.split(":", 1)[1].strip()
            continue

        if stripped.lower().startswith("bssid") and ":" in stripped:
            bssid_value = stripped.split(":", 1)[1].strip()
            current_bssid = {
                "bssid": bssid_value,
                "signal": None,
                "radio_type": None,
                "band": None,
                "channel": None,
            }
            current_ssid["bssids"].append(current_bssid)
            continue

        if current_bssid is None:
            continue

        if stripped.lower().startswith("signal") and ":" in stripped:
            current_bssid["signal"] = stripped.split(":", 1)[1].strip()
            continue

        if stripped.lower().startswith("radio type") and ":" in stripped:
            current_bssid["radio_type"] = stripped.split(":", 1)[1].strip()
            continue

        if stripped.lower().startswith("band") and ":" in stripped:
            current_bssid["band"] = stripped.split(":", 1)[1].strip()
            continue

        if stripped.lower().startswith("channel") and ":" in stripped:
            channel_text = stripped.split(":", 1)[1].strip()
            try:
                current_bssid["channel"] = int(channel_text)
            except ValueError:
                current_bssid["channel"] = channel_text
            continue

    payload = {
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source": "netsh wlan show networks mode=bssid",
        "network_count": len(networks),
        "networks": networks,
    }
    return payload


def flatten_for_csv(payload: dict[str, Any]) -> list[dict[str, Any]]:
    """Flatten nested SSID/BSSID data into CSV rows."""
    rows: list[dict[str, Any]] = []
    captured_at = payload.get("captured_at")

    for network in payload.get("networks", []):
        ssid = network.get("ssid")
        authentication = network.get("authentication")
        encryption = network.get("encryption")

        for b in network.get("bssids", []):
            rows.append(
                {
                    "captured_at": captured_at,
                    "ssid": ssid,
                    "authentication": authentication,
                    "encryption": encryption,
                    "bssid": b.get("bssid"),
                    "signal": b.get("signal"),
                    "radio_type": b.get("radio_type"),
                    "band": b.get("band"),
                    "channel": b.get("channel"),
                }
            )

    return rows


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fieldnames = [
        "captured_at",
        "ssid",
        "authentication",
        "encryption",
        "bssid",
        "signal",
        "radio_type",
        "band",
        "channel",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Scan nearby Wi-Fi metadata (SSID/BSSID/signal/channel) on Windows."
    )
    parser.add_argument(
        "--json-out",
        default="wifi_scan.json",
        help="Path to JSON output file (default: wifi_scan.json)",
    )
    parser.add_argument(
        "--csv-out",
        default="wifi_scan.csv",
        help="Path to CSV output file (default: wifi_scan.csv)",
    )
    parser.add_argument(
        "--print",
        action="store_true",
        help="Print JSON payload to stdout as well.",
    )
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    try:
        raw = run_netsh()
        payload = parse_netsh_output(raw)
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    json_path = Path(args.json_out)
    csv_path = Path(args.csv_out)

    write_json(json_path, payload)
    rows = flatten_for_csv(payload)
    write_csv(csv_path, rows)

    print(f"Saved JSON: {json_path.resolve()}")
    print(f"Saved CSV:  {csv_path.resolve()}")
    print(f"Networks found: {payload['network_count']}")

    if args.print:
        print(json.dumps(payload, indent=2))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
