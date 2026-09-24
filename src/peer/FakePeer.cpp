// =============================================================================
// FakePeer.cpp - The peer side of the handshake, as a loopback test server.
// =============================================================================

#include "peer/FakePeer.hpp"

#include <arpa/inet.h>     // sockaddr_in, inet_ntoa
#include <cstring>         // memcmp
#include <netinet/in.h>    // sockaddr_in
#include <sys/socket.h>    // socket, bind, listen, accept, send, recv
#include <unistd.h>        // close

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

}  // namespace

FakePeer::FakePeer(const std::vector<uint8_t>& infoHash, const std::string& serverPeerId)
    : infoHash_(infoHash), serverPeerId_(serverPeerId) {}

FakePeer::~FakePeer() {
    if (listenFd_ >= 0) ::close(listenFd_);
    if (thread_.joinable()) thread_.join();
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
    // We only need one connection for the test; when it's done we exit.
    struct sockaddr_in clientAddr {};
    socklen_t clen = sizeof(clientAddr);
    int clientFd = ::accept(listenFd_,
                            reinterpret_cast<struct sockaddr*>(&clientAddr),
                            &clen);
    if (clientFd >= 0) {
        handleConnection(clientFd);
        ::close(clientFd);
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

    size_t sent = 0;
    while (sent < reply.size()) {
        ssize_t n = ::send(clientFd, reply.data() + sent, reply.size() - sent, 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void FakePeer::join() {
    if (thread_.joinable()) thread_.join();
}