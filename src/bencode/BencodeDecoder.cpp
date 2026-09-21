#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeException.hpp"
#include <cstring>  // For memcpy if needed

// =============================================================================
// BencodeDecoder.cpp - Implementation of the Bencode Decoder
// =============================================================================
//
// This file contains the actual parsing logic. Let's walk through each method.
//
// BENCODE FORMAT REFERENCE:
// - Integer: "i<number>e"        Example: "i42e" = 42
// - String:  "<length>:<data>"    Example: "5:hello" = "hello"
// - List:    "l<items>e"         Example: "li42ee" = [42]
// - Dict:    "d<key><value>e"    Example: "d3:key5:valuee" = {"key": "value"}
// =============================================================================

// =============================================================================
// CONSTRUCTOR
// =============================================================================
// Initializes the decoder with data to parse
// 'data' is a pointer to the first byte of input
// 'length' is how many bytes we have
// 'pos' starts at 0 (beginning of data)
BencodeDecoder::BencodeDecoder(const uint8_t* data, size_t length)
    : data(data), length(length), pos(0) {}  // Member initializer list (like Java's constructor)

// =============================================================================
// STATIC DECODE METHODS (Public API)
// =============================================================================

// Decode from a vector of bytes
// This is the main entry point users call
BencodeValue BencodeDecoder::decode(const std::vector<uint8_t>& input) {
    // Create a decoder instance with the input data
    BencodeDecoder decoder(input.data(), input.size());

    // Parse the entire input
    BencodeValue result = decoder.parse();

    // IMPORTANT: Check that we consumed ALL the input
    // If there's leftover data, it's an error (like trailing characters in JSON)
    // For example: "i42eEXTRA" should fail because "EXTRA" is not part of the integer
    if (decoder.pos != decoder.length) {
        throw BencodeException("Unexpected trailing data at position " + std::to_string(decoder.pos));
    }

    return result;
}

// Decode from a std::string (convenience overload)
// Converts the string to bytes and calls the vector version
// In Java: byte[] bytes = input.getBytes(StandardCharsets.UTF_8);
BencodeValue BencodeDecoder::decode(const std::string& input) {
    std::vector<uint8_t> bytes(input.begin(), input.end());
    return decode(bytes);
}

// =============================================================================
// MAIN PARSE METHOD (The Dispatcher)
// =============================================================================
// This method looks at the current byte and decides which parser to call
// It's called "recursive descent" because it calls itself for nested structures
BencodeValue BencodeDecoder::parse() {
    // First, check if we've reached the end of input
    if (pos >= length) {
        throw BencodeException("Unexpected end of input at position " + std::to_string(pos));
    }

    uint8_t current = data[pos];  // Look at current byte

    // Dispatch based on the first character
    // In Java: switch (current) { case 'i': return parseInteger(); ... }
    if (current == 'i') {
        return parseInteger();      // Integer: starts with 'i'
    } else if (current == 'l') {
        return parseList();         // List: starts with 'l'
    } else if (current == 'd') {
        return parseDictionary();   // Dictionary: starts with 'd'
    } else if (current >= '0' && current <= '9') {
        return parseString();       // String: starts with a digit (the length)
    } else {
        // If none of the above, it's invalid Bencode
        throw BencodeException(
            "Unexpected byte at position " + std::to_string(pos) + ": " + static_cast<char>(current)
        );
    }
}

// =============================================================================
// INTEGER PARSER
// =============================================================================
// Parses Bencode integers in format: i<number>e
// Example: "i42e" -> 42
// Example: "i-7e" -> -7
BencodeValue BencodeDecoder::parseInteger() {
    size_t start = pos;  // Remember where we started (for error messages)
    pos++;               // Skip the 'i' character

    // Check for negative sign
    bool negative = false;
    if (pos < length && data[pos] == '-') {
        negative = true;
        pos++;  // Skip the '-'
    }

    // Validate: must have at least one digit after 'i' (or '-' if negative)
    if (pos >= length || data[pos] < '0' || data[pos] > '9') {
        throw BencodeException("Invalid integer at position " + std::to_string(start));
    }

    // Read digits until we hit 'e'
    // Example: for "i42e", we read '4', then '2', building the number 42
    long long value = 0;
    while (pos < length && data[pos] != 'e') {
        uint8_t b = data[pos];

        // Validate: each character must be a digit
        if (b < '0' || b > '9') {
            throw BencodeException("Invalid digit in integer at position " + std::to_string(pos));
        }

        // Build the number: shift existing digits left and add new digit
        // Example: if value=4 and b='2', then value = 4*10 + 2 = 42
        value = value * 10 + (b - '0');
        pos++;
    }

    // Validate: must find closing 'e'
    if (pos >= length) {
        throw BencodeException("Unterminated integer starting at position " + std::to_string(start));
    }

    pos++;  // Skip the 'e'

    // Apply negative sign if needed
    if (negative) {
        value = -value;
    }

    // Create and return the integer value
    return BencodeValue::makeInteger(value);
}

// =============================================================================
// STRING PARSER
// =============================================================================
// Parses Bencode strings in format: <length>:<content>
// Example: "5:hello" -> "hello"
// Example: "0:" -> "" (empty string)
// Example: "10:abcdefghij" -> "abcdefghij"
BencodeValue BencodeDecoder::parseString() {
    size_t start = pos;

    // Step 1: Read the length (digits before ':')
    // Example: for "5:hello", we read '5' to get length=5
    long long strLength = 0;
    while (pos < length && data[pos] != ':') {
        uint8_t b = data[pos];

        // Validate: length must be digits only
        if (b < '0' || b > '9') {
            throw BencodeException("Invalid character in string length at position " + std::to_string(pos));
        }

        // Build the length number
        strLength = strLength * 10 + (b - '0');
        pos++;
    }

    // Validate: must find ':'
    if (pos >= length) {
        throw BencodeException("Unterminated string length at position " + std::to_string(start));
    }

    pos++;  // Skip the ':'

    // Step 2: Read the actual string content
    // Validate: make sure we have enough bytes left
    if (pos + static_cast<size_t>(strLength) > length) {
        throw BencodeException(
            "String length " + std::to_string(strLength) +
            " exceeds available bytes at position " + std::to_string(pos)
        );
    }

    // Copy the string bytes
    // In Java: byte[] result = Arrays.copyOfRange(data, pos, pos + strLength);
    std::vector<uint8_t> result(data + pos, data + pos + strLength);
    pos += static_cast<size_t>(strLength);  // Move past the string content

    return BencodeValue::makeString(result);
}

// =============================================================================
// LIST PARSER
// =============================================================================
// Parses Bencode lists in format: l<item1><item2>...e
// Example: "li42ee" -> [42]
// Example: "li42e5:helloe" -> [42, "hello"]
// Example: "le" -> [] (empty list)
// Lists can contain any Bencode value, including nested lists!
BencodeValue BencodeDecoder::parseList() {
    pos++;  // Skip the 'l'

    std::vector<BencodeValue> list;  // Create empty list (like ArrayList in Java)

    // Keep parsing items until we hit 'e' or end of input
    while (pos < length && data[pos] != 'e') {
        // Recursively parse each item (could be integer, string, list, or dict!)
        list.push_back(parse());  // push_back is like add() in Java's ArrayList
    }

    // Validate: must find closing 'e'
    if (pos >= length) {
        throw BencodeException("Unterminated list");
    }

    pos++;  // Skip the 'e'

    return BencodeValue::makeList(list);
}

// =============================================================================
// DICTIONARY (MAP) PARSER
// =============================================================================
// Parses Bencode dictionaries in format: d<key1><value1><key2><value2>e
// Example: "d3:cow3:moo4:spami42ee" -> {"cow": "moo", "spam": 42}
// Keys MUST be strings, values can be any Bencode value
BencodeValue BencodeDecoder::parseDictionary() {
    pos++;  // Skip the 'd'

    std::map<std::string, BencodeValue> map;  // Like TreeMap in Java

    // Keep parsing key-value pairs until we hit 'e'
    while (pos < length && data[pos] != 'e') {
        // Step 1: Parse the key (must be a string)
        // In Bencode, dictionary keys are always strings
        if (pos >= length || data[pos] < '0' || data[pos] > '9') {
            throw BencodeException("Dictionary key must be a string at position " + std::to_string(pos));
        }

        // Parse the key as a string
        BencodeValue keyVal = parseString();

        // Convert the key from bytes to std::string
        // In Java: String key = new String(keyBytes, StandardCharsets.UTF_8);
        std::string key(keyVal.asString().begin(), keyVal.asString().end());

        // Step 2: Parse the value (can be any Bencode type)
        BencodeValue value = parse();  // Recursive call!

        // Step 3: Add to map
        // In Java: map.put(key, value);
        map[key] = value;
    }

    // Validate: must find closing 'e'
    if (pos >= length) {
        throw BencodeException("Unterminated dictionary");
    }

    pos++;  // Skip the 'e'

    return BencodeValue::makeDict(map);
}

/*
 * RECURSIVE DESCENT PARSING SUMMARY:
 *
 * The key insight is that parse() calls itself recursively:
 *
 * parse() -> parseInteger()  (no recursion, base case)
 * parse() -> parseString()   (no recursion, base case)
 * parse() -> parseList()     -> calls parse() for each item (recursion!)
 * parse() -> parseDictionary() -> calls parseString() for key, parse() for value (recursion!)
 *
 * This handles arbitrarily nested structures like:
 *   d3:keyl4:spaami42eee -> {"key": ["spam", 42]}
 *
 * JAVA ANALOGY:
 * Think of this like parsing a JSON tree:
 *   parse() might call parseArray() which calls parse() for each element
 *   parse() might call parseObject() which calls parse() for each value
 *
 * The recursion works because:
 * 1. Base cases (integers, strings) don't recurse
 * 2. Recursive cases (lists, dicts) eventually hit base cases
 * 3. The input is guaranteed to be finite, so recursion terminates
 */
