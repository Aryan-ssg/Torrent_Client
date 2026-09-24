#pragma once

// =============================================================================
// PeerHandshake.hpp - The first bytes we exchange with a peer
// =============================================================================
//
// Before any data flows, both sides must agree they are talking about the
// SAME torrent. Every BitTorrent connection starts with exactly this:
//
//    1 byte   : the number 19 (length of the protocol name)
//   19 bytes  : "BitTorrent protocol"
//    8 bytes  : reserved (zeros = no extensions; Phase 9 flips bits here)
//   20 bytes  : info_hash  (the torrent we want - Phase 2)
//   20 bytes  : peer_id    (the identity of the peer we're contacting)
//   --------
//   68 bytes  total
//
// We send ours, then read exactly 68 bytes back. If the reply contains our
// info_hash, the peer is serving this torrent and we can move on. Anything
// else (wrong protocol, wrong info_hash) means it's a different swarm or a
// random internet probe - drop it.
//
// JAVA COMPARISON:
// Think of this as a "hello" exchange before a REST call: like sending a
// magic token in the first request and checking the token in the response.
// =============================================================================

#include "tracker/Peer.hpp"  // For your self.dev peers (ip + port)

#include <cstdint>   // For uint8_t
#include <string>    // For std::string
#include <vector>    // For std::vector

class PeerHandshake {
public:
    // Outcome of one connection attempt.
    struct Result {
        bool ok = false;           // handshake accepted
        std::string peerId;        // the peer's 20-byte identity (if ok)
        std::string error;         // human-readable failure reason (if !ok)
    };

    // Peer address, OUR info_hash, OUR peer_id, and a connect timeout.
    // Note: info_hash comes straight from Phase 2's TorrentFile.infoHash.
    static Result perform(const Peer& peer,
                          const std::vector<uint8_t>& infoHash,
                          const std::string& ourPeerId,
                          int timeoutSeconds = 10);
};