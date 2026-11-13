#pragma once

// The operations shared by the command line (main.cpp) and the GUI (gui.cpp).
// Driver failures are thrown (ra::install_error when Raw Accel is missing, ra::error / std::exception otherwise).

#include "RawAccel.h"
#include "Config.h"
#include "Driver.h"
#include "Calibrate.h"

#include <string>
#include <vector>

namespace oddpi {

    bool IsOddpiProfile(const ra::profile& p);

    // True when ODDPI is the default (first) profile in the driver.
    bool IsOddpiActive(const Snapshot& s);

    // Explains which mice ODDPI does NOT reach (or reaches differently). One message per entry.
    std::vector<std::wstring> CoverageNotes(const Snapshot& s, int effectiveDpi);

    struct ApplyOutcome {
        std::wstring backup;                 // backup of the previous driver state
        std::vector<std::wstring> notes;     // CoverageNotes after applying
    };

    // Validates the config, backs up the current driver state and makes the config live.
    // Returns false (with `errors`) when the config is invalid; nothing is changed then.
    bool ApplyConfig(const Config& cfg, ApplyOutcome& outcome, std::vector<std::wstring>& errors);

    // Turns ODDPI off: removes its profile from the driver and keeps any other Raw Accel profiles
    // (no profiles left = Raw Accel passes the mouse through unchanged). Backs up first.
    // Returns false if ODDPI was not in the driver (nothing to do).
    bool DisableOddpi(std::wstring& backup);

    // True when the driver holds an ODDPI profile at all (active or not).
    bool HasOddpiProfile(const Snapshot& s);

    // arg: empty = latest backup, "original" = before ODDPI was first used, else a file path.
    // Returns the restored file. `empty` tells whether the driver now has no settings at all.
    std::wstring RestoreBackup(const std::wstring& arg, bool& empty);

    // Calibration logs live in %LOCALAPPDATA%\ODDPI\calibrations.
    std::wstring CalibrationDirectory();
    std::vector<std::wstring> ListCalibrationLogs();   // newest first
    std::wstring SaveCalibrationLog(const CalOptions& opt, const std::vector<CalTrial>& trials);  // "" on failure

    // The config with the calibration's corrections multiplied in.
    Config WithCalibration(const Config& cfg, const CalResult& r);
}
