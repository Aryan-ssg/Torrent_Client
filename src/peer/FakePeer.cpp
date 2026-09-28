// =============================================================================
// FakePeer.cpp - The peer side of the protocol, as a loopback test server.
// =============================================================================

#include "peer/FakePeer.hpp"

#include "peer/PeerMessage.hpp"

#include <arpa/inet.h>     // sockaddr_in, inet_ntoa
#include <cstring>         // memcmp
#include <netinet/in.h>    // sockaddr_in
#include <sys/socket.h>    // socket, bind, listen, accept, send, recv
#include <unistd.h>        // close

#include <algorithm>       // std::min
#include <atomic>         // std::atomic (shutdown flag)
#include <chrono>          // std::chrono::milliseconds (Phase 7 latency)
#include <thread>          // std::this_thread::sleep_for (Phase 7 latency)

namespace {

// Read exactly `n` bytes from a socket (TCP has no message boundaries, so a
// single recv() may return less). Returns false if the peer closes early.
bool readExact(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// Send ALL of `n` bytes (the server-side twin of sendAll()).
bool sendAll(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, p + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// Send one length-prefixed peer message: [4-byte length][id][payload].
// This is the server-side mirror of sendMessage() in PeerMessage.cpp — we
// write the same bytes, but on a raw file descriptor instead of a TcpSocket.
bool sendFrame(int fd, uint8_t id, const std::vector<uint8_t>& payload) {
    uint32_t length = 1 + static_cast<uint32_t>(payload.size());
    uint8_t header[5];
    header[0] = static_cast<uint8_t>(length >> 24);
    header[1] = static_cast<uint8_t>(length >> 16);
    header[2] = static_cast<uint8_t>(length >> 8);
    header[3] = static_cast<uint8_t>(length);
    header[4] = id;

    if (!sendAll(fd, header, 5)) return false;
    if (!payload.empty() && !sendAll(fd, payload.data(), payload.size())) return false;
    return true;
}

// Read one peer message from a raw fd, honoring the length prefix and
// skipping keep-alives. Returns false on close/error or absurd length.
// The server-side twin of readMessage().
bool readFrame(int fd, uint8_t& id, std::vector<uint8_t>& payload) {
    for (int tries = 0; tries < 16; tries++) {
        uint8_t lenBytes[4];
        if (!readExact(fd, lenBytes, 4)) return false;
        uint32_t length = (static_cast<uint32_t>(lenBytes[0]) << 24) |
                          (static_cast<uint32_t>(lenBytes[1]) << 16) |
                          (static_cast<uint32_t>(lenBytes[2]) << 8) |
                          (static_cast<uint32_t>(lenBytes[3]));
        if (length == 0) continue;           // keep-alive
        if (length < 1 || length > kMaxPeerMessageSize) return false;

        std::vector<uint8_t> body(length);
        if (!readExact(fd, body.data(), length)) return false;
        id = body[0];
        payload.assign(body.begin() + 1, body.end());
        return true;
    }
    return false;
}

}  // namespace

FakePeer::FakePeer(const std::vector<uint8_t>& infoHash, const std::string& serverPeerId)
    : infoHash_(infoHash), serverPeerId_(serverPeerId) {}

FakePeer::FakePeer(const std::vector<uint8_t>& infoHash, const std::string& serverPeerId,
                   const std::vector<uint8_t>& content, size_t pieceLength)
    : infoHash_(infoHash), serverPeerId_(serverPeerId),
      content_(content), pieceLength_(pieceLength), seeder_(true) {}

FakePeer::~FakePeer() {
    // Wake the parked accept() BEFORE closing the descriptor, and join. Doing
    // it the other way round is the classic bug: close() does not reliably
    // unblock a thread already sitting in accept(), and the descriptor number
    // can be recycled, so join() would hang - or the accept would end up
    // watching a stranger's socket.
    shutdown();
    if (thread_.joinable()) thread_.join();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

// =============================================================================
// start()
// =============================================================================
// Phase 8 preview: the server-side socket trinity is
//   socket() -> bind() -> listen()
// then accept() per incoming connection. Port 0 tells the OS "pick any free
// port for me"; we read it back with getsockname().
// =============================================================================
void FakePeer::start() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);

    // SO_REUSEADDR: if a previous test left the port in TIME_WAIT, let us
    // still bind. (A real client does this so restarts aren't painful.)
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
    addr.sin_port = 0;                              // OS chooses the port

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd_, 4) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }

    // Ask the OS which port it actually gave us.
    socklen_t len = sizeof(addr);
    getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);

    // Let the accept loop run in the background.
    thread_ = std::thread(&FakePeer::acceptLoop, this);
}

void FakePeer::acceptLoop() {
    // Serve up to maxConnections_ visitors. We exit after the last one so
    // join() can return.
    //
    // The shutdown_ check is not decoration: shutdown() wakes a parked
    // accept() by connecting to ourselves, and this loop has to notice.
    for (int served = 0; served < maxConnections_; served++) {
        struct sockaddr_in clientAddr {};
        socklen_t clen = sizeof(clientAddr);
        int clientFd = ::accept(listenFd_,
                                reinterpret_cast<struct sockaddr*>(&clientAddr),
                                &clen);
        if (clientFd < 0) return;   // listen socket closed (destructor)

        if (shutdown_.load()) {
            // Woken by shutdown(). Do not talk to the poke; just leave.
            ::close(clientFd);
            return;
        }

        activeClientFd_.store(clientFd);
        handleConnection(clientFd);
        activeClientFd_.store(-1);
        ::close(clientFd);
        servedCount_++;
    }
}

// =============================================================================
// shutdown()
// =============================================================================
// Make join() able to return even though the accept thread is parked in
// accept() waiting for a visitor that will never come.
//
// Closing the listening descriptor is NOT enough, and is actually dangerous:
// on Linux, close() does not reliably wake a thread already blocked in
// accept(), and the descriptor number can be recycled underneath it, so the
// accept can end up watching somebody else's socket.
//
// The portable way to wake a parked accept() is to give it something to
// accept: connect to ourselves once, from this same process, on a throwaway
// socket. accept() returns that connection, the loop sees shutdown_ is set,
// and exits. The listen socket is only closed afterwards, on a thread that is
// no longer blocked in it.
// =============================================================================
void FakePeer::shutdown() {
    if (shutdown_.exchange(true)) return;  // already asked to stop
    if (listenFd_ < 0) return;

    // A peer that is mid-conversation is blocked in read() on a client socket,
    // not parked in accept(), so poking the listener below would never reach
    // it. shutdown(SHUT_RDWR) is the call that actually wakes a blocked
    // recv(); close() does not reliably, for the same reason closing a listen
    // fd does not wake accept().
    const int clientFd = activeClientFd_.exchange(-1);
    if (clientFd >= 0) {
        ::shutdown(clientFd, SHUT_RDWR);
    }

    // The wake-up connection. It never completes a handshake - acceptLoop
    // checks shutdown_ before talking to anyone - so an empty connect is
    // enough. Ignore the result: a failure here just means the accept was
    // not actually parked.
    int waker = ::socket(AF_INET, SOCK_STREAM, 0);
    if (waker >= 0) {
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(waker, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        ::close(waker);
    }
}

// =============================================================================
// handleConnection()
// =============================================================================
// Real peer behavior: read our client's 68-byte handshake, sanity-check it,
// and if it's for our torrent, answer with our own handshake. The reply has
// the SAME layout http://PeerHandshake uses, with the roles of info_hash and
// peer_id swapped:
//
//   [0..19]      19 + "BitTorrent protocol"
//   [20..27]     reserved zeros
//   [28..47]     the torrent's info_hash (must match what the client wants)
//   [48..67]     our peer_id
// =============================================================================
void FakePeer::handleConnection(int clientFd) {
    unsigned char request[68];
    if (!readExact(clientFd, request, 68)) return;

    // Reject garbage: wrong first byte, or wrong protocol string.
    if (request[0] != 19) return;
    if (std::memcmp(request + 1, "BitTorrent protocol", 19) != 0) return;

    // The real acceptance test: the connecting client must want OUR torrent.
    // This mirrors PeerHandshake's info_hash check on the other side.
    if (std::memcmp(request + 28, infoHash_.data(), 20) != 0) return;

    accepted_ = 1;

    // Build the reply handshake (68 bytes).
    std::string reply;
    reply += static_cast<char>(19);
    reply += "BitTorrent protocol";
    reply.append(8, '\0');
    reply.append(infoHash_.begin(), infoHash_.end());
    reply += serverPeerId_;

    if (!sendAll(clientFd, reply.data(), reply.size())) return;

    // Phase 5: in seeder mode, keep talking (piece protocol) after the
    // handshake. Otherwise we're done, like Phase 4's simple test.
    if (seeder_) {
        handleSeeder(clientFd);
    }
}

// =============================================================================
// handleSeeder()
// =============================================================================
// Pretend to be a well-behaved seeder for one client:
//
//   1. right after the handshake, send our BITFIELD (all pieces owned)
//   2. when the client says INTERESTED, answer UNCHOKE
//   3. when the client sends REQUEST(index, begin, length), reply with
//      PIECE(index, begin, data) for that slice of the content
//
// This is the exact wire behaviour a real seeder uses, minus the cleverness
// (choking algorithms, out-of-order serving, etc. come in Phase 7/8).
// =============================================================================
void FakePeer::handleSeeder(int clientFd) {
    // Phase 7: optional artificial latency, so a timing test can observe that
    // several peers (each in its own thread) finish sooner than one peer
    // alone. A real slow modem peer "thinks" exactly like this.
    if (serveDelayMs_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(serveDelayMs_));
    }

    // --- Build the BITFIELD: one bit per piece, MSB-first within each byte.
    size_t numPieces = (content_.size() + pieceLength_ - 1) / pieceLength_;
    std::vector<uint8_t> bitfield((numPieces + 7) / 8, 0);
    for (size_t p = 0; p < numPieces; p++) {
        bitfield[p / 8] |= static_cast<uint8_t>(0x80 >> (p % 8));
    }
    if (!sendFrame(clientFd, MSG_BITFIELD, bitfield)) return;

    uint8_t id = 0;
    std::vector<uint8_t> payload;
    while (readFrame(clientFd, id, payload)) {
        if (id == MSG_INTERESTED) {
            // Grant access. (Real clients choke/unchoke for fairness; we are
            // generously always unchoked.)
            if (!sendFrame(clientFd, MSG_UNCHOKE, {})) return;
        } else if (id == MSG_REQUEST) {
            uint32_t index = 0, begin = 0, length = 0;
            if (!parseRequestPayload(payload, index, begin, length)) return;

            // Bounds-check the request against our content exactly; a request
            // outside the file (or longer than a block) is an error.
            uint64_t offset = static_cast<uint64_t>(index) * pieceLength_ + begin;
            if (offset + length > content_.size() || length > kBlockSize) {
                return;  // malformed request -> just stop talking
            }

            // Serve the slice. (destroyByte() flag lets tests prove the
            // client's SHA-1 check catches corruption.)
            std::vector<uint8_t> slice(content_.begin() + offset,
                                       content_.begin() + offset + length);
            if (destroyedByte_ != std::numeric_limits<size_t>::max() &&
                destroyedByte_ >= offset && destroyedByte_ < offset + length) {
                slice[destroyedByte_ - offset] ^= 0xFF;  // one flipped byte
            }

            if (!sendFrame(clientFd, MSG_PIECE,
                           buildPiecePayload(index, begin, slice.data(), slice.size()))) {
                return;
            }
        }
        // Other messages (HAVE, CANCEL, ...) are irrelevant to a generous
        // fake seeder; keep reading.
    }
}

void FakePeer::join() {
    if (thread_.joinable()) thread_.join();
}