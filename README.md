# NanoTrade Algorithm

## Overview
**NanoTrade Algorithm** is a **high-performance algorithmic trading backtester** designed for **low-latency strategy execution and quantitative finance research**. It integrates **real-time data acquisition, technical indicators, and advanced backtesting capabilities** to help traders evaluate and optimize their strategies.

## Features
✅ **Live Data Download:** Uses **libcurl** to fetch historical data from Yahoo Finance.  
✅ **Technical Indicators:** Supports **Simple Moving Average (SMA), Exponential Moving Average (EMA), Momentum, and Relative Strength Index (RSI)**.  
✅ **Performance Metrics:** Computes **PnL (Profit and Loss), Max Drawdown, and Sharpe Ratio**.  
✅ **Low-Latency Execution:** Designed for **speed and efficiency**, enabling rapid strategy evaluation.  
✅ **CMake Build System:** Easily buildable on **Linux/macOS/Windows**.  
✅ **CSV Parsing:** Converts downloaded CSV data into structured price data for analysis.  

## Project Structure
```
NanoTradeAlgorithm
├── CMakeLists.txt
├── README.md
└── src
    ├── Backtester.cpp
    ├── Backtester.h
    ├── CSVReader.cpp
    ├── CSVReader.h
    ├── Downloader.cpp
    ├── Downloader.h
    ├── Indicators.cpp
    ├── Indicators.h
    └── main.cpp
```

## Build & Run Instructions

### Prerequisites
Ensure you have the following installed:
- **GCC/G++ (C++17 or later)**
- **CMake**
- **libcurl (for HTTP requests)**

#### Install libcurl (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install libcurl4-openssl-dev
```

### Compilation Steps
```bash
git clone https://github.com/YourUsername/NanoTradeAlgorithm.git
cd NanoTradeAlgorithm
mkdir build && cd build
cmake ..
make
```

### Running the Trading Simulator
```bash
./NanoTradeAlgorithm
```

### Expected Output
```bash
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

## Technical Indicators Implemented
This project includes multiple technical indicators for backtesting trading strategies:

### **Simple Moving Average (SMA)**
- Computes the average closing price over a specified period.
- Helps identify trends in market price movements.

### **Exponential Moving Average (EMA)**
- Gives more weight to recent prices for a more responsive trend indicator.
- Uses a smoothing factor **α = 2 / (period + 1)**.

### **Momentum Indicator**
- Measures the rate of price change over a given period.
- Formula: `Momentum = Current Price - Price N periods ago`.

### **Relative Strength Index (RSI)**
- Measures the magnitude of recent price changes to evaluate overbought or oversold conditions.
- Uses a 14-day default period and calculates the average gain/loss.
- RSI values range from **0 to 100**, with values above 70 considered **overbought** and below 30 considered **oversold**.

## Future Enhancements
- 📊 **More Trading Strategies (MACD, Bollinger Bands, RSI Enhancements, etc.)**
- 🚀 **Multithreaded execution for performance boost.**
- 🔄 **Integration with live market data feeds.**
- 🧠 **Machine Learning-based predictive trading models.**

## Contributing
Feel free to submit pull requests or open issues. Let's build a powerful backtester together! 🚀

