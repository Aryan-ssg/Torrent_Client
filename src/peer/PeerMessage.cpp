// =============================================================================
// PeerMessage.cpp - Length-prefixed message framing (see PeerMessage.hpp)
// =============================================================================

#include "peer/PeerMessage.hpp"

#include "net/NetException.hpp"

#include <cstring>   // For std::memcpy

// Big-endian helpers. The internet says "most significant byte first", so
// when our machine (which stores numbers little-endian) writes a length we
// manually place the bytes most-significant first — same lesson as Phase 3's
// compact peers, here written as shifts into an array.
static void putU32BE(uint8_t out[4], uint32_t v) {
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

static uint32_t getU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

void sendMessage(TcpSocket& sock, uint8_t id, const std::vector<uint8_t>& payload) {
    // Frame: [length: 4][id: 1][payload]
    // length counts id + payload, so it is 1 more than payload.size().
    uint32_t length = 1 + static_cast<uint32_t>(payload.size());

    uint8_t header[5];
    putU32BE(header, length);
    header[4] = id;

    sock.sendAll(header, 5);
    if (!payload.empty()) {
        sock.sendAll(payload.data(), payload.size());
    }
}

void sendMessage(TcpSocket& sock, uint8_t id) {
    sendMessage(sock, id, std::vector<uint8_t>());
}

PeerMessage readMessage(TcpSocket& sock) {
    for (;;) {
        // Read the 4-byte length prefix first (exactly, because TCP streams).
        uint8_t lenBytes[4];
        sock.recvExact(lenBytes, 4);
        uint32_t length = getU32BE(lenBytes);

        // length == 0 is a keep-alive: skip it and read the next message.
        if (length == 0) continue;

        // The id byte is part of 'length'. "Never trust the network": a
        // malicious length could claim gigabytes and make us buffer forever.
        if (length > kMaxPeerMessageSize) {
            throw NetException("Peer message too large: " + std::to_string(length));
        }
        if (length < 1) {
            throw NetException("Peer message length underflow");
        }

        // Read the id + payload in one exact read.
        std::vector<uint8_t> body(length);
        sock.recvExact(body.data(), length);

        PeerMessage msg;
        msg.id = body[0];
        msg.payload.assign(body.begin() + 1, body.end());
        return msg;
    }
}

std::vector<uint8_t> buildRequestPayload(uint32_t index, uint32_t begin, uint32_t length) {
    std::vector<uint8_t> p(12);
    putU32BE(p.data() + 0, index);
    putU32BE(p.data() + 4, begin);
    putU32BE(p.data() + 8, length);
    return p;
}

bool parseRequestPayload(const std::vector<uint8_t>& payload,
                         uint32_t& index, uint32_t& begin, uint32_t& length) {
    if (payload.size() != 12) return false;
    index  = getU32BE(payload.data() + 0);
    begin  = getU32BE(payload.data() + 4);
    length = getU32BE(payload.data() + 8);
    return true;
}

std::vector<uint8_t> buildPiecePayload(uint32_t index, uint32_t begin,
                                       const uint8_t* data, size_t len) {
    std::vector<uint8_t> p(8 + len);
    putU32BE(p.data() + 0, index);
    putU32BE(p.data() + 4, begin);
    std::memcpy(p.data() + 8, data, len);
    return p;
}

bool parsePiecePayload(const std::vector<uint8_t>& payload,
                       uint32_t& index, uint32_t& begin,
                       const uint8_t*& data, size_t& len) {
    if (payload.size() < 8) return false;
    index = getU32BE(payload.data() + 0);
    begin = getU32BE(payload.data() + 4);
    data  = payload.data() + 8;
    len   = payload.size() - 8;
    return true;
}