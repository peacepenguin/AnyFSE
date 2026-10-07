#include <windows.h>
#include "../../App/Constants.hpp"
#include "DebugLog.h"
#include "Config.h"
#include "HidReadFilter.h"
#include "IATHook.h"
#include "Native.h"

namespace ACSEFilter::Hook
{
    namespace
    {
        BOOL WINAPI HookReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead, LPDWORD bytesRead, LPOVERLAPPED overlapped)
        {
            // Track before submitting: another thread can observe completion before ReadFile returns.
            const DWORD incomingError = GetLastError();
            RememberPendingRead(file, buffer, bytesToRead, overlapped);
            SetLastError(incomingError);
            const BOOL result = Native::ReadFile(file, buffer, bytesToRead, bytesRead, overlapped);
            const DWORD lastError = GetLastError();
            if (result)
            {
                DWORD actualBytes = bytesRead ? *bytesRead : 0;
                if (!overlapped || ::GetOverlappedResult(file, overlapped, &actualBytes, FALSE))
                {
                    if (overlapped) CompletePendingRead(file, overlapped, actualBytes);
                    else PatchCompletedRead(file, buffer, actualBytes, bytesToRead);
                }
            }
            else if (lastError != ERROR_IO_PENDING) DropPendingRead(overlapped);
            SetLastError(lastError);
            return result;
        }

        BOOL WINAPI HookGetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytes, BOOL wait)
        {
            const BOOL result = ::GetOverlappedResult(file, overlapped, bytes, wait);
            const DWORD lastError = GetLastError();
            if (result && bytes) CompletePendingRead(file, overlapped, *bytes);
            else if (!result && lastError != ERROR_IO_INCOMPLETE) DropPendingRead(overlapped);
            SetLastError(lastError);
            return result;
        }

        BOOL WINAPI HookGetOverlappedResultEx(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytes, DWORD timeout, BOOL alertable)
        {
            const BOOL result = ::GetOverlappedResultEx(file, overlapped, bytes, timeout, alertable);
            const DWORD lastError = GetLastError();
            if (result && bytes) CompletePendingRead(file, overlapped, *bytes);
            else if (!result && lastError != ERROR_IO_INCOMPLETE && lastError != WAIT_TIMEOUT && lastError != WAIT_IO_COMPLETION)
                DropPendingRead(overlapped);
            SetLastError(lastError);
            return result;
        }

        void CompleteWait(DWORD result, DWORD count, const HANDLE *handles, BOOL waitAll)
        {
            if (result < WAIT_OBJECT_0 + count)
            {
                if (waitAll) CompletePendingReadsAfterWait(count, handles);
                else CompletePendingReadsAfterWait(1, handles + (result - WAIT_OBJECT_0));
            }
        }

        DWORD WINAPI HookWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD milliseconds)
        {
            const DWORD result = Native::WaitForMultipleObjects(count, handles, waitAll, milliseconds);
            const DWORD lastError = GetLastError();
            CompleteWait(result, count, handles, waitAll);
            SetLastError(lastError);
            return result;
        }

        DWORD WINAPI HookWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD milliseconds, BOOL alertable)
        {
            const DWORD result = ::WaitForMultipleObjectsEx(count, handles, waitAll, milliseconds, alertable);
            const DWORD lastError = GetLastError();
            CompleteWait(result, count, handles, waitAll);
            SetLastError(lastError);
            return result;
        }

        DWORD WINAPI HookWaitForSingleObject(HANDLE handle, DWORD milliseconds)
        {
            const DWORD result = ::WaitForSingleObject(handle, milliseconds);
            const DWORD lastError = GetLastError();
            if (result == WAIT_OBJECT_0) CompletePendingReadsAfterWait(1, &handle);
            SetLastError(lastError);
            return result;
        }

        DWORD WINAPI HookWaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable)
        {
            const DWORD result = ::WaitForSingleObjectEx(handle, milliseconds, alertable);
            const DWORD lastError = GetLastError();
            if (result == WAIT_OBJECT_0) CompletePendingReadsAfterWait(1, &handle);
            SetLastError(lastError);
            return result;
        }
    }

    DWORD WINAPI Install(LPVOID context)
    {
        if (!Native::ResolveKernelFunctions())
        {
            LOG(L"Failed to resolve required kernel functions.");
            return 1;
        }
        const HMODULE targetModule = GetModuleHandleW(Constants::AsusOptimizationProcess);
        if (!targetModule || targetModule == static_cast<HMODULE>(context))
        {
            LOG(L"Failed to resolve the target executable module.");
            return 1;
        }
        const ImportHookSpec specs[] = {
            {Config::kReadFile, reinterpret_cast<void *>(HookReadFile)},
            {Config::kWaitForMultipleObjects, reinterpret_cast<void *>(HookWaitForMultipleObjects)},
            {Config::kWaitForMultipleObjectsEx, reinterpret_cast<void *>(HookWaitForMultipleObjectsEx)},
            {Config::kWaitForSingleObject, reinterpret_cast<void *>(HookWaitForSingleObject)},
            {Config::kWaitForSingleObjectEx, reinterpret_cast<void *>(HookWaitForSingleObjectEx)},
            {Config::kGetOverlappedResult, reinterpret_cast<void *>(HookGetOverlappedResult)},
            {Config::kGetOverlappedResultEx, reinterpret_cast<void *>(HookGetOverlappedResultEx)},
        };
        size_t reads = 0;
        size_t completions = 0;
        for (size_t i = 0; i < ARRAYSIZE(specs); ++i)
        {
            const size_t patched = PatchModuleImports(targetModule, &specs[i], 1);
            LOG_VERBOSE(L"Import %S: %zu slots patched", specs[i].functionName, patched);
            if (i == 0) reads += patched;
            else completions += patched;
        }
        if (!reads || !completions)
        {
            LOG(L"Incomplete hook coverage: ReadFile=%zu completion=%zu. ASUS button suppression is not verified.", reads, completions);
            return 1;
        }
        LOG(L"Read and completion imports patched in ASUS executable.");
        CancelPreHookButtonReads();
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, ACSEFilter::Hook::Install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
