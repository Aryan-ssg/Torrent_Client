#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeException.hpp"
#include <cstring>

BencodeDecoder::BencodeDecoder(const uint8_t* data, size_t length)
    : data(data), length(length), pos(0) {}

BencodeValue BencodeDecoder::decode(const std::vector<uint8_t>& input) {
    BencodeDecoder decoder(input.data(), input.size());
    BencodeValue result = decoder.parse();
    if (decoder.pos != decoder.length) {
        throw BencodeException("Unexpected trailing data at position " + std::to_string(decoder.pos));
    }
    return result;
}

BencodeValue BencodeDecoder::decode(const std::string& input) {
    std::vector<uint8_t> bytes(input.begin(), input.end());
    return decode(bytes);
}

BencodeValue BencodeDecoder::parse() {
    if (pos >= length) {
        throw BencodeException("Unexpected end of input at position " + std::to_string(pos));
    }

    uint8_t current = data[pos];

    if (current == 'i') {
        return parseInteger();
    } else if (current == 'l') {
        return parseList();
    } else if (current == 'd') {
        return parseDictionary();
    } else if (current >= '0' && current <= '9') {
        return parseString();
    } else {
        throw BencodeException(
            "Unexpected byte at position " + std::to_string(pos) + ": " + static_cast<char>(current)
        );
    }
}

BencodeValue BencodeDecoder::parseInteger() {
    size_t start = pos;
    pos++; // skip 'i'

    bool negative = false;
    if (pos < length && data[pos] == '-') {
        negative = true;
        pos++;
    }

    if (pos >= length || data[pos] < '0' || data[pos] > '9') {
        throw BencodeException("Invalid integer at position " + std::to_string(start));
    }

    long long value = 0;
    while (pos < length && data[pos] != 'e') {
        uint8_t b = data[pos];
        if (b < '0' || b > '9') {
            throw BencodeException("Invalid digit in integer at position " + std::to_string(pos));
        }
        value = value * 10 + (b - '0');
        pos++;
    }

    if (pos >= length) {
        throw BencodeException("Unterminated integer starting at position " + std::to_string(start));
    }

    pos++; // skip 'e'

    if (negative) {
        value = -value;
    }

    return BencodeValue::makeInteger(value);
}

BencodeValue BencodeDecoder::parseString() {
    size_t start = pos;

    // Read length digits before ':'
    long long strLength = 0;
    while (pos < length && data[pos] != ':') {
        uint8_t b = data[pos];
        if (b < '0' || b > '9') {
            throw BencodeException("Invalid character in string length at position " + std::to_string(pos));
        }
        strLength = strLength * 10 + (b - '0');
        pos++;
    }

    if (pos >= length) {
        throw BencodeException("Unterminated string length at position " + std::to_string(start));
    }

    pos++; // skip ':'

    // Check we have enough bytes
    if (pos + static_cast<size_t>(strLength) > length) {
        throw BencodeException(
            "String length " + std::to_string(strLength) +
            " exceeds available bytes at position " + std::to_string(pos)
        );
    }

    // Copy the string bytes
    std::vector<uint8_t> result(data + pos, data + pos + strLength);
    pos += static_cast<size_t>(strLength);

    return BencodeValue::makeString(result);
}

BencodeValue BencodeDecoder::parseList() {
    pos++; // skip 'l'
    std::vector<BencodeValue> list;

    while (pos < length && data[pos] != 'e') {
        list.push_back(parse());
    }

    if (pos >= length) {
        throw BencodeException("Unterminated list");
    }

    pos++; // skip 'e'
    return BencodeValue::makeList(list);
}

BencodeValue BencodeDecoder::parseDictionary() {
    pos++; // skip 'd'
    std::map<std::string, BencodeValue> map;

    while (pos < length && data[pos] != 'e') {
        // Keys must be strings
        if (pos >= length || data[pos] < '0' || data[pos] > '9') {
            throw BencodeException("Dictionary key must be a string at position " + std::to_string(pos));
        }

        BencodeValue keyVal = parseString();
        std::string key(keyVal.asString().begin(), keyVal.asString().end());
        BencodeValue value = parse();
        map[key] = value;
    }

    if (pos >= length) {
        throw BencodeException("Unterminated dictionary");
    }

    pos++; // skip 'e'
    return BencodeValue::makeDict(map);
}
