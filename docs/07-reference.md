# 07 — Reference: Reusable Blocks, the File Map, and Testing

This final chapter is a shelf of everything reusable plus the "where is
everything" map. Use it as a cheat sheet.

## Reusable building blocks (used across phases)

### `TcpSocket` — our one network primitive (`src/net/`)

Every phase that talks to the internet uses this class:

| Method | What it guarantees |
|---|---|
| `connect(host, port, timeout)` | DNS lookup + dial, tries every address, never hangs (timeout) |
| `sendAll(data, len)` | loops until the OS accepted *all* bytes |
| `recvExact(data, len)` | loops until exactly `len` bytes arrive (handles the TCP stream) |
| `recvSome(data, len)` | one receive attempt: >0 bytes, 0 = close, -1 = error/timeout |
| `close()` / destructor | closes the descriptor; RAII-like cleanup |

Ownership rules: a socket owns an OS resource, so copies are forbidden (two
objects closing one socket = disaster). Moves are allowed — the object hands the
resource to a new owner. (RAII = "Resource Acquisition Is Initialization": the
object's constructor acquires a resource and its destructor guarantees it
releases it, so leaks are impossible by construction.)

### Exceptions — our two error languages

| Class | Thrown for | Derives from |
|---|---|---|
| `BencodeException` | malformed bencode, bad torrent structure | `std::runtime_error` |
| `NetException` | DNS, connect, send/recv, TLS failures | `std::runtime_error` |

Both are caught by `catch (const std::exception&)` — like catching a common
`Exception` supertype in Java.

### "Never trust the network" — the rulebook

Every phase applies these in code:

1. **Check lengths.** Any blob whose size isn't "right" (peers not %6, strings
   longer than available bytes, handshakes shorter than 68) is rejected.
2. **Check hashes.** Info hash must match (Phase 4). Piece hashes must match
   (Phase 5, next). If it fails → the peer is bad, not us.
3. **Check structure.** Bencode dictionaries must be dictionaries, integers
   must be integers; a `failure reason` supersedes everything.
4. **Timeouts.** Dead peers must cost seconds, not minutes.
5. **Graceful failure.** `perform()` variants return a result with an error
   string rather than crashing the whole client.

## The complete file map (Phases 0–5)

| File | Purpose (plain-English) |
|---|---|
| **Build & project** | |
| `CMakeLists.txt` | build config: what to compile, what libraries to link (like `pom.xml`) |
| `README.md` | the short front-door description |
| **Tests** | |
| `src/main.cpp` | the test runner: 20 checks that print PASS/FAIL |
| **Phase 1 — bencode** | |
| `include/bencode/BencodeValue.hpp` | the tagged-union value (label + one of four shapes) |
| `include/bencode/BencodeDecoder.hpp` | the parser: parse/dispatch methods, static `decode()` |
| `include/bencode/BencodeException.hpp` | the "your bencode is invalid" exception |
| `src/bencode/BencodeDecoder.cpp` | integer/string/list/dict parsing + trailing-data check |
| **Phase 2 — torrent parser** | |
| `include/torrent/TorrentFile.hpp` | the data-holder: announce, name, pieceLength, length, pieces, infoHash |
| `include/torrent/TorrentParser.hpp` | the parser interface |
| `src/torrent/TorrentParser.cpp` | reads file, decodes, extracts fields, computes info hash on RAW bytes |
| **Phase 3 — tracker** | |
| `include/tracker/Peer.hpp` | one peer: `ip` + `port` + `toString()` |
| `include/tracker/TrackerRequest.hpp` | announce parameters + `buildAnnounceUrl()` + `urlEncode()` + `generatePeerId()` |
| `src/tracker/TrackerRequest.cpp` | building the %-encoded announce URL and the peer_id |
| `include/tracker/TrackerResponse.hpp` | the parsed reply (interval, seeders/leechers, peers, failure reason) |
| `include/tracker/HttpTracker.hpp` | the public `announce()` + `decodeCompactPeers()` |
| `src/tracker/HttpTracker.cpp` | URL parse, HTTP GET, redirects, chunked decode, TLS upgrade, response→bencode→peers |
| **Phase 4 — handshake** | |
| `include/peer/PeerHandshake.hpp` | the handshake interface + `Result` struct |
| `src/peer/PeerHandshake.cpp` | build 68-byte handshake, send, read exactly 68, verify info_hash |
| `include/peer/FakePeer.hpp` / `src/peer/FakePeer.cpp` | loopback test server playing the peer side (preview of Phase 8 server socket) |
| **Phase 5 — messages + piece download** | |
| `include/peer/PeerMessage.hpp` | the length-prefixed wire format + the message ids + `sendMessage`/`readMessage` |
| `src/peer/PeerMessage.cpp` | framing helpers, big-endian request/piece payload build/parse |
| `include/peer/PieceDownloader.hpp` / `src/peer/PieceDownloader.cpp` | download ONE piece: handshake → interested → unchoke → request blocks → SHA-1 verify |
| **Phase 6 — piece manager + disk** | |
| `include/piece/PieceManager.hpp` / `src/piece/PieceManager.cpp` | preallocate the file, ownership bitmap, `storePiece()` (SHA-1 gate + offset write), `nextPieceToFetch()`, `scanDisk()` resume |
| `include/piece/FileException.hpp` | the "disk layer failed" exception (open/truncate/write errors) |

## How the tests are organized

`main.cpp` runs sections in this order:

```
=== Bencode Decoder Tests ===      (~16 tests: ints, strings, lists, dicts,
                                    nesting, rejection of garbage/trailing data)
=== Torrent Parser Tests ===       (parse the Ubuntu torrent, print facts,
                                    verify 6 sanity checks, show the info hash)
=== Tracker Tests ===              (real announce to tracker.opentrackr.org,
                                    decode the compact 6-byte peers)
=== Peer Handshake Tests ===       (attempt up to 25 real peers — graceful
                                    failures on this restricted network)
--- Deterministic loopback test ---(handshake against FakePeer on 127.0.0.1)
=== Peer Message + Piece Download Tests ===
                                    (request payload codec round-trip;
                                    download 3 pieces from a loopback seeder:
                                    2 full 16 KiB + the shorter final piece;
                                    corrupted data rejected by SHA-1)
=== Piece Manager + Disk Tests ===  (download an 8-piece synthetic file over
                                    one connection per piece; byte-compare the
                                    result; resume via scanDisk() after 3/8;
                                    one corrupted byte on disk detected and
                                    refetched; storePiece rejects garbage)
=== Results ===                     Passed: N / Failed: M
```

## How to run everything

```bash
cmake -B build          # configure
cmake --build build     # compile
./build/peerflow        # run all 32 tests
```

A completely clean rebuild (if things ever feel stale):

```bash
rm -rf build && cmake -B build && cmake --build build && ./build/peerflow
```

## Current status (Phases 0–6)

```
Phase 0  Setup & tools         ✅ done
Phase 1  Bencode decoder       ✅ done  (16 tests)
Phase 2  Torrent parser        ✅ done  (info hash verified)
Phase 3  Tracker announce      ✅ done  (Peers found: 50)
Phase 4  Peer handshake        ✅ done  (loopback proof; real peers blocked by firewall here)
Phase 5  Messages + pieces     ✅ done  (loopback seeder: 3 pieces downloaded & SHA-1 verified,
                                        corrupted piece rejected)
Phase 6  Piece manager + disk  ✅ done  (8-piece synthetic file downloaded to disk and
                                        byte-verified; resume + disk-corruption recovery;
                                        storePiece gate; hardened parser (overflow +
                                        nesting-depth caps); 32 tests total)
Phase 7  Many peers            ⬜ concurrency
Phase 8  Upload / seeding      ⬜ listening + tit-for-tat
Phase 9  Extras                ⬜ magnet links, UDP trackers, DHT
```

## The road ahead (Phase 7 in one breath)

Phase 6 downloads pieces **one at a time**: the manager asks for the next
missing piece, waits for it, writes it, and repeats — one network round trip
per block. Phase 7 makes it fast: many TCP connections to many peers at once,
each pulling different blocks of the same piece, coordinated through the
manager. The last big missing piece before Phase 7 is a **real server side**
(Phase 8: listening + seeding), so for now the loopback `FakePeer` seeder
keeps playing that part.

---

*[Back to the index](README.md)*