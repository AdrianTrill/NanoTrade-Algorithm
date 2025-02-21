#ifndef BACKTESTER_H
#define BACKTESTER_H

#include <vector>
#include "CSVReader.h"

struct TradeResult {
    double finalPnL;
    double maxDrawdown;
    double sharpeRatio;
};

class Backtester {
public:
    // 1. Simple moving average crossover strategy
    static TradeResult runSMACrossover(
            const std::vector<PriceData>& data,
            int shortPeriod,
            int longPeriod,
            double initialCapital = 10000.0
    );

    // 2. Basic momentum strategy: if today's close > close from 'lookback' days ago, go long, else flat
    static TradeResult runMomentumStrategy(
            const std::vector<PriceData>& data,
            int lookback,
            double initialCapital = 10000.0
    );

private:
    static double calculateMaxDrawdown(const std::vector<double>& equityCurve);
    static double calculateSharpeRatio(const std::vector<double>& returns, double riskFreeRate = 0.0);

    static std::vector<double> computeDailyReturns(const std::vector<double>& equityCurve);
};

#endif
