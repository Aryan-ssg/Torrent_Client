#pragma once

// =============================================================================
// TcpSocket.hpp - Our one TCP primitive, shared by every Phase
// =============================================================================
//
// Phase 3 (tracker) and Phase 4+ (peers) both need the exact same thing:
//   open a TCP connection, send bytes, receive bytes.
//
// Before this class existed, HttpTracker.cpp had its own private socket code.
// Peers are just more of the same, so we pull a small TcpSocket class out and
// reuse it everywhere. This is "don't repeat yourself" (DRY).
//
// Guarantees this class exists for:
//   - connect(): DNS lookup + TCP handshake, with a timeout so a dead peer
//                can't hang our program forever
//   - sendAll(): the OS may accept only PART of a send() call, so we loop
//   - recvExact(): "recv can return fewer bytes than you asked" is a core
//                BitTorrent lesson - this loops until we have exactly len
//
// JAVA COMPARISON:
// Java's Socket covers all of this automatically. In C++ there is no socket
// class in the standard library, so we write the ~100 lines ourselves.
// =============================================================================

#include <cstddef>  // For size_t
#include <string>   // For std::string
#include <sys/types.h>  // For ssize_t

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    // A socket owns an OS resource (the "file descriptor"), so copying would
    // be dangerous (two objects closing the same socket). C++ lets us forbid
    // copies and allow only moves - like a Java object that is not Cloneable.
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // Resolve host (DNS name or IP literal) and open a TCP connection.
    // On failure throws NetException (never leaves a dead socket object).
    void connect(const std::string& host, const std::string& port, int timeoutSeconds);

    // Send ALL len bytes, looping until send() accepts every byte.
    void sendAll(const void* data, size_t len);

    // Read EXACTLY len bytes, looping. Throws NetException if the other side
    // closes (or times out) before len bytes arrive.
    void recvExact(void* data, size_t len);

    // Read whatever is currently available (up to len bytes).
    // Returns: >0 bytes read, 0 = clean close (EOF), -1 = error/timeout.
    ssize_t recvSome(void* data, size_t len);

    // Exposed for OpenSSL, which needs the raw descriptor (Phase 3 TLS).
    int fd() const { return fd_; }

    void close();

private:
    int fd_ = -1;  // -1 means "no open socket"
};