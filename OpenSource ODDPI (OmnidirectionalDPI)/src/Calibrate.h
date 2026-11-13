#pragma once

// Directional calibration: measures how you flick in each direction and evens it out.
//
// A short flick test: targets appear around a centre point in many directions. We record the
// raw mouse counts (read-only Raw Input - nothing is injected or hooked) and measure how far the
// first, ballistic part of each flick goes compared with the target. Directions where you
// consistently overshoot get a lower multiplier, directions where you undershoot a higher one,
// so every direction ends up behaving like the average one. A consistent angular error across
// all directions (grip tilt) becomes a Rotation correction.
//
// The stock Raw Accel driver can only scale the four half-axes (Right/Left/Up/Down), so the
// per-direction measurements are least-squares fitted onto those four multipliers.

#include <string>
#include <vector>

namespace oddpi {

    struct CalSample {
        double t;      // seconds since the target appeared
        double x, y;   // cursor position in mouse counts, screen orientation (+y = down)
    };

    struct CalTrial {
        int direction = 0;        // index into the direction list
        double angleDeg = 0;      // target direction, maths convention (0 = right, 90 = up)
        double distance = 0;      // target distance in counts
        bool practice = false;
        bool timedOut = false;
        std::vector<CalSample> samples;
    };

    struct CalTrialResult {
        bool valid = false;
        const wchar_t* rejectReason = L"";
        double reach = 0;       // primary movement length along the target direction / target distance
        double angleErr = 0;    // degrees, positive = movement came out counter-clockwise of the target
        double reaction = 0;    // seconds until movement started
    };

    struct CalDirectionStats {
        double angleDeg = 0;
        int valid = 0;
        int total = 0;
        double medianReach = 0;   // 0 if not enough data
    };

    struct CalResult {
        bool ok = false;
        std::wstring error;
        // Corrections to multiply onto the current settings (1.0 = no change).
        // Already shrunk toward 1 where the measurement is uncertain.
        double right = 1, left = 1, up = 1, down = 1;
        double raw[4] = { 1, 1, 1, 1 };          // R, L, U, D before shrinking
        double uncertainty[4] = {};              // bootstrap std dev of each, same order
        double rotationDeg = 0;          // add to the current Rotation (positive = clockwise)
        double rawRotationDeg = 0;       // measured value before the deadband
        double spreadBefore = 0;         // how unequal the directions were (std dev of reach, %)
        double spreadAfterFit = 0;       // predicted remaining spread after the correction (%)
        int validTrials = 0;
        int totalTrials = 0;
        std::vector<CalDirectionStats> directions;
    };

    struct CalOptions {
        int dpi = 800;                 // for physical target distances only
        int directions = 16;
        int repsPerDirection = 8;      // single flicks vary ~10-15%, so fewer is too noisy
        int practiceTrials = 8;
        double reachCm = 5.0;          // longest flick, in cm of hand movement (no game set)

        // Game mode: when > 0, the test behaves like your game. Targets are placed at view
        // angles (up to ~35 deg) and the screen uses the game's FOV, so every flick is the same
        // hand movement as in game. See Games.h / CountsPerDegree.
        double countsPerDegree = 0;
        double fov = 103;              // horizontal, degrees
        std::wstring label;            // shown in the test window, e.g. "Valorant 0.35 = 37.0 cm/360"

        // true: 3D test (fixed crosshair, the camera turns like in an FPS). false: 2D cursor test.
        bool scene3d = true;

        // 3D test: every click is one flick; it stops once every correction is known to within
        // stopUncertainty (checked after each round of all directions, from minRepsPerDirection on;
        // at most repsPerDirection rounds).
        bool sound = true;             // sound effects (3D test, sens finder, Gridshot)
        double volume = 0.6;

        bool adaptive = true;
        int minRepsPerDirection = 3;
        double stopUncertainty = 0.04;   // +-4%: corrections within the noise are shrunk anyway
        int practice3d = 4;
    };

    // Opens the fullscreen test. Returns false if the user aborted (Esc / window closed).
    bool RunCalibrationWindow(const CalOptions& opt, std::vector<CalTrial>& trials);

    // Plays the 3D test with a simulated player off-screen in fast-forward (no window, no real
    // input). gains = how far its first flick goes relative to the target for Right/Left/Up/Down.
    // Saves a few frames as PNG into framesDir when it is not empty. For end-to-end tests.
    bool RunCalibration3DBot(const CalOptions& opt, const double gains[4], double tiltDeg,
                             std::vector<CalTrial>& trials, const std::wstring& framesDir, unsigned seed = 1);

    CalTrialResult AnalyzeTrial(const CalTrial& trial);
    CalResult FitCalibration(const std::vector<CalTrial>& trials, int directionCount);

    // ---- Sens finder ------------------------------------------------------------------------
    // KovaaK's-style: each round plays a lower and a higher sensitivity blind (A/B, alternating
    // blocks of targets), scores time-to-click with a miss penalty, then moves toward the better
    // one and narrows the range. Uses the 3D trainer (opt.countsPerDegree = the current sens).

    struct SensSideStats {
        double multiplier = 1;   // relative to the current sens
        int targets = 0, hits = 0;
        double score = 0;        // mean seconds per target incl. miss penalty (lower = better)
        double se = 0;
    };

    struct SensRound {
        SensSideStats low, high;
        double centerBefore = 1, centerAfter = 1;
    };

    struct SensFinderOptions {
        CalOptions cal;              // dpi, countsPerDegree (current sens), fov, label
        std::wstring gameName;       // for the result text, e.g. "Valorant"
        double gameSens = 0;         // current in-game sensitivity
        int rounds = 6;
        int targetsPerRound = 16;    // split evenly between the two sensitivities
        int blockSize = 4;           // targets in a row before switching A/B
        double startFactor = 2.0;    // first round tests current / 2 and current * 2
        double shrink = 0.55;        // range factor kept each round
    };

    struct SensFinderResult {
        bool finished = false;
        double multiplier = 1;       // best sens = current sens * multiplier
        std::vector<SensRound> rounds;
    };

    bool RunSensFinder(const SensFinderOptions& opt, SensFinderResult& result);

    // Simulated player whose aim is best at idealMultiplier (slower and less accurate away from
    // it), played off-screen in fast-forward. For end-to-end tests.
    bool RunSensFinderBot(const SensFinderOptions& opt, double idealMultiplier, unsigned seed,
                          SensFinderResult& result, const std::wstring& framesDir);

    // How well the 4-value model (Right/Left/Up/Down) explains each measured direction.
    struct CalDirectionCheck {
        double angleDeg = 0;
        int n = 0;
        double median = 0;      // measured first-flick length / target
        double se = 0;          // bootstrap standard error of that median
        double predicted = 0;   // what the best 4-value fit predicts for this direction
    };
    // chi2 = sum(((median - predicted) / se)^2); about (directions - 4) if the model explains
    // everything up to noise, much larger if some directions (e.g. diagonals) differ for real.
    std::vector<CalDirectionCheck> CheckDirections(const std::vector<CalTrial>& trials, int directionCount, double& chi2);

    // Every flick's raw samples as CSV, so results can be re-analysed later (`oddpi analyze`).
    bool SaveTrials(const std::wstring& path, const CalOptions& opt, const std::vector<CalTrial>& trials);
    bool LoadTrials(const std::wstring& path, CalOptions& opt, std::vector<CalTrial>& trials, std::wstring& error);

    // Runs the analysis on synthetic flicks with known errors and checks they are recovered.
    // Returns 0 on success.
    int RunCalibrationSelfTest();
}
