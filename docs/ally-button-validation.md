# Ally button override validation

Run these checks on a supported Ally with ASUS Optimization installed. Build and
install the updated binaries before testing; restart ASUS Optimization so it loads
the updated hook DLL. A DLL loaded by a previous build is not replaced in memory.

Enable AnyFSE trace logging and capture the hook's `[ACSEFilter]` debug output.
Confirm that the injector service is running, that the listener logs received
button reports, and that the hook logs `Suppress ASUS button report` for each
remapped system-button report. A running service or a loaded DLL alone does not
prove suppression.

| Scenario | Expected result |
| --- | --- |
| Enable remapping with the injector service absent | The elevated listener creates and starts the service without a settings-page service operation. |
| Reboot directly into FSE | One listener starts at logon; mappings work without opening desktop settings. |
| Upgrade from a version using HKCU Run | The `AnyFSE Hotkeys` Run value is removed and is never recreated. |
| Open the app/settings while the logon task starts | All requests use `AnyFSE Listener`; one listener task instance runs. |
| Invoke the legacy `/HidListener` command | It requests the dedicated task, without spawning a listener through the general elevation task. |
| Delete or disable the listener task, then request startup | A specific task-start error is logged; no alternative launcher silently takes over. |
| Uninstall with the listener running | The listener task is stopped/deleted and the legacy Run entry is removed before service cleanup. |
| Save a new mapping from ordinary, unelevated settings | The already-running elevated listener uses the new mapping immediately. |
| Start with only hotkeys enabled, then enable remapping | Raw input becomes active without restarting the listener. |
| Disable remapping while keeping hotkeys enabled | ASUS behavior returns, custom button actions stop, and hotkeys continue working. |
| Re-enable remapping in that same listener | Custom actions and suppression both resume. |
| Stop the injector service with remapping enabled | The listener starts it again on its next reconciliation, within about ten seconds after the stop completes. |
| Restart ASUS Optimization | The injector detects the new process and installs the hook again. |
| Start before the ASUS HID device is enumerated | The listener stays alive and retries registration within about ten seconds. |
| Short press, hold, and Mode combinations | Each supported gesture produces its selected action; the conflicting ASUS action does not also run. |
| Disable extra commands, then reload | Previously enabled extra-command bindings no longer execute. |

## Filter regression checks

Using a Windows HID/read harness or an instrumented ASUS process, verify:

- Synchronous matching six-byte reports become `5A 00 00 00 00 00`.
- Nonmatching vendors, products, report IDs, lengths, keys, and trailing bytes remain unchanged.
- Two pending reads on one thread are both filtered, even if an unrelated read occurs between them.
- A read issued on one thread and completed on another is filtered before the completion call returns.
- Single-object, multiple-object, wait-all, and alertable wait variants filter completed reads; timeouts do not patch unfinished buffers.
- `GetOverlappedResult` and `GetOverlappedResultEx` filter successful completions and remove failed operations from tracking.
- Failed reads preserve their return values and error codes.
- A raw-input packet containing multiple HID reports dispatches each report in order, including Mode press/release transitions.

## Compatibility boundary

Hooks target the normal imports of the ASUS executable. Calls made inside other
DLLs, dynamically resolved APIs, `ReadFileEx`, and IO completion-port APIs are not
covered. Per-import patch counts and missing-coverage diagnostics are available;
actual suppression must still be confirmed on the installed ASUS version. Do not
interpret an import count as proof that ASUS uses that path for its button reads.
