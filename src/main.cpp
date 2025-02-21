#include <iostream>
#include <ctime>
#include "Downloader.h"
#include "Backtester.h"

static long toUnixTime(int year, int month, int day) {
    // Very rough function to get Unix time (UTC) for demonstration.
    // In real code, use <chrono> or a proper date library (date.h).
    std::tm timeStruct = {};
    timeStruct.tm_year = year - 1900;  // years since 1900
    timeStruct.tm_mon  = month - 1;    // months since January (0-11)
    timeStruct.tm_mday = day;
    timeStruct.tm_hour = 0;
    timeStruct.tm_min  = 0;
    timeStruct.tm_sec  = 0;

    return static_cast<long>(std::mktime(&timeStruct));
}

int main() {
    // Example: Download 1 year of daily data for Apple (AAPL) from Yahoo Finance
    long start = toUnixTime(2022, 1, 1);
    long end   = toUnixTime(2023, 1, 1);

    std::string symbol = "AAPL";     // or "BTC-USD", "TSLA", etc.
    std::string interval = "1d";     // 1d, 1wk, 1mo, etc.

    std::cout << "Downloading data for " << symbol << "...\n";
    auto priceData = Downloader::downloadYahooData(symbol, start, end, interval);
    std::cout << "Downloaded " << priceData.size() << " rows.\n";

    if (priceData.empty()) {
        std::cerr << "No data available. Exiting.\n";
        return 1;
    }

    // 1. Run SMA crossover
    int shortPeriod = 20;
    int longPeriod  = 50;
    double initialCapital = 10000.0;

    auto smaResult = Backtester::runSMACrossover(priceData, shortPeriod, longPeriod, initialCapital);
    std::cout << "\n=== SMA Crossover Strategy ===\n";
    std::cout << "Final PnL:      " << smaResult.finalPnL << "\n";
    std::cout << "Max Drawdown:   " << smaResult.maxDrawdown * 100.0 << "%\n";
    std::cout << "Daily Sharpe:   " << smaResult.sharpeRatio << "\n";

    // 2. Run Momentum Strategy
    int lookback = 10;
    auto momentumResult = Backtester::runMomentumStrategy(priceData, lookback, initialCapital);
    std::cout << "\n=== Momentum Strategy ===\n";
    std::cout << "Final PnL:      " << momentumResult.finalPnL << "\n";
    std::cout << "Max Drawdown:   " << momentumResult.maxDrawdown * 100.0 << "%\n";
    std::cout << "Daily Sharpe:   " << momentumResult.sharpeRatio << "\n";

    return 0;
}
