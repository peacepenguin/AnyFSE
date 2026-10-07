#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstddef>
#include <windows.h>
#include <shellapi.h>
#include "Ally/Ally.hpp"
#include "Ally/Handlers.hpp"
#include "Logging/LogManager.hpp"
#include "Configuration/Config.hpp"
#include "Tools/Process.hpp"
#include "Tools/Paths.hpp"
#include "Tools/Registry.hpp"
#include "Tools/Unicode.hpp"
#include "Tools/PowerEfficiency.hpp"
#include "Tools/Elevated.hpp"
#include "App/Constants.hpp"
#include "Ally.hpp"
#include "Ally/Services.hpp"
#include "App/GamingExperience.hpp"

using namespace AnyFSE::App;

namespace Ally
{

    static Logger log = LogManager::GetLogger("HIDListener");
    static const wchar_t *HidListenerClass = L"HIDListener";
    static constexpr int HotkeyEnterFSEWithReboot = (int)GamingExperience::ConfirmationMode::Reboot;
    static constexpr int HotkeyEnterFSENow = (int)GamingExperience::ConfirmationMode::Now;
    static constexpr UINT ReloadSettingsMessage = WM_APP + 1;
    static constexpr UINT_PTR ReconcileTimer = 1;

    static void ReconcileInjectorService()
    {
        static bool configured = false;
        // Service configuration belongs to the elevated listener, not the settings UI.
        // CreateInjector is idempotent and also repairs an installed but stopped service.
        if (Config::AllyHidEnable)
        {
            if (!configured || !Services::IsInjectorServiceRunning()) configured = Services::EnableInjectorService();
        }
        else
        {
            Services::DisableInjectorService();
            configured = false;
        }
    }

    static bool UpdateHotkeys(HWND hWnd)
    {
        UnregisterHotKey(hWnd, HotkeyEnterFSEWithReboot);
        UnregisterHotKey(hWnd, HotkeyEnterFSENow);

        if (!Config::HotkeysEnable)
        {
            return false;
        }

        bool rebootRegistered = RegisterHotKey(
            hWnd,
            HotkeyEnterFSEWithReboot,
            MOD_WIN | MOD_SHIFT | MOD_NOREPEAT,
            VK_F11) != FALSE;
        if (!rebootRegistered)
        {
            log.Warn(log.APIError(), "Could not register Win+Shift+F11");
        }

        bool nowRegistered = RegisterHotKey(
            hWnd,
            HotkeyEnterFSENow,
            MOD_WIN | MOD_CONTROL | MOD_NOREPEAT,
            VK_F11) != FALSE;
        if (!nowRegistered)
        {
            log.Warn(log.APIError(), "Could not register Win+Ctrl+F11");
        }

        return rebootRegistered || nowRegistered;
    }

    bool IsSupported()
    {
        // Enumeration can finish after the logon task starts, or change after a device reconnect.
        return FindHIDDevice() != nullptr;
    }

    RogAllyVersion GetRogAllyVersion()
    {
        static RogAllyVersion version = RogAllyVersion::NotDefined;

        if (version == RogAllyVersion::NotDefined)
        {
            std::wstring product = Registry::ReadString(
                L"HKLM\\SYSTEM\\HardwareConfig\\Current",
                L"BaseBoardProduct",
                L"");

            log.Debug("Detected ASUS device: '%s'", Unicode::to_string(product).c_str());

            version =   (product == L"RC73XA") ? RogAllyVersion::XboxROGAllyX
                      : (product == L"RC73YA") ? RogAllyVersion::XboxROGAlly
                      : (product == L"RC72LA") ? RogAllyVersion::ROGAllyX
                      : (product == L"RC71L")  ? RogAllyVersion::ROGAlly
                                               : RogAllyVersion::NotSupported;
        }
        return version;
    }

    bool IsXBoxRogAlly()
    {
        return GetRogAllyVersion() == RogAllyVersion::XboxROGAllyX
            || GetRogAllyVersion() == RogAllyVersion::XboxROGAlly;
    }

    HANDLE FindHIDDevice()
    {
        UINT numDevices = 0;
        // Call once with NULL to get the required array size
        if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return nullptr;

        std::vector<RAWINPUTDEVICELIST> deviceList(numDevices);
        const UINT deviceCount = GetRawInputDeviceList(deviceList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST));
        if (deviceCount == UINT(-1)) return nullptr;

        USHORT usagePage = 0;
        USHORT usage = 0;
        HANDLE hDevice = NULL;

        for (UINT i = 0; i < deviceCount; i++)
        {
            if (deviceList[i].dwType == RIM_TYPEHID)
            {
                UINT pathSize = 0;
                GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICENAME, NULL, &pathSize);

                std::wstring devicePath(pathSize, 0);
                GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICENAME, devicePath.data(), &pathSize);

                std::transform(devicePath.begin(), devicePath.end(), devicePath.begin(), ::toupper);

                if (devicePath.find(L"VID_0B05") != std::string::npos)
                {
                    log.Debug("Detected ASUS HID device at: '%s'", Unicode::to_string(devicePath).c_str());
                }

                if (devicePath.find(L"VID_0B05") != std::string::npos
                    && ( devicePath.find(L"PID_1ABE") != std::string::npos
                        || devicePath.find(L"PID_1B4C") != std::string::npos )
                    && devicePath.find(L"MI_02") != std::string::npos
                    && devicePath.find(L"COL01") != std::string::npos)
                {
                    log.Debug("Selected ASUS HID device: '%s'", Unicode::to_string(devicePath).c_str());
                    return deviceList[i].hDevice;
                }
            }
        }
        return NULL;
    }

    bool GetRawInputDevice(HANDLE hDevice, HWND hWnd, RAWINPUTDEVICE *pRid)
    {
        RID_DEVICE_INFO deviceInfo;
        deviceInfo.cbSize = sizeof(RID_DEVICE_INFO);
        UINT infoSize = sizeof(RID_DEVICE_INFO);

        if (GetRawInputDeviceInfo(hDevice, RIDI_DEVICEINFO, &deviceInfo, &infoSize) > 0)
        {
            pRid->dwFlags = RIDEV_INPUTSINK;
            pRid->hwndTarget = hWnd;
            pRid->usUsagePage = deviceInfo.hid.usUsagePage;
            pRid->usUsage = deviceInfo.hid.usUsage;
            return true;
        }
        return false;
    }

    void Load()
    {
        Config::Load();
        ButtonBind.clear();
        ButtonBind[Ally::EventCode::ACPress] =          IsXBoxRogAlly() ? Handlers::GetByName(Config::AllyHidLibraryPress) : Handlers::GetByName(Config::AllyHidACPress);
        ButtonBind[Ally::EventCode::ACHold] =           Handlers::GetByName(Config::AllyHidACHold);
        ButtonBind[Ally::EventCode::CCPress] =          IsXBoxRogAlly() ? Handlers::GetByName(Config::AllyHidACPress) : Handlers::GetByName(Config::AllyHidCCPress);
        ButtonBind[Ally::EventCode::LibraryPress] =     Handlers::GetByName(Config::AllyHidLibraryPress);

        ButtonBind[Ally::EventCode::MACPress] =         IsXBoxRogAlly() ? Handlers::GetByName(Config::AllyHidModeLibraryPress) : Handlers::GetByName(Config::AllyHidModeACPress);
        ButtonBind[Ally::EventCode::MACHold] =          Handlers::GetByName(Config::AllyHidModeACHold);
        ButtonBind[Ally::EventCode::MCCPress] =         IsXBoxRogAlly() ? Handlers::GetByName(Config::AllyHidModeACPress) : Handlers::GetByName(Config::AllyHidModeCCPress);
        ButtonBind[Ally::EventCode::MLibraryPress] =    Handlers::GetByName(Config::AllyHidModeLibraryPress);

        ButtonBind[Ally::EventCode::ROGHoldRelease] =   NULL;
        ButtonBind[Ally::EventCode::ModePress] =        NULL;
        ButtonBind[Ally::EventCode::Release] =          NULL;
        ButtonBind[Ally::EventCode::Unknown] =          NULL;

        if (Config::AllyHidExtraCommandsEnable)
        {
            ButtonBind[Ally::EventCode::ToggleMicrophone] = Handlers::ToggleMicrophone;
            ButtonBind[Ally::EventCode::ScreenShot] =       Handlers::TakeScreenShoot;
            ButtonBind[Ally::EventCode::ShowKeyboard] =     Handlers::ShowKeyboard;
            ButtonBind[Ally::EventCode::ToggleRecord] =     Handlers::ToggleRecord;
        }

        log.Trace("Bind Mode is %s", IsXBoxRogAlly() ? "XBoxRoxAlly" : "RogAlly");

        log.Trace("Bind ACPress to %s", IsXBoxRogAlly() ? Config::AllyHidLibraryPress : Config::AllyHidACPress );
        log.Trace("Bind ACHold to %s", Config::AllyHidACHold);
        log.Trace("Bind CCPress to %s", IsXBoxRogAlly() ? Config::AllyHidACPress : Config::AllyHidCCPress );
        log.Trace("Bind LibraryPress to %s", Config::AllyHidLibraryPress );

        log.Trace("Bind Mode + ACPress to %s", IsXBoxRogAlly() ? Config::AllyHidModeLibraryPress : Config::AllyHidModeACPress );
        log.Trace("Bind Mode + ACHold to %s", Config::AllyHidModeACHold);
        log.Trace("Bind Mode + CCPress to %s", IsXBoxRogAlly() ? Config::AllyHidModeACPress : Config::AllyHidModeCCPress );
        log.Trace("Bind Mode + LibraryPress to %s", Config::AllyHidModeLibraryPress );
    }

    void OnInput(bool allyEnabled, HANDLE hDevice, HRAWINPUT rawInput, bool * pbModePressed)
    {
        if (!allyEnabled)
        {
            return;
        }

        UINT size = 0;
        if (GetRawInputData(rawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)
            || size < sizeof(RAWINPUTHEADER)) return;
        std::vector<BYTE> data(size);
        if (GetRawInputData(rawInput, RID_INPUT, data.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
        const auto *raw = reinterpret_cast<const RAWINPUT *>(data.data());
        if (raw->header.dwType != RIM_TYPEHID
            || (raw->header.hDevice != hDevice && raw->header.hDevice != FindHIDDevice())) return;
        constexpr size_t reportOffset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
        if (size < reportOffset || raw->data.hid.dwSizeHid < 2
            || raw->data.hid.dwCount > (size - reportOffset) / raw->data.hid.dwSizeHid) return;

        for (DWORD i = 0; i < raw->data.hid.dwCount; ++i)
        {
            const BYTE *report = raw->data.hid.bRawData + size_t(i) * raw->data.hid.dwSizeHid;
            if (report[0] != 0x5A) continue;
            EventCode buttonCode = static_cast<EventCode>(report[1]);
            log.Trace("Ally button report: %u", unsigned(report[1]));
            if (buttonCode == ModePress) *pbModePressed = true;
            else if (buttonCode == Release) *pbModePressed = false;
            else if (*pbModePressed && (buttonCode == ACHold || buttonCode == ACPress || buttonCode == CCPress || buttonCode == LibraryPress))
                buttonCode = static_cast<EventCode>(buttonCode + ModePress);

            const auto binding = ButtonBind.find(buttonCode);
            if (binding != ButtonBind.end() && binding->second) binding->second();
        }
    }

    void OnHotkey(int id)
    {
        switch (id)
        {
            case HotkeyEnterFSEWithReboot:
                GamingExperience::EnterFSEModeWithReboot();
                break;
            case HotkeyEnterFSENow:
                GamingExperience::EnterFSEModeNow();
                break;
        }
    }

    void OnUser(HWND hwnd, bool& allyEnabled, HANDLE& hDevice, RAWINPUTDEVICE& rid)
    {
        Config::Load();
        bool enableAlly = Config::AllyHidEnable && Ally::IsSupported();
        if (enableAlly)
        {
            Ally::Load();
            if (!allyEnabled)
            {
                hDevice = Ally::FindHIDDevice();
                if (Ally::GetRawInputDevice(hDevice, hwnd, &rid))
                {
                    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
                    {
                        log.Error(log.APIError(), "Could not register Ally raw input");
                        enableAlly = false;
                    }
                }
                else enableAlly = false;
            }
        }
        else if (allyEnabled)
        {
            rid.dwFlags = RIDEV_REMOVE;
            rid.hwndTarget = NULL;
            if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
                log.Error(log.APIError(), "Could not unregister Ally raw input");
            rid = {};
            hDevice = NULL;
        }

        allyEnabled = enableAlly;
        ReconcileInjectorService();
        log.Info("Listener raw input ready=%d device=%p window=%p FSE=%d",
            allyEnabled, hDevice, hwnd, GamingExperience::IsFullscreenMode());
        bool hotkeysRegistered = UpdateHotkeys(hwnd);
        if (!Config::AllyHidEnable && !hotkeysRegistered)
        {
            PostQuitMessage(0);
        }
    }

    DWORD WINAPI HIDListener(LPVOID lpParam)
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        log.Info("Listener started: pid=%lu session=%lu FSE=%d AllyHid=%d hotkeys=%d",
            GetCurrentProcessId(), session, GamingExperience::IsFullscreenMode(), Config::AllyHidEnable, Config::HotkeysEnable);
        bool allyEnabled = Config::AllyHidEnable && Ally::IsSupported();
        if (!Config::AllyHidEnable && !Config::HotkeysEnable)
        {
            return -1;
        }

        HANDLE hDevice = allyEnabled ? Ally::FindHIDDevice() : NULL;

        if (FindWindow(HidListenerClass,NULL) != NULL)
        {
            log.Info("Listener startup skipped: sink window already exists");
            return -1;
        }

        // Task Scheduler serializes normal starts with IGNORE_NEW. Keep this guard for old binaries
        // during upgrades and manual /HidListenerJob invocations outside the scheduled task.
        struct InstanceGuard
        {
            HANDLE handle = CreateMutexW(nullptr, TRUE, Constants::HidListenerMutex);
            bool taken = handle != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
            ~InstanceGuard() { if (handle) CloseHandle(handle); }
        } instance;
        if (!instance.taken)
        {
            log.Debug("Another HID/hotkey listener already owns the instance mutex");
            return -1;
        }

        AnyFSE::Tools::EnablePowerEfficencyMode(true);
        if (allyEnabled)
        {
            Load();
        }

        // Create hidden window for raw input
        WNDCLASS wc = {0};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = HidListenerClass;
        RegisterClass(&wc);

        HWND hwnd = CreateWindow(HidListenerClass, NULL, 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);
        if (!hwnd)
        {
            log.Error(log.APIError(), "Listener window creation failed");
            return -1;
        }

        // Only permit a parameter-free reload notification from the unelevated settings UI.
        // The listener reads the saved configuration itself; no privileged command is carried in the message.
        if (!ChangeWindowMessageFilterEx(hwnd, ReloadSettingsMessage, MSGFLT_ALLOW, nullptr))
            log.Error(log.APIError(), "Could not allow listener settings notifications");
        ReconcileInjectorService();
        if (!SetTimer(hwnd, ReconcileTimer, 10000, nullptr))
            log.Error(log.APIError(), "Could not start injector reconciliation timer");

        // Register raw input for ASUS Rog Ally service device
        RAWINPUTDEVICE rid = {};
        if (allyEnabled && Ally::GetRawInputDevice(hDevice, hwnd, &rid))
        {
            if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            {
                log.Error(log.APIError(), "Could not register Ally raw input");
                allyEnabled = false;
            }
        }
        else allyEnabled = false;

        log.Info("Listener raw input ready=%d device=%p window=%p FSE=%d",
            allyEnabled, hDevice, hwnd, GamingExperience::IsFullscreenMode());
        bool hotkeysRegistered = UpdateHotkeys(hwnd);
        if (!Config::AllyHidEnable && !hotkeysRegistered)
        {
            DestroyWindow(hwnd);
            return -1;
        }

        log.Trace("Starting Ally HID and hotkey sink window");

        MSG msg;

        bool bModePressed = false;

        while (GetMessage(&msg, NULL, 0, 0) > 0)
        {
            switch (msg.message)
            {
            case WM_INPUT:
                OnInput(allyEnabled, hDevice, (HRAWINPUT)msg.lParam, &bModePressed);
                break;
            case WM_HOTKEY:
                OnHotkey((int)msg.wParam);
                break;
            case ReloadSettingsMessage:
                OnUser(hwnd, allyEnabled, hDevice, rid);
                bModePressed = false;
                break;
            case WM_TIMER:
                if (msg.wParam == ReconcileTimer)
                {
                    if (Config::AllyHidEnable && !allyEnabled) OnUser(hwnd, allyEnabled, hDevice, rid);
                    else ReconcileInjectorService();
                }
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        UnregisterHotKey(hwnd, HotkeyEnterFSEWithReboot);
        UnregisterHotKey(hwnd, HotkeyEnterFSENow);
        KillTimer(hwnd, ReconcileTimer);
        DestroyWindow(hwnd);
        log.Trace("Exit HID and hotkey sink thread");
        return 0;
    }

    void EnableACSEInjector(bool bEnable)
    {
        bEnable &= IsNativeHandlerEnabled();

        if (IsInjectorEnabled() == bEnable )
        {
            return;
        }

        const std::wstring injectorExe = std::filesystem::path(AnyFSE::Tools::Paths::GetInstallPath()).append(Constants::InjectorExe).wstring();
        SHELLEXECUTEINFOW execute = {};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        execute.lpFile = injectorExe.c_str();
        execute.lpParameters = bEnable ? Constants::CreateServiceArgument : Constants::RemoveServiceArgument;
        execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute))
        {
            log.Error(log.APIError(), "Could not launch injector service operation");
            return;
        }
        if (!execute.hProcess)
        {
            log.Error("Injector service operation returned no process handle");
            return;
        }
        DWORD exitCode = ERROR_GEN_FAILURE;
        if (WaitForSingleObject(execute.hProcess, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(execute.hProcess, &exitCode))
            log.Error(log.APIError(), "Could not obtain injector service operation result");
        else if (exitCode != ERROR_SUCCESS)
            log.Error("Injector service operation failed: %lu", exitCode);
        CloseHandle(execute.hProcess);
    }

    bool IsNativeHandlerEnabled()
    {
        return 0 != Process::FindFirstByExe(Constants::AsusOptimizationProcess);
    }

    bool IsInjectorEnabled()
    {
        return 0 != Process::FindFirstByExe(Constants::InjectorExe);
    }

    bool UpdateHidListener()
    {
        HWND hWnd = FindWindow(Ally::HidListenerClass, L"");
        if (hWnd)
        {
            if (PostMessage(hWnd, ReloadSettingsMessage, 0, 0)) return true;
            log.Error(log.APIError(), "Could not notify HID listener of settings changes");
        }
        return false;
    }

    bool IsListenerRequired()
    {
        return Config::HotkeysEnable || Config::AllyHidEnable;
    }

    void RemoveLegacyListenerAutorun()
    {
        if (Registry::ValueExists(Constants::HidListenerAutorunKey, Constants::HidListenerAutorunValue)
            && !Registry::DeleteValue(Constants::HidListenerAutorunKey, Constants::HidListenerAutorunValue))
            log.Error("Could not remove legacy HID listener Run entry");
    }

    bool EnsureListener()
    {
        RemoveLegacyListenerAutorun();
        if (CheckListener()) Elevated::StartListenerTask();
        // This is called in WinMain's early-exit chain; starting the listener does not handle the main request.
        return false;
    }

    bool CheckListener()
    {
        return IsListenerRequired() && FindWindow(HidListenerClass, NULL) == NULL;
    }
}
