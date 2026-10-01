# Module 1 — The C++ You Need to Read This Project

> **Goal of this module:** you will be able to open any header in
> `include/` and understand its shape — what the types mean, what `&` and
> `const` are doing, and why a class deleted its copy constructor.
>
> Still no networking. That is Module 4.

---

## The good news first

Most C++ tutorials teach you a huge language. **You do not need most of it.**

I checked what this project actually uses. Here is the complete list of C++
features that appear in its 8,569 lines of header and source code:

| Feature | Used here? |
|---|---|
| classes, structs, member functions | yes, constantly |
| `const` in five different positions | yes, constantly |
| references (`&`) and pointers (`*`) | yes, constantly |
| `std::string`, `std::vector` | yes, constantly |
| `enum`, `enum class` | yes |
| `static_cast` | yes, 183 times |
| `namespace`, `using` aliases | yes |
| `constexpr`, `static constexpr` | yes |
| lambdas (`[&]`) | yes, a dozen |
| **inheritance / `virtual`** | **3 places — all exception classes** |
| **smart pointers (`shared_ptr`)** | **never** |
| **templates of your own** | **never** |
| **`string_view`, `std::array`** | **never** |

**🎯 Mental model:** this is a C++ program written in a deliberately plain
style. That is a gift. You are learning the parts of C++ that are actually
load-bearing, and skipping the parts that are only interesting to language
lawyers.

(Of those 8,569 lines, about 1,550 are the test suite. The feature list above
excludes the tests' own complexity, which doesn't change any of this.)

---

## 1. 🟢 `std::` — where types come from

Every C++ line in this project that names a standard-library type spells it
with a `std::` prefix:

```cpp
std::string          std::vector         std::size_t
std::mutex           std::function       std::chrono::seconds
```

**What is it?** `std::` means "from the standard library." C++ keeps its
built-in types (`int`, `char`, `bool`) separate from its library types
(`string`, `vector`, `mutex`).

**Java parallel:** In Java, `String` is the only string type and lives in
`java.lang`, which is imported automatically. C++ has no `java.lang`. If
something isn't built in, you must qualify it with `std::` or write a
`using` line.

```cpp
using std::string;      // now you can write string instead of std::string
```

**This project never writes that.** It always uses the full `std::` prefix.
That is a deliberate style choice and it means you will always know where a
type came from.

### 🟡 `namespace` — what the `::` really means

`std` is a **namespace**: a named box of related names. `chrono` is a
namespace *inside* it, which is why you see `std::chrono::seconds`.

| C++ | Java |
|---|---|
| `std::string` | `java.lang.String` |
| `std::chrono::seconds` | `java.time.Duration` |

**🎯 The rule:** read `A::B::C` as *"C, which is in B, which is in A."* You
will not be reading the standard library's source, so you never need to know
what's inside any of these boxes — only that the prefix before them is
`std`.

---

## 2. 🟢 Types: the numbers, and why they are spelled oddly

You have seen `uint8_t` many times in the other docs. Here is what it is.

**A byte is not the same thing as a `char` or an `int` in C++.** It is its own
type, with a deliberate name so the width is impossible to misread:

| Type | Meaning | Bytes | Signed? |
|---|---|---|---|
| `uint8_t` | one byte, 0–255 | 1 | no |
| `uint16_t` | two bytes, 0–65535 | 2 | no |
| `uint32_t` | four bytes | 4 | no |
| `uint64_t` | eight bytes | 8 | no |
| `int32_t` | four bytes, can be negative | 4 | yes |
| `size_t` | a count — "how many things" | machine-sized | no |

**`uint8_t` means "unsigned integer, 8 bits."** Unsigned means it cannot be
negative: it goes 0 to 255 instead of −128 to 127. The `_t` suffix just means
"type."

**Java parallel:** Java's `byte` is signed (−128 to 127) and you have to
write `(b & 0xFF)` constantly to get an unsigned byte. **C++ gives you a
proper unsigned byte type.** This project uses `uint8_t` 228 times for
exactly this reason — every byte read off a socket is 0–255, never negative.

### 🟡 `size_t` — the type for "a count"

`size_t` appears 273 times, more than any other type. It means *"the size of
something"* and it is what you get from `.size()`:

```cpp
std::vector<uint8_t> payload;
size_t n = payload.size();      // always size_t, never int
```

This project does **not** use Java's habit of putting every count in an
`int`. A torrent file can describe more pieces than an `int` comfortably
holds, and sizes never go negative, so `size_t` is both safer and more honest.

### 🔵 Signed vs unsigned, and why `recvSome` returns `ssize_t`

One exception. Look at `TcpSocket::recvSome`:

```cpp
ssize_t recvSome(void* data, size_t len);
```

The **parameter** is `size_t` (a count: "read up to this many bytes"). The
**return value** is `ssize_t` — **signed** — because it has to be able to say
*error*:

```
  >0  bytes read
   0  the other side closed cleanly
  -1  error or timeout
```

**🎯 The rule:** an unsigned type cannot hold −1, so any function that reports
errors with a number has to return a *signed* type. You'll meet this pattern
again in Module 4.

---

## 3. 🟢 References (`&`) — the one big difference from Java

This is the concept that trips up every Java programmer, so it gets the most
space. You need two forms.

### Pass by reference — the "no copy" version

Compare two functions from the same file, `include/peer/PeerMessage.hpp`:

```cpp
void sendMessage(TcpSocket& sock, uint8_t id, const std::vector<uint8_t>& payload);
```

```cpp
PeerMessage readMessage(TcpSocket& sock);
```

Look at the parameter types. `TcpSocket&` is a **reference**. Read it as:

> *"This function needs the actual `TcpSocket` object, not a photocopy of it."*

`std::vector<uint8_t>` can be 16 KB of data. If Java-style value passing were
used, every call would copy all of it. `&` means *"I'll work on the original."*

### 🟢 The two forms, side by side

| You write | It means | Java equivalent |
|---|---|---|
| `TcpSocket sock` | a **copy** | (Java has no equivalent) |
| `TcpSocket& sock` | the **original**, writable | normal Java object param |
| `const TcpSocket& sock` | the **original**, read-only | *no equivalent* |

The middle row is why the first row is unusual. In Java, `void f(Socket s)`
already means "the original." **In C++ it means a copy**, so C++ makes you
write `&` to get Java's default behaviour.

### Now the important part: look at the difference

In that `sendMessage` line, the two parameters are deliberately different:

```cpp
void sendMessage(TcpSocket& sock,                        // writable  →
                 uint8_t id,
                 const std::vector<uint8_t>& payload);    // read-only  ←
```

- `TcpSocket&` — **no `const`**. This function *changes* the socket; it sends
  bytes through it.
- `const std::vector<uint8_t>&` — **`const` present**. This function only
  *reads* the payload to send it.

> ### **WITHOUT vs WITH**
>
> ```
> void sendMessage(TcpSocket& sock, std::vector<uint8_t>& payload);
>   Allowed: the function could quietly modify the caller's payload.
>   Nothing stops it. Nothing warns you. The caller has no idea.
>
> void sendMessage(TcpSocket& sock, const std::vector<uint8_t>& payload);
>   If the function even TRIES to change payload, the compiler refuses to
>   compile. The protection is automatic and happens before you run.
>
> THE RULE  add const to a reference parameter unless the function has a
>           real job changing that thing.
> ```

**🎯 Mental model:** `&` gives the function the original object instead of a
copy. `const` says *"and I promise not to touch it"* — a promise the compiler
enforces.

### 🟡 A const reference costs almost nothing

You might think `const T&` still copies. It doesn't — it's the same "use the
original" behaviour as `&`, just read-only. Copying a 16 KB vector on every
message would be slow; a reference is free. This is why virtually every
function in the project takes `const std::string&` instead of `std::string`.

### 🟡 Pointers (`*`) — the same idea, but nullable

A **pointer** is a reference that is allowed to be "nothing":

```cpp
size_t nextPieceToFetch(const Bitfield* peerHas = nullptr) const;
```

`const Bitfield*` means *"a read-only `Bitfield`, or possibly no object at
all."* `nullptr` is the C++ way of saying "nothing" (this project uses it 25
times; Java's equivalent is `null`).

The `*` here appears **after** `Bitfield` and **before** the name. That's
C++ style. (C, its ancestor, writes `Bitfield* peerHas` in some places and
`Bitfield *peerHas` in others — both mean the same thing.)

> **When do you use which?**
> `&` — when the object must exist. `*` — when "no object" is a legitimate
> state.
>
> In this project the only pointer parameters are optional: a peer that has
> no bitfield, an optional block to write. Everything mandatory is a
> reference.

---

## 4. 🟢 `const` — five positions, five meanings

`const` is the single most repeated idea in the codebase. It appears in
almost every declaration, in five different places. Each means something
slightly different, and confusing them is the usual cause of C++ bugs.

All five, from real code:

```cpp
// ①  a parameter: the function will not change it
void connect(const std::string& host, const std::string& port, int timeoutSeconds);

// ②  a member function: it will not change the OBJECT
size_t pieceLength(size_t index) const;

// ③  after the return type: it will not change what it returns
const std::string& path() const;

// ④  a local variable: this variable will not be reassigned
size_t pieceCount_ = 0;

// ⑤  a member variable: this field is set once and never again
std::string outputPath_;
```

Read ② carefully, because it's the non-obvious one. A `const` at the end of
a member function means *"calling this does not change the object's data."*
It is a promise to every future caller:

```cpp
size_t pieceCount() const { return states_.size(); }
```

This can never modify `states_`, so it is safe to call from anywhere,
including from another thread reading while workers write. That safety is the
whole point of writing `const` — and it's why this codebase is so full of it.

> ### **WITHOUT vs WITH**
>
> ```
> A function with no const
>   The compiler must assume it could change ANY field. So a reader holding
>   a const PieceManager literally cannot call it. And a reviewer has no
>   way to know whether calling it is safe while another thread writes.
>
> A function with const
>   "This cannot change anything." Provable, checkable, and a real part of
>   how this project stays thread-safe.
>
> THE RULE  a member function gets a trailing const unless it is meant to
>           change the object's state.
> ```

### 🟡 `mutable` — the escape hatch

There's one place this project breaks its own rule. `PieceManager` has a
const method `pieceState()`, but it must lock a mutex to read safely — and
locking a mutex technically modifies it. So the mutex is marked `mutable`:

```cpp
mutable std::mutex mutex_;   // may be touched by const methods
```

**Read it as:** *"the mutex is exempt. Every other field stays untouched by
const methods; this one is allowed to change its internal lock state."*
It's a narrow, deliberate exception — not a loophole the codebase leans on.

---

## 5. 🟢 `struct` vs `class` — the same thing, one word apart

The two are **identical in every way except default visibility.** `struct`
defaults to public, `class` defaults to private:

```cpp
struct PeerMessage {              // public by default
    uint8_t id = 0;
    std::vector<uint8_t> payload;
};
```

```cpp
class PieceManager {              // private by default
public:                          // ...so you must opt in
    size_t pieceCount() const;
private:
    std::string outputPath_;
    std::mutex mutex_;
};
```

**What's the convention here?** Data carriers (`PeerMessage`, `TorrentFile`,
`Peer`) are `struct`s with no methods — they're just bundles of data. Things
with **behaviour and private state** (`PieceManager`, `TcpSocket`,
`ConcurrentDownloader`) are `class`es that hide their fields behind `public:`.

**Java parallel:** both are just `class`. C++ gives you the choice of which
default to start from. Pick the one that matches your intent and the
visibility follows.

---

## 6. 🟢 Enums — naming a small fixed set of values

The project uses two flavours, and it matters that you can tell them apart.

**`enum class` — the safe modern one.** From `PieceManager`:

```cpp
enum class State {
    kPending = 0,   // not fetched yet
    kClaimed,       // a worker is fetching it right now
    kOwned          // verified SHA-1 AND written to disk
};
```

To use it you write `State::kOwned` — the `enum class` name is required in
front. That's the point: it stops `kOwned` from floating loose in your program
where it could collide with another `kOwned`.

**A plain `enum` — the loose older one.** From `PeerMessage`:

```cpp
enum : uint8_t {
    MSG_CHOKE = 0,
    MSG_UNCHOKE = 1,
    MSG_HAVE = 4,
    // ... 9 message types total
};
```

No `class`, and you use the names bare: `MSG_CHOKE`, not
`Messages::MSG_CHOKE`. The `: uint8_t` part says *"these values fit in one
byte"* — which matters, because these values are literally sent as bytes on
the wire.

> **Java parallel:** both are ordinary enums. Java requires a type name
> (`State.K_OWNED`) and C++'s `enum class` is the closer match. The loose
> `enum` behaves more like Java's old `int` constants — the name isn't
> required, which is exactly the sloppiness `enum class` fixes.

### 🟢 Naming conventions you'll see everywhere

Not a language rule, but you need it to read the code fluently:

| Look like | Means | Example |
|---|---|---|
| `kFooBar` | a compile-time constant | `kBlockSize`, `kNotFound` |
| `MSG_CHOKE` | a bare enum value | `MSG_HAVE` |
| `foo_` | a **private** field | `outputPath_`, `fd_` |
| `camelCase` | a function or local | `pieceCount()` |
| `PascalCase` | a class or struct | `PieceManager` |

`constexpr` marks something the compiler can work out at compile time:

```cpp
constexpr size_t kBlockSize = 16384;       // 16 KiB, fixed forever
constexpr size_t kMaxPeerMessageSize = 1u << 20;   // 1 MiB
```

You saw `1u << 20` in the docs already: `<<` shifts bits left. 1 shifted
left by 20 is 1 followed by 20 zero bits — 1 MiB. Compilers love that trick,
and so does this codebase for every powers-of-two cap.

---

## 7. 🟢 `static_cast` — telling the compiler a type conversion is intended

`static_cast` appears 183 times. It's C++'s safe cast: "I know these are
different types, and I mean to convert."

The most common use here is **narrowing a number into a byte**, and it's a
real lesson:

```cpp
out[0] = static_cast<uint8_t>(v >> 24);
```

That's taking a 32-bit number, shifting the top byte down, and forcing it into
a `uint8_t`. **Java parallel:** the same line in Java is

```java
out[0] = (byte)(v >> 24);
```

…which is subtly wrong — Java's `byte` is signed, so casting 255 into a byte
gives you **−1**. Java programmers work around it constantly with `(b & 0xFF)`.
**In C++ there's no trap**: `uint8_t` is genuinely 0–255, so
`static_cast<uint8_t>(...)` gives you exactly what you meant. No `& 0xFF` ever
needed.

**🎯 Mental model:** `static_cast` is not a suggestion, it's an assertion.
"It is safe to treat this as that type, and I take responsibility."

---

## 8. 🟡 Default arguments, overloading, and one-line functions

Three small things you'll see constantly.

**A default argument** — `PieceManager.hpp`:
```cpp
bool storePiece(size_t index, const std::vector<uint8_t>& bytes,
                ClaimToken token = kNoClaim);
```
Callers who don't care about tokens write `storePiece(i, bytes)` and get the
default. Same as Java, no default methods needed.

**Overloading** — `PeerMessage.cpp` has two functions sharing a name:
```cpp
void sendMessage(TcpSocket& sock, uint8_t id, const std::vector<uint8_t>& payload);
void sendMessage(TcpSocket& sock, uint8_t id);
```
Two functions, same name, different parameters. Java has this too. C++ picks
the right one by the arguments you pass.

**A function defined in the header** — remember `Bitfield.hpp` from Module 0?
```cpp
size_t pieceCount() const { return pieceCount_; }
```
That's a whole function living in a header, on one line. Perfectly normal in
C++ for trivial accessors, and it's why no `Bitfield.cpp` exists for them.

---

## 9. 🟡 Lambdas — a function with no name

Twelve times, always the same shape. From `PeerSession.cpp`:

```cpp
auto offsetOf = [&](size_t block) { return block * kBlockSize; };
```

**Java parallel:** like `(size_t block) -> block * kBlockSize` assigned to a
variable. Same idea.

The `[&]` is the interesting part: *"if I use any variable from the
surrounding function, let me **read** it."* Change it to `[=]` for read-all,
`[&]` for read-or-write, `[this]` for only members. The project mostly wants
to *read* a `kBlockSize` or an abort flag, so `[&]` shows up a lot. Module 9
explains the concurrency implications, which are the whole reason `[&]` is
subtle.

---

## 10. 🔵 Ownership: why `TcpSocket` deleted its copy constructor

Every `TcpSocket` owns an OS file descriptor — a number the kernel gave it,
meaning "this process owns this open connection."

Here's the trap, and the project guards against it explicitly:

```cpp
TcpSocket(const TcpSocket&) = delete;                  // no copying
TcpSocket& operator=(const TcpSocket&) = delete;       // no copying
TcpSocket(TcpSocket&& other) noexcept;                 // moving is fine
TcpSocket& operator=(TcpSocket&& other) noexcept;
~TcpSocket();                                          // closes on destruction
```

**The problem:** if you copy a `TcpSocket`, now two objects each think they
own the same file descriptor. When both are destroyed, the connection is
closed twice — and on many systems the second close can close a *completely
different* connection that reused the number in the meantime.

**The fix:** C++ lets a class say *"copying me is forbidden."* The `= delete`
does exactly that, and gives a compile error if anyone tries.

The project carries this same six-line pattern in three files (`TcpSocket`,
`PeerSession`, and `HttpTracker`'s internal `Connection`). Same pattern, same
reason, every time.

**Java parallel:** the comment in the code says it best — *"like a Java object
that is not `Cloneable`."* Java makes you *remember* not to copy a socket. C++
lets the class **enforce** it, permanently, with zero runtime cost.

There's also the `-1` sentinel right there:
```cpp
int fd_ = -1;   // -1 means "no open socket"
```
A default-constructed `TcpSocket` owns nothing, so it starts with `-1` to say
"nothing to close." Same trick as `stateful = 0` in `TorrentFile` — a field
given a meaningful starting value so it never holds garbage.

**🔵 LATER:** this pattern is part of "the Rule of Five," and the reason C++
makes you write a destructor, a copy constructor, and a move constructor by
hand. Nothing in this project needs it yet beyond what you see above. When
we get to Module 10 we'll see the full idiom, and Module 11 will show what
happens when it's violated.

---

## 🟡 What you deliberately don't need yet

This is the payoff of the checklist at the top. Skip all of it:

| Skip | Why it's safe to skip |
|---|---|
| `shared_ptr` / `unique_ptr` | The project uses **zero** smart pointers. Ownership is explicit and manual. |
| `virtual` / polymorphism | The **only** inheritance is 3 exception classes from `std::runtime_error`. Everything else is composition. |
| Templates you write yourself | Zero. `std::function` is used, but you never define a template. |
| `string_view`, `std::array` | Never appear. |
| Operator overloading for your own types | The only `operator` overloads are the six move/copy-assignment ones from §10. |
| Move semantics in depth | You saw the shape. The why comes in Module 10. |

---

## Check your understanding

**1.** Why does `sendMessage` take `TcpSocket& sock` but `const std::vector<uint8_t>& payload`?

<details><summary>Answer</summary>

Because `sendMessage` sends *through* the socket (it changes it), but only
*reads* the payload. The `const` is a promise the compiler enforces: without
it, the function could silently change the caller's payload.

</details>

**2.** What does a `const` at the end of a member function promise?

<details><summary>Answer</summary>

That calling it doesn't change the object's data. So `size_t pieceCount() const`
can never touch `states_`. It's what makes a function provably safe to call
from another thread.

</details>

**3.** In Java, `void f(List<byte> b)` gets the original list. What does the C++ equivalent look like, and what's different?

<details><summary>Answer</summary>

`void f(const std::vector<uint8_t>& b)`. The difference: in C++, *omitting* `&`
would silently make a **copy**, which in Java you never have to think about.
And Java's `byte` is signed, so it's not equivalent to `uint8_t` at all.

</details>

**4.** `out[0] = static_cast<uint8_t>(v >> 24);` — what's the Java equivalent, and why is it worse?

<details><summary>Answer</summary>

`(byte)(v >> 24)`. Worse because Java's `byte` is signed: the value 255 becomes −1, and you have to write `(b & 0xFF)` to undo it. C++'s `uint8_t` is truly 0–255, so there's nothing to undo.

</details>

**5.** Why does `TcpSocket` write `TcpSocket(const TcpSocket&) = delete;`?

<details><summary>Answer</summary>

Because it owns an OS file descriptor. A copy would mean two objects both
try to close the same connection — and the second close could hit an unrelated
connection that reused the number. `= delete` turns that mistake into a
compile error instead of a rare bug. The Java equivalent is "just remember not
to clone a Socket."

</details>

---

## What's next

You can now read a header and know what every part is doing. Types, `&`,
`const`, `struct` vs `class`, enums, and the naming conventions.

**[Module 2 — Bytes and Binary Data →](02-bytes-and-binary.md)**

That module is where the groundwork pays off: a byte is a byte on the wire,
big-endian and little-endian are genuinely different, and `uint8_t` finally
stops being an abstract type and starts being *data on a network*. The
`demo-01-byte-order.cpp` we wrote earlier finally becomes readable — you'll be
able to see the bug with your own eyes.