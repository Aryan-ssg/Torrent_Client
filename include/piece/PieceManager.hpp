#pragma once

// =============================================================================
// PieceManager.hpp - Own a torrent's pieces and write them to disk
// =============================================================================
//
// Phase 5 proved we can fetch ONE verified piece. Phase 6 turns that into a
// real download engine: remember which pieces we own, fetch the ones we
// don't, and write verified piece data to the correct place in the output
// file.
//
// Responsibilities:
//   - an OWNERSHIP MAP: each piece is PENDING (not yet fetched), OWNED
//     (verified + on disk), or CLAIMED (a worker is fetching it right now).
//     Phase 7 reads this map from MANY threads at once, so it is guarded by a
//     mutex. The "claimed" state is what stops two workers from wasting two
//     downloads on the same piece.
//   - storePiece(): the LAST gate before bytes touch disk. It re-hashes the
//     piece with SHA-1 against the torrent's stored hash. Wrong hash =
//     rejected, nothing written, piece NOT marked - "never trust the network"
//     (and never trust a buggy downloader).
//   - writes at the correct offset (pieceIndex * pieceLength), so pieces can
//     arrive in any order, like a real client
//   - scanDisk(): RESUME support. On restart we re-read the file and re-hash
//     every piece; only pieces whose hash matches count as owned. This means
//     even a corrupted or half-written file on disk is handled honestly -
//     "never trust the disk either". (Re-hashing a 6 GB file at startup is
//     expensive in production; clients usually skip it via a checkpoint file.
//     For learning, verifying on resume is far more honest.)
//
// The file is PREALLOCATED to its full size up front so any piece can be
// written into place no matter when it arrives.
//
// Java parallel: like a RandomAccessFile-based chunk writer plus a
// BitSet<bool[]> tracker of "which chunks are done", synchronized so several
// download threads can claim different chunks without duplicating work.
// =============================================================================

#include "peer/Bitfield.hpp"
#include "torrent/TorrentFile.hpp"

#include <chrono>
#include <cstddef>     // For size_t
#include <cstdint>     // For uint8_t
#include <fstream>     // For std::fstream
#include <mutex>       // For std::mutex (thread safety, Phase 7)
#include <string>      // For std::string
#include <vector>      // For std::vector

class PieceManager {
public:
    // Per-piece state, visible to the whole process. The three values are the
    // lifecycle of one piece:
    //   PENDING -> CLAIMED -> OWNED          (normal download)
    //   PENDING -> CLAIMED -> PENDING        (worker failed: try again later)
    //   CLAIMED -> PENDING                   (claim timed out: slow peer)
    enum class State {
        kPending = 0,  // not fetched yet
        kClaimed,      // a download worker is fetching it right now
        kOwned         // verified SHA-1 AND written to disk
    };

    // Proof that YOU are the worker holding a claim.
    //
    // This exists because of stuck-claim reclaim. A worker can be talking to a
    // peer that has stopped responding but whose socket has not yet timed out;
    // long after the piece is handed to somebody else, the slow worker may
    // come back with a perfectly valid piece. Without a token it would happily
    // write it - possibly for a piece that a second worker has since
    // re-downloaded, or dropped the piece back to PENDING. Passing the token
    // to storePiece() makes "is my claim still mine?" a single comparison
    // under the lock, so a stale worker is turned away instead of racing.
    //
    // Java parallel: like a version stamp, or an AtomicLong you compare before
    // writing, so a slow thread cannot clobber a newer writer.
    using ClaimToken = uint64_t;
    static constexpr ClaimToken kNoClaim = 0;

    // torrent: the metadata (piece length, hashes, total size).
    // outputPath: where the completed file will live.
    PieceManager(const TorrentFile& torrent, const std::string& outputPath);

    // Returns the size of piece `index` (the last piece may be shorter).
    size_t pieceLength(size_t index) const;

    // True once every piece is written and verified.
    bool complete() const;

    size_t pieceCount() const { return states_.size(); }
    size_t completedCount() const;   // how many pieces are marked owned

    // Do we already own (have verified) this piece?
    bool hasPiece(size_t index) const;

    // The index of the next PENDING piece, or kNotFound if none.
    //
    // `peerHas`, when given, restricts the search to pieces that peer owns.
    // Without this a downloader blindly asks peer 0 for pieces 0, N, 2N... and
    // blocks forever whenever peer 0 does not own one of them - which, in any
    // real swarm, is most of them.
    size_t nextPieceToFetch(const Bitfield* peerHas = nullptr) const;

    // Worker protocol: atomically mark a PENDING piece as CLAIMED so no other
    // worker picks it. Returns kNoClaim if someone else already claimed or
    // owns it. The returned token must be passed to storePiece().
    ClaimToken claimPiece(size_t index);

    // Undo a claim (the download failed). The piece goes back to PENDING so
    // another worker can retry it later.
    void releasePiece(size_t index);

    // Verify SHA-1 against the torrent AND write at the right offset.
    // Returns false if the hash didn't match (nothing marked, nothing written).
    // On success the piece becomes OWNED (its claim is resolved).
    //
    // If `token` is not kNoClaim, it must be the token this worker received
    // from claimPiece(); if the claim has since been reclaimed by a watchdog
    // or by another worker, the write is refused. That is what stops a slow
    // worker's late arrival from corrupting a piece someone else has finished.
    bool storePiece(size_t index, const std::vector<uint8_t>& bytes,
                    ClaimToken token = kNoClaim);

    // Re-claim a piece whose worker has stopped making progress, so a single
    // slow peer cannot stall the tail of a download forever. Returns kNoClaim
    // if the piece is not actually stuck, or someone else took it first.
    //
    // `stuckAfter` is how long a claim may stand untouched. Pieces are only
    // reclaimed from workers that are demonstrably not finishing, never from
    // one that is merely slow.
    ClaimToken reclaimStuckPiece(std::chrono::seconds stuckAfter);

    // Touch a claim's clock - called as a piece's blocks arrive, so an
    // in-progress download is never mistaken for a stalled one.
    void heartbeatClaim(ClaimToken token);

    // Resume: re-read the output file and mark every piece whose data on
    // disk hashes correctly. Corrupted pieces simply stay "not owned".
    void scanDisk();

    // The output file we are writing (handy for tests that inspect/corrupt).
    const std::string& path() const { return outputPath_; }

    // Total bytes on disk once finished - what the progress bar measures
    // against. Cached, because the UI asks for it on every repaint.
    long long totalBytes() const { return fullLength_; }

    // Sentinel for "no next piece" (same trick as std::string::npos).
    static constexpr size_t kNotFound = static_cast<size_t>(-1);

private:
    TorrentFile torrent_;        // a copy (the manager owns its data)
    std::string outputPath_;
    std::fstream file_;          // held open for the life of the manager
    std::vector<State> states_;  // per-piece lifecycle state (all values
                                 // accessed only while holding mutex_)
    size_t fullLength_ = 0;      // torrent_.length, cached for clarity

    // Per-piece claim bookkeeping, indexed the same way as states_ (both
    // guarded by mutex_). nextClaim_ is a monotonically increasing counter so
    // every claim gets a distinct token.
    std::vector<ClaimToken> claimTokens_;
    std::vector<std::chrono::steady_clock::time_point> claimTimes_;
    ClaimToken nextClaim_ = 1;

    // Guards states_, claimTokens_ and claimTimes_. Phase 7 reads and writes
    // them from several worker threads at once; every access must hold this.
    // (The rest of the object is immutable after construction.)
    mutable std::mutex mutex_;
};