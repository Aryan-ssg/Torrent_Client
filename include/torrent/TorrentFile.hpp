#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TorrentFile {
    std::string announce;
    std::string name;

    // These two are value-initialised, which looks like belt-and-braces but
    // is not. A torrent need not CONTAIN them: a multi-file torrent has a
    // `files` list and no top-level `length` at all, so the parser simply
    // never assigns the field. Without an initialiser the struct then carries
    // whatever was on the stack - which showed up as a 3.3 MB file claiming to
    // be 86.1 TiB - and that garbage flows straight into ftruncate().
    long long pieceLength = 0;
    long long length = 0;
    std::vector<uint8_t> pieces;    // concatenated SHA-1 hashes (20 bytes each)
    std::vector<uint8_t> infoHash;  // 20-byte SHA-1 of bencoded info dict

    // BEP 12 multi-tracker list: outer vector = tiers, inner = trackers in that
    // tier. Tier 0 is tried first; within a tier the trackers are
    // interchangeable, so all of them are tried before moving to the next tier.
    // Empty when the torrent has no `announce-list`, in which case `announce`
    // above is the only tracker.
    std::vector<std::vector<std::string>> announceTiers;

    // Every tracker URL from every tier, flattened. Handy for a UI line or a
    // quick "does this torrent have any UDP tracker at all?" check.
    std::vector<std::string> allAnnounceUrls() const;
};
