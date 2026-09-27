# 04 — Phase 2: The Torrent Parser and the Info Hash

## What we built and why

A `.torrent` file is a bencoded dictionary (Phase 1's gift to us). Phase 2
reads that file and pulls the specific facts our client needs into a neat
object called **`TorrentFile` (a data-holder with typed, named fields — the
C++-struct equivalent of a Java POJO/record)**:

```cpp
struct TorrentFile {
    std::string announce;             // where the tracker lives (URL)
    std::string name;                 // file name, e.g. "ubuntu-....iso"
    long long pieceLength;            // size of one piece, in bytes
    long long length;                 // total file size, in bytes
    std::vector<uint8_t> pieces;      // all piece hashes glued together
    std::vector<uint8_t> infoHash;    // the 20-byte fingerprint of the torrent
};
```

Why this struct and not a bag of loose variables? Because it is a **value type
(a plain, copyable bundle of data with no hidden behaviour — exactly like
carrying a filled-out form from office to office)**. Every later phase asks for
"a torrent", and handing them one `TorrentFile` (instead of ten arguments)
keeps every call site simple.

Two notes even in this tiny struct:

- `std::string` for text fields (`announce`, `name`) — these are human-readable
  text, so text types are fine.
- `std::vector<uint8_t>` for binary fields (`pieces`, `infoHash`) — these are
  raw bytes (hashes can contain any byte, including zero), so we use byte
  vectors, the *bytes vs text* rule from Phase 1 applied one more time.

The **critical milestone** of the phase is the **info hash (a 20-byte
fingerprint that uniquely identifies this torrent everywhere in the BitTorrent
world)**. Every future phase needs it, and getting it wrong means we'd be
downloading the *wrong swarm* of peers without anyone noticing — the worst
kind of bug: silent and socially awkward.

## Anatomy of a `.torrent` file

The whole file is one big bencoded dictionary. Rendered as a "JSON-ish" view:

```
d                                        # top-level dictionary begins
  8:announce 41:https://.../announce    # "announce" → tracker URL (Phase 3)
  4:info     d ... e                     # "info"    → THE most important part
  ...                                    # (a torrent may hold a few more keys)
e                                        # top-level dictionary ends
```

The keys we care about right now:

| Key | Meaning | Used in |
|---|---|---|
| `announce` | the tracker's URL | Phase 3 |
| **`info`** | a **nested dictionary** holding everything about the *file itself* | Phases 2, 5, 6 |

The star is `info`: it is the *heart* of the torrent. Everything *outside*
`info` (like `announce`) is advisory metadata; everything *inside* `info`
defines the actual file, its pieces, and — as we'll see — the torrent's very
identity.

### The info dictionary (single-file torrent)

For a torrent that contains **one file (our parser's current scope; the Ubuntu
ISO is one file)**, the fields we extract are:

| Info key | Meaning | Example (Ubuntu test) |
|---|---|---|
| `name` | the file's name | `ubuntu-24.04.1-desktop-amd64.iso` |
| `piece length` | how big each piece is, in bytes | 262144 (256 KB) |
| `length` | total size of the file, in bytes | 6203355136 (≈ 6.2 GB) |
| `pieces` | all piece SHA-1 hashes, concatenated end-to-end | 473,280 bytes |

There exists a *multi-file* variant (a `files` **list** of `{path, length}`
dictionaries instead of a single `length`) — used by e.g. "disc image"
torrents from a directory. We note it honestly as a later-phase extension; our
parser handles the single-file form, which is what the Ubuntu torrent is.

## Pieces: the file cut into chunks

The whole reason torrents work. The file is chopped into equal chunks called
**pieces (the atomic unit of download, verification, and exchange)**:

```
[ piece 0 ][ piece 1 ][ piece 2 ] ... [ piece N-1 ]
   256 KB     256 KB     256 KB      (the last one may be smaller)
```

For *each* piece, the torrent stores its **SHA-1 hash (a 20-byte fingerprint
of that piece's data)** — all of them joined end-to-end into one long `pieces`
string:

```
pieces =  hash(p0)  hash(p1)  hash(p2)   ... 
          └─20 b─┘  └─20 b─┘  └─20 b─┘
```

Counting pieces is therefore one line: **`pieces.size() / 20`**.

Why *pieces* at all? Three reasons, in order of importance:

1. **Verification (trust).** After downloading a piece, we hash what we got and
   compare with the stored hash. Mismatch → that peer sent garbage; try another
   peer. (Phases 5–6.)
2. **Parallelism (speed).** Piece #2 can come from neighbour A while piece #9
   comes from neighbour B — downloading *from many people at once* is the
   headline feature of BitTorrent.
3. **Fairness (economics).** Pieces are the currency: "you give me a piece, I
   give you a piece." Trading whole files would be lopsided; pieces make
   exchange even.

### The trade-off hidden in `piece length`

Smaller pieces → finer granularity (less waste when a peer has only a bit) but
more overhead (more hashes to store, more messages, more verification
passes). Larger pieces → less overhead but coarser (a "only need 100 KB"
request forces grabbing a 256 KB unit). Real torrents settle in the 256 KB –
4 MB range; Ubuntu's 262144 (256 KB) is a typical sweet spot.

### The Ubuntu test file, by the numbers

```
piece length:   262144 bytes           (256 KB)
total size:     6,203,355,136 bytes    (≈ 6.2 GB)
num pieces:     6,203,355,136 ÷ 262,144 = 23,664
pieces bytes:   23,664 × 20 = 473,280 bytes
```

Which explains a lovely sanity check: the real `.torrent` file is ~473 KB —
**the bulk of it is just the `pieces` string.** (A neat confirmation that the
number of pieces we *compute* matches the size of the `pieces` field the file
*carries*.)

## Hashing, in one short lesson

**Hashing (a function that takes data of ANY size and produces a fixed-size
"fingerprint")**, specifically SHA-1:

```
SHA-1("hello")  →  aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d   (40 hex chars = 20 bytes)
SHA-1("hellp")  →  d4baaad7a68a379b1e133e0fea0603051b0124ca
```

Change **one byte** → completely different fingerprint (sha1 of `hellp` shares
*no* visible pattern with `hello`). That's the **avalanche effect (the
property that a tiny change in the input produces a wildly different hash)**.

Three properties make hashes useful, and worth stating precisely:

- **Fixed-size.** Any length in → always 20 bytes out. (Compare: an *encoder*
  of variable size.)
- **One-way.** You cannot recover the input from the hash. A fingerprint can
  prove "I saw this", never "this is what it looked like".
- **Collision-resistant in practice.** Two *different* inputs giving the *same*
  hash should be astronomically unlikely.

> Distinguish hash vs. encryption (a common muddle): **encryption is
> reversible** (with the key you get the data back); **hashing is
> one-way**. A good analogy: an envelope's *wax seal* fingerprints the letter,
> but you can't reconstruct the letter from the seal.

BitTorrent uses SHA-1 in two places, both this phase onwards:

- the **info hash** (this chapter) — the torrent's identity, compared by every
  client; and
- **piece verification** (Phase 5) — trusting downloaded data.

And we use **OpenSSL's battle-tested `SHA1()`** rather than rolling our own —
same anti-pattern warning as the build chapter: crypto is for experts to write,
everyone else to *call*. (One honest technical note: SHA-1 is no longer
considered secure for *signatures* against a determined adversary, and the
BitTorrent community has been discussing SHA-256 as a future `v2` extension.
But for its two uses here — matching a peer-computed fingerprint and
verifying fixed-length chunks — it remains the interoperable standard. Every
real client in the world still uses it, so *we* must too.)

## The star of the phase: the info hash, and the "don't re-encode" trick

> **The info hash = `SHA-1` of the *exact raw bytes* of the `info`
> dictionary — the bytes exactly as they appear in the file.**

Why "exact raw bytes", so emphatically? Because *every peer on the internet*
computes the torrent's identity by hashing the raw `info` bytes **as they
appear in the original file**. If instead we *decoded* the bencode and then
*re-encoded* it before hashing, any difference — a reordered key, a different
integer spelling, a changed whitespace — would produce a *different hash*,
and we'd be looking for a different swarm than everyone else. Fatal.

The picture (with the info dict opened up, real-ish):

```
[.. announce ..][ 4:info ][ d ... 4:pieces ... e ][ ...other keys... ][e]
                                    ^^^^^^^^^^^^
                                    SHA-1 hashes THIS EXACT byte range
```

The consequences of the rule are *concrete*, not ceremonial. Compute the info
hash the wrong way and you join a swarm that literally does not exist (or
worse, someone else's). This is the one line of this phase the whole protocol
quietly depends on.

### The trap: a naive "find `4:info`" search is wrong

The obvious shortcut — scan for the text bytes `4:info` — has a subtle bug:
**that byte pattern can legally appear inside a string value.** Suppose a
torrent contained a top-level `comment` string `"4:infoxyz"`. A naive scan
would match *inside* that string and return a position that is *not* the start
of the real info value — hashing garbage, silently, and only sometimes.

So we **walk the bencode structure properly** over the raw bytes, using three
helpers that deliberately never touch our decoded objects:

### Helper 1 — `findInfoValueStart(raw)`: walk the top-level dict

`findInfoValueStart` reads the top-level dictionary key-by-key the way Phase 1
did, but **in raw byte space** and *without decoding* anything expensive:

- skip the opening `d`;
- loop until the closing `e`:
  - read the key's length digits up to `:` (keys are always strings);
  - if the key is literally `i` `n` `f` `o` (length 4, those four bytes) →
    return the position just after it, i.e. **the first byte of the `info`
    value** (the `d` that starts the info dictionary);
  - otherwise skip the key's bytes and **skip the whole value** via
    `skipBencodeValue` (below) and continue.

### Helper 2 — `skipBencodeValue(raw, pos)`: leap over an entire value

A small, recursive function that, from *any* value's first byte, returns the
position just past its end:

```
'b' == 'i'        → integers: advance to the closing 'e'
'b' == 'd' | 'l'  → containers: advance past opening, then skip each
                    element in turn (recursion), then past the 'e'
'0'..'9'          → strings: read length, advance length bytes (position
                    past the bytes)
anything else     → throw (not a legal value)
```

Notice it is **Phase 1's dispatcher again**, in miniature. Recursion shows up
once more: a container inside a container is skipped by calling itself.

### Helper 3 — `findDictEnd(raw, dictStart)`: find the info dict's closing `e`

Given the `d` that opens the *info* dictionary, find its *matching* closing
`e` by depth counting:

```
int depth = 0
walk forward:
  'd' or 'l'  → depth++          (entering a container)
  'e'         → depth--;  if depth == 0 → this is OUR closing 'e'
  'i'         → skip to that integer's 'e'
  '0'..'9'    → skip that string entirely (via its length)
```

The subtle part worth your attention: **string data is skipped by its length,
never scanned byte-by-byte.** The `pieces` string is 473,280 bytes of ~random
hash bytes, some of which are literally the byte `'e'` or `'d'`. If we naively
depth-counted *every* byte, we'd think the dict ended dozens of times inside
`pieces`. Skipping whole values by length is what makes the depth counter
trustworthy. (This mirrors why length-prefixing is unambiguous — Phase 1's
lesson paying off again.)

### Helper 4 — `computeInfoHash(raw)`: hash the exact range

```
infoStart = findInfoValueStart(raw)     // position of the info 'd'
infoEnd   = findDictEnd(raw, infoStart) // position of its matching 'e'
hash = SHA1(raw[infoStart .. infoEnd])  // INCLUSIVE of both d and e
```

Note the **inclusive range**: the info hash covers the `d` … `e` *including
both* — because that's the bencode of the whole info value, and that's what
every other client hashes. Off-by-one here = wrong hash = wrong swarm. The code
writes it as `length = infoEnd - infoStart + 1` — that `+1` is the "inclusive
close" and is a deliberate, commented decision, not a typo.

> The rule of this chapter, unforgettable:
> **Hash exactly what was in the file. Don't re-encode. Remember where the
> info section began and ended while walking. A single byte of difference
> makes the hash wrong.**

## Reading the file in the first place

`readFile(filepath)` returns the file's raw bytes using a stream in **binary
mode (`std::ios::binary`)**: critical, because bencode is byte data, not text.
In text mode on some platforms, bytes could be translated (newline handling);
binary mode guarantees the bytes arrive unmodified — because we're about to
hash them, and "unmodified" is the entire point.

## Putting it all together: `TorrentParser::parse()`

Mirrors the code in `src/torrent/TorrentParser.cpp`:

```
1. readFile(path)       → read the .torrent file's raw bytes            (bytes)
2. decode(raw)          → bencode it into a BencodeValue (Phase 1)      (structure)
3. check root is a DICT → a .torrent is always one big dictionary
4. take "announce"      → the string → torrent.announce                  (text)
5. take "info"          → must be a DICT or we throw
     "name"         → torrent.name                                       (text)
     "piece length" → torrent.pieceLength                                (number)
     "length"       → torrent.length                                     (number)
     "pieces"       → torrent.pieces: raw bytes of ALL hashes            (binary)
6. computeInfoHash(raw) → re-walk RAW bytes, SHA-1 the info dict range   (bytes for hashing)
     → torrent.infoHash
```

Every extraction is *guarded*: we check "does the key exist AND is its type
right?" before trusting it — a wrong type (e.g. `announce` came back as an
integer) is treated as *absent*, never as a crash. And the two *hard*
requirements — root must be a dict, `info` must be present and a dict — throw
a `BencodeException` if violated: garbage in, loud refusal out (Phase 1's
strictness, promoted to file level).

One helper you'll meet in the `.cpp`: `stringValueAsUtf8()`, which converts a
byte-vector string to a real `std::string` for fields that are *text* (names,
URLs). It's the mirror of Phase 1's bytes-vs-text rule: **know when you're
holding bytes, and only reinterpret as text at the edge.**

## Two views of the same data

Step 2 gives us a *decoded* structure; step 6 re-walks the *raw* bytes. Why
both?

> **The decoded structure is for reading; the raw bytes are for hashing.**
> Two views of the same file, each used for what it's good at.

Read a `name` or `length` quickly via the decoded map. Hash the info dict
correctly via the raw bytes. Mixing the two — decoding *then re-encoding* for
the hash — is precisely the bug the "don't re-encode" rule exists to prevent.

## The code, piece by piece (classes, functions, algorithms, techniques)

Same tour as Phase 1: every user-defined class and function, every built-in we
lean on, and the named techniques this parser is a textbook example of.

### The user-defined types (a struct, a class, and one struct reuse)

| Type | Header | Its one job | Java cousin |
|---|---|---|---|
| `TorrentFile` (**struct**) | `include/torrent/TorrentFile.hpp` | a plain data holder for the facts we extracted | a POJO / record |
| `TorrentParser` (**class**) | `include/torrent/TorrentParser.hpp` + `.cpp` | read a `.torrent` file → a `TorrentFile` | a static utility / service class |
| `BencodeValue` (reused) | `include/bencode/BencodeValue.hpp` | Phase 1's decoded tree, which we *query* for fields | a parsed JSON node |

Two things to notice in that table: `TorrentFile` is a **`struct`**, not a
class — in C++ the difference is only default access (struct = public,
class = private); we use `struct` to say "just data". And `TorrentParser` is
**all `static`**: it has no state, so it exposes static methods and is never
instantiated. That's the "utility class" idiom, like a Java class whose methods
are all `static`.

### `TorrentFile` — the fields and the "20 bytes each" convention

```cpp
std::string announce;   // the tracker URL
std::string name;       // the file/torrent name
long long  pieceLength; // bytes per piece (Ubuntu: 262144)
long long  length;      // total bytes of the file
std::vector<uint8_t> pieces;    // ALL piece hashes, concatenated (20 bytes each)
std::vector<uint8_t> infoHash;  // the 20-byte SHA-1 of the bencoded info dict
```

Technique: **a flat data-transfer object (DTO)**. The parser's whole job is to
fill this in and hand it over; no logic lives here. Note `pieces` is stored as
one big byte blob, not a `vector` of 20-byte arrays — the natural unit is
*one* hash, fetched in Phase 6 as
`pieces.begin() + i*20 … +20`. That "one flat buffer, index arithmetic to
carve out records" style is very C++, and it keeps memory contiguous.

### `TorrentParser` — function by function

| Function | Technique | What it does |
|---|---|---|
| `parse(filepath)` (public, static) | **pipeline of small steps**; **defensive field extraction with `map::find` + type check** | the 6 steps: read → decode → verify dict → pull `announce` → pull `info` fields → compute hash |
| `readFile(filepath)` (private, static) | **RAII file handle** + `istreambuf_iterator` slurp | open the file in binary, read *all* bytes into a `vector<uint8_t>`; throw if it can't open |
| `findInfoValueStart(raw)` | **structural walk, not substring search**; **raw-bytes pointer arithmetic** | walk the top-level dict and return the index just past the `info` value's start |
| `skipBencodeValue(raw, pos, depth)` (file-local `static`) | **recursive skip walker**; mirrors the decoder but *doesn't build* values; depth-capped | given a position, return the position *just past* the value that starts there (used to "leap over" every non-info value) |
| `findDictEnd(raw, dictStart)` | **depth counter** (iterative, not recursive) | from the `info` dict's opening `d`, return the index of its *matching* closing `e` |
| `computeInfoHash(raw)` | **pointer + length → digest**; reuses Phase 1's helpers | hash the exact byte range `[infoValueStart, dictEnd]` with `SHA1()` |
| `checkedAppendDigit(...)` (file-local `static`) | **overflow guard** (same helper idea as Phase 1) | used by the three raw-walkers so a hostile `.torrent` can't overflow a length |

Three of these deserve their own "why":

- **`readFile` uses `std::ifstream` (a RAII handle).** The stream closes
  itself when the function returns, even on the exception path — you can't
  leak a file handle. (Java's `try-with-resources` / `Files.readAllBytes` is
  the same idea.)
- **Why a structural walk, not `find("4:info")`?** Because the byte pattern
  `4:info` can appear *inside a string value* (a file could literally be named
  `4:info`). Searching for a substring finds the wrong place; walking the
  dict's grammar can only land on real keys. (Already teased above; this is
  the code that proves it.)
- **`findDictEnd` counts depth with a `while` loop instead of recursing** — a
  nice contrast with `skipBencodeValue`, which *is* recursive. Same job
  (find the matching `e`), two valid styles; the iterative one can't blow the
  stack at all, so it's the extra-defensive choice for the raw scanner.

### The extraction pattern (the quiet engineering of `parse()`)

Every field is pulled the same defensive way:

```cpp
auto it = info.find("piece length");                 // look the key up
if (it != info.end() && it->second.getType()         // present?
        == BencodeValue::INTEGER) {                   // …and the right type?
    torrent.pieceLength = it->second.asInteger();
}
```

Technique: **the find-then-type-check idiom**. `map::find` returns an
*iterator*; comparing it to `map::end()` is the safe "was it there?" test
(calling `operator[]` would silently *insert* a default — a classic C++ trap).
Then the `getType()` check means a malicious `.torrent` that puts a *string*
where a number belongs is simply ignored (or, for `info` itself, rejected
outright) instead of being blindly cast. This is "never trust the input"
applied field-by-field.

### The built-in types we rely on here

| Built-in | What it gives us | Java equivalent |
|---|---|---|
| `std::ifstream` | a binary-mode file reader (RAII) | `InputStream` / `Files` |
| `std::istreambuf_iterator<char>` | slurps a stream into a container (iterator-range read) | `Files.readAllBytes` |
| `std::vector<uint8_t>` | the raw file bytes, and the `pieces` blob | `byte[]` |
| `std::map<K,V>` (from Phase 1) | sorted, key-looked-up info dict | `TreeMap` |
| iterators + `map::end()` | the find-then-check lookup pattern | `Map.get` + `containsKey` |
| `static` members | a stateless utility class | all-`static` Java class |
| OpenSSL `SHA1()` | the digest function over a raw byte range | `MessageDigest.getInstance("SHA-1")` |
| `const uint8_t*` + `size_t` | pointer+length (a "span by hand") | `byte[]` + offset/length |

### The algorithms/patterns this file is a textbook example of

1. **Two-pass, two-view processing** — decode for *reading*, raw-walk for
   *hashing*. The "don't re-encode" rule as an architecture.
2. **Structural parsing over substring search** — walk the grammar to locate a
   key; substring search is subtly wrong.
3. **Recursive skip-walker** (`skipBencodeValue`) — a cursor function that
   *jumps over* a value without materialising it; recursive because containers
   nest, depth-capped for safety.
4. **Iterative depth counting** (`findDictEnd`) — match a closing delimiter with
   a counter; no recursion, so no stack risk.
5. **Hash a byte range directly** (`SHA1(ptr, len, out)`) — compute a digest
   over the original bytes, not a re-serialisation.
6. **Find-then-type-check field extraction** — `map::find` + `getType()` for
   every field; never blind-cast, never `operator[]` on untrusted input.
7. **Fail-fast with position-rich errors** — every throw names the byte offset
   where things went wrong.
8. **Overflow prevention** (`checkedAppendDigit`) — applied to *every* digit
   run the raw-walkers read, because a length is untrusted input too.
9. **RAII for the file handle** — the `ifstream` releases the OS resource
   automatically.
10. **Reuse, don't re-implement** — the parser leans on Phase 1's
    `BencodeDecoder`; the only new logic is the raw-byte walking that hashing
    uniquely requires.

### A one-paragraph mental model

> `TorrentParser::parse` is a **six-step pipeline**: slurp the file (RAII) →
> hand the bytes to Phase 1's decoder → sanity-check the root is a dict →
> defensively copy out `announce` and the four `info` fields with
> find-then-type-check → and then, on the *original* bytes, locate the `info`
> dict's exact range (recursive skip-walker + depth counter) and `SHA1()` it.
> The decoded tree is for reading; the raw bytes are for hashing — and never
> the same step, never re-encoded.

## The real test output (Ubuntu 24.04 torrent)

When `./build/peerflow` runs, Phase 2 prints this exact block:

```
Announce:      https://torrent.ubuntu.com/announce
Name:          ubuntu-24.04.1-desktop-amd64.iso
Piece length:  262144 bytes
File length:   6203355136 bytes
Num pieces:    23664
Info hash:     4a3f5e08bcef825718eda30637230585e3330599

PASS: torrent parsed successfully
```

How do we *know* the info hash is right? Cross-check against a real client —
open the same `.torrent` in qBittorrent or Transmission; both display the same
hash, because any correct parser must. That cross-check is the phase's
official checkpoint in our plan: **if your hash matches what a real client
shows, your parser and your raw-bytes-walking are correct.**

(Fun fact: trackers don't even need the torrent to be *named* — peers are
identified only by this 40-hex-character hash. The info hash *is* the torrent's
true name on the network.)

## The six sanity checks the test performs

The test prints the fields, then *verifies* them rather than trusting the
printout:

- `announce` is not empty
- `name` is not empty
- `pieceLength` > 0
- `length` > 0
- `pieces.size()` is a multiple of 20 (a complete list of whole hashes)
- `infoHash.size() == 20` (a SHA-1 digest is exactly 20 bytes)

All six hold → `PASS: torrent parsed successfully`.

## Where this leaves us

We can now read a recipe card perfectly, and bag the torrent's identity — the
info hash — which every other phase will flash like an ID card:

- **Phase 3** shows that ID to a tracker when announcing.
- **Phase 4** shows that ID to peers in the handshake.
- **Phases 5–6** use the *piece* hashes to verify every byte we download.

We've also, incidentally, done our first real interoperability test: an actual
off-the-internet torrent file parses into exactly the numbers its authors
encoded.

---

*Next: [05 — Phase 3: The Tracker](05-phase3-tracker.md)*