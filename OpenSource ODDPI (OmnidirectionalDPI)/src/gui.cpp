// ODDPI settings window (oddpi-gui.exe).
//
// Same engine as the command line: edits oddpi.ini next to the exe and drives the signed
// Raw Accel driver. Nothing here touches mouse input except the calibration test, which only
// reads raw input while its own window is open.
//
// Look: follows the Windows dark/light app setting. Cards, fields, sliders, buttons and the
// toggle are drawn by us (GDI+ for anti-aliased shapes); text inputs and lists are standard
// controls coloured to match.

#include "RawAccel.h"   // must come before <Windows.h>
#include "Actions.h"
#include "Calibrate.h"
#include "Config.h"
#include "Games.h"
#include "Gridshot.h"
#include "ProfileBuilder.h"

#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }  // GDI+ headers need these with NOMINMAX
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cmath>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace oddpi;
using Gdiplus::REAL;

namespace {

    constexpr double kPi = 3.14159265358979323846;

    // ------------------------------------------------------------------------------------------
    // Theme
    // ------------------------------------------------------------------------------------------

    struct Theme {
        bool dark = true;
        COLORREF bg{}, card{}, cardBorder{}, field{}, fieldBorder{}, text{}, dim{}, accent{}, accentHover{}, accentPressed{},
                 accentText{}, ok{}, warn{}, danger{}, track{}, grid{}, gridStrong{}, secondary{}, secondaryHover{};
    };

    Theme T;
    HBRUSH g_brBg = nullptr, g_brCard = nullptr, g_brField = nullptr;

    bool SystemUsesDarkApps()
    {
        // ODDPI_THEME=light|dark overrides the Windows setting.
        wchar_t force[16] = {};
        if (GetEnvironmentVariableW(L"ODDPI_THEME", force, 16)) {
            if (_wcsicmp(force, L"light") == 0) return false;
            if (_wcsicmp(force, L"dark") == 0) return true;
        }
        DWORD value = 1, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value == 0;
    }

    void MakeTheme(bool dark)
    {
        T.dark = dark;
        if (dark) {
            T.bg = RGB(17, 19, 24);        T.card = RGB(26, 29, 36);      T.cardBorder = RGB(40, 44, 54);
            T.field = RGB(35, 39, 48);     T.fieldBorder = RGB(54, 60, 73);
            T.text = RGB(232, 234, 239);   T.dim = RGB(146, 153, 167);
            T.accent = RGB(79, 140, 255);  T.accentHover = RGB(106, 160, 255); T.accentPressed = RGB(58, 118, 232);
            T.accentText = RGB(255, 255, 255);
            T.ok = RGB(52, 211, 153);      T.warn = RGB(251, 191, 36);    T.danger = RGB(248, 113, 113);
            T.track = RGB(55, 61, 74);     T.grid = RGB(45, 50, 61);      T.gridStrong = RGB(112, 120, 138);
            T.secondary = RGB(38, 42, 52); T.secondaryHover = RGB(48, 53, 65);
        }
        else {
            T.bg = RGB(242, 243, 246);     T.card = RGB(255, 255, 255);   T.cardBorder = RGB(225, 228, 234);
            T.field = RGB(246, 247, 250);  T.fieldBorder = RGB(208, 212, 220);
            T.text = RGB(17, 24, 39);      T.dim = RGB(105, 112, 126);
            T.accent = RGB(37, 99, 235);   T.accentHover = RGB(59, 118, 245); T.accentPressed = RGB(29, 78, 216);
            T.accentText = RGB(255, 255, 255);
            T.ok = RGB(5, 150, 105);       T.warn = RGB(217, 119, 6);     T.danger = RGB(220, 38, 38);
            T.track = RGB(214, 218, 226);  T.grid = RGB(230, 232, 237);   T.gridStrong = RGB(150, 157, 170);
            T.secondary = RGB(255, 255, 255); T.secondaryHover = RGB(240, 242, 246);
        }
        for (HBRUSH* b : { &g_brBg, &g_brCard, &g_brField }) {
            if (*b) DeleteObject(*b);
        }
        g_brBg = CreateSolidBrush(T.bg);
        g_brCard = CreateSolidBrush(T.card);
        g_brField = CreateSolidBrush(T.field);
    }

    Gdiplus::Color C(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

    void RoundPath(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float rad)
    {
        rad = std::min(rad, std::min(r.Width, r.Height) / 2);
        const float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
        p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
        p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
        p.CloseFigure();
    }

    void FillRound(Gdiplus::Graphics& gr, const RECT& rc, float rad, COLORREF fill, COLORREF border, float borderW = 1.0f)
    {
        Gdiplus::RectF r(REAL(rc.left) + 0.5f, REAL(rc.top) + 0.5f, REAL(rc.right - rc.left) - 1.0f, REAL(rc.bottom - rc.top) - 1.0f);
        Gdiplus::GraphicsPath path;
        RoundPath(path, r, rad);
        Gdiplus::SolidBrush b(C(fill));
        gr.FillPath(&b, &path);
        if (borderW > 0) {
            Gdiplus::Pen pen(C(border), borderW);
            gr.DrawPath(&pen, &path);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Control model
    // ------------------------------------------------------------------------------------------

    struct SliderRow {
        const wchar_t* label;
        double Config::* field;
        double min, max;
        int decimals;
        HWND hLabel = nullptr, hTrack = nullptr, hEdit = nullptr;
        bool bad = false;   // text in the box is not a number
    };

    enum ModeBit : unsigned {
        kClassic = 1u << int(DynamicMode::Classic),
        kNatural = 1u << int(DynamicMode::Natural),
        kJump = 1u << int(DynamicMode::Jump),
        kPower = 1u << int(DynamicMode::Power),
        kSync = 1u << int(DynamicMode::Synchronous),
        kAnyDynamic = kClassic | kNatural | kJump | kPower | kSync,
    };

    struct DynRow {
        const wchar_t* label;
        double Config::* field;   // nullptr = the CapMode combo
        unsigned modes;
        HWND hLabel = nullptr, hCtl = nullptr;
        bool bad = false;
    };

    enum Id : int {
        kIdGeneralBase = 1000,
        kIdDirectionalBase = 1100,
        kIdDynBase = 1200,
        kIdDpi = 1300,
        kIdMode = 1400,
        kIdGain,
        kIdGame = 1500,
        kIdGameSens,
        kIdPower = 1600,
        kIdApply = 2000,
        kIdCalibrate,
        kIdUndo,
        kIdOriginal,
        kIdReload,
        kIdConvert,
        kIdSensFinder,
        kIdGridshot,
    };
    constexpr int kButtonCount = 8;

    enum class ButtonKind { Primary, Accent, Secondary };

    struct Card {
        RECT rc;
        std::wstring title, subtitle;
    };

    struct FieldBox {
        HWND edit;
        RECT rc;
    };

    struct App {
        HINSTANCE inst = nullptr;
        HWND hwnd = nullptr;
        HWND diagram = nullptr;
        HWND status = nullptr;
        HWND dpiLabel = nullptr, dpiEdit = nullptr, dpiHint = nullptr;
        HWND modeLabel = nullptr, modeCombo = nullptr, gainToggle = nullptr, dynOffText = nullptr;
        HWND gameLabel = nullptr, gameCombo = nullptr, gameSensEdit = nullptr, gameInfo = nullptr;
        bool gameSensBad = false;
        std::vector<GameInfo> games;   // combo: 0 = not set, 1..n = games, n+1 = "cm/360"
        HWND buttons[kButtonCount] = {};
        HWND powerToggle = nullptr;    // ODDPI on/off switch in the header
        HWND hot = nullptr;            // control under the mouse (buttons, toggle)
        HWND focusedEdit = nullptr;
        HFONT font = nullptr, fontSemi = nullptr, fontTitle = nullptr, fontSmall = nullptr;
        int fontH = 16;
        UINT dpi = 96;

        std::vector<Card> cards;
        std::vector<FieldBox> fields;

        std::wstring iniPath;
        Config cfg;
        bool dirty = false;
        bool syncing = false;
        bool dpiBad = false;
        int driverDpi = 0;

        // Driver state for the header pills
        std::wstring raVersion;        // empty = not available
        std::wstring raError;
        bool oddpiActive = false;
        std::vector<std::wstring> notes;

        std::wstring message;          // last action result
        std::vector<std::wstring> loadWarnings;

        // Preview of the edited (not yet applied) settings
        std::unique_ptr<PreviewModel> preview;
        std::vector<std::wstring> previewErrors;
        int previewDpi = 0;

        std::vector<SliderRow> general = {
            { L"Sensitivity", &Config::sensitivity, 0.10, 3.00, 2 },
            { L"Rotation (\x00B0)", &Config::rotation, -30.0, 30.0, 1 },
            { L"Angle snap (\x00B0)", &Config::angleSnap, 0.0, 45.0, 1 },
        };
        std::vector<SliderRow> directional = {
            { L"Right", &Config::right, 0.50, 1.50, 3 },
            { L"Left", &Config::left, 0.50, 1.50, 3 },
            { L"Up", &Config::up, 0.50, 1.50, 3 },
            { L"Down", &Config::down, 0.50, 1.50, 3 },
            { L"Blend horizontal", &Config::blendHorizontal, 0.50, 1.50, 3 },
            { L"Blend vertical", &Config::blendVertical, 0.50, 1.50, 3 },
        };
        std::vector<DynRow> dynamic = {
            { L"Acceleration", &Config::acceleration, kClassic },
            { L"Exponent", &Config::exponent, kClassic },
            { L"Decay rate", &Config::decayRate, kNatural },
            { L"Limit", &Config::limit, kNatural },
            { L"Scale", &Config::scale, kPower },
            { L"Power exponent", &Config::powerExponent, kPower },
            { L"Sync speed", &Config::syncSpeed, kSync },
            { L"Motivity", &Config::motivity, kSync },
            { L"Gamma", &Config::gamma, kSync },
            { L"Input offset", &Config::inputOffset, kClassic | kNatural },
            { L"Output offset", &Config::outputOffset, kPower },
            { L"Smooth", &Config::smooth, kJump | kSync },
            { L"Cap mode", nullptr, kClassic | kPower },
            { L"Cap input", &Config::capInput, kClassic | kJump | kPower },
            { L"Cap output", &Config::capOutput, kClassic | kJump | kPower },
            { L"Horizontal strength", &Config::horizontalStrength, kAnyDynamic },
            { L"Vertical strength", &Config::verticalStrength, kAnyDynamic },
        };
    };

    App g;

    int S(int v) { return MulDiv(v, int(g.dpi), 96); }

    // Logical layout at 96 DPI, scaled by S().
    constexpr int kClientW = 1100, kClientH = 712;
    constexpr int kHeaderH = 60;
    constexpr int kPowerW = 132;   // header on/off switch

    std::wstring Format(double v, int decimals)
    {
        wchar_t buf[64];
        if (decimals < 0) swprintf_s(buf, L"%g", v);
        else swprintf_s(buf, L"%.*f", decimals, v);
        return buf;
    }

    std::wstring GetText(HWND h)
    {
        int n = GetWindowTextLengthW(h);
        std::wstring s(size_t(n) + 1, L'\0');
        GetWindowTextW(h, s.data(), n + 1);
        s.resize(size_t(n));
        return s;
    }

    bool ParseDouble(const std::wstring& s, double& out)
    {
        const wchar_t* p = s.c_str();
        wchar_t* end = nullptr;
        double v = wcstod(p, &end);
        if (end == p) return false;
        while (*end && iswspace(*end)) ++end;
        if (*end || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

    std::wstring DescribeError(const std::exception& e)
    {
        if (dynamic_cast<const ra::install_error*>(&e)) {
            return L"Raw Accel is not installed (or its driver is not running). Install it from "
                   L"github.com/RawAccelOfficial/rawaccel and reboot.";
        }
        const char* m = e.what();
        int n = MultiByteToWideChar(CP_ACP, 0, m, -1, nullptr, 0);
        std::wstring w(size_t(std::max(n, 1)), L'\0');
        MultiByteToWideChar(CP_ACP, 0, m, -1, w.data(), n);
        w.resize(wcslen(w.c_str()));
        return w;
    }

    bool IsBadEdit(HWND h)
    {
        for (auto* rows : { &g.general, &g.directional }) {
            for (auto& r : *rows) if (r.hEdit == h) return r.bad;
        }
        for (auto& r : g.dynamic) if (r.hCtl == h) return r.bad;
        if (h == g.dpiEdit) return g.dpiBad;
        if (h == g.gameSensEdit) return g.gameSensBad;
        return false;
    }

    void InvalidateHeader()
    {
        RECT r = { 0, 0, S(kClientW), S(kHeaderH) };
        InvalidateRect(g.hwnd, &r, FALSE);
    }

    void InvalidateField(HWND edit)
    {
        for (auto& f : g.fields) {
            if (f.edit != edit) continue;
            RECT r = f.rc;
            InflateRect(&r, 2, 2);
            InvalidateRect(g.hwnd, &r, FALSE);
        }
    }

    // ------------------------------------------------------------------------------------------
    // State <-> controls
    // ------------------------------------------------------------------------------------------

    void UpdateTitle()
    {
        SetWindowTextW(g.hwnd, g.dirty ? L"ODDPI \x2014 changes not applied yet" : L"ODDPI");
        InvalidateHeader();
    }

    void RefreshDriverState()
    {
        g.raVersion.clear();
        g.raError.clear();
        g.oddpiActive = false;
        g.notes.clear();
        try {
            const ra::version_t v = DriverVersion();
            const Snapshot s = ReadDriver();
            g.driverDpi = s.Base().default_dev_cfg.dpi;
            wchar_t buf[64];
            swprintf_s(buf, L"Raw Accel %d.%d.%d", v.major, v.minor, v.patch);
            g.raVersion = buf;
            g.oddpiActive = IsOddpiActive(s);
            g.notes = CoverageNotes(s, EffectiveDpi(g.cfg, g.driverDpi));
        }
        catch (const std::exception& e) {
            g.raError = DescribeError(e);
        }
        InvalidateHeader();
        if (g.powerToggle) InvalidateRect(g.powerToggle, nullptr, FALSE);
    }

    void UpdateStatus()
    {
        std::wstring s;
        if (!g.message.empty()) s = g.message;
        else if (!g.raError.empty()) s = g.raError;
        else if (g.dirty) s = L"You have changes that are not applied yet. Press Apply to save and use them.";
        else if (g.oddpiActive) s = L"Your settings are active.";
        else s = L"ODDPI is off \x2014 your mouse is not changed by it. Turn it on with the switch at the top right.";

        for (auto& n : g.notes) s += L"\r\nNote: " + n;
        for (auto& w : g.loadWarnings) s += L"\r\nConfig: " + w;

        std::vector<std::wstring> bad;
        for (auto* rows : { &g.general, &g.directional }) {
            for (auto& r : *rows) if (r.bad) bad.push_back(r.label);
        }
        for (auto& r : g.dynamic) if (r.bad) bad.push_back(r.label);
        if (g.dpiBad) bad.push_back(L"Mouse DPI");
        if (g.gameSensBad) bad.push_back(L"In-game sensitivity");
        for (auto& b : bad) s += L"\r\n'" + b + L"' is not a valid number (the last valid value is used).";

        // Typed values beyond the slider are allowed, but are usually a typo.
        for (auto* rows : { &g.general, &g.directional }) {
            for (auto& r : *rows) {
                const double v = g.cfg.*r.field;
                if (r.bad || (v >= r.min - 1e-9 && v <= r.max + 1e-9)) continue;
                wchar_t buf[160];
                swprintf_s(buf, L"\r\n%ls = %g is outside the usual range (%g to %g). Intended?", r.label, v, r.min, r.max);
                s += buf;
            }
        }

        SetWindowTextW(g.status, s.c_str());
    }

    void RebuildPreview()
    {
        g.previewErrors.clear();
        g.preview.reset();
        g.previewDpi = EffectiveDpi(g.cfg, g.driverDpi);
        auto profile = std::make_unique<ra::profile>();
        if (BuildProfile(g.cfg, g.previewDpi, *profile, g.previewErrors)) {
            g.preview = std::make_unique<PreviewModel>(*profile, g.previewDpi);
        }
        InvalidateRect(g.diagram, nullptr, FALSE);
    }

    void SetSliderRow(SliderRow& r)
    {
        const double v = g.cfg.*r.field;
        const double scale = std::pow(10.0, r.decimals);
        SendMessageW(r.hTrack, TBM_SETPOS, TRUE, LPARAM(std::lround(std::clamp(v, r.min, r.max) * scale)));
        SetWindowTextW(r.hEdit, Format(v, r.decimals).c_str());
        r.bad = false;
    }

    void Layout();  // forward

    int EffectiveMouseDpi() { return g.cfg.mouseDpi > 0 ? g.cfg.mouseDpi : g.driverDpi; }
    int CmComboIndex() { return int(g.games.size()) + 1; }

    int GameComboSelection()
    {
        return int(SendMessageW(g.gameCombo, CB_GETCURSEL, 0, 0));
    }

    void UpdateGameInfo()
    {
        std::wstring text;
        const int sel = GameComboSelection();
        if (sel >= 1 && sel <= int(g.games.size()) && g.cfg.gameSensitivity > 0) {
            const double cm = Cm360(g.cfg.gameSensitivity, g.games[size_t(sel - 1)].yaw, EffectiveMouseDpi());
            wchar_t buf[64];
            if (cm > 0) swprintf_s(buf, L"%.1f cm/360", cm);
            else swprintf_s(buf, L"needs DPI");
            text = buf;
        }
        else if (sel == CmComboIndex()) text = L"cm/360 \x2192";
        else if (sel >= 1) text = L"sens \x2192";
        SetWindowTextW(g.gameInfo, text.c_str());
    }

    void SyncGameRow()
    {
        int sel = 0;
        if (g.cfg.cm360 > 0) sel = CmComboIndex();
        else if (const GameInfo* gi = FindGame(g.games, g.cfg.game)) sel = int(gi - g.games.data()) + 1;
        SendMessageW(g.gameCombo, CB_SETCURSEL, WPARAM(sel), 0);

        double v = sel == CmComboIndex() ? g.cfg.cm360 : g.cfg.gameSensitivity;
        SetWindowTextW(g.gameSensEdit, v > 0 && sel > 0 ? Format(v, -1).c_str() : L"");
        EnableWindow(g.gameSensEdit, sel > 0);
        g.gameSensBad = false;
        UpdateGameInfo();
    }

    void ShowDynamicRows()
    {
        const unsigned bit = 1u << int(g.cfg.mode);
        const bool off = g.cfg.mode == DynamicMode::Off;
        for (auto& r : g.dynamic) {
            const int show = (!off && (r.modes & bit)) ? SW_SHOW : SW_HIDE;
            ShowWindow(r.hLabel, show);
            ShowWindow(r.hCtl, show);
        }
        ShowWindow(g.gainToggle, off ? SW_HIDE : SW_SHOW);
        ShowWindow(g.dynOffText, off ? SW_SHOW : SW_HIDE);
        Layout();
    }

    void SyncControls()
    {
        g.syncing = true;
        for (auto& r : g.general) SetSliderRow(r);
        for (auto& r : g.directional) SetSliderRow(r);
        SetWindowTextW(g.dpiEdit, std::to_wstring(g.cfg.mouseDpi).c_str());
        g.dpiBad = false;
        SyncGameRow();
        SendMessageW(g.modeCombo, CB_SETCURSEL, WPARAM(int(g.cfg.mode)), 0);
        InvalidateRect(g.gainToggle, nullptr, FALSE);
        for (auto& r : g.dynamic) {
            if (r.field) SetWindowTextW(r.hCtl, Format(g.cfg.*r.field, -1).c_str());
            else SendMessageW(r.hCtl, CB_SETCURSEL, WPARAM(int(g.cfg.capMode)), 0);
            r.bad = false;
        }
        g.syncing = false;
        ShowDynamicRows();
        RebuildPreview();
        UpdateStatus();
    }

    void Changed()
    {
        g.dirty = true;
        g.message.clear();
        UpdateGameInfo();
        UpdateTitle();
        RebuildPreview();
        UpdateStatus();
    }

    bool LoadIni()
    {
        Config cfg;
        std::vector<std::wstring> errors;
        g.loadWarnings.clear();
        if (!LoadConfig(g.iniPath, cfg, errors, g.loadWarnings)) {
            std::wstring text = L"Could not read " + g.iniPath + L":\n";
            for (auto& e : errors) text += L"\n" + e;
            MessageBoxW(g.hwnd, text.c_str(), L"ODDPI", MB_ICONWARNING);
            return false;
        }
        g.cfg = cfg;
        g.dirty = false;
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Actions
    // ------------------------------------------------------------------------------------------

    bool DoApply()
    {
        std::wstring error;
        if (!SaveConfig(g.iniPath, g.cfg, error)) {
            MessageBoxW(g.hwnd, error.c_str(), L"ODDPI", MB_ICONERROR);
            return false;
        }
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        bool ok = false;
        try {
            ApplyOutcome outcome;
            std::vector<std::wstring> errors;
            ok = ApplyConfig(g.cfg, outcome, errors);
            if (ok) {
                g.dirty = false;
                g.message = L"Applied and saved to " + g.iniPath + L".";
            }
            else {
                std::wstring text = L"These settings can't be applied:\n";
                for (auto& e : errors) text += L"\n" + e;
                MessageBoxW(g.hwnd, text.c_str(), L"ODDPI", MB_ICONWARNING);
            }
        }
        catch (const std::exception& e) {
            MessageBoxW(g.hwnd, DescribeError(e).c_str(), L"ODDPI", MB_ICONERROR);
        }
        SetCursor(old);
        RefreshDriverState();
        UpdateTitle();
        UpdateStatus();
        return ok;
    }

    void DoPower()
    {
        if (!g.raError.empty()) {
            MessageBoxW(g.hwnd, g.raError.c_str(), L"ODDPI", MB_ICONWARNING);
            return;
        }
        if (!g.oddpiActive) {
            if (DoApply()) g.message = L"ODDPI is on.";
            UpdateStatus();
            return;
        }
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        try {
            std::wstring backup;
            DisableOddpi(backup);
            g.message = L"ODDPI is off \x2014 your mouse is not changed by it. Flip the switch to turn it back on.";
        }
        catch (const std::exception& e) {
            MessageBoxW(g.hwnd, DescribeError(e).c_str(), L"ODDPI", MB_ICONERROR);
        }
        SetCursor(old);
        RefreshDriverState();
        UpdateStatus();
    }

    void DoRestore(bool original)
    {
        if (original && MessageBoxW(g.hwnd,
                L"Put Raw Accel back exactly as it was before ODDPI was first used?\n\n"
                L"Your oddpi.ini is kept; press Apply to use it again.",
                L"ODDPI", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return;
        }
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        try {
            bool empty = false;
            RestoreBackup(original ? L"original" : L"", empty);
            g.message = original ? L"Restored the settings from before ODDPI was first used."
                                 : L"Undid the last apply (Raw Accel is back to the state before it).";
        }
        catch (const std::exception& e) {
            MessageBoxW(g.hwnd, DescribeError(e).c_str(), L"ODDPI", MB_ICONERROR);
        }
        SetCursor(old);
        RefreshDriverState();
        UpdateStatus();
    }

    void DoCalibrate()
    {
        if (g.cfg.mouseDpi <= 0) {
            MessageBoxW(g.hwnd,
                L"Type your mouse's DPI in the Mouse DPI box first (the DPI set in the mouse or its "
                L"software, e.g. 800 or 1600). The test uses it to make flicks a real distance.",
                L"ODDPI calibration", MB_ICONINFORMATION);
            SetFocus(g.dpiEdit);
            SendMessageW(g.dpiEdit, EM_SETSEL, 0, -1);
            return;
        }
        CalOptions opt;
        opt.dpi = g.cfg.mouseDpi;
        opt.countsPerDegree = CountsPerDegree(g.cfg, g.games, opt.dpi, opt.label);
        opt.fov = g.cfg.fov;
        opt.sound = g.cfg.sound;
        opt.volume = g.cfg.volume;
        if (opt.countsPerDegree <= 0) {
            const int answer = MessageBoxW(g.hwnd,
                L"Set the game you play and its in-game sensitivity first? Then every test flick is the "
                L"same hand movement as in your game, which gives a more accurate result.\n\n"
                L"Yes = set it now (Game row)\nNo = test with standard flicks up to 5 cm",
                L"ODDPI calibration", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDYES) {
                SetFocus(g.gameCombo);
                SendMessageW(g.gameCombo, CB_SHOWDROPDOWN, TRUE, 0);
                return;
            }
            if (answer != IDNO) return;
        }

        std::wstring intro =
            L"A short 3D aim test: the mouse turns the camera like in your game. Flick to each red target "
            L"in one quick motion and click. It stops by itself once the result is precise (usually 50-100 "
            L"targets, about a minute). Esc cancels.\n\n";
        if (opt.countsPerDegree > 0) intro += L"Matching your game: " + opt.label + L".\n\n";
        intro += L"Your current settings are applied first, so the test measures exactly how your "
                 L"mouse feels now.";
        if (MessageBoxW(g.hwnd, intro.c_str(), L"ODDPI calibration", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            return;
        }
        if (!DoApply()) return;

        std::vector<CalTrial> trials;
        const bool done = RunCalibrationWindow(opt, trials);
        SetForegroundWindow(g.hwnd);
        if (!done) {
            g.message = L"Calibration cancelled; nothing was changed.";
            UpdateStatus();
            return;
        }

        const std::wstring log = SaveCalibrationLog(opt, trials);
        const CalResult r = FitCalibration(trials, opt.directions);
        if (!r.ok) {
            MessageBoxW(g.hwnd, r.error.c_str(), L"ODDPI calibration", MB_ICONWARNING);
            return;
        }

        const Config next = WithCalibration(g.cfg, r);
        wchar_t buf[2048];
        swprintf_s(buf,
            L"Usable flicks: %d of %d\n"
            L"Your directions differ by %.1f%%; after this correction about %.1f%%.\n"
            L"Grip tilt: %+.1f\x00B0%ls\n\n"
            L"Setting        now      \x2192  new        (measured \x00B1 noise)\n"
            L"Right        %6.3f  \x2192  %6.3f     (x%.3f \x00B1 %.3f)\n"
            L"Left         %6.3f  \x2192  %6.3f     (x%.3f \x00B1 %.3f)\n"
            L"Up           %6.3f  \x2192  %6.3f     (x%.3f \x00B1 %.3f)\n"
            L"Down         %6.3f  \x2192  %6.3f     (x%.3f \x00B1 %.3f)\n"
            L"Rotation     %6.1f  \x2192  %6.1f\n\n"
            L"%ls"
            L"Apply the new values?",
            r.validTrials, r.totalTrials, r.spreadBefore, r.spreadAfterFit,
            r.rawRotationDeg, r.rotationDeg == 0 ? L" (under 1\x00B0, ignored)" : L"",
            g.cfg.right, next.right, r.raw[0], r.uncertainty[0],
            g.cfg.left, next.left, r.raw[1], r.uncertainty[1],
            g.cfg.up, next.up, r.raw[2], r.uncertainty[2],
            g.cfg.down, next.down, r.raw[3], r.uncertainty[3],
            g.cfg.rotation, next.rotation,
            (std::fabs(next.right / g.cfg.right - 1) < 0.02 && std::fabs(next.left / g.cfg.left - 1) < 0.02 &&
             std::fabs(next.up / g.cfg.up - 1) < 0.02 && std::fabs(next.down / g.cfg.down - 1) < 0.02)
                ? L"All changes are under 2% \x2014 your directions are already even.\n\n" : L"");

        if (MessageBoxW(g.hwnd, buf, L"ODDPI calibration results", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            g.message = L"Calibration finished; results not applied." +
                        (log.empty() ? std::wstring() : L" Flick log: " + log);
            UpdateStatus();
            return;
        }
        g.cfg = next;
        SyncControls();
        if (DoApply()) {
            g.message = L"Calibration applied." + (log.empty() ? std::wstring() : L" Flick log: " + log);
        }
        UpdateTitle();
        UpdateStatus();
    }

    void DoConvert()
    {
        const int sel = GameComboSelection();
        if (sel < 1 || sel > int(g.games.size()) || !(g.cfg.gameSensitivity > 0)) {
            MessageBoxW(g.hwnd, L"Pick your game in the Game row and type your in-game sensitivity next to it. "
                                L"This shows the same sensitivity in every other game.",
                        L"ODDPI sens converter", MB_ICONINFORMATION);
            SetFocus(g.gameCombo);
            return;
        }
        const GameInfo& a = g.games[size_t(sel - 1)];
        const double sens = g.cfg.gameSensitivity;
        wchar_t line[256];
        std::wstring text;
        const double cm = Cm360(sens, a.yaw, EffectiveMouseDpi());
        if (cm > 0) swprintf_s(line, L"%ls %g at %d DPI = %.2f cm/360\n\n", a.name.c_str(), sens, EffectiveMouseDpi(), cm);
        else swprintf_s(line, L"%ls %g (set Mouse DPI to see cm/360)\n\n", a.name.c_str(), sens);
        text += line;
        text += L"Same 360 distance in:\n";
        for (const auto& b : g.games) {
            if (&b == &a) continue;
            swprintf_s(line, L"%ls:   %.4g%ls\n", b.name.c_str(), sens * a.yaw / b.yaw, b.estimate ? L"   (estimate)" : L"");
            text += line;
        }
        text += L"\nMatches 360 distance only; zoom/ADS and FOV-dependent scaling are not included. "
                L"Add games in games.ini next to oddpi-gui.exe.";
        MessageBoxW(g.hwnd, text.c_str(), L"ODDPI sens converter", MB_ICONINFORMATION);
    }

    // 60-second Gridshot ("test your aim"); every run goes on the improvement board.
    void DoGridshot()
    {
        if (g.cfg.mouseDpi <= 0) {
            MessageBoxW(g.hwnd, L"Type your mouse's DPI in the Mouse DPI box first.", L"ODDPI Gridshot", MB_ICONINFORMATION);
            SetFocus(g.dpiEdit);
            return;
        }
        if (g.dirty && !DoApply()) return;   // play with the settings shown here

        GridshotOptions go;
        go.cal.dpi = g.cfg.mouseDpi;
        go.cal.countsPerDegree = CountsPerDegree(g.cfg, g.games, go.cal.dpi, go.cal.label);
        go.cal.fov = g.cfg.fov;
        go.cal.sound = g.cfg.sound;
        go.cal.volume = g.cfg.volume;
        const int sel = GameComboSelection();
        if (sel >= 1 && sel <= int(g.games.size()) && g.cfg.gameSensitivity > 0) {
            go.gameName = g.games[size_t(sel - 1)].name;
            go.gameSens = g.cfg.gameSensitivity;
        }
        go.oddpiOn = g.oddpiActive;
        wchar_t settings[96];
        swprintf_s(settings, L"R%.2f L%.2f U%.2f D%.2f", g.cfg.right, g.cfg.left, g.cfg.up, g.cfg.down);
        go.settings = settings;

        GridshotResult r;
        const bool done = RunGridshot(go, r);
        SetForegroundWindow(g.hwnd);
        if (!done) {
            g.message = L"Gridshot closed before a run finished.";
            UpdateStatus();
            return;
        }
        wchar_t msg[200];
        swprintf_s(msg, L"Gridshot: %d runs this session, last score %d%ls. Best ever %d.", r.runs, r.last.score,
                   r.personalBest ? L" (new personal best!)" : L"", r.stats.best);
        g.message = msg;
        UpdateStatus();
    }

    void DoSensFinder()
    {
        const int sel = GameComboSelection();
        const bool useCm = sel == CmComboIndex() && g.cfg.cm360 > 0;
        const bool useGame = sel >= 1 && sel <= int(g.games.size()) && g.cfg.gameSensitivity > 0;
        if (!useCm && !useGame) {
            MessageBoxW(g.hwnd, L"Pick your game in the Game row and type your in-game sensitivity next to it first. "
                                L"The sens finder starts from it and narrows down.",
                        L"ODDPI sens finder", MB_ICONINFORMATION);
            SetFocus(g.gameCombo);
            return;
        }
        if (g.cfg.mouseDpi <= 0) {
            MessageBoxW(g.hwnd, L"Type your mouse's DPI in the Mouse DPI box first.", L"ODDPI sens finder", MB_ICONINFORMATION);
            SetFocus(g.dpiEdit);
            return;
        }

        SensFinderOptions so;
        so.cal.dpi = g.cfg.mouseDpi;
        so.cal.countsPerDegree = CountsPerDegree(g.cfg, g.games, so.cal.dpi, so.cal.label);
        so.cal.fov = g.cfg.fov;
        so.cal.sound = g.cfg.sound;
        so.cal.volume = g.cfg.volume;
        if (useGame) {
            so.gameName = g.games[size_t(sel - 1)].name;
            so.gameSens = g.cfg.gameSensitivity;
        }
        const int total = 4 + so.rounds * so.targetsPerRound;
        wchar_t intro[600];
        swprintf_s(intro,
            L"Finds your best sensitivity, like KovaaK's sens finder.\n\n"
            L"Each round you play two sensitivities, \"A\" and \"B\", without knowing which is lower. Flick to each red "
            L"target and click, as fast and accurately as you can. It moves toward the one you aimed better with and "
            L"narrows the range every round.\n\n"
            L"%d rounds, about %d targets (2-3 minutes). Starts from %ls. Your ODDPI direction settings stay on, "
            L"exactly as in game. Esc cancels.",
            so.rounds, total, so.cal.label.c_str());
        if (MessageBoxW(g.hwnd, intro, L"ODDPI sens finder", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
        if (g.dirty && !DoApply()) return;   // test with the settings shown here

        SensFinderResult r;
        const bool done = RunSensFinder(so, r);
        SetForegroundWindow(g.hwnd);
        if (!done) {
            g.message = L"Sens finder cancelled; nothing was changed.";
            UpdateStatus();
            return;
        }

        const GameInfo* game = useGame ? &g.games[size_t(sel - 1)] : nullptr;
        const int dpi = EffectiveMouseDpi();
        std::wstring text;
        wchar_t line[256];
        double newValue;
        if (game) {
            swprintf_s(line, L"%.3g", g.cfg.gameSensitivity * r.multiplier);
            newValue = _wtof(line);
            swprintf_s(line, L"Your sensitivity: %ls %.3g  (%.1f cm/360)\nBefore: %g  (%.1f cm/360)\n\nRounds (lower vs higher sens, time per target incl. misses):\n",
                       game->name.c_str(), newValue, Cm360(newValue, game->yaw, dpi), g.cfg.gameSensitivity,
                       Cm360(g.cfg.gameSensitivity, game->yaw, dpi));
        }
        else {
            swprintf_s(line, L"%.3g", g.cfg.cm360 / r.multiplier);
            newValue = _wtof(line);
            swprintf_s(line, L"Your sensitivity: %.3g cm/360\nBefore: %g cm/360\n\nRounds (lower vs higher sens, time per target incl. misses):\n",
                       newValue, g.cfg.cm360);
        }
        text += line;
        for (size_t i = 0; i < r.rounds.size(); ++i) {
            const auto& rd = r.rounds[i];
            const double base = game ? g.cfg.gameSensitivity : 1.0;
            swprintf_s(line, L"  %zu.  %.3g: %.0f ms, %d/%d hits   vs   %.3g: %.0f ms, %d/%d hits   \x2192 %.3g\n", i + 1,
                       base * rd.low.multiplier, rd.low.score * 1000, rd.low.hits, rd.low.targets,
                       base * rd.high.multiplier, rd.high.score * 1000, rd.high.hits, rd.high.targets,
                       base * rd.centerAfter);
            text += line;
        }
        text += game ? L"\nSave it in ODDPI? You also need to set it in the game yourself."
                     : L"\nSave the new cm/360 in ODDPI? You also need to set it in the game yourself.";

        if (MessageBoxW(g.hwnd, text.c_str(), L"ODDPI sens finder", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            g.message = L"Sens finder finished; result not saved.";
            UpdateStatus();
            return;
        }
        if (game) g.cfg.gameSensitivity = newValue;
        else g.cfg.cm360 = newValue;
        const bool wasDirty = g.dirty;
        SyncControls();
        std::wstring error;
        if (!SaveConfig(g.iniPath, g.cfg, error)) {
            MessageBoxW(g.hwnd, error.c_str(), L"ODDPI", MB_ICONERROR);
            return;
        }
        g.dirty = wasDirty;
        swprintf_s(line, game ? L"Saved %ls sensitivity %.3g \x2014 set it in the game too." : L"Saved %ls%.3g cm/360 \x2014 set it in the game too.",
                   game ? game->name.c_str() : L"", newValue);
        g.message = line;
        UpdateTitle();
        UpdateStatus();
    }

    // ------------------------------------------------------------------------------------------
    // Diagram (inside the right card; the card draws the title)
    // ------------------------------------------------------------------------------------------

    void PaintDiagram(HDC hdc, const RECT& rc)
    {
        using namespace Gdiplus;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        Bitmap buffer(w, h, PixelFormat32bppARGB);
        Graphics gr(&buffer);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        gr.Clear(C(T.card));

        SolidBrush textBrush(C(T.text)), dimBrush(C(T.dim));
        FontFamily family(L"Segoe UI");
        const REAL px = REAL(g.dpi) / 96.0f;
        Font font(&family, 12.5f * px, FontStyleRegular, UnitPixel);
        Font bold(&family, 12.5f * px, FontStyleBold, UnitPixel);
        Font smallFont(&family, 11.0f * px, FontStyleRegular, UnitPixel);

        if (!g.preview) {
            std::wstring msg = L"These settings can't be used:\n";
            for (auto& e : g.previewErrors) msg += L"\n\x2022 " + e;
            RectF box(8 * px, 8 * px, REAL(w) - 16 * px, REAL(h) - 16 * px);
            SolidBrush red(C(T.danger));
            gr.DrawString(msg.c_str(), -1, &font, box, nullptr, &red);
        }
        else {
            const bool dyn = g.cfg.mode != DynamicMode::Off;
            const std::vector<double> speeds = dyn ? std::vector<double>{ 2, 10, 30 } : std::vector<double>{ 10 };
            const Color colors[3] = { C(T.accent), C(T.warn), C(T.ok) };

            std::vector<std::vector<double>> sens(speeds.size(), std::vector<double>(361));
            double maxSens = 1.0;
            for (size_t si = 0; si < speeds.size(); ++si) {
                for (int a = 0; a <= 360; ++a) {
                    sens[si][size_t(a)] = g.preview->At(a % 360, speeds[si]).sens;
                    maxSens = std::max(maxSens, sens[si][size_t(a)]);
                }
            }
            // Grid rings at a "nice" step so there are only a few, whatever the range
            // (typing 100 into a box must not draw 200 rings).
            double ringStep = 0.5;
            for (double s : { 0.5, 1.0, 2.0, 2.5, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0, 1000.0 }) {
                ringStep = s;
                if (maxSens * 1.1 / s <= 4.0) break;
            }
            const double ringMax = std::max(1.5, std::ceil(maxSens * 1.1 / ringStep) * ringStep);

            const REAL labelPad = 40 * px;
            const REAL cx = REAL(w) / 2, cy = REAL(h) / 2;
            const REAL R = std::min(REAL(w), REAL(h)) / 2 - labelPad;
            auto pt = [&](double deg, double s) {
                const double r = s / ringMax * R;
                return PointF(REAL(cx + std::cos(deg * kPi / 180) * r), REAL(cy - std::sin(deg * kPi / 180) * r));
            };

            Pen grid(C(T.grid), 1.0f * px);
            Pen one(C(T.gridStrong), 1.4f * px);
            one.SetDashStyle(DashStyleDash);
            std::vector<double> rings;
            for (double ring = ringStep; ring <= ringMax + 1e-9; ring += ringStep) rings.push_back(ring);
            for (double ring : rings) {
                const REAL r = REAL(ring / ringMax * R);
                gr.DrawEllipse(std::fabs(ring - 1.0) < 1e-9 ? &one : &grid, cx - r, cy - r, 2 * r, 2 * r);
            }
            if (ringStep > 1.0) {   // the 1.0 reference ring is always shown
                const REAL r = REAL(1.0 / ringMax * R);
                gr.DrawEllipse(&one, cx - r, cy - r, 2 * r, 2 * r);
            }
            for (int a = 0; a < 360; a += 45) gr.DrawLine(&grid, pt(a, 0), pt(a, ringMax));

            for (size_t si = speeds.size(); si-- > 0;) {
                std::vector<PointF> poly(360);
                for (int a = 0; a < 360; ++a) poly[size_t(a)] = pt(a, sens[si][size_t(a)]);
                const Color c = colors[si];
                if (si == 0) {
                    SolidBrush fill(Color(T.dark ? 46 : 36, c.GetR(), c.GetG(), c.GetB()));
                    gr.FillPolygon(&fill, poly.data(), INT(poly.size()));
                }
                Pen pen(c, (si == 0 ? 2.6f : 1.8f) * px);
                pen.SetLineJoin(LineJoinRound);
                gr.DrawPolygon(&pen, poly.data(), INT(poly.size()));
            }

            // Ring values on small chips along the upper-right diagonal, readable over the curve.
            {
                StringFormat mid;
                mid.SetAlignment(StringAlignmentCenter);
                mid.SetLineAlignment(StringAlignmentCenter);
                SolidBrush chip(C(T.card, 220));
                for (double ring : rings) {
                    const PointF p = pt(60, ring);
                    wchar_t lbl[16];
                    swprintf_s(lbl, ring < 10 ? L"%.1f" : L"%.0f", ring);
                    const REAL cw = REAL(ring < 100 ? 26 : 34) * px;
                    RectF box(p.X - cw / 2, p.Y - 8 * px, cw, 16 * px);
                    GraphicsPath path;
                    RoundPath(path, box, 6 * px);
                    gr.FillPath(&chip, &path);
                    gr.DrawString(lbl, -1, &smallFont, box, &mid, &dimBrush);
                }
            }

            struct DirLabel { int deg; const wchar_t* name; };
            const DirLabel labels[] = { { 0, L"Right" }, { 90, L"Up" }, { 180, L"Left" }, { 270, L"Down" } };
            StringFormat center;
            center.SetAlignment(StringAlignmentCenter);
            center.SetLineAlignment(StringAlignmentCenter);
            for (const auto& l : labels) {
                PointF p = pt(l.deg, ringMax);
                const REAL dx = REAL(std::cos(l.deg * kPi / 180)), dy = REAL(-std::sin(l.deg * kPi / 180));
                const REAL bx = p.X + dx * 30 * px, by = p.Y + dy * 20 * px;
                RectF nameBox(bx - 45 * px, by - 17 * px, 90 * px, 16 * px);
                RectF valBox(bx - 45 * px, by - 1 * px, 90 * px, 18 * px);
                gr.DrawString(l.name, -1, &smallFont, nameBox, &center, &dimBrush);
                wchar_t v[32];
                swprintf_s(v, L"%.3f\x00D7", sens[0][size_t(l.deg)]);
                gr.DrawString(v, -1, &bold, valBox, &center, &textBrush);
            }

            if (dyn) {
                const wchar_t* unit = g.previewDpi > 0 ? L"in/s" : L"counts/ms";
                REAL y = 2 * px;
                for (size_t si = 0; si < speeds.size(); ++si) {
                    Pen pen(colors[si], 2.6f * px);
                    gr.DrawLine(&pen, PointF(4 * px, y + 9 * px), PointF(22 * px, y + 9 * px));
                    wchar_t s[64];
                    swprintf_s(s, L"%.0f %ls", speeds[si], unit);
                    gr.DrawString(s, -1, &smallFont, PointF(27 * px, y), &textBrush);
                    y += 19 * px;
                }
            }
        }

        Graphics screen(hdc);
        screen.DrawImage(&buffer, rc.left, rc.top, w, h);
    }

    LRESULT CALLBACK DiagramProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            PaintDiagram(hdc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // ------------------------------------------------------------------------------------------
    // Main window painting: header, cards, field frames
    // ------------------------------------------------------------------------------------------

    struct Pill {
        std::wstring text;
        COLORREF dot;
        bool hasDot;
    };

    std::vector<Pill> HeaderPills()
    {
        std::vector<Pill> pills;
        if (g.dirty) pills.push_back({ L"Unsaved changes", T.warn, true });
        if (!g.raError.empty()) pills.push_back({ L"Raw Accel not found", T.danger, true });
        else pills.push_back({ g.raVersion, 0, false });
        return pills;
    }

    void PaintMain(HDC hdc)
    {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        const int w = rc.right, h = rc.bottom;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HGDIOBJ oldFont = SelectObject(mem, g.font);
        SetBkMode(mem, TRANSPARENT);

        // Pill geometry (needs text widths).
        struct PillBox { Pill p; RECT r; };
        std::vector<PillBox> pills;
        {
            int x = S(kClientW - 16 - kPowerW - 10);   // left of the on/off switch
            const int y = S(17), ph = S(26);
            for (const auto& p : HeaderPills()) {
                SIZE sz{};
                GetTextExtentPoint32W(mem, p.text.c_str(), int(p.text.size()), &sz);
                const int pw = sz.cx + S(p.hasDot ? 34 : 24);
                RECT r = { x - pw, y, x, y + ph };
                pills.push_back({ p, r });
                x -= pw + S(8);
            }
        }

        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gr.Clear(C(T.bg));

            for (const auto& c : g.cards) FillRound(gr, c.rc, REAL(S(12)), T.card, T.cardBorder);

            for (const auto& f : g.fields) {
                if (!IsWindowVisible(f.edit)) continue;
                const bool enabled = IsWindowEnabled(f.edit) != FALSE;
                COLORREF border = T.fieldBorder;
                if (IsBadEdit(f.edit)) border = T.danger;
                else if (f.edit == g.focusedEdit) border = T.accent;
                FillRound(gr, f.rc, REAL(S(7)), enabled ? T.field : T.card, border, f.edit == g.focusedEdit ? 1.6f : 1.0f);
            }

            for (const auto& pb : pills) {
                FillRound(gr, pb.r, REAL(S(13)), T.card, T.cardBorder);
                if (pb.p.hasDot) {
                    Gdiplus::SolidBrush dot(C(pb.p.dot));
                    const REAL d = REAL(S(8));
                    gr.FillEllipse(&dot, REAL(pb.r.left + S(12)), REAL(pb.r.top + (pb.r.bottom - pb.r.top) / 2) - d / 2, d, d);
                }
            }

            // App mark: a small "omnidirectional" rosette next to the name.
            const REAL mx = REAL(S(24)), my = REAL(S(30)), mr = REAL(S(11));
            Gdiplus::Pen markPen(C(T.accent), REAL(S(2)));
            markPen.SetStartCap(Gdiplus::LineCapRound);
            markPen.SetEndCap(Gdiplus::LineCapRound);
            for (int a = 0; a < 360; a += 45) {
                const REAL len = (a % 90 == 0) ? mr : mr * 0.62f;
                gr.DrawLine(&markPen, mx + mr, my, mx + mr + len * REAL(std::cos(a * kPi / 180)), my - len * REAL(std::sin(a * kPi / 180)));
            }
        }

        // Text (GDI, ClearType).
        auto text = [&](const std::wstring& s, int x, int y, HFONT f, COLORREF c) {
            SelectObject(mem, f);
            SetTextColor(mem, c);
            TextOutW(mem, x, y, s.c_str(), int(s.size()));
            SIZE sz{};
            GetTextExtentPoint32W(mem, s.c_str(), int(s.size()), &sz);
            return sz.cx;
        };

        int tx = S(56);
        tx += text(L"ODDPI", tx, S(15), g.fontTitle, T.text);
        text(L"Omnidirectional DPI  \x00B7  powered by the Raw Accel driver", tx + S(12), S(23), g.fontSmall, T.dim);

        for (const auto& c : g.cards) {
            if (c.title.empty()) continue;
            int x = c.rc.left + S(16);
            x += text(c.title, x, c.rc.top + S(13), g.fontSemi, T.text);
            if (!c.subtitle.empty()) text(c.subtitle, x + S(10), c.rc.top + S(15), g.fontSmall, T.dim);
        }

        for (const auto& pb : pills) {
            SelectObject(mem, g.font);
            SetTextColor(mem, pb.p.hasDot ? T.text : T.dim);
            RECT r = pb.r;
            r.left += S(pb.p.hasDot ? 26 : 12);
            DrawTextW(mem, pb.p.text.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    // ------------------------------------------------------------------------------------------
    // Owner-drawn buttons and toggle
    // ------------------------------------------------------------------------------------------

    ButtonKind KindOf(int id)
    {
        if (id == kIdApply) return ButtonKind::Primary;
        if (id == kIdCalibrate || id == kIdSensFinder || id == kIdGridshot) return ButtonKind::Accent;
        return ButtonKind::Secondary;
    }

    void DrawButton(const DRAWITEMSTRUCT* di)
    {
        const RECT rc = di->rcItem;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(di->hDC);
        HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        const bool pressed = (di->itemState & ODS_SELECTED) != 0;
        const bool hot = g.hot == di->hwndItem;
        const bool focus = (di->itemState & ODS_FOCUS) != 0 && GetFocus() == di->hwndItem;
        const ButtonKind kind = KindOf(int(di->CtlID));

        COLORREF fill, border, fg;
        switch (kind) {
        case ButtonKind::Primary:
            fill = pressed ? T.accentPressed : hot ? T.accentHover : T.accent;
            border = fill;
            fg = T.accentText;
            break;
        case ButtonKind::Accent:
            fill = pressed ? T.secondaryHover : hot ? T.secondaryHover : T.secondary;
            border = T.accent;
            fg = T.accent;
            break;
        default:
            fill = pressed || hot ? T.secondaryHover : T.secondary;
            border = T.fieldBorder;
            fg = T.text;
            break;
        }
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gr.Clear(C(T.bg));
            RECT local = { 0, 0, w, h };
            FillRound(gr, local, REAL(S(8)), fill, focus ? T.accent : border, focus ? 2.0f : 1.0f);
        }
        wchar_t label[64];
        GetWindowTextW(di->hwndItem, label, 64);
        SelectObject(mem, kind == ButtonKind::Secondary ? g.font : g.fontSemi);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, fg);
        RECT local = { 0, 0, w, h };
        DrawTextW(mem, label, -1, &local, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        BitBlt(di->hDC, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void DrawToggle(const DRAWITEMSTRUCT* di)
    {
        const RECT rc = di->rcItem;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(di->hDC);
        HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        const bool on = g.cfg.gain;
        const bool hot = g.hot == di->hwndItem;
        const int tw = S(38), th = S(20);
        const int ty = (h - th) / 2;
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gr.Clear(C(T.card));
            RECT track = { 0, ty, tw, ty + th };
            FillRound(gr, track, REAL(th) / 2, on ? (hot ? T.accentHover : T.accent) : T.track, on ? T.accent : T.fieldBorder);
            Gdiplus::SolidBrush knob(C(RGB(255, 255, 255)));
            const REAL kd = REAL(th - S(6));
            const REAL kx = on ? REAL(tw - S(3)) - kd : REAL(S(3));
            gr.FillEllipse(&knob, kx, REAL(ty + S(3)), kd, kd);
        }
        SelectObject(mem, g.font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, T.text);
        RECT tr = { tw + S(10), 0, w, h };
        DrawTextW(mem, L"Gain (recommended)", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        BitBlt(di->hDC, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    // Header switch: "ODDPI on" (green) / "ODDPI off".
    void DrawPower(const DRAWITEMSTRUCT* di)
    {
        const RECT rc = di->rcItem;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(di->hDC);
        HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        const bool on = g.oddpiActive;
        const bool usable = g.raError.empty();
        const bool hot = g.hot == di->hwndItem;
        const bool focus = (di->itemState & ODS_FOCUS) != 0 && GetFocus() == di->hwndItem;
        const int tw = S(34), th = S(18), tx = S(10), ty = (h - th) / 2;
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gr.Clear(C(T.bg));
            RECT pill = { 0, 0, w, h };
            FillRound(gr, pill, REAL(h) / 2, hot ? T.secondaryHover : T.card, focus ? T.accent : T.cardBorder);
            RECT track = { tx, ty, tx + tw, ty + th };
            FillRound(gr, track, REAL(th) / 2, on ? T.ok : T.track, on ? T.ok : T.fieldBorder);
            Gdiplus::SolidBrush knob(C(usable ? RGB(255, 255, 255) : T.dim));
            const REAL kd = REAL(th - S(6));
            const REAL kx = on ? REAL(tx + tw - S(3)) - kd : REAL(tx + S(3));
            gr.FillEllipse(&knob, kx, REAL(ty + S(3)), kd, kd);
        }
        SelectObject(mem, g.fontSemi);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, usable ? T.text : T.dim);
        RECT tr = { tx + tw + S(9), 0, w, h };
        DrawTextW(mem, on ? L"ODDPI on" : L"ODDPI off", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        BitBlt(di->hDC, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    // Tracks the hovered owner-drawn control.
    LRESULT CALLBACK HoverProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
    {
        if (m == WM_MOUSEMOVE && g.hot != h) {
            HWND old = g.hot;
            g.hot = h;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
            if (old) InvalidateRect(old, nullptr, FALSE);
        }
        else if (m == WM_MOUSELEAVE) {
            if (g.hot == h) g.hot = nullptr;
            InvalidateRect(h, nullptr, FALSE);
        }
        return DefSubclassProc(h, m, wp, lp);
    }

    // ------------------------------------------------------------------------------------------
    // Slider: our own control (the common trackbar only repaints its narrow thumb area, which
    // leaves trails with a custom-drawn knob and fill). It speaks the same TBM_SETRANGEMIN/MAX,
    // TBM_SETPOS, TBM_GETPOS, TBM_SETPAGESIZE messages and notifies the parent with WM_HSCROLL,
    // so the rest of the code treats it like a trackbar. Mouse, arrows, PgUp/PgDn, Home/End.
    // ------------------------------------------------------------------------------------------

    struct SliderState {
        LONG min = 0, max = 100, pos = 0, page = 10;
        bool hot = false, drag = false;
    };

    SliderState* Slider(HWND h) { return reinterpret_cast<SliderState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

    int KnobRadius() { return S(8); }

    void SliderGeometry(HWND h, const SliderState& s, int& x0, int& x1, int& knobX, int& cy)
    {
        RECT rc;
        GetClientRect(h, &rc);
        x0 = rc.left + KnobRadius() + S(3);
        x1 = rc.right - KnobRadius() - S(3);
        cy = (rc.top + rc.bottom) / 2;
        const double f = s.max > s.min ? double(s.pos - s.min) / double(s.max - s.min) : 0.0;
        knobX = x0 + int(std::lround(f * (x1 - x0)));
    }

    void SliderSet(HWND h, SliderState& s, LONG pos, bool notify)
    {
        pos = std::clamp(pos, s.min, s.max);
        if (pos == s.pos) return;
        s.pos = pos;
        InvalidateRect(h, nullptr, FALSE);
        if (notify) SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), LPARAM(h));
    }

    void SliderFromMouse(HWND h, SliderState& s, int x)
    {
        int x0, x1, knobX, cy;
        SliderGeometry(h, s, x0, x1, knobX, cy);
        const double f = x1 > x0 ? std::clamp(double(x - x0) / double(x1 - x0), 0.0, 1.0) : 0.0;
        SliderSet(h, s, s.min + LONG(std::lround(f * (s.max - s.min))), true);
    }

    void PaintSlider(HWND h, HDC hdc, const SliderState& s)
    {
        RECT rc;
        GetClientRect(h, &rc);
        const int w = rc.right, hh = rc.bottom;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, hh);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Gdiplus::Graphics gr(mem);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gr.Clear(C(T.card));

            int x0, x1, knobX, cy;
            SliderGeometry(h, s, x0, x1, knobX, cy);
            const REAL th = REAL(S(4));
            Gdiplus::SolidBrush track(C(T.track)), fill(C(IsWindowEnabled(h) ? T.accent : T.gridStrong));
            Gdiplus::GraphicsPath full;
            RoundPath(full, Gdiplus::RectF(REAL(x0), cy - th / 2, REAL(x1 - x0), th), th / 2);
            gr.FillPath(&track, &full);
            if (knobX > x0) {
                Gdiplus::GraphicsPath part;
                RoundPath(part, Gdiplus::RectF(REAL(x0), cy - th / 2, REAL(knobX - x0), th), th / 2);
                gr.FillPath(&fill, &part);
            }

            const bool focus = GetFocus() == h;
            const REAL r = REAL(KnobRadius() - ((s.hot || s.drag || focus) ? 0 : S(1)));
            if (focus || s.drag) {   // soft halo
                Gdiplus::SolidBrush halo(C(T.accent, 60));
                const REAL hr = r + REAL(S(4));
                gr.FillEllipse(&halo, knobX - hr, cy - hr, 2 * hr, 2 * hr);
            }
            Gdiplus::SolidBrush knob(C(RGB(255, 255, 255)));
            Gdiplus::Pen ring(C(T.accent), REAL(S(2)));
            gr.FillEllipse(&knob, knobX - r, cy - r, 2 * r, 2 * r);
            gr.DrawEllipse(&ring, knobX - r, cy - r, 2 * r, 2 * r);
        }
        BitBlt(hdc, 0, 0, w, hh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    LRESULT CALLBACK SliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        SliderState* s = Slider(h);
        switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(new SliderState));
            break;
        case WM_NCDESTROY:
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case TBM_SETRANGEMIN: s->min = LONG(lp); s->pos = std::max(s->pos, s->min); InvalidateRect(h, nullptr, FALSE); return 0;
        case TBM_SETRANGEMAX: s->max = LONG(lp); s->pos = std::min(s->pos, s->max); InvalidateRect(h, nullptr, FALSE); return 0;
        case TBM_SETPAGESIZE: { LONG old = s->page; s->page = std::max(1L, LONG(lp)); return old; }
        case TBM_SETPOS: SliderSet(h, *s, LONG(lp), false); return 0;
        case TBM_GETPOS: return s->pos;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            PaintSlider(h, hdc, *s);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            SetCapture(h);
            s->drag = true;
            SliderFromMouse(h, *s, GET_X_LPARAM(lp));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (!s->hot) {
                s->hot = true;
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            if (s->drag) SliderFromMouse(h, *s, GET_X_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            if (s->drag) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            s->drag = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSELEAVE:
            s->hot = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN: {
            const LONG step = std::max(1L, s->page / 10);
            switch (wp) {
            case VK_LEFT: case VK_DOWN: SliderSet(h, *s, s->pos - step, true); return 0;
            case VK_RIGHT: case VK_UP: SliderSet(h, *s, s->pos + step, true); return 0;
            case VK_PRIOR: SliderSet(h, *s, s->pos + s->page, true); return 0;
            case VK_NEXT: SliderSet(h, *s, s->pos - s->page, true); return 0;
            case VK_HOME: SliderSet(h, *s, s->min, true); return 0;
            case VK_END: SliderSet(h, *s, s->max, true); return 0;
            }
            break;
        }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // ------------------------------------------------------------------------------------------
    // Window creation and layout
    // ------------------------------------------------------------------------------------------

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0, DWORD ex = 0)
    {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.hwnd,
                                 HMENU(INT_PTR(id)), g.inst, nullptr);
        SendMessageW(h, WM_SETFONT, WPARAM(g.font), FALSE);
        return h;
    }

    HWND MakeEdit(int id, DWORD extra = 0)
    {
        HWND h = Make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT | extra, id);
        return h;
    }

    HWND MakeCombo(int id)
    {
        HWND h = Make(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
        return h;
    }

    void CreateSliderRow(SliderRow& r, int id)
    {
        r.hLabel = Make(WC_STATICW, r.label, SS_LEFT | SS_CENTERIMAGE);
        r.hTrack = Make(L"ODDPI_Slider", L"", WS_TABSTOP, id + 50);
        const double scale = std::pow(10.0, r.decimals);
        SendMessageW(r.hTrack, TBM_SETRANGEMIN, FALSE, LPARAM(std::lround(r.min * scale)));
        SendMessageW(r.hTrack, TBM_SETRANGEMAX, FALSE, LPARAM(std::lround(r.max * scale)));
        SendMessageW(r.hTrack, TBM_SETPAGESIZE, 0, LPARAM(std::lround(scale / (r.decimals >= 2 ? 20 : 1))));
        r.hEdit = MakeEdit(id);
    }

    HWND MakeButton(const wchar_t* text, int id)
    {
        HWND h = Make(WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW, id);
        SetWindowSubclass(h, HoverProc, 1, 0);
        return h;
    }

    void CreateControls()
    {
        for (size_t i = 0; i < g.general.size(); ++i) CreateSliderRow(g.general[i], kIdGeneralBase + int(i));
        g.dpiLabel = Make(WC_STATICW, L"Mouse DPI", SS_LEFT | SS_CENTERIMAGE);
        g.dpiHint = Make(WC_STATICW, L"Your mouse's real DPI", SS_LEFT | SS_CENTERIMAGE);
        g.dpiEdit = MakeEdit(kIdDpi, ES_NUMBER);

        g.gameLabel = Make(WC_STATICW, L"Game", SS_LEFT | SS_CENTERIMAGE);
        g.gameCombo = MakeCombo(kIdGame);
        SendMessageW(g.gameCombo, CB_ADDSTRING, 0, LPARAM(L"(not set)"));
        for (const auto& gi : g.games) {
            std::wstring item = gi.name + (gi.estimate ? L" (estimate)" : L"");
            SendMessageW(g.gameCombo, CB_ADDSTRING, 0, LPARAM(item.c_str()));
        }
        SendMessageW(g.gameCombo, CB_ADDSTRING, 0, LPARAM(L"Other: I know my cm/360"));
        g.gameSensEdit = MakeEdit(kIdGameSens);
        g.gameInfo = Make(WC_STATICW, L"", SS_RIGHT | SS_CENTERIMAGE);

        for (size_t i = 0; i < g.directional.size(); ++i) CreateSliderRow(g.directional[i], kIdDirectionalBase + int(i));

        g.modeLabel = Make(WC_STATICW, L"Mode", SS_LEFT | SS_CENTERIMAGE);
        g.modeCombo = MakeCombo(kIdMode);
        for (const wchar_t* m : { L"Off", L"Classic", L"Natural", L"Jump", L"Power", L"Synchronous" }) {
            SendMessageW(g.modeCombo, CB_ADDSTRING, 0, LPARAM(m));
        }
        g.gainToggle = Make(WC_BUTTONW, L"Gain (recommended)", WS_TABSTOP | BS_OWNERDRAW, kIdGain);
        SetWindowSubclass(g.gainToggle, HoverProc, 1, 0);
        g.dynOffText = Make(WC_STATICW, L"Off \x2014 sensitivity doesn't depend on how fast you move.", SS_LEFT);

        for (size_t i = 0; i < g.dynamic.size(); ++i) {
            auto& r = g.dynamic[i];
            r.hLabel = Make(WC_STATICW, r.label, SS_LEFT | SS_CENTERIMAGE);
            if (r.field) r.hCtl = MakeEdit(kIdDynBase + int(i));
            else {
                r.hCtl = MakeCombo(kIdDynBase + int(i));
                for (const wchar_t* m : { L"Output", L"Input", L"Both" }) SendMessageW(r.hCtl, CB_ADDSTRING, 0, LPARAM(m));
            }
        }

        const wchar_t* names[kButtonCount] = { L"Apply", L"Calibrate\x2026", L"Undo last apply", L"Restore original",
                                               L"Reload file", L"Sens converter\x2026", L"Sens finder\x2026", L"Gridshot\x2026" };
        for (int i = 0; i < kButtonCount; ++i) g.buttons[i] = MakeButton(names[i], kIdApply + i);
        g.powerToggle = MakeButton(L"ODDPI on/off", kIdPower);

        g.diagram = CreateWindowExW(0, L"ODDPI_Diagram", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                                    g.hwnd, nullptr, g.inst, nullptr);
        g.status = Make(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0);
    }

    // Windows only draws standard controls (combo boxes, scrollbars) dark when the app opts in.
    // That opt-in is not a documented API; it is what Explorer and Notepad use, looked up by ordinal
    // from uxtheme.dll. If it is missing, the controls simply stay light.
    void AllowDarkControls(HWND h, bool dark)
    {
        using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
        static const auto allow = reinterpret_cast<AllowDarkModeForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(133)));
        if (allow) allow(h, dark);
    }

    void EnableAppDarkMode(bool dark)
    {
        using SetPreferredAppModeFn = int(WINAPI*)(int);   // 0 default, 1 allow dark, 3 force light
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return;
        auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
        if (setMode) setMode(dark ? 1 : 3);
    }

    // Sets the theme on the standard controls that support it (dark lists, scrollbars).
    void ThemeControls()
    {
        EnableAppDarkMode(T.dark);
        auto theme = [](HWND h, const wchar_t* darkName) {
            AllowDarkControls(h, T.dark);
            SetWindowTheme(h, T.dark ? darkName : L"Explorer", nullptr);
            SendMessageW(h, WM_THEMECHANGED, 0, 0);
        };
        theme(g.modeCombo, L"DarkMode_CFD");
        theme(g.gameCombo, L"DarkMode_CFD");
        for (auto& r : g.dynamic) if (!r.field) theme(r.hCtl, L"DarkMode_CFD");
        theme(g.status, L"DarkMode_Explorer");

        const BOOL dark = T.dark;
        DwmSetWindowAttribute(g.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        const COLORREF caption = T.bg, captionText = T.text;
        DwmSetWindowAttribute(g.hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        DwmSetWindowAttribute(g.hwnd, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));
    }

    void Layout()
    {
        if (!g.modeCombo) return;
        g.cards.clear();
        g.fields.clear();

        HDWP dwp = BeginDeferWindowPos(120);
        auto R = [](int x, int y, int w, int h) { return RECT{ S(x), S(y), S(x + w), S(y + h) }; };
        auto place = [&](HWND h, const RECT& r) {
            dwp = DeferWindowPos(dwp, h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        constexpr int kFieldH = 30;
        auto field = [&](HWND edit, int x, int y, int w) {
            RECT f = R(x, y, w, kFieldH);
            g.fields.push_back({ edit, f });
            const int eh = g.fontH + S(2);
            const int top = f.top + (f.bottom - f.top - eh) / 2;
            place(edit, RECT{ f.left + S(9), top, f.right - S(9), top + eh });
        };
        auto combo = [&](HWND h, int x, int y, int w) {
            place(h, R(x, y + 1, w, 300));
        };

        // ---- Left column
        const int lx = 16, lw = 520, rowH = 34, cardTop = 68;
        const int labelX = lx + 18, labelW = 130;
        const int fieldW = 86, fieldX = lx + lw - 16 - fieldW;
        const int sliderX = lx + 150, sliderW = fieldX - 12 - sliderX;
        auto rowY = [&](int top, int i) { return top + 44 + i * rowH; };
        auto sliderRow = [&](SliderRow& r, int y) {
            place(r.hLabel, R(labelX, y, labelW, kFieldH));
            place(r.hTrack, R(sliderX - 4, y, sliderW + 8, kFieldH));
            field(r.hEdit, fieldX, y, fieldW);
        };

        int top = cardTop;
        int ch = 44 + 5 * rowH + 6;
        g.cards.push_back({ R(lx, top, lw, ch), L"General", L"" });
        for (size_t i = 0; i < g.general.size(); ++i) sliderRow(g.general[i], rowY(top, int(i)));
        int y = rowY(top, 3);
        place(g.dpiLabel, R(labelX, y, labelW, kFieldH));
        place(g.dpiHint, R(sliderX, y, sliderW, kFieldH));
        field(g.dpiEdit, fieldX, y, fieldW);
        y = rowY(top, 4);
        place(g.gameLabel, R(labelX, y, labelW, kFieldH));
        combo(g.gameCombo, sliderX, y, 168);
        place(g.gameInfo, R(sliderX + 172, y, sliderW - 172, kFieldH));
        field(g.gameSensEdit, fieldX, y, fieldW);
        top += ch + 12;

        ch = 44 + int(g.directional.size()) * rowH + 6;
        g.cards.push_back({ R(lx, top, lw, ch), L"Directional", L"\"Up\" = pushing the mouse away from you" });
        for (size_t i = 0; i < g.directional.size(); ++i) sliderRow(g.directional[i], rowY(top, int(i)));
        top += ch + 12;

        const int bottom = 640;
        g.cards.push_back({ R(lx, top, lw, bottom - top), L"", L"" });
        place(g.status, R(lx + 16, top + 10, lw - 24, bottom - top - 18));

        // ---- Right column: diagram on top, speed curve below
        const int rx = 548, rw = kClientW - 16 - rx;
        const bool off = g.cfg.mode == DynamicMode::Off;
        const unsigned bit = 1u << int(g.cfg.mode);
        int visible = 0;
        for (auto& r : g.dynamic) if (!off && (r.modes & bit)) ++visible;
        const int gridRows = (visible + 1) / 2;
        const int speedH = off ? 44 + rowH + 30 : 44 + rowH + gridRows * rowH + 8;
        const int speedTop = bottom - speedH;

        g.cards.push_back({ R(rx, cardTop, rw, speedTop - 12 - cardTop), L"Sensitivity by direction",
                            L"live preview \x00B7 1.0 = unchanged" });
        place(g.diagram, R(rx + 12, cardTop + 40, rw - 24, speedTop - 12 - cardTop - 50));

        g.cards.push_back({ R(rx, speedTop, rw, speedH), L"Speed curve", L"optional, like Raw Accel" });
        y = speedTop + 44;
        place(g.modeLabel, R(rx + 18, y, 70, kFieldH));
        combo(g.modeCombo, rx + 90, y, 170);
        place(g.gainToggle, R(rx + 286, y, 220, kFieldH));
        place(g.dynOffText, R(rx + 18, y + rowH + 4, rw - 36, 22));

        const int colW = (rw - 32) / 2;
        int slot = 0;
        for (auto& r : g.dynamic) {
            if (off || !(r.modes & bit)) continue;
            const int col = slot % 2, row = slot / 2;
            const int x = rx + 18 + col * colW;
            const int yy = y + rowH + row * rowH;
            place(r.hLabel, R(x, yy, 140, kFieldH));
            if (r.field) field(r.hCtl, x + 142, yy, colW - 160);
            else combo(r.hCtl, x + 142, yy, colW - 160);
            ++slot;
        }

        place(g.powerToggle, R(kClientW - 16 - kPowerW, 17, kPowerW, 26));

        // ---- Bottom bar
        const int by = 656, bh = 38;
        place(g.buttons[kIdCalibrate - kIdApply], R(16, by, 132, bh));
        place(g.buttons[kIdSensFinder - kIdApply], R(156, by, 124, bh));
        place(g.buttons[kIdConvert - kIdApply], R(288, by, 136, bh));
        place(g.buttons[kIdGridshot - kIdApply], R(432, by, 104, bh));
        int bx = kClientW - 16;
        auto right = [&](int id, int w) {
            bx -= w;
            place(g.buttons[id - kIdApply], R(bx, by, w, bh));
            bx -= 10;
        };
        right(kIdApply, 120);
        right(kIdOriginal, 140);
        right(kIdUndo, 136);
        right(kIdReload, 112);

        EndDeferWindowPos(dwp);
        RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    HFONT MakeFont(int pt10, int weight)
    {
        return CreateFontW(-MulDiv(pt10, int(g.dpi), 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }

    void CreateFonts()
    {
        for (HFONT* f : { &g.font, &g.fontSemi, &g.fontTitle, &g.fontSmall }) {
            if (*f) DeleteObject(*f);
        }
        g.font = MakeFont(100, FW_NORMAL);
        g.fontSemi = MakeFont(105, FW_SEMIBOLD);
        g.fontTitle = MakeFont(170, FW_BOLD);
        g.fontSmall = MakeFont(90, FW_NORMAL);
        HDC dc = GetDC(nullptr);
        HGDIOBJ old = SelectObject(dc, g.font);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        g.fontH = tm.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }

    void ApplyFontToChildren()
    {
        EnumChildWindows(g.hwnd, [](HWND h, LPARAM f) -> BOOL {
            SendMessageW(h, WM_SETFONT, WPARAM(f), TRUE);
            return TRUE;
        }, LPARAM(g.font));
        for (HWND c : { g.modeCombo, g.gameCombo }) SendMessageW(c, CB_SETITEMHEIGHT, WPARAM(-1), S(22));
        for (auto& r : g.dynamic) if (!r.field) SendMessageW(r.hCtl, CB_SETITEMHEIGHT, WPARAM(-1), S(22));
    }

    // ------------------------------------------------------------------------------------------
    // Input handling
    // ------------------------------------------------------------------------------------------

    void OnSliderEdit(SliderRow& r)
    {
        double v;
        if (!ParseDouble(GetText(r.hEdit), v)) {
            r.bad = true;
            InvalidateField(r.hEdit);
            UpdateStatus();
            return;
        }
        r.bad = false;
        InvalidateField(r.hEdit);
        g.cfg.*r.field = v;
        g.syncing = true;
        const double scale = std::pow(10.0, r.decimals);
        SendMessageW(r.hTrack, TBM_SETPOS, TRUE, LPARAM(std::lround(std::clamp(v, r.min, r.max) * scale)));
        g.syncing = false;
        Changed();
    }

    void OnTrack(HWND track)
    {
        for (auto* rows : { &g.general, &g.directional }) {
            for (auto& r : *rows) {
                if (r.hTrack != track) continue;
                const double v = double(SendMessageW(track, TBM_GETPOS, 0, 0)) / std::pow(10.0, r.decimals);
                g.cfg.*r.field = v;
                g.syncing = true;
                SetWindowTextW(r.hEdit, Format(v, r.decimals).c_str());
                g.syncing = false;
                r.bad = false;
                InvalidateField(r.hEdit);
                Changed();
                return;
            }
        }
    }

    void OnCommand(int id, int code, HWND ctl)
    {
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            HWND old = g.focusedEdit;
            g.focusedEdit = code == EN_SETFOCUS ? ctl : nullptr;
            if (old) InvalidateField(old);
            InvalidateField(ctl);
            return;
        }
        if (code == EN_CHANGE && !g.syncing) {
            if (id >= kIdGeneralBase && id < kIdGeneralBase + int(g.general.size())) OnSliderEdit(g.general[size_t(id - kIdGeneralBase)]);
            else if (id >= kIdDirectionalBase && id < kIdDirectionalBase + int(g.directional.size())) OnSliderEdit(g.directional[size_t(id - kIdDirectionalBase)]);
            else if (id == kIdDpi) {
                double v;
                std::wstring t = GetText(ctl);
                if (t.empty()) t = L"0";
                g.dpiBad = !ParseDouble(t, v) || v < 0 || v > 100000;
                InvalidateField(ctl);
                if (!g.dpiBad) {
                    g.cfg.mouseDpi = int(v);
                    Changed();
                }
                else UpdateStatus();
            }
            else if (id == kIdGameSens) {
                std::wstring t = GetText(ctl);
                double v = 0;
                g.gameSensBad = !t.empty() && (!ParseDouble(t, v) || v < 0);
                InvalidateField(ctl);
                if (!g.gameSensBad) {
                    if (GameComboSelection() == CmComboIndex()) g.cfg.cm360 = v;
                    else g.cfg.gameSensitivity = v;
                    Changed();
                }
                else UpdateStatus();
            }
            else if (id >= kIdDynBase && id < kIdDynBase + int(g.dynamic.size())) {
                auto& r = g.dynamic[size_t(id - kIdDynBase)];
                double v;
                r.bad = !ParseDouble(GetText(ctl), v);
                InvalidateField(ctl);
                if (!r.bad && r.field) {
                    g.cfg.*r.field = v;
                    Changed();
                }
                else UpdateStatus();
            }
            return;
        }
        if (code == CBN_SELCHANGE) {
            const int sel = int(SendMessageW(ctl, CB_GETCURSEL, 0, 0));
            if (id == kIdMode) {
                g.cfg.mode = DynamicMode(sel);
                ShowDynamicRows();
                Changed();
            }
            else if (id == kIdGame) {
                // The box next to the list holds the in-game sensitivity, or cm/360 for "Other".
                if (sel == CmComboIndex()) {
                    g.cfg.game.clear();
                }
                else {
                    g.cfg.cm360 = 0;
                    g.cfg.game = (sel >= 1 && sel <= int(g.games.size())) ? g.games[size_t(sel - 1)].name : L"";
                }
                g.syncing = true;
                const double v = sel == CmComboIndex() ? g.cfg.cm360 : g.cfg.gameSensitivity;
                SetWindowTextW(g.gameSensEdit, v > 0 && sel > 0 ? Format(v, -1).c_str() : L"");
                EnableWindow(g.gameSensEdit, sel > 0);
                InvalidateField(g.gameSensEdit);
                g.syncing = false;
                Changed();
                if (sel > 0) {
                    SetFocus(g.gameSensEdit);
                    SendMessageW(g.gameSensEdit, EM_SETSEL, 0, -1);
                }
            }
            else if (id >= kIdDynBase) {
                g.cfg.capMode = CapMode(sel);
                Changed();
            }
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
        case kIdGain:
            g.cfg.gain = !g.cfg.gain;
            InvalidateRect(g.gainToggle, nullptr, FALSE);
            Changed();
            break;
        case kIdApply: DoApply(); break;
        case kIdCalibrate: DoCalibrate(); break;
        case kIdConvert: DoConvert(); break;
        case kIdSensFinder: DoSensFinder(); break;
        case kIdGridshot: DoGridshot(); break;
        case kIdPower: DoPower(); break;
        case kIdUndo: DoRestore(false); break;
        case kIdOriginal: DoRestore(true); break;
        case kIdReload:
            if (g.dirty && MessageBoxW(g.hwnd, L"Discard your changes and reload oddpi.ini?", L"ODDPI",
                                       MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
                break;
            }
            if (LoadIni()) {
                g.message = L"Reloaded " + g.iniPath + L".";
                SyncControls();
                UpdateTitle();
            }
            break;
        }
    }

    void ResizeForDpi()
    {
        RECT rc = { 0, 0, S(kClientW), S(kClientH) };
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE, 0, g.dpi);
        SetWindowPos(g.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    }

    bool IsEditControl(HWND h)
    {
        wchar_t cls[16];
        GetClassNameW(h, cls, 16);
        return _wcsicmp(cls, WC_EDITW) == 0;
    }

    bool IsDimLabel(HWND h)
    {
        return h == g.dpiHint || h == g.gameInfo || h == g.dynOffText;
    }

    void ApplyTheme()
    {
        MakeTheme(SystemUsesDarkApps());
        ThemeControls();
        RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            CreateFonts();
            CreateControls();
            ApplyFontToChildren();
            ThemeControls();
            ResizeForDpi();
            // Hide the dotted focus rectangles; focus is shown by our own drawing.
            SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
            Layout();
            return 0;
        }
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            CreateFonts();
            ApplyFontToChildren();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ResizeForDpi();
            Layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyTheme();
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintMain(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlID == kIdGain) DrawToggle(di);
            else if (di->CtlID == kIdPower) DrawPower(di);
            else DrawButton(di);
            return TRUE;
        }
        case WM_HSCROLL:
            if (lp && !g.syncing) OnTrack(HWND(lp));
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp), HWND(lp));
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = HDC(wp);
            SetTextColor(dc, IsBadEdit(HWND(lp)) ? T.danger : T.text);
            SetBkColor(dc, T.field);
            return LRESULT(g_brField);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = HDC(wp);
            SetTextColor(dc, T.text);
            SetBkColor(dc, T.field);
            return LRESULT(g_brField);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = HDC(wp);
            HWND h = HWND(lp);
            if (h == g.status) {
                SetTextColor(dc, T.dim);
                SetBkColor(dc, T.card);
                return LRESULT(g_brCard);
            }
            if (IsEditControl(h)) {   // disabled edit
                SetTextColor(dc, T.dim);
                SetBkColor(dc, T.card);
                return LRESULT(g_brCard);
            }
            SetTextColor(dc, IsDimLabel(h) ? T.dim : T.text);
            SetBkColor(dc, T.card);
            return LRESULT(g_brCard);
        }
        case WM_CLOSE:
            if (g.dirty && MessageBoxW(hwnd, L"Your changes are not applied. Close anyway?", L"ODDPI",
                                       MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().wstring();
    }
}

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiIn, nullptr);

    MakeTheme(SystemUsesDarkApps());

    g.inst = inst;
    g.iniPath = (std::filesystem::path(ExeDirectory()) / L"oddpi.ini").wstring();
    g.games = LoadGames(ExeDirectory());

    WNDCLASSW sc{};
    sc.lpfnWndProc = SliderProc;
    sc.hInstance = inst;
    sc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    sc.lpszClassName = L"ODDPI_Slider";
    RegisterClassW(&sc);

    WNDCLASSW dc{};
    dc.lpfnWndProc = DiagramProc;
    dc.hInstance = inst;
    dc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dc.lpszClassName = L"ODDPI_Diagram";
    RegisterClassW(&dc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ODDPI_Main";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"ODDPI",
                                (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    std::error_code ec;
    if (!std::filesystem::exists(g.iniPath, ec)) {
        std::wstring error;
        WriteDefaultConfig(g.iniPath, error);
    }
    LoadIni();
    RefreshDriverState();
    SyncControls();
    UpdateTitle();

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.preview.reset();
    Gdiplus::GdiplusShutdown(gdiToken);
    return int(msg.wParam);
}
