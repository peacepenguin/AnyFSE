#pragma once

namespace AnyFSE::App::Constants
{
    // Package / identity
    inline constexpr wchar_t PackageFamilyName[] = L"ArtemShpynov.AnyFSE_by4wjhxmygwn4";
    inline constexpr wchar_t AppUserModelId[] = L"ArtemShpynov.AnyFSE_by4wjhxmygwn4!App";
    inline constexpr wchar_t PackageAtomName[] = L"ArtemShpynov.AnyFSE_by4wjhxmygwn4";
    inline constexpr wchar_t WaitingExitMutex[] = L"ArtemShpynov.AnyFSE_WaitingExitFSE";

    // Desktop Xbox startup support (explicit opt-in system-file patch).
    inline constexpr wchar_t ElevatedApplyDesktopXboxStartup[] = L"ApplyDesktopXboxStartup";
    inline constexpr wchar_t ElevatedRestoreDesktopXboxStartup[] = L"RestoreDesktopXboxStartup";
    inline constexpr wchar_t XboxStartupGameModeDll[] = L"gamemode.dll";
    inline constexpr wchar_t XboxStartupSettingsDll[] = L"SettingsHandlers_Gaming.dll";
    inline constexpr wchar_t XboxStartupShellDll[] = L"twinui.pcshell.dll";
    inline constexpr char XboxStartupSupportedExport[] = "IsGamingFullScreenExperienceSupported";
    inline constexpr char XboxStartupCanSetExport[] = "CanSetGamingFullScreenExperience";
    inline constexpr char XboxStartupSetExport[] = "SetGamingFullScreenExperience";
    inline constexpr wchar_t XboxStartupNtDll[] = L"ntdll.dll";
    inline constexpr char XboxStartupVersionExport[] = "RtlGetVersion";
    inline constexpr char XboxStartupDeviceFormExport[] = "RtlGetDeviceFamilyInfoEnum";
    inline constexpr wchar_t WindowsVersionRegKey[] = L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    inline constexpr wchar_t WindowsRevisionRegValue[] = L"UBR";
    inline constexpr wchar_t XboxStartupBackupDirectory[] = L"AnyFSE-XboxStartupBackups";
    inline constexpr wchar_t XboxStartupBackupSuffix[] = L".original";
    inline constexpr wchar_t XboxStartupNewSuffix[] = L".anyfse-new-";
    inline constexpr wchar_t XboxStartupOldSuffix[] = L".anyfse-old-";
    inline constexpr wchar_t XboxStartupBackupSecurity[] = L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)";

    // Product files
    inline constexpr wchar_t AnyFseSettingsDll[] = L"AnyFSE.Settings.dll";
    inline constexpr wchar_t AnyFseExe[] = L"AnyFSE.exe";
    inline constexpr const wchar_t* PopupperProcessNames[] = { L"SystemSettings.exe" };
    inline constexpr wchar_t AnyFseTaskName[] = L"AnyFSE";
    inline constexpr wchar_t AnyFseListenerTaskName[] = L"AnyFSE Listener";
    inline constexpr wchar_t HidListenerJobArgument[] = L"/HidListenerJob";
    inline constexpr wchar_t HidListenerMutex[] = L"Local\\AnyFSE.HidListener";
    inline constexpr wchar_t ListenerTaskLogonDelay[] = L"PT5S";
    inline constexpr wchar_t ElevatedEventPrefix[] = L"Local\\AnyFSE.Task.Command.";
    inline constexpr wchar_t ElevatedCallEvent[] = L"Local\\AnyFSE.Task.Call";
    inline constexpr wchar_t ElevatedStartLauncher[] = L"StartLauncher";
    inline constexpr wchar_t ElevatedStartupApps[] = L"StartupApps";
    inline constexpr wchar_t ElevatedEnableGamingHandheld[] = L"EnableGamingHandheld";
    inline constexpr wchar_t ElevatedRestoreGamingPC[] = L"RestoreGamingPC";

    inline constexpr wchar_t AnyFseTaskArgument[] = L"/task";
    inline constexpr char AnyFseTaskArgumentA[] = "/task";
    inline constexpr wchar_t TaskSchedulerRoot[] = L"\\";
    inline constexpr wchar_t AnyFseTaskSecurityPrefix[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;";
    inline constexpr wchar_t InstallerExe[] = L"AnyFSE.Installer.exe";
    inline constexpr wchar_t UninstallerExe[] = L"unins000.exe";
    inline constexpr wchar_t UninstallerUpdateArguments[] = L"/s /u";
    inline constexpr wchar_t InjectorExe[] = L"AnyFSE.ACSEFilterInjector.exe";

    // ASUS / ACSE integration
    inline constexpr wchar_t InjectorServiceName[] = L"ACSEFilterInjector";
    inline constexpr wchar_t InjectorServiceArgument[] = L"--service";
    inline constexpr wchar_t CreateServiceArgument[] = L"--create-service";
    inline constexpr wchar_t RemoveServiceArgument[] = L"--remove-service";
    inline constexpr wchar_t InjectorServiceDisplayName[] = L"AnyFSE ACSE Filter Injector";
    inline constexpr wchar_t InjectorServiceDescription[] = L"Injects ACSEFilterHook into ASUS Optimization process and blocks it from ASUS-specific keys processing.";


    inline constexpr wchar_t AsusOptimizationService[] = L"ASUSOptimization";
    inline constexpr wchar_t AsusOptimizationProcess[] = L"AsusOptimization.exe";
    inline constexpr wchar_t ArmouryCrateServiceProcess[] = L"ArmouryCrateSE.Service.exe";

    // Registry
    inline constexpr wchar_t UninstallAnyFseRegKey[] = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AnyFSE";
    inline constexpr wchar_t DeviceFormRegKey[] = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
    inline constexpr wchar_t DeviceFormRegValue[] = L"DeviceForm";
    inline constexpr wchar_t DeviceFormBackupRegValue[] = L"DeviceForm_AnyFSE";
    inline constexpr unsigned long HandheldDeviceForm = 46;
    inline constexpr wchar_t GamingHomeAppRegKey[] = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\GamingConfiguration";
    inline constexpr wchar_t GamingHomeAppRegValue[] = L"GamingHomeApp";
    inline constexpr wchar_t StartupToGamingHomeRegValue[] = L"StartupToGamingHome";
    inline constexpr wchar_t StartupToGamingHomeRestoreRegValue[] = L"StartupToGamingHome_Restore";
    inline constexpr wchar_t SystemDialogResultsRegKey[] = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\GamingConfiguration\\SystemDialogResults";
    inline constexpr wchar_t EnterGamingPostureConfirmationRegValue[] = L"EnterGamingPostureConfirmation";
    inline constexpr wchar_t EnterGamingPostureConfirmationConfigRegValue[] = L"EnterGamingPostureConfirmation_Config";
    inline constexpr wchar_t EnterGamingPostureConfirmationRestoreRegValue[] = L"EnterGamingPostureConfirmation_Restore";

    // Installer / updater assets
    inline constexpr wchar_t RootCertificateStore[] = L"ROOT";
    inline constexpr wchar_t TrustedPeopleCertificateStore[] = L"TrustedPeople";
    inline constexpr wchar_t PublisherCertFriendlyName[] = L"AnyFSE package temp certificate";
    inline constexpr wchar_t TempCertFile[] = L"AnyFSE.Temp.cer";
    inline constexpr wchar_t AppxFilePrefix[] = L"AnyFSE-";
    inline constexpr wchar_t ZipExtension[] = L".zip";
    inline constexpr wchar_t ReleaseZipPrefix[] = L"AnyFSE.";
    inline constexpr wchar_t TempInstallDirName[] = L"AnyFSE_install";
    inline constexpr wchar_t GitHubReleaseRoot[] = L"https://github.org/ashpynov/AnyFSE/releases/download/v";
    inline constexpr wchar_t CodebergReleaseRoot[] = L"https://codeberg.org/ashpynov/AnyFSE/releases/download/v";
    inline constexpr wchar_t UpdaterCommandMessage[] = L"AnyFSE.Updater.Command";
    inline constexpr wchar_t AnyFseProtocolSettings[] = L"anyfse://settings";
    inline constexpr wchar_t ProtocolSeparator[] = L"://";
    // Migration-only identifiers: no new HKCU Run entry is written.
    inline constexpr wchar_t HidListenerAutorunKey[] = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    inline constexpr wchar_t HidListenerAutorunValue[] = L"AnyFSE Hotkeys";
    inline constexpr wchar_t MainWindowClass[] = L"AnyFSE";
    inline constexpr wchar_t VideoWindowClass[] = L"AnyFSE.VideoWindow";
    inline constexpr wchar_t SettingsDialogClass[] = L"AnyFSESettingsDialogClass";
    inline constexpr wchar_t SettingsLineClass[] = L"AnyFSE_SettingsLineClass";
    inline constexpr wchar_t UpdaterNotifyWindowClass[] = L"AnyFSE_Updater_NotifyWnd";
    inline constexpr wchar_t UpdaterUserAgentHeader[] = L"User-Agent: AnyFSE-Updater\r\n";
    inline constexpr wchar_t UpdaterGitHubAcceptHeader[] = L"Accept: application/vnd.github.v3+json\r\n";
    inline constexpr wchar_t UpdaterSessionUserAgent[] = L"AnyFSE-Updater/1.0";
    inline constexpr wchar_t UpdaterTempExePrefix[] = L"AnyFSE.";
    inline constexpr wchar_t UpdaterTempExeSuffix[] = L".Update.exe";

}

namespace Constants = AnyFSE::App::Constants;
