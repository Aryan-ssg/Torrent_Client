# 11 — Reference: Reusable Blocks, the File Map, and Testing

A shelf of everything reusable plus the "where is everything" map. Use it as a
cheat sheet.

## The two binaries

| Binary | What it is |
|---|---|
| `build/peerflow` | the command-line client — this is what you use |
| `build/peerflow-tests` | the automated test suite (38 checks) |

Both are thin entry points over one static library (`peerflow_core`), so the
engine compiles once and links twice. The tests live in their own binary so the
client stays quick to build and the suite can be run on its own.

## Reusable building blocks

### `TcpSocket` — our one network primitive (`src/net/`)

Every phase that talks to the internet uses this class:

| Method | What it guarantees |
|---|---|
| `connect(host, port, timeout)` | DNS lookup + dial, tries every address, honours the timeout (non-blocking `connect()` + `poll()` + `SO_ERROR`) |
| `sendAll(data, len)` | loops until the OS accepted *all* bytes |
| `recvExact(data, len)` | loops until exactly `len` bytes arrive (handles the TCP stream) |
| `recvSome(data, len)` | one receive attempt: >0 bytes, 0 = close, -1 = error/timeout |
| `close()` / destructor | closes the descriptor; RAII-like cleanup |

Ownership rules: a socket owns an OS resource, so copies are forbidden (two
objects closing one socket = disaster). Moves are allowed — the object hands the
resource to a new owner. (RAII = "Resource Acquisition Is Initialization": the
object's constructor acquires a resource and its destructor guarantees it
releases it, so leaks are impossible by construction.)

**Why `connect()` is non-blocking.** `SO_RCVTIMEO` / `SO_SNDTIMEO` look like
they should bound a connect, and it is a natural mistake to assume they do. On
Linux they apply to `send()` / `recv()` **only** — never to `connect()`. A
plain blocking `connect()` to a black-holed peer therefore sits in the kernel
for the whole SYN-retry window (~130s at the default `tcp_syn_retries=6`) no
matter what timeout you asked for, and one bad sweep can take an hour. So:
switch the socket to `O_NONBLOCK`, let `connect()` return `EINPROGRESS`,
`poll()` for `POLLOUT` against our own deadline, then read `SO_ERROR` to learn
the real verdict (writable ≠ succeeded), and finally restore blocking mode for
the `send`/`recv` calls that follow.

### `Bitfield` — what a peer has (`src/peer/`)

One bit per piece, packed **MSB-first** (piece 0 is bit 7 of byte 0 — the
opposite of the obvious reading, and a quiet way to request pieces nobody has).

`parse()` enforces the two rules the spec states: the blob must be exactly
`(pieces + 7) / 8` bytes, and the spare high bits of the last byte must be zero.
It is all-or-nothing, so a reused `Bitfield` cannot end up half-applied.

**A peer that sends no bitfield owns nothing.** That is the correct default and
it is deliberate: defaulting to "all ones" looks like a hung download rather
than like a bug.

### `PeerSession` — one connection, many pieces (`src/peer/`)

| Method | What it does |
|---|---|
| `connect(peer, options)` | TCP → handshake → read BITFIELD → INTERESTED → wait for UNCHOKE. Throws if any step fails |
| `usable()` | connected **and** not currently choked |
| `available()` | the peer's bitfield — what it says it has |
| `fetchPiece(index, hash, out, err)` | pipelines that piece's 16 KiB blocks, reassembles, verifies the SHA-1 before returning it |
| `abandon()` | CANCEL the blocks in flight and stop reading |
| `piecesDelivered()` / `blocksReceived()` | diagnostics for the UI and the tests |

Keeps up to 8 block requests in flight, matching each reply by `(index, begin)`
because a peer may answer out of order. A CHOKE abandons the piece but keeps the
connection. Keep-alives go out every 100 seconds.

### `PathSafety` — untrusted filenames (`src/util/`)

`isSafePathComponent()` and `makeSafeOutputPath()`. A `.torrent`'s `name` is
chosen by its publisher and is the one field that becomes dangerous. This
**rejects rather than cleans**: separators, `..`, `.`, drive letters, `~`,
control characters, trailing dots/spaces, and Windows device names.

### Exceptions — our three error languages

| Class | Thrown for | Derives from |
|---|---|---|
| `BencodeException` | malformed bencode, bad torrent structure, a multi-file torrent | `std::runtime_error` |
| `NetException` | DNS, connect, send/recv, TLS, UDP tracker failures | `std::runtime_error` |
| `FileException` | the disk layer failed (open/truncate/write) | `std::runtime_error` |

All are caught by `catch (const std::exception&)` — like catching a common
`Exception` supertype in Java.

### "Never trust the network" — the rulebook

1. **Check lengths.** A peer blob that is not a multiple of 6, a string longer
   than the bytes available, a handshake under 68 bytes, a bitfield of the
   wrong size — all rejected.
2. **Check hashes.** The info hash must match (Phase 4). Every piece must
   match (Phases 5, 6 and 8) — and Phase 8 checks twice: once in the session,
   once in the manager, so a bug in either is caught by the other.
3. **Check structure.** Bencode dictionaries must be dictionaries, integers
   must be integers; a `failure reason` supersedes everything.
4. **Check identity.** A peer's BITFIELD tells you what it has; a missing one
   means nothing, never everything.
5. **Enforceable timeouts.** Dead peers must cost seconds, not minutes — and
   the transport has to know about the deadline, or it is not a deadline.
6. **Reject untrusted paths** rather than sanitising them.
7. **Graceful failure.** Everything that can fail returns a verdict with a
   reason rather than crashing the client.

## The complete file map

```
Torrent_Client/
├── CMakeLists.txt          one static library, two executables
├── README.md               the front door
├── docs/                   this guide
├── include/                headers - "the plan"
│   ├── bencode/            Phase 1
│   ├── torrent/            Phase 2
│   ├── tracker/            Phases 3 + 8 (HTTP, UDP, the pool)
│   ├── net/                shared (Phases 3-8)
│   ├── peer/               Phases 4-8 (handshake, messages, sessions, concurrency)
│   ├── piece/              Phases 6-8 (ownership table, claims, resume)
│   ├── ui/                 Phase 8 (the live display)
│   ├── util/               Phase 8 (path safety)
│   └── tests/              the suite's entry point
├── src/                    implementations - "the real code"
│   ├── main.cpp            Phase 8: the command-line client
│   ├── tests/              the test suite
│   └── ...one directory per phase, as above
└── test/                   the .torrent used by the live-network checks
```

| File | Purpose (plain English) |
|---|---|
| **Build & entry points** | |
| `CMakeLists.txt` | `peerflow_core` (static lib) + `peerflow` + `peerflow-tests` |
| `src/main.cpp` | the CLI: parse flags, parse torrent, announce, download, display |
| `src/tests/TestMain.cpp` | the suite's three-line `main()`; returns non-zero on failure |
| `src/tests/TestRunner.cpp` | every check, grouped into sections |
| **Phase 1 — bencode** | |
| `bencode/BencodeValue.hpp` | the tagged-union value (label + one of four shapes) |
| `bencode/BencodeDecoder.hpp` | the parser: parse/dispatch methods, static `decode()` |
| `bencode/BencodeException.hpp` | "your bencode is invalid" |
| `src/bencode/BencodeDecoder.cpp` | integer/string/list/dict parsing + trailing-data check |
| **Phase 2 — torrent parser** | |
| `torrent/TorrentFile.hpp` | the data-holder: announce, `announceTiers`, name, pieceLength, length, pieces, infoHash |
| `torrent/TorrentParser.hpp` | `parse(path)` and `parseString(bencoded)` — split so parsing is testable without a file |
| `src/torrent/TorrentParser.cpp` | reads file, decodes, extracts fields, **rejects multi-file**, computes the info hash on RAW bytes |
| **Phase 3 — HTTP tracker** | |
| `tracker/Peer.hpp` | one peer: `ip` + `port` + `toString()` |
| `tracker/TrackerRequest.hpp` | announce parameters + `buildAnnounceUrl()` + `urlEncode()` + `generatePeerId()` |
| `tracker/TrackerResponse.hpp` | the parsed reply (interval, seeders/leechers, peers, failure reason) |
| `tracker/HttpTracker.hpp` | the public `announce()` + `decodeCompactPeers()` |
| `src/tracker/HttpTracker.cpp` | URL parse, HTTP GET, redirects, chunked decode, TLS upgrade, response→bencode→peers |
| **Phase 4 — handshake** | |
| `peer/PeerHandshake.hpp` | the handshake interface + `Result` struct |
| `src/peer/PeerHandshake.cpp` | build the 68-byte handshake, send, read exactly 68, verify the info hash |
| `peer/FakePeer.hpp` / `.cpp` | loopback server playing the peer side; can misbehave on demand |
| **Phase 5 — messages** | |
| `peer/PeerMessage.hpp` | the length-prefixed wire format, the message ids, `sendMessage`/`readMessage`, the REQUEST/PIECE codecs |
| `src/peer/PeerMessage.cpp` | framing helpers, big-endian payload build/parse, the size cap |
| `peer/PieceDownloader.hpp` / `.cpp` | download **one** piece over **one** connection — the simple path the tests still use |
| **Phase 6 — piece manager + disk** | |
| `piece/PieceManager.hpp` / `.cpp` | preallocate the file, the ownership table, `storePiece()` (SHA-1 gate + offset write), `claimPiece()` with tokens, `reclaimStuckPiece()`, `heartbeatClaim()`, `pieceState()`, `scanDisk()` resume |
| `piece/FileException.hpp` | "the disk layer failed" |
| **Phase 7 + 8 — concurrency and sessions** | |
| `peer/ConcurrentDownloader.hpp` / `.cpp` | N workers over a shared `PieceManager`: a peer queue, session reuse, the stuck-claim watchdog, the single-threaded progress reporter, resume |
| `peer/Bitfield.hpp` / `.cpp` | which pieces a peer has, with a validated wire format |
| `peer/PeerSession.hpp` / `.cpp` | one long-lived connection: handshake, bitfield, choke, keep-alives, pipelined `fetchPiece()` |
| **Phase 8 — tracker pool, UDP, UI, safety** | |
| `tracker/UdpTracker.hpp` / `.cpp` | BEP 15: big-endian helpers, connect + announce, `action == 3`, a budget it will not overrun |
| `tracker/TrackerPool.hpp` / `.cpp` | scheme dispatch, the BEP 12 tier walk, shuffling, peer de-duplication, the last-resort tier |
| `ui/TerminalUI.hpp` / `.cpp` | the live display, the plain-line fallback, the piece map, speed smoothing |
| `util/PathSafety.hpp` / `.cpp` | refuses torrent-supplied paths that could escape a directory |

## How the tests are organized

`peerflow-tests` runs sections in this order:

```
=== Bencode Decoder Tests ===   integers, strings, lists, dicts, nesting,
                                rejection of garbage and trailing data
=== Torrent Parser Tests ===    parse the Ubuntu torrent; multi-file torrents
                                are REFUSED with a clear reason
=== Tracker Tests ===           the compact-peer byte-order regression; BEP 15
                                golden-byte tests; URL parsing; then a real
                                HTTPS announce
=== Peer Handshake Tests ===    up to 80 real peers (10s each), then a
                                deterministic loopback handshake
=== Peer Message + Piece Download Tests ===
                                payload codec round-trip; 3 pieces from a
                                loopback seeder (2 full + the shorter last);
                                corrupted data rejected by SHA-1
=== Piece Manager + Disk Tests ===  full download byte-compared; resume after
                                3/8 pieces; a corrupted byte on disk detected
                                and refetched; storePiece rejects garbage
=== Parallel Download Tests === 4 workers + 4 seeders: all 12 pieces over
                                FEWER THAN 12 connections (the reuse
                                regression test), byte-verified; timing test
=== Results ===                 Passed: 38 / Failed: 0
```

Two of these deserve their place in particular, because each pins a bug that
once shipped:

- **The compact-peer byte-order check.** An entire release of this project
  dialled every peer at a reversed address, and the tracker test passed anyway
  because it only counted them.
- **"12 pieces over fewer than 12 connections."** The old test asserted 12
  pieces == 12 connections, pinning the connect-per-piece behaviour that
  `PeerSession` replaced.

## How to run everything

```bash
cmake -B build          # configure
cmake --build build     # compile

./build/peerflow --help          # the client
./build/peerflow-tests           # the suite
./build/peerflow file.torrent -o ~/Downloads   # download something
```

A completely clean rebuild, if things ever feel stale:

```bash
rm -rf build && cmake -B build && cmake --build build && ./build/peerflow-tests
```

## Current status

```
Phase 0  Setup & tools         ✅ done
Phase 1  Bencode decoder       ✅ done
Phase 2  Torrent parser        ✅ done  (info hash verified; multi-file refused clearly)
Phase 3  HTTP tracker          ✅ done  (DNS, TLS, redirects, chunked, compact peers)
Phase 4  Peer handshake        ✅ done  (real peers + a deterministic loopback proof)
Phase 5  Messages + pieces     ✅ done  (framing, REQUEST/PIECE, SHA-1 gate)
Phase 6  Piece manager + disk  ✅ done  (verify-before-write, preallocation, scanDisk resume)
Phase 7  Concurrency           ✅ done  (worker pool, claim/release, ~4x speedup)
Phase 8  A real client         ✅ done  (CLI, UDP trackers, announce-list tiers,
                                         path safety, bitfields, connection
                                         reuse, pipelining, live display,
                                         resume) — 38 tests
Phase 9  Seeding               ⬜ listening + serving pieces to other clients
Phase 10 Extras                ⬜ DHT, uTP, magnet links, multi-file torrents
```

## The road ahead

**Phase 9 — seeding.** Flip the client into a server: listen on a port, accept
incoming handshakes, and serve the pieces we already own. `FakePeer` has
previewed the socket/bind/listen/accept core since Phase 4, and the comment in
its header still says so. Tit-for-tat is the last of the swarm maths.

**Phase 10 — compatibility.** In rough order of how much each unlocks:

1. **Multi-file torrents.** The single biggest gap. The parser already detects
   and refuses them; the work is mapping a global piece offset onto
   `(file, offsetInFile)` segments and writing across the boundaries. Every
   path component still has to go through `PathSafety` — and a multi-file
   torrent's root folder name is attacker-controlled too.
2. **DHT.** Without it, a torrent whose trackers are all blocked finds nobody
   even though peers exist. That is the Ubuntu ISO's exact situation.
3. **uTP** — BitTorrent's UDP peer transport, invisible to a TCP-only client.
4. **Magnet links** — needs `metadata` exchange over the peer protocol.

Deliberately last, because they are cosmetic: **BEP 47 padding** (treating
`.pad/N` as an ordinary file still writes byte-correct output, just with junk
files alongside) and **block-level parallelism** (one piece per worker is
already complete for the 16 KiB pieces our tests use; real 256 KiB pieces would
benefit from splitting a single piece across several peers).

---

*[Back to the index](README.md)*
