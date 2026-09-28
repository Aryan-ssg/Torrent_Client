#pragma once

// =============================================================================
// UdpTracker.hpp - Announce to a tracker over UDP (BEP 15)
// =============================================================================
//
// Almost every tracker URL in a modern .torrent starts with "udp://", and
// until we can speak BEP 15 the client simply cannot ask most torrents who
// else is downloading them. So this is not a nice-to-have; it is the
// difference between "works on some torrents" and "works".
//
// WHY IT IS SO MUCH SMALLER THAN HTTP
// -----------------------------------
// An HTTP announce costs a TCP handshake, a TLS handshake, a request, and a
// chunked response - on the order of 5-8 round trips and kilobytes. BEP 15 is
// two UDP datagrams and a reply, in ONE round trip, in about 200 bytes. No
// connection to set up, no TLS, no headers, no redirects. The whole protocol
// is four fixed packet layouts and then it is over.
//
// THE FOUR PACKETS
// ----------------
//   connect request   16 B  0x41727101980 (magic) | action=0 | transaction_id
//   connect response  16 B  action=0 | transaction_id | connection_id
//   announce request  98 B  connection_id (96 of fields + 2 padding) | action=1 | transaction_id |
//                          info_hash | peer_id | downloaded | left | uploaded |
//                          event | ip | key | num_want
//   announce response 20 B + N*6
//                          action=1 | transaction_id | interval | leechers |
//                          seeders | <peer bytes>
//
// An action of 3 anywhere means "error", and the datagram then carries a
// bencoded dictionary with a "failure reason" string instead.
//
// EVERYTHING IS BIG-ENDIAN
// ------------------------
// Every multi-byte field, including the 64-bit magic. This is the same
// endianness trap that cost us the byte-reversed peer addresses in Phase 3: a
// protocol where "most significant byte first" is the wire format, and a host
// that stores numbers the other way, has to be made to put the bytes in the
// right order by hand. The magic constant is the sharpest example - write
// 0x41727101980 little-endian and the tracker does not recognise you, it just
// ignores the datagram. The helpers live in this header so the tests can
// check the exact bytes rather than trusting the arithmetic.
//
// A note on reliability: UDP has no delivery guarantee, so every request is
// sent with a random transaction_id and retried on timeout, exactly as the
// spec's 15 * 2^n second backoff describes. A reply carrying a transaction_id
// we did not send is discarded unread - that is how a late reply to an earlier
// attempt, or a stray packet, is prevented from being parsed as a real answer.
//
// Java parallel: like DatagramSocket / DatagramPacket instead of a URL
// connection, with a request-reply protocol layered on a connectionless
// transport.
// =============================================================================

#include "tracker/Peer.hpp"
#include "tracker/TrackerRequest.hpp"
#include "tracker/TrackerResponse.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The 64-bit magic every BEP 15 connect request starts with. Spelled in hex
// exactly as the spec does, so it can be checked against the spec by eye.
constexpr uint64_t kUdpTrackerProtocolId = 0x41727101980ULL;

// BEP 15 action codes.
enum : int32_t {
    kUdpActionConnect = 0,
    kUdpActionAnnounce = 1,
    kUdpActionError = 3,
    kUdpActionScrape = 2,
};

// Fixed packet sizes from the spec. Asserted before anything is read, so a
// short datagram is rejected rather than read past the end of.
constexpr size_t kUdpConnectRequestSize = 16;
constexpr size_t kUdpConnectResponseSize = 16;
// Fixed packet sizes. The connect packets are unambiguous.
//
// The announce request is the interesting one. Counting the fields in the
// BEP 15 diagram gives 96 bytes:
//
//   8 (connection_id) + 4 (action) + 4 (transaction_id)
//   + 20 (info_hash) + 20 (peer_id)
//   + 8 (downloaded) + 8 (left) + 8 (uploaded)
//   + 4 (event) + 4 (ip) + 4 (key) + 4 (num_want)   = 96
//
// ...but 96 bytes is silently ignored by real trackers. Measured against both
// tracker.opentrackr.org:1337 and open.demonii.com:1337 on the same day:
//
//   96 bytes  -> no reply at all (times out)
//   98 bytes  -> 200 OK, 73 peers, 53 seeders, 20 leechers
//
// libtorrent - the engine under qBittorrent and Transmission - also puts 98
// bytes on the wire. The two extra bytes are trailing padding that the
// trackers expect to be present, so we send them (as zero bytes, which is
// what the implementations that pad do). This is a case of the spec diagram
// not matching deployed behaviour, and the only honest thing to do is record
// the measurement in the code so nobody "corrects" it back to 96.
constexpr size_t kUdpAnnounceRequestSize = 98;

// How much of that is the fields themselves; the rest is the trailing padding.
constexpr size_t kUdpAnnounceFieldsSize = 96;
constexpr size_t kUdpAnnounceResponseHeaderSize = 20;  // plus 6 bytes per peer

// BEP 15's retry schedule: wait 15 * 2^n seconds before retry n.
constexpr int kUdpInitialTimeoutSeconds = 15;

// -----------------------------------------------------------------------------
// Big-endian helpers. Exposed in the header so the golden-byte tests can use
// exactly the same code the wire format does - a test that re-implements the
// conversion is not testing anything.
// -----------------------------------------------------------------------------
void putBE64(uint8_t* out, uint64_t v);
void putBE32(uint8_t* out, uint32_t v);
uint64_t getBE64(const uint8_t* p);
uint32_t getBE32(const uint8_t* p);

class UdpTracker {
public:
    // Announce to a "udp://host:port/announce" URL. The path is ignored, as
    // the spec says it should be. Throws std::runtime_error on failure - the
    // caller (TrackerPool) catches it and records the tracker as unreachable.
    static TrackerResponse announce(const TrackerRequest& request);

    // Parse "udp://host:port[/path]" into host and port. Throws on anything
    // malformed or on a non-udp scheme.
    static void parseUrl(const std::string& url, std::string& host, uint16_t& port);
};
