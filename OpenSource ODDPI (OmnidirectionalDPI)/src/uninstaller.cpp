// uninstaller.exe - removes ODDPI and, if ticked, the Raw Accel driver.
//
// Checklist:
//   always     ODDPI's profile is taken out of the Raw Accel driver (mouse back to normal right away)
//   [x]        ODDPI program files (next to this exe)
//   [x]        ODDPI settings + data (oddpi.ini, games.ini, %LOCALAPPDATA%\ODDPI)
//   [ ]        Raw Accel driver + its registry entries (admin, restart)
//   [ ]        Raw Accel program folder (only when one is found)
//
// Raw Accel driver removal does what Raw Accel's own uninstaller.exe does
// (github.com/RawAccelOfficial/rawaccel, uninstaller/uninstaller.cpp + common/utility-install.hpp):
//   1. remove "rawaccel" from the mouse class UpperFilters (SetupDi class registry property)
//   2. rename %SystemRoot%\System32\drivers\rawaccel.sys -> .tmp and delete it at reboot
// and additionally cleans up what that uninstaller leaves behind:
//   3. "rawaccel" in any mouse device's own UpperFilters
//   4. the "rawaccel" kernel service key (Services\rawaccel)
// Safety rule: the driver file and service are only removed once NO filter list names rawaccel
// any more. A filter entry pointing at a missing driver would stop the mouse from working.
//
// Admin work runs in an elevated copy of this exe (--elevated ...), so per-user data is
// always removed for the user who started the uninstaller.

#include "Driver.h"
#include "ProfileBuilder.h"   // kProfileName

#include <windows.h>
#include <commctrl.h>
#include <setupapi.h>
#include <devguid.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Uninstall ODDPI";
constexpr wchar_t kRaName[] = L"rawaccel";

// Exit codes of the elevated helper (bit flags).
constexpr DWORD kExitOk = 0;
constexpr DWORD kExitFailed = 1;
constexpr DWORD kExitReboot = 2;
constexpr DWORD kExitFolderFailed = 4;

// ------------------------------------------------------------------ helpers

std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::wstring(buf, n);
}

std::wstring ExeDir() { return fs::path(ExePath()).parent_path().wstring(); }

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring Expand(const wchar_t* s)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(s, buf, DWORD(std::size(buf)));
    return (n > 0 && n <= std::size(buf)) ? std::wstring(buf) : std::wstring();
}

std::wstring ErrorText(DWORD err)
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"error " + std::to_wstring(err);
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

std::wstring Widen(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

FILE* OpenFile(const wchar_t* path, const wchar_t* mode)
{
    FILE* f = nullptr;
    return _wfopen_s(&f, path, mode) == 0 ? f : nullptr;
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
    CloseHandle(token);
    return ok && e.TokenIsElevated;
}

bool IsUnder(const fs::path& child, const fs::path& parent)
{
    if (parent.empty()) return false;
    std::wstring c = fs::weakly_canonical(child).wstring(), p = fs::weakly_canonical(parent).wstring();
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return c.size() >= p.size() && _wcsnicmp(c.c_str(), p.c_str(), p.size()) == 0;
}

// ------------------------------------------------------------------ processes

struct ProcInfo { DWORD pid; std::wstring name, path; };

std::vector<ProcInfo> FindProcesses(std::initializer_list<const wchar_t*> names)
{
    std::vector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        for (auto* n : names) {
            if (_wcsicmp(pe.szExeFile, n) != 0) continue;
            ProcInfo p{ pe.th32ProcessID, pe.szExeFile, L"" };
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(std::size(buf));
                if (QueryFullProcessImageNameW(h, 0, buf, &len)) p.path.assign(buf, len);
                CloseHandle(h);
            }
            out.push_back(p);
        }
    }
    CloseHandle(snap);
    return out;
}

bool KillProcess(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool ok = TerminateProcess(h, 0) && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
    CloseHandle(h);
    return ok;
}

// ------------------------------------------------------------------ Raw Accel folder

bool LooksLikeRawAccelFolder(const fs::path& dir)
{
    std::error_code ec;
    return fs::is_regular_file(dir / L"rawaccel.exe", ec) &&
           (fs::is_regular_file(dir / L"writer.exe", ec) || fs::is_regular_file(dir / L"wrapper.dll", ec) ||
            fs::is_regular_file(dir / L"driver" / L"rawaccel.sys", ec));
}

std::wstring FindRawAccelFolder()
{
    // A running Raw Accel GUI tells us exactly where it lives.
    for (auto& p : FindProcesses({ L"rawaccel.exe" })) {
        if (!p.path.empty() && LooksLikeRawAccelFolder(fs::path(p.path).parent_path()))
            return fs::path(p.path).parent_path().wstring();
    }

    std::vector<fs::path> roots = {
        fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"Programs",
        KnownFolder(FOLDERID_ProgramFiles),
        KnownFolder(FOLDERID_ProgramFilesX86),
        KnownFolder(FOLDERID_Downloads),
        KnownFolder(FOLDERID_Desktop),
        KnownFolder(FOLDERID_Documents),
        fs::path(ExeDir()).parent_path(),   // extracted next to ODDPI
        fs::path(ExeDir()),
    };
    std::error_code ec;
    for (auto& root : roots) {
        if (root.empty() || !fs::is_directory(root, ec)) continue;
        for (auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_directory(ec)) continue;
            std::wstring name = e.path().filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name.find(L"rawaccel") == std::wstring::npos && name.find(L"raw accel") == std::wstring::npos) continue;
            if (LooksLikeRawAccelFolder(e.path())) return e.path().wstring();
            // Zip extracted into a same-named subfolder.
            for (auto& sub : fs::directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec)) {
                if (sub.is_directory(ec) && LooksLikeRawAccelFolder(sub.path())) return sub.path().wstring();
            }
        }
    }
    return L"";
}

// Never delete a drive root, the Windows folder, a user profile root or a known folder itself.
bool SafeToDeleteFolder(const fs::path& dir)
{
    if (dir.empty() || !dir.has_relative_path() || !LooksLikeRawAccelFolder(dir)) return false;
    auto canon = fs::weakly_canonical(dir);
    for (auto id : { FOLDERID_Windows, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_Profile,
                     FOLDERID_Desktop, FOLDERID_Downloads, FOLDERID_Documents, FOLDERID_LocalAppData }) {
        auto k = KnownFolder(id);
        if (!k.empty() && _wcsicmp(fs::weakly_canonical(k).wstring().c_str(), canon.wstring().c_str()) == 0) return false;
    }
    return !IsUnder(canon, KnownFolder(FOLDERID_Windows));
}

bool DeleteFolder(const std::wstring& dir, std::wstring& log)
{
    if (!SafeToDeleteFolder(dir)) {
        log += L"  ! Skipped " + dir + L" (doesn't look like a Raw Accel folder).\n";
        return false;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) {
        log += L"  ! Could not fully delete " + dir + L": " + Widen(ec.message().c_str()) + L"\n";
        return false;
    }
    log += L"  - Deleted " + dir + L"\n";
    return true;
}

// ------------------------------------------------------------------ driver settings

// Removes ODDPI's profile from the live driver, keeping any other Raw Accel profiles.
void TurnOffOddpi(std::wstring& log)
{
    try {
        oddpi::DriverVersion();
        const oddpi::Snapshot cur = oddpi::ReadDriver();
        std::vector<const ra::modifier_settings*> others;
        bool had = false;
        for (unsigned i = 0; i < cur.ModifierCount(); ++i) {
            if (wcsncmp(cur.Modifier(i).prof.name, oddpi::kProfileName, ra::MAX_NAME_LEN) == 0) had = true;
            else others.push_back(&cur.Modifier(i));
        }
        if (!had) { log += L"  - ODDPI was not active in the driver.\n"; return; }
        if (others.empty()) {
            oddpi::WriteDriver(oddpi::Snapshot{});
        }
        else {
            std::vector<const ra::device_settings*> devs;
            for (unsigned i = 0; i < cur.DeviceCount(); ++i) devs.push_back(&cur.Device(i));
            oddpi::WriteDriver(oddpi::Snapshot::Build(cur.Base(), others, devs));
        }
        log += L"  - Turned ODDPI off in the Raw Accel driver (mouse is back to normal).\n";
    }
    catch (const ra::install_error&) {
        log += L"  - Raw Accel driver not running, no ODDPI settings to remove.\n";
    }
    catch (const std::exception& e) {
        log += L"  ! Could not turn ODDPI off in the driver: " + Widen(e.what()) + L"\n";
    }
}

// ------------------------------------------------------------------ Raw Accel driver removal (admin)

// Removes every case-insensitive "rawaccel" from a REG_MULTI_SZ list. Returns how many were removed.
int StripFromMultiSz(std::vector<wchar_t>& buf, std::vector<std::wstring>& kept)
{
    int removed = 0;
    kept.clear();
    for (const wchar_t* p = buf.data(); p < buf.data() + buf.size() && *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, kRaName) == 0) ++removed;
        else kept.emplace_back(p);
    }
    return removed;
}

std::vector<wchar_t> JoinMultiSz(const std::vector<std::wstring>& items)
{
    std::vector<wchar_t> out;
    for (auto& s : items) { out.insert(out.end(), s.begin(), s.end()); out.push_back(0); }
    out.push_back(0);
    return out;
}

// Class-wide filter list (where Raw Accel's installer puts itself). Returns false on error.
bool RemoveClassFilter(std::wstring& log, bool& changed)
{
    changed = false;
    DWORD type = 0, size = 0;
    if (!SetupDiGetClassRegistryPropertyW(&GUID_DEVCLASS_MOUSE, SPCRP_UPPERFILTERS, &type, nullptr, 0, &size, nullptr, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_DATA || err == ERROR_FILE_NOT_FOUND) return true;  // no filters at all
        if (err != ERROR_INSUFFICIENT_BUFFER) {
            log += L"  ! Could not read mouse filter list: " + ErrorText(err) + L"\n";
            return false;
        }
    }
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (!SetupDiGetClassRegistryPropertyW(&GUID_DEVCLASS_MOUSE, SPCRP_UPPERFILTERS, &type,
                                          reinterpret_cast<BYTE*>(buf.data()), size, nullptr, nullptr, nullptr)) {
        log += L"  ! Could not read mouse filter list: " + ErrorText(GetLastError()) + L"\n";
        return false;
    }
    std::vector<std::wstring> kept;
    if (StripFromMultiSz(buf, kept) == 0) return true;

    auto joined = JoinMultiSz(kept);
    BOOL ok = kept.empty()
        ? SetupDiSetClassRegistryPropertyW(&GUID_DEVCLASS_MOUSE, SPCRP_UPPERFILTERS, nullptr, 0, nullptr, nullptr)
        : SetupDiSetClassRegistryPropertyW(&GUID_DEVCLASS_MOUSE, SPCRP_UPPERFILTERS,
                                           reinterpret_cast<const BYTE*>(joined.data()),
                                           DWORD(joined.size() * sizeof(wchar_t)), nullptr, nullptr);
    if (!ok) {
        log += L"  ! Could not update mouse filter list: " + ErrorText(GetLastError()) + L"\n";
        return false;
    }
    std::wstring rest;
    for (auto& k : kept) rest += (rest.empty() ? L"" : L", ") + k;
    log += L"  - Removed rawaccel from the mouse filter list (left: " + (rest.empty() ? L"none" : rest) + L").\n";
    changed = true;
    return true;
}

// Per-device filter lists (Raw Accel doesn't use these, but leftovers here would be just as bad).
bool RemoveDeviceFilters(std::wstring& log, bool& changed)
{
    changed = false;
    bool allOk = true;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_MOUSE, nullptr, nullptr, 0);   // incl. not-present
    if (set == INVALID_HANDLE_VALUE) return true;
    SP_DEVINFO_DATA dev{ sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        DWORD type = 0, size = 0;
        SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_UPPERFILTERS, &type, nullptr, 0, &size);
        if (size == 0) continue;
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_UPPERFILTERS, &type,
                                               reinterpret_cast<BYTE*>(buf.data()), size, nullptr)) continue;
        std::vector<std::wstring> kept;
        if (StripFromMultiSz(buf, kept) == 0) continue;
        auto joined = JoinMultiSz(kept);
        BOOL ok = kept.empty()
            ? SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_UPPERFILTERS, nullptr, 0)
            : SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_UPPERFILTERS,
                                                reinterpret_cast<const BYTE*>(joined.data()),
                                                DWORD(joined.size() * sizeof(wchar_t)));
        if (ok) { changed = true; log += L"  - Removed rawaccel from a mouse device's own filter list.\n"; }
        else { allOk = false; log += L"  ! Could not update a mouse device's filter list: " + ErrorText(GetLastError()) + L"\n"; }
    }
    SetupDiDestroyDeviceInfoList(set);
    return allOk;
}

// Marks the kernel service for deletion (its registry key goes away at the next restart,
// or immediately if the driver isn't loaded).
bool RemoveService(std::wstring& log, bool& changed)
{
    changed = false;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { log += L"  ! Could not open the service manager: " + ErrorText(GetLastError()) + L"\n"; return false; }
    bool ok = true;
    SC_HANDLE svc = OpenServiceW(scm, kRaName, DELETE);
    if (!svc) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_DOES_NOT_EXIST) { ok = false; log += L"  ! Could not open the rawaccel service: " + ErrorText(err) + L"\n"; }
    }
    else {
        if (DeleteService(svc)) { changed = true; log += L"  - Removed the rawaccel service registry entry.\n"; }
        else {
            DWORD err = GetLastError();
            if (err == ERROR_SERVICE_MARKED_FOR_DELETE) { changed = true; log += L"  - rawaccel service already marked for removal.\n"; }
            else { ok = false; log += L"  ! Could not remove the rawaccel service: " + ErrorText(err) + L"\n"; }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

// Same as Raw Accel's uninstaller: rename to .tmp and delete that at reboot (the file is in use).
bool RemoveDriverFile(std::wstring& log, bool& changed)
{
    changed = false;
    const fs::path target = Expand(L"%SystemRoot%\\System32\\drivers\\rawaccel.sys");
    const fs::path tmp = target.wstring() + L".tmp";
    std::error_code ec;
    if (target.empty() || !fs::exists(target, ec)) return true;
    changed = true;
    if (MoveFileExW(target.c_str(), tmp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (!MoveFileExW(tmp.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            log += L"  ! Renamed rawaccel.sys but could not schedule its deletion: " + ErrorText(GetLastError()) + L"\n";
            return false;
        }
        log += L"  - rawaccel.sys will be deleted at the next restart.\n";
        return true;
    }
    if (fs::remove(target, ec)) { log += L"  - Deleted rawaccel.sys.\n"; return true; }
    log += L"  ! Could not remove rawaccel.sys: " + ErrorText(GetLastError()) + L"\n";
    return false;
}

// Returns the kExit* flags.
DWORD RemoveRawAccelDriver(std::wstring& log)
{
    DWORD result = kExitOk;
    bool c1 = false, c2 = false, c3 = false, c4 = false;
    bool filtersOk = RemoveClassFilter(log, c1);
    filtersOk = RemoveDeviceFilters(log, c2) && filtersOk;
    if (!filtersOk) {
        // Keep the driver: a filter entry without its driver would break the mouse.
        log += L"  ! Left the rawaccel driver installed because its filter entry could not be removed.\n";
        return kExitFailed;
    }
    if (!RemoveService(log, c3)) result |= kExitFailed;
    if (!RemoveDriverFile(log, c4)) result |= kExitFailed;
    if (c1 || c2 || c3 || c4) result |= kExitReboot;
    else log += L"  - No Raw Accel driver was installed.\n";
    return result;
}

// ------------------------------------------------------------------ ODDPI files

const wchar_t* const kProgramFiles[] = {
    L"oddpi.exe", L"oddpi-gui.exe", L"oddpi.pdb", L"oddpi-gui.pdb", L"uninstaller.pdb",
    L"README.txt", L"README.md", L"LICENSE", L"LICENSE.txt", L"LICENSE-RawAccel.txt", L"CHANGELOG.md",
};
const wchar_t* const kSettingFiles[] = { L"oddpi.ini", L"oddpi.ini.bak", L"games.ini" };

template <size_t N>
void DeleteListed(const fs::path& dir, const wchar_t* const (&names)[N], std::wstring& log)
{
    for (auto* n : names) {
        std::error_code ec;
        fs::path p = dir / n;
        if (!fs::exists(p, ec)) continue;
        if (fs::remove(p, ec)) log += L"  - Deleted " + p.filename().wstring() + L"\n";
        else log += L"  ! Could not delete " + p.wstring() + L": " + Widen(ec.message().c_str()) + L"\n";
    }
}

// After we exit: delete this exe and the folder if it is now empty (rmdir without /s).
void ScheduleSelfDelete(bool removeDir)
{
    std::wstring self = ExePath(), dir = ExeDir();
    std::wstring cmd = L"cmd.exe /d /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + self + L"\"";
    if (removeDir) cmd += L" & rmdir \"" + dir + L"\"";
    std::wstring tmp = fs::temp_directory_path().wstring();
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       tmp.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ------------------------------------------------------------------ elevated helper

// --elevated [--driver] [--folder <path>] [--log <file>]
int RunElevated(int argc, wchar_t** argv)
{
    bool driver = false;
    std::wstring folder, logFile;
    for (int i = 2; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--driver") == 0) driver = true;
        else if (_wcsicmp(argv[i], L"--folder") == 0 && i + 1 < argc) folder = argv[++i];
        else if (_wcsicmp(argv[i], L"--log") == 0 && i + 1 < argc) logFile = argv[++i];
    }
    std::wstring log;
    DWORD code = kExitOk;
    if (driver) code |= RemoveRawAccelDriver(log);
    if (!folder.empty() && !DeleteFolder(folder, log)) code |= kExitFolderFailed;

    if (!logFile.empty()) {
        if (FILE* f = OpenFile(logFile.c_str(), L"wb")) {
            fwrite(log.data(), sizeof(wchar_t), log.size(), f);
            fclose(f);
        }
    }
    return int(code);
}

// Runs this exe elevated. Returns false if the user said no at the UAC prompt.
bool RunElevatedHelper(bool driver, const std::wstring& folder, std::wstring& log, DWORD& code)
{
    wchar_t tmpDir[MAX_PATH], tmpFile[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    GetTempFileNameW(tmpDir, L"odu", 0, tmpFile);

    std::wstring args = L"--elevated";
    if (driver) args += L" --driver";
    if (!folder.empty()) args += L" --folder \"" + folder + L"\"";
    args += L" --log \"" + std::wstring(tmpFile) + L"\"";

    std::wstring self = ExePath();
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) { DeleteFileW(tmpFile); return false; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);

    if (FILE* f = OpenFile(tmpFile, L"rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::wstring s(size_t(n) / sizeof(wchar_t), L'\0');
        if (n > 0) fread(s.data(), sizeof(wchar_t), s.size(), f);
        fclose(f);
        log += s;
    }
    DeleteFileW(tmpFile);
    return true;
}

// ------------------------------------------------------------------ window

enum : int {
    kIdOff = 100, kIdProgram, kIdData, kIdRaDriver, kIdRaFolder,
    kIdUninstall = IDOK, kIdCancel = IDCANCEL,
};

struct Row { int id; const wchar_t* title; std::wstring detail; bool checked, enabled; };

struct App {
    HWND hwnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
    UINT dpi = 96;
    std::wstring raFolder;
    std::vector<Row> rows;
    std::vector<HWND> checks, details;
    HWND intro = nullptr, okBtn = nullptr, cancelBtn = nullptr;
    bool busy = false;
} g;

int S(int v) { return MulDiv(v, int(g.dpi), 96); }

void MakeFonts()
{
    if (g.font) DeleteObject(g.font);
    if (g.bold) DeleteObject(g.bold);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g.bold = CreateFontIndirectW(&ncm.lfMessageFont);
}

void Layout()
{
    const int pad = S(16), w = S(520), checkH = S(22), gap = S(6);

    // Detail lines (paths can be long) get exactly the height their text needs.
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    auto textHeight = [&](HWND h, int width) {
        int len = GetWindowTextLengthW(h);
        std::wstring s(size_t(len) + 1, L'\0');
        GetWindowTextW(h, s.data(), len + 1);
        s.resize(size_t(len));
        RECT r{ 0, 0, width, 0 };
        DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        return int(r.bottom - r.top);
    };
    int y = pad;
    SendMessageW(g.intro, WM_SETFONT, WPARAM(g.font), TRUE);
    const int introH = textHeight(g.intro, w);
    MoveWindow(g.intro, pad, y, w, introH, TRUE);
    y += introH + S(12);
    for (size_t i = 0; i < g.rows.size(); ++i) {
        SendMessageW(g.checks[i], WM_SETFONT, WPARAM(g.bold), TRUE);
        SendMessageW(g.details[i], WM_SETFONT, WPARAM(g.font), TRUE);
        MoveWindow(g.checks[i], pad, y, w, checkH, TRUE);
        y += checkH;
        const int detailH = textHeight(g.details[i], w - S(22));
        MoveWindow(g.details[i], pad + S(22), y, w - S(22), detailH, TRUE);
        y += detailH + gap + S(4);
    }
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    y += S(6);
    const int bw = S(110), bh = S(30);
    SendMessageW(g.okBtn, WM_SETFONT, WPARAM(g.font), TRUE);
    SendMessageW(g.cancelBtn, WM_SETFONT, WPARAM(g.font), TRUE);
    MoveWindow(g.cancelBtn, pad + w - bw, y, bw, bh, TRUE);
    MoveWindow(g.okBtn, pad + w - 2 * bw - S(8), y, bw, bh, TRUE);
    y += bh + pad;

    RECT rc{ 0, 0, w + 2 * pad, y };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, g.dpi);
    SetWindowPos(g.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
}

bool Checked(int id) { return IsDlgButtonChecked(g.hwnd, id) == BST_CHECKED; }

void UpdateButtons()
{
    // Removing the Raw Accel folder without its driver is allowed, but warn in the detail line.
    SetWindowTextW(g.okBtn, (Checked(kIdRaDriver) && !IsElevated()) ? L"Uninstall…" : L"Uninstall");
}

// Asks to close running ODDPI / Raw Accel windows. Returns false if the user cancels.
bool CloseRunning(bool includeRawAccel)
{
    for (;;) {
        auto procs = includeRawAccel ? FindProcesses({ L"oddpi-gui.exe", L"oddpi.exe", L"rawaccel.exe" })
                                     : FindProcesses({ L"oddpi-gui.exe", L"oddpi.exe" });
        if (procs.empty()) return true;
        std::wstring names;
        for (auto& p : procs) names += L"  • " + p.name + L"\n";
        int r = MessageBoxW(g.hwnd,
            (L"These programs are still running:\n\n" + names +
             L"\nClose them for you? Unsaved changes in them will be lost.\n"
             L"(No = I'll close them myself, then try again.)").c_str(),
            kTitle, MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return false;
        if (r == IDYES) for (auto& p : procs) KillProcess(p.pid);
    }
}

void DoUninstall()
{
    const bool program = Checked(kIdProgram), data = Checked(kIdData),
               raDriver = Checked(kIdRaDriver), raFolder = Checked(kIdRaFolder) && !g.raFolder.empty();

    std::wstring confirm = L"This will:\n\n  • turn ODDPI off in the Raw Accel driver\n";
    if (program) confirm += L"  • delete ODDPI's program files\n";
    if (data) confirm += L"  • delete your ODDPI settings, backups, calibration logs and scores\n";
    if (raDriver) confirm += L"  • uninstall the Raw Accel driver (needs admin + a restart)\n";
    if (raFolder) confirm += L"  • delete the Raw Accel folder " + g.raFolder + L"\n";
    confirm += L"\nContinue?";
    if (MessageBoxW(g.hwnd, confirm.c_str(), kTitle, MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    if (!CloseRunning(raDriver || raFolder)) return;

    g.busy = true;
    EnableWindow(g.okBtn, FALSE);
    EnableWindow(g.cancelBtn, FALSE);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring log = L"ODDPI\n";
    TurnOffOddpi(log);   // ~1 s: Raw Accel delays every write

    const fs::path dir = ExeDir();
    if (data) {
        DeleteListed(dir, kSettingFiles, log);
        fs::path appData = fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"ODDPI";
        std::error_code ec;
        if (!appData.has_relative_path() || appData.filename() != L"ODDPI") {}
        else if (fs::exists(appData, ec)) {
            fs::remove_all(appData, ec);
            log += ec ? L"  ! Could not fully delete " + appData.wstring() + L": " + Widen(ec.message().c_str()) + L"\n"
                      : L"  - Deleted " + appData.wstring() + L" (backups, calibration logs, scores)\n";
        }
    }
    if (program) DeleteListed(dir, kProgramFiles, log);

    // Raw Accel: driver always needs admin; the folder only if we can't delete it ourselves.
    bool reboot = false, raFailed = false;
    if (raDriver || raFolder) {
        log += L"\nRaw Accel\n";
        std::wstring folderForHelper;
        if (raFolder) {
            const bool userOwned = IsUnder(g.raFolder, KnownFolder(FOLDERID_Profile));
            if (userOwned || IsElevated()) { if (!DeleteFolder(g.raFolder, log)) raFailed = true; }
            else folderForHelper = g.raFolder;
        }
        if (raDriver || !folderForHelper.empty()) {
            if (IsElevated()) {
                DWORD code = kExitOk;
                if (raDriver) code |= RemoveRawAccelDriver(log);
                if (!folderForHelper.empty() && !DeleteFolder(folderForHelper, log)) code |= kExitFolderFailed;
                reboot = (code & kExitReboot) != 0;
                raFailed |= (code & (kExitFailed | kExitFolderFailed)) != 0;
            }
            else {
                DWORD code = kExitOk;
                if (!RunElevatedHelper(raDriver, folderForHelper, log, code)) {
                    log += L"  ! Admin permission was declined, Raw Accel was left installed.\n";
                    raFailed = true;
                }
                else {
                    reboot = (code & kExitReboot) != 0;
                    raFailed |= (code & (kExitFailed | kExitFolderFailed)) != 0;
                }
            }
        }
    }

    // Is anything of ours left in the folder? (rmdir only removes it if empty anyway.)
    if (program) ScheduleSelfDelete(true);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    const bool problems = log.find(L"  ! ") != std::wstring::npos;
    std::wstring msg = (problems ? L"Finished, with problems:\n\n" : L"ODDPI was uninstalled.\n\n") + log;
    if (!program) msg += L"\nThe program files were kept; you can delete the folder yourself any time.\n";
    if (reboot) {
        msg += L"\nRaw Accel is removed after a restart. Until then your mouse works as before"
               L" (ODDPI is already off).\n\nRestart now?";
        if (MessageBoxW(g.hwnd, msg.c_str(), kTitle, MB_YESNO | (problems ? MB_ICONWARNING : MB_ICONINFORMATION)) == IDYES) {
            std::wstring cmd = Expand(L"%SystemRoot%\\System32\\shutdown.exe") + L" /r /t 5 /c \"Finishing Raw Accel removal\"";
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    else {
        MessageBoxW(g.hwnd, msg.c_str(), kTitle, MB_OK | (problems ? MB_ICONWARNING : MB_ICONINFORMATION));
    }
    (void)raFailed;
    DestroyWindow(g.hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (g.busy) return 0;
        if (LOWORD(wp) == kIdUninstall) { DoUninstall(); return 0; }
        if (LOWORD(wp) == kIdCancel) { DestroyWindow(hwnd); return 0; }
        if (HIWORD(wp) == BN_CLICKED) UpdateButtons();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = HDC(wp);
        SetBkMode(dc, TRANSPARENT);
        for (HWND d : g.details) {
            if (HWND(lp) == d) { SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT)); break; }
        }
        return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        MakeFonts();
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        Layout();
        return 0;
    }
    case WM_CLOSE:
        if (!g.busy) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunWindow(HINSTANCE inst)
{
    const fs::path dir = ExeDir();
    const std::wstring appData = (fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"ODDPI").wstring();
    g.raFolder = FindRawAccelFolder();

    std::error_code ec;
    const bool raInstalled = fs::exists(Expand(L"%SystemRoot%\\System32\\drivers\\rawaccel.sys"), ec);

    g.rows = {
        { kIdOff, L"Turn ODDPI off in the Raw Accel driver",
          L"Always done: your mouse goes back to normal right away. Other Raw Accel profiles are kept.", true, false },
        { kIdProgram, L"ODDPI program files",
          L"oddpi.exe, oddpi-gui.exe, this uninstaller and the readme files in " + dir.wstring(), true, true },
        { kIdData, L"ODDPI settings and data",
          L"oddpi.ini, games.ini and " + appData + L" (backups, calibration logs, Gridshot scores)", true, true },
        { kIdRaDriver, L"Raw Accel driver",
          raInstalled ? std::wstring(L"Removes the driver and its registry entries (mouse filter, service). Needs admin and a restart.")
                      : std::wstring(L"Not installed."), false, raInstalled },
        { kIdRaFolder, L"Raw Accel program folder",
          g.raFolder.empty() ? std::wstring(L"Not found. If you have one, delete it yourself.") : g.raFolder, false, !g.raFolder.empty() },
    };

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"ODDPI_Uninstaller";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g.hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 600, 400, nullptr, nullptr, inst, nullptr);
    g.dpi = GetDpiForWindow(g.hwnd);
    MakeFonts();

    g.intro = CreateWindowExW(0, L"STATIC",
        L"Choose what to remove. ODDPI never changed anything else on your PC: "
        L"Raw Accel is a separate program you installed yourself, so it stays unless you tick it.",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g.hwnd, nullptr, inst, nullptr);
    for (auto& r : g.rows) {
        HWND c = CreateWindowExW(0, L"BUTTON", r.title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 0, 0, g.hwnd, HMENU(INT_PTR(r.id)), inst, nullptr);
        CheckDlgButton(g.hwnd, r.id, r.checked ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(c, r.enabled);
        HWND d = CreateWindowExW(0, L"STATIC", r.detail.c_str(), WS_CHILD | WS_VISIBLE | SS_EDITCONTROL | SS_NOPREFIX,
                                 0, 0, 0, 0, g.hwnd, nullptr, inst, nullptr);
        g.checks.push_back(c);
        g.details.push_back(d);
    }
    g.okBtn = CreateWindowExW(0, L"BUTTON", L"Uninstall", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 0, 0, g.hwnd, HMENU(INT_PTR(kIdUninstall)), inst, nullptr);
    g.cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 0, 0, g.hwnd, HMENU(INT_PTR(kIdCancel)), inst, nullptr);
    Layout();
    UpdateButtons();

    // Centre on the monitor.
    RECT wr;
    GetWindowRect(g.hwnd, &wr);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(g.hwnd, nullptr,
                 mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (wr.right - wr.left)) / 2,
                 mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (wr.bottom - wr.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g.hwnd, SW_SHOW);
    SetFocus(g.okBtn);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g.hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && _wcsicmp(argv[1], L"--report") == 0) {
        // Read-only: what would be found. Changes nothing.
        std::wstring r = L"exeDir=" + ExeDir() + L"\nrawAccelFolder=" + FindRawAccelFolder() + L"\n";
        std::error_code ec;
        r += L"driverFile=" + std::to_wstring(fs::exists(Expand(L"%SystemRoot%\\System32\\drivers\\rawaccel.sys"), ec)) + L"\n";
        DWORD type = 0, size = 0;
        SetupDiGetClassRegistryPropertyW(&GUID_DEVCLASS_MOUSE, SPCRP_UPPERFILTERS, &type, nullptr, 0, &size, nullptr, nullptr);
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        if (size && SetupDiGetClassRegistryPropertyW(&GUID_DEVCLASS_MOUSE, SPCRP_UPPERFILTERS, &type,
                                                     reinterpret_cast<BYTE*>(buf.data()), size, nullptr, nullptr, nullptr)) {
            std::vector<std::wstring> kept;
            int n = StripFromMultiSz(buf, kept);
            r += L"classFilterHasRawaccel=" + std::to_wstring(n) + L"\nclassFilterAfter=";
            for (auto& k : kept) r += k + L";";
            r += L"\n";
        }
        if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
            SC_HANDLE s = OpenServiceW(scm, kRaName, SERVICE_QUERY_STATUS);
            r += L"service=" + std::to_wstring(s != nullptr) + L"\n";
            if (s) CloseServiceHandle(s);
            CloseServiceHandle(scm);
        }
        if (FILE* f = OpenFile(argv[2], L"w, ccs=UTF-8")) { fputws(r.c_str(), f); fclose(f); }
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--elevated") == 0) {
        if (!IsElevated()) return int(kExitFailed);
        return RunElevated(argc, argv);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    return RunWindow(inst);
}
