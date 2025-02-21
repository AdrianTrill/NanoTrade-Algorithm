#include "Backtester.h"
#include "Indicators.h"
#include <cmath>
#include <iostream>

static double safeClose(const std::vector<PriceData>& data, int index) {
    if (index < 0 || index >= (int)data.size()) return 0.0;
    return data[index].close;
}

std::vector<double> Backtester::computeDailyReturns(const std::vector<double>& equityCurve) {
    std::vector<double> returns;
    returns.reserve(equityCurve.size());

    for (size_t i = 1; i < equityCurve.size(); ++i) {
        double dailyReturn = (equityCurve[i] - equityCurve[i - 1]) / equityCurve[i - 1];
        returns.push_back(dailyReturn);
    }
    return returns;
}

double Backtester::calculateMaxDrawdown(const std::vector<double>& equityCurve) {
    double maxDrawdown = 0.0;
    double peak = equityCurve[0];

    for (size_t i = 1; i < equityCurve.size(); ++i) {
        if (equityCurve[i] > peak) {
            peak = equityCurve[i];
        }
        double drawdown = (peak - equityCurve[i]) / peak;
        if (drawdown > maxDrawdown) {
            maxDrawdown = drawdown;
        }
    }
    return maxDrawdown;
}

double Backtester::calculateSharpeRatio(const std::vector<double>& returns, double riskFreeRate) {
    if (returns.empty()) return 0.0;

    // Mean
    double sum = 0.0;
    for (double r : returns) {
        sum += r;
    }
    double mean = sum / returns.size();

    // Std dev
    double sqSum = 0.0;
    for (double r : returns) {
        sqSum += (r - mean) * (r - mean);
    }
    double variance = sqSum / (returns.size() - 1);
    double stdDev = std::sqrt(variance);

    // Sharpe (daily, not annualized)
    if (stdDev == 0.0) return 0.0;
    return (mean - riskFreeRate) / stdDev;
}

TradeResult Backtester::runSMACrossover(
        const std::vector<PriceData>& data,
        int shortPeriod,
        int longPeriod,
        double initialCapital
) {
    if (shortPeriod >= longPeriod || data.empty()) {
        return {0.0, 0.0, 0.0};
    }

    auto shortSMA = Indicators::SMA(data, shortPeriod);
    auto longSMA  = Indicators::SMA(data, longPeriod);

    double capital = initialCapital;
    int sharesHeld = 0;
    std::vector<double> equityCurve;
    equityCurve.reserve(data.size());

    for (size_t i = 0; i < data.size(); ++i) {
        double closePrice = data[i].close;

        // Signal: if shortSMA > longSMA => buy, else flat
        bool buySignal = false;
        if (i >= (size_t)(longPeriod - 1)) {
            if (shortSMA[i] > longSMA[i]) {
                buySignal = true;
            }
        }

        // If buySignal and no shares, buy 1 share
        if (buySignal && sharesHeld == 0) {
            if (capital >= closePrice) {
                sharesHeld = 1;
                capital -= closePrice;
            }
        }
            // If no buySignal and we have shares, sell
        else if (!buySignal && sharesHeld > 0) {
            capital += (sharesHeld * closePrice);
            sharesHeld = 0;
        }

        double dailyEquity = capital + sharesHeld * closePrice;
        equityCurve.push_back(dailyEquity);
    }

    // Liquidate at the end
    if (sharesHeld > 0 && !data.empty()) {
        capital += sharesHeld * data.back().close;
        sharesHeld = 0;
        equityCurve.back() = capital;
    }

    double finalPnL = equityCurve.back() - initialCapital;
    double maxDD = calculateMaxDrawdown(equityCurve);
    auto dailyReturns = computeDailyReturns(equityCurve);
    double sharpe = calculateSharpeRatio(dailyReturns);

    return { finalPnL, maxDD, sharpe };
}

TradeResult Backtester::runMomentumStrategy(
        const std::vector<PriceData>& data,
        int lookback,
        double initialCapital
) {
    if (lookback <= 0 || data.empty()) {
        return {0.0, 0.0, 0.0};
    }

    double capital = initialCapital;
    int sharesHeld = 0;
    std::vector<double> equityCurve;
    equityCurve.reserve(data.size());

    for (size_t i = 0; i < data.size(); ++i) {
        double closePrice = data[i].close;
        bool buySignal = false;

        // If today's close > close from 'lookback' days ago => buy
        if ((int)i - lookback >= 0) {
            if (closePrice > safeClose(data, i - lookback)) {
                buySignal = true;
            }
        }

        if (buySignal && sharesHeld == 0) {
            if (capital >= closePrice) {
                sharesHeld = 1;
                capital -= closePrice;
            }
        } else if (!buySignal && sharesHeld > 0) {
            capital += (sharesHeld * closePrice);
            sharesHeld = 0;
        }

        double dailyEquity = capital + sharesHeld * closePrice;
        equityCurve.push_back(dailyEquity);
    }

    // Liquidate
    if (sharesHeld > 0 && !data.empty()) {
        capital += sharesHeld * data.back().close;
        sharesHeld = 0;
        equityCurve.back() = capital;
    }

    double finalPnL = equityCurve.back() - initialCapital;
    double maxDD = calculateMaxDrawdown(equityCurve);
    auto dailyReturns = computeDailyReturns(equityCurve);
    double sharpe = calculateSharpeRatio(dailyReturns);

    return { finalPnL, maxDD, sharpe };
}
