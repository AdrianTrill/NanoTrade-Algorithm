#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "nanotrade/backtest_engine.h"
#include "nanotrade/indicators.h"
#include "nanotrade/market_data.h"
#include "nanotrade/strategies.h"

namespace {

using nanotrade::BacktestConfig;
using nanotrade::BacktestEngine;
using nanotrade::Bar;
using nanotrade::MarketDataLoader;
using nanotrade::MomentumStrategy;
using nanotrade::Signal;
using nanotrade::SmaCrossoverStrategy;

std::string makeDate(int day) {
    std::ostringstream value;
    value << "2024-01-" << std::setw(2) << std::setfill('0') << day;
    return value.str();
}

std::vector<Bar> makeBars(const std::vector<double>& closes) {
    std::vector<Bar> bars;
    bars.reserve(closes.size());

    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double close = closes[i];
        const double open = i == 0 ? close : closes[i - 1];
        const double high = std::max(open, close) + 1.0;
        const double low = std::min(open, close) - 1.0;
        bars.push_back(Bar{makeDate(static_cast<int>(i + 1)), open, high, low, close, 1000 + static_cast<long long>(i)});
    }

    return bars;
}

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectNear(double actual, double expected, double tolerance, const std::string& message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::ostringstream error;
        error << message << " (expected " << expected << ", got " << actual << ")";
        throw std::runtime_error(error.str());
    }
}

void expectNaN(double value, const std::string& message) {
    if (!std::isnan(value)) {
        throw std::runtime_error(message);
    }
}

void expectThrows(const std::function<void()>& fn, const std::string& message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }

    throw std::runtime_error(message);
}

void testSimpleMovingAverage() {
    const auto bars = makeBars({10.0, 11.0, 12.0, 13.0, 12.0, 11.0});
    const auto values = nanotrade::indicators::simpleMovingAverage(bars, 3);

    expectNaN(values[0], "SMA should be undefined before enough history exists.");
    expectNaN(values[1], "SMA should be undefined before enough history exists.");
    expectNear(values[2], 11.0, 1e-9, "Unexpected SMA value.");
    expectNear(values[3], 12.0, 1e-9, "Unexpected SMA value.");
    expectNear(values[4], 12.3333333333, 1e-9, "Unexpected SMA value.");
    expectNear(values[5], 12.0, 1e-9, "Unexpected SMA value.");
}

void testExponentialMovingAverage() {
    const auto bars = makeBars({10.0, 11.0, 12.0, 13.0, 12.0, 11.0});
    const auto values = nanotrade::indicators::exponentialMovingAverage(bars, 3);

    expectNaN(values[0], "EMA should be undefined before the seed window is ready.");
    expectNaN(values[1], "EMA should be undefined before the seed window is ready.");
    expectNear(values[2], 11.0, 1e-9, "Unexpected EMA seed value.");
    expectNear(values[3], 12.0, 1e-9, "Unexpected EMA value.");
    expectNear(values[4], 12.0, 1e-9, "Unexpected EMA value.");
    expectNear(values[5], 11.5, 1e-9, "Unexpected EMA value.");
}

void testMomentumAndRsi() {
    const auto momentum_bars = makeBars({10.0, 11.0, 12.0, 13.0, 12.0, 11.0});
    const auto momentum_values = nanotrade::indicators::momentum(momentum_bars, 2);

    expectNaN(momentum_values[0], "Momentum should be undefined before enough history exists.");
    expectNaN(momentum_values[1], "Momentum should be undefined before enough history exists.");
    expectNear(momentum_values[2], 2.0, 1e-9, "Unexpected momentum value.");
    expectNear(momentum_values[3], 2.0, 1e-9, "Unexpected momentum value.");
    expectNear(momentum_values[4], 0.0, 1e-9, "Unexpected momentum value.");
    expectNear(momentum_values[5], -2.0, 1e-9, "Unexpected momentum value.");

    const auto rising_rsi = nanotrade::indicators::relativeStrengthIndex(makeBars({10.0, 11.0, 12.0, 13.0}), 3);
    expectNear(rising_rsi[3], 100.0, 1e-9, "Unexpected RSI value for a rising series.");

    const auto falling_rsi = nanotrade::indicators::relativeStrengthIndex(makeBars({13.0, 12.0, 11.0, 10.0}), 3);
    expectNear(falling_rsi[3], 0.0, 1e-9, "Unexpected RSI value for a falling series.");

    const auto flat_rsi = nanotrade::indicators::relativeStrengthIndex(makeBars({10.0, 10.0, 10.0, 10.0}), 3);
    expectNear(flat_rsi[3], 50.0, 1e-9, "Unexpected RSI value for a flat series.");
}

void testStrategySignals() {
    const auto bars = makeBars({10.0, 11.0, 12.0, 13.0, 12.0, 11.0, 10.0});

    const SmaCrossoverStrategy sma_strategy(2, 3);
    const auto sma_signals = sma_strategy.generateSignals(bars);
    expectTrue(sma_signals == std::vector<Signal>({Signal::Flat, Signal::Flat, Signal::Long, Signal::Long, Signal::Long, Signal::Flat, Signal::Flat}),
               "SMA crossover signals did not match expectations.");

    const MomentumStrategy momentum_strategy(2);
    const auto momentum_signals = momentum_strategy.generateSignals(makeBars({10.0, 9.0, 11.0, 12.0, 10.0}));
    expectTrue(momentum_signals == std::vector<Signal>({Signal::Flat, Signal::Flat, Signal::Long, Signal::Long, Signal::Flat}),
               "Momentum signals did not match expectations.");
}

void testBacktestExecution() {
    const std::vector<Bar> bars = {
        {"2024-01-02", 10.0, 11.0, 9.0, 10.0, 1000},
        {"2024-01-03", 11.0, 12.0, 10.0, 11.0, 1100},
        {"2024-01-04", 12.0, 13.0, 11.0, 12.0, 1200},
        {"2024-01-05", 13.0, 14.0, 12.0, 13.0, 1300}
    };

    const std::vector<Signal> signals = {Signal::Flat, Signal::Long, Signal::Long, Signal::Flat};

    BacktestConfig config;
    config.initial_cash = 1000.0;
    config.order_quantity = 2;

    const auto result = BacktestEngine::run(bars, signals, "manual_test", config);

    expectTrue(result.trades.size() == 2, "Expected one entry and one exit trade.");
    expectTrue(result.trades[0].date == "2024-01-04", "Entry should occur on the next bar open.");
    expectNear(result.trades[0].price, 12.0, 1e-9, "Unexpected entry price.");
    expectTrue(result.trades[1].date == "2024-01-05", "Exit should occur on the last bar close.");
    expectNear(result.trades[1].price, 13.0, 1e-9, "Unexpected exit price.");
    expectNear(result.metrics.final_equity, 1002.0, 1e-9, "Unexpected final equity.");
    expectNear(result.metrics.net_profit, 2.0, 1e-9, "Unexpected net profit.");
}

void testDeterminism() {
    const auto bars = makeBars({10.0, 11.0, 12.0, 13.0, 12.0, 11.0, 10.0, 12.0, 13.0});
    const SmaCrossoverStrategy strategy(2, 4);

    BacktestConfig config;
    config.initial_cash = 5000.0;
    config.order_quantity = 5;
    config.commission_per_trade = 1.0;
    config.slippage_bps = 5.0;

    const auto first = BacktestEngine::run(bars, strategy, config);
    const auto second = BacktestEngine::run(bars, strategy, config);

    expectNear(first.metrics.final_equity, second.metrics.final_equity, 1e-9, "Backtests should be deterministic.");
    expectNear(first.metrics.max_drawdown, second.metrics.max_drawdown, 1e-9, "Backtests should be deterministic.");
    expectTrue(first.trades.size() == second.trades.size(), "Trade counts should match.");

    for (std::size_t i = 0; i < first.trades.size(); ++i) {
        expectTrue(first.trades[i].date == second.trades[i].date, "Trade dates should match.");
        expectTrue(first.trades[i].side == second.trades[i].side, "Trade sides should match.");
        expectNear(first.trades[i].price, second.trades[i].price, 1e-9, "Trade prices should match.");
    }
}

void testFinalStateInvariants() {
    const auto bars = makeBars({10.0, 11.0, 12.0, 13.0, 14.0});
    const std::vector<Signal> signals = {
        Signal::Flat,
        Signal::Long,
        Signal::Long,
        Signal::Long,
        Signal::Long
    };

    BacktestConfig config;
    config.initial_cash = 1000.0;
    config.order_quantity = 2;

    const auto result = BacktestEngine::run(bars, signals, "invariant_test", config);

    expectTrue(result.equity_curve.size() == bars.size(), "Equity curve should contain one point per input bar.");
    expectTrue(!result.trades.empty(), "Expected at least one trade.");
    expectTrue(result.trades.back().reason == "end_of_backtest", "Expected the final trade to be a forced close.");
    expectTrue(result.equity_curve.back().position_quantity == 0, "Forced close should leave no open position.");
    expectNear(result.equity_curve.back().market_value, 0.0, 1e-9, "Forced close should clear market value.");
    expectNear(result.equity_curve.back().cash, result.metrics.final_equity, 1e-9, "Final equity should equal final cash when flat.");
}

void testInsufficientCashPreventsEntry() {
    const auto bars = makeBars({100.0, 101.0, 102.0, 103.0});
    const std::vector<Signal> signals = {
        Signal::Flat,
        Signal::Long,
        Signal::Long,
        Signal::Long
    };

    BacktestConfig config;
    config.initial_cash = 50.0;
    config.order_quantity = 1;

    const auto result = BacktestEngine::run(bars, signals, "cash_constraint_test", config);

    expectTrue(result.trades.empty(), "Orders that cannot be funded should not be executed.");
    expectNear(result.metrics.final_equity, config.initial_cash, 1e-9, "Final equity should remain unchanged without trades.");

    for (const auto& point : result.equity_curve) {
        expectTrue(point.position_quantity == 0, "Portfolio should remain flat when every order is rejected.");
        expectNear(point.equity, config.initial_cash, 1e-9, "Equity should remain constant when no trades execute.");
    }
}

void testInvalidInputHandling() {
    expectThrows(
        [] { MarketDataLoader::loadCsvString("Date,Open,High,Low,Close,Volume\n2024-01-02,10,11,9,bad,1000\n"); },
        "Malformed numeric values should throw."
    );

    expectThrows(
        [] { MarketDataLoader::loadCsvString("Date,Open,High,Low,Close,Volume\n2024-01-03,10,11,9,10,1000\n2024-01-02,10,11,9,10,1000\n"); },
        "Descending dates should throw."
    );

    expectThrows(
        [] { SmaCrossoverStrategy strategy(5, 3); (void)strategy; },
        "Invalid strategy parameters should throw."
    );

    expectThrows(
        [] { BacktestEngine::run({}, {}, "empty"); },
        "Backtests require at least one bar."
    );
}

void runTest(const std::string& name, const std::function<void()>& fn, int& failures) {
    try {
        fn();
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
}

}  // namespace

int main() {
    int failures = 0;

    runTest("simple moving average", testSimpleMovingAverage, failures);
    runTest("exponential moving average", testExponentialMovingAverage, failures);
    runTest("momentum and RSI", testMomentumAndRsi, failures);
    runTest("strategy signals", testStrategySignals, failures);
    runTest("trade execution", testBacktestExecution, failures);
    runTest("determinism", testDeterminism, failures);
    runTest("final state invariants", testFinalStateInvariants, failures);
    runTest("insufficient cash invariant", testInsufficientCashPreventsEntry, failures);
    runTest("invalid input handling", testInvalidInputHandling, failures);

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
