/*
 * demo-01-byte-order.cpp - THE HISTORICAL BUG, SIDE BY SIDE
 * =============================================================================
 *
 * The point of this file is the two halves marked  BUG  and  FIX.
 * The BUG half is the real code that shipped in PeerFlow and broke every
 * peer connection. The FIX half is what it became.
 *
 * Both run here, on your machine, offline. The BUG half prints an IP that
 * is NOT the one it was given. That is the whole lesson.
 *
 *   g++ -O0 -o /tmp/demo01 docs/12-tutor/demos/demo-01-byte-order.cpp
 *   /tmp/demo01
 *
 * ---------------------------------------------------------------------------
 * THE CONTRAST
 * ---------------------------------------------------------------------------
 *   WITHOUT the fix (commit a4e60e4's parent):
 *       tracker says  58.219.119.153  ->  we dial  153.119.219.58   WRONG
 *       Every peer address is reversed. Ports are fine, so the output
 *       still LOOKS plausible, and a test that only COUNTS peers passes.
 *
 *   WITH the fix:
 *       tracker says  58.219.119.153  ->  we dial  58.219.119.153   RIGHT
 *
 * WHY THE TEST DIDN'T CATCH IT: the tracker test asserted "we got 50 peers".
 * It got 50 peers. They were the wrong 50 peers. A test that checks the
 * COUNT of untrusted data is not a test of that data. There is now a
 * regression test for the exact address.
 *
 * ---------------------------------------------------------------------------
 * THE ROOT CAUSE, in one sentence
 * ---------------------------------------------------------------------------
 * There are two "byte orders" here and they cancel:
 *
 *   1. NETWORK order (big-endian): the order bytes appear on the wire.
 *      A tracker sends 58.219.119.153 as  3A DB 77 99.
 *   2. HOST order: your CPU's own order. x86/ARM are LITTLE-endian:
 *      they store 0x3ADB7799 as  99 77 DB 3A.
 *
 *   in_addr::s_addr is specified to be in NETWORK order. So the 4 bytes you
 *   read off the wire are ALREADY the right bytes to put in it. Any arithmetic
 *   you do in between is a second, unwanted conversion.
 *
 * Rule of thumb: if the data came off the network and is going back onto a
 * socket, memcpy. If you're computing something, shift.
 * ==========================================================================
 */

#include <arpa/inet.h>   // inet_ntop - turns a struct in_addr into a printable string
#include <cstdint>
#include <cstring>
#include <cstdio>

int main() {
    // The exact 6 bytes a real BitTorrent tracker puts on the wire for one
    // peer: 4 IP bytes + 2 port bytes, all big-endian.
    //   58 . 219 . 119 . 153  =  0x3A 0xDB 0x77 0x99
    //   port 6881               =  0x1A 0xE1
    const unsigned char wire[6] = {0x3A, 0xDB, 0x77, 0x99, 0x1A, 0xE1};

    std::printf("What the tracker actually put on the wire:\n");
    std::printf("  IP   %u.%u.%u.%u   (bytes 3A DB 77 99)\n",
                wire[0], wire[1], wire[2], wire[3]);
    std::printf("  port %u        (bytes 1A E1)\n\n",
                (unsigned)((wire[4] << 8) | wire[5]));

    // Detect our own byte order, and cross-check the runtime probe against the
    // compiler's own answer so a mistake here can't silently mislead you.
    //
    //   The value 1 is 0x00000001; its significant byte is 0x01.
    //   LITTLE-endian: least significant byte at the LOWEST address
    //                  -> memory reads 01 00 00 00
    //   BIG-endian:    most significant byte at the lowest address
    //                  -> memory reads 00 00 00 01
    std::printf("Your CPU's byte order: ");
    const uint32_t probe = 1;
    unsigned char probeBytes[4];
    std::memcpy(probeBytes, &probe, 4);
    const bool littleEndian = (probeBytes[0] == 0x01);

    if (littleEndian) {
        std::printf("LITTLE-endian\n");
        std::printf("  the value 0x3ADB7799 lives in memory as  99 77 DB 3A\n");
    } else {
        std::printf("BIG-endian\n");
        std::printf("  the value 0x3ADB7799 lives in memory as  3A DB 77 99\n");
    }
    std::printf("  runtime probe: bytes of the value 1 = %02X %02X %02X %02X\n",
                probeBytes[0], probeBytes[1], probeBytes[2], probeBytes[3]);
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
    const bool compilerSaysLittle = (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
    std::printf("  compiler says:  %s   %s\n",
                compilerSaysLittle ? "LITTLE-endian" : "BIG-endian",
                (compilerSaysLittle == littleEndian) ? "(agrees)" : "(DISAGREES - bug in the probe!)");
#endif
    std::printf("\n");

    if (!littleEndian) {
        std::printf("  Heads up: on a big-endian host the BUG section below would print\n");
        std::printf("  the CORRECT address, because the two conversions cancel out.\n");
        std::printf("  That is part of why this shipped - the code looks right to the\n");
        std::printf("  author, and only fails on real hardware.\n\n");
    }

    // -------------------------------------------------------------------------
    //  BUG  -  the code that shipped. Every peer address was reversed.
    // -------------------------------------------------------------------------
    std::printf("=== BUG  (the version that shipped) =========================\n");
    {
        in_addr addr {};
        addr.s_addr = (wire[0] << 24) | (wire[1] << 16) | (wire[2] << 8) | wire[3];

        // What the bits we just assigned are, as a plain number. This is the
        // part the author got RIGHT: the shift expression really does compute
        // 0x3ADB7799, which is 58.219.119.153.
        const uint32_t asNumber = ntohl(addr.s_addr);
        std::printf("  The shift expression itself was CORRECT. It computed\n");
        std::printf("  0x3ADB7799 = 58.219.119.153  <- the address we wanted.\n");
        std::printf("\n");
        std::printf("  But s_addr is defined to hold NETWORK byte order, so assigning\n");
        std::printf("  that number to it performed a SECOND, unwanted conversion.\n");
        std::printf("  Reading s_addr back out gives the bits reversed:\n");
        std::printf("  0x%08X = %u.%u.%u.%u\n\n", asNumber,
                    (asNumber >> 24) & 0xFF, (asNumber >> 16) & 0xFF,
                    (asNumber >> 8) & 0xFF, asNumber & 0xFF);

        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, text, sizeof text);
        std::printf("\n  we dial  >>> %s <<<   which is a DIFFERENT MACHINE.\n\n", text);
    }

    // -------------------------------------------------------------------------
    //  FIX  -  what it became. Copy the bytes; do no arithmetic at all.
    // -------------------------------------------------------------------------
    std::printf("=== FIX  (current code, HttpTracker.cpp) ===================\n");
    {
        in_addr addr {};
        std::memcpy(&addr.s_addr, wire, 4);

        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, text, sizeof text);
        std::printf("  memcpy the 4 wire bytes straight in - zero arithmetic.\n");
        std::printf("  we now dial  >>> %s <<<   correct.\n\n", text);
    }

    // -------------------------------------------------------------------------
    //  WHY THE PORT WAS NEVER WRONG  -  the instructive part
    // -------------------------------------------------------------------------
    std::printf("=== why ports survived and IPs did not ======================\n");
    std::printf("  The port is decoded to a plain uint16_t, used as a NUMBER, and\n");
    std::printf("  handed to connect() - which converts it back. Round trip, so it\n");
    std::printf("  cancels out:\n");
    {
        uint16_t buggyPort = (wire[4] << 8) | wire[5];
        std::printf("    (0x1A<<8)|0xE1 = %u  ->  correct: %u  OK\n",
                    buggyPort, buggyPort);
    }
    std::printf("\n  The IP has no such escape hatch. The wrong value went STRAIGHT\n");
    std::printf("  into connect(). So one half of the struct was right and one was\n");
    std::printf("  wrong - which is the worst kind of bug: it looks alive.\n\n");

    // -------------------------------------------------------------------------
    //  THE GENERAL RULE
    // -------------------------------------------------------------------------
    std::printf("=== the rule =================================================\n");
    std::printf("  data off the wire, going back onto a socket  ->  memcpy\n");
    std::printf("  computing a new number                        ->  shift + htons()\n\n");

    std::printf("  PeerFlow does exactly this in:\n");
    std::printf("    src/tracker/HttpTracker.cpp  decodeCompactPeers()  (IP bytes)\n");
    std::printf("    src/peer/PeerMessage.cpp    putU32BE/getU32BE     (lengths)\n");
    std::printf("    src/tracker/UdpTracker.cpp   putBE32/getBE32       (BEP 15)\n");
    std::printf("  Three places, one rule. When you read those files, they are all\n");
    std::printf("  this same lesson wearing different clothes.\n");

    return 0;
}
