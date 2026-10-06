#include "HidReadFilter.h"
#include "Config.h"
#include "DebugLog.h"
#include <hidsdi.h>
#include <map>
#include <mutex>

namespace ACSEFilter
{
    namespace
    {
        struct PendingRead
        {
            HANDLE file;
            void *buffer;
            DWORD requestedBytes;
        };

        std::mutex pendingMutex;
        std::map<LPOVERLAPPED, PendingRead> pendingReads;

        bool IsTargetHandle(HANDLE file)
        {
            HIDD_ATTRIBUTES attributes = { sizeof(HIDD_ATTRIBUTES) };
            if (!file || file == INVALID_HANDLE_VALUE || !HidD_GetAttributes(file, &attributes)
                || attributes.VendorID != Config::kTargetVendorId) return false;
            for (const auto product : Config::kTargetProductIds)
                if (product == attributes.ProductID) return true;
            return false;
        }

        bool IsTargetReport(const BYTE *buffer, DWORD length)
        {
            if (!buffer || length != Config::kExpectedReadLength || buffer[0] != 0x5A) return false;
            for (DWORD i = 2; i < length; ++i)
                if (buffer[i] != 0) return false;
            for (const auto key : Config::kReplacementKeys)
                if (buffer[1] == key) return true;
            return false;
        }
    }

    void PatchCompletedRead(HANDLE file, void *buffer, DWORD actualBytes, DWORD requestedBytes)
    {
        if (requestedBytes != Config::kExpectedReadLength
            || !IsTargetReport(static_cast<BYTE *>(buffer), actualBytes) || !IsTargetHandle(file)) return;
        LOG(L"Suppress ASUS button report 0x%02X", static_cast<BYTE *>(buffer)[1]);
        CopyMemory(buffer, Config::kReplacementBytes, Config::kExpectedReadLength);
    }

    void RememberPendingRead(HANDLE file, void *buffer, DWORD requestedBytes, LPOVERLAPPED overlapped)
    {
        if (!overlapped) return;
        std::lock_guard<std::mutex> lock(pendingMutex);
        // An OVERLAPPED may be reused only after its previous operation completes.
        pendingReads.erase(overlapped);
        if (buffer && requestedBytes == Config::kExpectedReadLength && IsTargetHandle(file))
            pendingReads.emplace(overlapped, PendingRead{file, buffer, requestedBytes});
    }

    void CompletePendingRead(HANDLE file, LPOVERLAPPED overlapped, DWORD actualBytes)
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        const auto found = pendingReads.find(overlapped);
        if (found == pendingReads.end() || found->second.file != file) return;
        const PendingRead read = found->second;
        pendingReads.erase(found);
        PatchCompletedRead(read.file, read.buffer, actualBytes, read.requestedBytes);
    }

    void CompletePendingReadsAfterWait(DWORD count, const HANDLE *handles)
    {
        if (!handles) return;
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto it = pendingReads.begin(); it != pendingReads.end();)
        {
            LPOVERLAPPED overlapped = it->first;
            const PendingRead read = it->second;
            // A null event signals completion through the file handle. The low bit of hEvent is an IOCP flag.
            const HANDLE event = overlapped->hEvent
                ? reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(overlapped->hEvent) & ~ULONG_PTR{1}) : read.file;
            bool waited = false;
            for (DWORD i = 0; i < count; ++i) waited |= handles[i] == event;
            if (!waited) { ++it; continue; }
            DWORD actualBytes = 0;
            if (::GetOverlappedResult(read.file, overlapped, &actualBytes, FALSE))
            {
                PatchCompletedRead(read.file, read.buffer, actualBytes, read.requestedBytes);
                it = pendingReads.erase(it);
            }
            else if (GetLastError() != ERROR_IO_INCOMPLETE) it = pendingReads.erase(it);
            else ++it;
        }
    }

    void DropPendingRead(LPOVERLAPPED overlapped)
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingReads.erase(overlapped);
    }
}
