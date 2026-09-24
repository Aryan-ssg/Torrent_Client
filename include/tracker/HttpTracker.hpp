#pragma once

// =============================================================================
// HttpTracker.hpp - Speaks HTTP(S) to a BitTorrent tracker
// =============================================================================
//
// This is the "network" part of Phase 3. Given a TrackerRequest, it:
//   1. builds the announce URL (TrackerRequest::buildAnnounceUrl)
//   2. opens a TCP connection, upgrading to TLS for https:// trackers
//   3. sends a GET request, parses the response, follows redirects
//   4. bencode-decodes the body with our Phase 1 decoder
//   5. unpacks the compact 6-byte peers into readable IP:port pairs
//
// STEP MAP (from the Phase 3 build order):
//   3.3 raw TCP + HTTP GET      -> httpGet()
//   3.4 HTTP response parsing   -> HttpResponse parser in HttpTracker.cpp
//   3.5 HTTPS via OpenSSL       -> upgradeToTls()
//   3.6 TrackerResponse parsing -> announce()
//   3.7 compact peer decoding   -> decodeCompactPeers() in HttpTracker.cpp
// =============================================================================

#include "tracker/TrackerRequest.hpp"
#include "tracker/TrackerResponse.hpp"

#include <string>  // For std::string

class HttpTracker {
public:
    // Send one announce request and get back the parsed peer list.
    // Follows up to `maxRedirects` HTTP redirects (301/302) before giving up.
    static TrackerResponse announce(const TrackerRequest& request, int maxRedirects = 3);
};

// Six bytes encode one IPv4 peer: 4-byte big-endian IP + 2-byte big-endian
// port. Shared here so the decoder is testable on its own.
void decodeCompactPeers(const std::string& blob, std::vector<Peer>& out);