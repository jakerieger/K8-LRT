/*
    K8Tool - v3.0.0 - Library removal tool for Bobdule's Kontakt 8

    LICENSE

        ISC License

        Copyright 2026 Jake Rieger

        Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
        granted, provided that the above copyright notice and this permission notice appear in all copies.

        THE SOFTWARE IS PROVIDED “AS IS” AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING
        ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL,
        DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
        PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
        WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

    REMOVAL PROCESS

        This process of steps is executed by the program to remove libraries. In theory, you could do all
        of this manually. K8Tool just makes it a lot easier.

        - Locate library entries in the registry. These are located under two locations:
            - HKEY_LOCAL_MACHINE\SOFTWARE\Native Instruments              (Primary)
            - HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Native Instruments  (Secondary, rare)
        - Library entries have a `ContentDir` value that stores the location of the actual library on
          disk. We store this and the library name retrieved from the registry key to a list.
        - When a library is selected for removal, we take the following actions:
            1.  Find the corresponding <LibraryName>.xml file located in:
                  C:\Program Files\Common Files\Native Instruments\Service Center
            2.  If it doesn't exist, check the `NativeAccess.xml` file in the same path for an entry.
            3.  Save the `SNPID` value from the XML file and delete it (DO NOT REMOVE NativeAccess.xml)
            4.  Find the corresponding .cache file located in:
                  ~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache
                The filename has the format "K{SNPID}...".cache
            5.  Delete the .cache file.
            6.  Delete and create a backup of ~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3.
                Kontakt will rebuild this next time it's launched.
            7.  Look for the associated `.jwt` file located in:
                  C:\Users\Public\Documents\Native Instruments\Native Access\ras3
            8.  Delete the .jwt file.
            9.  Delete the library content directory (if the user selected to do so).
            10. Delete the registry key (and create a backup if requested).
        - Relocating a library simply involves moving the content directory to the new location
          and updating the `ContentDir` registry value

    REVISION HISTORY

        3.0.0  (2026-02-01)  migrated to C++ codebase, multi-threading, progress indicator,
                             refactored file operations
        2.0.0  (2026-01-31)  bug fixes for registry querying, relocating libraries,
                             removed support for Windows 7, string pool
                             memory management, wide path support
        1.1.0  (2026-01-26)  additional directory checks and removals, UI additions and changes
        1.0.0  (2026-01-25)  UI redux, added functionality, updates
        0.3.1  (2026-01-23)  memory model improvements
        0.3.0  (2026-01-23)  sweeping code changes, bug fixes, and logging
        0.2.0  (2026-01-23)  tons of bug fixes and code improvements
        0.1.0  (2026-01-22)  initial release of K8Tool (formerly K8Tool)
*/

// ReSharper disable All
#include "version.h"
#include "resource.h"

#pragma warning(disable : 4312)

#define _CRT_SECURE_NO_WARNINGS 1
#define _NO_IO 1

#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
    #define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// STL
#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <format>
#include <ranges>
#include <unordered_map>
#include <sstream>
#include <memory>
#include <thread>
#include <filesystem>
#include <stop_token>
#include <optional>

// Windows API
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#include <winhttp.h>
#include <winreg.h>

// Conflicts with zip_file.hpp (I don't need these anyway)
#ifdef min
    #undef min
#endif

#ifdef max
    #undef max
#endif

// Vendor
#include <tinyxml2.h>
#include <zip_file.hpp>

#define _Unused(x) (void)(x)
#define _Not_Implemented() _Fatal("Function '%s' is not implemeted (%s:%d)", __FUNCTION__, __FILE__, __LINE__ - 1)

namespace K8 {
    namespace fs = std::filesystem;

#pragma region Globals

    namespace Globals {
        static constexpr auto kServiceCenter = R"(C:\Program Files\Common Files\Native Instruments\Service Center)";
        static constexpr auto kNativeAccessXML =
          R"(C:\Program Files\Common Files\Native Instruments\Service Center\NativeAccess.xml)";
        static constexpr auto kLibrariesCache = R"(Native Instruments\Kontakt 8\LibrariesCache)";
        static constexpr auto kRAS3           = R"(C:\Users\Public\Documents\Native Instruments\Native Access\ras3)";
        static constexpr auto kKompleteDB3    = R"(Native Instruments\Kontakt 8\komplete.db3)";
    }  // namespace Globals

#pragma endregion

#pragma region CustomEvents

#define WM_UPDATE_CHECK_COMPLETED (WM_USER + 1)
#define WM_REMOVE_SELECTED_COMPLETED (WM_USER + 2)
#define WM_COLLECT_BACKUPS_COMPLETED (WM_USER + 3)
#define WM_UPDATE_PROGRESS_TEXT (WM_USER + 4)
#define WM_RELOCATE_SELECTED_COMPLETED (WM_USER + 5)
#define WM_REMOVE_COMPLETED (WM_USER + 6)

#pragma endregion

#pragma region Logging
    static constexpr auto kLogFilename = "K8.log";

    enum class LogLevel {
        Info,
        Warn,
        Error,
        Fatal,
        Debug,
    };

    class Logger {
        FILE* _file {nullptr};
        bool _consoleAttached {false};

    public:
        Logger() {
            const errno_t result = fopen_s(&_file, kLogFilename, "a+");
            if (result == 0) {
                Log(LogLevel::Info, "--- K8Tool Started ---");
            } else {
                MessageBox(nullptr, "Failed to initialize logger.", "K8Tool", MB_OK | MB_ICONERROR);
                std::quick_exit(-1);
            }
        }

        ~Logger() {
            if (_file) {
                Log(LogLevel::Info, "--- K8Tool Stopped ---");
                fclose(_file);
            }
        }

        void SetConsoleAttached(bool attached) {
            _consoleAttached = attached;
        }

        void Log(LogLevel level, const char* fmt, ...) const {
            if (!_file)
                return;

            SYSTEMTIME st;
            GetLocalTime(&st);

            const char* levelStr;
            switch (level) {
                case LogLevel::Info:
                    levelStr = "INFO";
                    break;
                case LogLevel::Warn:
                    levelStr = "WARN";
                    break;
                case LogLevel::Error:
                    levelStr = "ERROR";
                    break;
                case LogLevel::Fatal:
                    levelStr = "FATAL";
                    break;
                case LogLevel::Debug:
                default:
                    levelStr = "DEBUG";
                    break;
            }

            va_list args;
            va_start(args, fmt);
            char body[1024] = {0};
            vsnprintf(body, sizeof(body), fmt, args);
            va_end(args);

            char msg[2048] = {0};
            snprintf(msg,
                     2048,
                     "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %s\n",
                     st.wYear,
                     st.wMonth,
                     st.wDay,
                     st.wHour,
                     st.wMinute,
                     st.wSecond,
                     st.wMilliseconds,
                     levelStr,
                     body);
            fprintf(_file, "%s", msg);
            fflush(_file);

            if (level == LogLevel::Fatal) {
                char msgbox_msg[2048] = {0};
                snprintf(msgbox_msg, 2048, "A fatal error has occurred and K8Tool must shutdown:\n\n%s", body);
                MessageBoxA(nullptr, msgbox_msg, "Fatal Error", MB_OK | MB_ICONERROR);
            }

#ifndef NDEBUG
            if (_consoleAttached) {
                printf("%s", msg);
            }
#endif
        }

        bool Valid() const {
            return (_file != nullptr);
        }

        std::string GetLogContents() const {
            if (!Valid())
                return "";

            const auto current = ftell(_file);

            fseek(_file, 0, SEEK_END);
            const auto size = ftell(_file);
            fseek(_file, 0, SEEK_SET);

            if (size <= 0)
                return "";

            std::string contents;
            contents.resize(size);
            fread(contents.data(), 1, size, _file);

            fseek(_file, current, SEEK_SET);

            return contents;
        }
    };

    inline Logger* logger;

#define _Info(fmt, ...) logger->Log(LogLevel::Info, fmt, ##__VA_ARGS__)
#define _Warn(fmt, ...) logger->Log(LogLevel::Warn, fmt, ##__VA_ARGS__)
#define _Error(fmt, ...) logger->Log(LogLevel::Error, fmt, ##__VA_ARGS__)
#define _Debug(fmt, ...) logger->Log(LogLevel::Debug, fmt, ##__VA_ARGS__)
#define _Fatal(fmt, ...)                                                                                               \
    do {                                                                                                               \
        logger->Log(LogLevel::Fatal, fmt, ##__VA_ARGS__);                                                              \
        std::quick_exit(-1);                                                                                           \
    } while (0);

#pragma endregion

#pragma region StringPool

    class StringPool {
        std::vector<std::unique_ptr<char[]>> _pool;
        std::unordered_map<std::string_view, const char*> _strings;

    public:
        StringPool() = default;

        const char* Intern(const std::string& str) {
            if (const auto it = _strings.find(str); it != _strings.end()) {
                return it->second;
            }

            const auto len = str.length() + 1;
            auto copy      = std::make_unique<char[]>(len);
            std::memcpy(copy.get(), str.c_str(), len);

            const char* ptr = copy.get();
            _pool.push_back(std::move(copy));
            _strings[std::string_view(ptr, len - 1)] = ptr;

            return ptr;
        }
    };

#pragma endregion

#pragma region Data

    struct LibraryInfo {
        const char* name;        // str pool
        const char* contentDir;  // str pool
        uintmax_t sizeOnDisk;
        HKEY registryRoot;   // HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER
        const char* subKey;  // str pool
    };

    using LibraryList = std::vector<LibraryInfo>;

#pragma endregion

#pragma region Utility

    namespace Util {
        static bool PathExists(const fs::path& path) {
            std::error_code ec;
            return fs::exists(path, ec);
        }

        static bool DeletePath(const fs::path& path) {
            std::error_code ec;
            return fs::remove_all(path, ec);
            return !ec;
        }

        static std::string GetEnvVar(const char* name) {
            char* buffer      = nullptr;
            size_t size       = 0;
            const errno_t err = _dupenv_s(&buffer, &size, name);
            if (err != 0 || !buffer) {
                return {};
            }
            std::string result(buffer);
            free(buffer);
            return result;
        }

        static std::string GetLocalAppData() {
            return GetEnvVar("LOCALAPPDATA");
        }

        static std::string GetUserProfile() {
            return GetEnvVar("USERPROFILE");
        }

        static std::string GetCommonDocuments() {
            return GetEnvVar("PUBLIC");
        }

        static bool FileExists(const fs::path& path) {
            std::error_code ec;
            return fs::exists(path, ec) && !fs::is_directory(path, ec);
        }

        static std::wstring ToWideStr(const std::string& str) {
            if (str.empty())
                return {};
            const int size = ::MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
            std::wstring result(size, 0);
            MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
            return result;
        }

        static std::string ToStr(const std::wstring& wstr) {
            if (wstr.empty())
                return {};
            const int size =
              ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
            std::string result(size, 0);
            ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
            return result;
        }

        static std::string ToCRLF(const std::string& input) {
            std::string output;
            output.reserve(input.size() + (input.size() / 10));

            for (const auto c : input) {
                if (c == '\n')
                    output += '\r';
                output += c;
            }

            return output;
        }

        static uintmax_t GetDirectorySize(const fs::path& dir) {
            uintmax_t size = 0;
            std::error_code ec;

            for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
                if (ec) {
                    _Warn("Error iterating: %s", ec.message().c_str());
                    ec.clear();
                    continue;
                }

                if (entry.is_regular_file(ec) && !ec) {
                    size += entry.file_size(ec);
                }
            }

            return size;
        }

        static std::string FormatFileSize(uintmax_t bytes) {
            const char* suffixes[] = {"B", "KB", "MB", "GB", "TB", "PB"};
            int suffixIndex        = 0;
            double size            = static_cast<double>(bytes);

            while (size >= 1000.0 && suffixIndex < 5) {
                size /= 1024.0;
                suffixIndex++;
            }

            // Format with 1 decimal place
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.1f %s", size, suffixes[suffixIndex]);

            return std::string(buffer);
        }

        static bool ExportListViewToCSV(HWND hListView, const fs::path& csvPath) {
            std::ofstream file(csvPath);
            if (!file.is_open()) {
                _Error("Failed to create CSV file: %s", csvPath.string().c_str());
                return false;
            }

            HWND hHeader    = ListView_GetHeader(hListView);
            int columnCount = Header_GetItemCount(hHeader);
            char buffer[512];

            // Header
            for (int col = 0; col < columnCount; col++) {
                LVCOLUMN lvc   = {};
                lvc.mask       = LVCF_TEXT;
                lvc.pszText    = buffer;
                lvc.cchTextMax = sizeof(buffer);
                ListView_GetColumn(hListView, col, &lvc);
                file << "\"" << buffer << "\"";
                if (col < columnCount - 1)
                    file << ",";
            }
            file << "\n";

            // Data
            int itemCount = ListView_GetItemCount(hListView);
            for (int row = 0; row < itemCount; row++) {
                for (int col = 0; col < columnCount; col++) {
                    ListView_GetItemText(hListView, row, col, buffer, sizeof(buffer));
                    file << "\"" << buffer << "\"";
                    if (col < columnCount - 1)
                        file << ",";
                }
                file << "\n";
            }

            file.close();
            return true;
        }
    }  // namespace Util

#pragma endregion

#pragma region XML

    namespace XML {
        struct LibraryXMLInfo {
            std::string snpid;
            std::string name;
        };

        static std::optional<LibraryXMLInfo> GetSNPID(const fs::path& xmlPath, const std::string& libraryName) {
            namespace x = tinyxml2;

            x::XMLDocument doc;
            if (doc.LoadFile(xmlPath.string().c_str()) != x::XML_SUCCESS) {
                _Error("Failed to load XML file: %s", xmlPath.string().c_str());
                return std::nullopt;
            }

            x::XMLElement* root = doc.FirstChildElement("ProductHints");
            if (!root) {
                _Error("Root element 'ProductHints' not found in XML: %s", xmlPath.string().c_str());
                return std::nullopt;
            }

            for (x::XMLElement* product = root->FirstChildElement("Product"); product != nullptr;
                 product                = product->NextSiblingElement("Product")) {
                const char* name = product->FirstChildElement("Name")->GetText();
                if (name && std::string(name) == libraryName) {
                    const char* snpid = product->FirstChildElement("SNPID")->GetText();
                    if (!snpid) {
                        _Error("SNPID attribute not found in XML: %s", xmlPath.string().c_str());
                        return std::nullopt;
                    }
                    return LibraryXMLInfo {snpid, name};
                }
            }

            _Warn("Library '%s' not found in %s", libraryName.c_str(), xmlPath.string().c_str());
            return std::nullopt;
        }
    }  // namespace XML

#pragma endregion

#pragma region Registry

    namespace Registry {
        static constexpr std::array kKeyExclusionList = {
          "Massive",
          "Massive X",
          "Reaktor 6",
          "Battery 4",
          "FM8",
          "Absynth 5",
          "Absynth 6",
          "Guitar Rig 7 Pro",
          "Traktor Pro 4",
          "Maschine 3",
          "Komplete Kontrol",
          "Kontakt 5",
          "Kontakt 6",
          "Kontakt 7",
          "Kontakt 8",
          "Native Access",
          "Monark",
          "Super 8",
          "TRK-01",
          "Form",
          "Rounds",
          "Molekular",
          "Raum",
          "Replika XT",
          "Choral",
          "Flair",
          "Phasis",
          "Bite",
          "Dirt",
          "Freak",
          "Driver",
          "Solid EQ",
          "Solid Bus Comp",
          "Solid Dynamics",
          "VC 2A",
          "VC 76",
          "VC 160",
          "Vari Comp",
          "Enhanced EQ",
          "Passive EQ",
          "RC 24",
          "RC 48",
        };

        static bool EnableBackupPrivileges() {
            HANDLE hToken;
            TOKEN_PRIVILEGES tp;
            LUID luid;

            if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
                if (::LookupPrivilegeValue(nullptr, SE_BACKUP_NAME, &luid)) {
                    tp.PrivilegeCount           = 1;
                    tp.Privileges[0].Luid       = luid;
                    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    ::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
                }

                ::CloseHandle(hToken);
                _Info("Enabled registry backup privileges.");

                return true;
            }

            _Error("Failed to enable registry backup privileges. Make sure you're running K8Tool as Admin.");
            return false;
        }

        static void QueryLibraries(HKEY hKey, const std::string& subKey, StringPool& pool, LibraryList& libraries) {
            HKEY hSubKey;
            if (::RegOpenKeyExA(hKey, subKey.c_str(), 0, KEY_READ, &hSubKey) != ERROR_SUCCESS) {
                return;
            }

            DWORD index = 0;
            char name[256];
            DWORD nameSize = sizeof(name);

            while (::RegEnumKeyExA(hSubKey, index, name, &nameSize, nullptr, nullptr, nullptr, nullptr) ==
                   ERROR_SUCCESS) {
                const auto excluded =
                  std::ranges::find(kKeyExclusionList, std::string(name)) != kKeyExclusionList.end();
                if (!excluded) {
                    std::string libraryKeyPath = subKey + "\\" + name;
                    HKEY hLibraryKey;

                    if (::RegOpenKeyExA(hKey, libraryKeyPath.c_str(), 0, KEY_READ, &hLibraryKey) == ERROR_SUCCESS) {
                        char contentDir[512];
                        DWORD contentDirSize = sizeof(contentDir);
                        DWORD type;

                        if (::RegQueryValueExA(hLibraryKey,
                                               "ContentDir",
                                               nullptr,
                                               &type,
                                               (LPBYTE)contentDir,
                                               &contentDirSize) == ERROR_SUCCESS &&
                            type == REG_SZ) {
                            LibraryInfo info;
                            info.name         = pool.Intern(name);
                            info.contentDir   = pool.Intern(contentDir);
                            info.registryRoot = hKey;
                            info.subKey       = pool.Intern(libraryKeyPath);
                            info.sizeOnDisk   = Util::GetDirectorySize(contentDir);

                            libraries.push_back(info);
                        }

                        ::RegCloseKey(hLibraryKey);
                    }
                }

                nameSize = sizeof(name);
                ++index;
            }

            ::RegCloseKey(hSubKey);
        }

        static bool DeleteKey(HKEY hKey, const char* subKey) {
            const LONG result = ::RegDeleteTreeA(hKey, subKey);
            if (result != ERROR_SUCCESS) {
                _Error("Failed to delete registry key: %s (Error: %ld)", subKey, result);
                return false;
            }
            _Info("Deleted registry key: %s", subKey);
            return true;
        }

        static bool SetContentDir(HKEY hKey, const char* subKey, const char* newPath) {
            HKEY hSubKey;
            if (::RegOpenKeyExA(hKey, subKey, 0, KEY_SET_VALUE, &hSubKey) != ERROR_SUCCESS) {
                _Error("Failed to open registry key for writing: %s", subKey);
                return false;
            }

            const LONG result =
              ::RegSetValueExA(hSubKey, "ContentDir", 0, REG_SZ, (const BYTE*)newPath, (DWORD)strlen(newPath) + 1);
            ::RegCloseKey(hSubKey);

            if (result != ERROR_SUCCESS) {
                _Error("Failed to set ContentDir value for key: %s", subKey);
                return false;
            }

            _Info("Updated ContentDir for %s -> %s", subKey, newPath);
            return true;
        }

        static bool BackupKey(HKEY hKey, const char* subKey, const fs::path& backupPath) {
            HKEY hSubKey;
            if (::RegOpenKeyExA(hKey, subKey, 0, KEY_READ, &hSubKey) != ERROR_SUCCESS) {
                _Error("Failed to open registry key for backup: %s", subKey);
                return false;
            }

            std::error_code ec;
            fs::create_directories(backupPath.parent_path(), ec);

            const auto wBackupPath = Util::ToWideStr(backupPath.string());
            const LONG result      = ::RegSaveKeyW(hSubKey, wBackupPath.c_str(), nullptr);
            ::RegCloseKey(hSubKey);

            if (result != ERROR_SUCCESS) {
                _Error("Failed to backup registry key: %s to %s (Error: %ld)",
                       subKey,
                       backupPath.string().c_str(),
                       result);
                return false;
            }

            _Info("Backed up registry key: %s -> %s", subKey, backupPath.string().c_str());
            return true;
        }
    };  // namespace Registry

#pragma endregion

#pragma region FileOperations

    namespace FileOps {
        static bool PerformOperation(IFileOperation* pfo) {
            if (!pfo)
                return false;

            const HRESULT hr = pfo->PerformOperations();
            if (FAILED(hr)) {
                _Error("IFileOperation::PerformOperations failed (HRESULT: 0x%08X)", hr);
                return false;
            }

            BOOL anyAborted = FALSE;
            pfo->GetAnyOperationsAborted(&anyAborted);

            return !anyAborted;
        }

        static bool DeleteItem(const fs::path& path) {
            if (!Util::PathExists(path)) {
                _Warn("Path does not exist, skipping delete: %s", path.string().c_str());
                return true;
            }

            IFileOperation* pfo = nullptr;
            HRESULT hr          = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo));
            if (FAILED(hr)) {
                _Error("Failed to create IFileOperation instance (HRESULT: 0x%08X)", hr);
                return false;
            }

            hr = pfo->SetOperationFlags(FOF_NO_UI | FOF_NOCONFIRMATION);
            if (FAILED(hr)) {
                _Error("Failed to set operation flags (HRESULT: 0x%08X)", hr);
                pfo->Release();
                return false;
            }

            IShellItem* pItem = nullptr;
            const auto wPath  = Util::ToWideStr(path.string());
            hr                = ::SHCreateItemFromParsingName(wPath.c_str(), nullptr, IID_PPV_ARGS(&pItem));
            if (FAILED(hr)) {
                _Error("Failed to create shell item from path: %s (HRESULT: 0x%08X)", path.string().c_str(), hr);
                pfo->Release();
                return false;
            }

            hr = pfo->DeleteItem(pItem, nullptr);
            pItem->Release();

            if (FAILED(hr)) {
                _Error("Failed to queue delete operation for: %s (HRESULT: 0x%08X)", path.string().c_str(), hr);
                pfo->Release();
                return false;
            }

            const bool success = PerformOperation(pfo);
            pfo->Release();

            if (success) {
                _Info("Deleted: %s", path.string().c_str());
            } else {
                _Error("Failed to delete: %s", path.string().c_str());
            }

            return success;
        }

        static bool MoveItem(const fs::path& source, const fs::path& destination) {
            if (!Util::PathExists(source)) {
                _Error("Source path does not exist: %s", source.string().c_str());
                return false;
            }

            std::error_code ec;
            fs::create_directories(destination.parent_path(), ec);
            if (ec) {
                _Error("Failed to create destination directory: %s", destination.parent_path().string().c_str());
                return false;
            }

            IFileOperation* pfo = nullptr;
            HRESULT hr          = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo));
            if (FAILED(hr)) {
                _Error("Failed to create IFileOperation instance (HRESULT: 0x%08X)", hr);
                return false;
            }

            hr = pfo->SetOperationFlags(FOF_NO_UI | FOF_NOCONFIRMATION);
            if (FAILED(hr)) {
                _Error("Failed to set operation flags (HRESULT: 0x%08X)", hr);
                pfo->Release();
                return false;
            }

            IShellItem* pSource = nullptr;
            const auto wSource  = Util::ToWideStr(source.string());
            hr                  = ::SHCreateItemFromParsingName(wSource.c_str(), nullptr, IID_PPV_ARGS(&pSource));
            if (FAILED(hr)) {
                _Error("Failed to create shell item from source: %s (HRESULT: 0x%08X)", source.string().c_str(), hr);
                pfo->Release();
                return false;
            }

            IShellItem* pDest = nullptr;
            const auto wDest  = Util::ToWideStr(destination.parent_path().string());
            hr                = ::SHCreateItemFromParsingName(wDest.c_str(), nullptr, IID_PPV_ARGS(&pDest));
            if (FAILED(hr)) {
                _Error("Failed to create shell item from destination: %s (HRESULT: 0x%08X)",
                       destination.parent_path().string().c_str(),
                       hr);
                pSource->Release();
                pfo->Release();
                return false;
            }

            const auto destFilename = Util::ToWideStr(destination.filename().string());
            hr                      = pfo->MoveItem(pSource, pDest, destFilename.c_str(), nullptr);
            pSource->Release();
            pDest->Release();

            if (FAILED(hr)) {
                _Error("Failed to queue move operation (HRESULT: 0x%08X)", hr);
                pfo->Release();
                return false;
            }

            const bool success = PerformOperation(pfo);
            pfo->Release();

            if (success) {
                _Info("Moved: %s -> %s", source.string().c_str(), destination.string().c_str());
            } else {
                _Error("Failed to move: %s -> %s", source.string().c_str(), destination.string().c_str());
            }

            return success;
        }

        static bool CopyItem(const fs::path& source, const fs::path& destination) {
            if (!Util::PathExists(source)) {
                _Error("Source path does not exist: %s", source.string().c_str());
                return false;
            }

            std::error_code ec;
            fs::create_directories(destination.parent_path(), ec);
            if (ec) {
                _Error("Failed to create destination directory: %s", destination.parent_path().string().c_str());
                return false;
            }

            IFileOperation* pfo = nullptr;
            HRESULT hr          = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo));
            if (FAILED(hr)) {
                _Error("Failed to create IFileOperation instance (HRESULT: 0x%08X)", hr);
                return false;
            }

            hr = pfo->SetOperationFlags(FOF_NO_UI | FOF_NOCONFIRMATION);
            if (FAILED(hr)) {
                _Error("Failed to set operation flags (HRESULT: 0x%08X)", hr);
                pfo->Release();
                return false;
            }

            IShellItem* pSource = nullptr;
            const auto wSource  = Util::ToWideStr(source.string());
            hr                  = ::SHCreateItemFromParsingName(wSource.c_str(), nullptr, IID_PPV_ARGS(&pSource));
            if (FAILED(hr)) {
                _Error("Failed to create shell item from source: %s (HRESULT: 0x%08X)", source.string().c_str(), hr);
                pfo->Release();
                return false;
            }

            IShellItem* pDest = nullptr;
            const auto wDest  = Util::ToWideStr(destination.parent_path().string());
            hr                = ::SHCreateItemFromParsingName(wDest.c_str(), nullptr, IID_PPV_ARGS(&pDest));
            if (FAILED(hr)) {
                _Error("Failed to create shell item from destination: %s (HRESULT: 0x%08X)",
                       destination.parent_path().string().c_str(),
                       hr);
                pSource->Release();
                pfo->Release();
                return false;
            }

            const auto destFilename = Util::ToWideStr(destination.filename().string());
            hr                      = pfo->CopyItem(pSource, pDest, destFilename.c_str(), nullptr);
            pSource->Release();
            pDest->Release();

            if (FAILED(hr)) {
                _Error("Failed to queue copy operation (HRESULT: 0x%08X)", hr);
                pfo->Release();
                return false;
            }

            const bool success = PerformOperation(pfo);
            pfo->Release();

            if (success) {
                _Info("Copied: %s -> %s", source.string().c_str(), destination.string().c_str());
            } else {
                _Error("Failed to copy: %s -> %s", source.string().c_str(), destination.string().c_str());
            }

            return success;
        }
    }  // namespace FileOps

#pragma endregion

#pragma region UpdateChecking

    namespace Update {
        static constexpr auto kUpdateURL        = L"api.github.com";
        static constexpr auto kUpdatePath       = L"/repos/jakerieger/K8-LRT/releases/latest";
        static constexpr int kResultUpToDate    = 0;
        static constexpr int kResultNewer       = 1;
        static constexpr int kResultFuture      = 2;
        static constexpr int kResultCheckFailed = -1;
        static constexpr auto kLatestReleaseUrl = "https://github.com/jakerieger/K8-LRT/releases/latest";

        struct CheckResult {
            int result;
            char currentVersion[32];
        };

        struct Version {
            int major;
            int minor;
            int patch;

            static Version Parse(const std::string& str) {
                Version v {0, 0, 0};
                if (sscanf_s(str.c_str(), "v%d.%d.%d", &v.major, &v.minor, &v.patch) != 3) {
                    sscanf_s(str.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch);
                }
                return v;
            }

            int Compare(const Version& other) const {
                if (major != other.major)
                    return major - other.major;
                if (minor != other.minor)
                    return minor - other.minor;
                return patch - other.patch;
            }
        };

        static std::string FetchLatestVersion() {
            HINTERNET hSession = nullptr;
            HINTERNET hConnect = nullptr;
            HINTERNET hRequest = nullptr;
            std::string result;

            hSession = ::WinHttpOpen(L"K8Tool/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);

            if (!hSession) {
                _Error("WinHttpOpen failed");
                return result;
            }

            hConnect = ::WinHttpConnect(hSession, kUpdateURL, INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!hConnect) {
                _Error("WinHttpConnect failed");
                ::WinHttpCloseHandle(hSession);
                return result;
            }

            hRequest = ::WinHttpOpenRequest(hConnect,
                                            L"GET",
                                            kUpdatePath,
                                            nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);

            if (!hRequest) {
                _Error("WinHttpOpenRequest failed");
                ::WinHttpCloseHandle(hConnect);
                ::WinHttpCloseHandle(hSession);
                return result;
            }

            const wchar_t* headers = L"User-Agent: K8Tool\r\n";
            if (!::WinHttpSendRequest(hRequest, headers, -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                _Error("WinHttpSendRequest failed");
                ::WinHttpCloseHandle(hRequest);
                ::WinHttpCloseHandle(hConnect);
                ::WinHttpCloseHandle(hSession);
                return result;
            }

            if (!::WinHttpReceiveResponse(hRequest, nullptr)) {
                _Error("WinHttpReceiveResponse failed");
                ::WinHttpCloseHandle(hRequest);
                ::WinHttpCloseHandle(hConnect);
                ::WinHttpCloseHandle(hSession);
                return result;
            }

            DWORD statusCode     = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            ::WinHttpQueryHeaders(hRequest,
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  nullptr,
                                  &statusCode,
                                  &statusCodeSize,
                                  nullptr);

            if (statusCode != 200) {
                _Error("HTTP request failed with status code: %lu", statusCode);
                ::WinHttpCloseHandle(hRequest);
                ::WinHttpCloseHandle(hConnect);
                ::WinHttpCloseHandle(hSession);
                return result;
            }

            DWORD bytesAvailable = 0;
            std::vector<char> buffer;

            while (::WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                std::vector<char> chunk(bytesAvailable + 1);
                DWORD bytesRead = 0;

                if (::WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
                    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + bytesRead);
                }
            }

            if (!buffer.empty()) {
                result = std::string(buffer.begin(), buffer.end());
            }

            ::WinHttpCloseHandle(hRequest);
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);

            return result;
        }

        static std::string ParseTagName(const std::string& json) {
            const auto tagPos = json.find("\"tag_name\"");
            if (tagPos == std::string::npos)
                return {};

            const auto colonPos = json.find(':', tagPos);
            if (colonPos == std::string::npos)
                return {};

            const auto quoteStart = json.find('"', colonPos);
            if (quoteStart == std::string::npos)
                return {};

            const auto quoteEnd = json.find('"', quoteStart + 1);
            if (quoteEnd == std::string::npos)
                return {};

            return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }

        static void Check(HWND hwnd) {
            auto* result   = new CheckResult {};
            result->result = kResultCheckFailed;
            strcpy_s(result->currentVersion, "Unknown");

            auto finish = [&](bool error = false) {
                if (error) {
                    delete result;
                    ::PostMessage(hwnd, WM_UPDATE_CHECK_COMPLETED, 0, (LPARAM) nullptr);
                } else {
                    ::PostMessage(hwnd, WM_UPDATE_CHECK_COMPLETED, 0, (LPARAM)result);
                }
            };

            const std::string json = FetchLatestVersion();
            if (json.empty()) {
                _Error("Failed to fetch latest version");
                return finish(true);
            }

            const std::string latestTag = ParseTagName(json);
            if (latestTag.empty()) {
                _Error("Failed to parse tag name from JSON");
                return finish(true);
            }

            strcpy_s(result->currentVersion, latestTag.c_str());

            const Version current = Version::Parse(VER_PRODUCTVERSION_STR);
            const Version latest  = Version::Parse(latestTag);

            const int comparison = current.Compare(latest);
            if (comparison < 0) {
                result->result = kResultNewer;
            } else if (comparison > 0) {
                result->result = kResultFuture;
            } else {
                result->result = kResultUpToDate;
            }

            return finish();
        }
    }  // namespace Update

#pragma endregion

#pragma region Dialog

    namespace Dialog {
        namespace Data {
            struct RemoveSelectedDialogData {
                LibraryInfo libraryInfo;
                bool backupRegistry;
                bool removeContent;
            };
        }  // namespace Data

        namespace DialogProc {
            static INT_PTR CALLBACK About(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                switch (msg) {
                    case WM_NOTIFY: {
                        LPNMHDR pnmh = (LPNMHDR)lParam;
                        if (pnmh->idFrom == IDC_REPO_LINK && (pnmh->code == NM_CLICK || pnmh->code == NM_RETURN)) {
                            PNMLINK pnmLink = (PNMLINK)lParam;
                            ::ShellExecuteW(nullptr,
                                            L"open",
                                            L"https://github.com/jakerieger/K8-LRT",
                                            nullptr,
                                            nullptr,
                                            SW_SHOWNORMAL);
                            return (INT_PTR)TRUE;
                        }
                        break;
                    }

                    case WM_INITDIALOG: {
                        auto latest_v = (char*)lParam;
                        if (latest_v) {
                            ::SetDlgItemTextA(hwnd, IDC_VER_LABEL, std::format("Version {}", latest_v).c_str());
                            ::SetDlgItemTextA(hwnd, IDC_BUILD_LABEL, std::format("Build {}", VER_BUILD).c_str());
                        }

                        return (INT_PTR)TRUE;
                    }

                    case WM_COMMAND:
                        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                            ::EndDialog(hwnd, LOWORD(wParam));
                            return (INT_PTR)TRUE;
                        }

                        if (LOWORD(wParam) == IDC_CHECK_UPDATES_BUTTON) {
                            std::thread(Update::Check, ::GetParent(hwnd)).detach();
                        }

                        break;
                }

                return (INT_PTR)FALSE;
            }

            static INT_PTR CALLBACK LogViewer(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                switch (msg) {
                    case WM_INITDIALOG: {
                        const auto contents = logger->GetLogContents();
                        if (contents.empty()) {
                            ::MessageBox(hwnd,
                                         "Failed to retrieve contents of log file.",
                                         "Error",
                                         MB_OK | MB_ICONERROR);
                            return (INT_PTR)FALSE;
                        }

                        const auto logContents = Util::ToCRLF(contents);
                        const HWND logViewer   = ::GetDlgItem(hwnd, IDC_LOGVIEW_EDIT);
                        ::SetWindowText(logViewer, logContents.c_str());
                        const auto textLength = ::GetWindowTextLength(logViewer);
                        ::SendMessage(logViewer, EM_SETSEL, (WPARAM)textLength, (LPARAM)textLength);
                        ::SendMessage(logViewer, WM_VSCROLL, SB_BOTTOM, 0);

                        return (INT_PTR)FALSE;
                    }

                    case WM_SIZE: {
                        int newWidth  = LOWORD(lParam);
                        int newHeight = HIWORD(lParam);

                        HWND hEdit = ::GetDlgItem(hwnd, IDC_LOGVIEW_EDIT);

                        ::MoveWindow(hEdit, 0, 0, newWidth, newHeight, TRUE);
                        return (INT_PTR)TRUE;
                    }

                    case WM_COMMAND:
                        if (LOWORD(wParam) == IDCANCEL) {
                            ::EndDialog(hwnd, LOWORD(wParam));
                            return (INT_PTR)TRUE;
                        }
                        break;
                }

                return (INT_PTR)FALSE;
            }

            static INT_PTR CALLBACK Remove(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                return (INT_PTR)FALSE;
            }

            static INT_PTR CALLBACK RemoveSelected(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                static Data::RemoveSelectedDialogData* data = nullptr;

                switch (msg) {
                    case WM_INITDIALOG: {
                        data = (Data::RemoveSelectedDialogData*)lParam;

                        const auto hNameLabel = ::GetDlgItem(hwnd, IDC_REMOVE_SELECTED_NAME);
                        ::SetWindowText(hNameLabel, data->libraryInfo.name);

                        HFONT hFont = CreateFont(16,
                                                 0,
                                                 0,
                                                 0,
                                                 FW_BOLD,
                                                 FALSE,
                                                 FALSE,
                                                 FALSE,
                                                 DEFAULT_CHARSET,
                                                 OUT_DEFAULT_PRECIS,
                                                 CLIP_DEFAULT_PRECIS,
                                                 DEFAULT_QUALITY,
                                                 DEFAULT_PITCH | FF_DONTCARE,
                                                 "Segoe UI");
                        ::SendMessage(hNameLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

                        const HWND hContentDirLabel = ::GetDlgItem(hwnd, IDC_REMOVE_SELECTED_CONTENT_DIR);
                        ::SetWindowText(hContentDirLabel, data->libraryInfo.contentDir);

                        ::CheckDlgButton(hwnd,
                                         IDC_REMOVE_SELECTED_BACKUP_CHECK,
                                         data->backupRegistry ? BST_CHECKED : BST_UNCHECKED);
                        ::CheckDlgButton(hwnd,
                                         IDC_REMOVE_SELECTED_CONTENT_DIR_CHECK,
                                         data->removeContent ? BST_CHECKED : BST_UNCHECKED);

                        RECT parentRect, dlgRect;
                        const HWND hParent = ::GetParent(hwnd);
                        ::GetWindowRect(hParent, &parentRect);
                        ::GetWindowRect(hwnd, &dlgRect);

                        const int dlgW    = dlgRect.right - dlgRect.left;
                        const int dlgH    = dlgRect.bottom - dlgRect.top;
                        const int parentX = parentRect.left;
                        const int parentY = parentRect.top;
                        const int parentW = parentRect.right - parentRect.left;
                        const int parentH = parentRect.bottom - parentRect.top;

                        const int x = parentX + (parentW - dlgW) / 2;
                        const int y = parentY + (parentH - dlgH) / 2;

                        ::SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

                        const auto hWarningLabel = ::GetDlgItem(hwnd, IDC_REMOVE_SELECTED_WARNING_TEXT);
                        ::SendMessage(hWarningLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

                        return (INT_PTR)TRUE;
                    }

                    case WM_COMMAND: {
                        switch (LOWORD(wParam)) {
                            case ID_REMOVE_SELECTED_REMOVE: {
                                data->backupRegistry =
                                  (::IsDlgButtonChecked(hwnd, IDC_REMOVE_SELECTED_BACKUP_CHECK) == BST_CHECKED);
                                data->removeContent =
                                  (::IsDlgButtonChecked(hwnd, IDC_REMOVE_SELECTED_CONTENT_DIR_CHECK) == BST_CHECKED);

                                ::EndDialog(hwnd, ID_REMOVE_SELECTED_REMOVE);
                                return (INT_PTR)TRUE;
                            }

                            case ID_REMOVE_SELECTED_CANCEL:
                            case IDCANCEL: {
                                ::EndDialog(hwnd, IDCANCEL);
                                return (INT_PTR)TRUE;
                            }
                        }
                        break;
                    }

                    case WM_CLOSE: {
                        ::EndDialog(hwnd, IDCANCEL);
                        return (INT_PTR)TRUE;
                    }
                }

                return (INT_PTR)FALSE;
            }

            static INT_PTR CALLBACK RelocateSelected(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                return (INT_PTR)FALSE;
            }
        }  // namespace DialogProc

        static void ShowAbout(HINSTANCE hInst, HWND hwnd, const char* version) {
            ::DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_ABOUT_BOX), hwnd, DialogProc::About, (LPARAM)version);
        }

        static void ShowLogViewer(HINSTANCE hInst, HWND hwnd) {
            ::DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_LOGVIEW_BOX), hwnd, DialogProc::LogViewer, (LPARAM) nullptr);
        }

        static INT_PTR ShowRemove(HINSTANCE hInst, HWND hwnd) {
            return (INT_PTR)FALSE;
        }

        static INT_PTR ShowRemoveSelected(HINSTANCE hInst, HWND hwnd, const Data::RemoveSelectedDialogData* data) {
            const auto result = ::DialogBoxParam(hInst,
                                                 MAKEINTRESOURCE(IDD_REMOVE_SELECTED_BOX),
                                                 hwnd,
                                                 DialogProc::RemoveSelected,
                                                 (LPARAM)data);
            return result;
        }

        static INT_PTR ShowRelocateSelected(HINSTANCE hInst, HWND hwnd) {
            return (INT_PTR)FALSE;
        }
    }  // namespace Dialog

#pragma endregion

#pragma region LibraryScanner

    namespace LibraryScanner {
        static LibraryList Scan(StringPool& pool) {
            LibraryList libraries;
            Registry::QueryLibraries(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Native Instruments)", pool, libraries);
            Registry::QueryLibraries(HKEY_LOCAL_MACHINE, R"(SOFTWARE\WOW6432Node\Native Instruments)", pool, libraries);
            _Info("Found %zu libraries", libraries.size());
            return libraries;
        }
    }  // namespace LibraryScanner

#pragma endregion

#pragma region Threading

    namespace Threads {
        struct RemoveSelectedResult {
            bool success;
            bool cancelled;
        };

        static void RemoveSelected(HWND hwndOwner,
                                   HWND hwndProgress,
                                   const std::string& libraryName,
                                   const std::string& contentDir,
                                   HKEY registryRoot,
                                   const std::string& registrySubKey,
                                   bool backupRegistry,
                                   bool deleteContentDir,
                                   std::stop_token stopToken) {
            _Info("Starting removal process for library: %s", libraryName.c_str());

            auto* result      = new RemoveSelectedResult {};
            result->success   = false;
            result->cancelled = false;

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            // Send progress update
            {
                std::string status = "Locating library XML files...";
                char* statusCopy   = new char[status.size() + 1];
                strcpy_s(statusCopy, status.size() + 1, status.c_str());
                ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            std::optional<XML::LibraryXMLInfo> xmlInfo;
            fs::path xmlPath = fs::path(Globals::kServiceCenter) / (libraryName + ".xml");
            if (!Util::FileExists(xmlPath)) {
                xmlPath = fs::path(Globals::kNativeAccessXML);
            }
            xmlInfo = XML::GetSNPID(xmlPath, libraryName);

            if (!xmlInfo.has_value()) {
                _Error("Could not find SNPID for library: %s", libraryName.c_str());
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            // Send progress update
            {
                std::string status = "Removing library XML file...";
                char* statusCopy   = new char[status.size() + 1];
                strcpy_s(statusCopy, status.size() + 1, status.c_str());
                ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (xmlPath.filename() != "NativeAccess.xml") {
                if (!FileOps::DeleteItem(xmlPath)) {
                    _Error("Failed to delete XML file: %s", xmlPath.string().c_str());
                }
            }

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            // Send progress update
            {
                std::string status = "Removing cache files...";
                char* statusCopy   = new char[status.size() + 1];
                strcpy_s(statusCopy, status.size() + 1, status.c_str());
                ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            const fs::path cacheDir = fs::path(Util::GetLocalAppData()) / Globals::kLibrariesCache;
            if (Util::PathExists(cacheDir)) {
                for (const auto& entry : fs::directory_iterator(cacheDir)) {
                    if (entry.path().filename().string().find("K" + xmlInfo->snpid) != std::string::npos) {
                        FileOps::DeleteItem(entry.path());
                    }
                }
            }

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            // Send progress update
            {
                std::string status = "Removing database file...";
                char* statusCopy   = new char[status.size() + 1];
                strcpy_s(statusCopy, status.size() + 1, status.c_str());
                ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            const fs::path db3Path = fs::path(Util::GetLocalAppData()) / Globals::kKompleteDB3;
            if (Util::FileExists(db3Path)) {
                const fs::path backup = db3Path.string() + ".bak";
                FileOps::CopyItem(db3Path, backup);
                FileOps::DeleteItem(db3Path);
            }

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            // Send progress update
            {
                std::string status = "Removing JWT files...";
                char* statusCopy   = new char[status.size() + 1];
                strcpy_s(statusCopy, status.size() + 1, status.c_str());
                ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            const fs::path ras3Dir = Globals::kRAS3;
            if (Util::PathExists(ras3Dir)) {
                for (const auto& entry : fs::directory_iterator(ras3Dir)) {
                    if (entry.path().extension() == ".jwt") {
                        if (entry.path().filename().string().find(xmlInfo->snpid) != std::string::npos) {
                            FileOps::DeleteItem(entry.path());
                        }
                    }
                }
            }

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            if (deleteContentDir && !contentDir.empty()) {
                // Send progress update
                {
                    std::string status = "Removing content directory (this may take a while)...";
                    char* statusCopy   = new char[status.size() + 1];
                    strcpy_s(statusCopy, status.size() + 1, status.c_str());
                    ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                if (!FileOps::DeleteItem(contentDir)) {
                    _Error("Failed to delete content directory: %s", contentDir.c_str());
                }
            }

            if (stopToken.stop_requested()) {
                result->cancelled = true;
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            // Send progress update
            {
                std::string status = "Removing registry entries...";
                char* statusCopy   = new char[status.size() + 1];
                strcpy_s(statusCopy, status.size() + 1, status.c_str());
                ::PostMessage(hwndProgress, WM_UPDATE_PROGRESS_TEXT, 0, (LPARAM)statusCopy);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (backupRegistry) {
                const fs::path backupPath = fs::current_path() / "backup" / std::format("{}.reg", libraryName);
                if (!Registry::BackupKey(registryRoot, registrySubKey.c_str(), backupPath)) {
                    _Warn("Failed to backup registry key for library: %s", libraryName.c_str());
                }
            }

            if (!Registry::DeleteKey(registryRoot, registrySubKey.c_str())) {
                _Error("Failed to delete registry key for library: %s", libraryName.c_str());
                ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
                return;
            }

            result->success = true;
            ::PostMessage(hwndOwner, WM_REMOVE_SELECTED_COMPLETED, 0, (LPARAM)result);
        }
    };  // namespace Threads

#pragma endregion

#pragma region Application

    class ApplicationError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class Application {
        static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

        HINSTANCE _hInstance {nullptr};
        HWND _hwnd {nullptr};
        HWND _hProgress {nullptr};
        LPCSTR _title;
        UINT _width, _height;
        int _nCmdShow;
        bool _consoleAttached {false};

        // UI Members
        HFONT _font;
        HWND _label;
        HWND _listView;
        HWND _removeButton;
        HWND _removeSelectedButton;
        HWND _relocateSelectedButton;
        HWND _rescanLibrariesButton;
        HWND _progressLabel;
        HWND _progressBar;

        // UI Element IDs
        static constexpr int kIDC_ListView               = 101;
        static constexpr int kIDC_RemoveButton           = 102;
        static constexpr int kIDC_RemoveSelectedButton   = 103;
        static constexpr int kIDC_RelocateSelectedButton = 104;
        static constexpr int kIDC_SelectLibraryLabel     = 105;
        static constexpr int kIDC_RescanLibrariesButton  = 106;
        static constexpr int kIDC_ProgressLabel          = 107;
        static constexpr int kIDC_ProgressBar            = 108;

        // Business-logic members
        LibraryList _libraries       = {};
        StringPool _strPool          = {};
        std::string _selectedLibrary = {};
        int _selectedIndex           = -1;

        std::jthread _worker {};

    public:
        explicit Application(HINSTANCE hInstance, LPCSTR title, UINT width, UINT height, int nCmdShow)
            : _hInstance(hInstance), _title(title), _width(width), _height(height), _nCmdShow(nCmdShow) {
            Initialize();
        }

        ~Application() {
            Shutdown();
        }

        int Run() const {
            ::ShowWindow(_hwnd, _nCmdShow);
            ::UpdateWindow(_hwnd);

            // Spawn update check thread
            std::thread(Update::Check, _hwnd).detach();

            MSG msg = {0};
            while (::GetMessage(&msg, nullptr, 0, 0)) {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
            }

            return (int)msg.wParam;
        }

    private:
        void ToggleSelectedButtons(bool enabled) const {
            ::EnableWindow(_removeSelectedButton, (BOOL)enabled);
            ::EnableWindow(_relocateSelectedButton, (BOOL)enabled);
        }

        void AttachConsole() {
            if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* fDummy;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                freopen_s(&fDummy, "CONOUT$", "w", stderr);
                _consoleAttached = true;
            } else {
                _consoleAttached = false;
            }
        }

        void ReleaseConsole() {
            if (!_consoleAttached)
                return;

            if (!FreeConsole()) {
                _Error("Unknown error occurred releasing console.");
                return;
            }

            _consoleAttached = false;
        }

        void Initialize() {
#ifndef NDEBUG
            AttachConsole();
#endif

            logger->SetConsoleAttached(_consoleAttached);

            if (!Registry::EnableBackupPrivileges()) {
                throw ApplicationError("Failed to enable registry backup privileges.");
            }

            // Create backup directory if it does not exist
            if (!fs::exists("backup")) {
                fs::create_directory("backup");
            }

            // Create export directory if it does not exist
            if (!fs::exists("export")) {
                fs::create_directory("export");
            }

            // Initialize COM
            auto hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr)) {
                throw ApplicationError("Failed to initialize COM library.");
            }

            // Initialize Common Controls
            INITCOMMONCONTROLSEX icc;
            icc.dwSize = sizeof(icc);
            icc.dwICC  = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
            InitCommonControlsEx(&icc);

            // Create window
            WNDCLASS wc      = {0};
            wc.lpfnWndProc   = Application::WndProc;
            wc.hInstance     = _hInstance;
            wc.lpszClassName = "K8Tool_AppClass";
            wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
            wc.hIcon         = ::LoadIcon(_hInstance, MAKEINTRESOURCE(IDI_APPICON));

            ::RegisterClass(&wc);

            const int screen_w = ::GetSystemMetrics(SM_CXSCREEN);
            const int screen_h = ::GetSystemMetrics(SM_CYSCREEN);
            const int win_x    = (screen_w - _width) / 2;
            const int win_y    = (screen_h - _height) / 2;

            _hwnd = CreateWindowEx(0,
                                   wc.lpszClassName,
                                   _title,
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                   win_x,
                                   win_y,
                                   _width,
                                   _height,
                                   nullptr,
                                   nullptr,
                                   _hInstance,
                                   this);
            if (_hwnd == nullptr) {
                throw ApplicationError("Failed to create window.");
            }

            constexpr DWORD use = TRUE;
            hr                  = ::DwmSetWindowAttribute(_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use, sizeof(use));
            if (FAILED(hr)) {
                _Warn("Failed to enable dark mode for title bar.");
            }
        }

        void Shutdown() {
#ifndef NDEBUG
            ReleaseConsole();
#endif
            ::CoUninitialize();
        }

        void ScanAndPopulate(bool showDlg = false) {
            _Info("Scanning for libraries...");
            _libraries = LibraryScanner::Scan(_strPool);
            ListView_DeleteAllItems(_listView);
            for (size_t i = 0; i < _libraries.size(); ++i) {
                LVITEM lvi {};
                lvi.mask     = LVIF_TEXT;
                lvi.iItem    = static_cast<int>(i);
                lvi.iSubItem = 0;
                lvi.pszText  = const_cast<char*>(_libraries[i].name);
                ListView_InsertItem(_listView, &lvi);

                ListView_SetItemText(_listView, static_cast<int>(i), 1, const_cast<char*>(_libraries[i].contentDir));

                // Convert size on disk to string
                const auto sizeOnDisk = Util::FormatFileSize(_libraries[i].sizeOnDisk);
                ListView_SetItemText(_listView, static_cast<int>(i), 2, const_cast<char*>(sizeOnDisk.c_str()));
            }
            _Info("Populated ListView with %zu libraries", _libraries.size());
            if (showDlg) {
                ::MessageBox(
                  nullptr,
                  std::format("Scan completed successfully.\n\nLibraries found: {}", _libraries.size()).c_str(),
                  "K8Tool",
                  MB_OK | MB_ICONINFORMATION);
            }
        }

        void RescanAndReset(bool showDlg = false) {
            _selectedLibrary.clear();
            _selectedIndex = -1;
            ToggleSelectedButtons(false);
            ScanAndPopulate(showDlg);
            ::SetWindowText(_label, std::format("Select a library to remove (found {}):", _libraries.size()).c_str());
        }

        void ShowProgress(const char* statusText = nullptr) const {
            // Hide non-progress UI elements
            ::ShowWindow(_label, SW_HIDE);
            ::ShowWindow(_rescanLibrariesButton, SW_HIDE);
            ::ShowWindow(_listView, SW_HIDE);
            ::ShowWindow(_removeButton, SW_HIDE);
            ::ShowWindow(_removeSelectedButton, SW_HIDE);
            ::ShowWindow(_relocateSelectedButton, SW_HIDE);

            // Show progress UI elements
            ::ShowWindow(_progressLabel, SW_SHOW);
            ::ShowWindow(_progressBar, SW_SHOW);
            // Start progress bar marquee animation
            ::SendMessage(_progressBar, PBM_SETMARQUEE, TRUE, 30);

            if (statusText != nullptr) {
                UpdateProgressText(statusText);
            }
        }

        void HideProgress() const {
            // Show non-progress UI elements
            ::ShowWindow(_label, SW_SHOW);
            ::ShowWindow(_rescanLibrariesButton, SW_SHOW);
            ::ShowWindow(_listView, SW_SHOW);
            ::ShowWindow(_removeButton, SW_SHOW);
            ::ShowWindow(_removeSelectedButton, SW_SHOW);
            ::ShowWindow(_relocateSelectedButton, SW_SHOW);

            // Hide progress UI elements
            ::ShowWindow(_progressLabel, SW_HIDE);
            ::ShowWindow(_progressBar, SW_HIDE);
            // Stop progress bar animation
            ::SendMessage(_progressBar, PBM_SETMARQUEE, FALSE, NULL);
        }

        void UpdateProgressText(const char* text) const {
            ::SetWindowText(_progressLabel, text);
        }

        void OnCreate(HWND hwnd) {
            _font = CreateFont(16,
                               0,
                               0,
                               0,
                               FW_NORMAL,
                               FALSE,
                               FALSE,
                               FALSE,
                               DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE,
                               "Segoe UI");
            if (!_font) {
                _Warn("Failed to create default font. Falling back to system font.");
            }

            HMENU hMenubar = CreateMenu();
            HMENU hMenu    = CreateMenu();

            AppendMenu(hMenu, MF_STRING, ID_MENU_VIEW_LOG, "&View Log");
            AppendMenu(hMenu, MF_STRING, ID_MENU_RESCAN_LIBRARIES, "&Rescan Libraries");
            AppendMenu(hMenu, MF_STRING, ID_MENU_COLLECT_BACKUPS, "&Collect Backups and Zip");
            AppendMenu(hMenu, MF_STRING, ID_MENU_EXPORT_LIBRARY_LIST, "&Export Library List");
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_MENU_ABOUT, "&About");
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_MENU_EXIT, "E&xit");

            AppendMenu(hMenubar, MF_POPUP, (UINT_PTR)hMenu, "&Menu");
            SetMenu(hwnd, hMenubar);

            _label = ::CreateWindow("STATIC",
                                    "Select a library to remove:",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    10,
                                    10,
                                    220,
                                    16,
                                    hwnd,
                                    (HMENU)kIDC_SelectLibraryLabel,
                                    _hInstance,
                                    nullptr);
            ::SendMessage(_label, WM_SETFONT, (WPARAM)_font, TRUE);

            _rescanLibrariesButton = ::CreateWindowEx(0,
                                                      "BUTTON",
                                                      "Rescan Libraries",
                                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                      _width - 146,
                                                      5,
                                                      120,
                                                      25,
                                                      hwnd,
                                                      (HMENU)kIDC_RescanLibrariesButton,
                                                      _hInstance,
                                                      nullptr);
            ::SendMessage(_rescanLibrariesButton, WM_SETFONT, (WPARAM)_font, TRUE);

            _listView = ::CreateWindowEx(WS_EX_CLIENTEDGE,
                                         WC_LISTVIEW,
                                         "",
                                         WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                         10,
                                         34,
                                         564,
                                         274,
                                         hwnd,
                                         (HMENU)kIDC_ListView,
                                         _hInstance,
                                         nullptr);
            ListView_SetExtendedListViewStyle(_listView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

            LVCOLUMN lvc;
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            // Column 1: Name
            lvc.iSubItem = 0;
            lvc.pszText  = (char*)"Name";
            lvc.cx       = 200;
            ListView_InsertColumn(_listView, 0, &lvc);
            // Column 2: ContentDir
            lvc.iSubItem = 1;
            lvc.pszText  = (char*)"Content Directory";
            lvc.cx       = 260;
            ListView_InsertColumn(_listView, 1, &lvc);
            // Column 3: Size on disk
            lvc.iSubItem = 2;
            lvc.pszText  = (char*)"Size";
            lvc.cx       = 80;
            ListView_InsertColumn(_listView, 2, &lvc);

            constexpr auto buttonWidth  = 184;
            constexpr auto buttonHeight = 30;
            _removeButton               = ::CreateWindowEx(0,
                                             "BUTTON",
                                             "Remove...",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                                             10,
                                             320,
                                             buttonWidth,
                                             buttonHeight,
                                             hwnd,
                                             (HMENU)kIDC_RemoveButton,
                                             _hInstance,
                                             nullptr);
            ::SendMessage(_removeButton, WM_SETFONT, (WPARAM)_font, TRUE);

            _removeSelectedButton = ::CreateWindowEx(0,
                                                     "BUTTON",
                                                     "Remove Selected",
                                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                                     (_width / 2 - (buttonWidth / 2)) - 8,
                                                     320,
                                                     buttonWidth,
                                                     buttonHeight,
                                                     hwnd,
                                                     (HMENU)kIDC_RemoveSelectedButton,
                                                     _hInstance,
                                                     nullptr);
            ::SendMessage(_removeSelectedButton, WM_SETFONT, (WPARAM)_font, TRUE);

            _relocateSelectedButton = ::CreateWindowEx(0,
                                                       "BUTTON",
                                                       "Relocate Selected",
                                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                                       _width - buttonWidth - 26,
                                                       320,
                                                       buttonWidth,
                                                       buttonHeight,
                                                       hwnd,
                                                       (HMENU)kIDC_RelocateSelectedButton,
                                                       _hInstance,
                                                       nullptr);
            ::SendMessage(_relocateSelectedButton, WM_SETFONT, (WPARAM)_font, TRUE);

            // Progress bar and label (initially hidden - missing WS_VISIBLE flag)
            _progressLabel = ::CreateWindow("STATIC",
                                            "Removing library 'Ultimate Heavy Drums'...",
                                            WS_CHILD | SS_CENTER,
                                            0,
                                            (_height / 3),
                                            _width,
                                            16,
                                            hwnd,
                                            (HMENU)kIDC_ProgressLabel,
                                            _hInstance,
                                            nullptr);
            ::SendMessage(_progressLabel, WM_SETFONT, (WPARAM)_font, TRUE);

            _progressBar = ::CreateWindowEx(0,
                                            PROGRESS_CLASS,
                                            "",
                                            WS_CHILD | PBS_MARQUEE,
                                            30,
                                            (_height / 3) + 30,
                                            _width - 80,
                                            20,
                                            hwnd,
                                            (HMENU)kIDC_ProgressBar,
                                            _hInstance,
                                            nullptr);

            ScanAndPopulate();

            ::SetWindowText(_label, std::format("Select a library to remove (found {}):", _libraries.size()).c_str());
        }

        void OnRemove() {
            ::MessageBox(_hwnd, "This feature isn't available yet.", "K8Tool", MB_OK | MB_ICONWARNING);
            // const auto result = Dialog::ShowRemove(_hInstance, _hwnd);
            // if (result == ID_REMOVE_REMOVE) {}
        }

        void OnRemoveSelected() {
            if (_worker.joinable()) {
                ::MessageBox(_hwnd,
                             "An operation is already running. Please wait until it finishes.",
                             "K8Tool",
                             MB_OK | MB_ICONWARNING);
                return;
            }

            if (_selectedLibrary.empty())
                return;

            const LibraryInfo& lib = _libraries[_selectedIndex];

            Dialog::Data::RemoveSelectedDialogData data;
            data.libraryInfo    = lib;
            data.backupRegistry = false;
            data.removeContent  = true;

            const auto result = Dialog::ShowRemoveSelected(_hInstance, _hwnd, &data);
            if (result == ID_REMOVE_SELECTED_REMOVE) {
                _Info(
                  "Removing library:\n  - Name: %s\n  - Content Directory: %s\n  - Backup: %s\n  - Remove Content: %s",
                  lib.name,
                  lib.contentDir,
                  data.backupRegistry ? "True" : "False",
                  data.removeContent ? "True" : "False");

                ShowProgress(std::format("Removing '{}'...", lib.name).c_str());

                // Spawn removal thread
                _worker = std::jthread([&](std::stop_token st) {
                    Threads::RemoveSelected(_hwnd,
                                            _hProgress,
                                            lib.name,
                                            lib.contentDir,
                                            lib.registryRoot,
                                            lib.subKey,
                                            data.backupRegistry,
                                            data.removeContent,
                                            st);
                });
            }
        }

        void OnRelocateSelected() {
            ::MessageBox(_hwnd, "This feature isn't available yet.", "K8Tool", MB_OK | MB_ICONWARNING);
            // const auto result = Dialog::ShowRelocateSelected(_hInstance, _hwnd);
            // if (result == ID_RELOCATE_SELECTED_RELOCATE) {}
        }

        void OnRescanLibraries() {
            const auto response =
              ::MessageBox(_hwnd,
                           "Are you sure you want to clear the current library list and scan again?",
                           "K8Tool",
                           MB_YESNO | MB_ICONQUESTION);
            if (response == IDYES) {
                RescanAndReset(true);
            }
        }

        void OnCollectBackups() const {
            _Info("Collecting backups...");
            std::thread([&] {
                if (!fs::exists("backup")) {
                    ::PostMessage(_hwnd, WM_COLLECT_BACKUPS_COMPLETED, 0, (LPARAM) nullptr);
                }

                miniz_cpp::zip_file zip;
                for (const auto& backup : fs::directory_iterator("backup")) {
                    if (backup.is_regular_file() && backup.path().filename().extension() == ".reg") {
                        zip.write(backup.path().string());
                    }
                }

                const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                const auto filename  = std::format("K8Tool-Backup-{}.zip", timestamp);
                zip.save(filename);

                const char* result = _strdup(filename.c_str());
                ::PostMessage(_hwnd, WM_COLLECT_BACKUPS_COMPLETED, 0, (LPARAM)result);
            }).detach();
        }

        void OnExit() {
            const auto response =
              ::MessageBox(_hwnd, "Are you sure you want to exit?", "K8Tool", MB_YESNO | MB_ICONQUESTION);
            if (response == IDYES) {
                if (_worker.joinable()) {
                    _worker.request_stop();
                    _worker.join();
                }
                ::PostQuitMessage(0);
            }
        }

        void OnUpdateCheckCompleted(const Update::CheckResult* result) const {
            switch (result->result) {
                case Update::kResultUpToDate: {
                    _Info("Update check completed (UP-TO-DATE)");
                    ::MessageBox(_hwnd, "You're running the latest version!", "Update", MB_OK | MB_ICONINFORMATION);
                    break;
                }

                case Update::kResultNewer: {
                    const auto message = std::format("A new version of K8-LRT is available!\n\n"
                                                     "Current: {}\n"
                                                     "Latest: {}\n\n"
                                                     "Visit the GitHub releases page to download?",
                                                     VER_PRODUCTVERSION_STR,
                                                     &result->currentVersion[0] + 1);
                    _Info("Update check completed (OUTDATED)");

                    const int response =
                      ::MessageBox(_hwnd, message.c_str(), "Update Available", MB_YESNO | MB_ICONINFORMATION);

                    if (response == IDYES) {
                        ::ShellExecute(NULL, "open", Update::kLatestReleaseUrl, NULL, NULL, SW_SHOWNORMAL);
                    }

                    break;
                }

                case Update::kResultFuture: {
                    const auto message =
                      std::format("You are running a development build. K8Tool may not be stable.\n\n"
                                  "Current: {}\n"
                                  "Yours: {}\n\n"
                                  "While this version may work, we recommend downloading the latest "
                                  "stable release of K8Tool.",
                                  &result->currentVersion[0] + 1,
                                  VER_PRODUCTVERSION_STR);
                    _Info("Update check completed (DEV BUILD)");
                    ::MessageBox(_hwnd, message.c_str(), "Update", MB_OK | MB_ICONWARNING);
                    break;
                }
            }
            delete result;
        }

        void OnRemoveSelectedCompleted(const Threads::RemoveSelectedResult* result) {
            HideProgress();

            const bool success   = result->success;
            const bool cancelled = result->cancelled;
            delete result;

            _worker = {};

            if (success) {
                ::MessageBox(_hwnd, "Library removed successfully.", "K8Tool", MB_OK | MB_ICONINFORMATION);
                RescanAndReset(true);
            } else {
                if (cancelled) {
                    ::MessageBox(_hwnd, "Operation was cancelled.", "K8Tool", MB_OK | MB_ICONWARNING);
                } else {
                    ::MessageBox(_hwnd, "Failed to remove library.", "K8Tool", MB_OK | MB_ICONERROR);
                }
            }
        }

        void OnExportLibraryList() {
            const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            const auto filename  = std::format("LibraryList-{}.csv", timestamp);
            const auto filepath  = fs::current_path() / "export" / filename;
            const auto exported  = Util::ExportListViewToCSV(_listView, filepath);
            if (!exported) {
                ::MessageBox(_hwnd, "Failed to export library list.", "K8Tool", MB_OK | MB_ICONERROR);
                return;
            }
            ::MessageBox(_hwnd,
                         std::format("Exported library list to:\n{}", filepath.string()).c_str(),
                         "K8Tool",
                         MB_OK | MB_ICONINFORMATION);
        }

        LRESULT CALLBACK HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            switch (msg) {
                case WM_CREATE: {
                    OnCreate(hwnd);
                    return 0;
                }

                case WM_ERASEBKGND: {
                    const auto hdc = (HDC)wParam;
                    RECT rect;
                    ::GetClientRect(hwnd, &rect);
                    ::FillRect(hdc, &rect, ::GetSysColorBrush(COLOR_WINDOW));
                    return 1;
                }

                case WM_CTLCOLORSTATIC: {
                    HDC hdc = (HDC)wParam;
                    // HWND control = (HWND)lParam;
                    ::SetBkMode(hdc, TRANSPARENT);
                    return (LRESULT)::GetSysColorBrush(COLOR_WINDOW);
                }

                case WM_NOTIFY: {
                    LPNMHDR lpnmh = (LPNMHDR)lParam;
                    if (lpnmh->idFrom == kIDC_ListView && lpnmh->code == LVN_ITEMCHANGED) {
                        LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
                        if ((pnmv->uChanged & LVIF_STATE) && (pnmv->uNewState & LVIS_SELECTED)) {
                            _selectedIndex   = pnmv->iItem;
                            char buffer[256] = {'\0'};
                            ListView_GetItemText(lpnmh->hwndFrom, _selectedIndex, 0, buffer, 256);
                            _selectedLibrary = buffer;

                            ToggleSelectedButtons(!_selectedLibrary.empty());
                        }
                    }

                    break;
                }

                case WM_COMMAND: {
                    const auto command = LOWORD(wParam);
                    switch (command) {
                        case kIDC_RemoveButton: {
                            OnRemove();
                            break;
                        }

                        case kIDC_RemoveSelectedButton: {
                            OnRemoveSelected();
                            break;
                        }

                        case kIDC_RelocateSelectedButton: {
                            OnRelocateSelected();
                            break;
                        }

                        case kIDC_RescanLibrariesButton:
                        case ID_MENU_RESCAN_LIBRARIES: {
                            OnRescanLibraries();
                            break;
                        }

                        case ID_MENU_VIEW_LOG: {
                            Dialog::ShowLogViewer(_hInstance, hwnd);
                            break;
                        }

                        case ID_MENU_ABOUT: {
                            Dialog::ShowAbout(_hInstance, hwnd, VER_PRODUCTVERSION_STR);
                            break;
                        }

                        case ID_MENU_EXIT: {
                            OnExit();
                            break;
                        }

                        case ID_MENU_COLLECT_BACKUPS: {
                            OnCollectBackups();
                            break;
                        }

                        case ID_MENU_EXPORT_LIBRARY_LIST: {
                            OnExportLibraryList();
                            break;
                        }
                    }

                    return 0;
                }

                case WM_CLOSE: {
                    ::DestroyWindow(hwnd);
                    break;
                }

                case WM_DESTROY: {
                    ::PostQuitMessage(0);
                    return 0;
                }

                case WM_UPDATE_CHECK_COMPLETED: {
                    const auto result = reinterpret_cast<Update::CheckResult*>(lParam);
                    if (!result) {
                        _Error("Failed to get result from update check.");
                        break;
                    }
                    OnUpdateCheckCompleted(result);
                    break;
                }

                case WM_REMOVE_SELECTED_COMPLETED: {
                    const auto result = reinterpret_cast<Threads::RemoveSelectedResult*>(lParam);
                    if (!result) {
                        _Error("Failed to get result from update check.");
                        break;
                    }
                    OnRemoveSelectedCompleted(result);
                    break;
                }

                case WM_COLLECT_BACKUPS_COMPLETED: {
                    const auto result = reinterpret_cast<const char*>(lParam);
                    if (!result) {
                        _Error("Failed to collect backups.");
                        ::MessageBox(_hwnd, "Failed to collect backups.", "K8Tool", MB_ICONERROR | MB_OK);
                    }

                    const auto _msg = std::format("Collected backups to:\n{}",
                                                  fs::absolute(fs::current_path() / result).string().c_str());
                    _Info(_msg.c_str());
                    ::MessageBox(_hwnd, _msg.c_str(), "K8Tool", MB_ICONINFORMATION | MB_OK);
                    delete result;
                }

                case WM_UPDATE_PROGRESS_TEXT: {
                    UpdateProgressText((const char*)lParam);
                }
            }

            return ::DefWindowProc(hwnd, msg, wParam, lParam);
        }
    };

    LRESULT CALLBACK Application::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Application* pThis = nullptr;

        if (msg == WM_NCCREATE) {
            const auto pCreate = (CREATESTRUCT*)lParam;
            pThis              = (Application*)pCreate->lpCreateParams;
            pThis->_hwnd       = hwnd;
            ::SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
        } else {
            pThis = (Application*)::GetWindowLongPtr(hwnd, GWLP_USERDATA);
        }

        if (pThis) {
            return pThis->HandleMessage(hwnd, msg, wParam, lParam);
        }

        return ::DefWindowProc(hwnd, msg, wParam, lParam);
    }

#pragma endregion
}  // namespace K8

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    _Unused(hPrevInstance);
    _Unused(lpCmdLine);

    K8::logger = new K8::Logger;

    using namespace K8;
    try {
        const int result = Application {hInstance, "K8Tool - v" VER_FILEVERSION_STR, 600, 420, nCmdShow}.Run();
        delete logger;
        return result;
    } catch (const std::exception& ex) { _Fatal("A fatal error occurred during startup:\n\n%s", ex.what()); }
}