#include <cctype>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "nanotrade/backtest_engine.h"
#include "nanotrade/market_data.h"
#include "nanotrade/reporting.h"
#include "nanotrade/strategies.h"

namespace {

struct CliOptions {
    std::string csv_path = "data/sample_ohlcv.csv";
    std::string strategy_name = "sma";
    std::size_t short_window = 3;
    std::size_t long_window = 5;
    std::size_t lookback = 3;
    std::size_t rsi_period = 5;
    double rsi_entry = 30.0;
    double rsi_exit = 55.0;
    double initial_cash = 10000.0;
    int order_quantity = 10;
    double commission = 0.0;
    double slippage_bps = 0.0;
    bool show_help = false;
    bool list_strategies = false;
};

std::string nextValue(int argc, char** argv, int& index, const std::string& flag) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + flag);
    }
    return argv[++index];
}

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::size_t parseSize(const std::string& value, const std::string& flag) {
    try {
        const std::size_t parsed = static_cast<std::size_t>(std::stoul(value));
        if (parsed == 0) {
            throw std::invalid_argument("");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument("Expected a positive integer for " + flag);
    }
}

int parseInt(const std::string& value, const std::string& flag) {
    try {
        const int parsed = std::stoi(value);
        if (parsed <= 0) {
            throw std::invalid_argument("");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument("Expected a positive integer for " + flag);
    }
}

double parseNonNegativeDouble(const std::string& value, const std::string& flag, bool strictly_positive = false) {
    try {
        const double parsed = std::stod(value);
        if (strictly_positive ? parsed <= 0.0 : parsed < 0.0) {
            throw std::invalid_argument("");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument(
            std::string("Expected a ") + (strictly_positive ? "positive" : "non-negative") + " number for " + flag
        );
    }
}

void printUsage() {
    std::cout
        << "NanoTrade Algorithm\n"
        << "Deterministic single-instrument backtester\n\n"
        << "Usage:\n"
        << "  nanotrade [options]\n\n"
        << "Core options:\n"
        << "  --csv <path>               Input CSV file (default: data/sample_ohlcv.csv)\n"
        << "  --strategy <name>          sma | ema | momentum | rsi (default: sma)\n"
        << "  --list-strategies          Print supported strategies and exit\n"
        << "  --help                     Show this help message and exit\n\n"
        << "Strategy parameters:\n"
        << "  --short-window <n>         Short window for SMA/EMA crossover\n"
        << "  --long-window <n>          Long window for SMA/EMA crossover\n"
        << "  --lookback <n>             Lookback for momentum strategy\n"
        << "  --rsi-period <n>           RSI period\n"
        << "  --rsi-entry <value>        RSI entry threshold\n"
        << "  --rsi-exit <value>         RSI exit threshold\n\n"
        << "Execution parameters:\n"
        << "  --initial-cash <value>     Starting cash balance\n"
        << "  --quantity <n>             Shares per trade\n"
        << "  --commission <value>       Fixed commission per order\n"
        << "  --slippage-bps <value>     Slippage applied to fills in basis points\n\n"
        << "Simulation model:\n"
        << "  Signals are evaluated on the close and filled on the next bar open.\n"
        << "  Positions are long-only with a fixed quantity per order.\n\n"
        << "Examples:\n"
        << "  nanotrade\n"
        << "  nanotrade --strategy ema --short-window 3 --long-window 6\n"
        << "  nanotrade --strategy momentum --lookback 3 --commission 1.00 --slippage-bps 5\n";
}

void printStrategies() {
    std::cout
        << "Supported strategies:\n"
        << "  sma       Short SMA crosses above long SMA\n"
        << "  ema       Short EMA crosses above long EMA\n"
        << "  momentum  Close is above the close N bars ago\n"
        << "  rsi       Enter below a lower RSI threshold, exit above an upper threshold\n";
}

CliOptions parseArgs(int argc, char** argv) {
    CliOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg == "--list-strategies") {
            options.list_strategies = true;
        } else if (arg == "--csv") {
            options.csv_path = nextValue(argc, argv, i, arg);
        } else if (arg == "--strategy") {
            options.strategy_name = toLower(nextValue(argc, argv, i, arg));
        } else if (arg == "--short-window") {
            options.short_window = parseSize(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--long-window") {
            options.long_window = parseSize(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--lookback") {
            options.lookback = parseSize(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--rsi-period") {
            options.rsi_period = parseSize(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--rsi-entry") {
            options.rsi_entry = parseNonNegativeDouble(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--rsi-exit") {
            options.rsi_exit = parseNonNegativeDouble(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--initial-cash") {
            options.initial_cash = parseNonNegativeDouble(nextValue(argc, argv, i, arg), arg, true);
        } else if (arg == "--quantity") {
            options.order_quantity = parseInt(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--commission") {
            options.commission = parseNonNegativeDouble(nextValue(argc, argv, i, arg), arg);
        } else if (arg == "--slippage-bps") {
            options.slippage_bps = parseNonNegativeDouble(nextValue(argc, argv, i, arg), arg);
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }

    return options;
}

std::unique_ptr<nanotrade::Strategy> makeStrategy(const CliOptions& options) {
    using nanotrade::EmaCrossoverStrategy;
    using nanotrade::MomentumStrategy;
    using nanotrade::RsiThresholdStrategy;
    using nanotrade::SmaCrossoverStrategy;

    if (options.strategy_name == "sma") {
        return std::make_unique<SmaCrossoverStrategy>(options.short_window, options.long_window);
    }
    if (options.strategy_name == "ema") {
        return std::make_unique<EmaCrossoverStrategy>(options.short_window, options.long_window);
    }
    if (options.strategy_name == "momentum") {
        return std::make_unique<MomentumStrategy>(options.lookback);
    }
    if (options.strategy_name == "rsi") {
        return std::make_unique<RsiThresholdStrategy>(options.rsi_period, options.rsi_entry, options.rsi_exit);
    }

    throw std::invalid_argument("Unsupported strategy: " + options.strategy_name);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const CliOptions options = parseArgs(argc, argv);

        if (options.show_help) {
            printUsage();
            return EXIT_SUCCESS;
        }
        if (options.list_strategies) {
            printStrategies();
            return EXIT_SUCCESS;
        }

        const auto bars = nanotrade::MarketDataLoader::loadCsvFile(options.csv_path);
        const auto strategy = makeStrategy(options);

        nanotrade::BacktestConfig config;
        config.initial_cash = options.initial_cash;
        config.order_quantity = options.order_quantity;
        config.commission_per_trade = options.commission;
        config.slippage_bps = options.slippage_bps;

        const nanotrade::BacktestResult result = nanotrade::BacktestEngine::run(bars, *strategy, config);
        std::cout << nanotrade::formatSummary(result) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        std::cerr << "Run 'nanotrade --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
