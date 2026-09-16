#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class BencodeValue {
public:
    enum Type { INTEGER, STRING, LIST, DICT };

private:
    Type type;
    long long intValue;
    std::vector<uint8_t> stringValue;
    std::vector<BencodeValue> listValue;
    std::map<std::string, BencodeValue> dictValue;

public:
    BencodeValue() : type(INTEGER), intValue(0) {}

    static BencodeValue makeInteger(long long value) {
        BencodeValue v;
        v.type = INTEGER;
        v.intValue = value;
        return v;
    }

    static BencodeValue makeString(const std::vector<uint8_t>& value) {
        BencodeValue v;
        v.type = STRING;
        v.stringValue = value;
        return v;
    }

    static BencodeValue makeList(const std::vector<BencodeValue>& value) {
        BencodeValue v;
        v.type = LIST;
        v.listValue = value;
        return v;
    }

    static BencodeValue makeDict(const std::map<std::string, BencodeValue>& value) {
        BencodeValue v;
        v.type = DICT;
        v.dictValue = value;
        return v;
    }

    Type getType() const { return type; }

    long long asInteger() const { return intValue; }
    const std::vector<uint8_t>& asString() const { return stringValue; }
    const std::vector<BencodeValue>& asList() const { return listValue; }
    const std::map<std::string, BencodeValue>& asDict() const { return dictValue; }

    std::string stringValueAsUtf8() const {
        return std::string(stringValue.begin(), stringValue.end());
    }
};
