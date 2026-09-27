# 09 — Phase 7: Downloading in Parallel

## What we built and why

Phases 5–6 were strictly **sequential**: fetch one piece, write it, fetch the
next — like a single waiter carrying one plate at a time. Phase 7 is where a
BitTorrent client earns its speed: **many workers talking to many peers at the
same time**. This is the difference between watching a download crawl and
watching a swarm chew through a file.

The milestone:

```
PASS: 4 workers/4 peers fetched all 12 pieces exactly once each (12 connections, file byte-verified)
PASS: parallel speedup - 8 pieces in 253 ms (sequential would be ~960 ms)
```

## The shape of a parallel download

`ConcurrentDownloader` (`include/peer/ConcurrentDownloader.hpp`,
`src/peer/ConcurrentDownloader.cpp`) is the engine. It answers one method:

```cpp
static Result download(torrent, peers, outputPath, ourPeerId,
                       workerCount, timeoutSeconds);
```

and spawns `workerCount` **worker threads** (Java: think an
`ExecutorService` with a fixed pool). Every worker runs the same loop:

```
1. is the run aborted? done? no more pieces?  -> stop
2. ask the PieceManager for the next PENDING piece
3. claim it  ->  only one worker becomes its owner
4. download it from ONE peer, hash-verified (Phase 5's PieceDownloader)
5. storePiece()  ->  re-verified, written at the right offset (Phase 6)
6. on failure: release the claim, count the failure
```

## Why "claim" is the whole trick

Without claiming, two workers could pick the *same* piece, both download it,
and waste an entire connection. The PieceManager's ownership map (Phase 6)
grows a new pair of atomic operations for exactly this:

- **`claimPiece(i)`** — atomically move piece `i` from `PENDING` to
  `CLAIMED`, *only if it was still `PENDING`*. Returns false for a loser.
- **`releasePiece(i)`** — a failed download moves the piece back to
  `PENDING` so another worker (or the same one later) can retry it.

Both are guarded by the manager's mutex, so simultaneous claims are decided
the way a mutex decides everything: one winner per piece, everyone else moves
on to the *next* PENDING piece.

This is why the test asserts the seeders served **exactly 12 connections for
12 pieces** — a duplicate download would have shown up as connection #13.

## Which peer asks which piece?

Real clients pick a peer *per piece*. We do the simplest load-balanced version:

```cpp
const Peer& peer = peers[idx % peers.size()];   // piece i -> peer i % N
```

`idx` is the *piece index*, so piece 0 → peer 0, piece 1 → peer 1, and so on
around the swarm. That spreads the load evenly **by construction** — the reason
this is safer than pinning each worker to one peer is a story the test suite
already paid for: if the claim races hand one worker most of the pieces, a
pinned worker over-dials "its" peer and the download chokes. Working *per
piece* means no single seeder ever carries more weight than its share.

## Failing honestly: retries, then abort

A swarm is full of dead peers. Each worker tolerates failure up to a cap
(`kMaxWorkerFailures = 3`), and one good piece wipes the slate clean — so a
healthy peer is never abandoned because of one hiccup. But when a worker has
failed three times on the same piece, it's the mayday: it sets a shared
`std::atomic<bool> aborted`, and every other worker sees the flag at its next
loop and stops too. The first failure is remembered (under its own mutex) for
the `Result::error`, so the caller gets a truthful story: `piece 9 failed
after retries (…)` rather than a silent partial file. Everything shares one
`firstError`; the run reports `ok=false` and the manager's file simply isn't
marked complete — exactly the honest failure every phase has drilled into us.

Counting is done with a second `std::atomic<size_t>` (`storedCount`), so
`Result::piecesDownloaded` is exact even though several threads punch it
concurrently.

## The PieceManager, now thread-safe

Everything these workers share is the PieceManager, and the only mutable field
in it is the ownership map — so the whole object reduces to **one mutex
guarding `states_`**. Every public method (`nextPieceToFetch`, `claimPiece`,
`releasePiece`, `storePiece`, `completedCount`, `complete`, `scanDisk`) locks
before touching the map. `storePiece` even holds the lock *during the disk
write*, so the seek+write is atomic with respect to claim decisions: two
workers can never write the same offset, and `complete()` can never be
half-true. Cheap, obviously correct, and until a profiler says otherwise,
perfectly fine at this scale.

## The FakePeer: simulating slow, many

The FakePeer seeder gains two knobs that make the timing test possible:

- **`setMaxConnections(n)`** — serve exactly `n` visitors then exit, so the
  test can assert connection *counts*.
- **`setServeDelayMs(ms)`** — sleep while serving each connection, standing in
  for a slow modem peer. Four seeders each sleeping 120 ms per connection, in
  parallel, take ~120–250 ms total — where one would need ~960 ms.
- **`servedCount()`** — the total connections actually handled, the number the
  "no duplicate downloads" assertion sums.
- **`shutdown()`** — close the listen socket so a blocked `accept()` fails and
  `join()` returns even if a run aborted early. (Without it, a seeder parked
  awaiting connections that will never come would make the test *hang* — a
  real lesson the suite taught us during development.)

## The tests and what they prove

- **Correctness + no duplicates.** 4 workers, 4 seeders, a 12-piece synthetic
  file. Pass requires `ok`, all 12 pieces, the *sum* of `servedCount` == 12
  (no piece fetched twice), and the file byte-matching the synthesized content.
- **Speedup.** 8 pieces, 4 seeders, 120 ms of fake latency each. Sequential
  = 8 × 120 ms ≈ 960 ms. The test passes only if the parallel wall-clock is
  under 2/3 of that — typically ~250 ms.

```
=== Parallel Download Tests (Phase 7) ===

PASS: 4 workers/4 peers fetched all 12 pieces exactly once each (12 connections, file byte-verified)
PASS: parallel speedup - 8 pieces in 253 ms (sequential would be ~960 ms)
```

The second line is the whole point of the phase, measured honestly: ~4× faster
because four peers genuinely worked at the same time.

## The honest limit (what we did NOT do)

Our synthetic pieces are 16 KiB — **one block each** — so "one piece per
connection" is the *full* parallel picture here. Real clients go one level
deeper: for a 256 KiB Ubuntu piece they split it into sixteen 16 KiB blocks and
fetch **different blocks of the same piece from different peers**, reassembling
at the end. Our `PieceDownloader` already assembles blocks in the right order
per `begin` offset, so block-level parallelism is a natural next refinement —
and the docs mark it as such rather than pretending we built it.

## Java parallels

The whole architecture maps cleanly onto the Java world:

- worker threads ≈ `ExecutorService` fixed pool;
- the claim protocol ≈ `ConcurrentHashMap.computeIfAbsent`-style atomic "I
  take this chunk" (or an `AtomicBoolean` per piece);
- `std::atomic<bool> aborted` ≈ a `volatile boolean runFlag` (well, a proper
  `AtomicBoolean`);
- the mutex-guarded PieceManager ≈ a synchronized `chunkTracker`;
- each worker's failure counter ≈ retry budget in a resilient downloader.

## What Phase 7 taught us

- **Parallelism = workers + a safe shared map.** Claiming one piece at a time
  is the discrete unit that keeps concurrent download correct.
- **Load-balance by index, not by worker** — per-piece peer selection prevents
  one overloaded seeder from choking a run.
- **Abort is a shared flag, errors are shared facts** — atomics for state,
  a mutex for the one human-readable message.
- **Measure honestly**: the timing test compares wall-*clock* against a
  sequential baseline, not a guess.

---

*Next: [10 — Reference](10-reference.md)*