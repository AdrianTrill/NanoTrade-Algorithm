#!/usr/bin/env python3
"""The study in the README, run through the engine's JSON API.

1. Strategies against buying and holding: each built-in strategy with its default parameters, long only, over each
   bundled stock's whole history, paying 1 basis point of commission and 2 of slippage on every fill.
2. What a search finds: the SMA crossover's 300 pairs of windows (fast 10 to 200 days, slow 20 to 400, fast below
   slow), searched on each stock and on 20 random markets that have nothing to find.
3. Walk-forward: the same search on four years at a time, each winner traded on the year after.

The engine is deterministic, so the study is too. This writes the results to docs/study.json and the tables into the
README, between its <!-- study:NAME --> markers; with --check it writes nothing and fails unless a fresh run
reproduces both exactly, which CI requires.

  python3 tools/study.py build/nanotrade            # run the study; write docs/study.json and the README's tables
  python3 tools/study.py build/nanotrade --check    # run it; fail if either would change

Standard library only.
"""

import argparse
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STOCKS = ["AAPL", "BA", "CAT", "DELL", "EBAY", "F", "GE"]
STRATEGIES = ["sma_cross", "ema_cross", "macd", "momentum", "donchian", "rsi_reversion", "bollinger", "rsi2_pullback"]
COSTS = {"commission_bps": 1, "slippage_bps": 2}
SEARCH = {"x": {"param": "fast", "from": 10, "to": 200, "step": 10},
          "y": {"param": "slow", "from": 20, "to": 400, "step": 20}}
RANDOM_MARKETS = 20
RANDOM_BARS = 6000  # 23 years of business days
PASS = 0.95  # the deflated Sharpe ratio a winner needs before it counts as more than luck
MARKER = re.compile(r"(<!-- study:([a-z_]+) -->\n)(.*?)(<!-- /study:\2 -->)", re.S)


def call(binary, request):
    completed = subprocess.run([binary, "api", "-"], input=json.dumps(request), capture_output=True, text=True,
                               check=True)
    response = json.loads(completed.stdout)
    if not response["ok"]:
        raise SystemExit(f"{request['command']} failed: {response['error']['message']}")
    return response["result"]


def r(value, digits=4):
    return None if value is None else round(value, digits)


def median(values):
    values = [value for value in values if value is not None]
    return r(statistics.median(values)) if values else None


def strategies_vs_holding(binary):
    rows = []
    for symbol in STOCKS:
        for strategy in STRATEGIES:
            result = call(binary, {"command": "backtest", "data": symbol, "strategy": {"id": strategy},
                                   "config": COSTS, "output": {"series": False, "trades": False}})
            m, b = result["metrics"], result["benchmark"]
            rows.append({"stock": symbol, "strategy": strategy, "years": r(m["years"], 2),
                         "sharpe": r(m["sharpe"]), "hold_sharpe": r(b["sharpe"]),
                         "cagr": r(m["cagr"]), "hold_cagr": r(b["cagr"]),
                         "max_drawdown": r(m["max_drawdown"]), "hold_max_drawdown": r(b["max_drawdown"]),
                         "exposure": r(m["exposure"]), "trades": m["trades"]})
    return rows


def search(binary, data, market):
    result = call(binary, {"command": "sweep", "data": data, "strategy": {"id": "sma_cross"}, "config": COSTS,
                           "sweep": SEARCH})
    o, best = result["overfitting"], result["best"]
    return {"market": market, "pairs": result["valid"], "fast": best["x"], "slow": best["y"],
            "sharpe": r(o["best_sharpe"]), "luck_sharpe": r(o["expected_max_sharpe"]),
            "deflated_sharpe": r(o["deflated_sharpe"]), "pbo": r(o["pbo"]), "oos_loss": r(o["oos_loss_probability"])}


def walk_forward(binary, symbol):
    result = call(binary, {"command": "walkforward", "data": symbol, "strategy": {"id": "sma_cross"},
                           "config": COSTS, "output": {"series": False, "trades": False},
                           "walkforward": {**SEARCH, "in_sample_years": 4, "out_of_sample_years": 1}})
    m, b, folds = result["metrics"], result["benchmark"], result["folds"]
    in_sample = [fold["in_sample"]["sharpe"] for fold in folds if fold["in_sample"]["sharpe"] is not None]
    return {"stock": symbol, "windows": len(folds), "years": r(m["years"], 2),
            "in_sample_sharpe": r(statistics.mean(in_sample)), "sharpe": r(m["sharpe"]), "hold_sharpe": r(b["sharpe"]),
            "cagr": r(m["cagr"]), "hold_cagr": r(b["cagr"]), "efficiency": r(result["efficiency"])}


def run(binary):
    catalog = call(binary, {"command": "catalog"})["strategies"]
    names = {strategy["id"]: strategy["name"] for strategy in catalog}
    return {
        "about": "Written by tools/study.py; see the README.",
        "costs": COSTS,
        "search": SEARCH,
        "strategies": [{"id": strategy, "name": names[strategy]} for strategy in STRATEGIES],
        "strategies_vs_holding": strategies_vs_holding(binary),
        "search_stocks": [search(binary, symbol, symbol) for symbol in STOCKS],
        "search_random": [search(binary, {"synthetic": {"seed": seed, "bars": RANDOM_BARS}}, f"Random market {seed}")
                          for seed in range(1, RANDOM_MARKETS + 1)],
        "walk_forward": [walk_forward(binary, symbol) for symbol in STOCKS],
    }


def number(value, digits=2):
    return "–" if value is None else f"{value:.{digits}f}"


def percent(value, digits=0):
    return "–" if value is None else f"{value * 100:.{digits}f}%"


def probability(value):
    if value is None:
        return "–"
    if value >= 0.9995:
        return "> 99.9%"
    return "< 0.1%" if value < 0.0005 else f"{value * 100:.1f}%"


def strategies_table(study):
    rows = study["strategies_vs_holding"]
    stocks = len(STOCKS)
    lines = ["| Strategy | Sharpe ratio | Annual return | Worst drawdown | Time in the market | "
             "Sharpe ratio above holding's | Shallower drawdown |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for strategy in study["strategies"]:
        mine = [row for row in rows if row["strategy"] == strategy["id"]]
        beats = sum(row["sharpe"] > row["hold_sharpe"] for row in mine)
        shallower = sum(row["max_drawdown"] < row["hold_max_drawdown"] for row in mine)
        lines.append(f"| {strategy['name']} | {number(median(row['sharpe'] for row in mine))} | "
                     f"{percent(median(row['cagr'] for row in mine), 1)} | "
                     f"{percent(median(row['max_drawdown'] for row in mine))} | "
                     f"{percent(median(row['exposure'] for row in mine))} | {beats} of {stocks} | "
                     f"{shallower} of {stocks} |")
    hold = [row for row in rows if row["strategy"] == STRATEGIES[0]]
    lines.append(f"| **Buy and hold** | **{number(median(row['hold_sharpe'] for row in hold))}** | "
                 f"**{percent(median(row['hold_cagr'] for row in hold), 1)}** | "
                 f"**{percent(median(row['hold_max_drawdown'] for row in hold))}** | **100%** | | |")
    return "\n".join(lines)


def search_row(row):
    return (f"| {row['market']} | {row['fast']:g} / {row['slow']:g} | {number(row['sharpe'])} | "
            f"{number(row['luck_sharpe'])} | {probability(row['deflated_sharpe'])} | {percent(row['pbo'])} | "
            f"{percent(row['oos_loss'])} |")


def search_table(study, every_random_market=False):
    stocks, randoms = study["search_stocks"], study["search_random"]
    lines = ["| Market | Best pair, fast / slow | Its Sharpe ratio | Best by luck alone | Deflated Sharpe ratio | "
             "Probability of overfitting | Lost out of sample |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    lines += [search_row(row) for row in stocks]
    if every_random_market:
        lines += [search_row(row) for row in randoms]
    else:
        lines.append(f"| *{len(randoms)} random markets, median* | | {number(median(row['sharpe'] for row in randoms))} | "
                     f"{number(median(row['luck_sharpe'] for row in randoms))} | "
                     f"{probability(median(row['deflated_sharpe'] for row in randoms))} | "
                     f"{percent(median(row['pbo'] for row in randoms))} | "
                     f"{percent(median(row['oos_loss'] for row in randoms))} |")
    passed = [sum(row["deflated_sharpe"] >= PASS for row in group) for group in (stocks, randoms)]
    lines += ["", f"The winner's deflated Sharpe ratio reaches {PASS:.0%} on {passed[0]} of the {len(stocks)} stocks and "
                  f"on {passed[1]} of the {len(randoms)} random markets."]
    return "\n".join(lines)


def walk_forward_table(study):
    rows = study["walk_forward"]
    lines = ["| Stock | Years traded | Windows | Sharpe ratio in sample | Sharpe ratio out of sample | "
             "Buy and hold, same years | Efficiency |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['stock']} | {row['years']:.0f} | {row['windows']} | {number(row['in_sample_sharpe'])} | "
                     f"{number(row['sharpe'])} | {number(row['hold_sharpe'])} | {number(row['efficiency'])} |")
    above = sum(row["sharpe"] > row["hold_sharpe"] for row in rows)
    lines += ["", f"Medians: {number(median(row['in_sample_sharpe'] for row in rows))} in sample, "
                  f"{number(median(row['sharpe'] for row in rows))} out of sample, and "
                  f"{number(median(row['hold_sharpe'] for row in rows))} for buying and holding; the walk-forward "
                  f"beat holding's Sharpe ratio on {above} of the {len(rows)} stocks."]
    return "\n".join(lines)


def sections(study):
    return {"strategies": strategies_table(study), "search": search_table(study),
            "walk_forward": walk_forward_table(study)}


def fill(readme, tables):
    found = []

    def replace(match):
        name = match.group(2)
        if name not in tables:
            raise SystemExit(f"README.md has a study:{name} section, and the study has no table by that name")
        found.append(name)
        return match.group(1) + tables[name] + "\n" + match.group(4)

    text = MARKER.sub(replace, readme)
    missing = sorted(set(tables) - set(found))
    if missing:
        raise SystemExit("README.md has no section for " + ", ".join(f"<!-- study:{name} -->" for name in missing))
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="the nanotrade command line, such as build/nanotrade")
    parser.add_argument("--check", action="store_true", help="fail unless the results and README are reproduced")
    args = parser.parse_args()

    study = run(args.binary)
    results = json.dumps(study, indent=1, ensure_ascii=False) + "\n"
    tables = sections(study)
    for title, table in (("Strategies against buying and holding, medians over the stocks", tables["strategies"]),
                         ("The search", search_table(study, every_random_market=True)),
                         ("Walk-forward", tables["walk_forward"])):
        print(f"{title}\n\n{table}\n")

    paths = {ROOT / "docs" / "study.json": results}
    readme_path = ROOT / "README.md"
    paths[readme_path] = fill(readme_path.read_text(encoding="utf-8"), tables)
    stale = [path for path, text in paths.items()
             if not path.exists() or path.read_text(encoding="utf-8") != text]
    if args.check:
        for path in stale:
            print(f"{path.relative_to(ROOT)} would change: run tools/study.py and commit the result", file=sys.stderr)
        if not stale:
            print("docs/study.json and the README's tables are reproduced exactly")
        return 1 if stale else 0
    for path in stale:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(paths[path], encoding="utf-8", newline="\n")
        print(f"wrote {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
