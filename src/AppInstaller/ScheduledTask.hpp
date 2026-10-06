#pragma once

#include <string>

namespace AnyFSE::ToolsEx::ScheduledTask
{
    void RegisterAnyFSETask(const std::wstring &installPath);
    void DeleteAnyFSETask();

    // Sole owner of elevated HID/hotkey listener startup, both at logon and on demand.
    // Runs the listener directly so Task Scheduler can track it and suppress duplicate starts.
    void RegisterListenerTask(const std::wstring &installPath);
    void DeleteListenerTask();
}
