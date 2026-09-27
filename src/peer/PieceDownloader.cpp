// =============================================================================
// PieceDownloader.cpp - The piece-fetching conversation (Phase 5)
// =============================================================================

#include "peer/PieceDownloader.hpp"

#include "peer/PeerHandshake.hpp"
#include "peer/PeerMessage.hpp"
#include "net/TcpSocket.hpp"
#include "net/NetException.hpp"

#include <openssl/sha.h>

#include <algorithm>   // For std::equal
#include <cstddef>     // For size_t
#include <string>      // For std::string
#include <vector>      // For std::vector

// How many messages we'll read in a row while waiting for a specific one.
// A hostile peer could answer with an endless stream of chokes/haves; this
// cap makes "never trust the network" concrete: give up politely.
static constexpr int kMaxMessagesToWait = 200;

namespace {

// Wait for a particular message id (e.g. UNCHOKE) while tolerating the
// chatter that may come before it (HAVE, BITFIELD, CHOKE, ...). Returns true
// when the wanted message arrived.
bool waitFor(TcpSocket& sock, uint8_t wanted, PeerMessage& out, std::string& err) {
    for (int i = 0; i < kMaxMessagesToWait; i++) {
        PeerMessage msg = readMessage(sock);
        if (msg.id == wanted) {
            out = std::move(msg);
            return true;
        }
        // CHOKE before UNCHOKE is normal (the peer is careful); keep waiting.
        // Everything else (HAVE, BITFIELD, ...) is also just noise here.
    }
    err = "peer never sent the message we wanted";
    return false;
}

}  // namespace

PieceDownloader::Result PieceDownloader::download(const Peer& peer,
                                                  const std::vector<uint8_t>& infoHash,
                                                  const std::string& ourPeerId,
                                                  uint32_t pieceIndex,
                                                  size_t pieceLength,
                                                  const std::vector<uint8_t>& expectedHash,
                                                  int timeoutSeconds) {
    Result result;

    try {
        if (expectedHash.size() != 20) {
            result.error = "expectedHash must be 20 bytes";
            return result;
        }

        // 1. One connection for the whole conversation.
        TcpSocket socket;
        socket.connect(peer.ip, std::to_string(peer.port), timeoutSeconds);

        // 2. The 68-byte hello. If the peer doesn't serve this torrent, the
        //    info_hash check inside exchange() fails here and we stop.
        PeerHandshake::Result hs = PeerHandshake::exchange(socket, infoHash, ourPeerId);
        if (!hs.ok) {
            result.error = "handshake failed: " + hs.error;
            return result;
        }

        // 3. "I want your pieces." After this the peer should UNCHOKE us.
        sendMessage(socket, MSG_INTERESTED);

        // 4. Read messages until we are UNCHOKED (ignore BITFIELD/HAVE/CHOKE
        //    chatter). It might take a moment for the peer to process us.
        PeerMessage scratch;
        if (!waitFor(socket, MSG_UNCHOKE, scratch, result.error)) {
            return result;
        }

        // 5. Ask for the piece block by block. Blocks are kBlockSize (16 KiB)
        //    except the last one, which is whatever remains.
        std::vector<uint8_t> assembled;
        assembled.reserve(pieceLength);

        size_t begin = 0;
        while (begin < pieceLength) {
            size_t blockLen = std::min(kBlockSize, pieceLength - begin);
            std::string reqErr;
            PeerMessage piece;

            // Send one REQUEST: index, begin, length.
            sendMessage(socket, MSG_REQUEST,
                        buildRequestPayload(pieceIndex,
                                            static_cast<uint32_t>(begin),
                                            static_cast<uint32_t>(blockLen)));

            // Wait for the matching PIECE message.
            if (!waitFor(socket, MSG_PIECE, piece, reqErr)) {
                result.error = "waiting for piece " + std::to_string(begin) + ": " + reqErr;
                return result;
            }

            // Parse index/begin/data out of the PIECE payload.
            uint32_t gotIndex = 0, gotBegin = 0;
            const uint8_t* data = nullptr;
            size_t dataLen = 0;
            if (!parsePiecePayload(piece.payload, gotIndex, gotBegin, data, dataLen)) {
                result.error = "malformed PIECE payload";
                return result;
            }

            // "Never trust the network": the piece must be FOR OUR PIECE, at
            // the position we asked for, and exactly the size we asked for.
            if (gotIndex != pieceIndex || gotBegin != begin || dataLen != blockLen) {
                result.error = "peer sent unexpected block (index " +
                               std::to_string(gotIndex) + " begin " +
                               std::to_string(gotBegin) + " len " +
                               std::to_string(dataLen) + ")";
                return result;
            }

            assembled.insert(assembled.end(), data, data + dataLen);
            begin += blockLen;
        }

        // 6. The moment of truth: SHA-1 the assembled piece and compare it
        //    with what the torrent file promised.
        if (assembled.size() != pieceLength) {
            result.error = "assembled piece size mismatch";
            return result;
        }

        std::vector<uint8_t> actual(SHA_DIGEST_LENGTH);
        SHA1(assembled.data(), assembled.size(), actual.data());

        if (!std::equal(actual.begin(), actual.end(), expectedHash.begin())) {
            result.error = "SHA-1 verification failed - peer sent corrupted piece";
            return result;
        }

        result.ok = true;
        result.data = std::move(assembled);
    } catch (const std::exception& e) {
        result.error = e.what();  // connect / timeout / mid-read / framing errors
    }

    return result;
}