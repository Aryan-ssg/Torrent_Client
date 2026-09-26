# 02 — Phase 0: Setup and Tools

## What Phase 0 is about

Before the plumbing (Phases 3–4) or even the parsing (Phases 1–2), there is a
quiet but crucial job: **make sure our C++ code can be turned into a running
program reliably and effortlessly.** Computers don't run the code we write the
way we write it — a whole pipeline of tools has to convert our human-readable
text into machine instructions, find the libraries it depends on, and glue
everything into one runnable file.

This phase answers two questions:

1. **Where does our code live, and how is it organised?** (project layout)
2. **How do we build it?** (the build system)

The milestone: `cmake --build build` succeeds and produces an executable
(a runnable program file) called `peerflow` — and `./build/peerflow` runs a
suite of tests that all pass.

> If Java is your reference point: this chapter is the "why Maven instead of
> raw `javac`?" chapter. C++ has no universal standard build tool, so each
> project picks one — we use CMake.

## The two kinds of files: source code vs. compiled code

| | What it is | Everyday analogy |
|---|---|---|
| **Source code (.cpp / .hpp)** | the text a human writes and reads | the recipe written in the cookbook |
| **Compiled code (the executable)** | machine instructions the CPU actually runs | the dish that comes out of the kitchen |
| **The compiler** | the tool that translates | the cook who turns recipe → dish |

Only source code is saved in Git and shared. The compiled executable is
*generated* and thrown away whenever we rebuild (like washing the plate —
nobody saves plates, only recipes).

## The toolchain, one tool at a time

| Tool | What it does | Everyday analogy |
|---|---|---|
| **C++17 (the language)** | the vocabulary and grammar of our recipe | the cookbook's style and rules |
| **`g++` (the compiler)** | translates C++ source into machine instructions | the cook reading the recipe and doing the work |
| **CMake (the build system)** | manages the *whole* build: what, in what order, with what flags, with what libraries | the head chef who never forgets a step |
| **`make` (CMake's worker)** | executes the exact recipes CMake generated | the kitchen staff who do each step |
| **OpenSSL, Threads (libraries)** | pre-built, trustworthy bundles of code (cryptography, concurrency support) | pre-made, certified ingredients |
| **Git (version control)** | tracks every change to the source | a time machine for the recipe book |

### What "compiling" really does — the hidden pipeline

One `g++` command is really four stages, like a food truck line:

```
[ mycode.cpp ] ──(1. preprocessor)──> expanded text
              ──(2. compiler)──────> assembly (human-ish machine language)
              ──(3. assembler)─────> mycode.o  (machine code, "object file")
              ──(4. linker)────────> peerflow  (the final executable)
```

1. **Preprocessing (1):** a *copy-paste-and-tweak* stage. Every `#include` line
   is replaced by the contents of that file (headers), and `#pragma once` tells
   the preprocessor "paste this header only once". This is pure text
   manipulation before any real translating happens.
2. **Compiling (2):** the brain of the wheel — converts the (now expanded) C++
   into **assembly (a slightly human-readable version of machine code)**.
3. **Assembling (3):** converts that assembly into actual machine-code bytes,
   stored in an **object file (a file ending in `.o` containing machine code
   for *ours* but with every external name still unresolved)**.
4. **Linking (4):** the matchmaker for code — takes all our `.o` files plus the
   *libraries* (OpenSSL, Threads) and resolves every reference ("this function
   is actually in that library over there"), producing one runnable
   executable.

This is exactly why C++ code needs a **header (`#pragma once`)** and a
**.cpp**: compiling one `.cpp` (called one **translation unit (a single piece
of source code fed to the compiler as one job)**) only needs to know *there
exists* a function `SHA1(...)` (declared in a header); the *actual* bytes live
in a library or another `.cpp` and get connected at link time. Planning in the
headers, doing in the `.cpp` files.

## Why CMake and not typing `g++` by hand?

Imagine doing stage 1–4 *by hand* for every file, every time:

```
g++ -std=c++17 -I include -c src/main.cpp -o build/main.o
g++ -std=c++17 -I include -c src/bencode/BencodeDecoder.cpp -o build/BencodeDecoder.o
g++ -std=c++17 -I include -c src/peer/PeerHandshake.cpp -o build/PeerHandshake.o   # ...and 5 more
g++ build/main.o build/BencodeDecoder.o ... -o peerflow -lssl -lcrypto -lpthread
```

Problems, in order of annoyance:

- **Boring and error-prone:** you must list every file, in every command.
- **Out of date fast:** the moment we add a file, every command changes.
- **No dependency tracking:** if one `.cpp` changes, nothing stops you from
  running only *some* build commands and producing a broken mix.

CMake fixes all three. It is a **build system (a program that decides what to
compile, in what order, with what flags, and runs the compiler for us)**. You
describe your project once in `CMakeLists.txt`, and CMake handles the rest —
including **incremental builds (recompiling only the files that changed, not
everything)**, which is the secret of fast builds.

In Java terms, the mapping is:

| Java ecosystem | This project |
|---|---|
| `pom.xml` (Maven) | `CMakeLists.txt` |
| Maven manages dependencies | CMake's `find_package` finds libraries |
| `mvn clean package` | `cmake --build build` |
| `java -jar target/app.jar` | `./build/peerflow` |
| `target/` (generated, git-ignored) | `build/` (generated, git-ignored) |

There is one honest difference: Java runs on a **JVM (a "virtual computer"
that executes bytecode)**, so it never translates to native machine code.
C++ compiles straight to native code — which is why picking the right build
tooling is a bigger deal in C++.

## The project layout (where things go, and why)

```
Torrent_Client/
├── CMakeLists.txt      # the build definition (the "pom.xml")
├── README.md           # the project's front door
├── docs/               # the guide you're reading
├── include/            # HEADERS (.hpp): the "plans" — what things look like
├── src/                # SOURCES (.cpp): the "implementation" — what things do
└── test/               # the Ubuntu .torrent we parse in Phase 2
```

Why split `include/` from `src/`? Because it's healthy **separation of
concerns (the principle of separating *what something promises* from *how it
delivers*)**:

- Anyone using our code can read the `.hpp` headers and understand the
  interface (like a public API contract).
- The messy details inside `.cpp` files can change without breaking anyone who
  only relied on the header's promises.

Think of it like a restaurant menu (`include/` — what's available) versus the
kitchen (`src/` — how each dish is actually made). Customers only need the
menu; chefs need the kitchen.

Within those folders, files are grouped by **feature (a module = a related,
cohesive set of files that handles one job)**:

```
include/            src/
├── bencode/   ←→   ├── bencode/    # Phase 1: speaking/reading bencode
├── torrent/   ←→   ├── torrent/    # Phase 2: reading .torrent files
├── tracker/   ←→   ├── tracker/    # Phase 3: talking to the tracker
├── net/       ←→   ├── net/        # shared network helper (Phases 3 & 4)
└── peer/      ←→   └── peer/       # Phase 4: talking to peers
```

Each `foo/` directory has a matching `.hpp` (plan) and `.cpp` (work) — a
convention you can rely on when navigating.

## What `CMakeLists.txt` actually says

Like reading a Maven `pom.xml`, line by line:

```cmake
cmake_minimum_required(VERSION 3.14)     # refuse to run on older CMakes
project(PeerFlow LANGUAGES CXX)          # project name = PeerFlow; it's C++
set(CMAKE_CXX_STANDARD 17)               # write code using the C++17 rules
set(CMAKE_CXX_STANDARD_REQUIRED ON)      # never silently fall back to older C++
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # also write compile_commands.json
                                         #   (IDE tools use this to understand the code)

find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)  # find the SSL library
find_package(Threads REQUIRED)           # find thread support

add_executable(peerflow                  # build a PROGRAM called "peerflow"
    src/main.cpp                         #   the test runner
    src/bencode/BencodeDecoder.cpp       #   Phase 1
    src/torrent/TorrentParser.cpp        #   Phase 2
    src/tracker/TrackerRequest.cpp       #   Phase 3
    src/tracker/HttpTracker.cpp          #   Phase 3
    src/net/TcpSocket.cpp                #   shared network helper
    src/peer/PeerHandshake.cpp           #   Phase 4
    src/peer/FakePeer.cpp                #   Phase 4 (test server)
)

target_include_directories(peerflow PRIVATE include)
                                   # "when compiling, also look HERE for
                                   #   #include "...." files"

target_link_libraries(peerflow PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
                                   # "when linking, attach these libraries"
```

Two words worth unpacking:

- **`PRIVATE`** means "this setting is for `peerflow` alone; it's not part of
  the contract I export to anything that reuses the target." If we later built
  a *library* other programs use, `PRIVATE` vs `PUBLIC` decides what leaks to
  them. For one executable, `PRIVATE` is the right instinct.
- **`find_package`** is CMake's "go find this library on this machine" — it
  locates OpenSSL's headers and libraries (often via the package config the
  OS's package manager installed) and exposes targets like `OpenSSL::SSL`.
  If OpenSSL isn't installed, this is the line that fails loudly at configure
  time — a friendly failure *before* any code compiles.

### What does linking actually link? (static vs. shared libraries)

C++ projects don't re-write common functionality. A **library (a pre-compiled
bundle of reusable code)** comes in two flavours:

- **Static library:** its code is *copied into* our executable at link time
  (`.a` files). Bigger file, but no dependency at runtime.
- **Shared library (a "shared object", `.so`):** only a *reference* is baked
  in; the real code is loaded from the OS when the program starts. Every
  program on the system can share one copy — that's why it's "shared". The
  `.so` must still be installed at runtime or the program won't start.

OpenSSL on Linux is typically shared — that's why running `peerflow` needs the
same machine to still *have* libssl. `Threads` is kernel-level threading
support (used by the `FakePeer` test server in Phase 4).

### Why C++17?

Modern C++ keeps evolving. C++17 (a specific, stable revision) gives us
conveniences the earlier code of this project exploits (`std::optional`,
`std::string_view`, and friends) while staying close enough to the metal that
*everything we do is visible*. In Java money: Java 17 — recent and stable.

The project also deliberately makes the compiler **strict**: we enable options
that turn common mistakes into loud errors, so bugs surface at compile time
instead of at 2 a.m. over a network.

## The two-step dance: configure, then build

The command line hides a nice division of labour:

```bash
# STEP 1 — CONFIGURE: CMake reads CMakeLists.txt ONCE and prepares build/
cmake -B build
```

CMake is a **generator of build recipes**, not a compiler itself. During
*configure* it:

- reads `CMakeLists.txt`,
- hunts for OpenSSL and threads (`find_package`),
- figures out your compiler and platform,
- writes a full recipe of exact `g++` commands into the `build/` folder.

It remembers this as a **CMake cache (a saved file of every setting, so
re-running configure doesn't re-ask everything)**.

```bash
# STEP 2 — BUILD: run the generated recipe
cmake --build build
```

Now `make` (CMake's chosen worker, the engine that "runs the recipe") takes
over: it checks timestamps, recompiles only the files that changed since the
last build (**incremental build — the speed-up that makes iterations
pleasant**), then links the executable. Result: `build/peerflow`.

```bash
# STEP 3 — RUN the tests
./build/peerflow
```

All three in one go:

```bash
cmake -B build && cmake --build build && ./build/peerflow
```

(`&&` = "only run the next if the previous succeeded" — if compilation fails,
the tests never run, which is exactly what you want.)

> `peerflow` is the **executable (a file the OS can run directly — the program
> itself)**. On Windows the same project would produce `peerflow.exe`; the
> difference is just the OS's file conventions.

## What a "clean rebuild" means (and how to do one)

Sometimes the world gets stale: old object files from a deleted source file,
cached assumptions from an earlier CMake version, or just you wanting
certainty. The nuclear option is to delete the entire generated folder and
start from scratch:

```bash
rm -rf build
cmake -B build
cmake --build build
```

Why is this safe? **`build/` contains only generated files — everything in it
can be recreated from source in seconds.** Deleting it is like clearing the
workspace, not losing the recipe. That's also why `build/` is git-ignored.

## Git and `.gitignore`: keeping the recipe, not the leftovers

If the project has a time machine, it's **Git (a version control system that
records every change to files, letting you rewind, compare, branch, and share
the project)**. We use it to checkpoint each finished phase.

But some files must *never* be committed, because they're either generated
(`build/`) or private (`*.torrent` test files). The **`.gitignore` (a list of
patterns telling Git "don't track these")** does that, the same way Maven's
`target/` is kept out of Java repositories:

```gitignore
build/      # compiled/generated folder
*.o         # object files
*.d         # dependency files
peerflow    # the executable itself (generated)
test/*.torrent   # downloaded test data (big, re-downloadable)
```

Rule of thumb: **commit source, never output.**

## What OpenSSL is doing here before we've written any networking

OpenSSL is our **cryptography library (a battle-tested bundle of algorithms
for hashing, encryption, and secure connections)**. Two future phases lean on
it:

- **Phase 2 needs SHA-1** to compute the torrent's **info hash** (the 20-byte
  fingerprint; Chapter 04). We call OpenSSL's `SHA1()`.
- **Phase 3 needs TLS** to speak `https://` to real trackers (Chapter 05).

Writing your own cryptography is a **serious anti-pattern** — like carving a
bank lock from wood instead of buying a certified one. The correctness burden
of cryptography is enormous, so we use the industry-standard, heavily-audited
library. This is *not* cheating: real clients depend on the same library.

## Testing philosophy: PASS and FAIL, no mystery frameworks

The cleverest part of `main.cpp` is that it's both our program *and* its own
test runner. It follows a deliberately humble pattern:

- every check prints a line starting with `PASS:` or `FAIL:`,
- counters at the end (`passed` / `failed`) tally the result,
- the program exits with code `0` on full success (and non-zero if any test
  failed — the exit code computers can check).

```
PASS: "i42e" -> 42
PASS: dictionary -> {cow=moo, spam=42}
...
=== Results ===
Passed: 20
Failed: 0
```

Why hand-rolled instead of a framework like Google Test? Two reasons:

1. **Radical transparency:** with no framework, every line is ours and there is
   no magic — the documented philosophy of this project.
2. **Fewer moving parts:** a framework is a dependency you must learn, install,
   and trust. Our tests are as simple as printing a line.

The catch is on us: hand-rolled tests are only as good as the checks we write.
That's exactly why each chapter points at *which* tests prove *what* — so
`20/20` isn't a number, it's twenty specific guarantees.

## Prerequisites in this room (this machine, at the time of writing)

For completeness, the environment this project was built on:

| Tool | Version on this machine |
|---|---|
| Compiler | `g++ 9.4.0` |
| CMake | `3.16.3` |
| OpenSSL | `1.1.1f` |
| OS | Linux (Ubuntu-based) |

On most Linux systems the packages to install are `g++`, `cmake`,
`libssl-dev`, and `libcurl`… is *not* needed — we wrote our own HTTP (Chapter
05). Point being: **the build system shields us from these versions**; you only
need A C++17 compiler, CMake ≥ 3.14, and OpenSSL.

## The checklist — how Phase 0 earns its "done"

- ✅ `cmake -B build` configures without errors
- ✅ `cmake --build build` compiles cleanly (no warnings we accept quietly)
- ✅ `./build/peerflow` prints `Passed: 20`, `Failed: 0`
- ✅ source is in Git; generated files in `.gitignore`

Everything from here on rides on this foundation being boring and reliable —
so we can spend our attention on bencode bytes (Phase 1) instead of build
drama.

---

*Next: [03 — Phase 1: Bencode decoder](03-phase1-bencode.md)*