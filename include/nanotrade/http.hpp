#ifndef NANOTRADE_HTTP_HPP
#define NANOTRADE_HTTP_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small HTTP/1.1 server for `nanotrade serve`: one request per connection, Content-Length bodies, a fixed pool of
// worker threads, and nothing but the platform's sockets. It binds to 127.0.0.1 unless told otherwise.
namespace nanotrade::http {

using Headers = std::vector<std::pair<std::string, std::string>>;  // names lowercased

struct Request {
    std::string method;
    std::string target;  // as sent: "/api?x=1"
    std::string path;    // percent-decoded, without the query: "/api"
    std::string query;   // after the '?', not decoded
    Headers headers;
    std::string body;

    std::string_view header(std::string_view lowercase_name) const;
};

struct Response {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;
    Headers headers;  // extra headers, such as Cache-Control
};

using Handler = std::function<Response(const Request&)>;

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};  // 0 picks a free port; port() tells which
    std::size_t threads{4};
    std::size_t max_body_bytes{std::size_t{64} << 20U};
    std::size_t max_header_bytes{std::size_t{64} << 10U};
    std::chrono::milliseconds io_timeout{10'000};
};

class Server {
public:
    // Binds and listens at once; throws std::runtime_error saying why it can't, such as the port being in use.
    Server(ServerOptions options, Handler handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const noexcept;
    void start();  // serves on background threads; throws std::logic_error after stop()
    // Thread-safe and idempotent: returns once every thread has finished and the port is closed. Throws
    // std::logic_error on the server's own threads, such as in a handler, where it would wait for itself.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The file under `root` that a decoded request path names, with index.html for directories. Refuses anything that
// could reach outside root: ".." segments, backslashes, colons, NUL bytes, and symbolic links that lead out.
std::optional<Response> serve_static(const std::filesystem::path& root, std::string_view path);

// Decodes %XX escapes. '+' stays '+', as in a path. Malformed escapes and encoded NUL bytes give nullopt.
std::optional<std::string> url_decode(std::string_view text);

const char* reason_phrase(int status) noexcept;

struct ClientResponse {
    int status{0};
    Headers headers;
    std::string body;

    std::string_view header(std::string_view lowercase_name) const;
};

// A blocking request, for tests and tools. Throws std::runtime_error when the server can't be reached.
ClientResponse fetch(const std::string& host,
                     std::uint16_t port,
                     const std::string& method,
                     const std::string& target,
                     const std::string& body = {},
                     const std::string& content_type = {});

// Sends raw bytes and returns everything the server sends back before closing: for testing malformed requests.
std::string exchange(const std::string& host, std::uint16_t port, std::string_view bytes);

}  // namespace nanotrade::http

#endif
