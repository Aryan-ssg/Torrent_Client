# 10 — Phase 8: From Library to Client

Phases 0–7 built a BitTorrent *engine*. It could parse a torrent, talk to
trackers, shake hands with peers, download pieces, verify them and write them
to disk, in parallel, over many connections.

What it could not do was **be used**. There was no command, no display, and —
the part that mattered — it opened one TCP connection per piece. For the 6 GB
Ubuntu ISO that is 23,664 connections, which no real swarm survives.

This phase closed that gap. Nothing here is new protocol theory; it is the
unglamorous work of making the engine into a program you can point at a file.

> **TL;DR**
> ```bash
> ./build/peerflow ubuntu.torrent -o ~/Downloads
> ```
> It finds peers (HTTP *and* UDP trackers), downloads every piece, verifies
> each one against the torrent's own SHA-1 hashes, and shows you a live
> progress display. Ctrl-C is safe; rerunning resumes.

---

## The eight things this phase added

| Piece | File | What it fixed |
|---|---|---|
| The command | `src/main.cpp` | there was no program to run |
| UDP trackers | `tracker/UdpTracker.*` | most torrents list `udp://` first and we refused to speak it |
| Tiered announces | `tracker/TrackerPool.*` | torrents list 5–10 trackers; we tried exactly one |
| Safe filenames | `util/PathSafety.*` | a torrent could name its own output file `../../.ssh/authorized_keys` |
| Piece awareness | `peer/Bitfield.*` | we asked peers for pieces they did not have, and waited forever |
| Connection reuse | `peer/PeerSession.*` | 23,664 connections instead of a couple of dozen |
| Honest progress | `ui/TerminalUI.*` | a two-hour download with no output looks exactly like a hung one |
| Working resume | `piece/PieceManager.*` | the help text promised resume; nothing had ever called it |

---

## 1. The command line

Nothing exotic: parse flags, validate them, do the five things in order, exit
with a meaningful code.

```
> KeePass-2.61.1-Setup.exe.torrent
  name        KeePass-2.61.1-Setup.exe
  size        4.4 MiB
  pieces      141 x 32.0 KiB
  info hash   ea4a54f2234378bac30753b321529a3b610ebf2f

> finding peers (peer id -PF0001-je7iv9d9377x)
  tracker.opentrackr.org:1337  12 peers, 11 seeders, 1 leechers (0.59s)
  2 trackers unavailable, skipped

> downloading to /tmp/demo/KeePass-2.61.1-Setup.exe
[2%] 96.0 KiB / 4.4 MiB  at 50.1 KiB/s  peers 4  eta 0:01:27
...
[100%] 4.4 MiB / 4.4 MiB  done

Download complete.
  total time 1m 15s
```

### Why hand-rolled argument parsing

`getopt()` is POSIX-only and this project builds on Windows too. Twelve lines
here beat a dependency for flags this small.

### Why the ranges are validated

```cpp
const int kMaxWorkers = 256;
if (o.workers < 1 || o.workers > kWorkers) { /* "…must be between 1 and 256" */ }
```

and parsing is strict:

```cpp
long value = std::stol(text, &i);
if (i != text.size()) throw "…has trailing characters";
```

`std::stoi("12abc")` returns `12` without complaining, so `-j 12abc` would
silently mean twelve workers. A typo should be an error message, not a setting
you did not ask for. (This is Java's `Integer.parseInt`, which throws — the
strictness is the default there and something you have to add back in C++.)

### Exit codes

| Code | Meaning |
|---|---|
| `0` | finished, everything verified |
| `1` | failed (no peers, verification gave up, bad torrent) |
| `2` | the command line was wrong |
| `130` | Ctrl-C |

### Ctrl-C

A signal handler must be **async-signal-safe**, which in practice means doing
almost nothing inside one. Touching `std::cout` or allocating there is a data
race waiting to happen. So the handler flips one flag and an ordinary thread
notices:

```cpp
volatile sig_atomic_t g_interrupted = 0;
extern "C" void onInterrupt(int) { g_interrupted = 1; }
```

`volatile sig_atomic_t` is the only kind of variable the standard actually
promises you may touch from a signal handler.

---

## 2. UDP trackers (BEP 15)

This is the single highest-value change in the project. Almost every tracker
URL in a modern `.torrent` starts with `udp://`, and until we could speak it
the client simply could not ask most torrents who else is downloading them.

**UDP trackers are much simpler than HTTP.** An HTTP announce costs a TCP
handshake, a TLS handshake, a request and a chunked response — 5–8 round trips
and kilobytes. BEP 15 is **two datagrams and one round trip, about 200 bytes**,
with no connection to set up and no TLS.

| Packet | Size | Contents |
|---|---|---|
| connect request | 16 B | `0x41727101980` (magic) + `action=0` + `transaction_id` |
| connect response | 16 B | `action=0` + `transaction_id` + `connection_id` |
| announce request | **98 B** | `connection_id`, `action=1`, `transaction_id`, info hash, peer id, downloaded/left/uploaded, event, IP, key, num_want |
| announce response | 20 B + N×6 | `action=1`, `transaction_id`, `interval`, leechers, seeders, then the peers |

### The three details that matter

**Everything is big-endian, including the 64-bit magic.** This is the same
endianness trap that already cost us the byte-reversed peer addresses in
Phase 3, but with a sharper edge: write `0x41727101980` little-endian and the
tracker does not reject you, it does not *answer* — you just wait out a
timeout wondering why. So the helpers are explicit and there are **golden-byte
tests** that check against hex copied from the spec rather than testing that
the arithmetic round-trips (which it would, either way).

**Transaction IDs are random, and a mismatch is discarded unread.** UDP makes
no delivery guarantee, so a reply can be late, duplicated, or somebody else's.
Echoing a random 32-bit id on each request is what lets us tell. The
alternative — parsing whatever arrives — is how a client ends up acting on a
packet that answered a request from twenty minutes ago.

**`action == 3` is an error, not a malformed packet.** It arrives carrying a
bencoded `{"failure reason": …}`, so we parse it and show the tracker's own
explanation instead of a generic "bad reply".

### The 98-byte surprise

Counting the fields in the spec's diagram gives **96** bytes. Real trackers
ignore 96-byte announces entirely. Measured against two trackers on the same
day:

```
96 bytes  ->  no reply at all (times out)
98 bytes  ->  200 OK, 73 peers, 53 seeders, 20 leechers
```

libtorrent — the engine under qBittorrent and Transmission — also puts 98
bytes on the wire. The extra two are trailing padding. **This is recorded in
the source with the measurement**, because the instinct to "correct" 98 back
to 96 is strong and the failure is silent.

*(An earlier version of this project had the same class of bug and cost a day
of confusion: the compact-peer decoder built the right IP *number* with bit
shifts and assigned it to a field that was already in network order, so every
peer address came out reversed. See Phase 3.)*

---

## 3. Tiered announces (BEP 12)

Real torrents list **5–10 trackers, grouped into tiers**, and our code read
exactly one of them.

```json
"announce-list": [ ["udp://a:1337/announce", "udp://b:1337/announce"],
                   ["udp://backup:6969/announce"] ]
```

BEP 12's rule: every tracker *within* a tier is interchangeable, so try them
all before moving to the next tier; tier 1 is tried before tier 2.

`TrackerPool` does three things that matter:

1. **Shuffles within a tier.** Torrents are written with the maintainer's
   favourite tracker first. If every client honours that ordering they all hit
   the same tracker in the same second.
2. **Announces concurrently within a tier.** Sequential attempts against
   several dead trackers is what turns a five-second start into a five-minute
   one.
3. **De-duplicates peers by address.** A peer offered by four trackers is one
   peer, not four — otherwise the UI's peer count lies and the scheduler
   redials the same swarm repeatedly.

Plus a **last-resort tier** of well-known public trackers, because a healthy
torrent can still be undownloadable: the Ubuntu ISO names
`torrent.ubuntu.com`, which answers *"Requested download is not authorized"*
to every IP outside Canonical's ranges. It goes **last, never first** — a
private tracker is authoritative for its own swarm and the public ones may not
carry it at all.

### The deadline that wasn't

The KeePass torrent has 9 tiers, two of which are dead. Our first run found
**zero peers** and gave up, even though tiers 1–8 include trackers known to
work.

The pool had a 30-second overall deadline. BEP 15's retry schedule is
**15s, then 30s, then 60s** — so one unreachable tracker cost 108 seconds on
its own. The pool declared itself out of time while still inside tier 0 and
never reached the tiers that would have answered.

> **A deadline the transport cannot see is not a deadline.**

So both transports now take a `maxSeconds` budget, check it before every
attempt, and clamp each attempt's timeout to whatever remains. The pool passes
each tracker what is left of its window, capped at 25 seconds so one tracker
cannot monopolise a generous deadline. The dead tracker now fails cleanly in
25.77s as `out of time budget`, and the walk moves on.

---

## 4. Filenames are attacker-controlled

We built the output path by concatenating the output directory with
`torrent.name`. That name is chosen by **whoever published the torrent**, and
it is the one piece of a `.torrent` that turns into something dangerous:

```
info.name = "../../../../home/user/.bashrc"
info.name = "/etc/cron.d/pwn"
info.name = "..\\..\\Windows\\System32\\evil.dll"
```

That is the difference between "download this torrent" and "overwrite any file
the user can write".

`util/PathSafety` **rejects rather than cleans**. Sanitising is a losing game
— there are encodings, Unicode look-alikes, trailing dots that Windows strips
silently, and 8.3 short names, and every one is a way to write a file the user
did not agree to. A torrent whose name cannot be used safely is a torrent we
cannot serve, and saying so in one sentence beats guessing at the author's
intent.

It rejects: empty, `.`, `..`, any `/` or `\`, a drive letter, a leading `~`,
control characters, trailing dots or spaces, and Windows device names
(`CON`, `NUL`, `COM1`…).

Verified against six attack shapes; nothing was written outside the target
directory.

---

## 5. Knowing what a peer has

Once a download is more than one piece long, the most useful thing to know
about a peer is **which pieces it has**. It decides three things:

- which pieces are worth requesting (asking for one a peer does not have is
  not an error, it is a **stall** — nothing answers, so you wait out the read
  timeout);
- how many pieces are left that nobody can serve;
- when the download is genuinely finished.

The wire format is counter-intuitive: **MSB-first**. Piece 0 is bit 7 of byte
0. Write it the other way round and you get a client that quietly requests
pieces nobody has.

`Bitfield::parse()` enforces the two rules the spec states, because both are
ways a peer can confuse us:

| Check | Why |
|---|---|
| blob length must be `(pieces + 7) / 8` | a wrong length means a different dialect, or a different torrent |
| spare high bits of the last byte must be zero | otherwise the sender disagrees with us about the piece count |

And the default matters: **a peer that sends no bitfield is treated as owning
nothing**, not everything. Defaulting to "all ones" is a bug that looks like a
hung download rather than like a bug, because the client cheerfully requests
pieces and waits forever for answers nobody is obliged to send.

---

## 6. Connection reuse — the one that mattered most

Phases 5–7 fetched a piece by opening a connection, handshaking, saying
INTERESTED, waiting for UNCHOKE, pulling the piece, and hanging up. Then the
next piece started from scratch.

For a small torrent that is wasteful. For a real one it is fatal.

### The arithmetic

The Ubuntu ISO is 6.2 GB in 256 KiB pieces — **23,664 pieces**. The old design
means 23,664 connections and 23,664 handshakes, and roughly 470 sequential
requests aimed at each of the ~50 peers one tracker hands out. Real clients
keep the connection and move many pieces over it.

A `PeerSession` is exactly that: one connection, alive until it dies.

### Pipelining

Asking for one 16 KiB block and waiting means paying a full network round trip
per block. A 256 KiB piece is 16 round trips, and on a 100 ms link that is
1.6 seconds of pure waiting.

So we keep up to **8 block requests in flight** and match each arriving PIECE
back to the block that asked for it. Matching is by `(index, begin)` — not
just for speed, but for **correctness**: a peer may legitimately answer out of
order.

The test that keeps this honest is a blunt one:

```
PASS: 4 workers fetched all 12 pieces over just 7 connection(s)
```

The Phase 7 test used to assert *12 pieces == 12 connections* — it was pinning
the very behaviour we had just replaced. It now asserts pieces arrive over
strictly fewer connections, so reintroducing a connect-per-piece path fails it.

### Choke, HAVE, keep-alives

- A **CHOKE** aborts the piece in flight but keeps the connection. Real peers
  choke and unchoke constantly as a fairness measure; treating a choke as a
  dead peer throws away a working connection.
- **HAVE** grows the bitfield as the peer completes pieces, which is how a
  long-lived session stays accurate.
- **Keep-alives** every 100 seconds, so an idle-but-healthy peer is not reaped
  by a read timeout — and so we are not reaped by the peer.

---

## 7. Claims that cannot be stolen

Phase 7's claim protocol was the right idea and slightly incomplete. A piece
marked `CLAIMED` stays claimed until its worker resolves it — but a worker
talking to a peer that has stopped responding may sit there far longer than
anyone is willing to wait, and the last few pieces of a download can stall
behind one unresponsive peer forever.

So a claim can now be **reclaimed** after 90 seconds of no progress.

Which immediately creates a new race: the slow worker's socket might come
alive, it hands back a perfectly valid piece, and it writes it — over the piece
somebody else has meanwhile re-downloaded. Which is why `claimPiece()` returns
a **token**, and `storePiece()` refuses a write whose token no longer matches:

```cpp
if (token != kNoClaim) {
    if (index >= claimTokens_.size() || claimTokens_[index] != token) {
        return false;  // reclaimed underneath us: drop it
    }
}
```

One comparison under the lock turns a slow-but-eventually-valid answer from a
data-corruption bug into a discarded packet. (Java parallel: a version stamp,
or an `AtomicLong` you compare before writing so a slow thread cannot clobber
a newer writer.)

---

## 8. Progress you can look at

A download that takes two hours and prints nothing is indistinguishable from a
hung program. So the client shows what is happening, in place.

**The callback is invoked from one thread.** Workers do not call it — they only
bump a handful of atomics, and a dedicated reporter thread snapshots them ten
times a second. This matters twice over: per-worker throttling does not give a
global 10 Hz (N workers at 10 Hz is 10N), and a callback that draws to a
terminal must not be entered from several threads at once. It also means the
UI needs **no lock of its own**.

**Speed is a rolling window, not a lifetime average.** An average never
recovers from a slow start and its ETA drifts further and further from reality.
The raw windowed number was honest but flickered between 98 and 122 KiB/s on a
perfectly steady link, which reads as a problem rather than as progress — so
it is smoothed with an exponential moving average.

**The piece map is real state, downsampled.** A 23,664-piece ISO cannot be
drawn one character per piece, so each cell covers a run of pieces and is
filled in proportion to how much of that run is done. A download that is
crawling almost always has a very particular *shape* of hole, and that is the
thing worth seeing.

**Hand-rolled ANSI, not ncurses**, for three reasons:

1. no new dependency — the project is C++17 plus OpenSSL and has stayed that
   way on purpose;
2. it **degrades instead of breaking**: `isatty()` decides, so piping to a
   file gets periodic plain lines that still diff, while ncurses has no useful
   answer for a non-terminal stdout;
3. it is testable, because there is no global terminal state to set up.

One detail that is easy to get wrong: **the cursor is made visible again on
every exit path**, including a signal. An interrupted download that leaves an
invisible cursor makes the user think their terminal is broken.

---

## 9. Resume, which had never actually worked

Phase 6 wrote `scanDisk()` — re-hash every piece on disk and mark the ones that
match — and the help text had promised "rerun the same command to resume" since
the day the CLI was written.

`scanDisk()` was called from exactly **one place: the tests.** Every single CLI
run re-downloaded the whole file from scratch. A promise in the help text that
nothing implements is worse than no promise at all, because the user plans
around it.

It now happens inside `ConcurrentDownloader::download()`, so every caller gets
it rather than having to remember. A size check runs first, because hashing a
freshly preallocated 6 GB of zeros to learn that it is empty would read 6 GB
for nothing.

The progress counters are then **baselined at what was recovered**, because
the bar describes the file, not this run's transfers. Without that, resuming a
99%-complete file cheerfully displays `0%`.

Verified properly: corrupt two pieces of a finished download, rerun, and
`scanDisk()` finds exactly those two, refetches them, and the file comes back
**byte-identical** to a known-good copy.

---

## The bugs this phase turned up

Every one of these was found by running the thing, not by reading it. That is
the recurring lesson of this project.

| Bug | Symptom | Cause |
|---|---|---|
| Byte-reversed peer IPs | tracker "worked", every handshake failed | assigned a host-order integer to a field already in network order |
| Uninitialised `length` | a 3 MB file claimed to be 86.1 TiB | multi-file torrents have no `length` key, so the field was never assigned |
| 96-byte announce | no reply at all, ever | the spec diagram disagrees with deployed trackers |
| `addrinfo` use-after-free | connect timed out against a tracker Python reached in 0.17s | copied the struct, not the address it pointed at, then freed it |
| Wrong last-piece size | the short final piece never verified | computed from `pieceCount × pieceLength` instead of the real total |
| `accept()` never woken | tests hung on cleanup | closing a listen fd does not unblock a thread already parked in `accept()` |
| Unenforceable announce deadline | 0 peers from a 9-tier torrent | BEP 15's 105s retry schedule outlived the pool's 30s deadline |
| Path traversal | — | a torrent's `name` used as a filename |
| Resume never wired | full re-download every run | `scanDisk()` called only from tests |
| `std::string` to `%s` | a tracker host rendered as `下載` | varargs undefined behaviour |
| Peer counter drift | `peers -2 connected` | decremented for a session that had been counted |

The last two are worth dwelling on, because neither would survive code review
by reading alone. The `std::string`-to-`%s` bug sent me hunting for a
non-ASCII tracker URL inside the torrent; there wasn't one, and the real cause
was that passing a `std::string` object to a `%s` conversion makes the callee
read the object's bytes as a `char*`. It printed a plausible-looking line the
whole time.

---

## What Phase 8 deliberately does not do

Being clear about this is more useful than a roadmap of optimism.

| Not done | Why it matters |
|---|---|
| **Multi-file torrents** | the biggest compatibility gap: most real torrents (series, albums, CD1/CD2) are multi-file and are **refused with a clear message** |
| **Seeding** | download-only is a valid leecher, but the swarm gets nothing back, so some peers ignore us |
| **DHT** | tracker-only. If every tracker is blocked we find nobody, even though peers exist — which is exactly the Ubuntu-ISO situation |
| **uTP** | BitTorrent's UDP peer transport. TCP-only peers work; uTP-only ones are invisible to us |
| **BEP 47 padding** | cosmetic: padding is real zero bytes, so treating `.pad/N` as an ordinary file still writes byte-correct output, just with junk files alongside |
| **Magnet links** | needs `metadata` exchange over the peer protocol |
| **Block-level parallelism** | one piece per worker is complete for 16 KiB pieces; real 256 KiB pieces would benefit from splitting one piece across peers |

Single-file torrents work today, end to end, verified byte-for-byte — which is
what a 4.4 MiB KeePass installer and a multi-gigabyte ISO both go through.

---

## The code, piece by piece

### The types this phase added

| Type | Header | Its one job |
|---|---|---|
| `TrackerPool` | `tracker/TrackerPool.hpp` | scheme dispatch + the BEP 12 tier walk |
| `TrackerResult` | same | peers, swarm counts, and a human-readable line per tracker |
| `UdpTracker` | `tracker/UdpTracker.hpp` | BEP 15: two datagrams, one round trip |
| `Bitfield` | `peer/Bitfield.hpp` | which pieces a peer has, with a validated wire format |
| `PeerSession` | `peer/PeerSession.hpp` | one long-lived connection, many pieces |
| `TerminalUI` | `ui/TerminalUI.hpp` | the live display, and the plain-line fallback |
| `PathSafety` | `util/PathSafety.hpp` | refuses torrent-supplied paths that could escape a directory |

### Techniques worth naming

1. **Golden-byte protocol tests** — check the exact bytes against the spec, not
   that the arithmetic round-trips.
2. **Non-blocking connect + `poll()` + `SO_ERROR`** — writable ≠ succeeded.
3. **Enforceable deadlines** — a budget the transport itself checks.
4. **Tiered, shuffled, parallel, de-duplicated announces** with a
   last-resort fallback.
5. **Rejection over sanitisation** for untrusted paths.
6. **Connection reuse with request pipelining**, matched by `(index, begin)`.
7. **Claim tokens + time-based reclaim** so a late worker cannot clobber.
8. **Atomics plus one reporter thread** for a lock-free, single-threaded UI.
9. **Rolling-window rate with an EMA** so the ETA is not a lie.
10. **Graceful degradation by `isatty()`** instead of a hard dependency.
11. **Restore the cursor on every exit path.**
12. **Async-signal-safe interrupt handling** — flip a flag, nothing else.

### A one-paragraph mental model

> `main` parses and validates flags, parses the torrent, and asks
> `TrackerPool` for peers — which shuffles each BEP 12 tier, announces to that
> tier's trackers in parallel, each capped by the deadline it was given, and
> falls through to the next tier if all of them are dead. Then
> `ConcurrentDownloader` starts N workers, each holding a `PeerSession` to one
> peer: it reads that peer's `Bitfield`, claims a piece the peer actually has,
> pipelines its 16 KiB blocks, and hands the assembled result to
> `PieceManager::storePiece`, which re-verifies the SHA-1 and writes it at the
> right offset. A watchdog reclaims any claim that has gone quiet for 90
> seconds, and a single reporter thread turns a handful of atomics into a
> progress callback the terminal UI can draw. Nothing from a `.torrent` is
> trusted: lengths, hashes, tracker replies and the file's own name are all
> checked before they are used.

---

*[Back to the index](README.md)*
