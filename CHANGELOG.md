# Changelog

## v1.1.0 (2026-11-13)

### Added
- `uninstaller.exe`: a checklist that removes ODDPI's program files and your settings/data, and turns ODDPI off in the driver (other Raw Accel profiles are kept)
- Optional in the uninstaller: remove the **Raw Accel driver** and its registry entries (mouse filter, service, driver file; needs admin and a restart) and the **Raw Accel program folder**. This also cleans up the `rawaccel` service key that Raw Accel's own uninstaller leaves behind.

## v1.0.0 (2026-11-13)

First public release.

### Settings
- Per-direction sensitivity (`Right` / `Left` / `Up` / `Down`), with optional smooth blending by angle
- Rotation and angle snapping
- Speed curves: Classic, Natural, Jump, Power, Synchronous, with horizontal/vertical strength
- Settings window with a live polar diagram of sensitivity in every direction, computed with Raw Accel's own math
- ODDPI on/off switch; every apply is backed up, with **Undo last apply** and **Restore original**

### Aim tests
- 3D calibration test that measures 16 directions and fits `Right` / `Left` / `Up` / `Down` and `Rotation`, stopping automatically once the results are precise enough
- Sens finder: blind A/B rounds that narrow down your best in-game sensitivity
- Gridshot: a 60-second aim test with an improvement board (rolling average, personal best, first vs recent runs, ODDPI on vs off)
- Sound effects in all tests (`[Trainer] Sound` / `Volume`)

### Games
- Built-in sensitivity scales for CS2, CS:GO, Apex Legends, TF2, Titanfall 2, Quake Champions, Valorant, Overwatch 2, Call of Duty, Fortnite and Marvel Rivals; add more in `games.ini`
- Sens converter (cm/360 and matching sensitivity across games)

### Other
- Full command-line interface (`oddpi.exe`)
- Portable: the C++ runtime is linked statically, so nothing needs to be installed except Raw Accel