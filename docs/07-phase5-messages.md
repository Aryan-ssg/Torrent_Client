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