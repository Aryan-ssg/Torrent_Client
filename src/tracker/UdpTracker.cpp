// =============================================================================
// UdpTracker.cpp - BEP 15 tracker announces over UDP
// =============================================================================

#include "tracker/UdpTracker.hpp"

#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeValue.hpp"
#include "net/NetException.hpp"
#include "tracker/HttpTracker.hpp"  // decodeCompactPeers (shared wire format)

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

// =============================================================================
// Big-endian helpers
// =============================================================================
// "Most significant byte first". The shifts are written out longhand rather
// than memcpy'd onto a uint64_t, because memcpy would produce the NATIVE byte
// order, which on x86 is little-endian and would be exactly wrong here.
void putBE64(uint8_t* out, uint64_t v) {
    out[0] = static_cast<uint8_t>(v >> 56);
    out[1] = static_cast<uint8_t>(v >> 48);
    out[2] = static_cast<uint8_t>(v >> 40);
    out[3] = static_cast<uint8_t>(v >> 32);
    out[4] = static_cast<uint8_t>(v >> 24);
    out[5] = static_cast<uint8_t>(v >> 16);
    out[6] = static_cast<uint8_t>(v >> 8);
    out[7] = static_cast<uint8_t>(v);
}

void putBE32(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

uint64_t getBE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

uint32_t getBE32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | p[i];
    return v;
}

namespace {

// A fresh transaction id. Random rather than a counter: a tracker sees many
// clients, and two of them picking the same id would make it impossible to
// tell whose reply is whose. UDP can also deliver a stale reply from a much
// earlier session, and a random id is what makes that reply recognisable as
// junk instead of being parsed as an answer.
uint32_t randomTransactionId() {
    static thread_local std::mt19937 rng(std::random_device{}());
    return static_cast<uint32_t>(rng());
}

// One datagram socket, bound implicitly by the first sendto(). Reused for
// both the connect and the announce, as the spec requires - the announce must
// go to the same address the connection_id came from.
class UdpSocket {
public:
    ~UdpSocket() {
        if (fd_ >= 0) ::close(fd_);
    }

    void open() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw NetException(std::string("UDP socket: ") + std::strerror(errno));
    }

    // Resolve and remember where we are talking to.
    void resolve(const std::string& host, uint16_t port) {
        struct addrinfo hints {};
        hints.ai_family = AF_INET;  // BEP 15 is IPv4-only
        hints.ai_socktype = SOCK_DGRAM;

        struct addrinfo* results = nullptr;
        const std::string portStr = std::to_string(port);
        int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &results);
        if (rc != 0 || results == nullptr) {
            throw NetException("DNS lookup failed for " + host + ": " + gai_strerror(rc));
        }

        // Copy the ADDRESS BYTE FOR BYTE, never the addrinfo itself.
        // struct addrinfo's ai_addr is a *pointer* into a block that
        // freeaddrinfo() is about to release, so `dest_ = *results;` followed
        // by freeaddrinfo() leaves us sending to freed memory - which is
        // exactly what happened the first time this was written, and why the
        // connect request timed out against a tracker that answered Python in
        // 0.17s.
        std::memcpy(&dest_, results->ai_addr, sizeof(dest_));
        destLen_ = results->ai_addrlen;
        ::freeaddrinfo(results);
    }

    void setTimeout(int seconds) {
        struct timeval tv {};
        tv.tv_sec = seconds;
        // sendto() and recvfrom() honour this, unlike connect() on TCP.
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    // Send one datagram and wait for one reply. Returns false on timeout, so
    // the caller can retry per the spec's backoff.
    bool exchange(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
        if (::sendto(fd_, request.data(), request.size(), 0,
                     reinterpret_cast<sockaddr*>(&dest_), destLen_) < 0) {
            return false;  // e.g. the network is down; treat like a timeout
        }

        // Generous buffer: the reply is small, but a hostile or broken tracker
        // could send a large datagram and we must not overflow anything.
        std::vector<uint8_t> buffer(4096);
        ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), 0, nullptr, nullptr);
        if (n < 0) return false;  // timed out
        reply.assign(buffer.begin(), buffer.begin() + n);
        return true;
    }

private:
    int fd_ = -1;
    struct sockaddr_in dest_ {};  // a copy, so freeaddrinfo() cannot dangle it
    socklen_t destLen_ = 0;
};

// Pull "failure reason" out of a BEP 15 error datagram. The payload is
// bencoded, exactly like an HTTP tracker's response, so we reuse Phase 1.
std::string parseFailureReason(const std::vector<uint8_t>& payload) {
    try {
        std::vector<uint8_t> copy = payload;
        BencodeValue root = BencodeDecoder::decode(copy);
        if (root.getType() != BencodeValue::DICT) return {};
        const auto& dict = root.asDict();
        auto it = dict.find("failure reason");
        if (it != dict.end() && it->second.getType() == BencodeValue::STRING) {
            return it->second.stringValueAsUtf8();
        }
    } catch (const std::exception&) {
        // An error we cannot read is still an error; the caller reports the
        // action code without a message rather than pretending it succeeded.
    }
    return {};
}

}  // namespace

// =============================================================================
// parseUrl()
// =============================================================================
void UdpTracker::parseUrl(const std::string& url, std::string& host, uint16_t& port) {
    if (url.rfind("udp://", 0) != 0) {
        throw std::runtime_error("not a udp:// URL: " + url);
    }
    std::string rest = url.substr(6);

    // Strip any path and query. BEP 15 ignores them ("/announce" is customary
    // but meaningless), but a malformed URL may still carry one.
    size_t slash = rest.find_first_of("/?#");
    if (slash != std::string::npos) rest = rest.substr(0, slash);

    // A bracketed IPv6 literal would need careful handling; BEP 15 is IPv4
    // only, so reject rather than half-support it.
    if (rest.empty()) {
        throw std::runtime_error("udp tracker URL has no host: " + url);
    }

    size_t colon = rest.rfind(':');
    if (colon == std::string::npos) {
        // No port given. The spec's default for UDP trackers is 6969.
        host = rest;
        port = 6969;
        return;
    }

    host = rest.substr(0, colon);
    const std::string portText = rest.substr(colon + 1);
    if (portText.empty()) {
        throw std::runtime_error("udp tracker URL has an empty port: " + url);
    }
    // strtoul rather than stoi: stoi throws std::invalid_argument, which is
    // fine, but it also happily accepts "80abc" and gives 80. Reject trailing
    // junk explicitly - a tracker URL with a typo should fail, not be
    // silently reinterpreted.
    char* end = nullptr;
    unsigned long value = std::strtoul(portText.c_str(), &end, 10);
    if (end == portText.c_str() || *end != '\0' || value == 0 || value > 65535) {
        throw std::runtime_error("udp tracker URL has a bad port \"" + portText + "\": " + url);
    }
    port = static_cast<uint16_t>(value);
}

// =============================================================================
// announce()
// =============================================================================
TrackerResponse UdpTracker::announce(const TrackerRequest& request, int maxSeconds) {
    // Wall-clock budget for this tracker. Every attempt below checks it before
    // sleeping, and clamps its own timeout to whatever is left, so a dead
    // tracker cannot overrun the caller's deadline. See the header for why
    // this matters: BEP 15's 15/30/60s schedule otherwise costs 105s.
    const auto startedAt = std::chrono::steady_clock::now();
    auto remaining = [&]() -> int {
        if (maxSeconds <= 0) return kUdpInitialTimeoutSeconds * 4;  // effectively no cap
        const auto spent = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - startedAt)
                               .count();
        const long long left = static_cast<long long>(maxSeconds) - spent;
        return left > 0 ? static_cast<int>(left) : 0;
    };

    if (request.infoHash.size() != 20) {
        throw std::runtime_error("UDP announce needs a 20-byte info hash");
    }
    if (request.peerId.size() != 20) {
        throw std::runtime_error("UDP announce needs a 20-byte peer id");
    }

    std::string host;
    uint16_t port = 0;
    parseUrl(request.announceUrl, host, port);

    UdpSocket sock;
    sock.open();
    sock.resolve(host, port);

    // ---- Step 1: the connect handshake -----------------------------------
    //
    // Retry on the spec's schedule: 15 * 2^n seconds. The first attempt gets
    // the base timeout, and each subsequent one doubles it.
    uint32_t connectTxn = randomTransactionId();
    std::vector<uint8_t> connectReq(kUdpConnectRequestSize, 0);
    putBE64(connectReq.data() + 0, kUdpTrackerProtocolId);
    putBE32(connectReq.data() + 8, static_cast<uint32_t>(kUdpActionConnect));
    putBE32(connectReq.data() + 12, connectTxn);

    uint64_t connectionId = 0;
    bool connected = false;
    std::string lastError = "no reply";

    for (int attempt = 0; attempt < 3 && !connected; attempt++) {
        const int left = remaining();
        if (left <= 0) {
            lastError = "out of time budget";
            break;
        }
        // Never schedule an attempt longer than the budget we were given.
        const int timeout = std::min(kUdpInitialTimeoutSeconds * (1 << attempt), left);
        sock.setTimeout(timeout);

        std::vector<uint8_t> reply;
        if (!sock.exchange(connectReq, reply)) {
            lastError = "timed out after " + std::to_string(timeout) + "s";
            continue;
        }
        if (reply.size() < kUdpConnectResponseSize) {
            lastError = "short connect reply (" + std::to_string(reply.size()) + " bytes)";
            continue;
        }

        int32_t action = static_cast<int32_t>(getBE32(reply.data()));
        uint32_t txn = getBE32(reply.data() + 4);

        if (action == kUdpActionError) {
            lastError = "tracker error: " + parseFailureReason(
                                              std::vector<uint8_t>(reply.begin() + 8, reply.end()));
            break;  // an explicit error will not fix itself on retry
        }
        if (txn != connectTxn) {
            // A reply to somebody else's request, or a stale one. Dropping it
            // unread is the whole reason transaction ids exist.
            lastError = "transaction id mismatch (stale reply)";
            continue;
        }
        if (action != kUdpActionConnect) {
            lastError = "unexpected action " + std::to_string(action) + " in connect reply";
            continue;
        }

        connectionId = getBE64(reply.data() + 8);
        connected = true;
    }

    if (!connected) {
        throw NetException("UDP connect to " + host + ":" + std::to_string(port) +
                           " failed: " + lastError);
    }

    // ---- Step 2: the announce --------------------------------------------
    std::vector<uint8_t> announceReq(kUdpAnnounceFieldsSize, 0);
    size_t o = 0;
    putBE64(announceReq.data() + o, connectionId);
    o += 8;
    putBE32(announceReq.data() + o, static_cast<uint32_t>(kUdpActionAnnounce));
    o += 4;
    putBE32(announceReq.data() + o, 0);  // transaction id, filled per attempt
    const size_t txnOffset = o;
    o += 4;
    std::memcpy(announceReq.data() + o, request.infoHash.data(), 20);
    o += 20;
    std::memcpy(announceReq.data() + o, request.peerId.data(), 20);
    o += 20;
    putBE64(announceReq.data() + o, static_cast<uint64_t>(request.downloaded));
    o += 8;
    putBE64(announceReq.data() + o, static_cast<uint64_t>(request.left));
    o += 8;
    putBE64(announceReq.data() + o, static_cast<uint64_t>(request.uploaded));
    o += 8;
    // event: 0 = none, 1 = completed, 2 = started, 3 = stopped
    int32_t eventCode = 0;
    if (request.event == "completed") {
        eventCode = 1;
    } else if (request.event == "started") {
        eventCode = 2;
    } else if (request.event == "stopped") {
        eventCode = 3;
    }
    putBE32(announceReq.data() + o, static_cast<uint32_t>(eventCode));
    o += 4;
    putBE32(announceReq.data() + o, 0);  // IP: 0 = let the tracker use our source address
    o += 4;
    putBE32(announceReq.data() + o, 0);  // key: random, used only for BEP 41
    o += 4;
    // num_want. The spec's default is 50, which is often too few to be useful
    // for a download that needs to survive peers disappearing, so ask for more.
    putBE32(announceReq.data() + o, 200);
    o += 4;

    // Sanity: the fields must exactly fill the 96 bytes of real content, so
    // that adding or resizing a field is caught here rather than becoming a
    // subtly wrong announce that a tracker just ignores.
    if (o != kUdpAnnounceFieldsSize) {
        throw NetException("internal error: announce fields are " + std::to_string(o) +
                           " bytes, expected " + std::to_string(kUdpAnnounceFieldsSize));
    }

    // The last two bytes are the trailing padding real trackers expect; see the
    // measurement note in UdpTracker.hpp. Without them the announce is ignored.
    announceReq.resize(kUdpAnnounceRequestSize, 0);

    TrackerResponse response;
    bool announced = false;
    lastError = "no reply";

    for (int attempt = 0; attempt < 3 && !announced; attempt++) {
        const int left = remaining();
        if (left <= 0) {
            lastError = "out of time budget";
            break;
        }
        const int timeout = std::min(kUdpInitialTimeoutSeconds * (1 << attempt), left);
        sock.setTimeout(timeout);

        const uint32_t txn = randomTransactionId();
        putBE32(announceReq.data() + txnOffset, txn);

        std::vector<uint8_t> reply;
        if (!sock.exchange(announceReq, reply)) {
            lastError = "timed out after " + std::to_string(timeout) + "s";
            continue;
        }
        if (reply.size() < kUdpAnnounceResponseHeaderSize) {
            lastError = "short announce reply (" + std::to_string(reply.size()) + " bytes)";
            continue;
        }

        int32_t action = static_cast<int32_t>(getBE32(reply.data()));
        uint32_t gotTxn = getBE32(reply.data() + 4);

        if (action == kUdpActionError) {
            lastError = "tracker error: " +
                        parseFailureReason(
                            std::vector<uint8_t>(reply.begin() + 8, reply.end()));
            break;
        }
        if (gotTxn != txn) {
            lastError = "transaction id mismatch (stale reply)";
            continue;
        }
        if (action != kUdpActionAnnounce) {
            lastError = "unexpected action " + std::to_string(action) + " in announce reply";
            continue;
        }

        response.interval = getBE32(reply.data() + 8);
        response.incomplete = getBE32(reply.data() + 12);
        response.complete = getBE32(reply.data() + 16);

        // The rest of the datagram is the compact peer list: 6 bytes each.
        const size_t peerBytes = reply.size() - kUdpAnnounceResponseHeaderSize;
        if (peerBytes % 6 != 0) {
            lastError = "peer blob is " + std::to_string(peerBytes) +
                        " bytes, not a multiple of 6";
            continue;
        }
        if (peerBytes > 0) {
            decodeCompactPeers(std::string(reply.begin() + kUdpAnnounceResponseHeaderSize,
                                          reply.end()),
                               response.peers);
        }
        announced = true;
    }

    if (!announced) {
        throw NetException("UDP announce to " + host + ":" + std::to_string(port) +
                           " failed: " + lastError);
    }
    return response;
}
