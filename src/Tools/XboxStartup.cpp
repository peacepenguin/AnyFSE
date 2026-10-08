// XboxStartupEnabler integration, Copyright (c) 2026 Victor Jimenez (MIT).
// See THIRD_PARTY_NOTICES/XboxStartupEnabler.txt for the upstream license.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <exception>
#include <cwchar>
#include <utility>
#include "XboxStartup.hpp"
#include "XboxStartupPlan.hpp"
#include "Tools/Unicode.hpp"
#include "Tools/Registry.hpp"
#include "App/Constants.hpp"
#include "Logging/LogManager.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace AnyFSE::Tools::XboxStartup
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace c = App::Constants;
        Logger log = LogManager::GetLogger("XboxStartup");
        struct Spec { const wchar_t *name; Target target; bool optional = false; };
        // Matches upstream's generic handheld-check patch. Optional targets are skipped when absent from this Windows build.
        const Spec specs[] = {{c::XboxStartupGameModeDll, Target::GameMode},
            {c::XboxStartupSettingsDll, Target::Settings}, {c::XboxStartupShellDll, Target::Shell},
            {c::XboxStartupSettingsEnvironmentDll, Target::SettingsEnvironment, true}};
        void Check(bool success, const char *operation)
        {
            if (!success) throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
        }
        class Privilege
        {
            HANDLE token = nullptr;
            TOKEN_PRIVILEGES previous = {};
            bool adjusted = false;
        public:
            explicit Privilege(const wchar_t *name)
            {
                Check(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) != FALSE, "OpenProcessToken");
                TOKEN_PRIVILEGES requested = {};
                requested.PrivilegeCount = 1;
                if (!LookupPrivilegeValueW(nullptr, name, &requested.Privileges[0].Luid))
                {
                    CloseHandle(token);
                    token = nullptr;
                    Check(false, "LookupPrivilegeValue");
                }
                requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                DWORD size = sizeof(previous);
                SetLastError(ERROR_SUCCESS);
                const BOOL result = AdjustTokenPrivileges(token, FALSE, &requested, sizeof(previous), &previous, &size);
                const DWORD error = GetLastError();
                if (!result || error != ERROR_SUCCESS)
                {
                    CloseHandle(token);
                    token = nullptr;
                    SetLastError(error);
                    Check(false, "Enable file-maintenance privilege");
                }
                adjusted = true;
            }
            ~Privilege()
            {
                if (adjusted) AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
                if (token) CloseHandle(token);
            }
            Privilege(const Privilege&) = delete;
            Privilege& operator=(const Privilege&) = delete;
        };
        // RtlGetVersion bypasses the GetVersionEx app-compat shim, which lies about the build on manifested apps.
        // Diagnostic only: included in status/error text so an unlisted build is obvious instead of a bare "unsupported layout".
        std::wstring WindowsBuildString()
        {
            using RtlGetVersionFn = LONG (NTAPI *)(OSVERSIONINFOEXW *);
            const auto ntdll = GetModuleHandleW(c::XboxStartupNtDll);
            const auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, c::XboxStartupVersionExport)) : nullptr;
            OSVERSIONINFOEXW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (!rtlGetVersion || rtlGetVersion(&info) != 0) return L"unknown";
            const DWORD updateBuildRevision = Registry::ReadDWORD(c::WindowsVersionRegKey, c::WindowsRevisionRegValue);
            wchar_t text[64] = {};
            swprintf_s(text, L"%u.%u.%u.%u", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber, updateBuildRevision);
            return text;
        }
        fs::path SystemDirectory()
        {
            wchar_t path[MAX_PATH] = {};
            const auto length = GetSystemDirectoryW(path, MAX_PATH);
            Check(length && length < MAX_PATH, "GetSystemDirectory");
            return path;
        }
        fs::path BackupDirectory()
        {
            PWSTR path = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path)))
                throw std::runtime_error("ProgramData is unavailable");
            const fs::path result = fs::path(path) / c::XboxStartupBackupDirectory;
            CoTaskMemFree(path);
            return result;
        }
        Bytes Read(const fs::path& path)
        {
            const auto size = fs::file_size(path);
            if (size > 128 * 1024 * 1024) throw std::runtime_error("Unexpectedly large system image");
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("Cannot open " + path.u8string());
            Bytes result(static_cast<std::size_t>(size));
            if (!stream.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(result.size())))
                throw std::runtime_error("Cannot read " + path.u8string());
            return result;
        }
        void Write(const fs::path& path, const Bytes& bytes)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(file != INVALID_HANDLE_VALUE, "Create new file");
            DWORD written = 0;
            const BOOL okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            const BOOL flushed = okay ? FlushFileBuffers(file) : FALSE;
            const DWORD error = GetLastError();
            CloseHandle(file);
            SetLastError(error);
            if (!okay || !flushed || written != bytes.size())
            {
                // CREATE_NEW guarantees this is our incomplete file, not a pre-existing backup.
                DeleteFileW(path.c_str());
                SetLastError(error);
                Check(false, "Write and flush new file");
            }
        }
        std::wstring Hash(const Bytes& bytes)
        {
            std::array<UCHAR, 32> digest{};
            if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
                throw std::runtime_error("SHA256 failed");
            const wchar_t *hex = L"0123456789abcdef";
            std::wstring result;
            for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
            return result;
        }
        fs::path Backup(const fs::path& directory, const Spec& spec, const Bytes& patched)
        {
            return directory / (std::wstring(spec.name) + L"." + Hash(patched) + c::XboxStartupBackupSuffix);
        }
        void PrepareBackups(const fs::path& directory)
        {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(c::XboxStartupBackupSecurity, SDDL_REVISION_1,
                &descriptor, nullptr) != FALSE, "Build backup directory permissions");
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
            const BOOL created = CreateDirectoryW(directory.c_str(), &attributes);
            const DWORD error = GetLastError();
            if (!created && error != ERROR_ALREADY_EXISTS)
            {
                LocalFree(descriptor);
                SetLastError(error);
                Check(false, "Create backup directory");
            }
            const auto flags = GetFileAttributesW(directory.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                LocalFree(descriptor);
                throw std::runtime_error("Backup directory is not a normal directory");
            }
            const BOOL secured = SetFileSecurityW(directory.c_str(), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
                | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
            const DWORD securedError = GetLastError();
            LocalFree(descriptor);
            SetLastError(securedError);
            Check(secured != FALSE, "Protect backup directory");
        }
        class FileSecurity
        {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            PACL dacl = nullptr;
            SECURITY_INFORMATION information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
        public:
            explicit FileSecurity(const fs::path& path)
            {
                const DWORD error = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, information, nullptr, nullptr, &dacl, nullptr, &descriptor);
                if (error != ERROR_SUCCESS) { SetLastError(error); Check(false, "Read original file permissions"); }
                SECURITY_DESCRIPTOR_CONTROL control = 0;
                DWORD revision = 0;
                Check(GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE, "Read ACL protection");
                information |= (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
            }
            ~FileSecurity() { if (descriptor) LocalFree(descriptor); }
            std::wstring Describe() const
            {
                LPWSTR text = nullptr;
                Check(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1,
                    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                    &text, nullptr) != FALSE, "Serialize original permissions");
                const std::wstring result(text);
                LocalFree(text);
                return result;
            }
            void Restore(const fs::path& path) const
            {
                Check(SetFileSecurityW(path.c_str(), information, descriptor) != FALSE, "Restore original owner and ACL");
            }
            void AllowReplacement(const fs::path& path) const
            {
                alignas(SID) BYTE adminSid[SECURITY_MAX_SID_SIZE] = {};
                DWORD sidSize = sizeof(adminSid);
                Check(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &sidSize) != FALSE, "Create administrators SID");
                DWORD error = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                    adminSid, nullptr, nullptr, nullptr);
                if (error != ERROR_SUCCESS) { SetLastError(error); Check(false, "Take temporary ownership"); }
                EXPLICIT_ACCESSW grant = {};
                grant.grfAccessPermissions = FILE_ALL_ACCESS;
                grant.grfAccessMode = GRANT_ACCESS;
                grant.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                grant.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
                grant.Trustee.ptstrName = reinterpret_cast<LPWSTR>(adminSid);
                PACL writable = nullptr;
                error = SetEntriesInAclW(1, &grant, dacl, &writable);
                if (error == ERROR_SUCCESS)
                    error = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, writable, nullptr);
                if (writable) LocalFree(writable);
                if (error != ERROR_SUCCESS) { SetLastError(error); Check(false, "Grant temporary replacement access"); }
            }
            FileSecurity(const FileSecurity&) = delete;
            FileSecurity& operator=(const FileSecurity&) = delete;
        };
        std::wstring UniqueSuffix()
        {
            GUID id{};
            if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot allocate replacement identifier");
            wchar_t text[40]{};
            StringFromGUID2(id, text, 40);
            return text;
        }
        void Record(const fs::path& journal, const std::wstring& message)
        {
            SYSTEMTIME time{};
            GetSystemTime(&time);
            wchar_t stamp[40]{};
            swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
            const auto text = Unicode::to_string(std::wstring(stamp) + message + L"\r\n");
            HANDLE file = CreateFileW(journal.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(file != INVALID_HANDLE_VALUE, "Open recovery journal");
            DWORD written = 0;
            const bool okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE
                && written == text.size() && FlushFileBuffers(file) != FALSE;
            const DWORD error = GetLastError();
            CloseHandle(file);
            SetLastError(error);
            Check(okay, "Persist recovery journal");
        }
        void RecoveryRecord(const fs::path& journal, const std::wstring& message) noexcept
        {
            try { Record(journal, message); }
            catch (...) { log.Error("Could not persist recovery journal entry: %ls", message.c_str()); }
        }
        void Replace(const fs::path& path, const Bytes& expected, const Bytes& replacement, const fs::path& journal, bool recovery = false)
        {
            if (Read(path) != expected) throw std::runtime_error("System image changed during operation; retry inspection");
            FileSecurity security(path);
            const auto suffix = UniqueSuffix();
            const fs::path staged = path.wstring() + c::XboxStartupNewSuffix + suffix;
            const fs::path previous = path.wstring() + c::XboxStartupOldSuffix + suffix;
            const auto pending = L"REPLACE_PENDING target=" + path.wstring() + L" staged=" + staged.wstring()
                + L" previous=" + previous.wstring() + L" before=" + Hash(expected) + L" after=" + Hash(replacement);
            if (recovery) RecoveryRecord(journal, pending);
            else Record(journal, pending);
            bool renamed = false, installed = false;
            try
            {
                Write(staged, replacement);
                security.AllowReplacement(path);
                Check(MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Rename loaded system image");
                renamed = true;
                Check(MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Install replacement image");
                installed = true;
                if (Read(path) != replacement) throw std::runtime_error("Installed image verification failed");
                security.Restore(path);
                security.Restore(previous);
            }
            catch (...)
            {
                const auto failure = std::current_exception();
                try
                {
                    if (renamed)
                    {
                        security.AllowReplacement(previous);
                        if (installed)
                        {
                            security.AllowReplacement(path);
                            Check(DeleteFileW(path.c_str()) != FALSE, "Remove failed replacement");
                        }
                        Check(MoveFileExW(previous.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Roll back replacement");
                    }
                    security.Restore(path);
                    RecoveryRecord(journal, L"FILE_ROLLBACK_OK target=" + path.wstring());
                }
                catch (const std::exception& error)
                {
                    RecoveryRecord(journal, L"FILE_ROLLBACK_FAILED target=" + path.wstring() + L" previous=" + previous.wstring()
                        + L" error=" + Unicode::to_wstring(error.what()));
                    throw std::runtime_error("Replacement and rollback failed; original retained at " + previous.u8string() + ": " + error.what());
                }
                std::error_code ignored;
                fs::remove(staged, ignored);
                std::rethrow_exception(failure);
            }
            RecoveryRecord(journal, L"REPLACE_OK target=" + path.wstring());
            RecoveryRecord(journal, L"REBOOT_DELETE_PENDING path=" + previous.wstring());
            if (!MoveFileExW(previous.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
            {
                RecoveryRecord(journal, L"REBOOT_DELETE_NOT_SCHEDULED path=" + previous.wstring());
                log.Warn("Old loaded image retained until manual cleanup: %ls", previous.c_str());
            }
        }
        struct Change { fs::path path; Bytes before, after, original; };
        bool All(const std::vector<Site>& sites, State state)
        {
            return std::all_of(sites.begin(), sites.end(), [state](const Site& site) { return site.state == state; });
        }
        bool Any(const std::vector<Site>& sites, bool verified)
        {
            return std::any_of(sites.begin(), sites.end(), [verified](const Site& site) { return site.verified == verified; });
        }
        // What AnyFSE knows about one installed target. Patched state comes from a validated backup that reproduces the installed
        // image, not from recognizing patched bytes, because a discovered site's compare is gone once it has been replaced.
        struct Analysis
        {
            std::vector<Site> plan; // Resolved on the pristine image.
            Bytes original;         // Pristine image: the installed file itself, or the backup it was patched from.
            bool patched = false;   // Installed image is AnyFSE's patch of `original`.
            bool unverifiedApplied = false;
            bool foreign = false;   // Every verified site is patched but no AnyFSE backup reproduces the image.
        };
        Analysis Analyze(const Spec& spec, const Bytes& image, const fs::path& backups)
        {
            Analysis result;
            const auto backup = Backup(backups, spec, image);
            if (fs::exists(backup))
            {
                auto original = Read(backup);
                auto plan = BuildPlan(spec.target, original);
                if (All(plan, State::Original))
                {
                    const bool all = PatchImage(spec.target, original, true) == image;
                    if (all || PatchImage(spec.target, original) == image)
                    {
                        result.unverifiedApplied = all && Any(plan, false);
                        result.plan = std::move(plan);
                        result.original = std::move(original);
                        result.patched = true;
                        return result;
                    }
                }
            }
            result.plan = BuildPlan(spec.target, image);
            if (All(result.plan, State::Original)) result.original = image;
            else if (Any(result.plan, true) && All(result.plan, State::Patched)) result.foreign = true;
            else throw std::runtime_error("Partially patched image with no matching AnyFSE backup: restore it with sfc /scannow first");
            return result;
        }
        void Execute(bool restore, bool includeUnverified = false)
        {
            // Enforce at the mutation boundary, including elevated and CLI invocations.
            // Restoration remains available for previously modified handhelds.
            if (!restore && IsHandheldDevice())
                throw std::runtime_error("Desktop Xbox startup patches are disabled on handheld devices");
            Privilege takeOwnership(SE_TAKE_OWNERSHIP_NAME);
            Privilege restoreOwnership(SE_RESTORE_NAME);
            const auto directory = SystemDirectory(), backups = BackupDirectory();
            std::vector<Change> changes;
            std::vector<std::pair<fs::path, Bytes>> snapshots;
            // Resolve and validate every target before changing any system file.
            for (const auto& spec : specs)
            {
                try
                {
                    const auto path = directory / spec.name;
                    if (spec.optional && !fs::exists(path)) continue;
                    const auto before = Read(path);
                    const auto analysis = Analyze(spec, before, backups);
                    snapshots.emplace_back(path, before);
                    if (restore)
                    {
                        if (analysis.foreign) throw std::runtime_error("Backup does not match the currently installed Windows image");
                        if (analysis.patched) changes.push_back({path, before, analysis.original, analysis.original});
                    }
                    else
                    {
                        // A plain apply never adds or removes unverified sites the user confirmed earlier.
                        if (analysis.foreign || (analysis.patched && (!includeUnverified || analysis.unverifiedApplied))) continue;
                        const auto after = PatchImage(spec.target, analysis.original, includeUnverified);
                        if (after != before) changes.push_back({path, before, after, analysis.original});
                    }
                }
                catch (const std::exception& error)
                {
                    throw std::runtime_error(Unicode::to_string(spec.name) + " (Windows " + Unicode::to_string(WindowsBuildString())
                        + "): " + error.what());
                }
            }
            PrepareBackups(backups);
            const auto journal = backups / (L"operation-" + UniqueSuffix() + L".log");
            Record(journal, restore ? L"BEGIN restore" : L"BEGIN apply");
            log.Info("Xbox startup recovery journal: %ls", journal.c_str());
            // Snapshot all validated targets before replacing any, including unchanged files.
            // Snapshots are not used as original restore backups.
            for (const auto& snapshot : snapshots)
            {
                const auto hash = Hash(snapshot.second);
                const auto saved = backups / (snapshot.first.filename().wstring() + L"." + hash + L".snapshot");
                if (!fs::exists(saved)) Write(saved, snapshot.second);
                if (Read(saved) != snapshot.second) throw std::runtime_error("Snapshot verification failed");
                FileSecurity security(snapshot.first);
                Record(journal, L"SECURITY target=" + snapshot.first.wstring() + L" sddl=" + security.Describe());
                Record(journal, L"SNAPSHOT target=" + snapshot.first.wstring() + L" sha256=" + hash
                    + L" backup=" + saved.wstring());
            }
            if (!restore && !changes.empty())
            {
                for (const auto& change : changes)
                {
                    const fs::path backup = backups / (change.path.filename().wstring() + L"." + Hash(change.after) + c::XboxStartupBackupSuffix);
                    if (fs::exists(backup))
                    {
                        if (Read(backup) != change.original) throw std::runtime_error("Existing original backup does not match");
                    }
                    else Write(backup, change.original);
                    if (Read(backup) != change.original) throw std::runtime_error("Original backup verification failed");
                    Record(journal, L"ORIGINAL_BACKUP target=" + change.path.wstring() + L" backup=" + backup.wstring()
                        + L" original=" + Hash(change.original) + L" patched=" + Hash(change.after));
                }
            }
            std::size_t completed = 0;
            try
            {
                for (const auto& change : changes)
                {
                    Replace(change.path, change.before, change.after, journal);
                    ++completed;
                    log.Info("%s desktop Xbox startup patch: %ls", restore ? "Restored" : "Applied", change.path.c_str());
                }
            }
            catch (...)
            {
                const auto failure = std::current_exception();
                RecoveryRecord(journal, L"FAILED; rolling back completed replacements");
                while (completed)
                {
                    const auto& change = changes[--completed];
                    try { Replace(change.path, change.after, change.before, journal, true); RecoveryRecord(journal, L"ROLLBACK_OK target=" + change.path.wstring()); }
                    catch (const std::exception& error) { RecoveryRecord(journal, L"ROLLBACK_FAILED target=" + change.path.wstring() + L" error=" + Unicode::to_wstring(error.what())); log.Error("Rollback failed for %ls: %s", change.path.c_str(), error.what()); }
                }
                std::rethrow_exception(failure);
            }
            RecoveryRecord(journal, L"COMPLETED");
        }
    }
    bool IsHandheldDevice()
    {
        // Block both real handhelds and registry-spoofed handhelds. A backup marker
        // is not sufficient evidence that a device is physically a desktop.
        if (Registry::ReadDWORD(c::DeviceFormRegKey, c::DeviceFormRegValue) == c::HandheldDeviceForm)
            return true;
        using QueryDeviceForm = VOID (NTAPI *)(ULONGLONG *, DWORD *, DWORD *);
        const auto ntdll = GetModuleHandleW(c::XboxStartupNtDll);
        const auto query = ntdll ? reinterpret_cast<QueryDeviceForm>(GetProcAddress(ntdll, c::XboxStartupDeviceFormExport)) : nullptr;
        if (!query) return true; // Fail closed when Windows device classification is unavailable.
        DWORD form = 0;
        query(nullptr, nullptr, &form);
        return form == c::HandheldDeviceForm;
    }
    Status Inspect()
    {
        Status status;
        status.details = L"Windows " + WindowsBuildString() + L"\n";
        try
        {
            const auto directory = SystemDirectory(), backups = BackupDirectory();
            for (const auto& spec : specs)
            {
                try
                {
                    if (spec.optional && !fs::exists(directory / spec.name)) continue;
                    const auto analysis = Analyze(spec, Read(directory / spec.name), backups);
                    // A target with no verified site (an optional DLL without the check) needs nothing for the validated patch.
                    const bool enabled = analysis.patched || analysis.foreign || !Any(analysis.plan, true);
                    status.allPatched &= enabled;
                    status.anyPatched |= analysis.patched || analysis.foreign;
                    if (analysis.foreign) status.canRestore = false;
                    status.details += std::wstring(spec.name) + (analysis.foreign ? L": enabled outside AnyFSE (no backup to restore)\n"
                        : analysis.patched ? L": enabled\n" : enabled ? L": nothing to patch\n" : L": original\n");
                    if (analysis.foreign || analysis.unverifiedApplied) continue;
                    for (const auto& site : analysis.plan)
                    {
                        if (site.verified) continue;
                        status.unverifiedPending = true;
                        status.unverifiedDetails += std::wstring(spec.name) + L" " + Unicode::to_wstring(site.description) + L"\n";
                    }
                }
                catch (const std::exception& error)
                {
                    status.allPatched = status.canApply = status.canRestore = false;
                    status.details += std::wstring(spec.name) + L": unsupported (" + Unicode::to_wstring(error.what()) + L")\n";
                }
            }
        }
        catch (const std::exception& error)
        {
            status.allPatched = status.canApply = status.canRestore = false;
            status.details += Unicode::to_wstring(error.what());
        }
        if (IsHandheldDevice())
        {
            status.canApply = false;
            status.unverifiedPending = false;
            status.details += L"Desktop patch application disabled: handheld device or unavailable device classification.\n";
        }
        if (!status.canApply) status.unverifiedPending = false;
        status.canRestore &= status.anyPatched;
        return status;
    }
    void Apply() { Execute(false); }
    void ApplyUnverified() { Execute(false, true); }
    void Restore() { Execute(true); }
    std::wstring Scan()
    {
        // Read-only: lists device-form checks in every System32 DLL so a new layout or a newly gated DLL can be investigated.
        // Unlisted DLLs are never patched; these checks also gate unrelated features such as search and input.
        std::wstring report = L"Windows " + WindowsBuildString() + L"\n";
        const std::string marker = c::XboxStartupDeviceFormExport;
        std::size_t scanned = 0;
        for (const auto& entry : fs::directory_iterator(SystemDirectory()))
        {
            if (!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), c::XboxStartupDllExtension)) continue;
            try
            {
                const auto image = Read(entry.path());
                if (std::search(image.begin(), image.end(), marker.begin(), marker.end()) == image.end()) continue;
                ++scanned;
                const auto findings = DiscoverHandheldChecks(image);
                if (findings.empty()) continue;
                const bool target = std::any_of(std::begin(specs), std::end(specs),
                    [&](const Spec& spec) { return !_wcsicmp(spec.name, entry.path().filename().c_str()); });
                report += entry.path().filename().wstring() + (target ? L" (patch target)\n" : L" (not a patch target)\n");
                for (const auto& finding : findings)
                    report += L"  " + std::wstring(finding.known ? L"[known] " : finding.patchable ? L"" : L"[report only] ")
                        + Unicode::to_wstring(finding.description) + L"\n";
            }
            catch (const std::exception& error)
            {
                log.Warn("Device-form scan skipped %ls: %s", entry.path().c_str(), error.what());
            }
        }
        report += std::to_wstring(scanned) + L" DLLs import " + Unicode::to_wstring(marker) + L"\n";
        return report;
    }
}
