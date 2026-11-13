#pragma once

#include "RawAccel.h"

#include <cstddef>
#include <string>
#include <vector>

namespace oddpi {

    // A complete copy of the Raw Accel driver's settings, in the driver's own wire format:
    //   io_base | modifier_settings[modifier_data_size] | device_settings[device_data_size]
    class Snapshot {
    public:
        Snapshot();

        const ra::io_base& Base() const;
        ra::io_base& Base();

        unsigned ModifierCount() const { return Base().modifier_data_size; }
        unsigned DeviceCount() const { return Base().device_data_size; }

        const ra::modifier_settings& Modifier(unsigned i) const;
        const ra::device_settings& Device(unsigned i) const;

        // True when the driver holds no settings at all (Raw Accel is effectively off).
        bool Empty() const { return ModifierCount() == 0; }

        // Builds a new snapshot from parts (sizes in the base are set automatically).
        static Snapshot Build(const ra::io_base& base,
                              const std::vector<const ra::modifier_settings*>& modifiers,
                              const std::vector<const ra::device_settings*>& devices);

        std::vector<std::byte> bytes;
    };

    // Throws ra::error (or a subclass) with a readable message on failure.
    ra::version_t DriverVersion();          // validates the driver is compatible
    Snapshot ReadDriver();
    void WriteDriver(const Snapshot& snap); // takes ~1 s: Raw Accel delays every write

    // Backups live in %LOCALAPPDATA%\ODDPI\backups
    std::wstring BackupDirectory();
    std::wstring OriginalBackupPath();      // first-ever state, never overwritten
    std::wstring SaveBackup(const Snapshot& snap, ra::version_t version);  // returns the file written
    Snapshot LoadBackup(const std::wstring& path, ra::version_t driverVersion);
    std::vector<std::wstring> ListBackups(); // newest first, excludes the original
}
