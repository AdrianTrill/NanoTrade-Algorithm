#include "nanotrade/http.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>

namespace nanotrade::http {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// The platform's sockets, behind a few functions.

#if defined(_WIN32)
using Socket = SOCKET;
const Socket invalid_socket = INVALID_SOCKET;

void initialize_sockets() {
    struct Winsock {
        Winsock() {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        }
        ~Winsock() {
            WSACleanup();
        }
    };
    static const Winsock winsock;
}

void close_socket(Socket socket) {
    closesocket(socket);
}

std::string last_error() {
    return std::system_category().message(WSAGetLastError());
}

bool address_in_use() {
    return WSAGetLastError() == WSAEADDRINUSE;
}

int poll_one(Socket socket, int timeout_ms) {
    WSAPOLLFD entry{};
    entry.fd = socket;
    entry.events = POLLRDNORM;
    return WSAPoll(&entry, 1, timeout_ms);
}

void set_timeouts(Socket socket, std::chrono::milliseconds timeout) {
    const auto value = static_cast<DWORD>(timeout.count());
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}

long send_some(Socket socket, const char* data, std::size_t size) {
    return ::send(socket, data, static_cast<int>(std::min(size, std::size_t{1} << 20U)), 0);
}

long receive_some(Socket socket, char* data, std::size_t size) {
    return ::recv(socket, data, static_cast<int>(size), 0);
}

bool interrupted() {
    return WSAGetLastError() == WSAEINTR;
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;

void initialize_sockets() {}

void close_socket(Socket socket) {
    ::close(socket);
}

std::string last_error() {
    return std::strerror(errno);
}

bool address_in_use() {
    return errno == EADDRINUSE;
}

int poll_one(Socket socket, int timeout_ms) {
    pollfd entry{};
    entry.fd = socket;
    entry.events = POLLIN;
    return ::poll(&entry, 1, timeout_ms);
}

void set_timeouts(Socket socket, std::chrono::milliseconds timeout) {
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

long send_some(Socket socket, const char* data, std::size_t size) {
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;  // a client that hangs up must not kill the server with SIGPIPE
#else
    constexpr int flags = 0;  // macOS: SO_NOSIGPIPE, set with the timeouts
#endif
    return static_cast<long>(::send(socket, data, size, flags));
}

long receive_some(Socket socket, char* data, std::size_t size) {
    return static_cast<long>(::recv(socket, data, size, 0));
}

bool interrupted() {
    return errno == EINTR;
}
#endif

bool send_all(Socket socket, std::string_view data) {
    while (!data.empty()) {
        const long sent = send_some(socket, data.data(), data.size());
        if (sent < 0 && interrupted()) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

// Appends what arrives to `buffer`: false on a closed connection, an error, or a timeout.
bool receive_into(Socket socket, std::string& buffer) {
    char chunk[16384];
    for (;;) {
        const long received = receive_some(socket, chunk, sizeof(chunk));
        if (received < 0 && interrupted()) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        return true;
    }
}

sockaddr_in resolve(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        throw std::runtime_error("can't resolve host " + host);
    }
    sockaddr_in address{};
    std::memcpy(&address, found->ai_addr, sizeof(address));
    freeaddrinfo(found);
    address.sin_port = htons(port);
    return address;
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

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view find_header(const Headers& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

bool is_token(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
}

// Where the header block ends, and how long its terminator is: CRLF CRLF, or bare LF LF from lax clients.
std::pair<std::size_t, std::size_t> header_end(const std::string& buffer) {
    const std::size_t crlf = buffer.find("\r\n\r\n");
    const std::size_t lf = buffer.find("\n\n");
    if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) {
        return {crlf, 4};
    }
    if (lf != std::string::npos) {
        return {lf, 2};
    }
    return {std::string::npos, 0};
}

std::string serialize(const Response& response, bool head) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "X-Content-Type-Options: nosniff\r\n";
    for (const auto& [name, value] : response.headers) {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    if (!head) {
        out << response.body;
    }
    return out.str();
}

Response error_response(int status, std::string_view message) {
    Response response;
    response.status = status;
    response.body = std::string("{\"ok\":false,\"error\":{\"message\":\"") + std::string(message) + "\"}}";
    return response;
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20U) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

std::string_view Request::header(std::string_view lowercase_name) const {
    return find_header(headers, lowercase_name);
}

std::string_view ClientResponse::header(std::string_view lowercase_name) const {
    return find_header(headers, lowercase_name);
}

const char* reason_phrase(int status) noexcept {
    switch (status) {
        case 100:
            return "Continue";
        case 200:
            return "OK";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 413:
            return "Content Too Large";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Status";
    }
}

std::optional<std::string> url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) {
            return std::nullopt;
        }
        const char decoded = static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
        if (decoded == '\0') {
            return std::nullopt;
        }
        out.push_back(decoded);
        i += 2;
    }
    return out;
}

std::optional<Response> serve_static(const std::filesystem::path& root, std::string_view path) {
    namespace fs = std::filesystem;
    if (path.empty() || path.front() != '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string relative(path.substr(1));
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = std::min(relative.find('/', start), relative.size());
        if (relative.substr(start, end - start) == "..") {
            return std::nullopt;
        }
        start = end + 1;
    }
    if (relative.empty() || relative.back() == '/') {
        relative += "index.html";
    }
    std::error_code error;
    fs::path file = root / fs::path(relative);
    if (fs::is_directory(file, error)) {
        file /= "index.html";
    }
    if (!fs::is_regular_file(file, error)) {
        return std::nullopt;
    }
    // A symbolic link inside root may point anywhere; only files that resolve inside it are served.
    const fs::path real_root = fs::weakly_canonical(root, error);
    const fs::path real_file = fs::weakly_canonical(file, error);
    const auto mismatch = std::mismatch(real_root.begin(), real_root.end(), real_file.begin(), real_file.end());
    if (error || mismatch.first != real_root.end()) {
        return std::nullopt;
    }
    std::ifstream input(real_file, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();

    static const std::pair<const char*, const char*> types[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".csv", "text/csv; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".ico", "image/x-icon"},
        {".wasm", "application/wasm"},
        {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"},
        {".md", "text/markdown; charset=utf-8"},
        {".webmanifest", "application/manifest+json"},
    };
    Response response;
    response.content_type = "application/octet-stream";
    const std::string extension = lowercase(real_file.extension().string());
    for (const auto& [suffix, type] : types) {
        if (extension == suffix) {
            response.content_type = type;
        }
    }
    response.body = content.str();
    response.headers.emplace_back("Cache-Control", "no-cache");
    return response;
}

namespace {

// The server whose thread this is, if any: stop() on one of its own threads would wait for itself.
thread_local const void* serving = nullptr;

}  // namespace

struct Server::Impl {
    ServerOptions options;
    Handler handler;
    Socket listener{invalid_socket};
    std::uint16_t port{0};
    std::atomic<bool> stopping{false};
    std::mutex lifecycle;  // serializes start() and stop()
    bool started{false};
    bool stopped{false};
    std::thread acceptor;
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Socket> queue;

    static constexpr std::size_t max_queue = 256;

    void accept_loop() {
        serving = this;
        while (!stopping.load()) {
            if (poll_one(listener, 100) <= 0) {
                continue;
            }
            const Socket client = ::accept(listener, nullptr, nullptr);
            if (client == invalid_socket) {
                continue;
            }
            set_timeouts(client, options.io_timeout);
            std::unique_lock<std::mutex> lock(mutex);
            if (queue.size() >= max_queue) {
                lock.unlock();
                send_all(client, serialize(error_response(503, "the server is busy"), false));
                close_socket(client);
                continue;
            }
            queue.push_back(client);
            lock.unlock();
            ready.notify_one();
        }
    }

    void work() {
        serving = this;
        for (;;) {
            Socket client = invalid_socket;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (queue.empty()) {
                    return;
                }
                client = queue.front();
                queue.pop_front();
            }
            serve_connection(client);
            close_socket(client);
        }
    }

    void respond(Socket client, const Response& response, bool head) {
        send_all(client, serialize(response, head));
    }

    void serve_connection(Socket client) {
        std::string buffer;
        // A response to HEAD has no body, refusals included, so this is known before anything else is parsed.
        const auto head_request = [&] {
            return buffer.compare(0, 5, "HEAD ") == 0;
        };
        auto [end, terminator] = header_end(buffer);
        while (end == std::string::npos) {
            if (buffer.size() > options.max_header_bytes) {
                respond(client, error_response(431, "the request's headers are too large"), head_request());
                return;
            }
            if (!receive_into(client, buffer)) {
                return;  // the client went away or stalled
            }
            std::tie(end, terminator) = header_end(buffer);
        }
        if (end > options.max_header_bytes) {
            respond(client, error_response(431, "the request's headers are too large"), head_request());
            return;
        }
        const bool is_head = head_request();

        Request request;
        std::istringstream head(buffer.substr(0, end));
        std::string line;
        std::getline(head, line);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t first_space = line.find(' ');
        const std::size_t last_space = line.rfind(' ');
        if (first_space == std::string::npos || first_space == last_space) {
            respond(client, error_response(400, "malformed request line"), is_head);
            return;
        }
        request.method = line.substr(0, first_space);
        request.target = line.substr(first_space + 1, last_space - first_space - 1);
        const std::string version = line.substr(last_space + 1);
        if (!is_token(request.method) || request.target.empty() || request.target.front() != '/' ||
            request.target.find(' ') != std::string::npos) {
            respond(client, error_response(400, "malformed request line"), is_head);
            return;
        }
        if (version != "HTTP/1.1" && version != "HTTP/1.0") {
            respond(client,
                    error_response(version.rfind("HTTP/", 0) == 0 ? 505 : 400, "unsupported HTTP version"),
                    is_head);
            return;
        }
        while (std::getline(head, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || !is_token(line.substr(0, colon))) {
                respond(client, error_response(400, "malformed header"), is_head);
                return;
            }
            request.headers.emplace_back(lowercase(line.substr(0, colon)), std::string(trim(line.substr(colon + 1))));
        }

        if (!request.header("transfer-encoding").empty()) {
            respond(client,
                    error_response(501, "chunked request bodies aren't supported; send a Content-Length"),
                    is_head);
            return;
        }
        std::size_t length = 0;
        const std::string_view declared = request.header("content-length");
        if (!declared.empty()) {
            if (declared.size() > 18 ||
                !std::all_of(declared.begin(), declared.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                respond(client, error_response(400, "malformed Content-Length"), is_head);
                return;
            }
            length = static_cast<std::size_t>(std::stoull(std::string(declared)));
            if (length > options.max_body_bytes) {
                respond(client, error_response(413, "the request body is too large"), is_head);
                return;
            }
        }
        if (length > 0 && lowercase(request.header("expect")) == "100-continue") {
            send_all(client, "HTTP/1.1 100 Continue\r\n\r\n");
        }
        request.body = buffer.substr(end + terminator);
        while (request.body.size() < length) {
            if (!receive_into(client, request.body)) {
                return;
            }
        }
        request.body.resize(length);

        const std::size_t question = request.target.find('?');
        const auto path = url_decode(request.target.substr(0, question));
        if (!path) {
            respond(client, error_response(400, "malformed path"), is_head);
            return;
        }
        request.path = *path;
        request.query = question == std::string::npos ? std::string() : request.target.substr(question + 1);

        Response response;
        try {
            response = handler(request);
        } catch (const std::exception& error) {
            response = error_response(500, "internal error: " + json_escape(error.what()));
        } catch (...) {
            response = error_response(500, "internal error");
        }
        respond(client, response, is_head);
    }
};

Server::Server(ServerOptions options, Handler handler) : impl_(std::make_unique<Impl>()) {
    initialize_sockets();
    impl_->options = std::move(options);
    impl_->handler = std::move(handler);
    if (impl_->options.threads == 0) {
        impl_->options.threads = 1;
    }
    const sockaddr_in address = resolve(impl_->options.host, impl_->options.port);
    const Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == invalid_socket) {
        throw std::runtime_error("can't create a socket: " + last_error());
    }
    const int on = 1;
#if defined(_WIN32)
    // On Windows SO_REUSEADDR would let another process take the port over; this makes the port ours alone.
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string reason =
            address_in_use() ? "port " + std::to_string(impl_->options.port) + " is in use" : last_error();
        close_socket(listener);
        throw std::runtime_error("can't listen on " + impl_->options.host + ": " + reason);
    }
    if (::listen(listener, 128) != 0) {
        const std::string reason = last_error();
        close_socket(listener);
        throw std::runtime_error("can't listen on " + impl_->options.host + ": " + reason);
    }
    sockaddr_in bound{};
    socklen_t size = sizeof(bound);
    getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &size);
    impl_->listener = listener;
    impl_->port = ntohs(bound.sin_port);
}

Server::~Server() {
    stop();
}

std::uint16_t Server::port() const noexcept {
    return impl_->port;
}

void Server::start() {
    const std::lock_guard<std::mutex> guard(impl_->lifecycle);
    if (impl_->stopped) {
        throw std::logic_error("a stopped server can't start again");
    }
    if (impl_->started) {
        return;
    }
    impl_->started = true;
    for (std::size_t i = 0; i < impl_->options.threads; ++i) {
        impl_->workers.emplace_back([this] { impl_->work(); });
    }
    impl_->acceptor = std::thread([this] { impl_->accept_loop(); });
}

void Server::stop() {
    if (serving == impl_.get()) {
        throw std::logic_error("stop() can't run on the server's own threads, such as in a handler");
    }
    const std::lock_guard<std::mutex> guard(impl_->lifecycle);
    if (impl_->stopped) {
        return;
    }
    impl_->stopped = true;
    {
        // Under the queue's lock, so a worker can't check the flag and then miss the notification.
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping.store(true);
    }
    impl_->ready.notify_all();
    if (impl_->acceptor.joinable()) {
        impl_->acceptor.join();
    }
    for (std::thread& worker : impl_->workers) {
        worker.join();
    }
    impl_->workers.clear();
    for (const Socket client : impl_->queue) {
        close_socket(client);
    }
    impl_->queue.clear();
    // Closing the listener refuses new connections at once, instead of leaving them to time out.
    close_socket(impl_->listener);
    impl_->listener = invalid_socket;
}

namespace {

Socket connect_to(const std::string& host, std::uint16_t port) {
    initialize_sockets();
    const sockaddr_in address = resolve(host, port);
    const Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == invalid_socket) {
        throw std::runtime_error("can't create a socket: " + last_error());
    }
    set_timeouts(socket, std::chrono::milliseconds(30'000));
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string reason = last_error();
        close_socket(socket);
        throw std::runtime_error("can't connect to " + host + ":" + std::to_string(port) + ": " + reason);
    }
    return socket;
}

}  // namespace

std::string exchange(const std::string& host, std::uint16_t port, std::string_view bytes) {
    const Socket socket = connect_to(host, port);
    send_all(socket, bytes);
    std::string reply;
    while (receive_into(socket, reply)) {
    }
    close_socket(socket);
    return reply;
}

ClientResponse fetch(const std::string& host,
                     std::uint16_t port,
                     const std::string& method,
                     const std::string& target,
                     const std::string& body,
                     const std::string& content_type) {
    std::string request = method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    if (!content_type.empty()) {
        request += "Content-Type: " + content_type + "\r\n";
    }
    if (!body.empty() || method == "POST" || method == "PUT") {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n" + body;
    const std::string reply = exchange(host, port, request);

    ClientResponse response;
    const auto [end, terminator] = header_end(reply);
    if (end == std::string::npos) {
        throw std::runtime_error("incomplete response from " + host + ":" + std::to_string(port));
    }
    std::istringstream head(reply.substr(0, end));
    std::string line;
    std::getline(head, line);
    const std::size_t space = line.find(' ');
    if (line.rfind("HTTP/1.", 0) != 0 || space == std::string::npos) {
        throw std::runtime_error("malformed response status line");
    }
    response.status = std::atoi(line.c_str() + space + 1);
    while (std::getline(head, line)) {
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers.emplace_back(lowercase(line.substr(0, colon)), std::string(trim(line.substr(colon + 1))));
        }
    }
    response.body = reply.substr(end + terminator);
    return response;
}

}  // namespace nanotrade::http
