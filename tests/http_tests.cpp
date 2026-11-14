// The HTTP server over real connections on 127.0.0.1: echoed requests, bodies that must come back byte for byte, the
// static files and the paths that must never reach outside the site, every way a request is refused, concurrent
// clients, and stopping. One server with the default limits and one with small limits serve a site built in the
// system's temporary directory, which is removed at the end.

#include "check.hpp"

#include "nanotrade/http.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace http = nanotrade::http;
using namespace std::string_literals;

constexpr const char* host = "127.0.0.1";
constexpr std::size_t strict_body_limit = 1024;
constexpr std::size_t strict_header_limit = 1024;

const std::string index_html = "<!doctype html><title>NanoTrade</title>\n";
const std::string docs_html = "<!doctype html><title>Docs</title>\n";
const std::string outside_html = "<!doctype html><title>outside the root</title>\n";
const std::string secret_text = "the secret\n";
const std::string not_found_body = R"({"ok":false,"error":{"message":"not found"}})";
const std::string boom_message = "kaboom: \"quoted\", back\\slash,\nand a newline";

// ---- Text ----

std::string hex_byte(unsigned char byte) {
    static constexpr char digits[] = "0123456789abcdef";
    return {digits[byte >> 4U], digits[byte & 0x0FU]};
}

// The text with line breaks, control characters, and bytes past ASCII spelled out, for failure messages.
std::string printable(std::string_view text) {
    constexpr std::size_t limit = 240;
    std::string out;
    for (const char c : text.substr(0, limit)) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\r') {
            out += "\\r";
        } else if (c == '\n') {
            out += "\\n";
        } else if (byte < 0x20U || byte > 0x7EU) {
            out += "\\x" + hex_byte(byte);
        } else {
            out += c;
        }
    }
    return text.size() > limit ? out + "..." : out;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20U) {
            out += "\\u00" + hex_byte(byte);
        } else {
            out += c;
        }
    }
    return out + '"';
}

// Whether the text could stand between the quotes of a JSON string.
bool json_string_content(std::string_view text) {
    const auto hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) < 0x20U || text[i] == '"') {
            return false;
        }
        if (text[i] != '\\') {
            continue;
        }
        ++i;
        if (i == text.size()) {
            return false;
        }
        if (text[i] == 'u') {
            if (text.size() - i < 5 || !hex(text[i + 1]) || !hex(text[i + 2]) || !hex(text[i + 3]) ||
                !hex(text[i + 4])) {
                return false;
            }
            i += 4;
        } else if (std::string_view("\"\\/bfnrt").find(text[i]) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

std::string every_byte() {
    std::string bytes;
    for (int value = 0; value < 256; ++value) {
        bytes.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

std::string pseudo_random_bytes(std::size_t size) {
    std::string bytes(size, '\0');
    std::uint32_t state = 0x9E3779B9U;
    for (char& c : bytes) {
        state = state * 1664525U + 1013904223U;
        c = static_cast<char>(state >> 24U);
    }
    return bytes;
}

std::string milliseconds_of(std::chrono::steady_clock::duration elapsed) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms";
}

void expect_same_bytes(const std::string& actual, const std::string& expected, const std::string& context) {
    if (actual == expected) {
        return;
    }
    std::size_t at = 0;
    while (at < actual.size() && at < expected.size() && actual[at] == expected[at]) {
        ++at;
    }
    throw check::Failure(context + " (expected " + std::to_string(expected.size()) + " bytes, got " +
                         std::to_string(actual.size()) + ", first difference at byte " + std::to_string(at) +
                         "; expected \"" + printable(expected) + "\", got \"" + printable(actual) + "\")");
}

// ---- The handler ----

// What the handler answers on /echo, so a test can say exactly what a request should get back.
std::string echo_json(std::string_view method,
                      std::string_view target,
                      std::string_view path,
                      std::string_view query,
                      std::size_t body_size,
                      std::string_view content_type) {
    return "{\"method\":" + json_quote(method) + ",\"target\":" + json_quote(target) + ",\"path\":" + json_quote(path) +
           ",\"query\":" + json_quote(query) + ",\"body_size\":" + std::to_string(body_size) +
           ",\"content_type\":" + json_quote(content_type) + "}";
}

// /echo describes the request, /raw sends its body back, /boom and /boom-int throw, and anything else is a file of
// the site or a 404.
http::Handler site_handler(fs::path site_root) {
    return [root = std::move(site_root)](const http::Request& request) -> http::Response {
        if (request.path == "/boom") {
            throw std::runtime_error(boom_message);
        }
        if (request.path == "/boom-int") {
            throw 42;
        }
        http::Response response;
        if (request.path == "/echo") {
            response.body = echo_json(request.method,
                                      request.target,
                                      request.path,
                                      request.query,
                                      request.body.size(),
                                      request.header("content-type"));
            response.headers.emplace_back("X-Handler", "echo");
            return response;
        }
        if (request.path == "/raw") {
            response.content_type = "application/octet-stream";
            response.body = request.body;
            return response;
        }
        if (auto file = http::serve_static(root, request.path)) {
            return std::move(*file);
        }
        response.status = 404;
        response.body = not_found_body;
        return response;
    };
}

http::Handler ok_handler() {
    return [](const http::Request&) {
        return http::Response{};
    };
}

http::ServerOptions any_port() {
    http::ServerOptions options;
    options.port = 0;
    return options;
}

http::ServerOptions strict_options() {
    http::ServerOptions options = any_port();
    options.threads = 2;
    options.max_body_bytes = strict_body_limit;
    options.max_header_bytes = strict_header_limit;
    options.io_timeout = std::chrono::milliseconds(2'000);
    return options;
}

// ---- The site ----

struct SiteFile {
    const char* name;
    std::string content;
    const char* type;
};

const std::vector<SiteFile>& site_files() {
    static const std::vector<SiteFile> files = {
        {"index.html", index_html, "text/html; charset=utf-8"},
        {"app.js", "console.log('chart');\n", "text/javascript; charset=utf-8"},
        {"style.css", "body { margin: 0; }\n", "text/css; charset=utf-8"},
        {"data.json", "{\"bars\":[1,2,3]}\n", "application/json; charset=utf-8"},
        {"prices.csv", "date,close\n2024-01-02,185.64\n", "text/csv; charset=utf-8"},
        {"engine.wasm", "\0asm\x01\0\0\0"s, "application/wasm"},
        {"logo.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n", "image/svg+xml"},
        {"geist.woff2", "wOF2\0\x01\0\0"s, "font/woff2"},
        {"blob.xyz", every_byte(), "application/octet-stream"},
        {"LOUD.CSS", "p { color: red; }\n", "text/css; charset=utf-8"},
    };
    return files;
}

bool try_write(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return !out.fail();
}

void write_file(const fs::path& path, std::string_view content) {
    if (!try_write(path, content)) {
        throw std::runtime_error("can't write " + path.string());
    }
}

void make_directory(const fs::path& path) {
    if (!fs::create_directory(path)) {
        throw std::runtime_error("can't create " + path.string());
    }
}

bool try_symlink(const fs::path& target, const fs::path& link, bool directory) {
    std::error_code error;
    if (directory) {
        fs::create_directory_symlink(target, link, error);
    } else {
        fs::create_symlink(target, link, error);
    }
    return !error;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device entropy;
        const fs::path parent = fs::temp_directory_path();
        for (int attempt = 0; attempt < 16 && path_.empty(); ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            fs::path candidate =
                parent / ("nanotrade-http-tests-" + std::to_string(stamp) + "-" + std::to_string(entropy()));
            if (fs::create_directory(candidate)) {
                path_ = std::move(candidate);
            }
        }
        if (path_.empty()) {
            throw std::runtime_error("can't create a temporary directory in " + parent.string());
        }
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

// base/site is the root; base/secret and base/outside/ sit beside it, where nothing may reach them. Returns whether
// the platform made the symbolic links.
bool build_site(const fs::path& base, const fs::path& root) {
    make_directory(root);
    for (const SiteFile& file : site_files()) {
        write_file(root / file.name, file.content);
    }
    make_directory(root / "docs");
    write_file(root / "docs" / "index.html", docs_html);
    make_directory(root / "a");
    make_directory(root / "empty");
    write_file(base / "secret", secret_text);
    make_directory(base / "outside");
    write_file(base / "outside" / "index.html", outside_html);
    // A file whose name holds a colon, so refusing one is tested against something that exists. NTFS takes the name
    // as a stream of a file called "colon", which serves the same purpose there.
    try_write(root / "colon:name.txt", "colon\n");
    return try_symlink(base / "secret", root / "link-out", false) &&
           try_symlink(base / "outside", root / "dir-out", true) &&
           try_symlink(root / "index.html", root / "link-in.html", false);
}

class Site {
public:
    Site()
        : root_(directory_.path() / "site"),
          links_(build_site(directory_.path(), root_)),
          server_(any_port(), site_handler(root_)),
          strict_(strict_options(), site_handler(root_)) {
        server_.start();
        strict_.start();
    }

    const fs::path& base() const noexcept {
        return directory_.path();
    }
    const fs::path& root() const noexcept {
        return root_;
    }
    bool links() const noexcept {
        return links_;
    }
    std::uint16_t port() const noexcept {
        return server_.port();
    }
    std::uint16_t strict_port() const noexcept {
        return strict_.port();
    }

private:
    TemporaryDirectory directory_;  // declared first, so it outlives the servers
    fs::path root_;
    bool links_;
    http::Server server_;
    http::Server strict_;
};

// ---- Replies ----

std::size_t header_count(const http::ClientResponse& reply, std::string_view name) {
    std::size_t count = 0;
    for (const auto& header : reply.headers) {
        if (header.first == name) {
            ++count;
        }
    }
    return count;
}

// Every response says how long its body is, that the connection closes after it, and that its type is not to be
// sniffed. A reply to HEAD has no body, and its Content-Length is that of the body a GET would get.
void expect_framing(const http::ClientResponse& reply, const std::string& context, bool head = false) {
    for (const char* name : {"content-length", "connection", "x-content-type-options"}) {
        check::expect_equal(header_count(reply, name), std::size_t{1}, context + ": one " + name + " header");
    }
    check::expect_equal(reply.header("connection"), "close", context + ": Connection");
    check::expect_equal(reply.header("x-content-type-options"), "nosniff", context + ": X-Content-Type-Options");
    const std::string_view length = reply.header("content-length");
    if (head) {
        check::expect(reply.body.empty(),
                      context + ": no body after the headers, got \"" + printable(reply.body) + '"');
        check::expect(!length.empty() && length.find_first_not_of("0123456789") == std::string_view::npos,
                      context + ": a numeric Content-Length, got \"" + printable(length) + '"');
    } else {
        check::expect_equal(length, std::to_string(reply.body.size()), context + ": Content-Length");
    }
}

// Parses a reply that exchange() returned: one status line, headers, a blank line, and the body.
http::ClientResponse parse_reply(std::string_view raw, const std::string& context) {
    const std::size_t end = raw.find("\r\n\r\n");
    check::expect(end != std::string_view::npos, context + ": a complete response, got \"" + printable(raw) + '"');
    const std::string_view head = raw.substr(0, end);
    http::ClientResponse reply;
    reply.body = std::string(raw.substr(end + 4));
    std::size_t start = 0;
    bool first_line = true;
    while (start <= head.size()) {
        const std::size_t stop = std::min(head.find("\r\n", start), head.size());
        const std::string_view line = head.substr(start, stop - start);
        start = stop + 2;
        if (first_line) {
            first_line = false;
            check::expect(line.starts_with("HTTP/1.1 ") && line.size() >= 12,
                          context + ": a status line, got \"" + printable(line) + '"');
            const std::from_chars_result parsed = std::from_chars(line.data() + 9, line.data() + 12, reply.status);
            check::expect(parsed.ec == std::errc() && parsed.ptr == line.data() + 12,
                          context + ": a numeric status, got \"" + printable(line) + '"');
            continue;
        }
        const std::size_t colon = line.find(':');
        check::expect(colon != std::string_view::npos && colon > 0,
                      context + ": a header line, got \"" + printable(line) + '"');
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        reply.headers.emplace_back(lowercase(line.substr(0, colon)), std::string(value));
    }
    return reply;
}

// Sends raw bytes, then checks the status line and the framing of the one response that comes back.
http::ClientResponse send_raw(std::uint16_t port,
                              std::string_view request,
                              const std::string& status_line,
                              const std::string& context,
                              bool head = false) {
    const std::string raw = http::exchange(host, port, request);
    check::expect(raw.starts_with(status_line + "\r\n"),
                  context + ": starts \"" + status_line + "\", got \"" + printable(raw) + '"');
    http::ClientResponse reply = parse_reply(raw, context);
    expect_framing(reply, context, head);
    return reply;
}

// The message in a JSON error body, {"ok":false,"error":{"message":"..."}}, after checking the body is one.
std::string error_message(const http::ClientResponse& reply, const std::string& context) {
    const std::string prefix = R"({"ok":false,"error":{"message":")";
    const std::string suffix = R"("}})";
    const std::string& body = reply.body;
    check::expect_equal(reply.header("content-type"), "application/json", context + ": Content-Type");
    check::expect(body.size() >= prefix.size() + suffix.size() && body.starts_with(prefix) && body.ends_with(suffix),
                  context + ": a JSON error body, got \"" + printable(body) + '"');
    std::string message = body.substr(prefix.size(), body.size() - prefix.size() - suffix.size());
    check::expect(!message.empty() && json_string_content(message),
                  context + ": a well-formed JSON string for the message, got \"" + printable(message) + '"');
    return message;
}

void expect_echo(const http::ClientResponse& reply, const std::string& expected, const std::string& context) {
    check::expect_equal(reply.status, 200, context + ": status");
    check::expect_equal(reply.header("content-type"), "application/json", context + ": Content-Type");
    check::expect_equal(reply.header("x-handler"), "echo", context + ": the handler's own header");
    check::expect_equal(reply.body, expected, context + ": echo");
    expect_framing(reply, context);
}

void expect_file(const std::optional<http::Response>& file,
                 const std::string& content,
                 const std::string& type,
                 const std::string& context) {
    check::expect(file.has_value(), context + " is served");
    check::expect_equal(file->status, 200, context + ": status");
    check::expect_equal(file->content_type, type, context + ": content type");
    expect_same_bytes(file->body, content, context + ": body");
}

void expect_not_found(const http::ClientResponse& reply, const std::string& context) {
    check::expect_equal(reply.status, 404, context + ": status");
    check::expect_equal(reply.body, not_found_body, context + ": the handler's 404, not a file");
    expect_framing(reply, context);
}

// ---- Requests the handler answers ----

void get_echoes_the_request(const Site& site) {
    struct Case {
        const char* target;
        const char* path;
        const char* query;
    };
    for (const Case& c : {
             Case{"/echo?symbol=AAPL&from=2020-01-01&q=a%20b", "/echo", "symbol=AAPL&from=2020-01-01&q=a%20b"},
             Case{"/ec%68o?x=%41+1&y", "/echo", "x=%41+1&y"},
             Case{"/echo?", "/echo", ""},
             Case{"/echo", "/echo", ""},
         }) {
        const http::ClientResponse reply = http::fetch(host, site.port(), "GET", c.target, "", "text/x-probe");
        expect_echo(reply, echo_json("GET", c.target, c.path, c.query, 0, "text/x-probe"), "GET "s + c.target);
    }

    // Header names are matched whatever their case, and values lose the blanks around them.
    const http::ClientResponse mixed_case =
        send_raw(site.port(),
                 "GET /echo?a=1 HTTP/1.1\r\nHost: test\r\nCONTENT-type: \t text/csv \t\r\n\r\n",
                 "HTTP/1.1 200 OK",
                 "a header name in mixed case");
    check::expect_equal(mixed_case.body, echo_json("GET", "/echo?a=1", "/echo", "a=1", 0, "text/csv"), "its echo");
}

void post_bodies_round_trip(const Site& site) {
    const std::vector<std::pair<std::string, std::string>> bodies = {
        {"text", "symbol=AAPL&strategy=sma_cross"},
        {"NUL bytes", "\0leading, \0\0 inner, and trailing\0"s},
        {"only NUL bytes", std::string(64, '\0')},
        {"every byte value", every_byte()},
        {"one byte", "x"},
    };
    for (const auto& [label, body] : bodies) {
        const http::ClientResponse raw =
            http::fetch(host, site.port(), "POST", "/raw", body, "application/octet-stream");
        check::expect_equal(raw.status, 200, label + ": status");
        check::expect_equal(raw.header("content-type"), "application/octet-stream", label + ": Content-Type");
        expect_same_bytes(raw.body, body, label + ": the body comes back");
        expect_framing(raw, label);

        const http::ClientResponse echo = http::fetch(host, site.port(), "POST", "/echo?kind=x", body, "text/x-body");
        expect_echo(echo, echo_json("POST", "/echo?kind=x", "/echo", "kind=x", body.size(), "text/x-body"), label);
    }

    const http::ClientResponse empty = http::fetch(host, site.port(), "POST", "/echo");
    expect_echo(empty, echo_json("POST", "/echo", "/echo", "", 0, ""), "an empty POST");
    const http::ClientResponse put = http::fetch(host, site.port(), "PUT", "/raw", "replaced");
    check::expect_equal(put.body, "replaced"s, "a PUT body comes back");
}

void five_megabyte_body_round_trips(const Site& site) {
    const std::string body = pseudo_random_bytes(std::size_t{5} << 20U);
    const http::ClientResponse raw = http::fetch(host, site.port(), "POST", "/raw", body, "application/octet-stream");
    check::expect_equal(raw.status, 200, "status");
    expect_same_bytes(raw.body, body, "the 5 MB body comes back");
    expect_framing(raw, "5 MB");

    const http::ClientResponse echo = http::fetch(host, site.port(), "POST", "/echo", body);
    expect_echo(echo, echo_json("POST", "/echo", "/echo", "", body.size(), ""), "the handler sees 5 MB");
}

void unknown_path_gets_the_handlers_404(const Site& site) {
    const http::ClientResponse reply = http::fetch(host, site.port(), "GET", "/no/such/thing?x=1");
    expect_not_found(reply, "/no/such/thing");
    check::expect_equal(reply.header("content-type"), "application/json", "the handler's Content-Type");
    send_raw(site.port(), "GET /missing HTTP/1.1\r\nHost: test\r\n\r\n", "HTTP/1.1 404 Not Found", "/missing");
}

void handler_exception_gets_500(const Site& site) {
    const http::ClientResponse boom =
        send_raw(site.port(), "GET /boom HTTP/1.1\r\n\r\n", "HTTP/1.1 500 Internal Server Error", "/boom");
    const std::string message = error_message(boom, "/boom");
    check::expect(message.find("kaboom") != std::string::npos, "the message names the exception: " + message);

    const http::ClientResponse thrown_int = http::fetch(host, site.port(), "GET", "/boom-int");
    check::expect_equal(thrown_int.status, 500, "a thrown int");
    error_message(thrown_int, "a thrown int");
    expect_framing(thrown_int, "a thrown int");

    expect_echo(http::fetch(host, site.port(), "GET", "/echo"),
                echo_json("GET", "/echo", "/echo", "", 0, ""),
                "the server goes on serving");
}

// ---- Static files ----

void static_index_pages(const Site& site) {
    const std::string html = "text/html; charset=utf-8";
    expect_file(http::serve_static(site.root(), "/"), index_html, html, "/");
    expect_file(http::serve_static(site.root(), "/docs"), docs_html, html, "/docs");
    expect_file(http::serve_static(site.root(), "/docs/"), docs_html, html, "/docs/");
    check::expect(!http::serve_static(site.root(), "/empty/").has_value(), "a directory without index.html");
    check::expect(!http::serve_static(site.root(), "/missing.html").has_value(), "a missing file");

    const std::vector<std::pair<std::string, std::string>> pages = {
        {"/", index_html},
        {"/docs", docs_html},
        {"/docs/", docs_html},
    };
    for (const auto& [target, content] : pages) {
        const http::ClientResponse reply = http::fetch(host, site.port(), "GET", target);
        check::expect_equal(reply.status, 200, "GET " + target);
        check::expect_equal(reply.header("content-type"), html, "GET " + target + ": Content-Type");
        expect_same_bytes(reply.body, content, "GET " + target);
        expect_framing(reply, "GET " + target);
    }
}

void static_mime_types(const Site& site) {
    for (const SiteFile& file : site_files()) {
        const std::string target = "/"s + file.name;
        expect_file(http::serve_static(site.root(), target), file.content, file.type, target);

        const http::ClientResponse reply = http::fetch(host, site.port(), "GET", target);
        check::expect_equal(reply.status, 200, "GET " + target);
        check::expect_equal(reply.header("content-type"), file.type, "GET " + target + ": Content-Type");
        expect_same_bytes(reply.body, file.content, "GET " + target);
        expect_framing(reply, "GET " + target);
    }
}

void static_refuses_paths_out_of_the_root(const Site& site) {
    std::vector<std::string> paths = {
        "/../secret",
        "/a/../../secret",
        "/..",
        "/a/..",
        "/docs/../index.html",
        "/..\\secret",
        "/docs\\index.html",
        "/colon:name.txt",
        "/index.html::$DATA",
        "/C:/secret",
        "/index.html\0.txt"s,
        "",
        "index.html",
    };
    // "//" and an absolute path: joined to the root, it would replace it. Skipped when the path has no narrow form.
    try {
        paths.push_back("/" + (site.base() / "secret").generic_string());
    } catch (const std::exception&) {
    }
    for (const std::string& path : paths) {
        check::expect(!http::serve_static(site.root(), path).has_value(), "refuses \"" + printable(path) + '"');
    }

    // Through the server too, where the path is percent-decoded first.
    for (const char* target : {"/../secret",
                               "/a/../../secret",
                               "/%2e%2e/secret",
                               "/a/..%2F..%2Fsecret",
                               "/..%5Csecret",
                               "/..\\secret",
                               "/colon:name.txt"}) {
        expect_not_found(http::fetch(host, site.port(), "GET", target), "GET "s + target);
    }
}

void static_refuses_links_out_of_the_root(const Site& site) {
    if (!site.links()) {
        return;  // the platform won't make symbolic links here, as on Windows without the privilege
    }
    for (const char* path : {"/link-out", "/dir-out", "/dir-out/", "/dir-out/index.html"}) {
        check::expect(!http::serve_static(site.root(), path).has_value(),
                      "refuses "s + path + ", a link to outside the root");
        expect_not_found(http::fetch(host, site.port(), "GET", path), "GET "s + path);
    }
    expect_file(http::serve_static(site.root(), "/link-in.html"),
                index_html,
                "text/html; charset=utf-8",
                "a link that stays inside the root");
}

void url_decoding() {
    const auto decodes = [](std::string_view text, const std::string& expected) {
        const std::optional<std::string> decoded = http::url_decode(text);
        check::expect(decoded.has_value(), "url_decode(\"" + std::string(text) + "\") decodes");
        check::expect_equal(*decoded, expected, "url_decode(\"" + std::string(text) + "\")");
    };
    decodes("%41", "A");
    decodes("%2F", "/");
    decodes("%2f", "/");
    decodes("a+b", "a+b");
    decodes("+", "+");
    decodes("", "");
    decodes("/plain/path", "/plain/path");
    decodes("/a%20b/%41%42c", "/a b/ABc");
    decodes("%e2%82%AC", "\xe2\x82\xac");
    decodes("100%25", "100%");
    decodes("%2541", "%41");

    for (const char* text : {"%ZZ", "%4", "%00", "%", "abc%", "a%0", "%G1", "%1G", "%%41", "/a%00b"}) {
        check::expect(!http::url_decode(text).has_value(), "url_decode(\""s + text + "\") gives nullopt");
    }
}

// ---- Requests the server refuses ----

void oversized_body_gets_413(const Site& site) {
    const std::uint16_t port = site.strict_port();
    const http::ClientResponse at_limit = http::fetch(host, port, "POST", "/echo", std::string(strict_body_limit, 'b'));
    expect_echo(at_limit,
                echo_json("POST", "/echo", "/echo", "", strict_body_limit, ""),
                "a body of exactly max_body_bytes");

    for (const std::size_t size : {strict_body_limit + 1, 4 * strict_body_limit}) {
        const std::string context =
            std::to_string(size) + " bytes over a limit of " + std::to_string(strict_body_limit);
        const http::ClientResponse reply = http::fetch(host, port, "POST", "/echo", std::string(size, 'b'));
        check::expect_equal(reply.status, 413, context);
        error_message(reply, context);
        expect_framing(reply, context);
    }
    send_raw(port,
             "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 1025\r\n\r\n" + std::string(1025, 'b'),
             "HTTP/1.1 413 Content Too Large",
             "the status line of a 413");
}

void malformed_request_line_gets_400(const Site& site) {
    for (const char* request : {"NONSENSE\r\n\r\n",
                                "\r\n\r\n",
                                "GET\r\n\r\n",
                                "GET /echo\r\n\r\n",
                                "GET echo HTTP/1.1\r\n\r\n",
                                "GET /echo HTTP/1.1 extra\r\n\r\n",
                                "G@T /echo HTTP/1.1\r\n\r\n",
                                "GET /echo FTP/1.1\r\n\r\n"}) {
        const std::string context = "request line \"" + printable(request) + '"';
        error_message(send_raw(site.port(), request, "HTTP/1.1 400 Bad Request", context), context);
    }
}

void malformed_headers_lengths_and_paths_get_400(const Site& site) {
    for (const char* request : {"GET /echo HTTP/1.1\r\nNo colon here\r\n\r\n",
                                "GET /echo HTTP/1.1\r\nBad Name: x\r\n\r\n",
                                "POST /echo HTTP/1.1\r\nContent-Length: ten\r\n\r\n",
                                "POST /echo HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                                "POST /echo HTTP/1.1\r\nContent-Length: 1234567890123456789012\r\n\r\n",
                                "GET /a%ZZ HTTP/1.1\r\n\r\n",
                                "GET /a%00b HTTP/1.1\r\n\r\n"}) {
        const std::string context = "request \"" + printable(request) + '"';
        error_message(send_raw(site.port(), request, "HTTP/1.1 400 Bad Request", context), context);
    }
}

void unsupported_version_gets_505(const Site& site) {
    const http::ClientResponse reply = send_raw(site.port(),
                                                "GET /echo HTTP/2.0\r\nHost: test\r\n\r\n",
                                                "HTTP/1.1 505 HTTP Version Not Supported",
                                                "HTTP/2.0");
    error_message(reply, "HTTP/2.0");

    const http::ClientResponse old =
        send_raw(site.port(), "GET /echo?v=1.0 HTTP/1.0\r\n\r\n", "HTTP/1.1 200 OK", "HTTP/1.0 is still served");
    check::expect_equal(old.body, echo_json("GET", "/echo?v=1.0", "/echo", "v=1.0", 0, ""), "HTTP/1.0 echo");
}

void chunked_body_gets_501(const Site& site) {
    const http::ClientResponse reply =
        send_raw(site.port(),
                 "POST /echo HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
                 "HTTP/1.1 501 Not Implemented",
                 "Transfer-Encoding: chunked");
    error_message(reply, "Transfer-Encoding: chunked");
}

void oversized_headers_get_431(const Site& site) {
    const std::uint16_t port = site.strict_port();
    const std::string start = "GET /echo HTTP/1.1\r\nHost: test\r\nX-Padding: ";
    send_raw(port,
             start + std::string(strict_header_limit / 2, 'p') + "\r\n\r\n",
             "HTTP/1.1 200 OK",
             "headers within the limit");

    const std::string status_line = "HTTP/1.1 431 Request Header Fields Too Large";
    const std::string padding(2 * strict_header_limit, 'p');
    error_message(send_raw(port, start + padding + "\r\n\r\n", status_line, "a whole header block over the limit"),
                  "431");
    error_message(send_raw(port, start + padding, status_line, "a header block still arriving past the limit"), "431");

    const http::ClientResponse long_target = http::fetch(host, port, "GET", "/echo?" + padding);
    check::expect_equal(long_target.status, 431, "a request line over the limit");
    expect_framing(long_target, "a request line over the limit");
}

void expect_continue_gets_100_first(const Site& site) {
    const std::string raw = http::exchange(host,
                                           site.port(),
                                           "POST /echo?c=1 HTTP/1.1\r\nHost: test\r\nContent-Type: text/plain\r\n"
                                           "Content-Length: 5\r\nExpect: 100-continue\r\n\r\nhello");
    const std::string interim = "HTTP/1.1 100 Continue\r\n\r\n";
    check::expect(raw.starts_with(interim), "100 Continue comes first, got \"" + printable(raw) + '"');
    const std::string rest = raw.substr(interim.size());
    check::expect(rest.starts_with("HTTP/1.1 200 OK\r\n"), "then the response, got \"" + printable(rest) + '"');
    expect_echo(parse_reply(rest, "after 100 Continue"),
                echo_json("POST", "/echo?c=1", "/echo", "c=1", 5, "text/plain"),
                "after 100 Continue");

    // A body over the limit is refused at once instead of being asked for.
    const http::ClientResponse refused =
        send_raw(site.strict_port(),
                 "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 5000\r\nExpect: 100-continue\r\n\r\n",
                 "HTTP/1.1 413 Content Too Large",
                 "Expect: 100-continue with a body over the limit");
    error_message(refused, "413 instead of 100 Continue");
}

void head_gets_headers_without_a_body(const Site& site) {
    const http::ClientResponse echo = http::fetch(host, site.port(), "HEAD", "/echo?h=1");
    check::expect_equal(echo.status, 200, "HEAD /echo");
    check::expect_equal(echo.header("x-handler"), "echo", "HEAD /echo reaches the handler");
    expect_framing(echo, "HEAD /echo", true);
    check::expect_equal(echo.header("content-length"),
                        std::to_string(echo_json("HEAD", "/echo?h=1", "/echo", "h=1", 0, "").size()),
                        "HEAD /echo: the Content-Length of the body it leaves out");

    const http::ClientResponse file =
        send_raw(site.port(), "HEAD / HTTP/1.1\r\nHost: test\r\n\r\n", "HTTP/1.1 200 OK", "HEAD /", true);
    check::expect_equal(file.header("content-length"), std::to_string(index_html.size()), "HEAD /: Content-Length");
    check::expect_equal(file.header("content-type"), "text/html; charset=utf-8", "HEAD /: Content-Type");

    const http::ClientResponse missing =
        send_raw(site.port(), "HEAD /missing HTTP/1.1\r\n\r\n", "HTTP/1.1 404 Not Found", "HEAD /missing", true);
    check::expect_equal(missing.header("content-length"),
                        std::to_string(not_found_body.size()),
                        "HEAD /missing: Content-Length");

    send_raw(site.port(), "HEAD /boom HTTP/1.1\r\n\r\n", "HTTP/1.1 500 Internal Server Error", "HEAD /boom", true);
}

// A response to HEAD never has a body (RFC 9110, section 9.3.2), not even when the request is refused.
void head_gets_no_body_when_refused(const Site& site) {
    struct Case {
        std::uint16_t port;
        std::string request;
        const char* status_line;
    };
    const std::string padding = "X-Padding: " + std::string(2 * strict_header_limit, 'p');
    const std::string too_large = "HTTP/1.1 431 Request Header Fields Too Large";
    const std::vector<Case> cases = {
        {site.port(), "HEAD /echo\r\n\r\n", "HTTP/1.1 400 Bad Request"},
        {site.port(), "HEAD echo HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request"},
        {site.port(), "HEAD / HTTP/2.0\r\n\r\n", "HTTP/1.1 505 HTTP Version Not Supported"},
        {site.port(), "HEAD /echo HTTP/1.1\r\nNo colon here\r\n\r\n", "HTTP/1.1 400 Bad Request"},
        {site.port(), "HEAD /echo HTTP/1.1\r\nContent-Length: ten\r\n\r\n", "HTTP/1.1 400 Bad Request"},
        {site.port(), "HEAD /a%ZZ HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request"},
        {site.port(),
         "HEAD /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         "HTTP/1.1 501 Not Implemented"},
        {site.strict_port(), "HEAD /echo HTTP/1.1\r\nContent-Length: 5000\r\n\r\n", "HTTP/1.1 413 Content Too Large"},
        {site.strict_port(), "HEAD /echo HTTP/1.1\r\n" + padding + "\r\n\r\n", too_large.c_str()},
        {site.strict_port(), "HEAD /echo HTTP/1.1\r\n" + padding, too_large.c_str()},
    };
    for (const Case& c : cases) {
        send_raw(c.port, c.request, c.status_line, "\"" + printable(c.request) + '"', true);
    }
}

void every_response_is_framed(const Site& site) {
    struct Case {
        std::uint16_t port;
        std::string request;
        const char* status_line;
    };
    const std::vector<Case> cases = {
        {site.port(), "GET /echo HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK"},
        {site.port(), "GET /index.html HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK"},
        {site.port(), "GET /engine.wasm HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK"},
        {site.port(), "POST /raw HTTP/1.1\r\nContent-Length: 3\r\n\r\n\0\0\0"s, "HTTP/1.1 200 OK"},
        {site.port(), "GET /missing HTTP/1.1\r\n\r\n", "HTTP/1.1 404 Not Found"},
        {site.port(), "GET /boom HTTP/1.1\r\n\r\n", "HTTP/1.1 500 Internal Server Error"},
        {site.port(), "GET /boom-int HTTP/1.1\r\n\r\n", "HTTP/1.1 500 Internal Server Error"},
        {site.port(), "BROKEN\r\n\r\n", "HTTP/1.1 400 Bad Request"},
        {site.port(), "GET / HTTP/2.0\r\n\r\n", "HTTP/1.1 505 HTTP Version Not Supported"},
        {site.port(),
         "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         "HTTP/1.1 501 Not Implemented"},
        {site.strict_port(), "POST /echo HTTP/1.1\r\nContent-Length: 2048\r\n\r\n", "HTTP/1.1 413 Content Too Large"},
        {site.strict_port(),
         "GET /echo?" + std::string(2 * strict_header_limit, 'q') + " HTTP/1.1\r\n\r\n",
         "HTTP/1.1 431 Request Header Fields Too Large"},
    };
    for (const Case& c : cases) {
        send_raw(c.port, c.request, c.status_line, "\"" + printable(c.request) + '"');
    }
    send_raw(site.port(), "HEAD /echo HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "HEAD /echo", true);
}

// ---- Threads, stopping, and ports ----

// Nothing listens on the port: a fetch fails, and soon. The bound is generous, since Windows retries a refused
// connection to localhost for about 2 s.
void expect_port_closed(std::uint16_t port, const std::string& context) {
    const auto started = std::chrono::steady_clock::now();
    check::expect_throws<std::runtime_error>([port] { http::fetch(host, port, "GET", "/"); },
                                             context + ": a fetch fails");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check::expect(elapsed < std::chrono::seconds(5), context + ": the refused fetch took " + milliseconds_of(elapsed));
}

// Runs body(0) to body(count - 1) on threads of their own, released together, and joins them all. The body mustn't
// throw.
void run_together(std::size_t count, const std::function<void(std::size_t)>& body) {
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(count);
    const auto release_and_join = [&go, &threads] {
        go.store(true);
        for (std::thread& thread : threads) {
            thread.join();
        }
    };
    try {
        for (std::size_t i = 0; i < count; ++i) {
            threads.emplace_back([&go, &body, i] {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                body(i);
            });
        }
    } catch (...) {
        release_and_join();
        throw;
    }
    release_and_join();
}

void concurrent_clients_get_their_own_echo(const Site& site) {
    constexpr std::size_t clients = 32;
    std::vector<int> statuses(clients, 0);
    std::vector<std::string> bodies(clients);
    std::vector<std::string> errors(clients);
    const auto target = [](std::size_t i) {
        return "/echo?client=" + std::to_string(i);
    };
    const auto body = [](std::size_t i) {
        return std::string(100 * i + 1, static_cast<char>('a' + i % 26));
    };
    const auto type = [](std::size_t i) {
        return "text/x-client-" + std::to_string(i);
    };

    run_together(clients, [&](std::size_t i) {
        try {
            const http::ClientResponse reply = http::fetch(host, site.port(), "POST", target(i), body(i), type(i));
            statuses[i] = reply.status;
            bodies[i] = reply.body;
        } catch (const std::exception& error) {
            errors[i] = error.what();
        }
    });

    for (std::size_t i = 0; i < clients; ++i) {
        const std::string context = "client " + std::to_string(i);
        check::expect(errors[i].empty(), context + ": " + errors[i]);
        check::expect_equal(statuses[i], 200, context + ": status");
        check::expect_equal(
            bodies[i],
            echo_json("POST", target(i), "/echo", "client=" + std::to_string(i), body(i).size(), type(i)),
            context + ": its own echo");
    }
}

void stop_is_prompt_and_idempotent() {
    http::Server server(any_port(), ok_handler());
    server.start();
    check::expect_equal(http::fetch(host, server.port(), "GET", "/").status, 200, "the server serves");

    const auto started = std::chrono::steady_clock::now();
    server.stop();
    const auto first = std::chrono::steady_clock::now() - started;
    server.stop();
    server.stop();
    const auto again = std::chrono::steady_clock::now() - started - first;
    check::expect(first < std::chrono::milliseconds(500), "stop() took " + milliseconds_of(first));
    check::expect(again < std::chrono::milliseconds(250), "stop() again took " + milliseconds_of(again));

    http::Server never_started(any_port(), ok_handler());
    const auto idle_started = std::chrono::steady_clock::now();
    never_started.stop();
    never_started.stop();
    const auto idle = std::chrono::steady_clock::now() - idle_started;
    check::expect(idle < std::chrono::milliseconds(250), "stop() before start() took " + milliseconds_of(idle));
}

void stop_from_many_threads_at_once() {
    http::Server server(any_port(), ok_handler());
    server.start();
    check::expect_equal(http::fetch(host, server.port(), "GET", "/").status, 200, "the server serves");

    constexpr std::size_t stoppers = 8;
    std::vector<std::chrono::steady_clock::duration> took(stoppers);
    std::vector<std::string> errors(stoppers);
    run_together(stoppers, [&](std::size_t i) {
        const auto started = std::chrono::steady_clock::now();
        try {
            server.stop();
        } catch (const std::exception& error) {
            errors[i] = error.what();
        }
        took[i] = std::chrono::steady_clock::now() - started;
    });
    for (std::size_t i = 0; i < stoppers; ++i) {
        const std::string context = "stop() on thread " + std::to_string(i);
        check::expect(errors[i].empty(), context + " threw: " + errors[i]);
        check::expect(took[i] < std::chrono::seconds(1), context + " took " + milliseconds_of(took[i]));
    }
}

void start_after_stop_throws_logic_error() {
    std::chrono::steady_clock::time_point destroying;
    {
        http::Server server(any_port(), ok_handler());
        server.start();
        check::expect_equal(http::fetch(host, server.port(), "GET", "/").status, 200, "the server serves");
        server.stop();
        check::expect_throws<std::logic_error>([&server] { server.start(); }, "start() after stop()");
        check::expect_throws<std::logic_error>([&server] { server.start(); }, "start() after stop(), a second time");
        server.stop();
        destroying = std::chrono::steady_clock::now();
    }
    const auto elapsed = std::chrono::steady_clock::now() - destroying;
    check::expect(elapsed < std::chrono::seconds(1), "destroying it took " + milliseconds_of(elapsed));

    http::Server never_started(any_port(), ok_handler());
    never_started.stop();
    check::expect_throws<std::logic_error>([&never_started] { never_started.start(); },
                                           "start() after stop() on a server that never started");
}

void stop_closes_the_port() {
    http::Server server(any_port(), ok_handler());
    server.start();
    const std::uint16_t port = server.port();
    check::expect_equal(http::fetch(host, port, "GET", "/").status, 200, "the server serves");
    server.stop();
    expect_port_closed(port, "after stop(), with the server still there");
}

void stop_in_a_handler_throws_logic_error() {
    std::atomic<bool> threw{false};
    http::Server other(any_port(), ok_handler());
    other.start();
    http::Server server(any_port(), [&server, &other, &threw](const http::Request& request) {
        http::Response response;
        if (request.path == "/stop") {
            try {
                server.stop();
            } catch (const std::logic_error&) {
                threw.store(true);
                response.body = R"({"stop":"threw std::logic_error"})";
            }
        } else if (request.path == "/stop-uncaught") {
            server.stop();
        } else if (request.path == "/stop-other") {
            other.stop();
        }
        return response;
    });
    server.start();
    const std::uint16_t port = server.port();

    const http::ClientResponse caught = http::fetch(host, port, "GET", "/stop");
    check::expect(threw.load(), "stop() in a handler throws std::logic_error");
    check::expect_equal(caught.status, 200, "the handler that caught it: status");
    check::expect_equal(caught.body, R"({"stop":"threw std::logic_error"})"s, "the handler that caught it says so");
    expect_framing(caught, "the handler that caught it");
    check::expect_equal(http::fetch(host, port, "GET", "/").status, 200, "the server goes on serving");

    const http::ClientResponse uncaught = http::fetch(host, port, "GET", "/stop-uncaught");
    check::expect_equal(uncaught.status, 500, "a handler that leaves the std::logic_error uncaught");
    error_message(uncaught, "a handler that leaves the std::logic_error uncaught");
    expect_framing(uncaught, "a handler that leaves the std::logic_error uncaught");
    check::expect_equal(http::fetch(host, port, "GET", "/").status, 200, "the server goes on serving after the 500");

    // Only a server's own threads are refused: a handler may stop another server.
    check::expect_equal(http::fetch(host, port, "GET", "/stop-other").status, 200, "a handler stops another server");
    check::expect_throws<std::logic_error>([&other] { other.start(); }, "the other server has stopped");

    server.stop();
    expect_port_closed(port, "after stop() from the test's thread");
}

void destructor_stops_a_started_server() {
    std::atomic<bool> entered{false};
    std::uint16_t port = 0;
    http::ClientResponse slow_reply;
    std::string slow_error;
    std::thread client;
    std::chrono::steady_clock::time_point destroying;
    {
        http::Server server(any_port(), [&entered](const http::Request& request) {
            http::Response response;
            if (request.path == "/slow") {
                entered.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                response.body = R"({"slow":true})";
            }
            return response;
        });
        server.start();
        port = server.port();
        check::expect_equal(http::fetch(host, port, "GET", "/").status, 200, "the server serves");

        client = std::thread([&slow_reply, &slow_error, port] {
            try {
                slow_reply = http::fetch(host, port, "GET", "/slow");
            } catch (const std::exception& error) {
                slow_error = error.what();
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        destroying = std::chrono::steady_clock::now();
    }
    const auto elapsed = std::chrono::steady_clock::now() - destroying;
    client.join();

    check::expect(entered.load(), "the slow request reached the handler");
    check::expect(slow_error.empty(), "the request in flight is answered: " + slow_error);
    check::expect_equal(slow_reply.status, 200, "the request in flight: status");
    check::expect_equal(slow_reply.body, R"({"slow":true})"s, "the request in flight: body");
    check::expect(elapsed < std::chrono::seconds(1), "the destructor took " + milliseconds_of(elapsed));
    expect_port_closed(port, "after the destructor");
}

void second_server_on_a_taken_port_throws() {
    http::Server first(any_port(), ok_handler());
    check::expect(first.port() != 0, "port 0 picks a free port, and port() names it");
    http::ServerOptions same;
    same.port = first.port();
    const std::string context = "a second server on port " + std::to_string(same.port);
    check::expect_throws<std::runtime_error>([&same] { http::Server second(same, ok_handler()); }, context);

    std::string message;
    try {
        http::Server second(same, ok_handler());
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    check::expect(message.find("in use") != std::string::npos, context + " says why: " + message);

    first.stop();
    const http::Server reopened(same, ok_handler());
    check::expect_equal(reopened.port(), same.port, "once the first server stops, its port is free again");
}

void reason_phrases() {
    const std::pair<int, std::string_view> known[] = {
        {100, "Continue"},
        {200, "OK"},
        {400, "Bad Request"},
        {404, "Not Found"},
        {413, "Content Too Large"},
        {431, "Request Header Fields Too Large"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {505, "HTTP Version Not Supported"},
    };
    for (const auto& [status, phrase] : known) {
        check::expect_equal(std::string_view(http::reason_phrase(status)),
                            phrase,
                            "reason_phrase(" + std::to_string(status) + ")");
    }
    for (const int status : {-1, 0, 299, 999}) {
        const char* phrase = http::reason_phrase(status);
        check::expect(phrase != nullptr && *phrase != '\0',
                      "reason_phrase(" + std::to_string(status) + ") is still some text");
    }
}

}  // namespace

int main() {
    try {
        const Site site;
        const auto with = [&site](void (*test)(const Site&)) {
            return std::function<void()>([&site, test] { test(site); });
        };
        return check::run({
            {"GET echoes the method, target, path, query, and a header", with(get_echoes_the_request)},
            {"POST bodies round-trip, NUL bytes and all", with(post_bodies_round_trip)},
            {"a 5 MB body round-trips", with(five_megabyte_body_round_trips)},
            {"an unknown path gets the handler's 404", with(unknown_path_gets_the_handlers_404)},
            {"a handler exception gets 500 with a JSON error body", with(handler_exception_gets_500)},
            {"serve_static serves index.html for / and for a directory", with(static_index_pages)},
            {"serve_static picks the MIME type by extension", with(static_mime_types)},
            {"serve_static refuses paths out of the root", with(static_refuses_paths_out_of_the_root)},
            {"serve_static refuses symbolic links out of the root", with(static_refuses_links_out_of_the_root)},
            {"url_decode", url_decoding},
            {"a body over max_body_bytes gets 413", with(oversized_body_gets_413)},
            {"a malformed request line gets 400", with(malformed_request_line_gets_400)},
            {"malformed headers, lengths, and paths get 400", with(malformed_headers_lengths_and_paths_get_400)},
            {"HTTP/2.0 gets 505", with(unsupported_version_gets_505)},
            {"Transfer-Encoding: chunked gets 501", with(chunked_body_gets_501)},
            {"a header block over max_header_bytes gets 431", with(oversized_headers_get_431)},
            {"Expect: 100-continue gets 100 Continue before the response", with(expect_continue_gets_100_first)},
            {"HEAD gets the headers with Content-Length but no body", with(head_gets_headers_without_a_body)},
            {"HEAD gets no body when the request is refused", with(head_gets_no_body_when_refused)},
            {"every response has Content-Length, Connection: close, and nosniff", with(every_response_is_framed)},
            {"32 concurrent clients each get their own echo", with(concurrent_clients_get_their_own_echo)},
            {"stop() is prompt when idle and idempotent", stop_is_prompt_and_idempotent},
            {"stop() from 8 threads at once", stop_from_many_threads_at_once},
            {"start() after stop() throws std::logic_error, then the server is destroyed cleanly",
             start_after_stop_throws_logic_error},
            {"after stop(), a fetch to the port fails at once", stop_closes_the_port},
            {"stop() in a handler throws std::logic_error, and the server goes on",
             stop_in_a_handler_throws_logic_error},
            {"the destructor stops a started server", destructor_stops_a_started_server},
            {"a second server on a taken port throws std::runtime_error until the first stops",
             second_server_on_a_taken_port_throws},
            {"reason_phrase", reason_phrases},
        });
    } catch (const std::exception& error) {
        std::cout << "FAIL  setting up the site and its servers: " << error.what() << '\n';
        return 1;
    }
}
