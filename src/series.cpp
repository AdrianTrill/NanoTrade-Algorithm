#include "nanotrade/series.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace nanotrade {

void BarSeries::reserve(std::size_t bars) {
    time.reserve(bars);
    open.reserve(bars);
    high.reserve(bars);
    low.reserve(bars);
    close.reserve(bars);
    volume.reserve(bars);
}

void BarSeries::push_back(Timestamp t, double o, double h, double l, double c, double v) {
    time.push_back(t);
    open.push_back(o);
    high.push_back(h);
    low.push_back(l);
    close.push_back(c);
    volume.push_back(v);
}

bool BarSeries::intraday() const noexcept {
    return std::any_of(time.begin(), time.end(), [](Timestamp t) { return t % seconds_per_day != 0; });
}

double BarSeries::bars_per_year() const noexcept {
    if (size() < 2) {
        return 252.0;
    }
    const double years = years_between(time.front(), time.back());
    if (years <= 0.0) {
        return 252.0;
    }
    return static_cast<double>(size() - 1) / years;
}

std::size_t BarSeries::lower_bound(Timestamp from) const noexcept {
    return static_cast<std::size_t>(std::lower_bound(time.begin(), time.end(), from) - time.begin());
}

std::size_t BarSeries::upper_bound(Timestamp to) const noexcept {
    return static_cast<std::size_t>(std::upper_bound(time.begin(), time.end(), to) - time.begin());
}

namespace {

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

// Splits one CSV record. Quoted fields may contain the delimiter and doubled quotes, but not line breaks: market data
// never has them, and refusing them keeps line numbers in error messages exact.
std::vector<std::string> split_fields(std::string_view line, char delimiter, std::size_t line_number) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    bool was_quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == '"' && text::trim(field).empty()) {
            quoted = true;
            was_quoted = true;
            field.clear();
        } else if (c == delimiter) {
            fields.emplace_back(was_quoted ? field : std::string(text::trim(field)));
            field.clear();
            was_quoted = false;
        } else {
            field.push_back(c);
        }
    }
    if (quoted) {
        throw Error("line " + std::to_string(line_number) + ": a quoted field is not closed");
    }
    fields.emplace_back(was_quoted ? field : std::string(text::trim(field)));
    return fields;
}

// Header names are compared without case, spaces, dots, underscores, or slashes: "Adj. Close", "adj_close", and
// "AdjClose" are one column.
std::string normalize_header(std::string_view name) {
    std::string out;
    for (const char c : text::lowercase(text::trim(name))) {
        if (c != ' ' && c != '.' && c != '_' && c != '-' && c != '/' && c != '"' && c != '<' && c != '>') {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<std::size_t> find_column(const std::vector<std::string>& headers,
                                       std::initializer_list<std::string_view> names) {
    for (const std::string_view name : names) {
        for (std::size_t i = 0; i < headers.size(); ++i) {
            if (headers[i] == name) {
                return i;
            }
        }
    }
    return std::nullopt;
}

bool is_missing(std::string_view field) {
    const std::string lower = text::lowercase(field);
    return lower.empty() || lower == "null" || lower == "nan" || lower == "na" || lower == "n/a" || lower == "-";
}

// Prices may carry a currency sign or thousands separators, as Nasdaq.com's exports do.
std::optional<double> parse_price(std::string_view field) {
    std::string cleaned;
    for (const char c : field) {
        if (c != '$' && c != ',' && c != ' ') {
            cleaned.push_back(c);
        }
    }
    return text::parse_number(cleaned);
}

// Dates such as 01/02/2024 are month first in American exports and day first in European ones. The whole column
// decides: a first part above 12 means day first, a second part above 12 means month first.
struct SlashDate {
    int first{0};
    int second{0};
    int year{0};
    std::string rest;  // a time of day, if any
};

std::optional<SlashDate> parse_slash_date(std::string_view field) {
    SlashDate date;
    std::size_t position = 0;
    const auto number = [&](std::size_t max_digits) -> std::optional<int> {
        int value = 0;
        std::size_t count = 0;
        while (position < field.size() && field[position] >= '0' && field[position] <= '9' && count < max_digits) {
            value = value * 10 + (field[position] - '0');
            ++position;
            ++count;
        }
        return count == 0 ? std::nullopt : std::optional<int>(value);
    };
    const auto first = number(2);
    if (!first || position >= field.size() || (field[position] != '/' && field[position] != '.')) {
        return std::nullopt;
    }
    const char separator = field[position++];
    const auto second = number(2);
    if (!second || position >= field.size() || field[position] != separator) {
        return std::nullopt;
    }
    ++position;
    const std::size_t year_start = position;
    const auto year = number(4);
    if (!year || position - year_start != 4) {
        return std::nullopt;
    }
    date.first = *first;
    date.second = *second;
    date.year = *year;
    date.rest = std::string(field.substr(position));
    return date;
}

std::string two(int value) {
    return std::string(1, static_cast<char>('0' + value / 10)) + static_cast<char>('0' + value % 10);
}

struct Row {
    Timestamp time{0};
    double open{0}, high{0}, low{0}, close{0}, volume{0};
    std::size_t line{0};
};

}  // namespace

LoadedSeries load_csv(std::string_view text, std::string symbol, const CsvOptions& options) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    const std::vector<std::string_view> lines = split_lines(text);
    std::size_t header_line = 0;
    while (header_line < lines.size() && text::trim(lines[header_line]).empty()) {
        ++header_line;
    }
    if (header_line == lines.size()) {
        throw Error("the CSV file is empty");
    }

    // The delimiter is whichever of comma, semicolon, and tab splits the header into the most columns.
    const std::string_view header_text = lines[header_line];
    char delimiter = ',';
    std::size_t best = 0;
    for (const char candidate : {',', ';', '\t'}) {
        const auto count = static_cast<std::size_t>(std::count(header_text.begin(), header_text.end(), candidate));
        if (count > best) {
            best = count;
            delimiter = candidate;
        }
    }

    std::vector<std::string> headers;
    for (const std::string& field : split_fields(header_text, delimiter, header_line + 1)) {
        headers.push_back(normalize_header(field));
    }
    const auto date_column = find_column(headers, {"date", "datetime", "timestamp", "time", "opentime", "day"});
    const auto open_column = find_column(headers, {"open", "o"});
    const auto high_column = find_column(headers, {"high", "h"});
    const auto low_column = find_column(headers, {"low", "l"});
    const auto close_column = find_column(headers, {"close", "closelast", "last", "price", "c"});
    const auto volume_column = find_column(headers, {"volume", "vol", "v"});
    const auto adj_open = find_column(headers, {"adjopen", "adjustedopen"});
    const auto adj_high = find_column(headers, {"adjhigh", "adjustedhigh"});
    const auto adj_low = find_column(headers, {"adjlow", "adjustedlow"});
    const auto adj_close = find_column(headers, {"adjclose", "adjustedclose"});
    const auto adj_volume = find_column(headers, {"adjvolume", "adjustedvolume"});

    if (!date_column) {
        throw Error("the CSV header has no date column (looked for Date, Datetime, Timestamp, or Time)");
    }
    if (!close_column && !adj_close) {
        throw Error("the CSV header has no close column (looked for Close, Adj Close, Last, or Price)");
    }

    LoadedSeries loaded;
    loaded.series.symbol = std::move(symbol);
    LoadReport& report = loaded.report;

    // How the prices are adjusted: whole adjusted OHLC columns (Quandl's WIKI format) or, as Yahoo does, only an
    // adjusted close, by whose ratio to the close the open, high, and low are scaled.
    const bool use_adjusted_columns = options.adjust && adj_open && adj_high && adj_low && adj_close;
    const bool scale_by_adjusted_close = options.adjust && !use_adjusted_columns && adj_close && close_column;
    report.adjusted = use_adjusted_columns ? "columns" : (scale_by_adjusted_close ? "close" : "");

    // Slash dates decide their order over the whole column first.
    bool month_first = true;
    {
        bool any_slash = false;
        bool first_above_12 = false;
        bool second_above_12 = false;
        for (std::size_t i = header_line + 1; i < lines.size(); ++i) {
            if (text::trim(lines[i]).empty()) {
                continue;
            }
            const auto fields = split_fields(lines[i], delimiter, i + 1);
            if (*date_column >= fields.size()) {
                continue;
            }
            if (const auto date = parse_slash_date(fields[*date_column])) {
                any_slash = true;
                first_above_12 = first_above_12 || date->first > 12;
                second_above_12 = second_above_12 || date->second > 12;
            }
        }
        if (any_slash) {
            if (first_above_12 && second_above_12) {
                throw Error("the dates mix month-first and day-first orders");
            }
            month_first = !first_above_12;
            report.date_order = month_first ? "month first" : "day first";
            if (!first_above_12 && !second_above_12) {
                report.warnings.emplace_back("dates such as 01/02 could be either order; read them month first");
            }
        }
    }

    std::vector<Row> rows;
    rows.reserve(lines.size());
    for (std::size_t i = header_line + 1; i < lines.size(); ++i) {
        const std::size_t line_number = i + 1;
        if (text::trim(lines[i]).empty()) {
            continue;
        }
        ++report.rows;
        const std::vector<std::string> fields = split_fields(lines[i], delimiter, line_number);
        const auto field = [&](std::optional<std::size_t> column) -> std::string_view {
            return column && *column < fields.size() ? std::string_view(fields[*column]) : std::string_view();
        };
        const auto where = [&](std::string_view column) {
            return "line " + std::to_string(line_number) + ", " + std::string(column);
        };

        std::string date_text(field(date_column));
        if (const auto slash = parse_slash_date(date_text)) {
            const int month = month_first ? slash->first : slash->second;
            const int day = month_first ? slash->second : slash->first;
            date_text = std::to_string(slash->year) + "-" + two(month) + "-" + two(day) + slash->rest;
        }
        const auto time = parse_timestamp(date_text);
        if (!time) {
            throw Error(where("date") + ": '" + std::string(field(date_column)) + "' is not a date");
        }

        const auto price_column = [&](std::optional<std::size_t> plain, std::optional<std::size_t> adjusted) {
            return use_adjusted_columns ? adjusted : plain;
        };
        constexpr std::array<std::string_view, 3> plain_names{"open", "high", "low"};
        constexpr std::array<std::string_view, 3> adjusted_names{"adjusted open", "adjusted high", "adjusted low"};
        const auto& names = use_adjusted_columns ? adjusted_names : plain_names;
        const std::optional<std::size_t> close_source =
            use_adjusted_columns || !close_column ? adj_close : close_column;
        if (is_missing(field(close_source))) {
            ++report.skipped;
            continue;
        }
        const auto read = [&](std::optional<std::size_t> column, std::string_view name) -> std::optional<double> {
            const std::string_view value = field(column);
            if (!column || is_missing(value)) {
                return std::nullopt;
            }
            const auto number = parse_price(value);
            if (!number) {
                throw Error(where(name) + ": '" + std::string(value) + "' is not a number");
            }
            return number;
        };
        const auto require_positive = [&](double value, std::string_view name) {
            if (!std::isfinite(value) || value <= 0.0) {
                throw Error(where(name) + ": prices must be positive, got " + text::format_number(value));
            }
        };
        const std::string_view close_name = close_source == adj_close ? "adjusted close" : "close";
        const std::optional<double> close_value = read(close_source, close_name);
        if (!close_value) {
            ++report.skipped;
            continue;
        }
        const double close = *close_value;
        // The close first: missing columns are filled in from it, and an error should name a column in the file.
        require_positive(close, close_name);
        double open = read(price_column(open_column, adj_open), names[0]).value_or(close);
        double high = read(price_column(high_column, adj_high), names[1]).value_or(std::max(open, close));
        double low = read(price_column(low_column, adj_low), names[2]).value_or(std::min(open, close));
        double volume = read(use_adjusted_columns && adj_volume ? adj_volume : volume_column, "volume").value_or(0.0);

        for (const auto& [name, value] :
             {std::pair<std::string_view, double>{names[0], open}, {names[1], high}, {names[2], low}}) {
            require_positive(value, name);
        }
        double final_close = close;
        if (scale_by_adjusted_close) {
            if (const auto adjusted = read(adj_close, "adjusted close")) {
                require_positive(*adjusted, "adjusted close");
                const double ratio = *adjusted / close;
                open *= ratio;
                high *= ratio;
                low *= ratio;
                final_close = *adjusted;
            }
        }
        if (!std::isfinite(volume) || volume < 0.0) {
            report.warnings.push_back(where("volume") + ": negative volume read as 0");
            volume = 0.0;
        }
        const double top = std::max({open, high, low, final_close});
        const double bottom = std::min({open, high, low, final_close});
        if (high != top || low != bottom) {
            ++report.repaired;
            high = top;
            low = bottom;
        }
        rows.push_back({*time, open, high, low, final_close, volume, line_number});
    }

    if (rows.empty()) {
        throw Error("the CSV file has no bars with prices");
    }

    // Newest first is common (Quandl, Nasdaq.com); anything else out of order is sorted, keeping the file's order
    // between equal timestamps so the last duplicate wins.
    const bool ascending =
        std::is_sorted(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.time < b.time; });
    if (!ascending) {
        const bool descending =
            std::is_sorted(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.time > b.time; });
        if (descending) {
            std::reverse(rows.begin(), rows.end());
            report.reversed = true;
        } else {
            std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.time < b.time; });
            report.sorted = true;
        }
    }

    BarSeries& series = loaded.series;
    series.reserve(rows.size());
    std::size_t kept_line = 0;
    for (const Row& row : rows) {
        if (!series.empty() && series.time.back() == row.time) {
            ++report.duplicates;
            if (row.line > kept_line) {  // the row later in the file wins
                const std::size_t last = series.size() - 1;
                series.open[last] = row.open;
                series.high[last] = row.high;
                series.low[last] = row.low;
                series.close[last] = row.close;
                series.volume[last] = row.volume;
                kept_line = row.line;
            }
            continue;
        }
        series.push_back(row.time, row.open, row.high, row.low, row.close, row.volume);
        kept_line = row.line;
    }
    if (series.size() < 2) {
        throw Error("the CSV file needs at least two bars");
    }
    return loaded;
}

LoadedSeries load_csv_file(const std::string& path, std::string symbol, const CsvOptions& options) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("can't open " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return load_csv(buffer.str(), std::move(symbol), options);
}

}  // namespace nanotrade
