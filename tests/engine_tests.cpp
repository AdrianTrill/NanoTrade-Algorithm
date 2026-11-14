// The backtest engine: small scenarios worked by hand, where every share, price, commission, equity, and P&L is
// checked, then properties on real data under seeded random signals: the books balance, nothing looks ahead, a run
// repeats exactly, no fill takes a position past the leverage cap, without gaps no order is trimmed, and after each
// fill the leverage is within the cap.

#include "check.hpp"

#include "nanotrade/engine.hpp"
#include "nanotrade/error.hpp"
#include "nanotrade/random.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nanotrade::BacktestConfig;
using nanotrade::BacktestResult;
using nanotrade::BarSeries;
using nanotrade::Fill;
using nanotrade::Random;
using nanotrade::Sizing;
using nanotrade::Trade;

using Directions = std::vector<std::int8_t>;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double hand_tolerance = 1e-9;  // relative, for the numbers worked by hand

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// ---- Scenarios ----

struct Bar {
    double open;
    double high;
    double low;
    double close;
};

// Daily bars from 2024-01-01.
BarSeries make_bars(std::initializer_list<Bar> rows) {
    BarSeries bars;
    bars.symbol = "TEST";
    nanotrade::Timestamp stamp = nanotrade::days_from_civil(2024, 1, 1) * nanotrade::seconds_per_day;
    for (const Bar& row : rows) {
        bars.push_back(stamp, row.open, row.high, row.low, row.close, 1000.0);
        stamp += nanotrade::seconds_per_day;
    }
    return bars;
}

Directions directions(std::initializer_list<int> values) {
    Directions out;
    for (const int value : values) {
        out.push_back(static_cast<std::int8_t>(value));
    }
    return out;
}

BacktestResult run(const BarSeries& bars, const Directions& direction, const BacktestConfig& config) {
    return nanotrade::run_backtest(bars, direction, config, 0, bars.size());
}

// Entry at bar 1's open, which gaps down from bar 0's close, so the order fits.
BarSeries rising_bars() {
    return make_bars({
        {100, 101, 99, 100},
        {99, 104, 98, 103},
        {104, 108, 103, 107},
        {110, 111, 108, 109},
        {108, 109, 106, 107},
    });
}

// ---- Expectations ----

void expect_value(double actual, double expected, const std::string& what) {
    check::expect_near(actual, expected, hand_tolerance * std::max(1.0, std::fabs(expected)), what);
}

void expect_values(const std::vector<double>& actual, const std::vector<double>& expected, const std::string& what) {
    check::expect_equal(actual.size(), expected.size(), what + ": length");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expect_value(actual[i], expected[i], what + " at " + std::to_string(i));
    }
}

// The cash at each close: the equity less the position's value.
void expect_cash(const BacktestResult& result,
                 const BarSeries& bars,
                 const std::vector<double>& expected,
                 const std::string& what) {
    std::vector<double> cash;
    for (std::size_t i = 0; i < result.equity.size(); ++i) {
        cash.push_back(result.equity[i] - result.position[i] * bars.close[result.begin + i]);
    }
    expect_values(cash, expected, what);
}

struct ExpectedFill {
    std::size_t bar;
    double shares;
    double price;
    double commission;
    double slippage;
    const char* reason;
};

void expect_fills(const BacktestResult& result, const std::vector<ExpectedFill>& expected, const std::string& what) {
    check::expect_equal(result.fills.size(), expected.size(), what + ": fills");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const Fill& fill = result.fills[i];
        const ExpectedFill& want = expected[i];
        const std::string label = what + ": fill " + std::to_string(i) + ' ';
        check::expect_equal(fill.bar, want.bar, label + "bar");
        expect_value(fill.shares, want.shares, label + "shares");
        expect_value(fill.price, want.price, label + "price");
        expect_value(fill.commission, want.commission, label + "commission");
        expect_value(fill.slippage, want.slippage, label + "slippage");
        check::expect_equal(fill.reason, std::string(want.reason), label + "reason");
    }
}

struct ExpectedTrade {
    int direction;
    std::size_t entry_bar;
    std::size_t exit_bar;
    double entry_price;
    double exit_price;
    double shares;
    double pnl;
    double return_pct;
    const char* exit_reason;
    bool open;
};

void expect_trades(const BacktestResult& result, const std::vector<ExpectedTrade>& expected, const std::string& what) {
    check::expect_equal(result.trades.size(), expected.size(), what + ": trades");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const Trade& trade = result.trades[i];
        const ExpectedTrade& want = expected[i];
        const std::string label = what + ": trade " + std::to_string(i) + ' ';
        check::expect_equal(trade.direction, want.direction, label + "direction");
        check::expect_equal(trade.entry_bar, want.entry_bar, label + "entry bar");
        check::expect_equal(trade.exit_bar, want.exit_bar, label + "exit bar");
        expect_value(trade.entry_price, want.entry_price, label + "entry price");
        expect_value(trade.exit_price, want.exit_price, label + "exit price");
        expect_value(trade.shares, want.shares, label + "shares");
        expect_value(trade.pnl, want.pnl, label + "pnl");
        expect_value(trade.return_pct, want.return_pct, label + "return");
        check::expect_equal(trade.exit_reason, std::string(want.exit_reason), label + "exit reason");
        check::expect_equal(trade.open, want.open, label + "open");
    }
}

struct Totals {
    double commissions;
    double slippage;
    double borrow;
    double traded_value;
    std::size_t trimmed;
    bool ruined;
};

void expect_totals(const BacktestResult& result, const Totals& expected, const std::string& what) {
    expect_value(result.commissions, expected.commissions, what + ": commissions");
    expect_value(result.slippage, expected.slippage, what + ": slippage");
    expect_value(result.borrow, expected.borrow, what + ": borrowing");
    expect_value(result.traded_value, expected.traded_value, what + ": traded value");
    check::expect_equal(result.trimmed, expected.trimmed, what + ": orders trimmed");
    check::expect_equal(result.ruined, expected.ruined, what + ": ruined");
}

// ---- Entries, exits, and costs ----

// Long at the close of bar 0, flat at the close of bar 2: in at bar 1's open, out at bar 3's.
void entry_and_exit_at_the_next_open() {
    const BarSeries bars = rising_bars();
    const BacktestResult result = run(bars, directions({1, 1, 0, 0, 0}), BacktestConfig{});
    check::expect_equal(result.begin, std::size_t{0}, "begin");
    check::expect_equal(result.end, std::size_t{5}, "end");
    check::expect_equal(result.initial_capital, 100'000.0, "initial capital");
    check::expect_equal(result.bars_per_year, bars.bars_per_year(), "bars per year, measured");
    expect_value(result.bars_per_year, 365.2425, "daily bars every calendar day");
    // 100,000 / 100 at bar 0's close is 1,000 shares, bought at 99 for 99,000, leaving 1,000 in cash.
    expect_fills(result, {{1, 1000.0, 99.0, 0.0, 0.0, "entry"}, {3, -1000.0, 110.0, 0.0, 0.0, "exit"}}, "long");
    expect_values(result.equity, {100'000.0, 104'000.0, 108'000.0, 111'000.0, 111'000.0}, "equity");
    expect_cash(result, bars, {100'000.0, 1000.0, 1000.0, 111'000.0, 111'000.0}, "cash");
    expect_values(result.position, {0.0, 1000.0, 1000.0, 0.0, 0.0}, "position");
    expect_values(result.exposure, {0.0, 103'000.0 / 104'000.0, 107'000.0 / 108'000.0, 0.0, 0.0}, "exposure");
    expect_trades(result, {{1, 1, 3, 99.0, 110.0, 1000.0, 11'000.0, 11'000.0 / 99'000.0, "exit", false}}, "long");
    expect_totals(result, {0.0, 0.0, 0.0, 99'000.0 + 110'000.0, 0, false}, "long");
}

// 10 bps of slippage, 5 bps of commission, and 1 a fill. The order is sized at bar 0's close so that its cost, with
// slippage and commission, comes to the equity: (100,000 - 1) / (100 * 1.001 * 1.0005) = 99,999 / 100.15005 = 998.49
// -> 998 shares (998 * 100.15005 = 99,949.75; 999 would cost 100,049.90). The buy pays 99 * 1.001 = 99.099, the sell
// gets 110 * 0.999 = 109.89, and each commission is 0.0005 of the fill's value plus 1.
void slippage_and_commission() {
    BacktestConfig config;
    config.slippage_bps = 10.0;
    config.commission_bps = 5.0;
    config.commission_fixed = 1.0;
    const BarSeries bars = rising_bars();
    const BacktestResult result = run(bars, directions({1, 1, 0, 0, 0}), config);
    const double shares = 998.0;
    const double bought = shares * 99.099;                // 98,900.802
    const double sold = shares * 109.89;                  // 109,670.22
    const double buy_commission = bought * 0.0005 + 1.0;  // 50.450401
    const double sell_commission = sold * 0.0005 + 1.0;   // 55.83511
    expect_fills(result,
                 {{1, shares, 99.099, buy_commission, shares * 99.0 * 0.001, "entry"},
                  {3, -shares, 109.89, sell_commission, shares * 110.0 * 0.001, "exit"}},
                 "with costs");
    const double cash = 100'000.0 - bought - buy_commission;  // 1,048.747599
    const double final_cash = cash + sold - sell_commission;  // 110,663.132489
    expect_values(result.equity,
                  {100'000.0, cash + shares * 103.0, cash + shares * 107.0, final_cash, final_cash},
                  "equity");
    expect_cash(result, bars, {100'000.0, cash, cash, final_cash, final_cash}, "cash");
    expect_values(result.position, {0.0, shares, shares, 0.0, 0.0}, "position");
    const double pnl = sold - bought - buy_commission - sell_commission;  // 10,663.132489
    expect_value(pnl, final_cash - 100'000.0, "the worked P&L");
    expect_trades(result, {{1, 1, 3, 99.099, 109.89, shares, pnl, pnl / bought, "exit", false}}, "with costs");
    expect_totals(result,
                  {buy_commission + sell_commission, shares * (99.0 + 110.0) * 0.001, 0.0, bought + sold, 0, false},
                  "with costs");
}

// Half the equity at bar 0's close of 30 is 1,666.67 shares: 1,666 whole, or all of it with fractional shares.
void whole_and_fractional_shares() {
    const BarSeries bars = make_bars({
        {30, 31, 29, 30},
        {29, 33, 28, 32},
        {32, 34, 31, 33},
    });
    BacktestConfig config;
    config.position_size = 0.5;
    const BacktestResult whole = run(bars, directions({1, 1, 1}), config);
    expect_fills(whole, {{1, 1666.0, 29.0, 0.0, 0.0, "entry"}, {2, -1666.0, 33.0, 0.0, 0.0, "end"}}, "whole");
    // 100,000 - 1,666 * 29 = 51,686 in cash.
    expect_values(whole.equity, {100'000.0, 51'686.0 + 1666.0 * 32.0, 51'686.0 + 1666.0 * 33.0}, "whole equity");
    expect_cash(whole, bars, {100'000.0, 51'686.0, 106'664.0}, "whole cash");
    expect_values(whole.position, {0.0, 1666.0, 0.0}, "whole position");
    expect_value(whole.exposure[1], 1666.0 * 32.0 / 104'998.0, "whole exposure");
    expect_trades(whole, {{1, 1, 2, 29.0, 33.0, 1666.0, 6664.0, 4.0 / 29.0, "end", false}}, "whole");
    expect_totals(whole, {0.0, 0.0, 0.0, 1666.0 * 29.0 + 1666.0 * 33.0, 0, false}, "whole");

    config.fractional_shares = true;
    const BacktestResult fractional = run(bars, directions({1, 1, 1}), config);
    const double shares = 50'000.0 / 30.0;
    expect_fills(fractional, {{1, shares, 29.0, 0.0, 0.0, "entry"}, {2, -shares, 33.0, 0.0, 0.0, "end"}}, "fractional");
    expect_values(fractional.equity,
                  {100'000.0, 100'000.0 + 3.0 * shares, 100'000.0 + 4.0 * shares},
                  "fractional equity");
    expect_values(fractional.position, {0.0, shares, 0.0}, "fractional position");
    expect_trades(fractional, {{1, 1, 2, 29.0, 33.0, shares, 4.0 * shares, 4.0 / 29.0, "end", false}}, "fractional");
}

// Sized at bar 0's close of 100, the order meets an open of 105 and is cut to what the equity buys there.
void gap_up_trims_to_the_leverage_cap() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {105, 106, 104, 105},
        {105, 107, 104, 106},
    });
    BacktestConfig config;
    const BacktestResult whole = run(bars, directions({1, 1, 1}), config);
    // 1,000 shares ordered; 100,000 / 105 = 952.4, so 952 are bought, for 99,960.
    expect_fills(whole, {{1, 952.0, 105.0, 0.0, 0.0, "entry"}, {2, -952.0, 106.0, 0.0, 0.0, "end"}}, "whole");
    expect_values(whole.equity, {100'000.0, 100'000.0, 100'952.0}, "whole equity");
    expect_trades(whole, {{1, 1, 2, 105.0, 106.0, 952.0, 952.0, 952.0 / 99'960.0, "end", false}}, "whole");
    expect_totals(whole, {0.0, 0.0, 0.0, 952.0 * 105.0 + 952.0 * 106.0, 1, false}, "whole");

    config.fractional_shares = true;
    const BacktestResult fractional = run(bars, directions({1, 1, 1}), config);
    const double shares = 100'000.0 / 105.0;
    expect_fills(fractional,
                 {{1, shares, 105.0, 0.0, 0.0, "entry"}, {2, -shares, 106.0, 0.0, 0.0, "end"}},
                 "fractional");
    expect_values(fractional.equity, {100'000.0, 100'000.0, 100'000.0 + shares}, "fractional equity");
    check::expect_equal(fractional.trimmed, std::size_t{1}, "fractional: orders trimmed");

    // Twice the equity, on margin: 2,000 ordered, 200,000 / 105 = 1,904.8, so 1,904, and cash of -99,920.
    config.fractional_shares = false;
    config.position_size = 2.0;
    config.max_leverage = 2.0;
    const BacktestResult leveraged = run(bars, directions({1, 1, 1}), config);
    expect_fills(leveraged, {{1, 1904.0, 105.0, 0.0, 0.0, "entry"}, {2, -1904.0, 106.0, 0.0, 0.0, "end"}}, "2x");
    expect_values(leveraged.equity, {100'000.0, -99'920.0 + 1904.0 * 105.0, -99'920.0 + 1904.0 * 106.0}, "2x equity");
    expect_cash(leveraged, bars, {100'000.0, -99'920.0, -99'920.0 + 1904.0 * 106.0}, "2x cash");
    expect_trades(leveraged, {{1, 1, 2, 105.0, 106.0, 1904.0, 1904.0, 1.0 / 105.0, "end", false}}, "2x");
    check::expect_equal(leveraged.trimmed, std::size_t{1}, "2x: orders trimmed");
}

// ---- Stops ----

// Long 1,000 at 100 with a 5% stop at 95.
BarSeries stop_bars(Bar third) {
    return make_bars({
        {100, 101, 99, 100},
        {100, 102, 97, 98},
        third,
        {96, 97, 95, 96},
    });
}

void stop_loss_at_its_level_or_the_gapped_open() {
    BacktestConfig config;
    config.stop_loss = 0.05;
    // Bar 2 opens at 98 and trades down to 94: out at the stop's 95.
    const BacktestResult within = run(stop_bars({98, 99, 94, 96}), directions({1, 1, 1, 1}), config);
    expect_fills(within, {{1, 1000.0, 100.0, 0.0, 0.0, "entry"}, {2, -1000.0, 95.0, 0.0, 0.0, "stop_loss"}}, "within");
    expect_values(within.equity, {100'000.0, 98'000.0, 95'000.0, 95'000.0}, "within: equity");
    expect_values(within.position, {0.0, 1000.0, 0.0, 0.0}, "within: position");
    expect_trades(within, {{1, 1, 2, 100.0, 95.0, 1000.0, -5000.0, -0.05, "stop_loss", false}}, "within");
    expect_value(within.trades[0].mae, 0.05, "within: MAE reaches the stop");
    expect_value(within.trades[0].mfe, 0.02, "within: MFE from bar 1's high of 102");

    // Bar 2 opens at 90, through the stop: out at the open, and bar 2's low of 88 came after the trade.
    const BacktestResult gap = run(stop_bars({90, 92, 88, 91}), directions({1, 1, 1, 1}), config);
    expect_fills(gap, {{1, 1000.0, 100.0, 0.0, 0.0, "entry"}, {2, -1000.0, 90.0, 0.0, 0.0, "stop_loss"}}, "gap");
    expect_values(gap.equity, {100'000.0, 98'000.0, 90'000.0, 90'000.0}, "gap: equity");
    expect_trades(gap, {{1, 1, 2, 100.0, 90.0, 1000.0, -10'000.0, -0.1, "stop_loss", false}}, "gap");
    expect_value(gap.trades[0].mae, 0.1, "gap: MAE at the open");
}

// Long 1,000 at 100 with a 10% target at 110.
void take_profit_at_its_level_or_the_gapped_open() {
    BacktestConfig config;
    config.take_profit = 0.1;
    const auto bars_with = [](Bar third) {
        return make_bars({
            {100, 101, 99, 100},
            {100, 105, 99, 104},
            third,
            {111, 112, 110, 111},
        });
    };
    const BacktestResult within = run(bars_with({106, 112, 105, 111}), directions({1, 1, 1, 1}), config);
    expect_fills(within,
                 {{1, 1000.0, 100.0, 0.0, 0.0, "entry"}, {2, -1000.0, 110.0, 0.0, 0.0, "take_profit"}},
                 "within");
    expect_values(within.equity, {100'000.0, 104'000.0, 110'000.0, 110'000.0}, "within: equity");
    expect_trades(within, {{1, 1, 2, 100.0, 110.0, 1000.0, 10'000.0, 0.1, "take_profit", false}}, "within");

    const BacktestResult gap = run(bars_with({115, 118, 114, 116}), directions({1, 1, 1, 1}), config);
    expect_fills(gap, {{1, 1000.0, 100.0, 0.0, 0.0, "entry"}, {2, -1000.0, 115.0, 0.0, 0.0, "take_profit"}}, "gap");
    expect_values(gap.equity, {100'000.0, 104'000.0, 115'000.0, 115'000.0}, "gap: equity");
    expect_trades(gap, {{1, 1, 2, 100.0, 115.0, 1000.0, 15'000.0, 0.15, "take_profit", false}}, "gap");
}

// A 5% stop and a 5% target, and bar 2 reaches both: the order within the bar is unknown, so the stop came first.
void a_bar_reaching_both_levels_takes_the_stop() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 104, 96, 100},
        {100, 106, 94, 105},
    });
    BacktestConfig config;
    config.stop_loss = 0.05;
    config.take_profit = 0.05;
    config.allow_short = true;
    const BacktestResult long_side = run(bars, directions({1, 1, 1}), config);
    expect_fills(long_side, {{1, 1000.0, 100.0, 0.0, 0.0, "entry"}, {2, -1000.0, 95.0, 0.0, 0.0, "stop_loss"}}, "long");
    expect_values(long_side.equity, {100'000.0, 100'000.0, 95'000.0}, "long: equity");
    expect_trades(long_side, {{1, 1, 2, 100.0, 95.0, 1000.0, -5000.0, -0.05, "stop_loss", false}}, "long");

    const BacktestResult short_side = run(bars, directions({-1, -1, -1}), config);
    expect_fills(short_side,
                 {{1, -1000.0, 100.0, 0.0, 0.0, "entry"}, {2, 1000.0, 105.0, 0.0, 0.0, "stop_loss"}},
                 "short");
    expect_values(short_side.equity, {100'000.0, 100'000.0, 95'000.0}, "short: equity");
    expect_trades(short_side, {{-1, 1, 2, 100.0, 105.0, 1000.0, -5000.0, -0.05, "stop_loss", false}}, "short");
}

// A 10% trailing stop on a long bought at 100. Each bar's level comes from the highs before it: 90 on bar 1 (the
// entry), 93.6 on bar 2, 99 on bar 3, and 108 on bars 4 and 5, from bar 3's high of 120 even though bar 4's high is
// lower. Bar 5 trades at 107: out at 108.
void trailing_stop_follows_the_highest_high() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 104, 95, 103},
        {103, 110, 94, 108},
        {108, 120, 100, 118},
        {117, 118, 109, 110},
        {110, 111, 107, 108},
        {108, 109, 107, 108},
    });
    BacktestConfig config;
    config.trailing_stop = 0.1;
    const BacktestResult result = run(bars, directions({1, 1, 1, 1, 1, 1, 1}), config);
    expect_fills(result,
                 {{1, 1000.0, 100.0, 0.0, 0.0, "entry"}, {5, -1000.0, 108.0, 0.0, 0.0, "trailing_stop"}},
                 "long");
    expect_values(result.equity,
                  {100'000.0, 103'000.0, 108'000.0, 118'000.0, 110'000.0, 108'000.0, 108'000.0},
                  "equity");
    expect_values(result.position, {0.0, 1000.0, 1000.0, 1000.0, 1000.0, 0.0, 0.0}, "position");
    expect_trades(result, {{1, 1, 5, 100.0, 108.0, 1000.0, 8000.0, 0.08, "trailing_stop", false}}, "long");
    expect_value(result.trades[0].mae, 0.06, "MAE from bar 2's low of 94");
    expect_value(result.trades[0].mfe, 0.2, "MFE from bar 3's high of 120");
}

// A 10% trailing stop on a short sold at 100: levels of 110, 105.6 (bar 1's low of 96), then 99 from bar 2's low of
// 90, kept on bar 4 though bar 3's low is 91. Bar 4 trades at 100: out at 99.
void short_trailing_stop_follows_the_lowest_low() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 104, 96, 97},
        {97, 105, 90, 91},
        {92, 98, 91, 97},
        {97, 100, 95, 99},
        {99, 100, 98, 99},
    });
    BacktestConfig config;
    config.allow_short = true;
    config.trailing_stop = 0.1;
    const BacktestResult result = run(bars, directions({-1, -1, -1, -1, -1, -1}), config);
    expect_fills(result,
                 {{1, -1000.0, 100.0, 0.0, 0.0, "entry"}, {4, 1000.0, 99.0, 0.0, 0.0, "trailing_stop"}},
                 "short");
    // Short 1,000 at 100 holds 200,000 in cash.
    expect_values(result.equity, {100'000.0, 103'000.0, 109'000.0, 103'000.0, 101'000.0, 101'000.0}, "equity");
    expect_values(result.position, {0.0, -1000.0, -1000.0, -1000.0, 0.0, 0.0}, "position");
    expect_trades(result, {{-1, 1, 4, 100.0, 99.0, 1000.0, 1000.0, 0.01, "trailing_stop", false}}, "short");
    expect_value(result.trades[0].mae, 0.05, "MAE from bar 2's high of 105");
    expect_value(result.trades[0].mfe, 0.1, "MFE from bar 2's low of 90");
}

// Stopped out on bar 2 while the signal stays long: no new long until the signal leaves long (bar 4) and comes back
// (bar 5), filling at bar 6's open. A short signal after the stop enters at once.
void no_reentry_after_a_stop_until_the_signal_changes() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 102, 97, 98},
        {98, 99, 94, 96},
        {96, 97, 95, 96},
        {96, 98, 95, 97},
        {97, 99, 96, 98},
        {98, 100, 97, 99},
        {99, 100, 98, 100},
    });
    BacktestConfig config;
    config.stop_loss = 0.05;
    const BacktestResult result = run(bars, directions({1, 1, 1, 1, 0, 1, 1, 1}), config);
    // 95,000 / 98 = 969.4 shares at bar 5's close; bought at 98 for 94,962, leaving 38.
    expect_fills(result,
                 {{1, 1000.0, 100.0, 0.0, 0.0, "entry"},
                  {2, -1000.0, 95.0, 0.0, 0.0, "stop_loss"},
                  {6, 969.0, 98.0, 0.0, 0.0, "entry"},
                  {7, -969.0, 100.0, 0.0, 0.0, "end"}},
                 "long");
    expect_values(result.equity,
                  {100'000.0, 98'000.0, 95'000.0, 95'000.0, 95'000.0, 95'000.0, 38.0 + 969.0 * 99.0, 38.0 + 96'900.0},
                  "equity");
    expect_values(result.position, {0.0, 1000.0, 0.0, 0.0, 0.0, 0.0, 969.0, 0.0}, "position");
    expect_trades(result,
                  {{1, 1, 2, 100.0, 95.0, 1000.0, -5000.0, -0.05, "stop_loss", false},
                   {1, 6, 7, 98.0, 100.0, 969.0, 1938.0, 2.0 / 98.0, "end", false}},
                  "long");

    // 95,000 / 96 = 989.6: short 989 at bar 3's open.
    config.allow_short = true;
    const BarSeries first_four = make_bars({
        {100, 101, 99, 100},
        {100, 102, 97, 98},
        {98, 99, 94, 96},
        {96, 97, 95, 96},
    });
    const BacktestResult reversed = run(first_four, directions({1, 1, -1, -1}), config);
    expect_fills(reversed,
                 {{1, 1000.0, 100.0, 0.0, 0.0, "entry"},
                  {2, -1000.0, 95.0, 0.0, 0.0, "stop_loss"},
                  {3, -989.0, 96.0, 0.0, 0.0, "entry"},
                  {3, 989.0, 96.0, 0.0, 0.0, "end"}},
                 "short after a long stop");
    expect_values(reversed.equity, {100'000.0, 98'000.0, 95'000.0, 95'000.0}, "short after a long stop: equity");
}

// ---- Shorts and reversals ----

// Short 1,000 at 100 with borrowing at 10% a year over 100 bars a year: 0.1% of the short's value at each close,
// 96 at bar 1's close of 96 and 92 at bar 2's.
void short_profit_and_borrowing() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 101, 95, 96},
        {96, 97, 90, 92},
        {93, 94, 91, 92},
    });
    BacktestConfig config;
    config.allow_short = true;
    config.borrow_rate = 0.1;
    config.bars_per_year = 100.0;
    const BacktestResult result = run(bars, directions({-1, -1, 0, 0}), config);
    check::expect_equal(result.bars_per_year, 100.0, "bars per year, as configured");
    expect_fills(result, {{1, -1000.0, 100.0, 0.0, 0.0, "entry"}, {3, 1000.0, 93.0, 0.0, 0.0, "exit"}}, "covered");
    // Cash: 200,000 from the sale, less 96, less 92, less 93,000 to buy back.
    expect_values(result.equity, {100'000.0, 199'904.0 - 96'000.0, 199'812.0 - 92'000.0, 106'812.0}, "covered: equity");
    expect_cash(result, bars, {100'000.0, 199'904.0, 199'812.0, 106'812.0}, "covered: cash");
    expect_values(result.position, {0.0, -1000.0, -1000.0, 0.0}, "covered: position");
    expect_values(result.exposure, {0.0, -96'000.0 / 103'904.0, -92'000.0 / 107'812.0, 0.0}, "covered: exposure");
    expect_trades(result, {{-1, 1, 3, 100.0, 93.0, 1000.0, 6812.0, 0.06812, "exit", false}}, "covered");
    expect_totals(result, {0.0, 0.0, 188.0, 100'000.0 + 93'000.0, 0, false}, "covered");

    // Held to the end: borrowing is paid at the last close too, before the buy back at 92.
    const BacktestResult held = run(bars, directions({-1, -1, -1, -1}), config);
    expect_fills(held, {{1, -1000.0, 100.0, 0.0, 0.0, "entry"}, {3, 1000.0, 92.0, 0.0, 0.0, "end"}}, "held");
    expect_values(held.equity, {100'000.0, 103'904.0, 107'812.0, 107'720.0}, "held: equity");
    expect_trades(held, {{-1, 1, 3, 100.0, 92.0, 1000.0, 7720.0, 0.0772, "end", false}}, "held");
    expect_value(held.borrow, 96.0 + 92.0 + 92.0, "held: borrowing");
}

// Half the equity long, then short on bar 1's signal: one fill sells 986 shares at 104, closing the 499 long and
// opening a 487 short, and its fixed commission of 10 is split between the two trades by shares, 499 to 487. Each
// order takes half of what the equity leaves after the fixed commission.
void reversal_closes_one_trade_and_opens_another() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 106, 99, 105},
        {104, 105, 100, 101},
        {100, 102, 98, 99},
    });
    BacktestConfig config;
    config.allow_short = true;
    config.position_size = 0.5;
    config.commission_fixed = 10.0;
    const BacktestResult result = run(bars, directions({1, -1, -1, -1}), config);
    // Bar 0: 0.5 * (100,000 - 10) / 100 = 49,995 / 100 = 499.95 -> 499 long, bought at 100 for 49,900 plus 10.
    // Bar 1: cash 100,000 - 49,900 - 10 = 50,090, equity 50,090 + 499 * 105 = 102,485, so
    // 0.5 * (102,485 - 10) / 105 = 51,237.5 / 105 = 487.98 -> 487 short. At bar 2's open the equity is
    // 50,090 + 499 * 104 = 101,986, room for (101,986 - 10) / 104 = 980.5 shares, so nothing is trimmed.
    expect_fills(result,
                 {{1, 499.0, 100.0, 10.0, 0.0, "entry"},
                  {2, -986.0, 104.0, 10.0, 0.0, "reverse"},
                  {3, 487.0, 99.0, 10.0, 0.0, "end"}},
                 "reversal");
    // Bar 2: cash 50,090 + 986 * 104 - 10 = 152,624. Bar 3: 152,624 - 487 * 99 - 10 = 104,401.
    expect_values(result.equity, {100'000.0, 102'485.0, 152'624.0 - 487.0 * 101.0, 104'401.0}, "equity");
    expect_cash(result, bars, {100'000.0, 50'090.0, 152'624.0, 104'401.0}, "cash");
    expect_values(result.position, {0.0, 499.0, -487.0, 0.0}, "position");
    const double long_pnl = 499.0 * 4.0 - 10.0 - 10.0 * 499.0 / 986.0;   // 1,980.94
    const double short_pnl = 487.0 * 5.0 - 10.0 * 487.0 / 986.0 - 10.0;  // 2,420.06
    expect_value(long_pnl + short_pnl, 4401.0, "the worked P&L");
    expect_trades(result,
                  {{1, 1, 2, 100.0, 104.0, 499.0, long_pnl, long_pnl / 49'900.0, "reverse", false},
                   {-1, 2, 3, 104.0, 99.0, 487.0, short_pnl, short_pnl / (104.0 * 487.0), "end", false}},
                  "reversal");
    expect_totals(result, {30.0, 0.0, 0.0, 49'900.0 + 986.0 * 104.0 + 487.0 * 99.0, 0, false}, "reversal");
}

// ---- Volatility sizing ----

// Two-bar windows of returns at 400 bars a year, so the realized volatility is 20 times the population deviation of
// the last two returns, and a 10% target. Returns: +1%, -1%, +1%, -3%.
BarSeries volatility_bars() {
    return make_bars({
        {100, 100.5, 99.5, 100},
        {100.5, 101.5, 100, 101},
        {101, 101, 99.5, 99.99},
        {100, 101.5, 99.8, 100.9899},
        {100.5, 101, 97.5, 97.960203},
        {98, 99, 97.5, 98.5},
    });
}

BacktestConfig volatility_config() {
    BacktestConfig config;
    config.sizing = Sizing::Volatility;
    config.volatility_window = 2;
    config.bars_per_year = 400.0;
    config.target_volatility = 0.1;
    return config;
}

void volatility_sizing_and_rebalancing() {
    const Directions always_long = directions({1, 1, 1, 1, 1, 1});
    const BarSeries bars = volatility_bars();
    const BacktestResult result = run(bars, always_long, volatility_config());
    // Bars 0 and 1 have no volatility yet. Bar 2: returns of +1% and -1%, a deviation of 1%, 20% a year, so half the
    // equity: 0.5 * 100,000 / 99.99 = 500.05 -> 500 shares, bought at 100.
    // Bar 3: still 20%, so 0.5 * 100,494.95 / 100.9899 = 497.5 -> 497, within 25% of 500: no order.
    // Bar 4: +1% and -3%, 40% a year, a quarter of 98,980.1015 / 97.960203 = 252.6 -> 252, beyond 25%: sell 248 at
    // bar 5's open of 98.
    expect_fills(result,
                 {{3, 500.0, 100.0, 0.0, 0.0, "entry"},
                  {5, -248.0, 98.0, 0.0, 0.0, "rebalance"},
                  {5, -252.0, 98.5, 0.0, 0.0, "end"}},
                 "threshold 25%");
    expect_values(result.equity,
                  {100'000.0, 100'000.0, 100'000.0, 100'494.95, 98'980.1015, 74'304.0 + 252.0 * 98.5},
                  "threshold 25%: equity");
    expect_cash(result, bars, {100'000.0, 100'000.0, 100'000.0, 50'000.0, 50'000.0, 99'126.0}, "threshold 25%: cash");
    expect_values(result.position, {0.0, 0.0, 0.0, 500.0, 500.0, 0.0}, "threshold 25%: position");
    // One trade, out over two fills averaging (248 * 98 + 252 * 98.5) / 500 = 98.252.
    expect_trades(result, {{1, 3, 5, 100.0, 98.252, 500.0, -874.0, -874.0 / 50'000.0, "end", false}}, "threshold 25%");

    // With no threshold every change of size trades: 497 at bar 3 sells 3 at 100.5; then a quarter of 98,987.720891
    // / 97.960203 = 252.6 -> 252 sells 245 more at 98.
    BacktestConfig config = volatility_config();
    config.rebalance_threshold = 0.0;
    const BacktestResult eager = run(bars, always_long, config);
    expect_fills(eager,
                 {{3, 500.0, 100.0, 0.0, 0.0, "entry"},
                  {4, -3.0, 100.5, 0.0, 0.0, "rebalance"},
                  {5, -245.0, 98.0, 0.0, 0.0, "rebalance"},
                  {5, -252.0, 98.5, 0.0, 0.0, "end"}},
                 "threshold 0");
    expect_values(eager.equity,
                  {100'000.0, 100'000.0, 100'000.0, 100'494.95, 98'987.720891, 99'133.5},
                  "threshold 0: equity");
    expect_cash(eager, bars, {100'000.0, 100'000.0, 100'000.0, 50'000.0, 50'301.5, 99'133.5}, "threshold 0: cash");
    expect_trades(eager,
                  {{1, 3, 5, 100.0, 49'133.5 / 500.0, 500.0, -866.5, -866.5 / 50'000.0, "end", false}},
                  "threshold 0");
}

// Returns of +0.1% and -0.1% are 2% a year at 400 bars a year: a 10% target asks for 5 times the equity, which the
// cap holds to max_leverage.
void volatility_sizing_is_capped() {
    const BarSeries bars = make_bars({
        {100, 100, 100, 100},
        {100, 100.1, 100, 100.1},
        {100, 100.1, 99.9, 99.9999},
        {99.5, 100, 99, 99.8},
    });
    BacktestConfig config = volatility_config();
    const BacktestResult once = run(bars, directions({1, 1, 1, 1}), config);
    // 100,000 / 99.9999 = 1,000.001 -> 1,000, bought at 99.5 and sold at 99.8.
    expect_fills(once, {{3, 1000.0, 99.5, 0.0, 0.0, "entry"}, {3, -1000.0, 99.8, 0.0, 0.0, "end"}}, "1x");
    expect_values(once.equity, {100'000.0, 100'000.0, 100'000.0, 100'300.0}, "1x: equity");

    config.max_leverage = 2.0;
    const BacktestResult twice = run(bars, directions({1, 1, 1, 1}), config);
    expect_fills(twice, {{3, 2000.0, 99.5, 0.0, 0.0, "entry"}, {3, -2000.0, 99.8, 0.0, 0.0, "end"}}, "2x");
    expect_values(twice.equity, {100'000.0, 100'000.0, 100'000.0, 100'600.0}, "2x: equity");
}

// ---- The window and the end ----

void open_at_the_end_without_close_at_end() {
    BacktestConfig config;
    config.close_at_end = false;
    config.commission_fixed = 5.0;
    const BarSeries bars = rising_bars();
    const BacktestResult result = run(bars, directions({1, 1, 1, 1, 1}), config);
    // Sized at bar 0's close to cover the fixed commission: (100,000 - 5) / 100 = 999.95 -> 999 shares.
    expect_fills(result, {{1, 999.0, 99.0, 5.0, 0.0, "entry"}}, "open");
    // Cash 100,000 - 999 * 99 - 5 = 100,000 - 98,901 - 5 = 1,094.
    expect_values(
        result.equity,
        {100'000.0, 1094.0 + 999.0 * 103.0, 1094.0 + 999.0 * 107.0, 1094.0 + 999.0 * 109.0, 1094.0 + 999.0 * 107.0},
        "equity");
    expect_cash(result, bars, {100'000.0, 1094.0, 1094.0, 1094.0, 1094.0}, "cash");
    expect_values(result.position, {0.0, 999.0, 999.0, 999.0, 999.0}, "position");
    // Marked at the last close of 107, after the entry's commission: 999 * (107 - 99) - 5 = 7,987.
    expect_trades(result, {{1, 1, 4, 99.0, 107.0, 999.0, 7987.0, 7987.0 / (99.0 * 999.0), "open", true}}, "open");
}

// Bars 0 and 1 only warm up: the signal at bar 1 fills at bar 2's open, and bar 0's never trades.
void begin_fills_the_signal_before_it() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {101, 103, 100, 102},
        {101, 104, 100, 103},
        {104, 105, 102, 104},
        {103, 104, 101, 102},
    });
    BacktestConfig config;
    config.allow_short = true;
    const BacktestResult result = nanotrade::run_backtest(bars, directions({-1, 1, 1, 0, 0}), config, 2, 5);
    check::expect_equal(result.begin, std::size_t{2}, "begin");
    check::expect_equal(result.end, std::size_t{5}, "end");
    // 100,000 / 102 = 980.4 at bar 1's close, bought at 101 for 98,980, leaving 1,020.
    expect_fills(result, {{2, 980.0, 101.0, 0.0, 0.0, "entry"}, {4, -980.0, 103.0, 0.0, 0.0, "exit"}}, "from bar 2");
    expect_values(result.equity, {1020.0 + 980.0 * 103.0, 1020.0 + 980.0 * 104.0, 1020.0 + 980.0 * 103.0}, "equity");
    expect_cash(result, bars, {1020.0, 1020.0, 1020.0 + 980.0 * 103.0}, "cash");
    expect_values(result.position, {980.0, 980.0, 0.0}, "position");
    expect_trades(result, {{1, 2, 4, 101.0, 103.0, 980.0, 1960.0, 1960.0 / 98'980.0, "exit", false}}, "from bar 2");

    // Flat at bar 1, so nothing at bar 2; long at bar 2 buys 50,000 / 103 = 485 at bar 3's open, and the window ends
    // there, at bar 3's close.
    config.position_size = 0.5;
    const BacktestResult inner = nanotrade::run_backtest(bars, directions({1, 0, 1, 1, 1}), config, 2, 4);
    expect_fills(inner, {{3, 485.0, 104.0, 0.0, 0.0, "entry"}, {3, -485.0, 104.0, 0.0, 0.0, "end"}}, "bars 2 to 3");
    expect_values(inner.equity, {100'000.0, 100'000.0}, "bars 2 to 3: equity");
}

// Short 1,000 at 100 with 100,000: at a close of 200 the equity is 200,000 - 200,000 = 0. The short is bought back
// there and nothing trades again, whatever the signal says.
void ruin_stops_trading() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 150, 99, 150},
        {160, 210, 155, 200},
        {190, 195, 180, 185},
        {180, 185, 170, 175},
    });
    BacktestConfig config;
    config.allow_short = true;
    const BacktestResult result = run(bars, directions({-1, -1, -1, 1, 1}), config);
    expect_fills(result, {{1, -1000.0, 100.0, 0.0, 0.0, "entry"}, {2, 1000.0, 200.0, 0.0, 0.0, "ruin"}}, "ruin");
    expect_values(result.equity, {100'000.0, 50'000.0, 0.0, 0.0, 0.0}, "equity");
    expect_cash(result, bars, {100'000.0, 200'000.0, 0.0, 0.0, 0.0}, "cash");
    expect_values(result.position, {0.0, -1000.0, 0.0, 0.0, 0.0}, "position");
    expect_values(result.exposure, {0.0, -150'000.0 / 50'000.0, 0.0, 0.0, 0.0}, "exposure");
    expect_trades(result, {{-1, 1, 2, 100.0, 200.0, 1000.0, -100'000.0, -1.0, "ruin", false}}, "ruin");
    check::expect(result.ruined, "ruined");
}

// ---- Excursions ----

// In at 100, held through bars 1 to 3 (highs 103, 108, 106; lows 97, 99, 95), out at bar 4's open of 104.
void maximum_adverse_and_favorable_excursions() {
    const BarSeries bars = make_bars({
        {100, 101, 99, 100},
        {100, 103, 97, 102},
        {102, 108, 99, 106},
        {106, 106, 95, 98},
        {104, 105, 103, 104},
    });
    BacktestConfig config;
    config.allow_short = true;
    const BacktestResult long_side = run(bars, directions({1, 1, 1, 0, 0}), config);
    expect_trades(long_side, {{1, 1, 4, 100.0, 104.0, 1000.0, 4000.0, 0.04, "exit", false}}, "long");
    expect_value(long_side.trades[0].mae, 0.05, "long MAE, from the low of 95");
    expect_value(long_side.trades[0].mfe, 0.08, "long MFE, from the high of 108");

    const BacktestResult short_side = run(bars, directions({-1, -1, -1, 0, 0}), config);
    expect_trades(short_side, {{-1, 1, 4, 100.0, 104.0, 1000.0, -4000.0, -0.04, "exit", false}}, "short");
    expect_value(short_side.trades[0].mae, 0.08, "short MAE, from the high of 108");
    expect_value(short_side.trades[0].mfe, 0.05, "short MFE, from the low of 95");
}

// ---- Bugs found by these tests ----

// Bar 2 gaps to `open` from bar 1's range of 99 to 101, and trades within a point of it.
BarSeries gap_bars(double open) {
    return make_bars({
        {100, 101, 99, 100},
        {100, 101, 99, 100},
        {open, open + 1.0, open - 1.0, open},
    });
}

// A trade's excursions reach at least as far as its exit, since the position was held at that price. A stop's exit
// always counted, but an exit at the open on the signal didn't: a long bought at 100 and sold at a gapped open of 90
// lost 10% and showed an MAE of 1%, from bar 1's low. fill_pending now notes the open before it fills. The other
// excursion of each trade is bar 1's 1%.
void a_signal_exit_at_a_gapped_open_counts_in_the_excursions() {
    struct Gap {
        int side;
        double open;
        double mae;
        double mfe;
    };
    for (const Gap& gap :
         {Gap{1, 90.0, 0.1, 0.01}, Gap{1, 110.0, 0.01, 0.1}, Gap{-1, 110.0, 0.1, 0.01}, Gap{-1, 90.0, 0.01, 0.1}}) {
        BacktestConfig config;
        config.allow_short = true;
        const BacktestResult result = run(gap_bars(gap.open), directions({gap.side, 0, 0}), config);
        const std::string what = std::string(gap.side > 0 ? "long" : "short") + " out at " + text_of(gap.open);
        const double pnl = gap.side * 1000.0 * (gap.open - 100.0);
        expect_fills(
            result,
            {{1, gap.side * 1000.0, 100.0, 0.0, 0.0, "entry"}, {2, -gap.side * 1000.0, gap.open, 0.0, 0.0, "exit"}},
            what);
        expect_trades(result, {{gap.side, 1, 2, 100.0, gap.open, 1000.0, pnl, pnl / 100'000.0, "exit", false}}, what);
        expect_value(result.trades[0].mae, gap.mae, what + ": MAE");
        expect_value(result.trades[0].mfe, gap.mfe, what + ": MFE");
        expect_values(result.equity, {100'000.0, 100'000.0, 100'000.0 + pnl}, what + ": equity");
    }
}

// The same for a reversal at the open, which closes one trade there and opens the other way. The new trade's
// excursions start from its entry: a point either way within bar 2. Out of a short at 110, the equity at the open is
// 200,000 - 1,000 * 110 = 90,000, room for 90,000 / 110 = 818.2 -> 818 shares long, so that order is trimmed.
void a_reversal_at_a_gapped_open_counts_in_the_excursions() {
    struct Gap {
        int side;  // of the trade reversed
        double open;
        double mae;  // of the trade reversed
        double mfe;
        double shares;  // of the trade opened
        std::size_t trimmed;
    };
    for (const Gap& gap : {Gap{1, 90.0, 0.1, 0.01, 1000.0, 0},
                           Gap{1, 110.0, 0.01, 0.1, 1000.0, 0},
                           Gap{-1, 110.0, 0.1, 0.01, 818.0, 1},
                           Gap{-1, 90.0, 0.01, 0.1, 1000.0, 0}}) {
        BacktestConfig config;
        config.allow_short = true;
        const BacktestResult result = run(gap_bars(gap.open), directions({gap.side, -gap.side, -gap.side}), config);
        const std::string what = std::string(gap.side > 0 ? "long" : "short") + " reversed at " + text_of(gap.open);
        const double pnl = gap.side * 1000.0 * (gap.open - 100.0);
        expect_fills(result,
                     {{1, gap.side * 1000.0, 100.0, 0.0, 0.0, "entry"},
                      {2, -gap.side * (1000.0 + gap.shares), gap.open, 0.0, 0.0, "reverse"},
                      {2, gap.side * gap.shares, gap.open, 0.0, 0.0, "end"}},
                     what);
        expect_trades(result,
                      {{gap.side, 1, 2, 100.0, gap.open, 1000.0, pnl, pnl / 100'000.0, "reverse", false},
                       {-gap.side, 2, 2, gap.open, gap.open, gap.shares, 0.0, 0.0, "end", false}},
                      what);
        expect_value(result.trades[0].mae, gap.mae, what + ": MAE of the trade reversed");
        expect_value(result.trades[0].mfe, gap.mfe, what + ": MFE of the trade reversed");
        expect_value(result.trades[1].mae, 1.0 / gap.open, what + ": MAE of the trade opened");
        expect_value(result.trades[1].mfe, 1.0 / gap.open, what + ": MFE of the trade opened");
        expect_values(result.equity, {100'000.0, 100'000.0, 100'000.0 + pnl}, what + ": equity");
        check::expect_equal(result.trimmed, gap.trimmed, what + ": orders trimmed");
    }
}

// result.trimmed counts orders cut down to fit the leverage cap after the price gapped, but orders were sized at the
// close without their costs while the cap at the fill counted them, so with position_size at the cap any commission
// or slippage trimmed every full-size entry, gap or no gap. Orders are now sized as the cap is reckoned: with 5 bps
// of commission, 100,000 / (100 * 1.0005) = 999.5 -> 999 shares, which fit at any open up to
// 100,000 / (999 * 1.0005) = 100.05. An open of 100, the signal's close, trims nothing, nor do 99 and 100.04; at 101
// only 100,000 / (101 * 1.0005) = 989.6 -> 989 fit.
void only_a_gap_trims_an_order() {
    struct Gap {
        double open;
        double shares;
        std::size_t trimmed;
    };
    for (const Gap& gap : {Gap{100.0, 999.0, 0}, Gap{99.0, 999.0, 0}, Gap{100.04, 999.0, 0}, Gap{101.0, 989.0, 1}}) {
        const BarSeries bars = make_bars({
            {100, 101, 99, 100},
            {gap.open, 102, 99, 100},
            {100, 101, 99, 100},
        });
        BacktestConfig config;
        config.commission_bps = 5.0;
        const BacktestResult result = run(bars, directions({1, 1, 1}), config);
        const std::string what = "an open of " + text_of(gap.open);
        const double bought = gap.shares * gap.open;
        const double sold = gap.shares * 100.0;
        const double cash = 100'000.0 - bought - bought * 0.0005;
        expect_fills(result,
                     {{1, gap.shares, gap.open, bought * 0.0005, 0.0, "entry"},
                      {2, -gap.shares, 100.0, sold * 0.0005, 0.0, "end"}},
                     what);
        expect_values(result.equity, {100'000.0, cash + sold, cash + sold - sold * 0.0005}, what + ": equity");
        check::expect_equal(result.trimmed, gap.trimmed, what + ": orders trimmed");
    }
}

// Four bars that never move from 100.
BarSeries flat_bars() {
    return make_bars({
        {100, 101, 99, 100},
        {100, 101, 99, 100},
        {100, 101, 99, 100},
        {100, 101, 99, 100},
    });
}

// Long at bar 0's close, reversed at bar 1's into a short held through bar 2's close.
void expect_reversal_within_the_cap(const BacktestResult& result, const std::string& what) {
    check::expect(result.exposure[2] >= -1.0 - 1e-12,
                  what + ": the exposure after the reversal, " + text_of(result.exposure[2]) + ", passes -1");
    check::expect_equal(result.trimmed, std::size_t{0}, what + ": orders trimmed");
}

// A reversal closes the old position and opens the new one in one fill, but was sized as if only the new side paid
// costs: reversing a long of 999 shares at 5 bps left a short worth 99,900 against an equity of 99,850.15, an
// exposure of -1.0005 with the price unchanged. The order now also budgets the cost of closing, out of the same
// equity.
void a_reversal_budgets_the_cost_of_closing() {
    const BarSeries bars = flat_bars();
    BacktestConfig config;
    config.allow_short = true;
    config.commission_bps = 5.0;
    const BacktestResult result = run(bars, directions({1, -1, -1, -1}), config);
    // Long: 100,000 / (100 * 1.0005) = 999.5 -> 999, for 99,900 and 49.95 of commission, leaving 50.05. At bar 1's
    // close the equity is 50.05 + 99,900 = 99,950.05, closing the 999 costs 999 * 100 * 0.0005 = 49.95, and the short
    // is (99,950.05 - 49.95) / (100 * 1.0005) = 99,900.10 / 100.05 = 998.50 -> 998. One fill sells 999 + 998 = 1,997
    // for 199,700, less 99.85 of commission.
    expect_fills(result,
                 {{1, 999.0, 100.0, 49.95, 0.0, "entry"},
                  {2, -1997.0, 100.0, 99.85, 0.0, "reverse"},
                  {3, 998.0, 100.0, 49.9, 0.0, "end"}},
                 "5 bps");
    // Cash 50.05, then 50.05 + 199,700 - 99.85 = 199,650.20, then 199,650.20 - 99,800 - 49.90 = 99,800.30.
    expect_cash(result, bars, {100'000.0, 50.05, 199'650.2, 99'800.3}, "5 bps: cash");
    expect_values(result.equity, {100'000.0, 99'950.05, 199'650.2 - 99'800.0, 99'800.3}, "5 bps: equity");
    expect_values(result.exposure, {0.0, 99'900.0 / 99'950.05, -99'800.0 / 99'850.2, 0.0}, "5 bps: exposure");
    // Each trade pays 0.05 a share in and 0.05 a share out: its own fill's, or its part of the reversal's.
    expect_trades(result,
                  {{1, 1, 2, 100.0, 100.0, 999.0, -99.9, -0.001, "reverse", false},
                   {-1, 2, 3, 100.0, 100.0, 998.0, -99.8, -0.001, "end", false}},
                  "5 bps");
    expect_reversal_within_the_cap(result, "5 bps");

    // In fractional shares the short is all of the 1.0 wanted: its value is the equity left after the fill, exactly.
    config.fractional_shares = true;
    const BacktestResult fractional = run(bars, directions({1, -1, -1, -1}), config);
    expect_reversal_within_the_cap(fractional, "5 bps, fractional");
    expect_value(fractional.exposure[2], -1.0, "5 bps, fractional: the exposure after the reversal");
}

// With 10 bps of slippage as well, each share costs 100 * 1.001 * 1.0005 = 100.15005 to trade.
void a_reversal_budgets_the_cost_of_closing_with_slippage() {
    const BarSeries bars = flat_bars();
    BacktestConfig config;
    config.allow_short = true;
    config.commission_bps = 5.0;
    config.slippage_bps = 10.0;
    const BacktestResult result = run(bars, directions({1, -1, -1, -1}), config);
    // Long: 100,000 / 100.15005 = 998.50 -> 998 at 100.1, for 99,899.80 and 49.9499 of commission, leaving 50.2501.
    // At bar 1's close the equity is 50.2501 + 99,800 = 99,850.2501, closing the 998 costs
    // 998 * 100 * (1.0015005 - 1) = 149.7499, and the short is (99,850.2501 - 149.7499) / 100.15005
    // = 99,700.5002 / 100.15005 = 995.51 -> 995. One fill sells 998 + 995 = 1,993 at 99.9 for 199,100.70, less
    // 0.0005 of that, 99.55035, of commission.
    expect_fills(result,
                 {{1, 998.0, 100.1, 49.9499, 99.8, "entry"},
                  {2, -1993.0, 99.9, 99.55035, 199.3, "reverse"},
                  {3, 995.0, 100.1, 49.79975, 99.5, "end"}},
                 "with slippage");
    // Cash 50.2501, then 50.2501 + 199,100.70 - 99.55035 = 199,051.39975, then less 995 * 100.1 = 99,599.50 and
    // 49.79975 of commission, 99,402.10.
    expect_cash(result, bars, {100'000.0, 50.2501, 199'051.39975, 99'402.1}, "with slippage: cash");
    expect_values(result.equity, {100'000.0, 99'850.2501, 199'051.39975 - 99'500.0, 99'402.1}, "with slippage: equity");
    expect_values(result.exposure,
                  {0.0, 99'800.0 / 99'850.2501, -99'500.0 / 99'551.39975, 0.0},
                  "with slippage: exposure");
    // The long: 998 * (99.9 - 100.1) - 49.9499 - 998 * 99.9 * 0.0005 = -299.40. The short: 995 * (99.9 - 100.1)
    // - 995 * 99.9 * 0.0005 - 49.79975 = -298.50.
    expect_trades(result,
                  {{1, 1, 2, 100.1, 99.9, 998.0, -299.4, -299.4 / (998.0 * 100.1), "reverse", false},
                   {-1, 2, 3, 99.9, 100.1, 995.0, -298.5, -298.5 / (995.0 * 99.9), "end", false}},
                  "with slippage");
    expect_totals(result,
                  {49.9499 + 99.55035 + 49.79975,
                   99.8 + 199.3 + 99.5,
                   0.0,
                   998.0 * 100.1 + 1993.0 * 99.9 + 995.0 * 100.1,
                   0,
                   false},
                  "with slippage");
    expect_reversal_within_the_cap(result, "with slippage");

    // In fractional shares the budget reckons each share sold at 0.0015005 of its price in costs, as for a buy, where a
    // sale pays 1 - 0.999 * 0.9995 = 0.0014995, so the short stays just inside -1. With n shares long, the equity at
    // bar 1's close is 100 n, the short is (100 n - 100 n * 0.0015005) / 100.15005 = n * 0.9984995 / 1.0015005, and
    // the equity after the reversal is the short's value plus the 0.000001 saved on each of the
    // n + n * 0.9984995 / 1.0015005 = 2 n / 1.0015005 shares sold at 100: an exposure of
    // -0.9984995 / (0.9984995 + 2 * 0.000001).
    config.fractional_shares = true;
    const BacktestResult fractional = run(bars, directions({1, -1, -1, -1}), config);
    expect_reversal_within_the_cap(fractional, "with slippage, fractional");
    expect_value(fractional.exposure[2],
                 -0.9984995 / (0.9984995 + 2.0 * 0.000001),
                 "with slippage, fractional: the exposure after the reversal");
}

// Sizing used to scale the equity by the exposure wanted and then take the costs out at their unscaled rate, so above
// 1x a position ended past the exposure wanted, and at the cap past the cap: at max_leverage 2, position_size 2, and
// 5 bps, an entry of 2 * 100,000 / (100 * 1.0005) = 1,999.0 -> 1,999 shares paid 99.95 of commission and left a
// position worth 199,900 against an equity of 99,900.05, 2.001 times it, with the price unchanged. Now the n shares
// leave x times the equity once their costs are paid: 100 n = x * (100,000 - 0.05 n), n = x * 100,000 / (100 * (1 +
// x * 0.0005)). At 2x that is 200,000 / 100.1 = 1,998.0 -> 1,998, for 199,800 and 99.90 of commission, leaving an
// equity of 99,900.10 and an exposure of 199,800 / 99,900.10 = 1.999998, within the cap. At 1.5x it is
// 150,000 / 100.075 = 1,498.9 -> 1,498, for 149,800 and 74.90, leaving 99,925.10 and 149,800 / 99,925.10 = 1.49912.
// A short is the same the other way, and in fractional shares the exposure is exactly the one wanted.
void an_entry_above_1x_ends_at_the_exposure_wanted() {
    struct Size {
        double exposure;
        double shares;
        double commission;  // 0.05 a share
    };
    for (const Size& size : {Size{2.0, 1998.0, 99.9}, Size{1.5, 1498.0, 74.9}}) {
        for (const int side : {1, -1}) {
            BacktestConfig config;
            config.allow_short = true;
            config.max_leverage = 2.0;
            config.position_size = size.exposure;
            config.commission_bps = 5.0;
            const BacktestResult result = run(flat_bars(), directions({side, side, side, side}), config);
            const std::string what = text_of(size.exposure) + "x " + (side > 0 ? "long" : "short");
            const double held = side * size.shares;
            const double equity = 100'000.0 - size.commission;
            expect_fills(
                result,
                {{1, held, 100.0, size.commission, 0.0, "entry"}, {3, -held, 100.0, size.commission, 0.0, "end"}},
                what);
            expect_values(result.equity, {100'000.0, equity, equity, equity - size.commission}, what + ": equity");
            expect_values(result.exposure,
                          {0.0, held * 100.0 / equity, held * 100.0 / equity, 0.0},
                          what + ": exposure");
            const double pnl = -2.0 * size.commission;
            expect_trades(result,
                          {{side, 1, 3, 100.0, 100.0, size.shares, pnl, pnl / (size.shares * 100.0), "end", false}},
                          what);
            check::expect(std::fabs(result.exposure[1]) <= size.exposure + 1e-9,
                          what + ": the exposure after the entry, " + text_of(result.exposure[1]) + ", passes " +
                              text_of(size.exposure));
            check::expect_equal(result.trimmed, std::size_t{0}, what + ": orders trimmed");

            config.fractional_shares = true;
            const BacktestResult fractional = run(flat_bars(), directions({side, side, side, side}), config);
            check::expect_near(fractional.exposure[1],
                               side * size.exposure,
                               1e-9,
                               what + ", fractional: the exposure after the entry");
            check::expect_equal(fractional.trimmed, std::size_t{0}, what + ", fractional: orders trimmed");
        }
    }
}

// ---- Validation ----

void expect_rejected(const BacktestConfig& config, const std::string& field, const std::string& what) {
    try {
        nanotrade::validate(config);
    } catch (const nanotrade::Error& error) {
        check::expect_equal(error.field(), "config." + field, what + ": the field at fault");
        check::expect(!std::string(error.what()).empty(), what + ": a message");
        return;
    }
    throw check::Failure(what + " was accepted");
}

using Setter = void (*)(BacktestConfig&, double);

struct NumberField {
    const char* name;
    Setter set;
    std::vector<double> invalid;
    std::vector<double> valid;  // the edges of the range, each on an otherwise default config
};

const std::vector<NumberField>& number_fields() {
    static const std::vector<NumberField> fields = {
        {"initial_capital",
         [](BacktestConfig& c, double v) { c.initial_capital = v; },
         {0.0, -1.0, 1.1e12, inf, nan},
         {0.01, 1e12}},
        {"max_leverage",
         [](BacktestConfig& c, double v) { c.max_leverage = v; },
         {0.0, 0.09, 10.5, -1.0, inf, nan},
         {1.0, 10.0}},
        {"position_size",
         [](BacktestConfig& c, double v) { c.position_size = v; },
         {0.0, -0.5, 1.01, inf, nan},
         {1e-6, 1.0}},
        {"target_volatility",
         [](BacktestConfig& c, double v) { c.target_volatility = v; },
         {0.0, -0.1, 2.01, inf, nan},
         {1e-6, 2.0}},
        {"rebalance_threshold",
         [](BacktestConfig& c, double v) { c.rebalance_threshold = v; },
         {-0.01, 1.01, nan},
         {0.0, 1.0}},
        {"commission_bps",
         [](BacktestConfig& c, double v) { c.commission_bps = v; },
         {-1.0, 1000.5, inf, nan},
         {0.0, 1000.0}},
        {"commission_fixed",
         [](BacktestConfig& c, double v) { c.commission_fixed = v; },
         {-0.01, 1.1e6, inf, nan},
         {0.0, 1e6}},
        {"slippage_bps",
         [](BacktestConfig& c, double v) { c.slippage_bps = v; },
         {-1.0, 1000.5, inf, nan},
         {0.0, 1000.0}},
        {"borrow_rate", [](BacktestConfig& c, double v) { c.borrow_rate = v; }, {-0.01, 1.01, nan}, {0.0, 1.0}},
        {"stop_loss", [](BacktestConfig& c, double v) { c.stop_loss = v; }, {-0.01, 1.0, nan}, {0.0, 0.99}},
        {"take_profit", [](BacktestConfig& c, double v) { c.take_profit = v; }, {-0.01, 10.5, nan}, {0.0, 10.0}},
        {"trailing_stop", [](BacktestConfig& c, double v) { c.trailing_stop = v; }, {-0.01, 1.0, nan}, {0.0, 0.99}},
        {"risk_free_rate", [](BacktestConfig& c, double v) { c.risk_free_rate = v; }, {-0.11, 0.51, nan}, {-0.1, 0.5}},
        {"bars_per_year",
         [](BacktestConfig& c, double v) { c.bars_per_year = v; },
         {-1.0, 1.1e6, inf, nan},
         {0.0, 1e6}},
    };
    return fields;
}

void validate_rejects_each_invalid_field() {
    nanotrade::validate(BacktestConfig{});
    for (const NumberField& field : number_fields()) {
        for (const double value : field.invalid) {
            BacktestConfig config;
            field.set(config, value);
            expect_rejected(config, field.name, std::string(field.name) + " = " + text_of(value));
        }
    }
    for (const std::size_t window : {std::size_t{0}, std::size_t{1}, std::size_t{501}}) {
        BacktestConfig config;
        config.volatility_window = window;
        expect_rejected(config, "volatility_window", "volatility_window = " + std::to_string(window));
    }
    // The position size is held to the leverage cap, whatever the cap.
    BacktestConfig config;
    config.max_leverage = 2.0;
    config.position_size = 2.5;
    expect_rejected(config, "position_size", "position_size 2.5 over a cap of 2");
    // run_backtest validates too.
    config = BacktestConfig{};
    config.slippage_bps = nan;
    const BarSeries bars = rising_bars();
    const Directions flat(bars.size(), std::int8_t{0});
    try {
        run(bars, flat, config);
        throw check::Failure("run_backtest accepted a NaN slippage");
    } catch (const nanotrade::Error& error) {
        check::expect_equal(error.field(), std::string("config.slippage_bps"), "run_backtest: the field at fault");
    }
}

void validate_accepts_the_edges() {
    for (const NumberField& field : number_fields()) {
        for (const double value : field.valid) {
            BacktestConfig config;
            field.set(config, value);
            try {
                nanotrade::validate(config);
            } catch (const nanotrade::Error& error) {
                throw check::Failure(std::string(field.name) + " = " + text_of(value) +
                                     " was rejected: " + error.what());
            }
        }
    }
    BacktestConfig config;
    config.volatility_window = 2;
    nanotrade::validate(config);
    config.volatility_window = 500;
    nanotrade::validate(config);
    config = BacktestConfig{};
    config.max_leverage = 0.1;
    config.position_size = 0.1;
    nanotrade::validate(config);
    config.max_leverage = 2.0;
    config.position_size = 2.0;
    nanotrade::validate(config);
}

void run_backtest_rejects_a_bad_range() {
    const BarSeries bars = rising_bars();
    const Directions flat(bars.size(), std::int8_t{0});
    const BacktestConfig config;
    const auto expect_range = [&](std::size_t begin, std::size_t end) {
        const std::string what = "[" + std::to_string(begin) + ", " + std::to_string(end) + ")";
        try {
            nanotrade::run_backtest(bars, flat, config, begin, end);
        } catch (const nanotrade::Error& error) {
            check::expect_equal(error.field(), std::string("range"), what + ": the field at fault");
            return;
        }
        throw check::Failure(what + " was accepted");
    };
    expect_range(2, 2);
    expect_range(3, 2);
    expect_range(0, 6);
    expect_range(5, 5);
    const Directions short_by_one(bars.size() - 1, std::int8_t{0});
    check::expect_throws<nanotrade::Error>([&] { nanotrade::run_backtest(bars, short_by_one, config, 0, 4); },
                                           "a direction for each bar");
    const BacktestResult last = nanotrade::run_backtest(bars, flat, config, 4, 5);
    expect_values(last.equity, {100'000.0}, "a window of one bar");
}

// ---- Properties on real data ----

const std::vector<BarSeries>& market() {
    static const std::vector<BarSeries> series = [] {
        std::vector<BarSeries> loaded;
        for (const char* symbol : {"AAPL", "BA", "CAT", "DELL", "EBAY", "F", "GE"}) {
            const std::string path = std::string(NANOTRADE_DATA_DIR) + "/" + symbol + ".csv";
            loaded.push_back(nanotrade::load_csv_file(path, symbol).series);
        }
        return loaded;
    }();
    return series;
}

// Holds each signal for a while: at each bar, with probability `switching`, a new one of -1, 0, and 1.
Directions random_directions(Random& random, std::size_t count, double switching) {
    Directions out(count, 0);
    std::int8_t current = 0;
    for (std::int8_t& value : out) {
        if (random.uniform() < switching) {
            current = static_cast<std::int8_t>(static_cast<int>(random.below(3)) - 1);
        }
        value = current;
    }
    return out;
}

struct Variant {
    Sizing sizing;
    bool allow_short;
    bool frills;  // costs, borrowing, and all three stops
};

std::string describe(const Variant& variant) {
    std::string text = variant.sizing == Sizing::Percent ? "percent sizing" : "volatility sizing";
    text += variant.allow_short ? ", long-short" : ", long only";
    text += variant.frills ? ", costs and stops" : ", no costs or stops";
    return text;
}

std::vector<Variant> all_variants() {
    std::vector<Variant> out;
    for (const Sizing sizing : {Sizing::Percent, Sizing::Volatility}) {
        for (const bool allow_short : {false, true}) {
            for (const bool frills : {false, true}) {
                out.push_back({sizing, allow_short, frills});
            }
        }
    }
    return out;
}

struct Case {
    std::string label;  // the symbol and seed, to reproduce a failure
    std::uint64_t seed{0};
    const BarSeries* bars{nullptr};
    Directions direction;
    BacktestConfig config;
    std::size_t begin{0};
    std::size_t end{0};
};

Case make_case(const Variant& variant, const BarSeries& bars, std::uint64_t seed) {
    Random random(seed);
    Case made;
    made.label = bars.symbol + " with seed " + std::to_string(seed);
    made.seed = seed;
    made.bars = &bars;
    BacktestConfig& config = made.config;
    config.sizing = variant.sizing;
    config.allow_short = variant.allow_short;
    // Fully invested, half invested, on margin within the cap, and on margin at the cap.
    const int leverage = static_cast<int>(random.below(4));
    config.max_leverage = leverage >= 2 ? 2.0 : 1.0;
    config.position_size = leverage == 0 ? 1.0 : (leverage == 1 ? 0.5 : (leverage == 2 ? 1.5 : 2.0));
    config.target_volatility = 0.1 + 0.2 * random.uniform();
    config.volatility_window = 5 + static_cast<std::size_t>(random.below(56));
    config.rebalance_threshold = 0.3 * random.uniform();
    config.fractional_shares = random.below(2) == 0U;
    config.bars_per_year = random.below(2) == 0U ? 0.0 : 252.0;
    if (variant.frills) {
        config.commission_bps = 10.0 * random.uniform();
        config.commission_fixed = random.below(2) == 0U ? 0.0 : 1.0;
        config.slippage_bps = 20.0 * random.uniform();
        config.borrow_rate = 0.1 * random.uniform();
        config.stop_loss = 0.03 + 0.1 * random.uniform();
        config.take_profit = 0.1 + 0.3 * random.uniform();
        config.trailing_stop = 0.05 + 0.15 * random.uniform();
    }
    made.direction = random_directions(random, bars.size(), 0.01 + 0.3 * random.uniform());
    made.begin = static_cast<std::size_t>(random.below(250));
    made.end = bars.size() - static_cast<std::size_t>(random.below(250));
    return made;
}

BacktestResult run_case(const Case& c) {
    return nanotrade::run_backtest(*c.bars, c.direction, c.config, c.begin, c.end);
}

bool every_variant(const Variant&) {
    return true;
}

bool percent_sized(const Variant& variant) {
    return variant.sizing == Sizing::Percent;
}

bool with_shorts(const Variant& variant) {
    return variant.allow_short;
}

// Runs `property` on every symbol under each variant `wanted`, and reports each failing variant with its first
// failure.
void for_each_case(const std::function<void(const Case&)>& property, bool (*wanted)(const Variant&)) {
    const std::vector<Variant> variants = all_variants();
    std::string failures;
    for (std::size_t v = 0; v < variants.size(); ++v) {
        if (!wanted(variants[v])) {
            continue;
        }
        try {
            for (std::size_t s = 0; s < market().size(); ++s) {
                property(make_case(variants[v], market()[s], 1000 * (v + 1) + s));
            }
        } catch (const check::Failure& failure) {
            failures += "\n      " + describe(variants[v]) + ": " + failure.what();
        }
    }
    check::expect(failures.empty(), "failed under" + failures);
}

bool same(double a, double b) {
    return a == b || (std::isnan(a) && std::isnan(b));
}

bool same_fill(const Fill& a, const Fill& b) {
    return a.bar == b.bar && same(a.shares, b.shares) && same(a.price, b.price) && same(a.commission, b.commission) &&
           same(a.slippage, b.slippage) && a.reason == b.reason;
}

bool same_trade(const Trade& a, const Trade& b) {
    return a.direction == b.direction && a.entry_bar == b.entry_bar && a.exit_bar == b.exit_bar &&
           same(a.entry_price, b.entry_price) && same(a.exit_price, b.exit_price) && same(a.shares, b.shares) &&
           same(a.pnl, b.pnl) && same(a.return_pct, b.return_pct) && same(a.mae, b.mae) && same(a.mfe, b.mfe) &&
           a.exit_reason == b.exit_reason && a.open == b.open;
}

// The loops below run over thousands of values, so they build a message only on failure.
void expect_same_series(const std::vector<double>& a,
                        const std::vector<double>& b,
                        std::size_t count,
                        const std::string& what) {
    check::expect(a.size() >= count && b.size() >= count, what + ": too short");
    for (std::size_t i = 0; i < count; ++i) {
        if (!same(a[i], b[i])) {
            throw check::Failure(what + " at " + std::to_string(i) + ": " + text_of(a[i]) + " then " + text_of(b[i]));
        }
    }
}

void expect_same_fills(const std::vector<Fill>& a,
                       const std::vector<Fill>& b,
                       std::size_t count,
                       const std::string& what) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!same_fill(a[i], b[i])) {
            throw check::Failure(what + ": fill " + std::to_string(i) + " changed");
        }
    }
}

void property_accounting() {
    for_each_case(
        [](const Case& c) {
            const BacktestResult result = run_case(c);
            double pnl = 0.0;
            for (const Trade& trade : result.trades) {
                if (trade.open) {
                    throw check::Failure(c.label + ": a trade left open with close_at_end");
                }
                pnl += trade.pnl;
            }
            const double expected = c.config.initial_capital + pnl;
            check::expect_near(result.equity.back(),
                               expected,
                               1e-6 * std::max(std::fabs(expected), c.config.initial_capital),
                               c.label + ": final equity against the initial capital plus every trade's P&L");
            check::expect_equal(result.position.back(), 0.0, c.label + ": the position after the last close");
            double commissions = 0.0;
            double slippage = 0.0;
            for (const Fill& fill : result.fills) {
                commissions += fill.commission;
                slippage += fill.slippage;
            }
            check::expect_near(result.commissions,
                               commissions,
                               1e-9 * std::max(1.0, commissions),
                               c.label + ": commissions against the fills'");
            check::expect_near(result.slippage,
                               slippage,
                               1e-9 * std::max(1.0, slippage),
                               c.label + ": slippage against the fills'");
        },
        every_variant);
}

// Every price after bar t replaced by a random walk from bar t's close, high and low around the open and close, and
// every signal after t replaced by one of the other two.
void rewrite_after(BarSeries& bars, Directions& direction, std::size_t t, Random& random) {
    double previous = bars.close[t];
    for (std::size_t i = t + 1; i < bars.size(); ++i) {
        const double open = previous * (0.95 + 0.1 * random.uniform());
        const double close = open * (0.95 + 0.1 * random.uniform());
        bars.open[i] = open;
        bars.close[i] = close;
        bars.high[i] = std::max(open, close) * (1.0 + 0.02 * random.uniform());
        bars.low[i] = std::min(open, close) * (1.0 - 0.02 * random.uniform());
        previous = close;
        const int shift = 1 + static_cast<int>(random.below(2));
        direction[i] = static_cast<std::int8_t>((direction[i] + 1 + shift) % 3 - 1);
    }
}

void expect_same_through(const BacktestResult& a, const BacktestResult& b, std::size_t t, const std::string& what) {
    const std::size_t closes = t + 1 - a.begin;
    expect_same_series(a.equity, b.equity, closes, what + ": equity");
    expect_same_series(a.position, b.position, closes, what + ": position");
    expect_same_series(a.exposure, b.exposure, closes, what + ": exposure");
    const auto fills_through = [t](const BacktestResult& result) {
        return static_cast<std::size_t>(
            std::count_if(result.fills.begin(), result.fills.end(), [t](const Fill& fill) { return fill.bar <= t; }));
    };
    const std::size_t fills = fills_through(a);
    check::expect_equal(fills_through(b), fills, what + ": fills through bar t");
    expect_same_fills(a.fills, b.fills, fills, what);
    for (std::size_t i = 0; i < a.trades.size() && a.trades[i].exit_bar <= t && !a.trades[i].open; ++i) {
        if (i >= b.trades.size() || !same_trade(a.trades[i], b.trades[i])) {
            throw check::Failure(what + ": trade " + std::to_string(i) + ", closed by bar t, changed");
        }
    }
}

// The fill at bar t's open, if there is one: the bar's first, for an order decided at the close before.
const Fill* open_fill(const BacktestResult& result, std::size_t t) {
    for (const Fill& fill : result.fills) {
        if (fill.bar == t) {
            const bool at_open = fill.reason == "entry" || fill.reason == "exit" || fill.reason == "reverse" ||
                                 fill.reason == "rebalance";
            return at_open ? &fill : nullptr;
        }
        if (fill.bar > t) {
            break;
        }
    }
    return nullptr;
}

// Each cut is made twice: once changing every price after bar t, and once keeping bar t + 1's open, when the order
// filled at that open must not change either, since it can see nothing of its bar but the open.
void property_no_lookahead() {
    for_each_case(
        [](const Case& c) {
            const BacktestResult base = run_case(c);
            Random random(c.seed + 7);
            // A bar at random, and the bar before a fill at random, where a peek would most likely show.
            std::vector<std::size_t> cuts = {c.begin + static_cast<std::size_t>(random.below(c.end - 1 - c.begin))};
            if (!base.fills.empty()) {
                const Fill& fill = base.fills[static_cast<std::size_t>(random.below(base.fills.size()))];
                cuts.push_back(std::clamp(fill.bar, c.begin + 1, c.end - 1) - 1);
            }
            for (const std::size_t t : cuts) {
                for (const bool keep_next_open : {false, true}) {
                    BarSeries bars = *c.bars;
                    Case changed = c;
                    rewrite_after(bars, changed.direction, t, random);
                    if (keep_next_open) {
                        bars.open[t + 1] = c.bars->open[t + 1];
                        bars.high[t + 1] = std::max(bars.high[t + 1], bars.open[t + 1]);
                        bars.low[t + 1] = std::min(bars.low[t + 1], bars.open[t + 1]);
                    }
                    changed.bars = &bars;
                    const BacktestResult other = run_case(changed);
                    const std::string what = c.label + ", changed after bar " + std::to_string(t);
                    expect_same_through(base, other, t, what);
                    if (keep_next_open) {
                        const Fill* before = open_fill(base, t + 1);
                        const Fill* after = open_fill(other, t + 1);
                        if ((before == nullptr) != (after == nullptr) ||
                            (before != nullptr && !same_fill(*before, *after))) {
                            throw check::Failure(what + " but bar " + std::to_string(t + 1) +
                                                 "'s open: the order filled at that open changed");
                        }
                    }
                }
            }
        },
        every_variant);
}

void property_determinism() {
    for_each_case(
        [](const Case& c) {
            const BacktestResult first = run_case(c);
            const BarSeries copy = *c.bars;  // the same bars at another address
            Case again = c;
            again.bars = &copy;
            const BacktestResult second = run_case(again);
            const std::string what = c.label + ": the second run";
            check::expect(first.begin == second.begin && first.end == second.end &&
                              same(first.initial_capital, second.initial_capital) &&
                              same(first.bars_per_year, second.bars_per_year),
                          what + ": the window");
            check::expect_equal(second.equity.size(), first.equity.size(), what + ": closes");
            expect_same_series(first.equity, second.equity, first.equity.size(), what + ": equity");
            expect_same_series(first.position, second.position, first.position.size(), what + ": position");
            expect_same_series(first.exposure, second.exposure, first.exposure.size(), what + ": exposure");
            check::expect_equal(second.fills.size(), first.fills.size(), what + ": fills");
            expect_same_fills(first.fills, second.fills, first.fills.size(), what);
            check::expect_equal(second.trades.size(), first.trades.size(), what + ": trades");
            for (std::size_t i = 0; i < first.trades.size(); ++i) {
                if (!same_trade(first.trades[i], second.trades[i])) {
                    throw check::Failure(what + ": trade " + std::to_string(i) + " changed");
                }
            }
            check::expect(same(first.commissions, second.commissions) && same(first.slippage, second.slippage) &&
                              same(first.borrow, second.borrow) && same(first.traded_value, second.traded_value) &&
                              first.trimmed == second.trimmed && first.ruined == second.ruined,
                          what + ": the totals");
        },
        every_variant);
}

// At each fill, the position it leaves, valued at the fill's price or the open, whichever is higher, against the
// equity at the open before it: the cash and position at the previous close, marked at the open.
void property_leverage_cap() {
    for_each_case(
        [](const Case& c) {
            const BacktestResult result = run_case(c);
            const BarSeries& bars = *c.bars;
            double cash = c.config.initial_capital;
            double held = 0.0;
            std::size_t next = 0;
            for (std::size_t t = result.begin; t < result.end; ++t) {
                if (t > result.begin) {
                    held = result.position[t - result.begin - 1];
                    cash = result.equity[t - result.begin - 1] - held * bars.close[t - 1];
                }
                for (; next < result.fills.size() && result.fills[next].bar == t; ++next) {
                    const Fill& fill = result.fills[next];
                    const double after = held + fill.shares;
                    if (after != 0.0) {
                        const double equity = cash + held * bars.open[t];
                        const double value = std::fabs(after) * std::max(fill.price, bars.open[t]);
                        const double limit = c.config.max_leverage * equity;
                        if (!(value <= limit + 1e-9 * std::fabs(limit))) {
                            throw check::Failure(c.label + ": the " + fill.reason + " at bar " + std::to_string(t) +
                                                 " holds " + text_of(value) + " against a cap of " + text_of(limit));
                        }
                    }
                    cash -= fill.shares * fill.price + fill.commission;
                    held = after;
                }
            }
        },
        percent_sized);
}

// The bars with every open moved to the close before it, the high and low widened to hold it.
BarSeries without_gaps(const BarSeries& gapped) {
    BarSeries bars = gapped;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        bars.open[i] = bars.close[i - 1];
        bars.high[i] = std::max(bars.high[i], bars.open[i]);
        bars.low[i] = std::min(bars.low[i], bars.open[i]);
    }
    return bars;
}

// With every open at the close before it, each order fills at the price it was sized at, so none is trimmed, under
// any sizing and whatever the costs.
void property_no_gap_no_trim() {
    for_each_case(
        [](const Case& c) {
            const BarSeries bars = without_gaps(*c.bars);
            Case gapless = c;
            gapless.bars = &bars;
            const BacktestResult result = run_case(gapless);
            check::expect_equal(result.trimmed, std::size_t{0}, c.label + ": orders trimmed without a gap");
        },
        every_variant);
}

// After each bar with fills, the leverage at its close, |position * close| / equity, passes max_leverage only as far
// as the price's move since the open takes it. Taking that move back out, and the bar's borrowing, charged at the
// close, gives the leverage at the open right after the fill, which must be within the cap. On data without gaps each
// order fills at the price it was sized at, so this holds to rounding.
void property_leverage_after_each_fill() {
    for_each_case(
        [](const Case& c) {
            const BarSeries bars = without_gaps(*c.bars);
            Case gapless = c;
            gapless.bars = &bars;
            const BacktestResult result = run_case(gapless);
            std::size_t checked = result.end;
            for (const Fill& fill : result.fills) {
                const std::size_t t = fill.bar;
                const std::size_t i = t - result.begin;
                const double held = result.position[i];
                if (t == checked || held == 0.0) {
                    continue;
                }
                checked = t;
                const double close = bars.close[t];
                const double borrow = held < 0.0 ? -held * close * c.config.borrow_rate / result.bars_per_year : 0.0;
                const double equity_at_open = result.equity[i] - held * (close - bars.open[t]) + borrow;
                const double leverage = std::fabs(held) * bars.open[t] / equity_at_open;
                if (!(equity_at_open > 0.0 && leverage <= c.config.max_leverage * (1.0 + 1e-9))) {
                    throw check::Failure(c.label + ": after the " + fill.reason + " at bar " + std::to_string(t) +
                                         ", a leverage of " + text_of(leverage) + " at the open (" +
                                         text_of(result.exposure[i]) + " at the close) against a cap of " +
                                         text_of(c.config.max_leverage));
                }
            }
        },
        with_shorts);
}

}  // namespace

int main() {
    return check::run({
        {"an order decided at a close fills at the next open, in and out", entry_and_exit_at_the_next_open},
        {"slippage and commission on each fill", slippage_and_commission},
        {"whole and fractional shares at half the equity", whole_and_fractional_shares},
        {"a gap up trims the order to the leverage cap", gap_up_trims_to_the_leverage_cap},
        {"a stop loss fills at its level, or at an open through it", stop_loss_at_its_level_or_the_gapped_open},
        {"a take profit fills at its level, or at an open through it", take_profit_at_its_level_or_the_gapped_open},
        {"a bar reaching both levels takes the stop", a_bar_reaching_both_levels_takes_the_stop},
        {"a trailing stop follows the highest high since entry", trailing_stop_follows_the_highest_high},
        {"a short's trailing stop follows the lowest low", short_trailing_stop_follows_the_lowest_low},
        {"after a stop, no re-entry until the signal changes", no_reentry_after_a_stop_until_the_signal_changes},
        {"a short's P&L and its borrowing", short_profit_and_borrowing},
        {"a reversal closes one trade and opens another in one fill", reversal_closes_one_trade_and_opens_another},
        {"volatility sizing and rebalancing", volatility_sizing_and_rebalancing},
        {"volatility sizing is capped at max_leverage", volatility_sizing_is_capped},
        {"without close_at_end the trade is left open, marked at the close", open_at_the_end_without_close_at_end},
        {"begin > 0 fills the signal at begin - 1", begin_fills_the_signal_before_it},
        {"equity reaching zero stops trading", ruin_stops_trading},
        {"maximum adverse and favorable excursions", maximum_adverse_and_favorable_excursions},
        {"a signal exit at a gapped open counts in the excursions",
         a_signal_exit_at_a_gapped_open_counts_in_the_excursions},
        {"a reversal at a gapped open counts in the excursions", a_reversal_at_a_gapped_open_counts_in_the_excursions},
        {"only a gap trims an order, not its costs", only_a_gap_trims_an_order},
        {"a reversal budgets the cost of closing", a_reversal_budgets_the_cost_of_closing},
        {"a reversal budgets the cost of closing, with slippage", a_reversal_budgets_the_cost_of_closing_with_slippage},
        {"an entry above 1x ends at the exposure wanted, within the cap",
         an_entry_above_1x_ends_at_the_exposure_wanted},
        {"validate rejects each invalid field, NaN included", validate_rejects_each_invalid_field},
        {"validate accepts the edges of each range", validate_accepts_the_edges},
        {"run_backtest rejects a bad range", run_backtest_rejects_a_bad_range},
        {"property: the books balance", property_accounting},
        {"property: nothing looks ahead", property_no_lookahead},
        {"property: a run repeats exactly", property_determinism},
        {"property: no fill takes a position past the leverage cap", property_leverage_cap},
        {"property: without gaps no order is trimmed", property_no_gap_no_trim},
        {"property: after each fill the leverage is within the cap", property_leverage_after_each_fill},
    });
}
