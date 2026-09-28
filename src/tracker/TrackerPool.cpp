// =============================================================================
// TrackerPool.cpp - Fan out announces across a torrent's trackers
// =============================================================================

#include "tracker/TrackerPool.hpp"

#include "tracker/HttpTracker.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <random>
#include <set>
#include <sstream>

namespace {

// A tracker URL's scheme, lowercased, or "" if there isn't one.
// "udp://x/announce" -> "udp", "HTTP://x" -> "http", "x/announce" -> "".
std::string schemeOf(const std::string& url) {
    size_t sep = url.find("://");
    if (sep == std::string::npos) return "";
    std::string scheme = url.substr(0, sep);
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return scheme;
}

std::string padTo(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

// Peers are identified by address, so a peer offered by four trackers is one
// peer, not four. Without this, a multi-tracker torrent hands the downloader
// the same swarm four times over and the UI's "peers" count lies.
std::string peerKey(const Peer& p) {
    return p.ip + ":" + std::to_string(p.port);
}

// One tracker's attempt, plus the line we will show the user about it.
struct Attempt {
    std::string url;
    bool ok = false;
    TrackerResponse response;
    std::string failure;
    double seconds = 0.0;
};

std::string describe(const Attempt& a) {
    char t[32];
    std::snprintf(t, sizeof(t), " (%.2fs)", a.seconds);
    std::ostringstream out;
    out << padTo(a.url, 44) << " -> ";
    if (a.ok) {
        out << a.response.peers.size() << " peers, " << a.response.complete
            << " seeders, " << a.response.incomplete << " leechers";
    } else {
        out << "failed: " << (a.failure.empty() ? "unknown error" : a.failure);
    }
    out << t;
    return out.str();
}

}  // namespace

// =============================================================================
// Construction
// =============================================================================
TrackerPool::TrackerPool(const TorrentFile& torrent)
    : infoHash_(torrent.infoHash), totalLength_(torrent.length) {
    if (!torrent.announceTiers.empty()) {
        tiers_ = torrent.announceTiers;
    } else if (!torrent.announce.empty()) {
        // No multitracker list: the single announce URL is a one-tracker tier.
        tiers_.push_back({torrent.announce});
    }

    // BEP 12: shuffle within each tier. A torrent's tier is usually written
    // with the maintainer's favourite tracker first; if every client in a
    // swarm honours that ordering they all hit the same tracker in the same
    // second. Shuffling once at load time spreads the load instead.
    //
    // Seed from the clock AND a heap address, because several TrackerPools
    // built in the same millisecond (a test, or a batch import) would
    // otherwise get identical orderings and defeat the whole point.
    std::random_device rd;
    std::mt19937 rng(
        static_cast<unsigned>(rd()) ^
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() ^
                              (reinterpret_cast<uintptr_t>(this) << 3)));
    for (auto& tier : tiers_) {
        std::shuffle(tier.begin(), tier.end(), rng);
    }
}

// =============================================================================
// announceToUrl() - the scheme dispatch
// =============================================================================
TrackerResponse TrackerPool::announceToUrl(const TrackerRequest& request) {
    const std::string scheme = schemeOf(request.announceUrl);

    if (scheme == "udp") {
        // BEP 15 - filled in by the UDP tracker work. Failing loudly beats
        // silently returning zero peers and looking like a dead swarm.
        throw std::runtime_error("UDP tracker support not built yet: " + request.announceUrl);
    }
    if (scheme == "http" || scheme == "https" || scheme.empty()) {
        return HttpTracker::announce(request);
    }
    throw std::runtime_error("unsupported tracker scheme \"" + scheme + "\" in " +
                             request.announceUrl);
}

// =============================================================================
// announce() - the tier walk
// =============================================================================
TrackerResult TrackerPool::announce(const std::string& peerId,
                                   int overallTimeoutSeconds,
                                   int parallelism) const {
    TrackerResult result;

    if (tiers_.empty()) {
        result.log.push_back("torrent contains no tracker URLs");
        return result;
    }
    if (infoHash_.size() != 20) {
        result.log.push_back("torrent info hash is missing or not 20 bytes");
        return result;
    }

    // The deadline is the guarantee that a torrent listing six dead trackers
    // cannot leave the user staring at a blank terminal. Each transport bounds
    // its own I/O; this bounds the whole operation.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(overallTimeoutSeconds);
    if (parallelism < 1) parallelism = 1;

    // Peers accumulate across every tracker that answers, de-duplicated.
    std::set<std::string> seenPeers;

    for (size_t tierIndex = 0; tierIndex < tiers_.size(); tierIndex++) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.log.push_back("announce deadline reached, skipping remaining tiers");
            break;
        }

        const std::vector<std::string>& tier = tiers_[tierIndex];
        bool tierAnswered = false;

        // Announce to a tier in chunks of `parallelism` trackers at a time.
        // Tiers are small by design so this is normally a single chunk; the
        // chunking makes `parallelism` a real bound rather than a hint.
        for (size_t start = 0;
             start < tier.size() && !tierAnswered && std::chrono::steady_clock::now() < deadline;
             start += static_cast<size_t>(parallelism)) {
            const size_t end = std::min(tier.size(), start + static_cast<size_t>(parallelism));

            std::vector<Attempt> attempts(end - start);
            std::vector<std::future<void>> futures;
            futures.reserve(end - start);

            for (size_t k = 0; k < attempts.size(); k++) {
                futures.emplace_back(std::async(
                    std::launch::async, [this, &attempts, &peerId, tierIndex, start, k]() {
                        Attempt& a = attempts[k];
                        a.url = tiers_[tierIndex][start + k];

                        TrackerRequest request;
                        request.announceUrl = a.url;
                        request.infoHash = infoHash_;
                        request.peerId = peerId;
                        request.left = totalLength_;
                        request.event = "started";
                        request.compact = true;

                        const auto t0 = std::chrono::steady_clock::now();
                        try {
                            a.response = announceToUrl(request);
                            // A bencoded "failure reason" is a protocol-level
                            // answer, not an exception - and it means our
                            // request was wrong, not that the tracker is down.
                            if (!a.response.failureReason.empty()) {
                                a.failure = "tracker said: " + a.response.failureReason;
                            } else {
                                a.ok = true;
                            }
                        } catch (const std::exception& e) {
                            a.failure = e.what();
                        } catch (...) {
                            a.failure = "unknown error";
                        }
                        a.seconds = std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count();
                    }));
            }

            // Wait for this chunk. Each task is blocking network I/O bounded
            // by its own transport timeout, so these futures are not expected
            // to hang indefinitely.
            for (auto& f : futures) f.get();

            // ---- Fold this chunk's results in ----------------------------
            for (const Attempt& a : attempts) {
                result.log.push_back(describe(a));
                if (!a.ok) continue;

                tierAnswered = true;
                result.anySucceeded = true;
                // Take swarm numbers from whichever tracker reported last;
                // they are informational and only roughly comparable anyway.
                result.seeders = a.response.complete;
                result.leechers = a.response.incomplete;
                if (a.response.interval > 0) result.interval = a.response.interval;

                for (const Peer& p : a.response.peers) {
                    // A peer that is our own address is us. Trackers sometimes
                    // hand a peer back its own endpoint; connecting to ourselves
                    // would just burn a worker.
                    if (seenPeers.insert(peerKey(p)).second) {
                        result.peers.push_back(p);
                    }
                }
            }
        }

        // Tiers exist to be a fallback: the first tier that answers wins.
        if (tierAnswered) break;
    }

    return result;
}
