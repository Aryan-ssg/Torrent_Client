# 03 — Phase 1: The Bencode Decoder

## What we built and why

The `.torrent` file — the recipe card from our big picture — is written in a
tiny data language called **bencode (a deliberately minimal format invented
for BitTorrent)**. Phase 1 builds the **decoder (a reader/parser: a function
that turns raw bytes written in a format into C++ objects our code can
actually use)**.

The milestone: decode any bencoded input and get back a structured value,
proven by **16 passing tests**.

## First: what is "encoding", and why do we even need it?

Let's build this from absolute zero.

**A computer's memory is a long sequence of bytes.** A **byte (an 8-bit
number: one of 256 possible values, 0–255)** is the atomic "letter" of
computer storage. When you think of a file, think of a very long string of
these 0–255 numbers.

**But a number alone means nothing.** Does the byte `65` mean the letter "A"
(text), the value sixty-five (a number), a part of a picture (colour), or one
pixel of sound (audio)? *It depends on the format.*

> A **format (a set of rules FOR arranging bytes so that readers know what
> they mean)** is the agreement between whoever wrote the file and whoever
> reads it. "The first byte is a length, the next bytes are a string…" is a
> format. Russian nested dolls are a format: the smallest doll is always
> inside, always in the same order.

So "encoding" is simply: **writing down a value using a format's rules.**
"Decoding" is reading it back. Bencode is one such format. JSON is another
(you likely know it). This one command — "decide what these bytes mean" — is
the whole of Phase 1.

### Bytes vs. text — the act of showing an "A"

Plain text files are also just bytes, mapped to letters by a table called
**ASCII (a table assigning numbers to letters: `A`=65, `B`=66, `a`=97,
`0`=48)**:

```
"hello"  is stored as  104 101 108 108 111
                        h   e   l   l   o
                      0x68 0x65 0x6c 0x6c 0x6f   (hex notation for the bytes)
```

But a **binary file (data that uses every byte value, including 0–31 and
128–255, not just printable letters)** cannot lean on "printable text" rules.
Torrent files are full of binary data — the 20-byte hashes are essentially
random bytes you can't print. Bencode was built to carry that safely.

### Why not just use JSON?

Let's put the same object side by side:

```json
{ "name": "hello", "spam": 42 }
```

```text
d4:name5:hello4:spami42ee     ← the same thing in bencode
```

JSON's problems for torrents:

1. **It's text-first.** Raw binary data (like another 20-byte hash) doesn't
   fit cleanly inside a JSON string without escaping tricks.
2. **It's fat.** Every `"`, `{`, `}`, `,`, `:` wastes space and invites bugs
   (a `.torrent` is transmitted to *every new peer*, so it ships around a lot
   — weight matters).
3. **It's not byte-stable.** JSON doesn't require a canonical form; different
   writers produce different bytes for the same data. Bencode *requires* the
   same data to always be the same bytes — and that property is **load-bearing**: the info hash (Phase 2) is a hash of *exact bytes*, and all peers
   must compute the same hash from the same file.

Bencode is: **tiny rulebook, four types, byte-stable, binary-safe.** Perfect
for its job.

## The four bencode types (the whole language)

The entire format fits in one table:

| Type | Rule (specification) | Example bytes | Means |
|---|---|---|---|
| **Integer** | `i` + a plain base-10 number + `e` | `i42e` | the number 42 |
| **String** | decimal length + `:` + exactly that many bytes | `5:hello` | the word "hello" |
| **List** | `l` + any number of values + `e` | `li42e5:helloe` | `[42, "hello"]` |
| **Dictionary** | `d` + alternating keys and values + `e` | `d3:cow3:moo4:spami42ee` | `{"cow":"moo","spam":42}` |

Decoded by a tiny mini-vocabulary:

- **`i`…`e`** = "the number between me and the closing `e`."
- **`5:`** = "the next **five** bytes are mine" — the digit(s) before `:` are
  the *length* of the string that follows.
- **`l`…`e`** = "a list: everything between is my items, in order."
- **`d`…`e`** = "a dictionary: my keys and values, alternating."

### The bencode rulebook (the fine print worth knowing)

The strict rules a conforming encoder should follow — the same rules our
decoder *validates*:

| Rule | Example of legal / illegal |
|---|---|
| Integers are base-10, **no leading zeros**, optional leading `-`, one `e` to close | `i0e` legal; `i03e` illegal; `i-42e` legal |
| String lengths are non-negative decimal | `0:` legal (empty string); `-3:x` illegal |
| Lists contain any values, any mixture, any nesting | `li42e5:helloe` legal |
| Dictionary **keys MUST be strings** | `d5:key3:vale` legal; a numeric key is illegal |
| Dictionary keys are **sorted** (lexicographically, byte order); spec demands it | encodes byte-stably so a re-encoder can't reorder |
| Everything ends with its own closing `e` | otherwise unterminated → error |

Note the *sorted keys* rule — it's the reason a given structure has exactly
one valid byte form. (Our decoder *accepts* any order — being lenient about
*sorting* is fine because we never re-encode for hash purposes; but being
strict about the *other* rules is not optional.)

### Why do strings carry their length first?

Because a bencoded string is raw bytes and may *contain* things that would
fool a text-parser: a colon, a lowercase `e`, a zero byte, a newline. If the
format said "the string continues until `:`", the string `"a:b"` would break.
By declaring the length first — *"the next 5 bytes are mine, period"* — there
is zero ambiguity:

> **A length-prefixed string (a string whose size is stated before its
> content)** is unambiguous: exactly one correct reading, no matter what bytes
> the content contains.

## Reading bytes by hand: a full trace

Theory is nice; let's trace the real example until it's boring.
`d3:cow3:moo4:spami42ee` is — one number at a time:

```
index: 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21
byte:  d  3  :  c  o  w  3  :  m  o  o  4  :  s  p  a  m  i  4  2  e  e
```

Start at position 0.

1. Byte `d` → a **dictionary**. With our cursor now at 1:
2. Byte `3` → a string is starting; read its length digits until `:` →
   `strLength = 3`; cursor past the `:` is 3; the key = bytes [3,6) = `cow`;
   cursor = 6.
3. Parse the value: byte 6 is `3` → string `moo` (bytes [8,11)); store
   `cow → "moo"`. Cursor = 11.
4. Byte 11 is `4` → string again; length 4; key = bytes [13,17) = `spam`;
   cursor = 17.
5. Parse the value: byte 17 is `i` → **integer**: read digits `4`,`2` until
   the `e` at 20 → value `42`; cursor = 21. Store `spam → 42`.
6. Byte 21 is `e` → the dictionary is **closed**. Done.

Result:

```
{ "cow": "moo", "spam": 42 }
```

Notice what happened at step 5: the value `42` was decided by a *different
set of rules* than the key was — a dictionary value can be any type, but a
**bencode key is always a string** (rulebook, above). The parser keeps the two
separate.

The Java parallel: this is JSON parsing with `{`, `"`, `:` replaced by the
characters `d`, `e`, `:` — but simpler, and byte-exact.

## Recursion: how nesting is handled

A list can contain lists, which contain lists… A dictionary can contain lists
of dictionaries of lists of…

The elegant trick is **recursion (a function that calls itself — like opening
Russian nesting dolls: the *procedure* for "open a doll" is the same whether
it's the outermost doll or the innermost one)**.

Our `parse()` is a **dispatcher (a function that looks at the first byte and
hands the work to the right specialist)**:

```
parse() sees 'i'      → parseInteger()          base case (read digits)
parse() sees '0'-'9'  → parseString()           base case (read length + bytes)
parse() sees 'l'      → parseList()             loop: parse() for every item
parse() sees 'd'      → parseDictionary()       loop: parseString() for key,
                                                 parse() for value
```

Where's the recursion? Inside `parseList()`, which says "for each of my items,
call `parse()`" — and if an item is itself a list, `parse()` hands it straight
back to `parseList()`, which again calls `parse()` for *its* items… any depth,
same procedure.

A two-level example, `li42el5:hellowee` (a list containing `42` and another
list):

```
parseList()        ┌─ parseList() item 1 ────────┐
  item 0 = 42      a list starts...
  item 1 = ─────────  parse() item 0 = "hello"  (a string → base case)
                     closing 'e' → done, back in the outer loop
  closing 'e' → done
Result: [42, ["hello"]]
```

**Why can't this loop forever?** Because recursion *terminates*:
- the base cases (integer, string) read their bytes and stop immediately;
- every recursive call **consumes bytes and moves the cursor forward**;
- the input is finite, so eventually the bottom is reached.

Recursion terminates the same way opening nested dolls does: the dolls get
smaller each time and there's a last one.

## What one byte can't do: the decoder's state

To read a stream you need to remember *where you are*. `BencodeDecoder` keeps
three pieces of state:

```cpp
class BencodeDecoder {
    const uint8_t* data;   // a pointer (an address) to the first input byte
    size_t length;         // how many bytes total
    size_t pos;            // the moving "cursor": where we'll read next
};
```

The **pointer (a value that *points at* a byte in memory, like a bookmark
saying "the bytes start over here")**, the **length (how many bytes we're
allowed to touch — the safety border)** and the **cursor (`pos`: our finger as
we read, 0 … length)**. Every function advances `pos`; every function checks
`pos < length` before reading — **never reading past the border** is what keeps
a forged/truncated torrent from crashing us.

## The four building blocks of our code

### 1. `BencodeValue` — one class, four shapes (the "tagged union")

In Java you might model "a value that can be four things" with an interface
and four records:

```java
interface BencodeValue {}
record IntegerValue(long value) implements BencodeValue {}
record StringValue(byte[] value) implements BencodeValue {}
record ListValue(List<BencodeValue> value) implements BencodeValue {}
record DictValue(Map<String, BencodeValue> value) implements BencodeValue {}
```

C++ uses the classic **tagged union (one object carrying a *label* saying
which shape it is, plus one slot of storage for the data of that shape)**:

```cpp
class BencodeValue {
    enum Type { INTEGER, STRING, LIST, DICT };   // the "label"
    Type type;                                   // which shape are we now?
    long long intValue;                          // used when INTEGER
    std::vector<uint8_t> stringValue;            // used when STRING
    std::vector<BencodeValue> listValue;         // used when LIST
    std::map<std::string, BencodeValue> dictValue; // used when DICT
};
```

Only **one** of those storage slots is meaningful at a time; `type` is the
judge. Picture a lunchbox labelled "apple" — open it and the apple slice is
there, but the same-size sandwich slot is empty. The label names the reality.

Two details matter:

- Why not just inherit from a base class? Inheritance *would* work, but a
  tagged union is more direct for "exactly four shapes, known up front" — and
  mirrors exactly what the decoder produces.
- `std::vector<uint8_t>` for strings — **a vector (a growable array, like
  Java's `ArrayList`) of bytes**. We deliberately do *not* use `std::string`
  for binary data, because strings in C++ can quietly stop at a **zero byte
  (`\0`, the "end of text" marker)**. A vector never does. This is the
  *bytes vs text* rule made structural: **torrent strings are bytes, not text.**

You never build a `BencodeValue` piecemeal; you ask a **factory method (a
function that builds the object AND sets its label for you)**:

```cpp
BencodeValue::makeInteger(42);   // an INTEGER, labelled, with 42 inside
BencodeValue::makeString(bytes); // a STRING, labelled, with the bytes inside
BencodeValue::makeList(items);   // a LIST...
BencodeValue::makeDict(map);     // a DICT...
```

And you read results with **getters (functions that return the stored data)**,
always checking the label first:

```cpp
value.getType();       // which shape?   (INTEGER / STRING / LIST / DICT)
value.asInteger();     // the number     (only valid if INTEGER)
value.asString();      // the raw bytes  (only valid if STRING)
value.asList();        // the items      (only valid if LIST)
value.asDict();        // the pairs      (only valid if DICT)
```

One more nicety: `BencodeValue` is a **value type (an object that is copied by
value, like a `long` or an `int` — not a handle with shared ownership)**. The
decoder hands it back by value; the map owns its copies; there is no manual
memory management anywhere. (You'll appreciate this when we meet types that
*can't* be copied — sockets, in Phase 4.)

### 2. The decoder — where the reading happens

`BencodeDecoder` puts the cursor to work:

- **`parse()`** — the dispatcher: look at the byte at `pos`, pick a
  specialist.
- **`parseInteger()`** — after the `i`, read digits until the `e`. The classic
  number-building loop
  ```
  value = value * 10 + (digit)      // 4 → (4*10 + 2) = 42
  ```
  handles the negative `i-7e` too: remember the `-`, apply it at the end.
- **`parseString()`** — read the length digits until `:`, then **verify that
  many bytes actually remain** before copying them (never blindly trust a
  claimed length!), then copy exactly that many.
- **`parseList()`** — skip the `l`, then keep calling `parse()` until it sees
  the closing `e`.
- **`parseDictionary()`** — skip the `d`, then alternate: **key must parse as
  a string**, then `parse()` its value, storing into a
  **`std::map` (a sorted key→value container, like Java's `TreeMap`)**.

### 3. Strictness: the "never trust the input" rule, on its home turf

The decoder is *deliberately* strict. On any violation it stops and throws an
**exception (an error object that travels up the call stack — "unwinds" —
until some `catch` handles it; a throw-catch pair is `throw new
RuntimeException(...)` / `catch(Exception e)` in Java)**:

| Violation | Why we refuse |
|---|---|
| Input runs out mid-integer/list/dict | the structure is truncated → malformed |
| A string length claims more bytes than exist | overflow/forged length → would read past `length` into garbage |
| A digit appears where a value must begin | garbage has no legal reading |
| An integer has a bad character before `e` | e.g. `i4ae` → invalid |
| Trailing bytes after a complete value (`i42eEXTRA`) | one message per input; leftovers mean corruption or a mixed-up byte stream |

Why be this picky? Two reasons, both practical:

1. **Hashing.** Phase 2 hashes *exact bytes*. If we silently accepted garbage,
   we might compute a wrong info hash and then happily join the wrong swarm.
   Garbage must *never* survive into later phases.
2. **Hostile input.** Trackers and peers (Phases 3–6) can send us data. If our
   parser is forgiving, malformed data could crash or confuse us. Strictness
   is a **security property**, and the very first appearance of the project's
   golden rule: **never trust the network — check everything.**

> **"Strict now = safe later."** The rejection tests (`i42eEXTRA`, `x`) are
> not bureaucracy; they are our immune system being tested so it keeps
> working forever.

### 4. The entry points

```cpp
BencodeDecoder::decode(vectorOfBytes); // the main entry: byte vector → value
BencodeDecoder::decode(string);        // convenience overload: string → value
```

Both do the same three things:

```
1. build a decoder over the bytes
2. parse() exactly ONE value
3. verify the cursor reached the very end — else "trailing data" exception
```

That step 3 is what makes `decode` *total*: it either produces one complete
value or it refuses — never a partial guess.

## The 16 tests that prove Phase 1

Every test in `src/main.cpp` is a two-player game: feed a string of bytes,
check the value that comes back. The family of tests:

| Test | Input → Expected | Proves |
|---|---|---|
| positive integer | `i42e` → 42 | integer parsing |
| negative integer | `i-7e` → -7 | the minus sign path |
| zero | `i0e` → 0 | base case works |
| string | `5:hello` → "hello" | length-prefix reading |
| empty string | `0:` → "" | zero-length edge case |
| simple list | `li42ee` → [42] | list framing |
| **nested list** | `li42el5:helloweee`-shaped → `[42, ["hello"]]` | **recursion** |
| simple dictionary | `d3:cow3:moo4:spami42ee` → {cow=moo, spam=42} | dict framing |
| **nested dictionary** | a dict inside a dict | recursion inside dicts |
| **trailing data** | `i42eEXTRA` → *rejected* | strict "one value only" |
| **invalid byte** | `x...` → *rejected* | the dispatcher refuses garbage |

The last two rows are the interesting ones: they assert the cases that *must
fail*. A test suite that only checks happy paths is missing half the story —
for a network client, the rejection tests are the other half of the
security.

The real output tail, as we see it when the program runs:

```
PASS: "i42e" -> 42
PASS: nested list -> [[42]]
PASS: dictionary -> {cow=moo, spam=42}
FAIL: trailing data not detected   ← would show if strictness ever breaks
...
Passed: 20
Failed: 0
```

(16 of the 20 totals are bencode tests; the rest come from Phases 2–4.)

## One defensive note for the future (a seed for later phases)

Recursion is wonderful, but a *pathologically nested* input could recurse very
deeply. Real `.torrent` files are shallow (a handful of levels), so it's a
non-issue today — but it's exactly the kind of thing that becomes a hardening
feature in Phase 7 ("never trust the network" grows up to mean "bound your
recursion too"). Good to have noticed it now.

## Tie it back to the big picture

The `.torrent` file — the pizza recipe card — is written in bencode. Phase 1
hands us the ability to *read* the recipe reliably. Phase 2 uses that to
extract the actual cooking instructions (tracker address, sizes, verification
codes, fingerprints) — and, memorably, to compute the recipe's own fingerprint
from the raw bytes.

---

*Next: [04 — Phase 2: Torrent parser](04-phase2-parser.md)*