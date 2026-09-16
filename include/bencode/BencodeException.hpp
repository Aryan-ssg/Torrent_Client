#pragma once

#include <stdexcept>
#include <string>

class BencodeException : public std::runtime_error {
public:
    explicit BencodeException(const std::string& message)
        : std::runtime_error(message) {}
};
