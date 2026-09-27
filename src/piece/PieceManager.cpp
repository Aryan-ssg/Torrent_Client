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
    return bytes;
}

}  // namespace

PieceManager::PieceManager(const TorrentFile& torrent, const std::string& outputPath)
    : torrent_(torrent), outputPath_(outputPath), fullLength_(torrent.length) {
    // Piece count: one piece per 20-byte SHA-1 in the torrent's pieces blob.
    size_t n = torrent_.pieces.size() / 20;
    owned_.assign(n, 0);

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
    size_t count = 0;
    for (uint8_t o : owned_) count += (o != 0);
    return count;
}

bool PieceManager::complete() const {
    return completedCount() == owned_.size();
}

bool PieceManager::hasPiece(size_t index) const {
    return index < owned_.size() && owned_[index] != 0;
}

size_t PieceManager::nextPieceToFetch() const {
    for (size_t i = 0; i < owned_.size(); i++) {
        if (owned_[i] == 0) return i;
    }
    return kNotFound;
}

bool PieceManager::storePiece(size_t index, const std::vector<uint8_t>& bytes) {
    if (index >= owned_.size()) return false;

    // 1. Exact size, then exact hash. Nothing touches disk on a mismatch.
    //    This is genuinely the last line of defense: even if every layer
    //    above us is buggy or lying, garbage never lands in the file.
    if (bytes.size() != pieceLength(index)) return false;
    if (!hashMatches(bytes, torrent_.pieces.data() + index * 20)) return false;

    // 2. Write at the piece's absolute offset inside the file.
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

    owned_[index] = 1;
    return true;
}

void PieceManager::scanDisk() {
    // Resume: re-verify EVERY piece currently on disk. Only a piece that
    // still hashes correctly counts as owned. A partial download or a few
    // corrupted bytes costs only the pieces that actually failed - the rest
    // is spared the re-download.
    for (size_t i = 0; i < owned_.size(); i++) {
        long offset = static_cast<long>(i) * torrent_.pieceLength;
        std::vector<uint8_t> bytes = readPiece(file_, offset, pieceLength(i));
        owned_[i] = (bytes.size() == pieceLength(i) &&
                     hashMatches(bytes, torrent_.pieces.data() + i * 20)) ? 1 : 0;
    }
}