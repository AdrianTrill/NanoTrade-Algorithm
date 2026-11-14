#include "nanotrade/api.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/expression.hpp"
#include "nanotrade/json.hpp"
#include "nanotrade/portfolio.hpp"
#include "nanotrade/synthetic.hpp"
#include "nanotrade/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace nanotrade::api {
namespace {

constexpr int series_digits = 7;  // charts need no more, and it keeps a 13,000-bar response small
constexpr std::size_t max_datasets = 64;
constexpr std::size_t max_cached_indicators = 400;
constexpr std::size_t max_trades = 5000;

// ---------------------------------------------------------------------------------------------------------------
// Datasets

struct Entry {
    std::shared_ptr<const Dataset> dataset;
    std::shared_ptr<indicators::Cache> cache;  // shares the dataset's lifetime through the Entry that holds both
    std::uint64_t added{0};
};

struct Registry {
    std::mutex mutex;
    std::map<std::string, Entry> entries;
    std::string directory;
    Json manifest;
    std::uint64_t counter{0};
};

Registry& registry() {
    static Registry instance;
    return instance;
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
               c == '.' || c == ':';
    });
}

// Names that can be files in the data directory: no dots, so nothing can climb out of it.
bool file_name(std::string_view name) {
    return valid_name(name) && name.find('.') == std::string_view::npos && name.find(':') == std::string_view::npos;
}

// Whether the data directory holds a file named exactly so. Opening the file by name would find AAPL.csv for "aapl"
// on a file system that ignores case, and the same request would then mean different things on different machines.
bool has_data_file(const std::string& directory, const std::string& file) {
#if defined(__EMSCRIPTEN__)
    // The browser has no data directory: its datasets arrive through load.
    (void)directory;
    (void)file;
    return false;
#else
    std::error_code error;
    const std::filesystem::path wanted(file);
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (it->path().filename() == wanted) {
            return true;
        }
    }
    return false;
#endif
}

const Json* manifest_entry(const Json& manifest, std::string_view name) {
    const Json* list = manifest.find("datasets");
    if (list == nullptr || !list->is_array()) {
        return nullptr;
    }
    for (const Json& item : list->items()) {
        const Json* symbol = item.find("symbol");
        if (symbol != nullptr && symbol->is_string() && symbol->as_string() == name) {
            return &item;
        }
    }
    return nullptr;
}

std::string string_field(const Json* object, std::string_view key) {
    const Json* value = object != nullptr ? object->find(key) : nullptr;
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

Entry acquire(const std::string& name, const std::string& field = "data") {
    Registry& reg = registry();
    std::string directory;
    Json manifest;
    {
        const std::lock_guard<std::mutex> lock(reg.mutex);
        const auto found = reg.entries.find(name);
        if (found != reg.entries.end()) {
            if (found->second.cache->size() > max_cached_indicators) {
                found->second.cache = std::make_shared<indicators::Cache>(found->second.dataset->series);
            }
            return found->second;
        }
        directory = reg.directory;
        manifest = reg.manifest;
    }
    std::vector<std::string> known = dataset_names();
    const std::string hint = text::closest(name, known);
    if (directory.empty() || !file_name(name)) {
        throw Error("unknown dataset '" + name + "'" + (hint.empty() ? "" : "; did you mean " + hint + "?"), field);
    }
    const std::string path = directory + "/" + name + ".csv";
    if (!has_data_file(directory, name + ".csv")) {
        throw Error("unknown dataset '" + name + "'" + (hint.empty() ? "" : "; did you mean " + hint + "?"), field);
    }
    LoadedSeries loaded = load_csv_file(path, name);
    const Json* about = manifest_entry(manifest, name);
    Dataset dataset;
    dataset.name = name;
    dataset.title = about != nullptr ? string_field(about, "name") : name;
    dataset.source = string_field(&manifest, "source");
    dataset.notes = string_field(about, "notes");
    dataset.series = std::move(loaded.series);
    dataset.report = std::move(loaded.report);
    register_dataset(std::move(dataset));
    return acquire(name, field);
}

// ---------------------------------------------------------------------------------------------------------------
// Reading requests

class Reader {
public:
    Reader(const Json* json, std::string path) : json_(json), path_(std::move(path)) {
        if (json_ != nullptr && !json_->is_null() && !json_->is_object()) {
            throw Error(path_ + " must be an object", path_);
        }
    }

    std::string field(std::string_view key) const {
        return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
    }
    const Json* get(std::string_view key) const {
        return json_ != nullptr && json_->is_object() ? json_->find(key) : nullptr;
    }
    bool has(std::string_view key) const {
        const Json* value = get(key);
        return value != nullptr && !value->is_null();
    }
    double number(std::string_view key, double fallback) const {
        const Json* value = get(key);
        if (value == nullptr || value->is_null()) {
            return fallback;
        }
        if (!value->is_number()) {
            throw Error(field(key) + " must be a number", field(key));
        }
        return value->as_number();
    }
    std::size_t whole(std::string_view key, std::size_t fallback, std::size_t min, std::size_t max) const {
        const double value = number(key, static_cast<double>(fallback));
        if (value != std::floor(value) || value < static_cast<double>(min) || value > static_cast<double>(max)) {
            throw Error(
                field(key) + " must be a whole number from " + std::to_string(min) + " to " + std::to_string(max),
                field(key));
        }
        return static_cast<std::size_t>(value);
    }
    bool flag(std::string_view key, bool fallback) const {
        const Json* value = get(key);
        if (value == nullptr || value->is_null()) {
            return fallback;
        }
        if (!value->is_bool()) {
            throw Error(field(key) + " must be true or false", field(key));
        }
        return value->as_bool();
    }
    // A seed: any whole number JSON carries exactly, below 2^53. Not a size_t, which is 32 bits in WebAssembly.
    std::uint64_t seed(std::string_view key, std::uint64_t fallback) const {
        const double value = number(key, static_cast<double>(fallback));
        if (value != std::floor(value) || value < 0.0 || value > 9007199254740991.0) {
            throw Error(field(key) + " must be a whole number from 0 to 9007199254740991", field(key));
        }
        return static_cast<std::uint64_t>(value);
    }
    std::string string(std::string_view key, const std::string& fallback) const {
        const Json* value = get(key);
        if (value == nullptr || value->is_null()) {
            return fallback;
        }
        if (!value->is_string()) {
            throw Error(field(key) + " must be a string", field(key));
        }
        return value->as_string();
    }
    Reader child(std::string_view key) const {
        return {get(key), field(key)};
    }
    const Json* json() const noexcept {
        return json_;
    }
    // Unknown fields are refused, so a misspelled option fails loudly instead of being ignored.
    void allow(std::initializer_list<const char*> keys) const {
        if (json_ == nullptr || !json_->is_object()) {
            return;
        }
        const std::vector<std::string> allowed(keys.begin(), keys.end());
        for (const std::string& key : json_->keys()) {
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
                const std::string hint = text::closest(key, allowed);
                throw Error("unknown field " + field(key) + (hint.empty() ? "" : "; did you mean " + hint + "?"),
                            field(key));
            }
        }
    }

private:
    const Json* json_;
    std::string path_;
};

struct Window {
    Entry entry;
    std::size_t begin{0};
    std::size_t end{0};

    const BarSeries& bars() const {
        return entry.dataset->series;
    }
};

// The data a request names at `field`: a dataset's name, or an object with a dataset, a synthetic market, or CSV text.
Entry resolve_data_at(const Json* data, const std::string& field) {
    if (data == nullptr || data->is_null()) {
        throw Error("say which data to use, such as \"data\": \"AAPL\"", field);
    }
    if (data->is_string()) {
        return acquire(data->as_string(), field);
    }
    const Reader reader(data, field);
    reader.allow({"dataset", "synthetic", "csv", "name"});
    if (reader.has("dataset")) {
        return acquire(reader.string("dataset", ""), reader.field("dataset"));
    }
    if (reader.has("synthetic")) {
        const Reader spec_reader = reader.child("synthetic");
        spec_reader.allow({"seed", "bars", "drift", "volatility", "start_price"});
        SyntheticSpec spec;
        spec.seed = spec_reader.seed("seed", spec.seed);
        spec.bars = spec_reader.whole("bars", spec.bars, 50, 20'000);
        spec.drift = spec_reader.number("drift", spec.drift);
        spec.volatility = spec_reader.number("volatility", spec.volatility);
        spec.start_price = spec_reader.number("start_price", spec.start_price);
        const std::string name = "synthetic:" + std::to_string(spec.seed) + ":" + std::to_string(spec.bars) + ":" +
                                 text::format_number(spec.drift) + ":" + text::format_number(spec.volatility) + ":" +
                                 text::format_number(spec.start_price);
        {
            Registry& reg = registry();
            const std::lock_guard<std::mutex> lock(reg.mutex);
            const auto found = reg.entries.find(name);
            if (found != reg.entries.end()) {
                return found->second;
            }
        }
        Dataset dataset;
        dataset.name = name;
        dataset.title = "Random market, seed " + std::to_string(spec.seed);
        dataset.source = "synthetic";
        dataset.notes = "GARCH(1,1) volatility around " + text::format_number(spec.volatility * 100.0) +
                        "% a year, drift " + text::format_number(spec.drift * 100.0) + "% a year";
        dataset.series = synthesize(spec);
        register_dataset(std::move(dataset));
        return acquire(name, field);
    }
    if (reader.has("csv")) {
        const std::string name = reader.string("name", "upload");
        LoadedSeries loaded = load_csv(reader.string("csv", ""), name);
        auto dataset = std::make_shared<Dataset>();
        dataset->name = name;
        dataset->title = name;
        dataset->source = "inline CSV";
        dataset->series = std::move(loaded.series);
        dataset->report = std::move(loaded.report);
        auto cache = std::make_shared<indicators::Cache>(dataset->series);
        return {std::move(dataset), std::move(cache), 0};
    }
    throw Error(field + " needs a dataset, a synthetic market, or csv", field);
}

Entry resolve_data(const Json& request) {
    return resolve_data_at(request.find("data"), "data");
}

// The dates a request's range names, the last included: a plain date such as 2012-12-31 covers its whole day.
struct Dates {
    std::optional<Timestamp> from;
    std::optional<Timestamp> to;
};

Dates parse_dates(const Json& request) {
    const Reader reader(request.find("range"), "range");
    reader.allow({"from", "to"});
    Dates dates;
    if (reader.has("from")) {
        const std::string text = reader.string("from", "");
        dates.from = parse_timestamp(text);
        if (!dates.from) {
            throw Error("range.from must be a date such as 2005-01-31, not '" + text + "'", "range.from");
        }
    }
    if (reader.has("to")) {
        const std::string text = reader.string("to", "");
        const auto to = parse_timestamp(text);
        if (!to) {
            throw Error("range.to must be a date such as 2012-12-31, not '" + text + "'", "range.to");
        }
        dates.to = is_calendar_date(text) ? *to + seconds_per_day - 1 : *to;
    }
    return dates;
}

Window resolve_window(const Json& request) {
    Window window;
    window.entry = resolve_data(request);
    const BarSeries& bars = window.bars();
    const Dates dates = parse_dates(request);
    window.begin = dates.from ? bars.lower_bound(*dates.from) : 0;
    window.end = dates.to ? bars.upper_bound(*dates.to) : bars.size();
    if (window.end < window.begin + 2) {
        throw Error("the date range holds fewer than two bars of " + window.entry.dataset->name + ", which runs from " +
                        format_date(bars.time.front()) + " to " + format_date(bars.time.back()),
                    "range");
    }
    return window;
}

StrategySpec parse_strategy(const Json& request) {
    const Reader reader(request.find("strategy"), "strategy");
    reader.allow({"id", "params", "long_entry", "long_exit", "short_entry", "short_exit"});
    StrategySpec spec;
    spec.id = reader.string("id", "sma_cross");
    if (find_strategy(spec.id) == nullptr) {
        std::vector<std::string> ids;
        for (const StrategyInfo& info : strategy_catalog()) {
            ids.push_back(info.id);
        }
        const std::string hint = text::closest(spec.id, ids);
        throw Error("unknown strategy '" + spec.id + "'" + (hint.empty() ? "" : "; did you mean " + hint + "?"),
                    "strategy.id");
    }
    const Reader params = reader.child("params");
    if (params.json() != nullptr && params.json()->is_object()) {
        const Json& object = *params.json();
        for (std::size_t i = 0; i < object.size(); ++i) {
            const std::string name = text::lowercase(object.keys()[i]);
            if (!object.items()[i].is_number()) {
                throw Error("strategy.params." + name + " must be a number", "strategy.params." + name);
            }
            spec.params[name] = object.items()[i].as_number();
        }
    }
    const bool has_rules =
        reader.has("long_entry") || reader.has("long_exit") || reader.has("short_entry") || reader.has("short_exit");
    if (has_rules && spec.id != "custom") {
        throw Error("only the custom strategy takes rules; set strategy.id to custom", "strategy.id");
    }
    if (spec.id == "custom") {
        if (has_rules) {
            spec.rules = {reader.string("long_entry", ""),
                          reader.string("long_exit", ""),
                          reader.string("short_entry", ""),
                          reader.string("short_exit", "")};
        } else {
            // No rules: the example, with its parameters' defaults under any given values.
            const StrategyInfo* example = find_strategy("custom");
            spec.rules = example->rules;
            for (const ParamSpec& param : example->params) {
                spec.params.try_emplace(param.name, param.default_value);
            }
        }
    }
    return spec;
}

BacktestConfig parse_config(const Json& request) {
    const Reader reader(request.find("config"), "config");
    reader.allow({"initial_capital",
                  "allow_short",
                  "sizing",
                  "position_size",
                  "target_volatility",
                  "volatility_window",
                  "max_leverage",
                  "rebalance_threshold",
                  "commission_bps",
                  "commission_fixed",
                  "slippage_bps",
                  "borrow_rate",
                  "stop_loss",
                  "take_profit",
                  "trailing_stop",
                  "fractional_shares",
                  "close_at_end",
                  "risk_free_rate",
                  "bars_per_year"});
    BacktestConfig config;
    config.initial_capital = reader.number("initial_capital", config.initial_capital);
    config.allow_short = reader.flag("allow_short", config.allow_short);
    const std::string sizing = reader.string("sizing", "percent");
    if (sizing == "percent") {
        config.sizing = Sizing::Percent;
    } else if (sizing == "volatility") {
        config.sizing = Sizing::Volatility;
    } else {
        throw Error("config.sizing must be percent or volatility, not '" + sizing + "'", "config.sizing");
    }
    config.position_size = reader.number("position_size", config.position_size);
    config.target_volatility = reader.number("target_volatility", config.target_volatility);
    config.volatility_window = reader.whole("volatility_window", config.volatility_window, 2, 500);
    config.max_leverage = reader.number("max_leverage", config.max_leverage);
    config.rebalance_threshold = reader.number("rebalance_threshold", config.rebalance_threshold);
    config.commission_bps = reader.number("commission_bps", config.commission_bps);
    config.commission_fixed = reader.number("commission_fixed", config.commission_fixed);
    config.slippage_bps = reader.number("slippage_bps", config.slippage_bps);
    config.borrow_rate = reader.number("borrow_rate", config.borrow_rate);
    config.stop_loss = reader.number("stop_loss", config.stop_loss);
    config.take_profit = reader.number("take_profit", config.take_profit);
    config.trailing_stop = reader.number("trailing_stop", config.trailing_stop);
    config.fractional_shares = reader.flag("fractional_shares", config.fractional_shares);
    config.close_at_end = reader.flag("close_at_end", config.close_at_end);
    config.risk_free_rate = reader.number("risk_free_rate", config.risk_free_rate);
    config.bars_per_year = reader.number("bars_per_year", config.bars_per_year);
    validate(config);
    return config;
}

SweepAxis parse_axis(const Reader& reader) {
    reader.allow({"param", "from", "to", "step"});
    SweepAxis axis;
    axis.param = text::lowercase(reader.string("param", ""));
    for (const char* key : {"from", "to"}) {
        if (!reader.has(key)) {
            throw Error(reader.field(key) + " is required", reader.field(key));
        }
    }
    axis.from = reader.number("from", 0.0);
    axis.to = reader.number("to", 0.0);
    axis.step = reader.number("step", 1.0);
    return axis;
}

SweepSpec parse_sweep_fields(const Reader& reader) {
    SweepSpec spec;
    if (!reader.has("x")) {
        throw Error(reader.field("x") + " is required: the parameter to sweep and its range", reader.field("x"));
    }
    spec.x = parse_axis(reader.child("x"));
    if (reader.has("y")) {
        spec.y = parse_axis(reader.child("y"));
    }
    spec.metric = reader.string("metric", spec.metric);
    spec.threads = reader.whole("threads", 0, 0, 256);
    return spec;
}

struct Output {
    bool series{true};
    bool trades{true};
    bool fills{false};
};

Output parse_output(const Json& request) {
    const Reader reader(request.find("output"), "output");
    reader.allow({"series", "trades", "fills"});
    Output output;
    output.series = reader.flag("series", output.series);
    output.trades = reader.flag("trades", output.trades);
    output.fills = reader.flag("fills", output.fills);
    return output;
}

// ---------------------------------------------------------------------------------------------------------------
// Writing responses

void write_metrics(JsonWriter& w, const Metrics& m) {
    w.begin_object();
    w.field("final_equity", m.final_equity);
    w.field("total_return", m.total_return);
    w.field("cagr", m.cagr);
    w.field("volatility", m.volatility);
    w.field("sharpe", m.sharpe);
    w.field("sortino", m.sortino);
    w.field("max_drawdown", m.max_drawdown);
    w.field("calmar", m.calmar);
    w.field("longest_drawdown_bars", m.longest_drawdown_bars);
    w.field("longest_drawdown_days", m.longest_drawdown_days);
    w.field("exposure", m.exposure);
    w.field("turnover", m.turnover);
    w.field("years", m.years);
    w.field("trades", m.trades);
    w.field("win_rate", m.win_rate);
    w.field("profit_factor", m.profit_factor);
    w.field("avg_trade", m.avg_trade);
    w.field("avg_win", m.avg_win);
    w.field("avg_loss", m.avg_loss);
    w.field("best_trade", m.best_trade);
    w.field("worst_trade", m.worst_trade);
    w.field("avg_bars_held", m.avg_bars_held);
    w.field("max_losing_streak", m.max_losing_streak);
    w.field("skewness", m.skewness);
    w.field("kurtosis", m.kurtosis);
    w.field("var_95", m.var_95);
    w.field("cvar_95", m.cvar_95);
    w.field("beta", m.beta);
    w.field("alpha", m.alpha);
    w.field("correlation", m.correlation);
    w.field("psr", m.psr);
    w.field("costs", m.costs);
    w.end_object();
}

void write_dataset(JsonWriter& w, const Dataset& dataset) {
    const BarSeries& bars = dataset.series;
    w.begin_object();
    w.field("name", dataset.name);
    w.field("title", dataset.title);
    w.field("source", dataset.source);
    w.field("notes", dataset.notes);
    w.field("bars", bars.size());
    w.field("first", format_timestamp(bars.time.front()));
    w.field("last", format_timestamp(bars.time.back()));
    w.field("intraday", bars.intraday());
    w.field("bars_per_year", bars.bars_per_year());
    const LoadReport& report = dataset.report;
    w.key("report").begin_object();
    w.field("rows", report.rows);
    w.field("skipped", report.skipped);
    w.field("repaired", report.repaired);
    w.field("duplicates", report.duplicates);
    w.field("reversed", report.reversed);
    w.field("sorted", report.sorted);
    w.field("adjusted", report.adjusted);
    w.field("date_order", report.date_order);
    w.key("warnings").begin_array();
    for (const std::string& warning : report.warnings) {
        w.value(warning);
    }
    w.end_array();
    w.end_object();
    w.end_object();
}

void write_window(JsonWriter& w, const Window& window) {
    const BarSeries& bars = window.bars();
    w.key("dataset");
    write_dataset(w, *window.entry.dataset);
    w.key("range").begin_object();
    w.field("from", format_timestamp(bars.time[window.begin]));
    w.field("to", format_timestamp(bars.time[window.end - 1]));
    w.field("begin", window.begin);
    w.field("end", window.end);
    w.field("bars", window.end - window.begin);
    w.end_object();
}

void write_times(JsonWriter& w, const BarSeries& bars, std::size_t begin, std::size_t end) {
    w.begin_array();
    for (std::size_t i = begin; i < end; ++i) {
        w.value(static_cast<std::int64_t>(bars.time[i]));
    }
    w.end_array();
}

void write_strategy(JsonWriter& w, const StrategySpec& spec, const expr::Params& params) {
    const StrategyInfo* info = find_strategy(spec.id);
    const Rules& rules = rules_of(spec);
    w.begin_object();
    w.field("id", spec.id);
    w.field("name", info->name);
    w.key("params").begin_object();
    for (const auto& [name, value] : params) {
        w.field(name, value);
    }
    w.end_object();
    w.key("rules").begin_object();
    w.field("long_entry", rules.long_entry);
    w.field("long_exit", rules.long_exit);
    w.field("short_entry", rules.short_entry);
    w.field("short_exit", rules.short_exit);
    w.end_object();
    w.end_object();
}

void write_trades(JsonWriter& w, const BarSeries& bars, const std::vector<Trade>& trades, std::size_t begin) {
    w.begin_array();
    const std::size_t count = std::min(trades.size(), max_trades);
    for (std::size_t i = trades.size() - count; i < trades.size(); ++i) {
        const Trade& trade = trades[i];
        w.begin_object();
        w.field("direction", trade.direction);
        w.field("entry", format_timestamp(bars.time[trade.entry_bar]));
        w.field("exit", format_timestamp(bars.time[trade.exit_bar]));
        w.field("entry_index", trade.entry_bar - begin);
        w.field("exit_index", trade.exit_bar - begin);
        w.field("entry_price", trade.entry_price);
        w.field("exit_price", trade.exit_price);
        w.field("shares", trade.shares);
        w.field("pnl", trade.pnl);
        w.field("return", trade.return_pct);
        w.field("bars", trade.exit_bar - trade.entry_bar);
        w.field("mae", trade.mae);
        w.field("mfe", trade.mfe);
        w.field("reason", trade.exit_reason);
        w.field("open", trade.open);
        w.end_object();
    }
    w.end_array();
}

void write_fills(JsonWriter& w, const BarSeries& bars, const std::vector<Fill>& fills, std::size_t begin) {
    w.begin_array();
    for (const Fill& fill : fills) {
        w.begin_object();
        w.field("time", format_timestamp(bars.time[fill.bar]));
        w.field("index", fill.bar - begin);
        w.field("shares", fill.shares);
        w.field("price", fill.price);
        w.field("commission", fill.commission);
        w.field("slippage", fill.slippage);
        w.field("reason", fill.reason);
        w.end_object();
    }
    w.end_array();
}

void write_calendar(JsonWriter& w, const BarSeries& bars, const BacktestResult& result) {
    w.begin_array();
    for (const YearReturns& year : calendar_returns(bars, result)) {
        w.begin_object();
        w.field("year", static_cast<std::int64_t>(year.year));
        w.key("months").numbers(year.months);
        w.field("total", year.total);
        w.end_object();
    }
    w.end_array();
}

void write_drawdowns(JsonWriter& w, const BarSeries& bars, const BacktestResult& result) {
    w.begin_array();
    for (const DrawdownPeriod& period : worst_drawdowns(result.equity, result.initial_capital, 5)) {
        const DrawdownSpan span = drawdown_span(period, result.begin, result.end);
        w.begin_object();
        w.field("peak", format_timestamp(bars.time[span.first]));
        w.field("trough", format_timestamp(bars.time[result.begin + period.trough]));
        if (period.recovered) {
            w.field("recovery", format_timestamp(bars.time[span.last]));
        } else {
            w.key("recovery").null();
        }
        w.field("depth", period.depth);
        w.field("bars", span.last - span.first);
        w.end_object();
    }
    w.end_array();
}

void write_equity_series(JsonWriter& w,
                         const BarSeries& bars,
                         const BacktestResult& result,
                         const BacktestResult& benchmark) {
    w.key("series").begin_object();
    w.key("time");
    write_times(w, bars, result.begin, result.begin + result.equity.size());
    w.key("equity").numbers(result.equity, series_digits);
    w.key("benchmark").numbers(benchmark.equity, series_digits);
    w.key("drawdown").numbers(drawdown_series(result.equity, result.initial_capital), series_digits);
    w.key("benchmark_drawdown").numbers(drawdown_series(benchmark.equity, benchmark.initial_capital), series_digits);
    w.key("exposure").numbers(result.exposure, series_digits);
    w.end_object();
}

void write_stats(JsonWriter& w, const BacktestResult& result) {
    w.key("stats").begin_object();
    w.field("commissions", result.commissions);
    w.field("slippage", result.slippage);
    w.field("borrow", result.borrow);
    w.field("traded_value", result.traded_value);
    w.field("fills", result.fills.size());
    w.field("trimmed", result.trimmed);
    w.field("ruined", result.ruined);
    w.field("bars_per_year", result.bars_per_year);
    w.end_object();
}

// ---------------------------------------------------------------------------------------------------------------
// Commands

void command_catalog(JsonWriter& w) {
    w.key("strategies").begin_array();
    for (const StrategyInfo& info : strategy_catalog()) {
        w.begin_object();
        w.field("id", info.id);
        w.field("name", info.name);
        w.field("family", info.family);
        w.field("summary", info.summary);
        w.key("params").begin_array();
        for (const ParamSpec& param : info.params) {
            w.begin_object();
            w.field("name", param.name);
            w.field("label", param.label);
            w.field("default", param.default_value);
            w.field("min", param.min);
            w.field("max", param.max);
            w.field("step", param.step);
            w.field("integer", param.integer);
            w.end_object();
        }
        w.end_array();
        w.key("less_than").begin_array();
        for (const auto& [smaller, larger] : info.less_than) {
            w.begin_array().value(smaller).value(larger).end_array();
        }
        w.end_array();
        w.key("rules").begin_object();
        w.field("long_entry", info.rules.long_entry);
        w.field("long_exit", info.rules.long_exit);
        w.field("short_entry", info.rules.short_entry);
        w.field("short_exit", info.rules.short_exit);
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.key("functions").begin_array();
    for (const expr::FunctionInfo& function : expr::function_reference()) {
        w.begin_object();
        w.field("name", function.name);
        w.field("signature", function.signature);
        w.field("returns", function.returns);
        w.field("description", function.description);
        w.end_object();
    }
    w.end_array();
    w.key("columns").begin_array();
    for (const std::string& column : expr::column_names()) {
        w.value(column);
    }
    w.end_array();
    w.key("metrics").begin_array();
    for (const char* metric :
         {"sharpe", "sortino", "cagr", "total_return", "calmar", "max_drawdown", "profit_factor", "win_rate"}) {
        w.value(metric);
    }
    w.end_array();
}

void command_datasets(JsonWriter& w) {
    w.key("datasets").begin_array();
    Json manifest;
    {
        Registry& reg = registry();
        const std::lock_guard<std::mutex> lock(reg.mutex);
        manifest = reg.manifest;
    }
    for (const std::string& name : dataset_names()) {
        if (const auto dataset = find_dataset(name)) {
            write_dataset(w, *dataset);
            continue;
        }
        // Listed in the manifest but not loaded yet.
        const Json* about = manifest_entry(manifest, name);
        w.begin_object();
        w.field("name", name);
        w.field("title", string_field(about, "name"));
        w.field("source", string_field(&manifest, "source"));
        w.field("notes", string_field(about, "notes"));
        const Json* bars = about != nullptr ? about->find("bars") : nullptr;
        w.field("bars", bars != nullptr && bars->is_number() ? bars->as_number() : 0.0);
        w.field("first", string_field(about, "first"));
        w.field("last", string_field(about, "last"));
        w.end_object();
    }
    w.end_array();
}

void command_load(JsonWriter& w, const Json& request) {
    const Reader reader(&request, "");
    reader.allow({"command", "name", "csv", "title", "notes", "source"});
    const std::string name = reader.string("name", "");
    if (!valid_name(name)) {
        throw Error("name must be 1 to 64 letters, digits, '-', '_', '.', or ':'", "name");
    }
    if (name.rfind("synthetic:", 0) == 0) {
        throw Error("names that start with synthetic: are kept for random markets", "name");
    }
    if (!reader.has("csv")) {
        throw Error("csv is required: the file's text", "csv");
    }
    LoadedSeries loaded = load_csv(reader.string("csv", ""), name);
    Dataset dataset;
    dataset.name = name;
    dataset.title = reader.string("title", name);
    dataset.source = reader.string("source", "uploaded");
    dataset.notes = reader.string("notes", "");
    dataset.series = std::move(loaded.series);
    dataset.report = std::move(loaded.report);
    register_dataset(std::move(dataset));
    w.key("dataset");
    write_dataset(w, *find_dataset(name));
}

void command_bars(JsonWriter& w, const Json& request) {
    const Window window = resolve_window(request);
    const BarSeries& bars = window.bars();
    write_window(w, window);
    const auto slice = [&](const std::vector<double>& column) {
        return std::span<const double>(column).subspan(window.begin, window.end - window.begin);
    };
    w.key("time");
    write_times(w, bars, window.begin, window.end);
    w.key("open").numbers(slice(bars.open), series_digits);
    w.key("high").numbers(slice(bars.high), series_digits);
    w.key("low").numbers(slice(bars.low), series_digits);
    w.key("close").numbers(slice(bars.close), series_digits);
    w.key("volume").numbers(slice(bars.volume), series_digits);
}

void command_backtest(JsonWriter& w, const Json& request) {
    const Window window = resolve_window(request);
    const StrategySpec strategy = parse_strategy(request);
    const BacktestConfig config = parse_config(request);
    const Output output = parse_output(request);
    const BarSeries& bars = window.bars();
    const BacktestReport report = run_report(bars, *window.entry.cache, strategy, config, window.begin, window.end);

    write_window(w, window);
    w.key("strategy");
    write_strategy(w, strategy, report.signals.params);
    w.key("metrics");
    write_metrics(w, report.metrics);
    w.key("benchmark");
    write_metrics(w, report.benchmark_metrics);
    write_stats(w, report.result);
    if (output.series) {
        write_equity_series(w, bars, report.result, report.benchmark);
        w.key("overlays").begin_array();
        for (const Overlay& overlay : report.signals.overlays) {
            w.begin_object();
            w.field("key", overlay.key);
            w.field("label", overlay.label);
            w.field("pane", overlay.price_scale ? "price" : "lower");
            w.field("bounded", overlay.bounded);
            w.key("values").numbers(
                std::span<const double>(overlay.values).subspan(window.begin, window.end - window.begin),
                series_digits);
            w.end_object();
        }
        w.end_array();
        w.key("levels").numbers(report.signals.levels);
    }
    if (output.trades) {
        w.key("trades");
        write_trades(w, bars, report.result.trades, window.begin);
    }
    if (output.fills) {
        w.key("fills");
        write_fills(w, bars, report.result.fills, window.begin);
    }
    w.key("calendar");
    write_calendar(w, bars, report.result);
    w.key("drawdowns");
    write_drawdowns(w, bars, report.result);
}

void write_axis(JsonWriter& w, const std::string& param, const std::vector<double>& values) {
    w.begin_object();
    w.field("param", param);
    w.key("values").numbers(values);
    w.end_object();
}

void write_overfit(JsonWriter& w, const OverfitReport& overfit) {
    w.begin_object();
    w.field("trials", overfit.trials);
    w.field("best_sharpe", overfit.best_sharpe);
    w.field("expected_max_sharpe", overfit.expected_max_sharpe);
    w.field("deflated_sharpe", overfit.deflated_sharpe);
    w.field("blocks", overfit.blocks);
    w.field("splits", overfit.splits);
    w.field("pbo", overfit.pbo);
    w.field("oos_loss_probability", overfit.oos_loss_probability);
    w.key("logits").numbers(overfit.logits, series_digits);
    w.key("degradation").begin_array();
    for (const auto& point : overfit.degradation) {
        w.begin_array().value(point[0], series_digits).value(point[1], series_digits).end_array();
    }
    w.end_array();
    w.end_object();
}

void command_sweep(JsonWriter& w, const Json& request, const Progress& progress) {
    const Window window = resolve_window(request);
    const StrategySpec strategy = parse_strategy(request);
    const BacktestConfig config = parse_config(request);
    const Reader reader(request.find("sweep"), "sweep");
    reader.allow({"x", "y", "metric", "threads"});
    const SweepSpec spec = parse_sweep_fields(reader);
    const SweepResult result =
        run_sweep(window.bars(), *window.entry.cache, strategy, config, window.begin, window.end, spec, progress);

    write_window(w, window);
    w.field("strategy", strategy.id);
    w.field("metric", spec.metric);
    w.key("x");
    write_axis(w, spec.x.param, result.xs);
    if (spec.y) {
        w.key("y");
        write_axis(w, spec.y->param, result.ys);
    } else {
        w.key("y").null();
    }
    std::size_t valid = 0;
    w.key("cells").begin_object();
    const auto column = [&](const char* name, const auto& get) {
        w.key(name).begin_array();
        for (const Trial& trial : result.trials) {
            if (trial.valid) {
                w.value(get(trial));
            } else {
                w.null();
            }
        }
        w.end_array();
    };
    column("score", [](const Trial& t) { return t.score; });
    column("sharpe", [](const Trial& t) { return t.metrics.sharpe; });
    column("cagr", [](const Trial& t) { return t.metrics.cagr; });
    column("total_return", [](const Trial& t) { return t.metrics.total_return; });
    column("max_drawdown", [](const Trial& t) { return t.metrics.max_drawdown; });
    column("trades", [](const Trial& t) { return static_cast<double>(t.metrics.trades); });
    w.end_object();
    // One message per kind of failure, such as the fast window not being shorter than the slow one.
    std::vector<std::string> errors;
    for (const Trial& trial : result.trials) {
        valid += trial.valid ? 1 : 0;
        if (!trial.valid && std::find(errors.begin(), errors.end(), trial.error) == errors.end() && errors.size() < 5) {
            errors.push_back(trial.error);
        }
    }
    w.field("trials", result.trials.size());
    w.field("valid", valid);
    w.key("errors").begin_array();
    for (const std::string& error : errors) {
        w.value(error);
    }
    w.end_array();
    if (result.best) {
        const Trial& best = result.trials[*result.best];
        w.key("best").begin_object();
        w.field("index", *result.best);
        w.field("x", best.x);
        w.key("y").value(best.y);
        w.field("score", best.score);
        w.key("metrics");
        write_metrics(w, best.metrics);
        w.end_object();
    } else {
        w.key("best").null();
    }
    // The ten best, by score.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < result.trials.size(); ++i) {
        if (result.trials[i].valid && !std::isnan(result.trials[i].score)) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return result.trials[a].score > result.trials[b].score;
    });
    order.resize(std::min<std::size_t>(order.size(), 10));
    w.key("top").begin_array();
    for (const std::size_t i : order) {
        const Trial& trial = result.trials[i];
        w.begin_object();
        w.field("index", i);
        w.field("x", trial.x);
        w.key("y").value(trial.y);
        w.field("score", trial.score);
        w.field("sharpe", trial.metrics.sharpe);
        w.field("cagr", trial.metrics.cagr);
        w.field("max_drawdown", trial.metrics.max_drawdown);
        w.field("trades", trial.metrics.trades);
        w.end_object();
    }
    w.end_array();
    w.key("overfitting");
    write_overfit(w, result.overfit);
}

void command_walk_forward(JsonWriter& w, const Json& request, const Progress& progress) {
    const Window window = resolve_window(request);
    const StrategySpec strategy = parse_strategy(request);
    const BacktestConfig config = parse_config(request);
    const Output output = parse_output(request);
    const Reader reader(request.find("walkforward"), "walkforward");
    reader.allow({"x", "y", "metric", "threads", "in_sample_years", "out_of_sample_years", "anchored"});
    WalkForwardSpec spec;
    spec.sweep = parse_sweep_fields(reader);
    spec.in_sample_years = reader.number("in_sample_years", spec.in_sample_years);
    spec.out_of_sample_years = reader.number("out_of_sample_years", spec.out_of_sample_years);
    spec.anchored = reader.flag("anchored", spec.anchored);
    const BarSeries& bars = window.bars();
    const WalkForwardResult result =
        run_walk_forward(bars, *window.entry.cache, strategy, config, window.begin, window.end, spec, progress);

    write_window(w, window);
    w.field("strategy", strategy.id);
    w.field("metric", spec.sweep.metric);
    w.field("x_param", spec.sweep.x.param);
    w.field("y_param", spec.sweep.y ? spec.sweep.y->param : std::string());
    w.field("anchored", spec.anchored);
    w.field("efficiency", result.efficiency);
    w.key("metrics");
    write_metrics(w, result.stitched_metrics);
    w.key("benchmark");
    write_metrics(w, result.benchmark_metrics);
    w.key("folds").begin_array();
    for (const WalkForwardFold& fold : result.folds) {
        w.begin_object();
        w.field("is_from", format_timestamp(bars.time[fold.is_begin]));
        w.field("is_to", format_timestamp(bars.time[fold.is_end - 1]));
        w.field("oos_from", format_timestamp(bars.time[fold.oos_begin]));
        w.field("oos_to", format_timestamp(bars.time[fold.oos_end - 1]));
        w.field("oos_index", fold.oos_begin - result.stitched.begin);
        w.field("x", fold.x);
        w.key("y").value(fold.y);
        for (const auto& [key, metrics] : {std::pair<const char*, const Metrics*>{"in_sample", &fold.in_sample},
                                           {"out_of_sample", &fold.out_of_sample}}) {
            w.key(key).begin_object();
            w.field("sharpe", metrics->sharpe);
            w.field("cagr", metrics->cagr);
            w.field("total_return", metrics->total_return);
            w.field("max_drawdown", metrics->max_drawdown);
            w.field("trades", metrics->trades);
            w.end_object();
        }
        w.end_object();
    }
    w.end_array();
    if (output.series) {
        write_equity_series(w, bars, result.stitched, result.benchmark);
    }
    if (output.trades) {
        w.key("trades");
        write_trades(w, bars, result.stitched.trades, result.stitched.begin);
    }
}

void command_monte_carlo(JsonWriter& w, const Json& request, const Progress& progress) {
    const Window window = resolve_window(request);
    const StrategySpec strategy = parse_strategy(request);
    const BacktestConfig config = parse_config(request);
    const Reader reader(request.find("montecarlo"), "montecarlo");
    reader.allow({"paths", "block", "seed"});
    MonteCarloSpec spec;
    spec.paths = reader.whole("paths", spec.paths, 10, 10'000);
    spec.block = reader.whole("block", spec.block, 1, 1000);
    spec.seed = reader.seed("seed", spec.seed);
    const BarSeries& bars = window.bars();
    const BacktestReport report = run_report(bars, *window.entry.cache, strategy, config, window.begin, window.end);
    const MonteCarloResult result = run_monte_carlo(report.result, report.metrics.years, spec, progress);

    write_window(w, window);
    w.field("strategy", strategy.id);
    w.field("paths", result.paths);
    w.field("block", result.block);
    w.field("seed", static_cast<std::int64_t>(spec.seed));
    w.field("loss_probability", result.loss_probability);
    w.key("metrics");
    write_metrics(w, report.metrics);
    w.key("time").begin_array();
    for (const std::size_t offset : result.checkpoints) {
        w.value(static_cast<std::int64_t>(bars.time[window.begin + offset]));
    }
    w.end_array();
    w.key("actual").begin_array();
    for (const std::size_t offset : result.checkpoints) {
        w.value(report.result.equity[offset], series_digits);
    }
    w.end_array();
    w.key("bands").begin_object();
    const char* names[] = {"p5", "p25", "p50", "p75", "p95"};
    for (std::size_t q = 0; q < 5; ++q) {
        w.key(names[q]).numbers(result.bands[q], series_digits);
    }
    w.end_object();
    w.key("distributions").begin_object();
    for (const auto& [name, values] :
         {std::pair<const char*, const std::vector<double>*>{"total_return", &result.total_returns},
          {"cagr", &result.cagrs},
          {"max_drawdown", &result.max_drawdowns},
          {"sharpe", &result.sharpes}}) {
        w.key(name).numbers(*values, series_digits);
    }
    w.end_object();
}

const char* weighting_name(Weighting weighting) {
    switch (weighting) {
        case Weighting::Fixed:
            return "fixed";
        case Weighting::InverseVolatility:
            return "inverse_volatility";
        default:
            return "equal";
    }
}

const char* rebalance_name(Rebalance rebalance) {
    switch (rebalance) {
        case Rebalance::Never:
            return "never";
        case Rebalance::Quarterly:
            return "quarterly";
        case Rebalance::Yearly:
            return "yearly";
        default:
            return "monthly";
    }
}

PortfolioSpec parse_portfolio(const Reader& reader, std::size_t markets) {
    PortfolioSpec spec;
    const bool weighted = reader.has("weights");
    const std::string weighting = reader.string("weighting", weighted ? "fixed" : "equal");
    const std::vector<std::string> weightings = {"equal", "fixed", "inverse_volatility"};
    if (weighting == "equal") {
        spec.weighting = Weighting::Equal;
    } else if (weighting == "fixed") {
        spec.weighting = Weighting::Fixed;
    } else if (weighting == "inverse_volatility") {
        spec.weighting = Weighting::InverseVolatility;
    } else {
        const std::string hint = text::closest(weighting, weightings);
        throw Error("portfolio.weighting must be equal, fixed, or inverse_volatility, not '" + weighting + "'" +
                        (hint.empty() ? "" : "; did you mean " + hint + "?"),
                    "portfolio.weighting");
    }
    if (weighted) {
        if (spec.weighting != Weighting::Fixed) {
            throw Error("weights go with fixed weighting, not " + weighting, "portfolio.weights");
        }
        const Json* list = reader.get("weights");
        if (!list->is_array()) {
            throw Error("portfolio.weights must be a list of numbers, one for each market", "portfolio.weights");
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            if (!list->items()[i].is_number()) {
                throw Error("portfolio.weights[" + std::to_string(i) + "] must be a number",
                            "portfolio.weights[" + std::to_string(i) + "]");
            }
            spec.weights.push_back(list->items()[i].as_number());
        }
    } else if (spec.weighting == Weighting::Fixed) {
        throw Error(
            "fixed weighting needs portfolio.weights, one for each of the " + std::to_string(markets) + " markets",
            "portfolio.weights");
    }
    const std::string rebalance = reader.string("rebalance", "monthly");
    const std::vector<std::string> schedules = {"never", "monthly", "quarterly", "yearly"};
    if (rebalance == "never") {
        spec.rebalance = Rebalance::Never;
    } else if (rebalance == "monthly") {
        spec.rebalance = Rebalance::Monthly;
    } else if (rebalance == "quarterly") {
        spec.rebalance = Rebalance::Quarterly;
    } else if (rebalance == "yearly") {
        spec.rebalance = Rebalance::Yearly;
    } else {
        const std::string hint = text::closest(rebalance, schedules);
        throw Error("portfolio.rebalance must be never, monthly, quarterly, or yearly, not '" + rebalance + "'" +
                        (hint.empty() ? "" : "; did you mean " + hint + "?"),
                    "portfolio.rebalance");
    }
    spec.volatility_window = reader.whole("volatility_window", spec.volatility_window, 2, 1000);
    return spec;
}

void command_portfolio(JsonWriter& w, const Json& request) {
    if (request.find("data") != nullptr) {
        throw Error("a portfolio names its markets in portfolio.markets, not data", "data");
    }
    const Reader reader(request.find("portfolio"), "portfolio");
    reader.allow({"markets", "weights", "weighting", "rebalance", "volatility_window"});
    const Json* list = reader.get("markets");
    if (list == nullptr || !list->is_array() || list->size() == 0) {
        throw Error("portfolio.markets must list the markets, such as [\"AAPL\", \"GE\", \"F\"]", "portfolio.markets");
    }
    if (list->size() > max_portfolio_markets) {
        throw Error("a portfolio holds at most " + std::to_string(max_portfolio_markets) + " markets",
                    "portfolio.markets");
    }
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const std::string field = "portfolio.markets[" + std::to_string(i) + "]";
        Entry entry = resolve_data_at(&list->items()[i], field);
        for (const Entry& earlier : entries) {
            if (earlier.dataset->name == entry.dataset->name) {
                throw Error(field + " repeats " + entry.dataset->name, field);
            }
        }
        entries.push_back(std::move(entry));
    }
    const PortfolioSpec spec = parse_portfolio(reader, entries.size());
    const Dates dates = parse_dates(request);
    const StrategySpec strategy = parse_strategy(request);
    const BacktestConfig config = parse_config(request);
    const Output output = parse_output(request);
    std::vector<PortfolioMarket> markets;
    for (const Entry& entry : entries) {
        markets.push_back({&entry.dataset->series, entry.cache.get()});
    }
    const PortfolioResult result = run_portfolio(markets, strategy, config, spec, dates.from, dates.to);
    const BarSeries& calendar = result.calendar;
    const BacktestResult& portfolio = result.result;

    w.key("range").begin_object();
    w.field("from", format_timestamp(calendar.time[portfolio.begin]));
    w.field("to", format_timestamp(calendar.time[portfolio.end - 1]));
    w.field("bars", portfolio.end - portfolio.begin);
    w.end_object();
    w.key("strategy");
    write_strategy(w, strategy, result.sleeves.front().report.signals.params);
    w.key("portfolio").begin_object();
    w.field("weighting", weighting_name(spec.weighting));
    w.field("rebalance", rebalance_name(spec.rebalance));
    w.field("volatility_window", spec.volatility_window);
    w.field("rebalances", result.rebalances);
    w.field("rebalance_traded", result.rebalance_traded);
    w.field("rebalance_cost", result.rebalance_cost);
    w.end_object();
    w.key("metrics");
    write_metrics(w, result.metrics);
    w.key("benchmark");
    write_metrics(w, result.benchmark_metrics);
    w.key("stats").begin_object();
    w.field("commissions", portfolio.commissions);
    w.field("slippage", portfolio.slippage);
    w.field("borrow", portfolio.borrow);
    w.field("traded_value", portfolio.traded_value);
    w.field("fills", result.fills);
    w.field("trimmed", portfolio.trimmed);
    w.field("ruined", portfolio.ruined);
    w.field("bars_per_year", portfolio.bars_per_year);
    w.end_object();
    w.key("markets").begin_array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Dataset& dataset = *entries[i].dataset;
        const PortfolioSleeve& sleeve = result.sleeves[i];
        w.begin_object();
        w.field("name", dataset.name);
        w.field("title", dataset.title);
        w.field("source", dataset.source);
        w.field("target", sleeve.target);
        w.field("final_weight", sleeve.final_weight);
        w.field("contribution", sleeve.contribution);
        w.key("metrics");
        write_metrics(w, sleeve.report.metrics);
        w.key("benchmark");
        write_metrics(w, sleeve.report.benchmark_metrics);
        w.end_object();
    }
    w.end_array();
    w.key("correlation").begin_array();
    for (const std::vector<double>& row : result.correlation) {
        w.numbers(row, series_digits);
    }
    w.end_array();
    if (output.series) {
        w.key("series").begin_object();
        w.key("time");
        write_times(w, calendar, portfolio.begin, portfolio.end);
        w.key("equity").numbers(portfolio.equity, series_digits);
        w.key("benchmark").numbers(result.benchmark.equity, series_digits);
        w.key("drawdown").numbers(drawdown_series(portfolio.equity, portfolio.initial_capital), series_digits);
        w.key("benchmark_drawdown")
            .numbers(drawdown_series(result.benchmark.equity, result.benchmark.initial_capital), series_digits);
        w.key("exposure").numbers(portfolio.exposure, series_digits);
        w.key("weights").begin_array();
        for (const PortfolioSleeve& sleeve : result.sleeves) {
            w.numbers(sleeve.weight, series_digits);
        }
        w.end_array();
        w.end_object();
    }
    w.key("calendar");
    write_calendar(w, calendar, portfolio);
    w.key("drawdowns");
    write_drawdowns(w, calendar, portfolio);
}

}  // namespace

void register_dataset(Dataset dataset) {
    if (dataset.series.size() < 2) {
        throw Error("a dataset needs at least two bars");
    }
    auto shared = std::make_shared<const Dataset>(std::move(dataset));
    auto cache = std::make_shared<indicators::Cache>(shared->series);
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    if (reg.entries.find(shared->name) == reg.entries.end() && reg.entries.size() >= max_datasets) {
        // Make room by dropping the oldest dataset; requests still using it keep their own reference.
        auto oldest = reg.entries.begin();
        for (auto it = reg.entries.begin(); it != reg.entries.end(); ++it) {
            if (it->second.added < oldest->second.added) {
                oldest = it;
            }
        }
        reg.entries.erase(oldest);
    }
    const std::string name = shared->name;
    reg.entries[name] = Entry{std::move(shared), std::move(cache), ++reg.counter};
}

std::shared_ptr<const Dataset> find_dataset(const std::string& name) {
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    const auto found = reg.entries.find(name);
    return found == reg.entries.end() ? nullptr : found->second.dataset;
}

void set_data_directory(const std::string& path) {
    Json manifest;
    std::ifstream input(path + "/manifest.json", std::ios::binary);
    if (input) {
        std::ostringstream text;
        text << input.rdbuf();
        manifest = Json::parse(text.str());
    }
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    reg.directory = path;
    reg.manifest = std::move(manifest);
}

std::vector<std::string> dataset_names() {
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    std::vector<std::string> names;
    // The manifest's datasets first, in its order, then the others by name.
    if (const Json* list = reg.manifest.find("datasets"); list != nullptr && list->is_array()) {
        for (const Json& item : list->items()) {
            const std::string symbol = string_field(&item, "symbol");
            if (!symbol.empty()) {
                names.push_back(symbol);
            }
        }
    }
    for (const auto& [name, entry] : reg.entries) {
        if (std::find(names.begin(), names.end(), name) == names.end() && entry.dataset->source != "synthetic") {
            names.push_back(name);
        }
    }
    return names;
}

std::string handle(std::string_view request_text, const Progress& progress) {
    JsonWriter w;
    try {
        const Json request = Json::parse(request_text);
        if (!request.is_object()) {
            throw Error("the request must be a JSON object");
        }
        const Json* command = request.find("command");
        if (command == nullptr || !command->is_string()) {
            throw Error(
                "command is required: version, catalog, datasets, load, bars, backtest, sweep, walkforward, "
                "montecarlo, or portfolio",
                "command");
        }
        const std::string& name = command->as_string();
        const std::vector<std::string> commands = {"version",
                                                   "catalog",
                                                   "datasets",
                                                   "load",
                                                   "bars",
                                                   "backtest",
                                                   "sweep",
                                                   "walkforward",
                                                   "montecarlo",
                                                   "portfolio"};
        if (std::find(commands.begin(), commands.end(), name) == commands.end()) {
            const std::string hint = text::closest(name, commands);
            throw Error("unknown command '" + name + "'" + (hint.empty() ? "" : "; did you mean " + hint + "?"),
                        "command");
        }
        if (name != "load") {
            const Reader reader(&request, "");
            reader.allow({"command",
                          "data",
                          "range",
                          "strategy",
                          "config",
                          "sweep",
                          "walkforward",
                          "montecarlo",
                          "portfolio",
                          "output"});
        }
        w.begin_object();
        w.field("ok", true);
        w.field("command", name);
        w.key("result").begin_object();
        if (name == "version") {
            w.field("name", "NanoTrade");
            w.field("version", version);
        } else if (name == "catalog") {
            command_catalog(w);
        } else if (name == "datasets") {
            command_datasets(w);
        } else if (name == "load") {
            command_load(w, request);
        } else if (name == "bars") {
            command_bars(w, request);
        } else if (name == "backtest") {
            command_backtest(w, request);
        } else if (name == "sweep") {
            command_sweep(w, request, progress);
        } else if (name == "walkforward") {
            command_walk_forward(w, request, progress);
        } else if (name == "montecarlo") {
            command_monte_carlo(w, request, progress);
        } else {
            command_portfolio(w, request);
        }
        w.end_object();
        w.end_object();
        return w.take();
    } catch (const Error& error) {
        JsonWriter out;
        out.begin_object();
        out.field("ok", false);
        out.key("error").begin_object();
        out.field("message", error.what());
        if (!error.field().empty()) {
            out.field("field", error.field());
        }
        if (error.position() != Error::npos) {
            out.field("position", error.position());
            out.field("length", error.length());
        }
        out.end_object();
        out.end_object();
        return out.take();
    } catch (const std::exception& error) {
        JsonWriter out;
        out.begin_object();
        out.field("ok", false);
        out.key("error").begin_object();
        out.field("message", std::string("internal error: ") + error.what());
        out.end_object();
        out.end_object();
        return out.take();
    }
}

std::string with_meta(std::string response, std::string_view runtime, double milliseconds) {
    if (response.empty() || response.back() != '}') {
        return response;
    }
    std::size_t threads = 1;
#if !defined(__EMSCRIPTEN__)
    threads = std::max(1U, std::thread::hardware_concurrency());
#endif
    std::string meta = ",\"meta\":{\"runtime\":";
    append_json_string(meta, runtime);
    meta += ",\"ms\":";
    text::append_number(meta, std::round(milliseconds * 1000.0) / 1000.0);
    meta += ",\"threads\":" + std::to_string(threads) + "}";
    response.insert(response.size() - 1, meta);
    return response;
}

}  // namespace nanotrade::api
