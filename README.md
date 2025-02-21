# AlgoTradingWithDownload

A **C++ Algorithmic Trading Backtesting Engine** that:

- **Automatically downloads** stock and crypto historical price data (Yahoo Finance).
- **Implements** two basic trading strategies:
  - **Simple Moving Average (SMA) Crossover**
  - **Momentum Strategy**
- **Computes performance metrics**:
  - Final Profit & Loss (PnL)
  - Maximum Drawdown
  - Sharpe Ratio

## Features

- Uses `libcurl` to **fetch historical market data** from Yahoo Finance.
- Parses CSV **into a structured format**.
- Runs **two example strategies** on the data.
- Evaluates **PnL, Max Drawdown, and Sharpe Ratio**.

## Installation

### 1. Install Dependencies

On **Ubuntu**:

```sh
sudo apt-get update
sudo apt-get install libcurl4-openssl-dev
```

### 2. Clone & Build

```sh
git clone https://github.com/YourUsername/AlgoTradingWithDownload.git
cd AlgoTradingWithDownload
mkdir build && cd build
cmake ..
make
```

### 3. Run

```sh
./AlgoTradingWithDownload
```

## Strategy Details

### 1. SMA Crossover Strategy

- **Buys** when a short moving average crosses above a longer moving average.
- **Sells** when the short moving average crosses below the long moving average.

### 2. Momentum Strategy

- **Buys** when the current price is higher than `lookback` days ago.
- **Sells** otherwise.

## Expected Output

```plaintext
Downloading data for AAPL...
Downloaded 252 rows.

=== SMA Crossover Strategy ===
Final PnL:      1234.56
Max Drawdown:   5.32%
Daily Sharpe:   1.23

=== Momentum Strategy ===
Final PnL:      890.12
Max Drawdown:   4.78%
Daily Sharpe:   1.10
```

## Notes

- **For actual trading,** include risk management and transaction cost handling.
- **Data sources** may change, so update URLs if needed.
- **To extend**, add more indicators (e.g., EMA, RSI, Bollinger Bands).

## License

MIT License. Free to use and modify.
