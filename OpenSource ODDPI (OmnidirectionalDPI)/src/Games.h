#pragma once

// Game sensitivity table ("yaw" = degrees the view turns per mouse count at in-game sensitivity 1).
//
//   degrees per count = sensitivity * yaw
//   cm/360            = 360 / (sensitivity * yaw) / DPI * 2.54
//   sens in game B    = sens in game A * yawA / yawB
//
// Values are ODDPI's own, checked against several public sources; nothing is copied from a
// converter site's database. Users can add or override games in games.ini next to the exe:
//
//   [My Game]
//   Yaw=0.022
//   Aliases=mg, mygame
//   Note=anything

#include "Config.h"

#include <string>
#include <vector>

namespace oddpi {

    struct GameInfo {
        std::wstring name;
        std::vector<std::wstring> aliases;
        double yaw = 0;
        std::wstring note;
        bool estimate = false;   // community estimate, not widely agreed
        bool user = false;       // from games.ini
    };

    // Built-in list plus games.ini from `directory` (user entries override by name).
    std::vector<GameInfo> LoadGames(const std::wstring& directory);

    // Case/punctuation-insensitive match on name or alias. nullptr if not found.
    const GameInfo* FindGame(const std::vector<GameInfo>& games, const std::wstring& nameOrAlias);

    double Cm360(double sens, double yaw, int dpi);

    // Mouse counts per degree of view rotation for the game set in the config ([Game] section),
    // or 0 when unknown. `label` describes it, e.g. "Valorant 0.35 = 37.0 cm/360".
    double CountsPerDegree(const Config& cfg, const std::vector<GameInfo>& games, int dpi, std::wstring& label);
}
