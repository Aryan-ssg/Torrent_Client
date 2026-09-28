#pragma once

// =============================================================================
// Bitfield.hpp - "which pieces does this peer have?"
// =============================================================================
//
// Once a download is more than one piece long, the single most useful thing
// you can know about a peer is which pieces it owns. That one fact decides
// three things:
//
//   - which pieces are worth REQUESTing from it (asking for a piece a peer
//     does not have means waiting for a reply that will never come, until
//     your read timeout fires);
//   - how many pieces are left that nobody can serve yet ("stalled" is very
//     often just "the peers we picked happen not to have the missing bit");
//   - when to send HAVE, and whether a peer's copy is worth anything.
//
// THE WIRE FORMAT IS COUNTER-INTUITIVE
// ------------------------------------
// A BITFIELD message is one bit per piece, packed MSB-first: piece 0 is bit 7
// of byte 0, piece 7 is bit 0 of byte 0, piece 8 is bit 7 of byte 1. That is
// the opposite of "bit 0 of byte 0 means piece 0", which is what you would
// write if you were not paying attention, and it is the sort of detail that
// quietly makes a client request pieces nobody has.
//
// A peer with 20 pieces therefore sends exactly 3 bytes, and the last byte
// has 4 spare bits. Those spare bits MUST be zero; a peer that sets them is
// malformed, and treating them as real pieces would hand us out-of-range
// indices.
//
// "Never trust the network" is taken literally here: the length is checked
// against the piece count we already know, the spare bits are checked, and a
// peer that sends NO bitfield is treated as owning NOTHING - not everything.
// Assuming "no bitfield means all ones" is a bug that looks like a hung
// download rather than like a bug, because the client cheerfully requests
// pieces and waits forever for answers nobody is obliged to send.
//
// Java parallel: like a BitSet<boolean>, except with a validated wire format
// and a defined meaning for "the peer never told us".
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Bitfield {
public:
    // An empty bitfield owns nothing. This is the "peer said nothing" state,
    // and it is the correct default, not an error.
    Bitfield() = default;

    // A bitfield of `pieceCount` pieces, all unset.
    explicit Bitfield(size_t pieceCount);

    // Parse a BITFIELD payload from the wire.
    //
    // Returns false (and leaves *this untouched) if the payload is the wrong
    // length for `pieceCount`, or if any spare bit in the final byte is set.
    // Both are protocol violations worth refusing: a wrong length usually means
    // the peer is a different client version talking a different dialect, and
    // stray spare bits mean the sender is confused about how many pieces the
    // torrent has.
    bool parse(const std::vector<uint8_t>& payload, size_t pieceCount);

    size_t pieceCount() const { return pieceCount_; }
    size_t byteSize() const { return bytes_.size(); }

    bool has(size_t index) const {
        if (index >= pieceCount_) return false;  // out of range: certainly not ours
        return (bytes_[index / 8] & (0x80u >> (index % 8))) != 0;
    }

    void set(size_t index) {
        if (index < pieceCount_) bytes_[index / 8] |= static_cast<uint8_t>(0x80u >> (index % 8));
    }

    void clear(size_t index) {
        if (index < pieceCount_) bytes_[index / 8] &= static_cast<uint8_t>(~(0x80u >> (index % 8)));
    }

    // How many pieces we own.
    size_t count() const;

    // The first piece we do NOT have, or Bitfield::kNoPiece. Used to ask
    // "what are you missing?" when talking to a peer.
    size_t firstMissing() const;

    bool complete() const { return pieceCount_ > 0 && count() == pieceCount_; }

    // Exactly the bytes to put on the wire in a BITFIELD message.
    const std::vector<uint8_t>& bytes() const { return bytes_; }

    // Render as a string of '#' and '.' - handy in logs and in the test output,
    // and a genuinely useful debugging view when a download mysteriously stalls.
    std::string toString() const;

    static constexpr size_t kNoPiece = static_cast<size_t>(-1);

private:
    size_t pieceCount_ = 0;
    std::vector<uint8_t> bytes_;
};
