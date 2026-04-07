# NanoTrade Algorithm

NanoTrade Algorithm is a compact C++17 trading backtester focused on deterministic simulation, clean architecture, and inspectable behavior. It is intentionally scoped as a single-instrument, long-only research engine so the implementation stays credible, easy to review, and useful as a software engineering project rather than a finance feature dump.

## Why this project is interesting

This repository is stronger as an engineering artifact than as a trading pitch:

- It implements a deterministic event loop with explicit signal timing and order-fill rules.
- It separates market data loading, indicators, strategy generation, execution, portfolio state, and reporting.
- It treats input validation, error handling, and repeatable tests as first-class concerns.
- It provides a small CLI and sample dataset so the behavior is reproducible without network access.

## Features

### Indicators

- SMA
- EMA
- Momentum
- RSI

### Strategies

- SMA crossover
- EMA crossover
- Momentum
- RSI threshold

### Backtest outputs

- Final equity
- Net profit
- Total return
- Max drawdown
- Equity curve snapshots
- Trade log with side, fill price, commission, and post-trade cash

## Architecture

```text
.
├── app/
│   └── main.cpp
├── data/
│   └── sample_ohlcv.csv
├── include/nanotrade/
│   ├── backtest_engine.h
│   ├── downloader.h
│   ├── indicators.h
│   ├── market_data.h
│   ├── reporting.h
│   └── strategies.h
├── src/
│   ├── backtest_engine.cpp
│   ├── downloader.cpp
│   ├── indicators.cpp
│   ├── market_data.cpp
│   ├── reporting.cpp
│   └── strategies.cpp
├── tests/
│   └── test_main.cpp
├── CMakeLists.txt
└── Makefile
```

Module responsibilities:

- `market_data`: CSV parsing, validation, and the `Bar` data model.
- `indicators`: Pure computations over validated market bars.
- `strategies`: Signal generation behind a small strategy interface.
- `backtest_engine`: Portfolio state, order simulation, trade log, equity curve, and metrics.
- `reporting`: Human-readable summaries for CLI output.
- `downloader`: Optional Yahoo Finance download path when `libcurl` is available at build time.

### Data flow

```text
CSV input
  -> std::vector<Bar>
  -> Strategy::generateSignals(...)
  -> std::vector<Signal>
  -> BacktestEngine::run(...)
  -> BacktestResult
  -> formatSummary(...)
  -> CLI / logs
```

### API overview

```cpp
#include "nanotrade/backtest_engine.h"
#include "nanotrade/market_data.h"
#include "nanotrade/strategies.h"

auto bars = nanotrade::MarketDataLoader::loadCsvFile("data/sample_ohlcv.csv");
nanotrade::SmaCrossoverStrategy strategy(3, 5);

nanotrade::BacktestConfig config;
config.initial_cash = 10000.0;
config.order_quantity = 10;

nanotrade::BacktestResult result = nanotrade::BacktestEngine::run(bars, strategy, config);
```

## How the backtester works

The simulation is intentionally simple and explicit:

1. Load and validate OHLCV bars from CSV.
2. Generate one signal per bar from the selected strategy.
3. Evaluate signals on each bar close.
4. Queue any required order.
5. Fill queued orders on the next bar open.
6. Mark the portfolio to market on each bar close.
7. Optionally close any remaining position on the final bar close.

This avoids same-bar lookahead and makes the event flow easy to inspect in code and tests.

## Trade simulation assumptions

- Single instrument
- Long-only
- Fixed share quantity per order
- No leverage
- Signals are evaluated on the close
- Orders are filled on the next bar open
- Optional fixed commission per order
- Optional slippage in basis points
- Remaining open positions can be force-closed on the last bar

These assumptions are deliberately narrow so results are understandable and reproducible.

## Build

### Option 1: CMake

```bash
cmake -S . -B build
cmake --build build
```

If `libcurl` is available, the optional Yahoo downloader module is compiled in automatically. The core backtester, CLI demo, and tests do not depend on network access.

### Option 2: Make

```bash
make
```

## Run

Default demo on the included sample data:

```bash
./build/nanotrade
```

Sample output:

```text
Strategy: sma_crossover
Bars processed: 20
Initial cash: 10000.00
Final equity: 9930.00
Net profit: -70.00
Total return: -0.70%
Max drawdown: 0.70%
Executed trades: 4
```

Try a different strategy:

```bash
./build/nanotrade --strategy ema --short-window 3 --long-window 6
./build/nanotrade --strategy momentum --lookback 3
./build/nanotrade --strategy rsi --rsi-period 5 --rsi-entry 35 --rsi-exit 55
```

Use your own CSV:

```bash
./build/nanotrade --csv path/to/data.csv --strategy sma --short-window 10 --long-window 30
```

Expected CSV columns:

```text
Date,Open,High,Low,Close,Volume
```

Yahoo-style CSVs with an extra `Adj Close` column also work.

## Run tests

### CMake

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Make

```bash
make test
```

The test suite covers:

- indicator correctness
- strategy signal generation
- next-bar-open execution behavior
- deterministic repeatability
- final state and cash-constraint invariants
- malformed input and invalid parameter handling

## Continuous integration

GitHub Actions builds and tests the project automatically on every push and pull request on:

- `ubuntu-latest`
- `macos-latest`

## Example workflow

```bash
./build/nanotrade --list-strategies
./build/nanotrade --strategy sma --short-window 3 --long-window 5
./build/nanotrade --strategy momentum --lookback 3 --commission 1.00 --slippage-bps 5
```

The CLI prints a concise summary and trade log rather than claiming profitability from the sample dataset.

## Limitations

- Single-symbol backtests only
- Long-only execution model
- No intraday bars or order book simulation
- No dividends, splits, or portfolio rebalancing
- No portfolio optimization or parameter search

These are intentional tradeoffs to keep the core engine small, deterministic, and easy to inspect.

## Future improvements

- Multi-asset portfolio support
- Additional fill models
- Config file input for repeatable scenarios
- More structured output formats such as JSON
- Parameterized strategy sweeps

## Why this is relevant for backend / systems / performance roles

The strongest signal in this project is not finance domain complexity. It is the shape of the code:

- typed data flow with clear ownership boundaries
- deterministic state transitions
- defensive validation and failure handling
- testable pure computation modules
- buildable, runnable, and inspectable repository structure

That makes the project a better fit for internships and new grad roles focused on backend services, infrastructure, systems programming, or performance-sensitive C++ code.
