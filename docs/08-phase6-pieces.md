# 08 — Phase 6: Pieces to Disk

## What we built and why

Phase 5 downloaded *one* verified piece. Phase 6 turns that into a real
**download engine**: remember which pieces we own, fetch the ones we don't,
and — the new part — **write verified piece data to the correct place in the
output file**. By the end, a whole synthetic file (8 pieces, 119,688 bytes)
lives on disk, byte-for-byte identical to what the seeder served.

The milestone: `PASS: full 119688-byte download matches content (8/8 pieces)`
— plus resume and corruption-recovery, because a *real* client has to be
honest with both the network *and* the disk.

## The two new ideas

Everything Phase 6 does hangs on two responsibilities:

1. **Ownership.** Know, for every piece, whether we have it yet. A real
   torrent can have thousands of pieces and arrive in any order; we need a
   map of "which pieces are mine".
2. **The disk gate.** Never write a byte that hasn't been SHA-1-checked —
   and never *trust the disk's own memory*, either: a file that was
   half-written, truncated, or corrupted before must be re-verified on start.

Both live in the `PieceManager` (`include/piece/PieceManager.hpp`).

### The ownership map: PENDING → CLAIMED → OWNED

Each piece is one of three states — its whole life cycle as a single enum:

```
PENDING -> CLAIMED -> OWNED     (normal download)
PENDING -> CLAIMED -> PENDING   (worker failed: try that piece again later)
```

- **`PENDING`** — not fetched yet.
- **`CLAIMED`** — a downloader is fetching it *right now*. Phase 7 reads this
  from many threads at once, so the map is guarded by a `std::mutex`, and
  "claimed" is exactly what stops two workers from wasting two downloads on
  the same piece.
- **`OWNED`** — verified by SHA-1 *and* written to disk.

### Preallocating the file (so any piece can land anywhere)

Real pieces don't arrive in order. So before fetching *anything*, the manager
creates the file at its **full final size** with one call — `open()` +
`ftruncate()` — and opens it read/write. Now *any* piece can be written at its
absolute offset the moment it arrives:

```
offset of piece i  =  i * pieceLength
```

(On most filesystems that truncate produces a **sparse file**: the empty
regions cost no disk until they're actually written — which is exactly why we
can afford to preallocate even a 6 GB Ubuntu ISO.)

## `storePiece()` — the last gate before bytes touch disk

This method is deliberately paranoid, because it is the last line of defense:

```cpp
1. exact size  ->  bytes.size() == pieceLength(index), or reject
2. exact hash  ->  SHA-1(bytes) == torrent.pieces[index*20 .. +20], or reject
3. then write  ->  seek(pieceOffset); write; flush
4. mark OWNED
```

The order matters: **nothing touches disk on a mismatch**. The piece stays
`CLAIMED`... no — it flat-out returns `false`, so the caller knows the fetch
was bad, and the piece simply isn't owned. A wrong hash means "the downloader
wasted a connection", not "the file now contains garbage". (Leaving the claim
in place would deadlock the downloader, so the caller — see Phase 7 — releases
it explicitly for retry.)

`storePiece` is also where the *short read* trap lives. When we later read a
piece back from disk, TCP-like partial reads don't happen, but a file can
still return fewer bytes than asked. The internal `readPiece()` checks
`file.gcount()` and **shrinks** the result to what actually arrived — so a
truncated-on-disk piece hashes *short* ("wrong size") and is marked not-owned,
instead of hashing zero-padding as if it were real. Safe resume in one line of
care.

## `scanDisk()` — resume, and "never trust the disk either"

The companion to `storePiece`. On a restart, the manager is handed the same
output path with **zero memory** of what happened. `scanDisk()` reads every
piece back from the file, re-hashes it, and marks `OWNED` only the pieces that
still match:

```cpp
for each piece i:
    bytes = readPiece(file, i * pieceLength, pieceLength(i))
    states_[i] = (bytes.size() == pieceLength(i) &&
                  hashMatches(bytes, torrent.pieces[i*20]) )
                   ? OWNED : PENDING;
```

Why is this honest, not wasteful? Because it makes the *state* true: a
half-written download or a few corrupted bytes costs only the pieces that
actually failed — the other 98% is spared the re-download. (Production clients
usually skip the re-hash by storing a checkpoint file; for *learning*,
verifying on every resume is far more honest and shows the real technique.)

## `FileException` — the disk layer's error language

File work fails in ways that aren't the network's fault: no permission,
out of space, the path lives in a read-only directory. Those get their own
exception type, `FileException`, so a caller can tell "disk trouble" apart
from "peer trouble" (the `BencodeException`/`NetException` siblings from
earlier phases).

## The connection model: one connection per piece

A real client manages long-lived connections and requests *many* pieces over
each one. Our Phase 6 downloader keeps it simpler: **one TCP connection per
piece** — dial the seeder, handshake, fetch, close, next piece. That's exactly
how the test seeder is configured:

```cpp
server.setMaxConnections(pieceCount);   // serve one visitor per piece
```

It's a bit chatty for production, but it exercises the full handshake per
piece and keeps the download loop beautifully simple — the perfect scaffold
Phase 7 builds its parallelism on.

## What the tests prove

The synthetic torrent is 8 pieces (7 × 16 KiB + one 5,000-byte tail), which
exercises both full and short pieces. Four tests, four distinct fears:

1. **Full download** — loop `nextPieceToFetch()` → `download()` →
   `storePiece()` until `complete()`, then byte-compare the file. Proves
   download + SHA-1 gate + offset writes.
2. **Resume** — download only pieces 0–2, start a *brand-new* manager over the
   same file, `scanDisk()`. It must see 3/8 owned and fetch exactly the other
   5. Proves disk is the only memory.
3. **Disk corruption** — write everything, then flip one byte *directly in the
   file* (the disk lied). A fresh manager + `scanDisk()` must NOT trust it,
   refetch that one piece, and end with a correct file.
4. **storePiece gate** — feed it a good hash of a *different* piece, a
   truncated blob, and pure garbage. All three must be rejected.

```
=== Piece Manager + Disk Tests (Phase 6) ===

PASS: full 119688-byte download matches content (8/8 pieces)
PASS: resume - scanDisk() recovered 3/8 pieces, fetched the rest, final bytes match
PASS: disk corruption - scanDisk() caught 1 bad piece, refetched it, final bytes match
PASS: storePiece rejects wrong piece / wrong size / garbage
```

## The code, piece by piece (classes, functions, algorithms, techniques)

The same tour as Phases 1–5.

### The user-defined types

| Type | Header | Its one job | Java cousin |
|---|---|---|---|
| `PieceManager` (**class**) | `include/piece/PieceManager.hpp` + `.cpp` | own the piece map, gate the writes, resume | a service class with a lock |
| `PieceManager::State` (**enum class**) | same header | `kPending` / `kClaimed` / `kOwned` | an enum |
| `FileException` (**class**) | `include/piece/FileException.hpp` | "the disk layer failed" | a custom `IOException` |
| `hashMatches(bytes, expected20)` (file-local) | `src/piece/PieceManager.cpp` | the one SHA-1 comparison, in one place | a private static helper |
| `readPiece(file, offset, len)` (file-local) | same file | read one piece's byte range back off disk | a helper method |
| `PieceDownloader` (reused) | `include/peer/PieceDownloader.hpp` | Phase 5's verified fetch | — |

`PieceManager` is the first class in the project that holds **mutable state
and a lock** — a genuine object with a lifecycle, not a bag of static helpers.

### `PieceManager` — members and methods

| Member | Technique | What it does |
|---|---|---|
| `enum class State { kPending, kClaimed, kOwned }` | **`enum class`** (type-safe enum) | the per-piece lifecycle; `enum class` prevents accidental mixing with plain ints |
| `states_` (`std::vector<State>`) | **parallel-array state table** | one entry per piece — the ownership map, in its simplest possible form |
| `mutable std::mutex mutex_` | **mutex-guarded state** (Phase 7 needs it; harmless now) | guards `states_`; `mutable` lets `const` methods like `complete()` still lock |
| `file_` (`std::fstream`) + `outputPath_` + `fullLength_` | **RAII file handle held for the object's life** | opened once in the constructor, closed by the destructor |
| `TorrentFile torrent_` (**by value**) | **composition / defensive copy** | the manager owns its metadata; callers can't mutate it underneath |
| constructor | **resource acquisition + preallocation** | counts pieces, `states_.assign(n, kPending)`, `open(O_CREAT\|O_RDWR)` + `ftruncate()` to the full size, then opens an `fstream` for read/write |
| `pieceLength(index)` (**const**) | **clamping arithmetic** | `min(pieceLength, fullLength_ - index*pieceLength)` — the short-tail rule in one line |
| `pieceCount()` / `completedCount()` / `complete()` / `hasPiece()` | **derived queries** | each takes the lock, then scans `states_` |
| `nextPieceToFetch()` (**const**) | **first-match scan** | first `kPending` index, or `kNotFound` (a `static constexpr size_t(-1)` sentinel, the same trick as `std::string::npos`) |
| `storePiece(index, bytes)` | **the gate** | size check → hash check → seek+write at `index*pieceLength` → mark `kOwned`; all under the lock |
| `scanDisk()` | **reconciliation pass** | re-read and re-hash every piece; only matches become `kOwned` |
| `kNotFound` | **named sentinel constant** | "no piece" without a magic number or an exception |

Two members deserve a note:

- **`mutable std::mutex`.** `complete()` and `nextPieceToFetch()` are `const`
  methods — they promise not to change the object — but they *must* take the
  lock to read `states_` safely. `mutable` is exactly the keyword for "this
  const method is allowed to touch this member." Small C++ detail, big
  readability win: the lock lives with the data it protects.
- **`TorrentFile torrent_` stored by value.** The manager takes a *copy* of
  the torrent metadata, so the `TorrentFile` the caller holds can't change
  under it (and the manager's `computeInfoHash`-era fields are self-contained).
  Cheap insurance: `TorrentFile` is six fields, not a 6 GB buffer.

### The two file-local helpers

```cpp
bool hashMatches(const std::vector<uint8_t>& bytes, const uint8_t* expected20) {
    std::vector<uint8_t> actual(SHA_DIGEST_LENGTH);
    SHA1(bytes.data(), bytes.size(), actual.data());
    return std::equal(actual.begin(), actual.end(), expected20);
}
```

- **`hashMatches`** — **DRY by extraction**: the identical "SHA-1 and compare"
  logic was needed in `storePiece` *and* `scanDisk`, so it lives in one
  function used by both. (The same gate, applied twice, in two places, is how
  bugs get in.)
- **`readPiece(file, offset, len)`** — reads a byte range back and then, the
  subtle part, **shrinks on a short read**:

```cpp
std::streamsize got = file.gcount();
if (got >= 0 && static_cast<size_t>(got) < len) {
    bytes.resize(static_cast<size_t>(got));   // don't hash stale zeros
}
```

Technique: **`gcount()`-based short-read detection**. `std::fstream::read`
does *not* throw when it reads less than asked — it sets failbit and leaves the
rest of your buffer untouched (here: zero-initialised). Hashing that buffer
would "verify" a truncated piece against garbage. Shrinking to what actually
arrived makes the caller's `size == expected` check fire, the hash fail, and
the piece stay not-owned — the safe outcome. This one line is a bug class
removed.

### `storePiece()` — the gate, in order

```
1. index in range?                      else return false
2. bytes.size() == pieceLength(index)?  else return false    ← size gate
3. hashMatches(bytes, torrent.pieces + index*20)?  else return false  ← hash gate
4. seekp(index * pieceLength); write; flush         ← the ONLY write path
5. states_[index] = kOwned; return true
```

The ordering is the design: **both checks happen before any byte reaches the
disk.** A wrong hash never lands in the file; the function just returns
`false` and the caller can release the piece for a retry. And because step 4
is the only place anything is written, "verified data only" is a property of
the *class*, not a habit of its callers.

### The preallocation trick (`open` + `ftruncate`)

```cpp
int fd = ::open(outputPath.c_str(), O_CREAT | O_RDWR, 0644);
if (fd < 0) throw FileException("Cannot create output file: " + outputPath);
if (::ftruncate(fd, static_cast<off_t>(fullLength_)) != 0) { ::close(fd); throw …; }
::close(fd);
```

Technique: **POSIX file setup with manual cleanup**. Create-or-open the path
at the exact final size (sparse on most filesystems), then close the raw
descriptor and reopen via `std::fstream` for the long-term read/write work.
Note the **error-path discipline**: the `ftruncate` failure closes `fd`
*before* throwing, so the descriptor can't leak on the unhappy path. (After
`::close(fd)` the `std::fstream` is the sole owner, and its destructor closes
it — RAII from here on.)

### The built-in types we rely on here

| Built-in | What it gives us | Java equivalent |
|---|---|---|
| `std::fstream` | read+write binary file, seekable | `RandomAccessFile` |
| `fstream::seekp` / `write` / `flush` | write at an absolute offset | `raf.seek(pos); raf.write(…)` |
| `fstream::gcount()` | "how many bytes did I *really* read?" | the `int` return of `InputStream.read` |
| `std::vector<State>` | the ownership map | `State[]` / `EnumSet` |
| `enum class` | type-safe state values | `enum State` |
| `std::mutex` + `lock_guard` | mutual exclusion | `synchronized` / `ReentrantLock` |
| `mutable` | let a `const` method take the lock | (no direct equivalent) |
| `static constexpr size_t kNotFound` | named sentinel | `-1` with a named constant |
| POSIX `open`/`ftruncate`/`close` | preallocate the file | `RandomAccessFile.setLength()` |
| OpenSSL `SHA1()` | the digest | `MessageDigest("SHA-1")` |
| `std::equal` | byte-exact hash compare | `Arrays.equals` |
| `std::min` | the short-tail piece length | `Math.min` |

### The algorithms/patterns this phase is a textbook example of

1. **Ownership state machine** — `PENDING → CLAIMED → OWNED` per piece, with
   a release path back to `PENDING`: a lifecycle modelled in one enum.
2. **The verify-before-write gate** — every byte hashed *before* it is stored,
   with the check order making "garbage on disk" structurally impossible.
3. **Preallocation for order-independence** — size the file once, then write
   any piece anywhere.
4. **Hash-indexed flat storage** — `pieces.data() + index*20` carves this
   piece's expected hash out of one contiguous blob.
5. **Reconciliation on resume** (`scanDisk`) — treat the filesystem as an
   untrusted cache and re-verify it.
6. **DRY by helper extraction** — one `hashMatches` for both call sites.
7. **Short-read detection** (`gcount()`) — never trust a buffer you didn't
   fully fill.
8. **RAII resource ownership** — the `fstream` lives as long as the manager;
   the raw `fd` is closed on every path, including the throw path.
9. **Mutex-guarded shared state** — one lock, `mutable` so `const` queries can
   take it; every public method locks (this is the foundation Phase 7 builds
   parallel workers on).
10. **Named sentinels** — `kNotFound` instead of a magic `-1`.
11. **Layer-specific exceptions** — `FileException` for disk faults, so callers
    can tell "the disk said no" from "a peer said no".
12. **Reuse** — Phase 5's `PieceDownloader` supplies verified bytes; the
    manager only decides *where they go* and *whether they're allowed to*.

### A one-paragraph mental model

> `PieceManager` is a **state machine over a piece table, wrapped around one
> open file**. Its constructor counts the pieces from the torrent's hash blob,
> marks them all `PENDING`, and preallocates the output file to its full size
> (so any piece can land at any offset later). Its workers ask
> `nextPieceToFetch()` for a `PENDING` index, `storePiece()` it when it
> arrives — and `storePiece` is the whole safety story: exact size, then exact
> SHA-1 against `pieces[index*20]`, and only then a `seekp`+`write` at
> `index*pieceLength`, all under one mutex, after which the piece becomes
> `kOwned`. `scanDisk()` is the same hash gate run in reverse over the file
> itself, so a restart trusts only what still verifies. The techniques
> underneath: an ownership state machine, verify-before-write, preallocation,
> flat-blob indexing, reconciliation, DRY helper extraction, `gcount()`
> short-read safety, RAII file ownership, and a `mutable` mutex guarding the
> one piece of shared mutable state.

## Java parallels

`PieceManager` is like a `RandomAccessFile`-based chunk writer plus a
`BitSet`/boolean array of "which chunks are done", guarded by a lock so that
multiple download threads can claim different chunks without duplicating work.
`scanDisk()` is a startup reconciliation pass — like re-validating local state
against a trusted source before trusting it. (Java's `try-with-resources`
fstream equivalent is our RAII-style constructor/destructor pair: the file is
opened once for the manager's lifetime and closed automatically.)

## What Phase 6 taught us

- **Ownership is a 3-state map**, and "claimed" is the seed of parallelism.
- **Verify before write** — the hash gate sits *inside* `storePiece`, as the
  very last gate, for a reason.
- **Preallocate, then write anywhere** — arrival order no longer matters.
- **Re-verify the disk on resume** — the network isn't the only thing that
  lies; a half-written file must cost only the pieces it actually ruined.
- **Short reads**: size-check *after* reading, not just sizes *before*
  writing.

---

*Next: [09 — Phase 7: Downloading in Parallel](09-phase7-concurrency.md)*