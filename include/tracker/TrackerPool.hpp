#pragma once

// =============================================================================
// TrackerPool.hpp - Ask every tracker in the torrent until someone answers
// =============================================================================
//
// A real .torrent rarely has one tracker. BEP 12 added `announce-list`, a list
// of TIERS, where each tier is a set of interchangeable trackers:
//
//   "announce-list": [ [ "trackerA", "trackerB" ], [ "backupC" ] ]
//
// The rule is: every tracker in tier 1 is interchangeable, so try them all
// before giving up on the tier; only if the whole tier is unreachable do you
// fall through to tier 2. Tier 1 is tried before tier 2, always.
//
// Two things this class exists to get right:
//
//  1. SCHEME DISPATCH. A tracker URL can be http://, https:// OR udp://, and
//     they are completely different protocols on the wire. UdpTracker does
//     BEP 15; HttpTracker does HTTP. This class looks at the scheme and calls
//     the right one, so callers never have to care.
//
//  2. NOT WASTING THE USER'S TIME. Announcing to a dead tracker naively costs
//     a full network timeout, and a torrent with three dead trackers before
//     the live one would sit there for minutes. So trackers inside a tier are
//     announced CONCURRENTLY under one shared deadline, and the overall
//     announce is bounded by a deadline too.
//
// BEP 12 also asks for two small courtesies, both implemented here: shuffle
// the trackers within a tier (so ten clients with the same torrent don't all
// hit the same one first and knock it over), and promote a tracker to the
// front of its tier once it answers.
//
// Java parallel: like a failover list of service endpoints, except the
// endpoints are dialled in parallel and grouped into tiers of interchangeable
// providers.
// =============================================================================

#include "torrent/TorrentFile.hpp"
#include "tracker/Peer.hpp"
#include "tracker/TrackerRequest.hpp"
#include "tracker/TrackerResponse.hpp"

#include <cstddef>
#include <string>
#include <vector>

// What one announce() call produced, from every tracker that answered.
struct TrackerResult {
    // The union of peers from every tracker that responded, de-duplicated.
    std::vector<Peer> peers;

    // Swarm numbers, taken from the most recently successful announce.
    long long seeders = 0;
    long long leechers = 0;
    long long interval = 0;

    // One human-readable line per tracker, for the CLI/UI to print. Example:
    //   "udp://tracker.example:6969/announce -> 42 peers (0.08s)"
    //   "http://dead.example/announce       -> failed: timed out"
    std::vector<std::string> log;

    // True if at least one tracker answered.
    bool anySucceeded = false;
};

class TrackerPool {
public:
    // Reads the torrent's announce / announce-list and builds the tier list.
    // A torrent with only `announce` yields a single one-tracker tier.
    //
    //   useFallbackTrackers  append one extra tier of well-known public
    //                        trackers, tried only if every tracker the torrent
    //                        itself names has failed. On by default, because
    //                        plenty of healthy torrents point at a tracker
    //                        that is dead or blocks your network.
    explicit TrackerPool(const TorrentFile& torrent, bool useFallbackTrackers = true);

    // Announce to every tracker, tiers in order, concurrently within a tier.
    //
    //   peerId                  our 20-byte peer id, sent to every tracker
    //   overallTimeoutSeconds   hard ceiling for the whole announce
    //   parallelism             how many trackers in a tier to hit at once
    //
    // Never throws: a tracker that fails is recorded in the log and skipped.
    // Returns whatever peers could be gathered.
    TrackerResult announce(const std::string& peerId,
                           int overallTimeoutSeconds = 30,
                           int parallelism = 4) const;

    // The tier list this pool will use (for tests and the UI's tracker line).
    const std::vector<std::vector<std::string>>& tiers() const { return tiers_; }

    // Announce to one specific URL, dispatching on scheme. Throws
    // std::runtime_error on failure - the caller (announce) catches it.
    static TrackerResponse announceToUrl(const TrackerRequest& request);

private:
    std::vector<std::vector<std::string>> tiers_;
    bool useFallbackTrackers_ = true;
    std::vector<uint8_t> infoHash_;
    long long totalLength_ = 0;
};
