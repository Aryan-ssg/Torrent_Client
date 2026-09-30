# Tutor Track — Learning PeerFlow End to End

> **This folder is different from `docs/` next door.**
>
> `docs/01`–`docs/11` are the project's *manual*: what BitTorrent is, what each
> phase does, and why. They are excellent and you should read them.
>
> This folder is a **teaching track**. It fills the gaps the manual does not
> cover — the C++ language itself, the build, the concurrency reasoning, and
> the tests as a design tool — and it is written for a **strong Java developer
> who is new to C++ and to networking**. It goes **code-first**: read real
> source, understand it, then write something.
>
> The two work together. Manual for the *what*, tutor track for the *how* and
> the *why not*.

## How a session runs

1. **Read** — a specific set of files and line ranges, named in advance.
2. **Teach** — I explain the code, in plain words, with Java parallels.
3. **Contrast** — for every rule, the **without / with** pair: what actually
   breaks if we skip it, and what breaks if we do it wrong. Where possible I
   hand you a **runnable demo** in `demos/` that breaks on purpose.
4. **Quiz** — you answer. You don't move on until you do.
5. **Write** — a small, real change to the codebase, plus its test.
6. **Notes** — appended to the module's file in this folder.

The notes are the artifact. After the track, they are a second manual covering
everything the first one deliberately skipped.

### Why every rule comes with a contrast

A rule on its own is just advice, and advice gets forgotten. A rule paired
with a *concrete failure* is memorable — and in a codebase like this one the
failures are real. This project shipped six of them; two are reproduced
verbatim in `demos/`, running on your machine, offline.

So the standing format of every explanation in this track is:

```
WITHOUT  ->  what actually happens (a crash, silent corruption, a hang)
WITH     ->  what the code does instead, and what that costs us
THE RULE ->  the one line worth remembering
```

The third part matters most. Nearly every rule in this codebase exists to
spend a little verbosity to eliminate an entire *category* of bug. That trade
is the house style, and once you see it you can predict most of the code
without reading it.

## The demos

Runnable programs that demonstrate a contrast by actually breaking. No network
access, no torrent file, no setup — just a compiler.

```bash
g++ -O0 -std=c++17          -o /tmp/demo01 docs/12-tutor/demos/demo-01-byte-order.cpp && /tmp/demo01
g++ -O0 -std=c++17 -pthread -o /tmp/demo02 docs/12-tutor/demos/demo-02-recvExact.cpp && /tmp/demo02
```

| Demo | The contrast it proves | Origin |
|---|---|---|
| `demo-01-byte-order.cpp` | the shipped reversed-IP bug vs the `memcpy` fix | commit `a4e60e4` |
| `demo-02-recvExact.cpp` | one naive `recv()` vs `recvExact()` | **found a live bug — see [00-orientation §9](00-orientation.md#9-a-real-bug-i-found-while-writing-the-demo)** |

Pipe `demo-02` through `stdbuf -o0`, or its output sits in the stdout buffer
and appears to hang.

More arrive as the track progresses. Each one is a bug this codebase either
shipped or could plausibly have shipped.

## The map

| # | Module | The one thing it teaches | Notes |
|---|---|---|---|
| 0 | [Orientation](00-orientation.md) | the build, the targets, the shape of the whole program | ✅ done |
| 1a | [C++ types, const, references](01a-cpp-types.md) | headers vs sources, `const&`, fixed-width ints, tagged unions | ⬜ |
| 1b | [C++ ownership](01b-cpp-ownership.md) | RAII, move, `= delete`, vector invalidation | ⬜ |
| 2 | Bencode | recursion over bytes; parsing untrusted input | ⬜ |
| 3 | Torrent parser | the info hash, and why raw bytes ≠ re-encoding | ⬜ |
| 4 | **Sockets** ⭐ | fds, DNS, TCP-as-stream, timeouts that actually time out | ⬜ |
| 5 | HTTP/HTTPS tracker | HTTP by hand, TLS, and the byte-order bug | ⬜ |
| 6 | UDP trackers | datagrams, BEP 15, tiers, deadlines | ⬜ |
| 7 | Wire protocol | the 68-byte handshake, framing, bitfields | ⬜ |
| 8 | Pieces and disk | verify-before-write, preallocation, resume | ⬜ |
| 9 | **Concurrency** ⭐ | threads, atomics, the claim/watchdog design | ⬜ |
| 10 | Peer sessions | connection reuse, pipelining, choke ≠ failure | ⬜ |
| 11 | CLI, UI, path safety | arg parsing, ANSI, signals, untrusted filenames | ⬜ |
| 12 | Testing | `FakePeer` as a lever; what is *not* tested | ⬜ |
| 13 | [Bug archaeology](13-bug-archaeology.md) | six real bugs, reconstructed from git | ⬜ |
| 14+ | Build Phase 9 & 10 | multi-file, seeding, DHT | ⬜ |

⭐ = the two modules where the real understanding lives. Expect them to take
longer than the rest. Everything else is comparatively quick once you're past
the C++ and networking basics.

## The two questions this track keeps asking

Every design decision in this codebase answers one of two questions. Once you
internalise them, most of the code explains itself:

> **1. Is this value trusted?** Everything in a `.torrent` file — the filename,
> the piece count, the tracker URLs, every byte off a peer's socket — is chosen
> by a stranger. The code's job is to *check lengths, check hashes, check
> lengths again* before believing any of it. `PathSafety` refusing
> `../../.ssh/authorized_keys` instead of cleaning it is the same instinct as
> rejecting a PIECE block that isn't exactly the size we asked for.

> **2. Who owns this resource, and when does it die?** In Java the GC answers
> that. In C++ you must answer it at every allocation. `TcpSocket` deleting its
> copy constructor is not pedantry — two objects closing one file descriptor is
> a real, hard-to-debug failure. This is the single biggest adjustment for a
> Java developer, and it gets a whole module (1b).

## The prerequisite vocabulary

Plain-English definitions, used throughout these notes. The manual in `docs/01`
has the full glossary; these are the ones you need constantly.

| Term | Meaning |
|---|---|
| **file descriptor (fd)** | a small integer the OS hands you when you open a file or socket. `3` is "the next free fd". Closing it releases the resource. |
| **stream vs datagram** | TCP is a stream (ordered, reliable, no message boundaries). UDP is a datagram (unreliable, but each one arrives whole or not at all). |
| **byte order (endianness)** | how a multi-byte number is laid out in memory. The network has one order; most CPUs today have the other. This project converts at every boundary. |
| **checksum / hash** | a fixed-size "fingerprint" of data that changes completely if a single byte changes. SHA-1 gives 20 bytes. |
| **piece** | a fixed-size slice of the file — the unit of verification. |
| **block** | a 16 KiB sub-slice of a piece — the unit of transfer. |
| **peer** | any other machine in the same swarm. |
| **seeder** | a peer that has 100% of the file. |
| **swarm** | all peers sharing one torrent. |
| **announce** | the request you send a tracker to register yourself and ask for peers. |
| **info hash** | SHA-1 of the torrent's `info` dictionary. The torrent's identity. |
| **RAII** | "Resource Acquisition Is Initialization": acquire in the constructor, release in the destructor, so leaks are impossible by construction. |
| **UB (undefined behaviour)** | code the C++ standard places no requirements on. Often "works until it doesn't". This project defends against it deliberately. |

---

*[Back to the main guide](../README.md)*
