#include "Driver.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace oddpi {

namespace {

    constexpr size_t kBaseSize = sizeof(ra::io_base);
    constexpr size_t kModSize = sizeof(ra::modifier_settings);
    constexpr size_t kDevSize = sizeof(ra::device_settings);

    size_t TotalSize(const ra::io_base& b)
    {
        return kBaseSize + size_t(b.modifier_data_size) * kModSize + size_t(b.device_data_size) * kDevSize;
    }

    // Like ra::io_control, but reports how many bytes the driver returned so the
    // struct layout can be checked against the installed driver.
    DWORD Ioctl(DWORD code, void* in, DWORD inSize, void* out, DWORD outSize)
    {
        HANDLE h = CreateFileW(L"\\\\.\\rawaccel", 0, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw ra::install_error();

        DWORD returned = 0;
        BOOL ok = DeviceIoControl(h, code, in, inSize, out, outSize, &returned, nullptr);
        DWORD err = GetLastError();
        CloseHandle(h);

        if (!ok) throw ra::sys_error("Raw Accel driver request failed", err);
        return returned;
    }

    std::string VersionString(ra::version_t v)
    {
        return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    }

    [[noreturn]] void ThrowLayoutMismatch()
    {
        throw ra::error(
            "The installed Raw Accel driver uses a different settings layout than this ODDPI build "
            "(built against Raw Accel " RA_VER_STRING "). Nothing was changed.");
    }

    struct BackupHeader {
        char magic[8];
        int32_t major, minor, patch;
        uint32_t baseSize, modSize, devSize;
        uint64_t payloadSize;
    };

    constexpr char kMagic[8] = { 'O', 'D', 'D', 'P', 'I', 'B', 'K', '1' };

    std::wstring Timestamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t buf[32];
        swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return buf;
    }

    void WriteBackupFile(const fs::path& path, const Snapshot& snap, ra::version_t v)
    {
        BackupHeader h{};
        std::memcpy(h.magic, kMagic, sizeof(kMagic));
        h.major = v.major;
        h.minor = v.minor;
        h.patch = v.patch;
        h.baseSize = uint32_t(kBaseSize);
        h.modSize = uint32_t(kModSize);
        h.devSize = uint32_t(kDevSize);
        h.payloadSize = snap.bytes.size();

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(reinterpret_cast<const char*>(snap.bytes.data()), std::streamsize(snap.bytes.size()));
        out.close();
        if (!out) throw ra::error("could not write backup file in %LOCALAPPDATA%\\ODDPI\\backups");
    }
}

// ---------------------------------------------------------------- Snapshot

Snapshot::Snapshot() : bytes(kBaseSize)
{
    const ra::io_base empty{};
    std::memcpy(bytes.data(), &empty, kBaseSize);
}

const ra::io_base& Snapshot::Base() const { return *reinterpret_cast<const ra::io_base*>(bytes.data()); }
ra::io_base& Snapshot::Base() { return *reinterpret_cast<ra::io_base*>(bytes.data()); }

const ra::modifier_settings& Snapshot::Modifier(unsigned i) const
{
    return *reinterpret_cast<const ra::modifier_settings*>(bytes.data() + kBaseSize + size_t(i) * kModSize);
}

const ra::device_settings& Snapshot::Device(unsigned i) const
{
    size_t offset = kBaseSize + size_t(ModifierCount()) * kModSize + size_t(i) * kDevSize;
    return *reinterpret_cast<const ra::device_settings*>(bytes.data() + offset);
}

Snapshot Snapshot::Build(const ra::io_base& base,
                         const std::vector<const ra::modifier_settings*>& modifiers,
                         const std::vector<const ra::device_settings*>& devices)
{
    ra::io_base header = base;
    header.modifier_data_size = unsigned(modifiers.size());
    header.device_data_size = unsigned(devices.size());

    Snapshot s;
    s.bytes.resize(TotalSize(header));
    std::byte* p = s.bytes.data();

    std::memcpy(p, &header, kBaseSize);
    p += kBaseSize;
    for (auto* m : modifiers) { std::memcpy(p, m, kModSize); p += kModSize; }
    for (auto* d : devices) { std::memcpy(p, d, kDevSize); p += kDevSize; }

    return s;
}

// ---------------------------------------------------------------- driver I/O

ra::version_t DriverVersion()
{
    return ra::valid_version_or_throw();
}

Snapshot ReadDriver()
{
    ra::io_base base{};
    if (Ioctl(ra::READ, nullptr, 0, &base, DWORD(kBaseSize)) != kBaseSize) ThrowLayoutMismatch();

    Snapshot s;
    s.bytes.resize(TotalSize(base));

    if (s.bytes.size() == kBaseSize) {
        std::memcpy(s.bytes.data(), &base, kBaseSize);
        return s;
    }

    // If our structs were a different size than the driver's, it would return a different
    // byte count here (only the base, or its own total). Either way: refuse to continue.
    DWORD got = Ioctl(ra::READ, nullptr, 0, s.bytes.data(), DWORD(s.bytes.size()));
    if (got != s.bytes.size()) ThrowLayoutMismatch();

    return s;
}

void WriteDriver(const Snapshot& snap)
{
    if (snap.bytes.size() != TotalSize(snap.Base())) {
        throw ra::error("internal error: settings buffer size does not match its header");
    }

    if (snap.Empty()) ra::reset();
    else ra::write(snap.bytes.data());

    // The driver stores the buffer verbatim, so reading it back must give identical bytes.
    Snapshot check = ReadDriver();
    bool same = check.bytes.size() == snap.bytes.size() &&
        std::memcmp(check.bytes.data(), snap.bytes.data(), snap.bytes.size()) == 0;
    if (!same && !(snap.Empty() && check.Empty())) {
        throw ra::error("the Raw Accel driver did not keep the new settings (read-back mismatch)");
    }
}

// ---------------------------------------------------------------- backups

std::wstring BackupDirectory()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path root = (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::temp_directory_path();
    fs::path dir = root / L"ODDPI" / L"backups";
    fs::create_directories(dir);
    return dir.wstring();
}

std::wstring OriginalBackupPath()
{
    return (fs::path(BackupDirectory()) / L"original.bin").wstring();
}

std::wstring SaveBackup(const Snapshot& snap, ra::version_t version)
{
    fs::path dir = BackupDirectory();

    // Keep the very first state forever, so the user can always get back to it.
    fs::path original = OriginalBackupPath();
    if (!fs::exists(original)) WriteBackupFile(original, snap, version);

    std::wstring stamp = Timestamp();
    fs::path path = dir / (L"backup-" + stamp + L".bin");
    for (int i = 2; fs::exists(path); ++i) {
        path = dir / (L"backup-" + stamp + L"-" + std::to_wstring(i) + L".bin");
    }

    WriteBackupFile(path, snap, version);
    return path.wstring();
}

Snapshot LoadBackup(const std::wstring& path, ra::version_t driverVersion)
{
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) throw ra::error("could not open backup file");

    BackupHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in || std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) {
        throw ra::error("not an ODDPI backup file");
    }

    ra::version_t made = { h.major, h.minor, h.patch };
    if (made < driverVersion || driverVersion < made) {
        throw ra::error("this backup was made with Raw Accel " + VersionString(made) +
                        " but " + VersionString(driverVersion) + " is installed");
    }
    if (h.baseSize != kBaseSize || h.modSize != kModSize || h.devSize != kDevSize) {
        ThrowLayoutMismatch();
    }

    Snapshot s;
    s.bytes.resize(size_t(h.payloadSize));
    if (s.bytes.size() < kBaseSize) throw ra::error("backup file is corrupt");
    in.read(reinterpret_cast<char*>(s.bytes.data()), std::streamsize(s.bytes.size()));
    if (!in || s.bytes.size() != TotalSize(s.Base())) throw ra::error("backup file is corrupt");

    return s;
}

std::vector<std::wstring> ListBackups()
{
    std::vector<std::wstring> files;
    for (auto& entry : fs::directory_iterator(BackupDirectory())) {
        auto name = entry.path().filename().wstring();
        if (entry.is_regular_file() && name.rfind(L"backup-", 0) == 0 && entry.path().extension() == L".bin") {
            files.push_back(entry.path().wstring());
        }
    }
    std::sort(files.rbegin(), files.rend());
    return files;
}

}  // namespace oddpi
