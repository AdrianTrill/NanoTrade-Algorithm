// nanotrade: the command line. Every subcommand builds a JSON request, hands it to the same api::handle() the
// server and the WebAssembly build call, and prints the response as a report, or as JSON with --json.

#include "nanotrade/api.hpp"
#include "nanotrade/error.hpp"
#include "nanotrade/http.hpp"
#include "nanotrade/json.hpp"
#include "nanotrade/synthetic.hpp"
#include "nanotrade/text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef NANOTRADE_DATA_DIR
#define NANOTRADE_DATA_DIR "data"
#endif
#ifndef NANOTRADE_SITE_DIR
#define NANOTRADE_SITE_DIR "build/site"
#endif

namespace {

using nanotrade::Json;
namespace fs = std::filesystem;

constexpr const char* usage = R"(NanoTrade: backtest trading strategies on daily bars.

usage:
  nanotrade run [options]           backtest a strategy and print its report
  nanotrade sweep [options]         try every combination of one or two parameters, and estimate overfitting
  nanotrade walkforward [options]   optimize in sample, trade out of sample, and roll forward
  nanotrade montecarlo [options]    resample the backtest's returns to see what luck could have done
  nanotrade serve [options]         serve the lab and its JSON API at http://127.0.0.1:8080
  nanotrade api [FILE]              answer a JSON request from FILE or stdin
  nanotrade api --check|--update DIR  compare DIR/*.request.json with their golden responses, or rewrite them
  nanotrade datasets | strategies   list the bundled datasets, or the strategies and the rule language
  nanotrade synth [options]         write a random market as CSV
  nanotrade bench [--data NAME]     time backtests, sweeps, and resampling

data and strategy:
  --data NAME|FILE.csv     a bundled dataset (AAPL, BA, CAT, DELL, EBAY, F, GE) or a CSV file; default AAPL
  --synthetic SEED         a random market instead, with --bars N, --drift X, and --volatility X
  --from DATE, --to DATE   the bars to trade; earlier bars still warm up the indicators
  --strategy ID            sma_cross (default), ema_cross, macd, momentum, donchian, rsi_reversion,
                           bollinger, rsi2_pullback, buy_hold, or custom
  --param NAME=VALUE       a strategy parameter, such as --param fast=10; repeatable
  --long RULE, --long-exit RULE, --short RULE, --short-exit RULE
                           custom rules in the strategy language, such as --long "rsi(close, 2) < 10"

trading:
  --capital X              initial capital (100000)
  --allow-short            take short positions when the rules say so
  --size X                 fraction of equity per position (1)
  --vol-target X           size positions for this annual volatility instead, such as 0.15
  --leverage X             largest position value as a multiple of equity (1)
  --commission BPS         per fill, in basis points; --fixed-commission X per fill
  --slippage BPS           against each fill, in basis points
  --borrow RATE            annual cost of shorts, such as 0.02
  --stop X, --take-profit X, --trailing X   exits as fractions of the entry price, such as 0.08
  --json                   print the JSON response instead of a report

sweep and walk-forward:
  --x NAME=FROM:TO[:STEP]  the parameter to sweep; --y for a second one
  --metric NAME            sharpe (default), sortino, cagr, total_return, calmar, max_drawdown, profit_factor
  --threads N              threads for the sweep (default: one per core)
  --is-years X, --oos-years X   walk-forward windows (4 and 1); --anchored to grow the in-sample window

monte carlo:
  --paths N (1000)  --block N (20)  --seed N (1)

serve:
  --port N (8080)  --host H (127.0.0.1)  --site DIR  --data-dir DIR  --threads N
)";

std::atomic<bool> interrupted{false};

void on_signal(int) {
    interrupted.store(true);
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("can't open " + path);
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

double to_number(const std::string& text, const std::string& flag) {
    const auto value = nanotrade::text::parse_number(text);
    if (!value) {
        throw std::invalid_argument(flag + " needs a number, not '" + text + "'");
    }
    return *value;
}

// A whole number within [low, high], checked before it is cast: casting an out-of-range double is undefined.
std::uint64_t to_whole(const std::string& text, const std::string& flag, double low, double high) {
    const double value = to_number(text, flag);
    if (value != std::floor(value) || value < low || value > high) {
        throw std::invalid_argument(flag + " needs a whole number from " + nanotrade::text::format_number(low) +
                                    " to " + nanotrade::text::format_number(high));
    }
    return static_cast<std::uint64_t>(value);
}

// ---------------------------------------------------------------------------------------------------------------
// Turning flags into a request

struct Options {
    std::string data{"AAPL"};
    std::optional<double> synthetic_seed;
    double synthetic_bars{2520};
    double drift{0.0};
    double volatility{0.2};
    std::string from;
    std::string to;
    std::string strategy;
    Json params = Json::object();
    std::string rules[4];
    Json config = Json::object();
    std::string x;
    std::string y;
    std::string metric{"sharpe"};
    double threads{0};
    double is_years{4};
    double oos_years{1};
    bool anchored{false};
    double paths{1000};
    double block{20};
    double seed{1};
    bool json{false};
    std::string data_dir{NANOTRADE_DATA_DIR};
};

Json parse_axis(const std::string& text, const std::string& flag) {
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos) {
        throw std::invalid_argument(flag + " needs NAME=FROM:TO[:STEP], such as fast=5:50:5");
    }
    std::vector<std::string> parts;
    std::stringstream range(text.substr(equals + 1));
    for (std::string part; std::getline(range, part, ':');) {
        parts.push_back(part);
    }
    if (parts.size() < 2 || parts.size() > 3) {
        throw std::invalid_argument(flag + " needs NAME=FROM:TO[:STEP], such as fast=5:50:5");
    }
    Json axis = Json::object();
    axis.set("param", text.substr(0, equals));
    axis.set("from", to_number(parts[0], flag));
    axis.set("to", to_number(parts[1], flag));
    axis.set("step", parts.size() == 3 ? to_number(parts[2], flag) : 1.0);
    return axis;
}

Options parse_options(const std::vector<std::string>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& flag = args[i];
        const auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(flag + " needs a value");
            }
            return args[++i];
        };
        const auto config_number = [&](const char* key) {
            options.config.set(key, to_number(value(), flag));
        };
        if (flag == "--json") {
            options.json = true;
        } else if (flag == "--data") {
            options.data = value();
        } else if (flag == "--data-dir") {
            options.data_dir = value();
        } else if (flag == "--synthetic") {
            options.synthetic_seed = to_number(value(), flag);
        } else if (flag == "--bars") {
            options.synthetic_bars = to_number(value(), flag);
        } else if (flag == "--drift") {
            options.drift = to_number(value(), flag);
        } else if (flag == "--volatility") {
            options.volatility = to_number(value(), flag);
        } else if (flag == "--from") {
            options.from = value();
        } else if (flag == "--to") {
            options.to = value();
        } else if (flag == "--strategy") {
            options.strategy = value();
        } else if (flag == "--param") {
            const std::string& pair = value();
            const std::size_t equals = pair.find('=');
            if (equals == std::string::npos) {
                throw std::invalid_argument("--param needs NAME=VALUE, such as fast=10");
            }
            options.params.set(pair.substr(0, equals), to_number(pair.substr(equals + 1), flag));
        } else if (flag == "--long") {
            options.rules[0] = value();
        } else if (flag == "--long-exit") {
            options.rules[1] = value();
        } else if (flag == "--short") {
            options.rules[2] = value();
        } else if (flag == "--short-exit") {
            options.rules[3] = value();
        } else if (flag == "--capital") {
            config_number("initial_capital");
        } else if (flag == "--allow-short") {
            options.config.set("allow_short", true);
        } else if (flag == "--size") {
            config_number("position_size");
        } else if (flag == "--vol-target") {
            options.config.set("sizing", "volatility");
            config_number("target_volatility");
        } else if (flag == "--leverage") {
            config_number("max_leverage");
        } else if (flag == "--commission") {
            config_number("commission_bps");
        } else if (flag == "--fixed-commission") {
            config_number("commission_fixed");
        } else if (flag == "--slippage") {
            config_number("slippage_bps");
        } else if (flag == "--borrow") {
            config_number("borrow_rate");
        } else if (flag == "--stop") {
            config_number("stop_loss");
        } else if (flag == "--take-profit") {
            config_number("take_profit");
        } else if (flag == "--trailing") {
            config_number("trailing_stop");
        } else if (flag == "--x") {
            options.x = value();
        } else if (flag == "--y") {
            options.y = value();
        } else if (flag == "--metric") {
            options.metric = value();
        } else if (flag == "--threads") {
            options.threads = to_number(value(), flag);
        } else if (flag == "--is-years") {
            options.is_years = to_number(value(), flag);
        } else if (flag == "--oos-years") {
            options.oos_years = to_number(value(), flag);
        } else if (flag == "--anchored") {
            options.anchored = true;
        } else if (flag == "--paths") {
            options.paths = to_number(value(), flag);
        } else if (flag == "--block") {
            options.block = to_number(value(), flag);
        } else if (flag == "--seed") {
            options.seed = to_number(value(), flag);
        } else {
            throw std::invalid_argument("unknown option " + flag + "; see nanotrade --help");
        }
    }
    return options;
}

// Answers a request, throwing its error message if it failed.
Json call(const Json& request, const nanotrade::Progress& progress = {}) {
    Json response = Json::parse(nanotrade::api::handle(request.dump(), progress));
    if (!response.find("ok")->as_bool()) {
        const Json& error = *response.find("error");
        std::string message = error.find("message")->as_string();
        if (const Json* field = error.find("field")) {
            message = field->as_string() + ": " + message;
        }
        throw std::runtime_error(message);
    }
    return response;
}

Json data_spec(const Options& options) {
    if (options.synthetic_seed) {
        Json synthetic = Json::object();
        synthetic.set("seed", *options.synthetic_seed);
        synthetic.set("bars", options.synthetic_bars);
        synthetic.set("drift", options.drift);
        synthetic.set("volatility", options.volatility);
        Json data = Json::object();
        data.set("synthetic", std::move(synthetic));
        return data;
    }
    const bool file = options.data.find('/') != std::string::npos || options.data.find('\\') != std::string::npos ||
                      fs::path(options.data).extension() == ".csv";
    if (!file) {
        return Json(options.data);
    }
    // A CSV file: register it under its file name, then refer to it by name.
    std::string name = fs::path(options.data).stem().string();
    std::replace_if(
        name.begin(),
        name.end(),
        [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_'; },
        '_');
    Json load = Json::object();
    load.set("command", "load");
    load.set("name", name.empty() ? std::string("file") : name);
    load.set("csv", read_file(options.data));
    const Json loaded = call(load);
    const Json& report = *loaded.find("result")->find("dataset")->find("report");
    const double repaired = report.find("repaired")->as_number();
    if (repaired > 0 && !options.json) {
        std::cerr << "note: " << repaired << " bars had a high or low that didn't contain the open and close, and were "
                  << "widened\n";
    }
    return Json(load.find("name")->as_string());
}

Json build_request(const std::string& command, const Options& options) {
    Json request = Json::object();
    request.set("command", command);
    request.set("data", data_spec(options));
    if (!options.from.empty() || !options.to.empty()) {
        Json range = Json::object();
        if (!options.from.empty()) {
            range.set("from", options.from);
        }
        if (!options.to.empty()) {
            range.set("to", options.to);
        }
        request.set("range", std::move(range));
    }
    Json strategy = Json::object();
    const bool custom = std::any_of(std::begin(options.rules), std::end(options.rules), [](const std::string& rule) {
        return !rule.empty();
    });
    strategy.set("id", options.strategy.empty() ? (custom ? "custom" : "sma_cross") : options.strategy);
    if (options.params.size() > 0) {
        strategy.set("params", options.params);
    }
    const char* names[] = {"long_entry", "long_exit", "short_entry", "short_exit"};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!options.rules[i].empty()) {
            strategy.set(names[i], options.rules[i]);
        }
    }
    request.set("strategy", std::move(strategy));
    if (options.config.size() > 0) {
        request.set("config", options.config);
    }
    return request;
}

void add_sweep_fields(Json& target, const Options& options) {
    if (options.x.empty()) {
        throw std::invalid_argument("name the parameter to sweep with --x, such as --x fast=5:50:5");
    }
    target.set("x", parse_axis(options.x, "--x"));
    if (!options.y.empty()) {
        target.set("y", parse_axis(options.y, "--y"));
    }
    target.set("metric", options.metric);
    target.set("threads", options.threads);
}

// ---------------------------------------------------------------------------------------------------------------
// Printing reports

double number(const Json& object, std::string_view key) {
    const Json* value = object.find(key);
    return value != nullptr && value->is_number() ? value->as_number() : std::nan("");
}

std::string fixed(double value, int decimals) {
    if (!std::isfinite(value)) {
        return "-";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string percent(double value, int decimals = 1) {
    return std::isfinite(value) ? fixed(value * 100.0, decimals) + "%" : "-";
}

// A probability to a tenth of a percent, which never rounds to certainty.
std::string probability(double value) {
    if (std::isfinite(value) && value >= 0.9995) {
        return ">99.9%";
    }
    if (std::isfinite(value) && value < 0.0005) {
        return "<0.1%";
    }
    return percent(value);
}

std::string grouped(double value) {
    if (!std::isfinite(value)) {
        return "-";
    }
    std::string digits = fixed(std::fabs(value), 0);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return (value < 0 ? "-" : "") + digits;
}

std::string pad(const std::string& text, std::size_t width, bool right = true) {
    if (text.size() >= width) {
        return text;
    }
    return right ? std::string(width - text.size(), ' ') + text : text + std::string(width - text.size(), ' ');
}

void print_header(const Json& result) {
    const Json& dataset = *result.find("dataset");
    const Json& range = *result.find("range");
    std::cout << dataset.find("name")->as_string() << " (" << dataset.find("title")->as_string() << "), "
              << range.find("from")->as_string() << " to " << range.find("to")->as_string() << ", "
              << grouped(number(range, "bars")) << " bars\n";
}

void print_metrics_table(const Json& strategy, const Json* benchmark, const char* label) {
    struct Row {
        const char* name;
        const char* key;
        int kind;  // 0 percent, 1 ratio, 2 count, 3 money, 4 probability
    };
    const Row rows[] = {
        {"Final equity", "final_equity", 3},
        {"Total return", "total_return", 0},
        {"CAGR", "cagr", 0},
        {"Volatility", "volatility", 0},
        {"Sharpe", "sharpe", 1},
        {"Sortino", "sortino", 1},
        {"Max drawdown", "max_drawdown", 0},
        {"Calmar", "calmar", 1},
        {"Exposure", "exposure", 0},
        {"Trades", "trades", 2},
        {"Win rate", "win_rate", 0},
        {"Profit factor", "profit_factor", 1},
        {"Avg trade", "avg_trade", 0},
        {"P(Sharpe > 0)", "psr", 4},
    };
    std::cout << '\n'
              << pad("", 16, false) << pad(label, 14) << (benchmark != nullptr ? pad("Buy & hold", 14) : "") << '\n';
    for (const Row& row : rows) {
        const auto show = [&](const Json& metrics) {
            const double v = number(metrics, row.key);
            switch (row.kind) {
                case 0:
                    return percent(v);
                case 1:
                    return fixed(v, 2);
                case 2:
                    return grouped(v);
                case 4:
                    return probability(v);
                default:
                    return grouped(v);
            }
        };
        std::cout << pad(row.name, 16, false) << pad(show(strategy), 14);
        if (benchmark != nullptr && row.kind != 2 && std::string(row.key) != "win_rate" &&
            std::string(row.key) != "profit_factor" && std::string(row.key) != "avg_trade") {
            std::cout << pad(show(*benchmark), 14);
        }
        std::cout << '\n';
    }
}

void print_backtest(const Json& result) {
    print_header(result);
    const Json& strategy = *result.find("strategy");
    std::cout << strategy.find("name")->as_string();
    const Json& params = *strategy.find("params");
    for (std::size_t i = 0; i < params.size(); ++i) {
        std::cout << (i == 0 ? ": " : ", ") << params.keys()[i] << ' '
                  << nanotrade::text::format_number(params.items()[i].as_number());
    }
    std::cout << '\n';
    print_metrics_table(*result.find("metrics"), result.find("benchmark"), "Strategy");

    const Json& drawdowns = *result.find("drawdowns");
    if (drawdowns.size() > 0) {
        std::cout << "\nWorst drawdowns\n";
        for (const Json& d : drawdowns.items()) {
            std::cout << "  " << pad(percent(-number(d, "depth")), 8) << "  " << d.find("peak")->as_string() << " to "
                      << d.find("trough")->as_string() << ", "
                      << (d.find("recovery")->is_null() ? "not recovered"
                                                        : "recovered " + d.find("recovery")->as_string())
                      << '\n';
        }
    }
    if (const Json* trades = result.find("trades"); trades != nullptr && trades->size() > 0) {
        const std::size_t shown = std::min<std::size_t>(trades->size(), 10);
        std::cout << "\nLast " << shown << " of " << trades->size() << " trades\n";
        for (std::size_t i = trades->size() - shown; i < trades->size(); ++i) {
            const Json& t = trades->items()[i];
            std::cout << "  " << (number(t, "direction") > 0 ? "long " : "short") << "  "
                      << t.find("entry")->as_string() << " to " << t.find("exit")->as_string() << "  "
                      << pad(fixed(number(t, "entry_price"), 2), 9) << " -> "
                      << pad(fixed(number(t, "exit_price"), 2), 9) << "  " << pad(percent(number(t, "return")), 8)
                      << "  " << t.find("reason")->as_string() << '\n';
        }
    }
}

void print_overfitting(const Json& overfit) {
    std::cout << "\nOverfitting\n";
    std::cout << "  Deflated Sharpe ratio  " << probability(number(overfit, "deflated_sharpe"))
              << "  (the chance the best Sharpe of " << fixed(number(overfit, "best_sharpe"), 2) << " beats the "
              << fixed(number(overfit, "expected_max_sharpe"), 2) << " that luck alone would give the "
              << "best of " << grouped(number(overfit, "trials")) << " trials)\n";
    if (number(overfit, "splits") > 0) {
        std::cout << "  Probability of backtest overfitting  " << probability(number(overfit, "pbo")) << "  ("
                  << grouped(number(overfit, "splits")) << " splits of " << grouped(number(overfit, "blocks"))
                  << " blocks: how often the in-sample winner finished in the bottom half out of sample)\n";
        std::cout << "  In-sample winner lost money out of sample  "
                  << probability(number(overfit, "oos_loss_probability")) << " of splits\n";
    }
}

void print_sweep(const Json& result) {
    print_header(result);
    const Json& x = *result.find("x");
    const Json* y = result.find("y");
    const std::string metric = result.find("metric")->as_string();
    std::cout << "Swept " << x.find("param")->as_string()
              << (y->is_null() ? std::string() : " and " + y->find("param")->as_string()) << ": "
              << grouped(number(result, "trials")) << " combinations, " << grouped(number(result, "valid"))
              << " valid, ranked by " << metric << '\n';
    const auto& errors = result.find("errors")->items();
    if (!errors.empty()) {
        std::cout << "  " << grouped(number(result, "trials") - number(result, "valid"))
                  << " couldn't run, such as: " << errors.front().as_string() << '\n';
    }
    std::cout << "\nTop combinations\n";
    for (const Json& top : result.find("top")->items()) {
        std::cout << "  " << x.find("param")->as_string() << ' '
                  << pad(nanotrade::text::format_number(number(top, "x")), 6);
        if (!y->is_null()) {
            std::cout << "  " << y->find("param")->as_string() << ' '
                      << pad(nanotrade::text::format_number(number(top, "y")), 6);
        }
        std::cout << "   Sharpe " << pad(fixed(number(top, "sharpe"), 2), 6) << "   CAGR "
                  << pad(percent(number(top, "cagr")), 7) << "   max drawdown "
                  << pad(percent(number(top, "max_drawdown")), 7) << "   trades " << grouped(number(top, "trades"))
                  << '\n';
    }
    print_overfitting(*result.find("overfitting"));
}

void print_walk_forward(const Json& result) {
    print_header(result);
    const std::string x = result.find("x_param")->as_string();
    const std::string y = result.find("y_param")->as_string();
    std::cout << "Walk-forward: optimize " << x << (y.empty() ? "" : " and " + y) << " by "
              << result.find("metric")->as_string() << " in sample, then trade the next window\n\n";
    std::cout << "  out of sample              chosen" << pad("", 12) << "Sharpe in   out    CAGR out\n";
    for (const Json& fold : result.find("folds")->items()) {
        std::string chosen = x + " " + nanotrade::text::format_number(number(fold, "x"));
        if (!y.empty()) {
            chosen += ", " + y + " " + nanotrade::text::format_number(number(fold, "y"));
        }
        std::cout << "  " << fold.find("oos_from")->as_string() << " to " << fold.find("oos_to")->as_string() << "  "
                  << pad(chosen, 22, false) << pad(fixed(number(*fold.find("in_sample"), "sharpe"), 2), 9)
                  << pad(fixed(number(*fold.find("out_of_sample"), "sharpe"), 2), 7)
                  << pad(percent(number(*fold.find("out_of_sample"), "cagr")), 11) << '\n';
    }
    print_metrics_table(*result.find("metrics"), result.find("benchmark"), "Out of sample");
    std::cout << "\nWalk-forward efficiency (out-of-sample CAGR over in-sample CAGR): "
              << fixed(number(result, "efficiency"), 2) << '\n';
}

void print_monte_carlo(const Json& result) {
    print_header(result);
    std::cout << "Stationary bootstrap of the strategy's daily returns: " << grouped(number(result, "paths"))
              << " paths, mean block of " << grouped(number(result, "block")) << " bars\n\n";
    std::cout << "                      5th     25th   median     75th     95th   actual\n";
    const Json& metrics = *result.find("metrics");
    const Json& distributions = *result.find("distributions");
    struct Row {
        const char* label;
        const char* key;
        bool ratio;
    };
    for (const Row& row : {Row{"Total return", "total_return", false},
                           Row{"CAGR", "cagr", false},
                           Row{"Max drawdown", "max_drawdown", false},
                           Row{"Sharpe", "sharpe", true}}) {
        const auto& values = distributions.find(row.key)->items();
        std::cout << pad(row.label, 14, false);
        for (const double q : {0.05, 0.25, 0.5, 0.75, 0.95}) {
            const double v =
                values.empty()
                    ? std::nan("")
                    : values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1))].as_number();
            std::cout << pad(row.ratio ? fixed(v, 2) : percent(v), 9);
        }
        const double actual = number(metrics, row.key);
        std::cout << pad(row.ratio ? fixed(actual, 2) : percent(actual), 9) << '\n';
    }
    std::cout << "\nPaths that lost money: " << percent(number(result, "loss_probability")) << '\n';
}

// ---------------------------------------------------------------------------------------------------------------
// Subcommands

bool stderr_is_terminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

// A percentage on stderr while a sweep, walk-forward test, or resampling runs, erased when it ends however it ends.
// Only on a terminal: a redirected stderr gets no carriage returns.
class ProgressLine {
public:
    explicit ProgressLine(bool enabled) : enabled_(enabled && stderr_is_terminal()) {}
    ProgressLine(const ProgressLine&) = delete;
    ProgressLine& operator=(const ProgressLine&) = delete;
    ~ProgressLine() {
        clear();
    }

    nanotrade::Progress callback() {
        if (!enabled_) {
            return {};
        }
        return [this](double done) {
            const int percent = std::clamp(static_cast<int>(done * 100.0), 0, 100);
            if (percent != shown_) {
                shown_ = percent;
                std::cerr << "\r  " << percent << "%" << std::flush;
            }
        };
    }

    void clear() {
        if (shown_ >= 0) {
            std::cerr << "\r      \r" << std::flush;
            shown_ = -1;
        }
    }

private:
    bool enabled_;
    int shown_{-1};
};

int run_research(const std::string& command, const std::vector<std::string>& args) {
    const Options options = parse_options(args);
    nanotrade::api::set_data_directory(options.data_dir);
    Json request = build_request(command == "run" ? "backtest" : command, options);
    if (command == "sweep") {
        Json sweep = Json::object();
        add_sweep_fields(sweep, options);
        request.set("sweep", std::move(sweep));
    } else if (command == "walkforward") {
        Json walk = Json::object();
        add_sweep_fields(walk, options);
        walk.set("in_sample_years", options.is_years);
        walk.set("out_of_sample_years", options.oos_years);
        walk.set("anchored", options.anchored);
        request.set("walkforward", std::move(walk));
    } else if (command == "montecarlo") {
        Json monte = Json::object();
        monte.set("paths", options.paths);
        monte.set("block", options.block);
        monte.set("seed", options.seed);
        request.set("montecarlo", std::move(monte));
    }
    if (options.json) {
        const auto start = std::chrono::steady_clock::now();
        std::string response = nanotrade::api::handle(request.dump());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << nanotrade::api::with_meta(std::move(response), "native", ms) << '\n';
        return 0;
    }
    ProgressLine progress(command != "run");
    const Json response = call(request, progress.callback());
    progress.clear();
    const Json& result = *response.find("result");
    if (command == "run") {
        print_backtest(result);
    } else if (command == "sweep") {
        print_sweep(result);
    } else if (command == "walkforward") {
        print_walk_forward(result);
    } else {
        print_monte_carlo(result);
    }
    return 0;
}

int run_api(const std::vector<std::string>& args) {
    nanotrade::api::set_data_directory(NANOTRADE_DATA_DIR);
    if (!args.empty() && (args[0] == "--check" || args[0] == "--update")) {
        if (args.size() != 2) {
            throw std::invalid_argument(args[0] + " needs a directory of *.request.json files");
        }
        const bool update = args[0] == "--update";
        std::vector<fs::path> requests;
        for (const auto& entry : fs::directory_iterator(args[1])) {
            const std::string name = entry.path().filename().string();
            if (name.size() > 13 && name.substr(name.size() - 13) == ".request.json") {
                requests.push_back(entry.path());
            }
        }
        std::sort(requests.begin(), requests.end());
        std::size_t failures = 0;
        for (const fs::path& path : requests) {
            const std::string name = path.filename().string();
            const fs::path expected_path = path.parent_path() / (name.substr(0, name.size() - 13) + ".response.json");
            const std::string response = nanotrade::api::handle(read_file(path.string())) + "\n";
            if (update) {
                std::ofstream(expected_path, std::ios::binary) << response;
                std::cout << "wrote " << expected_path.filename().string() << '\n';
                continue;
            }
            const std::string expected = fs::exists(expected_path) ? read_file(expected_path.string()) : std::string();
            if (response == expected) {
                std::cout << "ok    " << name << '\n';
                continue;
            }
            ++failures;
            const auto mismatch = std::mismatch(response.begin(), response.end(), expected.begin(), expected.end());
            const auto offset = static_cast<std::size_t>(mismatch.first - response.begin());
            std::cout << "FAIL  " << name << ": differs from " << expected_path.filename().string() << " at byte "
                      << offset << "\n      got      ..." << response.substr(offset > 40 ? offset - 40 : 0, 100)
                      << "\n      expected ..." << expected.substr(offset > 40 ? offset - 40 : 0, 100) << '\n';
        }
        if (!update) {
            std::cout << (requests.size() - failures) << " of " << requests.size() << " golden responses match\n";
        }
        return failures == 0 ? 0 : 1;
    }
    std::string request;
    if (args.empty() || args[0] == "-") {
        std::ostringstream text;
        text << std::cin.rdbuf();
        request = text.str();
    } else {
        request = read_file(args[0]);
    }
    std::cout << nanotrade::api::handle(request) << '\n';
    return 0;
}

int run_serve(const std::vector<std::string>& args) {
    nanotrade::http::ServerOptions server;
    std::string site = NANOTRADE_SITE_DIR;
    std::string data = NANOTRADE_DATA_DIR;
    bool quiet = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(args[i] + " needs a value");
            }
            return args[++i];
        };
        if (args[i] == "--port") {
            server.port = static_cast<std::uint16_t>(to_whole(value(), "--port", 0, 65535));
        } else if (args[i] == "--host") {
            server.host = value();
        } else if (args[i] == "--site") {
            site = value();
        } else if (args[i] == "--data-dir") {
            data = value();
        } else if (args[i] == "--threads") {
            server.threads = static_cast<std::size_t>(to_whole(value(), "--threads", 1, 256));
        } else if (args[i] == "--quiet") {
            quiet = true;
        } else {
            throw std::invalid_argument("unknown option " + args[i] + " for serve");
        }
    }
    nanotrade::api::set_data_directory(data);
    std::mutex log_mutex;
    const auto handler = [&](const nanotrade::http::Request& request) {
        const auto start = std::chrono::steady_clock::now();
        nanotrade::http::Response response;
        std::string note;
        if (request.path == "/js/runtime.js") {
            // Tells the page that the engine is here: the site's own copy of this module says WebAssembly.
            response.content_type = "text/javascript; charset=utf-8";
            response.body = "export const runtime = 'native';\n";
            response.headers.emplace_back("Cache-Control", "no-store");
        } else if (request.path == "/api/health") {
            response.body =
                std::string("{\"ok\":true,\"runtime\":\"native\",\"version\":\"") + nanotrade::api::version + "\"}";
        } else if (request.path == "/api") {
            if (request.method != "POST") {
                response.status = 405;
                response.body = R"({"ok":false,"error":{"message":"POST a JSON request to /api"}})";
                response.headers.emplace_back("Allow", "POST");
            } else {
                std::string body = nanotrade::api::handle(request.body);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                response.body = nanotrade::api::with_meta(std::move(body), "native", ms);
                const auto command = request.body.find("\"command\"");
                note = command == std::string::npos ? "" : " " + request.body.substr(command + 10, 24);
            }
            response.headers.emplace_back("Cache-Control", "no-store");
        } else if (request.method == "GET" || request.method == "HEAD") {
            if (auto file = nanotrade::http::serve_static(site, request.path)) {
                response = std::move(*file);
            } else {
                response.status = 404;
                response.content_type = "text/plain; charset=utf-8";
                response.body = "not found\n";
            }
        } else {
            response.status = 405;
            response.content_type = "text/plain; charset=utf-8";
            response.body = "method not allowed\n";
        }
        if (!quiet) {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << request.method << ' ' << request.path << ' ' << response.status << "  " << fixed(ms, 1)
                      << " ms" << note.substr(0, note.find(',')) << '\n'
                      << std::flush;
        }
        return response;
    };

    nanotrade::http::Server http_server(server, handler);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    http_server.start();
    std::cout << "NanoTrade " << nanotrade::api::version << " serving http://" << server.host << ':'
              << http_server.port() << "/\n  site: " << site
              << (fs::exists(fs::path(site) / "index.html") ? "" : " (missing: build the nanotrade_site target)")
              << "\n  data: " << data << "\nPress Ctrl+C to stop.\n"
              << std::flush;
    while (!interrupted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    http_server.stop();
    std::cout << "\nstopped\n";
    return 0;
}

int run_synth(const std::vector<std::string>& args) {
    nanotrade::SyntheticSpec spec;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& flag = args[i];
        const auto text = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(flag + " needs a value");
            }
            return args[++i];
        };
        if (flag == "--seed") {
            spec.seed = to_whole(text(), flag, 0, 9007199254740991.0);
        } else if (flag == "--bars") {
            spec.bars = static_cast<std::size_t>(to_whole(text(), flag, 50, 20000));
        } else if (flag == "--drift") {
            spec.drift = to_number(text(), flag);
        } else if (flag == "--volatility") {
            spec.volatility = to_number(text(), flag);
        } else if (flag == "--start-price") {
            spec.start_price = to_number(text(), flag);
        } else {
            throw std::invalid_argument("unknown option " + args[i] + " for synth");
        }
    }
    const nanotrade::BarSeries bars = nanotrade::synthesize(spec);
    std::string out = "Date,Open,High,Low,Close,Volume\n";
    for (std::size_t i = 0; i < bars.size(); ++i) {
        out += nanotrade::format_date(bars.time[i]);
        for (const double v : {bars.open[i], bars.high[i], bars.low[i], bars.close[i]}) {
            out += ',' + nanotrade::text::format_number(v, 6);
        }
        out += ',' + nanotrade::text::format_number(bars.volume[i]) + '\n';
    }
    std::cout << out;
    return 0;
}

int run_list(const std::string& what) {
    nanotrade::api::set_data_directory(NANOTRADE_DATA_DIR);
    Json request = Json::object();
    request.set("command", what == "datasets" ? "datasets" : "catalog");
    const Json result = *call(request).find("result");
    if (what == "datasets") {
        for (const Json& dataset : result.find("datasets")->items()) {
            std::cout << pad(dataset.find("name")->as_string(), 6, false) << "  "
                      << pad(dataset.find("title")->as_string(), 18, false) << pad(grouped(number(dataset, "bars")), 7)
                      << " bars  " << dataset.find("first")->as_string() << " to " << dataset.find("last")->as_string()
                      << '\n';
        }
        return 0;
    }
    for (const Json& strategy : result.find("strategies")->items()) {
        std::cout << pad(strategy.find("id")->as_string(), 15, false) << strategy.find("summary")->as_string() << '\n';
        const Json& rules = *strategy.find("rules");
        for (const char* key : {"long_entry", "long_exit", "short_entry", "short_exit"}) {
            const std::string& rule = rules.find(key)->as_string();
            if (!rule.empty()) {
                std::cout << pad("", 17, false) << pad(key, 12, false) << rule << '\n';
            }
        }
    }
    std::cout << "\nFunctions of the rule language\n";
    for (const Json& function : result.find("functions")->items()) {
        std::cout << "  " << pad(function.find("signature")->as_string(), 42, false)
                  << function.find("description")->as_string() << '\n';
    }
    std::cout << "\nPrices: open, high, low, close, volume, hl2. Write x[n] for x n bars ago, $name for a parameter.\n";
    return 0;
}

int run_bench(const std::vector<std::string>& args) {
    std::string data = "GE";
    bool json = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) {
            data = args[++i];
        } else if (args[i] == "--json") {
            json = true;
        } else {
            throw std::invalid_argument("unknown option " + args[i] + " for bench");
        }
    }
    nanotrade::api::set_data_directory(NANOTRADE_DATA_DIR);
    const auto time = [](const Json& request, int runs) {
        std::vector<double> times;
        const std::string text = request.dump();
        for (int r = 0; r < runs; ++r) {
            const auto start = std::chrono::steady_clock::now();
            const std::string response = nanotrade::api::handle(text);
            times.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (response.rfind("{\"ok\":true", 0) != 0) {
                throw std::runtime_error("benchmark request failed: " + response.substr(0, 300));
            }
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    };
    Json backtest = Json::object();
    backtest.set("command", "backtest");
    backtest.set("data", data);
    Json strategy = Json::object();
    strategy.set("id", "sma_cross");
    backtest.set("strategy", strategy);
    Json no_series = Json::object();
    no_series.set("series", false);
    no_series.set("trades", false);

    Json warm = backtest;
    time(warm, 3);  // loads the data and fills the indicator cache
    const double report_ms = time(backtest, 21);
    Json engine_only = backtest;
    engine_only.set("output", no_series);
    const double engine_ms = time(engine_only, 21);

    const auto sweep_request = [&](double threads) {
        Json sweep = Json::object();
        Json x = Json::object();
        x.set("param", "fast");
        x.set("from", 5);
        x.set("to", 200);
        x.set("step", 5);
        Json y = Json::object();
        y.set("param", "slow");
        y.set("from", 10);
        y.set("to", 400);
        y.set("step", 10);
        sweep.set("x", x);
        sweep.set("y", y);
        sweep.set("threads", threads);
        Json request = backtest;
        request.set("command", "sweep");
        request.set("sweep", sweep);
        return request;
    };
    const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
    const double sweep_one = time(sweep_request(1), 3);
    const double sweep_all = time(sweep_request(cores), 3);

    Json monte = backtest;
    monte.set("command", "montecarlo");
    Json paths = Json::object();
    paths.set("paths", 1000);
    monte.set("montecarlo", paths);
    const double monte_ms = time(monte, 5);

    const auto bars = static_cast<double>(nanotrade::api::find_dataset(data)->series.size());
    const double trials = 40.0 * 40.0;
    if (json) {
        std::cout << "{\"data\":\"" << data << "\",\"bars\":" << bars << ",\"report_ms\":" << report_ms
                  << ",\"backtest_ms\":" << engine_ms << ",\"sweep_trials\":" << trials
                  << ",\"sweep_ms_1_thread\":" << sweep_one << ",\"sweep_ms_all_threads\":" << sweep_all
                  << ",\"threads\":" << cores << ",\"montecarlo_ms\":" << monte_ms << "}\n";
        return 0;
    }
    std::cout << data << ": " << grouped(bars) << " bars, SMA crossover 20/50, medians\n";
    std::cout << "  backtest, metrics, and benchmark       " << pad(fixed(engine_ms, 2), 8) << " ms   "
              << fixed(bars / engine_ms / 1000.0, 1) << "M bars/s\n";
    std::cout << "  the same with series and trades JSON   " << pad(fixed(report_ms, 2), 8) << " ms\n";
    std::cout << "  sweep of 40 x 40 SMA pairs, 1 thread   " << pad(fixed(sweep_one, 0), 8) << " ms   "
              << grouped(trials / sweep_one * 1000.0) << " backtests/s\n";
    std::cout << "  the same on " << cores << " threads" << pad("", cores < 10 ? 20 : 19) << pad(fixed(sweep_all, 0), 8)
              << " ms   " << grouped(trials / sweep_all * 1000.0) << " backtests/s\n";
    std::cout << "  Monte Carlo, 1,000 paths               " << pad(fixed(monte_ms, 0), 8) << " ms\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
            std::cout << usage;
            return 0;
        }
        if (args[0] == "--version") {
            std::cout << "nanotrade " << nanotrade::api::version << '\n';
            return 0;
        }
        const std::string command = args[0];
        const std::vector<std::string> rest(args.begin() + 1, args.end());
        if (command == "run" || command == "sweep" || command == "walkforward" || command == "montecarlo") {
            return run_research(command, rest);
        }
        if (command == "api") {
            return run_api(rest);
        }
        if (command == "serve") {
            return run_serve(rest);
        }
        if (command == "synth") {
            return run_synth(rest);
        }
        if (command == "datasets" || command == "strategies") {
            return run_list(command);
        }
        if (command == "bench") {
            return run_bench(rest);
        }
        throw std::invalid_argument("unknown command " + command + "; see nanotrade --help");
    } catch (const std::exception& error) {
        std::cerr << "nanotrade: " << error.what() << '\n';
        return 1;
    }
}
