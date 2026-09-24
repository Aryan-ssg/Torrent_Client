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

#include <cstdint>   // For uint16_t
#include <string>    // For std::string
#include <thread>    // For std::thread
#include <vector>    // For std::vector

class FakePeer {
public:
    // serverPeerId must be EXACTLY 20 bytes (a valid BitTorrent peer_id).
    FakePeer(const std::vector<uint8_t>& infoHash, const std::string& serverPeerId);
    ~FakePeer();

    // Bind + listen on 127.0.0.1 (OS picks a free port) and spawn the thread.
    void start();

    // Block until the single connection has been handled.
    void join();

    uint16_t port() const { return port_; }

    // 1 if a client sent a valid handshake for our info_hash, else 0.
    int accepted() const { return accepted_; }

private:
    void acceptLoop();        // accept() one connection
    void handleConnection(int clientFd);  // verify + reply to the handshake

    std::vector<uint8_t> infoHash_;  // the torrent we pretend to serve
    std::string serverPeerId_;       // the peer_id we announce with
    int listenFd_ = -1;
    uint16_t port_ = 0;
    int accepted_ = 0;
    std::thread thread_;
};