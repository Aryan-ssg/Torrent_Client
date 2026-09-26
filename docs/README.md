# PeerFlow — The Complete Guide

This folder is the **plain-English, deep-dive manual** for everything we have
built in the PeerFlow BitTorrent client so far (Phases 0–4).

It is written for everyone — including people with **no programming or
networking background**. Fancy terms are always explained in plain words the
first time they appear, usually like this:

> **SHA-1 (a checksum: a fixed-size "fingerprint" of data that changes completely even if one byte changes)**

If you are a Java developer, you will also find "in Java this is…" parallels
throughout, because the source code itself was written with that mindset.

## How to read this guide

Read it top-to-bottom once for the story. Then jump back to any chapter as a
reference while browsing the code.

| Chapter | What it explains | You will understand |
|---|---|---|
| [01 – Big Picture](01-big-picture.md) | What a torrent client even does, using a daily-life analogy | seeds, leechers, swarms, trackers, peers, the whole pipeline |
| [02 – Phase 0: Setup](02-phase0-setup.md) | The tools (CMake, a compiler, C++) and how to build/run the project | `cmake --build build` and `./build/peerflow` |
| [03 – Phase 1: Bencode decoder](03-phase1-bencode.md) | The tiny binary language `.torrent` files are written in | integers, strings, lists, dicts, recursion, "tagged union" |
| [04 – Phase 2: Torrent parser](04-phase2-parser.md) | Reading a `.torrent` file and computing its info hash | announce URL, pieces, info hash, why we must not re-encode |
| [05 – Phase 3: Tracker](05-phase3-tracker.md) | Asking a matchmaker for peers over the internet | HTTP, TCP, DNS, TLS/HTTPS, compact peers, endianness |
| [06 – Phase 4: Peer handshake](06-phase4-handshake.md) | The first 68 bytes two clients exchange | the handshake layout, TCP streams, timeouts, testing on your own computer |
| [07 – Reference](07-reference.md) | Everything reusable + the full file map + the test suite | our `TcpSocket`, exceptions, "never trust the network", all 20 tests |

## Where the actual code lives

```
Torrent_Client/
├── CMakeLists.txt            # build configuration (like pom.xml in Maven)
├── README.md                 # short landing page (this project's front door)
├── docs/                     # ← you are here
├── include/                  # .hpp files = "the plan" (declarations)
│   ├── bencode/              # Phase 1
│   ├── torrent/              # Phase 2
│   ├── tracker/              # Phase 3
│   ├── net/                  # shared network helper (Phases 3 & 4)
│   └── peer/                 # Phase 4
├── src/                      # .cpp files = "the implementation" (the code itself)
│   ├── bencode/
│   ├── torrent/
│   ├── tracker/
│   ├── net/
│   └── peer/
└── test/                     # the .torrent file we use for testing
```

**Tip:** the source files are also heavily commented with the same friendly
tone. The docs here explain the *why*; the code comments explain the *how, line
by line*.

## The golden rule of this project

> **Never trust the network. Check lengths, check hashes, handle disconnects —
> a bad peer must never be able to crash your client.**

You will see this rule come up again and again in every chapter.