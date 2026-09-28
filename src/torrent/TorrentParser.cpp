#include "torrent/TorrentParser.hpp"
#include "bencode/BencodeDecoder.hpp"
#include "bencode/BencodeException.hpp"
#include <climits>   // For LLONG_MAX (overflow guard below)
#include <fstream>
#include <sstream>
#include <openssl/sha.h>

// How deep skipBencodeValue() may recurse. BencodeDecoder already caps real
// decoding at 200 (this parser decodes the file fully before touching the raw
// byte walkers below), so this is defence-in-depth for the skip walker too.
static constexpr size_t kMaxSkipDepth = 200;

namespace {

// Same overflow guard the bencode decoder uses: digit runs come from an
// untrusted .torrent file, and overflowing signed long long is undefined
// behavior in C++. Refuse instead.
long long checkedAppendDigit(long long value, uint8_t b, size_t pos) {
    long long digit = b - '0';
    if (value > (LLONG_MAX - digit) / 10) {
        throw BencodeException(
            "Number overflows 64-bit signed range at position " + std::to_string(pos));
    }
    return value * 10 + digit;
}

}  // namespace

std::vector<uint8_t> TorrentParser::readFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        throw BencodeException("Cannot open file: " + filepath);
    }
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );
    return bytes;
}

static size_t skipBencodeValue(const std::vector<uint8_t>& raw, size_t pos, size_t depth = 0) {
    if (depth > kMaxSkipDepth) {
        throw BencodeException("Bencode nesting deeper than " +
                               std::to_string(kMaxSkipDepth) + " while skipping");
    }
    if (pos >= raw.size()) {
        throw BencodeException("Unexpected end while skipping value");
    }

    uint8_t b = raw[pos];

    if (b == 'i') {
        // Integer: i<number>e — skip to 'e'
        pos++; // skip 'i'
        while (pos < raw.size() && raw[pos] != 'e') pos++;
        return pos + 1; // past 'e'
    } else if (b == 'l' || b == 'd') {
        // List or dict: skip opening, then skip each element until 'e'
        pos++; // skip 'l' or 'd'
        while (pos < raw.size() && raw[pos] != 'e') {
            pos = skipBencodeValue(raw, pos, depth + 1);
        }
        return pos + 1; // past 'e'
    } else if (b >= '0' && b <= '9') {
        // String: <len>:<bytes>
        long long strLen = 0;
        while (pos < raw.size() && raw[pos] != ':') {
            strLen = checkedAppendDigit(strLen, raw[pos], pos);
            pos++;
        }
        pos++; // skip ':'
        return pos + strLen;
    } else {
        throw BencodeException("Unexpected byte while skipping value at " + std::to_string(pos));
    }
}

size_t TorrentParser::findInfoValueStart(const std::vector<uint8_t>& raw) {
    // We need to walk the top-level dictionary in raw bytes,
    // properly skipping string values, to find the "info" key.
    //
    // A naive scan for "4:info" fails because that pattern can appear
    // inside string values. Instead we parse the top-level dict structure:
    //   d <key> <value> <key> <value> ... e
    // where keys are always bencode strings.

    if (raw.empty() || raw[0] != 'd') {
        throw BencodeException("Torrent file does not start with a dictionary");
    }

    size_t pos = 1; // skip opening 'd'

    while (pos < raw.size() && raw[pos] != 'e') {
        // Each key is a bencode string. Read its length.
        if (raw[pos] < '0' || raw[pos] > '9') {
            throw BencodeException("Expected string key in top-level dictionary at " + std::to_string(pos));
        }

        long long keyLen = 0;
        size_t keyStart = pos;
        while (pos < raw.size() && raw[pos] != ':') {
            keyLen = checkedAppendDigit(keyLen, raw[pos], pos);
            pos++;
        }
        pos++; // skip ':'

        // Now check if this key is "info"
        if (keyLen == 4 && pos + 4 <= raw.size()
            && raw[pos] == 'i' && raw[pos + 1] == 'n'
            && raw[pos + 2] == 'f' && raw[pos + 3] == 'o') {
            return pos + 4; // position after "info" — start of the value
        }

        // Not the info key — skip the key bytes and then skip the value
        pos += keyLen; // skip key bytes
        pos = skipBencodeValue(raw, pos); // skip the value
    }

    throw BencodeException("Could not find 'info' key in torrent file");
}

size_t TorrentParser::findDictEnd(const std::vector<uint8_t>& raw, size_t dictStart) {
    if (dictStart >= raw.size() || raw[dictStart] != 'd') {
        throw BencodeException("Expected 'd' at dictionary start");
    }

    size_t pos = dictStart;
    int depth = 0;

    while (pos < raw.size()) {
        uint8_t b = raw[pos];

        if (b == 'd' || b == 'l') {
            depth++;
            pos++;
        } else if (b == 'e') {
            depth--;
            if (depth == 0) {
                return pos;
            }
            pos++;
        } else if (b == 'i') {
            // Integer: i<number>e — skip to 'e'
            pos++;
            while (pos < raw.size() && raw[pos] != 'e') pos++;
            pos++; // skip 'e'
        } else if (b >= '0' && b <= '9') {
            // String: <len>:<bytes> — skip it entirely
            long long strLen = 0;
            while (pos < raw.size() && raw[pos] != ':') {
                strLen = checkedAppendDigit(strLen, raw[pos], pos);
                pos++;
            }
            pos++; // skip ':'
            pos += strLen;
        } else {
            throw BencodeException("Unexpected byte in dictionary scan at position " + std::to_string(pos));
        }
    }

    throw BencodeException("Unterminated dictionary starting at position " + std::to_string(dictStart));
}

std::vector<uint8_t> TorrentParser::computeInfoHash(const std::vector<uint8_t>& raw) {
    size_t infoValueStart = findInfoValueStart(raw);
    size_t infoDictEnd = findDictEnd(raw, infoValueStart);

    // Hash from the 'd' to the matching 'e' (inclusive)
    const uint8_t* data = raw.data() + infoValueStart;
    size_t length = infoDictEnd - infoValueStart + 1;

    std::vector<uint8_t> hash(SHA_DIGEST_LENGTH);
    SHA1(data, length, hash.data());

    return hash;
}

TorrentFile TorrentParser::parse(const std::string& filepath) {
    // Step 1: Read raw bytes
    std::vector<uint8_t> raw = readFile(filepath);

    // Step 2: Decode the bencoded data
    BencodeValue root = BencodeDecoder::decode(raw);

    if (root.getType() != BencodeValue::DICT) {
        throw BencodeException("Torrent file must be a bencoded dictionary");
    }

    const auto& top = root.asDict();

    TorrentFile torrent;

    // Step 3: Extract announce
    auto announceIt = top.find("announce");
    if (announceIt != top.end() && announceIt->second.getType() == BencodeValue::STRING) {
        torrent.announce = announceIt->second.stringValueAsUtf8();
    }

    // Step 3b: Extract announce-list (BEP 12) when present.
    //
    // Shape: a list of TIERS, each tier a list of tracker URLs. Tier order
    // matters (try tier 0 first); order within a tier does not (they are
    // interchangeable, which is exactly why the pool announces them all
    // concurrently).
    //
    // "Never trust the input": a torrent is untrusted data, so every level is
    // type-checked. A malformed announce-list is ignored rather than fatal -
    // `announce` is still there as a fallback, and a client that refuses to
    // start because of a broken extra key is worse than one that copes.
    auto alIt = top.find("announce-list");
    if (alIt != top.end() && alIt->second.getType() == BencodeValue::LIST) {
        for (const auto& tierValue : alIt->second.asList()) {
            if (tierValue.getType() != BencodeValue::LIST) continue;  // not a tier

            std::vector<std::string> tier;
            for (const auto& urlValue : tierValue.asList()) {
                if (urlValue.getType() != BencodeValue::STRING) continue;
                std::string url = urlValue.stringValueAsUtf8();
                if (!url.empty()) tier.push_back(url);
            }
            // An empty tier is meaningless; drop it rather than burn an
            // announce attempt discovering there is nothing to announce to.
            if (!tier.empty()) torrent.announceTiers.push_back(std::move(tier));
        }
    }

    // Step 4: Extract info dictionary fields
    auto infoIt = top.find("info");
    if (infoIt == top.end() || infoIt->second.getType() != BencodeValue::DICT) {
        throw BencodeException("Torrent file missing 'info' dictionary");
    }

    const auto& info = infoIt->second.asDict();

    // name
    auto nameIt = info.find("name");
    if (nameIt != info.end() && nameIt->second.getType() == BencodeValue::STRING) {
        torrent.name = nameIt->second.stringValueAsUtf8();
    }

    // piece length
    auto plIt = info.find("piece length");
    if (plIt != info.end() && plIt->second.getType() == BencodeValue::INTEGER) {
        torrent.pieceLength = plIt->second.asInteger();
    }

    // length (single-file torrent)
    auto lenIt = info.find("length");
    if (lenIt != info.end() && lenIt->second.getType() == BencodeValue::INTEGER) {
        torrent.length = lenIt->second.asInteger();
    }

    // pieces (raw SHA-1 hashes)
    auto piecesIt = info.find("pieces");
    if (piecesIt != info.end() && piecesIt->second.getType() == BencodeValue::STRING) {
        torrent.pieces = piecesIt->second.asString();
    }

    // Step 5: Compute info hash from RAW bencoded bytes
    torrent.infoHash = computeInfoHash(raw);

    return torrent;
}

// -----------------------------------------------------------------------------
// allAnnounceUrls()
// -----------------------------------------------------------------------------
// Flatten the tiers into one list. If the torrent has no announce-list, the
// single `announce` URL is the whole list - that keeps callers from having to
// special-case "old torrent without multitracker support".
// -----------------------------------------------------------------------------
std::vector<std::string> TorrentFile::allAnnounceUrls() const {
    std::vector<std::string> urls;
    for (const auto& tier : announceTiers) {
        for (const auto& url : tier) {
            urls.push_back(url);
        }
    }
    if (urls.empty() && !announce.empty()) {
        urls.push_back(announce);
    }
    return urls;
}
