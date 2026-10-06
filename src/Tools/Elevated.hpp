#pragma once

#include <functional>
#include <string>

namespace AnyFSE::Tools::Elevated
{
    // Register handlers at startup, before calling CallHandler().
    void Register(const std::wstring &name, std::function<void()> handler);
    bool CallHandler();
    // Synchronous: returns after the scheduled task exits. Concurrent calls are rejected.
    bool Call(const std::wstring &name);
    bool ElevatedStartupApps();
    // Starts the dedicated, long-lived listener task without waiting for it to exit.
    // Task Scheduler owns duplicate suppression for logon and on-demand requests.
    bool StartListenerTask();
}

namespace Elevated = AnyFSE::Tools::Elevated;
