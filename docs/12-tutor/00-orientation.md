# 00 — Orientation: The Build, the Targets, and the Shape of the Program

> **Session goal.** By the end you should be able to answer, without looking:
> *what happens between `./build/peerflow` starting and bytes hitting the disk?*
> That is the skeleton we hang all 12 remaining modules on.
>
> **Your background:** strong Java, new to C++, new to networking. This module
> is therefore mostly about the *build* and the *shape* — the things neither
> the manual nor a C++ tutorial tends to show you.

---

## 1. The build

Three commands, and they are not magic:

```bash
cmake -B build          # 1. configure: generate a Makefile for THIS machine
cmake --build build     # 2. compile: turn .cpp into .o, then link into binaries
./build/peerflow-tests  # 3. run
```

### Why two steps, and what `-B` means

**Step 1 is not a compiler.** `cmake` doesn't build anything. It *asks your
operating system questions* — which compiler is installed, where is OpenSSL,
does this machine support C++17 — and writes the answers into `build/`, most
importantly a `Makefile`.

> **JAVA PARALLEL:** this is the difference between `mvn` (a build *tool*, which
> runs plugins and resolves dependencies) and the compiler itself (`javac`).
> `mvn package` runs `javac` for you. Here, `cmake` produces the equivalent of
> a build script, and the Makefile that appears in `build/` is that script.

`-B build` means "put the generated files in a directory called `build`".
Running it again later is cheap and safe — it re-checks the answers and
regenerates. This is why **you should never edit anything in `build/`**; it is
disposable.

> ### **WITHOUT vs WITH — the build directory**
>
> ```
> WITHOUT  you edit build/Makefile to add a new source file
>          -> works today.
>          -> next `cmake -B build` silently OVERWRITES your edit,
>             and the new file vanishes from the build. You lose 20 minutes
>             wondering why your code "isn't running".
>
> WITH     the only file you ever edit is CMakeLists.txt
>          -> `cmake -B build` regenerates build/ from it
>          -> `rm -rf build` at any time is a SAFE, complete reset
>
> THE RULE  treat build/ as disposable. The source of truth is CMakeLists.txt.
> ```

Here is the **actual** command CMake generated for `src/main.cpp`, read out of
`build/compile_commands.json`:

```
/usr/bin/c++  -I/home/rahul/Documents/Torrent_Client/include  -std=gnu++17 \
    -o CMakeFiles/peerflow.dir/src/main.cpp.o -c .../src/main.cpp
```

Note `-I.../include`. That one flag is the subject of §3.

### `CMakeLists.txt` — the three targets

The whole build config is 117 lines, and 80 of them are comments. The
substance is this:

```cmake
project(PeerFlow LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(Threads REQUIRED)

add_library(peerflow_core STATIC      # 1. the engine
    src/bencode/BencodeDecoder.cpp
    src/torrent/TorrentParser.cpp
    ...17 files...
)
target_include_directories(peerflow_core PUBLIC include)
target_link_libraries(peerflow_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(peerflow src/main.cpp)              # 2. the CLI
target_link_libraries(peerflow PRIVATE peerflow_core)

add_executable(peerflow-tests src/tests/TestMain.cpp src/tests/TestRunner.cpp)  # 3. the suite
target_link_libraries(peerflow-tests PRIVATE peerflow_core)
```

Three things to understand here, and they are all about **why a file is
written the way it is**:

**(a) `LANGUAGES CXX`.** The *only* external dependencies are **OpenSSL** (for
SHA-1 and TLS) and **pthreads**. There is no JSON library, no logging library,
no argument parser, no terminal library. The argument parser is hand-written;
the terminal display is hand-written ANSI escapes; bencode is hand-written.
That is a deliberate constraint — you can read every line, and you can change
every line.

**(b) `include` as an include directory, `PUBLIC`.** `target_include_directories
(peerflow_core PUBLIC include)` means *anything that links against this library
can write `#include "peer/TcpSocket.hpp"`*. That is why `src/main.cpp` includes
`"peer/ConcurrentDownloader.hpp"` with no path gymnastics. The word `PUBLIC`
propagates the setting to dependents; `PRIVATE` would keep it to the library
itself.

**(c) One static library, two programs.** The engine is compiled **once** into
`libpeerflow_core.a`, then **linked twice**:

| Artifact | What it is |
|---|---|
| `build/libpeerflow_core.a` | the engine, 17 `.cpp` files, ~3.9 MB |
| `build/peerflow` | the CLI — **479 lines**, all of it wiring |
| `build/peerflow-tests` | the test suite — 1,529 lines |

> **JAVA PARALLEL:** this is a jar plus two thin mains. The engine is compiled
> once; `peerflow` and `peerflow-tests` are two entry points onto the same code.

#### **Measured on this machine, right now**

The static library is not a stylistic preference — it is a measurable one.
I touched one file at a time and timed the rebuild:

| What I changed | What got rebuilt | Time |
|---|---|---|
| *nothing* (no-op build) | nothing | **1.1 s** |
| `src/main.cpp` (1 file, 479 lines) | that 1 file + relink `peerflow` | **6.1 s** |
| `include/torrent/TorrentFile.hpp` (**32 lines**) | **7 `.cpp` files** + relink all 3 targets | **15.0 s** |

That third row is the one to internalise. `TorrentFile.hpp` is 32 lines and
has no function bodies at all — yet editing it forced a recompile of the
parser, the tracker pool, the downloader, the piece manager, the UI, the test
suite, and main. **A header edit is a whole-program edit**, because every file
that `#include`s it must be recompiled.

> ### **WITHOUT vs WITH — the static library**
>
> ```
> WITHOUT  (one executable, no library — say you put a helper in main.cpp,
>           or built a single `peerflow` target from all 20 .cpp files)
>          -> touching src/main.cpp can dirty the WHOLE engine
>          -> you rebuild 20 objects + relink for a one-line change
>          -> and `peerflow-tests` would have to be rebuilt too, dragging
>             OpenSSL, fake servers and 1,500 lines of tests into your edit-
>             compile-test cycle
>
> WITH     `peerflow_core` is compiled independently
>          -> the CLI's own changes relink only `peerflow`  (6.1 s measured)
>          -> the engine's .o files are untouched and reused
>          -> `peerflow-tests` keeps its own build state
>
> THE RULE  logic belongs in the library, never in main.cpp. main.cpp is
>          wiring; when it grows, the design is wrong.
> ```

`src/main.cpp` is 479 lines. `src/tests/TestRunner.cpp` is 1,504. Neither is
the product. **If you ever find yourself wanting to add logic to `main.cpp`,
that is a design smell in this project** — it belongs in the library.

#### **WITHOUT vs WITH — the include directory**

```cmake
target_include_directories(peerflow_core PUBLIC include)
```

This single line produces the `-I.../include` flag you saw above. It decides
what every `#include` in the project is allowed to look like.

> ```
> WITHOUT  no -I flag, so includes must be relative to the file:
>            #include "../include/peer/PeerSession.hpp"     in src/peer/
>            #include "../../include/peer/PeerSession.hpp" from src/tests/
>            #include "../../../include/peer/PeerSession.hpp" from a
>                           subdirectory you create later
>          -> the path is RELATIVE TO THE FILE DOING THE INCLUDING
>          -> move a file, or nest one level deeper, and EVERY include that
>             pointed at it breaks. In a 20-file project that is a cascade of
>             compiler errors that have nothing to do with your change.
>
> WITH     -I include makes `include/` a root:
>            #include "peer/PeerSession.hpp"      works from ANY file,
>                                                    in ANY directory, FOREVER
>          -> moving a file changes nothing, because the include is resolved
>             against the -I root, not against the including file
>
> THE RULE  include by the -I root ("peer/PeerSession.hpp"), never by a
>          relative path ("../../include/..."). This is the single most
>          common C++ mistake and C++ offers no protection against it.
> ```

`PUBLIC` vs `PRIVATE` is the other half:

> ```
> PRIVATE  include/ is added for peerflow_core's own compilation only
> WITH     -> but main.cpp also #includes "torrent/TorrentFile.hpp"
> PUBLIC   -> the setting propagates to everything that links peerflow_core,
>            so main.cpp and the test suite get the -I flag too
>
> THE RULE  PUBLIC = "my consumers need to see my headers too". Which is
>          exactly true of a library whose entire interface is in include/.
> ```

### What gets generated in `build/`

```
build/
├── CMakeCache.txt          # the OS answers, cached (rm -rf build to reset)
├── Makefile                # the generated build script
├── compile_commands.json   # exact compiler command per file (clangd, gdb)
├── libpeerflow_core.a      # the engine
├── peerflow                # the CLI
├── peerflow-tests          # the suite
└── phase6-out.bin, ...     # test artefacts (the suite writes into the CWD)
```

`compile_commands.json` is worth knowing about: it tells tools like `clangd`
and `gdb` the exact flags each file was compiled with. If your IDE is
confused, delete `build/` and re-run `cmake -B build`.

---

## 2. The baseline — verified before we started anything

Never start a debugging session without a known-good state. So, first:

```
$ ./build/peerflow-tests
...
=== Results ===
Passed: 38
Failed: 0

real  0m55.279s
```

**38/38, in 55 seconds.** And it is worth noticing *what* that run did: it
reached the real internet. Two of the 38 tests are live:

- **`tracker announce succeeded`** — a real HTTPS request to
  `tracker.opentrackr.org` using the real Ubuntu 24.04.1 info hash. It
  returned 8 peers.
- **`connected to at least one real peer`** — it handshaked with 3 of those 8
  for real (`peer_id = -qB5230-GZFsJoS8O!gU` — that's qBittorrent, a real
  client, speaking to our code).

The other 36 are hermetic: they run against `FakePeer` on `127.0.0.1`, or they
are pure byte-level checks. That split matters — it means when you break
something you can immediately tell whether you broke the *logic* or the
*network*.

> **A trap to know about now:** test 26 (`connected to at least one real peer`)
> **always passes**. Look at the code and you will see it prints
> `NOTE: no peers accepted...` and still increments `passed`. It exists to
> exercise the harness, not to assert the swarm is alive. Don't trust it as a
> regression signal.

**Note on the CWD:** the suite writes `phase6-out.bin` and friends **relative
to your current directory**. Run it from the repo root. It is not
parallel-safe and not re-entrant.

> ### **WITHOUT vs WITH — live tests vs hermetic tests**
>
> ```
> WITHOUT  all 38 tests hit the real internet
>          -> CI is flaky by construction: it fails when the network hiccups,
>             when a tracker is down, when the Ubuntu swarm is pre-empted
>          -> and worst of all: when your test fails you CANNOT TELL WHETHER
>             IT'S YOUR CODE OR THE INTERNET
>          -> 36/38 of these tests need none of that
>
> WITH     36 hermetic (FakePeer on 127.0.0.1, or pure byte checks),
>           2 explicitly live
>          -> break something, rerun: if the 36 still pass, the bug is in
>             what talks to the network. If they fail, it's your logic.
>          -> knowing the boundary turns a 55-second mystery into a 5-second
>             bisection
>
> THE RULE  keep the network OUT of the tests you rely on. Depend on it
>          only where the network is genuinely the thing under test.
> ```
>
> `FakePeer` exists purely to make that possible: a real server on `127.0.0.1`
> that speaks the real protocol, so the peer code can be tested with no
> internet, and — crucially — can be told to **misbehave on demand**
> (`destroyByte()` flips one byte so the SHA-1 gate has something to reject).
> You will meet it properly in Module 12.

---

## 3. The directory layout, and the one rule that governs it

```
Torrent_Client/
├── CMakeLists.txt      the build config (117 lines, 80 of them comments)
├── README.md           the front door
├── docs/               the manual — what BitTorrent is and why
│   └── 12-tutor/       ← this teaching track
├── include/            .hpp = "the plan" (declarations)
│   ├── bencode/        Phase 1  the language .torrent files are written in
│   ├── torrent/        Phase 2  reading a .torrent, computing the info hash
│   ├── tracker/        Phase 3+8 HTTP, UDP, the tier pool
│   ├── net/            shared   TcpSocket — the one network primitive
│   ├── peer/           Phases 4-8 handshake, messages, sessions, concurrency
│   ├── piece/          Phase 6-8 the ownership table, claims, resume
│   ├── ui/             Phase 8  the live terminal display
│   ├── util/           Phase 8  path safety
│   └── tests/          the suite's entry point
├── src/                .cpp = "the implementation"
│   ├── main.cpp        Phase 8  the CLI (479 lines, all wiring)
│   ├── tests/          the suite
│   └── one dir per phase, mirroring include/
└── test/               a .torrent file for live checks
```

**The rule: `include/` and `src/` mirror each other, directory for directory.**
`include/peer/PeerSession.hpp` is implemented by `src/peer/PeerSession.cpp`.
21 header/source pairs, no exceptions. The `include` prefix is stripped by
`target_include_directories`, which is why every include in the project is
written `"peer/PeerSession.hpp"` and never `"../../include/peer/PeerSession.hpp"`.

**8,569 lines total, of which 1,983 are the test suite.** So roughly 6,600
lines of product code. Distributed:

| Directory | `.hpp` + `.cpp` | Largest file |
|---|---|---|
| `bencode` | 677 | `BencodeDecoder.cpp` (356) |
| `torrent` | 371 | `TorrentParser.cpp` (314) |
| `tracker` | 1,501 | `HttpTracker.cpp` (502) |
| `net` | 301 | `TcpSocket.cpp` (219) |
| `peer` | 2,302 | `ConcurrentDownloader.cpp` (454) |
| `piece` | 437 | `PieceManager.cpp` (239) |
| `ui` | 517 | `TerminalUI.cpp` (406) |
| `util` | 174 | `PathSafety.cpp` (117) |
| `main.cpp` | 479 | — |

---

## 4. The call graph: from a command line to bytes on disk

This is the skeleton. Every module after this one hangs off a box in it.
Read `src/main.cpp:320` (`int main`) and confirm each step against the source.

```
./build/peerflow ubuntu.torrent
        │
        ├─1. parseArgs()                      main.cpp:204
        │      -o, -j, -t, -a, -q, --no-tui, -h, -v
        │      hand-rolled, strict: "12abc" is REJECTED, not parsed as 12
        │
        ├─2. TorrentParser::parse(path)       main.cpp:357
        │      └─> BencodeDecoder::decode(raw bytes)        [Module 2]
        │          extract announce, name, piece length, piece hashes
        │          REJECT multi-file torrents loudly
        │          compute infoHash from the RAW bencoded bytes
        │
        ├─3. makeSafeOutputPath(outdir, name) main.cpp:377
        │      the ONLY untrusted value that reaches a filesystem path
        │
        ├─4. generatePeerId()  "-PF0001-" + 12 random chars
        │
        ├─5. TrackerPool(torrent).announce(...)             [Modules 5, 6]
        │      tier 0: all trackers in the tier, 8 at a time, in parallel
        │      no answer? -> tier 1 -> ... -> built-in fallback tier
        │      └─> scheme "udp"  ? UdpTracker::announce()   (BEP 15)
        │          scheme "http"? HttpTracker::announce()   (HTTPS, redirects)
        │
        ├─6. ConcurrentDownloader::download(...)            [Modules 8, 9, 10]
        │      │
        │      ├─ PieceManager(torrent, outputPath)          [Module 8]
        │      │     ftruncate(fd, fullLength)   ← preallocate the whole file
        │      │     scanDisk() if a partial file exists  ← resume
        │      │
        │      ├─ N worker threads (default 8), each with its OWN PeerSession
        │      │     loop:
        │      │       pop a peer from the shared queue
        │      │       PeerSession::connect(peer)
        │      │         └─> TcpSocket::connect()           [Module 4]
        │      │             └─> PeerHandshake::exchange()   68 bytes, [Module 7]
        │      │                 verify the info hash matches
        │      │         read BITFIELD, send INTERESTED, wait for UNCHOKE
        │      │       index = manager.nextPieceToFetch(&session.available())
        │      │       token = manager.claimPiece(index)     ← atomic, no races
        │      │       session.fetchPiece(index, hash, ...)  ← 16 KiB blocks,
        │      │         pipelined 8 deep, SHA-1 verified   [Module 7]
        │      │       manager.storePiece(index, data, token)
        │      │         SHA-1 gate AGAIN, then write at offset
        │      │
        │      ├─ reporter thread: 10 Hz, calls the UI callback. The ONLY
        │      │    thread that writes to the terminal, by construction.
        │      └─ watchdog thread: every 5s, reclaims claims stuck > 90s
        │
        └─7. ui.finish()                      [Module 11]
               exit 0 = success, 1 = failure, 130 = Ctrl-C
```

### Four contrasts hiding in that diagram

None of these are visible in the call graph. All four are the reason the code
looks the way it does.

#### 1. Preallocation — `ftruncate` before a single byte arrives

> ```
> WITHOUT  write pieces as they arrive, growing the file
>          -> the file's SIZE lies about what you have.
>          -> a resume run opens the file, measures it, and concludes
>             "I have 40% of the data" when in fact you have one contiguous
>             run of whatever order the pieces happened to land in.
>             scanDisk() would then re-hash and find the real state anyway,
>             but only AFTER doing I/O sized by a wrong guess.
>          -> worse: a piece at offset 9 MB cannot be written until 9 MB
>             exists. A file is a contiguous array. Writing out of order
>             into an unpreallocated file forces the filesystem to SPARSE-
>             allocate and later fill, and on many filesystems that is slower
>             than one big sequential write.
>
> WITH     ftruncate(fd, fullLength) in the constructor
>          -> the file is its final size from second one
>          -> ANY piece can seek to index * pieceLength and write
>          -> order stops mattering entirely
>          -> SIZE becomes a legitimate hint (still verified, never trusted)
>
> THE RULE  preallocate, so that arrival order and layout are decoupled.
> ```
>
> **The cost, stated honestly:** free space must exist up front, and `--help`
> says so. The project accepted a real cost rather than the bug.
>
> **And this is where the 86.1 TiB bug bites:** `ftruncate` blindly trusts
> its size argument. An uninitialised `length` (a field the parser never
> assigns for a multi-file torrent) flowed straight in and asked the kernel
> for 86 terabytes. That is commit `742ad51`, and the fix was two `= 0`s in
> `TorrentFile.hpp`.

#### 2. The same SHA-1 check, twice

`PeerSession::fetchPiece` verifies the piece. `PieceManager::storePiece`
verifies it again.

> ```
> WITHOUT (one check, in the session)  a bug in the session's own
>          verification logic - an off-by-one in the block offsets, a piece
>          reassembled from blocks in the wrong order, a hash compared
>          against the wrong index into the pieces array - would let a
>          CORRUPT piece through and straight onto your disk. And it would
>          be permanently undetectable afterwards, because once written
>          there is no record that it was ever wrong.
>
> WITH (two checks, in different layers)
>          the two bugs would have to be in DIFFERENT places, with the
>          same error, at the same time. In practice: impossible to sustain.
>          Corrupted bytes never cross the API boundary in the first place,
>          and the manager refuses them even if they somehow do.
>
> THE RULE  defence in depth at layer boundaries. A function that receives
>          untrusted data verifies it itself, regardless of who sent it.
> ```

This is why `storePiece` checks in this exact order: **token → bounds → exact
size → hash → write.** Nothing touches disk on any mismatch, so a rejected
piece leaves no trace to clean up.

#### 3. Token check and state change under ONE lock

`PieceManager` has one `std::mutex`, and `storePiece` holds it across the
token comparison *and* the write *and* the state change.

> ```
> WITHOUT  lock(); ok = (claimTokens_[i] == myToken); unlock();
>          write data to disk;                 //  ...time passes...
>          lock(); states_[i] = kOwned; unlock();
>
>          In that gap the watchdog thread can fire:
>              reclaimStuckPiece(i)  ->  claimTokens_[i] = NEW_TOKEN
>          Now a DIFFERENT worker legitimately owns piece i. We did not know
>          that. We overwrite its work with our (stale, possibly different)
>          bytes, and set state to kOwned. The file now holds the wrong
>          piece, and every future claim is refused because it looks owned.
>          Nothing crashes. The download just silently produces a corrupt
>          file. This is a race condition, and it is the hardest bug class
>          to reproduce and to diagnose.
>
> WITH     lock() held across check AND write AND state change
>          reclaimStuckPiece() cannot interleave - it needs the same lock.
>          Either we check and write first, or the reclaim happens first and
>          our token no longer matches and we are turned away.
>
> THE RULE  if a check and the action it protects must be inseparable, they
>          go in ONE critical section. This is the textbook TOCTOU
>          (time-of-check-to-time-of-use) bug.
> ```

That is also *why* `reclaimStuckPiece` installs a **new** token rather than
clearing to `kNoClaim` (`PieceManager.cpp:156`): a fresh token is what makes
the stale worker's comparison fail.

#### 4. One reporter thread, by construction

Progress is read by the UI from several worker threads' worth of state, but
it is *written* to the terminal by exactly one thread.

> ```
> WITHOUT  each worker calls ui.onProgress() itself
>          -> 8 workers x 10 Hz = 80 cursor rewinds per second
>          -> TerminalUI has mutable state (linesDrawnLast_, speedSamples_,
>             bytesPerSecond_, the log deque) and would need a mutex
>          -> the ANSI redraw would interleave mid-line and produce garbage
>
> WITH     workers only touch std::atomic counters. ONE reporter thread
>          snapshots them at a true global 10 Hz and is the sole caller of
>          the UI callback. TerminalUI needs no lock at all.
>
> THE RULE  push concurrency to the boundary. If only one thread may touch
>          a thing, no lock is needed - choose the architecture so that's
>          true, rather than locking after the fact.
> ```

### The five jobs, mapped to phases

| Job | Phases | Module |
|---|---|---|
| Read the `.torrent` | 1–2 | 2, 3 |
| Find peers | 3 | 5, 6 |
| Talk to peers | 4 | 7 |
| Download pieces | 5–6 | 7, 8 |
| Save, and share | 6–8 | 8, 9, 10 |
| *Seeding (not built)* | 9 | — |
| *DHT, uTP, magnets (not built)* | 10 | — |

---

## 5. How this project grew — and what that tells you

34 commits, 16,458 insertions, 2,519 deletions, across 8 days (16–28 Sep 2026).
Read the subjects in order; they are a curriculum, and roughly a third of them
are **bug fixes, not features**.

```
d33f831  Add bencode decoder (Phase 1)                                  16 Sep
75dd5f8  Add torrent file parser (Phase 2)                              22 Sep
40db3a3  Rename to PeerFlow and add tracker announce (Phase 3)          25 Sep
672c795  Add peer handshake (Phase 4) with shared TcpSocket + FakePeer   25 Sep
060f9b1  Add peer messages and piece download with SHA-1 (Phase 5)      26 Sep
c98662d  Add piece manager with verified disk writes and resume (Ph 6)  26 Sep
9f7f990  Add concurrent downloading across many peers (Phase 7)         27 Sep
d591d60  Add CLI shell, split CMake targets, announce-list, TrackerPool  28 Sep
f99b37d  Add UDP tracker support (BEP 15)                               28 Sep
c943eaa  Add built-in fallback tracker tier                              28 Sep
5903282  Add PeerSession: reuse, pipelining, bitfield-aware claiming     28 Sep
68b5f64  Add live terminal UI and wire the download into the CLI         28 Sep
7b1aaf3  Fix peersConnected drifting negative                            28 Sep
742ad51  Reject multi-file torrents, fix uninitialised torrent fields    28 Sep
a0f85c6  Enforce the announce deadline inside the tracker transports     28 Sep
2a84353  Rebuild the CLI, and fix four bugs it turned up                 28 Sep
404b6dd  Document Phase 8, bring docs and comments up to date             28 Sep
2a9be64  Remove the test suite's dependency on a gitignored fixture      28 Sep
```

**The phase structure is not artificial.** Each phase was built, then *run*, and
running it is what produced the next commit. Phase 8's commit is literally
`Rebuild the CLI, and fix four bugs it turned up` — the bugs were not found by
reading the code, they were found by making the thing runnable and pointing it
at the internet. That is what Track 3 (bug archaeology) is about.

Two commits are also worth flagging now because they are about *process*:

- **`379b35e` "Correct docs that blamed the firewall for our own byte-order
  bug."** A documentation bug where the notes confidently attributed a failure
  to the environment instead of admitting the code was wrong. The lesson: when
  something inexplicable happens, the first hypothesis is your own code.
- **`2a9be64` "Remove the test suite's dependency on a gitignored fixture."**
  The suite read a 464 KB Ubuntu `.torrent` from `test/`, but that file is
  gitignored, so a fresh clone had failing tests. The fix spells the info hash
  inline instead. *The test was "passing" on the author's machine and broken
  for everyone else.*

---

## 6. Tooling you have available

| Tool | Present? | Use it for |
|---|---|---|
| `gdb` | ✅ | breakpoints, stepping, watching an fd change hands |
| `strace` | ✅ | **the single best learning tool in this repo** — see below |
| `ltrace` | ✅ | library calls (e.g. OpenSSL) |
| `valgrind` | ✗ | would be the tool for leaks/races; not installed |
| `clang-tidy`, `cppcheck` | ✗ | static analysis; not installed |

### `strace` — see the 68-byte handshake leave your machine

This is the highest-value 60 seconds you will spend in Session 1. It shows
real bytes going to a real peer, and it needs no code changes:

```bash
strace -f -e trace=connect,write,read -s 200 ./build/peerflow \
       test/ubuntu-24.04.1-desktop-amd64.iso.torrent -o /tmp/dl --no-tui
```

Then read the output for the handshake. You should see, for a working peer:

- a `connect(...) = 0` — the TCP connection succeeded
- a 68-byte `write(...)` that **starts with the byte 19 followed by the ASCII
  string `BitTorrent protocol`**

That is Phase 4, in a shell, with no debugger. `Ctrl-C` after you see it.

`gdb` once you want to go deeper:

```bash
gdb --args ./build/peerflow some.torrent --no-tui
(gdb) break PeerHandshake::exchange
(gdb) run
(gdb) next
(gdb) print peer.ip          # the struct is right there
```

---

## 7. Two stale spots in the existing docs

Flagging so you don't lose time trusting them:

1. **`docs/01-big-picture.md:321` says "All 20 tests pass"** and its status
   table stops at Phase 4. Stale — the README and reality say 38/38 through
   Phase 8.
2. **`test/ubuntu-24.04.1-desktop-amd64.iso.torrent` exists but is not read by
   any test.** Commit `2a9be64` removed that dependency. The live tests use an
   info hash spelled inline instead. So that file is a convenience for *you*
   to point the CLI at, not a test fixture.

---

## 8. Where the traps are (so you can look for them deliberately)

The test suite covers bencode, torrent parsing, the wire protocol, `PieceManager`,
and concurrency. These have **zero tests**, so a bug in them will be silent:

| Untested | Where | Why that's a risk |
|---|---|---|
| `Bitfield` | `src/peer/Bitfield.cpp` | MSB-first packing. A one-bit error means you request pieces nobody has, and the download stalls mysteriously. |
| `PathSafety` | `src/util/PathSafety.cpp` | Security-relevant. Reject-don't-sanitise logic with no test proving it rejects `../../.ssh/authorized_keys`. |
| `TrackerPool` | `src/tracker/TrackerPool.cpp` | Tier walk, deadline, shuffle, fallback. Only exercised when the internet cooperates. |
| `PeerSession` | `src/peer/PeerSession.cpp` | 388 lines of the most intricate logic. Tested only indirectly via `ConcurrentDownloader`. |
| `TerminalUI` | `src/ui/TerminalUI.cpp` | Cosmetic, so a low-risk gap. |

These five are your first exercises in Track 4.

---

## 9. A real bug I found while writing the demo

`demo-02-recvExact.cpp` was supposed to illustrate one thing: why
`recvExact()` loops. While writing it, the program **kept dying instantly with
no output**. That turned out to be a genuine, unfixed defect in PeerFlow.

### The mechanism

On Linux, `send()` (or `write()`) to a socket whose peer has closed raises
**`SIGPIPE`**, and the default action of `SIGPIPE` is to **terminate the
process**. Not throw. Not return an error. There is no `catch` block that can
help — the process is simply gone.

### Why PeerFlow is exposed

`src/net/TcpSocket.cpp:165`:

```cpp
ssize_t n = ::send(fd_, p + sent, len - sent, 0);
//                                            ^ no MSG_NOSIGNAL
```

and:

```
$ grep -rn "SIGPIPE\|MSG_NOSIGNAL" src/ include/
(no matches)
```

The signal is never ignored anywhere in the project.

### Proof

I reduced it to a self-contained program: accept a connection, close it
immediately, then write repeatedly.

```
writing repeatedly to a peer that has closed...
  send #1 -> 65536          <-- first write is absorbed by the kernel buffer
>>> EXIT CODE: 141          <-- 128 + 13 = killed by SIGPIPE on send #2
```

`141` is the shell's encoding for "died on signal 13".

### Why it matters here

This violates the project's own stated golden rule — *"a bad peer must never
be able to crash your client"* — and it is not a rare edge case. It needs only
a peer that disconnects while we are mid-send, which happens routinely in a
real swarm: peers go away, NAT timeouts fire, and a 6 GB ISO has 23,664 pieces
and thousands of peers. The client dies, mid-download, with no resume message
and no explanation.

> ```
> WITHOUT  a guard
>          -> TcpSocket::sendAll() can kill the process outright
>          -> the crash looks identical to Ctrl-C or an OOM kill
>          -> and it is UNAVOIDABLE: a remote stranger can trigger it
>
> WITH     a guard
>          -> send() returns -1 with errno == EPIPE
>          -> sendAll() throws NetException, exactly like any other error
>          -> PeerSession catches it, drops that one peer, keeps going
>
> THE RULE  a signal whose default action is "kill" is never a default you
>          want. Handle it once, deliberately, at startup.
> ```

### The two fixes

Either is roughly one line.

1. **`signal(SIGPIPE, SIG_IGN);` once, at the top of `main()`.** Blanket
   protection, and it also covers writes that happen *inside* OpenSSL, which
   you cannot pass `MSG_NOSIGNAL` to. This is what most production clients do.
2. **Pass `MSG_NOSIGNAL` to every `send()`.** Precise and explicit, but it must
   be repeated at each call site, and it will not protect `HttpTracker`'s
   OpenSSL writes.

The careful answer is **both**. Note that the project already installs
handlers for `SIGINT` and `SIGTERM` (`main.cpp:347`) — so the author thought
about signals, and simply missed this one. That is a good illustration of why
"is it handled?" is a question you have to ask about *every* signal, not about
the one you remembered.

> **I have deliberately not fixed this.** It is a real bug, it is
> well-scoped, and it is a perfect first exercise: find it, fix it, add a
> regression test that a peer disconnecting mid-send does not kill the client,
> and see that the suite still says 38/38. Say the word and we'll do it
> together.

---

## Your turn

**Before the next session**, do these:

```bash
# 1. Confirm your own baseline (should say Passed: 38, ~55s)
./build/peerflow-tests

# 2. Run both demos and read the output carefully
g++ -O0 -std=c++17          -o /tmp/demo01 docs/12-tutor/demos/demo-01-byte-order.cpp
/tmp/demo01

g++ -O0 -std=c++17 -pthread -o /tmp/demo02 docs/12-tutor/demos/demo-02-recvExact.cpp
stdbuf -o0 /tmp/demo02

# 3. See the 68-byte handshake on the wire
strace -f -e trace=connect,write -s 200 ./build/peerflow \
       test/ubuntu-24.04.1-desktop-amd64.iso.torrent -o /tmp/dl --no-tui
#    Ctrl-C once you see a write starting with \x19 "BitTorrent protocol"

# 4. The single most useful file, start now, read to the end
less src/main.cpp      # 479 lines, and it is the map for the whole track

# 5. Measure the header-coupling effect yourself
touch include/torrent/TorrentFile.hpp && time cmake --build build
#    32 lines of header -> 7 recompiles, ~15s
```

Then answer the quiz below.

---

## Module 0 — Quiz

Answer in your own words, briefly. "I don't know" is a valid and useful answer
— it tells me where to re-teach.

1. **Two commands, two jobs.** What does `cmake -B build` actually produce, and
   what would go wrong if you hand-edited the files it puts in `build/`?

2. **Three targets.** `peerflow_core` is a static library; `peerflow` and
   `peerflow-tests` are executables. In Maven terms, what is each of these, and
   why is it a problem to have put logic in `src/main.cpp` instead of the
   library?

3. **The mirror rule.** `include/peer/PeerSession.hpp` is implemented by
   `src/peer/PeerSession.cpp`. Give the `#include` line that finds it, and
   explain which line of `CMakeLists.txt` is what makes that work — and what
   that line would look like if it said `PRIVATE` instead.

4. **Header coupling.** Touching a 32-line header with no function bodies in it
   forced 7 recompiles. Explain *why* in terms of what the compiler must
   guarantee about a `#include`.

5. **The call graph.** Name the four big steps between `main()` starting and
   bytes being written to your output file — one line each, no detail.

6. **Preallocation.** `PieceManager`'s constructor calls `ftruncate` to make
   the file its full size *before* any piece arrives. Give **two** reasons this
   is worth doing, one about correctness and one about performance.

7. **Double verification.** A piece's SHA-1 is checked twice — once in
   `PeerSession::fetchPiece`, once in `PieceManager::storePiece`. The code
   comment says this is deliberate. Why keep both when one would do, and why
   does the *ordering* of the checks inside `storePiece` matter too?

8. **The TOCTOU trap.** `storePiece` holds its lock across the token check
   *and* the disk write. Describe, step by step, what the watchdog thread does
   if the lock were released between the check and the write. What is the
   resulting file state, and why is this bug so hard to diagnose after the
   fact?

9. **Single-writer progress.** Why is there exactly one reporter thread
   instead of letting each worker update the display? What does that buy
   `TerminalUI`?

10. **Live vs hermetic.** Two of the 38 tests hit the real internet. What is
    the practical advantage of knowing which tests those are, when something
    breaks?

11. **The always-passing test.** Test 26, "connected to at least one real peer",
    increments `passed` whether or not any peer answered. Why is that defensible
    — and what should you use instead if you want to know the real internet is
    working?

12. **SIGPIPE.** `src/net/TcpSocket.cpp:165` calls `send()` with no
    `MSG_NOSIGNAL`, and the project never ignores `SIGPIPE`. Explain what
    happens to the *process* when a peer disconnects mid-send, why no `catch`
    block can save it, and name both standard fixes plus their trade-off.

13. **The stale doc.** `docs/01-big-picture.md` says "All 20 tests pass" and
    stops at Phase 4. What is the risk of a document that has drifted, and
    which source should you have treated as authoritative in Session 1?

14. **Your own words, one paragraph.** Explain to a Java developer who has never
    heard of BitTorrent what `./build/peerflow some.torrent` does, in plain
    language, using the pizza analogy if you like. This is the "can explain it
    end to end" goal — do it now, and we'll compare against the real thing at
    the end of the track.

---

*[Next: 01a — C++ types, const, and references](01a-cpp-types.md)*
