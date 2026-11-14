// The CSV loader against the files market data comes in: plain OHLCV, Yahoo's with an adjusted close and "null" rows,
// Quandl's WIKI prices with adjusted columns, newest first, and Nasdaq.com's with dollar signs and month-first dates;
// the dialects of CSV, the repairs and reports the loader makes, and the errors it raises; BarSeries' bar rate,
// bounds, and intraday test; and the seven bundled datasets against their manifest.

#include "check.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/json.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using nanotrade::BarSeries;
using nanotrade::CsvOptions;
using nanotrade::Error;
using nanotrade::Json;
using nanotrade::LoadedSeries;
using nanotrade::LoadReport;
using nanotrade::Timestamp;

// ---- Helpers ----

// Seconds since the epoch of a UTC date and time of day.
Timestamp utc(std::int64_t year, unsigned month, unsigned day, std::int64_t hour = 0, std::int64_t minute = 0) {
    return nanotrade::days_from_civil(year, month, day) * nanotrade::seconds_per_day + hour * 3600 + minute * 60;
}

LoadedSeries load(std::string_view text, const CsvOptions& options = {}) {
    return nanotrade::load_csv(text, "TEST", options);
}

CsvOptions unadjusted() {
    CsvOptions options;
    options.adjust = false;
    return options;
}

// The message of the nanotrade::Error that loading `text` must throw.
std::string error_of(std::string_view text) {
    try {
        load(text);
    } catch (const Error& error) {
        return error.what();
    }
    throw check::Failure("loaded without an error: \"" + std::string(text.substr(0, 60)) + "\"");
}

void expect_contains(const std::string& message, std::string_view phrase, const std::string& what) {
    check::expect(message.find(phrase) != std::string::npos,
                  what + ": \"" + message + "\" should contain \"" + std::string(phrase) + "\"");
}

void expect_value(double actual, double expected, const std::string& what) {
    check::expect_near(actual, expected, 1e-9 * std::max(1.0, std::fabs(expected)), what);
}

// Bar `i` of `bars` has this time, open, high, low, close, and volume.
void expect_bar(const BarSeries& bars,
                std::size_t i,
                Timestamp t,
                double o,
                double h,
                double l,
                double c,
                double v,
                const std::string& what) {
    const std::string bar = what + ", bar " + std::to_string(i);
    check::expect(i < bars.size(), bar + " is missing");
    check::expect(
        bars.time[i] == t,
        bar + ": at " + nanotrade::format_timestamp(bars.time[i]) + ", expected " + nanotrade::format_timestamp(t));
    expect_value(bars.open[i], o, bar + ": open");
    expect_value(bars.high[i], h, bar + ": high");
    expect_value(bars.low[i], l, bar + ": low");
    expect_value(bars.close[i], c, bar + ": close");
    expect_value(bars.volume[i], v, bar + ": volume");
}

void expect_no_warnings(const LoadReport& report, const std::string& what) {
    check::expect(report.warnings.empty(),
                  what + ": unexpected warning \"" + (report.warnings.empty() ? "" : report.warnings.front()) + "\"");
}

// Nothing unusual about the file: every row a bar, oldest first, as written.
void expect_plain_report(const LoadReport& report, std::size_t rows, const std::string& what) {
    check::expect_equal(report.rows, rows, what + ": rows");
    check::expect_equal(report.skipped, std::size_t{0}, what + ": skipped");
    check::expect_equal(report.repaired, std::size_t{0}, what + ": repaired");
    check::expect_equal(report.duplicates, std::size_t{0}, what + ": duplicates");
    check::expect(!report.reversed, what + ": not reversed");
    check::expect(!report.sorted, what + ": not sorted");
    check::expect_equal(report.adjusted, std::string(), what + ": not adjusted");
    check::expect_equal(report.date_order, std::string(), what + ": no date order");
    expect_no_warnings(report, what);
}

// The two bars most dialect tests below spell out: 2024-01-02 10 11 9 10.5 100 and 2024-01-03 10.5 12 10 11.5 200.
void expect_two_bars(const LoadedSeries& loaded, const std::string& what) {
    check::expect_equal(loaded.series.size(), std::size_t{2}, what + ": bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 2), 10, 11, 9, 10.5, 100, what);
    expect_bar(loaded.series, 1, utc(2024, 1, 3), 10.5, 12, 10, 11.5, 200, what);
    expect_plain_report(loaded.report, 2, what);
}

// ---- Formats ----

void plain_file() {
    const LoadedSeries loaded = nanotrade::load_csv(
        "Date,Open,High,Low,Close,Volume\n"
        "2024-01-02,187.15,188.44,183.89,185.64,82488700\n"
        "2024-01-03,184.22,185.88,183.43,184.25,58414500\n"
        "2024-01-04,182.15,183.09,180.88,181.91,71983600\n",
        "AAPL");
    check::expect_equal(loaded.series.symbol, std::string("AAPL"), "the symbol");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 2), 187.15, 188.44, 183.89, 185.64, 82488700, "plain");
    expect_bar(loaded.series, 1, utc(2024, 1, 3), 184.22, 185.88, 183.43, 184.25, 58414500, "plain");
    expect_bar(loaded.series, 2, utc(2024, 1, 4), 182.15, 183.09, 180.88, 181.91, 71983600, "plain");
    check::expect(!loaded.series.intraday(), "daily bars are not intraday");
    expect_plain_report(loaded.report, 3, "plain");
}

constexpr std::string_view yahoo_csv =
    "Date,Open,High,Low,Close,Adj Close,Volume\n"
    "2024-01-02,187.149994,188.440002,183.889999,185.639999,184.734970,82488700\n"
    "2024-01-03,184.220001,185.880005,183.429993,184.250000,183.351746,58414500\n"
    "2024-01-04,182.149994,183.089996,180.880005,181.910004,181.023163,71983600\n";

void yahoo_adjusted_close() {
    const LoadedSeries loaded = load(yahoo_csv);
    check::expect_equal(loaded.report.adjusted, std::string("close"), "adjusted by the close");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    // The open, high, and low scale by Adj Close / Close; the close becomes the adjusted close; volume stays.
    const auto expect_scaled =
        [&](std::size_t i, Timestamp t, double o, double h, double l, double c, double adjusted, double v) {
            const double ratio = adjusted / c;
            expect_bar(loaded.series, i, t, o * ratio, h * ratio, l * ratio, adjusted, v, "Yahoo");
        };
    expect_scaled(0, utc(2024, 1, 2), 187.149994, 188.440002, 183.889999, 185.639999, 184.734970, 82488700);
    expect_scaled(1, utc(2024, 1, 3), 184.220001, 185.880005, 183.429993, 184.250000, 183.351746, 58414500);
    expect_scaled(2, utc(2024, 1, 4), 182.149994, 183.089996, 180.880005, 181.910004, 181.023163, 71983600);
    check::expect_equal(loaded.report.rows, std::size_t{3}, "rows");
    check::expect_equal(loaded.report.skipped, std::size_t{0}, "skipped");
    check::expect_equal(loaded.report.repaired, std::size_t{0}, "scaling keeps the bars consistent");
    check::expect(!loaded.report.reversed && !loaded.report.sorted, "in order");
}

void yahoo_null_rows() {
    const LoadedSeries loaded = load(
        "Date,Open,High,Low,Close,Adj Close,Volume\n"
        "2024-01-02,10,11,9,10.5,10.5,100\n"
        "2024-01-03,null,null,null,null,null,null\n"
        "2024-01-04,10.5,12,10,11.5,11.5,200\n"
        "2024-01-05,null,null,null,null,null,null\n"
        "2024-01-08,11.5,12.5,11,12,12,300\n");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 2), 10, 11, 9, 10.5, 100, "Yahoo with nulls");
    expect_bar(loaded.series, 1, utc(2024, 1, 4), 10.5, 12, 10, 11.5, 200, "Yahoo with nulls");
    expect_bar(loaded.series, 2, utc(2024, 1, 8), 11.5, 12.5, 11, 12, 300, "Yahoo with nulls");
    check::expect_equal(loaded.report.skipped, std::size_t{2}, "the null rows are skipped and counted");
    check::expect_equal(loaded.report.rows, std::size_t{5}, "every data row is read");
    check::expect_equal(loaded.report.adjusted, std::string("close"), "adjusted by the close");
}

// AAPL around its 7-for-1 split of 2014-06-09, newest first as Quandl served it: the raw prices jump, the adjusted
// ones don't, and the adjusted volume of the day before the split is seven times the raw one.
constexpr std::string_view wiki_csv =
    "Date,Open,High,Low,Close,Volume,Ex-Dividend,Split Ratio,Adj. Open,Adj. High,Adj. Low,Adj. Close,Adj. Volume\n"
    "2014-06-10,94.73,95.05,93.57,94.25,62777000.0,0.0,1.0,90.04,90.35,88.94,89.59,62777000.0\n"
    "2014-06-09,92.7,93.88,91.75,93.7,75415000.0,0.0,7.0,88.11,89.24,87.21,89.06,75415000.0\n"
    "2014-06-06,649.9,651.26,644.47,645.57,12497800.0,0.0,1.0,88.25,88.43,87.51,87.66,87484600.0\n";

void quandl_wiki() {
    const LoadedSeries loaded = load(wiki_csv);
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    expect_bar(loaded.series, 0, utc(2014, 6, 6), 88.25, 88.43, 87.51, 87.66, 87484600, "WIKI");
    expect_bar(loaded.series, 1, utc(2014, 6, 9), 88.11, 89.24, 87.21, 89.06, 75415000, "WIKI");
    expect_bar(loaded.series, 2, utc(2014, 6, 10), 90.04, 90.35, 88.94, 89.59, 62777000, "WIKI");
    check::expect(loaded.report.reversed, "newest first is reversed");
    check::expect(!loaded.report.sorted, "reversed, not sorted");
    check::expect_equal(loaded.report.adjusted, std::string("columns"), "adjusted columns");
    check::expect_equal(loaded.report.repaired, std::size_t{0}, "repaired");
    check::expect_equal(loaded.report.duplicates, std::size_t{0}, "duplicates");
}

void unadjusted_columns() {
    const LoadedSeries wiki = load(wiki_csv, unadjusted());
    check::expect_equal(wiki.series.size(), std::size_t{3}, "WIKI bars");
    expect_bar(wiki.series, 0, utc(2014, 6, 6), 649.9, 651.26, 644.47, 645.57, 12497800, "raw WIKI");
    expect_bar(wiki.series, 1, utc(2014, 6, 9), 92.7, 93.88, 91.75, 93.7, 75415000, "raw WIKI");
    expect_bar(wiki.series, 2, utc(2014, 6, 10), 94.73, 95.05, 93.57, 94.25, 62777000, "raw WIKI");
    check::expect_equal(wiki.report.adjusted, std::string(), "raw WIKI is not adjusted");
    check::expect(wiki.report.reversed, "raw WIKI is still reversed");

    const LoadedSeries yahoo = load(yahoo_csv, unadjusted());
    check::expect_equal(yahoo.series.size(), std::size_t{3}, "Yahoo bars");
    expect_bar(yahoo.series, 0, utc(2024, 1, 2), 187.149994, 188.440002, 183.889999, 185.639999, 82488700, "raw Yahoo");
    expect_bar(yahoo.series, 2, utc(2024, 1, 4), 182.149994, 183.089996, 180.880005, 181.910004, 71983600, "raw Yahoo");
    check::expect_equal(yahoo.report.adjusted, std::string(), "raw Yahoo is not adjusted");
}

void nasdaq_export() {
    const LoadedSeries loaded = load(
        "Date,Close/Last,Volume,Open,High,Low\n"
        "01/17/2024,$182.68,47317430,$181.27,$182.93,$180.30\n"
        "01/16/2024,$183.63,65603040,$182.16,$184.26,$180.934\n"
        "01/12/2024,$185.92,40477780,$186.06,$186.74,$185.19\n");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 12), 186.06, 186.74, 185.19, 185.92, 40477780, "Nasdaq");
    expect_bar(loaded.series, 1, utc(2024, 1, 16), 182.16, 184.26, 180.934, 183.63, 65603040, "Nasdaq");
    expect_bar(loaded.series, 2, utc(2024, 1, 17), 181.27, 182.93, 180.30, 182.68, 47317430, "Nasdaq");
    check::expect_equal(loaded.report.date_order, std::string("month first"), "the date order");
    expect_no_warnings(loaded.report, "Nasdaq");
    check::expect(loaded.report.reversed, "newest first is reversed");
    check::expect_equal(loaded.report.adjusted, std::string(), "not adjusted");
    check::expect_equal(loaded.report.repaired, std::size_t{0}, "repaired");
}

// ---- Date order ----

void day_first_dates() {
    const LoadedSeries slashes = load(
        "Date,Open,High,Low,Close,Volume\n"
        "02/01/2024,10,11,9,10.5,100\n"
        "03/01/2024,10.5,12,10,11.5,200\n"
        "15/01/2024,11.5,12.5,11,12,300\n");
    check::expect_equal(slashes.report.date_order, std::string("day first"), "15/01 is day first");
    expect_no_warnings(slashes.report, "day first");
    check::expect_equal(slashes.series.size(), std::size_t{3}, "bars");
    expect_bar(slashes.series, 0, utc(2024, 1, 2), 10, 11, 9, 10.5, 100, "day first");
    expect_bar(slashes.series, 1, utc(2024, 1, 3), 10.5, 12, 10, 11.5, 200, "day first");
    expect_bar(slashes.series, 2, utc(2024, 1, 15), 11.5, 12.5, 11, 12, 300, "day first");

    const LoadedSeries dots = load(
        "Date;Close\n"
        "02.01.2024;10\n"
        "31.01.2024;11\n");
    check::expect_equal(dots.report.date_order, std::string("day first"), "31.01 is day first");
    check::expect(dots.series.size() == std::size_t{2} && dots.series.time[0] == utc(2024, 1, 2) &&
                      dots.series.time[1] == utc(2024, 1, 31),
                  "dotted dates read day first");
}

void ambiguous_dates_warn() {
    const LoadedSeries loaded = load(
        "Date,Close\n"
        "01/02/2024,10\n"
        "01/03/2024,11\n"
        "02/01/2024,12\n");
    check::expect_equal(loaded.report.date_order, std::string("month first"), "ambiguous dates read month first");
    check::expect_equal(loaded.report.warnings.size(), std::size_t{1}, "one warning about the ambiguity");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    check::expect(loaded.series.time[0] == utc(2024, 1, 2) && loaded.series.time[1] == utc(2024, 1, 3) &&
                      loaded.series.time[2] == utc(2024, 2, 1),
                  "01/02/2024 is January 2 and 02/01/2024 is February 1");
}

void mixed_date_orders() {
    check::expect_throws<Error>([] { load("Date,Close\n13/01/2024,10\n01/14/2024,11\n"); },
                                "13/01/2024 and 01/14/2024 in one column");
    check::expect_throws<Error>([] { load("Date,Close\n01/14/2024,10\n15/01/2024,11\n02/02/2024,12\n"); },
                                "01/14/2024 and 15/01/2024 in one column");
}

// ---- CSV dialects ----

void semicolon_and_tab() {
    expect_two_bars(load("Date;Open;High;Low;Close;Volume\n"
                         "2024-01-02;10;11;9;10.5;100\n"
                         "2024-01-03;10.5;12;10;11.5;200\n"),
                    "semicolons");
    expect_two_bars(load("Date\tOpen\tHigh\tLow\tClose\tVolume\n"
                         "2024-01-02\t10\t11\t9\t10.5\t100\n"
                         "2024-01-03\t10.5\t12\t10\t11.5\t200\n"),
                    "tabs");
    // Commas inside tab-separated fields are thousands separators, not delimiters.
    const LoadedSeries tabs = load(
        "Date\tClose\tVolume\n"
        "2024-01-02\t1,234.5\t1,000\n"
        "2024-01-03\t1,240\t2,000\n");
    expect_bar(tabs.series, 0, utc(2024, 1, 2), 1234.5, 1234.5, 1234.5, 1234.5, 1000, "tabs and commas");
    expect_bar(tabs.series, 1, utc(2024, 1, 3), 1240, 1240, 1240, 1240, 2000, "tabs and commas");
}

void quoted_fields() {
    const LoadedSeries commas = load(
        "\"Date\",\"Name\",\"Open\",\"High\",\"Low\",\"Close\",\"Volume\"\n"
        "\"2024-01-02\",\"Acme \"\"Widgets\"\", Inc.\",\"1,234.50\",\"1,240\",\"1,230\",\"1,238.25\",\"1,000,000\"\n"
        "\"2024-01-03\",\"Acme \"\"Widgets\"\", Inc.\",1238.25,1250,1235,1249.5,1200000\n");
    check::expect_equal(commas.series.size(), std::size_t{2}, "bars");
    expect_bar(commas.series, 0, utc(2024, 1, 2), 1234.5, 1240, 1230, 1238.25, 1000000, "quoted commas");
    expect_bar(commas.series, 1, utc(2024, 1, 3), 1238.25, 1250, 1235, 1249.5, 1200000, "quoted commas");
    expect_plain_report(commas.report, 2, "quoted commas");

    expect_two_bars(load("Date;Note;Open;High;Low;Close;Volume\n"
                         "2024-01-02;\"split; 2 for 1\";10;11;9;10.5;100\n"
                         "2024-01-03;\"say \"\"hi\"\"; then \"\"bye\"\"\";10.5;12;10;11.5;200\n"),
                    "quoted semicolons");
}

void byte_order_mark() {
    expect_two_bars(load("\xEF\xBB\xBF"
                         "Date,Open,High,Low,Close,Volume\n"
                         "2024-01-02,10,11,9,10.5,100\n"
                         "2024-01-03,10.5,12,10,11.5,200\n"),
                    "a byte order mark");
    expect_two_bars(load("\xEF\xBB\xBF"
                         "\"Date\",\"Open\",\"High\",\"Low\",\"Close\",\"Volume\"\n"
                         "2024-01-02,10,11,9,10.5,100\n"
                         "2024-01-03,10.5,12,10,11.5,200\n"),
                    "a byte order mark before a quote");
}

void crlf_line_endings() {
    // Quoted last fields: a carriage return left behind would follow the closing quote.
    expect_two_bars(load("Date,Open,High,Low,Close,Volume\r\n"
                         "2024-01-02,10,11,9,10.5,\"100\"\r\n"
                         "2024-01-03,10.5,12,10,11.5,\"200\"\r\n"),
                    "CRLF");
    expect_two_bars(load("Date,Open,High,Low,Close,Volume\r\n"
                         "2024-01-02,10,11,9,10.5,100\r\n"
                         "2024-01-03,10.5,12,10,11.5,200"),
                    "CRLF without a final line ending");
    check::expect_equal(error_of("Date,Close\r\n2024-01-02,10\r\n2024-01-03,x\r\n"),
                        std::string("line 3, close: 'x' is not a number"),
                        "an error in a CRLF file");
}

void blank_lines() {
    expect_two_bars(load("\n"
                         " \t \n"
                         "Date,Open,High,Low,Close,Volume\n"
                         "\n"
                         "2024-01-02,10,11,9,10.5,100\n"
                         "   \n"
                         "\r\n"
                         "2024-01-03,10.5,12,10,11.5,200\n"
                         "\n"
                         "\n"),
                    "blank lines");
    // Line numbers count the blank lines.
    check::expect_equal(error_of("\nDate,Close\n\n2024-01-02,10\n\n2024-01-03,abc\n\n"),
                        std::string("line 6, close: 'abc' is not a number"),
                        "an error after blank lines");
}

// ---- Order, duplicates, and repairs ----

void duplicates_oldest_first() {
    const LoadedSeries loaded = load(
        "Date,Open,High,Low,Close,Volume\n"
        "2024-01-02,10,11,9,10.5,100\n"
        "2024-01-03,10.5,12,10,11.5,200\n"
        "2024-01-03,10.6,12.5,10.1,12,300\n"
        "2024-01-04,12,13,11,12.5,400\n"
        "2024-01-04,12.1,13.1,11.1,12.6,500\n"
        "2024-01-04,12.2,13.2,11.2,12.7,600\n");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 2), 10, 11, 9, 10.5, 100, "oldest first");
    expect_bar(loaded.series, 1, utc(2024, 1, 3), 10.6, 12.5, 10.1, 12, 300, "oldest first, the later row");
    expect_bar(loaded.series, 2, utc(2024, 1, 4), 12.2, 13.2, 11.2, 12.7, 600, "oldest first, the last row");
    check::expect_equal(loaded.report.duplicates, std::size_t{3}, "duplicates");
    check::expect_equal(loaded.report.rows, std::size_t{6}, "rows");
    check::expect(!loaded.report.reversed && !loaded.report.sorted, "in order");
}

void duplicates_newest_first() {
    const LoadedSeries loaded = load(
        "Date,Open,High,Low,Close,Volume\n"
        "2024-01-04,12,13,11,12.5,400\n"
        "2024-01-03,10.5,12,10,11.5,200\n"
        "2024-01-03,10.6,12.5,10.1,12,300\n"
        "2024-01-02,10,11,9,10.5,100\n"
        "2024-01-02,10.1,11.1,9.1,10.6,150\n"
        "2024-01-02,10.2,11.2,9.2,10.7,175\n");
    check::expect_equal(loaded.series.size(), std::size_t{3}, "bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 2), 10.2, 11.2, 9.2, 10.7, 175, "newest first, the last row");
    expect_bar(loaded.series, 1, utc(2024, 1, 3), 10.6, 12.5, 10.1, 12, 300, "newest first, the later row");
    expect_bar(loaded.series, 2, utc(2024, 1, 4), 12, 13, 11, 12.5, 400, "newest first");
    check::expect_equal(loaded.report.duplicates, std::size_t{3}, "duplicates");
    check::expect(loaded.report.reversed, "reversed");
    check::expect(!loaded.report.sorted, "reversed, not sorted");
}

void out_of_order_rows() {
    const LoadedSeries shuffled = load(
        "Date,Close\n"
        "2024-01-03,11\n"
        "2024-01-02,10\n"
        "2024-01-05,13\n"
        "2024-01-04,12\n");
    check::expect(shuffled.report.sorted, "sorted");
    check::expect(!shuffled.report.reversed, "sorted, not reversed");
    check::expect_equal(shuffled.series.size(), std::size_t{4}, "bars");
    for (std::size_t i = 0; i < shuffled.series.size(); ++i) {
        const double price = 10.0 + static_cast<double>(i);
        expect_bar(shuffled.series,
                   i,
                   utc(2024, 1, 2) + static_cast<Timestamp>(i) * nanotrade::seconds_per_day,
                   price,
                   price,
                   price,
                   price,
                   0,
                   "sorted");
    }

    const LoadedSeries repeated = load(
        "Date,Close\n"
        "2024-01-04,12\n"
        "2024-01-02,10\n"
        "2024-01-04,14\n"
        "2024-01-03,11\n");
    check::expect(repeated.report.sorted, "sorted with a duplicate");
    check::expect_equal(repeated.report.duplicates, std::size_t{1}, "the duplicate");
    check::expect_equal(repeated.series.size(), std::size_t{3}, "bars with a duplicate");
    expect_bar(repeated.series, 2, utc(2024, 1, 4), 14, 14, 14, 14, 0, "sorted, the later row");
}

void repaired_bars() {
    const LoadedSeries loaded = load(
        "Date,Open,High,Low,Close,Volume\n"
        "2024-01-02,10,11,9,10.5,100\n"       // fine
        "2024-01-03,10.5,11,10,11.5,200\n"    // the close above the high
        "2024-01-04,11.5,12,11.6,11.8,300\n"  // the open below the low
        "2024-01-05,12,11,13,12.5,400\n"      // the high and low swapped
        "2024-01-08,12.5,13,12,12.8,500\n");  // fine
    check::expect_equal(loaded.report.repaired, std::size_t{3}, "repaired");
    check::expect_equal(loaded.series.size(), std::size_t{5}, "bars");
    expect_bar(loaded.series, 0, utc(2024, 1, 2), 10, 11, 9, 10.5, 100, "a fine bar");
    expect_bar(loaded.series, 1, utc(2024, 1, 3), 10.5, 11.5, 10, 11.5, 200, "the high raised to the close");
    expect_bar(loaded.series, 2, utc(2024, 1, 4), 11.5, 12, 11.5, 11.8, 300, "the low lowered to the open");
    expect_bar(loaded.series, 3, utc(2024, 1, 5), 12, 13, 11, 12.5, 400, "the range widened");
    expect_bar(loaded.series, 4, utc(2024, 1, 8), 12.5, 13, 12, 12.8, 500, "a fine bar");
}

void missing_columns() {
    const LoadedSeries close_only = load("Date,Close\n2024-01-02,10\n2024-01-03,11\n");
    expect_bar(close_only.series, 0, utc(2024, 1, 2), 10, 10, 10, 10, 0, "close only");
    expect_bar(close_only.series, 1, utc(2024, 1, 3), 11, 11, 11, 11, 0, "close only");
    expect_plain_report(close_only.report, 2, "close only");

    const LoadedSeries open_close = load("Date,Open,Close\n2024-01-02,10,10.5\n2024-01-03,11.5,11\n");
    expect_bar(open_close.series, 0, utc(2024, 1, 2), 10, 10.5, 10, 10.5, 0, "open and close");
    expect_bar(open_close.series, 1, utc(2024, 1, 3), 11.5, 11.5, 11, 11, 0, "open and close");
    expect_plain_report(open_close.report, 2, "open and close");

    const LoadedSeries no_volume =
        load("Date,Open,High,Low,Close\n2024-01-02,10,11,9,10.5\n2024-01-03,10.5,12,10,11.5\n");
    expect_bar(no_volume.series, 0, utc(2024, 1, 2), 10, 11, 9, 10.5, 0, "no volume");
    expect_bar(no_volume.series, 1, utc(2024, 1, 3), 10.5, 12, 10, 11.5, 0, "no volume");

    const LoadedSeries no_open =
        load("Date,High,Low,Close,Volume\n2024-01-02,11,9,10.5,100\n2024-01-03,12,10,11.5,200\n");
    expect_bar(no_open.series, 0, utc(2024, 1, 2), 10.5, 11, 9, 10.5, 100, "no open");
    expect_bar(no_open.series, 1, utc(2024, 1, 3), 11.5, 12, 10, 11.5, 200, "no open");

    // Empty fields in columns that are there are filled in the same way.
    const LoadedSeries empty_fields = load(
        "Date,Open,High,Low,Close,Volume\n"
        "2024-01-02,,,,10.5,\n"
        "2024-01-03,11,,,11.5,\n");
    expect_bar(empty_fields.series, 0, utc(2024, 1, 2), 10.5, 10.5, 10.5, 10.5, 0, "empty fields");
    expect_bar(empty_fields.series, 1, utc(2024, 1, 3), 11, 11.5, 11, 11.5, 0, "empty fields");
    expect_plain_report(empty_fields.report, 2, "empty fields");
}

// ---- Intraday ----

void intraday_timestamps() {
    const LoadedSeries minutes = load(
        "Datetime,Open,High,Low,Close,Volume\n"
        "2024-01-02 09:30,10,11,9,10.5,100\n"
        "2024-01-02 09:31,10.5,12,10,11.5,200\n"
        "2024-01-02T09:32:00Z,11.5,12.5,11,12,300\n"
        "2024-01-02T04:33:00-05:00,12,13,11.5,12.5,400\n");
    check::expect(minutes.series.intraday(), "minute bars are intraday");
    check::expect_equal(minutes.series.size(), std::size_t{4}, "bars");
    expect_bar(minutes.series, 0, utc(2024, 1, 2, 9, 30), 10, 11, 9, 10.5, 100, "minutes");
    expect_bar(minutes.series, 1, utc(2024, 1, 2, 9, 31), 10.5, 12, 10, 11.5, 200, "minutes");
    expect_bar(minutes.series, 2, utc(2024, 1, 2, 9, 32), 11.5, 12.5, 11, 12, 300, "minutes, in UTC");
    expect_bar(minutes.series, 3, utc(2024, 1, 2, 9, 33), 12, 13, 11.5, 12.5, 400, "minutes, from New York time");
    expect_plain_report(minutes.report, 4, "minutes");

    const LoadedSeries slashes = load("Date,Close\n13/01/2024 09:30,10\n13/01/2024 16:00,11\n");
    check::expect_equal(slashes.report.date_order, std::string("day first"), "day-first dates with times");
    check::expect(slashes.series.size() == std::size_t{2} && slashes.series.time[0] == utc(2024, 1, 13, 9, 30) &&
                      slashes.series.time[1] == utc(2024, 1, 13, 16, 0),
                  "day-first dates keep their times");
    check::expect(slashes.series.intraday(), "slash dates with times are intraday");

    // Unix time in milliseconds, under Binance's "Open time".
    const LoadedSeries millis = load("Open time,Close\n1704187800000,10\n1704187860000,11\n");
    check::expect(millis.series.size() == std::size_t{2} && millis.series.time[0] == utc(2024, 1, 2, 9, 30) &&
                      millis.series.time[1] == utc(2024, 1, 2, 9, 31),
                  "milliseconds since the epoch");
    check::expect(millis.series.intraday(), "milliseconds are intraday");

    // Any bar off midnight makes a series intraday, before 1970 too.
    BarSeries bars;
    bars.push_back(utc(1962, 1, 2), 10, 11, 9, 10.5, 100);
    bars.push_back(utc(1962, 1, 3), 10.5, 12, 10, 11.5, 200);
    check::expect(!bars.intraday(), "midnights before 1970 are not intraday");
    bars.push_back(utc(1962, 1, 3, 9, 30), 11.5, 12, 11, 11.5, 300);
    check::expect(bars.intraday(), "one bar at 09:30 makes the series intraday");
    check::expect(!BarSeries{}.intraday(), "an empty series is not intraday");
}

// ---- Errors ----

void non_numeric_price() {
    check::expect_equal(error_of("Date,Open,High,Low,Close,Volume\n"
                                 "2024-01-02,10,11,9,10.5,100\n"
                                 "2024-01-03,10,11,9,abc,100\n"),
                        std::string("line 3, close: 'abc' is not a number"),
                        "a word for a close");
    const std::vector<std::string> columns = {"open", "high", "low", "close", "volume"};
    for (std::size_t column = 0; column < columns.size(); ++column) {
        std::vector<std::string> fields = {"2024-01-03", "10", "11", "9", "10.5", "100"};
        fields[column + 1] = "1O";
        std::string row;
        for (const std::string& field : fields) {
            row += (row.empty() ? "" : ",") + field;
        }
        const std::string message = error_of("Date,Open,High,Low,Close,Volume\n2024-01-02,10,11,9,10.5,100\n" + row);
        expect_contains(message, "line 3, " + columns[column] + ": '1O'", "a letter in the " + columns[column]);
        expect_contains(message, "not a number", "a letter in the " + columns[column]);
    }
    expect_contains(error_of("Date,Open,High,Low,Close,Adj Close,Volume\n"
                             "2024-01-02,10,11,9,10.5,10.4,100\n"
                             "2024-01-03,10,11,9,10.5,ten,100\n"),
                    "line 3, adjusted close: 'ten'",
                    "a word for an adjusted close");
}

void non_positive_price() {
    const std::string first_rows = "Date,Open,High,Low,Close,Volume\n2024-01-02,10,11,9,10.5,100\n";
    const std::string zero_open = error_of(first_rows + "2024-01-03,0,11,9,10.5,100\n");
    expect_contains(zero_open, "line 3, open: ", "a zero open");
    expect_contains(zero_open, "positive", "a zero open");
    expect_contains(error_of(first_rows + "2024-01-03,10,-11,9,10.5,100\n"), "line 3, high: ", "a negative high");
    expect_contains(error_of(first_rows + "2024-01-03,10,11,0,10.5,100\n"), "line 3, low: ", "a zero low");
    expect_contains(
        error_of("Date,Open,High,Low,Close,Volume\n2024-01-02,10,11,9,-10.5,100\n2024-01-03,10,11,9,10,1\n"),
        "line 2, close: ",
        "a negative close");
}

// The open, high, and low a file lacks are filled in from the close, but a bad close is still the close's fault.
void non_positive_close_names_the_close() {
    std::string misattributed;
    const auto expect_close = [&](std::string_view text, const std::string& what) {
        const std::string message = error_of(text);
        if (message.rfind("line 3, ", 0) != 0 || message.find("close: ") == std::string::npos) {
            misattributed += (misattributed.empty() ? "" : "; ") + what + ": \"" + message + "\"";
        }
    };
    expect_close("Date,Close\n2024-01-02,10\n2024-01-03,0\n", "no open, high, or low column");
    expect_close("Date,Open,Close\n2024-01-02,10,10.5\n2024-01-03,10,0\n", "no high or low column");
    expect_close(
        "Date,Open,High,Low,Close,Adj Close,Volume\n"
        "2024-01-02,10,11,9,10.5,10.4,100\n"
        "2024-01-03,10,11,9,10.5,0,100\n",
        "an adjusted close of 0");
    check::expect(misattributed.empty(), "the error should name the close: " + misattributed);
}

void bad_date() {
    const std::string february = error_of("Date,Close\n2024-01-02,10\n2024-02-30,11\n");
    expect_contains(february, "line 3, date: '2024-02-30'", "February 30");
    expect_contains(february, "not a date", "February 30");
    expect_contains(error_of("Date,Close\nyesterday,10\n2024-01-03,11\n"), "line 2, date: 'yesterday'", "a word");
    expect_contains(error_of("Date,Close\n01/13/2024,10\n02/30/2024,11\n"),
                    "line 3, date: '02/30/2024'",
                    "a month-first date that doesn't exist");
    expect_contains(error_of("Date,Close\n2024-01-02,10\n,11\n"), "line 3, date: ''", "no date");
}

void missing_date_or_close_column() {
    expect_contains(error_of("Open,High,Low,Close,Volume\n10,11,9,10.5,100\n10.5,12,10,11.5,200\n"),
                    "date column",
                    "no date column");
    expect_contains(error_of("Date,Open,High,Low,Volume\n2024-01-02,10,11,9,100\n2024-01-03,10.5,12,10,200\n"),
                    "close column",
                    "no close column");
}

void empty_file() {
    expect_contains(error_of(""), "empty", "nothing at all");
    expect_contains(error_of("\n\n"), "empty", "line endings");
    expect_contains(error_of(" \r\n\t\n"), "empty", "white space");
    expect_contains(error_of("\xEF\xBB\xBF"), "empty", "a byte order mark");
    expect_contains(error_of("\xEF\xBB\xBF\r\n"), "empty", "a byte order mark and a line ending");
}

void fewer_than_two_bars() {
    check::expect_throws<Error>([] { load("Date,Close\n"); }, "a header alone");
    check::expect_throws<Error>([] { load("Date,Close\n2024-01-02,null\n2024-01-03,null\n"); }, "only null rows");
    expect_contains(error_of("Date,Close\n2024-01-02,10\n"), "two bars", "one bar");
    expect_contains(error_of("Date,Close\n2024-01-02,10\n2024-01-03,null\n"), "two bars", "one bar and a null row");
    expect_contains(error_of("Date,Close\n2024-01-02,10\n2024-01-02,11\n"), "two bars", "two rows of one day");
}

void unclosed_quote() {
    const std::string message = error_of("Date,Close\n2024-01-02,10\n2024-01-03,\"11\n");
    expect_contains(message, "line 3", "an unclosed quote");
    expect_contains(message, "quote", "an unclosed quote");
    expect_contains(error_of("Date,Close\n2024-01-02,10\n2024-01-03,\"11\"\"\n"), "line 3", "a doubled closing quote");
    expect_contains(error_of("\"Date,Close\n2024-01-02,10\n2024-01-03,11\n"),
                    "line 1",
                    "an unclosed quote in the header");
}

std::string data_path(const std::string& file) {
    return std::string(NANOTRADE_DATA_DIR) + "/" + file;
}

void missing_file() {
    const std::string path = data_path("no-such-file.csv");
    try {
        nanotrade::load_csv_file(path, "NONE");
    } catch (const Error& error) {
        expect_contains(error.what(), path, "the error names the path");
        return;
    }
    throw check::Failure("loaded " + path);
}

// ---- BarSeries ----

void bar_rates() {
    // NYSE's 2023 calendar: the weekdays but for ten holidays, 250 sessions.
    const std::vector<Timestamp> holidays = {utc(2023, 1, 2),
                                             utc(2023, 1, 16),
                                             utc(2023, 2, 20),
                                             utc(2023, 4, 7),
                                             utc(2023, 5, 29),
                                             utc(2023, 6, 19),
                                             utc(2023, 7, 4),
                                             utc(2023, 9, 4),
                                             utc(2023, 11, 23),
                                             utc(2023, 12, 25)};
    BarSeries sessions;
    BarSeries calendar_days;
    for (std::int64_t day = nanotrade::days_from_civil(2023, 1, 1); day <= nanotrade::days_from_civil(2024, 12, 31);
         ++day) {
        const Timestamp midnight = day * nanotrade::seconds_per_day;
        calendar_days.push_back(midnight, 10, 11, 9, 10.5, 100);
        const std::int64_t weekday = (day + 4) % 7;  // 1970-01-01 was a Thursday; 0 is Sunday
        if (midnight < utc(2024, 1, 1) && weekday != 0 && weekday != 6 &&
            std::find(holidays.begin(), holidays.end(), midnight) == holidays.end()) {
            sessions.push_back(midnight, 10, 11, 9, 10.5, 100);
        }
    }
    check::expect_equal(sessions.size(), std::size_t{250}, "NYSE sessions in 2023");
    check::expect_near(sessions.bars_per_year(), 252.0, 1.0, "business days");
    check::expect_equal(calendar_days.size(), std::size_t{731}, "days in 2023 and 2024");
    check::expect_near(calendar_days.bars_per_year(), 365.0, 0.5, "calendar days");

    BarSeries weeks;
    for (std::int64_t week = 0; week <= 104; ++week) {
        weeks.push_back(utc(2023, 1, 2) + week * 7 * nanotrade::seconds_per_day, 10, 11, 9, 10.5, 100);
    }
    check::expect_near(weeks.bars_per_year(), 52.0, 0.5, "weeks");

    // Too few bars to measure still give a usable rate.
    BarSeries one;
    check::expect(std::isfinite(one.bars_per_year()) && one.bars_per_year() > 0.0, "no bars");
    one.push_back(utc(2024, 1, 2), 10, 11, 9, 10.5, 100);
    check::expect(std::isfinite(one.bars_per_year()) && one.bars_per_year() > 0.0, "one bar");
}

void lower_and_upper_bounds() {
    BarSeries bars;
    for (const Timestamp t : {utc(2024, 1, 2), utc(2024, 1, 3), utc(2024, 1, 5), utc(2024, 1, 8)}) {
        bars.push_back(t, 10, 11, 9, 10.5, 100);
    }
    check::expect_equal(bars.lower_bound(utc(2024, 1, 1)), std::size_t{0}, "lower bound before the first bar");
    check::expect_equal(bars.lower_bound(utc(2024, 1, 2)), std::size_t{0}, "lower bound at the first bar");
    check::expect_equal(bars.lower_bound(utc(2024, 1, 2, 12)), std::size_t{1}, "lower bound within a day");
    check::expect_equal(bars.lower_bound(utc(2024, 1, 4)), std::size_t{2}, "lower bound in a gap");
    check::expect_equal(bars.lower_bound(utc(2024, 1, 8)), std::size_t{3}, "lower bound at the last bar");
    check::expect_equal(bars.lower_bound(utc(2024, 1, 9)), std::size_t{4}, "lower bound after the last bar");
    check::expect_equal(bars.upper_bound(utc(2024, 1, 1)), std::size_t{0}, "upper bound before the first bar");
    check::expect_equal(bars.upper_bound(utc(2024, 1, 2)), std::size_t{1}, "upper bound at the first bar");
    check::expect_equal(bars.upper_bound(utc(2024, 1, 4)), std::size_t{2}, "upper bound in a gap");
    check::expect_equal(bars.upper_bound(utc(2024, 1, 5)), std::size_t{3}, "upper bound at a bar");
    check::expect_equal(bars.upper_bound(utc(2024, 1, 8)), std::size_t{4}, "upper bound at the last bar");
    check::expect_equal(bars.upper_bound(utc(2024, 1, 9)), std::size_t{4}, "upper bound after the last bar");
    // [lower_bound(from), upper_bound(to)) holds the bars from `from` to `to`, both included.
    check::expect(
        bars.lower_bound(utc(2024, 1, 3)) == std::size_t{1} && bars.upper_bound(utc(2024, 1, 5)) == std::size_t{3},
        "January 3 to 5 is bars 1 and 2");
    check::expect(bars.lower_bound(utc(2024, 1, 4)) == bars.upper_bound(utc(2024, 1, 4)), "a day without a bar");
    const BarSeries none;
    check::expect(
        none.lower_bound(utc(2024, 1, 2)) == std::size_t{0} && none.upper_bound(utc(2024, 1, 2)) == std::size_t{0},
        "no bars");
}

// ---- The bundled data ----

std::string read_text(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    check::expect(input.is_open(), "can't open " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

const Json& member(const Json& object, std::string_view key, Json::Type type) {
    const Json* value = object.find(key);
    check::expect(value != nullptr && value->type() == type, "the manifest's \"" + std::string(key) + "\" is missing");
    return *value;
}

struct Dataset {
    std::string symbol;
    std::string file;
    std::size_t bars{0};
    std::string first;
    std::string last;
};

std::vector<Dataset> read_manifest() {
    const Json manifest = Json::parse(read_text(data_path("manifest.json")));
    std::vector<Dataset> datasets;
    for (const Json& entry : member(manifest, "datasets", Json::Type::Array).items()) {
        Dataset dataset;
        dataset.symbol = member(entry, "symbol", Json::Type::String).as_string();
        dataset.file = member(entry, "file", Json::Type::String).as_string();
        const double bars = member(entry, "bars", Json::Type::Number).as_number();
        check::expect(bars >= 2.0 && bars == std::floor(bars), dataset.symbol + ": a count of bars");
        dataset.bars = static_cast<std::size_t>(bars);
        dataset.first = member(entry, "first", Json::Type::String).as_string();
        dataset.last = member(entry, "last", Json::Type::String).as_string();
        datasets.push_back(std::move(dataset));
    }
    return datasets;
}

void manifest_lists_the_datasets() {
    std::vector<std::string> symbols;
    for (const Dataset& dataset : read_manifest()) {
        check::expect_equal(dataset.file, dataset.symbol + ".csv", dataset.symbol + ": the file");
        symbols.push_back(dataset.symbol);
    }
    check::expect(symbols == std::vector<std::string>{"AAPL", "BA", "CAT", "DELL", "EBAY", "F", "GE"},
                  "the manifest lists AAPL, BA, CAT, DELL, EBAY, F, and GE");
}

void check_dataset(const Dataset& dataset) {
    const LoadedSeries loaded = nanotrade::load_csv_file(data_path(dataset.file), dataset.symbol);
    const BarSeries& bars = loaded.series;
    const LoadReport& report = loaded.report;
    const std::string& name = dataset.symbol;
    check::expect_equal(bars.symbol, name, name + ": the symbol");
    check::expect_equal(bars.size(), dataset.bars, name + ": bars");
    check::expect_equal(nanotrade::format_date(bars.time.front()), dataset.first, name + ": the first date");
    check::expect_equal(nanotrade::format_date(bars.time.back()), dataset.last, name + ": the last date");
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (i > 0 && bars.time[i - 1] >= bars.time[i]) {
            throw check::Failure(name + ": " + nanotrade::format_timestamp(bars.time[i]) + " doesn't follow " +
                                 nanotrade::format_timestamp(bars.time[i - 1]));
        }
        const double o = bars.open[i];
        const double h = bars.high[i];
        const double l = bars.low[i];
        const double c = bars.close[i];
        const double v = bars.volume[i];
        const bool consistent = std::isfinite(o) && std::isfinite(h) && std::isfinite(l) && std::isfinite(c) &&
                                0.0 < l && l <= std::min(o, c) && std::max(o, c) <= h && std::isfinite(v) && v >= 0.0;
        if (!consistent) {
            throw check::Failure(name + ": the bar of " + nanotrade::format_date(bars.time[i]) + " is inconsistent");
        }
    }
    check::expect(!bars.intraday(), name + ": daily bars");
    const std::size_t repaired = name == "BA" ? std::size_t{1} : name == "CAT" ? std::size_t{2} : std::size_t{0};
    check::expect_equal(report.repaired, repaired, name + ": repaired bars");
    check::expect_equal(report.rows, dataset.bars, name + ": rows");
    check::expect_equal(report.skipped, std::size_t{0}, name + ": skipped rows");
    check::expect_equal(report.duplicates, std::size_t{0}, name + ": duplicates");
    check::expect(!report.reversed && !report.sorted, name + ": oldest first, in order");
    check::expect_equal(report.adjusted, std::string(), name + ": no adjusted columns");
    check::expect_equal(report.date_order, std::string(), name + ": ISO dates");
    expect_no_warnings(report, name);
    check::expect_near(bars.bars_per_year(), 252.0, 1.5, name + ": business days per year");
}

// Every dataset is checked, and the failures of all of them reported together.
void bundled_datasets() {
    std::string failures;
    for (const Dataset& dataset : read_manifest()) {
        try {
            check_dataset(dataset);
        } catch (const std::exception& error) {
            failures += (failures.empty() ? "" : "; ") + std::string(error.what());
        }
    }
    check::expect(failures.empty(), failures);
}

}  // namespace

int main() {
    return check::run({
        {"a plain Date,Open,High,Low,Close,Volume file", plain_file},
        {"Yahoo: open, high, low, and close scaled by Adj Close / Close", yahoo_adjusted_close},
        {"Yahoo: null rows are skipped and counted", yahoo_null_rows},
        {"Quandl WIKI: adjusted columns and volume, newest first", quandl_wiki},
        {"adjust = false reads the raw columns", unadjusted_columns},
        {"Nasdaq.com: dollar prices and month-first dates", nasdaq_export},
        {"day-first dates", day_first_dates},
        {"ambiguous slash dates read month first, with a warning", ambiguous_dates_warn},
        {"a column mixing month-first and day-first dates is an error", mixed_date_orders},
        {"semicolon and tab delimiters", semicolon_and_tab},
        {"quoted fields holding the delimiter and doubled quotes", quoted_fields},
        {"a UTF-8 byte order mark", byte_order_mark},
        {"CRLF line endings", crlf_line_endings},
        {"blank lines", blank_lines},
        {"duplicates in an oldest-first file: the later row wins", duplicates_oldest_first},
        {"duplicates in a newest-first file: the later row wins", duplicates_newest_first},
        {"rows out of order are sorted", out_of_order_rows},
        {"high and low widened to contain the open and close", repaired_bars},
        {"missing open, high, low, and volume columns", missing_columns},
        {"intraday timestamps", intraday_timestamps},
        {"a non-numeric price names its line and column", non_numeric_price},
        {"a non-positive price names its line and column", non_positive_price},
        {"a non-positive close is blamed on the close, not on columns filled from it",
         non_positive_close_names_the_close},
        {"a bad date names its line", bad_date},
        {"a missing date or close column", missing_date_or_close_column},
        {"an empty file", empty_file},
        {"fewer than two bars", fewer_than_two_bars},
        {"an unclosed quote names its line", unclosed_quote},
        {"load_csv_file of a missing path", missing_file},
        {"bars per year: business days, calendar days, weeks", bar_rates},
        {"lower_bound and upper_bound", lower_and_upper_bounds},
        {"the manifest lists the seven datasets", manifest_lists_the_datasets},
        {"every bundled dataset matches its manifest", bundled_datasets},
    });
}
