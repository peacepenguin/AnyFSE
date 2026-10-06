#pragma once

#include <string>

namespace AnyFSE::ToolsEx::ScheduledTask
{
    void RegisterAnyFSETask(const std::wstring &installPath);
    void DeleteAnyFSETask();

    // Starts the elevated HID/hotkey listener at logon through Task Scheduler, so it does not depend on
    // Explorer (HKCU\Run) or on the app being launched; it also works when the Xbox full screen experience replaces the shell.
    void RegisterListenerTask(const std::wstring &installPath);
    void DeleteListenerTask();
}
