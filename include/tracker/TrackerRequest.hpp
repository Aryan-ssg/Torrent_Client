#pragma once

// =============================================================================
// TrackerRequest.hpp - The announce request we send to a tracker
// =============================================================================
//
// Sharing facts:
// - The tracker's announce URL (from Phase 2's TorrentFile.announce)
// - info_hash: 20 raw bytes identifying THIS torrent (Phase 2)
// - peer_id:    20 bytes naming OUR client (we pretend to be "-PF0001-...")
// - How much we have uploaded/downloaded so far (0 for now)
// - How many bytes are still left to download ("left")
// - compact=1: "give me peers in the tiny 6-byte binary format"
// - event: "started" on first announce, "completed"/"stopped" later
//
// buildAnnounceUrl() turns all of this into a single HTTP GET URL like:
//   https://tracker.opentrackr.org/announce?info_hash=%B9%2A...&peer_id=-P...
//
// JAVA COMPARISON:
// This is like a DTO (Data Transfer Object) or a Request POJO
// that you'd later serialize into a query string.
// =============================================================================

#include <cstdint>  // For uint16_t, uint8_t
#include <string>   // For std::string
#include <vector>   // For std::vector

struct TrackerRequest {
    // The tracker's announce URL (from the .torrent's announce key)
    std::string announceUrl;

    // 20 raw bytes: the SHA-1 of the info dict (Phase 2). MUST be sent
    // byte-for-byte: if even one byte is wrong, the tracker answers
    // "failure reason" because you asked about the wrong torrent.
    std::vector<uint8_t> infoHash;

    // 20 bytes identifying this client. Standard style: "-PF0001-" + 12
    // random alphanumeric chars (Azureus-style, so trackers know our version).
    std::string peerId;

    // The port we would listen on for incoming peer connections.
    //
    // We only ever CONNECT out - there is no listener, and that is deliberate:
    // binding a port with no accept loop just makes peers dial us and fail.
    // A pure leecher is fine with that, and most home peers are unreachable
    // inbound anyway. The tracker still wants a port in the announce, so we
    // send a plausible one. When seeding arrives (Phase 9) this becomes a real
    // bound port and the value must start matching it.
    uint16_t port = 6881;

    // Bytes uploaded/downloaded since this announce.
    long long uploaded = 0;
    long long downloaded = 0;

    // Bytes still to download. Phase 2's TorrentFile.length is perfect.
    long long left = 0;

    // compact=1 asks for peers as 6 bytes each (4 IP + 2 port) instead of
    // a huge list of dictionaries. Every real client uses compact.
    bool compact = true;

    // "started", "completed", "stopped", or "" (empty = periodic announce).
    std::string event = "started";

    // Build the full GET URL, with all params percent-encoded.
    std::string buildAnnounceUrl() const;
};

// Encode raw bytes into "%XX" form safe for a URL.
// info_hash and peer_id are binary, so most bytes must be escaped.
// (RFC 3986 says only A-Za-z0-9-._~ are safe to keep as-is.)
std::string urlEncode(const std::vector<uint8_t>& bytes);

// Same, for an already-textual string.
std::string urlEncode(const std::string& text);

// Make a fresh 20-byte peer_id: "-PF0001-" + 12 random alnum chars.
std::string generatePeerId();