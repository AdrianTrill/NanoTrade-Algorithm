#ifndef NANOTRADE_API_HPP
#define NANOTRADE_API_HPP

#include "nanotrade/research.hpp"
#include "nanotrade/series.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// NanoTrade's one interface: a JSON request in, a JSON response out. The command line, the HTTP server, and the
// WebAssembly build all call handle(), so the three answer every request with the same bytes. Responses carry no
// timing or platform details; a transport adds those with with_meta().
namespace nanotrade::api {

inline constexpr const char* version = "2.0.0";

// Answers a request such as {"command": "backtest", "data": "AAPL", "strategy": {...}}. Never throws: errors become
// {"ok": false, "error": {"message": ..., "field": ..., "position": ..., "length": ...}}.
std::string handle(std::string_view request, const Progress& progress = {});

// Adds "meta": {"runtime", "ms", "threads"} to a response.
std::string with_meta(std::string response, std::string_view runtime, double milliseconds);

struct Dataset {
    std::string name;    // what requests call it: "AAPL"
    std::string title;   // "Apple"
    std::string source;  // where the bars came from
    std::string notes;
    BarSeries series;
    LoadReport report;
};

// Registers a dataset under its name, replacing one of the same name. Thread-safe.
void register_dataset(Dataset dataset);
std::shared_ptr<const Dataset> find_dataset(const std::string& name);
// Datasets named in requests but not registered are loaded from this directory on first use: NAME.csv, with titles
// and notes from manifest.json when it has them.
void set_data_directory(const std::string& path);
std::vector<std::string> dataset_names();

}  // namespace nanotrade::api

#endif
