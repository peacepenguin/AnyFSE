#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "Ally/ACSEfilter/PendingIoRecovery.h"

namespace
{
    namespace c
    {
        constexpr wchar_t PipePrefix[] = L"\\\\.\\pipe\\AnyFSE.PendingIoRecoveryTest.";
        constexpr wchar_t KernelBaseModule[] = L"KernelBase.dll";
        constexpr char CompareHandlesExport[] = "CompareObjectHandles";
    }

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "%s (error %lu)\n", message, GetLastError());
            ExitProcess(1);
        }
    }

    struct Pipe
    {
        HANDLE server = INVALID_HANDLE_VALUE;
        HANDLE client = INVALID_HANDLE_VALUE;
        OVERLAPPED read{};
        BYTE buffer[6]{};

        explicit Pipe(const wchar_t* suffix)
        {
            const std::wstring name = std::wstring(c::PipePrefix) + std::to_wstring(GetCurrentProcessId()) + suffix;
            server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0, 1024, 0, nullptr);
            Check(server != INVALID_HANDLE_VALUE, "CreateNamedPipe");
            client = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(client != INVALID_HANDLE_VALUE, "Open pipe client");
            OVERLAPPED connect{};
            connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Check(connect.hEvent != nullptr, "Create connect event");
            const BOOL connected = ConnectNamedPipe(server, &connect);
            Check(connected || GetLastError() == ERROR_PIPE_CONNECTED, "Connect pipe");
            CloseHandle(connect.hEvent);
            read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Check(read.hEvent != nullptr, "Create read event");
        }

        void BeginRead()
        {
            ResetEvent(read.hEvent);
            DWORD bytes = 0;
            Check(!ReadFile(server, buffer, sizeof(buffer), &bytes, &read) && GetLastError() == ERROR_IO_PENDING, "Read must be pending");
        }

        void CompleteRead()
        {
            const BYTE report[] = {0x5A, 0x93, 0, 0, 0, 0};
            DWORD bytes = 0;
            Check(WriteFile(client, report, sizeof(report), &bytes, nullptr) != FALSE, "Write report");
            Check(WaitForSingleObject(read.hEvent, 2000) == WAIT_OBJECT_0, "Read completion timeout");
            Check(GetOverlappedResult(server, &read, &bytes, FALSE) && bytes == sizeof(report), "Read completion");
            Check(buffer[0] == 0x5A && buffer[1] == 0x93, "Report contents");
        }

        ~Pipe()
        {
            CloseHandle(client);
            CloseHandle(server);
            CloseHandle(read.hEvent);
        }
    };
}

int main()
{
    // The Windows runtime exports this API, but not every SDK ships kernelbase.lib.
    // Resolve it explicitly so the test only needs the standard kernel32 imports.
    const HMODULE kernelBase = GetModuleHandleW(c::KernelBaseModule);
    Check(kernelBase != nullptr, "Find KernelBase module");
    const FARPROC address = GetProcAddress(kernelBase, c::CompareHandlesExport);
    Check(address != nullptr, "Resolve CompareObjectHandles");
    decltype(&CompareObjectHandles) compareHandles = nullptr;
    static_assert(sizeof(compareHandles) == sizeof(address));
    std::memcpy(&compareHandles, &address, sizeof(compareHandles));

    Pipe target(L".target");
    Pipe unrelated(L".unrelated");
    target.BeginRead(); // Exists before interception/recovery, so no tracked buffer is available.
    unrelated.BeginRead();
    const auto matches = [&](HANDLE handle) { return compareHandles(handle, target.server) != FALSE; };
    Check(ACSEFilter::Startup::CancelPendingIo(matches) == ERROR_SUCCESS, "Snapshot recovery");
    Check(WaitForSingleObject(target.read.hEvent, 2000) == WAIT_OBJECT_0, "Cancellation timeout");
    DWORD bytes = 0;
    Check(!GetOverlappedResult(target.server, &target.read, &bytes, FALSE) && GetLastError() == ERROR_OPERATION_ABORTED,
        "Pre-hook read must complete as canceled");
    Check(!GetOverlappedResult(unrelated.server, &unrelated.read, &bytes, FALSE) && GetLastError() == ERROR_IO_INCOMPLETE,
        "Unrelated read must remain pending");
    target.BeginRead();
    target.CompleteRead(); // Cancellation must not close the device or poison subsequent reads.
    unrelated.CompleteRead();
    Check(ACSEFilter::Startup::CancelPendingIo(matches) == ERROR_SUCCESS, "Idle handles must be harmless");
    std::puts("Pending I/O recovery regression checks passed.");
}
