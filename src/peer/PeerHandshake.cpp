// =============================================================================
// PeerHandshake.cpp - Building, sending and checking the 68-byte handshake
// =============================================================================

#include "peer/PeerHandshake.hpp"

#include "net/TcpSocket.hpp"

#include <cstddef>  // For size_t
#include <string>   // For std::string

using std::string;

// =============================================================================
// buildHandshake()
// =============================================================================
// Laying bytes down in order gives exactly 68 bytes:
//   1 + 19 + 8 + 20 + 20 = 68
// =============================================================================
static string buildHandshake(const std::vector<uint8_t>& infoHash, const string& ourPeerId) {
    string handshake;

    handshake += static_cast<char>(19);          // 1 byte: protocol name length
    handshake += "BitTorrent protocol";          // 19 bytes: the name
    handshake.append(8, '\0');                   // 8 bytes: reserved zeros
    handshake.append(infoHash.begin(),           // 20 bytes: our torrent
                     infoHash.end());
    handshake += ourPeerId;                      // 20 bytes: who we are

    return handshake;                            // 68 bytes total
}

// =============================================================================
// perform()
// =============================================================================
// Handshake layout of the reply (same 68 bytes, info_hash + peer_id swapped):
//
//   [0]          : 19
//   [1..19]      : "BitTorrent protocol"
//   [20..27]     : reserved bytes (we ignore)
//   [28..47]     : the peer's info_hash  <-- MUST equal ours
//   [48..67]     : the peer's peer_id    <-- this is what we report back
//
// The whole point of checking [28..47]: if it's not our info_hash, this peer
// either has a different torrent or was just listening for anyone. Either way,
// talking to it is pointless and possibly malicious.
// =============================================================================
PeerHandshake::Result PeerHandshake::perform(const Peer& peer,
                                             const std::vector<uint8_t>& infoHash,
                                             const string& ourPeerId,
                                             int timeoutSeconds) {
    Result result;

    try {
        TcpSocket socket;

        // Phase 4 prerequisite: peer addresses are plain IP literals, but
        // TcpSocket::connect() resolves them (or the hostname) via getaddrinfo.
        socket.connect(peer.ip, std::to_string(peer.port), timeoutSeconds);

        string handshake = buildHandshake(infoHash, ourPeerId);
        socket.sendAll(handshake.data(), handshake.size());

        // The reply can arrive in any number of chunks, so use recvExact.
        unsigned char reply[68];
        socket.recvExact(reply, 68);

        // 1. First byte must be 19 (the protocol name length).
        if (reply[0] != 19) {
            result.error = "bad first byte " + std::to_string(reply[0]);
            return result;
        }

        // 2. Bytes 1..19 must be the protocol name.
        if (string(reinterpret_cast<char*>(reply) + 1, 19) != "BitTorrent protocol") {
            result.error = "wrong protocol string";
            return result;
        }

        // 3. Bytes 28..47 must be OUR info_hash (this is the critical check!).
        if (!std::equal(infoHash.begin(), infoHash.end(), reply + 28)) {
            result.error = "info_hash mismatch - peer serves a different torrent";
            return result;
        }

        // 4. All good - grab the peer's 20-byte peer_id for display/logging.
        result.ok = true;
        result.peerId.assign(reinterpret_cast<char*>(reply) + 48, 20);
    } catch (const std::exception& e) {
        result.error = e.what();  // "could not connect", "timed out", ...
    }

    return result;
}