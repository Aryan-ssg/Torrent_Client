#pragma once

#include "torrent/TorrentFile.hpp"
#include <string>
#include <vector>

class TorrentParser {
public:
    static TorrentFile parse(const std::string& filepath);

private:
    static std::vector<uint8_t> readFile(const std::string& filepath);
    static std::vector<uint8_t> computeInfoHash(const std::vector<uint8_t>& raw);
    static size_t findInfoValueStart(const std::vector<uint8_t>& raw);
    static size_t findDictEnd(const std::vector<uint8_t>& raw, size_t dictStart);
};
