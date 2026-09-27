#pragma once

// =============================================================================
// PeerMessage.hpp - The messages two peers exchange AFTER the handshake
// =============================================================================
//
// Phase 4 handled the very first bytes (the 68-byte handshake). Everything
// after it speaks this language:
//
//   [4 bytes: length, big-endian][1 byte: message id][payload (length-1 bytes)]
//
// The 4-byte length counts the id byte AND the payload (so "length 1" means
// "a message with just an id and no payload", like CHOKE).
//
// A length of 0 is special: it's a "keep-alive" — just a pulse every ~2
// minutes telling the other side "still here, nothing to say". It has NO id
// byte. readMessage() silently skips these.
//
// Message ids (the BitTorrent spec):
//   0  CHOKE            "you may not ask me for pieces right now"
//   1  UNCHOKE          "go ahead, ask me for pieces"
//   2  INTERESTED       "I want pieces you have"
//   3  NOT_INTERESTED   "nothing you have appeals to me"
//   4  HAVE             "I just got piece number X"
//   5  BITFIELD         "here's the whole list of pieces I own" (sent right
//                        after the handshake, before anything else)
//   6  REQUEST          "please send me index:begin, length bytes"
//   7  PIECE            "here is the data you asked for" (index, begin, bytes)
//   8  CANCEL           "never mind that request"
//
// Java parallel: this is the wire format of a messaging protocol, like
// length-prefixed records in a serialization framework. The handshake was
// special (fixed 68 bytes); everything from here on is length-prefixed, which
// is exactly why recvExact() (Phase 3) is so central.
// =============================================================================

#include "net/TcpSocket.hpp"

#include <cstddef>     // For size_t
#include <cstdint>     // For uint8_t, uint32_t
#include <string>      // For std::string
#include <vector>      // For std::vector

// The standard block size all BitTorrent clients ask for between peers.
constexpr size_t kBlockSize = 16384;  // 16 KiB

// Sane cap for the length prefix: a single message bigger than this is either
// a protocol violation or an attack. "Never trust the network".
constexpr size_t kMaxPeerMessageSize = 1u << 20;  // 1 MiB

// The message ids (see comment block above).
enum : uint8_t {
    MSG_CHOKE = 0,
    MSG_UNCHOKE = 1,
    MSG_INTERESTED = 2,
    MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4,
    MSG_BITFIELD = 5,
    MSG_REQUEST = 6,
    MSG_PIECE = 7,
    MSG_CANCEL = 8,
};

// One decoded peer message: the id plus the raw payload (everything after
// the id byte). Throws NetException if the peer closes / times out mid-read
// or sends an absurd length.
struct PeerMessage {
    uint8_t id = 0;
    std::vector<uint8_t> payload;
};

// Send a complete message: length prefix + id + payload.
// (length = 1 + payload.size(); "payload==empty" still sends a 1-byte frame.)
void sendMessage(TcpSocket& sock, uint8_t id, const std::vector<uint8_t>& payload);
void sendMessage(TcpSocket& sock, uint8_t id);   // id-only (CHOKE, UNCHOKE, ...)

// Read one complete message, honoring the length prefix and skipping
// keep-alives. Exactly like a file where record i is "length + bytes".
PeerMessage readMessage(TcpSocket& sock);

// -----------------------------------------------------------------------------
// REQUEST payload  (6):  [4: index][4: begin][4: length]   all big-endian
// PIECE payload    (7):  [4: index][4: begin][bytes...]
// -----------------------------------------------------------------------------
// Build the payload bytes for a REQUEST message.
std::vector<uint8_t> buildRequestPayload(uint32_t index, uint32_t begin, uint32_t length);

// Parse a REQUEST payload into its three numbers. Returns false if malformed.
bool parseRequestPayload(const std::vector<uint8_t>& payload,
                         uint32_t& index, uint32_t& begin, uint32_t& length);

// Build the payload bytes for a PIECE message (index + begin + data).
std::vector<uint8_t> buildPiecePayload(uint32_t index, uint32_t begin,
                                       const uint8_t* data, size_t len);

// Parse a PIECE payload into index + begin + the data slice.
bool parsePiecePayload(const std::vector<uint8_t>& payload,
                       uint32_t& index, uint32_t& begin,
                       const uint8_t*& data, size_t& len);