#pragma once

// =============================================================================
// FakePeer.hpp - A tiny local peer for testing the handshake WITHOUT network
// =============================================================================
//
// Real BitTorrent peers on the internet are unreachable from this machine
// (the network only allows outgoing ports 80/443, and peers listen on
// 6881/51413/etc.). So to prove PeerHandshake actually works, we stand up a
// fake peer on 127.0.0.1 that behaves exactly like a real peer over the wire:
//
//   - listens for one TCP connection
//   - reads the client's 68-byte handshake
//   - checks the layout + info_hash
//   - replies with its own valid 68-byte handshake
//
// It also previews Phase 8: the "server side" (socket / bind / listen /
// accept) that we'll use later to let real peers download from us.
//
// The accept loop runs on a separate std::thread so the test can block on
// connect() while the server keeps listening.
// =============================================================================

#include <cstdint>   // For uint16_t, size_t
#include <limits>    // For std::numeric_limits
#include <string>    // For std::string
#include <thread>    // For std::thread
#include <vector>    // For std::vector

class FakePeer {
public:
    // serverPeerId must be EXACTLY 20 bytes (a valid BitTorrent peer_id).
    FakePeer(const std::vector<uint8_t>& infoHash, const std::string& serverPeerId);

    // Phase 5 seeder mode: also serve `content` as a torrent whose piece size
    // is `pieceLength`. The client can then run the FULL download conversation
    // (handshake -> interested -> unchoke -> request -> piece) on loopback.
    FakePeer(const std::vector<uint8_t>& infoHash, const std::string& serverPeerId,
             const std::vector<uint8_t>& content, size_t pieceLength);

    ~FakePeer();

    // Bind + listen on 127.0.0.1 (OS picks a free port) and spawn the thread.
    void start();

    // Phase 4/5 handled ONE connection. Phase 6's download loop dials the
    // seeder once PER piece, so tell the server how many visitors to expect.
    // (It exits cleanly once that many have been served.)
    void setMaxConnections(int n) { maxConnections_ = n; }

    // Block until all expected connections have been handled.
    void join();

    uint16_t port() const { return port_; }

    // 1 if a client sent a valid handshake for our info_hash, else 0.
    int accepted() const { return accepted_; }

    // Corrupt the piece data: flips one byte at file offset `index`. Use this
    // to prove the client's SHA-1 check really rejects bad data.
    void destroyByte(size_t index) { destroyedByte_ = index; }

private:
    void acceptLoop();        // accept() one connection
    void handleConnection(int clientFd);  // verify + reply to the handshake
    void handleSeeder(int clientFd);      // Phase 5: serve pieces

    std::vector<uint8_t> infoHash_;  // the torrent we pretend to serve
    std::string serverPeerId_;       // the peer_id we announce with
    int listenFd_ = -1;
    uint16_t port_ = 0;
    int accepted_ = 0;
    std::thread thread_;

    // Phase 5 seeder state (empty content = handshake-only mode).
    std::vector<uint8_t> content_;
    size_t pieceLength_ = 0;
    bool seeder_ = false;
    size_t destroyedByte_ = std::numeric_limits<size_t>::max();

    // How many incoming connections to serve before the thread exits.
    int maxConnections_ = 1;
};