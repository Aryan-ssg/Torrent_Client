#pragma once

// =============================================================================
// ConcurrentDownloader.hpp - Download ALL pieces using many peers at once
// =============================================================================
//
// Phases 5-6 were strictly sequential (fetch one piece, write it, next), and
// Phase 7 made it parallel by giving each worker a fresh connection per piece.
// That still made 23,664 connections for a 6 GB ISO, which no real swarm
// survives for long.
//
// This version keeps one PeerSession per worker alive and moves many pieces
// over it, which is what real clients do. The scheduling is now shaped by
// what peers actually have:
//
//   - each worker owns a session with ONE peer, and that peer's bitfield
//     decides which pieces are worth asking for;
//   - when the peer's remaining pieces are claimed or owned, the worker
//     redials and picks a different peer;
//   - a claim that stops making progress is reclaimed after a timeout, so one
//     unresponsive peer cannot strand the last few pieces.
//
// PROGRESS REPORTING
// ------------------
// Workers never call the progress callback directly. They only bump a handful
// of atomics; a single reporter thread snapshots them on a timer and invokes
// the callback. That matters for two reasons: per-worker throttling does not
// give a global 10 Hz (N workers each at 10 Hz is 10N), and a callback that
// draws to a terminal must not be entered from several threads at once.
// Invoking it from one thread also means the UI needs no lock of its own.
// =============================================================================

#include "piece/PieceManager.hpp"
#include "torrent/TorrentFile.hpp"
#include "tracker/Peer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A snapshot of the download's state, handed to the progress callback on a
// single reporting thread.
struct DownloadProgress {
    long long bytesDone = 0;
    long long bytesTotal = 0;
    size_t piecesDone = 0;
    size_t piecesTotal = 0;
    int peersConnected = 0;   // sessions currently usable
    int peersTried = 0;       // distinct peers dialled so far
    bool finished = false;
    bool failed = false;
    std::string error;

    double fraction() const {
        if (bytesTotal <= 0) return 0.0;
        double f = static_cast<double>(bytesDone) / static_cast<double>(bytesTotal);
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    }
};

class ConcurrentDownloader {
public:
    // The outcome of a whole parallel run.
    struct Result {
        bool ok = false;              // true iff every piece ended up owned
        std::string error;            // first failure, if any
        size_t piecesDownloaded = 0;  // how many storePiece() calls succeeded
    };

    using ProgressCallback = std::function<void(const DownloadProgress&)>;

    // Download `torrent` into `outputPath` using up to `workerCount` parallel
    // workers spread across `peers`. Blocks until all pieces are fetched (or
    // the run aborts). `onProgress`, if given, is called from ONE dedicated
    // thread at roughly 10 Hz and must be safe to call without extra locking.
    static Result download(const TorrentFile& torrent,
                           const std::vector<Peer>& peers,
                           const std::string& outputPath,
                           const std::string& ourPeerId,
                           int workerCount,
                           int timeoutSeconds,
                           ProgressCallback onProgress = nullptr,
                           std::atomic<bool>* abortFlag = nullptr);
};
