# Module 0 — How the Project Is Built

> **Goal of this module:** you will be able to explain what happens when you
> type `cmake -B build && cmake --build build`, and what all the files in
> `build/` are for.
>
> That's it. We are not looking at any of the project's actual logic yet.

---

## The problem: turning text into a running program

Your computer cannot run `src/main.cpp`. It's just text. A CPU cannot read
English; it can only execute machine instructions — low-level commands like
"add these two numbers" or "jump to this memory location."

So something has to **translate** your text into those instructions. That
something is a **compiler**.

```
src/main.cpp          what you write        (text a human reads)
      │
      │  compiler
      ↓
main.o                machine instructions for THIS file only
      │
      │  linker
      ↓
peerflow              a program your OS can run
```

**🟢 MUST KNOW NOW — two separate steps, two different tools.**

The **compiler** works on **one file at a time**. It cannot combine files.
Then the **linker** takes all the compiled pieces and joins them into one
program.

This split matters, and it's the reason builds are fast. We'll come back to
it.

### The cooking analogy

| Cooking | Building |
|---|---|
| Ingredients | your `.cpp` and `.hpp` source files |
| The recipe | `CMakeLists.txt` — says what's needed |
| Detailed step-by-step instructions | the `Makefile` |
| Actually cooking | running `make` |
| The cooking itself | compiler, then linker |
| The finished dish | `build/peerflow` |

Notice what the analogy makes clear: **writing the recipe is not the same as
cooking.** You write `CMakeLists.txt` by hand, but the cooking is done for
you, and you can change the recipe and re-cook without rewriting it.

---

## 🟢 MUST KNOW NOW — source files (`.cpp`)

A **source file** contains actual instructions. It has code in it.

`src/main.cpp` has 31 lines of comment at the top explaining what the program
is for, and then the includes begin:

```cpp
#include "peer/ConcurrentDownloader.hpp"
#include "torrent/TorrentFile.hpp"
#include "torrent/TorrentParser.hpp"
// ... more includes

int main(int argc, char** argv) {
    // ... the program
}
```

**Java parallel:** this is a `.java` file. A `.cpp` file is compiled
separately into its own binary chunk; a `.java` file is not.

There are **20** source files in this project. That's the count of `.o` files
in `build/`:

```
build/CMakeFiles/peerflow_core.dir/src/bencode/BencodeDecoder.cpp.o
build/CMakeFiles/peerflow_core.dir/src/net/TcpSocket.cpp.o
build/CMakeFiles/peerflow_core.dir/src/piece/PieceManager.cpp.o
... 20 in total
```

Each `.cpp` produces exactly one `.o`.

---

## 🟢 MUST KNOW NOW — header files (`.hpp`)

A **header file** does *not* contain the instructions. It **describes** what
something looks like — like a table of contents, or an interface.

Compare the two halves of one piece of the project:

`include/torrent/TorrentFile.hpp` — the description:

```cpp
struct TorrentFile {
    std::string announce;
    std::string name;
    long long pieceLength = 0;
    long long length = 0;
    std::vector<uint8_t> pieces;
    std::vector<uint8_t> infoHash;
};
```

`src/torrent/TorrentParser.cpp` — the actual work:

```cpp
#include "torrent/TorrentFile.hpp"
// ...
TorrentFile TorrentParser::parse(const std::string& filepath) {
    // hundreds of lines that read a file and fill in a TorrentFile
}
```

The header says *a `TorrentFile` has these fields*. The `.cpp` says *how to
build one*.

**Why bother splitting them?** Because other files need to know the shape
without re-reading your implementation. `main.cpp` needs to know that
`TorrentFile` has a `.name` field. It shouldn't have to read the 314 lines of
`TorrentParser.cpp` to learn that.

**🟡 GOOD TO KNOW — but headers aren't purely declarations.** Five of this
project's 25 headers do contain code, all of it tiny. From
`include/peer/Bitfield.hpp`:

```cpp
size_t pieceCount() const { return pieceCount_; }
size_t byteSize()    const { return bytes_.size(); }
bool    complete()   const { return pieceCount_ > 0 && count() == pieceCount_; }
```

Those one-line functions are called **accessors** — they hand a field back to
whoever asks for it. They live in the header because there is no reason to put
a one-liner somewhere else.

> **🎯 The accurate rule:** the header makes a type *visible* and usually
> *declares* it. Sometimes it also defines small things. The substantial work
> is in the `.cpp`.

**This is exactly why `#pragma once` matters.** If a header defines code and
gets pasted into a file twice, the compiler sees the same function defined
twice and complains. Declaration-only headers would be harmless; definition-
carrying headers are not.

> ### **Java parallel**
>
> Similar to an interface, or a public class signature. But C++ takes it
> further: the header is the *only* thing another file sees. If a function
> isn't declared in the header, other code can't call it.
>
> **The important difference:** in Java, `public` and `private` are decided in
> one file. In C++, you literally have two files, and **which one you put
> something in decides whether it's visible to other code.**

### How a file includes a header

At the top of every `.cpp`:

```cpp
#include "torrent/TorrentFile.hpp"
```

> ### **The one thing to understand about this line**
>
> Look at the path: `torrent/TorrentFile.hpp`. It starts at the `include/`
> folder and does **not** contain `include/` or `..` or `../..`.
>
> That's because CMake told the compiler *"when you see an `#include`, look
> inside `include/` for it."*
>
> So `torrent/TorrentFile.hpp` means:
> **`include/`** + `torrent/` + `TorrentFile.hpp`
>
> If that instruction weren't there, the file would have to be written
> `"../../include/torrent/TorrentFile.hpp"` — and that relative path would
> break the moment the file moved. **CMake's one instruction buys clean
> includes everywhere.** We return to this in Module 10.

### The first line of every header

Every header starts with:

```cpp
#pragma once
```

**🟢 MUST KNOW NOW.** This says *"only paste my contents into this file once,
no matter how many times I'm included."*

Why does that matter here? Because **five of this project's headers define
code**, not just declare it. Without `#pragma once`, a file that included
`Bitfield.hpp` twice — directly and indirectly by way of two other headers —
would get `size_t pieceCount() const { ... }` pasted in twice, and the
compiler would reject the duplicate definition.

> **Java parallel:** Java's `import` already does this for you, automatically.
> C++ has no import system, so each header defends itself. `#pragma once` is
> the most common way. (`#ifndef`/`#define` guards are the older way — you
> will meet them in older C++ code. This project uses `#pragma once` in all 25
> headers.)

---

## 🟢 MUST KNOW NOW — the object file (`.o`)

The compiler's output for one file is an **object file**, extension `.o`.

It contains the machine instructions for that one file, plus a list of which
external names it needs (functions and variables defined elsewhere).

**Java parallel:** loose analogy — it's the closest thing to a `.class` file,
except a `.class` is fully linked and directly runnable by the JVM, while an
`.o` is half-finished and not runnable at all.

**🟡 GOOD TO KNOW.** The `.o` is *not* a program. You cannot run it. It's
missing the other 19 files.

---

## 🟢 MUST NOW — the linker

The **linker** takes all the `.o` files and:
- joins them together
- resolves the "I need this, someone else defines it" lists
- produces one runnable file

Output here:

```
build/peerflow          a real runnable program
```

> ### **WITHOUT vs WITH**
>
> ```
> WITHOUT a linker
>   You would have 20 unrunnable .o files. The operating system cannot start
>   any of them. Nothing works.
>
> WITH a linker
>   One command produces one program. This is the step you were already
>   trusting without knowing — every language you use has one. In Java it
>   happens invisibly: javac produces .class files and the JVM links them
>   at startup. You have probably never thought about it.
>
> THE RULE  the compiler works per-file; the linker joins the files. Both
>           steps are required, always, for every C++ program.
> ```

---

## 🟢 MUST KNOW NOW — the build system

You've now met every moving part. Here's the problem they solve.

**Imagine doing it by hand.** To build this project you'd type 20 compiler
commands, each with the right include path, then a linker command listing all
20 object files plus OpenSSL. And when you add a new `.cpp`, you'd have to
redo the whole thing.

**A build system automates that.** You describe what you want; it figures out
the commands.

```
CMakeLists.txt          you write this
      │
      │  cmake -B build
      ↓
Makefile                generated — the actual instructions
      │
      │  make
      ↓
compiler → linker       runs automatically
      ↓
build/peerflow          the program
```

**Two tools, two jobs:**

| Tool | Job |
|---|---|
| **CMake** | Reads `CMakeLists.txt`, writes a `Makefile` |
| **make** | Reads the `Makefile`, runs the commands in it |

**Java parallel:** this is roughly the relationship between **Maven**
(describes the project, resolves dependencies, runs plugins) and **javac**
(the thing that actually compiles). `mvn package` runs `javac` for you —
you never type it directly. Here, `cmake` produces the equivalent of a build
script and `make` executes it.

### The two commands, precisely

```bash
cmake -B build
```

- **Step 1 — "prepare."** CMake asks your computer some questions (*which
  compiler is installed? where is OpenSSL? does this machine support C++17?*)
  and writes the answers into `build/`, most importantly the `Makefile`.
  **This step compiles nothing.**
- `-B build` means "put the generated files in a folder called `build`."

```bash
cmake --build build
```

- **Step 2 — "go."** This reads the `Makefile` and actually runs the compiler
  and linker.

**🎯 Mental model:**

> `cmake -B build` = write the recipe
> `cmake --build build` = cook it

---

## 🟢 MUST KNOW NOW — the build folder

`build/` is a directory that **did not exist in the source**. Everything in it
is *generated*. You never type inside it.

```
build/
├── CMakeCache.txt     the answers to CMake's questions
├── Makefile           the generated instructions
├── compile_commands.json   every compile command, recorded
├── libpeerflow_core.a  the middle layer, as one big file
├── peerflow            the program you run
├── peerflow-tests      the test program
├── CMakeFiles/.../    the 20 .o files, buried
└── phase6-*.bin       scratch files the test suite left behind
```

> ### **The `.bin` files**
>
> After you run the tests, `build/` fills up with files like `phase6-out.bin`.
> Those are the test suite's scratch files. `ls build/` will show you more than
> the list above — that is normal, and nothing is wrong.
>
> It is the clearest possible demonstration that `build/` is disposable.
> **🎯 Mental model:** `build/` is a scratch space. `rm -rf build` and it comes
> back correct. It is disposable.

> ### **WITHOUT vs WITH**
>
> ```
> What if you edited build/Makefile by hand?
>   It works — until you type `cmake -B build` again, which overwrites your
>   edit, and the change silently disappears. This is a genuinely confusing
>   afternoon.
>
> The rule  edit CMakeLists.txt, never build/. If you want to know the
>          project's real build settings, CMakeLists.txt is the source of
>          truth and build/ is the scratch paper.
> ```

**🔵 LATER.** CMake can also generate Ninja files instead of a Makefile, and
has many other options. You only need to know that a build system sits between
your description and the compiler. Nothing more.

---

## 🟢 MUST KNOW NOW — this project's three outputs

`CMakeLists.txt` describes **three** separate things to build.

**1. `libpeerflow_core.a` — a static library**

The actual engine. All 17 of the project's `.cpp` files, in one file.

**2. `peerflow` — the program**

```
build/peerflow      920 KB, one runnable program
```

Run it: `./build/peerflow some-file.torrent`

**3. `peerflow-tests` — the test program**

```
build/peerflow-tests    748 KB
```

Run it: `./build/peerflow-tests` — this runs the project's 38 checks.

**Why three instead of one?** Because the code that *does the work* should not
be tangled up with the code that *does the talking*.

> ### **WITHOUT vs WITH**
>
> ```
> One program (everything mashed together):
>   Change one line deep inside. Recompile and relink everything.
>   Worse, the tests are tangled in with the program, so building the
>   program also drags in 1,500 lines of test code, OpenSSL test setup,
>   and fake servers.
>
> A library + two thin programs:
>   The engine is built once and stored (libpeerflow_core.a).
>   peerflow and peerflow-tests are just small wrappers that use it.
>   Change something in the engine → rebuild the engine, relink two thin
>   programs. Much less wasted work, and the tests can be run
>   independently.
> ```

We look at *how* CMake declares these in Module 10. For now, just know they
exist and why.

---

## 🟢 MUST KNOW NOW — where the files live

```
Torrent_Client/
├── CMakeLists.txt      ← the recipe
├── include/            ← headers: "here's what things look like"
│   ├── bencode/
│   ├── torrent/
│   ├── tracker/
│   ├── net/
│   ├── peer/
│   ├── piece/
│   ├── ui/
│   ├── util/
│   └── tests/
├── src/                ← source: "here's how things work"
│   ├── main.cpp        the entry point
│   ├── tests/          the test suite (2 files, ~1,500 lines)
│   ├── bencode/
│   ├── torrent/
│   └── ... (the same folders as include/, plus main.cpp)
├── test/               a .torrent file for trying things out
└── build/              ← generated; do not edit
```

> ### **`src/tests/` vs `test/` — easy to mix up**
>
> These two are unrelated, which is unfortunate:
>
> | | |
> |---|---|
> | `src/tests/` | **The test code.** Two `.cpp` files. This is what `peerflow-tests` is built from. |
> | `test/` | **Sample data.** Holds a `.torrent` file. No code at all. |
>
> Both are covered properly in Module 12. For now, just remember the test
> *code* is in `src/`, next to everything else.

**🎯 Mental model:** most `.hpp` files in `include/` have a matching `.cpp` in
`src/` with the same path.

```
include/peer/PeerSession.hpp   ← describes PeerSession
src/peer/PeerSession.cpp       ← implements PeerSession
```

**🟡 GOOD TO KNOW — but 7 of the 25 headers have no `.cpp` at all.**

If you go looking for `src/net/NetException.cpp`, it isn't there. That's not a
mistake. Those 7 files are:

```
torrent/TorrentFile.hpp          struct TorrentFile
net/NetException.hpp             class NetException
tracker/Peer.hpp                 struct Peer
tracker/TrackerResponse.hpp      struct TrackerResponse
piece/FileException.hpp          class FileException
bencode/BencodeValue.hpp         class BencodeValue
bencode/BencodeException.hpp     class BencodeException
```

They're all small — 18 to 43 lines — and all are either a **plain data
holder** (`struct TorrentFile`, `struct Peer`) or an **exception type**.

> ### **WITHOUT vs WITH**
>
> ```
> A header that DEFINES something big
>   Needs a .cpp, or the code gets compiled separately into every file that
>   includes it. Duplicated work, and a real risk of the classic
>   "multiple definition" linker error.
>
> A header that just DESCRIBES something
>   No code lives there, so no .cpp is needed. This is the common case, and
>   it is why you will find .hpp files with no partner.
>
> The rule  if you open a .hpp and find actual function bodies in it, expect
>          there is no matching .cpp. If you find only field names and
>          declarations, go look for the .cpp.
> ```

**The rule:** if you open a header and find real function bodies in it, the
implementation is right there. If you find only field names and declarations,
go look for the `.cpp`.

This is the one place where "every header has a source file" is false, so it
is worth checking by hand the first time you go looking for something.

---

## 🟡 GOOD TO KNOW — why incremental builds are fast

The compiler works on **one file at a time**. The linker joins the results.

This means: **if you change one file, only that one file needs recompiling.**
The other 19 `.o` files are already correct.

We measured this on your machine. You can reproduce it:

```bash
touch src/main.cpp                          && cmake --build build   # ~2 s
touch include/torrent/TorrentFile.hpp       && cmake --build build   # ~15 s
cmake --build build                                          # ~0.2 s
```

| You changed | What got rebuilt | Time |
|---|---|---|
| nothing | nothing | 0.2 s |
| `src/main.cpp` (1 file) | 1 file + relink | 2.1 s |
| `include/torrent/TorrentFile.hpp` (32 lines) | **7 files** + relink 3 things | 15.0 s |

The last row is interesting: that header has no instructions in it at all, yet
editing it forced 7 files to recompile. Why? Because 7 `.cpp` files
`#include` that header, and a `.cpp` must be recompiled if anything it
includes changes.

> **🎯 Mental model:** editing a header = editing many files. Headers are the
> shared vocabulary of the project, so changing the vocabulary means many
> people need to re-check their sentences.

**🔵 LATER.** People call this "header coupling" or "recompilation
bloat." It's the same thing. You don't need the term.

---

## 🟢 MUST KNOW NOW — compiling by hand, just to see

You can always skip the build system. The file `build/compile_commands.json`
records the **exact** command CMake ran for each file. For `main.cpp` it was:

```bash
/usr/bin/c++ -I/home/.../Torrent_Client/include -std=gnu++17 \
    -o CMakeFiles/peerflow.dir/src/main.cpp.o \
    -c /home/.../Torrent_Client/src/main.cpp
```

(The full paths are long; I've shortened them with `...` for readability. Run
`python3 -m json.tool build/compile_commands.json | head` if you want to see
them unabbreviated.)

Read it as: *"Use the C++ compiler, look for headers in `include/`, use the
2017 standard, and compile just this one file."*

Now you can see that `-I.../include` is the single instruction that makes all
those clean `#include` lines work.

**🔵 LATER.** The `-o` path is nested inside `CMakeFiles/`, not in the root of
`build/`. That's why a manual `ls build/*.o` finds nothing — the `.o` files are
buried. Use `find build -name '*.o'` if you want to see them.

---

## Your turn

Try these in order:

```bash
cmake -B build
cmake --build build
./build/peerflow --help
```

Then look at what appeared:

```bash
ls build/
cat build/Makefile | head -20
```

---

## Check your understanding

**1.** You change one line in `src/main.cpp` and rebuild. How many `.cpp`
files get recompiled?

<details><summary>Answer</summary>

One — `main.cpp` itself. The other 19 object files are already correct, and
the compiler works per-file. (It then relinks, which is fast.)

</details>

**2.** `include/torrent/TorrentFile.hpp` and `src/torrent/TorrentParser.cpp`
are a pair. Which one contains the actual instructions?

<details><summary>Answer</summary>

The `.cpp`. `TorrentFile.hpp` only declares what a `TorrentFile` looks
like — its field names and types. `TorrentParser.cpp` is 314 lines that
actually read a file and build one.

(One wrinkle worth knowing: this project's headers aren't purely
declarations. Five of them define small things inline, like a one-line
accessor. So the rule is "the real work is in the `.cpp`", not "headers
never contain code".)

</details>

**3.** Your friend says "I compiled it and it worked." What probably happened
that they didn't mention?

<details><summary>Answer</summary>

Two things: the compiler turned 20 `.cpp` files into 20 `.o` files, and
then the *linker* joined those 20 pieces into one runnable program. Only
step two produces something you can actually run.

</details>

**4.** `cmake -B build` is run. Have you compiled any code yet?

<details><summary>Answer</summary>

No. That step only writes a `Makefile` into `build/`. The compiling happens
when you run `cmake --build build` (or `make`).

</details>

**5.** Why is `#pragma once` at the top of every header?

<details><summary>Answer</summary>

To stop a header's contents being pasted in twice if two other headers both
include it. Without it, the compiler would see the same struct defined twice
and complain. (`#pragma once` is the short modern way; older code uses
`#ifndef`/`#define`.)

</details>

---

## What's next

You now know how the project *builds*. You still don't know any of its logic —
and that's fine, that comes next.

**[Module 1 — C++ Basics You Need →](01-cpp-basics.md)**

That module covers the language itself: what types mean, what `&` does, what a
`class` is, and what `const` means — all explained against Java, which you
already know.

---

*[Back to the Learning Guide](README.md)*
