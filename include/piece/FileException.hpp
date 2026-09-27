#pragma once

// =============================================================================
// FileException.hpp - Errors from the DISK layer (Phase 6)
// =============================================================================
//
// BencodeException = "bad bytes in a bencode value".
// NetException      = "the network layer misbehaved".
// FileException     = "we couldn't open/write the output file".
//
// It still derives from std::runtime_error, so a single
// catch (const std::exception&) catches all three, exactly like catching
// Exception/Throwable in Java.
// =============================================================================

#include <stdexcept>  // For std::runtime_error
#include <string>     // For std::string

class FileException : public std::runtime_error {
public:
    explicit FileException(const std::string& message)
        : std::runtime_error(message) {}
};