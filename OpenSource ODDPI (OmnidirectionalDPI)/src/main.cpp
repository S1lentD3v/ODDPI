// ODDPI - Omnidirectional DPI for any mouse.
//
// A settings tool for the signed Raw Accel driver: it never touches input itself,
// so anti-cheat sees only the official Raw Accel driver.

#include "RawAccel.h"
#include "Config.h"
#include "Driver.h"
#include "ProfileBuilder.h"
#include "Calibrate.h"
#include "Actions.h"
#include "Games.h"
#include "Gridshot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace oddpi;

namespace {

    const wchar_t kDefaultConfigPath[] = L"oddpi.ini";

    std::wstring Widen(const char* s)
    {
        int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (n <= 0) return L"(unprintable error)";
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
        w.resize(size_t(n) - 1);
        return w;
    }

    void PrintUsage()
    {
        wprintf(
            L"ODDPI - Omnidirectional DPI for any mouse (powered by the Raw Accel driver)\n"
            L"\n"
            L"Usage: oddpi <command> [file]\n"
            L"\n"
            L"  init    [config]   Create a commented config file (default: oddpi.ini)\n"
            L"  preview [config]   Show what the config does in every direction. Changes nothing.\n"
            L"  apply   [config]   Back up current Raw Accel settings, then make the config live\n"
            L"  calibrate [config] 3-minute flick test that measures how you aim in each direction\n"
            L"                     and evens them out (sets Right/Left/Up/Down and Rotation)\n"
            L"  calibrate2d [config] Same, with the older 2D cursor test instead of the 3D one\n"
            L"  analyze [log]      Re-run the analysis on a saved calibration log (default: latest)\n"
            L"  sensfinder [config] Find your best in-game sensitivity (rounds of A/B aim tests\n"
            L"                     that narrow down, like KovaaK's sens finder)\n"
            L"  gridshot [config]  60-second Gridshot aim test with an improvement board\n"
            L"  games              List known games and their sensitivity scale (yaw)\n"
            L"  convert <game> <sens> [to-game]\n"
            L"                     cm/360 and the same sensitivity in other games\n"
            L"  off                Turn ODDPI off (mouse back to normal; other Raw Accel profiles kept)\n"
            L"  on      [config]   Turn ODDPI back on (same as apply)\n"
            L"  status             Show the Raw Accel driver version and active settings\n"
            L"  restore [backup]   Undo the last apply. 'restore original' = state before ODDPI was\n"
            L"                     first used. Or pass a backup file path.\n"
            L"  backups            List backup files\n"
            L"\n"
            L"Requires Raw Accel " RA_M_STR(RA_VER_MAJOR) L"." RA_M_STR(RA_VER_MINOR) L".x to be installed: https://github.com/RawAccelOfficial/rawaccel\n");
    }

    bool Load(const std::wstring& path, Config& cfg)
    {
        std::vector<std::wstring> errors, warnings;
        bool ok = LoadConfig(path, cfg, errors, warnings);
        for (auto& w : warnings) wprintf(L"warning: %ls\n", w.c_str());
        for (auto& e : errors) fwprintf(stderr, L"error: %ls\n", e.c_str());
        return ok;
    }

    bool Build(const Config& cfg, int effectiveDpi, ra::profile& profile)
    {
        std::vector<std::wstring> errors;
        bool ok = BuildProfile(cfg, effectiveDpi, profile, errors);
        for (auto& e : errors) fwprintf(stderr, L"error: %ls\n", e.c_str());
        return ok;
    }

    void PrintNotes(const std::vector<std::wstring>& notes)
    {
        for (auto& n : notes) wprintf(L"note: %ls\n", n.c_str());
    }

    int CmdInit(const std::wstring& path)
    {
        std::wstring error;
        if (!WriteDefaultConfig(path, error)) {
            fwprintf(stderr, L"error: %ls\n", error.c_str());
            return 1;
        }
        wprintf(L"Created %ls\nEdit it, then run:  oddpi preview  and  oddpi apply\n", path.c_str());
        return 0;
    }

    int CmdPreview(const std::wstring& path)
    {
        Config cfg;
        if (!Load(path, cfg)) return 1;

        // Preview works without Raw Accel, but uses its DPI setting when it can read it.
        int driverDpi = 0;
        try {
            driverDpi = ReadDriver().Base().default_dev_cfg.dpi;
        }
        catch (const std::exception&) {}

        const int dpi = EffectiveDpi(cfg, driverDpi);
        auto profile = std::make_unique<ra::profile>();
        if (!Build(cfg, dpi, *profile)) return 1;

        PrintPreview(cfg, dpi, *profile);
        if (cfg.mouseDpi == 0 && dpi > 0) {
            wprintf(L"\nUsing mouse DPI %d from your current Raw Accel settings.\n", dpi);
        }
        wprintf(L"\nNothing was changed. Run 'oddpi apply' to make this live.\n");
        return 0;
    }

    int CmdApply(const std::wstring& path)
    {
        Config cfg;
        if (!Load(path, cfg)) return 1;

        wprintf(L"Applying (Raw Accel deliberately takes about a second)...\n");
        ApplyOutcome outcome;
        std::vector<std::wstring> errors;
        if (!ApplyConfig(cfg, outcome, errors)) {
            for (auto& e : errors) fwprintf(stderr, L"error: %ls\n", e.c_str());
            return 1;
        }
        wprintf(L"Backed up previous Raw Accel settings to:\n  %ls\n", outcome.backup.c_str());
        wprintf(L"Done - ODDPI is active.\n");
        PrintNotes(outcome.notes);
        return 0;
    }

    int CmdStatus()
    {
        const ra::version_t v = DriverVersion();
        wprintf(L"Raw Accel driver %d.%d.%d (ODDPI built against " RA_M_STR(RA_VER_MAJOR) L"."
                RA_M_STR(RA_VER_MINOR) L"." RA_M_STR(RA_VER_PATCH) L")\n", v.major, v.minor, v.patch);

        const Snapshot s = ReadDriver();
        if (s.Empty()) {
            wprintf(L"No settings loaded: mouse input passes through unchanged.\n");
            return 0;
        }

        const auto& cfg = s.Base().default_dev_cfg;
        wprintf(L"Default device config: %ls, DPI %d, polling rate %d%ls\n",
                cfg.disable ? L"DISABLED" : L"enabled", cfg.dpi, cfg.polling_rate,
                cfg.polling_rate == 0 ? L" (auto)" : L"");

        wprintf(L"\nProfiles:\n");
        for (unsigned i = 0; i < s.ModifierCount(); ++i) {
            wprintf(L"  %ls %ls\n", i == 0 ? L"[default]" : L"         ",
                    DescribeProfile(s.Modifier(i).prof).c_str());
        }

        if (s.DeviceCount() > 0) {
            wprintf(L"\nDevice mappings:\n");
            for (unsigned i = 0; i < s.DeviceCount(); ++i) {
                const auto& d = s.Device(i);
                wprintf(L"  %ls -> %ls%ls\n", d.name[0] ? d.name : d.id,
                        d.profile[0] ? d.profile : L"(default)",
                        d.config.disable ? L"  [disabled]" : L"");
            }
        }

        wprintf(L"\n%ls\n", IsOddpiActive(s)
            ? L"ODDPI is the default profile."
            : L"ODDPI is not active (run 'oddpi apply').");
        PrintNotes(CoverageNotes(s, cfg.dpi));
        return 0;
    }

    int CmdRestore(const std::wstring& arg)
    {
        wprintf(L"Restoring (Raw Accel deliberately takes about a second)...\n");
        bool empty = false;
        std::wstring path = RestoreBackup(arg, empty);
        wprintf(L"Restored %ls\n", path.c_str());
        wprintf(L"Done - %ls\n", empty
            ? L"Raw Accel has no settings loaded (mouse input unchanged)."
            : L"previous Raw Accel settings restored.");
        return 0;
    }

    const wchar_t* DirectionName(double deg)
    {
        static const wchar_t* names[] = { L"Right", L"Up-right", L"Up", L"Up-left",
                                          L"Left", L"Down-left", L"Down", L"Down-right" };
        double k = deg / 45.0;
        if (std::fabs(k - std::round(k)) > 1e-6) return L"";
        return names[int(std::lround(k)) % 8];
    }

    bool SetIni(const std::wstring& path, const wchar_t* section, const wchar_t* key, double value, int decimals)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%.*f", decimals, value);
        return WritePrivateProfileStringW(section, key, buf, path.c_str()) != FALSE;
    }

    // Asks for the mouse DPI. Returns 0 if the user just presses Enter.
    int PromptDpi()
    {
        for (int attempt = 0; attempt < 3; ++attempt) {
            wprintf(L"Type your mouse's DPI (the DPI set in the mouse or its software, e.g. 800)\n"
                    L"or just press Enter to assume 800: ");
            fflush(stdout);
            wchar_t buf[32] = {};
            if (!fgetws(buf, 32, stdin)) return 0;
            wchar_t* end = nullptr;
            long dpi = wcstol(buf, &end, 10);
            while (end && iswspace(*end)) ++end;
            if (end == buf || (buf[0] == L'\n' || buf[0] == L'\r')) return 0;
            if (end && *end == L'\0' && dpi >= 100 && dpi <= 50000) return int(dpi);
            wprintf(L"Please type a whole number between 100 and 50000.\n");
        }
        return 0;
    }

    std::wstring ExeDirectory()
    {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        return std::filesystem::path(p).parent_path().wstring();
    }

    std::wstring ReadLine()
    {
        wchar_t buf[128] = {};
        if (!fgetws(buf, 128, stdin)) return L"";
        std::wstring s = buf;
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        size_t i = 0;
        while (i < s.size() && iswspace(s[i])) ++i;
        return s.substr(i);
    }

    void PrintGameList(const std::vector<GameInfo>& games)
    {
        for (const auto& g : games) {
            std::wstring aliases;
            for (const auto& a : g.aliases) aliases += (aliases.empty() ? L"" : L", ") + a;
            wprintf(L"  %-22ls yaw %-9g %-22ls %ls%ls%ls\n", g.name.c_str(), g.yaw, aliases.c_str(),
                    g.note.c_str(), g.estimate ? L"  [estimate]" : L"", g.user ? L"  [games.ini]" : L"");
        }
    }

    // Asks for the game and in-game sensitivity and saves them. false = could not save.
    bool PromptGame(const std::vector<GameInfo>& games, const std::wstring& iniPath, Config& cfg)
    {
        wprintf(L"Which game do you play? The test then uses your real flick distances.\n"
                L"Type a name (cs2, valorant, apex, ow2, cod, fortnite, ... or 'list'),\n"
                L"'cm' if you know your cm/360, or press Enter to skip: ");
        fflush(stdout);
        for (int attempt = 0; attempt < 5; ++attempt) {
            std::wstring answer = ReadLine();
            if (answer.empty()) {
                cfg.game = L"none";
                wprintf(L"Skipped (saved Game=none; set [Game] in the config to change it).\n\n");
                return WritePrivateProfileStringW(L"Game", L"Game", L"none", iniPath.c_str()) != FALSE;
            }
            if (_wcsicmp(answer.c_str(), L"list") == 0) {
                PrintGameList(games);
                wprintf(L"Game: ");
                fflush(stdout);
                continue;
            }
            const bool cm = _wcsicmp(answer.c_str(), L"cm") == 0;
            const GameInfo* g = cm ? nullptr : FindGame(games, answer);
            if (!cm && !g) {
                wprintf(L"Unknown game '%ls'. Type 'list' to see them all, or 'cm': ", answer.c_str());
                fflush(stdout);
                continue;
            }
            for (int tries = 0; tries < 3; ++tries) {
                if (cm) wprintf(L"Your cm/360 (cm of mouse movement for a full turn): ");
                else wprintf(L"Your in-game sensitivity in %ls%ls: ", g->name.c_str(),
                             g->note.empty() ? L"" : (L" (" + g->note + L")").c_str());
                fflush(stdout);
                double v = 0;
                std::wstring s = ReadLine();
                wchar_t* end = nullptr;
                v = wcstod(s.c_str(), &end);
                if (end == s.c_str() || *end || !(v > 0)) {
                    wprintf(L"Please type a number greater than 0.\n");
                    continue;
                }
                wchar_t num[64];
                swprintf_s(num, L"%g", v);
                bool ok;
                if (cm) {
                    cfg.cm360 = v;
                    ok = WritePrivateProfileStringW(L"Game", L"Cm360", num, iniPath.c_str()) != FALSE;
                }
                else {
                    cfg.game = g->name;
                    cfg.gameSensitivity = v;
                    ok = WritePrivateProfileStringW(L"Game", L"Game", g->name.c_str(), iniPath.c_str()) != FALSE
                      && WritePrivateProfileStringW(L"Game", L"Sensitivity", num, iniPath.c_str()) != FALSE;
                }
                if (!ok) fwprintf(stderr, L"error: could not write to %ls\n", iniPath.c_str());
                else wprintf(L"Saved to %ls\n\n", iniPath.c_str());
                return ok;
            }
            break;
        }
        wprintf(L"Skipping the game for now.\n\n");
        return true;
    }

    // oddpi convert <game> <sens> [to-game]
    int CmdConvert(const std::wstring& from, const std::wstring& sensText, const std::wstring& to,
                   const std::wstring& configPath)
    {
        const auto games = LoadGames(ExeDirectory());
        if (from.empty() || sensText.empty()) {
            wprintf(L"Usage: oddpi convert <game> <sensitivity> [to-game]\n\nGames:\n");
            PrintGameList(games);
            return 1;
        }
        const GameInfo* a = FindGame(games, from);
        if (!a) {
            fwprintf(stderr, L"error: unknown game '%ls' (see 'oddpi games').\n", from.c_str());
            return 1;
        }
        wchar_t* end = nullptr;
        const double sens = wcstod(sensText.c_str(), &end);
        if (end == sensText.c_str() || *end || !(sens > 0)) {
            fwprintf(stderr, L"error: '%ls' is not a sensitivity.\n", sensText.c_str());
            return 1;
        }

        // DPI for cm/360: config MouseDPI, else Raw Accel's, else unknown.
        int dpi = 0;
        Config cfg;
        std::vector<std::wstring> errors, warnings;
        if (LoadConfig(configPath, cfg, errors, warnings)) dpi = cfg.mouseDpi;
        if (dpi <= 0) {
            try { dpi = ReadDriver().Base().default_dev_cfg.dpi; } catch (const std::exception&) {}
        }

        wprintf(L"%ls %g", a->name.c_str(), sens);
        if (dpi > 0) wprintf(L" at %d DPI = %.2f cm/360 (%.0f counts per 360)\n", dpi, Cm360(sens, a->yaw, dpi), 360.0 / (sens * a->yaw));
        else wprintf(L" = %.0f counts per 360 (set MouseDPI for cm/360)\n", 360.0 / (sens * a->yaw));
        if (a->estimate) wprintf(L"note: %ls's yaw is a community estimate.\n", a->name.c_str());

        if (!to.empty()) {
            const GameInfo* b = FindGame(games, to);
            if (!b) {
                fwprintf(stderr, L"error: unknown game '%ls' (see 'oddpi games').\n", to.c_str());
                return 1;
            }
            wprintf(L"\n  %ls: %.4g%ls\n", b->name.c_str(), sens * a->yaw / b->yaw, b->estimate ? L"  (estimate)" : L"");
            return 0;
        }
        wprintf(L"\nSame 360 distance in:\n");
        for (const auto& b : games) {
            if (&b == a) continue;
            wprintf(L"  %-22ls %.4g%ls\n", b.name.c_str(), sens * a->yaw / b.yaw, b.estimate ? L"  (estimate)" : L"");
        }
        wprintf(L"\nMatches 360 distance only; zoom/ADS and FOV-dependent scaling are not included.\n");
        return 0;
    }

    void PrintCalResult(const CalResult& r)
    {
        wprintf(L"\n  Direction            flicks   first-flick length\n");
        for (const auto& d : r.directions) {
            wprintf(L"  %-10ls %5.1f deg   %d/%d    ", DirectionName(d.angleDeg), d.angleDeg, d.valid, d.total);
            if (d.medianReach > 0) {
                double pct = (d.medianReach - 1) * 100;
                wprintf(L"%5.0f%% of target  (%ls)\n", d.medianReach * 100,
                        std::fabs(pct) < 1 ? L"on target" : pct > 0 ? L"overshoot" : L"undershoot");
            }
            else wprintf(L"  not enough data\n");
        }
        wprintf(L"\n  Usable flicks: %d of %d\n", r.validTrials, r.totalTrials);
        if (!r.ok) {
            fwprintf(stderr, L"\nerror: %ls\n", r.error.c_str());
            return;
        }
        wprintf(L"\n  Directions differ by %.1f%% now; after correction about %.1f%%.\n",
                r.spreadBefore, r.spreadAfterFit);
        wprintf(L"  Measured grip tilt: %+.1f deg%ls\n", r.rawRotationDeg,
                r.rotationDeg == 0 ? L" (under 1 deg, ignored)" : L"");
        wprintf(L"\n  Correction   measured        used   (shrunk toward 1.0 when within the noise)\n");
        const wchar_t* names[4] = { L"Right", L"Left", L"Up", L"Down" };
        const double used[4] = { r.right, r.left, r.up, r.down };
        for (int i = 0; i < 4; ++i) {
            wprintf(L"  %-8ls   x%.3f +-%.3f   x%.3f\n", names[i], r.raw[i], r.uncertainty[i], used[i]);
        }
    }

    // Re-runs the analysis on a saved calibration log (default: the latest). Changes nothing.
    int CmdAnalyze(const std::wstring& arg)
    {
        std::wstring path = arg;
        if (path.empty()) {
            auto logs = ListCalibrationLogs();
            if (logs.empty()) {
                fwprintf(stderr, L"error: no calibration logs yet (run 'oddpi calibrate').\n");
                return 1;
            }
            path = logs.front();
        }
        CalOptions opt;
        std::vector<CalTrial> trials;
        std::wstring error;
        if (!LoadTrials(path, opt, trials, error)) {
            fwprintf(stderr, L"error: %ls\n", error.c_str());
            return 1;
        }
        wprintf(L"%ls  (%d DPI, %zu flicks)\n", path.c_str(), opt.dpi, trials.size());

        // Every flick: reach % (angle error deg, reaction ms) per direction.
        wprintf(L"\n  Each flick: reach%% / angle error deg / reaction ms\n");
        for (int k = 0; k < opt.directions; ++k) {
            wprintf(L"  %5.1f deg:", 360.0 * k / opt.directions);
            for (const auto& t : trials) {
                if (t.practice || t.direction != k) continue;
                CalTrialResult a = AnalyzeTrial(t);
                if (a.valid) wprintf(L"  %3.0f/%+4.0f/%3.0f", a.reach * 100, a.angleErr, a.reaction * 1000);
                else wprintf(L"  [%ls]", a.rejectReason);
            }
            wprintf(L"\n");
        }

        CalResult r = FitCalibration(trials, opt.directions);
        PrintCalResult(r);
        return r.ok ? 0 : 1;
    }

    int CmdCalibrate(const std::wstring& path, bool scene3d)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (CmdInit(path) != 0) return 1;
        }

        Config cfg;
        if (!Load(path, cfg)) return 1;
        const std::wstring full = std::filesystem::absolute(path, ec).wstring();

        const int driverDpi = ReadDriver().Base().default_dev_cfg.dpi;
        CalOptions opt;
        if (cfg.mouseDpi > 0) opt.dpi = cfg.mouseDpi;
        else {
            int typed = PromptDpi();
            if (typed > 0) {
                // Saved so Raw Accel normalises by the real DPI too; ODDPI builds its output DPI
                // from the same value, so overall sensitivity does not change.
                if (!SetIni(full, L"General", L"MouseDPI", typed, 0)) {
                    fwprintf(stderr, L"error: could not write MouseDPI to %ls\n", full.c_str());
                    return 1;
                }
                cfg.mouseDpi = typed;
                opt.dpi = typed;
                wprintf(L"Saved MouseDPI=%d to %ls\n\n", typed, path.c_str());
            }
            else {
                opt.dpi = driverDpi > 0 ? driverDpi : 800;
                wprintf(L"Assuming %d DPI for target distances.\n\n", opt.dpi);
            }
        }

        // Which game: makes every test flick the same hand movement as in that game.
        const auto games = LoadGames(ExeDirectory());
        const bool gameSet = (FindGame(games, cfg.game) && cfg.gameSensitivity > 0) || cfg.cm360 > 0;
        if (!gameSet && _wcsicmp(cfg.game.c_str(), L"none") != 0) {
            if (!PromptGame(games, full, cfg)) return 1;
        }
        opt.countsPerDegree = CountsPerDegree(cfg, games, opt.dpi, opt.label);
        opt.fov = cfg.fov;
        opt.sound = cfg.sound;
        opt.volume = cfg.volume;
        opt.scene3d = scene3d;
        if (opt.countsPerDegree > 0) wprintf(L"Test matches %ls (FOV %.0f).\n\n", opt.label.c_str(), opt.fov);

        wprintf(L"Step 1/3: applying %ls so the test measures exactly how your mouse feels now.\n", path.c_str());
        if (CmdApply(path) != 0) return 1;

        wprintf(L"\nStep 2/3: the test window opens fullscreen (about a minute; it stops by itself, Esc cancels).\n");
        std::vector<CalTrial> trials;
        if (!RunCalibrationWindow(opt, trials)) {
            wprintf(L"Calibration cancelled. Your directional settings were not changed.\n");
            return 1;
        }

        const std::wstring logPath = SaveCalibrationLog(opt, trials);
        if (!logPath.empty()) wprintf(L"Flick log saved: %ls\n", logPath.c_str());
        else wprintf(L"warning: could not save the flick log to %ls\n", CalibrationDirectory().c_str());

        CalResult r = FitCalibration(trials, opt.directions);
        wprintf(L"\nStep 3/3: results\n");
        PrintCalResult(r);
        if (!r.ok) return 1;

        Config next = WithCalibration(cfg, r);

        wprintf(L"\n  Setting      current      new\n");
        wprintf(L"  Right       %7.3f  ->  %7.3f\n", cfg.right, next.right);
        wprintf(L"  Left        %7.3f  ->  %7.3f\n", cfg.left, next.left);
        wprintf(L"  Up          %7.3f  ->  %7.3f\n", cfg.up, next.up);
        wprintf(L"  Down        %7.3f  ->  %7.3f\n", cfg.down, next.down);
        wprintf(L"  Rotation    %7.1f  ->  %7.1f\n", cfg.rotation, next.rotation);

        const int dpi = EffectiveDpi(next, driverDpi);
        auto profile = std::make_unique<ra::profile>();
        if (!Build(next, dpi, *profile)) return 1;

        wprintf(L"\nSave these to %ls and apply them? [y/N] ", path.c_str());
        fflush(stdout);
        wchar_t answer[16] = {};
        if (!fgetws(answer, 16, stdin) || (answer[0] != L'y' && answer[0] != L'Y')) {
            wprintf(L"Not saved. Your config is unchanged.\n");
            return 0;
        }

        const std::wstring bak = full + L".bak";
        if (!CopyFileW(full.c_str(), bak.c_str(), FALSE)) {
            fwprintf(stderr, L"error: could not back up %ls\n", full.c_str());
            return 1;
        }
        bool ok = SetIni(full, L"Directional", L"Right", next.right, 3)
               && SetIni(full, L"Directional", L"Left", next.left, 3)
               && SetIni(full, L"Directional", L"Up", next.up, 3)
               && SetIni(full, L"Directional", L"Down", next.down, 3)
               && SetIni(full, L"General", L"Rotation", next.rotation, 1);
        if (!ok) {
            fwprintf(stderr, L"error: could not write %ls (previous version: %ls)\n", full.c_str(), bak.c_str());
            return 1;
        }
        wprintf(L"Saved (previous config: %ls)\n", bak.c_str());
        int rc = CmdApply(path);
        if (rc == 0) {
            wprintf(L"\nPlay with it for a while. Running 'oddpi calibrate' again refines the result.\n");
        }
        return rc;
    }

    // Finds the user's best in-game sensitivity with the 3D trainer (KovaaK's-style narrowing).
    int CmdSensFinder(const std::wstring& path)
    {
        Config cfg;
        if (!Load(path, cfg)) return 1;
        const auto games = LoadGames(ExeDirectory());
        const GameInfo* game = FindGame(games, cfg.game);
        if (!game || !(cfg.gameSensitivity > 0)) {
            fwprintf(stderr, L"error: set your game and in-game sensitivity first ([Game] in %ls, or the GUI's Game row).\n", path.c_str());
            return 1;
        }
        SensFinderOptions so;
        so.cal.dpi = cfg.mouseDpi > 0 ? cfg.mouseDpi : 800;
        so.cal.countsPerDegree = CountsPerDegree(cfg, games, so.cal.dpi, so.cal.label);
        so.cal.fov = cfg.fov;
        so.cal.sound = cfg.sound;
        so.cal.volume = cfg.volume;
        so.gameName = game->name;
        so.gameSens = cfg.gameSensitivity;

        wprintf(L"Sens finder for %ls %g: about %d targets in %d rounds (Esc cancels).\n"
                L"Your ODDPI directional settings stay on, exactly as in game.\n",
                game->name.c_str(), cfg.gameSensitivity, 4 + so.rounds * so.targetsPerRound, so.rounds);
        SensFinderResult r;
        if (!RunSensFinder(so, r)) {
            wprintf(L"Cancelled.\n");
            return 1;
        }

        wprintf(L"\n  Round   lower sens (time, hits)      higher sens (time, hits)     moved to\n");
        for (size_t i = 0; i < r.rounds.size(); ++i) {
            const auto& rd = r.rounds[i];
            wprintf(L"  %zu       %.3g  (%.0f ms, %d/%d)        %.3g  (%.0f ms, %d/%d)        %.3g\n", i + 1,
                    cfg.gameSensitivity * rd.low.multiplier, rd.low.score * 1000, rd.low.hits, rd.low.targets,
                    cfg.gameSensitivity * rd.high.multiplier, rd.high.score * 1000, rd.high.hits, rd.high.targets,
                    cfg.gameSensitivity * rd.centerAfter);
        }
        const double best = cfg.gameSensitivity * r.multiplier;
        wprintf(L"\nYour sensitivity: %ls %.3g  (%.1f cm/360, was %g = %.1f cm/360)\n", game->name.c_str(), best,
                Cm360(best, game->yaw, so.cal.dpi), cfg.gameSensitivity, Cm360(cfg.gameSensitivity, game->yaw, so.cal.dpi));

        wprintf(L"\nSave %.3g as your %ls sensitivity in %ls? (You also need to set it in the game.) [y/N] ",
                best, game->name.c_str(), path.c_str());
        fflush(stdout);
        wchar_t answer[16] = {};
        if (!fgetws(answer, 16, stdin) || (answer[0] != L'y' && answer[0] != L'Y')) {
            wprintf(L"Not saved.\n");
            return 0;
        }
        wchar_t num[32];
        swprintf_s(num, L"%.3g", best);
        std::error_code ec;
        const std::wstring full = std::filesystem::absolute(path, ec).wstring();
        if (!WritePrivateProfileStringW(L"Game", L"Sensitivity", num, full.c_str())) {
            fwprintf(stderr, L"error: could not write %ls\n", full.c_str());
            return 1;
        }
        wprintf(L"Saved. Now set %ls sensitivity to %ls in the game.\n", game->name.c_str(), num);
        return 0;
    }

    // 60-second Gridshot; every run is saved to the improvement board.
    int CmdGridshot(const std::wstring& path)
    {
        Config cfg;
        if (!Load(path, cfg)) return 1;
        const auto games = LoadGames(ExeDirectory());
        GridshotOptions go;
        go.cal.dpi = cfg.mouseDpi > 0 ? cfg.mouseDpi : 800;
        go.cal.countsPerDegree = CountsPerDegree(cfg, games, go.cal.dpi, go.cal.label);
        go.cal.fov = cfg.fov;
        go.cal.sound = cfg.sound;
        go.cal.volume = cfg.volume;
        if (const GameInfo* g = FindGame(games, cfg.game); g && cfg.gameSensitivity > 0) {
            go.gameName = g->name;
            go.gameSens = cfg.gameSensitivity;
        }
        try { go.oddpiOn = IsOddpiActive(ReadDriver()); } catch (const std::exception&) { go.oddpiOn = false; }
        wchar_t settings[96];
        swprintf_s(settings, L"R%.2f L%.2f U%.2f D%.2f", cfg.right, cfg.left, cfg.up, cfg.down);
        go.settings = settings;

        wprintf(L"Gridshot (ODDPI %ls). Click to start, click to play again, Esc to leave.\n", go.oddpiOn ? L"on" : L"off");
        GridshotResult r;
        if (!RunGridshot(go, r)) {
            wprintf(L"No run finished.\n");
            return 1;
        }
        const auto& s = r.stats;
        wprintf(L"Last score %d (%d hits, %d misses)%ls\n", r.last.score, r.last.hits, r.last.misses,
                r.personalBest ? L" - new personal best!" : L"");
        wprintf(L"%d runs in total, best %d", s.runs, s.best);
        if (s.window > 0) wprintf(L", last %d avg %.0f vs first %d avg %.0f (%+.0f%%)", s.window, s.recentAvg, s.window, s.firstAvg, s.improvement * 100);
        wprintf(L"\nHistory: %ls\n", DefaultHistoryPath().c_str());
        return 0;
    }

    int CmdBackups()
    {
        auto original = OriginalBackupPath();
        wprintf(L"Backup folder: %ls\n\n", BackupDirectory().c_str());

        if (GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  original   %ls\n", original.c_str());
        }

        auto backups = ListBackups();
        for (size_t i = 0; i < backups.size(); ++i) {
            wprintf(L"  %ls %ls\n", i == 0 ? L"latest    " : L"          ", backups[i].c_str());
        }
        if (backups.empty()) wprintf(L"  (no backups yet - they are made by 'oddpi apply')\n");
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring cmd = argv[1];
    const std::wstring arg = argc > 2 ? argv[2] : L"";
    const std::wstring arg3 = argc > 3 ? argv[3] : L"";
    const std::wstring arg4 = argc > 4 ? argv[4] : L"";
    const std::wstring config = arg.empty() ? kDefaultConfigPath : arg;

    auto is = [&](const wchar_t* name) { return _wcsicmp(cmd.c_str(), name) == 0; };

    try {
        if (is(L"init"))    return CmdInit(config);
        if (is(L"preview")) return CmdPreview(config);
        if (is(L"apply"))   return CmdApply(config);
        if (is(L"calibrate")) return CmdCalibrate(config, true);
        if (is(L"calibrate2d")) return CmdCalibrate(config, false);
        if (is(L"sensfinder")) return CmdSensFinder(config);
        if (is(L"gridshot")) return CmdGridshot(config);
        if (is(L"selftestgrid")) {
            // Hidden: a simulated player that improves run after run plays 8 Gridshot runs into a
            // throw-away history file; checks that runs are saved and the board shows the trend.
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            GridshotOptions go;
            go.cal.dpi = 1600;
            go.cal.countsPerDegree = 1.0 / (0.153 * 0.07);
            go.cal.fov = 103;
            go.gameName = L"Valorant";
            go.gameSens = 0.153;
            go.settings = L"R1.15 L1.01 U1.00 D0.84";
            go.historyPath = std::wstring(tmp) + L"oddpi-gridshot-selftest.csv";
            DeleteFileW(go.historyPath.c_str());
            GridshotResult r;
            const bool ok = RunGridshotBot(go, 1.8, 1, 8, r, arg);
            const auto h = LoadHistory(go.historyPath);
            for (size_t i = 0; i < h.size(); ++i) {
                wprintf(L"  run %zu: score %6d  hits %3d  misses %2d  %4.0f ms  ODDPI %ls\n", i + 1, h[i].score, h[i].hits,
                        h[i].misses, h[i].avgKillMs, h[i].oddpiOn ? L"on" : L"off");
            }
            const ProgressStats s = ComputeProgress(h);
            wprintf(L"\n  runs %d, best %d, first %d avg %.0f -> last %d avg %.0f (%+.0f%%), on %d / off %d\n", s.runs, s.best,
                    s.window, s.firstAvg, s.window, s.recentAvg, s.improvement * 100, s.onRuns, s.offRuns);
            const bool pass = ok && h.size() == 8 && s.improvement > 0 && s.onRuns == 4 && s.offRuns == 4 && r.runs == 8;
            wprintf(pass ? L"  All checks passed\n" : L"  FAILED\n");
            DeleteFileW(go.historyPath.c_str());
            return pass ? 0 : 1;
        }
        if (is(L"off")) {
            wprintf(L"Turning ODDPI off (Raw Accel deliberately takes about a second)...\n");
            std::wstring backup;
            if (!DisableOddpi(backup)) {
                wprintf(L"ODDPI is already off.\n");
                return 0;
            }
            const Snapshot s = ReadDriver();
            wprintf(L"Done - ODDPI is off. %ls\nTurn it back on with: oddpi on\n",
                    s.Empty() ? L"Your mouse is completely unchanged." : L"Your other Raw Accel profiles are still active.");
            return 0;
        }
        if (is(L"on")) return CmdApply(config);
        if (is(L"selftestsens")) {
            // Hidden: simulated players with known best sens; does the finder land near it?
            SensFinderOptions so;
            so.cal.dpi = 1600;
            so.cal.countsPerDegree = 1.0 / (0.125 * 0.07);
            so.cal.fov = 103;
            so.gameName = L"Valorant";
            so.gameSens = 0.125;
            const double ideals[] = { 0.6, 0.8, 1.0, 1.3, 1.7 };
            int fails = 0;
            double sumAbs = 0;
            int runs = 0;
            wprintf(L"  ideal   seed   found    error   rounds (center after each)\n");
            for (double ideal : ideals) {
                for (unsigned seed = 1; seed <= 5; ++seed) {
                    SensFinderResult r;
                    const bool ok = RunSensFinderBot(so, ideal, seed, r, (seed == 1 && ideal == 0.8 && !arg.empty()) ? arg : L"");
                    const double err = r.multiplier / ideal - 1;
                    sumAbs += std::fabs(err);
                    ++runs;
                    const bool bad = !ok || std::fabs(err) > 0.15;
                    fails += bad ? 1 : 0;
                    wprintf(L"  %5.2f   %4u   %5.3f   %+5.1f%%  ", ideal, seed, r.multiplier, err * 100);
                    for (auto& rd : r.rounds) wprintf(L" %.2f", rd.centerAfter);
                    wprintf(L"%ls\n", bad ? L"   <- off by more than 15%" : L"");
                }
            }
            wprintf(L"\n  average error %.1f%%, %d of %d runs off by more than 15%%\n", 100 * sumAbs / runs, fails, runs);
            return fails > runs / 10 ? 1 : 0;
        }
        if (is(L"diagnose")) {
            // Hidden: how far each measured direction is from what Right/Left/Up/Down predict.
            std::wstring path = arg;
            if (path.empty()) {
                auto logs = ListCalibrationLogs();
                if (logs.empty()) { fwprintf(stderr, L"error: no calibration logs\n"); return 1; }
                path = logs.front();
            }
            CalOptions opt;
            std::vector<CalTrial> trials;
            std::wstring error;
            if (!LoadTrials(path, opt, trials, error)) { fwprintf(stderr, L"error: %ls\n", error.c_str()); return 1; }
            double chi2 = 0;
            const auto checks = CheckDirections(trials, opt.directions, chi2);
            wprintf(L"%ls\n\n  Direction    flicks  measured   4-value fit   difference   (noise +-)\n", path.c_str());
            int used = 0;
            for (const auto& c : checks) {
                if (c.n < 2) continue;
                ++used;
                const double diff = c.median / c.predicted - 1, noise = c.se / c.predicted;
                wprintf(L"  %-10ls %5.1f  %d   %6.1f%%     %6.1f%%     %+6.1f%%    (+-%.1f%%)%ls\n",
                        DirectionName(c.angleDeg), c.angleDeg, c.n, c.median * 100, c.predicted * 100,
                        diff * 100, noise * 100, std::fabs(diff) > 2 * noise ? L"  <- beyond noise" : L"");
            }
            wprintf(L"\n  chi-square %.1f for %d degrees of freedom (about %d expected if 4 values explain everything)\n",
                    chi2, used - 4, used - 4);
            return 0;
        }
        if (is(L"selftest3d")) {
            // Hidden: a simulated player with a known bias plays the whole 3D test off-screen.
            // Optional argument: folder to save a few rendered frames into.
            CalOptions opt;
            opt.dpi = 1600;
            opt.countsPerDegree = 1.0 / (0.125 * 0.07);   // Valorant 0.125
            opt.fov = 103;
            opt.label = L"Valorant 0.125 (simulated player)";
            const double gains[4] = { 1.10, 0.97, 0.95, 1.06 };   // R L U D
            const double tilt = 2.0;
            std::vector<CalTrial> trials;
            const ULONGLONG t0 = GetTickCount64();
            const unsigned seed = arg3.empty() ? 1u : unsigned(_wtoi(arg3.c_str()));
            if (!arg4.empty()) opt.stopUncertainty = _wtof(arg4.c_str());
            const bool done = RunCalibration3DBot(opt, gains, tilt, trials, arg == L"-" ? L"" : arg, seed);
            const DWORD ms = DWORD(GetTickCount64() - t0);
            int practice = 0;
            for (auto& t : trials) practice += t.practice ? 1 : 0;
            wprintf(L"\n%ls: %zu clicks (%d warm-up + %zu measured), %lu ms wall time\n",
                    done ? L"finished" : L"DID NOT FINISH", trials.size(), practice, trials.size() - practice, ms);
            const CalResult r = FitCalibration(trials, opt.directions);
            PrintCalResult(r);
            if (!r.ok) return 1;
            int fails = 0;
            auto check = [&](const wchar_t* what, double got, double want, double tol) {
                const bool ok = std::fabs(got - want) <= tol;
                wprintf(L"  [%ls] %-32ls got %7.3f  want %7.3f\n", ok ? L"ok  " : L"FAIL", what, got, want);
                if (!ok) ++fails;
            };
            wprintf(L"\nAfter correction every direction should be equal (simulated bias R1.10 L0.97 U0.95 D1.06, tilt +2):\n");
            check(L"Right/Left", r.right * gains[0] / (r.left * gains[1]), 1.0, 0.06);
            check(L"Up/Down", r.up * gains[2] / (r.down * gains[3]), 1.0, 0.06);
            check(L"Right/Up", r.right * gains[0] / (r.up * gains[2]), 1.0, 0.06);
            check(L"Tilt (deg)", r.rawRotationDeg, tilt, 1.0);
            check(L"Clicks <= 4 + 128", double(trials.size()), 132, 132);
            wprintf(fails ? L"\n%d check(s) FAILED\n" : L"\nAll checks passed\n", fails);
            return fails || !done ? 1 : 0;
        }
        if (is(L"test3d")) {
            // Hidden: opens the 3D test with the config's game, changes nothing, saves nothing.
            Config cfg;
            if (!Load(config, cfg)) return 1;
            CalOptions opt;
            opt.dpi = cfg.mouseDpi > 0 ? cfg.mouseDpi : 800;
            opt.countsPerDegree = CountsPerDegree(cfg, LoadGames(ExeDirectory()), opt.dpi, opt.label);
            opt.fov = cfg.fov;
        opt.sound = cfg.sound;
        opt.volume = cfg.volume;
            std::vector<CalTrial> trials;
            bool done = RunCalibrationWindow(opt, trials);
            wprintf(L"%ls, %zu flicks\n", done ? L"finished" : L"cancelled", trials.size());
            return 0;
        }
        if (is(L"analyze"))   return CmdAnalyze(arg);
        if (is(L"selftest"))  return RunCalibrationSelfTest();
        if (is(L"convert"))   return CmdConvert(arg, arg3, arg4, kDefaultConfigPath);
        if (is(L"games")) {
            wprintf(L"Yaw = degrees per mouse count at sensitivity 1. Add your own in games.ini next to oddpi.exe.\n\n");
            PrintGameList(LoadGames(ExeDirectory()));
            return 0;
        }
        if (is(L"status"))  return CmdStatus();
        if (is(L"restore")) return CmdRestore(arg);
        if (is(L"backups")) return CmdBackups();
        if (is(L"help") || is(L"--help") || is(L"-h") || is(L"/?")) {
            PrintUsage();
            return 0;
        }

        fwprintf(stderr, L"Unknown command '%ls'.\n\n", cmd.c_str());
        PrintUsage();
        return 1;
    }
    catch (const ra::install_error&) {
        fwprintf(stderr, L"error: Raw Accel is not installed (or its driver is not running).\n"
                         L"       Install it from https://github.com/RawAccelOfficial/rawaccel and reboot.\n");
    }
    catch (const std::exception& e) {
        fwprintf(stderr, L"error: %ls\n", Widen(e.what()).c_str());
    }
    return 1;
}
