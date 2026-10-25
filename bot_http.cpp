//
// A small blocking HTTP/1.1 client for the chat model: http or https (OpenSSL, verified), chunked or sized replies.
//
//   Only ever called from the chat worker thread.
//

#include "bots.h"

#include "common/logging.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <array>
#include <charconv>
#include <memory>

namespace bots
{

namespace
{

struct Url
{
    bool        tls{};
    std::string host;
    std::string port;
    std::string path;
};

auto parseUrl(const std::string& text) -> std::optional<Url>
{
    Url        url;
    const auto schemeEnd = text.find("://");
    if (schemeEnd == std::string::npos)
    {
        return std::nullopt;
    }

    const auto scheme = text.substr(0, schemeEnd);
    if (scheme != "http" && scheme != "https")
    {
        return std::nullopt;
    }
    url.tls = scheme == "https";

    const auto hostStart = schemeEnd + 3;
    const auto pathStart = text.find('/', hostStart);
    const auto authority = text.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    url.path             = pathStart == std::string::npos ? "/" : text.substr(pathStart);

    if (const auto colon = authority.rfind(':'); colon != std::string::npos)
    {
        url.host = authority.substr(0, colon);
        url.port = authority.substr(colon + 1);
    }
    else
    {
        url.host = authority;
        url.port = url.tls ? "443" : "80";
    }

    if (url.host.empty())
    {
        return std::nullopt;
    }
    return url;
}

auto connectTo(const Url& url, const int timeoutSeconds) -> int
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* found = nullptr;
    if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &found) != 0)
    {
        return -1;
    }

    int fd = -1;
    for (auto* candidate = found; candidate != nullptr; candidate = candidate->ai_next)
    {
        fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        timeval timeout{ .tv_sec = timeoutSeconds, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
        {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(found);
    return fd;
}

// Plain socket or TLS session behind one read/write pair.
class Connection
{
public:
    ~Connection()
    {
        if (ssl_ != nullptr)
        {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (fd_ >= 0)
        {
            close(fd_);
        }
    }

    auto open(const Url& url, const int timeoutSeconds) -> bool
    {
        fd_ = connectTo(url, timeoutSeconds);
        if (fd_ < 0 || !url.tls)
        {
            return fd_ >= 0;
        }

        static SSL_CTX* context = []
        {
            auto* ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            return ctx;
        }();

        ssl_ = SSL_new(context);
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, url.host.c_str());
        SSL_set1_host(ssl_, url.host.c_str());
        return SSL_connect(ssl_) == 1;
    }

    auto write(const std::string& data) -> bool
    {
        for (std::size_t sent = 0; sent < data.size();)
        {
            const auto n = ssl_ ? SSL_write(ssl_, data.data() + sent, static_cast<int>(data.size() - sent))
                                : static_cast<int>(::write(fd_, data.data() + sent, data.size() - sent));
            if (n <= 0)
            {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    auto readAll() -> std::string
    {
        std::string            out;
        std::array<char, 8192> buffer{};
        while (true)
        {
            const auto n = ssl_ ? SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()))
                                : static_cast<int>(::read(fd_, buffer.data(), buffer.size()));
            if (n <= 0)
            {
                break;
            }
            out.append(buffer.data(), static_cast<std::size_t>(n));
        }
        return out;
    }

private:
    int  fd_{ -1 };
    SSL* ssl_{ nullptr };
};

auto dechunk(const std::string& body) -> std::string
{
    std::string out;
    std::size_t at = 0;
    while (at < body.size())
    {
        const auto lineEnd = body.find("\r\n", at);
        if (lineEnd == std::string::npos)
        {
            break;
        }

        std::size_t size = 0;
        std::from_chars(body.data() + at, body.data() + lineEnd, size, 16);
        if (size == 0)
        {
            break;
        }

        out.append(body, lineEnd + 2, size);
        at = lineEnd + 2 + size + 2;
    }
    return out;
}

} // namespace

auto httpPost(const std::string& urlText, const std::string& apiKey, const std::string& body, const int timeoutSeconds) -> HttpResult
{
    const auto url = parseUrl(urlText);
    if (!url)
    {
        return { 0, "bad LLM_URL" };
    }

    Connection connection;
    if (!connection.open(*url, timeoutSeconds))
    {
        return { 0, fmt::format("could not connect to {}:{}", url->host, url->port) };
    }

    auto request = fmt::format("POST {} HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\nAccept: application/json\r\n"
                               "Content-Length: {}\r\nConnection: close\r\n",
                               url->path, url->host, body.size());
    if (!apiKey.empty())
    {
        request += fmt::format("Authorization: Bearer {}\r\n", apiKey);
    }
    request += "\r\n" + body;

    if (!connection.write(request))
    {
        return { 0, "send failed" };
    }

    const auto response   = connection.readAll();
    const auto headersEnd = response.find("\r\n\r\n");
    if (headersEnd == std::string::npos)
    {
        return { 0, "no response" };
    }

    int status = 0;
    if (const auto space = response.find(' '); space != std::string::npos)
    {
        std::from_chars(response.data() + space + 1, response.data() + headersEnd, status);
    }

    auto       content = response.substr(headersEnd + 4);
    const auto headers = response.substr(0, headersEnd);
    if (headers.find("chunked") != std::string::npos)
    {
        content = dechunk(content);
    }

    return { status, std::move(content) };
}

} // namespace bots
