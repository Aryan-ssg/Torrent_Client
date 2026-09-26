# PeerFlow — a BitTorrent Client in C++

PeerFlow is a BitTorrent client built **from scratch in C++17**, one layer at a
time, with every line written and explained — no magic, no hidden frameworks.

Current milestone: **20/20 tests passing** through Phase 4 (peer handshake).

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
| 5 | Messages + downloading a verified piece | ⬜ next |
| 6–9 | Piece manager + disk, concurrency, seeding, extras | ⬜ |

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
./build/peerflow          # runs all 20 tests, prints PASS/FAIL + results
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
│   └── peer/                #   Phase 4  handshake (+ FakePeer test server)
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
- 🧱 `TcpSocket` reusable network primitive shared by all phases

## Known environment notes

- `torrent.ubuntu.com` restricts by IP range and rejects this machine.
- This network's firewall allows only a few outbound ports (80/443), so
  arbitrary peer ports (6881, 51413, …) are unreachable — which is why Phase 4
  tests run against a local fake peer on `127.0.0.1`.

## Roadmap

Phase 5 — peer wire messages: request 16 KB blocks, assemble a piece, and
verify its SHA-1 against the torrent's piece hashes.
Then: piece manager + disk → concurrency → seeding → extras (magnet, UDP
trackers, DHT).

## Learning resources

- [Big-picture guide (docs)](docs/01-big-picture.md)
- [BitTorrent specification](https://wiki.theory.org/BitTorrentSpecification)