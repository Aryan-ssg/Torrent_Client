#pragma once

// =============================================================================
// Peer.hpp - A single peer we can talk to
// =============================================================================
//
// The tracker tells us who else is downloading the same torrent.
// Each peer is simply an IP address and a port, e.g. 203.0.113.7:6881.
//
// We keep them in a std::vector<Peer> once Phase 3.7 decodes the
// tracker's "compact peers" binary blob.
// =============================================================================

#include <cstdint>  // For uint16_t (0-65535, the port range)
#include <string>   // For std::string

struct Peer {
    std::string ip;    // Human-readable address, e.g. "203.0.113.7"
    uint16_t port = 0; // 1-65535, e.g. 6881

    // For quick debugging: "203.0.113.7:6881"
    std::string toString() const {
        return ip + ":" + std::to_string(port);
    }
};