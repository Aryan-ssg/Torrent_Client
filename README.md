# Torrent Client (C++)

A BitTorrent client implementation in C++, currently featuring a **Bencode decoder** for parsing `.torrent` files.

## What is Bencode?

Bencode is a simple encoding format used by BitTorrent to store metadata in `.torrent` files. It supports 4 data types:

| Type | Format | Example | Java Equivalent |
|------|--------|---------|-----------------|
| Integer | `i<number>e` | `i42e` = 42 | `long` |
| String | `<length>:<content>` | `5:hello` = "hello" | `String` or `byte[]` |
| List | `l<items>e` | `li42ee` = [42] | `List<Object>` |
| Dictionary | `d<key><value>e` | `d3:key5:valuee` = {"key": "value"} | `Map<String, Object>` |

## Project Structure

```
Torrent_Client/
├── CMakeLists.txt              # Build configuration (like pom.xml in Maven)
├── README.md
├── include/
│   └── bencode/
│       ├── BencodeDecoder.hpp  # Decoder class header
│       ├── BencodeException.hpp # Custom exception class
│       └── BencodeValue.hpp    # Universal value type
└── src/
    ├── main.cpp                # Test cases
    └── bencode/
        └── BencodeDecoder.cpp  # Decoder implementation
```

## How to Build

```bash
mkdir build
cd build
cmake ..
make
```

Or use CMake directly:

```bash
cmake -B build
cmake --build build
```

## How to Run Tests

```bash
./build/peerflow
```

## Key Concepts for Java Developers

### 1. BencodeValue (Like a Sealed Interface)

In Java, you might have:
```java
interface BencodeValue {}
record IntegerValue(long value) implements BencodeValue {}
record StringValue(byte[] value) implements BencodeValue {}
record ListValue(List<BencodeValue> value) implements BencodeValue {}
record DictValue(Map<String, BencodeValue> value) implements BencodeValue {}
```

In C++, we use a tagged union pattern:
```cpp
class BencodeValue {
    enum Type { INTEGER, STRING, LIST, DICT };
    // ... fields for each type
    // ... getter methods like asInteger(), asString(), etc.
};
```

### 2. Static Factory Methods

In Java:
```java
BencodeValue val = BencodeValue.of(42);
```

In C++:
```cpp
BencodeValue val = BencodeValue::makeInteger(42);
```

### 3. References vs Pointers

In Java, all object variables are references (like pointers).

In C++:
```cpp
// Reference (like Java reference - always valid)
const std::string& str = something;

// Pointer (can be null, like Java reference)
std::string* ptr = &something;
```

### 4. const Keyword

C++ has `const` to indicate "I won't modify this":
```cpp
// This method doesn't modify the object
Type getType() const { return type; }

// This parameter won't be modified
void print(const std::string& str);
```

Java doesn't have `const` (it has `final` which is different).

### 5. Exception Handling

Same syntax, slightly different conventions:
```java
// Java
try {
    decode(input);
} catch (Exception e) {
    System.out.println(e.getMessage());
}
```

```cpp
// C++
try {
    decode(input);
} catch (const std::exception& e) {
    std::cout << e.what() << std::endl;
}
```

## Current Features

- ✅ Integer parsing (positive, negative, zero)
- ✅ String parsing (including empty strings)
- ✅ List parsing (including nested lists)
- ✅ Dictionary parsing (including nested dictionaries)
- ✅ Error detection (trailing data, invalid input)
- ✅ Comprehensive test suite

## Next Steps (TODO)

- [ ] Parse `.torrent` files
- [ ] Implement tracker communication
- [ ] Implement peer wire protocol
- [ ] Add download/upload functionality
- [ ] Add piece management

## Learning Resources

- [Bencode specification](https://wiki.theory.org/BitTorrentSpecification#Bencoding)
- [BitTorrent specification](https://wiki.theory.org/BitTorrentSpecification)
- [C++ for Java developers](https://learnxinyminutes.com/docs/c++/)
