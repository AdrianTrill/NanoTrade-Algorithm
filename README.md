# NanoTrade

NanoTrade is a backtesting engine in C++20. A strategy is a set of rules in a small language, such as `close > sma(close, 200) and rsi(close, 2) < 10`, read at each bar's close and traded at the next bar's open, so no rule can trade on prices it hasn't seen. The engine handles long and short positions, sizing by a share of equity or by a volatility target, stop losses, take profits, trailing stops, commissions, slippage, and the cost of borrowing for shorts, and reports more than thirty measures of the result against buying and holding. Then it asks whether the result is real: it searches parameters on every core and measures how much of the winner's edge luck explains, tests walk-forward, and resamples the returns to show what luck could have done. Everything it does is one JSON API, answered from the command line or from its own HTTP server, with the same bytes on every platform. It is tested on 68,292 daily bars of seven US stocks from 1962 to 2013, bundled with it, on Linux, macOS, and Windows.

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

An order is sized at the close so that its shares, slippage, and commission come to the position wanted, and a reversal also pays to close the old side out of the same equity. If the next open gaps so far that the order no longer fits under the leverage cap, it is trimmed, and counted.

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


## Is it real?

A backtest that looks good is the easy part. The research tools ask whether it would have held up.

- **Parameter sweeps** try every combination of one or two parameters on every core, sharing the indicators between trials, and rank them by the Sharpe ratio, Sortino ratio, CAGR, total return, Calmar ratio, drawdown, or profit factor.
- **The deflated Sharpe ratio** (Bailey and López de Prado, 2014) is the probability that the winner's true Sharpe ratio beats the best one that luck alone would give among that many trials, given how widely the trials' Sharpe ratios spread, and the skew and fat tails of the winner's returns.
- **The probability of backtest overfitting** (Bailey, Borwein, López de Prado, and Zhu, 2017) cuts the history into 16 blocks, or fewer for the widest searches. For every way to pick half of them (12,870 for 16), it finds the best trial on that half and ranks it on the other. PBO is how often the winner lands in the bottom half out of sample.
- **Walk-forward testing** optimizes on a window of years, trades the year after, and rolls forward, or keeps the start anchored. The out-of-sample years are stitched into one equity curve, and the efficiency is their mean annual return over the in-sample one.
- **Monte Carlo resampling** draws a thousand alternative histories from the strategy's own daily returns by a stationary bootstrap (Politis and Romano, 1994), whose random-length blocks keep volatility clusters together. It reports bands of equity over time and the spread of the return, drawdown, and Sharpe ratio, with the chance of ending below the starting capital.
- **Random markets** are the control: `synthesize()` makes daily bars with GARCH(1,1) volatility clustering and jumps, and no structure a strategy could exploit, so whatever a search finds on one is luck. A seed gives the same market on every platform.

Continuing the example above, a search of 400 pairs of windows:

```cpp
#include "nanotrade/research.hpp"

nanotrade::SweepSpec sweep;
sweep.x = {"fast", 10, 200, 10};
sweep.y = nanotrade::SweepAxis{"slow", 20, 400, 20};

nanotrade::StrategySpec crossover;
crossover.id = "sma_cross";

const nanotrade::SweepResult search = nanotrade::run_sweep(bars, cache, crossover, config, 0, bars.size(), sweep);
// search.overfit.deflated_sharpe, search.overfit.pbo, and every trial's metrics in search.trials
```


## Command line

```bash
nanotrade run --data AAPL --strategy rsi2_pullback --commission 1 --slippage 2
nanotrade run --data F --long "rsi(close, 2) < 10 and close > sma(close, 200)" --long-exit "close > sma(close, 5)"
nanotrade sweep --data GE --x fast=10:200:10 --y slow=20:400:20
nanotrade walkforward --data CAT --strategy rsi_reversion --x period=5:25:5 --is-years 5 --oos-years 2
nanotrade montecarlo --data BA --strategy momentum --paths 2000
nanotrade serve                   # the API and the lab at http://127.0.0.1:8080
```

A sweep ends with its verdict:

```text
GE (General Electric), 1962-01-02 to 2013-12-31, 13,090 bars
Swept fast and slow: 400 combinations, 300 valid, ranked by sharpe
  100 couldn't run, such as: Fast SMA (20) must be less than Slow SMA (20)

Top combinations
  fast     20  slow    240   Sharpe   0.53   CAGR    8.3%   max drawdown   39.1%   trades 39
  fast     20  slow    220   Sharpe   0.53   CAGR    8.1%   max drawdown   43.0%   trades 45
  fast    180  slow    400   Sharpe   0.52   CAGR    8.5%   max drawdown   42.9%   trades 17
  ...

Overfitting
  Deflated Sharpe ratio  99.8%  (the chance the best Sharpe of 0.53 beats the 0.13 that luck alone would give the best of 300 trials)
  Probability of backtest overfitting  80.3%  (12,870 splits of 16 blocks: how often the in-sample winner finished in the bottom half out of sample)
  In-sample winner lost money out of sample  0.2% of splits
```

`nanotrade --help` lists every option: costs, sizing, stops, shorts, date ranges, random markets, CSV files, and `--json` for the raw response.

## One JSON API

The command line, the server, and the browser all send the same JSON requests to one function, `nanotrade::api::handle`, and get the same responses.

| Command | What it does |
| --- | --- |
| `version` | The engine's version |
| `catalog` | The strategies with their parameters, ranges, and rules, and the rule language's functions |
| `datasets` | The bundled datasets, with their date ranges and notes |
| `load` | Adds a CSV file's text as a named dataset |
| `bars` | A dataset's bars over a date range |
| `backtest` | A backtest: its measures against buying and holding, equity and indicator series, trades, monthly and yearly returns, and worst drawdowns |
| `sweep` | Every combination of one or two parameters, the ten best, and the overfitting measures |
| `walkforward` | A walk-forward test, window by window, and its chained equity |
| `montecarlo` | A backtest's returns resampled into bands and distributions |

```json
{"command": "backtest", "data": "AAPL", "range": {"from": "2003-01-01"},
 "strategy": {"id": "custom", "params": {"trend": 200},
              "long_entry": "close > sma(close, $trend) and rsi(close, 2) < 10", "long_exit": "close > sma(close, 5)"},
 "config": {"commission_bps": 1, "slippage_bps": 2, "stop_loss": 0.08}}
```

A request is checked before anything runs. An error names the field at fault, suggests the name that was meant, and points into a rule:

```json
{"ok": false, "error": {"message": "unknown function 'smaa'; did you mean sma?", "field": "strategy.long_entry", "position": 8, "length": 4}}
{"ok": false, "error": {"message": "unknown field config.comission_bps; did you mean commission_bps?", "field": "config.comission_bps"}}
```

**The same request gives the same bytes everywhere.** The engine computes exp, log, and the normal quantile itself instead of trusting each platform's math library, compiles without floating-point contraction, draws random numbers with its own generator and distributions, and writes each number as the shortest decimal that reads back exactly. [`tests/golden/`](tests/golden) holds 23 requests with their responses, and CI requires every one byte for byte with GCC, Clang, Apple Clang, and MSVC.

## The server

`nanotrade serve` answers `POST /api` with the JSON API, `GET /api/health` with the engine's version, and anything else from the lab's files. It is a small HTTP/1.1 server on the operating system's own sockets (POSIX or Winsock) with a pool of threads, so a sweep from the page runs natively on every core. It caps the size of headers and bodies, times out slow clients, answers 503 when its queue is full, and serves files only from inside the site's directory: no `..`, no backslashes or colons, and no symbolic links that lead out.

## Data

[`data/`](data/README.md) holds daily bars of Apple, Boeing, Caterpillar, Dell, eBay, Ford, and General Electric from the Quandl WIKI dataset, which is in the public domain, adjusted for splits and dividends. [`tools/prepare_data.py`](tools/prepare_data.py) downloads them at a pinned commit, and CI requires a fresh download to reproduce them byte for byte.

## Build and test

Requirements: CMake 3.21+ and a C++20 compiler (GCC 11+, Clang 14+, Apple Clang 15+, or MSVC 2022).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/nanotrade run --data AAPL --strategy rsi2_pullback
```

The tests work small cases by hand, checking every share, price, commission, and P&L of fills, stops, trailing stops, shorts, reversals, and sizing. They check each built-in strategy bar by bar against an independent version of it, on three stocks and two random markets; the indicators and the metrics against their definitions; and the CSV loader on each source's format and every bundled file. The research tests check that a sweep's cells equal separate backtests and come out bit for bit the same on one thread or eight, that the probability of overfitting is near one half on pure noise and small for a real edge among noise, that walk-forward windows tile the range and choose without peeking, and that resampling and random markets repeat exactly for a seed. The API's tests check every command and every way a request can be wrong, the server's tests check its parsing, limits, and refusals with real sockets, and the golden responses and the command line run end to end. On all seven stocks, under random signals and every combination of sizing, shorting, costs, and stops, four properties hold: the books balance (the final equity is the initial capital plus every trade's P&L), nothing looks ahead (rewriting every price after a bar changes nothing up to it), a run repeats exactly, and no fill takes a position past the leverage cap. CI runs them with GCC, Clang, Apple Clang, and MSVC, with warnings as errors, and under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.

Released under the [MIT License](LICENSE).
