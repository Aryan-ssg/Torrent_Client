// =============================================================================
// TcpSocket.cpp - POSIX sockets behind a small, safe C++ wrapper
// =============================================================================

#include "net/TcpSocket.hpp"

#include "net/NetException.hpp"

#include <cerrno>       // For errno
#include <cstring>      // For std::strerror
#include <fcntl.h>      // fcntl, F_GETFL, F_SETFL, O_NONBLOCK (non-blocking connect)
#include <netdb.h>      // getaddrinfo, gai_strerror
#include <poll.h>       // poll (our real connect timeout)
#include <sys/socket.h> // socket, connect, send, recv, getsockopt, SO_ERROR
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
// TIME-OUT: we need a real one, because trackers hand out plenty of
// black-holed peers (firewalled home routers that swallow SYMs). The obvious
// approach - setsockopt(SO_RCVTIMEO/SO_SNDTIMEO) then a blocking connect() -
// DOES NOT WORK: on Linux those two options apply to send()/recv() only, and
// never to connect(). A blocking connect() to a black hole therefore sits in
// the kernel for the whole TCP SYN-retry window (~130s with the default
// tcp_syn_retries=6) no matter what we ask for. Twenty dead peers would then
// take the best part of an hour.
//
// So we do it the way everyone really does it:
//   1. flip the socket to non-blocking, so connect() returns straight away
//      with EINPROGRESS (or an immediate hard error like ECONNREFUSED);
//   2. poll() for POLLOUT with OUR deadline;
//   3. ask SO_ERROR whether the handshake actually succeeded;
//   4. flip the socket back to blocking for the rest of its life.
// SO_RCVTIMEO/SO_SNDTIMEO are still set below because they DO work for the
// send()/recv() calls that come afterwards.
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

    std::string lastError = "no route to host";

    for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastError = std::strerror(errno);
            continue;
        }

        // These two DO apply to send()/recv(), so keep them for later.
        struct timeval tv {};
        tv.tv_sec = timeoutSeconds;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // 1. Non-blocking mode, so connect() cannot park in the kernel.
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            lastError = std::strerror(errno);
            ::close(fd);
            continue;
        }

        // 2. Kick off the handshake.
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            int err = errno;
            if (err != EINPROGRESS) {
                // A hard, immediate failure (refused / unreachable / no
                // route). Nothing to wait for - try the next address.
                lastError = std::strerror(err);
                ::close(fd);
                continue;
            }

            // 3. EINPROGRESS: the SYN is on the wire. Wait for it - but only
            //    for as long as the caller allowed.
            struct pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, timeoutSeconds * 1000);
            if (pr == 0) {
                lastError = "timed out after " + std::to_string(timeoutSeconds) + "s";
                ::close(fd);
                continue;
            }
            if (pr < 0) {
                lastError = std::strerror(errno);
                ::close(fd);
                continue;
            }

            // 4. "Writable" only means the handshake FINISHED - it does not
            //    mean it SUCCEEDED. SO_ERROR carries the real verdict.
            int soError = 0;
            socklen_t soLen = sizeof(soError);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen) < 0 || soError != 0) {
                lastError = std::strerror(soError != 0 ? soError : errno);
                ::close(fd);
                continue;
            }
        }

        // 5. Back to blocking: send()/recv() (and the SO_*TIMEO options we
        //    set above) assume an ordinary blocking socket.
        int blocking = ::fcntl(fd, F_GETFL, 0);
        if (blocking >= 0) {
            ::fcntl(fd, F_SETFL, blocking & ~O_NONBLOCK);
        }

        fd_ = fd;   // keep this one
        break;
    }
    ::freeaddrinfo(results);  // release the DNS results list

    if (fd_ < 0) {
        throw NetException("Could not connect to " + host + ":" + port + " (" + lastError + ")");
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