#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TorrentFile {
    std::string announce;
    std::string name;
    long long pieceLength;
    long long length;
    std::vector<uint8_t> pieces;    // concatenated SHA-1 hashes (20 bytes each)
    std::vector<uint8_t> infoHash;  // 20-byte SHA-1 of bencoded info dict
};
