#include "nanotrade/downloader.h"

#include <sstream>
#include <stdexcept>

#include "nanotrade/market_data.h"

#if defined(NANOTRADE_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace nanotrade {

#if defined(NANOTRADE_HAVE_CURL)
namespace {

std::size_t writeCallback(void* contents, std::size_t size, std::size_t nmemb, void* user_data) {
    const std::size_t total_size = size * nmemb;
    auto* buffer = static_cast<std::string*>(user_data);
    buffer->append(static_cast<const char*>(contents), total_size);
    return total_size;
}

}  // namespace

std::vector<Bar> YahooDownloader::downloadHistoricalBars(
    const std::string& symbol,
    std::int64_t start_timestamp,
    std::int64_t end_timestamp,
    const std::string& interval
) {
    if (symbol.empty()) {
        throw std::invalid_argument("Ticker symbol must not be empty.");
    }
    if (interval.empty()) {
        throw std::invalid_argument("Download interval must not be empty.");
    }
    if (start_timestamp >= end_timestamp) {
        throw std::invalid_argument("Start timestamp must be earlier than end timestamp.");
    }

    std::ostringstream url;
    url << "https://query1.finance.yahoo.com/v7/finance/download/" << symbol
        << "?period1=" << start_timestamp
        << "&period2=" << end_timestamp
        << "&interval=" << interval
        << "&events=history&includeAdjustedClose=true";

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        throw std::runtime_error("Failed to initialize libcurl.");
    }

    std::string response_buffer;
    curl_easy_setopt(handle, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "NanoTrade-Algorithm/1.0");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_buffer);

    const CURLcode request_code = curl_easy_perform(handle);
    long http_status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(handle);

    if (request_code != CURLE_OK) {
        throw std::runtime_error(std::string("Yahoo download failed: ") + curl_easy_strerror(request_code));
    }
    if (http_status >= 400) {
        throw std::runtime_error("Yahoo download failed with HTTP status " + std::to_string(http_status));
    }

    return MarketDataLoader::loadCsvString(response_buffer);
}

#else

std::vector<Bar> YahooDownloader::downloadHistoricalBars(
    const std::string&,
    std::int64_t,
    std::int64_t,
    const std::string&
) {
    throw std::runtime_error("Yahoo downloader is unavailable because libcurl was not enabled at build time.");
}

#endif

}  // namespace nanotrade
