#include "Actions.h"
#include "ProfileBuilder.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace oddpi {

namespace {

    bool IsOddpiName(const wchar_t* name)
    {
        return wcsncmp(name, kProfileName, ra::MAX_NAME_LEN) == 0;
    }

    bool HasProfile(const Snapshot& s, const wchar_t* name)
    {
        for (unsigned i = 0; i < s.ModifierCount(); ++i) {
            if (wcsncmp(s.Modifier(i).prof.name, name, ra::MAX_NAME_LEN) == 0) return true;
        }
        return false;
    }

    std::wstring Fmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[1024];
        va_list args;
        va_start(args, fmt);
        vswprintf_s(buf, fmt, args);
        va_end(args);
        return buf;
    }
}

bool IsOddpiProfile(const ra::profile& p) { return IsOddpiName(p.name); }

bool IsOddpiActive(const Snapshot& s)
{
    return !s.Empty() && IsOddpiProfile(s.Modifier(0).prof);
}

std::vector<std::wstring> CoverageNotes(const Snapshot& s, int effectiveDpi)
{
    std::vector<std::wstring> notes;
    if (s.Empty()) return notes;

    if (s.Base().default_dev_cfg.disable) {
        notes.push_back(L"Raw Accel's default device config is disabled, so mice without their own "
                        L"device entry are not affected. Enable it in Raw Accel's device menu.");
    }

    for (unsigned i = 0; i < s.DeviceCount(); ++i) {
        const auto& d = s.Device(i);
        const wchar_t* name = d.name[0] ? d.name : d.id;
        if (d.config.disable) {
            notes.push_back(Fmt(L"Raw Accel is disabled for device '%ls'.", name));
        }
        else if (d.profile[0] && !IsOddpiName(d.profile) && HasProfile(s, d.profile)) {
            notes.push_back(Fmt(L"Device '%ls' is mapped to Raw Accel profile '%ls', so ODDPI does not "
                                L"affect it. Map it to 'ODDPI' (or remove the mapping) in Raw Accel.",
                                name, d.profile));
        }
        else if (d.config.dpi > 0 && d.config.dpi != effectiveDpi) {
            notes.push_back(Fmt(L"Device '%ls' has its own DPI (%d) in Raw Accel, so ODDPI's sensitivity on it "
                                L"is scaled by %.3fx. Set MouseDPI=%d if that is its real DPI.",
                                name, d.config.dpi,
                                (effectiveDpi > 0 ? effectiveDpi : ra::NORMALIZED_DPI) / double(d.config.dpi),
                                d.config.dpi));
        }
    }
    return notes;
}

bool ApplyConfig(const Config& cfg, ApplyOutcome& outcome, std::vector<std::wstring>& errors)
{
    const ra::version_t version = DriverVersion();
    const Snapshot current = ReadDriver();

    const int dpi = EffectiveDpi(cfg, current.Base().default_dev_cfg.dpi);
    auto profile = std::make_unique<ra::profile>();
    if (!BuildProfile(cfg, dpi, *profile, errors)) return false;

    outcome.backup = SaveBackup(current, version);

    // The driver uses the precomputed data in modifier_settings as-is,
    // so it must be initialised here exactly like Raw Accel's own tools do.
    auto oddpiSettings = std::make_unique<ra::modifier_settings>();
    oddpiSettings->prof = *profile;
    ra::init_data(*oddpiSettings);

    ra::io_base base = current.Base();  // keeps the user's default device config
    if (cfg.mouseDpi > 0) base.default_dev_cfg.dpi = cfg.mouseDpi;

    // ODDPI first = default for every mouse; keep the user's other profiles after it
    // so device-specific mappings in Raw Accel keep working.
    std::vector<const ra::modifier_settings*> modifiers = { oddpiSettings.get() };
    for (unsigned i = 0; i < current.ModifierCount(); ++i) {
        if (!IsOddpiProfile(current.Modifier(i).prof)) modifiers.push_back(&current.Modifier(i));
    }

    std::vector<const ra::device_settings*> devices;
    for (unsigned i = 0; i < current.DeviceCount(); ++i) devices.push_back(&current.Device(i));

    Snapshot next = Snapshot::Build(base, modifiers, devices);
    WriteDriver(next);

    outcome.notes = CoverageNotes(next, dpi);
    return true;
}

bool HasOddpiProfile(const Snapshot& s)
{
    for (unsigned i = 0; i < s.ModifierCount(); ++i) {
        if (IsOddpiProfile(s.Modifier(i).prof)) return true;
    }
    return false;
}

bool DisableOddpi(std::wstring& backup)
{
    const ra::version_t version = DriverVersion();
    const Snapshot current = ReadDriver();
    if (!HasOddpiProfile(current)) return false;

    backup = SaveBackup(current, version);

    std::vector<const ra::modifier_settings*> others;
    for (unsigned i = 0; i < current.ModifierCount(); ++i) {
        if (!IsOddpiProfile(current.Modifier(i).prof)) others.push_back(&current.Modifier(i));
    }
    if (others.empty()) {
        WriteDriver(Snapshot{});   // nothing else configured: Raw Accel passes input through
        return true;
    }
    std::vector<const ra::device_settings*> devices;
    for (unsigned i = 0; i < current.DeviceCount(); ++i) devices.push_back(&current.Device(i));
    WriteDriver(Snapshot::Build(current.Base(), others, devices));
    return true;
}

std::wstring RestoreBackup(const std::wstring& arg, bool& empty)
{
    std::wstring path;
    if (arg.empty()) {
        auto backups = ListBackups();
        if (backups.empty()) throw std::runtime_error("no backups yet (they are made when you apply)");
        path = backups.front();
    }
    else if (_wcsicmp(arg.c_str(), L"original") == 0) {
        path = OriginalBackupPath();
    }
    else {
        path = arg;
    }

    const ra::version_t version = DriverVersion();
    Snapshot snap = LoadBackup(path, version);
    WriteDriver(snap);
    empty = snap.Empty();
    return path;
}

std::wstring CalibrationDirectory()
{
    auto dir = std::filesystem::path(BackupDirectory()).parent_path() / L"calibrations";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::vector<std::wstring> ListCalibrationLogs()
{
    std::vector<std::wstring> logs;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(CalibrationDirectory(), ec)) {
        if (e.path().extension() == L".csv") logs.push_back(e.path().wstring());
    }
    std::sort(logs.rbegin(), logs.rend());  // names are timestamps
    return logs;
}

std::wstring SaveCalibrationLog(const CalOptions& opt, const std::vector<CalTrial>& trials)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"cal-%04d%02d%02d-%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring path = (std::filesystem::path(CalibrationDirectory()) / name).wstring();
    return SaveTrials(path, opt, trials) ? path : L"";
}

Config WithCalibration(const Config& cfg, const CalResult& r)
{
    Config next = cfg;
    next.right *= r.right;
    next.left *= r.left;
    next.up *= r.up;
    next.down *= r.down;
    next.rotation += r.rotationDeg;
    return next;
}

}  // namespace oddpi
