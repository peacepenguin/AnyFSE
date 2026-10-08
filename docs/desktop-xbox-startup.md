# Desktop Xbox startup

AnyFSE ports the XboxStartupEnabler patcher into its native C++ runtime. The
implementation is adapted from `victorrjimenezz/XboxStartupEnabler` at commit
`e52ab92951f56b7c1ee404fa9c870a6a3d2732e5` (MIT; see
`THIRD_PARTY_NOTICES/XboxStartupEnabler.txt`). No separate .NET application is
required.

## How the patch works

The code lives in two files:

- [`src/Tools/XboxStartupPlan.cpp`](../src/Tools/XboxStartupPlan.cpp) /
  [`.hpp`](../src/Tools/XboxStartupPlan.hpp) — the pure, side-effect-free PE
  engine. `BuildPlan(target, image)` reads a DLL image already loaded into
  memory (a `std::vector<uint8_t>`) and locates every patch site, refusing if
  what it finds does not exactly match a known original or already-patched
  byte sequence. `PatchImage(target, image)` applies the plan and recomputes
  the PE checksum. Neither function touches the filesystem, the registry, or
  the running system, which is what `tests/XboxStartupPlanTests.cpp` and
  `scripts/Test-XboxStartup.ps1` exercise without needing a real Windows
  install.
- [`src/Tools/XboxStartup.cpp`](../src/Tools/XboxStartup.cpp) — everything
  around that: elevation, reading/writing the target System32 DLLs (three
  always present, plus an optional fourth on newer builds), backups, the
  journal, rollback, and the handheld guard. This is the only file that mutates
  anything, and only through `Apply()`/`Restore()`.

### Why these DLLs, and what API they gate

Windows exposes the "Gaming Fullscreen Experience" (the Xbox full-screen
Home/game-mode UI normally reserved for OEM handhelds) through
`api-ms-win-gaming-experience-l1-1-0.dll`, which forwards to **`gamemode.dll`**
in `C:\Windows\System32`. Three of its exports matter:

| Export | Role |
| --- | --- |
| `IsGamingFullScreenExperienceSupported` | Whether the OS will let the feature be enabled at all. |
| `CanSetGamingFullScreenExperience` | Whether the current user/session is allowed to toggle it. |
| `SetGamingFullScreenExperience` | Actually turns it on/off; internally branches on whether the caller is an approved "Home app". |

More DLLs independently re-check the device's eligibility before exposing
UI for it:

- **`SettingsHandlers_Gaming.dll`** backs the Windows Settings > Gaming page
  (whether to show the Xbox startup controls at all, and which state to default
  them to).
- **`twinui.pcshell.dll`** is part of the shell (`explorer`/`sihost`) and
  gates the Home-app picker / startup surface shown outside Settings.
- **`SettingsEnvironment.Desktop.dll`** gates the Settings "Gaming posture"
  page on newer builds (it carries `ShouldShowGamingPostureSettings` and the
  "Choose home app" strings). It is **optional**: builds without this check are
  patched with no entry here, so AnyFSE skips it when the DLL has no handheld
  check. It was the missing piece that kept the native Settings page hidden on
  26100/26200 desktops even after the other three were patched.

All of these checks ultimately resolve to the same underlying fact: the device's
*device form*, obtained via the undocumented `ntdll!RtlGetDeviceFamilyInfoEnum`
(the same API `IsHandheldDevice()` in `XboxStartup.cpp` calls to decide
whether patching is even allowed to run — see "Handheld guard" below). OEM
handhelds report device form **46 (`0x2E`)**; everything the patcher touches
is a place where compiled code compares a cached copy of that value to `0x2E`
or calls one of the three `gamemode.dll` exports above (the handheld-check DLLs
are `SettingsHandlers_Gaming.dll`, `twinui.pcshell.dll`, and, where present,
`SettingsEnvironment.Desktop.dll`). `AnyFSE::App::Constants::HandheldDeviceForm`
holds the same value `46` for the registry-based guard, confirming it is the
same enum used system-wide.

### The verified patch sites, byte for byte

These are the layouts AnyFSE has validated on real builds and applies without
confirmation (three gaming sites in `gamemode.dll`, plus one or more handheld
checks in each handheld-check DLL). Layouts outside these patterns are handled
by structural discovery instead — see "Surviving new layouts" below.

All offsets below are found dynamically (via the export table or a section
scan), never hardcoded as fixed file offsets — the bytes themselves are what
AnyFSE matches against, so the same patch works across Windows builds as long
as the compiler keeps emitting the same instructions. `-1`/`??` below means
"any byte accepted" (a register or displacement encoded in that position).

**`gamemode.dll` — 3 sites, resolved via `Target::GameMode`:**

1. **`IsGamingFullScreenExperienceSupported` prologue.**
   Original: `48 89 5C 24 08 48` → `mov [rsp+8], rbx` (the function's normal
   stack-home-space prologue, plus the first byte of the next instruction as
   a loose anchor).
   Patched: `B8 01 00 00 00 C3` → `mov eax, 1` ; `ret`. The export now
   unconditionally reports the feature as supported, ignoring whatever logic
   followed.

   The `26100.9278` variant starts with `48 83 EC 28 48 8D 0D ... E8 ...
   84 C0 74 ...` (a small stack frame, RIP-relative feature-state address, call,
   and byte-result test). That complete 20-byte prefix is recognized as an
   alternative at the same named export; the replacement remains six bytes.

2. **`CanSetGamingFullScreenExperience` prologue.**
   Original: `48 89 5C 24 20 57` → `mov [rsp+20h], rbx` ; `push rdi`.
   Patched: same `B8 01 00 00 00 C3` stub — unconditionally reports the
   caller as permitted.

   If either export starts with `F3 0F 1E FA` (`ENDBR64`), that landing pad
   is preserved and the known prologue immediately after it is patched instead.
   This supports that specific compiler variation; other unknown prologues remain refused.

3. **A conditional branch inside `SetGamingFullScreenExperience`.**
   AnyFSE scans up to 128 bytes past that export's entry point, bounded by its
   exclusive end in the x64 exception/function table, for
   `84 C0` (`test al, al`) immediately followed by either an unpatched
   `0F 84 ?? ?? ?? ??` (`je rel32`, a 6-byte near-conditional-jump, called
   the "home-app branch" in the code/logs) or an already-NOP'd run of six
   `90` bytes. The 6-byte jump is replaced with six `90` (`nop`) bytes, so
   execution always falls through past whatever the branch used to skip.
   Exactly one such site must be found (`sites.size() == 3` is enforced for
   the whole DLL); more or fewer is treated as an unrecognized layout and
   refused rather than guessed at. The complete pattern must fit inside the
   function, and an original branch must target file-backed executable code.
   The destination can be a separate cold block or shared epilogue; it need not
   share the setter's function-table entry.
   Missing, malformed, or overlapping function metadata is refused. The scan
   does not expand into neighboring functions when compiler output changes.

   When no legacy `test al, al` gate exists, the engine also recognizes
   `call IsGamingFullScreenExperienceSupported; test eax, eax; je rel32`.
   The direct call's decoded relative target must be the named support export.
   This is the BOOL support gate present in the inspected `26100.9278` setter,
   not a home-app gate. Only that jump is NOP'd; the setter body remains intact.
   Unrelated HRESULT/BOOL checks are ignored. Multiple matching candidates are
   rejected. If both forms occur, the legacy home-app gate takes precedence so
   previous patch detection and backup restoration keep producing the same bytes.

**`SettingsHandlers_Gaming.dll`, `twinui.pcshell.dll`, and (when present)
`SettingsEnvironment.Desktop.dll` — resolved via `Target::Settings` /
`Target::Shell` / `Target::SettingsEnvironment`, same pattern, scanned across
every executable section:**

```
83 7C 24 ??  2E  0F 94 ??
```

Disassembled, this 8-byte sequence is two instructions:
`cmp dword [rsp+disp8], 2Eh` (`83 7C 24 disp8 2E`) followed by
`sete r/m8` (`0F 94 ModRM`), i.e. *"compare the cached device-form value on
the stack to 46 (handheld), and set a one-byte register to 1 if it matches."*
`disp8` and the destination register in the `sete` ModRM byte are accepted as
any value (`-1`/wildcard) because the compiler is free to pick a different
stack slot or register per call site; only the opcodes and the literal `0x2E`
comparand must match.

Patched: `B0+reg 01 90 90 90 90 90 90` → `mov <reg8>, 1` followed by six
`nop`s padding it back out to the original 8 bytes. The comparison and its
result are both removed; the register is simply forced to `1` ("yes, this is
a handheld"), using the *same* register the original `sete` would have
written to, so nothing downstream that reads that register needs to change.
Each handheld-check DLL must contain at least one recognized check, and all
matching checks are patched, as in the upstream patcher — except
`SettingsEnvironment.Desktop.dll`, which is optional and resolves to an empty
(no-op) plan on builds that have no such check. One Settings site, one shell
site, and one `SettingsEnvironment` site were observed on 26100/26200; those
counts are not compatibility requirements. Compiler inlining can change the
count between Windows builds.

### Patched vs. original detection, and why mismatches are refused

`BuildPlan` calls `Resolve()` for each site, which checks, in order:

1. Does the site already hold the *exact* replacement bytes? → already
   `State::Patched` (this makes apply/detect idempotent and lets `status`
   report "enabled").
2. Otherwise, does it hold the *exact* known-original bytes (wildcards
   aside)? → `State::Original`.
3. Otherwise → throw. As of the diagnostics added for build robustness, the
   error names the exact file offset and prints both the expected pattern
   and the actual bytes found there (e.g.
   `expected [48 89 5c 24 08 48] ... found [48 89 5c 24 20 48]`), and the
   DLL/Windows build number are layered on by the callers in
   `XboxStartup.cpp`. This is what makes a failure on a new Windows build
   actionable: the affected function can be disassembled and its behavior
   checked before adding a new accepted layout. Copying the reported bytes into
   a signature is not enough to establish compatibility.

A whole-image checksum (`Checksum()`, the standard PE/COFF checksum algorithm)
is recomputed after patching, matching what the Windows loader and image
validators expect from a well-formed PE file.

### Surviving new layouts: discovery and unverified sites

Discovery is driven by a real x64 disassembler, the vendored Zydis amalgamation
(`src/Tools/zydis`, MIT; see `THIRD_PARTY_NOTICES/Zydis.txt`), compiled statically
into each binary. Instructions are decoded per `RUNTIME_FUNCTION` from the
exception table, so analysis follows actual operands and data flow rather than
fixed byte patterns.

Each `Site` is either **verified** (a layout validated on real builds; applied by
the installer and the normal **Enable** button) or **unverified** (found
structurally on an unknown layout; applied only by `ApplyUnverified()` after the
user reviews the exact sites in Settings and confirms, or with
`AnyFSE.exe /XboxStartup apply-unverified`).

- **Import-anchored discovery** (`DiscoverHandheldChecks`): decodes each function
  and finds every `call` through the import/delay-import slot for
  `RtlGetDeviceFamilyInfoEnum` (the call target is resolved with
  `ZydisCalcAbsoluteAddress`). It takes the device-form output slot from the
  nearest preceding `lea r8, [stack slot]` (bailing if R8 is rewritten before the
  call), then tracks that stack slot — and any register the value is copied into
  via `mov`/`movzx`/`movsx` — forward to a `cmp <value>, 0x2E` in the same
  function. Register choice, addressing mode and displacement width do not matter
  because the match is on decoded operands. Supported consumers of the compare:
  `setz`/`setnz` (→ `mov reg, 1/0`, encoded for the actual destination register),
  `jz` (→ unconditional `jmp` to the same target, shortest encoding that fits),
  `jnz` (→ `nop`, falling through to the handheld path). Any other consumer
  (`cmovz`, `setz [mem]`, `ja`, …) is reported only and never patched. The
  validated `cmp [rsp+x],2Eh; setz` layout is flagged verified, so discovery adds
  no duplicate site for it. Rewrites are produced by the Zydis encoder and padded
  with `ZydisEncoderNopFill`, so they are exactly what an assembler would emit.
- **Unrecognized export prologues** (gamemode `IsSupported`/`CanSet`): stubbed
  with `mov eax,1; ret` as an unverified site only when the export is a
  `RUNTIME_FUNCTION` entry (optionally after `ENDBR64`), the function is at least
  as long as the stub, and no relative branch or call in any decoded function
  targets inside the overwritten bytes (`ZydisCalcAbsoluteAddress` resolves each
  branch target). Decoding per function entry keeps the stream aligned, so a
  misread can only refuse the stub, never approve an unsafe one.
- **Patched-state detection** comes from a validated backup that reproduces the
  installed image (with or without the unverified sites), not from patched-byte
  signatures, because a discovered compare no longer exists once replaced.
  Applying never silently adds or removes unverified sites the user has
  already confirmed.
- **Unlisted DLLs are never patched.** `AnyFSE.exe /XboxStartup scan` lists
  device-form checks in every System32 DLL. On 26100 this includes the search
  indexer (`mssrch.dll`, `tquery.dll`), `InputHost.dll`,
  `AboutSettingsHandlers.dll` and `WSAIFabricHost.dll`, which gate unrelated
  features; it exists to identify when Windows moves a home-app check into a
  new DLL, which then needs a reviewed `specs` entry.

### Handheld guard

Before anything in `Execute()` (used by both `Apply()` and `Restore()`) is
allowed to touch a file, `IsHandheldDevice()` runs first:

1. Registry: `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OEM\DeviceForm`
   == `46`.
2. If that doesn't say handheld, call `ntdll!RtlGetDeviceFamilyInfoEnum`
   directly (the same function the patched code paths above compare against)
   and check whether it reports form `46`.
3. If neither API is available, **fail closed** (treated as handheld, so
   patching is refused) rather than assume desktop.

`Apply()` refuses outright on a handheld; `Restore()` is always allowed, so a
device that was patched and later reclassified (or a spoofed registry entry)
can still be put back.

## Enable and test

1. Build/install AnyFSE on a Windows version with the Gaming Fullscreen Experience
   APIs. This does not add those APIs to older Windows builds.
2. On a non-handheld PC whose DLLs match a recognized layout, the installer itself
   offers to apply the patch (`AppInstaller::ShowXboxModeCheckPage` in
   `src/AppInstaller/AppInstaller_Page.cpp`), right after the license page. It is
   opt-in: **Skip** continues installing AnyFSE without touching any system file,
   **Enable** applies the patch (via `Tools::XboxStartup::Apply()`, since the
   installer already runs elevated) and then continues installing either way, so a
   patch failure never blocks the AnyFSE install itself. The prompt only appears
   when `Tools::XboxStartup::Inspect().canApply` is true; a handheld, an
   already-patched device, or an unrecognized Windows build skips straight past it
   with no prompt and no system file touched. The same action is always available
   afterward in AnyFSE settings, choose **Enable Xbox startup on PC**, which calls
   the same `Tools::XboxStartup::Apply()` through AnyFSE's existing elevated
   scheduled-task handler and is similarly only enabled once `canApply` is true.
3. Restart to load the changed Windows components. The operation does
   not reboot or change startup settings automatically.
4. Choose your Home app and turn on **Enter FSE on startup** in AnyFSE. For testing
   the Microsoft Xbox home screen, choose the native Xbox launcher. For testing
   AnyFSE's launcher, choose Playnite, Steam, or another supported launcher.
5. Sign in again and confirm the selected Home app starts. Verify the same settings
   are exposed in Windows Settings > Gaming.

Activation and DLL restoration do not change `OEM\DeviceForm`. Any older registry
override must be restored separately. Real handhelds continue to work without needing
these patches.

`AnyFSE.exe /XboxStartup status` (or `verify`) inspects the target DLLs and shows
per-file state, also writing it to the AnyFSE log. Inspection works before the
normal FSE API availability check. `apply`, `apply-unverified`, `restore`, and the
read-only `scan` request the same elevated handlers used by settings; they require
an installed AnyFSE elevation task.

## Compatibility and restoration

The recognized layouts use three gaming API changes and one or more recognized
handheld checks in each handheld-check DLL (Settings, shell, and the optional
`SettingsEnvironment.Desktop.dll`). Matching byte signatures must be in
executable x64 PE sections. Missing exports, unknown prologues, ambiguous branches,
missing handheld checks, malformed PE data, and partially patched individual images
are refused. The upstream project documents testing Windows 11 build 26200.8457;
its screenshot also records the six sites on the 26100 DLL family. Matching patterns
are a compatibility check, not a guarantee for every Windows release.

PE directory arrays and names must fit within their backing sections. Overlapping
raw sections, overlapping section RVA ranges, and overlapping patch sites are
rejected. Function bounds follow Microsoft's
[x64 PE function-table format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-pdata-section).
These checks improve refusal behavior; they do not establish the semantics of a
future Windows implementation or replace testing on a disposable machine.

A read-only inspection of local DLLs at version **10.0.26100.9278** now recognizes
three gaming API sites, one Settings check, one shell check, and one
`SettingsEnvironment.Desktop.dll` check. All pass apply/detect/reapply in memory.
The gaming sites are at file offsets `0x10F30`, `0x10A30`, and `0x11188` in the
inspected file (offsets are still resolved dynamically).
The alternate prologue and BOOL support gate above account for the difference
from the upstream sample layout. This verifies patch planning and idempotence,
not post-reboot behavior; the actual system files were not modified by the probe.

All target images are validated before replacement. Original bytes are backed up
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

Run the **Test Xbox Startup Patch Plan** VS Code task, or
`scripts/Test-XboxStartup.ps1` in an x64 Visual Studio Developer PowerShell.
It builds and runs `tests/XboxStartupPlanTests.cpp` against the same pure PE engine
used by the application. Checks cover all patch sites, byte-register preservation,
idempotence, preserving unrelated image bytes, ignoring patterns in data sections,
and refusal of truncated, ambiguous, forwarded-export, wrong-machine and missing
check fixtures. It also checks variable check counts, landing-pad preservation and idempotence,
function-boundary enforcement, branch destinations, and malformed section/directory
metadata. Alternate-layout checks cover the support-function prefix, exact direct-call
target, absent/ambiguous calls, patched-state detection, and legacy-gate precedence.
The dev-build CI runs this suite. The suite never writes Windows system files.

After compiling the tests, the same executable can inspect the real DLLs
without modifying them. The fourth path is optional (newer builds only). In
PowerShell:

```powershell
./build/tests/XboxStartupPlanTests.exe `
    "$env:SystemRoot/System32/gamemode.dll" `
    "$env:SystemRoot/System32/SettingsHandlers_Gaming.dll" `
    "$env:SystemRoot/System32/twinui.pcshell.dll" `
    "$env:SystemRoot/System32/SettingsEnvironment.Desktop.dll"
```

It reports recognized site counts, offsets and states, verifies apply/detect/reapply
in memory, or reports the refusal reason for each file, and returns
a failure exit code if any image is unsupported. Recognition is not an installed
behavior test and does not bypass the handheld guard in the application.

On a disposable compatible Windows test machine, additionally verify:

- Status reports every target file original, then enabled after activation.
- Home app selection and startup controls appear on a non-handheld.
- `OEM\DeviceForm` remains unchanged during both apply and restore.
- Native Xbox and an AnyFSE launcher each start at sign-in when selected.
- Restore returns the DLLs byte-for-byte to their saved originals, including owner
  and DACL, and the original device form stays intact.
- An unsupported Windows build fails before any system file is changed.
- A simulated Windows update causes stale-backup restoration to be refused.
- Existing Ally mappings and the dedicated background listener still work.

## Recovery during dev-build testing

Installing or starting AnyFSE does not apply the desktop DLL patches. On an Ally,
test normal handheld behavior first; enable the experimental patches only if needed.
The function-hook prototype discussed for future work is not implemented in this build.

Before applying or restoring, all supported DLL images present are snapshotted under
`%ProgramData%\AnyFSE-XboxStartupBackups`, including unchanged images. Each
`<dll>.<SHA256>.snapshot` is verified by reading it back before any replacement.
Original restore backups are separately named `.original` and also verified.
An already externally patched image can be snapshotted, but that snapshot cannot
recover the unpatched original; restore using the original patcher in that case.

Each operation writes a flushed `operation-{GUID}.log` in the same directory.
It records UTC timestamps, target and backup paths, SHA-256 hashes, original
owner/group/DACL as SDDL, temporary rename paths, replacement results, rollback
results, and pending reboot cleanup. Keep this directory when reporting a test
failure. A journal without `COMPLETED` requires inspection; it does not trigger
automatic recovery after a crash or power loss. Recovery attempts continue even
if writing the journal fails, with that failure reported in the regular AnyFSE log.

To revert DLL changes, use **Restore desktop Xbox startup changes** or run
`AnyFSE.exe /XboxStartup restore` from the installed build, then restart Windows.
Run `AnyFSE.exe /XboxStartup status` afterward. Restore refuses backups that no
longer match the current Windows image, including after Windows servicing.
Do not copy snapshots over newer Windows DLLs manually.

The DLL journal does not cover independent launcher, Home app, startup, Ally-button,
or installer settings. Record their prior values before testing and restore those
settings separately. Restore DLL changes before uninstalling AnyFSE; retain backups
and journals until testing is finished.
Desktop patch application is blocked for devices identified as gaming handhelds by
either the OEM registry form or Windows' `RtlGetDeviceFamilyInfoEnum` query. This
also blocks existing registry-spoofed handhelds, even if an AnyFSE backup marker is
present. Missing query support blocks application conservatively. The guard runs
before privileges, backups, permission changes, or DLL replacements and covers the
UI, CLI, and elevated handler. Restore remains available to undo earlier patches.
