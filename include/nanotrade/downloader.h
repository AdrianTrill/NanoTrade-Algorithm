#ifndef NANOTRADE_DOWNLOADER_H
#define NANOTRADE_DOWNLOADER_H

#include <cstdint>
#include <string>
#include <vector>

#include "nanotrade/market_data.h"

namespace nanotrade {

class YahooDownloader {
public:
    static std::vector<Bar> downloadHistoricalBars(
        const std::string& symbol,
        std::int64_t start_timestamp,
        std::int64_t end_timestamp,
        const std::string& interval = "1d"
    );
};

}  // namespace nanotrade

#endif
