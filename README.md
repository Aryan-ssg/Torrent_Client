# PeerFlow — a BitTorrent Client in C++

PeerFlow is a BitTorrent client built **from scratch in C++17**, one layer at a
time, with every line written and explained — no magic, no hidden frameworks.

Current milestone: **34/34 tests passing** through Phase 7 (parallel
downloads across many peers; hardened parser against overflow + runaway
nesting).

> 🍕 The whole project in one breath: read the `.torrent` recipe card, ask a
> matchmaker (tracker) who else is cooking, shake hands with neighbours
> (peers), swap verified slices (pieces), and finally save + share the file.

## Status

| Phase | What you build | Status |
|---|---|---|
| 0 | Setup and tools (CMake, C++17, OpenSSL) | ✅ |
| 1 | Bencode decoder | ✅ |
| 2 | Torrent parser + info hash | ✅ |
| 3 | Tracker announce, get peers | ✅ — real HTTPS tracker: `Peers found: 50` |
| 4 | Peer handshake | ✅ — verified on loopback; this network blocks peer ports |
| 5 | Messages + downloading a verified piece | ✅ — loopback seeder: request 16 KiB blocks, assemble, SHA-1 verify |
| 6 | Piece manager + disk + resume | ✅ — fetch all pieces, verify + write at correct offset, `scanDisk()` resume |
| 7 | Concurrency (many peers) | ✅ — N worker threads share a mutex-guarded PieceManager; claim states stop duplicate downloads; work spread across peers per piece; 8 pieces in ~250 ms vs ~960 ms sequential |
| 8–9 | Seeding, extras | ⬜ |

## Start here

- **[The Complete Guide → `docs/`](docs/README.md)** — a friendly,
  phase-by-phase manual written for everyone, with all the theory
  (networking, bencode, hashing, endianness) explained in plain words.
- The source files are heavily commented with the same tone: one mental model,
  `why` in the docs, `how` in the code.

## Quick start

```bash
cmake -B build
cmake --build build
./build/peerflow          # runs all 34 tests, prints PASS/FAIL + results
```

Requires: a C++17 compiler, CMake ≥ 3.14, OpenSSL. (Similar to `mvn package`
then `java -jar` if you come from Java.)

## Project layout

```
Torrent_Client/
├── CMakeLists.txt           # build config (the "pom.xml")
├── docs/                    # ← the full guide (start here)
├── include/                 # headers (the "plans")
│   ├── bencode/             #   Phase 1  decoder
│   ├── torrent/             #   Phase 2  .torrent parser
│   ├── tracker/             #   Phase 3  announce + peer list
│   ├── net/                 #   shared  TcpSocket + exceptions
│   ├── peer/                #   Phases 4–7 handshake + messages + FakePeer + concurrency
│   └── piece/               #   Phase 6  PieceManager (own + write pieces)
├── src/                     # implementations (the "real code")
└── test/                    # the Ubuntu 24.04 .torrent used for testing
```

## Features so far

- ✅ Bencode decoding (integers, strings, lists, dicts, nesting) with strict
  validation — 16 tests
- ✅ `.torrent` parser extracting announce, name, piece length, size, piece
  hashes — and the **info hash** computed from the exact raw bytes
- ✅ Real HTTPS tracker announce (DNS, TCP, TLS, HTTP, redirects, compact
  peers) against `tracker.opentrackr.org`
- ✅ 68-byte peer handshake with `recvExact` streaming reads, timeouts, and
  info-hash verification — proven against a loopback `FakePeer`
- ✅ Peer wire messages (length-prefixed) + a full **piece download**: request
  16 KiB blocks, assemble them, SHA-1 the result against the torrent's piece
hashes — verified on a loopback seeder, including the shorter final piece
   and the rejection of a deliberately corrupted piece
- ✅ `PieceManager`: download **every** piece (one TCP connection each, like a
  real client), verify SHA-1 at the last gate, write at the exact file offset;
  **resume** re-verifies the file on disk (`scanDisk()`) and only refetches
  the pieces that failed the hash — including detecting one deliberately
  corrupted byte on disk
- ✅ **Parallel downloads** (`ConcurrentDownloader`): N worker threads share
  the mutex-guarded PieceManager; each claims a piece so no two workers ever
  fetch the same one, and a failure releases it for retry. Proven end-to-end:
  12 pieces fetched over four loopback seeders with exactly 12 connections,
  plus a timing test showing a real ~4× speedup
- 🧱 `TcpSocket` reusable network primitive shared by all phases
- ✅ Parser hardening: 64-bit overflow guards on every digit run (integers and
  string lengths), a 200-level nesting cap that turns adversarial `llll...` input
  into a clean error instead of a stack crash, and strict BEP-3 zero rules
  (rejects `i03e`, `i-0e`)

## Known environment notes

- `torrent.ubuntu.com` restricts by IP range and rejects this machine.
- This network's firewall allows only a few outbound ports (80/443), so
  arbitrary peer ports (6881, 51413, …) are unreachable — which is why Phases
  4–7 tests run against local fake peer/seeders on `127.0.0.1`.

## Roadmap

Phase 8 — the upload side: listen for connections and seed pieces to other
clients (our FakePeer has been previewing the server socket since Phase 4).
Then: extras (magnet, UDP trackers, DHT).

Note for later phases: with 16 KiB synthetic pieces (one block each),
piece-per-worker is the complete parallel picture; block-level parallelism
(fetching different parts of ONE big piece from different peers) becomes
relevant for real 256 KiB pieces like Ubuntu's.

## Learning resources

- [Big-picture guide (docs)](docs/01-big-picture.md)
- [BitTorrent specification](https://wiki.theory.org/BitTorrentSpecification)