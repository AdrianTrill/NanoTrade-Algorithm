# NanoTrade

NanoTrade is a backtesting engine in C++20. A strategy is a set of rules in a small language, such as `close > sma(close, 200) and rsi(close, 2) < 10`, read at each bar's close and traded at the next bar's open, so no rule can trade on prices it hasn't seen. The engine handles long and short positions, sizing by a share of equity or by a volatility target, stop losses, take profits, trailing stops, commissions, slippage, and the cost of borrowing for shorts, and reports thirty measures of the result against buying and holding. It is tested on 68,292 daily bars of seven US stocks from 1962 to 2013, bundled with it, on Linux, macOS, and Windows.

## The rule language

```text
crossover(ema(close, 12), ema(close, 26)) and rsi(close, 14) < 70
close > highest(high, 55)[1]
zscore(close, 20) < -2 and close > sma(close, $trend)
```

A rule combines the prices (`open`, `high`, `low`, `close`, `volume`, `hl2`), numbers, arithmetic, comparisons, `and`, `or`, `not`, and twenty functions: `sma`, `ema`, `rsi`, `stdev`, `highest`, `lowest`, `roc`, `change`, `zscore`, `atr`, `macd`, `macd_signal`, `bb_upper`, `bb_lower`, `crossover`, `crossunder`, `abs`, `min`, `max`, and `log`. `x[n]` is `x` as it was `n` bars ago, and `$name` is a parameter. Rules are type-checked before they run, and an error names the rule and the characters at fault, with a suggestion for a misspelled name: `unknown function 'smaa'; did you mean sma?`.

An indicator is undefined until its window is full, and a comparison with an undefined value doesn't hold, so a 200-day average starts trading 200 bars in. Every indicator a strategy uses is computed once and shared: rules over the same `sma(close, 20)` read one array.

## Strategies

Every built-in strategy is written in the rule language, so a custom one is no different from them.

| Strategy | Goes long when | Exits when |
| --- | --- | --- |
| Buy and hold | always | never |
| SMA crossover | `sma(close, $fast) > sma(close, $slow)` | the entry stops holding |
| EMA crossover | `ema(close, $fast) > ema(close, $slow)` | the entry stops holding |
| MACD | the MACD line is above its signal line | the entry stops holding |
| Time-series momentum | `roc(close, $lookback) > 0` | the entry stops holding |
| Donchian breakout | `close > highest(high, $entry)[1]` | `close < lowest(low, $exit)[1]` |
| RSI reversion | `rsi(close, $period) < $lower` | `rsi(close, $period) > $upper` |
| Bollinger reversion | `close < bb_lower(close, $period, $k)` | `close > sma(close, $period)` |
| RSI(2) pullback | `close > sma(close, $trend) and rsi(close, 2) < $dip` | `close > sma(close, $exit)` |

With shorts allowed, each takes the mirror-image short. Parameters are checked against their ranges, and pairs that must be ordered, such as a fast window below a slow one, are checked too.

## How a backtest runs

1. The rules are evaluated over every bar, giving the position wanted at each close: long, short, or flat.
2. A change of position is ordered at that close and filled at the next bar's open, moved against the trade by the slippage, and charged the commission.
3. Within each bar, stops are checked against its range. An open beyond a stop fills at the open; a bar that reached both the stop and the take profit is assumed to have reached the stop first. After a stop, the strategy has to change its mind before it can enter the same way again.
4. Equity is marked at every close. Shorts pay their borrowing cost each bar, and a position still open at the end is closed at the last close.

Orders sized at a close may no longer fit when the next open gaps; they are trimmed to the leverage cap, and counted.

## Use it from C++

```cpp
#include "nanotrade/engine.hpp"
#include "nanotrade/metrics.hpp"
#include "nanotrade/strategy.hpp"

const nanotrade::BarSeries bars = nanotrade::load_csv_file("data/AAPL.csv", "AAPL").series;
nanotrade::indicators::Cache cache(bars);

nanotrade::StrategySpec strategy;
strategy.id = "custom";
strategy.params = {{"trend", 200}, {"dip", 10}};
strategy.rules.long_entry = "close > sma(close, $trend) and rsi(close, 2) < $dip";
strategy.rules.long_exit = "close > sma(close, 5)";

nanotrade::BacktestConfig config;
config.commission_bps = 1;
config.slippage_bps = 2;

const nanotrade::Signals signals = nanotrade::generate_signals(strategy, cache, config.allow_short);
const nanotrade::BacktestResult result = nanotrade::run_backtest(bars, signals.direction, config, 0, bars.size());
const nanotrade::Metrics metrics = nanotrade::compute_metrics(bars, result, {}, config.risk_free_rate);
```

The loader reads the CSV files of Yahoo, Stooq, Nasdaq.com, and Quandl: it finds the columns by name, uses adjusted prices when the file has them, reads dates in either order, sorts files that are newest first, and repairs and reports bars whose high and low don't contain the open and close.

## Data

[`data/`](data/README.md) holds daily bars of Apple, Boeing, Caterpillar, Dell, eBay, Ford, and General Electric from the Quandl WIKI dataset, which is in the public domain, adjusted for splits and dividends. [`tools/prepare_data.py`](tools/prepare_data.py) downloads them at a pinned commit, and CI requires a fresh download to reproduce them byte for byte.

## Build and test

Requirements: CMake 3.21+ and a C++20 compiler (GCC 11+, Clang 14+, Apple Clang 15+, or MSVC 2022).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The tests check the indicators against direct reimplementations, the language's parsing, types, and errors, each built-in strategy against a hand-written version of it, the engine's fills, costs, and stops against hand-computed cases, and properties over the bundled data: equity always adds up to the trades, and changing every price after a bar changes nothing before it. CI runs them with GCC, Clang, Apple Clang, and MSVC, with warnings as errors, and under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.

Released under the [MIT License](LICENSE).
