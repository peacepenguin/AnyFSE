# Desktop Xbox startup

AnyFSE ports the XboxStartupEnabler patcher into its native C++ runtime. The
implementation is adapted from `peacepenguin/XboxStartupEnabler` at commit
`e52ab92951f56b7c1ee404fa9c870a6a3d2732e5` (MIT; see
`THIRD_PARTY_NOTICES/XboxStartupEnabler.txt`). No separate .NET application is
required.

## Enable and test

1. Build/install AnyFSE on a Windows version with the Gaming Fullscreen Experience
   APIs. This does not add those APIs to older Windows builds.
2. In AnyFSE settings, choose **Enable Xbox startup on PC**. This explicitly patches
   `gamemode.dll`, `SettingsHandlers_Gaming.dll`, and `twinui.pcshell.dll` through
   AnyFSE's existing elevated scheduled-task handler.
3. Sign out or restart to load the changed Windows components. The operation does
   not reboot or change startup settings automatically.
4. Choose your Home app and turn on **Enter FSE on startup** in AnyFSE. For testing
   the Microsoft Xbox home screen, choose the native Xbox launcher. For testing
   AnyFSE's launcher, choose Playnite, Steam, or another supported launcher.
5. Sign in again and confirm the selected Home app starts. Verify the same settings
   are exposed in Windows Settings > Gaming.

New activation does not change `OEM\DeviceForm`. When migrating an older AnyFSE
form-factor override, activation restores the prior value using AnyFSE's existing
registry backup. Real handhelds continue to work without needing these patches.

`AnyFSE.exe /XboxStartup status` (or `verify`) inspects the three DLLs and shows
per-file state, also writing it to the AnyFSE log. Inspection works before the
normal FSE API availability check. `apply` and `restore` request the same elevated
handlers used by settings; they require an installed AnyFSE elevation task.

## Compatibility and restoration

The recognized layout is the upstream six-site layout: three gaming API changes,
two Settings checks, and one shell check. Matching byte signatures must be in
executable x64 PE sections. Missing exports, unknown prologues, ambiguous branches,
unexpected site counts, malformed PE data, and partially patched individual images
are refused. The upstream project documents testing Windows 11 build 26200.8457;
its screenshot also records the six sites on the 26100 DLL family. Matching patterns
are a compatibility check, not a guarantee for every Windows release.

All three images are validated before replacement. Original bytes are backed up
under `%ProgramData%\AnyFSE-XboxStartupBackups`, keyed by the SHA-256 of the full
patched image. Backup files are retained across upgrades and uninstall. Replacement
preserves the original owner and DACL and schedules old loaded images for removal
at the next reboot. Completed file replacements are rolled back if a later target
fails; rollback failures are logged with the affected path.

Use **Restore desktop Xbox startup changes** before uninstalling when you want to
undo the patches. Restoration validates that applying the known patches to the
saved original reproduces the *entire current file*. It refuses to restore a stale
backup over a newer Windows update. Files that are already original are skipped.
If the standalone XboxStartupEnabler already applied the patches, restore them
with that tool first if you want AnyFSE-managed restoration.

Windows servicing and `sfc /scannow` can replace patched files. AnyFSE does not
silently reapply patches at logon, during installation, or after an update.

## Regression checks

Run `scripts/Test-XboxStartup.ps1` in an x64 Visual Studio Developer PowerShell.
It builds and runs `tests/XboxStartupPlanTests.cpp` against the same pure PE engine
used by the application. Checks cover all patch sites, byte-register preservation,
idempotence, preserving unrelated image bytes, ignoring patterns in data sections,
and refusal of truncated, ambiguous, forwarded-export, wrong-machine and unexpected
site-count fixtures. The suite never writes Windows system files.

On a disposable compatible Windows test machine, additionally verify:

- Status reports three original files, then three enabled files after activation.
- Home app selection and startup controls appear on a non-handheld.
- `OEM\DeviceForm` remains unchanged unless undoing an older AnyFSE-owned override.
- Native Xbox and an AnyFSE launcher each start at sign-in when selected.
- Restore returns the DLLs byte-for-byte to their saved originals, including owner
  and DACL, and the original device form stays intact.
- An unsupported Windows build fails before any system file is changed.
- A simulated Windows update causes stale-backup restoration to be refused.
- Existing Ally mappings and the dedicated background listener still work.
