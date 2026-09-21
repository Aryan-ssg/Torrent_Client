#pragma once

// =============================================================================
// BencodeException.hpp - Custom Exception Class
// =============================================================================
//
// This file defines a custom exception class for Bencode decoding errors.
//
// JAVA COMPARISON:
// In Java, you might create a custom exception like:
//   public class BencodeException extends RuntimeException {
//       public BencodeException(String message) {
//           super(message);
//       }
//   }
//
// In C++, we inherit from std::runtime_error (similar to RuntimeException in Java)
// =============================================================================

#include <stdexcept>  // For std::runtime_error
#include <string>     // For std::string

class BencodeException : public std::runtime_error {
public:
    // Constructor takes an error message (just like Java exceptions)
    // 'explicit' keyword prevents accidental implicit conversions
    explicit BencodeException(const std::string& message)
        : std::runtime_error(message) {}  // Call parent constructor with message
};

/*
 * HOW TO USE THIS EXCEPTION:
 *
 * throw BencodeException("Something went wrong");
 *
 * try {
 *     // code that might fail
 * } catch (const BencodeException& e) {
 *     std::cout << e.what() << std::endl;  // Prints the error message
 * }
 *
 * NOTE: In C++, 'what()' method returns the error message (similar to getMessage() in Java)
 */
