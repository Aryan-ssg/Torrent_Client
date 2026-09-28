# 05 — Phase 3: The Tracker

## What we built and why

The tracker is the **matchmaker** from our story — the directory that answers
the age-old question "where are the people who have what I want?". We send it
a message called an **announce (a registration: "here I am, this is the
torrent I want, this is how to reach me" — plus a question: "who else is
here?")**, and it answers with a list of **peers (the addresses of other
computers currently sharing the same torrent)**.

The tracker does **not** store the file itself — never a byte of upload. It
stores *addresses*. Think of a real neighbourhood party: the matchmaker at the
door has a clipboard of who has the extra chairs (peer list); the chairs
themselves stay in people's homes.

The milestone: `Peers found: 50` — a real, working peer list obtained from a
real tracker over the real internet — printed by **our own networked C++
code**, built from raw sockets with nothing hidden.

## The whole flow in one picture

```
 We (PeerFlow)                           Tracker
    │                 announce                           │
    │  -GET /announce?info_hash=... HTTP/1.1───────────► │
    │                                                    │
    │  ◄────── 200 OK + bencoded body ────────────────   │
    │            { interval: 3502,
    │              complete: 48, incomplete: 2,
    │              peers: <N*6 raw bytes> }               │
    │                                                    │
    │   decode the 6-byte peers → list of ip:port        │
```

To talk to the tracker we must understand several **layers** of the internet —
each of which this project implements *ourselves* (no library hides the
magic):

```
HTTP    (the language of the website request)
TLS/SSL (the encryption lock — the tracker URL is https://)
TCP     (the reliable pipe)
socket  (our program's plug into that pipe)
DNS     (finding the tracker's IP from its name)
```

We go through each layer bottom-up, then reassemble the whole machine.

## Layer 1 — IP addresses and ports (the destination)

- Every computer on the internet has an **IP address (a unique house
  address)**. Today there are two families:
  - **IPv4 — four bytes**, written dotted-decimal, e.g. `191.166.219.106`.
    Four bytes = 4 billion addresses, which is *why* it's running out.
  - **IPv6 — sixteen bytes**, written as eight groups of hex, e.g.
    `2606:4700::1111`. The answer to "four billion isn't enough".
- A computer runs *many* programs at once. Each program listens on a **port
  (a numbered door on that house)**, a number from 1–65535. Web servers use
  door **443** (HTTPS); BitTorrent peers commonly use door **6881**.

So a full destination is written **`IP:PORT` (house : door)** — `IP` says
*which building*, `port` says *which door of that building*. Both must be
right; `191.166.219.106:6881` and `191.166.219.106:80` are different
computers' doors.

> Two quiet facts that matter later: the address `127.0.0.1` is **your own
> machine** (the "local-only" door — we use it in Phase 4's tests), and every
> address has special reserved ranges that aren't routable on the internet.

## Layer 2 — DNS: the internet's phonebook

We don't want to memorize `104.16.43.2`; we want `tracker.opentrackr.org`.
**DNS (Domain Name System — the internet's phonebook)** is the distributed
service that resolves a **hostname (a human-readable name for a machine)** into
one or more IP addresses. When you open a browser to a URL, DNS work has
already happened before any data moves.

In code we call `getaddrinfo()` — C's swiss-army-knife resolver, the same
idea as Java's `InetAddress.getAllByName()`. You hand it a hostname *or* an IP
literal and a service (port number or name), plus **hints (a description of
what kind of connection you want)**:

```cpp
struct addrinfo hints {};
hints.ai_family = AF_UNSPEC;     // "IPv4 OR IPv6, whichever exists"
hints.ai_socktype = SOCK_STREAM; // "I want TCP, not UDP"
getaddrinfo(host, port, &hints, &results);
```

Two nice properties:

- One call returns *every* usable address (IPv4 **and** IPv6) for that name —
  so if the tracker's `:443`-IPv4 is unreachable, we try its IPv6.
- Passing a *literal* IP like `"127.0.0.1"` also works — `getaddrinfo` accepts
  either, so Phase 4's loopback tests use the *same* code path.

The hint `{}` (an empty brace initializer) is worth understanding: it means
"start every field at zero" — otherwise struct fields would hold garbage
values. Zeroing means "no particular preference", which is what we want.

## Layer 3 — TCP and sockets

**TCP (Transmission Control Protocol — a reliable, ordered, two-way pipe)**
is the workhorse underneath nearly everything (web, email, BitTorrent). It
gives us:

- **Reliability.** Every byte we send arrives — *in order, once, uncorrupted*.
  TCP re-sends lost data automatically, like a courier who re-delivers a lost
  parcel until you sign for it.
- **A byte stream, not packets.** The OS hands us a continuous stream with
  **no boundaries** between what the other side "sent" vs. what we "receive".
  If the other side sent 68 bytes, `recv()` may hand us 25, then 30, then 13 —
  any split. **This single fact drives the `recvExact()` pattern that every
  later phase depends on, so we'll keep coming back to it.**

To open that pipe, C++ has no standard socket class (Java's `java.net.Socket`
covers all this for free). We wrote ours. A **socket (our program's endpoint
of a TCP pipe)** is delivered as a **file descriptor (a small integer the OS
uses to identify an open resource — a number, not an object)**.

```
socket()   → create the endpoint
connect()  → dial the remote house+door
send()     → push bytes in
recv()     → pull bytes out
close()    → hang up
```

The three-way handshake: `connect()` performs a TCP dance ("hello",
"hello-back", "agreed"), after which our descriptor and the remote's are
directly wired.

### The `TcpSocket` class (reusable: Phases 3, 4, and everything after)

Both the tracker (Phase 3) and peers (Phases 4+) need identical socket
behavior, so it lives in `src/net/TcpSocket.cpp`. The essentials:

- **`connect(host, port, timeout)`** — DNS + dial, *with a timeout* so a dead
  server can't hang the program forever. It loops through every address
  `getaddrinfo` returned, trying each until one `connect()` succeeds.
- **`sendAll(data, len)`** — the OS may accept only *part* of a big `send()`;
  this loops until every byte is accepted.
- **`recvExact(data, len)`** — the loop that keeps calling `recv()` until
  exactly `len` bytes have arrived:
  ```
  got = 0
  while got < len:
      n = recv(buf+got, len-got)   // n may be smaller than what's missing!
      got += n
  ```
  This is *the* building block of all BitTorrent messaging, because everything
  on the wire is length-prefixed ("4 bytes telling you how long the next
  message is", "68 bytes for the handshake", …). You ask for a precise number,
  the helper makes the OS deliver it.
- **`recvSome(data, len)`** — one single receive attempt: returns bytes read
  (>0), `0` on a clean close (EOF), `-1` on error/timeout.

### The lesson of ownership: why sockets can't be copied

A `TcpSocket` owns an OS resource — the file descriptor. Now think about what
"copying" would mean: two objects both believing *they* own the same `fd`.
Both would `close()` it — the second close would kill a socket we might have
reused, or worse. C++ lets us say so explicitly:

```cpp
TcpSocket(const TcpSocket&) = delete;             // copying: forbidden
TcpSocket& operator=(const TcpSocket&) = delete;
TcpSocket(TcpSocket&& other) noexcept;            // moving: allowed
```

**Move semantics (transferring ownership of a resource from one object to
another, leaving the source empty)** is the C++ idiom for "this thing can't be
duplicated, but it can be handed over." The move constructor "steals" `fd`, and
marks the old object's `fd = -1` (`-1` = "I own nothing") so its destructor
won't close a socket it no longer owns. This is how a function can *return* a
connected socket: **return-by-value triggers a move, not a copy.** The
`Connection` struct in `HttpTracker.cpp` composes a `TcpSocket` with an `SSL*`
and gets the same move-only treatment, so a freshly-connected (and TLS-
wrapped) connection can be handed back from a factory function cleanly. The
Java instinct "an object is a reference, just pass it around" hits the C++
reality: **some resources cannot be duplicated — only moved.**

### Timeouts: the first law of "never trust the network"

```cpp
struct timeval tv;
tv.tv_sec = timeoutSeconds;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
```

**Timeout (a deadline: if the other side doesn't answer within N seconds, give
up)** — swarms are full of dead or firewalled computers. Without timeouts, the
first dead peer would freeze the whole client forever. With them set, a
silent peer makes `recv()`/`send()` return an error instead of hanging.
"Never trust the network" starts with **don't wait forever.**

Even the error handling has a subtlety: `EINTR` means "a signal interrupted
your call — it wasn't a real error, retry." Our `recvSome`/`sendAll` retry on
`EINTR` and treat everything else as an error wrapped in a `NetException`.

## Layer 4 — HTTP: the language of the web

**HTTP (HyperText Transfer Protocol — a request/response protocol: the client
says a method + a URL, the server answers with a status + a body)**. It is
plain text riding over TCP. Our announce request, *byte for byte what goes on
the wire*:

```http
GET /announce?info_hash=%B4...&peer_id=-PF0001-... HTTP/1.1\r\n
Host: tracker.opentrackr.org\r\n
User-Agent: PeerFlow/0.1\r\n
Accept: */*\r\n
Connection: close\r\n
\r\n
```

**(`\r\n` = carriage-return + line-feed, the two-byte "end of line" the
protocol mandates.** HTTP text is picky: it demands `\r\n`, not just `\n`.
This is the kind of detail that mangles a request if missed — and it's the
reason the header block is always followed by the double newline `\r\n\r\n`.)

Anatomy of a response:

```http
HTTP/1.1 200 OK\r\n
Content-Type: text/plain\r\n
...\r\n
\r\n
d8:intervali3502e...                 ← the body: our bencoded peer list!
```

We parse three things from it:

1. **Status line.** `HTTP/1.1 200 OK` — `200` = success, `30x` = "redirect,
   go look elsewhere", `4xx/5xx` = errors. (`301` = permanently moved,
   `302`/`303`/`307` = temporary redirect — all mean "follow that `Location`".)
2. **Headers.** `Name: value` lines. We lower-case header names and keep them
   in a map. We care about `Location` (redirect target) and
   `Transfer-Encoding` (chunked framing).
3. **Blank line `\r\n\r\n`**, which splits headers from body, then the **body**
   itself.

### Two HTTP wrinkles we handle by hand

**1. Redirects.** Some trackers redirect us (opentrackr sends `http://` →
`https://`). On a `30x` we look up `Location` and **resolve it** against the
current URL: `Location` can be absolute (`https://other/announce`) or
root-relative (`/announce`), so we build "%scheme://%host:%port" + that path
ourselves. We cap the chase (give up after 3 redirects) so a broken server
can't loop us forever.

**2. Framing — how do we know the body has ended?**
- We send `Connection: close`, promising the server it may hang up when done;
  that makes "body ended" equal "the pipe closed" — so our `readAll()` simply
  reads until EOF (`recvSome` returns 0). This is the simplest correct framing
  for small tracker responses.
- Some servers ignore that and use **chunked transfer encoding (a body broken
  into labelled blocks: `<hex-size>\r\n<bytes>\r\n` each, ending with a
  zero-sized block)**. We decode it (`strtoul(..., 16)` reads the hex size,
  then we copy that many bytes). Being prepared for both is cheap insurance.

> Why hand-roll HTTP at all? Because **every step is visible and ours**. You
> could link `libcurl` and write this whole phase in 10 lines — but then the
> entire stack would be a black box. *"Nothing hidden" is the point of this
> project.*

## Layer 5 — HTTPS / TLS: the padlock

The tracker URL is `https://` — HTTP's encrypted sibling. The "S" is provided
by **TLS (Transport Layer Security — a cryptographic handshake that turns a
plain-text pipe into an encrypted one: both sides now write into a locked box
whose key only they share)**.

The handshake, from our side's point of view:

1. We open a plain TCP connection to port 443 (Layer 3's work).
2. In the TLS greeting we send **SNI (Server Name Indication — a field saying
   "I'd like to speak to `tracker.opentrackr.org`, please"** — necessary
   because one server hosts many sites behind many certificates, like a
   building with one reception desk and many tenants).
3. The server responds with its **certificate (a digital ID card: the
   server's public key, signed by a Certificate Authority)**.
4. We verify that chain against our **trust store (the OS's list of trusted
   Certificate Authorities — the "godfathers" everyone agrees to believe)**,
   and we confirm the name *on the card* matches the hostname we asked for.
5. If all checks pass, both sides derive the same secret session keys. From
   then on, `SSL_write`/`SSL_read` replace `send()`/`recv()`.

In code, `upgradeToTls()` does steps 1–5:

```cpp
SSL_set_fd(ssl, c.socket.fd());       // wrap the connected socket
SSL_set_tlsext_host_name(ssl, host);  // SNI
SSL_set1_host(ssl, host);             // verify cert name == hostname
SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr); // truly check the chain
SSL_connect(ssl);                     // run the handshake
```

We **always** set `SSL_VERIFY_PEER` — this is what turns n believe-we-are-who-
we-say encryption into *verified* encryption. Without hostname verification,
you'd happily encrypt your data to *whoever* answers. One shared `SSL_CTX`
(created once, loading `/etc/ssl/certs`) is reused for every connection; the
`Connection` destructor frees the `SSL*` — same RAII discipline as the socket
itself.

## The announce request: what we actually send

### The parameters

| Parameter | Value in our code | Plain English |
|---|---|---|
| `info_hash` | the 20 raw bytes from Phase 2 | the torrent's ID card *in raw binary* |
| `peer_id` | 20 bytes like `-PF0001-…` | *our* ID card (below) |
| `port` | 6881 | the door *we* will listen on later (Phase 8) |
| `uploaded` | 0 | bytes we've uploaded so far |
| `downloaded` | 0 | bytes we've downloaded |
| `left` | the file's size | bytes still to download |
| `compact` | 1 | "give me peers in the tiny 6-byte format" |
| `event` | `started` | "I'm just arriving" (`completed`/`stopped` come later) |

### peer_id — who we say we are

A **peer_id (a 20-byte identifier each client invents for itself)** follows a
widely-used convention called the Azureus style:

```
-PF0001-  x7k2p 9m4q …
└─┬──┘    └────┬──────┘
 8 chars     12 random chars   = 20 bytes total
```

- `-PF…-` advertises "PeerFlow version 0.001", like a browser's `User-Agent`.
- The **12 random characters** come from `std::random_device`
  (OS-level randomness, `like /dev/urandom`), drawn from a fixed alphabet.
  Uniqueness matters: the tracker tells *us apart* by `peer_id`, and two
  clients sharing an ID confuse it.

### URL encoding — why `%XX` is everywhere

The announce is a URL. But `info_hash` is **raw binary** — twenty bytes that
will certainly include *illegal* URL characters: `?`, `&`, `#`, spaces, and
invisible control bytes. So every "unsafe" byte becomes `%HH`, its two hex
digits:

```cpp
if (isUrlSafe(c)) out += c;      // A-Z a-z 0-9 - _ . ~ stay as-is
else { out += '%';
       out += hex[b >> 4];       // high nibble ("tens digit")
       out += hex[b & 0x0F];     // low nibble  ("units digit")
}
```

The encode keeps only the **RFC 3986 "unreserved" set** (`A-Za-z0-9-._~`);
everything else is percent-encoded so the tracker can decode back the *exact*
original bytes. Example: the byte `\xB4` → `%B4`; almost the whole info_hash
becomes `%XX` chains.

**The join trap (`?` vs `&`):** some tracker URLs already carry credentials,
e.g. `https://tracker.example/announce?passkey=abc123`. If we blindly appended
`?info_hash=...`, the server would read `passkey=abc123?info_hash=...` as one
garbage value. So `buildAnnounceUrl()` checks: already a `?` → join with `&`,
else `?`. One line, but exactly the difference between "works" and "the
tracker never sees our info_hash".

## The tracker's reply: bencode again!

The response *body* is **bencoded — our Phase 1 decoder reads it**, closing a
beautiful loop (the decoder that opened this project now parses the internet's
answer). The keys:

| Key | Meaning | We store in |
|---|---|---|
| `interval` | seconds to wait before re-announcing | `interval` |
| `min interval` | suggested minimum wait | `minInterval` |
| `complete` | how many peers have the whole file (seeders) | `complete` |
| `incomplete` | how many peers are still downloading (leechers) | `incomplete` |
| `peers` | the actual peers (see below) | `peers` |
| `failure reason` | if present, we were *rejected* | `failureReason` |
| `warning message` | optional advisory note | `warningMessage` |

If `failure reason` appears, we **stop and report it** — the classic cause is
a bad `info_hash`, i.e. our Phase 2 hash *must* be byte-perfect or the tracker
can't tell which torrent we want.

### Compact peers: 6 bytes per peer — and endianness

When `compact=1`, `peers` is one binary blob where **every 6 bytes are one
peer**:

```
[ byte 1 ][ byte 2 ][ byte 3 ][ byte 4 ] [ byte 5 ][ byte 6 ]
   └───────── IPv4 address ─────────┘      └── port ──┘
```

`191.166.219.106` = bytes `191 166 219 106` (one byte per dotted number). The
port `6881` = hex `0x1AE1`, stored as bytes `0x1A 0xE1`.

**Endianness — the byte-order trap every network programmer remembers.** Our
CPU natively stores multi-byte numbers **little-endian (least-significant byte
first)**: the number `0x1AE1` in memory is bytes `E1 1A`. But the *internet*
standard is **big-endian (most-significant byte first)**: bytes `1A E1` —
like "tens-digit then units-digit" vs "units then tens". If we naively read
`1A E1` as a number, our machine would see `0xE11A` = wrong by a lot. So we
reassemble **by hand, deliberately big-endian**:

```cpp
addr.s_addr  = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];  // IPv4
uint16_t port = (b[4] << 8) | b[5];                                // port
```

That mental model — "the wire is big-endian, my CPU is little-endian, *I*
must flip" — recurs in every phase, so it's worth keeping tight. We also guard
the blob: its size **must be a multiple of 6** or we throw (a stray byte means
a corrupt or forged response — never trust the network). And we support the
older **non-compact format** (a list of `{ip, port}` dictionaries) as a
fallback, for tracker-response flexibility.

## The real-world findings (things the internet taught us)

This phase confronted us with reality — worth remembering:

1. **The Ubuntu tracker blocks us.** `https://torrent.ubuntu.com/announce`
   answered:
   > `failure reason: Requested download is not authorized for us with this
   > tracker`
   It restricts by IP range and only serves certain networks. **This is not a
   bug in our code** — real clients get rejected by trackers all the time. We
   support it gracefully (the test prints the failure reason instead of
   crashing), which is exactly what a correct client does.
2. **`tracker.opentrackr.org` works over HTTPS.** It answered with
   `complete=48, incomplete=2, interval=3502, peers=<N×6 bytes>`.
3. **Filtered vs. refused is an important distinction.** A *refused*
   connection sends you an explicit "no" (`RST`); a *filtered* one just
   swallows your packets and leaves you waiting (hello, timeouts from Layer 3).
   Roughly half of any real swarm is filtered or offline at a given instant,
   which is why the Phase 4 sweep tries up to 80 peers and stops at the first
   success.
4. **⚠️ We once blamed the network for our own bug.** This test originally
   only *counted* the peers it decoded, so it happily reported `Peers found:
   50` while every address inside was byte-reversed by a Phase 3 bug in
   `decodeCompactPeers()` (see Phase 4's FakePeer section). Counting is not
   verifying — and "the network is blocking us" is a claim you have to earn.
   There is now an offline regression test that pins the exact decoded bytes.

## `127.0.0.1` — your own computer's local address

Never filtered, because traffic to it never leaves the machine. Phases 4–7
lean on it heavily.

## Building the whole thing: `HttpTracker`

The full pipeline, top to bottom:

```
1. TrackerRequest.buildAnnounceUrl()  → the full GET URL (params, encoding, ?-join)
2. httpGet(url)                       → DNS → TCP → TLS → HTTP → redirects → body
3. BencodeDecoder::decode(body)       → a BencodeValue dictionary (Phase 1!)
4. read interval/complete/incomplete  → TrackerResponse fields
5. decodeCompactPeers(peers blob)     → vector<Peer> → "Peers found: N"
```

`TrackerResponse` bundles the answer:

```cpp
struct TrackerResponse {
    std::vector<Peer> peers;          // ip:port objects
    long long interval, minInterval, complete, incomplete;
    std::string failureReason, warningMessage;
};
```

And `Peer` — the tiniest star of the show — is a plain address pair:

```cpp
struct Peer {
    std::string ip;      // e.g. "191.166.219.106"
    uint16_t port;       // e.g. 6881
    std::string toString() const { return ip + ":" + std::to_string(port); }
};
```

## The code, piece by piece (classes, functions, algorithms, techniques)

The same tour as Phases 1–2: every class and function, every built-in we lean
on, and the named techniques this phase is an example of.

### The user-defined types

| Type | Header | Its one job | Java cousin |
|---|---|---|---|
| `TrackerRequest` (**struct**) | `include/tracker/TrackerRequest.hpp` | the announce parameters + `buildAnnounceUrl()` | a DTO with a `toUrl()` method |
| `TrackerResponse` (**struct**) | `include/tracker/TrackerResponse.hpp` | the tracker's parsed answer | a response DTO / record |
| `Peer` (**struct**) | `include/tracker/Peer.hpp` | one address pair: `ip` + `port` | a tiny value object |
| `HttpTracker` (**class**) | `include/tracker/HttpTracker.hpp` + `.cpp` | speak HTTP(S) to a tracker, all-`static` | a static service class |
| `TcpSocket` (**class**) | `include/net/TcpSocket.hpp` + `.cpp` | the reusable socket primitive | a non-cloneable `Socket` |
| `Url` (file-local `struct`) | `src/tracker/HttpTracker.cpp` | the four pieces of a parsed URL | a parsed-URI record |
| `HttpResponse` (file-local `struct`) | `src/tracker/HttpTracker.cpp` | status + header map + body | an HTTP response object |
| `Connection` (file-local `struct`) | `src/tracker/HttpTracker.cpp` | a `TcpSocket` + optional `SSL*` | a session wrapper |
| `NetException` | `include/net/NetException.hpp` | "the network failed" | a custom `IOException` |

Notice the pattern: **the three data carriers are `struct`s (public fields, no
behaviour), while the two workers (`HttpTracker`, `TcpSocket`) are `class`es
with private state.** And four of the nine types live *inside*
`HttpTracker.cpp` — implementation detail, deliberately not in any header.

### `TrackerRequest` — DTO + a method that does string surgery

- **Fields**: `announceUrl`, `infoHash` (20 bytes), `peerId` (20 bytes),
  `port`, `uploaded`, `downloaded`, `left`, `compact`, `event`.
- **`buildAnnounceUrl()` (const member function)** — glue `?`/`&` + every
  parameter. The subtle bit is the *conditional separator*:

```cpp
bool alreadyHasQuery = (announceUrl.find('?') != std::string::npos);
url += alreadyHasQuery ? '&' : '?';
```

Technique: **query-string assembly with separator detection** — many tracker
URLs already carry a passkey (`…/announce?passkey=abc`); blindly adding `?`
would silently swallow our parameters into that value.
- `const` on the method means it *reads* the struct without modifying it.

### The free functions `generatePeerId()` and `urlEncode()`

- **`generatePeerId()`** — builds the 20-byte ID: the fixed prefix `"-PF0001-"`
  (8 bytes: `-` + client code `PF` + version `0001` + `-`), then pads to 20 with
  random characters from a 36-char alphabet. Technique: **`std::random_device`**
  (OS entropy, like Java's `SecureRandom`) plus modulo indexing into an
  alphabet. The randomness matters: the tracker uses `peer_id` to tell us apart
  from other clients.
- **`urlEncode()`** — percent-encoding, one `%XX` per unsafe byte. Two
  overloads (`vector<uint8_t>` and `string`; the string one converts and
  delegates — **overloading**). It splits on `isUrlSafe()`: RFC 3986's
  *unreserved* set `A-Za-z0-9-._~` passes through, everything else becomes
  `hex[b >> 4]` + `hex[b & 0x0F]` (high nibble, low nibble). This is *why* the
  binary info hash can travel in a URL at all.

### `TcpSocket` — the reusable primitive (the DRY lesson)

The class exists because tracker code and peer code need *identical*
socket behaviour. Its design is the interesting part:

- **Deleted copy, allowed move** — `TcpSocket(const TcpSocket&) = delete;`
  A socket owns an OS file descriptor; two objects closing one fd is a bug.
  Moves are implemented by *stealing* `fd_` and setting the source to `-1`
  ("empty"). This is the **Rule of Five / resource ownership** pattern (here
  only the destructor + move ops are user-written).
- **Destructor = cleanup** — `~TcpSocket() { close(); }` (RAII: the
  descriptor is released no matter how the function exits).
- **`connect(host, port, timeout)`** — the multi-address DNS loop:

```cpp
for (struct addrinfo* ai = results; ai; ai = ai->ai_next) {
    int fd = ::socket(...);                       // try this address
    setsockopt(fd, ..., SO_RCVTIMEO/SO_SNDTIMEO);  // arm the timeout
    if (::connect(fd, ...) == 0) { fd_ = fd; break; }  // keep the winner
    ::close(fd);                                    // else close and retry
}
```

Techniques: **`getaddrinfo` resolution** (one call that may return several
IPv4/IPv6 addresses — the C equivalent of Java's
`InetAddress.getAllByName()`), the **try-each-address loop** (happiness
pathways), **socket options for I/O timeouts** (`SO_RCVTIMEO`/`SO_SNDTIMEO`
turn a black-hole peer into an error instead of a hang), and `freeaddrinfo`
to avoid leaking the DNS list.
- **`sendAll()` / `recvExact()` / `recvSome()`** — the two-part read/write
  discipline the whole project rests on. `sendAll` loops (send may take
  *part* of your buffer; `EINTR` is retried). `recvSome` is one attempt
  (returns >0 bytes, `0` = clean EOF, `-1` = error/timeout) and `recvExact`
  loops on top of it until it has *exactly* `len` bytes — the TCP-stream fix.

### `HttpTracker.cpp` — the file-local machinery

| Function | Technique | What it does |
|---|---|---|
| `parseUrl(raw)` | manual tokenising; `rfind` + default ports | split `https://host:port/path?q` into `Url{tls, host, port, pathAndQuery}`; default 443/80 |
| `resolveLocation(base, location)` | **relative-URL resolution** | turn a redirect's `Location` (absolute or `/path`) into a full URL |
| `sendAll(c, data)` / `recvSome(c, …)` | **polymorphic wrapper** over "raw socket *or* TLS" | if `c.ssl` use `SSL_write`/`SSL_read`, else the plain socket — one API, two transports |
| `readAll(c)` | read-until-EOF loop | slurp the whole reply (we send `Connection: close`) |
| `decodeChunked(body)` | **stateful wire-format un-chunker** | undo HTTP chunked encoding: `<hex-size>\r\n<bytes>` repeatedly, stop at size 0 |
| `performRequest(c, text)` | **header/body splitting** + `istringstream` parsing | send, read, split at `\r\n\r\n`, parse the status line (`atoi`) and `Name: value` headers (lowercased keys), then de-chunk if needed |
| `connectTo(u, timeout)` | factory returning by value (moved out) | wrap `TcpSocket::connect` into a `Connection` |
| `sslContext()` | **function-local `static` (Meyers singleton)** | one shared `SSL_CTX*` created once, trust store loaded, reused for all connections |
| `upgradeToTls(c, host)` | **TLS with real verification** | `SSL_set_tlsext_host_name` (SNI), `SSL_set1_host` (name check), `SSL_VERIFY_PEER` (chain check), then `SSL_connect` |
| `httpGet(url, maxRedirects)` | **redirect-following loop** | for up to N hops: connect, TLS, GET, follow 301/302/303/307, return the final response |
| `decodeCompactPeers(blob, out)` | **fixed-size record decoding** | split the blob into 6-byte records; `memcpy` the 4 address bytes (already network order) and reassemble the 2 port bytes (see below) |
| `HttpTracker::announce(req, maxRedirects)` | the **orchestrator** | `httpGet` → check 200 → bencode-decode body (Phase 1!) → pull fields → decode peers → `TrackerResponse` |

Three of these deserve a beat:

- **`Connection` — the move-only session wrapper.** It holds a `TcpSocket`
  plus an `SSL*`, deletes its copy constructor (two owners of one connection =
  disaster) and implements a **move constructor** that nulls the source's
  `ssl`. Its destructor `SSL_free`s. So `Connection` is returned *by value*
  out of `connectTo` and moved around safely — modern C++ resource handling.
- **The shared `SSL_CTX` (Meyers singleton).** A `static` local inside
  `sslContext()` is initialised **once, thread-safely, on first use** — the
  C++ idiom for "a Java `static final` field, but lazy."
- **Compact-peer decoding, the endianness payoff.** Six bytes per peer:
  4-byte big-endian IP + 2-byte big-endian port. The code reassembles them by
  hand — `(b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]` for the IP and
  `(b[4]<<8)|b[5]` for the port — then `inet_ntop` formats the address as text
  and pushes a `Peer`. The `size % 6` check up front is "never trust the
  network": a malformed blob is rejected before we ever index into it.

### The extraction pattern (same defensive habit as Phase 2)

`announce()` pulls `interval`, `complete`, `peers`, … with the identical
`dict.find(key)` + `getType()` + "ignore if wrong type" approach. Two
protocol-specific rules on top:

- **`failure reason` short-circuits everything** — if the tracker rejects us
  (usually a wrong info hash), return immediately with the reason and ignore
  any peers.
- **`peers` has two shapes** — a *string* (compact 6-byte blob) or a *list* of
  dicts (`compact=0`). The code branches on `getType()` and handles both, so
  it works with old trackers too. A nice example of **polymorphic input
  handling**.

### The built-in types we rely on here

| Built-in | What it gives us | Java equivalent |
|---|---|---|
| `getaddrinfo` / `addrinfo` | DNS resolution returning many addresses | `InetAddress.getAllByName()` |
| `setsockopt` (`SO_RCVTIMEO`…) | I/O timeouts | `socket.setSoTimeout()` |
| `inet_ntop` | binary IP → `"1.2.3.4"` text | `InetAddress.getHostAddress()` |
| `std::istringstream` + `getline` | line-by-line header parsing | `BufferedReader.readLine()` |
| `std::strtoul(…, 16)` | parse a hex chunk size | `Integer.parseInt(s, 16)` |
| `std::map<std::string,std::string>` | the (lowercased) HTTP header map | `Map<String,String>` |
| `std::random_device` | OS randomness for peer_id | `SecureRandom` |
| `std::string::rfind/find/substr` | URL tokenising | `String.indexOf/substring` |
| OpenSSL `SSL_CTX` / `SSL` | TLS session | `SSLSocketFactory` / `SSLSocket` |
| move semantics + `= delete` | single-owner resources | non-`Cloneable`, `AutoCloseable` |
| RAII destructors | automatic cleanup | `try-with-resources` |

### The algorithms/patterns this phase is a textbook example of

1. **Layered pipeline** — DNS → TCP → TLS → HTTP → bencode → peers, each layer
   a separate function with a clear contract. (The "onion" of the internet,
   peeled one function at a time.)
2. **The happy-pathway loop** — try every resolved address until one connects.
3. **Timeouts as a design requirement** — `SO_RCVTIMEO`/`SO_SNDTIMEO` so a dead
   peer costs seconds, not minutes.
4. **Partial-I/O loops** — `sendAll` and `recvExact`: the discipline of
   assuming the network gives you *less* than you asked for.
5. **Percent-encoding (URL encoding)** — turning arbitrary binary into legal
   URL text and back.
6. **Conditional separators in query assembly** — the `?` vs `&` trap.
7. **Singleton via function-local `static`** — one lazily-created `SSL_CTX`.
8. **Move-only resource wrappers** — `Connection` and `TcpSocket` own OS/TLS
   resources; copies are *deleted*, moves hand ownership over.
9. **Polymorphic transport (one API, two backends)** — `sendAll`/`recvSome`
   work over plain TCP *or* TLS via `if (c.ssl)`.
10. **Redirect resolution with a hop cap** — following `Location` safely
    (absolute or root-relative) up to `maxRedirects`.
11. **Wire-format decoders** — HTTP chunked bodies and 6-byte compact-peer
    records, each with a `size % n` sanity check.
12. **Hand-written endianness conversion** — big-endian IP/port reassembly by
    bit-shifting.
13. **Failure short-circuit + defensive field extraction** — the Phase 2
    find-then-type-check habit, now applied to untrusted tracker replies.
14. **Reuse (DRY)** — `TcpSocket` extracted from the tracker's private code so
    Phases 4–7 share one socket implementation.

### A one-paragraph mental model

> Phase 3 is a **layered pipeline of small, testable functions** wearing a
> handful of C++ resource-safety tools. `TrackerRequest` assembles a properly
> encoded URL; `parseUrl` tokenises it; `connectTo` + `sslContext` +
> `upgradeToTls` open a *verified* TLS connection (one lazily-created, shared
> context; one `Connection` that owns its socket and SSL state and can only be
> *moved*); `performRequest` writes raw HTTP, reads to EOF, splits headers, and
> un-chunks; `httpGet` loops through redirects; `announce` bencode-decodes the
> body with Phase 1's decoder, pulls fields defensively, and
> `decodeCompactPeers` reads fixed 6-byte records into `Peer` objects. The
> techniques underneath: happy-pathway loops, partial-I/O loops,
> percent-encoding, a function-local `static` singleton, move-only RAII
> wrappers, and hand-rolled endianness conversion.

> **The byte-order trap (cost us one real bug).** The obvious way to rebuild
> the address looks right and is wrong:
>
> ```cpp
> addr.s_addr = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];  // NO
> std::memcpy(&addr.s_addr, b, 4);                                   // yes
> ```
>
> The shifts build the correct *number* (`0x3ADB7799` = 58.219.119.153), but
> `in_addr::s_addr` is **already stored in network byte order** — assigning a
> host integer to it writes the bytes in the machine's native little-endian
> order, so `inet_ntop()` read them back reversed and we dialled
> `153.119.219.58`. The *port* was still correct, which is exactly why nobody
> noticed. The tracker's bytes are already in wire order; the only correct
> move is to copy them straight in. The lesson generalises: **before
> hand-assembling an address, check whether the destination field is already
> in network order — if it is, don't shift, just copy.**

## The real test output

```
Announce URL:  https://tracker.opentrackr.org/announce?info_hash=%4A%3F...&...
Talking to tracker...
interval:      3502s
seeders:       48
leechers:      2
Peers found:   50
   191.166.219.106:6881
   23.183.106.176:42000
   ...

PASS: tracker announce succeeded
```

(50 this run vs. 48 earlier — the swarm changes constantly; that's exactly why
`interval` tells us when to ask again.)

## Things to remember from Phase 3

- The tracker **never carries file data** — only addresses.
- **Big-endian** everywhere on the wire; reassemble compact peers by hand.
- HTTP is text; TLS wraps it; TCP is a *stream*; `recv` returns partial data;
  therefore **`recvExact` loops**.
- Timeouts + length checks = "never trust the network" in action.
- The tracker expects a **byte-perfect `info_hash`** — Phase 2's raw-bytes
  walking pays off here.
- We now hold a **list of real peer addresses**, the very input Phase 4's
  handshake needs.

---

*Next: [06 — Phase 4: Peer handshake](06-phase4-handshake.md)*