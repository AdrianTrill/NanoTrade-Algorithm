#include "Indicators.h"
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <cmath>

// Simple Moving Average
std::vector<double> Indicators::SMA(const std::vector<PriceData>& data, int period) {
    if (period <= 0) {
        throw std::invalid_argument("Period must be > 0");
    }
    std::vector<double> smaValues(data.size(), 0.0);

    double sum = 0.0;
    for (size_t i = 0; i < data.size(); ++i) {
        sum += data[i].close;
        if (i >= static_cast<size_t>(period))
            sum -= data[i - period].close;

        if (i >= static_cast<size_t>(period - 1))
            smaValues[i] = sum / period;
        else
            smaValues[i] = 0.0; // Not enough data
    }
    return smaValues;
}

// Exponential Moving Average
std::vector<double> Indicators::EMA(const std::vector<PriceData>& data, int period) {
    if (period <= 0) {
        throw std::invalid_argument("Period must be > 0");
    }
    std::vector<double> emaValues(data.size(), 0.0);
    if (data.empty())
        return emaValues;

    // Smoothing factor
    double alpha = 2.0 / (period + 1);
    emaValues[0] = data[0].close;  // start with first close price

    for (size_t i = 1; i < data.size(); ++i) {
        emaValues[i] = data[i].close * alpha + emaValues[i - 1] * (1 - alpha);
    }
    return emaValues;
}

// Momentum: current price minus the price 'period' days ago
std::vector<double> Indicators::Momentum(const std::vector<PriceData>& data, int period) {
    if (period <= 0) {
        throw std::invalid_argument("Period must be > 0");
    }
    std::vector<double> momentum(data.size(), 0.0);

    for (size_t i = 0; i < data.size(); ++i) {
        if (i < static_cast<size_t>(period))
            momentum[i] = 0.0; // not enough data to compute momentum
        else
            momentum[i] = data[i].close - data[i - period].close;
    }
    return momentum;
}

// Relative Strength Index (RSI)
// This implementation calculates RSI for each day using a simple average of gains and losses over 'period' days.
std::vector<double> Indicators::RSI(const std::vector<PriceData>& data, int period) {
    if (period <= 0) {
        throw std::invalid_argument("Period must be > 0");
    }
    std::vector<double> rsi(data.size(), 0.0);
    if (data.size() < static_cast<size_t>(period))
        return rsi; // Not enough data

    // For the first RSI value, calculate average gain and loss using a simple sum over 'period' days.
    double gainSum = 0.0;
    double lossSum = 0.0;
    for (int i = 1; i <= period; ++i) {
        double change = data[i].close - data[i - 1].close;
        if (change > 0)
            gainSum += change;
        else
            lossSum += -change;
    }
    double avgGain = gainSum / period;
    double avgLoss = lossSum / period;

    // Avoid division by zero; if no losses, RSI is 100.
    double rs = (avgLoss == 0) ? 0 : avgGain / avgLoss;
    rsi[period] = (avgLoss == 0) ? 100 : 100 - (100 / (1 + rs));

    // Use Wilder's smoothing method for subsequent values
    for (size_t i = period + 1; i < data.size(); ++i) {
        double change = data[i].close - data[i - 1].close;
        double gain = (change > 0) ? change : 0;
        double loss = (change < 0) ? -change : 0;

        avgGain = ((avgGain * (period - 1)) + gain) / period;
        avgLoss = ((avgLoss * (period - 1)) + loss) / period;
        rs = (avgLoss == 0) ? 0 : avgGain / avgLoss;
        rsi[i] = (avgLoss == 0) ? 100 : 100 - (100 / (1 + rs));
    }

    // For indices where we can't calculate RSI, leave them as 0.
    for (size_t i = 0; i < static_cast<size_t>(period); ++i)
        rsi[i] = 0.0;

    return rsi;
}
