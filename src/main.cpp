// =============================================================================
// main.cpp - Test Suite for the Bencode Decoder
// =============================================================================
//
// This file contains test cases to verify the Bencode decoder works correctly.
// It's like a JUnit test class in Java, but using a simple manual testing approach.
//
// JAVA COMPARISON:
// In Java, you'd use:
//   @Test
//   public void testInteger() {
//       BencodeValue result = BencodeDecoder.decode("i42e");
//       assertEquals(42, result.asInteger());
//   }
//
// In C++ (this project), we use simple functions that print PASS/FAIL
// =============================================================================

#include <iostream>    // For std::cout (like System.out.println in Java)
#include <vector>      // For std::vector (like ArrayList in Java)
#include <cassert>     // For assert() (used in some test frameworks)

#include "bencode/BencodeDecoder.hpp"   // Our decoder class
#include "bencode/BencodeException.hpp" // Our custom exception

// =============================================================================
// TEST TRACKING VARIABLES
// =============================================================================
// These count how many tests passed and failed
// In Java, JUnit automatically tracks this; here we do it manually
static int passed = 0;  // Like AtomicInteger in Java
static int failed = 0;

// =============================================================================
// TEST HELPER FUNCTIONS
// =============================================================================
// These functions run a test and print PASS or FAIL
// They're like test methods in JUnit, but using functions instead of methods

// Test an integer value
// Example: testInteger("i42e", 42) should PASS
static void testInteger(const std::string& input, long long expected) {
    try {
        // Try to decode the input
        BencodeValue result = BencodeDecoder::decode(input);

        // Check if result is an integer AND matches expected value
        if (result.getType() == BencodeValue::INTEGER && result.asInteger() == expected) {
            std::cout << "PASS: \"" << input << "\" -> " << result.asInteger() << "\n";
            passed++;  // Increment passed counter
        } else {
            std::cout << "FAIL: \"" << input << "\" -> unexpected value\n";
            failed++;  // Increment failed counter
        }
    } catch (const std::exception& e) {
        // If any exception occurred, the test failed
        std::cout << "FAIL: \"" << input << "\" threw " << e.what() << "\n";
        failed++;
    }
}

// Test a string value
// Example: testString("5:hello", "hello") should PASS
static void testString(const std::string& input, const std::string& expected) {
    try {
        BencodeValue result = BencodeDecoder::decode(input);

        // Check if result is a string AND matches expected value
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

// Test a list value (only checks size, not contents)
// Example: testList("li42ee", 1) should PASS (list with 1 element)
static void testList(const std::string& input, size_t expectedSize) {
    try {
        BencodeValue result = BencodeDecoder::decode(input);

        // Check if result is a list AND has expected size
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

// Test nested list: [[42]]
// This tests that lists can contain other lists
static void testNestedList() {
    try {
        std::string input = "lli42eee";  // Bencode for [[42]]
        BencodeValue result = BencodeDecoder::decode(input);

        // Verify the structure:
        // - Result is a list with 1 element
        // - That element is also a list with 1 element
        // - That inner element is integer 42
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

// Test dictionary: {"cow": "moo", "spam": 42}
static void testDictionary() {
    try {
        std::string input = "d3:cow3:moo4:spami42ee";  // Bencode for {"cow": "moo", "spam": 42}
        BencodeValue result = BencodeDecoder::decode(input);

        if (result.getType() == BencodeValue::DICT) {
            const auto& map = result.asDict();  // Get the map (like getMap() in Java)

            // Find the "cow" entry
            auto cowIt = map.find("cow");  // Like map.get("cow") but returns iterator
            // Find the "spam" entry
            auto spamIt = map.find("spam");

            // Check if "cow" maps to "moo"
            bool cowMatch = cowIt != map.end() &&
                cowIt->second.getType() == BencodeValue::STRING &&
                cowIt->second.stringValueAsUtf8() == "moo";

            // Check if "spam" maps to 42
            bool spamMatch = spamIt != map.end() &&
                spamIt->second.getType() == BencodeValue::INTEGER &&
                spamIt->second.asInteger() == 42;

            // All checks must pass
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

// Test nested dictionary: {"info": {"key": "value"}}
static void testNestedDictionary() {
    try {
        std::string input = "d4:infod3:key5:valueee";  // Bencode for {"info": {"key": "value"}}
        BencodeValue result = BencodeDecoder::decode(input);

        if (result.getType() == BencodeValue::DICT) {
            const auto& outer = result.asDict();

            // Find "info" key
            auto infoIt = outer.find("info");
            if (infoIt != outer.end() && infoIt->second.getType() == BencodeValue::DICT) {
                // "info" maps to another dictionary
                const auto& inner = infoIt->second.asDict();

                // Find "key" in the inner dictionary
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

// Test that trailing data is detected as an error
// Example: "i42eEXTRA" should fail because "EXTRA" is not part of the integer
static void testTrailingDataDetection() {
    try {
        BencodeDecoder::decode("i42eEXTRA");  // This should throw!
        std::cout << "FAIL: trailing data not detected\n";
        failed++;
    } catch (const BencodeException& e) {
        // We expect an exception here - this is the correct behavior
        std::cout << "PASS: trailing data detected -> " << e.what() << "\n";
        passed++;
    }
}

// Test that invalid input is detected as an error
// Example: "xyz" is not valid Bencode (should start with i, l, d, or digit)
static void testInvalidInput() {
    try {
        BencodeDecoder::decode("xyz");  // This should throw!
        std::cout << "FAIL: invalid input not detected\n";
        failed++;
    } catch (const BencodeException& e) {
        // We expect an exception here - this is the correct behavior
        std::cout << "PASS: invalid input detected -> " << e.what() << "\n";
        passed++;
    }
}

// =============================================================================
// MAIN FUNCTION (Entry point of the program)
// =============================================================================
// In Java: public static void main(String[] args)
int main() {
    std::cout << "=== Bencode Decoder Tests ===\n\n";

    // -------------------------------------------------------------------------
    // Integer Tests
    // -------------------------------------------------------------------------
    // Format: i<number>e
    testInteger("i42e", 42);            // Positive integer
    testInteger("i0e", 0);              // Zero
    testInteger("i-7e", -7);            // Negative integer
    testInteger("i123456789e", 123456789);  // Large integer

    // -------------------------------------------------------------------------
    // String Tests
    // -------------------------------------------------------------------------
    // Format: <length>:<content>
    testString("5:hello", "hello");      // Simple string
    testString("0:", "");                // Empty string
    testString("10:abcdefghij", "abcdefghij");  // Longer string
    testString("1:a", "a");             // Single character

    // -------------------------------------------------------------------------
    // List Tests
    // -------------------------------------------------------------------------
    // Format: l<items>e
    testList("li42ee", 1);              // List with one integer
    testList("li42e5:helloe", 2);       // List with integer and string
    testList("le", 0);                  // Empty list

    // -------------------------------------------------------------------------
    // Nested Structure Tests
    // -------------------------------------------------------------------------
    // Lists and dictionaries can contain other lists/dictionaries
    testNestedList();                   // [[42]]
    testDictionary();                   // {"cow": "moo", "spam": 42}
    testNestedDictionary();             // {"info": {"key": "value"}}

    // -------------------------------------------------------------------------
    // Error Detection Tests
    // -------------------------------------------------------------------------
    // These should fail (throw exceptions) because input is invalid
    testTrailingDataDetection();        // "i42eEXTRA" has extra data
    testInvalidInput();                 // "xyz" is not valid Bencode

    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------
    // Print how many tests passed/failed (like JUnit summary)
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";

    // Return 0 if all tests passed, 1 if any failed
    // This is used by test frameworks to determine success/failure
    // In Java: System.exit(failed > 0 ? 1 : 0);
    return (failed > 0) ? 1 : 0;
}

/*
 * JAVA COMPARISON SUMMARY:
 *
 * 1. System.out.println() vs std::cout:
 *    Java: System.out.println("Hello");
 *    C++:  std::cout << "Hello" << std::endl;
 *
 * 2. try-catch is the same in both languages!
 *    Java: try { ... } catch (Exception e) { ... }
 *    C++:  try { ... } catch (const std::exception& e) { ... }
 *
 * 3. Vector/ArrayList:
 *    Java: ArrayList<BencodeValue> list = new ArrayList<>();
 *    C++:  std::vector<BencodeValue> list;
 *
 * 4. Map/HashMap:
 *    Java: HashMap<String, BencodeValue> map = new HashMap<>();
 *    C++:  std::map<std::string, BencodeValue> map;
 *
 * 5. Function syntax:
 *    Java: public static void testInteger(String input, long expected)
 *    C++:  static void testInteger(const std::string& input, long long expected)
 *
 * 6. 'const' keyword:
 *    Java doesn't have const (it has 'final' which is different)
 *    C++: const std::string& means "I promise not to modify this string"
 */
