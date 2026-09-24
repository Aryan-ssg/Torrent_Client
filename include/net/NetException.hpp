#pragma once

// =============================================================================
// NetException.hpp - Error thrown by anything on the network layer
// =============================================================================
//
// Like BencodeException for bencoding, this is the "network side" error.
// It still derives from std::runtime_error, so a catch (const
// std::exception&) catches both (like catching Exception/Throwable in Java).
// =============================================================================

#include <stdexcept>  // For std::runtime_error
#include <string>     // For std::string

class NetException : public std::runtime_error {
public:
    explicit NetException(const std::string& message)
        : std::runtime_error(message) {}
};