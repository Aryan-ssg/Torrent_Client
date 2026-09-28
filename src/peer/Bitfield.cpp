// =============================================================================
// Bitfield.cpp - "which pieces does this peer have?"
// =============================================================================

#include "peer/Bitfield.hpp"

#include <stdexcept>

Bitfield::Bitfield(size_t pieceCount) : pieceCount_(pieceCount) {
    bytes_.assign((pieceCount + 7) / 8, 0);
}

// =============================================================================
// parse()
// =============================================================================
bool Bitfield::parse(const std::vector<uint8_t>& payload, size_t pieceCount) {
    const size_t expected = (pieceCount + 7) / 8;

    // Length first. A BITFIELD whose size does not match the piece count is
    // either a different BitTorrent dialect or a peer talking about a
    // different torrent, and guessing which byte to believe is exactly how a
    // client ends up requesting pieces from -1.
    if (payload.size() != expected) {
        return false;
    }

    // Now the spare bits. The final byte holds 8 - (pieceCount % 8) bits that
    // do not correspond to any piece; the spec requires them to be zero.
    const size_t spare = expected * 8 - pieceCount;
    if (spare > 0 && expected > 0) {
        const uint8_t mask = static_cast<uint8_t>((1u << spare) - 1u);
        if ((payload[expected - 1] & mask) != 0) {
            return false;
        }
    }

    // Only now is it safe to commit: this function is all-or-nothing, so a
    // caller that reuses one Bitfield across reconnects cannot end up with a
    // half-applied, partly-trusted view of a peer.
    pieceCount_ = pieceCount;
    bytes_ = payload;
    return true;
}

size_t Bitfield::count() const {
    size_t n = 0;
    for (uint8_t b : bytes_) {
        // Kernighan's trick: clears the lowest set bit each pass, so the loop
        // runs once per SET bit rather than once per bit. For a 24k-piece
        // torrent's bitfield that is the difference between ~3k and ~190k
        // iterations, and this gets called on every connect.
        while (b) {
            b &= static_cast<uint8_t>(b - 1);
            n++;
        }
    }
    return n;
}

size_t Bitfield::firstMissing() const {
    for (size_t i = 0; i < pieceCount_; i++) {
        if (!has(i)) return i;
    }
    return kNoPiece;
}

std::string Bitfield::toString() const {
    std::string out;
    out.reserve(pieceCount_);
    for (size_t i = 0; i < pieceCount_; i++) {
        out.push_back(has(i) ? '#' : '.');
    }
    return out;
}
