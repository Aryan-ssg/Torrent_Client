# PeerFlow — Learning Guide

> **Who this is for**
>
> You know Java. You are new to C++ and new to networking. You learn best
> when one idea is introduced at a time, explained in plain words, with
> small examples.
>
> **This guide is written for exactly that.** It will not assume you already
> know about sockets, build systems, threads, or the C++ memory model. If a
> concept isn't needed yet, it isn't here — you'll be told when we reach it.

---

## Start here

```bash
cmake -B build
cmake --build build
./build/peerflow --help
```

Those three lines build the program. [Module 0](00-how-the-build-works.md)
explains what each one actually does.

Then read, in this order:

1. [Module 0 — How the project is built](00-how-the-build-works.md) *(start here)*
1. [Module 1 — C++ basics you need](01-cpp-basics.md) *(written)*

Modules 2–12 aren't written yet. The table above lists what each one will
cover so you can see where you're heading.

---

## The 12 modules

Each module builds on the last. Don't skip ahead — later modules assume the
earlier ones.

| # | Module | You will be able to |
|---|---|---|
| 0 | [How the project is built](00-how-the-build-works.md) | Explain what `cmake -B build` and `cmake --build build` do, and what the files in `build/` are |
| 1 | [C++ basics you need](01-cpp-basics.md) *(written)* | Read the project's C++: types, references, `const`, `struct` vs `class`, enums, ownership |
| 2 | [Bytes and binary data](02-bytes-and-binary.md) | Understand how a number becomes bytes on the wire, and what "byte order" means |
| 3 | [`.torrent` files and bencode](03-torrent-and-bencode.md) | Explain what a `.torrent` file contains and how the code reads one |
| 4 | [Networking basics](04-networking-basics.md) | Understand IP addresses, ports, TCP, and sockets — from the ground up |
| 5 | [Trackers](05-trackers.md) | Explain what an "announce" is and how the code asks a tracker for peers |
| 6 | [Talking to peers](06-peers.md) | Explain the 68-byte handshake and the messages two peers exchange |
| 7 | [Pieces and blocks](07-pieces-and-blocks.md) | Explain how a file is split, requested, and checked for correctness |
| 8 | [Writing to disk](08-writing-to-disk.md) | Explain how verified pieces become a real file, and how downloads resume |
| 9 | [Working at the same time](09-concurrency.md) | Explain why the download uses threads, and what a "race condition" is |
| 10 | [How the code is organised](10-architecture.md) | Explain why the project is split into three build targets |
| 11 | [Debugging](11-debugging.md) | Use `gdb`, `strace`, and logging to investigate a problem |
| 12 | [Testing and real bugs](12-testing-and-bugs.md) | Explain how the project tests itself, and how bugs were found |

---

## How each module is written

Important ideas are explained with this pattern:

1. **What is it?** — in plain words
2. **Why do we need it?** — the problem it solves
3. **A tiny example** — the smallest thing that shows the idea
4. **How this project uses it** — with real file names and line numbers
5. **The one-line mental model** — something to remember

**Labels tell you how much attention to give something:**

| Label | Meaning |
|---|---|
| 🟢 **MUST KNOW NOW** | You'll need this to read the code. Explained properly. |
| 🟡 **GOOD TO KNOW** | Helpful context. Skim it. |
| 🔵 **LATER** | Not needed yet. It's noted so you don't worry about it. |

Things marked 🔵 include: `TOCTOU` (Module 9), memory ordering (Module 9),
`epoll` (Module 4), ABI and ELF internals (Module 0), and advanced CMake
generator behaviour (Module 0). You do not need them now.

**Tiny checks.** After an important idea you'll find a short question like
this:

> **Check your understanding**
>
> If `main.cpp` changes, do we need to recompile `peer.cpp`?
>
> <details><summary>Answer</summary>
>
> No. `main.cpp` and `peer.cpp` are compiled separately into separate object
> files. Changing one doesn't change the other, so only the one you touched
> gets recompiled. This is why builds are fast.
>
> </details>

Fold the answer yourself, or just scroll. They're there to check yourself, not
to test you.

---

## Two important things about this project

**Everything in a `.torrent` file is untrusted.** The file name, the tracker
addresses, the piece count — all of it was chosen by a stranger who published
it. The code checks these things rather than trusting them. You'll see this
constantly, and it explains a lot of otherwise-odd-looking code.

**The code compares against the original Java-friendly explanations.** The
project was written with Java readers in mind, so many files already contain
"in Java this is…" notes. Those are useful, but they sometimes skip steps.
This guide fills the gaps.

---

## A note on terms

Some words in the code are ordinary English but are not used that way:

| Word | What it means here |
|---|---|
| **peer** | another computer downloading the same file |
| **tracker** | a server that tells you which peers exist |
| **swarm** | all the peers downloading one file |
| **piece** | a fixed-size chunk of the file (the unit of checking) |
| **block** | a smaller chunk of a piece (the unit of transfer) |
| **seeder** | a peer that already has the whole file |
| **announce** | the message that tells a tracker you exist |
| **info hash** | a 20-byte fingerprint identifying one torrent |

We introduce each properly when we reach it. You don't need to memorise this
table.

---

*[Back to the main README](../README.md)*
