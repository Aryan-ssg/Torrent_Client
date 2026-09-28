// =============================================================================
// ConcurrentDownloader.cpp - The parallel download engine
// =============================================================================

#include "peer/ConcurrentDownloader.hpp"

#include "peer/PeerSession.hpp"
#include "peer/PieceDownloader.hpp"  // generatePeerId lives here today

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace {

// How long a claim may stand without any block arriving before a watchdog
// hands the piece to somebody else. Generous: a legitimate slow peer on a bad
// link can take a while, and reclaiming from a worker that was about to
// succeed just wastes a second full download of that piece.
constexpr int kStuckClaimSeconds = 90;

// How often the reporter thread wakes up to publish progress.
constexpr int kReportIntervalMs = 100;

// Per-worker consecutive failure budget. A worker that keeps hitting broken
// peers backs off instead of burning the whole peer list in seconds.
constexpr int kMaxWorkerFailures = 12;

}  // namespace

// =============================================================================
// download()
// =============================================================================
ConcurrentDownloader::Result ConcurrentDownloader::download(
    const TorrentFile& torrent,
    const std::vector<Peer>& peers,
    const std::string& outputPath,
    const std::string& ourPeerId,
    int workerCount,
    int timeoutSeconds,
    ProgressCallback onProgress,
    std::atomic<bool>* abortFlag) {

    Result result;

    // ---- Validate everything before creating a file or a thread ----------
    // Failing here is much nicer than failing halfway through, with a
    // half-written file on disk and a confusing error.
    if (peers.empty()) {
        result.error = "no peers to download from";
        return result;
    }
    if (workerCount <= 0) {
        result.error = "workerCount must be >= 1";
        return result;
    }
    if (torrent.infoHash.size() != 20 || torrent.pieces.empty() ||
        torrent.pieces.size() % 20 != 0) {
        result.error = "torrent metadata is incomplete";
        return result;
    }
    if (torrent.pieceLength <= 0 || torrent.length <= 0) {
        result.error = "torrent has no length or piece length";
        return result;
    }

    // ---- Shared state ----------------------------------------------------
    PieceManager manager(torrent, outputPath);
    const size_t pieceCount = manager.pieceCount();

    // Our own abort flag, ORed with any the caller supplied. Using one object
    // means the workers do not need to know which of the two exists.
    std::atomic<bool> ownAbort{false};
    auto aborted = [&]() { return ownAbort.load() || (abortFlag && abortFlag->load()); };

    // The peer pool: a shared work queue of addresses no worker has tried, so
    // two workers never dial the same dead peer at the same time. When it
    // empties, a worker may re-shuffle and retry, because a peer that was
    // unreachable a minute ago may be reachable now.
    std::mutex peerMutex;
    std::deque<Peer> peerQueue(peers.begin(), peers.end());
    std::atomic<int> peersConnected{0};
    std::atomic<size_t> peersTried{0};

    // Progress counters. Workers ONLY write these; the reporter thread reads
    // them. That is the entire concurrency contract for the progress path.
    std::atomic<long long> bytesDone{0};
    std::atomic<size_t> piecesDone{0};
    // Last time ANY worker stored a piece. Workers consult this before
    // deciding to back off, so a worker with nothing to do does not idle while
    // the rest of the run is making progress.
    std::atomic<long long> lastProgressMillis{0};

    lastProgressMillis.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    // ---- The reporter ----------------------------------------------------
    // A dedicated thread so the callback is invoked from exactly one place.
    // It also means the terminal is only ever written by one thread, which
    // removes the need for any UI-side locking.
    std::atomic<bool> runFinished{false};
    std::thread reporter;
    if (onProgress) {
        reporter = std::thread([&]() {
            while (!runFinished.load()) {
                DownloadProgress p;
                p.bytesDone = bytesDone.load();
                p.bytesTotal = manager.totalBytes();
                p.piecesDone = piecesDone.load();
                p.piecesTotal = pieceCount;
                p.peersConnected = peersConnected.load();
                p.peersTried = static_cast<int>(peersTried.load());
                p.finished = manager.complete();
                onProgress(p);
                std::this_thread::sleep_for(std::chrono::milliseconds(kReportIntervalMs));
            }
            // One final snapshot so the UI can render the final state without
            // the caller having to guess when the run really ended.
            DownloadProgress p;
            p.bytesDone = bytesDone.load();
            p.bytesTotal = manager.totalBytes();
            p.piecesDone = piecesDone.load();
            p.piecesTotal = pieceCount;
            p.peersConnected = peersConnected.load();
            p.peersTried = static_cast<int>(peersTried.load());
            p.finished = manager.complete();
            onProgress(p);
        });
    }

    // ---- A watchdog for claims that stopped moving -----------------------
    // Runs on its own timer and reclaims pieces whose worker has gone silent.
    // Without it, a single unresponsive peer holds its piece forever and the
    // download never reaches 100%.
    //
    // It waits on a condition variable rather than sleeping, so stopping it is
    // immediate. A plain sleep_for here would add its whole interval to the
    // wall-clock time of EVERY run, which is exactly the kind of thing that
    // makes a 200 ms download report as 5 seconds.
    std::mutex watchdogMutex;
    std::condition_variable watchdogWake;
    std::atomic<bool> watchdogStop{false};
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(watchdogMutex);
        while (!watchdogStop.load()) {
            watchdogWake.wait_for(lock, std::chrono::seconds(5),
                                  [&]() { return watchdogStop.load(); });
            if (watchdogStop.load()) break;
            lock.unlock();
            for (int i = 0; i < 8; i++) {
                PieceManager::ClaimToken token =
                    manager.reclaimStuckPiece(std::chrono::seconds(kStuckClaimSeconds));
                if (token == PieceManager::kNoClaim) break;
            }
            lock.lock();
        }
    });

    // ---- The worker body -------------------------------------------------
    auto worker = [&](int workerIndex) {
        (void)workerIndex;
        int failures = 0;
        // Rounds spent dialling peers without gaining a piece. A peer whose
        // bitfield contains nothing we still need is not an error, it is just
        // no use - and a swarm where every peer we try is either dead or
        // complete is a normal (if disappointing) state. Counting these is what
        // stops the worker spinning through the peer list at full speed
        // forever, which is what an unguarded "no useful peer -> redial" does.
        int idleRounds = 0;
        // When we last actually stored a piece. Used to decide whether to
        // pause: the run as a whole may be progressing (other workers) even
        // while this one has nothing to do, and sleeping then would just make
        // a healthy download look slow.
        auto lastProgressAt = std::chrono::steady_clock::now();
        auto refreshProgressClock = [&]() {
            lastProgressAt = std::chrono::steady_clock::now();
            lastProgressMillis.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        };
        // Adopt the shared value, in case another worker got there first.
        lastProgressAt = std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(lastProgressMillis.load()));

        PeerSession::Options options;
        options.infoHash = &torrent.infoHash;
        options.peerId = ourPeerId;
        options.pieceCount = pieceCount;
        options.pieceLength = torrent.pieceLength;
        options.totalLength = torrent.length;
        options.connectTimeoutSeconds = timeoutSeconds;
        options.pipelineDepth = 8;
        // A read timeout several times the connect timeout: failing to reach a
        // peer is normal, but dropping a peer that has already connected
        // because it is slow costs us the whole handshake.
        options.readTimeoutSeconds = timeoutSeconds * 3;

        PeerSession session;

        while (!aborted() && !manager.complete()) {
            // 1. Make sure we have a session worth talking to. A session is
            //    worth keeping while the peer still owns a piece we lack.
            bool needNewPeer = false;
            if (!session.usable()) {
                needNewPeer = true;
            } else {
                // The peer may own nothing useful any more: every piece it
                // had is now claimed or owned. Asking it for more would just
                // waste a round trip, so retire the session.
                needNewPeer = manager.nextPieceToFetch(&session.available()) == PieceManager::kNotFound;
            }

            if (needNewPeer) {
                if (session.usable()) peersConnected.fetch_sub(1);
                session.close();
                idleRounds++;

                // Only pause when the download as a whole has made no progress
                // for a while. If other workers are still storing pieces there
                // is no point idling: the right move is to keep cycling the
                // peer list, because a peer that has just come online may hold
                // the exact piece we are missing.
                const bool stalled = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                             .count() -
                                         lastProgressMillis.load() >
                                     3000;

                // A long run of full passes with nothing to show for it means
                // this swarm cannot give us the remaining pieces. Say so
                // rather than spinning.
                if (stalled && idleRounds > static_cast<int>(peers.size()) * 4) {
                    ownAbort.store(true);
                    break;
                }
                if (stalled) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }

                Peer peer;
                {
                    std::lock_guard<std::mutex> lock(peerMutex);
                    if (peerQueue.empty()) {
                        // Everyone has been tried. Refill it so a peer that
                        // has come back online since we last tried can be
                        // retried - peers flap constantly in a real swarm.
                        std::vector<Peer> again(peers.begin(), peers.end());
                        std::shuffle(again.begin(), again.end(),
                                     std::mt19937(std::random_device{}()));
                        peerQueue.assign(again.begin(), again.end());
                    }
                    peer = peerQueue.front();
                    peerQueue.pop_front();
                    peersTried.fetch_add(1);
                }

                try {
                    session.connect(peer, options);
                    peersConnected.fetch_add(1);
                    failures = 0;
                } catch (const std::exception&) {
                    // Most peers in a real swarm are unreachable at any given
                    // moment. This is expected, not an error worth reporting.
                    failures++;
                    if (failures >= kMaxWorkerFailures) {
                        ownAbort.store(true);
                        break;
                    }
                    continue;
                }
            }

            // 2. Claim the next piece this peer actually has.
            const size_t index = manager.nextPieceToFetch(&session.available());
            if (index == PieceManager::kNotFound) continue;

            const PieceManager::ClaimToken token = manager.claimPiece(index);
            if (token == PieceManager::kNoClaim) continue;  // lost the race

            // Blocks arriving keep the claim's clock alive, so a piece that is
            // genuinely progressing is never mistaken for a stalled one.
            session.setProgressFn([&]() { manager.heartbeatClaim(token); });

            // 3. Fetch it over the connection we already have.
            std::vector<uint8_t> expectedHash(
                torrent.pieces.begin() + static_cast<long>(index) * 20,
                torrent.pieces.begin() + static_cast<long>(index) * 20 + 20);

            std::vector<uint8_t> data;
            std::string error;
            if (!session.fetchPiece(index, expectedHash, data, error)) {
                manager.releasePiece(index);
                failures++;
                if (session.choked()) {
                    // Fairness, not failure: wait a moment and use the same
                    // connection, it will very likely unchoke us.
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    failures--;  // do not count a choke against the peer
                }
                continue;
            }

            // 4. The gate. storePiece re-verifies the hash, so the SHA-1 was
            //    checked twice - once here, once in the manager.
            const size_t len = manager.pieceLength(index);
            if (manager.storePiece(index, data, token)) {
                bytesDone.fetch_add(static_cast<long long>(len));
                piecesDone.fetch_add(1);
                failures = 0;
                idleRounds = 0;  // real progress: the peer list is worth retrying
                refreshProgressClock();
            } else {
                // storePiece refused it (or our claim was reclaimed while we
                // were downloading). Either way, make the piece available
                // again rather than stranding it.
                manager.releasePiece(index);
                failures++;
            }
        }

        if (session.usable()) peersConnected.fetch_sub(1);
        session.close();
    };

    // ---- Run the workers --------------------------------------------------
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(workerCount));
    for (int w = 0; w < workerCount; w++) {
        workers.emplace_back(worker, w);
    }
    for (auto& t : workers) t.join();

    watchdogStop.store(true);
    watchdogWake.notify_all();  // wake it now rather than waiting out the timer
    watchdog.join();

    runFinished.store(true);
    if (reporter.joinable()) reporter.join();

    // ---- The verdict, computed only once every thread has finished --------
    result.piecesDownloaded = piecesDone.load();
    result.ok = manager.complete();
    if (!result.ok) {
        result.error = aborted()
                           ? "download aborted before every piece was verified"
                           : ("stopped with " +
                              std::to_string(pieceCount - result.piecesDownloaded) +
                              " piece(s) still missing");
    }
    return result;
}
