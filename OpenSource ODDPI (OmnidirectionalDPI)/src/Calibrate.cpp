#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>

#include "Calibrate.h"
#include "Sounds.h"
#include "Gridshot.h"

#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }  // GDI+ headers need these with NOMINMAX
#include <objidl.h>
#include <gdiplus.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")

namespace oddpi {

namespace {

    constexpr double kPi = 3.14159265358979323846;
    double Rad(double deg) { return deg * kPi / 180.0; }
    double Deg(double rad) { return rad * 180.0 / kPi; }

    double WrapDeg(double d)
    {
        while (d > 180) d -= 360;
        while (d <= -180) d += 360;
        return d;
    }

    double Median(std::vector<double> v)
    {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        size_t n = v.size();
        return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    }

    double StdDevPct(const std::vector<double>& v)
    {
        if (v.size() < 2) return 0;
        double mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
        double ss = 0;
        for (double x : v) ss += (x - mean) * (x - mean);
        return 100.0 * std::sqrt(ss / double(v.size() - 1)) / mean;
    }

    // Raw Accel scales x by Right/Left and y by Up/Down, so in direction `angle` a correction
    // (r,l,u,d) scales movement length by sqrt((cx*cos)^2 + (cy*sin)^2).
    double ModelScale(const double c[4], double angleDeg)
    {
        double co = std::cos(Rad(angleDeg)), si = std::sin(Rad(angleDeg));
        double cx = co >= 0 ? c[0] : c[1];
        double cy = si >= 0 ? c[2] : c[3];
        return std::sqrt(cx * cx * co * co + cy * cy * si * si);
    }

    // Solves A x = b (4x4) with partial pivoting. Returns false if singular.
    bool Solve4(double A[4][4], double b[4], double x[4])
    {
        for (int col = 0; col < 4; ++col) {
            int piv = col;
            for (int r = col + 1; r < 4; ++r)
                if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            if (std::fabs(A[piv][col]) < 1e-12) return false;
            std::swap(A[piv], A[col]);
            std::swap(b[piv], b[col]);
            for (int r = col + 1; r < 4; ++r) {
                double f = A[r][col] / A[col][col];
                for (int k = col; k < 4; ++k) A[r][k] -= f * A[col][k];
                b[r] -= f * b[col];
            }
        }
        for (int r = 3; r >= 0; --r) {
            double s = b[r];
            for (int k = r + 1; k < 4; ++k) s -= A[r][k] * x[k];
            x[r] = s / A[r][r];
        }
        return true;
    }

    double DirectionAngle(int index, int count) { return 360.0 * index / count; }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------------------

CalTrialResult AnalyzeTrial(const CalTrial& trial)
{
    CalTrialResult r;
    auto reject = [&](const wchar_t* why) { r.valid = false; r.rejectReason = why; return r; };

    if (trial.timedOut) return reject(L"timed out");
    if (trial.samples.size() < 3 || trial.distance <= 0) return reject(L"no movement");

    // Resample the step-wise cursor path onto a 1 ms grid (linear between input reports).
    const double dt = 0.001;
    const double T = trial.samples.back().t;
    const size_t n = size_t(T / dt) + 1;
    if (n < 20) return reject(L"too short");

    std::vector<double> px(n), py(n);
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        double t = double(i) * dt;
        while (j + 1 < trial.samples.size() && trial.samples[j + 1].t < t) ++j;
        const CalSample& a = trial.samples[j];
        const CalSample& b = trial.samples[std::min(j + 1, trial.samples.size() - 1)];
        double f = (b.t > a.t) ? std::clamp((t - a.t) / (b.t - a.t), 0.0, 1.0) : 1.0;
        px[i] = a.x + (b.x - a.x) * f;
        py[i] = a.y + (b.y - a.y) * f;
    }

    // Progress along the target direction (screen y is down, the angle is maths-style).
    const double ux = std::cos(Rad(trial.angleDeg)), uy = -std::sin(Rad(trial.angleDeg));
    const double D = trial.distance;
    std::vector<double> s(n), v(n);
    for (size_t i = 0; i < n; ++i) s[i] = px[i] * ux + py[i] * uy;

    const size_t h = 8;  // +-8 ms smoothing window; works down to 125 Hz polling
    for (size_t i = 0; i < n; ++i) {
        size_t lo = i > h ? i - h : 0, hi = std::min(i + h, n - 1);
        v[i] = (hi > lo) ? (s[hi] - s[lo]) / (double(hi - lo) * dt) : 0;
    }

    size_t onset = n;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] > 0.05 * D) { onset = i; break; }
    }
    if (onset == n) return reject(L"did not move toward the target");
    r.reaction = double(onset) * dt;
    if (r.reaction < 0.08) return reject(L"moved before the target appeared");
    if (r.reaction > 1.0) return reject(L"reacted too slowly");  // distracted, not a natural flick

    // Peak speed of the primary (ballistic) movement.
    size_t peak = onset;
    for (size_t i = onset; i < n; ++i) {
        if (v[i] > v[peak]) peak = i;
        if (s[i] > 0.3 * D && v[i] < 0.3 * v[peak]) break;  // primary is clearly over
    }
    const double vmax = v[peak];
    if (vmax <= 0) return reject(L"no movement");

    // End of the primary movement: speed drops to 10% of the peak, or a dip in speed that is
    // followed by a new acceleration (the start of a correction).
    size_t end = n - 1;
    for (size_t i = peak + 1; i + 1 < n; ++i) {
        if (v[i] <= 0.1 * vmax) { end = i; break; }
        if (v[i] < 0.6 * vmax && v[i] <= v[i - 1] && v[i] <= v[i + 1]) {
            double later = v[i];
            for (size_t k = i; k < std::min(i + 40, n); ++k) later = std::max(later, v[k]);
            if (later > v[i] + 0.1 * vmax) { end = i; break; }
        }
    }

    r.reach = s[end] / D;
    r.angleErr = WrapDeg(Deg(std::atan2(-py[end], px[end])) - trial.angleDeg);

    if (r.reach < 0.3 || r.reach > 2.5) return reject(L"flick far off the target distance");
    if (std::fabs(r.angleErr) > 30) return reject(L"flick in the wrong direction");

    r.valid = true;
    return r;
}

namespace {

    // Keep overall sensitivity the same: average scale over all directions = 1.
    void NormalizeAxes(double c[4], int directionCount)
    {
        double avg = 0;
        for (int k = 0; k < directionCount; ++k) avg += ModelScale(c, DirectionAngle(k, directionCount));
        avg /= double(directionCount);
        for (int i = 0; i < 4; ++i) c[i] = std::clamp(c[i] / avg, 0.7, 1.4);
    }

    // Fits Right/Left/Up/Down to per-direction median reaches (0 = no data).
    // Desired scale per direction is 1/reach. Least squares on the squared model
    //   want^2 = cx^2 cos^2 + cy^2 sin^2   (linear in R^2, L^2, U^2, D^2)
    // with a light ridge toward the average so a half-axis with little data stays sane.
    bool FitAxes(const std::vector<double>& med, const std::vector<int>& weight, int directionCount, double c[4])
    {
        double A[4][4] = {}, b[4] = {}, z[4] = {};
        double prior = 0;
        int n = 0;
        for (double m : med) {
            if (m > 0) { prior += 1.0 / (m * m); ++n; }
        }
        if (n == 0) return false;
        prior /= double(n);

        for (int k = 0; k < directionCount; ++k) {
            double m = med[size_t(k)];
            if (m <= 0) continue;
            double ang = DirectionAngle(k, directionCount);
            double co = std::cos(Rad(ang)), si = std::sin(Rad(ang));
            double row[4] = {};
            row[co >= 0 ? 0 : 1] = co * co;
            row[si >= 0 ? 2 : 3] = si * si;
            double want = 1.0 / (m * m);
            double w = double(weight[size_t(k)]);
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) A[i][j] += w * row[i] * row[j];
                b[i] += w * row[i] * want;
            }
        }
        const double ridge = 0.05;
        for (int i = 0; i < 4; ++i) {
            A[i][i] += ridge;
            b[i] += ridge * prior;
        }
        if (!Solve4(A, b, z)) return false;
        for (int i = 0; i < 4; ++i) c[i] = std::sqrt(std::max(z[i], 0.25));
        NormalizeAxes(c, directionCount);
        return true;
    }

}  // namespace

CalResult FitCalibration(const std::vector<CalTrial>& trials, int directionCount)
{
    CalResult res;
    res.directions.resize(size_t(directionCount));
    std::vector<std::vector<double>> reaches(static_cast<size_t>(directionCount));
    std::vector<double> angleErrs;

    for (int k = 0; k < directionCount; ++k) res.directions[size_t(k)].angleDeg = DirectionAngle(k, directionCount);

    for (const auto& t : trials) {
        if (t.practice || t.direction < 0 || t.direction >= directionCount) continue;
        ++res.totalTrials;
        ++res.directions[size_t(t.direction)].total;
        CalTrialResult a = AnalyzeTrial(t);
        if (!a.valid) continue;
        ++res.validTrials;
        ++res.directions[size_t(t.direction)].valid;
        reaches[size_t(t.direction)].push_back(a.reach);
        angleErrs.push_back(a.angleErr);
    }

    // Per-direction medians; a direction needs at least 2 good flicks.
    std::vector<double> medians;
    int covered = 0;
    for (int k = 0; k < directionCount; ++k) {
        if (reaches[size_t(k)].size() >= 2) {
            res.directions[size_t(k)].medianReach = Median(reaches[size_t(k)]);
            medians.push_back(res.directions[size_t(k)].medianReach);
            ++covered;
        }
    }

    if (res.validTrials < directionCount * 2 || covered < directionCount * 3 / 4) {
        res.error = L"Not enough usable flicks (" + std::to_wstring(res.validTrials) + L" of " +
                    std::to_wstring(res.totalTrials) + L"). Try again: rest still on the centre, then make one "
                    L"quick flick to each target.";
        return res;
    }
    res.spreadBefore = StdDevPct(medians);

    const size_t dirs = static_cast<size_t>(directionCount);
    std::vector<double> med(dirs);
    std::vector<int> weight(dirs);
    for (int k = 0; k < directionCount; ++k) {
        med[size_t(k)] = res.directions[size_t(k)].medianReach;
        weight[size_t(k)] = res.directions[size_t(k)].valid;
    }

    double raw[4];
    if (!FitAxes(med, weight, directionCount, raw)) {
        res.error = L"Could not fit the measurements (singular system).";
        return res;
    }

    // How sure are we? Bootstrap: resample the flicks of every direction, refit, and look at
    // how much each correction moves. Real flicks vary by ~10-15% each, so with a few flicks
    // per direction a few % of "correction" can be pure noise.
    std::mt19937 rng(12345);  // fixed: the same log always gives the same result
    const int B = 300;
    double sum[4] = {}, sumSq[4] = {};
    int good = 0;
    std::vector<double> bootMed(dirs);
    for (int iter = 0; iter < B; ++iter) {
        for (int k = 0; k < directionCount; ++k) {
            const auto& list = reaches[size_t(k)];
            if (list.size() < 2) { bootMed[size_t(k)] = 0; continue; }
            std::uniform_int_distribution<size_t> pick(0, list.size() - 1);
            std::vector<double> sample(list.size());
            for (double& x : sample) x = list[pick(rng)];
            bootMed[size_t(k)] = Median(sample);
        }
        double cb[4];
        if (!FitAxes(bootMed, weight, directionCount, cb)) continue;
        ++good;
        for (int i = 0; i < 4; ++i) {
            sum[i] += cb[i];
            sumSq[i] += cb[i] * cb[i];
        }
    }

    // Shrink each correction toward 1 by how reliable it is: a change much larger than its
    // uncertainty is kept almost fully, one within the noise is mostly dropped.
    double c[4];
    for (int i = 0; i < 4; ++i) {
        double mean = good ? sum[i] / good : raw[i];
        double sd = good > 1 ? std::sqrt(std::max(0.0, sumSq[i] / good - mean * mean)) : 0;
        double d = raw[i] - 1;
        double keep = (d * d + sd * sd) > 0 ? d * d / (d * d + sd * sd) : 0;
        res.uncertainty[i] = sd;
        res.raw[i] = raw[i];
        c[i] = 1 + keep * d;
    }
    NormalizeAxes(c, directionCount);

    res.right = c[0];
    res.left = c[1];
    res.up = c[2];
    res.down = c[3];

    std::vector<double> predicted;
    for (const auto& d : res.directions) {
        if (d.medianReach > 0) predicted.push_back(d.medianReach * ModelScale(c, d.angleDeg));
    }
    res.spreadAfterFit = StdDevPct(predicted);

    // Grip tilt: the median angular error over every direction. Under 1 degree is noise.
    // Movement that came out counter-clockwise is fixed by rotating clockwise (positive).
    res.rawRotationDeg = Median(angleErrs);
    res.rotationDeg = std::fabs(res.rawRotationDeg) < 1.0 ? 0.0 : std::clamp(res.rawRotationDeg, -15.0, 15.0);

    res.ok = true;
    return res;
}

std::vector<CalDirectionCheck> CheckDirections(const std::vector<CalTrial>& trials, int directionCount, double& chi2)
{
    const size_t dirs = static_cast<size_t>(directionCount);
    std::vector<std::vector<double>> reaches(dirs);
    for (const auto& t : trials) {
        if (t.practice || t.direction < 0 || t.direction >= directionCount) continue;
        const CalTrialResult a = AnalyzeTrial(t);
        if (a.valid) reaches[size_t(t.direction)].push_back(a.reach);
    }

    std::vector<CalDirectionCheck> out(dirs);
    std::vector<double> med(dirs, 0);
    std::vector<int> weight(dirs, 0);
    std::mt19937 rng(777);
    for (size_t k = 0; k < dirs; ++k) {
        auto& c = out[k];
        c.angleDeg = DirectionAngle(int(k), directionCount);
        c.n = int(reaches[k].size());
        if (c.n < 2) continue;
        c.median = med[k] = Median(reaches[k]);
        weight[k] = c.n;
        // Bootstrap standard error of the median.
        std::uniform_int_distribution<size_t> pick(0, reaches[k].size() - 1);
        double s = 0, s2 = 0;
        const int B = 400;
        for (int b = 0; b < B; ++b) {
            std::vector<double> sample(reaches[k].size());
            for (double& x : sample) x = reaches[k][pick(rng)];
            const double m = Median(sample);
            s += m;
            s2 += m * m;
        }
        c.se = std::sqrt(std::max(0.0, s2 / B - (s / B) * (s / B)));
    }

    // Best 4-value fit: the model says reach(angle) = k / scale(angle).
    double c[4];
    chi2 = 0;
    if (!FitAxes(med, weight, directionCount, c)) return out;
    double num = 0, den = 0;
    for (size_t k = 0; k < dirs; ++k) {
        if (out[k].n < 2) continue;
        const double shape = 1.0 / ModelScale(c, out[k].angleDeg);
        num += out[k].median * shape;
        den += shape * shape;
    }
    const double scale = den > 0 ? num / den : 1;
    for (size_t k = 0; k < dirs; ++k) {
        if (out[k].n < 2) continue;
        out[k].predicted = scale / ModelScale(c, out[k].angleDeg);
        const double se = std::max(out[k].se, 0.01);
        chi2 += std::pow((out[k].median - out[k].predicted) / se, 2);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Trial log (CSV)
// ---------------------------------------------------------------------------------------------

bool SaveTrials(const std::wstring& path, const CalOptions& opt, const std::vector<CalTrial>& trials)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return false;
    fprintf(f, "# ODDPI calibration log v1\n");
    fprintf(f, "# directions=%d dpi=%d reps=%d practice=%d reachcm=%.2f\n",
            opt.directions, opt.dpi, opt.repsPerDirection, opt.practiceTrials, opt.reachCm);
    fprintf(f, "# countsperdegree=%.6f fov=%.1f scene=%s\n", opt.countsPerDegree, opt.fov, opt.scene3d ? "3d" : "2d");
    fprintf(f, "trial,direction,angle,distance,practice,timedout,t,x,y\n");
    for (size_t i = 0; i < trials.size(); ++i) {
        const auto& t = trials[i];
        for (const auto& s : t.samples) {
            fprintf(f, "%zu,%d,%.3f,%.3f,%d,%d,%.6f,%.0f,%.0f\n", i, t.direction, t.angleDeg, t.distance,
                    t.practice ? 1 : 0, t.timedOut ? 1 : 0, s.t, s.x, s.y);
        }
    }
    bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

bool LoadTrials(const std::wstring& path, CalOptions& opt, std::vector<CalTrial>& trials, std::wstring& error)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) {
        error = L"Could not open " + path;
        return false;
    }
    trials.clear();
    char line[512];
    long long lastTrial = -1;
    bool header = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            if (strstr(line, "directions=")) {
                sscanf_s(line, "# directions=%d dpi=%d reps=%d practice=%d reachcm=%lf",
                         &opt.directions, &opt.dpi, &opt.repsPerDirection, &opt.practiceTrials, &opt.reachCm);
                header = true;
            }
            continue;
        }
        long long trial;
        int dir, practice, timedOut;
        double angle, dist, t, x, y;
        if (sscanf_s(line, "%lld,%d,%lf,%lf,%d,%d,%lf,%lf,%lf", &trial, &dir, &angle, &dist, &practice,
                     &timedOut, &t, &x, &y) != 9) continue;  // column header line
        if (trial != lastTrial) {
            CalTrial ct;
            ct.direction = dir;
            ct.angleDeg = angle;
            ct.distance = dist;
            ct.practice = practice != 0;
            ct.timedOut = timedOut != 0;
            trials.push_back(std::move(ct));
            lastTrial = trial;
        }
        trials.back().samples.push_back({ t, x, y });
    }
    fclose(f);
    if (!header || trials.empty() || opt.directions <= 0) {
        error = L"Not an ODDPI calibration log: " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Self test with synthetic flicks
// ---------------------------------------------------------------------------------------------

namespace {

    // Minimum-jerk position profile 0..1.
    double MinJerk(double u)
    {
        u = std::clamp(u, 0.0, 1.0);
        return u * u * u * (10 - 15 * u + 6 * u * u);
    }

    std::vector<CalTrial> MakeSyntheticTrials(int dirs, int reps, double gR, double gL, double gU, double gD,
                                              double tiltDeg, double pollHz, double noiseSd, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::normal_distribution<double> noise(0.0, noiseSd);
        std::normal_distribution<double> angNoise(0.0, 2.0);
        std::uniform_real_distribution<double> dist(0.45, 1.0);
        const double g[4] = { gR, gL, gU, gD };

        std::vector<CalTrial> out;
        for (int rep = 0; rep < reps; ++rep) {
            for (int k = 0; k < dirs; ++k) {
                CalTrial t;
                t.direction = k;
                t.angleDeg = DirectionAngle(k, dirs);
                t.distance = 1575 * dist(rng);  // 5 cm at 800 DPI

                // Primary flick: reach = user bias * direction gain, plus noise and grip tilt.
                const double reach = 0.93 * ModelScale(g, t.angleDeg) * (1 + noise(rng));
                const double ang = Rad(t.angleDeg + tiltDeg + angNoise(rng));
                const double ex = std::cos(ang) * reach * t.distance, ey = -std::sin(ang) * reach * t.distance;
                const double tx = std::cos(Rad(t.angleDeg)) * t.distance, ty = -std::sin(Rad(t.angleDeg)) * t.distance;

                const double rt = 0.22, prim = 0.17, pause = 0.04, corr = 0.15;
                const double total = rt + prim + pause + corr + 0.05;
                double cx = 0, cy = 0;  // integer counts actually reported
                t.samples.push_back({ 0, 0, 0 });
                for (double time = 1.0 / pollHz; time < total; time += 1.0 / pollHz) {
                    double fx, fy;
                    if (time < rt) { fx = 0; fy = 0; }
                    else if (time < rt + prim) {
                        double m = MinJerk((time - rt) / prim);
                        fx = ex * m; fy = ey * m;
                    }
                    else if (time < rt + prim + pause) { fx = ex; fy = ey; }
                    else {
                        double m = MinJerk((time - rt - prim - pause) / corr);
                        fx = ex + (tx - ex) * m; fy = ey + (ty - ey) * m;
                    }
                    double nx = std::round(fx), ny = std::round(fy);
                    if (nx != cx || ny != cy) {
                        cx = nx; cy = ny;
                        t.samples.push_back({ time, cx, cy });
                    }
                }
                t.samples.push_back({ total, cx, cy });
                out.push_back(std::move(t));
            }
        }
        return out;
    }

}  // namespace

int RunCalibrationSelfTest()
{
    int failures = 0;
    auto check = [&](bool ok, const wchar_t* what, double got, double want) {
        wprintf(L"  [%ls] %-34ls got %7.3f  want %7.3f\n", ok ? L"ok  " : L"FAIL", what, got, want);
        if (!ok) ++failures;
    };

    struct Case { const wchar_t* name; double r, l, u, d, tilt, hz, noise; int reps; double tol; };
    const Case cases[] = {
        { L"neutral, 1000 Hz",                    1.00, 1.00, 1.00, 1.00,  0.0, 1000, 0.05, 5, 0.05 },
        { L"uneven directions, 1000 Hz",          1.15, 1.00, 0.90, 1.05,  0.0, 1000, 0.05, 5, 0.05 },
        { L"uneven + 4 deg tilt, 125 Hz",         0.95, 1.10, 1.08, 0.92,  4.0,  125, 0.05, 5, 0.05 },
        { L"uneven + -3 deg tilt, 8000 Hz",       1.10, 0.95, 1.00, 1.12, -3.0, 8000, 0.05, 5, 0.05 },
        // Real flicks vary ~13% each (first real log). Only a strong effect should survive.
        { L"realistic noise, Down overshoot",     1.00, 1.00, 1.00, 1.12,  0.0, 1000, 0.13, 8, 0.06 },
        { L"realistic noise, neutral",            1.00, 1.00, 1.00, 1.00,  0.0, 1000, 0.13, 8, 0.05 },
    };

    unsigned seed = 1;
    for (const auto& c : cases) {
        wprintf(L"%ls\n", c.name);
        auto trials = MakeSyntheticTrials(16, c.reps, c.r, c.l, c.u, c.d, c.tilt, c.hz, c.noise, seed++);
        CalResult res = FitCalibration(trials, 16);
        if (!res.ok) {
            wprintf(L"  [FAIL] fit failed: %ls\n", res.error.c_str());
            ++failures;
            continue;
        }
        // The fit is normalised, so compare ratios: corrected gain should be equal everywhere.
        check(std::fabs(res.right * c.r / (res.left * c.l) - 1) < c.tol, L"Right/Left after correction", res.right * c.r / (res.left * c.l), 1.0);
        check(std::fabs(res.up * c.u / (res.down * c.d) - 1) < c.tol, L"Up/Down after correction", res.up * c.u / (res.down * c.d), 1.0);
        check(std::fabs(res.right * c.r / (res.up * c.u) - 1) < c.tol, L"Right/Up after correction", res.right * c.r / (res.up * c.u), 1.0);
        check(std::fabs(res.rawRotationDeg - c.tilt) < 1.0, L"Rotation (deg)", res.rawRotationDeg, c.tilt);
        check(res.validTrials >= res.totalTrials * 9 / 10, L"Valid flicks", res.validTrials, res.totalTrials);

        // The flick log must round-trip to the same result.
        wchar_t tmpDir[MAX_PATH], tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        GetTempFileNameW(tmpDir, L"odp", 0, tmp);
        CalOptions opt, loadedOpt;
        opt.directions = 16;
        std::vector<CalTrial> loaded;
        std::wstring err;
        bool io = SaveTrials(tmp, opt, trials) && LoadTrials(tmp, loadedOpt, loaded, err);
        DeleteFileW(tmp);
        CalResult again = io ? FitCalibration(loaded, loadedOpt.directions) : CalResult{};
        check(io && again.ok && std::fabs(again.left - res.left) < 1e-3 && std::fabs(again.rawRotationDeg - res.rawRotationDeg) < 0.05,
              L"Log save/load gives same Left", again.left, res.left);
    }

    wprintf(failures ? L"\n%d check(s) FAILED\n" : L"\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Test window
// ---------------------------------------------------------------------------------------------

namespace {

    double Now()
    {
        static const double freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return double(f.QuadPart);
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) / freq;
    }

    struct InputEvent {
        double t;
        double dx, dy;
        bool leftDown;
    };

    // Reads raw mouse input on its own thread so every report gets a precise timestamp,
    // independent of rendering. Read-only: nothing is hooked, blocked or injected.
    class RawMouseReader {
    public:
        bool Start()
        {
            thread_ = std::thread([this] { Run(); });
            std::unique_lock lock(mutex_);
            started_.wait(lock, [this] { return ready_; });
            return ok_;
        }

        void Stop()
        {
            if (threadId_) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
            if (thread_.joinable()) thread_.join();
        }

        void Drain(std::vector<InputEvent>& out)
        {
            std::lock_guard lock(mutex_);
            out.swap(events_);
            events_.clear();
        }

        bool SawAbsolute() const { return absolute_; }

    private:
        static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_INPUT) {
                auto* self = reinterpret_cast<RawMouseReader*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (self) self->OnInput(HRAWINPUT(lp));
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        void OnInput(HRAWINPUT h)
        {
            const double t = Now();
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
            if (ri.header.dwType != RIM_TYPEMOUSE) return;

            const RAWMOUSE& m = ri.data.mouse;
            InputEvent e{ t, 0, 0, (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0 };
            if (m.usFlags & MOUSE_MOVE_ABSOLUTE) absolute_ = true;
            else {
                e.dx = m.lLastX;
                e.dy = m.lLastY;
            }
            if (e.dx == 0 && e.dy == 0 && !e.leftDown) return;

            std::lock_guard lock(mutex_);
            events_.push_back(e);
        }

        void Run()
        {
            threadId_ = GetCurrentThreadId();
            WNDCLASSW wc = {};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"ODDPI_RawMouse";
            RegisterClassW(&wc);
            HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            bool ok = false;
            if (hwnd) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(this));
                RAWINPUTDEVICE rid{ 0x01, 0x02, RIDEV_INPUTSINK, hwnd };  // generic desktop / mouse
                ok = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
            }
            {
                std::lock_guard lock(mutex_);
                ok_ = ok;
                ready_ = true;
            }
            started_.notify_all();

            if (ok) {
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
                RAWINPUTDEVICE rid{ 0x01, 0x02, RIDEV_REMOVE, nullptr };
                RegisterRawInputDevices(&rid, 1, sizeof(rid));
            }
            if (hwnd) DestroyWindow(hwnd);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
        }

        std::thread thread_;
        DWORD threadId_ = 0;
        std::mutex mutex_;
        std::condition_variable started_;
        bool ready_ = false, ok_ = false;
        std::atomic<bool> absolute_{ false };
        std::vector<InputEvent> events_;
    };

    std::atomic<bool> g_abort{ false };

    LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) g_abort = true;
            return 0;
        case WM_ACTIVATE: {
            if (LOWORD(wp) != WA_INACTIVE) {
                RECT rc;
                GetWindowRect(hwnd, &rc);
                ClipCursor(&rc);
            }
            else ClipCursor(nullptr);
            return 0;
        }
        case WM_CLOSE:
            g_abort = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void FillCircle(HDC dc, int x, int y, int r, COLORREF color)
    {
        HBRUSH b = CreateSolidBrush(color);
        HGDIOBJ oldB = SelectObject(dc, b);
        HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
        SelectObject(dc, oldP);
        SelectObject(dc, oldB);
        DeleteObject(b);
    }

    void RingCircle(HDC dc, int x, int y, int r, COLORREF color, int width)
    {
        HPEN p = CreatePen(PS_SOLID, width, color);
        HGDIOBJ oldP = SelectObject(dc, p);
        HGDIOBJ oldB = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
        SelectObject(dc, oldB);
        SelectObject(dc, oldP);
        DeleteObject(p);
    }

    enum class Phase { Intro, Wait, Active, Done };

    // Practice first, then balanced shuffled blocks of every direction.
    // Distances are reachCounts * U(minFrac, 1).
    std::vector<CalTrial> MakePlan(const CalOptions& opt, double reachCounts, double minFrac, std::mt19937& rng)
    {
        std::uniform_real_distribution<double> distFrac(minFrac, 1.0);
        std::vector<CalTrial> plan;
        for (int i = 0; i < opt.practiceTrials; ++i) {
            CalTrial t;
            t.practice = true;
            t.direction = int((i * 5 + 1) % opt.directions);
            plan.push_back(t);
        }
        for (int rep = 0; rep < opt.repsPerDirection; ++rep) {
            std::vector<int> block(size_t(opt.directions));
            std::iota(block.begin(), block.end(), 0);
            std::shuffle(block.begin(), block.end(), rng);
            for (int k : block) {
                CalTrial t;
                t.direction = k;
                plan.push_back(t);
            }
        }
        for (auto& t : plan) {
            t.angleDeg = DirectionAngle(t.direction, opt.directions);
            t.distance = reachCounts * distFrac(rng);
        }
        return plan;
    }

    bool RunCalibration2D(const CalOptions& opt, std::vector<CalTrial>& trials);
    bool RunCalibration3D(const CalOptions& opt, std::vector<CalTrial>& trials);

}  // namespace

bool RunCalibrationWindow(const CalOptions& opt, std::vector<CalTrial>& trials)
{
    g_abort = false;
    trials.clear();
    return opt.scene3d ? RunCalibration3D(opt, trials) : RunCalibration2D(opt, trials);
}

namespace {

bool RunCalibration2D(const CalOptions& opt, std::vector<CalTrial>& trials)
{
    g_abort = false;
    trials.clear();

    // Real pixels, not DPI-scaled ones.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);

    // ---- Geometry. Without a game: the longest flick is reachCm of hand movement, drawn
    // linearly. With a game: cursor counts are view angles (countsPerDegree) shown through a
    // perspective projection with the game's FOV, like aiming in game.
    const bool gameMode = opt.countsPerDegree > 0;
    const double radiusPx = 0.40 * std::min(W, H);
    const double focal = (W / 2.0) / std::tan(Rad(std::clamp(opt.fov, 30.0, 160.0) / 2));
    const double maxDeg = std::min(35.0, Deg(std::atan(radiusPx / focal)));
    const double reachCounts = gameMode ? maxDeg * opt.countsPerDegree : opt.reachCm / 2.54 * opt.dpi;

    auto toPx = [&](double x, double y, int& ox, int& oy) {
        double k;
        if (!gameMode) k = radiusPx / reachCounts;
        else {
            const double r = std::hypot(x, y);
            const double ang = std::min(r / opt.countsPerDegree, 80.0);
            k = r > 0 ? focal * std::tan(Rad(ang)) / r : 0;
        }
        ox = W / 2 + int(std::lround(x * k));
        oy = H / 2 + int(std::lround(y * k));
    };

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> delay(0.15, 0.45);
    const std::vector<CalTrial> plan = MakePlan(opt, reachCounts, gameMode ? 0.35 : 0.45, rng);
    const int counted = opt.directions * opt.repsPerDirection;

    RawMouseReader reader;
    if (!reader.Start()) {
        fwprintf(stderr, L"error: could not read raw mouse input.\n");
        return false;
    }

    // ---- Fullscreen window on the primary monitor.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ODDPI_Calibration";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"ODDPI calibration", WS_POPUP | WS_VISIBLE,
                                0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        reader.Stop();
        fwprintf(stderr, L"error: could not open the calibration window.\n");
        return false;
    }
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    RECT clip = { 0, 0, W, H };
    ClipCursor(&clip);

    HDC wdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HFONT font = CreateFontW(-std::max(16, H / 45), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT bigFont = CreateFontW(-std::max(22, H / 30), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);

    const int cx0 = W / 2, cy0 = H / 2;
    const int targetR = std::max(12, int(radiusPx * 0.045));
    std::wstring header = L"ODDPI direction calibration    -    Esc = cancel";
    {
        wchar_t buf[256];
        const double cm = opt.dpi > 0 ? reachCounts / opt.dpi * 2.54 : 0;
        if (gameMode) swprintf_s(buf, L"    -    as in %ls: targets up to %.0f\x00B0 (%.1f cm)", opt.label.c_str(), maxDeg, cm);
        else swprintf_s(buf, L"    -    flicks up to %.1f cm (set your game for a closer match)", cm);
        header += buf;
    }

    Phase phase = Phase::Intro;
    size_t index = 0;
    double curX = 0, curY = 0;        // counts
    double waitStart = 0, lastMove = 0, waitNeeded = 0, t0 = 0;
    double flashUntil = 0;
    bool lastHit = false;
    CalTrial current;
    std::vector<InputEvent> events;

    auto startWait = [&](double now) {
        phase = Phase::Wait;
        waitStart = lastMove = now;
        waitNeeded = 0.25 + delay(rng);
        curX = curY = 0;
    };

    auto finishTrial = [&](double t, bool timedOut) {
        current.timedOut = timedOut;
        int tx, ty, px, py;
        toPx(std::cos(Rad(current.angleDeg)) * current.distance, -std::sin(Rad(current.angleDeg)) * current.distance, tx, ty);
        toPx(curX, curY, px, py);
        lastHit = !timedOut && std::hypot(px - tx, py - ty) <= targetR;
        flashUntil = t + 0.25;
        trials.push_back(std::move(current));
        current = CalTrial{};
        ++index;
        if (index >= plan.size()) phase = Phase::Done;
        else startWait(t);
    };

    while (!g_abort && phase != Phase::Done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_abort = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        reader.Drain(events);
        for (const auto& e : events) {
            switch (phase) {
            case Phase::Intro:
                if (e.leftDown) startWait(e.t);
                break;
            case Phase::Wait:
                if (e.dx != 0 || e.dy != 0) lastMove = e.t;
                break;
            case Phase::Active:
                if (e.t < t0) break;
                curX += e.dx;
                curY += e.dy;
                current.samples.push_back({ e.t - t0, curX, curY });
                if (e.leftDown) finishTrial(e.t, false);
                break;
            default:
                break;
            }
        }
        events.clear();

        const double now = Now();
        if (phase == Phase::Wait && now - std::max(waitStart, lastMove) >= waitNeeded) {
            current = plan[index];
            current.samples.clear();
            current.samples.push_back({ 0, 0, 0 });
            curX = curY = 0;
            t0 = now;
            phase = Phase::Active;
        }
        if (phase == Phase::Active && now - t0 > 3.0) finishTrial(now, true);

        // ---- Draw
        RECT all = { 0, 0, W, H };
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 22));
        FillRect(mem, &all, bg);
        DeleteObject(bg);

        SetTextColor(mem, RGB(150, 150, 160));
        RECT top = { 20, 16, W - 20, 80 };
        DrawTextW(mem, header.c_str(), -1, &top, DT_LEFT | DT_SINGLELINE);

        if (phase == Phase::Intro) {
            SelectObject(mem, bigFont);
            SetTextColor(mem, RGB(235, 235, 240));
            RECT box = { W / 6, H / 2 - H / 5, W - W / 6, H / 2 + H / 5 };
            DrawTextW(mem,
                L"Hold still on the centre. A red target will appear.\n"
                L"Flick to it in ONE quick motion and click - like a flick in a game.\n"
                L"Don't slow down to be careful; we measure your natural flick.\n\n"
                L"Click to start.",
                -1, &box, DT_CENTER | DT_WORDBREAK);
            SelectObject(mem, font);
        }
        else {
            wchar_t progress[64];
            if (index < size_t(opt.practiceTrials))
                swprintf_s(progress, L"Practice  %zu / %d", index + 1, opt.practiceTrials);
            else
                swprintf_s(progress, L"%zu / %d", std::min(index - opt.practiceTrials + 1, size_t(counted)), counted);
            RECT pr = { 20, 16, W - 20, 80 };
            DrawTextW(mem, progress, -1, &pr, DT_RIGHT | DT_SINGLELINE);

            const bool flashing = now < flashUntil;
            COLORREF ring = phase == Phase::Wait
                ? (flashing ? (lastHit ? RGB(80, 200, 120) : RGB(200, 90, 90)) : RGB(110, 110, 125))
                : RGB(60, 60, 70);
            RingCircle(mem, cx0, cy0, 12, ring, 2);

            if (phase == Phase::Active) {
                int tx, ty;
                toPx(std::cos(Rad(current.angleDeg)) * current.distance, -std::sin(Rad(current.angleDeg)) * current.distance, tx, ty);
                FillCircle(mem, tx, ty, targetR, RGB(230, 70, 70));
                FillCircle(mem, tx, ty, std::max(2, targetR / 6), RGB(255, 220, 220));
            }

            int px, py;
            toPx(curX, curY, px, py);
            px = std::clamp(px, 0, W - 1);
            py = std::clamp(py, 0, H - 1);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(240, 240, 245));
            HGDIOBJ oldPen = SelectObject(mem, pen);
            MoveToEx(mem, px - 9, py, nullptr); LineTo(mem, px - 3, py);
            MoveToEx(mem, px + 4, py, nullptr); LineTo(mem, px + 10, py);
            MoveToEx(mem, px, py - 9, nullptr); LineTo(mem, px, py - 3);
            MoveToEx(mem, px, py + 4, nullptr); LineTo(mem, px, py + 10);
            SelectObject(mem, oldPen);
            DeleteObject(pen);
        }

        if (wdc) BitBlt(wdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        if (FAILED(DwmFlush())) Sleep(1);
    }

    ClipCursor(nullptr);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(font);
    DeleteObject(bigFont);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (wdc) ReleaseDC(hwnd, wdc);
    if (hwnd) DestroyWindow(hwnd);
    if (wc.lpszClassName) UnregisterClassW(wc.lpszClassName, wc.hInstance);
    reader.Stop();

    if (reader.SawAbsolute()) {
        wprintf(L"note: some mouse input was absolute (remote desktop, tablet or VM) and was ignored.\n");
    }
    return !g_abort && phase == Phase::Done;
}


// ---------------------------------------------------------------------------------------------
// 3D test: fixed crosshair, the camera turns with the mouse exactly like in an FPS.
//
// Every click is one measured flick (no "home" target): the next target appears, a set
// distance and direction away from wherever the crosshair is, once the mouse is still. Targets
// sit in yaw/pitch space, and the camera's yaw/pitch is linear in mouse counts, so the counts a
// flick needs are exactly (cos, -sin) * distance - the space the analysis works in.
// The test stops by itself once the result is precise enough (adaptive), or after
// repsPerDirection rounds of all directions.
//
// A simulated player (bot) can play it off-screen in fast-forward for end-to-end testing.
// ---------------------------------------------------------------------------------------------

struct Vec3 { double x, y, z; };

double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// x = right, y = up, z = forward at yaw 0 / pitch 0. Positive yaw turns right.
Vec3 Direction(double yawDeg, double pitchDeg)
{
    const double y = Rad(yawDeg), p = Rad(pitchDeg);
    return { std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y) };
}

struct Camera {
    double yaw = 0, pitch = 0;
    double focal = 1;
    int W = 0, H = 0;
    Vec3 f{}, r{}, u{};

    void Update()
    {
        pitch = std::clamp(pitch, -89.0, 89.0);
        const double y = Rad(yaw), p = Rad(pitch);
        f = Direction(yaw, pitch);
        r = { std::cos(y), 0, -std::sin(y) };
        u = { -std::sin(p) * std::sin(y), std::cos(p), -std::sin(p) * std::cos(y) };
    }

    bool Project(const Vec3& v, double& sx, double& sy, double& z) const
    {
        z = Dot(v, f);
        if (z < 0.02) return false;
        sx = W / 2.0 + focal * Dot(v, r) / z;
        sy = H / 2.0 - focal * Dot(v, u) / z;
        return true;
    }

    double AngleTo(const Vec3& v) const
    {
        const double len = std::sqrt(Dot(v, v));
        return Deg(std::acos(std::clamp(Dot(v, f) / len, -1.0, 1.0)));
    }
};

Gdiplus::Color GC(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void VerticalGradient(HDC dc, int x0, int y0, int x1, int y1, COLORREF top, COLORREF bottom)
{
    if (y1 <= y0 || x1 <= x0) return;
    TRIVERTEX v[2] = {
        { LONG(x0), LONG(y0), COLOR16(GetRValue(top) << 8), COLOR16(GetGValue(top) << 8), COLOR16(GetBValue(top) << 8), 0 },
        { LONG(x1), LONG(y1), COLOR16(GetRValue(bottom) << 8), COLOR16(GetGValue(bottom) << 8), COLOR16(GetBValue(bottom) << 8), 0 },
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

// Sky and floor split at the horizon, a faint grid on the sky and a floor grid for motion cues.
void DrawWorld(HDC dc, const Camera& cam)
{
    const int W = cam.W, H = cam.H;
    double horizon = H / 2.0 + cam.focal * std::tan(Rad(cam.pitch));
    const int hy = int(std::clamp(horizon, -1.0, double(H) + 1));
    VerticalGradient(dc, 0, 0, W, std::max(0, hy), RGB(9, 12, 20), RGB(30, 38, 56));
    VerticalGradient(dc, 0, std::max(0, hy), W, H, RGB(24, 27, 34), RGB(11, 12, 15));

    std::vector<POINT> pts;
    auto flush = [&]() {
        if (pts.size() >= 2) Polyline(dc, pts.data(), int(pts.size()));
        pts.clear();
    };
    auto add = [&](const Vec3& v) {
        double sx, sy, z;
        if (cam.Project(v, sx, sy, z) && std::fabs(sx) < 6.0 * W && std::fabs(sy) < 6.0 * H)
            pts.push_back({ LONG(std::lround(sx)), LONG(std::lround(sy)) });
        else flush();
    };

    // Sky: meridians and parallels above the horizon.
    HPEN sky = CreatePen(PS_SOLID, 1, RGB(38, 46, 64));
    HGDIOBJ old = SelectObject(dc, sky);
    for (int yaw = -180; yaw < 180; yaw += 15) {
        for (int p = 0; p <= 75; p += 3) add(Direction(yaw, p));
        flush();
    }
    for (int p = 15; p <= 75; p += 15) {
        for (int yaw = -180; yaw <= 180; yaw += 3) add(Direction(yaw, p));
        flush();
    }

    // Floor: a grid on the plane 1.6 units below the eye, brighter near the viewer.
    HPEN nearPen = CreatePen(PS_SOLID, 1, RGB(58, 64, 78));
    HPEN farPen = CreatePen(PS_SOLID, 1, RGB(36, 40, 50));
    const double floorY = -1.6, extent = 48, step = 2;
    for (int pass = 0; pass < 2; ++pass) {
        SelectObject(dc, pass == 0 ? farPen : nearPen);
        const double lo = pass == 0 ? 14 : 0, hi = pass == 0 ? extent : 14;
        for (double k = -extent; k <= extent + 1e-9; k += step) {
            for (int axis = 0; axis < 2; ++axis) {
                for (double t = -extent; t <= extent + 1e-9; t += 1.0) {
                    const double x = axis == 0 ? k : t, z = axis == 0 ? t : k;
                    const double d = std::hypot(x, z);
                    if (d < lo || d > hi) { flush(); continue; }
                    add({ x, floorY, z });
                }
                flush();
            }
        }
    }

    HPEN horizonPen = CreatePen(PS_SOLID, 2, RGB(70, 84, 110));
    SelectObject(dc, horizonPen);
    for (int yaw = -180; yaw <= 180; yaw += 2) add(Direction(yaw, 0));
    flush();

    SelectObject(dc, old);
    for (HPEN p : { sky, nearPen, farPen, horizonPen }) DeleteObject(p);
}

void DrawTarget(Gdiplus::Graphics& gr, const Camera& cam, const Vec3& v, double angRadiusDeg)
{
    double sx, sy, z;
    if (!cam.Project(v, sx, sy, z)) return;
    const float r = float(std::max(4.0, cam.focal * std::tan(Rad(angRadiusDeg)) / z));
    const float x = float(sx), y = float(sy);

    Gdiplus::SolidBrush glow(Gdiplus::Color(40, 255, 80, 80));
    gr.FillEllipse(&glow, x - r * 1.6f, y - r * 1.6f, r * 3.2f, r * 3.2f);

    Gdiplus::GraphicsPath path;
    path.AddEllipse(x - r, y - r, 2 * r, 2 * r);
    Gdiplus::PathGradientBrush shade(&path);
    shade.SetCenterPoint(Gdiplus::PointF(x - r * 0.35f, y - r * 0.4f));
    shade.SetCenterColor(Gdiplus::Color(255, 255, 196, 186));
    Gdiplus::Color edge(255, 168, 28, 36);
    INT count = 1;
    shade.SetSurroundColors(&edge, &count);
    gr.FillPath(&shade, &path);
    Gdiplus::Pen rim(Gdiplus::Color(140, 90, 10, 16), 1.2f);
    gr.DrawEllipse(&rim, x - r, y - r, 2 * r, 2 * r);
}

void DrawCrosshair(Gdiplus::Graphics& gr, int W, int H, Gdiplus::Color color, float scale)
{
    const float cx = W / 2.0f, cy = H / 2.0f;
    const float gap = 4 * scale, len = 7 * scale;
    auto lines = [&](Gdiplus::Pen& pen) {
        gr.DrawLine(&pen, cx - gap - len, cy, cx - gap, cy);
        gr.DrawLine(&pen, cx + gap, cy, cx + gap + len, cy);
        gr.DrawLine(&pen, cx, cy - gap - len, cx, cy - gap);
        gr.DrawLine(&pen, cx, cy + gap, cx, cy + gap + len);
    };
    Gdiplus::Pen outline(Gdiplus::Color(200, 0, 0, 0), 4.0f * scale);
    Gdiplus::Pen inner(color, 2.0f * scale);
    lines(outline);
    lines(inner);
    Gdiplus::SolidBrush dot(color);
    gr.FillRectangle(&dot, cx - scale, cy - scale, 2 * scale, 2 * scale);
}

void FillRoundRect(Gdiplus::Graphics& gr, float x, float y, float w, float h, float rad, Gdiplus::Color c)
{
    Gdiplus::GraphicsPath p;
    const float d = rad * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    Gdiplus::SolidBrush b(c);
    gr.FillPath(&b, &p);
}

// A simulated player with a known directional bias, for end-to-end tests.
struct BotPlayer {
    double gain[4] = { 1, 1, 1, 1 };   // R, L, U, D: how far its first flick goes relative to the target
    double tiltDeg = 0;
    std::mt19937 rng{ 4321 };
    std::normal_distribution<double> noise{ 0.0, 0.10 };
    std::normal_distribution<double> unit{ 0.0, 1.0 };
    std::uniform_real_distribution<double> uni{ 0.0, 1.0 };

    size_t planned = SIZE_MAX;
    double tStart = 0, tPrim = 0, tPause = 0, tCorr = 0, tClick = 0;
    double ex = 0, ey = 0, fx = 0, fy = 0;
    double sentX = 0, sentY = 0;
    bool clicked = false;

    double Gain(double angleDeg) const
    {
        const double co = std::cos(Rad(angleDeg)), si = std::sin(Rad(angleDeg));
        const double gx = co >= 0 ? gain[0] : gain[1], gy = si >= 0 ? gain[2] : gain[3];
        return std::sqrt(gx * gx * co * co + gy * gy * si * si);
    }

    static double MinJerk(double u)
    {
        u = std::clamp(u, 0.0, 1.0);
        return u * u * u * (10 - 15 * u + 6 * u * u);
    }

    void Plan(size_t index, double t0, double angleDeg, double dist, double radiusCounts)
    {
        planned = index;
        const double reach = Gain(angleDeg) * (1 + noise(rng));
        const double a = Rad(angleDeg + tiltDeg + 1.5 * unit(rng));
        ex = std::cos(a) * reach * dist;
        ey = -std::sin(a) * reach * dist;
        fx = std::cos(Rad(angleDeg)) * dist + 0.3 * radiusCounts * unit(rng);
        fy = -std::sin(Rad(angleDeg)) * dist + 0.3 * radiusCounts * unit(rng);
        tStart = t0 + 0.20 + 0.08 * uni(rng);
        tPrim = tStart + 0.16;
        tPause = tPrim + 0.035;
        tCorr = tPause + 0.13;
        tClick = tCorr + 0.03;
        sentX = sentY = 0;
        clicked = false;
    }

    void Position(double t, double& x, double& y) const
    {
        if (t < tStart) { x = y = 0; return; }
        if (t < tPrim) { const double m = MinJerk((t - tStart) / (tPrim - tStart)); x = ex * m; y = ey * m; return; }
        if (t < tPause) { x = ex; y = ey; return; }
        const double m = MinJerk((t - tPause) / (tCorr - tPause));
        x = ex + (fx - ex) * m;
        y = ey + (fy - ey) * m;
    }

    // Mouse reports at 1000 Hz for times in (from, to].
    void Emit(double from, double to, std::vector<InputEvent>& out)
    {
        for (double t = std::floor(from * 1000 + 1) / 1000; t <= to; t += 0.001) {
            double x, y;
            Position(t, x, y);
            const double dx = std::round(x) - sentX, dy = std::round(y) - sentY;
            const bool click = !clicked && t >= tClick;
            if (dx != 0 || dy != 0 || click) {
                out.push_back({ t, dx, dy, click });
                sentX += dx;
                sentY += dy;
                if (click) clicked = true;
            }
        }
    }
};

bool SavePng(HBITMAP bmp, const std::wstring& path)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            Gdiplus::Bitmap b(bmp, nullptr);
            return b.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
        }
    }
    return false;
}

struct BotRun {
    BotPlayer player;
    std::wstring framesDir;
    unsigned seed = 1;
};

bool RunCalibration3DImpl(const CalOptions& opt, std::vector<CalTrial>& trials, BotRun* bot)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    const float ui = float(H) / 1080.0f;   // HUD scale

    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiIn, nullptr);
    sfx::Configure(!bot && opt.sound, opt.volume);

    Camera cam;
    cam.W = W;
    cam.H = H;
    cam.focal = (W / 2.0) / std::tan(Rad(std::clamp(opt.fov, 30.0, 160.0) / 2));
    cam.Update();

    // Targets must be on screen when they appear: stay inside ~85% of the vertical half-FOV.
    const double maxDeg = std::min(35.0, 0.85 * Deg(std::atan((H / 2.0) / cam.focal)));
    const bool gameMode = opt.countsPerDegree > 0;
    const double cpd = gameMode ? opt.countsPerDegree : (opt.reachCm / 2.54 * opt.dpi) / maxDeg;
    const double targetDeg = 1.5;
    const int dirs = opt.directions;
    const int minBlocks = opt.adaptive ? std::min(opt.minRepsPerDirection, opt.repsPerDirection) : opt.repsPerDirection;
    const int maxBlocks = opt.repsPerDirection;

    std::mt19937 rng(bot ? 99u + bot->seed : std::random_device{}());
    std::uniform_real_distribution<double> distFrac(0.35, 1.0);
    std::uniform_real_distribution<double> delay(0.12, 0.30);

    // ---- Input and window (none for the bot: it renders off-screen in fast-forward)
    RawMouseReader reader;
    HWND hwnd = nullptr;
    WNDCLASSW wc = {};
    HDC wdc = nullptr;
    if (!bot) {
        if (!reader.Start()) {
            fwprintf(stderr, L"error: could not read raw mouse input.\n");
            Gdiplus::GdiplusShutdown(gdiToken);
            return false;
        }
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ODDPI_Calibration3D";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"ODDPI calibration", WS_POPUP | WS_VISIBLE,
                               0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            reader.Stop();
            fwprintf(stderr, L"error: could not open the calibration window.\n");
            Gdiplus::GdiplusShutdown(gdiToken);
            return false;
        }
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        RECT clip = { 0, 0, W, H };
        ClipCursor(&clip);
        wdc = GetDC(hwnd);
    }
    HDC screenDc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, W, H);
    ReleaseDC(nullptr, screenDc);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    auto font = [&](float size, int weight) {
        return CreateFontW(-int(size * ui), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    HFONT fSmall = font(15, FW_NORMAL), fBody = font(19, FW_NORMAL), fBold = font(19, FW_SEMIBOLD), fBig = font(34, FW_BOLD);
    HGDIOBJ oldFont = SelectObject(mem, fBody);
    SetBkMode(mem, TRANSPARENT);

    std::wstring subtitle;
    {
        wchar_t buf[256];
        const double cm = opt.dpi > 0 ? maxDeg * cpd / opt.dpi * 2.54 : 0;
        if (gameMode) swprintf_s(buf, L"As in %ls  \x00B7  FOV %.0f  \x00B7  flicks up to %.0f\x00B0 (%.1f cm)", opt.label.c_str(), opt.fov, maxDeg, cm);
        else swprintf_s(buf, L"Flicks up to %.0f\x00B0 = %.1f cm  \x00B7  set your game in ODDPI for a closer match", maxDeg, cm);
        subtitle = buf;
    }

    // ---- State
    enum class P { Intro, Waiting, Active, Done };
    P phase = P::Intro;
    std::deque<int> queue;
    int practiceLeft = opt.practice3d;
    int blocks = 0;               // full rounds of all directions started
    int measured = 0;             // measured flicks completed
    double precision = 1.0;       // largest correction uncertainty so far (1 = unknown)
    double clock = 0;             // seconds (virtual for the bot)
    const double start = Now();
    double lastClick = 0, lastMove = 0, waitNeeded = 0.2, t0 = 0, doneAt = 0;
    double flashUntil = -1;
    bool lastHit = false;
    int hits = 0, shots = 0;
    double hitTimeSum = 0;
    double spawnYaw = 0, spawnPitch = 0;
    Vec3 target{};
    CalTrial current;
    std::vector<InputEvent> events;
    struct Effect { Vec3 dir; double t; bool hit; };
    std::vector<Effect> effects;
    double curX = 0, curY = 0;
    std::vector<std::wstring> savedFrames;
    bool savedIntro = false, savedFlick = false, savedHit = false, savedLate = false;
    double botPitchMin = 0, botPitchMax = 0;

    auto turn = [&](double dx, double dy) {
        cam.yaw += dx / cpd;
        cam.pitch -= dy / cpd;
        cam.Update();
    };

    auto nextBlock = [&]() {
        std::vector<int> block(static_cast<size_t>(dirs));
        std::iota(block.begin(), block.end(), 0);
        std::shuffle(block.begin(), block.end(), rng);
        queue.assign(block.begin(), block.end());
        ++blocks;
    };

    // After each full round (from minBlocks on): stop when every correction is known to
    // within stopUncertainty.
    auto roundFinished = [&]() -> bool {
        if (blocks < minBlocks) return false;
        const CalResult r = FitCalibration(trials, dirs);
        if (r.ok) {
            precision = 0;
            for (double u : r.uncertainty) precision = std::max(precision, u);
            if (opt.adaptive && precision <= opt.stopUncertainty) return true;
        }
        return blocks >= maxBlocks;
    };

    auto spawn = [&](double now) {
        CalTrial t;
        double d = maxDeg * distFrac(rng);
        if (practiceLeft > 0) {
            t.practice = true;
            t.direction = (opt.practice3d - practiceLeft) * 5 % dirs;
            --practiceLeft;
        }
        else {
            if (queue.empty()) {
                if (blocks > 0 && roundFinished()) { phase = P::Done; doneAt = now; sfx::Play(sfx::Sound::Done); return; }
                nextBlock();
            }
            // Keep the view near the horizon like in a real fight: take the first queued direction
            // whose target stays within +-pitchLimit, shortening the flick if none does.
            const double pitchLimit = std::min(maxDeg, 20.0);
            size_t pick = 0;
            bool fits = false;
            for (size_t i = 0; i < queue.size() && !fits; ++i) {
                const double s = std::sin(Rad(DirectionAngle(queue[i], dirs)));
                if (std::fabs(cam.pitch + d * s) <= pitchLimit) { pick = i; fits = true; }
            }
            t.direction = queue[pick];
            queue.erase(queue.begin() + std::ptrdiff_t(pick));
            if (!fits) {
                const double s = std::sin(Rad(DirectionAngle(t.direction, dirs)));
                const double room = s >= 0 ? pitchLimit - cam.pitch : cam.pitch + pitchLimit;
                d = std::clamp(room / std::max(0.2, std::fabs(s)), 3.0, d);
            }
        }
        t.angleDeg = DirectionAngle(t.direction, dirs);
        t.distance = d * cpd;
        t.samples.push_back({ 0, 0, 0 });
        current = std::move(t);
        curX = curY = 0;
        spawnYaw = cam.yaw;
        spawnPitch = cam.pitch;
        target = Direction(spawnYaw + d * std::cos(Rad(current.angleDeg)), spawnPitch + d * std::sin(Rad(current.angleDeg)));
        t0 = now;
        phase = P::Active;
    };

    auto finishTrial = [&](double t, bool timedOut) {
        current.timedOut = timedOut;
        lastHit = !timedOut && cam.AngleTo(target) <= targetDeg;
        sfx::Play(lastHit ? sfx::Sound::Hit : sfx::Sound::Miss);
        ++shots;
        if (lastHit) { ++hits; hitTimeSum += t - t0; }
        effects.push_back({ target, t, lastHit });
        flashUntil = t + 0.18;
        if (!current.practice) ++measured;
        trials.push_back(std::move(current));
        current = CalTrial{};
        lastClick = lastMove = t;
        waitNeeded = delay(rng);
        phase = P::Waiting;
    };

    auto render = [&](double now) {
        DrawWorld(mem, cam);
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            if (phase == P::Active) DrawTarget(gr, cam, target, targetDeg);

            // Hit/miss rings expanding from where the target was.
            for (const auto& e : effects) {
                const double age = now - e.t;
                if (age < 0 || age > 0.3) continue;
                double sx, sy, z;
                if (!cam.Project(e.dir, sx, sy, z)) continue;
                const float base = float(cam.focal * std::tan(Rad(targetDeg)) / z);
                const float r = base * float(1.0 + 1.6 * age / 0.3);
                const BYTE a = BYTE(220 * (1 - age / 0.3));
                Gdiplus::Pen ring(e.hit ? Gdiplus::Color(a, 90, 230, 140) : Gdiplus::Color(a, 240, 90, 90), 3.0f * ui);
                gr.DrawEllipse(&ring, float(sx) - r, float(sy) - r, 2 * r, 2 * r);
            }

            const bool flashing = now < flashUntil;
            const Gdiplus::Color cross = flashing ? (lastHit ? Gdiplus::Color(255, 90, 230, 140) : Gdiplus::Color(255, 240, 90, 90))
                                                  : Gdiplus::Color(255, 0, 255, 200);
            DrawCrosshair(gr, W, H, cross, ui);

            // HUD panels
            const Gdiplus::Color panel(170, 10, 12, 18);
            FillRoundRect(gr, 20 * ui, 18 * ui, 640 * ui, 64 * ui, 12 * ui, panel);
            FillRoundRect(gr, W / 2.0f - 230 * ui, 18 * ui, 460 * ui, 64 * ui, 12 * ui, panel);
            FillRoundRect(gr, W - 380 * ui, 18 * ui, 360 * ui, 64 * ui, 12 * ui, panel);

            // Progress: rounds completed toward the minimum, then precision toward the goal.
            double progress;
            const int totalPractice = opt.practice3d;
            if (phase == P::Done) progress = 1;
            else if (precision >= 1.0) progress = 0.7 * std::min(1.0, double(measured) / double(minBlocks * dirs));
            else progress = 0.7 + 0.3 * std::clamp((0.08 - precision) / std::max(1e-6, 0.08 - opt.stopUncertainty), 0.0, 1.0);
            (void)totalPractice;
            const float bx = W / 2.0f - 200 * ui, by = 60 * ui, bw = 400 * ui, bh = 8 * ui;
            FillRoundRect(gr, bx, by, bw, bh, bh / 2, Gdiplus::Color(255, 45, 52, 66));
            if (progress > 0.01) FillRoundRect(gr, bx, by, float(bw * progress), bh, bh / 2, Gdiplus::Color(255, 79, 140, 255));

            if (phase == P::Intro || phase == P::Done) {
                FillRoundRect(gr, W / 2.0f - 420 * ui, H / 2.0f + 60 * ui, 840 * ui, 230 * ui, 18 * ui, Gdiplus::Color(215, 12, 14, 20));
            }
        }

        auto text = [&](const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
            SelectObject(mem, f);
            SetTextColor(mem, c);
            DrawTextW(mem, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
        };
        auto R = [&](float x, float y, float w, float h) { return RECT{ LONG(x), LONG(y), LONG(x + w), LONG(y + h) }; };
        const COLORREF white = RGB(236, 238, 243), dim = RGB(150, 158, 172);

        text(L"ODDPI calibration", R(38 * ui, 24 * ui, 600 * ui, 26 * ui), fBold, white, DT_LEFT | DT_SINGLELINE);
        text(subtitle, R(38 * ui, 50 * ui, 610 * ui, 24 * ui), fSmall, dim, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        std::wstring status;
        if (phase == P::Intro) status = L"Click to start";
        else if (practiceLeft > 0 || (phase == P::Active && current.practice)) status = L"Warm-up";
        else if (phase == P::Done) status = L"Done";
        else if (precision >= 1.0) status = L"Measuring your flicks\x2026";
        else status = L"Fine-tuning \x2014 ends automatically";
        text(status, R(W / 2.0f - 220 * ui, 26 * ui, 440 * ui, 28 * ui), fBold, white, DT_CENTER | DT_SINGLELINE);

        wchar_t stats[128];
        if (shots > 0) swprintf_s(stats, L"%d targets  \x00B7  %d%% hits  \x00B7  %.0f ms", shots, 100 * hits / shots,
                                  hits ? 1000 * hitTimeSum / hits : 0.0);
        else swprintf_s(stats, L"0 targets");
        text(stats, R(W - 370 * ui, 24 * ui, 340 * ui, 26 * ui), fBold, white, DT_RIGHT | DT_SINGLELINE);
        text(L"Esc to cancel", R(W - 370 * ui, 50 * ui, 340 * ui, 24 * ui), fSmall, dim, DT_RIGHT | DT_SINGLELINE);

        if (phase == P::Intro) {
            text(L"Flick to each red target and click",
                 R(W / 2.0f - 400 * ui, H / 2.0f + 82 * ui, 800 * ui, 44 * ui), fBig, white, DT_CENTER | DT_SINGLELINE);
            text(L"One quick motion per target. Your mouse turns the camera exactly like in your game.\n"
                 L"Aim naturally - don't slow down. It stops by itself once your result is precise\n"
                 L"(usually 50\x2013" L"100 targets, about a minute). The first few are a warm-up.",
                 R(W / 2.0f - 400 * ui, H / 2.0f + 140 * ui, 800 * ui, 90 * ui), fBody, dim, DT_CENTER | DT_WORDBREAK);
            text(L"Click to start", R(W / 2.0f - 400 * ui, H / 2.0f + 240 * ui, 800 * ui, 34 * ui), fBold, RGB(120, 170, 255),
                 DT_CENTER | DT_SINGLELINE);
        }
        if (phase == P::Done) {
            wchar_t buf[160];
            swprintf_s(buf, L"%d targets  \x00B7  %d%% hits", shots, shots ? 100 * hits / shots : 0);
            text(L"Done!", R(W / 2.0f - 400 * ui, H / 2.0f + 90 * ui, 800 * ui, 50 * ui), fBig, white, DT_CENTER | DT_SINGLELINE);
            text(buf, R(W / 2.0f - 400 * ui, H / 2.0f + 150 * ui, 800 * ui, 30 * ui), fBody, dim, DT_CENTER | DT_SINGLELINE);
            text(L"Working out your results\x2026", R(W / 2.0f - 400 * ui, H / 2.0f + 190 * ui, 800 * ui, 30 * ui), fBody, dim,
                 DT_CENTER | DT_SINGLELINE);
        }
    };

    auto saveFrame = [&](const wchar_t* name) {
        if (!bot || bot->framesDir.empty()) return;
        render(clock);
        const std::wstring path = bot->framesDir + L"\\" + name + L".png";
        if (SavePng(bmp, path)) savedFrames.push_back(path);
    };

    // ---- Main loop
    const double botStep = 1.0 / 240.0;
    while (!g_abort) {
        // Time and input
        events.clear();
        double now;
        if (bot) {
            const double prev = clock;
            clock += botStep;
            now = clock;
            if (phase == P::Intro && now >= 0.3) events.push_back({ now, 0, 0, true });
            if (phase == P::Active) {
                const size_t index = trials.size();
                if (bot->player.planned != index) {
                    bot->player.Plan(index, t0, current.angleDeg, current.distance, targetDeg * cpd);
                }
                bot->player.Emit(prev, now, events);
            }
        }
        else {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) g_abort = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            reader.Drain(events);
            now = Now() - start;
            for (auto& e : events) e.t -= start;
        }

        for (const auto& e : events) {
            switch (phase) {
            case P::Intro:
                if (e.leftDown) {
                    sfx::Play(sfx::Sound::Start);
                    phase = P::Waiting;
                    lastClick = lastMove = e.t;
                    waitNeeded = 0.4;
                }
                break;
            case P::Waiting:
                turn(e.dx, e.dy);
                if (e.dx != 0 || e.dy != 0) lastMove = e.t;
                break;
            case P::Active:
                if (e.t < t0) break;
                turn(e.dx, e.dy);
                curX += e.dx;
                curY += e.dy;
                current.samples.push_back({ e.t - t0, curX, curY });
                if (e.leftDown) finishTrial(e.t, false);
                break;
            default:
                break;
            }
        }

        // The next target appears once the mouse is still (a clean start for the flick).
        if (phase == P::Waiting && now - lastClick >= waitNeeded && now - lastMove >= 0.10) spawn(now);
        if (phase == P::Active && now - t0 > 3.0) finishTrial(now, true);

        if (bot) {
            if (!savedIntro && phase == P::Intro && now > 0.1) { savedIntro = true; saveFrame(L"1-intro"); }
            if (!savedFlick && phase == P::Active && trials.size() == 6 && now - t0 > 0.30) { savedFlick = true; saveFrame(L"2-mid-flick"); }
            if (!savedHit && phase == P::Waiting && trials.size() == 7 && now - lastClick > 0.05) { savedHit = true; saveFrame(L"3-after-click"); }
            if (!savedLate && phase == P::Active && measured == 40 && now - t0 > 0.1) { savedLate = true; saveFrame(L"4-later"); }
            if (phase == P::Done) { saveFrame(L"5-done"); break; }
            botPitchMin = std::min(botPitchMin, cam.pitch);
            botPitchMax = std::max(botPitchMax, cam.pitch);
            if (now > 900) break;   // safety
            continue;
        }

        if (phase == P::Done && now - doneAt > 0.8) break;
        render(now);
        if (wdc) BitBlt(wdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        if (FAILED(DwmFlush())) Sleep(1);
    }

    if (bot) {
        for (const auto& f : savedFrames) wprintf(L"frame: %ls\n", f.c_str());
        wprintf(L"view pitch stayed within %.1f .. %.1f deg, %.0f s of play\n", botPitchMin, botPitchMax, clock);
    }

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    for (HFONT f : { fSmall, fBody, fBold, fBig }) DeleteObject(f);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (!bot) {
        ClipCursor(nullptr);
        if (wdc) ReleaseDC(hwnd, wdc);
        if (hwnd) DestroyWindow(hwnd);
        if (wc.lpszClassName) UnregisterClassW(wc.lpszClassName, wc.hInstance);
        reader.Stop();
        if (reader.SawAbsolute()) {
            wprintf(L"note: some mouse input was absolute (remote desktop, tablet or VM) and was ignored.\n");
        }
    }
    Gdiplus::GdiplusShutdown(gdiToken);
    return !g_abort && phase == P::Done;
}

bool RunCalibration3D(const CalOptions& opt, std::vector<CalTrial>& trials)
{
    return RunCalibration3DImpl(opt, trials, nullptr);
}

}  // namespace

bool RunCalibration3DBot(const CalOptions& opt, const double gains[4], double tiltDeg,
                         std::vector<CalTrial>& trials, const std::wstring& framesDir, unsigned seed)
{
    g_abort = false;
    trials.clear();
    BotRun bot;
    bot.seed = seed;
    bot.player.rng.seed(seed * 7919u + 13u);
    for (int i = 0; i < 4; ++i) bot.player.gain[i] = gains[i];
    bot.player.tiltDeg = tiltDeg;
    bot.framesDir = framesDir;
    return RunCalibration3DImpl(opt, trials, &bot);
}

// ---------------------------------------------------------------------------------------------
// Sens finder
// ---------------------------------------------------------------------------------------------

namespace {

// A simulated player with a known best sensitivity: away from it (in log terms) every flick
// gets slower, and above it misses get more likely (targets are smaller in hand movement).
struct SensBot {
    double ideal = 1.0;
    std::mt19937 rng{ 1 };
    std::normal_distribution<double> n01{ 0.0, 1.0 };
    std::uniform_real_distribution<double> uni{ 0.0, 1.0 };

    size_t planned = SIZE_MAX;
    double tStart = 0, tPrim = 0, tPause = 0, tCorr = 0, tClick = 0;
    double ex = 0, ey = 0, fx = 0, fy = 0, sentX = 0, sentY = 0;
    bool clicked = false;

    void Plan(size_t index, double t0, double tx, double ty, double radiusCounts, double multiplier)
    {
        planned = index;
        const double off = std::log(multiplier / ideal);
        const double slow = 1.0 + 1.5 * off * off;                          // U-shape in time
        const double missP = std::clamp(0.06 + 0.9 * std::max(0.0, off) * std::max(0.0, off)
                                        + 0.3 * std::max(0.0, -off) * std::max(0.0, -off), 0.0, 0.9);
        const double reach = 1.0 + 0.10 * n01(rng) + 0.25 * off;           // overshoot when too fast
        ex = tx * reach;
        ey = ty * reach;
        const bool miss = uni(rng) < missP;
        const double a = uni(rng) * 2 * kPi;
        fx = tx + (miss ? 1.6 * radiusCounts * std::cos(a) : 0.3 * radiusCounts * n01(rng));
        fy = ty + (miss ? 1.6 * radiusCounts * std::sin(a) : 0.3 * radiusCounts * n01(rng));
        tStart = t0 + 0.20 + 0.06 * uni(rng);
        tPrim = tStart + 0.17 * slow * (1 + 0.1 * n01(rng) * 0.5);
        tPause = tPrim + 0.04;
        tCorr = tPause + 0.15 * slow;
        tClick = tCorr + 0.03;
        sentX = sentY = 0;
        clicked = false;
    }

    void Position(double t, double& x, double& y) const
    {
        if (t < tStart) { x = y = 0; return; }
        if (t < tPrim) { const double m = BotPlayer::MinJerk((t - tStart) / (tPrim - tStart)); x = ex * m; y = ey * m; return; }
        if (t < tPause) { x = ex; y = ey; return; }
        const double m = BotPlayer::MinJerk((t - tPause) / (tCorr - tPause));
        x = ex + (fx - ex) * m;
        y = ey + (fy - ey) * m;
    }

    void Emit(double from, double to, std::vector<InputEvent>& out)
    {
        for (double t = std::floor(from * 1000 + 1) / 1000; t <= to; t += 0.001) {
            double x, y;
            Position(t, x, y);
            const double dx = std::round(x) - sentX, dy = std::round(y) - sentY;
            const bool click = !clicked && t >= tClick;
            if (dx != 0 || dy != 0 || click) {
                out.push_back({ t, dx, dy, click });
                sentX += dx;
                sentY += dy;
                if (click) clicked = true;
            }
        }
    }
};

struct SensBotRun {
    SensBot player;
    std::wstring framesDir;
};

constexpr double kMissPenalty = 0.6;   // seconds added to a missed target

SensSideStats SideStats(std::vector<double> scores, int hits, double multiplier)
{
    SensSideStats s;
    s.multiplier = multiplier;
    s.targets = int(scores.size());
    s.hits = hits;
    if (scores.empty()) return s;
    // Drop the single worst target (a distraction, a mis-click) when there are enough.
    std::sort(scores.begin(), scores.end());
    if (scores.size() >= 6) scores.pop_back();
    const double n = double(scores.size());
    double sum = 0, sum2 = 0;
    for (double v : scores) { sum += v; sum2 += v * v; }
    s.score = sum / n;
    s.se = n > 1 ? std::sqrt(std::max(0.0, (sum2 - sum * sum / n) / (n - 1)) / n) : 0.5;
    return s;
}

bool RunSensFinderImpl(const SensFinderOptions& sopt, SensFinderResult& result, SensBotRun* bot)
{
    const CalOptions& opt = sopt.cal;
    result = SensFinderResult{};
    g_abort = false;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    const float ui = float(H) / 1080.0f;

    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiIn, nullptr);
    sfx::Configure(!bot && opt.sound, opt.volume);

    Camera cam;
    cam.W = W;
    cam.H = H;
    cam.focal = (W / 2.0) / std::tan(Rad(std::clamp(opt.fov, 30.0, 160.0) / 2));
    cam.Update();
    const double maxDeg = std::min(35.0, 0.85 * Deg(std::atan((H / 2.0) / cam.focal)));
    const double baseCpd = opt.countsPerDegree > 0 ? opt.countsPerDegree : (opt.reachCm / 2.54 * opt.dpi) / maxDeg;
    const double targetDeg = 1.5;
    const double pitchLimit = std::min(maxDeg, 20.0);

    std::mt19937 rng(bot ? 4242u : std::random_device{}());
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::uniform_real_distribution<double> distFrac(0.35, 1.0);
    std::uniform_real_distribution<double> delay(0.12, 0.30);

    // ---- Window / input (none for the bot)
    RawMouseReader reader;
    HWND hwnd = nullptr;
    WNDCLASSW wc = {};
    HDC wdc = nullptr;
    if (!bot) {
        if (!reader.Start()) {
            fwprintf(stderr, L"error: could not read raw mouse input.\n");
            Gdiplus::GdiplusShutdown(gdiToken);
            return false;
        }
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ODDPI_SensFinder";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"ODDPI sens finder", WS_POPUP | WS_VISIBLE,
                               0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            reader.Stop();
            Gdiplus::GdiplusShutdown(gdiToken);
            return false;
        }
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        RECT clip = { 0, 0, W, H };
        ClipCursor(&clip);
        wdc = GetDC(hwnd);
    }
    HDC screenDc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, W, H);
    ReleaseDC(nullptr, screenDc);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    auto font = [&](float size, int weight) {
        return CreateFontW(-int(size * ui), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    HFONT fSmall = font(15, FW_NORMAL), fBody = font(19, FW_NORMAL), fBold = font(19, FW_SEMIBOLD),
          fBig = font(34, FW_BOLD), fHuge = font(56, FW_BOLD);
    HGDIOBJ oldFont = SelectObject(mem, fBody);
    SetBkMode(mem, TRANSPARENT);

    auto gameSensText = [&](double multiplier) {
        wchar_t buf[160];
        const double cm = opt.dpi > 0 ? 360.0 / (1.0 / baseCpd * multiplier) / opt.dpi * 2.54 : 0;
        if (sopt.gameSens > 0 && !sopt.gameName.empty())
            swprintf_s(buf, L"%ls %.3g  (%.1f cm/360)", sopt.gameName.c_str(), sopt.gameSens * multiplier, cm);
        else
            swprintf_s(buf, L"x%.2f of your current sens  (%.1f cm/360)", multiplier, cm);
        return std::wstring(buf);
    };

    // ---- State
    enum class P { Intro, Switch, Waiting, Active, Done };
    P phase = P::Intro;
    const int warmup = 4;
    const int perRound = std::max(2 * sopt.blockSize, sopt.targetsPerRound / (2 * sopt.blockSize) * 2 * sopt.blockSize);
    double center = 1.0;                        // multiplier of the current sens
    double halfWidth = std::log(std::max(1.05, sopt.startFactor));
    int round = 0, inRound = 0, warmupLeft = warmup;
    bool aIsLow = true;                         // which of the blind labels A/B is the lower sens
    double multiplier = 1.0;                    // sens in use right now
    std::vector<double> lowScores, highScores;
    int lowHits = 0, highHits = 0;
    int totalTargets = 0, totalHits = 0;
    double clock = 0;
    const double start = Now();
    double lastClick = 0, lastMove = 0, waitNeeded = 0.4, t0 = 0, switchUntil = 0, doneAt = 0;
    double flashUntil = -1;
    bool lastHit = false;
    Vec3 target{};
    double targetCounts = 0, targetAngle = 0;
    double curX = 0, curY = 0;
    std::vector<InputEvent> events;
    struct Effect { Vec3 dir; double t; bool hit; };
    std::vector<Effect> effects;
    bool savedRound = false, savedSwitch = false;

    auto lowMult = [&]() { return center * std::exp(-halfWidth); };
    auto highMult = [&]() { return center * std::exp(halfWidth); };
    auto sideOf = [&](int k) { return (k / sopt.blockSize) % 2 == 0 ? aIsLow : !aIsLow; };   // true = low
    auto labelOf = [&](int k) { return (k / sopt.blockSize) % 2 == 0 ? L"A" : L"B"; };

    auto turn = [&](double dx, double dy) {
        const double cpd = baseCpd / multiplier;   // higher multiplier = faster turning
        cam.yaw += dx / cpd;
        cam.pitch -= dy / cpd;
        cam.Update();
    };

    auto beginRound = [&](double now) {
        aIsLow = uni(rng) < 0.5;
        inRound = 0;
        lowScores.clear();
        highScores.clear();
        lowHits = highHits = 0;
        multiplier = sideOf(0) ? lowMult() : highMult();
        phase = P::Switch;
        switchUntil = now + 0.9;
        sfx::Play(sfx::Sound::Switch);
    };

    auto spawn = [&](double now) {
        // Random direction, kept near the horizon; same angular distances at every sens.
        double angle = uni(rng) * 360.0;
        const double d = maxDeg * distFrac(rng);
        if (std::fabs(cam.pitch + d * std::sin(Rad(angle))) > pitchLimit) angle = -angle;   // flip vertically
        targetAngle = angle;
        targetCounts = d * (baseCpd / multiplier);
        target = Direction(cam.yaw + d * std::cos(Rad(angle)), cam.pitch + d * std::sin(Rad(angle)));
        curX = curY = 0;
        t0 = now;
        phase = P::Active;
    };

    auto finishRound = [&](double now) {
        SensRound r;
        r.low = SideStats(lowScores, lowHits, lowMult());
        r.high = SideStats(highScores, highHits, highMult());
        r.centerBefore = center;
        // Move toward the better side by up to half the range, scaled by how clear the
        // difference is (z of 2 = full step), then narrow. Noise can't drag it far.
        const double diff = r.low.score - r.high.score;   // > 0: high was faster
        const double se = std::sqrt(r.low.se * r.low.se + r.high.se * r.high.se) + 1e-6;
        const double strength = std::clamp(diff / (2 * se), -1.0, 1.0);
        center *= std::exp(strength * halfWidth / 2);
        halfWidth *= sopt.shrink;
        r.centerAfter = center;
        result.rounds.push_back(r);
        ++round;
        if (round >= sopt.rounds) {
            result.multiplier = center;
            result.finished = true;
            multiplier = center;
            phase = P::Done;
            doneAt = now;
            sfx::Play(sfx::Sound::Done);
        }
        else beginRound(now);
    };

    auto finishTarget = [&](double t, bool timedOut) {
        const bool hit = !timedOut && cam.AngleTo(target) <= targetDeg;
        sfx::Play(hit ? sfx::Sound::Hit : sfx::Sound::Miss);
        const double secs = timedOut ? 3.0 : t - t0;
        lastHit = hit;
        effects.push_back({ target, t, hit });
        flashUntil = t + 0.18;
        ++totalTargets;
        totalHits += hit ? 1 : 0;
        lastClick = lastMove = t;
        waitNeeded = delay(rng);
        if (warmupLeft > 0) {
            if (--warmupLeft == 0) beginRound(t);
            else phase = P::Waiting;
            return;
        }
        const double score = secs + (hit ? 0 : kMissPenalty);
        if (sideOf(inRound)) { lowScores.push_back(score); lowHits += hit; }
        else { highScores.push_back(score); highHits += hit; }
        ++inRound;
        if (inRound >= perRound) { finishRound(t); return; }
        if (inRound % sopt.blockSize == 0) {           // switch A <-> B
            multiplier = sideOf(inRound) ? lowMult() : highMult();
            phase = P::Switch;
            switchUntil = t + 0.8;
            sfx::Play(sfx::Sound::Switch);
            return;
        }
        phase = P::Waiting;
    };

    auto render = [&](double now) {
        DrawWorld(mem, cam);
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            if (phase == P::Active) DrawTarget(gr, cam, target, targetDeg);
            for (const auto& e : effects) {
                const double age = now - e.t;
                if (age < 0 || age > 0.3) continue;
                double sx, sy, z;
                if (!cam.Project(e.dir, sx, sy, z)) continue;
                const float base = float(cam.focal * std::tan(Rad(targetDeg)) / z);
                const float r = base * float(1.0 + 1.6 * age / 0.3);
                const BYTE a = BYTE(220 * (1 - age / 0.3));
                Gdiplus::Pen ring(e.hit ? Gdiplus::Color(a, 90, 230, 140) : Gdiplus::Color(a, 240, 90, 90), 3.0f * ui);
                gr.DrawEllipse(&ring, float(sx) - r, float(sy) - r, 2 * r, 2 * r);
            }
            const bool flashing = now < flashUntil;
            DrawCrosshair(gr, W, H, flashing ? (lastHit ? Gdiplus::Color(255, 90, 230, 140) : Gdiplus::Color(255, 240, 90, 90))
                                             : Gdiplus::Color(255, 0, 255, 200), ui);

            const Gdiplus::Color panel(170, 10, 12, 18);
            FillRoundRect(gr, 20 * ui, 18 * ui, 640 * ui, 64 * ui, 12 * ui, panel);
            FillRoundRect(gr, W / 2.0f - 230 * ui, 18 * ui, 460 * ui, 64 * ui, 12 * ui, panel);
            FillRoundRect(gr, W - 380 * ui, 18 * ui, 360 * ui, 64 * ui, 12 * ui, panel);

            const int total = warmup + sopt.rounds * perRound;
            const double progress = phase == P::Done ? 1.0 : double(totalTargets) / double(total);
            const float bx = W / 2.0f - 200 * ui, by = 60 * ui, bw = 400 * ui, bh = 8 * ui;
            FillRoundRect(gr, bx, by, bw, bh, bh / 2, Gdiplus::Color(255, 45, 52, 66));
            if (progress > 0.01) FillRoundRect(gr, bx, by, float(bw * progress), bh, bh / 2, Gdiplus::Color(255, 79, 140, 255));

            if (phase == P::Intro || phase == P::Done)
                FillRoundRect(gr, W / 2.0f - 440 * ui, H / 2.0f + 60 * ui, 880 * ui, 250 * ui, 18 * ui, Gdiplus::Color(215, 12, 14, 20));
            if (phase == P::Switch)
                FillRoundRect(gr, W / 2.0f - 220 * ui, H / 2.0f + 70 * ui, 440 * ui, 136 * ui, 18 * ui, Gdiplus::Color(200, 12, 14, 20));
        }

        auto text = [&](const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
            SelectObject(mem, f);
            SetTextColor(mem, c);
            DrawTextW(mem, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
        };
        auto R = [&](float x, float y, float w, float h) { return RECT{ LONG(x), LONG(y), LONG(x + w), LONG(y + h) }; };
        const COLORREF white = RGB(236, 238, 243), dim = RGB(150, 158, 172), accent = RGB(120, 170, 255);

        text(L"ODDPI sens finder", R(38 * ui, 24 * ui, 600 * ui, 26 * ui), fBold, white, DT_LEFT | DT_SINGLELINE);
        {
            wchar_t buf[200];
            swprintf_s(buf, L"Starting from %ls  \x00B7  FOV %.0f", gameSensText(1.0).c_str(), opt.fov);
            text(buf, R(38 * ui, 50 * ui, 610 * ui, 24 * ui), fSmall, dim, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        std::wstring status;
        if (phase == P::Intro) status = L"Click to start";
        else if (warmupLeft > 0) status = L"Warm-up";
        else if (phase == P::Done) status = L"Done";
        else {
            wchar_t buf[96];
            swprintf_s(buf, L"Round %d of %d  \x00B7  Sensitivity %ls", round + 1, sopt.rounds, labelOf(inRound));
            status = buf;
        }
        text(status, R(W / 2.0f - 220 * ui, 26 * ui, 440 * ui, 28 * ui), fBold, white, DT_CENTER | DT_SINGLELINE);

        wchar_t stats[96];
        swprintf_s(stats, L"%d targets  \x00B7  %d%% hits", totalTargets, totalTargets ? 100 * totalHits / totalTargets : 0);
        text(stats, R(W - 370 * ui, 24 * ui, 340 * ui, 26 * ui), fBold, white, DT_RIGHT | DT_SINGLELINE);
        text(L"Esc to cancel", R(W - 370 * ui, 50 * ui, 340 * ui, 24 * ui), fSmall, dim, DT_RIGHT | DT_SINGLELINE);

        if (phase == P::Switch && warmupLeft == 0) {
            wchar_t buf[48];
            swprintf_s(buf, L"Sensitivity %ls", labelOf(inRound));
            text(buf, R(W / 2.0f - 220 * ui, H / 2.0f + 80 * ui, 440 * ui, 84 * ui), fHuge, white, DT_CENTER | DT_SINGLELINE);
            text(inRound == 0 ? L"New round \x2014 the range got narrower" : L"Switching\x2026",
                 R(W / 2.0f - 220 * ui, H / 2.0f + 166 * ui, 440 * ui, 30 * ui), fBody, dim, DT_CENTER | DT_SINGLELINE);
        }
        if (phase == P::Intro) {
            text(L"Find your sensitivity", R(W / 2.0f - 420 * ui, H / 2.0f + 80 * ui, 840 * ui, 44 * ui), fBig, white, DT_CENTER | DT_SINGLELINE);
            text(L"Each round you play two sensitivities, \"A\" and \"B\" (you won't know which is which).\n"
                 L"Flick to each red target and click, as fast and accurately as you can.\n"
                 L"It moves toward whichever you aimed better with and narrows down every round.",
                 R(W / 2.0f - 420 * ui, H / 2.0f + 138 * ui, 840 * ui, 90 * ui), fBody, dim, DT_CENTER | DT_WORDBREAK);
            wchar_t buf[96];
            swprintf_s(buf, L"%d rounds \x00B7 about %d targets \x00B7 Click to start", sopt.rounds, warmup + sopt.rounds * perRound);
            text(buf, R(W / 2.0f - 420 * ui, H / 2.0f + 250 * ui, 840 * ui, 34 * ui), fBold, accent, DT_CENTER | DT_SINGLELINE);
        }
        if (phase == P::Done) {
            text(L"Your sensitivity", R(W / 2.0f - 420 * ui, H / 2.0f + 78 * ui, 840 * ui, 30 * ui), fBody, dim, DT_CENTER | DT_SINGLELINE);
            text(gameSensText(center), R(W / 2.0f - 420 * ui, H / 2.0f + 112 * ui, 840 * ui, 60 * ui), fBig, white, DT_CENTER | DT_SINGLELINE);
            wchar_t buf[160];
            swprintf_s(buf, L"%d targets \x00B7 %d%% hits \x00B7 tested %.3g to %.3g",
                       totalTargets, totalTargets ? 100 * totalHits / totalTargets : 0,
                       sopt.gameSens > 0 ? sopt.gameSens / sopt.startFactor : 1 / sopt.startFactor,
                       sopt.gameSens > 0 ? sopt.gameSens * sopt.startFactor : sopt.startFactor);
            text(buf, R(W / 2.0f - 420 * ui, H / 2.0f + 182 * ui, 840 * ui, 30 * ui), fBody, dim, DT_CENTER | DT_SINGLELINE);
            text(L"Set it in your game, then play with it for a while.", R(W / 2.0f - 420 * ui, H / 2.0f + 220 * ui, 840 * ui, 30 * ui),
                 fBody, dim, DT_CENTER | DT_SINGLELINE);
        }
    };

    auto saveFrame = [&](const wchar_t* name) {
        if (!bot || bot->framesDir.empty()) return;
        render(clock);
        SavePng(bmp, bot->framesDir + L"\\" + name + L".png");
    };

    const double botStep = 1.0 / 240.0;
    while (!g_abort) {
        events.clear();
        double now;
        if (bot) {
            const double prev = clock;
            clock += botStep;
            now = clock;
            if (phase == P::Intro && now >= 0.3) events.push_back({ now, 0, 0, true });
            if (phase == P::Active) {
                const size_t index = size_t(totalTargets);
                if (bot->player.planned != index) {
                    bot->player.Plan(index, t0, std::cos(Rad(targetAngle)) * targetCounts, -std::sin(Rad(targetAngle)) * targetCounts,
                                     targetDeg * baseCpd / multiplier, multiplier);
                }
                bot->player.Emit(prev, now, events);
            }
        }
        else {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) g_abort = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            reader.Drain(events);
            now = Now() - start;
            for (auto& e : events) e.t -= start;
        }

        for (const auto& e : events) {
            switch (phase) {
            case P::Intro:
                if (e.leftDown) {
                    sfx::Play(sfx::Sound::Start);
                    phase = P::Waiting;
                    lastClick = lastMove = e.t;
                    waitNeeded = 0.4;
                }
                break;
            case P::Switch:
            case P::Waiting:
                turn(e.dx, e.dy);
                if (e.dx != 0 || e.dy != 0) lastMove = e.t;
                break;
            case P::Active:
                if (e.t < t0) break;
                turn(e.dx, e.dy);
                curX += e.dx;
                curY += e.dy;
                if (e.leftDown) finishTarget(e.t, false);
                break;
            default:
                break;
            }
        }

        if (phase == P::Switch && now >= switchUntil) { phase = P::Waiting; lastClick = now; waitNeeded = 0.15; }
        if (phase == P::Waiting && now - lastClick >= waitNeeded && now - lastMove >= 0.10) spawn(now);
        if (phase == P::Active && now - t0 > 3.0) finishTarget(now, true);

        if (bot) {
            if (!savedSwitch && phase == P::Switch && round == 1 && now > switchUntil - 0.5) { savedSwitch = true; saveFrame(L"sf-1-switch"); }
            if (!savedRound && phase == P::Active && round == 2 && inRound == 5 && now - t0 > 0.3) { savedRound = true; saveFrame(L"sf-2-round"); }
            if (phase == P::Done) { saveFrame(L"sf-3-done"); break; }
            if (now > 1200) break;
            continue;
        }

        if (phase == P::Done) {
            // Keep the result on screen until a click (or 8 s).
            bool clicked = false;
            for (const auto& e : events) clicked = clicked || (e.leftDown && e.t > doneAt + 0.5);
            if (clicked || now - doneAt > 8.0) break;
        }
        render(now);
        if (wdc) BitBlt(wdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        if (FAILED(DwmFlush())) Sleep(1);
    }

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    for (HFONT f : { fSmall, fBody, fBold, fBig, fHuge }) DeleteObject(f);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (!bot) {
        ClipCursor(nullptr);
        if (wdc) ReleaseDC(hwnd, wdc);
        if (hwnd) DestroyWindow(hwnd);
        if (wc.lpszClassName) UnregisterClassW(wc.lpszClassName, wc.hInstance);
        reader.Stop();
    }
    Gdiplus::GdiplusShutdown(gdiToken);
    return result.finished && !g_abort;
}

}  // namespace

bool RunSensFinder(const SensFinderOptions& opt, SensFinderResult& result)
{
    return RunSensFinderImpl(opt, result, nullptr);
}

bool RunSensFinderBot(const SensFinderOptions& opt, double idealMultiplier, unsigned seed,
                      SensFinderResult& result, const std::wstring& framesDir)
{
    SensBotRun bot;
    bot.player.ideal = idealMultiplier;
    bot.player.rng.seed(seed * 2654435761u + 7u);
    bot.framesDir = framesDir;
    return RunSensFinderImpl(opt, result, &bot);
}

// ---------------------------------------------------------------------------------------------
// Gridshot with an improvement board
// ---------------------------------------------------------------------------------------------

int GridshotScore(int hits, int misses)
{
    const int shots = hits + misses;
    if (shots <= 0) return 0;
    return int(std::lround(hits * 100.0 * double(hits) / double(shots)));
}

std::wstring DefaultHistoryPath()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\ODDPI" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\gridshot.csv";
}

namespace {

    std::vector<std::wstring> SplitCsv(const std::wstring& line)
    {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (;;) {
            const size_t comma = line.find(L',', start);
            out.push_back(line.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
        return out;
    }

    std::wstring NoCommas(std::wstring s)
    {
        for (auto& c : s) if (c == L',' || c == L'\n' || c == L'\r') c = L' ';
        return s;
    }

    std::wstring Thousands(int v)
    {
        std::wstring s = std::to_wstring(std::abs(v)), out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i > 0 && (s.size() - i) % 3 == 0) out += L',';
            out += s[i];
        }
        return (v < 0 ? L"-" : L"") + out;
    }
}

std::vector<RunRecord> LoadHistory(const std::wstring& path)
{
    std::vector<RunRecord> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return out;
    wchar_t buf[1024];
    while (fgetws(buf, 1024, f)) {
        std::wstring line = buf;
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
        if (line.empty() || line.rfind(L"when,", 0) == 0) continue;
        const auto c = SplitCsv(line);
        if (c.size() < 11) continue;
        RunRecord r;
        r.when = c[0];
        r.score = _wtoi(c[1].c_str());
        r.hits = _wtoi(c[2].c_str());
        r.misses = _wtoi(c[3].c_str());
        r.seconds = _wtof(c[4].c_str());
        r.avgKillMs = _wtof(c[5].c_str());
        r.game = c[6];
        r.gameSens = _wtof(c[7].c_str());
        r.cm360 = _wtof(c[8].c_str());
        r.oddpiOn = c[9] == L"1";
        r.settings = c[10];
        out.push_back(r);
    }
    fclose(f);
    return out;
}

bool AddToHistory(const std::wstring& path, const RunRecord& r)
{
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return false;
    if (!exists) fwprintf(f, L"when,score,hits,misses,seconds,avgKillMs,game,gameSens,cm360,oddpi,settings\n");
    fwprintf(f, L"%ls,%d,%d,%d,%.1f,%.0f,%ls,%g,%.2f,%d,%ls\n", NoCommas(r.when).c_str(), r.score, r.hits, r.misses,
             r.seconds, r.avgKillMs, NoCommas(r.game).c_str(), r.gameSens, r.cm360, r.oddpiOn ? 1 : 0,
             NoCommas(r.settings).c_str());
    const bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

ProgressStats ComputeProgress(const std::vector<RunRecord>& h)
{
    ProgressStats s;
    s.runs = int(h.size());
    double onSum = 0, offSum = 0;
    for (const auto& r : h) {
        s.best = std::max(s.best, r.score);
        if (r.oddpiOn) { onSum += r.score; ++s.onRuns; }
        else { offSum += r.score; ++s.offRuns; }
    }
    if (s.onRuns) s.onAvg = onSum / s.onRuns;
    if (s.offRuns) s.offAvg = offSum / s.offRuns;
    if (s.runs >= 4) {
        s.window = std::min(5, s.runs / 2);
        double a = 0, b = 0;
        for (int i = 0; i < s.window; ++i) {
            a += h[size_t(i)].score;
            b += h[size_t(s.runs - 1 - i)].score;
        }
        s.firstAvg = a / s.window;
        s.recentAvg = b / s.window;
        s.improvement = s.firstAvg > 0 ? s.recentAvg / s.firstAvg - 1 : 0;
    }
    return s;
}

namespace {

struct GridBotRun {
    SensBot player;
    int runs = 1;
    double startIdeal = 1;
    std::wstring framesDir;
};

struct ChartText {
    std::wstring text;
    RECT r;
    COLORREF color;
    UINT fmt;
    int font;   // 0 small, 1 body, 2 bold
};

// Score over time: a dot per run (blue = ODDPI on, grey = off), a 5-run rolling average and the
// personal best. Shapes go to `gr`, labels to `labels` (drawn later with GDI ClearType text).
void DrawProgressChart(Gdiplus::Graphics& gr, std::vector<ChartText>& labels, const std::vector<RunRecord>& all,
                       float x, float y, float w, float chartH, float ui, bool highlightLast)
{
    const COLORREF dim = RGB(150, 158, 172);
    if (all.empty()) {
        labels.push_back({ L"No runs yet \x2014 your progress will show up here.", RECT{ LONG(x), LONG(y), LONG(x + w), LONG(y + chartH) },
                           dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 1 });
        return;
    }
    const size_t maxShown = 60;
    const size_t first = all.size() > maxShown ? all.size() - maxShown : 0;
    std::vector<RunRecord> h(all.begin() + std::ptrdiff_t(first), all.end());

    int lo = h[0].score, hi = h[0].score, best = 0;
    for (const auto& r : all) best = std::max(best, r.score);
    for (const auto& r : h) { lo = std::min(lo, r.score); hi = std::max(hi, r.score); }
    hi = std::max(hi, best);
    double span = std::max(1000.0, double(hi - lo));
    const double yLo = std::max(0.0, lo - span * 0.12), yHi = hi + span * 0.12;

    const float left = x + 64 * ui, right = x + w - 12 * ui, top = y + 8 * ui, bottom = y + chartH - 26 * ui;
    auto px = [&](size_t i) { return h.size() == 1 ? (left + right) / 2 : left + float(i) * (right - left) / float(h.size() - 1); };
    auto py = [&](double v) { return bottom - float((v - yLo) / (yHi - yLo)) * (bottom - top); };

    // Grid lines + y labels
    Gdiplus::Pen grid(Gdiplus::Color(255, 40, 46, 58), 1.0f);
    for (int k = 0; k <= 3; ++k) {
        const double v = yLo + (yHi - yLo) * k / 3.0;
        const float yy = py(v);
        gr.DrawLine(&grid, left, yy, right, yy);
        labels.push_back({ Thousands(int(std::lround(v / 10) * 10)), RECT{ LONG(x), LONG(yy - 9 * ui), LONG(left - 8 * ui), LONG(yy + 9 * ui) },
                           dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 0 });
    }

    // Personal best
    {
        Gdiplus::Pen pb(Gdiplus::Color(200, 251, 191, 36), 1.5f * ui);
        pb.SetDashStyle(Gdiplus::DashStyleDash);
        const float yy = py(best);
        gr.DrawLine(&pb, left, yy, right, yy);
        labels.push_back({ L"Best " + Thousands(best), RECT{ LONG(right - 200 * ui), LONG(yy - 22 * ui), LONG(right), LONG(yy - 2 * ui) },
                           RGB(251, 191, 36), DT_RIGHT | DT_SINGLELINE, 0 });
    }

    // Rolling average (5 runs)
    if (h.size() >= 2) {
        std::vector<Gdiplus::PointF> pts;
        for (size_t i = 0; i < h.size(); ++i) {
            const size_t a = i >= 4 ? i - 4 : 0;
            double sum = 0;
            for (size_t j = a; j <= i; ++j) sum += h[j].score;
            pts.push_back(Gdiplus::PointF(px(i), py(sum / double(i - a + 1))));
        }
        Gdiplus::Pen avg(Gdiplus::Color(255, 52, 211, 153), 2.5f * ui);
        avg.SetLineJoin(Gdiplus::LineJoinRound);
        gr.DrawLines(&avg, pts.data(), INT(pts.size()));
    }

    // Runs
    for (size_t i = 0; i < h.size(); ++i) {
        const bool last = highlightLast && i + 1 == h.size();
        const float r = (last ? 6.5f : 4.0f) * ui;
        Gdiplus::SolidBrush dot(h[i].oddpiOn ? Gdiplus::Color(255, 79, 140, 255) : Gdiplus::Color(255, 140, 146, 160));
        gr.FillEllipse(&dot, px(i) - r, py(h[i].score) - r, 2 * r, 2 * r);
        if (last) {
            Gdiplus::Pen ring(Gdiplus::Color(255, 255, 255, 255), 2.0f * ui);
            gr.DrawEllipse(&ring, px(i) - r, py(h[i].score) - r, 2 * r, 2 * r);
        }
    }

    // X labels: first and last date
    auto day = [](const std::wstring& when) { return when.substr(0, std::min<size_t>(10, when.size())); };
    labels.push_back({ day(h.front().when), RECT{ LONG(left - 40 * ui), LONG(bottom + 6 * ui), LONG(left + 120 * ui), LONG(bottom + 24 * ui) },
                       dim, DT_LEFT | DT_SINGLELINE, 0 });
    if (h.size() > 1)
        labels.push_back({ day(h.back().when), RECT{ LONG(right - 160 * ui), LONG(bottom + 6 * ui), LONG(right), LONG(bottom + 24 * ui) },
                           dim, DT_RIGHT | DT_SINGLELINE, 0 });
}

bool RunGridshotImpl(const GridshotOptions& gopt, GridshotResult& result, GridBotRun* bot)
{
    const CalOptions& opt = gopt.cal;
    result = GridshotResult{};
    g_abort = false;
    const std::wstring historyPath = gopt.historyPath.empty() ? DefaultHistoryPath() : gopt.historyPath;
    std::vector<RunRecord> history = LoadHistory(historyPath);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    const float ui = float(H) / 1080.0f;

    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiIn, nullptr);
    sfx::Configure(!bot && opt.sound, opt.volume);

    Camera cam;
    cam.W = W;
    cam.H = H;
    cam.focal = (W / 2.0) / std::tan(Rad(std::clamp(opt.fov, 30.0, 160.0) / 2));
    cam.Update();
    const double maxDeg = std::min(35.0, 0.85 * Deg(std::atan((H / 2.0) / cam.focal)));
    const double cpd = opt.countsPerDegree > 0 ? opt.countsPerDegree : (opt.reachCm / 2.54 * opt.dpi) / maxDeg;
    const double targetDeg = 2.0;
    const double cellYaw[5] = { -16, -8, 0, 8, 16 }, cellPitch[3] = { 8, 0, -8 };

    std::mt19937 rng(bot ? 77u : std::random_device{}());

    RawMouseReader reader;
    HWND hwnd = nullptr;
    WNDCLASSW wc = {};
    HDC wdc = nullptr;
    if (!bot) {
        if (!reader.Start()) {
            fwprintf(stderr, L"error: could not read raw mouse input.\n");
            Gdiplus::GdiplusShutdown(gdiToken);
            return false;
        }
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ODDPI_Gridshot";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"ODDPI Gridshot", WS_POPUP | WS_VISIBLE,
                               0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            reader.Stop();
            Gdiplus::GdiplusShutdown(gdiToken);
            return false;
        }
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        RECT clip = { 0, 0, W, H };
        ClipCursor(&clip);
        wdc = GetDC(hwnd);
    }
    HDC screenDc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, W, H);
    ReleaseDC(nullptr, screenDc);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    auto font = [&](float size, int weight) {
        return CreateFontW(-int(size * ui), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    HFONT fSmall = font(15, FW_NORMAL), fBody = font(19, FW_NORMAL), fBold = font(19, FW_SEMIBOLD),
          fBig = font(34, FW_BOLD), fHuge = font(72, FW_BOLD), fTimer = font(40, FW_BOLD);
    HGDIOBJ oldFont = SelectObject(mem, fBody);
    SetBkMode(mem, TRANSPARENT);

    std::wstring subtitle;
    {
        wchar_t buf[200];
        const double cm = opt.dpi > 0 ? 360.0 * cpd / opt.dpi * 2.54 : 0;
        if (gopt.gameSens > 0 && !gopt.gameName.empty())
            swprintf_s(buf, L"%ls %g (%.1f cm/360)  \x00B7  ODDPI %ls", gopt.gameName.c_str(), gopt.gameSens, cm, gopt.oddpiOn ? L"on" : L"off");
        else swprintf_s(buf, L"%.1f cm/360  \x00B7  ODDPI %ls", cm, gopt.oddpiOn ? L"on" : L"off");
        subtitle = buf;
    }

    // ---- State
    enum class P { Board, Countdown, Play, Results };
    P phase = P::Board;
    struct Target { int cell; double yaw, pitch; Vec3 dir; };
    std::vector<Target> targets;
    int hits = 0, misses = 0;
    double clock = 0;
    const double start = Now();
    double phaseStart = 0, lastTick = 0;
    double flashUntil = -1;
    bool lastHit = false;
    std::vector<InputEvent> events;
    struct Effect { Vec3 dir; double t; bool hit; };
    std::vector<Effect> effects;
    int botPlan = 0;
    bool botArmed = false;
    bool savedBoard = false, savedPlay = false;
    bool newBest = false;
    bool oddpiOn = gopt.oddpiOn;

    auto spawnTarget = [&](int avoidCell) {
        std::vector<int> free;
        for (int c = 0; c < 15; ++c) {
            bool used = c == avoidCell;
            for (const auto& t : targets) used = used || t.cell == c;
            if (!used) free.push_back(c);
        }
        const int cell = free[std::uniform_int_distribution<size_t>(0, free.size() - 1)(rng)];
        Target t;
        t.cell = cell;
        t.yaw = cellYaw[cell % 5];
        t.pitch = cellPitch[cell / 5];
        t.dir = Direction(t.yaw, t.pitch);
        targets.push_back(t);
    };

    auto startCountdown = [&](double now) {
        phase = P::Countdown;
        phaseStart = now;
        lastTick = -1;
        cam.yaw = cam.pitch = 0;
        cam.Update();
        targets.clear();
        hits = misses = 0;
        effects.clear();
        if (bot) {
            // The simulated player gets better run after run (for a meaningful test chart).
            bot->player.ideal = 1.0 + (bot->startIdeal - 1.0) * std::pow(0.6, result.runs);
            oddpiOn = result.runs % 2 == 0;
        }
    };

    auto finishRun = [&](double now) {
        RunRecord r;
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t when[32];
        swprintf_s(when, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        r.when = when;
        r.hits = hits;
        r.misses = misses;
        r.seconds = gopt.seconds;
        r.score = GridshotScore(hits, misses);
        r.avgKillMs = hits ? gopt.seconds * 1000.0 / hits : 0;
        r.game = gopt.gameName;
        r.gameSens = gopt.gameSens;
        r.cm360 = opt.dpi > 0 ? 360.0 * cpd / opt.dpi * 2.54 : 0;
        r.oddpiOn = oddpiOn;
        r.settings = gopt.settings;
        const int previousBest = history.empty() ? -1 : ComputeProgress(history).best;
        newBest = r.score > previousBest;
        history.push_back(r);
        AddToHistory(historyPath, r);
        ++result.runs;
        result.last = r;
        result.personalBest = newBest;
        result.stats = ComputeProgress(history);
        phase = P::Results;
        phaseStart = now;
        targets.clear();
        sfx::Play(sfx::Sound::Done);
    };

    auto render = [&](double now) {
        DrawWorld(mem, cam);
        std::vector<ChartText> labels;
        const bool board = phase == P::Board || phase == P::Results;
        const float px0 = W / 2.0f - 520 * ui, py0 = H / 2.0f - 330 * ui, pw = 1040 * ui, ph = 660 * ui;
        const float chartX = px0 + 50 * ui, chartY = py0 + 250 * ui, chartW = pw - 100 * ui, chartH = 300 * ui;
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            for (const auto& t : targets) DrawTarget(gr, cam, t.dir, targetDeg);
            for (const auto& e : effects) {
                const double age = now - e.t;
                if (age < 0 || age > 0.3) continue;
                double sx, sy, z;
                if (!cam.Project(e.dir, sx, sy, z)) continue;
                const float base = float(cam.focal * std::tan(Rad(targetDeg)) / z);
                const float r = base * float(1.0 + 1.6 * age / 0.3);
                const BYTE a = BYTE(220 * (1 - age / 0.3));
                Gdiplus::Pen ring(e.hit ? Gdiplus::Color(a, 90, 230, 140) : Gdiplus::Color(a, 240, 90, 90), 3.0f * ui);
                gr.DrawEllipse(&ring, float(sx) - r, float(sy) - r, 2 * r, 2 * r);
            }
            if (!board) {
                const bool flashing = now < flashUntil;
                DrawCrosshair(gr, W, H, flashing ? (lastHit ? Gdiplus::Color(255, 90, 230, 140) : Gdiplus::Color(255, 240, 90, 90))
                                                 : Gdiplus::Color(255, 0, 255, 200), ui);
            }

            const Gdiplus::Color panel(170, 10, 12, 18);
            FillRoundRect(gr, 20 * ui, 18 * ui, 640 * ui, 64 * ui, 12 * ui, panel);
            FillRoundRect(gr, W / 2.0f - 150 * ui, 18 * ui, 300 * ui, 80 * ui, 12 * ui, panel);
            FillRoundRect(gr, W - 380 * ui, 18 * ui, 360 * ui, 64 * ui, 12 * ui, panel);
            if (phase == P::Play) {
                const double left = std::clamp(1.0 - (now - phaseStart) / gopt.seconds, 0.0, 1.0);
                const float bx = W / 2.0f - 120 * ui, by = 82 * ui, bw = 240 * ui, bh = 6 * ui;
                FillRoundRect(gr, bx, by, bw, bh, bh / 2, Gdiplus::Color(255, 45, 52, 66));
                if (left > 0.01) FillRoundRect(gr, bx, by, float(bw * left), bh, bh / 2, Gdiplus::Color(255, 79, 140, 255));
            }
            if (board) {
                FillRoundRect(gr, px0, py0, pw, ph, 20 * ui, Gdiplus::Color(225, 12, 14, 20));
                FillRoundRect(gr, chartX - 16 * ui, chartY - 14 * ui, chartW + 32 * ui, chartH + 28 * ui, 12 * ui, Gdiplus::Color(255, 20, 23, 30));
                DrawProgressChart(gr, labels, history, chartX, chartY, chartW, chartH, ui, phase == P::Results);
            }
        }

        auto text = [&](const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
            SelectObject(mem, f);
            SetTextColor(mem, c);
            DrawTextW(mem, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
        };
        auto R = [&](float x, float y, float w, float h) { return RECT{ LONG(x), LONG(y), LONG(x + w), LONG(y + h) }; };
        const COLORREF white = RGB(236, 238, 243), dim = RGB(150, 158, 172), accent = RGB(120, 170, 255),
                       green = RGB(52, 211, 153), gold = RGB(251, 191, 36);

        text(L"ODDPI Gridshot", R(38 * ui, 24 * ui, 600 * ui, 26 * ui), fBold, white, DT_LEFT | DT_SINGLELINE);
        text(subtitle, R(38 * ui, 50 * ui, 610 * ui, 24 * ui), fSmall, dim, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Timer
        {
            double secs = gopt.seconds;
            if (phase == P::Play) secs = std::max(0.0, gopt.seconds - (now - phaseStart));
            wchar_t buf[16];
            swprintf_s(buf, L"%d:%02d", int(std::ceil(secs - 1e-9)) / 60, int(std::ceil(secs - 1e-9)) % 60);
            text(buf, R(W / 2.0f - 150 * ui, 22 * ui, 300 * ui, 56 * ui), fTimer, white, DT_CENTER | DT_SINGLELINE);
        }
        {
            wchar_t buf[96];
            const int shots = hits + misses;
            swprintf_s(buf, L"%ls", Thousands(GridshotScore(hits, misses)).c_str());
            text(buf, R(W - 370 * ui, 4 * ui, 340 * ui, 52 * ui), fBig, white, DT_RIGHT | DT_SINGLELINE);
            swprintf_s(buf, L"%d hits  \x00B7  %d%%  \x00B7  Esc to leave", hits, shots ? 100 * hits / shots : 100);
            text(buf, R(W - 370 * ui, 56 * ui, 340 * ui, 22 * ui), fSmall, dim, DT_RIGHT | DT_SINGLELINE);
        }

        if (phase == P::Countdown) {
            const int n = 3 - int(now - phaseStart);
            text(std::to_wstring(std::max(1, n)), R(W / 2.0f - 100 * ui, H / 2.0f - 160 * ui, 200 * ui, 110 * ui), fHuge, white,
                 DT_CENTER | DT_SINGLELINE);
        }

        if (board) {
            const ProgressStats s = ComputeProgress(history);
            if (phase == P::Board) {
                text(L"Gridshot", R(px0, py0 + 26 * ui, pw, 46 * ui), fBig, white, DT_CENTER | DT_SINGLELINE);
                wchar_t buf[200];
                swprintf_s(buf, L"%.0f seconds. Three targets at a time \x2014 hit as many as you can. Score = hits \x00D7 100 \x00D7 accuracy.", gopt.seconds);
                text(buf, R(px0 + 30 * ui, py0 + 80 * ui, pw - 60 * ui, 28 * ui), fBody, dim, DT_CENTER | DT_SINGLELINE);
            }
            else {
                const auto& r = result.last;
                text(L"Score " + Thousands(r.score), R(px0, py0 + 22 * ui, pw, 50 * ui), fBig, white, DT_CENTER | DT_SINGLELINE);
                wchar_t buf[200];
                swprintf_s(buf, L"%d hits  \x00B7  %d%% accuracy  \x00B7  %.0f ms per target  \x00B7  ODDPI %ls",
                           r.hits, (r.hits + r.misses) ? 100 * r.hits / (r.hits + r.misses) : 0, r.avgKillMs, r.oddpiOn ? L"on" : L"off");
                text(buf, R(px0, py0 + 78 * ui, pw, 28 * ui), fBody, dim, DT_CENTER | DT_SINGLELINE);
                if (newBest) text(L"New personal best!", R(px0, py0 + 108 * ui, pw, 30 * ui), fBold, gold, DT_CENTER | DT_SINGLELINE);
            }

            // Progress summary
            std::wstring line1, line2;
            {
                wchar_t buf[256];
                swprintf_s(buf, L"%d run%ls  \x00B7  best %ls", s.runs, s.runs == 1 ? L"" : L"s", Thousands(s.best).c_str());
                line1 = buf;
                if (s.window > 0) {
                    swprintf_s(buf, L"  \x00B7  last %d avg %ls vs first %d avg %ls  (%+.0f%%)", s.window,
                               Thousands(int(s.recentAvg)).c_str(), s.window, Thousands(int(s.firstAvg)).c_str(), s.improvement * 100);
                    line1 += buf;
                }
                if (s.onRuns > 0 && s.offRuns > 0) {
                    swprintf_s(buf, L"ODDPI on: avg %ls (%d runs)   \x00B7   off: avg %ls (%d runs)   \x00B7   %+.0f%% with ODDPI",
                               Thousands(int(s.onAvg)).c_str(), s.onRuns, Thousands(int(s.offAvg)).c_str(), s.offRuns,
                               (s.onAvg / s.offAvg - 1) * 100);
                    line2 = buf;
                }
                else if (s.runs > 0) line2 = L"Tip: play some runs with ODDPI off too (switch at the top of the ODDPI window) to compare.";
            }
            text(L"Your progress", R(chartX - 16 * ui, py0 + 150 * ui, 400 * ui, 28 * ui), fBold, white, DT_LEFT | DT_SINGLELINE);
            text(line1, R(chartX - 16 * ui, py0 + 180 * ui, chartW + 32 * ui, 26 * ui), fBody, s.improvement > 0.005 ? green : dim,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            text(line2, R(chartX - 16 * ui, py0 + 206 * ui, chartW + 32 * ui, 24 * ui), fSmall, dim, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            text(L"\x25CF blue = ODDPI on   \x25CF grey = ODDPI off   \x2014 5-run average   - - best",
                 R(chartX - 16 * ui, py0 + ph - 72 * ui, chartW + 32 * ui, 22 * ui), fSmall, dim, DT_LEFT | DT_SINGLELINE);
            text(phase == P::Board ? L"Click to start" : L"Click to play again  \x00B7  Esc to leave",
                 R(px0, py0 + ph - 46 * ui, pw, 30 * ui), fBold, accent, DT_CENTER | DT_SINGLELINE);

            for (const auto& l : labels) {
                RECT r = l.r;
                text(l.text, r, l.font == 0 ? fSmall : l.font == 1 ? fBody : fBold, l.color, l.fmt);
            }
        }
    };

    auto saveFrame = [&](const wchar_t* name) {
        if (!bot || bot->framesDir.empty()) return;
        render(clock);
        SavePng(bmp, bot->framesDir + L"\\" + name + L".png");
    };

    const double botStep = 1.0 / 240.0;
    while (!g_abort) {
        events.clear();
        double now;
        if (bot) {
            const double prev = clock;
            clock += botStep;
            now = clock;
            if ((phase == P::Board && now > 0.3) || (phase == P::Results && now - phaseStart > 0.5)) {
                if (phase == P::Results && result.runs >= bot->runs) break;
                events.push_back({ now, 0, 0, true });
            }
            if (phase == P::Play) {
                // Aim at the nearest target; re-plan after every click.
                if (!botArmed || bot->player.clicked) {
                    const Target* best = nullptr;
                    double bestAng = 1e9;
                    for (const auto& t : targets) {
                        const double a = cam.AngleTo(t.dir);
                        if (a < bestAng) { bestAng = a; best = &t; }
                    }
                    if (best) {
                        double dyaw = best->yaw - cam.yaw;
                        while (dyaw > 180) dyaw -= 360;
                        while (dyaw < -180) dyaw += 360;
                        const double dpitch = best->pitch - cam.pitch;
                        bot->player.Plan(size_t(++botPlan), now, dyaw * cpd, -dpitch * cpd, targetDeg * cpd, 1.0);
                        botArmed = true;
                    }
                }
                bot->player.Emit(prev, now, events);
            }
        }
        else {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) g_abort = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            reader.Drain(events);
            now = Now() - start;
            for (auto& e : events) e.t -= start;
        }

        for (const auto& e : events) {
            switch (phase) {
            case P::Board:
                if (e.leftDown) startCountdown(e.t);
                break;
            case P::Results:
                if (e.leftDown && e.t - phaseStart > 0.4) startCountdown(e.t);
                break;
            case P::Countdown:
                cam.yaw += e.dx / cpd;
                cam.pitch -= e.dy / cpd;
                cam.Update();
                break;
            case P::Play: {
                cam.yaw += e.dx / cpd;
                cam.pitch -= e.dy / cpd;
                cam.Update();
                if (!e.leftDown) break;
                int hitIndex = -1;
                for (size_t i = 0; i < targets.size(); ++i) {
                    if (cam.AngleTo(targets[i].dir) <= targetDeg) { hitIndex = int(i); break; }
                }
                lastHit = hitIndex >= 0;
                flashUntil = e.t + 0.15;
                if (lastHit) {
                    ++hits;
                    effects.push_back({ targets[size_t(hitIndex)].dir, e.t, true });
                    const int cell = targets[size_t(hitIndex)].cell;
                    targets.erase(targets.begin() + hitIndex);
                    spawnTarget(cell);
                    sfx::Play(sfx::Sound::Hit);
                }
                else {
                    ++misses;
                    sfx::Play(sfx::Sound::Miss);
                }
                break;
            }
            }
        }

        if (phase == P::Countdown) {
            const int n = int(now - phaseStart);
            if (n != int(lastTick) && n < 3) { lastTick = n; sfx::Play(sfx::Sound::Tick); }
            if (now - phaseStart >= 3.0) {
                phase = P::Play;
                phaseStart = now;
                targets.clear();
                for (int i = 0; i < 3; ++i) spawnTarget(-1);
                botArmed = false;
                sfx::Play(sfx::Sound::Start);
            }
        }
        if (phase == P::Play && now - phaseStart >= gopt.seconds) finishRun(now);

        if (bot) {
            if (!savedBoard && phase == P::Board && now > 0.1) { savedBoard = true; saveFrame(L"gs-1-board"); }
            if (!savedPlay && phase == P::Play && result.runs == bot->runs - 1 && now - phaseStart > 20) { savedPlay = true; saveFrame(L"gs-2-play"); }
            if (phase == P::Results && result.runs >= bot->runs && now - phaseStart > 0.45) { saveFrame(L"gs-3-results"); break; }
            if (now > 5000) break;
            continue;
        }
        render(now);
        if (wdc) BitBlt(wdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        if (FAILED(DwmFlush())) Sleep(1);
    }

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    for (HFONT f : { fSmall, fBody, fBold, fBig, fHuge, fTimer }) DeleteObject(f);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (!bot) {
        ClipCursor(nullptr);
        if (wdc) ReleaseDC(hwnd, wdc);
        if (hwnd) DestroyWindow(hwnd);
        if (wc.lpszClassName) UnregisterClassW(wc.lpszClassName, wc.hInstance);
        reader.Stop();
    }
    Gdiplus::GdiplusShutdown(gdiToken);
    return result.runs > 0;
}

}  // namespace

bool RunGridshot(const GridshotOptions& opt, GridshotResult& result)
{
    return RunGridshotImpl(opt, result, nullptr);
}

bool RunGridshotBot(const GridshotOptions& opt, double idealMultiplier, unsigned seed, int runs,
                    GridshotResult& result, const std::wstring& framesDir)
{
    GridBotRun bot;
    bot.player.rng.seed(seed * 2246822519u + 3u);
    bot.player.ideal = idealMultiplier;
    bot.startIdeal = idealMultiplier;
    bot.runs = std::max(1, runs);
    bot.framesDir = framesDir;
    return RunGridshotImpl(opt, result, &bot);
}

}  // namespace oddpi
