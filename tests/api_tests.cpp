// The JSON API end to end: every command through nanotrade::api::handle() and back through nanotrade::Json, the
// shapes of the responses, the errors and the fields they blame, loaded, inline, and synthetic data, the research
// commands, with_meta(), and handle() from many threads at once.

#include "check.hpp"

#include "nanotrade/api.hpp"
#include "nanotrade/json.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/synthetic.hpp"
#include "nanotrade/text.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace api = nanotrade::api;
using nanotrade::Json;
using nanotrade::Timestamp;

const std::vector<std::string> metric_names = {"final_equity",
                                               "total_return",
                                               "cagr",
                                               "volatility",
                                               "sharpe",
                                               "sortino",
                                               "max_drawdown",
                                               "calmar",
                                               "longest_drawdown_bars",
                                               "longest_drawdown_days",
                                               "exposure",
                                               "turnover",
                                               "years",
                                               "trades",
                                               "win_rate",
                                               "profit_factor",
                                               "avg_trade",
                                               "avg_win",
                                               "avg_loss",
                                               "best_trade",
                                               "worst_trade",
                                               "avg_bars_held",
                                               "max_losing_streak",
                                               "skewness",
                                               "kurtosis",
                                               "var_95",
                                               "cvar_95",
                                               "beta",
                                               "alpha",
                                               "correlation",
                                               "psr",
                                               "costs"};

// ---- Reading responses ----

// A response can run to hundreds of kilobytes, so a failure quotes only the start of one.
std::string excerpt(std::string_view text) {
    constexpr std::size_t limit = 200;
    return text.size() <= limit ? std::string(text) : std::string(text.substr(0, limit)) + "...";
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string json_string(std::string_view text) {
    std::string out;
    nanotrade::append_json_string(out, text);
    return out;
}

Json parse(const std::string& text) {
    try {
        return Json::parse(text);
    } catch (const std::exception& error) {
        throw check::Failure(std::string("not JSON (") + error.what() + "): " + excerpt(text));
    }
}

const Json& at(const Json& object, std::string_view key) {
    const Json* value = object.find(key);
    if (value == nullptr) {
        throw check::Failure("no \"" + std::string(key) + "\" in " + excerpt(object.dump()));
    }
    return *value;
}

const Json& typed(const Json& object, std::string_view key, Json::Type type, const char* kind) {
    const Json& value = at(object, key);
    if (value.type() != type) {
        throw check::Failure(std::string(key) + " should be " + kind + ", not " + excerpt(value.dump()));
    }
    return value;
}

const Json& object_at(const Json& object, std::string_view key) {
    return typed(object, key, Json::Type::Object, "an object");
}

const Json& array_at(const Json& object, std::string_view key) {
    return typed(object, key, Json::Type::Array, "an array");
}

// An array that must hold `length` items.
const Json& array_at(const Json& object, std::string_view key, std::size_t length) {
    const Json& value = array_at(object, key);
    if (value.size() != length) {
        throw check::Failure(std::string(key) + " holds " + std::to_string(value.size()) + " items, not " +
                             std::to_string(length));
    }
    return value;
}

double number_at(const Json& object, std::string_view key) {
    return typed(object, key, Json::Type::Number, "a number").as_number();
}

const std::string& text_at(const Json& object, std::string_view key) {
    return typed(object, key, Json::Type::String, "a string").as_string();
}

bool flag_at(const Json& object, std::string_view key) {
    return typed(object, key, Json::Type::Bool, "true or false").as_bool();
}

std::size_t count_at(const Json& object, std::string_view key) {
    const double value = number_at(object, key);
    if (!(value >= 0.0 && value == std::floor(value))) {
        throw check::Failure(std::string(key) + " should be a count, not " + nanotrade::text::format_number(value));
    }
    return static_cast<std::size_t>(value);
}

bool number_or_null(const Json& value) {
    return value.is_number() || value.is_null();
}

// The values of an array that must hold only numbers.
std::vector<double> numbers_in(const Json& array, std::string_view what) {
    std::vector<double> out;
    out.reserve(array.size());
    for (const Json& value : array.items()) {
        if (!value.is_number()) {
            throw check::Failure(std::string(what) + " should hold only numbers, not " + excerpt(value.dump()));
        }
        out.push_back(value.as_number());
    }
    return out;
}

Timestamp timestamp_of(const std::string& text) {
    const std::optional<Timestamp> parsed = nanotrade::parse_timestamp(text);
    if (!parsed) {
        throw check::Failure("'" + text + "' is not a timestamp");
    }
    return *parsed;
}

double seconds_of(const std::string& text) {
    return static_cast<double>(timestamp_of(text));
}

void expect_increasing(const std::vector<double>& values, const std::string& what) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (!(values[i - 1] < values[i])) {
            throw check::Failure(what + " should increase, but doesn't at item " + std::to_string(i));
        }
    }
}

void expect_metrics(const Json& metrics, const std::string& what) {
    for (const std::string& name : metric_names) {
        if (!number_or_null(at(metrics, name))) {
            throw check::Failure(what + "." + name + " should be a number or null, not " +
                                 excerpt(at(metrics, name).dump()));
        }
    }
}

// Two responses that should be equal, which can be long: where they part.
void expect_same(const std::string& actual, const std::string& expected, const std::string& what) {
    if (actual == expected) {
        return;
    }
    const auto parted = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
    const auto offset = static_cast<std::size_t>(parted.first - actual.begin());
    const std::size_t from = offset < 40 ? 0 : offset - 40;
    throw check::Failure(what + ": the responses part at byte " + std::to_string(offset) + ", '" +
                         actual.substr(from, 80) + "' against '" + expected.substr(from, 80) + "'");
}

// ---- Sending requests ----

// The parsed response to a request that must succeed.
Json succeed(const std::string& request) {
    const std::string response = api::handle(request);
    Json json = parse(response);
    const Json* ok = json.find("ok");
    if (ok == nullptr || !ok->is_bool() || !ok->as_bool()) {
        throw check::Failure("the request failed with " + excerpt(response) + ": " + excerpt(request));
    }
    return json;
}

Json result_of(const std::string& request) {
    return object_at(succeed(request), "result");
}

// The error of a request that must fail.
Json error_of(const std::string& request) {
    const std::string response = api::handle(request);
    const Json json = parse(response);
    const Json* ok = json.find("ok");
    if (ok == nullptr || !ok->is_bool() || ok->as_bool()) {
        throw check::Failure("expected an error, got " + excerpt(response) + ": " + excerpt(request));
    }
    if (json.keys() != std::vector<std::string>{"ok", "error"}) {
        throw check::Failure("an error response holds ok and error only: " + excerpt(response));
    }
    return object_at(json, "error");
}

// Checks that a request fails, blaming `field`, with `words` in its message, which it returns.
std::string expect_error(const std::string& request, const std::string& field, std::string_view words) {
    const Json error = error_of(request);
    const std::string message = text_at(error, "message");
    const Json* blamed = error.find("field");
    if (blamed == nullptr || !blamed->is_string() || blamed->as_string() != field) {
        throw check::Failure("the error should blame " + field + ": " + excerpt(error.dump()) + ", for " +
                             excerpt(request));
    }
    if (!contains(message, words)) {
        throw check::Failure("the message should mention '" + std::string(words) + "': " + message);
    }
    return message;
}

// ---- Data for the requests ----

// The i-th weekday from Monday 2024-01-01, at midnight.
Timestamp weekday(std::size_t i) {
    const auto days = static_cast<std::int64_t>(i / 5 * 7 + i % 5);
    return (nanotrade::days_from_civil(2024, 1, 1) + days) * nanotrade::seconds_per_day;
}

// Closes of four significant digits, which a response carries exactly.
double close_on(std::size_t i) {
    const double x = static_cast<double>(i);
    return std::round((50.0 + 8.0 * std::sin(x / 4.0) + 0.05 * x) * 100.0) / 100.0;
}

// Bars on the weekdays from 2024-01-01 as CSV text, oldest first unless `newest_first`.
std::string daily_csv(std::size_t bars, bool newest_first) {
    using nanotrade::text::format_number;
    std::vector<std::string> rows;
    rows.reserve(bars);
    for (std::size_t i = 0; i < bars; ++i) {
        const double closing = close_on(i);
        const double opening = i == 0 ? closing : close_on(i - 1);
        rows.push_back(nanotrade::format_date(weekday(i)) + "," + format_number(opening) + "," +
                       format_number(std::max(opening, closing) + 0.5) + "," +
                       format_number(std::min(opening, closing) - 0.5) + "," + format_number(closing) + "," +
                       std::to_string(1000 + i));
    }
    if (newest_first) {
        std::reverse(rows.begin(), rows.end());
    }
    std::string csv = "Date,Open,High,Low,Close,Volume\n";
    for (const std::string& row : rows) {
        csv += row + "\n";
    }
    return csv;
}

// Three bars a day, at 09:30, 12:00, and 16:00 UTC, from 2024-01-02 to 2024-01-04.
std::string intraday_csv() {
    return "Datetime,Open,High,Low,Close,Volume\n"
           "2024-01-02 09:30,10,11,9,10.5,100\n"
           "2024-01-02 12:00,10.5,11,10,10.8,110\n"
           "2024-01-02 16:00,10.8,11.2,10.6,11,120\n"
           "2024-01-03 09:30,11,11.5,10.9,11.2,130\n"
           "2024-01-03 12:00,11.2,11.4,11,11.1,140\n"
           "2024-01-03 16:00,11.1,11.3,10.9,11,150\n"
           "2024-01-04 09:30,11,11.6,10.8,11.5,160\n"
           "2024-01-04 12:00,11.5,11.9,11.4,11.8,170\n"
           "2024-01-04 16:00,11.8,12,11.7,11.9,180\n";
}

std::string intraday_bars(const std::string& range) {
    return R"({"command": "bars", "data": {"name": "intraday", "csv": )" + json_string(intraday_csv()) +
           R"(}, "range": )" + range + "}";
}

Json read_manifest() {
    std::ifstream input(std::string(NANOTRADE_DATA_DIR) + "/manifest.json", std::ios::binary);
    check::expect(static_cast<bool>(input), "manifest.json can be read");
    std::ostringstream text;
    text << input.rdbuf();
    return parse(text.str());
}

// Registers a dataset again, as it is now, when it goes out of scope. The registry is global, so a test that replaces a
// bundled dataset puts it back this way, even when a check fails, and later tests get the bundled file.
class RestoreDataset {
public:
    explicit RestoreDataset(const std::string& name) : saved_(api::find_dataset(name)) {
        check::expect(saved_ != nullptr, name + " is registered");
    }
    RestoreDataset(const RestoreDataset&) = delete;
    RestoreDataset& operator=(const RestoreDataset&) = delete;
    ~RestoreDataset() {
        try {
            api::register_dataset(*saved_);
        } catch (const std::exception& error) {
            std::cout << "could not restore " << saved_->name << ": " << error.what() << '\n';
        }
    }

private:
    std::shared_ptr<const api::Dataset> saved_;
};

// ---- Commands that need no data ----

void version_names_the_engine() {
    const Json response = succeed(R"({"command": "version"})");
    check::expect(response.keys() == std::vector<std::string>{"ok", "command", "result"}, "ok, command, and result");
    check::expect_equal(text_at(response, "command"), std::string("version"), "the command");
    const Json& result = object_at(response, "result");
    check::expect_equal(text_at(result, "name"), std::string("NanoTrade"), "the name");
    check::expect_equal(text_at(result, "version"), std::string(api::version), "the version");
}

void catalog_describes_every_strategy() {
    const Json catalog = result_of(R"({"command": "catalog"})");
    const std::vector<std::string> families = {"Benchmark", "Trend", "Breakout", "Mean reversion", "Custom"};
    std::vector<std::string> ids;
    for (const Json& strategy : array_at(catalog, "strategies").items()) {
        const std::string id = text_at(strategy, "id");
        check::expect(!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end(), "a new id: '" + id + "'");
        ids.push_back(id);
        check::expect(!text_at(strategy, "name").empty(), id + " has a name");
        const std::string& family = text_at(strategy, "family");
        check::expect(std::find(families.begin(), families.end(), family) != families.end(),
                      id + " is of a known family, not '" + family + "'");
        check::expect(!text_at(strategy, "summary").empty(), id + " has a summary");
        const Json& rules = object_at(strategy, "rules");
        for (const char* rule : {"long_entry", "long_exit", "short_entry", "short_exit"}) {
            static_cast<void>(text_at(rules, rule));
        }
        check::expect(!text_at(rules, "long_entry").empty() || !text_at(rules, "short_entry").empty(),
                      id + " has an entry rule");
        std::map<std::string, double> defaults;
        for (const Json& param : array_at(strategy, "params").items()) {
            const std::string name = text_at(param, "name");
            const std::string where = id + "." + name;
            check::expect(!name.empty() && !defaults.contains(name), where + " is a new parameter");
            check::expect(!text_at(param, "label").empty(), where + " has a label");
            const double low = number_at(param, "min");
            const double usual = number_at(param, "default");
            const double high = number_at(param, "max");
            check::expect(low <= usual && usual <= high, where + ": min <= default <= max");
            check::expect(number_at(param, "step") > 0.0, where + " has a positive step");
            if (flag_at(param, "integer")) {
                check::expect(usual == std::floor(usual), where + " defaults to a whole number");
            }
            defaults[name] = usual;
        }
        for (const Json& pair : array_at(strategy, "less_than").items()) {
            check::expect(
                pair.is_array() && pair.size() == 2 && pair.items()[0].is_string() && pair.items()[1].is_string(),
                id + ": less_than holds pairs of names");
            const auto smaller = defaults.find(pair.items()[0].as_string());
            const auto larger = defaults.find(pair.items()[1].as_string());
            check::expect(smaller != defaults.end() && larger != defaults.end(), id + ": less_than names parameters");
            check::expect(smaller->second < larger->second,
                          id + ": the defaults are in order, " + smaller->first + " < " + larger->first);
        }
    }
    for (const char* id : {"buy_hold", "sma_cross", "custom"}) {
        check::expect(std::find(ids.begin(), ids.end(), id) != ids.end(), std::string("the catalog has ") + id);
    }
}

void catalog_lists_functions_and_columns() {
    const Json catalog = result_of(R"({"command": "catalog"})");
    std::vector<std::string> functions;
    for (const Json& function : array_at(catalog, "functions").items()) {
        const std::string name = text_at(function, "name");
        check::expect(!name.empty() && std::find(functions.begin(), functions.end(), name) == functions.end(),
                      "a new function: '" + name + "'");
        functions.push_back(name);
        const std::string& signature = text_at(function, "signature");
        check::expect(signature.rfind(name + "(", 0) == 0, name + "'s signature starts with it: " + signature);
        check::expect(!text_at(function, "returns").empty(), name + " says what it returns");
        check::expect(!text_at(function, "description").empty(), name + " is described");
    }
    for (const char* name : {"sma", "ema", "rsi", "highest", "lowest", "crossover", "crossunder"}) {
        check::expect(std::find(functions.begin(), functions.end(), name) != functions.end(),
                      std::string("the functions include ") + name);
    }
    std::vector<std::string> columns;
    for (const Json& column : array_at(catalog, "columns").items()) {
        check::expect(
            column.is_string() && std::find(columns.begin(), columns.end(), column.as_string()) == columns.end(),
            "a new column: " + column.dump());
        columns.push_back(column.as_string());
    }
    for (const char* name : {"open", "high", "low", "close", "volume"}) {
        check::expect(std::find(columns.begin(), columns.end(), name) != columns.end(),
                      std::string("the columns include ") + name);
    }
    const std::vector<Json>& metrics = array_at(catalog, "metrics").items();
    check::expect(std::any_of(metrics.begin(),
                              metrics.end(),
                              [](const Json& metric) { return metric.is_string() && metric.as_string() == "sharpe"; }),
                  "sharpe is a metric to sweep for");
}

// The datasets command lists the manifest's from the manifest until a request loads one, and from its file after.
void datasets_follow_the_manifest() {
    const Json manifest = read_manifest();
    const std::vector<std::string> symbols = {"AAPL", "BA", "CAT", "DELL", "EBAY", "F", "GE"};
    const Json& entries = array_at(manifest, "datasets", symbols.size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::string when = pass == 0 ? " as listed" : " once loaded";
        const Json result = result_of(R"({"command": "datasets"})");
        const Json& listed = array_at(result, "datasets");
        const std::vector<std::string> names = api::dataset_names();
        check::expect(listed.size() >= symbols.size() && names.size() >= symbols.size(),
                      "every manifest dataset is listed");
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            const Json& entry = entries.items()[i];
            const Json& dataset = listed.items()[i];
            const std::string& symbol = symbols[i];
            check::expect_equal(text_at(entry, "symbol"), symbol, "the manifest's order");
            check::expect_equal(text_at(dataset, "name"), symbol, "the datasets command keeps the manifest's order");
            check::expect_equal(names[i], symbol, "dataset_names() keeps the manifest's order");
            check::expect_equal(text_at(dataset, "title"), text_at(entry, "name"), symbol + "'s title" + when);
            check::expect_equal(text_at(dataset, "notes"), text_at(entry, "notes"), symbol + "'s notes" + when);
            check::expect_equal(text_at(dataset, "source"), text_at(manifest, "source"), symbol + "'s source" + when);
            check::expect_equal(number_at(dataset, "bars"), number_at(entry, "bars"), symbol + "'s bars" + when);
            check::expect_equal(text_at(dataset, "first"), text_at(entry, "first"), symbol + "'s first bar" + when);
            check::expect_equal(text_at(dataset, "last"), text_at(entry, "last"), symbol + "'s last bar" + when);
        }
        if (pass == 0) {
            for (const std::string& symbol : symbols) {
                static_cast<void>(result_of(R"({"command": "bars", "data": )" + json_string(symbol) +
                                            R"(, "range": {"from": "2013-10-01", "to": "2013-10-10"}})"));
            }
        }
    }
}

// ---- Data by name ----

// The custom strategy's example is tested among the bugs below.
void every_built_in_strategy_runs_with_its_defaults() {
    const Json catalog = result_of(R"({"command": "catalog"})");
    for (const Json& strategy : array_at(catalog, "strategies").items()) {
        const std::string id = text_at(strategy, "id");
        if (id == "custom") {
            continue;
        }
        const Json result = result_of(R"({"command": "backtest", "data": "F", "range": {"from": "2000-01-01", )"
                                      R"("to": "2002-12-31"}, "strategy": {"id": )" +
                                      json_string(id) + R"(}, "output": {"series": false, "trades": false}})");
        const Json& used = object_at(result, "strategy");
        check::expect_equal(text_at(used, "id"), id, "the strategy run");
        const Json& params = object_at(used, "params");
        const Json& specs = array_at(strategy, "params");
        check::expect_equal(params.size(), specs.size(), id + " runs with its parameters");
        for (const Json& param : specs.items()) {
            const std::string& name = text_at(param, "name");
            check::expect_equal(number_at(params, name), number_at(param, "default"), id + " uses the default " + name);
        }
        check::expect_equal(object_at(used, "rules").dump(),
                            object_at(strategy, "rules").dump(),
                            id + " runs its catalog rules");
    }
}

void load_registers_a_dataset() {
    const std::string name = "api-tests-load";
    constexpr std::size_t rows = 40;
    const Json loaded =
        result_of(R"({"command": "load", "name": )" + json_string(name) +
                  R"(, "title": "Loaded by the API tests", "csv": )" + json_string(daily_csv(rows, false)) + "}");
    const Json& dataset = object_at(loaded, "dataset");
    check::expect_equal(text_at(dataset, "name"), name, "the name");
    check::expect_equal(text_at(dataset, "title"), std::string("Loaded by the API tests"), "the title");
    check::expect_equal(text_at(dataset, "source"), std::string("uploaded"), "the source");
    check::expect_equal(count_at(dataset, "bars"), rows, "the bars");
    check::expect_equal(text_at(dataset, "first"), nanotrade::format_date(weekday(0)), "the first bar");
    check::expect_equal(text_at(dataset, "last"), nanotrade::format_date(weekday(rows - 1)), "the last bar");
    check::expect(api::find_dataset(name) != nullptr, "registered");
    const std::vector<std::string> names = api::dataset_names();
    check::expect(std::find(names.begin(), names.end(), name) != names.end(), "named by dataset_names()");
    const Json listing = result_of(R"({"command": "datasets"})");
    const std::vector<Json>& listed = array_at(listing, "datasets").items();
    const auto entry = std::find_if(listed.begin(), listed.end(), [&](const Json& dataset_entry) {
        return text_at(dataset_entry, "name") == name;
    });
    check::expect(entry != listed.end(), "listed by the datasets command");
    check::expect(entry - listed.begin() >= 7, "after the manifest's seven");

    // bars and backtest use it by name.
    const Json bars = result_of(R"({"command": "bars", "data": )" + json_string(name) + "}");
    check::expect_equal(count_at(object_at(bars, "range"), "bars"), rows, "bars of all of it");
    const std::vector<double> times = numbers_in(array_at(bars, "time", rows), "time");
    const std::vector<double> closes = numbers_in(array_at(bars, "close", rows), "close");
    for (std::size_t i = 0; i < rows; ++i) {
        check::expect_equal(times[i], static_cast<double>(weekday(i)), "time " + std::to_string(i));
        check::expect_equal(closes[i], close_on(i), "close " + std::to_string(i));
    }
    const Json part = result_of(R"({"command": "bars", "data": )" + json_string(name) + R"(, "range": {"from": )" +
                                json_string(nanotrade::format_date(weekday(10))) + R"(, "to": )" +
                                json_string(nanotrade::format_date(weekday(19))) + "}}");
    const Json& range = object_at(part, "range");
    check::expect_equal(count_at(range, "begin"), std::size_t{10}, "a range of it begins at its from");
    check::expect_equal(count_at(range, "end"), std::size_t{20}, "and ends after its to");
    check::expect_equal(array_at(part, "close").size(), std::size_t{10}, "ten bars");
    const Json backtest =
        result_of(R"({"command": "backtest", "data": )" + json_string(name) + R"(, "strategy": {"id": "buy_hold"}})");
    check::expect_equal(text_at(object_at(backtest, "dataset"), "name"), name, "backtests it");
    check::expect_equal(count_at(object_at(backtest, "range"), "bars"), rows, "over all of it");
    check::expect_equal(count_at(object_at(backtest, "metrics"), "trades"), std::size_t{1}, "buying and holding it");
    const Json same = result_of(R"({"command": "backtest", "data": {"dataset": )" + json_string(name) +
                                R"(}, "strategy": {"id": "buy_hold"}})");
    check::expect_equal(object_at(same, "metrics").dump(),
                        object_at(backtest, "metrics").dump(),
                        "named by {\"dataset\": name}, the same backtest");

    // Loading the name again replaces the dataset.
    static_cast<void>(result_of(R"({"command": "load", "name": )" + json_string(name) + R"(, "csv": )" +
                                json_string(daily_csv(30, false)) + "}"));
    const Json replaced = result_of(R"({"command": "bars", "data": )" + json_string(name) + "}");
    check::expect_equal(count_at(object_at(replaced, "range"), "bars"), std::size_t{30}, "the new file's bars");
}

void load_refuses_bad_names() {
    const std::string csv = json_string(daily_csv(5, false));
    const std::vector<std::string> names = {"",
                                            ".hidden",
                                            "has space",
                                            "../escape",
                                            "a/b",
                                            "back\\slash",
                                            "semi;colon",
                                            "caf\xC3\xA9",
                                            std::string(65, 'n')};
    for (const std::string& name : names) {
        expect_error(R"({"command": "load", "name": )" + json_string(name) + R"(, "csv": )" + csv + "}",
                     "name",
                     "name must be");
        check::expect(api::find_dataset(name) == nullptr, "'" + name + "' is not registered");
    }
    expect_error(R"({"command": "load", "csv": )" + csv + "}", "name", "name must be");
    expect_error(R"({"command": "load", "name": 7, "csv": )" + csv + "}", "name", "name must be a string");
    expect_error(R"({"command": "load", "name": "api-tests-no-csv"})", "csv", "csv is required");
    check::expect(api::find_dataset("api-tests-no-csv") == nullptr, "nothing is registered without a file");
    const std::string longest(64, 'n');
    static_cast<void>(result_of(R"({"command": "load", "name": )" + json_string(longest) + R"(, "csv": )" + csv + "}"));
    check::expect(api::find_dataset(longest) != nullptr, "a name of 64 characters is fine");
}

// An explicit load is the user's choice: under a bundled dataset's name, it replaces that dataset for every later
// request, and the datasets command lists it in that dataset's place. RestoreDataset puts the bundled GE back after.
void loading_under_a_bundled_name_replaces_that_dataset() {
    const std::string december =
        R"({"command": "bars", "data": "GE", "range": {"from": "2013-12-01", "to": "2013-12-31"}})";
    const std::string bundled = api::handle(december);
    check::expect(flag_at(parse(bundled), "ok"), "GE answers from its file: " + excerpt(bundled));
    {
        const RestoreDataset restore("GE");
        constexpr std::size_t rows = 30;
        const Json loaded = result_of(R"({"command": "load", "name": "GE", "title": "My GE", "csv": )" +
                                      json_string(daily_csv(rows, false)) + "}");
        check::expect_equal(count_at(object_at(loaded, "dataset"), "bars"), rows, "loaded as GE");

        const Json bars = result_of(R"({"command": "bars", "data": "GE"})");
        check::expect_equal(text_at(object_at(bars, "dataset"), "title"), std::string("My GE"), "bars gets the file");
        const std::vector<double> closes = numbers_in(array_at(bars, "close", rows), "close");
        for (std::size_t i = 0; i < rows; ++i) {
            check::expect_equal(closes[i], close_on(i), "close " + std::to_string(i));
        }
        const Json backtest = result_of(R"({"command": "backtest", "data": "GE", "strategy": {"id": "buy_hold"}})");
        check::expect_equal(count_at(object_at(backtest, "range"), "bars"), rows, "and so does backtest");
        expect_error(december, "range", "fewer than two bars");

        const std::vector<std::string> names = api::dataset_names();
        check::expect_equal(std::count(names.begin(), names.end(), "GE"), std::ptrdiff_t{1}, "GE is named once");
        const Json listing = result_of(R"({"command": "datasets"})");
        const std::vector<Json>& listed = array_at(listing, "datasets").items();
        const auto ge = std::find_if(listed.begin(), listed.end(), [](const Json& entry) {
            return text_at(entry, "name") == "GE";
        });
        check::expect(ge != listed.end() && ge - listed.begin() == 6, "listed in GE's place in the manifest");
        check::expect_equal(text_at(*ge, "title"), std::string("My GE"), "with the loaded title");
        check::expect_equal(text_at(*ge, "source"), std::string("uploaded"), "and source");
        check::expect_equal(count_at(*ge, "bars"), rows, "and bars");
    }
    expect_same(api::handle(december), bundled, "the bundled GE once the test puts it back");
}

void bars_cover_their_range() {
    const Json result =
        result_of(R"({"command": "bars", "data": "DELL", "range": {"from": "2001-03-01", "to": "2001-03-31"}})");
    check::expect_equal(text_at(object_at(result, "dataset"), "name"), std::string("DELL"), "the dataset");
    const Json& range = object_at(result, "range");
    check::expect_equal(text_at(range, "from"), std::string("2001-03-01"), "the first bar in the range");
    check::expect_equal(text_at(range, "to"), std::string("2001-03-30"), "the last, a Friday");
    const std::size_t bars = count_at(range, "bars");
    check::expect_equal(bars, std::size_t{22}, "the trading days of March 2001");
    check::expect_equal(count_at(range, "end") - count_at(range, "begin"), bars, "end - begin");
    const std::vector<double> times = numbers_in(array_at(result, "time", bars), "time");
    expect_increasing(times, "time");
    check::expect_equal(times.front(), seconds_of("2001-03-01"), "the first time");
    check::expect_equal(times.back(), seconds_of("2001-03-30"), "a 'to' includes its own day");
    const std::vector<double> opens = numbers_in(array_at(result, "open", bars), "open");
    const std::vector<double> highs = numbers_in(array_at(result, "high", bars), "high");
    const std::vector<double> lows = numbers_in(array_at(result, "low", bars), "low");
    const std::vector<double> closes = numbers_in(array_at(result, "close", bars), "close");
    const std::vector<double> volumes = numbers_in(array_at(result, "volume", bars), "volume");
    for (std::size_t i = 0; i < bars; ++i) {
        check::expect(
            lows[i] <= std::min(opens[i], closes[i]) && std::max(opens[i], closes[i]) <= highs[i] && volumes[i] >= 0.0,
            "bar " + std::to_string(i) + " holds together");
    }
    // A range open at one end runs to that end of the data.
    const Json head = result_of(R"({"command": "bars", "data": "DELL", "range": {"to": "1988-08-19"}})");
    check::expect_equal(count_at(object_at(head, "range"), "begin"), std::size_t{0}, "no from: from the first bar");
    check::expect_equal(count_at(object_at(head, "range"), "bars"), std::size_t{3}, "1988-08-17 to 19");
    const Json tail = result_of(R"({"command": "bars", "data": "DELL", "range": {"from": "2013-10-28"}})");
    check::expect_equal(count_at(object_at(tail, "range"), "end"),
                        count_at(object_at(tail, "dataset"), "bars"),
                        "no to: to the last bar");
    check::expect_equal(count_at(object_at(tail, "range"), "bars"), std::size_t{2}, "2013-10-28 and 29");
}

void a_date_as_to_takes_in_the_whole_day() {
    const Json days = result_of(intraday_bars(R"({"from": "2024-01-02", "to": "2024-01-03"})"));
    check::expect(flag_at(object_at(days, "dataset"), "intraday"), "intraday bars");
    const Json& range = object_at(days, "range");
    check::expect_equal(count_at(range, "bars"), std::size_t{6}, "every bar of both days");
    check::expect_equal(text_at(range, "from"), std::string("2024-01-02 09:30"), "from the first bar of the first");
    check::expect_equal(text_at(range, "to"), std::string("2024-01-03 16:00"), "to the last bar of the last");
    const std::vector<double> times = numbers_in(array_at(days, "time", 6), "time");
    check::expect_equal(times.back(), seconds_of("2024-01-03 16:00"), "the last time");
    const Json one = result_of(intraday_bars(R"({"from": "2024-01-03", "to": "2024-01-03"})"));
    check::expect_equal(count_at(object_at(one, "range"), "bars"), std::size_t{3}, "one day: all three of its bars");
    // A time is an instant, not a day.
    for (const char* to : {"2024-01-03 12:00", "2024-01-03T12:00:00Z", "2024-01-03T13:00:00+01:00", "1704283200000"}) {
        const Json until = result_of(intraday_bars(std::string(R"({"to": ")") + to + R"("})"));
        const Json& until_range = object_at(until, "range");
        check::expect_equal(count_at(until_range, "bars"), std::size_t{5}, std::string("to ") + to + ": until 12:00");
        check::expect_equal(text_at(until_range, "to"), std::string("2024-01-03 12:00"), std::string("to ") + to);
    }
    // Every way of writing a date takes in the whole day.
    for (const char* to : {"2024/01/03", "2024.01.03", "20240103"}) {
        const Json day = result_of(intraday_bars(std::string(R"({"from": "2024-01-03", "to": ")") + to + R"("})"));
        check::expect_equal(count_at(object_at(day, "range"), "bars"), std::size_t{3}, std::string("to ") + to);
    }
}

// What resolve_window asks of a 'to': whether it is a day, which takes in every bar of that date, or an instant.
void is_calendar_date_tells_days_from_instants() {
    struct Case {
        const char* text;
        bool day;
    };
    const Case cases[] = {
        {"2024-01-03", true},
        {"2024/01/03", true},
        {"2024.01.03", true},
        {"20240103", true},
        {" 2024-01-03\t", true},
        {"1704283200", false},
        {"1704283200000", false},
        {"2024-01-03 12:00", false},
        {"2024-01-03T12:00:00Z", false},
        {"", false},
        {"   ", false},
        {"\t", false},
        {"soon", false},
        {"2024-02-30", false},
        {"2024-01-03x", false},
        {"2024-01-03 25:00", false},
        {"2024-01", false},
    };
    for (const Case& example : cases) {
        const std::string what = std::string("is_calendar_date(\"") + example.text + "\")";
        check::expect_equal(nanotrade::is_calendar_date(example.text), example.day, what);
        if (example.day) {
            const std::optional<Timestamp> midnight = nanotrade::parse_timestamp(example.text);
            check::expect(midnight.has_value() && *midnight % nanotrade::seconds_per_day == 0,
                          what + ": a day parse_timestamp reads as its midnight");
        }
    }
}

// ---- Backtests ----

void a_default_backtest_has_every_part() {
    const Json response =
        succeed(R"({"command": "backtest", "data": "AAPL", "range": {"from": "2005-01-01", "to": "2009-12-31"}})");
    check::expect(response.keys() == std::vector<std::string>{"ok", "command", "result"}, "ok, command, and result");
    check::expect_equal(text_at(response, "command"), std::string("backtest"), "the command");
    const Json& result = object_at(response, "result");
    check::expect_equal(text_at(object_at(result, "dataset"), "name"), std::string("AAPL"), "the dataset");
    const Json& range = object_at(result, "range");
    const std::size_t bars = count_at(range, "bars");
    check::expect_equal(text_at(range, "from"), std::string("2005-01-03"), "the first trading day of 2005");
    check::expect_equal(text_at(range, "to"), std::string("2009-12-31"), "the last of 2009");
    check::expect_equal(count_at(range, "end") - count_at(range, "begin"), bars, "end - begin");
    check::expect(bars > 1000, "five years of bars");

    const Json& strategy = object_at(result, "strategy");
    check::expect_equal(text_at(strategy, "id"), std::string("sma_cross"), "the default strategy");
    check::expect_equal(number_at(object_at(strategy, "params"), "fast"), 20.0, "its default fast window");
    check::expect_equal(number_at(object_at(strategy, "params"), "slow"), 50.0, "its default slow window");
    const Json& metrics = object_at(result, "metrics");
    expect_metrics(metrics, "metrics");
    expect_metrics(object_at(result, "benchmark"), "benchmark");
    check::expect(object_at(result, "benchmark").keys() == metrics.keys(), "the benchmark has the same metrics");
    const Json& stats = object_at(result, "stats");
    for (const char* key : {"commissions", "slippage", "borrow", "traded_value", "bars_per_year"}) {
        static_cast<void>(number_at(stats, key));
    }
    static_cast<void>(count_at(stats, "trimmed"));
    check::expect(!flag_at(stats, "ruined"), "not ruined");

    // One value per bar of the range.
    const Json& series = object_at(result, "series");
    const std::vector<double> times = numbers_in(array_at(series, "time", bars), "series.time");
    expect_increasing(times, "series.time");
    check::expect_equal(times.front(), seconds_of(text_at(range, "from")), "the series starts with the range");
    check::expect_equal(times.back(), seconds_of(text_at(range, "to")), "and ends with it");
    for (const char* key : {"benchmark", "drawdown", "benchmark_drawdown", "exposure"}) {
        static_cast<void>(numbers_in(array_at(series, key, bars), key));
    }
    const std::vector<double> equity = numbers_in(array_at(series, "equity", bars), "equity");
    check::expect_near(equity.back(),
                       number_at(metrics, "final_equity"),
                       1e-6 * equity.back(),
                       "the equity ends at the final equity");
    const Json& overlays = array_at(result, "overlays", 2);
    for (const Json& overlay : overlays.items()) {
        const std::string& key = text_at(overlay, "key");
        check::expect(!text_at(overlay, "label").empty(), key + " has a label");
        const std::string& pane = text_at(overlay, "pane");
        check::expect(pane == "price" || pane == "lower", key + " is drawn on a known pane, not " + pane);
        static_cast<void>(flag_at(overlay, "bounded"));
        check::expect_equal(array_at(overlay, "values").size(), bars, key + " has a value per bar");
    }
    static_cast<void>(array_at(result, "levels"));

    const Json& trades = array_at(result, "trades");
    check::expect(trades.size() > 0, "the averages crossed in five years");
    check::expect_equal(trades.size(), count_at(metrics, "trades"), "every trade is listed");
    for (const Json& trade : trades.items()) {
        const std::size_t entered = count_at(trade, "entry_index");
        const std::size_t exited = count_at(trade, "exit_index");
        check::expect(entered <= exited && exited < bars, "a trade within the range");
        check::expect_equal(seconds_of(text_at(trade, "entry")), times[entered], "the entry is at entry_index");
        check::expect_equal(seconds_of(text_at(trade, "exit")), times[exited], "the exit is at exit_index");
        check::expect_equal(count_at(trade, "bars"), exited - entered, "the bars held");
        check::expect_equal(number_at(trade, "direction"), 1.0, "long only by default");
        check::expect(!flag_at(trade, "open"), "closed at the end by default");
    }
    check::expect(count_at(stats, "fills") >= trades.size(), "a fill for every trade at least");
    check::expect(result.find("fills") == nullptr, "no fills unless asked for");

    // A row per calendar year the range touches, in order; these years are whole, so every month has a return.
    const std::int64_t first_year = nanotrade::civil_from_timestamp(timestamp_of(text_at(range, "from"))).year;
    const std::int64_t last_year = nanotrade::civil_from_timestamp(timestamp_of(text_at(range, "to"))).year;
    const Json& calendar = array_at(result, "calendar", static_cast<std::size_t>(last_year - first_year + 1));
    for (std::size_t i = 0; i < calendar.size(); ++i) {
        const Json& year = calendar.items()[i];
        check::expect_equal(number_at(year, "year"),
                            static_cast<double>(first_year + static_cast<std::int64_t>(i)),
                            "the years in order");
        static_cast<void>(numbers_in(array_at(year, "months", 12), "months"));
        static_cast<void>(number_at(year, "total"));
    }
    const Json& drawdowns = array_at(result, "drawdowns");
    check::expect(drawdowns.size() > 0 && drawdowns.size() <= 5, "the worst five drawdowns");
    for (const Json& drawdown : drawdowns.items()) {
        const std::string& peak = text_at(drawdown, "peak");
        const std::string& trough = text_at(drawdown, "trough");
        check::expect(text_at(range, "from") <= peak && peak <= trough && trough <= text_at(range, "to"),
                      "a drawdown within the range, from " + peak + " to " + trough);
        const Json& recovery = at(drawdown, "recovery");
        check::expect(recovery.is_null() || (recovery.is_string() && trough <= recovery.as_string()),
                      "recovered after the trough, or not yet");
        const double depth = number_at(drawdown, "depth");
        check::expect(depth > 0.0 && depth <= 1.0, "a depth that is a fraction");
        static_cast<void>(count_at(drawdown, "bars"));
    }
}

void output_flags_choose_the_parts() {
    const std::string request = R"({"command": "backtest", "data": "AAPL", "range": {"from": "2007-01-01", )"
                                R"("to": "2008-12-31"}, "strategy": {"id": "sma_cross", "params": {"fast": 10, )"
                                R"("slow": 30}}, "output": )";
    const Json plain = result_of(request + "{}}");
    struct Case {
        const char* output;
        bool series;
        bool trades;
        bool fills;
    };
    const Case cases[] = {
        {"{}", true, true, false},
        {R"({"series": false})", false, true, false},
        {R"({"trades": false})", true, false, false},
        {R"({"fills": true})", true, true, true},
        {R"({"series": false, "trades": false, "fills": true})", false, false, true},
        {R"({"series": true, "trades": true, "fills": false})", true, true, false},
    };
    for (const Case& option : cases) {
        const Json result = result_of(request + option.output + "}");
        const std::string what = std::string("output ") + option.output;
        for (const char* key : {"series", "overlays", "levels"}) {
            check::expect((result.find(key) != nullptr) == option.series, what + ": " + key);
        }
        check::expect((result.find("trades") != nullptr) == option.trades, what + ": trades");
        check::expect((result.find("fills") != nullptr) == option.fills, what + ": fills");
        for (const char* key : {"dataset",
                                "range",
                                "strategy",
                                "metrics",
                                "benchmark",
                                "stats",
                                "series",
                                "overlays",
                                "levels",
                                "trades",
                                "calendar",
                                "drawdowns"}) {
            if (result.find(key) != nullptr) {
                check::expect_equal(at(result, key).dump(), at(plain, key).dump(), what + " leaves " + key + " alone");
            }
        }
        if (option.fills) {
            const Json& fills = array_at(result, "fills");
            check::expect_equal(fills.size(), count_at(object_at(result, "stats"), "fills"), what + ": every fill");
            check::expect(fills.size() > 0, what + ": something traded");
            const std::size_t bars = count_at(object_at(result, "range"), "bars");
            for (const Json& fill : fills.items()) {
                check::expect(count_at(fill, "index") < bars, what + ": a fill within the range");
                check::expect(number_at(fill, "shares") != 0.0 && number_at(fill, "price") > 0.0,
                              what + ": shares at a price");
                static_cast<void>(number_at(fill, "commission"));
                static_cast<void>(number_at(fill, "slippage"));
                check::expect(!text_at(fill, "time").empty() && !text_at(fill, "reason").empty(),
                              what + ": a time and a reason");
            }
        }
    }
}

void ranges_need_two_bars() {
    const std::string message =
        expect_error(R"({"command": "backtest", "data": "EBAY", "range": {"from": "1990-01-01", "to": "1998-09-24"}})",
                     "range",
                     "fewer than two bars");
    check::expect(contains(message, "EBAY") && contains(message, "1998-09-24") && contains(message, "2013-12-31"),
                  "the message says when the data runs: " + message);
    expect_error(R"({"command": "bars", "data": "EBAY", "range": {"from": "2014-01-01"}})",
                 "range",
                 "fewer than two bars");
    expect_error(R"({"command": "backtest", "data": "EBAY", "range": {"from": "2005-06-01", "to": "2005-01-01"}})",
                 "range",
                 "fewer than two bars");
    expect_error(R"({"command": "montecarlo", "data": "EBAY", "range": {"to": "1998-09-24"}})",
                 "range",
                 "fewer than two bars");
    expect_error(R"({"command": "sweep", "data": "EBAY", "range": {"to": "1998-09-24"}, )"
                 R"("sweep": {"x": {"param": "fast", "from": 5, "to": 10}}})",
                 "range",
                 "fewer than two bars");
    const Json two =
        result_of(R"({"command": "backtest", "data": "EBAY", "range": {"from": "1998-09-24", "to": "1998-09-25"}})");
    check::expect_equal(count_at(object_at(two, "range"), "bars"), std::size_t{2}, "two bars are enough");
    check::expect_equal(array_at(object_at(two, "series"), "equity").size(), std::size_t{2}, "an equity per bar");
    expect_error(R"({"command": "bars", "data": "EBAY", "range": {"from": "2005-13-01"}})",
                 "range.from",
                 "must be a date");
    expect_error(R"({"command": "bars", "data": "EBAY", "range": {"to": "soon"}})", "range.to", "must be a date");
}

// ---- Errors ----

void unknown_fields_are_refused_with_a_hint() {
    const std::string message =
        expect_error(R"({"command": "backtest", "data": "AAPL", "config": {"comission_bps": 1}})",
                     "config.comission_bps",
                     "commission_bps");
    check::expect(contains(message, "did you mean"), "a hint: " + message);
    expect_error(R"({"command": "backtest", "data": "AAPL", "stratgy": {"id": "sma_cross"}})",
                 "stratgy",
                 "did you mean strategy?");
    expect_error(R"({"command": "bars", "data": "AAPL", "range": {"form": "2005-01-01"}})",
                 "range.form",
                 "did you mean from?");
    expect_error(R"({"command": "backtest", "data": "AAPL", "output": {"serie": false}})",
                 "output.serie",
                 "did you mean series?");
    expect_error(R"({"command": "backtest", "data": {"synthetic": {"sed": 3}}})",
                 "data.synthetic.sed",
                 "did you mean seed?");
    expect_error(R"({"command": "backtest", "data": "AAPL", "strategy": {"id": "sma_cross", "parms": {"fast": 10}}})",
                 "strategy.parms",
                 "did you mean params?");
    expect_error(R"({"command": "load", "name": "api-tests-titel", "titel": "x", "csv": "Date,Close\n2024-01-02,1\n"})",
                 "titel",
                 "did you mean title?");
    const std::string distant = expect_error(R"({"command": "backtest", "data": "AAPL", "config": {"zzzzzzzz": 1}})",
                                             "config.zzzzzzzz",
                                             "unknown field config.zzzzzzzz");
    check::expect(!contains(distant, "did you mean"), "no hint when nothing is close: " + distant);
}

void unknown_names_get_a_hint() {
    expect_error(R"({"command": "backtets", "data": "AAPL"})", "command", "did you mean backtest?");
    expect_error(R"({"command": "walkfoward"})", "command", "did you mean walkforward?");
    const std::string distant = expect_error(R"({"command": "xyzzy"})", "command", "unknown command 'xyzzy'");
    check::expect(!contains(distant, "did you mean"), "no hint when nothing is close: " + distant);
    expect_error(R"({"data": "AAPL"})", "command", "command is required");
    expect_error(R"({"command": "backtest", "data": "AAPL", "strategy": {"id": "sma_crosss"}})",
                 "strategy.id",
                 "did you mean sma_cross?");
    expect_error(R"({"command": "backtest", "data": "AAPL", "strategy": {"id": "rsi_reversio"}})",
                 "strategy.id",
                 "did you mean rsi_reversion?");
    expect_error(R"({"command": "backtest", "data": "AAPLL"})", "data", "did you mean AAPL?");
}

void wrong_types_name_their_field() {
    const std::string start = R"({"command": "backtest", "data": "AAPL", )";
    expect_error(start + R"("config": {"initial_capital": "x"}})", "config.initial_capital", "must be a number");
    expect_error(start + R"("config": {"allow_short": "yes"}})", "config.allow_short", "true or false");
    expect_error(start + R"("config": {"sizing": 1}})", "config.sizing", "must be a string");
    expect_error(start + R"("config": {"volatility_window": 2.5}})", "config.volatility_window", "whole number");
    expect_error(start + R"("config": {"initial_capital": -5}})", "config.initial_capital", "positive");
    expect_error(start + R"("config": [1]})", "config", "must be an object");
    expect_error(start + R"("range": "2005"})", "range", "must be an object");
    expect_error(start + R"("range": {"from": 2005}})", "range.from", "must be a string");
    expect_error(start + R"("strategy": {"params": {"fast": "10"}}})", "strategy.params.fast", "must be a number");
    expect_error(start + R"("strategy": {"id": 5}})", "strategy.id", "must be a string");
    expect_error(start + R"("output": {"series": "no"}})", "output.series", "true or false");
    expect_error(R"({"command": "backtest", "data": 5})", "data", "must be an object");
    expect_error(R"({"command": "montecarlo", "data": "AAPL", "montecarlo": {"paths": "many"}})",
                 "montecarlo.paths",
                 "must be a number");
    check::expect(contains(text_at(error_of(R"([{"command": "version"}])"), "message"), "must be a JSON object"),
                  "a request is an object");
}

void rule_errors_point_into_the_rule() {
    const std::string start = R"({"command": "backtest", "data": "AAPL", "strategy": {"id": "custom", )";
    const std::string rule = "rsi(close, 14) < oversold";
    const Json error = error_of(start + R"("long_entry": )" + json_string(rule) + "}}");
    check::expect_equal(text_at(error, "field"), std::string("strategy.long_entry"), "blames the rule");
    const std::size_t position = count_at(error, "position");
    const std::size_t length = count_at(error, "length");
    check::expect_equal(position, std::size_t{17}, "points at the unknown name");
    check::expect_equal(length, std::size_t{8}, "and covers it");
    check::expect_equal(rule.substr(position, length), std::string("oversold"), "which is oversold");
    check::expect(contains(text_at(error, "message"), "'oversold'"), "and names it");
    // Each rule is blamed by name, the short ones even in a long-only run.
    struct Case {
        const char* field;
        const char* rule;
        std::size_t position;
    };
    const Case cases[] = {
        {"long_exit", "close < sma(close, 20", 21},
        {"short_entry", "close < $missing", 8},
        {"short_exit", "close >", 7},
        {"long_entry", "close + 1", 0},
    };
    for (const Case& bad : cases) {
        const std::string field = std::string("strategy.") + bad.field;
        const std::string entry =
            std::string(bad.field) == "long_entry" ? std::string() : R"("long_entry": "close > open", )";
        const Json blamed = error_of(start + entry + json_string(bad.field) + ": " + json_string(bad.rule) + "}}");
        check::expect_equal(text_at(blamed, "field"), field, std::string("blames ") + bad.field);
        const std::size_t size = std::string_view(bad.rule).size();
        const std::size_t at_position = count_at(blamed, "position");
        check::expect_equal(at_position, bad.position, std::string("where ") + bad.rule + " goes wrong");
        // Within the rule, or just past its end when it stops short.
        check::expect(at_position == size || at_position + count_at(blamed, "length") <= size,
                      std::string("points into ") + bad.rule + ": " + blamed.dump());
    }
}

void parameter_errors_name_the_parameter() {
    const std::string start = R"({"command": "backtest", "data": "AAPL", "strategy": {"id": "sma_cross", "params": )";
    expect_error(start + R"({"fast": 60}}})", "strategy.params.fast", "must be less than");
    expect_error(start + R"({"slow": 10}}})", "strategy.params.fast", "must be less than");
    expect_error(start + R"({"fast": 1}}})", "strategy.params.fast", "must be from 2 to 200");
    expect_error(start + R"({"fast": 250}}})", "strategy.params.fast", "must be from 2 to 200");
    expect_error(start + R"({"fast": 10.5}}})", "strategy.params.fast", "whole number");
    expect_error(start + R"({"fast": null}}})", "strategy.params.fast", "must be a number");
    expect_error(start + R"({"fsat": 10}}})", "strategy.params.fsat", "no parameter");
    const Json fine = result_of(start + R"({"fast": 10, "slow": 30}}, "output": {"series": false}})");
    check::expect_equal(number_at(object_at(object_at(fine, "strategy"), "params"), "fast"), 10.0, "fast 10 runs");
}

void invalid_json_reports_a_position() {
    struct Case {
        std::string text;
        std::size_t position;
    };
    const std::vector<Case> cases = {
        {R"({"command": "backtest", "data": "AAPL",})", 39},
        {R"({"command": "version")", 21},
        {R"({"command": "version"} extra)", 23},
        {"", 0},
        {"not json", 0},
        {std::string(100, '['), 65},
    };
    for (const Case& bad : cases) {
        const Json error = error_of(bad.text);
        check::expect(contains(text_at(error, "message"), "invalid JSON"), "says so: " + text_at(error, "message"));
        check::expect_equal(count_at(error, "position"), bad.position, "the position in " + excerpt(bad.text));
        check::expect_equal(count_at(error, "length"), std::size_t{1}, "one byte");
        check::expect(error.find("field") == nullptr, "no field to blame");
    }
}

// ---- Data in the request ----

void inline_csv_runs_without_being_registered() {
    constexpr std::size_t rows = 60;
    const std::string data = R"({"name": "api-tests-inline", "csv": )" + json_string(daily_csv(rows, false)) + "}";
    const Json result = result_of(R"({"command": "backtest", "data": )" + data +
                                  R"json(, "strategy": {"id": "custom", "long_entry": "close > sma(close, 5)"}, )json"
                                  R"("output": {"fills": true}})");
    const Json& dataset = object_at(result, "dataset");
    check::expect_equal(text_at(dataset, "name"), std::string("api-tests-inline"), "named as asked");
    check::expect_equal(text_at(dataset, "source"), std::string("inline CSV"), "from the request");
    check::expect_equal(count_at(dataset, "bars"), rows, "every row");
    check::expect_equal(count_at(object_at(result, "range"), "bars"), rows, "the whole file by default");
    const Json& series = object_at(result, "series");
    const std::vector<double> times = numbers_in(array_at(series, "time", rows), "series.time");
    for (std::size_t i = 0; i < rows; ++i) {
        check::expect_equal(times[i], static_cast<double>(weekday(i)), "time " + std::to_string(i));
    }
    for (const char* key : {"equity", "benchmark", "drawdown", "benchmark_drawdown", "exposure"}) {
        check::expect_equal(array_at(series, key).size(), rows, std::string("series.") + key);
    }
    const Json& overlay = array_at(result, "overlays", 1).items()[0];
    check::expect_equal(text_at(overlay, "key"), std::string("sma(close,5)"), "the rule's average");
    check::expect_equal(array_at(overlay, "values").size(), rows, "a value per bar");
    const Json& trades = array_at(result, "trades");
    check::expect(trades.size() > 0, "the closes cross their average");
    for (const Json& trade : trades.items()) {
        check::expect(
            count_at(trade, "entry_index") <= count_at(trade, "exit_index") && count_at(trade, "exit_index") < rows,
            "a trade within the bars");
    }
    for (const Json& fill : array_at(result, "fills").items()) {
        check::expect(count_at(fill, "index") < rows, "a fill within the bars");
    }
    // The data belongs to the request: nothing is registered under its name.
    check::expect(api::find_dataset("api-tests-inline") == nullptr, "not registered");
    expect_error(R"({"command": "bars", "data": "api-tests-inline"})", "data", "unknown dataset");
    // A file newest first reads the same, and says so.
    const Json forward = result_of(R"({"command": "bars", "data": )" + data + "}");
    const Json backward =
        result_of(R"({"command": "bars", "data": {"csv": )" + json_string(daily_csv(rows, true)) + "}}");
    check::expect(!flag_at(object_at(object_at(forward, "dataset"), "report"), "reversed"), "oldest first");
    check::expect(flag_at(object_at(object_at(backward, "dataset"), "report"), "reversed"), "newest first, noticed");
    check::expect_equal(text_at(object_at(backward, "dataset"), "name"), std::string("upload"), "unnamed: upload");
    for (const char* key : {"range", "time", "open", "high", "low", "close", "volume"}) {
        check::expect_equal(at(backward, key).dump(), at(forward, key).dump(), std::string("the same ") + key);
    }
    check::expect(
        contains(text_at(error_of(R"({"command": "bars", "data": {"csv": "Price,Volume\n1,2\n3,4\n"}})"), "message"),
                 "date column"),
        "a file without dates is refused");
}

std::string synthetic_backtest(int seed) {
    return R"({"command": "backtest", "data": {"synthetic": {"seed": )" + std::to_string(seed) +
           R"(, "bars": 500}}, "strategy": {"id": "sma_cross", "params": {"fast": 10, "slow": 40}}})";
}

void synthetic_markets_follow_their_seed() {
    const std::string first = api::handle(synthetic_backtest(11));
    const Json parsed = parse(first);
    check::expect(flag_at(parsed, "ok"), "the backtest runs: " + excerpt(first));
    const Json& dataset = object_at(object_at(parsed, "result"), "dataset");
    check::expect_equal(text_at(dataset, "source"), std::string("synthetic"), "a synthetic market");
    check::expect_equal(count_at(dataset, "bars"), std::size_t{500}, "of the bars asked for");
    expect_same(api::handle(synthetic_backtest(11)), first, "the same seed");
    const std::string other = api::handle(synthetic_backtest(12));
    check::expect(flag_at(parse(other), "ok"), "another seed runs too");
    check::expect(other != first, "another seed, another market");

    // The bars are the ones synthesize() makes from the seed.
    nanotrade::SyntheticSpec spec;
    spec.seed = 11;
    spec.bars = 500;
    const nanotrade::BarSeries market = nanotrade::synthesize(spec);
    const Json bars = result_of(R"({"command": "bars", "data": {"synthetic": {"seed": 11, "bars": 500}}})");
    const std::vector<double> times = numbers_in(array_at(bars, "time", market.size()), "time");
    const std::vector<double> closes = numbers_in(array_at(bars, "close", market.size()), "close");
    for (std::size_t i = 0; i < market.size(); ++i) {
        check::expect_equal(times[i], static_cast<double>(market.time[i]), "time " + std::to_string(i));
        check::expect_near(closes[i], market.close[i], 1e-6 * market.close[i], "close " + std::to_string(i));
    }
    for (const std::string& name : api::dataset_names()) {
        const std::shared_ptr<const api::Dataset> listed = api::find_dataset(name);
        check::expect(listed == nullptr || listed->source != "synthetic", "a synthetic market is listed: " + name);
    }
}

// ---- Research ----

// A sweep's cells, best, and top agree with one another and with its axes. Returns the number of trials that ran.
std::size_t expect_consistent_sweep(const Json& result) {
    const std::vector<double> xs = numbers_in(array_at(object_at(result, "x"), "values"), "x.values");
    const Json& y_axis = at(result, "y");
    const std::vector<double> ys =
        y_axis.is_null() ? std::vector<double>{} : numbers_in(array_at(y_axis, "values"), "y.values");
    const std::size_t trials = count_at(result, "trials");
    check::expect_equal(trials, xs.size() * std::max<std::size_t>(ys.size(), 1), "a trial per combination");
    const auto x_of = [&](std::size_t i) {
        return xs[i % xs.size()];
    };
    const auto expect_y = [&](const Json& holder, std::size_t i, const std::string& what) {
        if (ys.empty()) {
            check::expect(at(holder, "y").is_null(), what + ": no y in a sweep of one parameter");
        } else {
            check::expect_equal(number_at(holder, "y"), ys[i / xs.size()], what + ": y");
        }
    };
    const Json& cells = object_at(result, "cells");
    for (const char* column : {"score", "sharpe", "cagr", "total_return", "max_drawdown", "trades"}) {
        check::expect_equal(array_at(cells, column).size(), trials, std::string("a cell per trial in ") + column);
    }
    const std::vector<Json>& scores = array_at(cells, "score").items();
    const std::vector<Json>& trade_counts = array_at(cells, "trades").items();
    std::size_t valid = 0;
    std::size_t scored = 0;
    std::optional<std::size_t> highest;
    for (std::size_t i = 0; i < trials; ++i) {
        if (!trade_counts[i].is_null()) {
            ++valid;
        }
        if (scores[i].is_number()) {
            ++scored;
            if (!highest || scores[i].as_number() > scores[*highest].as_number()) {
                highest = i;
            }
        }
    }
    check::expect_equal(count_at(result, "valid"), valid, "valid counts the trials that ran");
    const Json& errors = array_at(result, "errors");
    check::expect(errors.size() <= 5 && (errors.size() > 0) == (valid < trials), "errors when trials failed");

    const Json& best = at(result, "best");
    check::expect(highest.has_value(), "something scored");
    const std::size_t index = count_at(best, "index");
    check::expect_equal(index, *highest, "the best has the highest score");
    check::expect_equal(at(best, "score").dump(), scores[index].dump(), "the best's score is its cell's");
    check::expect_equal(number_at(best, "x"), x_of(index), "the best's x");
    expect_y(best, index, "the best");
    const Json& best_metrics = object_at(best, "metrics");
    expect_metrics(best_metrics, "best.metrics");
    for (const char* column : {"sharpe", "cagr", "total_return", "max_drawdown", "trades"}) {
        check::expect_equal(at(best_metrics, column).dump(),
                            array_at(cells, column).items()[index].dump(),
                            std::string("the best's ") + column + " is its cell's");
    }
    const std::string& metric = text_at(result, "metric");
    if (metric != "max_drawdown") {
        check::expect_equal(at(best, "score").dump(), at(best_metrics, metric).dump(), "the score is the metric");
    }

    const Json& top = array_at(result, "top", std::min<std::size_t>(scored, 10));
    check::expect_equal(count_at(top.items()[0], "index"), index, "the best tops the list");
    std::vector<std::size_t> listed;
    for (const Json& entry : top.items()) {
        const std::size_t i = count_at(entry, "index");
        check::expect(i < trials && std::find(listed.begin(), listed.end(), i) == listed.end(), "a new trial");
        check::expect(listed.empty() || number_at(entry, "score") <= scores[listed.back()].as_number(),
                      "in order of score");
        listed.push_back(i);
        for (const char* column : {"score", "sharpe", "cagr", "max_drawdown", "trades"}) {
            check::expect_equal(at(entry, column).dump(),
                                array_at(cells, column).items()[i].dump(),
                                std::string("top's ") + column + " is its cell's");
        }
        check::expect_equal(number_at(entry, "x"), x_of(i), "top's x");
        expect_y(entry, i, "top");
    }
    const double cutoff = scores[listed.back()].as_number();
    for (std::size_t i = 0; i < trials; ++i) {
        check::expect(!scores[i].is_number() || scores[i].as_number() <= cutoff ||
                          std::find(listed.begin(), listed.end(), i) != listed.end(),
                      "no trial left out scores above the top");
    }

    const Json& overfitting = object_at(result, "overfitting");
    check::expect_equal(count_at(overfitting, "trials"), valid, "overfitting counts the trials that ran");
    for (const char* key : {"best_sharpe", "expected_max_sharpe", "deflated_sharpe", "pbo", "oos_loss_probability"}) {
        check::expect(number_or_null(at(overfitting, key)), std::string("overfitting.") + key);
    }
    for (const char* key : {"pbo", "oos_loss_probability", "deflated_sharpe"}) {
        const Json& probability = at(overfitting, key);
        check::expect(probability.is_null() || (probability.as_number() >= 0.0 && probability.as_number() <= 1.0),
                      std::string("overfitting.") + key + " is a probability");
    }
    static_cast<void>(count_at(overfitting, "blocks"));
    const std::size_t splits = count_at(overfitting, "splits");
    const Json& logits = array_at(overfitting, "logits");
    const Json& degradation = array_at(overfitting, "degradation", logits.size());
    check::expect(logits.size() <= 1000 && logits.size() <= splits, "a sample of the splits");
    for (const Json& point : degradation.items()) {
        check::expect(point.is_array() && point.size() == 2, "degradation holds pairs");
    }
    return valid;
}

std::string two_parameter_sweep(const std::string& more) {
    return R"({"command": "sweep", "data": {"synthetic": {"seed": 21, "bars": 1200}}, )"
           R"("strategy": {"id": "sma_cross"}, "sweep": {"x": {"param": "fast", "from": 10, "to": 50, "step": 10}, )"
           R"("y": {"param": "slow", "from": 20, "to": 60, "step": 20})" +
           more + "}}";
}

void a_sweep_has_every_part() {
    const std::string response = api::handle(two_parameter_sweep(""));
    const Json parsed = parse(response);
    check::expect(flag_at(parsed, "ok"), "the sweep runs: " + excerpt(response));
    const Json& result = object_at(parsed, "result");
    check::expect_equal(text_at(result, "strategy"), std::string("sma_cross"), "the strategy");
    check::expect_equal(text_at(result, "metric"), std::string("sharpe"), "sharpe by default");
    const Json& x = object_at(result, "x");
    check::expect_equal(text_at(x, "param"), std::string("fast"), "x sweeps fast");
    check::expect(numbers_in(array_at(x, "values"), "x") == std::vector<double>{10.0, 20.0, 30.0, 40.0, 50.0},
                  "fast from 10 to 50 by 10");
    const Json& y = object_at(result, "y");
    check::expect_equal(text_at(y, "param"), std::string("slow"), "y sweeps slow");
    check::expect(numbers_in(array_at(y, "values"), "y") == std::vector<double>{20.0, 40.0, 60.0},
                  "slow from 20 to 60 by 20");
    check::expect_equal(count_at(result, "trials"), std::size_t{15}, "5 by 3");
    // Row by row: trial i runs x[i % 5] and y[i / 5]. A fast window that isn't shorter than the slow one can't run.
    const Json& cells = object_at(result, "cells");
    for (std::size_t i = 0; i < 15; ++i) {
        const double fast = 10.0 * static_cast<double>(i % 5 + 1);
        const double slow = 20.0 * static_cast<double>(i / 5 + 1);
        for (const char* column : {"score", "sharpe", "cagr", "total_return", "max_drawdown", "trades"}) {
            const bool blank = array_at(cells, column).items()[i].is_null();
            check::expect(fast < slow || blank, std::string(column) + " of a combination that can't run is null");
        }
        check::expect(fast >= slow || array_at(cells, "trades").items()[i].is_number(), "one that can runs");
    }
    check::expect_equal(expect_consistent_sweep(result), std::size_t{9}, "nine combinations run");
    expect_same(api::handle(two_parameter_sweep(R"(, "threads": 1)")), response, "one thread or many, the same sweep");
    for (const Json& error : array_at(result, "errors").items()) {
        check::expect(error.is_string() && contains(error.as_string(), "must be less than"),
                      "says why: " + error.dump());
    }
}

void a_sweep_of_one_parameter() {
    const Json result = result_of(
        R"({"command": "sweep", "data": {"synthetic": {"seed": 21, "bars": 1200}}, "strategy": {"id": "momentum"}, )"
        R"("sweep": {"x": {"param": "lookback", "from": 20, "to": 100, "step": 20}, "metric": "cagr"}})");
    check::expect(at(result, "y").is_null(), "no y axis");
    check::expect_equal(text_at(result, "metric"), std::string("cagr"), "the metric asked for");
    check::expect_equal(expect_consistent_sweep(result), std::size_t{5}, "every lookback runs");
    check::expect_equal(array_at(result, "errors").size(), std::size_t{0}, "no errors");
}

void a_walk_forward_has_every_part() {
    const std::string request =
        R"({"command": "walkforward", "data": {"synthetic": {"seed": 23, "bars": 1500}}, )"
        R"("strategy": {"id": "momentum"}, "walkforward": {"x": {"param": "lookback", "from": 20, "to": 60, )"
        R"("step": 20}, "in_sample_years": 2, "out_of_sample_years": 1)";
    const Json rolling = result_of(request + "}}");
    const Json anchored = result_of(request + R"(, "anchored": true}, "output": {"series": false}})");
    for (const Json* result : {&rolling, &anchored}) {
        const bool is_anchored = result == &anchored;
        const std::string what = is_anchored ? "anchored: " : "rolling: ";
        check::expect_equal(flag_at(*result, "anchored"), is_anchored, what + "anchored as asked");
        check::expect_equal(text_at(*result, "strategy"), std::string("momentum"), what + "the strategy");
        check::expect_equal(text_at(*result, "metric"), std::string("sharpe"), what + "the metric");
        check::expect_equal(text_at(*result, "x_param"), std::string("lookback"), what + "the parameter");
        check::expect_equal(text_at(*result, "y_param"), std::string(), what + "only one");
        check::expect(number_or_null(at(*result, "efficiency")), what + "the efficiency");
        expect_metrics(object_at(*result, "metrics"), "metrics");
        expect_metrics(object_at(*result, "benchmark"), "benchmark");
        const Json& range = object_at(*result, "range");
        const std::vector<Json>& folds = array_at(*result, "folds").items();
        check::expect(folds.size() >= 3, what + "1,500 bars make several folds");
        for (std::size_t f = 0; f < folds.size(); ++f) {
            const Json& fold = folds[f];
            const std::string at_fold = what + "fold " + std::to_string(f) + ": ";
            const std::string& is_from = text_at(fold, "is_from");
            const std::string& oos_from = text_at(fold, "oos_from");
            check::expect(is_from <= text_at(fold, "is_to") && text_at(fold, "is_to") < oos_from &&
                              oos_from <= text_at(fold, "oos_to"),
                          at_fold + "in sample, then out of sample");
            const double x = number_at(fold, "x");
            check::expect(x == 20.0 || x == 40.0 || x == 60.0, at_fold + "a lookback that was swept");
            check::expect(at(fold, "y").is_null(), at_fold + "no y");
            for (const char* part : {"in_sample", "out_of_sample"}) {
                const Json& measured = object_at(fold, part);
                for (const char* key : {"sharpe", "cagr", "total_return", "max_drawdown"}) {
                    check::expect(number_or_null(at(measured, key)), at_fold + part + "." + key);
                }
                static_cast<void>(count_at(measured, "trades"));
            }
            if (f == 0) {
                check::expect_equal(count_at(fold, "oos_index"), std::size_t{0}, at_fold + "out of sample from 0");
                check::expect_equal(is_from, text_at(range, "from"), at_fold + "starts with the range");
                continue;
            }
            const Json& previous = folds[f - 1];
            check::expect(oos_from > text_at(previous, "oos_to"), at_fold + "follows the last out of sample");
            check::expect(count_at(fold, "oos_index") > count_at(previous, "oos_index"), at_fold + "a later index");
            if (is_anchored) {
                check::expect_equal(is_from, text_at(range, "from"), at_fold + "anchored at the start");
            } else {
                check::expect(is_from > text_at(previous, "is_from"), at_fold + "rolled forward");
            }
        }
        check::expect_equal(text_at(folds.back(), "oos_to"), text_at(range, "to"), what + "to the end of the range");
        static_cast<void>(array_at(*result, "trades"));
    }
    check::expect(anchored.find("series") == nullptr, "output.series false leaves out the series");

    // The out-of-sample windows, stitched together.
    const Json& series = object_at(rolling, "series");
    const std::vector<double> times = numbers_in(array_at(series, "time"), "series.time");
    expect_increasing(times, "series.time");
    for (const char* key : {"equity", "benchmark", "drawdown", "benchmark_drawdown", "exposure"}) {
        check::expect_equal(array_at(series, key).size(), times.size(), std::string("series.") + key);
    }
    const std::vector<Json>& folds = array_at(rolling, "folds").items();
    for (const Json& fold : folds) {
        const std::size_t start = count_at(fold, "oos_index");
        check::expect(start < times.size() && times[start] == seconds_of(text_at(fold, "oos_from")),
                      "the series at oos_index is the fold's first out-of-sample bar");
    }
    check::expect_equal(times.back(), seconds_of(text_at(folds.back(), "oos_to")), "the series ends with the last");
    for (const Json& trade : array_at(rolling, "trades").items()) {
        const std::size_t entered = count_at(trade, "entry_index");
        const std::size_t exited = count_at(trade, "exit_index");
        check::expect(entered <= exited && exited < times.size(), "a trade within the series");
        check::expect_equal(seconds_of(text_at(trade, "entry")), times[entered], "its entry is at entry_index");
    }
}

std::string montecarlo_request(int seed) {
    return R"({"command": "montecarlo", "data": {"synthetic": {"seed": 25, "bars": 800}}, )"
           R"("strategy": {"id": "momentum"}, "montecarlo": {"paths": 60, "block": 10, "seed": )" +
           std::to_string(seed) + "}}";
}

void a_monte_carlo_has_every_part() {
    const std::string response = api::handle(montecarlo_request(5));
    const Json parsed = parse(response);
    check::expect(flag_at(parsed, "ok"), "it runs: " + excerpt(response));
    const Json& result = object_at(parsed, "result");
    check::expect_equal(text_at(result, "strategy"), std::string("momentum"), "the strategy");
    const std::size_t paths = count_at(result, "paths");
    check::expect_equal(paths, std::size_t{60}, "the paths");
    check::expect_equal(count_at(result, "block"), std::size_t{10}, "the block");
    check::expect_equal(count_at(result, "seed"), std::size_t{5}, "the seed");
    const Json& metrics = object_at(result, "metrics");
    expect_metrics(metrics, "metrics");

    const Json& range = object_at(result, "range");
    const std::vector<double> times = numbers_in(array_at(result, "time"), "time");
    const std::size_t points = times.size();
    check::expect(points >= 2 && points <= 160, "at most 160 checkpoints");
    expect_increasing(times, "time");
    check::expect_equal(times.front(), seconds_of(text_at(range, "from")), "from the first bar");
    check::expect_equal(times.back(), seconds_of(text_at(range, "to")), "to the last");
    const std::vector<double> actual = numbers_in(array_at(result, "actual", points), "actual");
    check::expect_near(actual.back(),
                       number_at(metrics, "final_equity"),
                       1e-6 * actual.back(),
                       "the actual equity ends at the final equity");
    const Json& bands = object_at(result, "bands");
    std::vector<std::vector<double>> levels;
    for (const char* band : {"p5", "p25", "p50", "p75", "p95"}) {
        levels.push_back(numbers_in(array_at(bands, band, points), band));
    }
    for (std::size_t k = 0; k < points; ++k) {
        for (std::size_t q = 1; q < levels.size(); ++q) {
            check::expect(levels[q - 1][k] <= levels[q][k], "the bands in order at checkpoint " + std::to_string(k));
        }
    }
    const Json& distributions = object_at(result, "distributions");
    for (const char* key : {"total_return", "cagr", "max_drawdown", "sharpe"}) {
        const std::vector<double> values = numbers_in(array_at(distributions, key, paths), key);
        check::expect(std::is_sorted(values.begin(), values.end()), std::string(key) + " is sorted");
    }
    const std::vector<double> returns = numbers_in(array_at(distributions, "total_return"), "total_return");
    const auto losses =
        static_cast<double>(std::count_if(returns.begin(), returns.end(), [](double r) { return r < 0.0; }));
    check::expect_near(number_at(result, "loss_probability"),
                       losses / static_cast<double>(paths),
                       1e-12,
                       "the loss probability is the share of paths that lose");

    expect_same(api::handle(montecarlo_request(5)), response, "the same seed, the same paths");
    check::expect(api::handle(montecarlo_request(6)) != response, "another seed, other paths");
}

// ---- Transports ----

void eight_threads_get_the_same_response() {
    const std::string request =
        R"({"command": "backtest", "data": "CAT", "range": {"from": "1995-01-01", "to": "1999-12-31"}, )"
        R"("strategy": {"id": "macd"}, "config": {"allow_short": true, "commission_bps": 2}, )"
        R"("output": {"fills": true}})";
    const std::string expected = api::handle(request);
    check::expect(flag_at(parse(expected), "ok"), "the backtest runs: " + excerpt(expected));
    for (int round = 0; round < 2; ++round) {
        if (round == 0) {
            // A fresh copy of the dataset, with an empty indicator cache for the threads to fill at once.
            const std::shared_ptr<const api::Dataset> dataset = api::find_dataset("CAT");
            check::expect(dataset != nullptr, "CAT is loaded");
            api::register_dataset(*dataset);
        }
        constexpr std::size_t threads = 8;
        std::vector<std::string> responses(threads);
        std::atomic<std::size_t> ready{0};
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                ready.fetch_add(1);
                while (ready.load() < threads) {
                    std::this_thread::yield();
                }
                responses[t] = api::handle(request);
            });
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
        for (std::size_t t = 0; t < threads; ++t) {
            expect_same(responses[t],
                        expected,
                        "round " + std::to_string(round) + ", thread " + std::to_string(t) + " against one thread");
        }
    }
}

void with_meta_adds_meta_and_nothing_else() {
    const std::vector<std::string> requests = {
        R"({"command": "version"})",
        R"({"command": "backtest", "data": "AAPL", "range": {"from": "2009-01-01", "to": "2009-03-31"}})",
        R"({"command": "backtest", "data": "AAPL", "config": {"comission_bps": 1}})",
    };
    for (const std::string& request : requests) {
        const std::string response = api::handle(request);
        const std::string with = api::with_meta(response, "native \"test\"", 12.3456);
        check::expect(with.size() > response.size() && with.back() == '}', "meta is added inside the object");
        check::expect(with.compare(0, response.size() - 1, response, 0, response.size() - 1) == 0,
                      "the response before it is unchanged");
        const Json before = parse(response);
        const Json after = parse(with);
        std::vector<std::string> keys = before.keys();
        keys.push_back("meta");
        check::expect(after.keys() == keys, "meta comes last, after the response's own keys");
        for (const std::string& key : before.keys()) {
            check::expect_equal(at(after, key).dump(), at(before, key).dump(), key + " is unchanged");
        }
        const Json& meta = object_at(after, "meta");
        check::expect(meta.keys() == std::vector<std::string>{"runtime", "ms", "threads"}, "runtime, ms, and threads");
        check::expect_equal(text_at(meta, "runtime"), std::string("native \"test\""), "the runtime");
        check::expect_near(number_at(meta, "ms"), 12.346, 1e-9, "the milliseconds, to the microsecond");
        check::expect(count_at(meta, "threads") >= std::size_t{1}, "the threads");
    }
    check::expect_equal(api::with_meta("", "native", 1.0), std::string(), "nothing to add to");
    check::expect_equal(api::with_meta("[1]", "native", 1.0), std::string("[1]"), "not an object");
}

// ---- Bugs found by these tests ----

// A custom strategy without rules runs the catalog's example with its parameters' defaults, as the lab's
// selectStrategy() does, but the example's rules used $trend, $fast, and $slow while its catalog entry declared no
// parameters, so {"id": "custom"} failed with "$trend is not a parameter". The entry now declares all three.
void the_custom_example_runs_with_its_defaults() {
    const Json catalog = result_of(R"({"command": "catalog"})");
    const std::vector<Json>& strategies = array_at(catalog, "strategies").items();
    const auto custom = std::find_if(strategies.begin(), strategies.end(), [](const Json& strategy) {
        return text_at(strategy, "id") == "custom";
    });
    check::expect(custom != strategies.end(), "the catalog has the custom strategy");
    struct Param {
        const char* name;
        double usual;
        double low;
        double high;
    };
    const Param expected[] = {{"trend", 200.0, 20.0, 400.0}, {"fast", 12.0, 2.0, 200.0}, {"slow", 26.0, 5.0, 400.0}};
    const Json& params = array_at(*custom, "params", std::size(expected));
    std::vector<std::string> declared;
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        const Json& param = params.items()[i];
        const std::string name = expected[i].name;
        check::expect_equal(text_at(param, "name"), name, "the example's parameters, in order");
        check::expect_equal(number_at(param, "default"), expected[i].usual, name + "'s default");
        check::expect_equal(number_at(param, "min"), expected[i].low, name + "'s least");
        check::expect_equal(number_at(param, "max"), expected[i].high, name + "'s most");
        check::expect(flag_at(param, "integer"), name + " is a whole number");
        declared.push_back(name);
    }
    const Json& rules = object_at(*custom, "rules");
    for (const std::string& rule : rules.keys()) {
        const std::string& source = text_at(rules, rule);
        for (std::size_t dollar = source.find('$'); dollar != std::string::npos;
             dollar = source.find('$', dollar + 1)) {
            std::size_t end = dollar + 1;
            while (end < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[end])) != 0 || source[end] == '_')) {
                ++end;
            }
            const std::string name = source.substr(dollar + 1, end - dollar - 1);
            check::expect(std::find(declared.begin(), declared.end(), name) != declared.end(),
                          "the example's " + rule + " uses $" + name + ", which its catalog entry should declare");
        }
    }
    const Json result = result_of(R"({"command": "backtest", "data": "F", "range": {"from": "2000-01-01", )"
                                  R"("to": "2002-12-31"}, "strategy": {"id": "custom"}})");
    const Json& used = object_at(result, "strategy");
    check::expect_equal(object_at(used, "rules").dump(), rules.dump(), "the example's rules");
    const Json& values = object_at(used, "params");
    check::expect_equal(values.size(), std::size(expected), "the example's parameters and no others");
    for (const Param& param : expected) {
        check::expect_equal(number_at(values, param.name), param.usual, std::string("the default ") + param.name);
    }
}

// parse_timestamp reads 9 to 11 digits as Unix time in seconds, an instant, but resolve_window took any 'to' of 10
// characters or fewer for a date and moved it to the end of its day, so "to": "1704283200" (2024-01-03 12:00 UTC) took
// in the bars until 2024-01-04 11:59:59. It now asks is_calendar_date.
void a_unix_time_as_to_is_an_instant() {
    const Json result = result_of(intraday_bars(R"({"to": "1704283200"})"));
    const Json& range = object_at(result, "range");
    check::expect_equal(text_at(range, "to"), std::string("2024-01-03 12:00"), "to 1704283200 ends at 12:00 UTC");
    check::expect_equal(count_at(range, "bars"), std::size_t{5}, "the bars until 2024-01-03 12:00");
}

// A synthetic market is registered as "synthetic:SEED:BARS:DRIFT:VOLATILITY:START_PRICE", and a synthetic request gets
// whatever is registered under that name, so loading a file under one replaced that seed's market with the file. load
// now refuses names that start with "synthetic:".
void a_loaded_file_cannot_take_a_synthetic_markets_place() {
    const std::string request = R"({"command": "bars", "data": {"synthetic": {"seed": 4242, "bars": 60}}})";
    const std::string market = api::handle(request);
    check::expect(flag_at(parse(market), "ok"), "the market: " + excerpt(market));
    const std::string name = "synthetic:4242:60:0:0.2:100";
    expect_error(R"({"command": "load", "name": )" + json_string(name) + R"(, "csv": )" +
                     json_string(daily_csv(60, false)) + "}",
                 "name",
                 "synthetic:");
    const std::shared_ptr<const api::Dataset> registered = api::find_dataset(name);
    check::expect(registered != nullptr && registered->source == "synthetic", "the market is still registered");
    expect_same(api::handle(request), market, "seed 4242 after a file was offered under its name");
}

// A dataset not yet registered was opened as NAME.csv, and a file system that ignores case found AAPL.csv for "aapl":
// on macOS and Windows, {"data": "aapl"} loaded a second copy of AAPL under that name, where Linux and the WebAssembly
// lab answered that there was no such dataset. The file's name must now match exactly, on every platform.
void dataset_names_are_matched_exactly() {
    expect_error(R"({"command": "bars", "data": "aapl", "range": {"from": "2013-12-01"}})",
                 "data",
                 "unknown dataset 'aapl'; did you mean AAPL?");
    check::expect(api::find_dataset("aapl") == nullptr, "nothing is registered as aapl");
}

}  // namespace

int main() {
    api::set_data_directory(NANOTRADE_DATA_DIR);
    return check::run({
        {"version names the engine", version_names_the_engine},
        {"the catalog describes every strategy", catalog_describes_every_strategy},
        {"the catalog lists the functions and columns", catalog_lists_functions_and_columns},
        {"datasets follow the manifest", datasets_follow_the_manifest},
        {"every built-in strategy runs with its defaults", every_built_in_strategy_runs_with_its_defaults},
        {"load registers a dataset for bars and backtest", load_registers_a_dataset},
        {"load refuses bad names", load_refuses_bad_names},
        {"loading under a bundled name replaces that dataset", loading_under_a_bundled_name_replaces_that_dataset},
        {"bars cover their range", bars_cover_their_range},
        {"a date as 'to' takes in the whole day", a_date_as_to_takes_in_the_whole_day},
        {"is_calendar_date tells days from instants", is_calendar_date_tells_days_from_instants},
        {"a default backtest has every part", a_default_backtest_has_every_part},
        {"output flags choose the parts", output_flags_choose_the_parts},
        {"ranges need two bars", ranges_need_two_bars},
        {"unknown fields are refused with a hint", unknown_fields_are_refused_with_a_hint},
        {"unknown commands, strategies, and datasets get a hint", unknown_names_get_a_hint},
        {"wrong types name their field", wrong_types_name_their_field},
        {"rule errors point into the rule", rule_errors_point_into_the_rule},
        {"parameter errors name the parameter", parameter_errors_name_the_parameter},
        {"invalid JSON reports a position", invalid_json_reports_a_position},
        {"inline CSV runs without being registered", inline_csv_runs_without_being_registered},
        {"synthetic markets follow their seed", synthetic_markets_follow_their_seed},
        {"a sweep has every part", a_sweep_has_every_part},
        {"a sweep of one parameter", a_sweep_of_one_parameter},
        {"a walk forward has every part", a_walk_forward_has_every_part},
        {"a Monte Carlo run has every part", a_monte_carlo_has_every_part},
        {"eight threads get the same response", eight_threads_get_the_same_response},
        {"with_meta adds meta and nothing else", with_meta_adds_meta_and_nothing_else},
        {"the custom example runs with its defaults", the_custom_example_runs_with_its_defaults},
        {"a Unix time as 'to' is an instant", a_unix_time_as_to_is_an_instant},
        {"a loaded file can't take a synthetic market's place", a_loaded_file_cannot_take_a_synthetic_markets_place},
        {"dataset names are matched exactly", dataset_names_are_matched_exactly},
    });
}
