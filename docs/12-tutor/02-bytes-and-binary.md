# Module 2 — Bytes and Binary Data

> **Goal of this module:** you will be able to look at a row of hex digits,
> work out what number it represents, and understand why this project has
> hand-written functions for byte order.
>
> Still no networking. But this is where the data that networks *carry*
> finally becomes concrete.

---

## 1. 🟢 A byte is eight bits, and it is never negative

You met `uint8_t` in Module 1. Now let's say what it actually *is*.

```
  8 bits:  0 1 0 0 1 1 0 1
           └───────────┘
            one byte, = 0b01001101 = 0x4D = 77 decimal
```

**A single byte has 256 possible values.** Because it is *unsigned*, they are
numbered **0 to 255** — never negative, never larger.

| Type | Range | Fits in |
|---|---|---|
| `uint8_t` | 0 … 255 | 1 byte |
| `uint16_t` | 0 … 65,535 | 2 bytes |
| `uint32_t` | 0 … 4,294,967,295 | 4 bytes |
| `uint64_t` | 0 … 18,446,744,073,709,551,615 | 8 bytes |

**🎯 Mental model:** every number the network carries is a pile of bytes. A
"4-byte length" is really four `uint8_t` values sitting next to each other.
The rest of this module is about how to put those bytes in the right order
and read them back.

### Why this project insists on `uint8_t` and not `char`

C++ also has a `char` type — one byte, but **signed on this machine**, so it
runs −128 to 127. That makes it the wrong tool for network data, where every
value is genuinely 0–255.

The project uses `uint8_t` 228 times and `char` 104 times. The `char` uses
are for text (`hexOf` returns a `std::string` of characters). **The `uint8_t`
uses are for data.** Whenever you see `uint8_t`, think *a byte off a wire*.

---

## 2. 🟢 Hexadecimal — reading `0x3A` without fear

Protocols are written in hex because it's a **compact way to write binary.**

One hex digit = **4 bits** = half a byte. So:

```
  hex digit:   0    1    2    3    4    5    6    7    8    9    A    B    C    D    E    F
  value:       0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
  4 bits:   0000 0001 0010 0011 0100 0101 0110 0111 1000 1001 1010 1011 1100 1101 1110 1111
```

So one byte = **two hex digits**. `0x` just means "the following digits are
hexadecimal."

**Java parallel:** Java has hex literals too (`0x3A`), and you can cast to
read them — `(byte)0x3A` is 58. Nothing new. The only new part is that C++
lets you say "this is exactly one unsigned byte" in the type itself.

### Read these two and you'll never need a converter

```
  0x00 =   0        0x3A =  58        0xFF = 255        0x80 = 128
  0x01 =   1        0xE1 = 225        0x7F = 127        0x01 =   1
```

* `0xFF` is all eight bits on → 255, the biggest byte.
* `0x80` is `1000 0000` in binary → only the top bit set → 128.

`0x80` matters in about two sections below, so it's worth having.

---

## 3. 🟢 Turning a byte into text you can read

Here's a real function from `src/peer/PeerSession.cpp`. It is the single
best byte-handling routine in the project, because it does the one thing you
always want to do with a byte: **print it**.

```cpp
std::string hexOf(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);      // the high 4 bits
        out.push_back(digits[b & 0x0F]);    // the low 4 bits
    }
    return out;
}
```

Call it with the bytes `{0x3A, 0xDB, 0x77}` and it returns `"3adb77"`.

Four ideas in six lines. Let's take them one at a time.

### `for (uint8_t b : bytes)` — the range-for loop

Not "loop over indices." C++ can hand you each element directly:

```cpp
for (uint8_t b : bytes) { ... }    // b is each byte, in order
```

**Java parallel:** `for (byte b : bytes)`. Identical idea.

### `b >> 4` — grab the high 4 bits

`>>` is a **right shift**. Shifting right by 4 throws away the low 4 bits and
slides the high 4 down into place:

```
  b = 0x3A = 0011 1010
               ^^^^ ^^^^  ← these are the LOW 4 bits, value 10 = 0xA
           ^^^^          ← this is b >> 4, value 0011 = 3 = 0x3
```

`b >> 4` is `3`.

### `b & 0x0F` — keep the low 4 bits

`&` is a **bitwise AND**. `0x0F` is `0000 1111` — ones in the low four
positions, zeros everywhere else. AND-ing with it *keeps* the low 4 bits and
**discards the high 4**:

```
  b        = 0011 1010
  0x0F     = 0000 1111
  AND      = 0000 1010    → 10 = 0xA
```

`b & 0x0F` is `10`, which is `0xA`.

> ### **WITHOUT vs WITH**
>
> ```
> Just dividing:  b / 16 = 58 / 16 = 3   and  58 % 16 = 10
> ```
>
> That also works! For *this* function. But it breaks for bytes whose high bit
> is set — and network data is full of them. With `uint8_t` being unsigned,
> `255 / 16 = 15` and `255 % 16 = 15`, which happens to be fine, but the
> moment the same arithmetic runs on a **signed** type (like a `char`), `255`
> is really −1, and −1/16 = 0. Silently wrong.
>
> ```
> Shifting and masking: 255 >> 4 = 15,  255 & 0x0F = 15   ✓ always
> ```
>
> **THE RULE** to split a byte into two hex digits: `high = b >> 4`,
> `low = b & 0x0F`. Shifts and masks never care about signedness.

### `digits[...]` — a lookup table

`digits` is just the string `"0123456789abcdef"`. Indexing it with a number
0–15 gives you the matching character. So `digits[3]` is `'3'` and
`digits[10]` is `'a'`.

That's the whole trick. **One byte becomes two characters because a byte
holds two nibbles.**

**🎯 Mental model:** `hexOf` is what you use whenever you need to *see* bytes.
Any time this project prints a hash or an info dict, this is what did it.

---

## 4. 🟢 The other direction: text becomes a number

Now the reverse. A `.torrent` file doesn't contain the number 12345 — it
contains five characters: `1`, `2`, `3`, `4`, `5`.

Somehow that has to become a number. Here's how, from
`src/bencode/BencodeDecoder.cpp`:

```cpp
long long digit = b - '0';
```

**What's happening?** `b` is one byte from the file. `'0'` is the byte for the
character zero. In ASCII, `'0'` is the number **48**. So:

```
  b      = '5'   = 53 decimal
  '0'    =      = 48 decimal
  b - '0' =      = 5        ← the number five
```

Subtracting one byte from another is how you convert a digit character into
its numeric value. **This only works because `'0'` is 48 and each digit is
one more than the last.**

Here are the numbers, verified on your machine just now:

| Character | Byte value | | Character | Byte value |
|---|---|---|---|---|
| `'0'` | 48 | | `'9'` | 57 |
| `'A'` | 65 | | `'a'` | 97 |

`b - '0'` works for digits because they happen to be *contiguous* starting at
48. Uppercase letters are contiguous too (`b - 'A'` → 0–25). Lowercase is the
trap: `'a'` is **97**, not 65, so `b - 'A'` on a lowercase letter gives you 32
instead of 0. Case-insensitive parsing needs `(b | 0x20) - 'a'` or a table.

### And this is how bencode recognises its own types

Look at how the decoder decides what it's looking at:

```cpp
if (current == 'i') {
    return parseInteger();        // an integer starts with the letter i
} else if (current == 'l') {
    return parseList(depth);      // a list starts with l
} else if (current == 'd') {
    return parseDictionary(depth);
}
```

`current` is one byte. It's being compared to a **character literal**, and the
compiler turns `'i'` into the byte 105. So the whole file format is decided
by which bytes appear.

**Java parallel:** `switch (c) { case 'i': ... }`. Same idea — but worth
noting the C++ detail: **a character literal is a byte value, not an abstract
character.** That's why `b - '0'` in the section above compiles at all.

**🔵 LATER.** In **C** (the language C++ grew out of), `'i'` has type `int`, so
`sizeof('i') == 4` and `sizeof(char) == 1`. C++ narrowed character literals to
`char`, so in C++ `sizeof('i') == 1`. You will meet code that assumes the C
behaviour; it's a classic source of confusion, and it's why this project casts
explicitly everywhere.

> **One thing to watch.** `checkedAppendDigit` in the same file guards against
> overflow while building numbers:
>
> ```cpp
> long long checkedAppendDigit(long long value, uint8_t b, size_t pos) {
>     long long digit = b - '0';
>     if (value > (LLONG_MAX - digit) / 10) {
>         throw BencodeException("Number overflows 64-bit signed range...");
>     }
>     return value * 10 + digit;
> }
> ```
>
> **Why:** the digits come from a `.torrent` file — a stranger's file. Enough
> of them and `value * 10 + digit` runs past the largest `long long`. In C++
> that isn't "a wrong number," it's **undefined behaviour**: the program is
> allowed to do anything at all.
>
> In Java the same overflow just quietly gives you a wrong number. **C++ being
> stricter here is a genuine safety win**, and it costs one comparison per
> digit.

---

## 5. 🟢 Arrays of bytes

Everything above happened one byte at a time. In practice you handle a
*run* of bytes as `std::vector<uint8_t>`:

```cpp
std::vector<uint8_t> payload;
payload.size();       // how many bytes
payload[i];           // the i-th byte (auto-converted to uint8_t)
payload.data();       // the raw bytes as a C array, for sending
```

**Java parallel:** `byte[]`. Very similar. Two differences:

| | Java `byte[]` | C++ `std::vector<uint8_t>` |
|---|---|---|
| Size | fixed at creation | **can grow** — `push_back()` |
| Growth | you reallocate by hand | handled for you |
| Default value | 0 | 0 |

The growable part matters here. Bencode parsing is `push_back` in a loop, and
this is `sendMessage` building a message one byte at a time.

`payload.data()` is the one bit of C++ that Java has no answer for. It hands
you a raw `uint8_t*` — a pointer straight at the bytes — which is exactly
what `sendAll` needs, because the OS doesn't know or care about vectors.

---

## 6. 🟢 Eight bits, eight switches: the `Bitfield`

Here's the payoff for everything so far. BitTorrent needs to record *"which
of these 20 pieces do you already have?"* Twenty yes/no answers.

The obvious encoding is **20 bytes**. But BitTorrent packs it into **3
bytes**, because each yes/no is really just **one bit**.

The project's `Bitfield` class does this packing. Here is the single most
important line in it (`include/peer/Bitfield.hpp`):

```cpp
bool has(size_t index) const {
    if (index >= pieceCount_) return false;
    return (bytes_[index / 8] & (0x80u >> (index % 8))) != 0;
}
```

### Breaking it into pieces

`index` is a piece number. Let's ask: *do you have piece 5?*

**`index / 8` — which byte?** Pieces are grouped 8 to a byte:

```
  piece:    0  1  2  3  4  5  6  7  │  8  9 10 11 12 13 14 15  │
  byte:     0  0  0  0  0  0  0  0  │  1  1  1  1  1  1  1  1  │
            └──────── byte 0 ─────────┘└──────── byte 1 ────────┘
```

Piece 5 lives in **byte 0** (5/8 = 0). Piece 9 lives in **byte 1** (9/8 = 1).

**`index % 8` — which bit inside that byte?**

```
  piece:      0  1  2  3  4  5  6  7
  bit in byte:0  1  2  3  4  5  6  7
```

Piece 5 is **bit 5** of its byte (5%8 = 5).

**`0x80 >> (index % 8)` — build a mask for exactly that bit.**

`0x80` is `1000 0000` — the top bit. Shifting it right moves that one bit
down to the position you asked for:

```
  0x80 >> 0  =  1 0 0 0 0 0 0 0  =  0x80    ← piece 0
  0x80 >> 1  =  0 1 0 0 0 0 0 0  =  0x40    ← piece 1
  0x80 >> 2  =  0 0 1 0 0 0 0 0  =  0x20    ← piece 2
  0x80 >> 3  =  0 0 0 1 0 0 0 0  =  0x10    ← piece 3
  0x80 >> 4  =  0 0 0 0 1 0 0 0  =  0x08    ← piece 4
  0x80 >> 5  =  0 0 0 0 0 1 0 0  =  0x04    ← piece 5
  0x80 >> 6  =  0 0 0 0 0 0 1 0  =  0x02    ← piece 6
  0x80 >> 7  =  0 0 0 0 0 0 0 1  =  0x01    ← piece 7
```

**`bytes_[index / 8] & mask`** — AND that byte with the mask. If the result
isn't zero, the bit was set, so we own the piece.

**🎯 Mental model:** *find the byte, find the bit, build a mask for that bit,
AND it with the byte.* The non-zero test tells you whether the bit was on.

### This is MSB-first, and it is not optional

Notice piece 0 got `0x80` — the **top** bit — not `0x01`, the bottom one. The
project's own comment flags this as the #1 way to get it wrong:

> *"That is the opposite of 'bit 0 of byte 0 means piece 0', which is what you
> would write if you were not paying attention."*

BitTorrent's protocol specifies this order. A client that packs bits the
other way round doesn't crash — it just concludes the peer has pieces it
doesn't, and waits forever for data that never arrives.

`set()` is the mirror image, turning a bit **on** instead of testing it:

```cpp
void set(size_t index) {
    if (index < pieceCount_) bytes_[index / 8] |= static_cast<uint8_t>(0x80u >> (index % 8));
}
```

`|=` means "OR it in" — turn those bits on, leave the rest alone.

### The spare-bits trap

20 pieces need 20 bits. 20 bits is 2½ bytes, so the wire message is **3
bytes** — with **4 bits left over** in the last byte that belong to no piece.

The protocol says those must be zero. So the code checks, using another mask:

```cpp
const size_t spare = expected * 8 - pieceCount;   // 4
const uint8_t mask = static_cast<uint8_t>((1u << spare) - 1u);   // 0x0F
if ((payload[expected - 1] & mask) != 0) {
    return false;      // reject the peer
}
```

If a peer sets those phantom bits and we believed them, we'd hand out piece
indices that don't exist — so the connection is refused instead.

**🎯 The lesson:** one yes/no = one bit. Packing is easy; **agreeing on the
bit order and validating the leftovers is where the bugs live.**

---

## 7. 🟢 Byte order: the same number, two arrangements

Here is the module's real topic, and the reason for those hand-written
functions.

A 4-byte number like `0x3ADB7799` has to be stored in 4 bytes. But **there are
two sensible ways to do it:**

```
  BIG-endian    (most significant byte first)
                3A  DB  77  99
                └highest value at the lowest address

  LITTLE-endian (least significant byte first)
                99  77  DB  3A
                └lowest value at the lowest address
```

Both store the exact same number. They just disagree about which end comes
first.

**Which one does your computer use?** x86 and ARM — basically every laptop and
phone — use **little-endian**. The internet has standardised on
**big-endian** for network data, and has since 1974.

So there are two orders in play, and they will fight you.

### The project's answer

`src/peer/PeerMessage.cpp` writes a 4-byte length onto the wire:

```cpp
static void putU32BE(uint8_t out[4], uint32_t v) {
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}
```

Line by line: take the top byte, put it first; next byte, second; and so on.
**That's big-endian, written out by hand.** `getU32BE` does the reverse:

```cpp
static uint32_t getU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}
```

Read it left to right: byte 0 is the most significant, so shift it furthest
left. The `|` joins them into one number.

**🎯 Mental model:** *your CPU thinks little-endian; the network speaks
big-endian. These functions are the translator.*

---

## 8. 🟢 The bug that shipped because of this

We have a demo written for exactly this moment. Run it:

```bash
g++ -O0 -o /tmp/demo01 docs/12-tutor/demos/demo-01-byte-order.cpp
/tmp/demo01
```

It will show you this, and you'll be able to read every line of it now:

```
  tracker says   58.219.119.153        bytes  3A DB 77 99
  we dialled        153.119.219.58     ← a completely different machine
```

**What went wrong.** The original code read 4 bytes off the wire and did
arithmetic on them:

```cpp
addr.s_addr = (wire[0] << 24) | (wire[1] << 16) | (wire[2] << 8) | wire[3];
```

That expression is **correct** — it really does compute `0x3ADB7799`, which is
58.219.119.153. The author got the arithmetic right.

But `s_addr` is defined to hold bytes in **network** order. So assigning the
number to it performed a *second, unwanted* conversion — and the two
byte-swaps cancelled into a reversal.

The fix is to do **no arithmetic at all**:

```cpp
std::memcpy(&addr.s_addr, wire, 4);
```

Copy the bytes straight in. If the data came off the network and is going
back onto a socket, copying is the only safe operation — every conversion you
insert is a chance to convert twice.

> ### **WITHOUT vs WITH**
>
> ```
> WITHOUT the fix:  58.219.119.153  →  153.119.219.58    every peer
> WITH the fix:     58.219.119.153  →  58.219.119.153    correct
> ```
>
> **THE RULE** *data off the wire, going back onto a socket → `memcpy`.
> Computing a new number → shift and mask.*
>
> This project follows it in three places: `decodeCompactPeers` in
> `src/tracker/HttpTracker.cpp` for IP bytes, `PeerMessage::putU32BE/getU32BE`
> for lengths, and `putBE32/getBE32` in `src/tracker/UdpTracker.cpp`.
> **Three places, one rule.**

### Why the port was fine and the IP wasn't

The demo explains this, and it's the most interesting part. The port was
decoded to a plain number, used as a number, then converted back by
`connect()` — the round trip cancels out, so the error never showed.

The IP had no such round trip: the wrong value went *straight* into
`connect()`. **Half the address was right and half was wrong** — which is the
worst kind of bug, because the output still looks plausible.

That's also why the test suite didn't catch it: the test asserted *"we got 50
peers"*. It got 50 peers. They were the wrong 50.

**🎯 A test that checks the _count_ of untrusted data is not a test of that
data.**

---

## 9. 🟢 Reading a real message

Now you can read the wire format. `sendMessage` builds this frame:

```
  ┌─────────────┬──────────┬─────────────────┐
  │  length: 4  │  id: 1   │  payload: n     │
  │  big-endian │          │                 │
  └─────────────┴──────────┴─────────────────┘
```

Say we're sending `MSG_REQUEST` (id 6) with a 3-byte payload. `length` counts
the id byte *and* the payload, so `1 + 3 = 4`. The bytes that go on the wire:

```
  length 4      id 6      payload
  00 00 00 04   06        de ad be
  └─ putU32BE ─┘
```

A peer reads this by taking 4 bytes, `getU32BE`-ing them to learn how much to
expect, then reading exactly that many more. **That's the whole protocol.**

> ### **WITHOUT vs WITH**
>
> ```
> WITHOUT a length prefix
>   Two peers exchange bytes. One sends a REQUEST, the other reads 3 bytes.
>   Then a PEACE message arrives with no length, and the reader misinterprets
>   its id byte as payload. Everything after that is garbage. There is no way
>   to resynchronise - the stream is corrupted forever.
>
> WITH a length prefix
>   Every message is self-describing. Read 4 bytes, know exactly how many
>   follow, read exactly that many, and you are always back in sync.
>
> THE RULE  if data can arrive in variable-size chunks, put its length in
>           front of it. Never hope the reader can guess where one message
>           ends.
> ```

**And this is why `recvExact` exists.** The OS is free to deliver 1 byte when
you asked for 4, or all 4 at once, or 2 and then stall. `TcpSocket::recvExact`
loops until it has the full amount. A single naive `recv()` would silently
return a short read and corrupt the framing above.

---

## 🔵 What you don't need yet

| Skip | Why |
|---|---|
| Floating point bytes | Not used anywhere in this project. All numbers are integers. |
| UTF-8 and encoding | PeerFlow is a byte-level protocol. Text only appears in `.torrent` filenames and console output. |
| Signed arithmetic on bytes | `uint8_t` is unsigned. There is no negative byte to worry about. |
| Bitfields in the C sense (`struct { unsigned a:1; }`) | The project packs bits manually with shifts and masks, not compiler bitfields. |

---

## Check your understanding

**1.** Why does the project use `uint8_t` rather than `char` for wire data?

<details><summary>Answer</summary>

`char` is signed on this machine, so it runs −128 to 127 — but every byte
from the network is 0–255. `uint8_t` is genuinely unsigned, so it holds all
256 values correctly and needs no `& 0xFF` fixups.

</details>

**2.** In `hexOf`, what does `b & 0x0F` produce for `b = 0x3A`, and why?

<details><summary>Answer</summary>

`0x0A` (10). `0x0F` is `0000 1111`, so ANDing keeps only the low 4 bits and
discards the high 4. It's the low nibble, which is the second hex digit.

</details>

**3.** A peer has pieces 0, 1 and 9. How many bytes is that bitfield, and which bit of which byte holds piece 9?

<details><summary>Answer</summary>

2 bytes — 10 bits fits in 2. Piece 9 is in byte `9 / 8 = 1`, at bit
`9 % 8 = 1`, so it's mask `0x80 >> 1 = 0x40` — the second-from-top bit of
byte 1.

</details>

**4.** Why does `has()` use `0x80 >> (index % 8)` rather than `0x01 << (index % 8)`?

<details><summary>Answer</summary>

Because BitTorrent's wire format is **MSB-first**: piece 0 is the *top* bit of
byte 0. Using `0x01 << ...` would put piece 0 at the bottom bit, which is the
mirror image. The client wouldn't crash — it would just believe the peer has
the wrong pieces and wait forever for data that never comes.

</details>

**5.** The demo's shift expression computed the right number, yet the address came out reversed. What was the extra step?

<details><summary>Answer</summary>

Assigning to `s_addr` performs a second conversion, because that field is
defined to hold network byte order. The arithmetic conversion and the
assignment conversion cancelled into a reversal. The fix is `memcpy` — zero
conversions for data that came off the network and is going back on.

</details>

---

## What's next

You can now read hex, decode a number from bytes, pack bits, and understand
why byte order needs hand-written code. **`uint8_t` is no longer an abstract
type — it's data on a wire.**

**[Module 3 — `.torrent` Files and Bencode →](03-torrent-and-bencode.md)**

Nothing leaves your computer yet. Module 3 takes the bytes you can now read
and shows how this project parses a `.torrent` file: what the `i`, `l`, `d`
and digit bytes mean, how a string length turns into a slice, and how the
info hash is computed. Bencode is a *text* format, so this is the gentlest
possible introduction to "reading someone else's data".

After that, Module 4 finally opens a socket — and everything from here will be
bytes we've already learned to read.