#include <iostream>
#include <vector>
#include <cassert>

#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeException.hpp"

static int passed = 0;
static int failed = 0;

static void testInteger(const std::string& input, long long expected) {
    try {
        BencodeValue result = BencodeDecoder::decode(input);
        if (result.getType() == BencodeValue::INTEGER && result.asInteger() == expected) {
            std::cout << "PASS: \"" << input << "\" -> " << result.asInteger() << "\n";
            passed++;
        } else {
            std::cout << "FAIL: \"" << input << "\" -> unexpected value\n";
            failed++;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: \"" << input << "\" threw " << e.what() << "\n";
        failed++;
    }
}

static void testString(const std::string& input, const std::string& expected) {
    try {
        BencodeValue result = BencodeDecoder::decode(input);
        if (result.getType() == BencodeValue::STRING &&
            result.stringValueAsUtf8() == expected) {
            std::cout << "PASS: \"" << input << "\" -> \"" << expected << "\"\n";
            passed++;
        } else {
            std::cout << "FAIL: \"" << input << "\" -> mismatch\n";
            failed++;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: \"" << input << "\" threw " << e.what() << "\n";
        failed++;
    }
}

static void testList(const std::string& input, size_t expectedSize) {
    try {
        BencodeValue result = BencodeDecoder::decode(input);
        if (result.getType() == BencodeValue::LIST &&
            result.asList().size() == expectedSize) {
            std::cout << "PASS: \"" << input << "\" -> list of " << expectedSize << " items\n";
            passed++;
        } else {
            std::cout << "FAIL: \"" << input << "\" -> wrong list size\n";
            failed++;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: \"" << input << "\" threw " << e.what() << "\n";
        failed++;
    }
}

static void testNestedList() {
    try {
        std::string input = "lli42eee";
        BencodeValue result = BencodeDecoder::decode(input);
        if (result.getType() == BencodeValue::LIST &&
            result.asList().size() == 1 &&
            result.asList()[0].getType() == BencodeValue::LIST &&
            result.asList()[0].asList().size() == 1 &&
            result.asList()[0].asList()[0].asInteger() == 42) {
            std::cout << "PASS: nested list -> [[42]]\n";
            passed++;
            return;
        }
        std::cout << "FAIL: nested list structure mismatch\n";
        failed++;
    } catch (const std::exception& e) {
        std::cout << "FAIL: nested list threw " << e.what() << "\n";
        failed++;
    }
}

static void testDictionary() {
    try {
        std::string input = "d3:cow3:moo4:spami42ee";
        BencodeValue result = BencodeDecoder::decode(input);
        if (result.getType() == BencodeValue::DICT) {
            const auto& map = result.asDict();
            auto cowIt = map.find("cow");
            auto spamIt = map.find("spam");

            bool cowMatch = cowIt != map.end() &&
                cowIt->second.getType() == BencodeValue::STRING &&
                cowIt->second.stringValueAsUtf8() == "moo";
            bool spamMatch = spamIt != map.end() &&
                spamIt->second.getType() == BencodeValue::INTEGER &&
                spamIt->second.asInteger() == 42;

            if (cowMatch && spamMatch && map.size() == 2) {
                std::cout << "PASS: dictionary -> {cow=moo, spam=42}\n";
                passed++;
                return;
            }
        }
        std::cout << "FAIL: dictionary content mismatch\n";
        failed++;
    } catch (const std::exception& e) {
        std::cout << "FAIL: dictionary threw " << e.what() << "\n";
        failed++;
    }
}

static void testNestedDictionary() {
    try {
        std::string input = "d4:infod3:key5:valueee";
        BencodeValue result = BencodeDecoder::decode(input);
        if (result.getType() == BencodeValue::DICT) {
            const auto& outer = result.asDict();
            auto infoIt = outer.find("info");
            if (infoIt != outer.end() && infoIt->second.getType() == BencodeValue::DICT) {
                const auto& inner = infoIt->second.asDict();
                auto keyIt = inner.find("key");
                if (keyIt != inner.end() &&
                    keyIt->second.getType() == BencodeValue::STRING &&
                    keyIt->second.stringValueAsUtf8() == "value") {
                    std::cout << "PASS: nested dictionary -> {info={key=value}}\n";
                    passed++;
                    return;
                }
            }
        }
        std::cout << "FAIL: nested dictionary mismatch\n";
        failed++;
    } catch (const std::exception& e) {
        std::cout << "FAIL: nested dictionary threw " << e.what() << "\n";
        failed++;
    }
}

static void testTrailingDataDetection() {
    try {
        BencodeDecoder::decode("i42eEXTRA");
        std::cout << "FAIL: trailing data not detected\n";
        failed++;
    } catch (const BencodeException& e) {
        std::cout << "PASS: trailing data detected -> " << e.what() << "\n";
        passed++;
    }
}

static void testInvalidInput() {
    try {
        BencodeDecoder::decode("xyz");
        std::cout << "FAIL: invalid input not detected\n";
        failed++;
    } catch (const BencodeException& e) {
        std::cout << "PASS: invalid input detected -> " << e.what() << "\n";
        passed++;
    }
}

int main() {
    std::cout << "=== Bencode Decoder Tests ===\n\n";

    // Integers
    testInteger("i42e", 42);
    testInteger("i0e", 0);
    testInteger("i-7e", -7);
    testInteger("i123456789e", 123456789);

    // Strings
    testString("5:hello", "hello");
    testString("0:", "");
    testString("10:abcdefghij", "abcdefghij");
    testString("1:a", "a");

    // Lists
    testList("li42ee", 1);
    testList("li42e5:helloe", 2);
    testList("le", 0);

    // Nested list
    testNestedList();

    // Dictionaries
    testDictionary();
    testNestedDictionary();

    // Edge cases
    testTrailingDataDetection();
    testInvalidInput();

    // Summary
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";

    return (failed > 0) ? 1 : 0;
}
