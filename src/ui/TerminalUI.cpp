// =============================================================================
// TerminalUI.cpp - The live download display
// =============================================================================

#include "ui/TerminalUI.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {

// --- ANSI escape codes ------------------------------------------------------
// Written out rather than generated so the whole vocabulary of what we can do
// to a terminal is visible in one place.
constexpr const char* kClearLine = "\033[K";          // erase to end of line
constexpr const char* kCursorUp1 = "\033[1A";         // up one row
constexpr const char* kHideCursor = "\033[?25l";
constexpr const char* kShowCursor = "\033[?25h";
constexpr const char* kBold = "\033[1m";
constexpr const char* kReset = "\033[0m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kDim = "\033[2m";

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
    auto two = [](long long v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    return std::to_string(s / 3600) + ":" + two((s % 3600) / 60) + ":" + two(s % 60);
}

std::string humanRate(double bytesPerSecond) {
    if (bytesPerSecond <= 0) return "--";
    return humanBytes(static_cast<uint64_t>(bytesPerSecond)) + "/s";
}

int terminalWidth() {
    int w = 80;
    // Deliberately not using TIOCGWINSZ without the right headers, and not
    // worth a dependency: COLUMNS is honoured by every shell we care about,
    // and 80 is a safe assumption otherwise.
    if (const char* cols = std::getenv("COLUMNS")) {
        int v = std::atoi(cols);
        if (v > 20) w = std::min(v, 200);
    }
    return w;
}

}  // namespace

// =============================================================================
// Construction
// =============================================================================
TerminalUI::TerminalUI(bool forceLines) {
    // isatty is the whole test. A pipe, a file, or a CI log means line mode.
    interactive_ = !forceLines && ::isatty(STDOUT_FILENO) && ::isatty(STDERR_FILENO);
    if (interactive_) {
        std::fputs(kHideCursor, stdout);
        std::fflush(stdout);
    }
}

void TerminalUI::emit(const std::string& s) {
    std::fputs(s.c_str(), stdout);
}

// =============================================================================
// onProgress()
// =============================================================================
void TerminalUI::onProgress(const DownloadProgress& progress) {
    if (finished_) return;

    // Rolling-window speed. Keep a few seconds of history and compute the
    // rate across it, so a burst of fast blocks does not pin the display at a
    // speed that has already stopped happening.
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (lastSampleMs_ == 0) {
        lastSampleMs_ = nowMs;
        lastBytes_ = static_cast<uint64_t>(progress.bytesDone);
    }
    speedSamples_.emplace_back(nowMs, static_cast<uint64_t>(progress.bytesDone));
    while (speedSamples_.size() >= 2 &&
           nowMs - speedSamples_.front().first > static_cast<uint64_t>(kSpeedWindowMs)) {
        speedSamples_.pop_front();
    }
    if (speedSamples_.size() >= 2) {
        const uint64_t dt = speedSamples_.back().first - speedSamples_.front().first;
        const int64_t db = static_cast<int64_t>(speedSamples_.back().second) -
                           static_cast<int64_t>(speedSamples_.front().second);
        if (dt > 0) bytesPerSecond_ = static_cast<double>(db) * 1000.0 / static_cast<double>(dt);
    }

    draw(progress);
}

// =============================================================================
// pieceMap()
// =============================================================================
// The real per-piece state, one character per piece, scaled to the terminal.
//
// A 23,664-piece ISO cannot be drawn one character per piece, so each cell
// covers a run of pieces. The cell is filled in proportion to how much of
// that run is actually done, which is honest: it is a downsampled view of
// real state, not a guess. The point is a glanceable picture of where the
// holes are, because a download that is crawling almost always has a very
// particular shape of hole.
// =============================================================================
std::string TerminalUI::pieceMap(const DownloadProgress& progress) const {
    if (progress.pieceMap.empty() || progress.piecesTotal == 0) return "";

    const int width = std::max(20, terminalWidth() - 2);
    if (progress.pieceMap.size() <= static_cast<size_t>(width)) {
        return progress.pieceMap;  // small torrent: show it exactly
    }

    const double scale = static_cast<double>(progress.pieceMap.size()) / width;
    std::string out;
    out.reserve(static_cast<size_t>(width));
    for (int c = 0; c < width; c++) {
        const size_t begin = static_cast<size_t>(c * scale);
        const size_t end = std::min(progress.pieceMap.size(),
                                    static_cast<size_t>((c + 1) * scale));
        if (begin >= end) {
            out.push_back(' ');
            continue;
        }
        size_t owned = 0;
        for (size_t i = begin; i < end; i++) {
            if (progress.pieceMap[i] == '#') owned++;
        }
        const double local = static_cast<double>(owned) / static_cast<double>(end - begin);
        // A run that is completely done becomes '#', an empty one '.', and
        // anything in between a mid-block, so partial progress stays visible.
        if (local >= 0.999) {
            out.push_back('#');
        } else if (local <= 0.001) {
            out.push_back('.');
        } else {
            static const char kRamp[] = ".:-=+*#%@";
            const int idx = static_cast<int>(local * 8.0);
            out.push_back(kRamp[std::min(7, std::max(0, idx))]);
        }
    }
    return out;
}

// =============================================================================
// renderBar()
// =============================================================================
void TerminalUI::renderBar(double fraction, int width) {
    fraction = std::max(0.0, std::min(1.0, fraction));
    const int filled = static_cast<int>(fraction * width);
    std::string bar(static_cast<size_t>(filled), '=');
    if (filled < width) bar += '>';
    bar += std::string(static_cast<size_t>(width - filled - (filled < width ? 1 : 0)), ' ');
    emit(bar);
}

// =============================================================================
// draw() - pick a strategy
// =============================================================================
void TerminalUI::draw(const DownloadProgress& progress) {
    if (interactive_) {
        drawInteractive(progress);
    } else {
        drawLines(progress);
    }
}

// =============================================================================
// drawLines() - the non-interactive fallback
// =============================================================================
// One line whenever something meaningful changed, so the output stays readable
// in a log and does not turn into ten thousand identical rows.
// =============================================================================
void TerminalUI::drawLines(const DownloadProgress& progress) {
    const int percent = static_cast<int>(progress.fraction() * 100);
    if (percent == lastPercent_ && !progress.finished) return;
    lastPercent_ = percent;

    std::ostringstream out;
    out << "[" << percent << "%] " << humanBytes(static_cast<uint64_t>(progress.bytesDone))
        << " / " << humanBytes(static_cast<uint64_t>(progress.bytesTotal))
        << "  at " << humanRate(bytesPerSecond_)
        << "  peers " << progress.peersConnected;
    if (progress.bytesTotal > 0 && bytesPerSecond_ > 0) {
        const double remaining =
            static_cast<double>(progress.bytesTotal - progress.bytesDone);
        out << "  eta " << humanDuration(remaining / bytesPerSecond_);
    }
    out << "\n";
    std::fputs(out.str().c_str(), stdout);
    std::fflush(stdout);
}

// =============================================================================
// drawInteractive() - the live display
// =============================================================================
void TerminalUI::drawInteractive(const DownloadProgress& progress) {
    const int width = terminalWidth();

    // Rewind to the top of the previous frame. This is the whole trick, and
    // it is why every row must be a fixed number of lines wide.
    std::string frame;
    for (int i = 0; i < linesDrawnLast_; i++) frame += kCursorUp1;

    std::ostringstream out;

    // --- the log pane, oldest first ----------------------------------------
    for (const std::string& line : logLines_) {
        out << kDim << "  " << line << kReset << kClearLine << "\n";
    }

    // --- the status block --------------------------------------------------
    const double fraction = progress.fraction();
    const int percent = static_cast<int>(fraction * 100);

    out << kBold;
    renderBar(fraction, std::max(10, width - 12));
    out << kReset << " " << percent << "%  " << kClearLine << "\n";

    out << "  " << humanBytes(static_cast<uint64_t>(progress.bytesDone)) << " / "
        << humanBytes(static_cast<uint64_t>(progress.bytesTotal)) << "   at "
        << humanRate(bytesPerSecond_);
    if (progress.bytesTotal > 0 && bytesPerSecond_ > 0) {
        const double remaining = static_cast<double>(progress.bytesTotal - progress.bytesDone);
        out << "   eta " << humanDuration(remaining / bytesPerSecond_);
    }
    out << kClearLine << "\n";

    out << "  pieces " << progress.piecesDone << "/" << progress.piecesTotal
        << "   peers " << progress.peersConnected << " connected, "
        << progress.peersTried << " tried" << kClearLine << "\n";

    // The piece map only earns its line once the torrent is big enough for it
    // to mean something.
    if (progress.piecesTotal > 1 && width > 30) {
        out << kGreen << pieceMap(progress) << kReset << kClearLine << "\n";
    }

    std::string text = out.str();
    emit(text);
    std::fflush(stdout);
    linesDrawnLast_ = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

// =============================================================================
// log()
// =============================================================================
void TerminalUI::log(const std::string& message) {
    logLines_.push_back(message);
    while (logLines_.size() > kMaxLogLines) logLines_.pop_front();

    // In line mode there is no live display to fold the message into, so it
    // goes straight out. Dropping events when not on a terminal would be the
    // worst possible behaviour for someone reading a log.
    if (!interactive_) {
        std::fputs(("  " + message + "\n").c_str(), stdout);
        std::fflush(stdout);
    }
}

// =============================================================================
// finish() / abandon()
// =============================================================================
void TerminalUI::finish(const DownloadProgress& progress, bool ok, const std::string& error) {
    if (finished_) return;
    finished_ = true;

    if (!interactive_) {
        std::fputs(ok ? "\nDownload complete.\n"
                      : ("\nDownload failed: " + error + "\n").c_str(),
                   stdout);
        std::fflush(stdout);
        return;
    }

    // Collapse the live display so the final summary is not drawn on top of it.
    std::string rewind;
    for (int i = 0; i < linesDrawnLast_; i++) rewind += kCursorUp1;
    std::string clear;
    for (int i = 0; i < linesDrawnLast_; i++) { clear += kClearLine; clear += "\n"; }
    emit(rewind + clear);
    linesDrawnLast_ = 0;

    std::ostringstream out;
    if (ok) {
        out << kGreen << kBold << "Done." << kReset << " "
            << humanBytes(static_cast<uint64_t>(progress.bytesTotal)) << " in "
            << humanBytes(static_cast<uint64_t>(progress.bytesDone)) << ", verified piece by piece.\n";
    } else {
        out << kRed << kBold << "Failed." << kReset << " " << error << "\n";
        if (progress.piecesTotal > 0) {
            out << kYellow << "  " << progress.piecesDone << "/" << progress.piecesTotal
                << " pieces completed before stopping.\n"
                << kReset;
        }
        out << kDim << "  Re-run the same command to resume: completed pieces are "
                       "re-verified from disk.\n"
            << kReset;
    }
    std::fputs(out.str().c_str(), stdout);
    std::fflush(stdout);
    std::fputs(kShowCursor, stdout);
    std::fflush(stdout);
}

void TerminalUI::abandon() {
    if (finished_) return;
    finished_ = true;

    if (interactive_) {
        // Put the cursor back on a clean line. Skipping this is how a program
        // leaves an invisible cursor and a user needs to run `reset`.
        std::string rewind;
        for (int i = 0; i < linesDrawnLast_; i++) rewind += kCursorUp1;
        std::string clear;
        for (int i = 0; i < linesDrawnLast_; i++) { clear += kClearLine; clear += "\n"; }
        emit(rewind + clear);
        std::fputs(kShowCursor, stdout);
        std::fflush(stdout);
    }
}
