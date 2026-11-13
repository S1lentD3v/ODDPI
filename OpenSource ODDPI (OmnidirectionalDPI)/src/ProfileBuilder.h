#pragma once

#include "RawAccel.h"
#include "Config.h"

#include <string>
#include <vector>

namespace oddpi {

    // Name of the Raw Accel profile ODDPI owns. It is always placed first, which makes it
    // the default profile for every mouse that has no device-specific mapping.
    inline constexpr wchar_t kProfileName[] = L"ODDPI";

    // The DPI Raw Accel will normalise the mouse by: MouseDPI from the config, else the DPI
    // already set in Raw Accel's default device config, else 0 (no normalisation).
    // Raw Accel scales output by OutputDPI / mouseDPI, so the profile must be built from
    // this value for Sensitivity=1.0 to really mean "unchanged".
    int EffectiveDpi(const Config& cfg, int driverDefaultDpi);

    // Translates an ODDPI config into a Raw Accel profile and runs Raw Accel's own validator.
    bool BuildProfile(const Config& cfg, int effectiveDpi, ra::profile& out, std::vector<std::wstring>& errors);

    // Runs movement through Raw Accel's exact driver math (ra::modifier) for previews.
    class PreviewModel {
    public:
        PreviewModel(const ra::profile& profile, int effectiveDpi);
        ~PreviewModel();
        PreviewModel(const PreviewModel&) = delete;
        PreviewModel& operator=(const PreviewModel&) = delete;

        struct Point { double angle; double sens; };  // cursor angle (deg), length out/in
        // degrees: hand direction (0 = right, 90 = up). speed: in/s at the mouse DPI
        // (counts/ms when no DPI is known), as one 1 ms report.
        Point At(double degrees, double speed) const;

    private:
        struct Impl;
        Impl* impl_;
    };

    // Prints how the profile transforms movement in every direction, using Raw Accel's
    // exact driver math (ra::modifier), so the preview matches what the driver will do.
    void PrintPreview(const Config& cfg, int effectiveDpi, const ra::profile& profile);

    // One-line summary of any Raw Accel profile (used by `oddpi status`).
    std::wstring DescribeProfile(const ra::profile& profile);
}
