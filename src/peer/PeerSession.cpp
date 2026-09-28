// =============================================================================
// PeerSession.cpp - One long-lived connection to one peer
// =============================================================================

#include "peer/PeerSession.hpp"

#include "peer/PeerHandshake.hpp"
#include "tracker/Peer.hpp"

#include <openssl/sha.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

// BEP 3 calls for a keep-alive roughly every two minutes. We send one a
// little under that, because the peer times US out if it stops hearing from
// us, and a session can legitimately sit idle between pieces while the
// manager decides what to fetch next.
constexpr int kKeepAliveIntervalSeconds = 100;

std::string hexOf(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

}  // namespace

// =============================================================================
// Lifetime
// =============================================================================
PeerSession::~PeerSession() {
    close();
}

void PeerSession::close() {
    socket_.close();
    connected_ = false;
    outstanding_.clear();
}

void PeerSession::abandon() {
    // Tell the peer we no longer want the blocks, so a well-behaved one stops
    // spending bandwidth on a leecher that has walked away. CANCEL is optional
    // in BEP 3 - peers must tolerate its absence - so a failure here is not
    // worth reporting; the connection is about to be reused or dropped anyway.
    if (socket_.fd() >= 0 && !outstanding_.empty()) {
        try {
            for (const auto& entry : outstanding_) {
                const uint32_t index = static_cast<uint32_t>(entry.first >> 32);
                const uint32_t begin = static_cast<uint32_t>(entry.first & 0xFFFFFFFFu);
                sendMessage(socket_, MSG_CANCEL,
                            buildRequestPayload(index, begin,
                                                static_cast<uint32_t>(entry.second.length)));
            }
        } catch (const std::exception&) {
            // Best effort only.
        }
    }
    outstanding_.clear();
}

// =============================================================================
// connect()
// =============================================================================
void PeerSession::connect(const Peer& peer, const Options& options) {
    close();

    if (options.infoHash == nullptr || options.infoHash->size() != 20) {
        throw std::runtime_error("PeerSession needs a 20-byte info hash");
    }
    if (options.pieceCount == 0) {
        throw std::runtime_error("PeerSession needs a piece count");
    }

    peer_ = peer;
    options_ = options;
    available_ = Bitfield(options.pieceCount);
    choked_ = false;
    lastError_.clear();
    piecesDelivered_ = 0;
    blocksReceived_ = 0;

    // 1. TCP. TcpSocket::connect() honours its own timeout via poll().
    socket_.connect(peer.ip, std::to_string(peer.port), options.connectTimeoutSeconds);

    // 2. The 68-byte handshake, and crucially the info-hash check inside it:
    //    a peer serving a different torrent is dropped here, before we waste
    //    a single block request on it.
    PeerHandshake::Result hs = PeerHandshake::exchange(socket_, *options.infoHash, options.peerId);
    if (!hs.ok) {
        socket_.close();
        throw std::runtime_error("handshake failed: " + hs.error);
    }

    // The read timeout for the rest of the session is deliberately much longer
    // than the connect timeout. Failing to CONNECT to a peer is expected in a
    // real swarm; a peer that connects and then goes quiet mid-download is
    // rare, and tearing such a session down turns a slow-but-working link
    // into a lost connection plus a restarted piece.
    struct timeval tv {};
    tv.tv_sec = options.readTimeoutSeconds;
    ::setsockopt(socket_.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(socket_.fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // 3. The peer's BITFIELD, then INTERESTED, then wait for UNCHOKE.
    //
    //    Peers send BITFIELD immediately after the handshake, but the spec
    //    does not actually require it, and some send HAVE messages first. So
    //    we read until we have a bitfield or UNCHOKE, treating everything else
    //    as chatter. A peer that sends neither is treated as owning nothing
    //    rather than everything - see Bitfield's header for why that default
    //    matters.
    bool gotBitfield = false;
    for (int i = 0; i < 32 && !gotBitfield; i++) {
        PeerMessage msg = readMessage(socket_);
        if (msg.id == MSG_BITFIELD) {
            if (!available_.parse(msg.payload, options.pieceCount)) {
                socket_.close();
                throw std::runtime_error("peer sent a malformed bitfield (" +
                                         std::to_string(msg.payload.size()) + " bytes for " +
                                         std::to_string(options.pieceCount) + " pieces)");
            }
            gotBitfield = true;
        } else if (msg.id == MSG_UNCHOKE) {
            // Some peers skip the bitfield and unchoke immediately. Fine: we
            // have no idea what they hold, so we will ask nothing until HAVE
            // messages tell us, which is safe (just slow).
            gotBitfield = true;
        }
        // anything else (HAVE, CHOKE) is chatter here
    }

    // 4. Ask. A peer with nothing to give will simply never unchoke us, and the
    //    read timeout above is what turns that into a clean failure.
    sendMessage(socket_, MSG_INTERESTED);

    for (int i = 0; i < 32; i++) {
        PeerMessage msg = readMessage(socket_);
        if (msg.id == MSG_UNCHOKE) {
            choked_ = false;
            connectedAt_ = std::chrono::steady_clock::now();
            lastWrite_ = connectedAt_;
            connected_ = true;
            return;
        }
        if (msg.id == MSG_BITFIELD && !gotBitfield) {
            if (available_.parse(msg.payload, options.pieceCount)) gotBitfield = true;
        }
        if (msg.id == MSG_CHOKE) choked_ = true;
    }

    socket_.close();
    throw std::runtime_error("peer never unchoked us");
}

void PeerSession::sendKeepAliveIfDue() {
    if (socket_.fd() < 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastWrite_ < std::chrono::seconds(kKeepAliveIntervalSeconds)) return;
    // A keep-alive is a zero-length message: the framing layer writes
    // length=0 and no id, and readMessage() skips it on the way in.
    sendMessage(socket_, static_cast<uint8_t>(0), std::vector<uint8_t>());
    lastWrite_ = now;
}

// =============================================================================
// pumpPiece() - the pipelined block loop
// =============================================================================
bool PeerSession::pumpPiece(size_t pieceIndex, size_t pieceLength, std::vector<uint8_t>& out,
                            std::string& error) {
    out.assign(pieceLength, 0);

    // The piece is cut into kBlockSize slices. The last one is short, exactly
    // as it is on disk and in the torrent's hash.
    const size_t blockCount = (pieceLength + kBlockSize - 1) / kBlockSize;
    std::vector<bool> blockSeen(blockCount, false);
    size_t received = 0;

    // Block offsets and lengths, precomputed once.
    auto offsetOf = [&](size_t block) { return block * kBlockSize; };
    auto lengthOf = [&](size_t block) {
        const size_t start = offsetOf(block);
        return std::min(kBlockSize, pieceLength - start);
    };

    outstanding_.clear();
    size_t nextToSend = 0;

    while (received < pieceLength) {
        // --- top the pipeline up -------------------------------------------
        while (nextToSend < blockCount && outstanding_.size() < options_.pipelineDepth) {
            const uint32_t begin = static_cast<uint32_t>(offsetOf(nextToSend));
            const uint32_t length = static_cast<uint32_t>(lengthOf(nextToSend));
            sendMessage(socket_, MSG_REQUEST,
                        buildRequestPayload(static_cast<uint32_t>(pieceIndex), begin, length));
            outstanding_[requestKey(static_cast<uint32_t>(pieceIndex), begin)] =
                Outstanding{nextToSend, lengthOf(nextToSend)};
            nextToSend++;
            lastWrite_ = std::chrono::steady_clock::now();
        }

        // Everything is requested and everything has arrived: done.
        if (nextToSend >= blockCount && outstanding_.empty()) break;

        // --- read one message ----------------------------------------------
        PeerMessage msg = readMessage(socket_);

        switch (msg.id) {
            case MSG_PIECE: {
                uint32_t gotIndex = 0;
                uint32_t gotBegin = 0;
                const uint8_t* data = nullptr;
                size_t dataLen = 0;
                if (!parsePiecePayload(msg.payload, gotIndex, gotBegin, data, dataLen)) {
                    error = "malformed PIECE payload";
                    outstanding_.clear();
                    return false;
                }

                const uint64_t key = requestKey(gotIndex, gotBegin);
                auto it = outstanding_.find(key);
                if (it == outstanding_.end()) {
                    // A block we never asked for. "Never trust the network":
                    // the SHA-1 gate would catch a corrupted result, but there
                    // is no reason to let a peer talk us into writing outside
                    // the buffer we allocated for this piece.
                    error = "peer sent an unrequested block";
                    outstanding_.clear();
                    return false;
                }

                // Validate the size against what we asked for, and against the
                // piece we are assembling. Either mismatch is a protocol
                // violation, and either one would otherwise let a peer write
                // past the end of our buffer.
                if (gotIndex != pieceIndex || dataLen != it->second.length ||
                    gotBegin + dataLen > pieceLength) {
                    error = "peer sent an oversized or mis-addressed block";
                    outstanding_.clear();
                    return false;
                }

                std::memcpy(out.data() + gotBegin, data, dataLen);
                blockSeen[it->second.blockIndex] = true;
                received += dataLen;
                blocksReceived_++;
                outstanding_.erase(it);
                if (progress_) progress_();
                break;
            }

            case MSG_CHOKE:
                // A fairness measure, not an error. Give the piece back and
                // keep the connection: this peer will very likely unchoke us.
                choked_ = true;
                outstanding_.clear();
                error = "peer choked us mid-piece";
                return false;

            case MSG_UNCHOKE:
                choked_ = false;
                break;

            case MSG_HAVE: {
                // The peer finished a piece. Useful even though we usually
                // have a full bitfield by now: it is how a long-lived session
                // stays accurate as the peer completes more of the file.
                if (msg.payload.size() >= 4) {
                    const uint32_t idx = (static_cast<uint32_t>(msg.payload[0]) << 24) |
                                         (static_cast<uint32_t>(msg.payload[1]) << 16) |
                                         (static_cast<uint32_t>(msg.payload[2]) << 8) |
                                         static_cast<uint32_t>(msg.payload[3]);
                    available_.set(idx);
                }
                break;
            }

            case MSG_BITFIELD: {
                // A re-send. Take it if it is well-formed, ignore it if not -
                // we already have a usable view and should not drop a working
                // session over a malformed refresh.
                Bitfield updated;
                if (updated.parse(msg.payload, options_.pieceCount)) {
                    available_ = std::move(updated);
                }
                break;
            }

            case MSG_NOT_INTERESTED:
                // The peer has nothing we want (any more). Worth noting; the
                // worker will find out soon enough by timing out.
                break;

            default:
                // REQUEST/CANCEL/whatever: we are a leecher, so a peer has no
                // business asking us for blocks. Ignore rather than disconnect.
                break;
        }
    }

    // Sanity: every block must have been filled exactly once. If the loop
    // exited because `received` reached the piece length, this holds by
    // construction; the check exists so a future change to the loop cannot
    // quietly hand back a piece with holes in it.
    for (size_t b = 0; b < blockCount; b++) {
        if (!blockSeen[b]) {
            error = "piece assembled with a missing block";
            return false;
        }
    }
    return true;
}

// =============================================================================
// fetchPiece()
// =============================================================================
bool PeerSession::fetchPiece(size_t pieceIndex,
                             const std::vector<uint8_t>& expectedHash,
                             std::vector<uint8_t>& out,
                             std::string& error) {
    error.clear();

    if (!usable()) {
        error = choked_ ? "peer is choked" : "session is not connected";
        return false;
    }
    if (expectedHash.size() != 20) {
        error = "expected hash is not 20 bytes";
        return false;
    }

    // Size of this piece. The last one is short unless the torrent happens to
    // divide evenly, so this is measured against the torrent's REAL total
    // length, not pieceCount * pieceLength - those differ by exactly the
    // amount the final piece is short, which is enough to make the block
    // request wrong and the SHA-1 fail on a download that is otherwise fine.
    const long long offset = static_cast<long long>(pieceIndex) * options_.pieceLength;
    if (offset < 0 || offset >= options_.totalLength) {
        error = "piece index out of range";
        return false;
    }
    const size_t len = static_cast<size_t>(
        std::min<long long>(options_.pieceLength, options_.totalLength - offset));

    sendKeepAliveIfDue();

    try {
        if (!pumpPiece(pieceIndex, len, out, error)) {
            return false;
        }
    } catch (const std::exception& e) {
        // A dead socket is terminal for this session; a choke already returned
        // above and left it reusable.
        error = e.what();
        lastError_ = error;
        close();
        return false;
    }

    // The SHA-1 gate. PieceManager verifies again before writing, but checking
    // here means corrupted bytes never cross the API boundary at all, and the
    // two independent checks catch a bug in either one.
    std::vector<uint8_t> actual(SHA_DIGEST_LENGTH);
    SHA1(out.data(), out.size(), actual.data());
    if (!std::equal(actual.begin(), actual.end(), expectedHash.begin())) {
        std::ostringstream oss;
        oss << "SHA-1 mismatch (expected " << hexOf(expectedHash) << ", got " << hexOf(actual) << ")";
        error = oss.str();
        lastError_ = error;
        // The connection itself is fine - the DATA was bad. Keep it.
        return false;
    }

    piecesDelivered_++;
    return true;
}
