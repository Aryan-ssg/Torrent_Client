// =============================================================================
// main.cpp - the peerflow command-line interface
// =============================================================================
//
// Until now this file was nothing but the test suite. The engine underneath
// had grown to seven phases - bencode, torrent parsing, trackers, handshakes,
// peer messages, verified pieces, and threaded downloads - but there was no
// way to actually USE it. This file is that front door:
//
//   ./build/peerflow ubuntu.torrent
//
// tells the truth about what the project is, which is a working BitTorrent
// client you point at a .torrent file.
//
// The shape of the program is deliberately boring, in the order the work
// actually happens:
//
//   1. parse the command line              (Options)
//   2. read + parse the .torrent file      (Phase 2)
//   3. ask the trackers who else is here   (Phase 3 + UDP)
//   4. download every piece, verifying it  (Phases 4-7)
//   5. draw what is happening, live        (TerminalUI)
//
// Each step is a class that already exists or is small and obvious, so if
// something breaks the stack trace lands somewhere you can read.
//
// Java parallel: this is the "main" that wires the application together and
// hands work to the services; the services themselves never knew it existed.
// =============================================================================

#include "torrent/TorrentFile.hpp"
#include "torrent/TorrentParser.hpp"
#include "tracker/TrackerPool.hpp"
#include "tracker/TrackerRequest.hpp"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

// =============================================================================
// Ctrl-C handling
// =============================================================================
// The one rule for a signal handler in C++ is that it must be async-signal-
// safe, which in practice means: do almost nothing. Touching std::cout or
// allocating memory inside one is a data race waiting to happen.
//
// The safe move is to flip a single atomic flag and let the ordinary thread
// notice. `volatile sig_atomic_t` is exactly the right type for this - it is
// the only kind of variable the standard actually promises you can touch from
// a signal handler.
// =============================================================================
namespace {
volatile sig_atomic_t g_interrupted = 0;

extern "C" void onInterrupt(int) {  // must be C linkage to be a handler
    g_interrupted = 1;
}

void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = onInterrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: we want recv()/poll() to return EINTR
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

bool interrupted() { return g_interrupted != 0; }
}  // namespace

// =============================================================================
// Options - the command line
// =============================================================================
// Hand-rolled argument parsing on purpose. getopt() is POSIX-only and this
// project is meant to build on Windows too, and a dozen lines here is cheaper
// than a dependency for flags this small.
// =============================================================================
namespace {

struct Options {
    std::string torrentPath;
    std::string outputDir = ".";
    int workers = 8;
    int timeoutSeconds = 10;
    bool noTui = false;
    bool showHelp = false;
};

void printUsage(const char* argv0) {
    std::cout
        << "peerflow - a BitTorrent client built from scratch in C++17\n\n"
        << "Usage:\n"
        << "  " << argv0 << " <file.torrent> [options]\n\n"
        << "Options:\n"
        << "  -o, --output <dir>     Where to write the file (default: current dir)\n"
        << "  -j, --workers <n>      Parallel download workers (default: 8)\n"
        << "  -t, --timeout <secs>   Per-peer network timeout (default: 10)\n"
        << "      --no-tui           Plain line output instead of the live display\n"
        << "  -h, --help             Show this help\n\n"
        << "Example:\n"
        << "  " << argv0 << " ubuntu-24.04.1.iso.torrent -o ~/Downloads -j 16\n";
}

Options parseArgs(int argc, char** argv) {
    Options o;

    auto needValue = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) {
            throw std::runtime_error(std::string(flag) + " needs a value");
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            o.showHelp = true;
        } else if (arg == "--no-tui") {
            o.noTui = true;
        } else if (arg == "-o" || arg == "--output") {
            o.outputDir = needValue(i, "--output");
        } else if (arg == "-j" || arg == "--workers") {
            o.workers = std::stoi(needValue(i, "--workers"));
        } else if (arg == "-t" || arg == "--timeout") {
            o.timeoutSeconds = std::stoi(needValue(i, "--timeout"));
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option: " + arg);
        } else if (o.torrentPath.empty()) {
            o.torrentPath = arg;  // first bare word is the torrent
        } else {
            throw std::runtime_error("unexpected extra argument: " + arg);
        }
    }
    return o;
}

// -----------------------------------------------------------------------
// Human-readable sizes, so the UI never shows "14535680" to a human.
// -----------------------------------------------------------------------
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
    if (seconds < 0 || seconds > 359999) return "--:--:--";
    long long s = static_cast<long long>(seconds);
    auto two = [](long long v) {
        return (v < 10 ? "0" : "") + std::to_string(v);
    };
    return std::to_string(s / 3600) + ":" + two((s % 3600) / 60) + ":" + two(s % 60);
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
        std::cerr << "peerflow: " << e.what() << "\n\n";
        printUsage(argv[0]);
        return 2;
    }

    if (opts.showHelp) {
        printUsage(argv[0]);
        return 0;
    }
    if (opts.torrentPath.empty()) {
        std::cerr << "peerflow: no .torrent file given\n\n";
        printUsage(argv[0]);
        return 2;
    }

    installSignalHandlers();

    try {
        // --- 2. Read and parse the torrent (Phase 2) --------------------
        std::cout << "Reading " << opts.torrentPath << "...\n";
        TorrentFile torrent = TorrentParser::parse(opts.torrentPath);

        std::cout << "  name         : " << torrent.name << "\n"
                  << "  size         : " << humanBytes(static_cast<uint64_t>(torrent.length))
                  << "\n"
                  << "  piece length : " << humanBytes(static_cast<uint64_t>(torrent.pieceLength))
                  << "\n"
                  << "  pieces       : " << (torrent.pieces.size() / 20) << "\n"
                  << "  info hash    : ";
        for (uint8_t b : torrent.infoHash) std::printf("%02x", b);
        std::cout << "\n";

        if (torrent.pieces.empty()) {
            std::cerr << "peerflow: torrent has no piece hashes\n";
            return 1;
        }

        // --- 3. Ask the trackers who else is here (Phase 3) -------------
        const std::string peerId = generatePeerId();
        std::cout << "\nAnnouncing to trackers (peer id " << peerId << ")...\n";

        TrackerPool pool(torrent);
        TrackerResult announce = pool.announce(peerId, 30, 8);

        for (const std::string& line : announce.log) {
            std::cout << "  " << line << "\n";
        }

        if (announce.peers.empty()) {
            std::cerr << "\nNo peers found. The torrent may be unseeded, or every "
                         "tracker in its list is unreachable from here.\n";
            return 1;
        }
        std::cout << "\nFound " << announce.peers.size() << " peers.\n";

        if (interrupted()) return 130;

        // Steps 4 (download) and 5 (live display) are wired up in the next
        // commits; the announce path is deliberately working end to end first.
        std::cout << "\n(Download pipeline not wired up yet - coming next.)\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\npeerflow: " << e.what() << "\n";
        return 1;
    }
}
