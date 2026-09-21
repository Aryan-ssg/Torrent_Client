#pragma once

// =============================================================================
// BencodeValue.hpp - Represents a Bencode Value (Like a Universal Data Type)
// =============================================================================
//
// This class can hold any of the 4 Bencode data types:
//   - INTEGER: A 64-bit integer (e.g., 42, -7)
//   - STRING: A sequence of bytes (e.g., "hello", binary data)
//   - LIST: An ordered collection of BencodeValues (like ArrayList<Object> in Java)
//   - DICT: A key-value map where keys are strings and values are BencodeValues (like HashMap<String, Object>)
//
// JAVA COMPARISON:
// Think of this like a sealed class hierarchy in Java:
//   interface BencodeValue {}
//   record IntegerValue(long value) implements BencodeValue {}
//   record StringValue(byte[] value) implements BencodeValue {}
//   record ListValue(List<BencodeValue> value) implements BencodeValue {}
//   record DictValue(Map<String, BencodeValue> value) implements BencodeValue {}
//
// But in C++, we use a tagged union pattern (one class with a type tag)
// =============================================================================

#include <cstdint>  // For uint8_t (unsigned 8-bit integer, 0-255)
#include <map>      // For std::map (like TreeMap in Java)
#include <string>   // For std::string
#include <vector>   // For std::vector (like ArrayList in Java)

class BencodeValue {
public:
    // =========================================================================
    // ENUM: Defines what type of Bencode value this is
    // =========================================================================
    // In Java: public enum Type { INTEGER, STRING, LIST, DICT }
    enum Type { INTEGER, STRING, LIST, DICT };

private:
    // =========================================================================
    // PRIVATE FIELDS (Instance Variables)
    // =========================================================================
    // Note: Only ONE of these fields is active at a time, based on 'type'
    // This is like Java's sealed interfaces where a record only has its specific fields

    Type type;                                      // Which type of value is stored
    long long intValue;                             // Stores integer value (if type == INTEGER)
    std::vector<uint8_t> stringValue;               // Stores string/bytes (if type == STRING)
    std::vector<BencodeValue> listValue;            // Stores list of values (if type == LIST)
    std::map<std::string, BencodeValue> dictValue;  // Stores dictionary (if type == DICT)

public:
    // Default constructor needed for std::map compatibility
    // In Java: BencodeValue() { this.type = INTEGER; this.intValue = 0; }
    BencodeValue() : type(INTEGER), intValue(0) {}

public:
    // =========================================================================
    // STATIC FACTORY METHODS (Like Java's static valueOf() or of() methods)
    // =========================================================================
    // These create new BencodeValue objects of each type
    // In Java: public static BencodeValue of(long value) { ... }

    // Creates an INTEGER value
    // Example: BencodeValue::makeInteger(42) creates a value representing 42
    static BencodeValue makeInteger(long long value) {
        BencodeValue v;
        v.type = INTEGER;
        v.intValue = value;
        return v;
    }

    // Creates a STRING value from raw bytes
    // Example: BencodeValue::makeString({'h','e','l','l','o'}) creates "hello"
    static BencodeValue makeString(const std::vector<uint8_t>& value) {
        BencodeValue v;
        v.type = STRING;
        v.stringValue = value;
        return v;
    }

    // Creates a LIST value containing other BencodeValues
    // Example: BencodeValue::makeList({makeInteger(1), makeString("hi")}) creates [1, "hi"]
    static BencodeValue makeList(const std::vector<BencodeValue>& value) {
        BencodeValue v;
        v.type = LIST;
        v.listValue = value;
        return v;
    }

    // Creates a DICT (dictionary/map) value
    // Example: BencodeValue::makeDict({{"name", makeString("Alice")}}) creates {"name": "Alice"}
    static BencodeValue makeDict(const std::map<std::string, BencodeValue>& value) {
        BencodeValue v;
        v.type = DICT;
        v.dictValue = value;
        return v;
    }

    // =========================================================================
    // GETTER METHODS (Access the stored value)
    // =========================================================================
    // In Java, you'd have getClass() or pattern matching with switch

    // Returns which type this value is
    // Usage: if (value.getType() == BencodeValue::INTEGER) { ... }
    Type getType() const { return type; }

    // Returns the integer value (only valid if type == INTEGER)
    // Usage: long long num = value.asInteger();
    long long asInteger() const { return intValue; }

    // Returns the raw string/bytes (only valid if type == STRING)
    // Usage: const auto& bytes = value.asString();
    const std::vector<uint8_t>& asString() const { return stringValue; }

    // Returns the list (only valid if type == LIST)
    // Usage: const auto& items = value.asList();
    const std::vector<BencodeValue>& asList() const { return listValue; }

    // Returns the dictionary (only valid if type == DICT)
    // Usage: const auto& map = value.asDict();
    const std::map<std::string, BencodeValue>& asDict() const { return dictValue; }

    // =========================================================================
    // HELPER METHOD: Convert string bytes to std::string
    // =========================================================================
    // Since Bencode strings can contain binary data, we store them as byte vectors
    // This helper converts to a regular string for convenience
    // Usage: std::string text = value.stringValueAsUtf8();
    std::string stringValueAsUtf8() const {
        return std::string(stringValue.begin(), stringValue.end());
    }
};

/*
 * KEY CONCEPTS FOR JAVA DEVELOPERS:
 *
 * 1. const keyword:
 *    - Used in method signatures to indicate the method doesn't modify the object
 *    - Similar to marking a method 'final' or having a 'this' reference in Java
 *    - Example: getType() const { return type; }  // This method doesn't change the object
 *
 * 2. Reference (&):
 *    - A reference is like a pointer but must always be valid (no null references)
 *    - const auto& means "don't copy, just reference, and don't allow modifications"
 *    - In Java, all object variables are references, but C++ can have copies or references
 *
 * 3. Static factory methods vs constructors:
 *    - C++ uses static methods like makeInteger() instead of constructors
 *    - This gives more control over object creation
 *    - In Java, you might see Integer.valueOf() instead of new Integer()
 */
