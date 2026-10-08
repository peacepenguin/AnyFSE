#pragma once
#include <string>
namespace AnyFSE::Tools::XboxStartup
{
    struct Status
    {
        bool allPatched = true;
        bool anyPatched = false;
        bool canApply = true;
        bool canRestore = true;
        // Device-form checks found on an unrecognized layout. Applied only through ApplyUnverified after user confirmation.
        bool unverifiedPending = false;
        std::wstring unverifiedDetails;
        std::wstring details;
    };
    bool IsHandheldDevice();
    Status Inspect();
    // Explicit elevated operations. Never invoked automatically at startup or installation.
    void Apply();
    void ApplyUnverified();
    void Restore();
    // Read-only diagnostic listing every device-form check in System32 DLLs.
    std::wstring Scan();
}
