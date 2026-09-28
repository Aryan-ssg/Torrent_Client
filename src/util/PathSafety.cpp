// =============================================================================
// PathSafety.cpp - Reject torrent-supplied paths that could escape a directory
// =============================================================================

#include "util/PathSafety.hpp"

#include <cctype>

namespace {

bool hasControlChar(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// isSafePathComponent()
// =============================================================================
bool isSafePathComponent(const std::string& component, std::string& clean,
                        std::string& error) {
    if (component.empty()) {
        error = "it is empty";
        return false;
    }
    if (hasControlChar(component)) {
        error = "it contains a control character";
        return false;
    }

    // The two names that mean "this directory" and "my parent". On their own
    // they are the whole attack; anywhere in a path they are worse.
    if (component == "." || component == "..") {
        error = "it is \"" + component + "\"";
        return false;
    }

    // A separator anywhere means this is not a component, it is a path the
    // author wrote for a different operating system (or to escape ours).
    if (component.find('/') != std::string::npos) {
        error = "it contains '/'";
        return false;
    }
    if (component.find('\\') != std::string::npos) {
        error = "it contains '\\' (a Windows path separator)";
        return false;
    }

    // "C:foo" or "C:\" - relative to a drive, which on Windows can escape the
    // process's working directory entirely.
    if (component.size() >= 2 && component[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(component[0]))) {
        error = "it starts with a drive letter (\"" + component.substr(0, 2) + "\")";
        return false;
    }

    // A leading '~' is expanded by the shell, not by us, but rejecting it costs
    // nothing and removes a confusing failure mode if a path is ever passed
    // through a shell later.
    if (component[0] == '~') {
        error = "it starts with '~'";
        return false;
    }

    // Trailing dots and spaces are silently stripped by Windows, so "evil.exe."
    // and "evil.exe" are the same file. Not an escape on its own, but it is a
    // way to make a name mean something other than what it looks like.
    if (component.back() == ' ' || component.back() == '.') {
        error = "it ends with a space or dot, which some systems strip silently";
        return false;
    }

    // Windows refuses these names outright, so a torrent using one could never
    // be written there anyway.
    static const char* kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2",
                                      "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                      "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                      "LPT6", "LPT7", "LPT8", "LPT9"};
    const size_t colon = component.find(':');
    const std::string base = component.substr(0, colon == std::string::npos ? 3 : colon);
    for (const char* r : kReserved) {
        if (base.size() >= 3 && base.compare(0, 3, r, 3) == 0) {
            error = std::string("\"") + r + "\" is a reserved device name";
            return false;
        }
    }

    clean = component;
    error.clear();
    return true;
}

// =============================================================================
// makeSafeOutputPath()
// =============================================================================
bool makeSafeOutputPath(const std::string& directory, const std::string& torrentName,
                        std::string& fullPath, std::string& error) {
    std::string clean;
    if (!isSafePathComponent(torrentName, clean, error)) {
        error = "the torrent's name is unsafe to use as a filename, because " + error +
                ". Refusing to write outside the output directory.";
        return false;
    }

    if (directory.empty()) {
        error = "the output directory is empty";
        return false;
    }

    fullPath = directory;
    if (fullPath.back() != '/') fullPath += '/';
    fullPath += clean;
    return true;
}
