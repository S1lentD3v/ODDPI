#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Games.h"

#include <cwctype>
#include <filesystem>

namespace oddpi {

namespace {

    std::wstring Normalize(const std::wstring& s)
    {
        std::wstring out;
        for (wchar_t c : s) {
            if (iswalnum(c)) out += wchar_t(towlower(c));
        }
        return out;
    }

    std::wstring Trim(std::wstring s)
    {
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        size_t i = 0;
        while (i < s.size() && iswspace(s[i])) ++i;
        return s.substr(i);
    }

    std::vector<GameInfo> BuiltIn()
    {
        // Widely agreed values (several independent sources give the same number).
        // Source-engine family: m_yaw 0.022.
        return {
            { L"Counter-Strike 2", { L"cs2", L"cs", L"counterstrike" }, 0.022, L"m_yaw 0.022" },
            { L"CS:GO", { L"csgo" }, 0.022, L"m_yaw 0.022" },
            { L"Apex Legends", { L"apex" }, 0.022, L"Source-based, same as CS" },
            { L"Team Fortress 2", { L"tf2" }, 0.022, L"m_yaw 0.022" },
            { L"Titanfall 2", { L"titanfall" }, 0.022, L"Source-based, same as CS" },
            { L"Quake Champions", { L"qc", L"quake" }, 0.022, L"same as CS" },
            { L"Valorant", { L"val", L"valo" }, 0.07, L"CS sens / 3.18" },
            { L"Overwatch 2", { L"ow", L"ow2", L"overwatch" }, 0.0066, L"CS sens x 3.33" },
            { L"Call of Duty", { L"cod", L"warzone", L"mw", L"bo6" }, 0.0066,
              L"hip-fire, Legacy sensitivity type (MW2019 and later)" },
            // Community estimates: fewer or inconsistent sources. Verify before relying on them.
            { L"Fortnite", { L"fn" }, 0.005555, L"sensitivity = X-axis slider %, e.g. 6.5", true },
            { L"Marvel Rivals", { L"rivals", L"mr" }, 0.0175, L"derived from converter ratios", true },
        };
    }
}

std::vector<GameInfo> LoadGames(const std::wstring& directory)
{
    std::vector<GameInfo> games = BuiltIn();

    const std::wstring path = (std::filesystem::path(directory) / L"games.ini").wstring();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return games;

    std::vector<wchar_t> buf(65536);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), DWORD(buf.size()), path.c_str());
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        const std::wstring name = p;
        wchar_t value[512];
        GetPrivateProfileStringW(name.c_str(), L"Yaw", L"", value, 512, path.c_str());
        wchar_t* end = nullptr;
        const double yaw = wcstod(value, &end);
        if (end == value || !(yaw > 0)) continue;

        GameInfo g;
        g.name = name;
        g.yaw = yaw;
        g.user = true;
        GetPrivateProfileStringW(name.c_str(), L"Note", L"", value, 512, path.c_str());
        g.note = value;
        GetPrivateProfileStringW(name.c_str(), L"Aliases", L"", value, 512, path.c_str());
        std::wstring aliases = value;
        size_t start = 0;
        while (start <= aliases.size()) {
            size_t comma = aliases.find(L',', start);
            std::wstring a = Trim(aliases.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (!a.empty()) g.aliases.push_back(a);
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }

        bool replaced = false;
        for (auto& existing : games) {
            if (Normalize(existing.name) == Normalize(g.name)) {
                existing = g;
                replaced = true;
            }
        }
        if (!replaced) games.push_back(g);
    }
    return games;
}

const GameInfo* FindGame(const std::vector<GameInfo>& games, const std::wstring& nameOrAlias)
{
    const std::wstring key = Normalize(nameOrAlias);
    if (key.empty()) return nullptr;
    for (const auto& g : games) {
        if (Normalize(g.name) == key) return &g;
        for (const auto& a : g.aliases) {
            if (Normalize(a) == key) return &g;
        }
    }
    return nullptr;
}

double Cm360(double sens, double yaw, int dpi)
{
    if (!(sens > 0) || !(yaw > 0) || dpi <= 0) return 0;
    return 360.0 / (sens * yaw) / dpi * 2.54;
}

double CountsPerDegree(const Config& cfg, const std::vector<GameInfo>& games, int dpi, std::wstring& label)
{
    wchar_t buf[256];
    if (cfg.cm360 > 0 && dpi > 0) {
        swprintf_s(buf, L"%.1f cm/360", cfg.cm360);
        label = buf;
        return cfg.cm360 / 2.54 * dpi / 360.0;
    }
    const GameInfo* g = FindGame(games, cfg.game);
    if (g && cfg.gameSensitivity > 0) {
        const double cm = Cm360(cfg.gameSensitivity, g->yaw, dpi);
        if (cm > 0) swprintf_s(buf, L"%ls %g = %.1f cm/360", g->name.c_str(), cfg.gameSensitivity, cm);
        else swprintf_s(buf, L"%ls %g", g->name.c_str(), cfg.gameSensitivity);
        label = buf;
        return 1.0 / (cfg.gameSensitivity * g->yaw);
    }
    label.clear();
    return 0;
}

}  // namespace oddpi
