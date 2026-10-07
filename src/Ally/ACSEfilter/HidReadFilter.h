#pragma once
#include <windows.h>

namespace ACSEFilter
{
    void PatchCompletedRead(HANDLE file, void *buffer, DWORD actualBytes, DWORD requestedBytes);
    void RememberPendingRead(HANDLE file, void *buffer, DWORD requestedBytes, LPOVERLAPPED overlapped);
    void CompletePendingRead(HANDLE file, LPOVERLAPPED overlapped, DWORD actualBytes);
    void CompletePendingReadsAfterWait(DWORD count, const HANDLE *handles);
    void DropPendingRead(LPOVERLAPPED overlapped);
    void CancelPreHookButtonReads();
}
