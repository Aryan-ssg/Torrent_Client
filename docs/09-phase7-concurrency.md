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

## The code, piece by piece (classes, functions, algorithms, techniques)

The same tour as Phases 1–6 — with the added dimension that *every* shared
piece of state now has an owner and a lock.

### The user-defined types

| Type | Header | Its one job | Java cousin |
|---|---|---|---|
| `ConcurrentDownloader` (**class**) | `include/peer/ConcurrentDownloader.hpp` + `.cpp` | the worker pool; all-`static` | a static service class wrapping an `ExecutorService` |
| `ConcurrentDownloader::Result` (**struct**) | same header | `{ok, error, piecesDownloaded}` | a result record |
| `worker` (**lambda**) | `src/peer/ConcurrentDownloader.cpp` | one worker thread's entire body | a `Callable<Chunk>` task |
| `PieceManager` (reused, now locked) | `include/piece/PieceManager.hpp` | the shared piece table + the disk | a `synchronized` service |
| `FakePeer` (reused, extended) | `include/peer/FakePeer.hpp` | N seeders + the test knobs | a stub server |
| `std::thread`, `std::atomic`, `std::mutex` | the standard library | threads, flags, the error lock | `Thread`, `AtomicBoolean`, a lock |

Note what's *not* a class: the worker body is a **lambda** (`auto worker =
[&](int workerIndex) { … };`) captured by reference. In C++ a lambda is a
first-class anonymous function object, and using one here instead of inventing
a `Worker` class is idiomatic — it needs no name, no header, and no virtual
dispatch.

An honest footnote: the lambda takes an `int workerIndex` parameter that the
body never uses. It's still passed in (`threads.emplace_back(worker, w)`),
because the worker identity is the natural thing to accept when you spawn a
pool and the first thing you'd want for a per-worker log line or a shard
policy. The current load balancing is `peers[idx % peers.size()]` — based on
the *piece*, not the worker — so the parameter is genuinely dead today.

### `ConcurrentDownloader::download()` — the whole engine, function by function

| Piece of it | Technique | What it does |
|---|---|---|
| argument validation up front | **fail fast before side effects** | empty `peers`, `workerCount <= 0`, and a malformed info hash / pieces blob each return an error result before a single thread starts |
| `PieceManager manager(torrent, outputPath)` | **one shared object** | every worker uses this instance; it is the only thing they have in common |
| `std::atomic<bool> aborted{false}` | **lock-free shared flag** | set once by the first worker that gives up; polled by all |
| `std::atomic<size_t> storedCount{0}` | **atomic counter** (`fetch_add`) | the run's progress counter, incremented from N threads |
| `std::mutex errorMutex` + `std::string firstError` | **mutex-guarded single writer** | the *one* human-readable message; only one thread ever writes it, guarded anyway |
| the `worker` lambda | **the claim → fetch → store loop** | see the step table below |
| `threads.emplace_back(worker, w)` | **thread spawn with an argument** | one thread per worker index |
| `for (auto& t : threads) t.join()` | **join-all barrier** | the main thread waits for every worker before deciding `ok` |
| `if (!aborted.load() && manager.complete())` | **post-conditions after the join** | the result is only `ok` if nobody aborted *and* the table is fully owned |

The worker loop, annotated by technique:

```cpp
if (aborted.load()) return;                  // atomic poll: someone gave up
if (manager.complete()) return;              // all pieces owned -> done
size_t idx = manager.nextPieceToFetch();     // first PENDING (or kNotFound)
if (idx == PieceManager::kNotFound) return;  // nothing left to do
if (!manager.claimPiece(idx)) continue;      // lost the race -> take another
const Peer& peer = peers[idx % peers.size()];// per-piece load balancing
auto r = PieceDownloader::download(…);       // Phase 5, unchanged
if (r.ok) { if (manager.storePiece(idx, r.data)) { storedCount.fetch_add(1);
                                              failures = 0; }
            else { manager.releasePiece(idx); failures++; } }
else { manager.releasePiece(idx); failures++;
       if (failures >= kMaxWorkerFailures && !aborted.exchange(true)) { … } }
```

Four techniques to name:

- **The check-then-act race, handled by the manager.** `nextPieceToFetch()`
  and `claimPiece()` are two calls, so *two workers can see the same
  `idx`* — that's a benign race, and the loser's `claimPiece()` returns
  `false` and the `continue` sends it around for another piece. The **claim
  is the atomic unit**; everything else is allowed to be racy.
- **`aborted.exchange(true)`** — not `aborted = true`. `exchange` *atomically*
  returns the old value, so only the **first** worker to give up enters the
  error-recording block (`&&` short-circuits the rest). That's a one-word
  solution to "everyone writes the same error at once."
- **`failures = 0` on success** — a per-worker retry budget that resets on
  progress, so one flaky peer doesn't doom an otherwise healthy run.
- **Join as a barrier, then decide.** The result is computed *after* every
  thread has finished, which is why `firstError` needs no atomic type of its
  own: by the time it's read, there can be no concurrent writer.

### `PieceManager` — what changed for thread safety

The class didn't grow; it grew *disciplines*. Every method that touches
`states_` now takes the lock first:

```cpp
bool PieceManager::claimPiece(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= states_.size() || states_[index] != State::kPending) {
        return false;                       // someone else got there first
    }
    states_[index] = State::kClaimed;
    return true;
}
```

- **`std::lock_guard<std::mutex> lock(mutex_)`** — the RAII lock. It locks on
  construction and **unlocks on scope exit**, including when the function
  returns early or throws. (Java: `synchronized (mutex) { … }` — same
  guarantee, different syntax. This is the C++ idiom you will see in every
  modern codebase.)
- **Lock held across the disk write in `storePiece`** — deliberate. The
  `seekp`+`write` is a compound operation on a *shared* `fstream`; two
  threads seeking and writing the same stream would interleave and corrupt
  each other. Holding the lock makes "check the hash, write the offset, mark
  owned" one indivisible step, and keeps `complete()` from ever observing a
  half-finished state. It trades a little parallelism (disk writes serialise)
  for a total absence of a whole bug class — the right trade at this scale.
- **`complete()` / `completedCount()` / `hasPiece()` are `const`** and still
  lock, which is why `mutex_` is `mutable` (Phase 6's note, now load-bearing).

### `FakePeer` — the test-side additions

| Addition | Technique | Why it exists |
|---|---|---|
| `setMaxConnections(n)` | bounded serve loop | the seeder exits after exactly `n` visitors, so `join()` is a sync point and connection *counts* are assertable |
| `setServeDelayMs(ms)` | `sleep_for` in `handleSeeder` | fakes a slow link so a wall-clock speedup test is meaningful |
| `servedCount()` | a counter incremented in `acceptLoop` | summing it across seeders is how the test proves no piece was fetched twice |
| `shutdown()` | close the listen socket | unblocks a parked `accept()` so `join()` can't hang if a run aborted early |

One thing to be honest about: `servedCount_` and `acceptLoop_` are touched by
the server thread and read by the test thread, with no atomics. That's safe
*only* because `join()` provides the happens-before edge — after the join, the
counter is stable. A textbook case of "the thread join *is* your
synchronisation."

### The built-in types we rely on here

| Built-in | What it gives us | Java equivalent |
|---|---|---|
| `std::thread` | one concurrent path of execution | `Thread` / `ExecutorService` |
| `std::vector<std::thread>` + `join()` | the pool, and the barrier that ends it | `ExecutorService.shutdown()` + `awaitTermination()` |
| `std::atomic<bool>` | a lock-free flag with `.load()` / `.exchange()` | `AtomicBoolean` |
| `std::atomic<size_t>` + `fetch_add(1)` | an exact concurrent counter | `AtomicLong.incrementAndGet()` |
| `std::mutex` + `lock_guard` | RAII mutual exclusion | `synchronized` / `ReentrantLock` |
| lambdas + `[&]` capture | anonymous tasks sharing the enclosing scope | an inner class capturing effectively-final locals |
| `enum class State` | the claim lifecycle | `enum State` |
| `std::chrono::steady_clock` | monotonic timing for the speedup test | `System.nanoTime()` |
| `this_thread::sleep_for` | the fake link latency | `Thread.sleep()` |
| `size_t` arithmetic (`idx % peers.size()`) | index-based load balancing | `idx % peers.size()` |

Note `steady_clock` (not `system_clock`): the speedup test must not be
disturbed by NTP adjustments or DST — a monotonic clock is the correct tool
for measuring elapsed time.

### The algorithms/patterns this phase is a textbook example of

1. **Worker pool / thread-per-task** — N threads, each running the same
   claim-fetch-store loop, joined at the end.
2. **Check-then-act with an atomic resolution** — `nextPieceToFetch()` then
   `claimPiece()`; the race is expected and resolved by the claim.
3. **Atomic state machine** — `PENDING → CLAIMED → OWNED/RELEASED` as the unit
   of work that keeps concurrent download correct.
4. **Lock-free signalling** — `std::atomic<bool>` for the abort flag, read on
   every loop iteration.
5. **Compare-and-swap-style "first one wins"** — `aborted.exchange(true)`
   returning the old value so exactly one thread records the error.
6. **RAII locking** — `lock_guard` releases on every exit path, throw
   included.
7. **Coarse-grained locking with a justified invariant** — one mutex around
   the piece table *and* the write, trading throughput for correctness.
8. **Atomic counters for progress** — `storedCount.fetch_add(1)`.
9. **Retry budgets with reset-on-success** — `failures = 0` after a good
   piece; abort only after `kMaxWorkerFailures`.
10. **Join as a barrier, then decide** — all result computation happens after
    every thread has finished, so no lock is needed for the final read.
11. **Index-based load balancing** — `peers[idx % N]`, distributing by work
    item rather than by worker.
12. **Test instrumentation as a first-class API** — counters and knobs
    (`servedCount`, `setServeDelayMs`, `setMaxConnections`) built into the
    fake peer so concurrency claims are *measurable*.
13. **Monotonic timing for performance assertions** — `steady_clock`
    thresholds instead of guessing.
14. **Reuse** — Phase 5's `PieceDownloader` and Phase 6's `PieceManager` are
    used unmodified; the only *new* logic is the loop, the claim protocol, and
    the abort story.

### A one-paragraph mental model

> `ConcurrentDownloader::download` validates its arguments, builds **one**
> shared `PieceManager`, declares two atomics (`aborted`, `storedCount`) and
> one mutex (for the single error string), then spawns `workerCount` threads
> running the same lambda. Each worker loops: poll `aborted`, ask for a
> `PENDING` piece, **claim it atomically** (losers `continue` and try
> another), fetch it from `peers[idx % N]` with Phase 5's verified
> `PieceDownloader`, and `storePiece` it — releasing the claim and counting a
> failure on any error, and after three consecutive failures flipping the
> `aborted` flag via `exchange` so every worker stops and one records the
> error. The main thread `join`s everyone, then declares `ok` only if nobody
> aborted and `manager.complete()` is true. The techniques underneath: a
> worker pool, check-then-act resolved by an atomic claim, an ownership state
> machine, lock-free abort signalling, RAII locking, coarse-grained locking
> across the disk write, atomic progress counters, retry budgets, a join
> barrier, index-based load balancing, and test instrumentation that makes the
> concurrency claims measurable.

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