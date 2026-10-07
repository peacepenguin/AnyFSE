#pragma once
#include <windows.h>
#include <processsnapshot.h>
#include "DebugLog.h"

namespace ACSEFilter::Startup
{
    // Run once after installing the read/completion hooks. Cancellation is asynchronous;
    // the owner must consume ERROR_OPERATION_ABORTED normally and issue its next read.
    // The predicate receives a retained duplicate, never a potentially stale snapshot handle.
    template<class Predicate>
    DWORD CancelPendingIo(Predicate isTarget)
    {
        HPSS snapshot = nullptr;
        DWORD result = PssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_HANDLES, 0, &snapshot);
        if (result != ERROR_SUCCESS)
        {
            LOG(L"Cannot enumerate pre-hook HID handles: %lu", result);
            return result;
        }
        HPSSWALK walk = nullptr;
        result = PssWalkMarkerCreate(nullptr, &walk);
        if (result == ERROR_SUCCESS)
        {
            DWORD matched = 0;
            DWORD canceled = 0;
            PSS_HANDLE_ENTRY entry{};
            while ((result = PssWalkSnapshot(snapshot, PSS_WALK_HANDLES, walk, &entry, sizeof(entry))) == ERROR_SUCCESS)
            {
                HANDLE file = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), entry.Handle, GetCurrentProcess(), &file, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
                if (isTarget(file))
                {
                    ++matched;
                    if (CancelIoEx(file, nullptr))
                    {
                        ++canceled;
                        LOG_VERBOSE(L"Requested cancellation of pre-hook button I/O on handle %p", entry.Handle);
                    }
                    else if (const DWORD error = GetLastError(); error != ERROR_NOT_FOUND)
                        LOG(L"Could not cancel pre-hook button I/O on handle %p: %lu", entry.Handle, error);
                }
                CloseHandle(file);
            }
            PssWalkMarkerFree(walk);
            LOG(L"Pre-hook button I/O recovery: matched=%lu cancellationRequests=%lu", matched, canceled);
            if (result == ERROR_NO_MORE_ITEMS) result = ERROR_SUCCESS;
            else LOG(L"Pre-hook HID handle enumeration failed: %lu", result);
        }
        else LOG(L"Cannot create HID handle walk marker: %lu", result);
        PssFreeSnapshot(GetCurrentProcess(), snapshot);
        return result;
    }
}
