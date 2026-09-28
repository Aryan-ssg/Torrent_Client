#pragma once

// =============================================================================
// PieceDownloader.hpp - Download and SHA-1-verify ONE piece from a peer
// =============================================================================
//
// Phase 4 = say hello. Phase 5 = actually get data. This is the full minimal
// conversation for fetching a single piece:
//
//   1. connect to the peer (TcpSocket)
//   2. handshake (68 bytes, PeerHandshake::exchange)
//   3. send INTERESTED, read messages until UNCHOKE
//   4. send REQUEST messages for each 16 KiB block of the piece
//   5. collect the PIECE messages and assemble the blocks in the right order
//   6. SHA-1 the whole piece and compare with the torrent's stored hash
//
// Step 6 is the "never trust the network" moment: the peer could hand us
// garbage (corrupted transmission, lies, attack). The hash check turns a bad
// peer into a REPORTED failure instead of corrupted storage.
//
// Real internet peers are reachable (Phase 4 proves it against the live
// swarm), but they are not controllable - a real one will not send a
// deliberately corrupted piece on cue. So the tests run this against the
// loopback FakePeer seeder, which can misbehave on demand.
//
// Java parallel: like a small client method that downloads one chunk/Range and
// verifies a checksum before returning it.
// =============================================================================

#include "tracker/Peer.hpp"

#include <cstddef>     // For size_t
#include <cstdint>     // For uint8_t
#include <string>      // For std::string
#include <vector>      // For std::vector

class PieceDownloader {
public:
    // Outcome of one piece download attempt.
    struct Result {
        bool ok = false;                // piece received AND hash-checked
        std::string error;              // plain-English failure, if !ok
        std::vector<uint8_t> data;      // the verified piece bytes (if ok)
    };

    // peer: who to ask; infoHash/ourPeerId: handshake identity (Phases 2/4);
    // pieceIndex: which piece (0-based); pieceLength: size of THAT piece
    // (the last piece of a torrent may be shorter!); expectedHash: the 20
    // SHA-1 bytes from TorrentFile.pieces for this piece; timeoutSeconds:
    // per-receive deadline so dead peers cost seconds, not minutes.
    static Result download(const Peer& peer,
                           const std::vector<uint8_t>& infoHash,
                           const std::string& ourPeerId,
                           uint32_t pieceIndex,
                           size_t pieceLength,
                           const std::vector<uint8_t>& expectedHash,
                           int timeoutSeconds = 10);
};