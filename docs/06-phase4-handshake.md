# 06 — Phase 4: The Peer Handshake

## What we built and why

Phase 3 handed us a list of peer addresses. Now we dial one of them and prove
we both want the *same* torrent. That proof is a fixed exchange of bytes called
the **handshake (a short, predetermined "hello" ritual between two clients so
they agree on exactly what they're about to discuss)**.

The milestone: `Handshake OK with <ip>:<port>, peer_id = ...` — the moment our
code opens a real TCP connection and *successfully completes* the protocol's
opening act.

## Why a handshake at all?

Peers are strangers on the internet. Before trading a single piece of data,
both sides must answer one question, and answer it *before anything else
flows*:

> **"Are we both talking about the same torrent?"**

You both show your **info_hash** (Phase 2's fingerprint) inside the very first
bytes. If the two hashes differ, the connection is pointless — you'd be
arranging pieces of *two different files*. So the identity exchange happens
**first, in one fixed 68-byte block**, and only after that proof may any other
message begin.

One honest clarification, because correctness matters here: the handshake proves
*agreement*, not *authenticity*. Both sides asserting "same hash" guards against
accidentally joining the wrong swarm — it is **not** a cryptographic
authentication of the peer's identity. The hash isn't signed. On the public
internet, a malicious peer could falsely claim your hash; the real defence
against bad *data* (not bad *identity*) is per-piece **verification against the
signed-ish hashes** — that fight happens in Phases 5–6. For now: the handshake's
job is simply "confirm we're both at the right meeting."

You can think of it in REST terms (Java-side framing: like sending a magic token
as the *first* field of the very first request, and checking that the response
echoes your token back before trusting anything). In BitTorrent it is both
simpler and stricter: fixed size, fixed layout, and the acceptance test is
byte-exact.

## The 68-byte handshake layout (the whole ritual)

Both sides send the *same* shape of message. Ours:

```
Byte  0          : 19                       ← the protocol name's length
Bytes 1 .. 19    : "BitTorrent protocol"    ← the protocol's name (19 chars)
Bytes 20 .. 27   : 8 reserved bytes (0x00)  ← "I support no extensions (yet)"
Bytes 28 .. 47   : info_hash (20 bytes)     ← the torrent I want (Phase 2!)
Bytes 48 .. 67   : our peer_id (20 bytes)   ← who I am
──────────────────────────────────────
Total            : 1 + 19 + 8 + 20 + 20 = 68 bytes
```

The **reply** uses the same 68-byte shape with the last two fields *swapped*
in role: the peer echoes *its* info_hash (bytes 28–47) and *its* peer_id
(bytes 48–67).

### Reading it byte by byte — the real wire bytes

This is the *actual* 68 bytes our client sends for the Ubuntu test torrent
(info hash `4a3f5e08...330599`, fake peer id `-PF0001-000000000000`):

```
offset   hex bytes                                 ASCII
00   13 42 69 74 54 6f 72 72 65 6e 74 20 70 72 6f 74   .BitTorrent prot
10   6f 63 6f 6c 00 00 00 00 00 00 00 00 4a 3f 5e 08   ocol........J?^.
20   bc ef 82 57 18 ed a3 06 37 23 05 85 e3 33 05 99   ...W....7#...3..
30   2d 50 46 30 30 30 31 2d 30 30 30 30 30 30 30 30   -PF0001-00000000
40   30 30 30 30                                       0000
```

Mapping offset → meaning (a ruler you'll use again in Phases 5–6):

```
00  0x13                    → length 19 (the "handshake marker")
01  42 69 74 54 … ("BitTorrent protocol", 19 bytes, ends at offset 0x13)
14  (0x14=20) 8×00          → reserved: all extensions off (8 bytes, 20..27)
1C  (0x1C=28) 4a 3f 5e 08 … → OUR info_hash, 20 bytes (28..47)
30  (0x30=48) 2d 50 46 …    → peer_id, 20 bytes (48..67)
```

Notice two beauties:

- The first two fields are almost "pure prose" — `19` + a 19-character name —
  a quick sentinel that instantly distinguishes a real torrent client from
  anything else (like a web server) that happens to be listening.
- `0x13`, `0x14`, `0x1C`, `0x30` aren't random: `19`, `20`, `28`, `48` are
  exactly where each field starts. The hex dump's structure *is* the spec.

### The reserved bytes — the future's switchboard

Why 8 bytes of zeros at 20..27? They are a **capability flag switchboard (a
bitmap: 64 individual bits, each a switch for an optional protocol feature)**.
All zeros says "I support no extensions." In later phases we'll *flip specific
bits* (e.g. magnet metadata exchange) to opt in. The byte *position* of each
feature is fixed by the ecosystem — so today's zeros are tomorrow's options.

## Building ours: `buildHandshake()`

The constructor in code is exactly the layout, written down in order:

```cpp
handshake += static_cast<char>(19);        //  1 byte
handshake += "BitTorrent protocol";        // 19 bytes
handshake.append(8, '\0');                 //  8 bytes  reserved zeros
handshake.append(infoHash.begin(),
                 infoHash.end());          // 20 bytes
handshake += ourPeerId;                    // 20 bytes
```

That's the *whole* handshake construction: five appends in the spec's order.
There is no framing, no length-prefixing *of the handshake itself* — its length
is a constant, 68. (In Phase 5, every *other* message **will** be
length-prefixed with a 4-byte count — the handshake is the one exception, which
is why its size is fixed and known.)

## Step by step through `PeerHandshake::perform()`

The function mirrors the wire behaviour you'd want from a real client:

```
1. dial the peer            → TcpSocket::connect(peer.ip, peer.port, timeout)
2. build & send 68 bytes    → sendAll()
3. read EXACTLY 68 bytes    → recvExact()        ← the streaming trap
4. verify reply in order:
     reply[0]     == 19
     reply[1..19] == "BitTorrent protocol"
     reply[28..47]== OUR info_hash      ← the acceptance test
5. extract peer_id          → reply[48..67]
```

### The streaming trap (third appearance, now automatic)

TCP is a **stream**. The peer's 68 bytes may arrive in *any* number of `recv()`
calls: 68 at once, 5 then 63, 1 then 67, 25+43, … If we read "whatever is
available" once, we might capture half a handshake and misinterpret garbage —
checking a *partial* info hash would even pass/fail wrongly. So
`recvExact()` loops, asking the OS repeatedly until the counter reaches 68:

```cpp
size_t got = 0;
while (got < wanted) {
    int n = recv(..., wanted - got);  // may return fewer than asked!
    got += n;
}
```

**"Read exactly N bytes" is the skill every BitTorrent message from here on
uses** (all later messages are length-prefixed, so we always know N up front).
We built it once in `TcpSocket`; `PeerHandshake` is its first consumer beyond
the tracker.

### Timeouts — the swarm is full of ghosts

A tracker's list is populated with plenty of peers that are dead, firewalled,
or sleeping. Each attempt must **give up gracefully**:

- `connect()` carries a timeout (a dead peer → connect eventually errors).
- `recvExact()` fails fast if the timeout's `recv()` errors rather than
  hanging forever (the "filtered = silent drop" reality from Phase 3).

Our tests use modest per-peer timeouts so 25 attempts don't take 25 minutes to
fail — each failure is *reported* in one line and we move on immediately.

### The acceptance test — byte-exact, both directions

The single check that decides "same swarm?":

> **reply bytes 28..47 must equal our `info_hash`, every one of the 20 bytes.**

Compare with `std::equal`, not with string semantics: it's a raw byte
comparison, tolerant of any byte values. Reasons to reject:

- The peer claims *another* hash → it serves a different torrent (or is a
  probe listening for anyone). Either way, talking further is pointless **and**
  potentially hostile.
- The peer never answers → timeout path, `ok=false`, try the next address.

The mirror-image honesty: *we* checked the peer's hash, and the peer checks
*ours* the same way on their side. Two agreeing hashes in both directions =
both sides in the same swarm.

### What if the peer talks gibberish?

Every failure mode funnels into one shape:

```cpp
struct Result {
    bool ok = false;        // handshake accepted?
    std::string peerId;     // the peer's 20-byte identity (if ok)
    std::string error;      // plain-English reason (if !ok)
};
```

`perform()` **never throws outward** — it wraps everything (connect errors,
timeouts, mid-read closes, wrong byte, wrong name, wrong hash) in a
`try/catch` and returns a `Result`. This is deliberate: *callers* want a
verdict on one peer, not an exception that aborts the swarm loop. A bad peer is
an annoyance, not a catastrophe — **try the next one**. (The Java habit
"exception = failure, return value = maybe" is inverted here on purpose: for
per-peer facts, a returned verdict is checkable; exceptions are for *our*
programming errors.)

## The FakePeer: proving it works without the internet

Here's the honest problem we hit on this machine:

> **The firewall only lets us reach ports ~80/443. Tracker peers listen on
> random ports (6881, 51413, …), and every connection attempt was silently
> dropped** — we verified even `1.1.1.1:6881` times out. So real-swarm
> handshakes can't complete *from this network*, no matter how correct our
> code is.

The professional answer: **build a fake peer on your own computer and test
against it.** **Loopback (the virtual network address `127.0.0.1` — literally
"this very machine" — where traffic never leaves the box and is never
firewall-filtered)** gives us the full TCP experience without the internet.

`FakePeer` is a miniature *server* that plays the peer side. This is also our
first taste of Phase 8, where *we* will be the server for real downloaders, so
the code doubles as a preview of `socket()` → `bind()` → `listen()` →
`accept()`.

### The server-side socket trinity

For a client, `connect()` is one call. For a server, it's an assembly line:

```
socket()   → create the endpoint (AF_INET + SOCK_STREAM = TCP)
bind()     → reserve a door (address + port) for this socket
listen()   → "start accepting visitors"; the OS queues them
accept()   → wait for ONE caller; returns a NEW socket
               dedicated to that caller
```

Three details make `FakePeer` behave like a good citizen:

- **`SO_REUSEADDR`** — lets us re-bind even if a previous run left the port in
  **TIME_WAIT (the TCP state where a just-closed connection lingers so stray
  last packets can arrive)**. Production clients set this so restarts aren't
  painful.
- **Port `0`** — "OS, pick any free port for me." We then call `getsockname()`
  to ask which one it chose. This guarantees the test never clashes with real
  services. (Java: `ServerSocket(0)` does the same trick.)
- **`htonl(INADDR_LOOPBACK)` / `ntohs()`** — the network-order conversions
  from Phase 3's endianness lesson, now for the server side: the OS expects its
  addresses in big-endian even on a little-endian CPU.

### If it returns 0, then what?

Once a caller arrives, `handleConnection()` runs the **same** checks we demand
of peers — symmetry by design:

1. read exactly 68 bytes (`readExact`, server-side twin of `recvExact`);
2. first byte `19`? protocol string right?
3. **their info_hash (bytes 28..47) must equal *our* torrent's** — otherwise
   it's someone probing the wrong torrent; politely hang up;
4. acceptance: reply with our own 68-byte handshake (our info_hash + our
   *known*, fixed peer_id) — a real reply a real client would accept.

Everything is *deterministic*: fixed peer_id, fixed info_hash, no internet, no
flaky timing. Run it a thousand times, same result.

### Why a thread?

`acceptLoop()` blocks waiting for a connection. If the test ran that in the
foreground, it could never *connect* at the same time — deadlock by
construction. So the server runs on a **thread (a separate path of execution
within one process, running concurrently with the main one)**:

- main thread: `connect()` to `127.0.0.1:fakePort`,
- the new thread: `accept()`, verify, reply.

`join()` (the thread counterpart) makes the main thread *wait* for the worker
to finish before asserting `accepted() == 1`. This is our first taste of
concurrency — and a seed for Phase 7, where many peers get handled in parallel
and threads become the engine of download speed.

## The real test output

```
=== Peer Handshake Tests ===

Trying to handshake with up to 25 peers...
Handshake failed with 191.166.219.106:6881: Could not connect to ...
... (all real swarm peers unreachable from this network) ...

Handshakes OK: 0/25

--- Deterministic loopback test (FakePeer) ---
Handshake OK with 127.0.0.1:35485, peer_id = -PF0001-000000000000

PASS: loopback handshake verified (68-byte layout + info_hash check)

Passed: 20
Failed: 0
```

Reading the two halves:

1. **Real-swarm attempts prove the *failure* path** works: 25 unreachable
   peers, 25 clean one-line reports — no hang, no crash. That graceful
   degradation is exactly the production behaviour a client must have when it
   meets the real (partly-dead) internet.
2. **The loopback handshake proves the *success* path**: correct 68-byte
   construction, correct `recvExact`, correct info_hash acceptance, correct
   peer_id extraction. `-PF0001-000000000000` is the ID the fake peer announced
   with.

On a permissive network, the *same code* pointed at real addresses prints the
same `Handshake OK with <real-ip>, peer_id = ...` line. The milestone needs
nothing but an environment that permits peer ports.

## What Phase 4 taught us

- **Bytes are the protocol**: 68 exact bytes, layout-checked field by field.
- **TCP streams** mean exactly-N-byte reads (`recvExact`) — the recurring
  theme of the project.
- **Info hashes are the identity check** — never skip it, never trust a
  mismatched reply.
- **Timeouts + graceful failure** turn a hostile swarm into an annoyance.
- **Test doubles (a stand-in for a real system, like a stunt double)**: when
  the real world is unreachable, fake it *faithfully* on loopback — this is
  how real protocol software gets tested everywhere.

---

*Next: [07 — Reference](07-reference.md)*