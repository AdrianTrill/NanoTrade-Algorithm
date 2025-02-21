#ifndef INDICATORS_H
#define INDICATORS_H

#include <vector>
#include "CSVReader.h"

class Indicators {
public:
    // Simple Moving Average
    static std::vector<double> SMA(const std::vector<PriceData>& data, int period);

    // Exponential Moving Average
    static std::vector<double> EMA(const std::vector<PriceData>& data, int period);

    // Momentum: difference between current close and close 'period' days ago
    static std::vector<double> Momentum(const std::vector<PriceData>& data, int period);

    // Relative Strength Index (RSI)
    // Uses a simple approach over the given period.
    static std::vector<double> RSI(const std::vector<PriceData>& data, int period);
};

#endif
