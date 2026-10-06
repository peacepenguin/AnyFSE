#include "IATHook.h"

#include <cstring>

namespace ACSEFilter
{
    namespace
    {
        bool IsReadablePeImage(HMODULE module)
        {
            if (!module)
            {
                return false;
            }

            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return false;
            }

            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const BYTE *>(module) + dos->e_lfanew);
            return nt->Signature == IMAGE_NT_SIGNATURE;
        }

        const ImportHookSpec *FindHook(const char *functionName, const ImportHookSpec *hooks, size_t hookCount)
        {
            if (!functionName)
            {
                return nullptr;
            }

            for (size_t i = 0; i < hookCount; ++i)
            {
                if (std::strcmp(functionName, hooks[i].functionName) == 0)
                {
                    return &hooks[i];
                }
            }

            return nullptr;
        }

        bool PatchThunk(IMAGE_THUNK_DATA *thunk, void *hookFunction)
        {
            auto *slot = reinterpret_cast<void **>(&thunk->u1.Function);
            if (*slot == hookFunction)
            {
                return true;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect))
            {
                return false;
            }

            *slot = hookFunction;

            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void *), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
            return true;
        }

        size_t PatchModuleImportsImpl(HMODULE module, const ImportHookSpec *hooks, size_t hookCount)
        {
            if (!IsReadablePeImage(module))
            {
                return 0;
            }

            const auto *base = reinterpret_cast<const BYTE *>(module);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

            if (!directory.VirtualAddress || !directory.Size)
            {
                return 0;
            }

            auto *import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(const_cast<BYTE *>(base) + directory.VirtualAddress);
            size_t patched = 0;

            for (; import->Name; ++import)
            {
                auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(const_cast<BYTE *>(base) + import->FirstThunk);
                auto *originalThunk = import->OriginalFirstThunk
                                          ? reinterpret_cast<IMAGE_THUNK_DATA *>(const_cast<BYTE *>(base) + import->OriginalFirstThunk)
                                          : nullptr;

                for (size_t index = 0; thunk[index].u1.Function; ++index)
                {
                    if (!originalThunk)
                    {
                        continue;
                    }

                    if (IMAGE_SNAP_BY_ORDINAL(originalThunk[index].u1.Ordinal))
                    {
                        continue;
                    }

                    const auto *importByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + originalThunk[index].u1.AddressOfData);
                    const ImportHookSpec *hook = FindHook(reinterpret_cast<const char *>(importByName->Name), hooks, hookCount);
                    if (hook)
                    {
                        if (PatchThunk(&thunk[index], hook->hookFunction)) ++patched;
                    }
                }
            }
            return patched;
        }

    } // namespace

    size_t PatchModuleImports(HMODULE module, const ImportHookSpec *hooks, size_t hookCount)
    {
        return PatchModuleImportsImpl(module, hooks, hookCount);
    }

} // namespace ACSEFilter
