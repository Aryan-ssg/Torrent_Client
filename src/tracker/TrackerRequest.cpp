// =============================================================================
// TrackerRequest.cpp - Building the announce URL
// =============================================================================
// Step 3.1/3.2 of Phase 3:
//   - generatePeerId():   who we say we are
//   - urlEncode():        turn binary bytes into URL-safe "%XX" text
//   - buildAnnounceUrl(): glue the tracker's URL + all params together
// =============================================================================

#include "tracker/TrackerRequest.hpp"

#include <random>  // For std::random_device (like Java's SecureRandom)

// =============================================================================
// generatePeerId()
// =============================================================================
// BitTorrent convention: peer_id is exactly 20 bytes. Most clients use:
//   "-" + 2-char client code + 4-char version  +  12 random characters
// So "-PF0001-" is "PeerFlow version 0001", then we pad with 12 random
// alphanumeric characters to reach 20 bytes total.
//
// The random part matters: the tracker uses the peer_id to tell US apart
// from other clients. Two clients with the same peer_id confuse it.
// =============================================================================
std::string generatePeerId() {
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";

    // std::random_device gives us OS-level random bytes (like /dev/urandom).
    std::random_device rd;

    std::string peerId = "-PF0001-";  // 8 bytes so far
    while (peerId.size() < 20) {
        peerId += alphabet[rd() % alphabet.size()];
    }
    return peerId;  // exactly 20 bytes
}

// =============================================================================
// urlEncode()
// =============================================================================
// A '%' followed by the 2 hex digits of the byte. Only "unreserved" RFC 3986
// characters (A-Za-z0-9-._~) may appear in a URL as themselves.
//
// Why is this needed? info_hash is 20 arbitrary bytes. It will contain
// characters that are illegal in URLs (like '?', '&', '#', spaces, or bytes
// with no printable character at all). Encoding them as %XX makes a URL the
// tracker can parse back into the exact same bytes.
// =============================================================================
static bool isUrlSafe(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string urlEncode(const std::vector<uint8_t>& bytes) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);

    for (uint8_t b : bytes) {
        char c = static_cast<char>(b);
        if (isUrlSafe(c)) {
            out += c;
        } else {
            out += '%';
            out += hex[b >> 4];        // high nibble
            out += hex[b & 0x0F];      // low nibble
        }
    }
    return out;
}

std::string urlEncode(const std::string& text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    return urlEncode(bytes);
}

// =============================================================================
// buildAnnounceUrl()
// =============================================================================
// Example result:
//   https://tracker.opentrackr.org/announce?info_hash=%AA%BB...&peer_id=-PF0001-
//   x7k2p9m4&port=6881&uploaded=0&downloaded=0&left=6203355136&compact=1&event=started
//
// Only traps:
//   1. The announce URL might already have "?param=value" (e.g. a passkey).
//      We must use '&' instead of '?' then, or the added params would be
//      swallowed into the value of the tracker's last parameter.
//   2. Every binary value flows through urlEncode().
// =============================================================================
std::string TrackerRequest::buildAnnounceUrl() const {
    std::string url = announceUrl;

    // If the URL already contains a '?', separate with '&'. Otherwise '?'.
    bool alreadyHasQuery = (announceUrl.find('?') != std::string::npos);
    url += alreadyHasQuery ? '&' : '?';

    // The tracker mandates "announce parameters":
    // info_hash, peer_id, port, uploaded, downloaded, left, compact.
    url += "info_hash=" + urlEncode(infoHash);
    url += "&peer_id=" + urlEncode(peerId);
    url += "&port=" + std::to_string(port);
    url += "&uploaded=" + std::to_string(uploaded);
    url += "&downloaded=" + std::to_string(downloaded);
    url += "&left=" + std::to_string(left);

    if (compact) {
        url += "&compact=1";
    }

    // event is optional: empty means "just another regular announce".
    if (!event.empty()) {
        url += "&event=" + event;
    }

    return url;
}