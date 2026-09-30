# PeerFlow — The Complete Guide

This folder is the **plain-English, deep-dive manual** for everything built in
the PeerFlow BitTorrent client so far (Phases 0–8).

It is written for everyone — including people with **no programming or
networking background**. Fancy terms are always explained in plain words the
first time they appear, usually like this:

> **SHA-1 (a checksum: a fixed-size "fingerprint" of data that changes completely even if one byte changes)**

If you are a Java developer, you will also find "in Java this is…" parallels
throughout, because the source code itself was written with that mindset.

## Start here

If you just want to **use** the client, you do not need to read any of this:

```bash
cmake -B build && cmake --build build
./build/peerflow some-file.torrent -o ~/Downloads
```

Everything below is for understanding how it works.

## How to read this guide

Read it top-to-bottom once for the story. Then jump back to any chapter as a
reference while browsing the code.

| Chapter | What it explains | You will understand |
|---|---|---|
| [01 – Big Picture](01-big-picture.md) | What a torrent client even does, using a daily-life analogy | seeds, leechers, swarms, trackers, peers, the whole pipeline |
| [02 – Phase 0: Setup](02-phase0-setup.md) | The tools (CMake, a compiler, C++) and how to build/run | the two binaries, and `cmake --build build` |
| [03 – Phase 1: Bencode decoder](03-phase1-bencode.md) | The tiny binary language `.torrent` files are written in | integers, strings, lists, dicts, recursion, "tagged union" |
| [04 – Phase 2: Torrent parser](04-phase2-parser.md) | Reading a `.torrent` file and computing its info hash | announce URL, piece hashes, why we must not re-encode |
| [05 – Phase 3: Tracker](05-phase3-tracker.md) | Asking a matchmaker for peers over HTTP | HTTP, TCP, DNS, TLS/HTTPS, redirects, chunked, compact peers |
| [06 – Phase 4: Peer handshake](06-phase4-handshake.md) | The first 68 bytes two clients exchange | the handshake layout, TCP streams, timeouts, testing on your own computer |
| [07 – Phase 5: Messages](07-phase5-messages.md) | The length-prefixed wire protocol | keep-alives, message ids, REQUEST/PIECE payloads, the SHA-1 gate |
| [08 – Phase 6: Pieces to disk](08-phase6-pieces.md) | Owning every piece, writing a file, resume | the ownership map, `storePiece()`, `scanDisk()`, "never trust the disk either" |
| [09 – Phase 7: Parallel downloads](09-phase7-concurrency.md) | Many workers, many peers, real speed | threads, claim/release, atomic abort, load-balancing peers |
| [10 – Phase 8: From library to client](10-phase8-client.md) | Turning the engine into something you can run | the CLI, UDP trackers, announce-list tiers, connection reuse, the live display, and the bugs found along the way |
| [11 – Reference](11-reference.md) | Everything reusable + the full file map + the test suite | `TcpSocket`, `Bitfield`, `PeerSession`, `PathSafety`, "never trust the network", all 38 tests |

> **Looking for a hands-on teaching track instead?** This manual is the
> *reference*. For a code-first course that teaches the C++ language itself,
> the build, and the concurrency design — written for a Java developer new to
> C++ — see **[12-tutor/](12-tutor/README.md)**. The two are meant to be read
> together: this folder for the *what*, the tutor track for the *how* and the
> *why not*.

## Where the actual code lives

```
Torrent_Client/
├── CMakeLists.txt            # build config: one library, two binaries
├── README.md                 # short landing page (this project's front door)
├── docs/                     # ← you are here
├── include/                  # .hpp files = "the plan" (declarations)
│   ├── bencode/              # Phase 1
│   ├── torrent/              # Phase 2
│   ├── tracker/              # Phases 3 + 8 (HTTP, UDP, the tracker pool)
│   ├── net/                  # shared network helper (Phases 3-8)
│   ├── peer/                 # Phases 4-8 (handshake, messages, sessions, concurrency)
│   ├── piece/                # Phases 6-8 (PieceManager, claims, resume)
│   ├── ui/                   # Phase 8 (the live terminal display)
│   ├── util/                 # Phase 8 (path safety)
│   └── tests/                # the test suite's entry point
├── src/                      # .cpp files = "the implementation" (the code itself)
│   ├── main.cpp              # Phase 8: the command-line client
│   ├── tests/                # the test suite
│   └── ...one directory per phase, as above
└── test/                     # the .torrent file the live-network checks use
```

**Tip:** the source files are also heavily commented with the same friendly
tone. The docs here explain the *why*; the code comments explain the *how,
line by line*.

## The golden rule of this project

> **Never trust the network. Check lengths, check hashes, check lengths again,
> handle disconnects — a bad peer must never be able to crash your client, and
> a bad torrent must never be able to write where it likes.**

You will see this rule come up again and again in every chapter. It is also the
reason several of the worst bugs in this project's history were found: each one
was a place where something from the network was *believed* rather than
*checked*.
