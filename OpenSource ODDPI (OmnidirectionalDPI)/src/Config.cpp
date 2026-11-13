#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Config.h"

#include <cmath>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <utility>

namespace oddpi {

namespace {

    const wchar_t kMissing[] = L"\x01ODDPI_MISSING";

    struct KnownSection {
        const wchar_t* name;
        std::initializer_list<const wchar_t*> keys;
    };

    const KnownSection kKnownSections[] = {
        { L"General",     { L"Sensitivity", L"Rotation", L"AngleSnap", L"MouseDPI" } },
        { L"Directional", { L"Right", L"Left", L"Up", L"Down", L"BlendHorizontal", L"BlendVertical" } },
        { L"Dynamic",     { L"Mode", L"Gain", L"Acceleration", L"Exponent", L"InputOffset",
                            L"OutputOffset", L"DecayRate", L"Limit", L"Scale", L"PowerExponent",
                            L"Gamma", L"Motivity", L"SyncSpeed", L"Smooth", L"CapMode",
                            L"CapInput", L"CapOutput", L"HorizontalStrength", L"VerticalStrength" } },
        { L"Game",        { L"Game", L"Sensitivity", L"Cm360", L"FOV" } },
        { L"Trainer",     { L"Sound", L"Volume" } },
    };

    // Strips inline comments (; or #) and surrounding whitespace.
    std::wstring Clean(std::wstring s)
    {
        auto cut = s.find_first_of(L";#");
        if (cut != std::wstring::npos) s.erase(cut);
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        size_t start = 0;
        while (start < s.size() && iswspace(s[start])) ++start;
        return s.substr(start);
    }

    // Reads a double-NUL-terminated list ("a\0b\0\0") as returned by the profile APIs.
    std::vector<std::wstring> SplitMulti(const std::vector<wchar_t>& buf)
    {
        std::vector<std::wstring> items;
        const wchar_t* p = buf.data();
        while (*p) {
            items.emplace_back(p);
            p += items.back().size() + 1;
        }
        return items;
    }

    std::vector<std::wstring> ReadMulti(DWORD (*fn)(const std::wstring&, wchar_t*, DWORD), const std::wstring& arg)
    {
        std::vector<wchar_t> buf(4096);
        for (;;) {
            DWORD n = fn(arg, buf.data(), DWORD(buf.size()));
            if (n < buf.size() - 2) break;
            buf.resize(buf.size() * 2);
        }
        return SplitMulti(buf);
    }

    class Reader {
    public:
        Reader(const std::wstring& path, std::vector<std::wstring>& errors)
            : path_(path), errors_(errors) {}

        void Double(const wchar_t* section, const wchar_t* key, double& value)
        {
            std::wstring s;
            if (!Get(section, key, s)) return;
            wchar_t* end = nullptr;
            double d = wcstod(s.c_str(), &end);
            if (end == s.c_str() || *end != L'\0' || !std::isfinite(d)) {
                Fail(section, key, s, L"is not a number");
                return;
            }
            value = d;
        }

        void Int(const wchar_t* section, const wchar_t* key, int& value)
        {
            std::wstring s;
            if (!Get(section, key, s)) return;
            wchar_t* end = nullptr;
            long n = wcstol(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != L'\0') {
                Fail(section, key, s, L"is not a whole number");
                return;
            }
            value = int(n);
        }

        void Bool(const wchar_t* section, const wchar_t* key, bool& value)
        {
            Choice(section, key, value, {
                { L"true", true }, { L"1", true }, { L"yes", true }, { L"on", true },
                { L"false", false }, { L"0", false }, { L"no", false }, { L"off", false } });
        }

        void String(const wchar_t* section, const wchar_t* key, std::wstring& value)
        {
            std::wstring s;
            if (Get(section, key, s)) value = s;
        }

        template <typename T>
        void Choice(const wchar_t* section, const wchar_t* key, T& value,
                    std::initializer_list<std::pair<const wchar_t*, T>> options)
        {
            std::wstring s;
            if (!Get(section, key, s)) return;
            for (auto& [name, v] : options) {
                if (_wcsicmp(s.c_str(), name) == 0) {
                    value = v;
                    return;
                }
            }
            std::wstring allowed;
            for (auto& opt : options) {
                if (!allowed.empty()) allowed += L", ";
                allowed += opt.first;
            }
            Fail(section, key, s, (L"is not one of: " + allowed).c_str());
        }

    private:
        bool Get(const wchar_t* section, const wchar_t* key, std::wstring& out)
        {
            wchar_t buf[512];
            GetPrivateProfileStringW(section, key, kMissing, buf, 512, path_.c_str());
            if (wcscmp(buf, kMissing) == 0) return false;
            out = Clean(buf);
            return !out.empty();  // "Key=" (empty) keeps the default
        }

        void Fail(const wchar_t* section, const wchar_t* key, const std::wstring& value, const wchar_t* why)
        {
            errors_.push_back(L"[" + std::wstring(section) + L"] " + key + L" = '" + value + L"' " + why);
        }

        const std::wstring& path_;
        std::vector<std::wstring>& errors_;
    };

    void CheckUnknownKeys(const std::wstring& path, std::vector<std::wstring>& warnings)
    {
        auto sections = ReadMulti([](const std::wstring& p, wchar_t* b, DWORD n) {
            return GetPrivateProfileSectionNamesW(b, n, p.c_str());
        }, path);

        for (auto& section : sections) {
            const KnownSection* known = nullptr;
            for (auto& k : kKnownSections) {
                if (_wcsicmp(k.name, section.c_str()) == 0) known = &k;
            }
            if (!known) {
                warnings.push_back(L"Unknown section [" + section + L"] is ignored");
                continue;
            }

            std::vector<wchar_t> buf(32768);
            GetPrivateProfileSectionW(section.c_str(), buf.data(), DWORD(buf.size()), path.c_str());

            for (auto& line : SplitMulti(buf)) {
                auto eq = line.find(L'=');
                std::wstring key = Clean(eq == std::wstring::npos ? line : line.substr(0, eq));
                if (key.empty()) continue;

                bool found = false;
                for (auto* k : known->keys) {
                    if (_wcsicmp(k, key.c_str()) == 0) found = true;
                }
                if (!found) {
                    warnings.push_back(L"Unknown key '" + key + L"' in [" + section + L"] is ignored (typo?)");
                }
            }
        }
    }

    const char kDefaultConfig[] = R"(; ============================================================================
; ODDPI - Omnidirectional DPI for any mouse, powered by the Raw Accel driver
; ============================================================================
;   oddpi preview   show what these settings do (safe, changes nothing)
;   oddpi apply     make them live (your previous Raw Accel settings are backed up)
;   oddpi restore   undo the last apply
;
; Multipliers: 1.0 = unchanged, 2.0 = twice as fast, 0.5 = half as fast.
; "Up" means pushing the mouse away from you (cursor moves up).

[General]
; Overall sensitivity, like changing the DPI for every direction at once.
Sensitivity=1.0

; Rotate all movement, in degrees. Positive = clockwise on screen.
; Use this if a "straight" horizontal swipe comes out tilted because of your grip.
Rotation=0

; Snap almost-horizontal / almost-vertical movement to perfectly straight.
; Degrees, 0-45. 0 = off.
AngleSnap=0

; Your mouse's real DPI. Only used so Dynamic speeds are in inches/second.
; 0 = leave Raw Accel's current value alone.
MouseDPI=0

[Directional]
; Sensitivity per direction of hand movement. Each is independent.
Right=1.0
Left=1.0
Up=1.0
Down=1.0

; Smooth blend by angle: the multiplier for purely horizontal vs purely vertical
; movement, changing gradually in between (45 degrees gets the midpoint).
; Unlike Up/Down/Left/Right this never bends the direction of movement.
; Cannot be combined with a Dynamic mode (Raw Accel uses one slot for both) -
; use HorizontalStrength / VerticalStrength below instead.
BlendHorizontal=1.0
BlendVertical=1.0

[Dynamic]
; Speed-based sensitivity: Off, Classic, Natural, Jump, Power, Synchronous
; Speeds are in inches/second when MouseDPI is set (counts/ms otherwise).
Mode=Off

; Gain=true: the curve shapes how fast the cursor speeds up (recommended).
; Gain=false: the curve is the sensitivity itself ("legacy" in Raw Accel).
Gain=true

; Classic: sensitivity grows with speed.        Uses Acceleration, Exponent, InputOffset, Cap*
Acceleration=0.005
Exponent=2
InputOffset=0

; Natural: grows quickly, then levels off at Limit.   Uses DecayRate, Limit, InputOffset
DecayRate=0.1
Limit=1.5

; Jump: switches to CapOutput above speed CapInput.   Uses CapInput, CapOutput, Smooth (0-1)
Smooth=0.5

; Power: Scale * speed^PowerExponent.                 Uses Scale, PowerExponent, OutputOffset, Cap*
Scale=1
PowerExponent=0.05
OutputOffset=0

; Synchronous: sensitivity 1.0 at SyncSpeed, ranging 1/Motivity..Motivity.
; Uses SyncSpeed, Motivity, Gamma, Smooth
SyncSpeed=5
Motivity=1.5
Gamma=1

; Cap for Classic / Power, jump point for Jump.
; CapMode: Output (limit the multiplier), Input (stop growing above a speed), InputOutput (both)
CapMode=Output
CapInput=15
CapOutput=1.5

; How strongly the Dynamic curve applies to horizontal vs vertical movement
; (0 = not at all, 1 = fully). Blended smoothly by angle.
HorizontalStrength=1.0
VerticalStrength=1.0

[Game]
; The game you play and its in-game sensitivity. Not used by the driver: calibration uses it
; so test flicks are the same hand movements as in your game. 'oddpi games' lists the names.
; Game=none skips the question.
Game=
Sensitivity=0
; Or give your cm/360 directly instead (needs MouseDPI):
Cm360=0
; Horizontal field of view in degrees, so the test looks like your game.
FOV=103

[Trainer]
; Sound effects in the calibration test, sens finder and Gridshot.
Sound=true
; 0 to 1
Volume=0.6
)";

}  // namespace

bool LoadConfig(const std::wstring& path, Config& cfg,
                std::vector<std::wstring>& errors, std::vector<std::wstring>& warnings)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        errors.push_back(L"Config file not found: " + path + L"  (create one with: oddpi init)");
        return false;
    }

    // The profile APIs resolve relative paths against the Windows directory, so use an absolute path.
    std::wstring full = std::filesystem::absolute(path, ec).wstring();
    if (ec) full = path;

    Reader r(full, errors);

    r.Double(L"General", L"Sensitivity", cfg.sensitivity);
    r.Double(L"General", L"Rotation", cfg.rotation);
    r.Double(L"General", L"AngleSnap", cfg.angleSnap);
    r.Int(L"General", L"MouseDPI", cfg.mouseDpi);

    r.Double(L"Directional", L"Right", cfg.right);
    r.Double(L"Directional", L"Left", cfg.left);
    r.Double(L"Directional", L"Up", cfg.up);
    r.Double(L"Directional", L"Down", cfg.down);
    r.Double(L"Directional", L"BlendHorizontal", cfg.blendHorizontal);
    r.Double(L"Directional", L"BlendVertical", cfg.blendVertical);

    r.Choice(L"Dynamic", L"Mode", cfg.mode, {
        { L"Off", DynamicMode::Off }, { L"None", DynamicMode::Off },
        { L"Classic", DynamicMode::Classic }, { L"Natural", DynamicMode::Natural },
        { L"Jump", DynamicMode::Jump }, { L"Power", DynamicMode::Power },
        { L"Synchronous", DynamicMode::Synchronous } });
    r.Bool(L"Dynamic", L"Gain", cfg.gain);
    r.Double(L"Dynamic", L"Acceleration", cfg.acceleration);
    r.Double(L"Dynamic", L"Exponent", cfg.exponent);
    r.Double(L"Dynamic", L"InputOffset", cfg.inputOffset);
    r.Double(L"Dynamic", L"OutputOffset", cfg.outputOffset);
    r.Double(L"Dynamic", L"DecayRate", cfg.decayRate);
    r.Double(L"Dynamic", L"Limit", cfg.limit);
    r.Double(L"Dynamic", L"Scale", cfg.scale);
    r.Double(L"Dynamic", L"PowerExponent", cfg.powerExponent);
    r.Double(L"Dynamic", L"Gamma", cfg.gamma);
    r.Double(L"Dynamic", L"Motivity", cfg.motivity);
    r.Double(L"Dynamic", L"SyncSpeed", cfg.syncSpeed);
    r.Double(L"Dynamic", L"Smooth", cfg.smooth);
    r.Choice(L"Dynamic", L"CapMode", cfg.capMode, {
        { L"Output", CapMode::Output }, { L"Input", CapMode::Input },
        { L"InputOutput", CapMode::InputOutput } });
    r.Double(L"Dynamic", L"CapInput", cfg.capInput);
    r.Double(L"Dynamic", L"CapOutput", cfg.capOutput);
    r.Double(L"Dynamic", L"HorizontalStrength", cfg.horizontalStrength);
    r.Double(L"Dynamic", L"VerticalStrength", cfg.verticalStrength);

    r.String(L"Game", L"Game", cfg.game);
    r.Double(L"Game", L"Sensitivity", cfg.gameSensitivity);
    r.Double(L"Game", L"Cm360", cfg.cm360);
    r.Double(L"Game", L"FOV", cfg.fov);

    r.Bool(L"Trainer", L"Sound", cfg.sound);
    r.Double(L"Trainer", L"Volume", cfg.volume);

    CheckUnknownKeys(full, warnings);

    return errors.empty();
}

bool WriteDefaultConfig(const std::wstring& path, std::wstring& error)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        error = L"Not overwriting existing file: " + path;
        return false;
    }

    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        error = L"Could not create " + path;
        return false;
    }
    out.write(kDefaultConfig, sizeof(kDefaultConfig) - 1);
    if (!out) {
        error = L"Could not write " + path;
        return false;
    }
    return true;
}

bool SaveConfig(const std::wstring& path, const Config& c, std::wstring& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !WriteDefaultConfig(path, error)) return false;

    std::wstring full = std::filesystem::absolute(path, ec).wstring();
    if (ec) full = path;

    bool ok = true;
    auto put = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        ok = ok && WritePrivateProfileStringW(section, key, value.c_str(), full.c_str()) != FALSE;
    };
    auto num = [](double v) {
        wchar_t buf[64];
        swprintf_s(buf, L"%.6g", v);
        return std::wstring(buf);
    };

    put(L"General", L"Sensitivity", num(c.sensitivity));
    put(L"General", L"Rotation", num(c.rotation));
    put(L"General", L"AngleSnap", num(c.angleSnap));
    put(L"General", L"MouseDPI", std::to_wstring(c.mouseDpi));

    put(L"Directional", L"Right", num(c.right));
    put(L"Directional", L"Left", num(c.left));
    put(L"Directional", L"Up", num(c.up));
    put(L"Directional", L"Down", num(c.down));
    put(L"Directional", L"BlendHorizontal", num(c.blendHorizontal));
    put(L"Directional", L"BlendVertical", num(c.blendVertical));

    put(L"Dynamic", L"Mode", ModeName(c.mode));
    put(L"Dynamic", L"Gain", c.gain ? L"true" : L"false");
    put(L"Dynamic", L"Acceleration", num(c.acceleration));
    put(L"Dynamic", L"Exponent", num(c.exponent));
    put(L"Dynamic", L"InputOffset", num(c.inputOffset));
    put(L"Dynamic", L"OutputOffset", num(c.outputOffset));
    put(L"Dynamic", L"DecayRate", num(c.decayRate));
    put(L"Dynamic", L"Limit", num(c.limit));
    put(L"Dynamic", L"Scale", num(c.scale));
    put(L"Dynamic", L"PowerExponent", num(c.powerExponent));
    put(L"Dynamic", L"Gamma", num(c.gamma));
    put(L"Dynamic", L"Motivity", num(c.motivity));
    put(L"Dynamic", L"SyncSpeed", num(c.syncSpeed));
    put(L"Dynamic", L"Smooth", num(c.smooth));
    put(L"Dynamic", L"CapMode", CapModeName(c.capMode));
    put(L"Dynamic", L"CapInput", num(c.capInput));
    put(L"Dynamic", L"CapOutput", num(c.capOutput));
    put(L"Dynamic", L"HorizontalStrength", num(c.horizontalStrength));
    put(L"Dynamic", L"VerticalStrength", num(c.verticalStrength));

    put(L"Game", L"Game", c.game);
    put(L"Game", L"Sensitivity", num(c.gameSensitivity));
    put(L"Game", L"Cm360", num(c.cm360));
    put(L"Game", L"FOV", num(c.fov));

    put(L"Trainer", L"Sound", c.sound ? L"true" : L"false");
    put(L"Trainer", L"Volume", num(c.volume));

    if (!ok) error = L"Could not write " + full;
    return ok;
}

const wchar_t* CapModeName(CapMode mode)
{
    switch (mode) {
    case CapMode::Input:       return L"Input";
    case CapMode::InputOutput: return L"InputOutput";
    default:                   return L"Output";
    }
}

const wchar_t* ModeName(DynamicMode mode)
{
    switch (mode) {
    case DynamicMode::Classic:     return L"Classic";
    case DynamicMode::Natural:     return L"Natural";
    case DynamicMode::Jump:        return L"Jump";
    case DynamicMode::Power:       return L"Power";
    case DynamicMode::Synchronous: return L"Synchronous";
    default:                       return L"Off";
    }
}

}  // namespace oddpi
