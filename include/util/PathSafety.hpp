#pragma once

// =============================================================================
// PathSafety.hpp - Reject torrent-supplied paths that could escape a directory
// =============================================================================
//
// A .torrent file is untrusted input from start to finish, and its `name` field
// is a filename the REMOTE side chose. The only reason we write that name to
// disk is that the user asked us to download a torrent, which means "put the
// content where I said" - it emphatically does not mean "put the content
// wherever the file's author felt like putting it".
//
// The attack this prevents is old, obvious, and still works on naive clients:
//
//     info.name = "../../../../home/user/.bashrc"
//     info.name = "/etc/cron.d/pwn"
//     info.name = "..\\..\\Windows\\System32\\evil.dll"   (Windows)
//
// which turns "download this torrent" into "overwrite an arbitrary file the
// user can write". A torrent title is attacker-controlled text, and a filename
// is the one thing a program must never take from untrusted input verbatim.
//
// The rule here is deliberately strict rather than clever: REJECT, do not
// sanitise. "Cleaning" a name (stripping slashes, dropping "..") is a losing
// game - there are encodings, Unicode look-alikes, trailing dots and spaces on
// Windows, and 8.3 short names, and every one of them is a way to write a file
// the user did not agree to. A torrent whose name cannot be used safely is
// not a torrent we can serve; saying so is better than guessing at the author's
// intent.
//
// Multi-file torrents will need the same check for every path component, which
// is why this lives in its own small header rather than inside main().
//
// Java parallel: like Files.isSafeChild() or a path-normalisation guard, which
// is a thing to think about in Java too - Path.resolve() happily walks out of
// a base directory if you let it.
// =============================================================================

#include <string>

// A single path component, e.g. one file name or one directory name.
//
// Returns false, with a human-readable reason in `error`, if the component is
// unsafe. A safe component is returned in `clean`.
//
// Rejects: empty, "." or "..", anything containing '/' or '\\', a Windows
// drive letter ("C:"), a leading '~', a NUL byte, and control characters.
bool isSafePathComponent(const std::string& component, std::string& clean,
                        std::string& error);

// Join a trusted directory with a torrent-supplied name, after checking it.
//
// This is the function the download path actually uses, so the check cannot be
// forgotten at a call site: there is no way to build an output path from a
// torrent name without going through the guard.
bool makeSafeOutputPath(const std::string& directory, const std::string& torrentName,
                        std::string& fullPath, std::string& error);
