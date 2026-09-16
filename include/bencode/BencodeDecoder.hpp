#pragma once

#include "BencodeValue.hpp"
#include <cstdint>
#include <string>
#include <vector>

class BencodeDecoder {
private:
    const uint8_t* data;
    size_t length;
    size_t pos;

    BencodeValue parse();
    BencodeValue parseInteger();
    BencodeValue parseString();
    BencodeValue parseList();
    BencodeValue parseDictionary();

public:
    BencodeDecoder(const uint8_t* data, size_t length);

    static BencodeValue decode(const std::vector<uint8_t>& input);
    static BencodeValue decode(const std::string& input);
};
