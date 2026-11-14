// The engine's JavaScript interface: the same api::handle() the server and the command line call.

#include "nanotrade/api.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <chrono>
#include <string>
#include <utility>

namespace {

// Answers a request, reporting progress to `progress` when it is a function, and adds the time it took.
std::string handle(const std::string& request, emscripten::val progress) {
    const auto start = std::chrono::steady_clock::now();
    nanotrade::Progress report;
    if (progress.typeOf().as<std::string>() == "function") {
        report = [progress](double done) {
            progress(done);
        };
    }
    std::string response = nanotrade::api::handle(request, report);
    const double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return nanotrade::api::with_meta(std::move(response), "wasm", milliseconds);
}

// Without the timing, so a check can compare it with the native build byte for byte.
std::string handle_exact(const std::string& request) {
    return nanotrade::api::handle(request);
}

std::string version() {
    return nanotrade::api::version;
}

}  // namespace

EMSCRIPTEN_BINDINGS(nanotrade) {
    emscripten::function("handle", &handle);
    emscripten::function("handleExact", &handle_exact);
    emscripten::function("version", &version);
}
