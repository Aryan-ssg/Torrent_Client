# 07 — Phase 5: Peer Messages and the Piece Download

## What we built and why

Phase 4 proved we can *say hello*. Phase 5 is where actual **data** starts
moving: after the 68-byte handshake, the two sides talk a *second language* —
small, length-prefixed **messages** — and the first thing we do with it is
download one complete, **SHA-1-verified piece** of the torrent.

The milestone: `PASS: downloaded piece X/Y` — our client requests 16 KiB
blocks, assembles them, hashes the result, and *refuses* the piece if the hash
doesn't match what the `.torrent` file promised.

## The message format (everything after the handshake)

The handshake was special: a fixed 68-byte block with no length prefix. Every
message after it is **framed**: the sender first writes the message's length,
then the message itself. The frame is exactly:

```
[4 bytes: length, big-endian][1 byte: message id][payload (length-1 bytes)]
```

- The 4-byte length counts the **id byte *and* the payload**, so `length = 1`
  means "just an id, no payload" (e.g. `CHOKE`).
- `length = 0` is the **keep-alive** — a silent pulse with *no* id, sent every
  ~2 minutes to say "still here, nothing new." `readMessage()` skips these.
- The length is big-endian — the endianness lesson from Phase 3, again. The
  network never assumes the computer's byte order.

Why is this good news for us? Because the frame tells us the exact byte count
up front — so **`recvExact()` from Phase 4 is the only read skill we need**.
Every frame becomes "read 4 length bytes, then read exactly `length` bytes".
No guessing, no partial messages, ever.

## The message ids (the spec's vocabulary)

| id | name | meaning (plain-English) |
|---|---|---|
| 0 | `CHOKE` | "you may not ask me for pieces right now" |
| 1 | `UNCHOKE` | "go ahead, ask me for pieces" |
| 2 | `INTERESTED` | "I want pieces you have" |
| 3 | `NOT_INTERESTED` | "nothing you have appeals to me" |
| 4 | `HAVE` | "I just got piece number X" |
| 5 | `BITFIELD` | "here's the whole list of pieces I own" (sent right after the handshake, before anything else) |
| 6 | `REQUEST` | "please send me `index:begin`, `length` bytes" |
| 7 | `PIECE` | "here is the data you asked for" (`index`, `begin`, bytes) |
| 8 | `CANCEL` | "never mind that request" |

Our seeder conversation only needs five of these — but the *codec* for the
whole vocabulary exists, because Phase 8 (seeding) will use the other side of
the same messages.

## The REQUEST and PIECE payloads

The two messages that carry positions have a small binary body:

```
REQUEST (6):  [4: index][4: begin][4: length]        all big-endian
PIECE   (7):  [4: index][4: begin][data bytes...]
```

- **index** — which *piece* of the torrent (0-based).
- **begin** — the byte offset *inside* that piece.
- **length** — how many bytes we want (REQUEST only).

Our helpers `buildRequestPayload()` / `parseRequestPayload()` and
`buildPiecePayload()` / `parsePiecePayload()` are the little endian-wrangling
codec: the C++ `uint32_t` lives in *host* byte order; the wire wants
*big-endian*, so every number goes out through `>> 24 / >> 16 / >> 8` and comes
back the same way. The Phase 5 test round-trips exactly this: build a REQUEST
payload, parse it, and check the three numbers come back identical.

## Two constants that matter

- `kBlockSize = 16384` — the **block**: the chunk size all standard clients
  request between peers (16 KiB). A REQUEST longer than this is a violation.
- `kMaxPeerMessageSize = 1 MiB` — the biggest frame we'll accept. Anything
  bigger is either a broken peer or an attack. "Never trust the network."
  (A real Ubuntu torrent's whole-piece requests can be big, but 1 MiB is a
  sane ceiling for a single message.)

## The whole conversation: downloading one piece

This is the *full* minimal dialogue — the whole reason Phases 3 and 4 exist:

```
1. CONNECT        TcpSocket::connect(peer.ip, peer.port, timeout)
2. HANDSHAKE      68-byte hello + info_hash proof (PeerHandshake)
3. INTERESTED     "I want pieces you have"
4. UNCHOKE        ← wait until the peer answers "go ahead"
5. REQUEST        "send me piece P, block from begin, length bytes"
                  (for our 16 KiB pieces, a REQUEST is the whole piece)
6. PIECE          ← the peer replies with [index][begin][the bytes]
7. assemble + SHA-1 hash the whole thing | compare to the stored hash
```

### Step by step through `PieceDownloader::download()`

`download()` (in `src/peer/PieceDownloader.cpp`) is the tidy orchestrator.
It returns a `Result { ok, error, data }` and **never throws outward** —
exactly the Phase 4 philosophy: a bad peer should cost one report, not crash
the client.

1. **Connect + handshake.** Dial the peer, then `PeerHandshake::exchange()`.
   No matching info_hash, no piece. (Phases 4's test is reused wholesale.)
2. **Say INTERESTED**, then loop reading messages until `UNCHOKE` arrives.
   This is polite BitTorrent: you don't *ask* until you've been *allowed*.
   (We skip quickly over intermediate `HAVE`/`BITFIELD` noise.)
3. **Request the block(s).** Build a REQUEST for each block of the piece. For
   our synthetic 16 KiB pieces that is exactly one block — `index`, `begin = 0`,
   `length = block size`. For a 256 KiB Ubuntu piece it would be sixteen
   REQUESTs, one per block.
4. **Assemble the PIECE replies.** BitTorrent lets blocks arrive out of order;
   we slot each `PIECE` response in at its `begin` offset.
5. **The gate: SHA-1.** Hash the assembled piece and compare with the 20-byte
   hash we sliced out of the torrent (`TorrentFile::pieces`). Match → `ok =
   true` and the data is trustworthy. Mismatch → rejected, with an error
   message *naming* SHA-1. This is the heart of "never trust the network": an
   attacker, a faulty seeder, or a corrupted link can hand you anything, and
   this single check turns it into a refused piece rather than a poisoned file.

### What could go wrong (and how it fails honestly)

- Peer never sends `UNCHOKE` → timeout; the caller sees `ok=false`, moves on.
- Peer closes mid-block → read error surfaces as `ok=false`.
- Peer sends wrong-sized or misordered data → assembly/hash fails.
- **Peer sends garbage that hashes wrong** → the SHA-1 check, our final gate,
  catches it even if everything above behaved.

Every failure keeps the *type* of the failure in `Result::error` so tests can
assert on it (this is how the "corrupted piece rejected by SHA-1" test works).

## The FakePeer becomes a seeder

Phase 4's FakePeer answered one handshake and hung up. Now it needs to serve
data — the **seeder** behaviour that a real peer would show. Its
`handleSeeder()` follows the protocol strictly:

1. After the handshake, immediately send **BITFIELD**: one bit per piece,
   MSB-first inside each byte, all ones ("I own everything").
2. When the client says **INTERESTED**, reply **UNCHOKE**.
3. When the client sends a **REQUEST**, bounds-check it against our content
   (index/begin/length must land inside the file, length ≤ 16 KiB) and serve
   the matching slice as a **PIECE** message. A request outside the file is a
   malformed/attacking client — we simply stop talking.

Two test-friendly knobs, both explained in their chapters:
`destroyByte()` flips one byte to *prove* hash rejection, and Phase 7 adds
`setServeDelayMs()` to fake a slow modem.

The test also proves the currency of honesty: the client's SHA-1 gate rejects
a piece the *seeder knows* is wrong. Both sides distrust the data until the
hash says otherwise.

## The code, piece by piece (classes, functions, algorithms, techniques)

The same tour as Phases 1–4.

### The user-defined types

| Type | Header | Its one job | Java cousin |
|---|---|---|---|
| `PeerMessage` (**struct**) | `include/peer/PeerMessage.hpp` | one decoded message: `id` + `payload` | a DTO / record |
| `PieceDownloader` (**class**) | `include/peer/PieceDownloader.hpp` + `.cpp` | run the full conversation for ONE piece; all-`static` | a static service class |
| `PieceDownloader::Result` (**struct**) | same header | `{ok, error, data}` — verdict + the verified bytes | a result record |
| `waitFor(...)` (file-local `static`) | `src/peer/PieceDownloader.cpp` | read messages until a wanted id shows up | a polling helper |
| `putU32BE` / `getU32BE` (file-local `static`) | `src/peer/PeerMessage.cpp` | big-endian pack/unpack for 32-bit numbers | `ByteBuffer` with `BIG_ENDIAN` |
| `PeerHandshake` (reused) | `include/peer/PeerHandshake.hpp` | Phase 4's `exchange()` over an open socket | — |
| `TcpSocket` (reused) | `include/net/TcpSocket.hpp` | `sendAll` / `recvExact` under the framing | — |

A design note worth stating plainly: this layer is **mostly free functions**,
not a class. Framing (`sendMessage`/`readMessage`) and the payload codecs are
pure functions over a socket — they hold no state, so making them methods
would be noise. Only the *orchestration* (`PieceDownloader::download`) is a
class member. That's a healthy instinct: reach for a class when there's state
to own, not by default.

### The framing layer — `sendMessage` / `readMessage`

| Function | Technique | What it does |
|---|---|---|
| `putU32BE(out[4], v)` / `getU32BE(p)` | **explicit endianness conversion by bit-shifting** | write/read a `uint32_t` most-significant-byte-first |
| `sendMessage(sock, id, payload)` | **frame construction** | length = `1 + payload.size()`, `putU32BE` it into a 5-byte header with the id, `sendAll` header then payload |
| `sendMessage(sock, id)` | **overload** | id-only messages (`CHOKE`/`UNCHOKE`/`INTERESTED`) delegate with an empty payload |
| `readMessage(sock)` | **length-prefixed deserialisation** in a skip-loop | `recvExact` 4 length bytes → decode → maybe skip keep-alive → sanity-check → `recvExact` the body → split id/payload |

`readMessage` is the most interesting function in the project so far, because
it is where four defensive decisions stack up:

1. **`recvExact(lenBytes, 4)`** — the length prefix itself needs an
   exactly-N read (TCP gave us no boundaries).
2. **Keep-alive skip** — `if (length == 0) continue;`. A zero length means
   "nothing to say", so the loop reads the next frame instead of returning a
   bogus empty message.
3. **Size cap** — `if (length > kMaxPeerMessageSize) throw`. This is the
   single most important line: without it, a malicious peer could claim a
   2 GB message and we'd try to allocate it, turning a protocol attack into an
   out-of-memory crash. Bounded allocation from untrusted input.
4. **Underflow check** — `if (length < 1) throw` catches the impossible
   "length says there's no id byte" case cleanly instead of reading
   `body[0]` out of bounds.

### The payload codecs — four small functions

| Function | Technique | Notes |
|---|---|---|
| `buildRequestPayload(index, begin, length)` | **fixed-layout serialisation** | allocates 12 bytes, `putU32BE`s the three numbers at offsets 0/4/8 |
| `parseRequestPayload(payload, …)` | **out-parameters** (`uint32_t&`) + size guard | `if (payload.size() != 12) return false;` then decode |
| `buildPiecePayload(index, begin, data, len)` | fixed layout + **`std::memcpy`** for the variable tail | 8-byte header + the raw bytes |
| `parsePiecePayload(payload, …)` | **zero-copy view** | returns a *pointer into* the payload (`data = payload.data() + 8`) and a length — no copy at all |

Two techniques to notice:

- **Out-parameters.** C++ has no multiple return values, so parse functions
  write through references: `bool parse(..., uint32_t& index, uint32_t& begin,
  size_t& len)`. It reads backwards in C++ style, but it means "return
  success/failure" plus "fill in the answers" without allocating a struct.
- **Zero-copy parsing.** `parsePiecePayload` hands back a *pointer into the
  existing buffer* rather than a fresh copy — the piece bytes are then
  `insert`ed straight into the assembly buffer. For a 256 KiB piece that's
  real work avoided, and it's idiomatic C++ (Java's `ByteBuffer` views do the
  same job).

### `PieceDownloader::download()` — the orchestrator, step by step

| Step | Code | Technique |
|---|---|---|
| 0 | `if (expectedHash.size() != 20) return error` | **validate the caller's own inputs first** — a programming-error guard before any I/O |
| 1 | `socket.connect(...)` | Phase 3's `TcpSocket` (DNS + timeout) |
| 2 | `PeerHandshake::exchange(...)` | Phase 4, reused verbatim over the same socket |
| 3 | `sendMessage(socket, MSG_INTERESTED)` | protocol politeness |
| 4 | `waitFor(socket, MSG_UNCHOKE, …)` | **tolerant polling**: skip `BITFIELD`/`HAVE`/`CHOKE` chatter until the id we want arrives, with a `kMaxMessagesToWait = 200` cap so a peer that never unchokes (or floods us with noise) fails politely instead of hanging |
| 5 | `while (begin < pieceLength) { … }` | **block-by-block assembly loop** with `std::min(kBlockSize, …)` for the short tail, `reserve()` up front, and a triple check per block: `gotIndex == pieceIndex && gotBegin == begin && dataLen == blockLen` |
| 6 | `SHA1(assembled…)` + `std::equal(actual, expectedHash)` | the **hash gate** |
| 7 | `result.data = std::move(assembled)` | move, don't copy — the piece is handed over with zero copies |

Two details that show real care:

- **The `gotIndex/gotBegin/dataLen` triple-check** (step 5) is
  defence-in-depth *before* the hash: a peer that answers with the wrong piece
  number, the wrong offset, or the wrong length is rejected with a precise
  error message, so the hash never even has to run. (The hash would catch it
  anyway — but failing early with a clear reason is better engineering.)
- **`std::move(assembled)`** into `result.data`: the local buffer is being
  handed to the caller, and a move transfers the pointer instead of copying
  up to 256 KiB. (This is why `Result` is a plain struct — move-friendly by
  design.)
- The whole body sits inside one `try { … } catch (const std::exception& e)
  { result.error = e.what(); }`, continuing the Phase 4 contract: **this
  function reports, it doesn't propagate.**

### The built-in types we rely on here

| Built-in | What it gives us | Java equivalent |
|---|---|---|
| `std::memcpy` | copy the raw block into a payload buffer | `System.arraycopy` |
| `std::min` | clamp each block to 16 KiB (and the tail) | `Math.min` |
| `std::equal` | byte-exact 20-byte hash comparison | `Arrays.equals` |
| `std::move` | transfer a buffer without copying | (no direct equivalent) |
| `vector::reserve` | pre-size a buffer to avoid reallocations | `new byte[len]` up front |
| `vector::insert` | append a range to a growing buffer | `System.arraycopy` at an offset |
| out-params (`uint32_t&`) | multiple return values | a returned `Optional`/record, or a small mutable holder |
| `enum : uint8_t` (`MSG_*`) | strongly-typed message ids | `enum` / constants |
| `constexpr size_t` (`kBlockSize`, `kMaxPeerMessageSize`) | compile-time protocol constants | `static final` |
| `vector::assign(ptr, ptr+n)` | copy a sub-range into the payload | `Arrays.copyOfRange` |

### The algorithms/patterns this phase is a textbook example of

1. **Length-prefixed framing** — every message declares its own size, which is
   what makes `recvExact` sufficient forever after.
2. **Big-endian serialisation helpers** — `putU32BE`/`getU32BE` isolate the
   endianness problem in two 4-line functions used by everything else.
3. **Bounded allocation from untrusted input** — the `kMaxPeerMessageSize`
   check before `std::vector<uint8_t> body(length)`.
4. **Tolerant message polling** (`waitFor`) — skip irrelevant ids, with a
   message-count cap so noise can't become a hang.
5. **Block-wise assembly** — request 16 KiB at a time, `insert` each reply at
   its offset, `reserve()` the total up front.
6. **Validate every echoed field** — index/begin/length must match what we
   asked for before the bytes are trusted.
7. **Hash-gated acceptance** — SHA-1 against the torrent's own fingerprint is
   the only path to `ok = true`.
8. **Zero-copy parsing + move semantics** — return views into buffers, hand
   ownership over with `std::move`.
9. **Overloading for convenience** — `sendMessage(sock, id)` delegates to the
   payload version.
10. **Exception → verdict** (once more) — every failure becomes
    `Result{ok=false, error="…"}`.
11. **Reuse of earlier phases** — `TcpSocket` and `PeerHandshake` are used
    unchanged; Phase 5 adds only the new layer.

### A one-paragraph mental model

> Phase 5 is **two thin layers over Phase 4's socket**. The framing layer turns
> a byte stream into `PeerMessage{id, payload}` values: read 4 length bytes
> exactly, skip keep-alives, refuse absurd sizes, then read the body exactly
> and split off the id. The codec layer converts that payload into numbers
> (REQUEST: index/begin/length; PIECE: index/begin + a *view* of the data),
> with `putU32BE`/`getU32BE` hiding the byte-order problem. On top sits
> `PieceDownloader::download`, the orchestrator: connect → handshake →
> INTERESTED → wait for UNCHOKE (tolerantly, with a cap) → request each 16 KiB
> block and verify index/begin/length → assemble → **SHA-1 and compare** →
> return the verified bytes by move. The techniques underneath:
> length-prefixed framing, big-endian serialisation, bounded allocation,
> tolerant polling, block assembly, hash-gated acceptance, zero-copy views,
> and move semantics.

## Java parallels

This is a request/response protocol over a stream — think of building a
`DataInputStream`-style length-prefixed record reader, or `HttpURLConnection`
request → response, with an integrity check (like comparing a returned
checksum) before you "commit" the payload. The `Result` struct is a small
value object carrying `ok`/`error`/`data`, which reads a lot like a Java
record or a sealed response type.

## The real test output

```
=== Peer Message + Piece Download Tests (Phase 5) ===

PASS: request payload round-trip (index/begin/length, big-endian)
PASS: downloaded piece 1/3 (16384 bytes, SHA-1 + content verified)
PASS: downloaded piece 2/3 (16384 bytes, SHA-1 + content verified)
PASS: downloaded piece 3/3 (5000 bytes, SHA-1 + content verified)
PASS: corrupted piece rejected by SHA-1 (SHA-1 verification failed - peer sent corrupted piece)
```

Reading it:

- The round-trip test nails the *codec* (big-endian `index/begin/length`).
- The three downloads cover two **full 16 KiB pieces *and* the shorter final
  piece (5000 bytes)** — the famous "last piece is shorter" edge case.
- The corrupted-piece test is the security proof: data that hashes wrong is
  refused, and the error *mentions SHA-1* so we know the gate did its job.

## What Phase 5 taught us

- **Message = frame + id + payload**: length-prefixed, big-endian, and
  `recvExact()` handles it all.
- **Polite protocol**: INTERESTED → UNCHOKE → REQUEST → PIECE. Sequence
  matters; we wait for permission before asking.
- **SHA-1 is the last gate**: every byte that lands in our file has been
  hashed and matched against the torrent's own fingerprint.
- **Short final pieces are normal** — the piece length is a *maximum*, not a
  guarantee.

---

*Next: [08 — Phase 6: Pieces to Disk](08-phase6-pieces.md)*