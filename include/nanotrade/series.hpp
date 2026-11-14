#ifndef NANOTRADE_SERIES_HPP
#define NANOTRADE_SERIES_HPP

#include "nanotrade/time.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nanotrade {

// Bars of one instrument, oldest first, stored by column so indicators run over contiguous arrays.
struct BarSeries {
    std::string symbol;
    std::vector<Timestamp> time;  // strictly increasing
    std::vector<double> open;
    std::vector<double> high;
    std::vector<double> low;
    std::vector<double> close;
    std::vector<double> volume;

    std::size_t size() const noexcept {
        return time.size();
    }
    bool empty() const noexcept {
        return time.empty();
    }
    void reserve(std::size_t bars);
    void push_back(Timestamp t, double o, double h, double l, double c, double v);
    bool intraday() const noexcept;  // any bar not at midnight
    // Bars per year, from the timestamps: about 252 for daily stocks, 365 for daily crypto, 52 for weekly bars.
    double bars_per_year() const noexcept;
    // The first bar at or after `from` and the first after `to`, as a half-open range of indices.
    std::size_t lower_bound(Timestamp from) const noexcept;
    std::size_t upper_bound(Timestamp to) const noexcept;
};

struct LoadReport {
    std::size_t rows{0};                // data rows read
    std::size_t skipped{0};             // rows without prices, such as Yahoo's "null" rows
    std::size_t repaired{0};            // bars whose high and low were widened to contain the open and close
    std::size_t duplicates{0};          // rows repeating a timestamp; the last one is kept
    bool reversed{false};               // the file was newest first
    bool sorted{false};                 // the file was out of order and has been sorted
    std::string adjusted;               // "", "columns" (adjusted OHLC columns) or "close" (scaled by Adj Close)
    std::string date_order;             // "", "month first" or "day first", for dates such as 01/02/2024
    std::vector<std::string> warnings;  // anything else worth knowing
};

struct LoadedSeries {
    BarSeries series;
    LoadReport report;
};

struct CsvOptions {
    bool adjust{true};  // use adjusted prices when the file has them
};

// Reads OHLCV bars from CSV text. Throws nanotrade::Error naming the line and column at fault.
LoadedSeries load_csv(std::string_view text, std::string symbol, const CsvOptions& options = {});
LoadedSeries load_csv_file(const std::string& path, std::string symbol, const CsvOptions& options = {});

}  // namespace nanotrade

#endif
