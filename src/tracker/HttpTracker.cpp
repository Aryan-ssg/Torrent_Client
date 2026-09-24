// =============================================================================
// HttpTracker.cpp - Raw sockets + OpenSSL to talk to a tracker
// =============================================================================
//
// This is the "no magic" networking layer. A tracker speaks plain HTTP, so
// Phase 3 shows the whole stack ourselves:
//
//   DNS -> TCP -> (TLS) -> HTTP request -> HTTP response -> bencode -> peers
//
// Nothing here is a library call that hides what happens on the wire.
// =============================================================================

#include "tracker/HttpTracker.hpp"

#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeException.hpp"
#include "net/NetException.hpp"
#include "net/TcpSocket.hpp"

#include <cctype>      // For std::tolower
#include <cstdlib>     // For std::atoi, std::strtoul
#include <cstring>     // For std::strerror
#include <map>         // For std::map
#include <sstream>     // For std::istringstream

#include <arpa/inet.h>  // inet_ntop, in_addr

#include <openssl/err.h>  // ERR_error_string
#include <openssl/ssl.h>  // SSL_*, TLS_client_method

// =============================================================================
// A parsed URL (http:// or https://)
// =============================================================================
// We only need the pieces an HTTP request needs.
// Example: "https://tracker.opentrackr.org:443/announce?x=1"
//   tls=true, host="tracker.opentrackr.org", port="443",
//   pathAndQuery="/announce?x=1"
// =============================================================================
struct Url {
    bool tls = false;
    std::string host;
    std::string port;
    std::string pathAndQuery;
};

static Url parseUrl(const std::string& raw) {
    Url u;
    std::string rest = raw;

    if (rest.rfind("https://", 0) == 0) {
        u.tls = true;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    } else {
        throw BencodeException("Unsupported URL scheme: " + raw);
    }

    // Separate "host:port" from "/path?query".
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.pathAndQuery = (slash == std::string::npos) ? "/" : rest.substr(slash);

    // Drop any "user:pass@" prefix (rare for trackers).
    size_t at = authority.find('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }

    // Last colon separates host and port. Colons inside an IPv6 address are
    // handled by using rfind (the :port is always after the last colon).
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = authority.substr(colon + 1);
        if (u.port.empty()) {
            // "https://host:/" -> use default
            u.port = u.tls ? "443" : "80";
        }
    } else {
        u.host = authority;
        u.port = u.tls ? "443" : "80";
    }

    if (u.host.empty()) {
        throw BencodeException("URL has no host: " + raw);
    }
    return u;
}

// =============================================================================
// Redirect target can be:
//   absolute: "https://other.example/announce"
//   root-relative: "/announce"
//   we just forward location as-is otherwise (rare for trackers)
// =============================================================================
static std::string resolveLocation(const std::string& baseUrl, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    if (location.empty() || location[0] == '/') {
        Url u = parseUrl(baseUrl);
        std::string scheme = u.tls ? "https://" : "http://";
        std::string authority = u.host + ":" + u.port;
        if (location.empty()) return baseUrl;
        return scheme + authority + location;
    }
    throw BencodeException("Unexpected redirect location: " + location);
}

// =============================================================================
// A live connection: a TcpSocket, optionally wrapped in a TLS session.
// The destructor frees the SSL state (TcpSocket closes its own descriptor,
// like Java's try-with-resources).
// =============================================================================
struct Connection {
    TcpSocket socket;
    SSL* ssl = nullptr;

    ~Connection() {
        if (ssl) SSL_free(ssl);
    }

    // Copying would duplicate ownership of the socket + SSL - forbid it.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection() = default;

    // Moving is fine: the socket is usable after a move, so a connected
    // Connection can be handed back from a factory function.
    Connection(Connection&& other) noexcept
        : socket(std::move(other.socket)), ssl(other.ssl) {
        other.ssl = nullptr;
    }

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            if (ssl) SSL_free(ssl);
            socket = std::move(other.socket);
            ssl = other.ssl;
            other.ssl = nullptr;
        }
        return *this;
    }
};

// =============================================================================
// Send ALL of `data`, looping because send() (or SSL_write) may accept only
// part of it.
// =============================================================================
static void sendAll(Connection& c, const std::string& data) {
    if (c.ssl) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = SSL_write(c.ssl, data.data() + sent,
                              static_cast<int>(data.size() - sent));
            if (n <= 0) {
                throw NetException("TLS send failed");
            }
            sent += static_cast<size_t>(n);
        }
    } else {
        c.socket.sendAll(data.data(), data.size());
    }
}

// =============================================================================
// Receive up to `len` bytes. Returns:
//   0  -> the remote side closed the connection cleanly
//  -1  -> error or timeout
//  >0  -> bytes received (may be fewer than asked - that's normal for TCP)
// =============================================================================
static ssize_t recvSome(Connection& c, char* buf, size_t len) {
    if (c.ssl) {
        int n = SSL_read(c.ssl, buf, static_cast<int>(len));
        if (n <= 0) {
            int err = SSL_get_error(c.ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) return 0;  // clean TLS close
            return -1;
        }
        return n;
    }
    return c.socket.recvSome(buf, len);
}

// =============================================================================
// Read until the remote end closes the connection (we sent Connection: close).
// =============================================================================
static std::string readAll(Connection& c) {
    std::string out;
    char buf[8192];
    for (;;) {
        ssize_t n = recvSome(c, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// =============================================================================
// Undo HTTP "chunked" transfer encoding: each chunk is "<hex-size>\r\n<bytes>".
// (Some servers send this even when we asked for Connection: close.)
// =============================================================================
static std::string decodeChunked(const std::string& body) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        // Read the hex length line up to "\r\n".
        size_t cr = body.find("\r\n", i);
        if (cr == std::string::npos) break;
        size_t size = static_cast<size_t>(std::strtoul(body.substr(i, cr - i).c_str(), nullptr, 16));
        i = cr + 2;
        if (size == 0) break;  // last chunk
        if (i + size > body.size()) break;  // malformed
        out.append(body, i, size);
        i += size;
        if (i + 2 > body.size()) break;
        i += 2;  // skip trailing CRLF
    }
    return out;
}

// =============================================================================
// An HTTP response: status code, header map (lowercased keys), raw body.
// =============================================================================
struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

// =============================================================================
// Send the request, read the entire reply, split it into status/headers/body.
// With Connection: close the framing is "read until EOF", which is the
// simplest correct approach for small tracker responses.
// =============================================================================
static HttpResponse performRequest(Connection& c, const std::string& requestText) {
    sendAll(c, requestText);
    std::string raw = readAll(c);
    if (raw.empty()) {
        throw BencodeException("Empty HTTP response (tracker hung up?)");
    }

    // Headers end at the first blank line "\r\n\r\n".
    size_t sep = raw.find("\r\n\r\n");
    std::string headText = raw.substr(0, sep);
    std::string body = (sep == std::string::npos) ? std::string() : raw.substr(sep + 4);

    HttpResponse resp;

    // Status line looks like: "HTTP/1.1 200 OK"
    std::istringstream hs(headText);
    std::string statusLine;
    std::getline(hs, statusLine);
    size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string::npos) {
        throw BencodeException("Malformed HTTP status line: " + statusLine);
    }
    resp.status = std::atoi(statusLine.substr(firstSpace + 1, 3).c_str());

    // Remaining lines are "Name: value" headers.
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // Trim leading whitespace of the value.
        size_t start = value.find_first_not_of(" \t");
        if (start != std::string::npos) value = value.substr(start);
        for (char& ch : key) ch = static_cast<char>(std::tolower(ch));
        resp.headers[key] = value;
    }

    // De-chunk the body if the server used chunked encoding.
    auto te = resp.headers.find("transfer-encoding");
    if (te != resp.headers.end() && te->second.find("chunked") != std::string::npos) {
        resp.body = decodeChunked(body);
    } else {
        resp.body = body;
    }

    return resp;
}

// =============================================================================
// Open a TCP connection to host:port. All the dirty work (DNS, socket(),
// connect(), timeouts) now lives in the shared TcpSocket class. DNS lookup
// details: see src/net/TcpSocket.cpp.
// =============================================================================
static Connection connectTo(const Url& u, int timeoutSeconds) {
    Connection c;
    c.socket.connect(u.host, u.port, timeoutSeconds);
    return c;  // moved out via Connection's move constructor
}

// =============================================================================
// One shared SSL context for all connections (created once, like a Java static
// final field). We load the system's trust store so real certificates verify.
// =============================================================================
static SSL_CTX* sslContext() {
    static SSL_CTX* ctx = [] {
        OPENSSL_init_ssl(0, nullptr);
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(c);  // load /etc/ssl/certs
        return c;
    }();
    return ctx;
}

// =============================================================================
// Wrap the connected socket in a TLS session.
//  - SSL_set_tlsext_host_name : SNI, tells the server WHICH cert to send
//  - SSL_set1_host             : verify the cert CN/SAN matches the name
//  - SSL_VERIFY_PEER           : truly check the certificate chain
// After this, SSL_write/SSL_read replace send()/recv().
// =============================================================================
static void upgradeToTls(Connection& c, const std::string& host) {
    SSL* ssl = SSL_new(sslContext());
    if (!ssl) {
        throw BencodeException("TLS: SSL_new failed");
    }
    SSL_set_fd(ssl, c.socket.fd());
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);

    if (SSL_connect(ssl) != 1) {
        unsigned long err = ERR_get_error();
        std::string msg = err ? ERR_error_string(err, nullptr) : "unknown TLS error";
        SSL_free(ssl);
        throw BencodeException("TLS handshake failed: " + msg);
    }
    c.ssl = ssl;
}

// =============================================================================
// httpGet(): the whole HTTP(S) GET dance. Follows redirects (trackers like
// opentrackr 301 us from http -> https), so we may loop a few times.
// =============================================================================
static HttpResponse httpGet(const std::string& url, int maxRedirects) {
    std::string current = url;
    for (int redirects = 0; redirects <= maxRedirects; redirects++) {
        Url parsed = parseUrl(current);
        Connection c = connectTo(parsed, 15);

        // Host header must match the DNS name, and (non-default) port.
        std::string hostHeader = parsed.host;
        if ((parsed.tls && parsed.port != "443") || (!parsed.tls && parsed.port != "80")) {
            hostHeader += ":" + parsed.port;
        }

        std::string requestText =
            "GET " + parsed.pathAndQuery + " HTTP/1.1\r\n"
            "Host: " + hostHeader + "\r\n"
            "User-Agent: PeerFlow/0.1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n\r\n";

        // TLS handshake MUST happen before sending any bytes.
        if (parsed.tls) {
            upgradeToTls(c, parsed.host);
        }

        HttpResponse resp = performRequest(c, requestText);

        bool isRedirect = (resp.status == 301 || resp.status == 302 ||
                           resp.status == 303 || resp.status == 307);
        if (isRedirect && resp.headers.count("location")) {
            current = resolveLocation(current, resp.headers["location"]);
            continue;  // try again at the new URL
        }
        return resp;
    }
    throw BencodeException("Too many HTTP redirects for " + url);
}

// =============================================================================
// decodeCompactPeers() - The "compact" peer encoding.
//
// With compact=1, "peers" is one long binary string where every 6 bytes are:
//   [4 bytes big-endian IPv4 address][2 bytes big-endian port]
//
// ENDIANNESS: "big-endian" means the most significant byte comes first.
// A port of 6881 = 0x1AE1 is stored as bytes 0x1A 0xE1 (big byte first).
// A little-endian machine would read those two bytes as 0xE11A, so we must
// reassemble them ourselves: (b0 << 8) | b1.
// =============================================================================
void decodeCompactPeers(const std::string& blob, std::vector<Peer>& out) {
    if (blob.size() % 6 != 0) {
        throw BencodeException("Malformed compact peers: size " + std::to_string(blob.size()) + " is not a multiple of 6");
    }

    for (size_t i = 0; i + 6 <= blob.size(); i += 6) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(blob.data() + i);

        // Reassemble the 4 IP bytes big-endian: b0 b1 b2 b3.
        in_addr addr {};
        addr.s_addr = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];

        // Reassemble the 2 port bytes big-endian.
        uint16_t port = static_cast<uint16_t>((b[4] << 8) | b[5]);

        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ipBuf, sizeof(ipBuf));

        out.push_back(Peer{ipBuf, port});
    }
}

// =============================================================================
// announce() - the public entry point of Phase 3.
// Pipeline: URL -> HTTP(S) -> bencode dict -> TrackerResponse.
// =============================================================================
TrackerResponse HttpTracker::announce(const TrackerRequest& request, int maxRedirects) {
    HttpResponse http = httpGet(request.buildAnnounceUrl(), maxRedirects);

    if (http.status != 200) {
        throw BencodeException("Tracker returned HTTP " + std::to_string(http.status));
    }

    // The body is bencoded - reuse our Phase 1 decoder (the info_hash trick
    // that got us here was built on exactly this parser!).
    std::vector<uint8_t> bencoded(http.body.begin(), http.body.end());
    BencodeValue root = BencodeDecoder::decode(bencoded);
    if (root.getType() != BencodeValue::DICT) {
        throw BencodeException("Tracker response is not a bencoded dictionary");
    }
    const auto& dict = root.asDict();

    TrackerResponse result;

    // If the tracker says "failure reason", it's rejecting the announce
    // (most commonly: our info_hash isn't a torrent it knows).
    auto failure = dict.find("failure reason");
    if (failure != dict.end() && failure->second.getType() == BencodeValue::STRING) {
        result.failureReason = failure->second.stringValueAsUtf8();
        return result;
    }

    auto warning = dict.find("warning message");
    if (warning != dict.end() && warning->second.getType() == BencodeValue::STRING) {
        result.warningMessage = warning->second.stringValueAsUtf8();
    }

    auto interval = dict.find("interval");
    if (interval != dict.end() && interval->second.getType() == BencodeValue::INTEGER) {
        result.interval = interval->second.asInteger();
    }
    auto minInterval = dict.find("min interval");
    if (minInterval != dict.end() && minInterval->second.getType() == BencodeValue::INTEGER) {
        result.minInterval = minInterval->second.asInteger();
    }
    auto complete = dict.find("complete");
    if (complete != dict.end() && complete->second.getType() == BencodeValue::INTEGER) {
        result.complete = complete->second.asInteger();
    }
    auto incomplete = dict.find("incomplete");
    if (incomplete != dict.end() && incomplete->second.getType() == BencodeValue::INTEGER) {
        result.incomplete = incomplete->second.asInteger();
    }

    auto peers = dict.find("peers");
    if (peers != dict.end()) {
        if (peers->second.getType() == BencodeValue::STRING) {
            // compact form: one binary blob of 6-byte records
            decodeCompactPeers(peers->second.stringValueAsUtf8(), result.peers);
        } else if (peers->second.getType() == BencodeValue::LIST) {
            // non-compact form (compact=0): a list of dictionaries
            for (const auto& entry : peers->second.asList()) {
                if (entry.getType() != BencodeValue::DICT) continue;
                const auto& pd = entry.asDict();
                auto ipIt = pd.find("ip");
                auto portIt = pd.find("port");
                if (ipIt != pd.end() && ipIt->second.getType() == BencodeValue::STRING &&
                    portIt != pd.end() && portIt->second.getType() == BencodeValue::INTEGER) {
                    result.peers.push_back(Peer{
                        ipIt->second.stringValueAsUtf8(),
                        static_cast<uint16_t>(portIt->second.asInteger())
                    });
                }
            }
        }
    }

    return result;
}