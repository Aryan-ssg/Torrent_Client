#pragma once

#include "torrent/TorrentFile.hpp"
#include <string>
#include <vector>

class TorrentParser {
public:
    // Read a .torrent off disk and parse it.
    static TorrentFile parse(const std::string& filepath);

    // Parse a .torrent's bencoded bytes directly.
    //
    // Split out from parse() so the parsing itself is testable without
    // writing a file for every case - a multi-file torrent, a truncated one,
    // a missing key. Each of those is easier to describe as 200 bytes of
    // bencode in a test than as a fixture on disk.
    static TorrentFile parseString(const std::string& bencoded);

private:
    static std::vector<uint8_t> readFile(const std::string& filepath);
    static std::vector<uint8_t> computeInfoHash(const std::vector<uint8_t>& raw);
    static size_t findInfoValueStart(const std::vector<uint8_t>& raw);
    static size_t findDictEnd(const std::vector<uint8_t>& raw, size_t dictStart);
};
