#pragma once

// =============================================================================
// TerminalUI.hpp - The live download display
// =============================================================================
//
// A download can take minutes, and "no output for four minutes" is
// indistinguishable from "hung". This draws what is happening, in place, so
// the terminal stays useful while the client works.
//
// WHY HAND-ROLLED ANSI INSTEAD OF NCURSES
// ----------------------------------------
// Three reasons, in order of weight:
//
//   1. No new dependency. The whole project is C++17 plus OpenSSL, and it has
//      stayed that way on purpose. Adding a C library for eight escape codes
//      would undo that.
//   2. It degrades instead of breaking. Piping the client into a file or
//      through `tee` is a normal thing to want, and ncurses has no useful
//      answer for a non-terminal stdout. Here, isatty() decides: interactive
//      gets the live display, anything else gets periodic plain lines that
//      still parse and diff well.
//   3. It is testable. No global terminal state to set up, so the formatting
//      can be exercised without a pty.
//
// The awkward part of a live display is that you cannot just "print" - you have
// to go back and rewrite what you printed last time. So the class counts the
// lines it drew last frame and moves the cursor back up to them, clearing each
// as it redraws. The two details that matter for not looking broken:
//
//   - the cursor is made visible again on exit, however we leave, because a
//     Ctrl-C at the wrong moment would otherwise leave an invisible cursor and
//     a terminal the user has to reset;
//   - SIGWINCH is not handled, so resizing mid-download can smear the layout.
//     The next frame redraws everything, so it self-heals; only the one frame
//     in between is wrong.
// =============================================================================

#include "peer/ConcurrentDownloader.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class TerminalUI {
public:
    // Decide once whether we are talking to a real terminal. `forceLines` is
    // the --no-tui flag, and it makes the choice deterministic for scripts and
    // for tests.
    explicit TerminalUI(bool forceLines = false);

    // True if we are drawing the live display (as opposed to line mode).
    bool interactive() const { return interactive_; }

    // Called from ConcurrentDownloader's single reporter thread, so no locking
    // is needed here. Safe to call at any rate.
    void onProgress(const DownloadProgress& progress);

    // A line for the scrolling log pane. Also printed immediately in line mode,
    // so events are never lost when the display is not interactive.
    void log(const std::string& message);

    // Leave the terminal in a sane state and print a final summary. Safe to
    // call more than once; only the first call does anything.
    void finish(const DownloadProgress& progress, bool ok, const std::string& error);

    // Recover from an exception or a signal without leaving escape codes in
    // the scrollback and the cursor hidden.
    void abandon();

private:
    void draw(const DownloadProgress& progress);
    void drawInteractive(const DownloadProgress& progress);
    void drawLines(const DownloadProgress& progress);
    void renderBar(double fraction, int width);
    std::string pieceMap(const DownloadProgress& progress) const;
    void emit(const std::string& s);

    bool interactive_ = false;
    bool finished_ = false;

    int linesDrawnLast_ = 0;   // how many rows the last frame occupied
    int lastPercent_ = -1;     // only redraw a line-mode line when it changes

    std::deque<std::string> logLines_;
    static constexpr size_t kMaxLogLines = 6;

    // Rolling-window speed tracking. A lifetime average is the wrong number:
    // it never recovers from a slow start and the ETA it produces drifts
    // further and further from reality.
    uint64_t lastBytes_ = 0;
    uint64_t lastSampleMs_ = 0;
    double bytesPerSecond_ = 0.0;

    // Ring of recent (timestamp, cumulative bytes) samples for the window.
    std::deque<std::pair<uint64_t, uint64_t>> speedSamples_;
    static constexpr int kSpeedWindowMs = 4000;
};
