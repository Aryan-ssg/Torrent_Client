// =============================================================================
// ConcurrentDownloader.cpp - The parallel download engine (Phase 7)
// =============================================================================

#include "peer/ConcurrentDownloader.hpp"

#include "peer/PieceDownloader.hpp"

#include <atomic>       // For std::atomic (cross-thread counters/flags)
#include <cstddef>      // For size_t
#include <mutex>        // For std::mutex (guards the first-error message)
#include <thread>       // For std::thread
#include <vector>       // For std::vector

namespace {

// How many times one worker may fail (claim a piece, fail to download it)
// before it stops trying. Without a cap, a worker pointed at a broken peer
// would spin forever on the same piece.
constexpr int kMaxWorkerFailures = 3;

}  // namespace

ConcurrentDownloader::Result ConcurrentDownloader::download(
    const TorrentFile& torrent,
    const std::vector<Peer>& peers,
    const std::string& outputPath,
    const std::string& ourPeerId,
    int workerCount,
    int timeoutSeconds) {
    Result result;

    if (peers.empty()) {
        result.error = "no peers to download from";
        return result;
    }
    if (workerCount <= 0) {
        result.error = "workerCount must be >= 1";
        return result;
    }
    if (torrent.infoHash.size() != 20 || torrent.pieces.size() % 20 != 0) {
        result.error = "torrent metadata is incomplete";
        return result;
    }

    // The manager is the one thing every worker shares. It is mutex-protected,
    // so claiming / storing from many threads at once is safe.
    PieceManager manager(torrent, outputPath);

    // Cross-thread state:
    //  - aborted_: the first worker that runs out of patience sets this; all
    //    others see it and stop, so the whole run is aborted cleanly.
    //  - storedCount_: how many pieces we successfully stored (diagnostics).
    //  - firstError: written once (under a mutex) and read only after every
    //    thread has joined, so it needs no atomic type of its own.
    std::atomic<bool> aborted{false};
    std::atomic<size_t> storedCount{0};
    std::mutex errorMutex;
    std::string firstError;

    auto worker = [&](int workerIndex) {
        int failures = 0;

        for (;;) {
            if (aborted.load()) return;                 // someone gave up
            if (manager.complete()) return;             // all pieces owned

            size_t idx = manager.nextPieceToFetch();
            if (idx == PieceManager::kNotFound) return; // really nothing left

            // Claim atomically: two workers may both see idx==N, but only one
            // wins claimPiece(). The loser loops and picks the NEXT piece.
            if (!manager.claimPiece(idx)) continue;

            size_t len = manager.pieceLength(idx);
            std::vector<uint8_t> expectedHash(
                torrent.pieces.begin() + static_cast<long>(idx) * 20,
                torrent.pieces.begin() + static_cast<long>(idx) * 20 + 20);

            // Pick the peer for this piece. Choosing peers[idx % N] spreads the
            // work evenly across the swarm (piece 0 -> peer 0, piece 1 -> peer
            // 1, ...) instead of pinning each worker to one peer - which could
            // overload a single peer if the claim races hand one worker most of
            // the pieces.
            const Peer& peer = peers[static_cast<size_t>(idx) % peers.size()];

            PieceDownloader::Result r = PieceDownloader::download(
                peer, torrent.infoHash, ourPeerId,
                static_cast<uint32_t>(idx), len, expectedHash, timeoutSeconds);

            if (r.ok) {
                if (manager.storePiece(idx, r.data)) {
                    storedCount.fetch_add(1);
                    failures = 0;   // one good piece wipes the slate clean
                } else {
                    // storePiece's own SHA-1 gate refused it: release for retry.
                    manager.releasePiece(idx);
                    failures++;
                }
            } else {
                // Release the claim so another worker can try this piece with
                // a (maybe different) peer, then count the failure.
                manager.releasePiece(idx);
                failures++;
                if (failures >= kMaxWorkerFailures && !aborted.exchange(true)) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    firstError = "piece " + std::to_string(idx) +
                                 " failed after retries (" + r.error + ")";
                }
            }
        }
    };

    // Spawn the workers, then wait for all of them.
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workerCount));
    for (int w = 0; w < workerCount; w++) {
        threads.emplace_back(worker, w);
    }
    for (auto& t : threads) {
        t.join();
    }

    result.piecesDownloaded = storedCount.load();
    if (!aborted.load() && manager.complete()) {
        result.ok = true;
    } else {
        result.error = firstError.empty() ? "run ended with pieces missing" : firstError;
    }
    return result;
}