#pragma once

// =============================================================================
// PeerSession.hpp - One long-lived connection to one peer
// =============================================================================
//
// WHY THIS CLASS EXISTS
// ---------------------
// Phases 5-7 could fetch a piece, and fetch many pieces in parallel, but they
// did it the expensive way: for EVERY piece they opened a fresh TCP
// connection, did a 68-byte handshake, sent INTERESTED, waited for UNCHOKE,
// pulled the piece, and hung up. Then the next piece started from scratch.
//
// For a small torrent that is merely wasteful. For a real one it is fatal:
// the Ubuntu ISO is 23,664 pieces, so that design means 23,664 connections
// and 23,664 handshakes, and roughly 470 sequential requests aimed at each of
// the ~50 peers a tracker hands out. Real clients keep the connection and
// move many pieces over it.
//
// So a PeerSession is exactly that: a connection that stays open, knows what
// the peer has, and can be asked for piece after piece until it dies.
//
// WHAT IT KNOWS ABOUT THE PEER
// ----------------------------
//   - its BITFIELD: which pieces it has. Without this, asking for piece N
//     from a peer that does not own it is not an error, it is a STALL - the
//     request is simply never answered, and you sit there until the read
//     timeout fires.
//   - whether it is currently CHOKED. Real peers choke and unchoke
//     constantly as a fairness measure. A choke aborts the piece in flight
//     but does NOT kill the connection.
//
// PIPELINING
// ----------
// Asking for one 16 KiB block and waiting for it means paying a full network
// round trip per block - a 256 KiB piece is 16 round trips, and on a link
// with 100ms of latency that is 1.6 seconds of pure waiting. So we keep
// several requests in flight at once and match each arriving PIECE back to
// the block that asked for it. A peer is allowed to answer out of order, and
// may legitimately deliver blocks of a piece in any sequence, so matching by
// (index, begin) is required for correctness, not just an optimisation.
//
// Java parallel: like a pooled HTTP connection that keeps its keep-alive and
// its own response bookkeeping, rather than a new one per request.
// =============================================================================

#include "net/TcpSocket.hpp"
#include "peer/Bitfield.hpp"
#include "peer/PeerMessage.hpp"
#include "tracker/Peer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class PeerSession {
public:
    struct Options {
        const std::vector<uint8_t>* infoHash = nullptr;
        std::string peerId;
        size_t pieceCount = 0;
        long long pieceLength = 0;
        // The torrent's real total size. Needed because the LAST piece is
        // almost never a full pieceLength - a 6 GB ISO's final piece is
        // whatever is left over - and deriving that from pieceCount *
        // pieceLength silently over-reports it, so the last piece is requested
        // at the wrong size and the hash never matches.
        long long totalLength = 0;
        int connectTimeoutSeconds = 10;
        // How long to wait for the next message before declaring the peer
        // dead. Generous on purpose: a peer on a slow link is healthy, and
        // we would rather wait than throw away a working connection.
        int readTimeoutSeconds = 30;
        // Block requests allowed in flight at once.
        size_t pipelineDepth = 8;
    };

    // Called after every block that arrives, so the caller can keep a stuck
    // claim's clock alive while data is genuinely flowing.
    using ProgressFn = std::function<void()>;

    PeerSession() = default;
    ~PeerSession();

    PeerSession(const PeerSession&) = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    // Connect, handshake, read the peer's bitfield, say INTERESTED and wait
    // for UNCHOKE. Throws std::runtime_error if any of that fails; a session
    // that failed to come up must not be used.
    void connect(const Peer& peer, const Options& options);

    // True while the connection is up and the peer has not choked us.
    bool usable() const { return socket_.fd() >= 0 && !choked_; }

    bool choked() const { return choked_; }
    const Peer& peer() const { return peer_; }

    // What the peer says it has. Empty (owns nothing) until the bitfield
    // arrives, which is deliberate: a peer we know nothing about is one we
    // must not request anything from.
    const Bitfield& available() const { return available_; }

    // Download one whole piece: queue its blocks, pipeline them, reassemble,
    // and verify the SHA-1 against `expectedHash` before handing it back.
    //
    // Returns false on choke, dead socket, a malformed reply, or a hash
    // mismatch. In every failure case the piece is abandoned and the session
    // is left in a defined state (a choke leaves it reusable; a dead socket
    // does not).
    bool fetchPiece(size_t pieceIndex,
                    const std::vector<uint8_t>& expectedHash,
                    std::vector<uint8_t>& out,
                    std::string& error);

    // Drop the piece in flight and stop reading. Used when the download is
    // aborted, so a worker is not left blocking on a peer.
    void abandon();

    // Close the connection. Safe to call twice; the destructor does it too.
    void close();

    // Called as blocks arrive, for keep-alive bookkeeping.
    void setProgressFn(ProgressFn fn) { progress_ = std::move(fn); }

    // --- diagnostics, for the UI and the tests ---
    size_t piecesDelivered() const { return piecesDelivered_; }
    size_t blocksReceived() const { return blocksReceived_; }
    double secondsConnected() const {
        return connectedAt_.time_since_epoch().count() == 0
                   ? 0.0
                   : std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                  connectedAt_)
                         .count();
    }
    const std::string& lastError() const { return lastError_; }

private:
    // One outstanding REQUEST, and where its data will land.
    struct Outstanding {
        size_t blockIndex = 0;  // which slice of the piece this is
        size_t length = 0;
    };

    static uint64_t requestKey(uint32_t index, uint32_t begin) {
        return (static_cast<uint64_t>(index) << 32) | begin;
    }

    // Read messages until the piece is whole, the peer chokes, or the socket
    // dies. Returns true only if every byte arrived.
    bool pumpPiece(size_t pieceIndex, size_t pieceLength, std::vector<uint8_t>& out,
                   std::string& error);

    void sendKeepAliveIfDue();

    TcpSocket socket_;
    Peer peer_;
    Options options_;
    Bitfield available_;
    bool choked_ = false;
    bool connected_ = false;
    std::string lastError_;
    std::chrono::steady_clock::time_point connectedAt_{};
    std::chrono::steady_clock::time_point lastWrite_{};
    size_t piecesDelivered_ = 0;
    size_t blocksReceived_ = 0;
    ProgressFn progress_;

    // Blocks in flight for the piece currently being fetched.
    std::unordered_map<uint64_t, Outstanding> outstanding_;
};
