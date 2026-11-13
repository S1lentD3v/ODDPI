#pragma once

#include <string>
#include <vector>

namespace oddpi {

    enum class DynamicMode { Off, Classic, Natural, Jump, Power, Synchronous };
    enum class CapMode { Output, Input, InputOutput };

    // Everything the user can set in oddpi.ini. Defaults = "no change to the mouse".
    struct Config {
        // [General]
        double sensitivity = 1.0;
        double rotation = 0.0;      // degrees, positive = clockwise on screen
        double angleSnap = 0.0;     // degrees, 0..45, 0 = off
        int mouseDpi = 0;           // 0 = leave Raw Accel's value alone

        // [Directional] multipliers per direction of hand movement
        double right = 1.0;
        double left = 1.0;
        double up = 1.0;
        double down = 1.0;
        double blendHorizontal = 1.0;
        double blendVertical = 1.0;

        // [Dynamic] speed-based sensitivity (Raw Accel curve parameters)
        DynamicMode mode = DynamicMode::Off;
        bool gain = true;
        double acceleration = 0.005;
        double exponent = 2.0;
        double inputOffset = 0.0;
        double outputOffset = 0.0;
        double decayRate = 0.1;
        double limit = 1.5;
        double scale = 1.0;
        double powerExponent = 0.05;
        double gamma = 1.0;
        double motivity = 1.5;
        double syncSpeed = 5.0;
        double smooth = 0.5;
        CapMode capMode = CapMode::Output;
        double capInput = 15.0;
        double capOutput = 1.5;
        double horizontalStrength = 1.0;
        double verticalStrength = 1.0;

        // [Game] the game you play, so calibration flicks match your real aim (see Games.h).
        // Not used by the driver.
        std::wstring game;            // name or alias from the games list; empty/"none" = not set
        double gameSensitivity = 0;   // in-game sensitivity
        double cm360 = 0;             // alternative to Game + Sensitivity
        double fov = 103;             // horizontal field of view in degrees, for the test's screen scale

        // [Trainer] calibration test, sens finder and Gridshot
        bool sound = true;
        double volume = 0.6;          // 0..1
    };

    // Loads an INI file. Missing keys keep their defaults. Returns false on hard errors.
    // Unknown keys are reported in `warnings` (usually a typo).
    bool LoadConfig(const std::wstring& path, Config& cfg,
                    std::vector<std::wstring>& errors, std::vector<std::wstring>& warnings);

    // Writes a fully commented default config. Fails if the file already exists.
    bool WriteDefaultConfig(const std::wstring& path, std::wstring& error);

    // Writes every setting into the file, keeping its comments (creates a commented default
    // file first if it does not exist).
    bool SaveConfig(const std::wstring& path, const Config& cfg, std::wstring& error);

    const wchar_t* ModeName(DynamicMode mode);
    const wchar_t* CapModeName(CapMode mode);
}
