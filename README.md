# PeerFlow — a BitTorrent Client in C++

A working BitTorrent client, built **from scratch in C++17**, one layer at a
time, with every line written and explained — no magic, no hidden frameworks.

Current state: **38/38 tests passing**. It downloads single-file torrents from
the real internet, verifies every piece against the torrent's own hashes, and
shows live progress while it does.

> 🍕 The whole project in one breath: read the `.torrent` recipe card, ask a
> matchmaker (tracker) who else is cooking, shake hands with neighbours
> (peers), swap verified slices (pieces), and save the file.

## Quick start

```bash
cmake -B build
cmake --build build
./build/peerflow some-file.torrent -o ~/Downloads
```

Requires a C++17 compiler, CMake ≥ 3.14, and OpenSSL. (Similar to `mvn package`
then `java -jar` if you come from Java.)

Run the test suite with:

```bash
./build/peerflow-tests
```

### Options

| Flag | Default | Meaning |
|---|---|---|
| `-o, --output <dir>` | `.` | where to write the file (created if missing) |
| `-j, --workers <n>` | 8 | parallel download connections |
| `-t, --timeout <secs>` | 10 | per-peer network timeout |
| `-a, --announce <secs>` | 60 | how long to spend finding peers |
| `--no-tui` | off | plain lines instead of the live display |
| `-q, --quiet` | off | errors only |
| `-h`, `--help` / `-v` | | help / version |

Ctrl-C is safe at any point: rerunning the same command resumes, because the
pieces already on disk are re-verified and only the missing ones are fetched.

## What it does today

| Phase | Feature | Status |
|---|---|---|
| 0 | Setup (CMake, C++17, OpenSSL) | ✅ |
| 1 | Bencode decoder — the language `.torrent` files are written in | ✅ |
| 2 | Torrent parser + info hash computed from the exact raw bytes | ✅ |
| 3 | Tracker announce over **HTTP/HTTPS** and **UDP (BEP 15)** | ✅ |
| 4 | 68-byte peer handshake, with info-hash verification | ✅ |
| 5 | The length-prefixed peer wire protocol; download one verified piece | ✅ |
| 6 | Piece ownership table, verify-before-write, preallocation, `scanDisk()` resume | ✅ |
| 7 | Worker pool with atomic claim/release across many peers | ✅ |
| 8 | A real client: CLI, announce-list tiers, path safety, bitfields, connection reuse, pipelining, live display | ✅ |
| 9 | Seeding (serving pieces to other clients) | ⬜ |
| 10 | Multi-file torrents, DHT, uTP, magnet links | ⬜ |

### In more detail

- **Bencode decoding** (integers, strings, lists, dicts, nesting) with strict
  validation — overflow guards on every digit run, a 200-level nesting cap that
  turns adversarial `llll…` input into a clean error instead of a stack crash,
  and strict BEP-3 zero rules.
- **`.torrent` parsing** extracting announce, `announce-list`, name, piece
  length and piece hashes — with the **info hash computed from the exact raw
  bytes** (re-encoding the dictionary would produce a different, wrong hash).
- **Tracker announces over both transports.** HTTP/HTTPS with DNS, TLS,
  redirects, chunked decoding and compact-peer parsing; and UDP via BEP 15,
  which is two datagrams in one round trip. Real trackers, ~50–70 peers each.
- **`announce-list` tiers (BEP 12)**: the trackers in a tier are interchangeable,
  so they are shuffled and tried in parallel, and a dead tier falls through to
  the next. Plus a last-resort tier of public trackers, because a healthy
  torrent can still name a tracker that blocks you — the Ubuntu ISO does.
- **68-byte peer handshakes** with streaming reads, real timeouts, and info-hash
  verification, proven against both real internet peers and a deterministic
  loopback peer.
- **The peer wire protocol**: length-prefixed framing, keep-alives, REQUEST and
  PIECE payloads, and a SHA-1 gate on every piece.
- **`PieceManager`**: verify-before-write (nothing reaches disk unverified),
  the file preallocated to full size, and `scanDisk()` re-verifying what is
  already there so resume is honest about a partially-written file.
- **Parallel downloads** with a shared piece table, atomic claims so no two
  workers fetch the same piece, and stuck-claim reclaim so one unresponsive
  peer cannot strand the tail of a download.
- **Connection reuse and pipelining.** A connection stays open and moves many
  pieces over it, with 8 block requests in flight. The naive design — one
  connection per piece — would mean 23,664 connections for a 6 GB ISO.
- **A live terminal display**: progress bar, speed, ETA, peer count, and a
  per-piece map so you can see *where* the holes are. Degrades to plain lines
  when the output is not a terminal.
- **Untrusted input, treated as such.** Torrent-supplied filenames are
  rejected if they could escape the output directory, rather than sanitised —
  `../../.ssh/authorized_keys` is refused, not cleaned.

## Known limitations

Stated plainly, because a client that overstates itself is worse than one that
does less.

- **Multi-file torrents are not supported.** Most real torrents (series, albums,
  `CD1`/`CD2`, software) are multi-file; they are **refused with a clear
  message** rather than mis-assembled. Single-file torrents — ISOs, and
  self-contained installers — work.
- **Seeding is not implemented.** The client is a leecher. It downloads fine,
  but the swarm gets nothing back, so some peers will ignore it.
- **No DHT.** Peer discovery is tracker-only, so a torrent whose trackers are
  all blocked finds nobody even if peers exist.
- **TCP only.** uTP, BitTorrent's UDP peer transport, is not implemented.
- **No magnet links.**
- **The output file is allocated at full size up front**, so the free space has
  to exist before you start. It is sparse, so it only occupies what you have
  actually downloaded.

## Learn more

- **[The Complete Guide → `docs/`](docs/README.md)** — a friendly, phase-by-phase
  manual written for everyone, with all the theory (networking, bencode,
  hashing, endianness) explained in plain words.
- [Big-picture guide](docs/01-big-picture.md)
- [BitTorrent specification](https://wiki.theory.org/BitTorrentSpecification)
- [BEP 12 — Multi-Tracker Metadata](https://www.bittorrent.org/beps/bep_0012.html)
- [BEP 15 — UDP Tracker Protocol](https://www.bittorrent.org/beps/bep_0015.html)
