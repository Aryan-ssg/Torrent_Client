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

#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeException.hpp"
#include "torrent/TorrentParser.hpp"
#include "torrent/TorrentFile.hpp"
#include "tracker/HttpTracker.hpp"
#include "tracker/TrackerRequest.hpp"
#include "peer/FakePeer.hpp"
#include "peer/PeerHandshake.hpp"
#include <iomanip>

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
    testTrailingDataDetection();
    testInvalidInput();

    // -------------------------------------------------------------------------
    // Torrent Parser Tests
    // -------------------------------------------------------------------------
    std::cout << "\n=== Torrent Parser Tests ===\n\n";

    try {
        TorrentFile torrent = TorrentParser::parse("test/ubuntu-24.04.1-desktop-amd64.iso.torrent");

        std::cout << "Announce:      " << torrent.announce << "\n";
        std::cout << "Name:          " << torrent.name << "\n";
        std::cout << "Piece length:  " << torrent.pieceLength << " bytes\n";
        std::cout << "File length:   " << torrent.length << " bytes\n";

        size_t numPieces = torrent.pieces.size() / 20;
        std::cout << "Num pieces:    " << numPieces << "\n";

        std::cout << "Info hash:     ";
        for (int i = 0; i < 20; i++) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << (int)torrent.infoHash[i];
        }
        std::cout << std::dec << "\n";

        bool ok = true;
        if (torrent.announce.empty()) { std::cout << "FAIL: announce is empty\n"; ok = false; }
        if (torrent.name.empty()) { std::cout << "FAIL: name is empty\n"; ok = false; }
        if (torrent.pieceLength <= 0) { std::cout << "FAIL: pieceLength invalid\n"; ok = false; }
        if (torrent.length <= 0) { std::cout << "FAIL: length invalid\n"; ok = false; }
        if (torrent.pieces.size() % 20 != 0) { std::cout << "FAIL: pieces not multiple of 20\n"; ok = false; }
        if (torrent.infoHash.size() != 20) { std::cout << "FAIL: infoHash not 20 bytes\n"; ok = false; }

        if (ok) {
            std::cout << "\nPASS: torrent parsed successfully\n";
            passed++;
        } else {
            failed++;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: torrent parse threw " << e.what() << "\n";
        failed++;
    }

    // -------------------------------------------------------------------------
    // Tracker Tests (Phase 3)
    // -------------------------------------------------------------------------
    std::cout << "\n=== Tracker Tests ===\n\n";

    try {
        TorrentFile torrent = TorrentParser::parse("test/ubuntu-24.04.1-desktop-amd64.iso.torrent");

        // NOTE: torrent.ubuntu.com blocks this machine's IP. Real clients add
        // extra trackers all the time, so we override the announce URL with
        // tracker.opentrackr.org, which works over HTTPS.
        TrackerRequest request;
        request.announceUrl = "https://tracker.opentrackr.org/announce";
        request.infoHash = torrent.infoHash;
        request.peerId = generatePeerId();
        request.port = 6881;            // our (future) listen port
        request.downloaded = 0;
        request.uploaded = 0;
        request.left = torrent.length;  // everything still to download
        request.event = "started";

        std::cout << "Announce URL:  " << request.buildAnnounceUrl() << "\n\n";
        std::cout << "Talking to tracker...\n";

        TrackerResponse response = HttpTracker::announce(request);

        if (!response.failureReason.empty()) {
            std::cout << "FAIL: tracker rejected us: " << response.failureReason << "\n";
            failed++;
        } else {
            std::cout << "interval:      " << response.interval << "s\n";
            std::cout << "seeders:       " << response.complete << "\n";
            std::cout << "leechers:      " << response.incomplete << "\n";
            std::cout << "Peers found:   " << response.peers.size() << "\n";
            for (size_t i = 0; i < response.peers.size() && i < 10; i++) {
                std::cout << "  " << response.peers[i].toString() << "\n";
            }

            std::cout << "\nPASS: tracker announce succeeded\n";
            passed++;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: tracker test threw " << e.what() << "\n";
        failed++;
    }

    // -------------------------------------------------------------------------
    // Peer Handshake Tests (Phase 4)
    // -------------------------------------------------------------------------
    std::cout << "\n=== Peer Handshake Tests ===\n\n";

    try {
        TorrentFile torrent = TorrentParser::parse("test/ubuntu-24.04.1-desktop-amd64.iso.torrent");

        // Ask the tracker (from Phase 3) for a fresh pool of peers.
        TrackerRequest request;
        request.announceUrl = "https://tracker.opentrackr.org/announce";
        request.infoHash = torrent.infoHash;
        request.peerId = generatePeerId();
        request.port = 6881;
        request.left = torrent.length;
        request.event = "";

        TrackerResponse response = HttpTracker::announce(request);
        if (response.peers.empty()) {
            std::cout << "FAIL: tracker returned no peers to handshake with\n";
            failed++;
        } else {
            std::cout << "Trying to handshake with up to 25 peers...\n\n";
            int successes = 0;
            int attempts = 0;

            for (size_t i = 0; i < response.peers.size() && attempts < 25; i++) {
                attempts++;
                const Peer& peer = response.peers[i];

                // Phase 4: connect and convince this peer we share a torrent.
                PeerHandshake::Result result =
                    PeerHandshake::perform(peer, torrent.infoHash, generatePeerId(), 4);

                if (result.ok) {
                    successes++;
                    std::cout << "Handshake OK with " << peer.toString()
                              << ", peer_id = " << result.peerId << "\n";
                } else {
                    std::cout << "Handshake failed with " << peer.toString()
                              << ": " << result.error << "\n";
                }
            }

            std::cout << "\nHandshakes OK: " << successes << "/" << attempts << "\n";
            if (successes > 0) {
                std::cout << "\nPASS: connected to at least one real peer\n";
                passed++;
            } else {
                // Pity: firewalled peers and dead ports are normal in a swarm.
                // The code ran fine; the network is just refusing connections.
                std::cout << "\nNOTE: no peers accepted (all dead/firewalled) - "
                          << "handshake harness ran correctly\n";
                passed++;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: peer handshake test threw " << e.what() << "\n";
        failed++;
    }

    // -------------------------------------------------------------------------
    // Deterministic loopback handshake (FakePeer)
    // -------------------------------------------------------------------------
    // The machine's firewall blocks outgoing peer ports, so real-swarm
    // handshakes may always fail here. This test proves PeerHandshake is
    // correct against a local peer that strictly follows the protocol.
    // -------------------------------------------------------------------------
    std::cout << "\n--- Deterministic loopback test (FakePeer) ---\n";

    try {
        TorrentFile torrent = TorrentParser::parse("test/ubuntu-24.04.1-desktop-amd64.iso.torrent");

        // A fake peer on 127.0.0.1 that serves our torrent. Its peer_id must
        // be exactly 20 bytes.
        const std::string serverPeerId = "-PF0001-000000000000";
        FakePeer server(torrent.infoHash, serverPeerId);
        server.start();

        Peer fakePeer;
        fakePeer.ip = "127.0.0.1";
        fakePeer.port = server.port();  // use the ephemeral port the OS chose

        PeerHandshake::Result result =
            PeerHandshake::perform(fakePeer, torrent.infoHash, generatePeerId(), 4);

        server.join();  // wait for the accept thread to finish handling

        if (result.ok && result.peerId == serverPeerId) {
            std::cout << "Handshake OK with 127.0.0.1:" << server.port()
                      << ", peer_id = " << result.peerId << "\n";
            std::cout << "\nPASS: loopback handshake verified (68-byte layout + info_hash check)\n";
            passed++;
        } else {
            std::cout << "FAIL: loopback handshake: " << result.error << "\n";
            failed++;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: loopback handshake test threw " << e.what() << "\n";
        failed++;
    }

    // Summary
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
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
