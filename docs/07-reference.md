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

## The complete file map (Phases 0–4)

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
=== Results ===                     Passed: N / Failed: M
```

## How to run everything

```bash
cmake -B build          # configure
cmake --build build     # compile
./build/peerflow        # run all 20 tests
```

A completely clean rebuild (if things ever feel stale):

```bash
rm -rf build && cmake -B build && cmake --build build && ./build/peerflow
```

## Current status (Phases 0–4)

```
Phase 0  Setup & tools         ✅ done
Phase 1  Bencode decoder       ✅ done  (16 tests)
Phase 2  Torrent parser        ✅ done  (info hash verified)
Phase 3  Tracker announce      ✅ done  (Peers found: 50)
Phase 4  Peer handshake        ✅ done  (loopback proof; real peers blocked by firewall here)
Phase 5  Messages + pieces     ⬜ next: request blocks, receive a piece, SHA-1 verify it
Phase 6  Piece manager + disk  ⬜ write files, resume
Phase 7  Many peers            ⬜ concurrency
Phase 8  Upload / seeding      ⬜ listening + tit-for-tat
Phase 9  Extras                ⬜ magnet links, UDP trackers, DHT
```

## The road ahead (Phase 5 in one breath)

Next we speak the **peer wire messages (the small length-prefixed messages
exchanged between peers after the handshake)**:

```
[4-byte length][1-byte ID][payload]
```

with IDs like `unchoke`, `interested`, `request`, `piece`, `bitfield`. We'll
ask a peer for 16 KB blocks of a piece, assemble them, SHA-1 them against
Phase 2's `pieces` hashes, and finally download our first verified piece.

---

*[Back to the index](README.md)*