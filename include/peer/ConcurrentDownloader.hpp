#pragma once

// =============================================================================
// ConcurrentDownloader.hpp - Download ALL pieces using several peers at once
// =============================================================================
//
// Phases 5–6 were strictly SEQUENTIAL: fetch one piece, wait, write it, fetch
// the next. That's like a single waiter carrying one plate at a time. Phase 7
// makes the download parallel, the way real clients work:
//
//   - spawn N WORKER THREADS (imagine N waiters)
//   - each worker loops:
//       1. ask the PieceManager for the next PENDING piece
//       2. claim it, so no other worker fetches the same piece
//       3. ask ONE peer from our peer list for it (handshake -> interested ->
//          request blocks -> SHA-1 verified)
//       4. store it with the manager (it re-verifies before touching disk)
//   - if a download fails, the piece is RELEASED back to PENDING so another
//     worker can retry it
//
// The peer for step 3 is picked from the swarm that Phase 3's tracker gave
// us (worker i -> peers[i % size]). Because each worker talks to its own peer
// over its own TCP connection, the fake seeders in the test run truly in
// parallel - which is exactly what the Phase 7 timing test measures.
//
// (Real clients take parallelism one level further: for big pieces they split
// ONE piece into blocks and fetch different blocks from different peers. Our
// synthetic pieces are 16 KiB - one block each - so piece-per-worker is the
// full picture here; block-level parallelism is the obvious next refinement
// for 256 KiB pieces like Ubuntu's.)
//
// Java parallel: ExecutorService with a pool of threads, each thread doing
// next-piece / claim / fetch / store against a thread-safe PieceManager
// (like ConcurrentHashMap guarding the completed-chunks tracker).
// =============================================================================

#include "piece/PieceManager.hpp"
#include "torrent/TorrentFile.hpp"
#include "tracker/Peer.hpp"

#include <cstddef>     // For size_t
#include <string>      // For std::string
#include <vector>      // For std::vector

class ConcurrentDownloader {
public:
    // The outcome of a whole parallel run.
    struct Result {
        bool ok = false;              // true iff every piece ended up owned
        std::string error;            // first failure, if any
        size_t piecesDownloaded = 0;  // how many storePiece() calls succeeded
    };

    // Download `torrent` into `outputPath` using up to `workerCount` parallel
    // workers spread across `peers`. Blocks until all pieces are fetched (or
    // a worker gives up after repeated failures and aborts the run).
    static Result download(const TorrentFile& torrent,
                           const std::vector<Peer>& peers,
                           const std::string& outputPath,
                           const std::string& ourPeerId,
                           int workerCount,
                           int timeoutSeconds);
};