// =============================================================================
// main.cpp - the peerflow command-line interface
// =============================================================================
//
// The engine underneath is seven phases of protocol work - bencode, torrent
// parsing, trackers, handshakes, peer messages, verified pieces, threaded
// downloads - and this file is the front door that makes it usable:
//
//   ./build/peerflow ubuntu.torrent
//
// The program's shape is the order the work actually happens:
//
//   1. parse the command line              (Options)
//   2. read + parse the .torrent file      (Phase 2)
//   3. ask the trackers who else is here   (Phase 3, HTTP and UDP)
//   4. download every piece, verifying it   (Phases 4-7)
//   5. draw what is happening, live        (TerminalUI)
//
// A NOTE ON TRUST
// ---------------
// Everything in a .torrent file is chosen by whoever published it: the file
// name, the piece sizes, the tracker URLs, the peer addresses. It is all
// untrusted input, and the only one of those we ever turn into something
// dangerous is the FILE NAME - which we would otherwise concatenate onto the
// output directory and hand to open(). That goes through PathSafety, which
// refuses rather than cleans. The rest is handled where it is parsed.
//
// Java parallel: this is the "main" that wires the application together and
// hands work to services that never knew it existed.
// =============================================================================

#include "peer/ConcurrentDownloader.hpp"
#include "torrent/TorrentFile.hpp"
#include "torrent/TorrentParser.hpp"
#include "tracker/TrackerPool.hpp"
#include "tracker/TrackerRequest.hpp"
#include "ui/TerminalUI.hpp"
#include "util/PathSafety.hpp"

#include <sys/stat.h>
#include <unistd.h>   // isatty, STDOUT_FILENO
#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.2.0";

// --- output style ----------------------------------------------------------
// Colour is opt-out via NO_COLOR (https://no-color.org) and automatically off
// when stdout is not a terminal, so piping into a file or a pager gives clean
// text with no escape codes in it.
bool g_colour = true;

const char* dim() { return g_colour ? "\033[2m" : ""; }
const char* bold() { return g_colour ? "\033[1m" : ""; }
const char* green() { return g_colour ? "\033[32m" : ""; }
const char* yellow() { return g_colour ? "\033[33m" : ""; }
const char* red() { return g_colour ? "\033[31m" : ""; }
const char* reset() { return g_colour ? "\033[0m" : ""; }

// =============================================================================
// Ctrl-C handling
// =============================================================================
// A signal handler must be async-signal-safe, which in practice means doing
// almost nothing inside one. Touching std::cout or allocating in there is a
// data race waiting to happen. The safe move is to flip one flag and let an
// ordinary thread notice - `volatile sig_atomic_t` is the only kind of
// variable the standard actually promises you can touch from a handler.
// =============================================================================
volatile sig_atomic_t g_interrupted = 0;

extern "C" void onInterrupt(int) {  // C linkage: it must be a real handler
    g_interrupted = 1;
}

void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = onInterrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // deliberately not SA_RESTART: we want I/O to return EINTR
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

bool interrupted() { return g_interrupted != 0; }

// =============================================================================
// Formatting helpers
// =============================================================================
std::string humanBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string humanDuration(double seconds) {
    if (seconds < 0 || seconds > 359999) return "unknown";
    long long s = static_cast<long long>(seconds);
    auto two = [](long long v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    if (s < 60) return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m " + two(s % 60) + "s";
    return std::to_string(s / 3600) + "h " + two((s % 3600) / 60) + "m";
}

std::string hexOf(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

// "  key : value" with the keys lined up, so a block of metadata reads as a
// table rather than a paragraph.
void printField(const char* key, const std::string& value) {
    std::printf("  %s%-11s%s %s\n", dim(), key, reset(), value.c_str());
}

// =============================================================================
// Options
// =============================================================================
// Hand-rolled rather than getopt(), which is POSIX-only and this project is
// meant to build on Windows too. A dozen lines here beats a dependency for
// flags this small.
// =============================================================================
struct Options {
    std::string torrentPath;
    std::string outputDir = ".";
    int workers = 8;
    int timeoutSeconds = 10;
    int announceSeconds = 60;
    bool noTui = false;
    bool showHelp = false;
    bool showVersion = false;
    bool quiet = false;
};

void printUsage(const char* argv0) {
    std::cout
        << bold() << "peerflow " << kVersion << reset()
        << " - a BitTorrent client built from scratch in C++17\n\n"
        << bold() << "USAGE" << reset() << "\n"
        << "  " << argv0 << " <file.torrent> [options]\n\n"
        << bold() << "OPTIONS" << reset() << "\n"
        << "  -o, --output <dir>     write the file here (default: current directory)\n"
        << "  -j, --workers <n>      parallel download workers (default: 8)\n"
        << "  -t, --timeout <secs>   per-peer network timeout (default: 10)\n"
        << "  -a, --announce <secs>  how long to spend on trackers (default: 60)\n"
        << "  -q, --quiet            only report errors\n"
        << "      --no-tui           plain lines instead of the live display\n"
        << "  -v, --version          print the version and exit\n"
        << "  -h, --help             print this help and exit\n\n"
        << bold() << "EXAMPLES" << reset() << "\n"
        << "  " << argv0 << " ubuntu-24.04.iso.torrent\n"
        << "  " << argv0 << " album.torrent -o ~/Downloads -j 16\n"
        << "  " << argv0 << " thing.torrent --no-tui | tee log.txt\n\n"
        << bold() << "NOTES" << reset() << "\n"
        << "  * Ctrl-C is safe. Rerunning the same command resumes: pieces\n"
        << "    already on disk are re-verified, and only the rest are fetched.\n"
        << "  * The output file is allocated at its full size up front, so the\n"
        << "    free space has to exist before you start.\n"
        << "  * Single-file torrents only. Multi-file torrents are rejected.\n";
}

// Strict integer parsing: std::stoi would accept "12abc" and give 12, which
// turns a typo into a silently wrong setting.
int parseInt(const std::string& text, const char* flag) {
    if (text.empty()) {
        throw std::runtime_error(std::string(flag) + " needs a number");
    }
    size_t i = 0;
    long value = 0;
    try {
        value = std::stol(text, &i);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(flag) + ": \"" + text + "\" is not a number");
    }
    if (i != text.size()) {
        throw std::runtime_error(std::string(flag) + ": \"" + text +
                                 "\" has trailing characters");
    }
    return static_cast<int>(value);
}

Options parseArgs(int argc, char** argv) {
    Options o;

    auto value = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs a value");
        return argv[++i];
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            o.showHelp = true;
        } else if (arg == "-v" || arg == "--version") {
            o.showVersion = true;
        } else if (arg == "--no-tui") {
            o.noTui = true;
        } else if (arg == "-q" || arg == "--quiet") {
            o.quiet = true;
        } else if (arg == "-o" || arg == "--output") {
            o.outputDir = value(i, "--output");
        } else if (arg == "-j" || arg == "--workers") {
            o.workers = parseInt(value(i, "--workers"), "--workers");
        } else if (arg == "-t" || arg == "--timeout") {
            o.timeoutSeconds = parseInt(value(i, "--timeout"), "--timeout");
        } else if (arg == "-a" || arg == "--announce") {
            o.announceSeconds = parseInt(value(i, "--announce"), "--announce");
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option: " + arg);
        } else if (o.torrentPath.empty()) {
            o.torrentPath = arg;
        } else {
            throw std::runtime_error("unexpected extra argument: " + arg);
        }
    }

    // Validate ranges here, where the error can name the flag, rather than
    // letting them reach the engine and come back as a confusing message.
    if (o.workers < 1 || o.workers > 256) {
        throw std::runtime_error("--workers must be between 1 and 256 (got " +
                                 std::to_string(o.workers) + ")");
    }
    if (o.timeoutSeconds < 1 || o.timeoutSeconds > 600) {
        throw std::runtime_error("--timeout must be between 1 and 600 seconds (got " +
                                 std::to_string(o.timeoutSeconds) + ")");
    }
    if (o.announceSeconds < 1 || o.announceSeconds > 600) {
        throw std::runtime_error("--announce must be between 1 and 600 seconds (got " +
                                 std::to_string(o.announceSeconds) + ")");
    }
    return o;
}

// Create the output directory (and any missing parents) if it is not there
// already. mkdir -p semantics, because "-o ~/Downloads/torrents" failing
// because ~/Downloads does not exist yet is a pointless papercut.
bool ensureDirectory(const std::string& path, std::string& error) {
    if (path.empty() || path == ".") return true;

    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        error = "the output path exists but is not a directory: " + path;
        return false;
    }

    // Walk the path creating each component in turn. A leading '/' is preserved
    // rather than treated as an empty first component, and a trailing slash is
    // harmless because an empty final component is simply skipped.
    std::string built;
    size_t i = 0;
    if (path[0] == '/') {
        built = "/";
        i = 1;
    }
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const std::string component =
            path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);

        if (!component.empty()) {
            if (built.empty() || built.back() != '/') built += '/';
            built += component;
            if (::stat(built.c_str(), &st) != 0) {
                if (::mkdir(built.c_str(), 0755) != 0 && errno != EEXIST) {
                    error = "cannot create directory " + built + ": " + std::strerror(errno);
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                error = built + " exists and is not a directory";
                return false;
            }
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return true;
}

// Shorten a URL for display: trackers are long and the tail is the part that
// differs.
std::string trimUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    const size_t start = scheme + 3;
    size_t end = url.find('/', start);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(start, end - start);
    return host;
}

}  // namespace

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    Options opts;
    try {
        opts = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%speerflow:%s %s\n\n", red(), reset(), e.what());
        printUsage(argv[0]);
        return 2;
    }

    // Colour only when a human is watching, and not when NO_COLOR is set.
    g_colour = ::isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;

    if (opts.showHelp) {
        printUsage(argv[0]);
        return 0;
    }
    if (opts.showVersion) {
        std::cout << "peerflow " << kVersion << "\n";
        return 0;
    }
    if (opts.torrentPath.empty()) {
        std::fprintf(stderr, "%speerflow:%s no .torrent file given\n\n", red(), reset());
        printUsage(argv[0]);
        return 2;
    }

    installSignalHandlers();

    const auto startedAt = std::chrono::steady_clock::now();

    try {
        // --- 2. Read and parse the torrent (Phase 2) ----------------------
        if (!opts.quiet) {
            std::printf("%s>%s %s%s%s\n", bold(), reset(), bold(), opts.torrentPath.c_str(),
                        reset());
        }
        TorrentFile torrent = TorrentParser::parse(opts.torrentPath);

        const size_t pieceCount = torrent.pieces.size() / 20;
        if (pieceCount == 0) {
            throw std::runtime_error("this torrent contains no piece hashes");
        }

        if (!opts.quiet) {
            printField("name", torrent.name);
            printField("size", humanBytes(static_cast<uint64_t>(torrent.length)));
            printField("pieces", std::to_string(pieceCount) + " x " +
                                     humanBytes(static_cast<uint64_t>(torrent.pieceLength)));
            printField("info hash", hexOf(torrent.infoHash));
        }

        // --- Where the file goes -----------------------------------------
        // The name came out of an untrusted file, so it is checked before it
        // is allowed anywhere near a filesystem path.
        std::string outputPath;
        std::string pathError;
        if (!makeSafeOutputPath(opts.outputDir, torrent.name, outputPath, pathError)) {
            throw std::runtime_error(pathError);
        }
        std::string dirError;
        if (!ensureDirectory(opts.outputDir, dirError)) {
            throw std::runtime_error(dirError);
        }

        // --- 3. Ask the trackers who else is here (Phase 3) ---------------
        const std::string peerId = generatePeerId();
        if (!opts.quiet) {
            std::printf("\n%s>%s finding peers (peer id %s)\n", bold(), reset(),
                        peerId.c_str());
        }

        TrackerPool pool(torrent);
        const TrackerResult announce =
            pool.announce(peerId, opts.announceSeconds, 8);

        // Only report the trackers that worked, plus a one-line count of the
        // ones that did not. A dead tracker is normal in a real torrent and
        // does not deserve eight lines of the user's attention.
        int trackerOk = 0;
        std::vector<std::string> failures;
        for (const std::string& line : announce.log) {
            const size_t arrow = line.find("-> ");
            if (arrow == std::string::npos) continue;
            const bool ok = line.find("failed:") == std::string::npos;
            if (ok) {
                trackerOk++;
                if (!opts.quiet) {
                    // NOTE: .c_str() is not optional here. trimUrl returns a
                    // std::string, and handing a std::string object to a "%s"
                    // conversion is undefined behaviour - the callee reads the
                    // object's bytes as a char pointer, which prints whatever
                    // the layout happens to contain. The symptom was a
                    // tracker host rendering as "下載".
                    std::printf("  %s%s%s  %s\n", green(), trimUrl(line.substr(0, arrow)).c_str(),
                                reset(), line.substr(arrow + 3).c_str());
                }
            } else {
                failures.push_back(trimUrl(line.substr(0, arrow)));
            }
        }
        if (!opts.quiet && !failures.empty()) {
            std::printf("  %s%d tracker%s unavailable, skipped%s\n", dim(),
                        static_cast<int>(failures.size()),
                        failures.size() == 1 ? "" : "s", reset());
        }

        if (announce.peers.empty()) {
            std::fprintf(stderr,
                         "\n%speerflow:%s found no peers after trying %d tracker%s.\n"
                         "  The torrent may be unseeded, or every tracker in it may be\n"
                         "  blocked from this network.\n",
                         red(), reset(), trackerOk + static_cast<int>(failures.size()),
                         (trackerOk + static_cast<int>(failures.size())) == 1 ? "" : "s");
            return 1;
        }

        if (!opts.quiet) {
            std::printf("\n%s>%s downloading to %s%s%s\n", bold(), reset(), bold(),
                        outputPath.c_str(), reset());
        }

        // --- 4 & 5. Download, drawing it live -----------------------------
        TerminalUI ui(opts.noTui);
        if (!opts.quiet) ui.log("peer id " + peerId);

        std::atomic<bool> abortFlag{false};
        const ConcurrentDownloader::Result dl = ConcurrentDownloader::download(
            torrent, announce.peers, outputPath, peerId, opts.workers, opts.timeoutSeconds,
            [&ui](const DownloadProgress& p) { ui.onProgress(p); }, &abortFlag);

        if (interrupted()) {
            ui.abandon();
            std::printf("\n%sInterrupted.%s %zu of %zu pieces are on disk.\n"
                        "Rerun the same command to resume.\n",
                        yellow(), reset(), dl.piecesDownloaded, pieceCount);
            return 130;
        }

        ui.finish({}, dl.ok, dl.error);

        if (!dl.ok) {
            std::fprintf(stderr, "\n%speerflow:%s %s\n", red(), reset(), dl.error.c_str());
            return 1;
        }

        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - startedAt)
                                   .count();
        if (!opts.quiet) {
            std::printf("  %stotal time%s %s\n", dim(), reset(),
                        humanDuration(elapsed).c_str());
        }
        return 0;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "\n%speerflow:%s %s\n", red(), reset(), e.what());
        return 1;
    }
}
