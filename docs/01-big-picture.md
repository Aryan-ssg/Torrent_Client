# 01 — The Big Picture: What a Torrent Client Actually Does

## The one-sentence summary

A **torrent client (a program like uTorrent, qBittorrent, Transmission, or
-- the one we're writing -- PeerFlow)** downloads a file by getting tiny
pieces of it from many other people's computers at the same time, instead of
downloading the whole file from one big server.

The magic is in that phrase: *"from many other people's computers at the same
time."* Every person who ever downloaded (or is downloading) the file becomes
a mini server for it. The more people share, the **faster** everyone gets the
file, and the **more reliable** it becomes. That's the opposite of a normal
website, where one server gets slower the more people visit it.

## The pizza analogy 🍕

Imagine you want a giant 6-gigabyte pizza by dinner, but no single restaurant
in town will hand you the whole pizza from one place. Handing you all 6 GB
costs them money and **bandwidth (the amount of internet traffic you can push
in a second — think of it as the width of a pipe; a wider pipe moves more
water, just as more bandwidth moves more data per second)**. One restaurant
would also be a single point of failure: if their kitchen is on fire, your
dinner is gone.

So instead, the town has invented this system:

1. **Somebody writes a recipe card (the `.torrent` file).** The card does
   **not** contain the pizza. It contains *instructions*:
   - *"Here is the matchmaker's address."* (Where to ask about neighbours.)
   - *"Here is the pizza's size and how it's sliced."* (File size, slice size.)
   - *"Here is a secret code for every single slice."* (The checksums used to
     verify each slice.)
   - *"Here is a fingerprint of the whole recipe."* (The info hash — the unique
     "this exact pizza" identity.)
2. **You take the card to a matchmaker (the tracker).** The matchmaker keeps a
   list of everyone currently cooking this pizza: who is still slicing (and
   which slices they have), and who has the whole pizza done.
3. **The matchmaker gives you a list of neighbours (peers)** — addresses of
   homes where the pizza is being or has been cooked.
4. **You knock on doors and trade.** You ask neighbour A for slice #42,
   neighbour B for slice #7, and so on. Everyone contributes different slices,
   so the whole pizza comes together from hundreds of homes rather than one
   restaurant. *While you cook your own slices, you also hand slices you
   already have to neighbours who ask — that's the unspoken contract of the
   neighbourhood.*
5. **You verify every slice** against the secret codes on the recipe card, so
   nobody can give you a fake or poisoned slice and get away with it.
6. **When dinner is ready, you keep your door open**: now YOU are the
   neighbour with a finished pizza who helps the next person finish theirs.

That is *exactly* how BitTorrent works — slices = pieces, secret codes =
SHA-1 hashes, recipe fingerprint = info hash, matchmaker = tracker,
neighbours = peers.

> **The single most important sentence in this whole project:** the `.torrent`
> file never contains the data. It contains *instructions for getting the data*,
> and the fingerprints to verify it. The data always lives in the swarm.

## Direct download vs. torrent — the honest comparison

| | Direct download (a website) | Torrent |
|---|---|---|
| Where the data lives | one server | everywhere (the swarm) |
| Speed for you | limited by that one server | limited by the *sum* of many peers |
| Speed for everyone else | slows down as more people download | speeds up as more people download |
| If the server dies | download breaks forever | download continues (peers remain) |
| What can be verified | almost nothing | every single piece, independently |
| Big problem | needs lots of money for bandwidth | needs people to keep sharing after they're done |

That last row is why trackers and "seed ratio" (we'll define both below) exist:
torrents only work if people keep their doors open after finishing.

## A brief history: why did someone invent this?

In 2001 **Bram Cohen** looked at a real problem: big files (Linux ISOs, videos,
games) were costly to host, and download speeds from a single server were
terrible for everyone. He designed BitTorrent so that **every downloader is
also an uploader**. The rules he chose:

- The file is split into **pieces** so work and verification are fine-grained.
- There is a **tracker** so strangers can find each other (the internet had no
  built-in "who is sharing what" service).
- The **tit-for-tat** rule punishes leeches and rewards sharers, so swarms stay
  healthy — without it, everyone would only download and nobody would upload.

Today BitTorrent is handled by many separate, compatible programs because the
*protocol* (the official set of rules on the wire) is a public standard. Our
PeerFlow speaks that same protocol to those same programs.

## The five jobs, in order

```
Read the .torrent  →  Find peers  →  Talk to peers  →  Download pieces  →  Save + share
    Phase 1-2          Phase 3        Phase 4           Phase 5-6           Phase 6-8
```

| # | Job | Plain-English description | Why in this order? | Phase |
|---|---|---|---|---|
| 1 | **Read the `.torrent`** | Open the recipe card and understand it: who the matchmaker is, how big the file is, the slice size, the verification codes, the recipe fingerprint | You can't ask for anything until you know *what* the thing is. **This is the foundation of everything.** | 1–2 |
| 2 | **Find peers** | Ask the matchmaker (tracker): "Who else is cooking this?" | You can't knock on doors you don't know exist. | 3 |
| 3 | **Talk to peers** | Introduce yourself properly (the handshake) and agree you're both making the *same* pizza | Strangers must prove trust before trading — otherwise you might trade slices from two different pizzas. | 4 |
| 4 | **Download pieces** | Request slices, receive them, verify each one with its secret code | This is the whole point — but you can only do it after 1–3. | 5–6 |
| 5 | **Save + share** | Write slices into a real file on disk, and give slices to others too | A finished download is useless until it's a real file; a finished client is rude if it doesn't share back. | 6–8 |

Jobs 4 and 5 overlap in time with each other and with job 2 (you announce
again periodically to find new peers). Real clients run many of these at once.

## The cast of characters (full glossary)

| Term | What it is | Plain-English |
|---|---|---|
| **BitTorrent** | the whole file-sharing system | the neighbourhood system + its rulebook |
| **Torrent** | the sharing session for one file | "our pizza night" — identified by an info hash, not just the file |
| **`.torrent` file** | the small recipe card | contains metadata, **never** the file itself |
| **Meta-data** | data describing other data | "the file is 6 GB" is *about* the file, not part of it |
| **Info hash** | a 20-byte fingerprint of the recipe's key part | the unique ID everyone uses to name the same torrent |
| **Tracker** | a central matchmaker server | keeps the list of who's cooking what |
| **Announce** | the message telling a tracker you exist and asking for peers | registering your address at the matchmaker's office |
| **Swarm** | all peers sharing one torrent | the whole neighbourhood cooking that one pizza |
| **Peer** | any computer in the swarm | a neighbour |
| **Seeder / seed** | a peer that has 100% of the file | done cooking, now just sharing |
| **Leecher** | a peer still downloading | still cooking — but still shares what it has |
| **Peer_id** | a 20-byte name each client chooses | an identity card that tells trackers and peers who you are |
| **Piece** | a fixed-size slice of the file (e.g. 256 KB) | the unit of *verification* — each piece has its own checksum |
| **Block** | a sub-slice of a piece (16 KB) | pieces are too big to ask in one go; blocks are the unit of *transfer* |
| **Piece length** | the size of one piece | the pizza-slice size |
| **Checksum / hash** | a fixed-size fingerprint of data | changes completely if even one byte changes; used to verify slices |
| **Upload** | sending data to someone else | giving a slice away |
| **Download** | receiving data from someone else | taking a slice |
| **Bandwidth** | your data-pipe width | how much data per second you can send/receive |
| **Protocol** | the official rules of how messages are formatted | the etiquette every client must obey to talk to every other client |
| **Client** | a program that speaks the protocol | us! and qBittorrent, uTorrent, Transmission… |
| **Choking** | refusing to upload to someone (temporarily) | keeping your door shut to a freeloader |
| **Unchoking** | starting to upload to someone again | opening the door because they started sharing back |

## Why download from strangers at all? (the deeper answer)

**Speed.** Your download speed becomes the *sum* of many peers' upload speeds,
not the limit of one server. Ten neighbours each share at 1 MB/s → the whole
file arrives at up to 10 MB/s.

**Reliability.** Files survive as long as even one seed remains, even if the
original publisher disappears. This matters for old, obscure, or "long-tail"
content (things few people want) that a big server would never bother hosting.

**Fairness.** The system is self-balancing: you get slices only as long as the
neighbours feeding you believe you're feeding others too. This single rule is
what keeps the whole thing alive — the tragedy would otherwise be "everyone
downloads, no one uploads, everyone starves."

**Cost.** Publishers pay almost nothing to serve millions — the peers do the
serving.

**Resumability.** Because the file is pieces, an interrupted download resumes
cleanly from where it stopped, and damaged pieces can be re-fetched instead of
restarting the whole file.

## The barter rule, in more detail (choking & unchoking)

Peers are not charities. A peer will only upload to you if you give it a
reason. The mechanism:

1. You tell a peer you're **interested** (you'd like its pieces).
2. The peer decides whether to **unchoke** you (start sending) or **choke**
   you (refuse for now). It wants the peers that upload to *it*.
3. This is **tit-for-tat (you scratch my back, I scratch yours)**: it uploads
   to roughly the few peers that upload the fastest to it.
4. Every so often, a client runs an **optimistic unchoke (a random "free
   trial" upload)** — otherwise brand-new peers with no reputation would never
   get a first slice, and could never earn anyone's trust.

Choking is, importantly, **temporary and polite** — it's a refusal to send *to
you right now*, not an insult or a ban. The maths of "who uploads to whom" is
one of the most interesting parts of real clients (Phase 8 tackles uploading).

## A minute-by-minute walk-through (one story, all the terms)

> **18:00** — You download `ubuntu.iso.torrent` from a website. Your client
> (PeerFlow) reads the card (Phase 1–2): matchmaker is `tracker.opentrackr.org`,
> file is 6.2 GB, slices are 256 KB, and it computes the recipe's fingerprint
> (the info hash).
>
> **18:00:01** — PeerFlow **announces** to the tracker: *"I exist, I want this
> exact pizza (here's the info hash), here's my ID (peer_id) and address
> (port)."* The tracker replies: *"Here are 50 neighbours."* (Phase 3.)
>
> **18:00:03** — PeerFlow **handshakes** with the first neighbours: *"Same
> pizza? / Same pizza. Good — let's talk."* (Phase 4.)
>
> **18:00:04** — PeerFlow starts asking for **blocks** of **pieces**: 16 KB
> bites at a time, from whichever neighbours have them, checking each piece's
> checksum as it completes. Meanwhile it announces again later to refresh the
> neighbour list, and it *returns* slices to the peers that feed it.
>
> **18:14** — All 6.2 GB verified and written to disk as `ubuntu.iso`
> (Phases 5–6). PeerFlow announces **`completed`** to the tracker and keeps
> running as a **seed**, sending the finished slices to whoever asks.

## The internet plumbing you'll need (expanded)

To understand Phases 3–4, you need a working model of the internet at four
levels. Think of sending a letter by courier:

**1. IP address — the house address of a computer.**
Every device connected to the internet has an **IP address (Internet Protocol
address — a unique numeric house address)**. The classic kind (IPv4) looks
like `203.0.113.7` (four numbers 0–255). Because we're running out of IPv4
addresses, a newer kind (IPv6) exists, e.g.
`2001:db8::ff00:42:8329`, but the idea is identical. For our client, we treat
both the same (`getaddrinfo` handles both).

**2. Port — the numbered door on that house.**
Inside one house there are many programs. A **port (a number 1–65535 that the
OS uses to decide which program deserves an incoming packet)** is that
decision point. Web servers listen on **443** (https) or **80** (http);
BitTorrent peers often listen on **6881** or **51413**. A full destination is
written `address:port` — house, then door: `203.0.113.7:6881`.

> The router (the house's post room) uses (address, port) to deliver each
> packet to exactly one program — so two different programs on the same
> machine can receive different mail simultaneously.

**3. DNS — the internet's phone book.**
Humans prefer names (`tracker.opentrackr.org`), computers prefer numbers.
**DNS (Domain Name System — a distributed global phone book)** translates a
name into its current IP address. It's "distributed" because no one computer
owns it; lookup queries hop through a hierarchy of name servers. Our client
asks the OS for the translation via `getaddrinfo`.

**4. TCP + sockets — the reliable two-way pipe.**
**TCP (Transmission Control Protocol — the rulebook for splitting data into
packets, numbering them, sending them, and putting them back together exactly
in order)** is what guarantees that if a program sends "HELLO", the receiving
program gets "HELLO" — nothing lost, nothing duplicated, nothing out of order.
(There's a simpler sibling, UDP, which neither orders nor re-sends lost data,
and is used later for UDP trackers and DHT — Phase 9.)

A **socket (a program's endpoint of a TCP pipe — a number the OS hands back
when the program asks for a connection)** is how our C++ code holds onto the
pipe. The mini-ritual our `TcpSocket` performs: `socket()` create, `connect()`
dial, `send()` / `recv()` talk, `close()` hang up. The same ritual repeats in
Phase 3 (to the tracker) and Phase 4 (to each peer).

**5. HTTP (and the padlock on top, HTTPS).**
**HTTP (HyperText Transfer Protocol — the request/response language of the
web)** is how a browser asks a server for a page: "GET /something please."
Trackers speak HTTP too. **HTTPS adds TLS (an encryption handshake that seals
the pipe)** so nobody on the path can read or forge the messages — that's what
the padlock icon in your browser means. The Ubuntu tracker and opentrackr both
require HTTPS, so Phase 3 implements the TLS handshake with OpenSSL.

One nuance worth foreshadowing: TCP is a **stream, not a collection of tidy
envelopes**. If a program sends 68 bytes, the pipe may deliver them as 68, or
as 5 + 63, or 1 + 67… Our client must therefore *read exactly the number of
bytes it expects* — the **`recvExact`** habit you'll see everywhere from Phase
4 onward. Remembering "TCP = a stream" will save you a lot of confusion.

*Don't worry — every layer gets its own full section in the phase where we
actually use it.*

## What the recipe card protects you from (and what it doesn't)

**It protects your data.** Each piece has a checksum from the original file.
If any peer hands you damaged or invented bytes, the checksum fails and you
throw that piece away and fetch it from someone else. "Never trust the
network" — you *can't* be fooled into saving a corrupt file.

**It does not protect you from .torrent itself.** If someone gives you a
`.torrent` file that describes malware, you're downloading malware *reliably*.
Torrents guarantee **integrity (the bytes you got are the bytes the card
describes)** — not **authenticity (that the file is what the name claims)** or
**safety (that it isn't malicious)**. Those safeguards belong to the sites
that publish `.torrent` files, not the protocol itself.

## The internet plumbing, in one picture

```
 Your computer                    Tracker (opentrackr.org = 172.247.108.70)
┌─────────────┐                   ┌─────────────────────────────┐
│ PeerFlow    │  DNS: "where is   │ HTTP/HTTPS GET /announce... │
│ (this swarm │—— tracker"?──────►│ (webserver listening on    │
│  member)    │◄— "IP 172.247.…"──│  port 443)                 │
│             │—— connect:443 ───►│                             │
│             │◄—— 200 OK + peer  │                             │
└─────────────┘   list ───────────┘                             │
                      │ peer addresses (IP:port)
                      ▼
┌─────────────┐  TCP to   ┌─────────────┐
│ Peer #1     │◄─────────►│ Peer #2     │  (other swarm members)
│ 203.0.113.7 │ handshake │ 198.51.100.9│
│     :6881   │   + data  │    :51413   │
└─────────────┘           └─────────────┘
```

Wait — one honest simplification in that drawing: **the tracker never sits in
the middle of the data.** After it hands you addresses, traffic flows *directly*
between you and peers (there's a variant, the UDP tracker, but even that only
carries addresses). The tracker's only job is matchmaking.

## Why the data only flows when we talk the same language

The magic that makes *every* client (uTorrent, qBittorrent, PeerFlow…) able to
swap slices is that they all obey one published **protocol**. A protocol is a
specification of byte layouts and message meanings — like everyone agreeing
that a pizza slice is 256 KB and the secret code on it is 20 bytes. Our
handshake, our messages, and our piece requests (Phases 4–6) are written
against that public specification so our client can trade with real-world
clients.

## What we have built so far

| Phase | Milestone reached | Proof |
|---|---|---|
| 0 | Project builds and runs | `./build/peerflow` prints test results |
| 1 | Decode bencode data | 16 bencode tests pass |
| 2 | Parse a real `.torrent` file + compute its info hash | Ubuntu torrent parsed, hash shown |
| 3 | Ask a real HTTPS tracker for peers | "Peers found: 50" |
| 4 | Complete a peer handshake | "Handshake OK with …, peer_id = …" (against our local test peer) |

All **20 tests pass**. Chapters 02–07 take each piece of this picture apart,
one layer at a time, until every byte on the wire is one you understand.

---

*Next: [02 — Phase 0: Setup](02-phase0-setup.md)*