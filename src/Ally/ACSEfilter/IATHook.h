#pragma once

#include <windows.h>

#include <cstddef>

namespace ACSEFilter
{

    struct ImportHookSpec
    {
        const char *functionName;
        void *hookFunction;
    };

    size_t PatchModuleImports(HMODULE module, const ImportHookSpec *hooks, size_t hookCount);

} // namespace ACSEFilter
