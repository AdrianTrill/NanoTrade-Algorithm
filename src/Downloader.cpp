#include "Downloader.h"
#include "CSVReader.h"
#include <curl/curl.h>
#include <sstream>
#include <iostream>

// This callback is used by libcurl to write the downloaded data into a std::string
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::vector<PriceData> Downloader::downloadYahooData(
        const std::string& symbol,
        long startTime,
        long endTime,
        const std::string& interval
) {
    // Construct Yahoo Finance CSV download URL
    // Example:
    // https://query1.finance.yahoo.com/v7/finance/download/BTC-USD?period1=1640995200&period2=1672531199&interval=1d&events=history&includeAdjustedClose=true
    // period1 = startTime (Unix epoch)
    // period2 = endTime (Unix epoch)

    std::stringstream url;
    url << "https://query1.finance.yahoo.com/v7/finance/download/" << symbol
        << "?period1=" << startTime
        << "&period2=" << endTime
        << "&interval=" << interval
        << "&events=history&includeAdjustedClose=true";

    std::string resultBuffer;
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Error initializing curl.\n";
        return {};
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resultBuffer);

    // Optional: set timeouts, user-agent, etc.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Curl request failed: " << curl_easy_strerror(res) << "\n";
    }
    curl_easy_cleanup(curl);

    // Now parse the CSV data from resultBuffer
    // We'll reuse our CSVReader but add a function that reads from a string instead of a file
    CSVReader reader;
    return reader.readFromString(resultBuffer);
}
