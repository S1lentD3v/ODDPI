#pragma once

// Gridshot: a timed "test your aim" mode in the 3D trainer, with an improvement board.
// Three targets on a grid at a time; each hit spawns a new one somewhere else.
// Score = hits x 100 x accuracy. Every finished run is saved with the sensitivity used and
// whether ODDPI was on; the board charts score over time (rolling average, personal best) and
// shows how much you improved, including ODDPI on vs off.

#include "Calibrate.h"

#include <string>
#include <vector>

namespace oddpi {

    struct RunRecord {
        std::wstring when;        // "2026-11-13 00:41"
        int score = 0;
        int hits = 0, misses = 0;
        double seconds = 60;
        double avgKillMs = 0;
        std::wstring game;        // e.g. "Valorant", may be empty
        double gameSens = 0;
        double cm360 = 0;
        bool oddpiOn = false;
        std::wstring settings;    // e.g. "R1.15 L1.01 U1.00 D0.84"
    };

    struct ProgressStats {
        int runs = 0;
        int best = 0;
        double firstAvg = 0;      // average of the first few runs
        double recentAvg = 0;     // average of the last few runs
        double improvement = 0;   // recentAvg / firstAvg - 1 (0 when too few runs)
        int window = 0;           // how many runs firstAvg/recentAvg use
        double onAvg = 0, offAvg = 0;   // ODDPI on / off averages (0 = no runs)
        int onRuns = 0, offRuns = 0;
    };

    struct GridshotOptions {
        CalOptions cal;           // dpi, countsPerDegree, fov, sound, volume
        std::wstring gameName;
        double gameSens = 0;
        bool oddpiOn = false;
        std::wstring settings;
        double seconds = 60;
        std::wstring historyPath;   // empty = DefaultHistoryPath()
    };

    struct GridshotResult {
        int runs = 0;             // runs finished this session (play again counts)
        RunRecord last;
        bool personalBest = false;
        ProgressStats stats;      // over the whole history, after the last run
    };

    std::wstring DefaultHistoryPath();                          // %LOCALAPPDATA%\ODDPI\gridshot.csv
    std::vector<RunRecord> LoadHistory(const std::wstring& path);   // oldest first
    bool AddToHistory(const std::wstring& path, const RunRecord& r);
    ProgressStats ComputeProgress(const std::vector<RunRecord>& history);
    int GridshotScore(int hits, int misses);

    // Opens the fullscreen Gridshot (click to play again, Esc to leave). False if nothing finished.
    bool RunGridshot(const GridshotOptions& opt, GridshotResult& result);

    // A simulated player plays `runs` runs off-screen in fast-forward (for tests).
    bool RunGridshotBot(const GridshotOptions& opt, double idealMultiplier, unsigned seed, int runs,
                        GridshotResult& result, const std::wstring& framesDir);
}
