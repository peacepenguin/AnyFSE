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
        std::wstring details;
    };
    bool IsHandheldDevice();
    Status Inspect();
    // Explicit elevated operations. Never invoked automatically at startup or installation.
    void Apply();
    void Restore();
}
