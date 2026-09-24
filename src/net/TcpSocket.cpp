// =============================================================================
// TcpSocket.cpp - POSIX sockets behind a small, safe C++ wrapper
// =============================================================================

#include "net/TcpSocket.hpp"

#include "net/NetException.hpp"

#include <cerrno>       // For errno
#include <cstring>      // For std::strerror
#include <netdb.h>      // getaddrinfo, gai_strerror
#include <sys/socket.h> // socket, connect, send, recv
#include <sys/time.h>   // struct timeval (for SO_RCVTIMEO)
#include <unistd.h>     // close

TcpSocket::~TcpSocket() {
    close();
}

// =============================================================================
// Move semantics: steal the source's file descriptor and mark the source as
// empty. The stolen descriptor must then be freed by OUR destructor instead.
// =============================================================================
TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : fd_(other.fd_) {
    other.fd_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();                    // free what we currently hold
        fd_ = other.fd_;            // steal the other's descriptor
        other.fd_ = -1;
    }
    return *this;
}

// =============================================================================
// connect()
// =============================================================================
// getaddrinfo() is C's swiss-army-knife resolver: give it a hostname OR an
// IP literal and it returns every usable address (IPv4 + IPv6), like
// Java's InetAddress.getAllByName(). We try each one until a connect() works.
//
// The SO_RCVTIMEO / SO_SNDTIMEO socket options turn a black-hole peer (that
// never answers) into a -1 return value instead of an infinite hang. Trackers
// hand out plenty of dead/firewalled peers, so this is mandatory.
// =============================================================================
void TcpSocket::connect(const std::string& host, const std::string& port, int timeoutSeconds) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP

    struct addrinfo* results = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (rc != 0) {
        throw NetException("DNS lookup failed for " + host + ": " + gai_strerror(rc));
    }

    for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv {};
        tv.tv_sec = timeoutSeconds;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd_ = fd;   // keep this one
            break;
        }
        ::close(fd);    // this address didn't work, keep trying
    }
    ::freeaddrinfo(results);  // release the DNS results list

    if (fd_ < 0) {
        throw NetException("Could not connect to " + host + ":" + port);
    }
}

// =============================================================================
// sendAll()
// =============================================================================
// A send() call is not required to transmit everything at once. Even a few
// hundred bytes can be split up. So "send all" is really a loop.
// =============================================================================
void TcpSocket::sendAll(const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd_, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;  // signal interrupted us, retry
            throw NetException("send() failed: " + std::string(std::strerror(errno)));
        }
        sent += static_cast<size_t>(n);
    }
}

// =============================================================================
// recvSome()
// =============================================================================
// One single attempt to receive. Returns 0 if the peer half-closes cleanly,
// -1 if it errored or timed out, or the number of bytes read (perhaps fewer
// than `len`). Most of BitTorrent reads use recvExact() on top of this.
// =============================================================================
ssize_t TcpSocket::recvSome(void* data, size_t len) {
    ssize_t n = ::recv(fd_, data, len, 0);
    if (n < 0) {
        if (errno == EINTR) return recvSome(data, len);
        return -1;
    }
    return n;
}

// =============================================================================
// recvExact()
// =============================================================================
// TCP is a STREAM: there are no message boundaries. 68 handshake bytes sent
// by the peer may arrive in one recv(), five recv()s, or any split in
// between. So code everywhere in BitTorrent looks like this:
//
//   size_t got = 0;
//   while (got < wanted) {
//       int n = recv(..., wanted - got);
//       got += n;   // n may be less than (wanted - got)!
//   }
// =============================================================================
void TcpSocket::recvExact(void* data, size_t len) {
    char* p = static_cast<char*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recvSome(p + got, len - got);
        if (n <= 0) {
            throw NetException("Connection closed or timed out mid-read");
        }
        got += static_cast<size_t>(n);
    }
}

void TcpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}