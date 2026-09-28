// =============================================================================
// PieceManager.cpp - The piece ownership tracker + file writer (Phase 6)
// =============================================================================

#include "piece/PieceManager.hpp"

#include "piece/FileException.hpp"

#include <openssl/sha.h>

#include <algorithm>     // For std::equal
#include <cstddef>       // For size_t
#include <fcntl.h>       // For O_CREAT, O_RDWR
#include <unistd.h>      // For ::ftruncate, ::close

namespace {

// The one gate every path through this class uses: does this byte range
// hash to the expected SHA-1 stored in the torrent?
bool hashMatches(const std::vector<uint8_t>& bytes,
                 const uint8_t* expected20) {
    std::vector<uint8_t> actual(SHA_DIGEST_LENGTH);
    SHA1(bytes.data(), bytes.size(), actual.data());
    return std::equal(actual.begin(), actual.end(), expected20);
}

// Read one piece's exact byte range back from the output file.
std::vector<uint8_t> readPiece(std::fstream& file, long pieceOffset, size_t len) {
    std::vector<uint8_t> bytes(len);
    file.clear();
    file.seekg(static_cast<std::streamoff>(pieceOffset));
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(len));
    // A SHORT READ must not be mistaken for success. Without this check the
    // tail of `bytes` would keep its zero-initialised (stale) values and get
    // hashed as if it were real data. Shrinking to what we actually read lets
    // the caller's size==expected check catch it, which would fail the hash
    // and mark the piece as NOT owned (safe resume behaviour).
    std::streamsize got = file.gcount();
    if (got >= 0 && static_cast<size_t>(got) < len) {
        bytes.resize(static_cast<size_t>(got));
    }
    return bytes;
}

}  // namespace

PieceManager::PieceManager(const TorrentFile& torrent, const std::string& outputPath)
    : torrent_(torrent), outputPath_(outputPath), fullLength_(torrent.length) {
    // Piece count: one piece per 20-byte SHA-1 in the torrent's pieces blob.
    // Everything starts PENDING (not yet fetched).
    size_t n = torrent_.pieces.size() / 20;
    states_.assign(n, State::kPending);
    claimTokens_.assign(n, kNoClaim);
    claimTimes_.assign(n, std::chrono::steady_clock::time_point{});

    if (n == 0) {
        throw FileException("Torrent has zero pieces");
    }

    // Preallocate the whole file, so later we can write ANY piece into its
    // correct position regardless of arrival order. (POSIX: create/truncate
    // to the exact final size; sparse on most filesystems.)
    int fd = ::open(outputPath.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        throw FileException("Cannot create output file: " + outputPath);
    }
    if (::ftruncate(fd, static_cast<off_t>(fullLength_)) != 0) {
        ::close(fd);
        throw FileException("Cannot size output file: " + outputPath);
    }
    ::close(fd);

    // Open it for read+write; we will bounce between seek+writes (store
    // pieces) and seek+reads (scanDisk resume) over the manager's life.
    file_.open(outputPath.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) {
        throw FileException("Cannot open output file for use: " + outputPath);
    }
}

size_t PieceManager::pieceLength(size_t index) const {
    size_t offset = index * torrent_.pieceLength;
    if (offset >= fullLength_) return 0;
    return std::min<size_t>(torrent_.pieceLength, fullLength_ - offset);
}

size_t PieceManager::completedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (State s : states_) count += (s == State::kOwned);
    return count;
}

bool PieceManager::complete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (State s : states_) {
        if (s != State::kOwned) return false;
    }
    return true;
}

bool PieceManager::hasPiece(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index < states_.size() && states_[index] == State::kOwned;
}

size_t PieceManager::nextPieceToFetch(const Bitfield* peerHas) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < states_.size(); i++) {
        if (states_[i] != State::kPending) continue;  // claimed or already ours
        // Only offer pieces this peer actually has. Asking for a piece the
        // peer does not own is not an error, it is a stall: the request sits
        // unanswered until our read timeout fires, and one such piece is
        // enough to make a whole worker look hung.
        if (peerHas != nullptr && !peerHas->has(i)) continue;
        return i;
    }
    return kNotFound;
}

PieceManager::ClaimToken PieceManager::claimPiece(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= states_.size() || states_[index] != State::kPending) {
        return kNoClaim;  // already claimed by another worker, or already owned
    }
    states_[index] = State::kClaimed;

    // A fresh token per claim. Monotonic rather than random so "newer than
    // mine" is a comparable quantity, and so a token can never be reused
    // within a process.
    const ClaimToken token = nextClaim_++;
    claimTokens_[index] = token;
    claimTimes_[index] = std::chrono::steady_clock::now();
    return token;
}

void PieceManager::releasePiece(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < states_.size() && states_[index] == State::kClaimed) {
        states_[index] = State::kPending;  // available for someone else to retry
        claimTokens_[index] = kNoClaim;
    }
}

PieceManager::ClaimToken PieceManager::reclaimStuckPiece(std::chrono::seconds stuckAfter) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < states_.size(); i++) {
        if (states_[i] != State::kClaimed) continue;
        if (now - claimTimes_[i] < stuckAfter) continue;

        // Reclaim it. The previous holder's token is NOT cleared to kNoClaim
        // - it is replaced with a new one, so the old holder's storePiece()
        // fails its token check and is turned away. That is the whole point:
        // without it, a slow-but-eventually-valid answer would overwrite a
        // piece somebody else has already finished.
        states_[i] = State::kPending;
        const ClaimToken token = nextClaim_++;
        claimTokens_[i] = token;
        claimTimes_[i] = now;
        return token;  // the caller now owns this piece instead
    }
    return kNoClaim;
}

void PieceManager::heartbeatClaim(ClaimToken token) {
    if (token == kNoClaim) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < claimTokens_.size(); i++) {
        if (claimTokens_[i] == token) {
            claimTimes_[i] = std::chrono::steady_clock::now();
            return;
        }
    }
}

bool PieceManager::storePiece(size_t index, const std::vector<uint8_t>& bytes,
                             ClaimToken token) {
    std::lock_guard<std::mutex> lock(mutex_);

    // "Is this still my claim?" - checked before anything else, and under the
    // same lock that performs the state change below, so a reclaim happening
    // concurrently cannot slip in between the test and the write.
    if (token != kNoClaim) {
        if (index >= claimTokens_.size() || claimTokens_[index] != token) {
            return false;  // reclaimed by a watchdog or another worker: drop it
        }
    }

    if (index >= states_.size()) return false;

    // 1. Exact size, then exact hash. Nothing touches disk on a mismatch.
    //    This is genuinely the last line of defense: even if every layer
    //    above us is buggy or lying, garbage never lands in the file.
    if (bytes.size() != pieceLength(index)) return false;
    if (!hashMatches(bytes, torrent_.pieces.data() + index * 20)) return false;

    // 2. Write at the piece's absolute offset inside the file. (Held under
    //    the mutex: only one thread writes the fstream at a time, and the
    //    seek+write must be atomic with respect to claim/store decisions.)
    uint64_t offset = static_cast<uint64_t>(index) * torrent_.pieceLength;
    file_.seekp(static_cast<std::streamoff>(offset));
    file_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!file_) {
        file_.clear();   // leave the stream usable for a retry
        throw FileException("write() failed at piece " + std::to_string(index) +
                            " in " + outputPath_);
    }
    file_.flush();

    states_[index] = State::kOwned;  // claim resolved: owned and on disk
    return true;
}

void PieceManager::scanDisk() {
    // Resume: re-verify EVERY piece currently on disk. Only a piece that
    // still hashes correctly counts as owned. A partial download or a few
    // corrupted bytes costs only the pieces that actually failed - the rest
    // is spared the re-download.
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < states_.size(); i++) {
        long offset = static_cast<long>(i) * torrent_.pieceLength;
        std::vector<uint8_t> bytes = readPiece(file_, offset, pieceLength(i));
        states_[i] = (bytes.size() == pieceLength(i) &&
                      hashMatches(bytes, torrent_.pieces.data() + i * 20))
                         ? State::kOwned
                         : State::kPending;
    }
}