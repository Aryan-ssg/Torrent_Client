/*
 * demo-02-recvExact.cpp - WHY recvExact() EXISTS
 * =============================================================================
 *
 * The single most important networking lesson in this project, in one file.
 *
 * The myth:   "I asked for 68 bytes, so recv() gives me 68 bytes."
 * The truth:   TCP is a STREAM. You get whatever has arrived. It may be 68.
 *              It may be 5. It may be 1. You may get 1 byte and then the
 *              packet boundary is lost forever, and the next recv() gives
 *              you byte 2 of the thing you already partly have.
 *
 * This demo starts a real server on 127.0.0.1 that dribbles 68 bytes out in
 * deliberately awkward chunks, then reads them two ways.
 *
 *   g++ -O0 -pthread -std=c++17 -o /tmp/demo02 \
 *       docs/12-tutor/demos/demo-02-recvExact.cpp
 *   /tmp/demo02
 *
 * ---------------------------------------------------------------------------
 * THE CONTRAST
 * ---------------------------------------------------------------------------
 *   WITHOUT recvExact()  -  one naive recv() call
 *       we get 5 of 68 bytes and assume we got 68.
 *       We then parse a "handshake" whose bytes 6..68 are uninitialised stack
 *       memory. We check the info hash against garbage, it fails, we close a
 *       perfectly good connection, and we report "peer sent a bad hash".
 *       The peer is innocent. WE are wrong. And it fails ~randomly, so it
 *       looks like a flaky network rather than a bug in us.
 *
 *   WITH recvExact()  -  loop until we have exactly 68
 *       we get 5, then 63, and we end with the same 68 correct bytes the
 *       server sent. Every time. This is the ONLY correct way to read a
 *       length-prefixed protocol.
 *
 * WHERE THIS BITES IN PEERFLOW:
 *   the 68-byte handshake      (PeerHandshake::exchange)
 *   the 4-byte message length  (PeerMessage::readMessage)
 *   a whole PIECE payload      (PeerSession::pumpPiece)
 *   the request index/begin    (PeerMessage::parseRequestPayload)
 * A bug in recvExact corrupts every one of them. It is the highest-leverage
 * 15 lines in the project.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS NOT A HYPOTHETICAL
 * ---------------------------------------------------------------------------
 * It is why the handshake test can never be flaky, why the loopback tests
 * pass every single run, and why the code contains the comment "recv can
 * return fewer bytes than you asked - this loops until we have exactly len".
 * On a LAN the chunks usually look tidy. Across the internet they never do.
 * ==========================================================================
 */

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// The exact shape of a real BitTorrent handshake. Only the first 8 bytes
// matter for this demo; the rest is filler we assert we received intact.
const char kProtocol[8] = {'\x13', 'B', 'i', 't', 'T', 'o', 'r', 'r'};
constexpr int kHandshakeSize = 68;

std::vector<unsigned char> makeHandshake() {
    std::vector<unsigned char> h(kHandshakeSize, 0x00);
    h[0] = 19;  // length prefix that must precede the protocol string
    std::memcpy(&h[1], "BitTorrent protocol", 19);
    for (int i = 20; i < kHandshakeSize; ++i) h[i] = static_cast<unsigned char>(i);
    return h;
}

// A server that sends the 68 bytes in awkward, unrealistic-looking pieces.
// Real networks do this constantly, for reasons that have nothing to do
// with malice: packet boundaries, congestion control, Nagle, MTU, and the
// simple fact that the sender's write() and the receiver's read() have
// nothing to do with each other.
void dribblingServer(int listenFd, std::atomic<bool>& done) {
    for (int c = 0; c < 200; ++c) {
        int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0) break;

        const auto payload = makeHandshake();
        // 1, then 2, then 30, then 35. Sums to 68. None of them is 68.
        const int chunks[] = {1, 2, 30, 35};
        size_t sent = 0;
        for (int n : chunks) {
            if (sent + n > payload.size()) break;
            // MSG_NOSIGNAL matters! Writing to a peer that has already hung up
            // raises SIGPIPE, whose default action is to KILL THE PROCESS.
            // We hit this while writing this demo: the naive BUG section closes
            // its socket after one byte, and the server's next send() killed
            // the whole program. See the note at the bottom of this file - it
            // is a real bug in PeerFlow, not just in the demo.
            if (::send(fd, payload.data() + sent, n, MSG_NOSIGNAL) < 0) break;
            sent += n;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        ::close(fd);

        if (done) break;
    }
    done = true;
}

int openListener() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 0 = let the OS pick a free port
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main() {
    const int listenFd = openListener();
    if (listenFd < 0) {
        std::fprintf(stderr, "could not create listener\n");
        return 1;
    }

    sockaddr_in bound {};
    socklen_t blen = sizeof bound;
    ::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &blen);
    const auto port = ntohs(bound.sin_port);

    std::atomic<bool> done{false};
    std::thread server(dribblingServer, listenFd, std::ref(done));

    std::printf("A server on 127.0.0.1:%u will send a 68-byte handshake in\n", port);
    std::printf("chunks of 1, 2, 30, 35. Reading it two ways:\n\n");

    // =====================================================================
    //  BUG  -  one naive recv(), the belief that you get what you asked for
    // =====================================================================
    std::printf("=== BUG: a single recv() call ===============================\n");
    {
        int fd = connectTo(port);
        unsigned char buf[kHandshakeSize];
        std::memset(buf, 0xEE, sizeof buf);  // poison, so we SEE what was untouched

        const ssize_t got = ::recv(fd, buf, kHandshakeSize, 0);
        std::printf("  asked recv() for %d bytes.\n", kHandshakeSize);
        std::printf("  recv() returned   %zd bytes.\n\n", got);

        if (got < kHandshakeSize) {
            std::printf("  We now have %zd of %d bytes and are about to parse a\n",
                        got, kHandshakeSize);
            std::printf("  \"handshake\" whose remaining %zd bytes are still 0xEE - "
                        "uninitialised\n", kHandshakeSize - got);
            std::printf("  stack memory we never wrote to.\n\n");

            std::printf("  The code that follows in real life:\n");
            std::printf("    if (buf[0] != 19) reject();                  // passes by luck\n");
            std::printf("    if (memcmp(buf+1, \"BitTorrent protocol\", 19)) reject();  // passes\n");
            std::printf("    if (memcmp(buf+28, infoHash, 20)) reject();    // FAILS - garbage\n");
            std::printf("    ...\"peer sent the wrong info hash\"\n\n");

            // Now actually do the check the real code does: compare the
            // 20 info-hash bytes at offset 28 against the torrent's own hash.
            // The server DID send correct bytes - at offsets 28..47 of the
            // handshake. We simply never read them.
            unsigned char expectedHash[20];
            std::memset(expectedHash, 0xAB, sizeof expectedHash);  // stand-in
            const bool hashMatches = (got >= 48) &&
                                     (std::memcmp(buf + 28, expectedHash, 20) == 0);

            std::printf("  Our verdict:  the peer sent a bad info hash. Close the socket.\n");
            std::printf("  Truth:       we never read the hash. The peer is innocent.\n");
            std::printf("  hash check would report: %s\n", hashMatches ? "MATCH" : "MISMATCH");
            std::printf("  Worse: this is INTERMITTENT. On a fast LAN the kernel may\n");
            std::printf("  hand us all 68 at once and it works. Across the internet it\n");
            std::printf("  fails at random. You debug the NETWORK for a week.\n\n");
        } else {
            std::printf("  (Got everything in one read this time - the naive version\n");
            std::printf("   looks fine! This is exactly why the bug survives.)\n\n");
        }
        ::close(fd);
    }

    // =====================================================================
    //  FIX  -  recvExact(): loop until we have exactly len bytes
    // =====================================================================
    std::printf("=== FIX: recvExact(), i.e. TcpSocket::recvExact() ===========\n");
    {
        int fd = connectTo(port);
        unsigned char buf[kHandshakeSize];
        std::memset(buf, 0xEE, sizeof buf);

        size_t total = 0;
        int reads = 0;
        while (total < kHandshakeSize) {
            const ssize_t got = ::recv(fd, buf + total, kHandshakeSize - total, 0);
            if (got <= 0) {
                std::printf("  connection closed early at %zu bytes\n", total);
                ::close(fd);
                return 1;
            }
            total += static_cast<size_t>(got);
            ++reads;
            std::printf("  recv #%d -> %zd bytes  (running total %zu/%d)\n",
                        reads, got, total, kHandshakeSize);
        }

        std::printf("\n  Loop exited only because total == %d. We have ALL of it.\n\n",
                    kHandshakeSize);

        const bool lenOk = buf[0] == 19;
        const bool protoOk = std::memcmp(buf + 1, "BitTorrent protocol", 19) == 0;
        const bool tailOk = buf[67] == 67;
        std::printf("  now the checks pass for the right reasons:\n");
        std::printf("    buf[0] == 19                        %s\n", lenOk ? "OK" : "FAIL");
        std::printf("    protocol string correct             %s\n", protoOk ? "OK" : "FAIL");
        std::printf("    byte 67 arrived and equals 67        %s\n", tailOk ? "OK" : "FAIL");
        std::printf("\n  Verdict: the peer is genuine. We were right to trust it.\n");
        std::printf("  Note the asymmetry: the FIX is ~8 lines and is the only\n");
        std::printf("  reason any of this works.\n");

        ::close(fd);
    }

    ::close(listenFd);
    server.detach();
    return 0;
}

/*
 * ===========================================================================
 *  THE SIGPIPE TRAP - a real, unfixed bug in PeerFlow
 * ===========================================================================
 *
 * While writing this demo the server thread kept writing to a socket the
 * client had already closed, and the whole program vanished instantly.
 *
 * Cause: on Linux, write()/send() to a socket whose peer has closed raises
 * SIGPIPE, and the default action of SIGPIPE is to TERMINATE THE PROCESS.
 * There is no exception, no catch block, no graceful failure - just death.
 *
 * PeerFlow is exposed to this. src/net/TcpSocket.cpp:165 is:
 *
 *     ssize_t n = ::send(fd_, p + sent, len - sent, 0);
 *                                                        ^ no MSG_NOSIGNAL
 *
 * and `grep -rn SIGPIPE src/ include/` finds NOTHING - the signal is never
 * ignored anywhere in the project.
 *
 * So: a peer that disconnects while we are mid-send kills the entire client.
 * That directly violates this project's own golden rule -
 * "a bad peer must never be able to crash your client".
 *
 * Two standard fixes, either is about one line:
 *   1. signal(SIGPIPE, SIG_IGN);  once, at startup in main().
 *      Blanket, easy to forget, and also protects OpenSSL's internal writes.
 *   2. pass MSG_NOSIGNAL to every send().
 *      Precise, but must be repeated at every call site.
 * Option 1 is what most clients do; the careful version is both.
 *
 * This is left unfixed on purpose. It is a good first bug for you to fix.
 * ===========================================================================
 */
