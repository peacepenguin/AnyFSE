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
#include "XboxStartup.hpp"
#include "XboxStartupPlan.hpp"
#include "Tools/Unicode.hpp"
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
        struct Spec { const wchar_t *name; Target target; };
        const Spec specs[] = {{c::XboxStartupGameModeDll, Target::GameMode},
            {c::XboxStartupSettingsDll, Target::Settings}, {c::XboxStartupShellDll, Target::Shell}};
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
        void Replace(const fs::path& path, const Bytes& expected, const Bytes& replacement)
        {
            if (Read(path) != expected) throw std::runtime_error("System image changed during operation; retry inspection");
            FileSecurity security(path);
            const auto suffix = UniqueSuffix();
            const fs::path staged = path.wstring() + c::XboxStartupNewSuffix + suffix;
            const fs::path previous = path.wstring() + c::XboxStartupOldSuffix + suffix;
            bool renamed = false, installed = false;
            try
            {
                Write(staged, replacement);
                security.AllowReplacement(path);
                Check(MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Rename loaded system image");
                renamed = true;
                Check(MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Install replacement image");
                installed = true;
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
                }
                catch (const std::exception& error)
                {
                    throw std::runtime_error("Replacement and rollback failed; original retained at " + previous.u8string() + ": " + error.what());
                }
                std::error_code ignored;
                fs::remove(staged, ignored);
                std::rethrow_exception(failure);
            }
            if (!MoveFileExW(previous.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
                log.Warn("Old loaded image retained until manual cleanup: %ls", previous.c_str());
        }
        struct Change { fs::path path; Bytes before, after; };
        bool All(const std::vector<Site>& sites, State state)
        {
            return std::all_of(sites.begin(), sites.end(), [state](const Site& site) { return site.state == state; });
        }
        void Execute(bool restore)
        {
            Privilege takeOwnership(SE_TAKE_OWNERSHIP_NAME);
            Privilege restoreOwnership(SE_RESTORE_NAME);
            const auto directory = SystemDirectory(), backups = BackupDirectory();
            std::vector<Change> changes;
            // Resolve and validate every target before changing any system file.
            for (const auto& spec : specs)
            {
                try
                {
                    const auto path = directory / spec.name;
                    const auto before = Read(path);
                    const auto plan = BuildPlan(spec.target, before);
                    if (restore)
                    {
                        if (All(plan, State::Original)) continue;
                        const auto backup = Backup(backups, spec, before);
                        const auto original = Read(backup);
                        if (!All(BuildPlan(spec.target, original), State::Original) || PatchImage(spec.target, original) != before)
                            throw std::runtime_error("Backup does not match the currently installed Windows image");
                        changes.push_back({path, before, original});
                    }
                    else
                    {
                        if (All(plan, State::Patched)) continue;
                        if (!All(plan, State::Original)) throw std::runtime_error("Partially patched image: restore it with its original patcher first");
                        changes.push_back({path, before, PatchImage(spec.target, before)});
                    }
                }
                catch (const std::exception& error)
                {
                    throw std::runtime_error(Unicode::to_string(spec.name) + ": " + error.what());
                }
            }
            if (!restore && !changes.empty())
            {
                PrepareBackups(backups);
                for (const auto& change : changes)
                {
                    const fs::path backup = backups / (change.path.filename().wstring() + L"." + Hash(change.after) + c::XboxStartupBackupSuffix);
                    if (fs::exists(backup))
                    {
                        if (Read(backup) != change.before) throw std::runtime_error("Existing original backup does not match");
                    }
                    else Write(backup, change.before);
                }
            }
            std::size_t completed = 0;
            try
            {
                for (const auto& change : changes)
                {
                    Replace(change.path, change.before, change.after);
                    ++completed;
                    log.Info("%s desktop Xbox startup patch: %ls", restore ? "Restored" : "Applied", change.path.c_str());
                }
            }
            catch (...)
            {
                const auto failure = std::current_exception();
                while (completed)
                {
                    const auto& change = changes[--completed];
                    try { Replace(change.path, change.after, change.before); }
                    catch (const std::exception& error) { log.Error("Rollback failed for %ls: %s", change.path.c_str(), error.what()); }
                }
                std::rethrow_exception(failure);
            }
        }
    }
    Status Inspect()
    {
        Status status;
        try
        {
            const auto directory = SystemDirectory(), backups = BackupDirectory();
            for (const auto& spec : specs)
            {
                try
                {
                    const auto image = Read(directory / spec.name);
                    const auto plan = BuildPlan(spec.target, image);
                    const bool patched = All(plan, State::Patched), original = All(plan, State::Original);
                    status.allPatched &= patched;
                    status.anyPatched |= !original;
                    status.canApply &= patched || original;
                    status.details += std::wstring(spec.name) + (patched ? L": enabled\n" : original ? L": original\n" : L": partial patch\n");
                    if (!original)
                    {
                        try
                        {
                            const auto backup = Read(Backup(backups, spec, image));
                            status.canRestore &= All(BuildPlan(spec.target, backup), State::Original) && PatchImage(spec.target, backup) == image;
                        }
                        catch (...) { status.canRestore = false; }
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
            status.details = Unicode::to_wstring(error.what());
        }
        status.canRestore &= status.anyPatched;
        return status;
    }
    void Apply() { Execute(false); }
    void Restore() { Execute(true); }
}
