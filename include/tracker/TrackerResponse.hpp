#pragma once

// =============================================================================
// TrackerResponse.hpp - What the tracker answers back
// =============================================================================
//
// The tracker's reply is itself bencoded (Step 3.6 decodes it with our
// Phase 1 decoder). The fields we care about:
//
//   interval         how many seconds to wait before announcing again
//   complete         how many peers have the whole file (seeders)
//   incomplete       how many peers are still downloading (leechers)
//   peers            the actual peers, as a compact binary blob (IPC+port)
//   failure reason   if present, the announce was rejected (e.g. bad infohash)
// =============================================================================

#include "tracker/Peer.hpp"  // For std::vector<Peer>

#include <cstdint>  // For long long
#include <string>   // For std::string
#include <vector>   // For std::vector

struct TrackerResponse {
    // The peers we can try to connect to in Phase 4.
    std::vector<Peer> peers;

    // How many seconds to wait before re-announcing for fresh peers.
    long long interval = 0;
    long long minInterval = 0;

    // Swarm health (nice to see, not strictly required).
    long long complete = 0;    // seeders
    long long incomplete = 0;  // leechers

    // If non-empty, the tracker rejected us and 'peers' should be ignored.
    std::string failureReason;

    // Optional note from the tracker; never fatal.
    std::string warningMessage;
};