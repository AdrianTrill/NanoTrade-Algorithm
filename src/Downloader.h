#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <string>
#include <vector>
#include "CSVReader.h"

class Downloader {
public:
    // Download historical data for a given symbol (e.g. "AAPL", "BTC-USD")
    // from startTime (Unix epoch) to endTime (Unix epoch).
    // interval = "1d", "1wk", "1mo", etc.
    // Returns vector of PriceData (date, open, high, low, close, volume).
    static std::vector<PriceData> downloadYahooData(
            const std::string& symbol,
            long startTime,
            long endTime,
            const std::string& interval = "1d"
    );
};

#endif
