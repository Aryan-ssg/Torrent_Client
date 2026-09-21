#pragma once

// =============================================================================
// BencodeDecoder.hpp - Class that Decodes Bencode Data
// =============================================================================
//
// This class parses (decodes) Bencode-encoded data and converts it into
// BencodeValue objects that can be easily used in your code.
//
// WHAT IS PARSING?
// Parsing is the process of analyzing a sequence of characters (or bytes)
// according to the rules of a language/format and building a data structure.
//
// For example, parsing "i42e" (Bencode format for integer 42) gives you
// a BencodeValue containing the integer 42.
//
// JAVA COMPARISON:
// This is similar to Jackson's ObjectMapper.readValue() or Gson's fromJson()
// but for Bencode format instead of JSON.
//
// Example in Java:
//   ObjectMapper mapper = new ObjectMapper();
//   JsonNode node = mapper.readTree(jsonString);
//
// Example in C++ (this project):
//   BencodeValue value = BencodeDecoder::decode(bencodeString);
// =============================================================================

#include "BencodeValue.hpp"  // Our value class that holds parsed data
#include <cstdint>           // For uint8_t
#include <string>            // For std::string
#include <vector>            // For std::vector

class BencodeDecoder {
private:
    // =========================================================================
    // PRIVATE FIELDS (State of the decoder while parsing)
    // =========================================================================
    // These track where we are in the input data while parsing

    const uint8_t* data;  // Pointer to the raw input bytes (like byte[] in Java)
    size_t length;        // Total length of the input data
    size_t pos;           // Current position we're reading from (like a cursor)

    // =========================================================================
    // PRIVATE PARSING METHODS (Recursive Descent Parser)
    // =========================================================================
    // These methods parse different parts of the Bencode format
    // They're recursive because lists and dicts can contain other lists/dicts

    // Main parse method - determines what type to parse based on current byte
    BencodeValue parse();

    // Parses an integer: starts with 'i', ends with 'e'
    // Example: "i42e" -> 42
    BencodeValue parseInteger();

    // Parses a string: format is "length:content"
    // Example: "5:hello" -> "hello"
    BencodeValue parseString();

    // Parses a list: starts with 'l', ends with 'e'
    // Example: "li42ee" -> [42]
    BencodeValue parseList();

    // Parses a dictionary: starts with 'd', ends with 'e'
    // Example: "d3:cow3:moo4:spami42ee" -> {"cow": "moo", "spam": 42}
    BencodeValue parseDictionary();

public:
    // =========================================================================
    // CONSTRUCTOR
    // =========================================================================
    // Initializes the decoder with the data to parse
    // In Java: public BencodeDecoder(byte[] data) { this.data = data; }
    BencodeDecoder(const uint8_t* data, size_t length);

    // =========================================================================
    // STATIC DECODE METHODS (The main entry points for users)
    // =========================================================================
    // These are static methods that create a decoder and use it to parse data
    // This is a common pattern: hide the constructor, use static factory methods

    // Decode from a byte vector
    // Usage: BencodeValue val = BencodeDecoder::decode(byteVector);
    static BencodeValue decode(const std::vector<uint8_t>& input);

    // Decode from a string (convenience method)
    // Usage: BencodeValue val = BencodeDecoder::decode("i42e");
    static BencodeValue decode(const std::string& input);
};

/*
 * HOW PARSING WORKS (RECURSIVE DESCENT):
 *
 * The parser works by looking at the current character and deciding what to do:
 *
 * 1. If current char is 'i' -> parse integer
 * 2. If current char is 'l' -> parse list
 * 3. If current char is 'd' -> parse dictionary
 * 4. If current char is '0'-'9' -> parse string (the number is the length)
 *
 * For lists and dictionaries, the parser calls itself recursively to parse
 * nested values. This is called "recursive descent parsing."
 *
 * JAVA ANALOGY:
 * Think of this like parsing a JSON tree where you have:
 *   if (current == '{') parseObject();
 *   else if (current == '[') parseArray();
 *   else if (current == '"') parseString();
 *   else if (current == '-' || isDigit(current)) parseNumber();
 *
 * The key difference is that Bencode is simpler (only 4 types).
 */
