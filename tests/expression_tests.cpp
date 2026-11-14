// The strategy language: how it parses, what it computes, and how it reports mistakes.

#include "check.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/expression.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/strategy.hpp"
#include "nanotrade/synthetic.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace expr = nanotrade::expr;
namespace ind = nanotrade::indicators;
using ind::Series;
using nanotrade::BarSeries;
using nanotrade::Error;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// Bars with the given closes; each opens 0.5 below its close, with the high and low one beyond.
BarSeries bars_with_closes(const std::vector<double>& closes) {
    BarSeries bars;
    bars.symbol = "TEST";
    const std::int64_t first = nanotrade::days_from_civil(2020, 1, 1);
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double open = closes[i] - 0.5;
        bars.push_back((first + static_cast<std::int64_t>(i)) * nanotrade::seconds_per_day,
                       open,
                       closes[i] + 1.0,
                       open - 1.0,
                       closes[i],
                       100.0 * static_cast<double>(i + 1));
    }
    return bars;
}

const BarSeries& counting() {
    static const BarSeries bars = bars_with_closes({1, 2, 3, 4, 5, 6});
    return bars;
}

const BarSeries& market() {
    static const BarSeries bars = nanotrade::synthesize({5, 800, 0.05, 0.25, 100.0});
    return bars;
}

expr::Program compile(std::string_view source, const expr::Params& params = {}) {
    return expr::Program::compile(source, params);
}

Series evaluate(std::string_view source, const BarSeries& bars, const expr::Params& params = {}) {
    ind::Cache cache(bars);
    return compile(source, params).evaluate(cache);
}

void expect_values(const Series& actual, const Series& expected, const std::string& what, double tolerance = 0.0) {
    check::expect_equal(actual.size(), expected.size(), what + ": length");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const bool actual_missing = std::isnan(actual[i]);
        const bool expected_missing = std::isnan(expected[i]);
        if (actual_missing || expected_missing) {
            check::expect(actual_missing && expected_missing,
                          what + " at " + std::to_string(i) + ": expected " + text_of(expected[i]) + ", got " +
                              text_of(actual[i]));
            continue;
        }
        check::expect_near(actual[i],
                           expected[i],
                           tolerance * std::max(1.0, std::fabs(expected[i])),
                           what + " at " + std::to_string(i));
    }
}

void expect_canonical(std::string_view source, const std::string& canonical, const expr::Params& params = {}) {
    check::expect_equal(compile(source, params).canonical(),
                        canonical,
                        "canonical form of '" + std::string(source) + "'");
}

// Compiling `source` must fail with a message containing `phrase`, pointing at [position, position + length).
void expect_error(std::string_view source,
                  const std::string& phrase,
                  std::size_t position,
                  std::size_t length,
                  const expr::Params& params = {}) {
    const std::string quoted = "'" + std::string(source.substr(0, 60)) + "'";
    try {
        compile(source, params);
    } catch (const Error& error) {
        const std::string message = error.what();
        check::expect(message.find(phrase) != std::string::npos,
                      quoted + ": the message \"" + message + "\" should contain \"" + phrase + "\"");
        check::expect_equal(error.position(), position, quoted + ": position of \"" + message + "\"");
        check::expect_equal(error.length(), length, quoted + ": length of \"" + message + "\"");
        return;
    }
    throw check::Failure(quoted + " compiled");
}

std::string message_of(std::string_view source, const expr::Params& params = {}) {
    try {
        compile(source, params);
    } catch (const Error& error) {
        return error.what();
    }
    return "(compiled)";
}

// ---- Precedence ----

void precedence_of_logic() {
    const BarSeries& bars = counting();  // closes 1 to 6
    // and binds tighter than or: 6, or above 0 and below 3.
    expect_values(evaluate("close == 6 or close > 0 and close < 3", bars), {1, 1, 0, 0, 0, 1}, "or of an and");
    expect_canonical("close == 6 or close > 0 and close < 3", "((close==6) or ((close>0) and (close<3)))");
    // not binds tighter than and.
    expect_values(evaluate("not close > 3 and close > 1", bars), {0, 1, 1, 0, 0, 0}, "and of a not");
    expect_canonical("not close > 3 and close > 1", "((not (close>3)) and (close>1))");
    expect_values(evaluate("not not close > 3", bars), {0, 0, 0, 1, 1, 1}, "double negation");
    expect_values(evaluate("close < 2 or close > 5 or close == 3", bars), {1, 0, 1, 0, 0, 1}, "chained or");
    // The symbolic spellings.
    expect_values(evaluate("close > 1 && close < 4 || !(close != 5)", bars), {0, 1, 1, 0, 1, 0}, "&&, ||, !");
    expect_canonical("close > 1 && close < 4 || !(close != 5)", "(((close>1) and (close<4)) or (not (close!=5)))");
    expect_values(evaluate("close >= 2 and close <= 4", bars), {0, 1, 1, 1, 0, 0}, ">= and <=");
}

void precedence_of_arithmetic() {
    const BarSeries& bars = counting();
    expect_values(evaluate("close + 2 * close", bars), {3, 6, 9, 12, 15, 18}, "* before +");
    expect_canonical("close + 2 * close", "(close+(2*close))");
    expect_values(evaluate("(close + 2) * close", bars), {3, 8, 15, 24, 35, 48}, "parentheses");
    expect_values(evaluate("close - 1 - 1", bars), {-1, 0, 1, 2, 3, 4}, "- is left-associative");
    expect_canonical("close - 1 - 1", "((close-1)-1)");
    expect_values(evaluate("close / 2 / 2", bars), {0.25, 0.5, 0.75, 1, 1.25, 1.5}, "/ is left-associative");
    expect_values(evaluate("-close * 2", bars), {-2, -4, -6, -8, -10, -12}, "unary minus");
    expect_canonical("-close * 2", "((-close)*2)");
    expect_canonical("2 * -close", "(2*(-close))");
    expect_canonical("--close", "(-(-close))");
    expect_values(evaluate("-close[1]", bars), {nan, -1, -2, -3, -4, -5}, "a lag binds tighter than a minus");
    expect_canonical("-close[1]", "(-close[1])");
    expect_values(evaluate("close + 1 > 3", bars), {0, 0, 1, 1, 1, 1}, "arithmetic before comparison");
    expect_canonical("close + 1 > 3", "((close+1)>3)");
    expect_canonical("1 + 2 * 3", "7");
    expect_canonical("(1 + 2) * 3", "9");
    expect_canonical("10 - 2 - 3", "5");
    expect_canonical("100 / 10 / 2", "5");
    expect_canonical("2 * 3 - 4 / 8", "5.5");
    expect_canonical("- 5", "-5");
    expect_canonical("--5", "5");
    expect_canonical("-(2 + 3) * 2", "-10");
    check::expect(compile("1 + 2").type() == expr::Type::Number, "a sum is a number");
    check::expect(compile("close > 1").type() == expr::Type::Condition, "a comparison is a condition");
    check::expect(compile("true").type() == expr::Type::Condition, "true is a condition");
    expect_values(evaluate("1 > 2", bars), {0, 0, 0, 0, 0, 0}, "a comparison of constants");
    expect_values(evaluate("2 * 3", bars), {6, 6, 6, 6, 6, 6}, "a constant fills every bar");
}

// ---- Canonical forms and constant folding ----

void canonical_forms() {
    expect_canonical("SMA( close , 20 )", "sma(close,20)");
    expect_canonical("Close > SMA(CLOSE, 20) AND RSI(close,14) < 30", "((close>sma(close,20)) and (rsi(close,14)<30))");
    expect_canonical("(((close)))", "close");
    expect_canonical("close >= 1e3", "(close>=1000)");
    expect_canonical("close > .5", "(close>0.5)");
    expect_canonical("close > 1.50", "(close>1.5)");
    expect_canonical("close > 2.5E-1", "(close>0.25)");
    expect_canonical("sma(close, 10 + 10)", "sma(close,20)");
    expect_canonical("true and not false", "(true and (not false))");
    expect_canonical("close != open", "(close!=open)");
    expect_canonical("crossover(ema(close, 12), ema(close, 26))", "crossover(ema(close,12),ema(close,26))");
    expect_canonical("hl2 > volume", "(hl2>volume)");
}

void constant_folding() {
    const expr::Params params{{"slow", 50}, {"dip", 10}, {"fast", 12}, {"k", 2.5}, {"period", 20}};
    expect_canonical("$slow*2", "100", params);
    expect_canonical("100 - $dip", "90", params);
    expect_canonical("sma(close, $slow * 2)", "sma(close,100)", params);
    expect_canonical("rsi(close, 2) > 100 - $dip", "(rsi(close,2)>90)", params);
    expect_canonical("bb_lower(close, $period, $k)", "bb_lower(close,20,2.5)", params);
    expect_canonical("ema(close, $FAST)", "ema(close,12)", params);
    expect_canonical("close[$fast - 10]", "close[2]", params);
    check::expect(compile("$slow * 2", params).type() == expr::Type::Number, "a folded constant is a number");
    expect_values(evaluate("$slow / 4", counting(), params), {12.5, 12.5, 12.5, 12.5, 12.5, 12.5}, "its value");
}

// ---- Lags ----

void lags() {
    const BarSeries& bars = market();
    ind::Cache cache(bars);
    const Series& close = cache.column("close");
    expect_values(compile("close[1]").evaluate(cache), ind::lag(close, 1), "close[1] is the lag");
    expect_values(compile("close[0]").evaluate(cache), close, "close[0] is the close");
    expect_canonical("close[0]", "close");
    expect_canonical("close[1]", "close[1]");
    expect_values(compile("close[1][2]").evaluate(cache), compile("close[3]").evaluate(cache), "lags add up");
    expect_canonical("close[1][2]", "close[1][2]");
    expect_values(compile("sma(close, 3)[2]").evaluate(cache), ind::lag(ind::sma(close, 3), 2), "a lagged indicator");
    expect_canonical("sma(close, 5)[2]", "sma(close,5)[2]");
    expect_values(compile("close[$n]", {{"n", 4}}).evaluate(cache), ind::lag(close, 4), "a lag by a parameter");
    expect_values(compile("close[2000]").evaluate(cache), Series(bars.size(), nan), "the longest lag");
    expect_canonical("5[3]", "5");

    const expr::Program lagged = compile("(close > open)[1]");
    check::expect(lagged.type() == expr::Type::Condition, "a lagged condition is a condition");
    expect_values(lagged.evaluate(cache), ind::lag(compile("close > open").evaluate(cache), 1), "its values");
}

// ---- Three-valued logic ----

void three_valued_logic() {
    const BarSeries& bars = counting();
    // close[2] > 0 is undefined on the first two bars and holds after; close[2] > 100 never holds.
    const Series all0(6, 0.0);
    const Series all1(6, 1.0);
    expect_values(evaluate("false and close[2] > 0", bars), all0, "false and NaN is false");
    expect_values(evaluate("close[2] > 0 and false", bars), all0, "NaN and false is false");
    expect_values(evaluate("true or close[2] > 0", bars), all1, "true or NaN is true");
    expect_values(evaluate("close[2] > 100 or true", bars), all1, "NaN or true is true");
    expect_values(evaluate("true and close[2] > 0", bars), {nan, nan, 1, 1, 1, 1}, "true and NaN is NaN");
    expect_values(evaluate("false or close[2] > 0", bars), {nan, nan, 1, 1, 1, 1}, "false or NaN is NaN");
    expect_values(evaluate("close[2] > 0 and close[2] > 100", bars), {nan, nan, 0, 0, 0, 0}, "NaN and NaN");
    expect_values(evaluate("not close[2] > 0", bars), {nan, nan, 0, 0, 0, 0}, "not NaN is NaN");
    expect_values(evaluate("close[1] > 0 or close[2] > 0", bars), {nan, 1, 1, 1, 1, 1}, "1 or NaN");
    expect_values(evaluate("close[1] > 100 and close[2] > 0", bars), {nan, 0, 0, 0, 0, 0}, "0 and NaN");
    expect_values(evaluate("close[1] > 0 and close[2] > 0", bars), {nan, nan, 1, 1, 1, 1}, "1 and NaN");
    expect_values(evaluate("close[2] == close[2]", bars), {nan, nan, 1, 1, 1, 1}, "comparing NaN gives NaN");
    expect_values(evaluate("close / (close - close) > 0", bars), Series(6, nan), "dividing by zero gives NaN");

    // Whatever the inputs, a condition is 1, 0, or NaN.
    ind::Cache cache(market());
    for (const char* source : {"close > sma(close, 5) or rsi(close, 3) < 40",
                               "not (close[3] > open)",
                               "crossover(close, ema(close, 10)) and volume > 0"}) {
        for (const double v : compile(source).evaluate(cache)) {
            check::expect(std::isnan(v) || v == 0.0 || v == 1.0, std::string(source) + " gives " + text_of(v));
        }
    }
}

// ---- Functions ----

void crossings() {
    const BarSeries bars = bars_with_closes({5, 4, 3, 4, 5, 6, 5, 4});
    expect_values(evaluate("crossover(close, 4.5)", bars), {nan, 0, 0, 0, 1, 0, 0, 0}, "crossover");
    expect_values(evaluate("crossunder(close, 4.5)", bars), {nan, 1, 0, 0, 0, 0, 0, 1}, "crossunder");
    // Touching counts as having been below: 4 <= 4 and then 5 > 4.
    expect_values(evaluate("crossover(close, 4)", bars), {nan, 0, 0, 0, 1, 0, 0, 0}, "crossover from equality");
    expect_values(evaluate("crossunder(close, 4)", bars), {nan, 0, 1, 0, 0, 0, 0, 0}, "crossunder from equality");
    // Both sides must be defined now and on the bar before.
    expect_values(evaluate("crossover(close, close[2] + 0.5)", bars),
                  {nan, nan, nan, 0, 1, 0, 0, 0},
                  "crossing a lagged series");
    check::expect(compile("crossover(close, open)").type() == expr::Type::Condition, "a crossing is a condition");
}

std::size_t first_defined_index(const Series& x) {
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isnan(x[i])) {
            return i;
        }
    }
    return x.size();
}

void bands_and_macd() {
    const BarSeries& bars = market();
    ind::Cache cache(bars);
    const Series& close = bars.close;
    const Series mean = ind::sma(close, 20);
    const Series deviation = ind::stdev(close, 20);
    Series upper(close.size(), nan);
    Series lower(close.size(), nan);
    for (std::size_t i = 0; i < close.size(); ++i) {
        upper[i] = mean[i] + 2.5 * deviation[i];
        lower[i] = mean[i] - 2.5 * deviation[i];
    }
    expect_values(compile("bb_upper(close, 20, 2.5)").evaluate(cache), upper, "bb_upper is sma + k stdev", 1e-14);
    expect_values(compile("bb_lower(close, 20, 2.5)").evaluate(cache), lower, "bb_lower is sma - k stdev", 1e-14);

    const Series fast = ind::ema(close, 12);
    const Series slow = ind::ema(close, 26);
    Series line(close.size(), nan);
    for (std::size_t i = 0; i < close.size(); ++i) {
        line[i] = fast[i] - slow[i];
    }
    expect_values(compile("macd(close, 12, 26)").evaluate(cache), line, "macd is ema - ema");
    expect_values(compile("macd_signal(close, 12, 26, 9)").evaluate(cache),
                  ind::ema(line, 9),
                  "macd_signal is an ema of the line");
    check::expect_equal(first_defined_index(compile("macd_signal(close, 12, 26, 9)").evaluate(cache)),
                        std::size_t{25 + 8},
                        "the signal starts 9 bars after the line");
}

void other_functions() {
    const BarSeries& bars = market();
    ind::Cache cache(bars);
    const Series& open = bars.open;
    const Series& close = bars.close;
    Series lower(close.size());
    Series upper(close.size());
    Series distance(close.size());
    Series logs(close.size());
    Series shifted_log(close.size());
    for (std::size_t i = 0; i < close.size(); ++i) {
        lower[i] = std::min(close[i], open[i]);
        upper[i] = std::max(close[i], 100.0);
        distance[i] = std::fabs(close[i] - 100.0);
        logs[i] = std::log(close[i]);
        shifted_log[i] = close[i] > 100.0 ? std::log(close[i] - 100.0) : nan;
    }
    expect_values(compile("min(close, open)").evaluate(cache), lower, "min");
    expect_values(compile("max(close, 100)").evaluate(cache), upper, "max");
    expect_values(compile("abs(close - 100)").evaluate(cache), distance, "abs");
    expect_values(compile("log(close)").evaluate(cache), logs, "log", 1e-15);
    expect_values(compile("log(close - 100)").evaluate(cache),
                  shifted_log,
                  "log of a non-positive number is NaN",
                  1e-14);
    expect_values(
        compile("max(close[1], open)").evaluate(cache),
        [&] {
            Series out(close.size(), nan);
            for (std::size_t i = 1; i < close.size(); ++i) {
                out[i] = std::max(close[i - 1], open[i]);
            }
            return out;
        }(),
        "max with an undefined side is undefined");

    expect_values(compile("sma(close, 20)").evaluate(cache), ind::sma(close, 20), "sma");
    expect_values(compile("ema(close, 20)").evaluate(cache), ind::ema(close, 20), "ema");
    expect_values(compile("rsi(close, 14)").evaluate(cache), ind::rsi(close, 14), "rsi");
    expect_values(compile("stdev(close, 20)").evaluate(cache), ind::stdev(close, 20), "stdev");
    expect_values(compile("highest(high, 10)").evaluate(cache), ind::highest(bars.high, 10), "highest");
    expect_values(compile("lowest(low, 10)").evaluate(cache), ind::lowest(bars.low, 10), "lowest");
    expect_values(compile("roc(close, 5)").evaluate(cache), ind::roc(close, 5), "roc");
    expect_values(compile("change(close, 5)").evaluate(cache), ind::change(close, 5), "change");
    expect_values(compile("zscore(close, 20)").evaluate(cache), ind::zscore(close, 20), "zscore");
    expect_values(compile("atr(14)").evaluate(cache), ind::atr(bars, 14), "atr");
    expect_values(compile("volume").evaluate(cache), bars.volume, "volume");
    expect_values(compile("sma(rsi(close, 14), 5)").evaluate(cache),
                  ind::sma(ind::rsi(close, 14), 5),
                  "an indicator of an indicator");
    Series midpoint(close.size());
    for (std::size_t i = 0; i < close.size(); ++i) {
        midpoint[i] = (bars.high[i] + bars.low[i]) / 2.0;
    }
    expect_values(compile("hl2").evaluate(cache), midpoint, "hl2");
}

// Every function in the reference compiles and evaluates to its declared type.
void function_reference_is_complete() {
    const auto& columns = expr::column_names();
    check::expect(columns == std::vector<std::string>({"open", "high", "low", "close", "volume", "hl2"}), "columns");
    const BarSeries& bars = market();
    ind::Cache cache(bars);
    std::vector<std::string> names;
    for (const expr::FunctionInfo& info : expr::function_reference()) {
        check::expect(std::find(names.begin(), names.end(), info.name) == names.end(), info.name + " listed once");
        names.push_back(info.name);
        check::expect(info.signature.rfind(info.name + "(", 0) == 0, info.name + ": its signature");
        check::expect(!info.description.empty(), info.name + ": a description");
        // Fill the signature's parameters with something of the right kind.
        std::string call = info.name + "(";
        const std::string inside =
            info.signature.substr(info.name.size() + 1, info.signature.size() - info.name.size() - 2);
        std::size_t start = 0;
        int period = 5;
        while (start <= inside.size()) {
            std::size_t comma = inside.find(',', start);
            if (comma == std::string::npos) {
                comma = inside.size();
            }
            std::string param = inside.substr(start, comma - start);
            param.erase(0, param.find_first_not_of(' '));
            if (call.back() != '(') {
                call += ", ";
            }
            if (param == "series" || param == "a" || param == "x") {
                call += "close";
            } else if (param == "b") {
                call += "open";
            } else if (param == "k") {
                call += "2";
            } else {
                call += std::to_string(period);
                period += 7;
            }
            start = comma + 1;
        }
        call += ")";
        const expr::Program program = compile(call);
        const bool condition = program.type() == expr::Type::Condition;
        check::expect_equal(std::string(condition ? "condition" : "number"), info.returns, call + " returns");
        check::expect_equal(program.evaluate(cache).size(), bars.size(), call + " has a value per bar");
    }
    check::expect(names.size() >= 20, "the documented functions");
}

// ---- Errors ----

void type_errors() {
    expect_error("close + (close > 1)", "arithmetic needs numbers", 8, 11);
    expect_error("true + 1", "arithmetic needs numbers", 0, 4);
    expect_error("close and open", "'and' joins conditions", 0, 5);
    expect_error("close > 1 and open", "'and' joins conditions", 14, 4);
    expect_error("close or open > 1", "'or' joins conditions", 0, 5);
    expect_error("1 < close < 3", "comparisons don't chain", 10, 1);
    expect_error("close > open == true", "comparisons don't chain", 13, 2);
    expect_error("(close > 1) == (open > 1)", "comparisons need numbers on both sides", 0, 11);
    expect_error("not close", "not applies to a condition", 4, 5);
    expect_error("-(close > 1)", "can't negate a condition", 1, 11);
    expect_error("sma(close > 1, 20)", "sma needs a number here", 4, 9);

    // A rule must be a condition: a number compiles, but a strategy refuses it, pointing at the whole rule.
    check::expect(compile("sma(close, 20)").type() == expr::Type::Number, "a number compiles");
    nanotrade::StrategySpec spec;
    spec.id = "custom";
    spec.rules.long_entry = "sma(close, 20)";
    ind::Cache cache(counting());
    try {
        nanotrade::generate_signals(spec, cache, false);
        throw check::Failure("a numeric rule was accepted");
    } catch (const Error& error) {
        check::expect(std::string(error.what()).find("must be a condition") != std::string::npos, error.what());
        check::expect_equal(error.field(), std::string("strategy.long_entry"), "the field");
        check::expect_equal(error.position(), std::size_t{0}, "the position");
        check::expect_equal(error.length(), spec.rules.long_entry.size(), "the length");
    }
}

void unknown_names() {
    expect_error("clsoe > 1", "unknown name 'clsoe'; did you mean close?", 0, 5);
    expect_error("close > smaa(close, 20)", "unknown function 'smaa'; did you mean sma?", 8, 4);
    expect_error("volumes > 1", "did you mean volume?", 0, 7);
    expect_error("rsi(clsoe, 14) < 30", "did you mean close?", 4, 5);
    expect_error("crossovr(close, open)", "did you mean crossover?", 0, 8);
    check::expect_equal(message_of("close > xyzzy"),
                        std::string("unknown name 'xyzzy'"),
                        "no hint when nothing is close");
    check::expect_equal(message_of("frobnicate(close) > 1"),
                        std::string("unknown function 'frobnicate'"),
                        "no hint for a function either");
    expect_error("sma > 1", "sma is a function: sma(series, period)", 0, 3);
    expect_error("close(1) > 1", "close is a price, not a function", 0, 5);
    expect_error("and", "'and' needs a condition on each side", 0, 3);
}

void wrong_arity() {
    expect_error("sma(close) > 1", "sma takes 2 arguments: sma(series, period)", 0, 10);
    expect_error("abs(close, 1) > 0", "abs takes 1 argument: abs(x)", 0, 13);
    expect_error("atr() > 1", "atr takes 1 argument: atr(period)", 0, 5);
    expect_error("macd_signal(close, 12, 26) > 0", "macd_signal takes 4 arguments", 0, 26);
    expect_error("crossover(close) and true", "crossover takes 2 arguments", 0, 16);
}

void periods_must_be_fixed_whole_numbers() {
    expect_error("sma(close, close) > 1", "the period must be a fixed number of bars", 11, 5);
    expect_error("rsi(close, sma(close, 2)) > 1", "the period must be a fixed number of bars", 11, 13);
    expect_error("bb_upper(close, 20, close) > 1", "this must be a fixed number", 20, 5);
    expect_error("close[close] > 1", "the number of bars back must be fixed", 6, 5);
    expect_error("sma(close, 0) > 1", "from 1 to 2000, not 0", 11, 1);
    expect_error("sma(close, 2001) > 1", "from 1 to 2000, not 2001", 11, 4);
    expect_error("sma(close, 2.5) > 1", "from 1 to 2000, not 2.5", 11, 3);
    expect_error("sma(close, -3) > 1", "from 1 to 2000, not -3", 11, 2);
    expect_error("sma(close, $n) > 1", "not 0.5", 11, 2, {{"n", 0.5}});
    expect_error("close[2001] > 1", "whole number from 0 to 2000", 6, 4);
    expect_error("close[-1] > 1", "whole number from 0 to 2000", 6, 2);
    expect_error("close[1.5] > 1", "whole number from 0 to 2000", 6, 3);
    check::expect(compile("sma(close, 2000) > 1").type() == expr::Type::Condition, "a period of 2000 is allowed");
    check::expect(compile("sma(close, 1) > 1").type() == expr::Type::Condition, "a period of 1 is allowed");
    check::expect(compile("close[2000] > 1").type() == expr::Type::Condition, "a lag of 2000 is allowed");
}

void parameters_must_exist() {
    const expr::Params params{{"fast", 10}, {"slow", 30}};
    expect_error("sma(close, $fsat) > 1", "$fsat is not a parameter; did you mean fast?", 11, 5, params);
    check::expect_equal(message_of("$nope > 1"), std::string("$nope is not a parameter"), "no hint without parameters");
    expect_error("$ > 1", "expected a parameter name after $", 0, 1);
    try {
        expr::Program::compile("sma(close, $fsat) > 1", params, "strategy.long_entry");
        throw check::Failure("compiled");
    } catch (const Error& error) {
        check::expect_equal(error.field(), std::string("strategy.long_entry"), "the error carries its field");
    }
}

void syntax_errors() {
    expect_error("close = 1", "use == to compare for equality", 6, 1);
    expect_error("(close > 1", "expected ')'", 10, 1);
    expect_error("close > 1)", "unmatched ')'", 9, 1);
    expect_error("close]", "unmatched ']'", 5, 1);
    expect_error("sma(close, 20", "expected ',' or ')' in the arguments of sma", 13, 1);
    expect_error("close[1 > 0", "expected ']'", 11, 1);
    expect_error("()", "expected a number, a price such as close, or a function", 1, 1);
    expect_error("", "the expression is empty", 0, 0);
    expect_error("   \t\n", "the expression is empty", 5, 0);
    expect_error("close >", "the expression ends too soon", 7, 0);
    expect_error("close open", "expected an operator such as and, or, <, or +, before 'open'", 6, 4);
    expect_error("close > 1 @", "unexpected character '@'", 10, 1);
    expect_error("1.2.3 > 0", "'1.2.3' is not a number", 0, 5);
    expect_error("1 / 0 > close", "division by zero", 0, 5);
    expect_error("close > 1 / (2 - 2)", "division by zero", 8, 11);
}

void length_and_depth_limits() {
    std::string longest = "close > 1";
    longest.resize(2000, ' ');
    check::expect(compile(longest).type() == expr::Type::Condition, "2000 characters are allowed");
    std::string too_long = longest + " ";
    try {
        compile(too_long);
        throw check::Failure("2001 characters compiled");
    } catch (const Error& error) {
        check::expect(std::string(error.what()).find("limited to 2000 characters") != std::string::npos, error.what());
    }
    check::expect_throws<Error>([&] { expr::referenced_parameters(too_long); }, "referenced_parameters too");

    const auto nested =
        [](std::size_t depth, const std::string& open, const std::string& inner, const std::string& close) {
            std::string source;
            for (std::size_t i = 0; i < depth; ++i) {
                source += open;
            }
            source += inner;
            for (std::size_t i = 0; i < depth; ++i) {
                source += close;
            }
            return source;
        };
    check::expect_equal(compile(nested(30, "(", "close", ")") + " > 1").canonical(),
                        std::string("(close>1)"),
                        "30 parentheses are fine");
    // Lags chain rather than nest.
    expect_values(evaluate(nested(300, "", "close", "[1]"), market()),
                  evaluate("close[300]", market()),
                  "300 lags in a row");
    for (const std::string& source : {nested(100, "(", "close", ")"),
                                      nested(100, "not ", "true", ""),
                                      nested(100, "-", "close", ""),
                                      nested(100, "abs(", "close", ")"),
                                      nested(600, "(", "close", ")")}) {
        try {
            compile(source);
            throw check::Failure("deep nesting compiled: " + source.substr(0, 40));
        } catch (const Error& error) {
            check::expect(std::string(error.what()).find("nested too deeply") != std::string::npos,
                          source.substr(0, 20) + ": " + error.what());
            check::expect(error.position() < source.size(), "the position is within the source");
        }
    }
}

// ---- What a program reports about itself ----

void parameters_in_order() {
    const expr::Params params{{"fast", 10}, {"slow", 30}, {"level", 50}, {"unused", 1}};
    const expr::Program program =
        compile("sma(close, $fast) > sma(close, $slow) and rsi(close, $fast) < $level", params);
    check::expect(program.parameters() == std::vector<std::string>({"fast", "slow", "level"}), "order of first use");
    check::expect(compile("close > 1", params).parameters().empty(), "none");
    check::expect(compile("$SLOW > $Fast", params).parameters() == std::vector<std::string>({"slow", "fast"}),
                  "lowercased");

    check::expect(expr::referenced_parameters("$b > $a and $B < 3") == std::vector<std::string>({"b", "a"}),
                  "referenced without compiling");
    check::expect(expr::referenced_parameters("rsi(close, $nope) < $x") == std::vector<std::string>({"nope", "x"}),
                  "parameters need not exist");
    check::expect(expr::referenced_parameters("close > ((").empty(), "the text only has to tokenize");
    try {
        expr::referenced_parameters("close = $a", "strategy.short_exit");
        throw check::Failure("tokenized");
    } catch (const Error& error) {
        check::expect_equal(error.field(), std::string("strategy.short_exit"), "the field");
        check::expect_equal(error.position(), std::size_t{6}, "the position");
    }
}

void plots() {
    const auto expect_plots =
        [](std::string_view source, const std::vector<expr::Plot>& expected, const expr::Params& params = {}) {
            const std::vector<expr::Plot> actual = compile(source, params).plots();
            const std::string where = "plots of '" + std::string(source) + "'";
            check::expect_equal(actual.size(), expected.size(), where + ": count");
            for (std::size_t i = 0; i < expected.size(); ++i) {
                check::expect_equal(actual[i].key, expected[i].key, where + ": key");
                check::expect_equal(actual[i].label, expected[i].label, where + ": label");
                check::expect_equal(actual[i].price_scale,
                                    expected[i].price_scale,
                                    where + ": price scale of " + actual[i].key);
                check::expect_equal(actual[i].bounded, expected[i].bounded, where + ": bounded of " + actual[i].key);
            }
        };
    expect_plots("close > sma(close, 20) and rsi(close, 14) < 30",
                 {{"sma(close,20)", "SMA(20)", true, false}, {"rsi(close,14)", "RSI(14)", false, true}});
    expect_plots("crossover(ema(close, 12), ema(close, 26))",
                 {{"ema(close,12)", "EMA(12)", true, false}, {"ema(close,26)", "EMA(26)", true, false}});
    expect_plots("macd(close, 12, 26) > macd_signal(close, 12, 26, 9)",
                 {{"macd(close,12,26)", "MACD(12, 26)", false, false},
                  {"macd_signal(close,12,26,9)", "MACD signal(12, 26, 9)", false, false}});
    expect_plots("sma(high, 10) > sma(close, 10) and sma(close, 10) > 0",
                 {{"sma(high,10)", "SMA(high, 10)", true, false}, {"sma(close,10)", "SMA(10)", true, false}});
    expect_plots("abs(change(close, 1)) > atr(14)",
                 {{"change(close,1)", "Change(1)", false, false}, {"atr(14)", "ATR(14)", false, false}});
    expect_plots(
        "sma(rsi(close, 14), 5) > 50",
        {{"rsi(close,14)", "RSI(14)", false, true}, {"sma(rsi(close,14),5)", "SMA(RSI(14), 5)", false, false}});
    expect_plots("close < bb_lower(close, $period, $k)",
                 {{"bb_lower(close,20,2)", "BB lower(20, 2)", true, false}},
                 {{"period", 20}, {"k", 2}});
    expect_plots("sma(volume, 20) < volume", {{"sma(volume,20)", "SMA(volume, 20)", false, false}});
    expect_plots("close > 1 and close[1] < open", {});
    expect_plots("crossover(close, open) or max(close, open) > min(high, low)", {});
}

void levels() {
    const auto expect_levels =
        [](std::string_view source, const std::vector<double>& expected, const expr::Params& params = {}) {
            const std::vector<double> actual = compile(source, params).levels();
            check::expect(actual == expected, "levels of '" + std::string(source) + "'");
        };
    expect_levels("rsi(close, 14) > 70 or rsi(close, 14) < 30", {30, 70});
    expect_levels("70 < rsi(close, 14)", {70});
    expect_levels("rsi(close, 14) < 30 and rsi(close, 7) < 30", {30});
    expect_levels("rsi(close, 2) > 100 - $dip", {90}, {{"dip", 10}});
    expect_levels("zscore(close, 20) < -2 or zscore(close, 20) > 2", {-2, 2});
    expect_levels("roc(close, 10) > 0.05", {0.05});
    expect_levels("stdev(close, 20) > 2 * 3", {6});
    expect_levels("close > sma(close, 20)", {});
    expect_levels("sma(close, 20) > 100", {});
    expect_levels("close > 100", {});
    expect_levels("rsi(close, 14) > rsi(close, 7)", {});
}

// Evaluating stores every indicator call in the cache under its canonical text, for others to reuse.
void evaluation_fills_the_cache() {
    const BarSeries& bars = market();
    ind::Cache cache(bars);
    bool computed = false;
    const auto stored = [&](const std::string& key) -> const Series& {
        computed = false;
        const Series& values = cache.get(key, [&] {
            computed = true;
            return Series{};
        });
        check::expect(!computed, key + " is in the cache");
        return values;
    };

    const expr::Program program = compile("sma(close, 20) > 1");
    const Series first = program.evaluate(cache);
    expect_values(stored("sma(close,20)"), ind::sma(bars.close, 20), "the cached sma");
    check::expect(stored("close") == bars.close, "the cached close");
    const std::size_t entries = cache.size();
    expect_values(program.evaluate(cache), first, "evaluating again gives the same");
    check::expect_equal(cache.size(), entries, "and computes nothing new");

    compile("macd_signal(close, 12, 26, 9) > 0").evaluate(cache);
    stored("macd_signal(close,12,26,9)");
    stored("macd(close,12,26)");
    stored("ema(close,12)");
    stored("ema(close,26)");
    compile("bb_upper(close, 30, 2) > close").evaluate(cache);
    expect_values(stored("sma(close,30)"), ind::sma(bars.close, 30), "the band's sma is shared");
    expect_values(stored("stdev(close,30)"), ind::stdev(bars.close, 30), "the band's stdev is shared");

    // An indicator another program already computed is used as it is.
    ind::Cache seeded(bars);
    seeded.get("sma(close,5)", [&] { return Series(bars.size(), 42.0); });
    expect_values(compile("sma(close, 5)").evaluate(seeded), Series(bars.size(), 42.0), "the cached series is used");
}

}  // namespace

int main() {
    return check::run({
        {"and, or, and not bind in that order", precedence_of_logic},
        {"arithmetic precedence", precedence_of_arithmetic},
        {"canonical forms", canonical_forms},
        {"constants and parameters fold", constant_folding},
        {"lags", lags},
        {"three-valued logic", three_valued_logic},
        {"crossover and crossunder on the exact bar", crossings},
        {"Bollinger bands and MACD", bands_and_macd},
        {"min, max, abs, log, and the indicators", other_functions},
        {"every documented function compiles", function_reference_is_complete},
        {"type errors", type_errors},
        {"unknown names, with hints", unknown_names},
        {"wrong numbers of arguments", wrong_arity},
        {"periods and lags must be fixed whole numbers in range", periods_must_be_fixed_whole_numbers},
        {"parameters must exist", parameters_must_exist},
        {"syntax errors", syntax_errors},
        {"length and nesting limits", length_and_depth_limits},
        {"parameters in order of first use", parameters_in_order},
        {"plots", plots},
        {"levels", levels},
        {"evaluation fills the shared cache", evaluation_fills_the_cache},
    });
}
