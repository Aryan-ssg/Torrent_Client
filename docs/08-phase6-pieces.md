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