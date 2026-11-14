#!/usr/bin/env python3
"""Prepares the bundled market data: daily bars of seven US stocks from the Quandl WIKI dataset, which is in the
public domain, as MarketData.jl publishes it at a pinned commit.

Writes data/SYMBOL.csv, split- and dividend-adjusted OHLCV with the oldest bar first, and data/manifest.json with each
file's SHA-256. The bars are written as the source has them, with one correction: the source misses Caterpillar's
2-for-1 split of July 1964, so the prices before it are halved and the volumes doubled. Bars whose high or low doesn't
contain the open and close are left as they are, for the loader to repair and report.

  python tools/prepare_data.py           # download, normalize, and write data/
  python tools/prepare_data.py --check   # download again and require the files to match the manifest

Standard library only.
"""

import argparse
import csv
import hashlib
import io
import json
import sys
import urllib.request
from pathlib import Path

COMMIT = "bd65d8488fc8ede7a12649a64266783d9abff4af"
SOURCE = f"https://raw.githubusercontent.com/JuliaQuant/MarketData.jl/{COMMIT}/data/{{symbol}}.csv"
BROWSE = f"https://github.com/JuliaQuant/MarketData.jl/tree/{COMMIT}/data"

DATASETS = [
    ("AAPL", "Apple", "Includes the 52% fall of 2000-09-29, after a profit warning: a real move, not an error."),
    ("BA", "Boeing", ""),
    ("CAT", "Caterpillar", "The source misses the 2-for-1 split of 1964-07-06; the bars before it are adjusted here."),
    ("DELL", "Dell", "Ends on 2013-10-29, when Dell was taken private."),
    ("EBAY", "eBay", "Starts the day it listed, 1998-09-24."),
    ("F", "Ford", "The 32% fall of 1998-04-08 is the spin-off of Associates First Capital, which the source doesn't "
                  "adjust for: shareholders received the shares, so it is not a loss."),
    ("GE", "General Electric", ""),
]

# Splits missing from the source: symbol -> (first date at the new share count, ratio).
MISSING_SPLITS = {"CAT": ("1964-07-06", 2.0)}


def number(value: float) -> str:
    """Six significant digits, without exponent notation for the prices at hand."""
    text = f"{value:.6g}"
    if "e" in text:
        raise ValueError(f"unexpected magnitude {value}")
    return text


def prepare(symbol: str) -> str:
    with urllib.request.urlopen(SOURCE.format(symbol=symbol), timeout=60) as response:
        text = response.read().decode("utf-8")
    rows = list(csv.DictReader(io.StringIO(text)))
    rows.sort(key=lambda row: row["Date"])
    split = MISSING_SPLITS.get(symbol)

    out = io.StringIO()
    out.write("Date,Open,High,Low,Close,Volume\n")
    for row in rows:
        price_factor, volume_factor = 1.0, 1.0
        if split and row["Date"] < split[0]:
            price_factor, volume_factor = 1.0 / split[1], split[1]
        prices = [float(row[f"Adj. {key}"]) * price_factor for key in ("Open", "High", "Low", "Close")]
        volume = round(float(row["Adj. Volume"]) * volume_factor)
        out.write(",".join([row["Date"], *map(number, prices), str(volume)]) + "\n")
    return out.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify data/ against a fresh download")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "data")
    args = parser.parse_args()

    manifest = {
        "source": "Quandl WIKI Prices, in the public domain, as published by MarketData.jl",
        "url": BROWSE,
        "commit": COMMIT,
        "adjusted": "splits and dividends",
        "datasets": [],
    }
    mismatches = []
    for symbol, name, notes in DATASETS:
        content = prepare(symbol)
        lines = content.splitlines()[1:]
        digest = hashlib.sha256(content.encode()).hexdigest()
        manifest["datasets"].append({
            "symbol": symbol,
            "name": name,
            "file": f"{symbol}.csv",
            "bars": len(lines),
            "first": lines[0].split(",")[0],
            "last": lines[-1].split(",")[0],
            "bytes": len(content.encode()),
            "sha256": digest,
            "notes": notes,
        })
        path = args.out / f"{symbol}.csv"
        if args.check:
            if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                mismatches.append(path.name)
        else:
            path.write_text(content, newline="\n")
        print(f"{symbol:5} {len(lines):6} bars  {lines[0][:10]} to {lines[-1][:10]}  sha256 {digest[:16]}")

    manifest_text = json.dumps(manifest, indent=2) + "\n"
    manifest_path = args.out / "manifest.json"
    if args.check:
        if not manifest_path.exists() or manifest_path.read_text() != manifest_text:
            mismatches.append(manifest_path.name)
        if mismatches:
            print(f"out of date: {', '.join(mismatches)}; run tools/prepare_data.py", file=sys.stderr)
            return 1
        print("data/ matches a fresh download")
    else:
        manifest_path.write_text(manifest_text, newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
