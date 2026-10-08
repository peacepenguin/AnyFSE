#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#include <stdexcept>
#include "Tools/XboxStartup.hpp"

#include "Tools/Registry.hpp"
#include "Tools/Elevated.hpp"
#include "Tools/Process.hpp"
#include "Tools/Paths.hpp"
#include "Logging/LogManager.hpp"
#include "Configuration/Config.hpp"
#include "App/Launchers.hpp"
#include "App/GamingExperience.hpp"
#include "App/Constants.hpp"
#include "App/App.hpp"
#include "Ally/Handlers.hpp"
#include "Ally/Ally.hpp"

namespace AnyFSE::App::CmdLine
{
    static Logger log = LogManager::GetLogger("CmdLine");

    bool AsHidListener(LPSTR lpCmdLine)
    {
        for (char *a = lpCmdLine; *a; a++)
        {
            if (_strnicmp(a, "/HidListener", 12) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool AsHidListenerJob(LPSTR lpCmdLine)
    {
        for (char *a = lpCmdLine; *a; a++)
        {
            if (_strnicmp(a, "/HidListenerJob", 15) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool AsElevated(LPSTR lpCmdLine)
    {
        return lpCmdLine && _stricmp(lpCmdLine, Constants::AnyFseTaskArgumentA) == 0;
    }

    bool AsFSE(LPSTR lpCmdLine)
    {
        for (char *a = lpCmdLine; *a; a++)
        {
            if (_strnicmp(a, "/FSE", 4) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool AsFSENow(LPSTR lpCmdLine)
    {
        for (char *a = lpCmdLine; *a; a++)
        {
            if (_strnicmp(a, "/FSENow", 7) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool AsFSEReboot(LPSTR lpCmdLine)
    {
        for (char *a = lpCmdLine; *a; a++)
        {
            if (_strnicmp(a, "/FSEReboot", 10) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool AsSettings(LPSTR lpCmdLine)
    {
        // no gamingapp:/// protocol specified
        if (strlen(lpCmdLine) == 0)
        {
            return true;
        }
        // Settings or Config is not exists
        //
        if (!Config::IsConfigured())
        {
            return true;
        }

        for (char *a = lpCmdLine; *a; a++)
        {
            if (_strnicmp(a, "/Settings", 9) == 0)
            {
                return true;
            }
        }

        // or Launcher == None or Launcher == Xbox
        if (Config::Launcher.Type == LauncherType::None
         || Config::Launcher.Type == LauncherType::Native)
        {
            return true;
        }

        // Registry != AnyFSE
        const std::wstring AnyFSEApp = Constants::AppUserModelId;
        const std::wstring selectedApp = Registry::ReadString(
            Constants::GamingHomeAppRegKey,
            Constants::GamingHomeAppRegValue);

        if (_wcsicmp(selectedApp.c_str(), AnyFSEApp.c_str() ) != 0)
        {
            return true;
        }

        if (Config::Launcher.StartCommand.find(L"://") == std::wstring::npos)
        {
            namespace fs = std::filesystem;
            if (!fs::exists(Config::Launcher.StartCommand))
            {
                log.Warn("Launcher file is unavailable; opening settings");
                return true;
            }
        }

        return false;
    }

    bool Elevated(LPSTR lpCmdLine, int &result)
    {
        if (!AsElevated(lpCmdLine))
        {
            return false;
        }

        Elevated::Register(Constants::ElevatedStartLauncher,  []() { Launchers::StartLauncher(false); });
        Elevated::Register(Constants::ElevatedStartupApps, []() { Launchers::LaunchStartupApps(true); });
        Elevated::Register(Constants::ElevatedApplyDesktopXboxStartup, Tools::XboxStartup::Apply);
        Elevated::Register(Constants::ElevatedApplyDesktopXboxStartupUnverified, Tools::XboxStartup::ApplyUnverified);
        Elevated::Register(Constants::ElevatedRestoreDesktopXboxStartup, Tools::XboxStartup::Restore);
        Elevated::Register(Constants::ElevatedRestoreGamingPC, GamingExperience::RestoreGamingPC);

        result = Elevated::CallHandler() ? 0 : 1;
        return true;
    }

    bool HidListenerJob(LPSTR lpCmdLine, int& result)
    {
        if (!AsHidListenerJob(lpCmdLine))
        {
            return false;
        }

        Ally::RemoveLegacyListenerAutorun();

        if (Ally::IsListenerRequired())
        {
            log.Debug("Starting background HID/hotkey listener\n");
            AnyFSE::Logging::LogManager::Initialize("AnyFSE/BackgroundListener", Config::LogLevel, Config::LogPath);
            result = Ally::HIDListener(NULL);
        }

        return true;
    }

    bool HidListener(LPSTR lpCmdLine, int& result)
    {
        if (!AsHidListener(lpCmdLine))
        {
            return false;
        }

        // Compatibility with existing protocol links and old Run entries: delegate to the same task.
        Ally::RemoveLegacyListenerAutorun();
        result = (!Ally::IsListenerRequired() || Elevated::StartListenerTask()) ? 0 : 1;
        return true;
    }

    bool DesktopXboxStartup(LPSTR lpCmdLine, int& result)
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return false;
        const bool requested = argc >= 2 && !_wcsicmp(argv[1], Constants::XboxStartupCommand);
        if (!requested) { LocalFree(argv); return false; }
        const std::wstring verb = argc == 3 ? argv[2] : L"";
        LocalFree(argv);
        const auto is = [&](const wchar_t *name) { return !_wcsicmp(verb.c_str(), name); };
        if (is(Constants::XboxStartupVerbStatus) || is(Constants::XboxStartupVerbVerify))
        {
            const auto status = Tools::XboxStartup::Inspect();
            const auto text = status.details + (status.unverifiedPending ? L"\nUnverified checks pending confirmation:\n" + status.unverifiedDetails : L"");
            log.Info("Desktop Xbox startup status:\n%ls", text.c_str());
            MessageBoxW(nullptr, text.c_str(), L"AnyFSE desktop Xbox startup", MB_OK | MB_ICONINFORMATION);
            result = status.canApply ? 0 : 1;
        }
        else if (is(Constants::XboxStartupVerbScan))
        {
            const auto report = Tools::XboxStartup::Scan();
            log.Info("Device-form check scan:\n%ls", report.c_str());
            MessageBoxW(nullptr, report.c_str(), L"AnyFSE device-form check scan", MB_OK | MB_ICONINFORMATION);
            result = 0;
        }
        else if (is(Constants::XboxStartupVerbApply) || is(Constants::XboxStartupVerbApplyUnverified) || is(Constants::XboxStartupVerbRestore))
        {
            const auto handler = is(Constants::XboxStartupVerbApply) ? Constants::ElevatedApplyDesktopXboxStartup
                : is(Constants::XboxStartupVerbRestore) ? Constants::ElevatedRestoreDesktopXboxStartup
                : Constants::ElevatedApplyDesktopXboxStartupUnverified;
            result = Elevated::Call(handler) ? 0 : 1;
        }
        else
        {
            MessageBoxW(nullptr, L"Usage: AnyFSE.exe /XboxStartup status|verify|scan|apply|apply-unverified|restore", L"AnyFSE desktop Xbox startup",
                MB_OK | MB_ICONERROR);
            result = 2;
        }
        return true;
    }

    bool FSE(LPSTR lpCmdLine, int& result)
    {
        if (!AsFSE(lpCmdLine))
        {
            return false;
        }

        result = 0;

        if (GamingExperience::IsFullscreenMode())
        {
            return true;
        }

        GamingExperience::ConfirmationMode mode = AsFSEReboot(lpCmdLine) ? GamingExperience::Reboot
                                                : AsFSENow(lpCmdLine)    ? GamingExperience::Now
                                                                         : GamingExperience::Ask;

        result = GamingExperience::EnterFSEMode( mode );
        return true;
    }

    bool Settings(LPSTR lpCmdLine, int &result)
    {
        if (!AsSettings(lpCmdLine))
        {
            return false;
        }

        App::ShowSettings();
        result = 0;
        return true;
    }
}