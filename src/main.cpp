// clang-format off
/*
    K8Tool - v3.0.0 - Library removal tool for Bobdule's Kontakt 8

    LICENSE

        Unlicense

        This is free and unencumbered software released into the public domain.

        Anyone is free to copy, modify, publish, use, compile, sell, or
        distribute this software, either in source code form or as a compiled
        binary, for any purpose, commercial or non-commercial, and by any
        means.

        In jurisdictions that recognize copyright laws, the author or authors
        of this software dedicate any and all copyright interest in the
        software to the public domain. We make this dedication for the benefit
        of the public at large and to the detriment of our heirs and
        successors. We intend this dedication to be an overt act of
        relinquishment in perpetuity of all present and future rights to this
        software under copyright law.

        THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
        EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
        MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
        IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
        OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
        ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
        OTHER DEALINGS IN THE SOFTWARE.

        For more information, please refer to <https://unlicense.org/>

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
                             unified file api via IFileOperation, refactored file operations,
                             dark mode
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
// clang-format on

#include "version.h"
#include "resource.h"

#pragma warning(disable : 4312)

#define _CRT_SECURE_NO_WARNINGS 1

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

// Windows API
#include <commctrl.h>
#include <windows.h>
#include <winhttp.h>
#include <winreg.h>

#define _UNUSED(x) (void)(x)

namespace K8 {
#pragma region CustomEvents

#define WM_UPDATE_CHECK_COMPLETED (WM_USER + 1)

#pragma endregion

#pragma region Logging
    static constexpr std::string_view kLogFilename = "K8Tool.log";

    enum class LogLevel {
        Info,
        Warn,
        Error,
        Fatal,
        Debug,
    };

    class Logger {
        FILE* _file {nullptr};
        bool _console_attached {false};

    public:
        Logger() {
            const errno_t result = fopen_s(&_file, kLogFilename.data(), "a+");
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
            _console_attached = attached;
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
            if (_console_attached) {
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

    static Logger g_Logger;

#define _INFO(fmt, ...) g_Logger.Log(LogLevel::Info, fmt, ##__VA_ARGS__)
#define _WARN(fmt, ...) g_Logger.Log(LogLevel::Warn, fmt, ##__VA_ARGS__)
#define _ERROR(fmt, ...) g_Logger.Log(LogLevel::Error, fmt, ##__VA_ARGS__)
#define _FATAL(fmt, ...)                                                                                               \
    do {                                                                                                               \
        g_Logger.Log(LogLevel::Fatal, fmt, ##__VA_ARGS__);                                                             \
        std::quick_exit(-1);                                                                                           \
    } while (FALSE)
#define _LOG(fmt, ...) g_Logger.Log(LogLevel::Debug, fmt, ##__VA_ARGS__)
#pragma endregion

#pragma region Utils

    namespace Utils {
        static std::string LF_To_CRLF(const std::string& input) {
            std::string output;
            output.reserve(input.size() + (input.size() / 10));

            for (const auto c : input) {
                if (c == '\n')
                    output += '\r';
                output += c;
            }

            return output;
        }
    }  // namespace Utils

#pragma endregion

#pragma region UpdateChecking

    class UpdateError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class Update {
        static constexpr auto kAgent       = L"K8Tool/1.0";
        static constexpr auto kApiEndpoint = L"api.github.com";
        static constexpr auto kRepoStub    = L"/repos/jakerieger/K8-LRT/releases/latest";

    public:
        static constexpr auto kLatestReleaseUrl = "https://github.com/jakerieger/K8-LRT/releases/latest";
        static constexpr auto kResultCurrent    = 0;
        static constexpr auto kResultOld        = 1;
        static constexpr auto kResultFuture     = 2;

        struct CheckResult {
            std::string currentVersion;
            int compare;  // 0: Current  1: Old  2: Future
        };

        static void CheckForUpdates(HWND hwnd) {
            const auto currentVersion = GetLatestVersion();
            if (currentVersion.empty())
                throw UpdateError("Current version string is empty");

            const auto compare = CompareVersions(currentVersion, VER_PRODUCTVERSION_STR);

            auto* result           = new CheckResult;
            result->currentVersion = currentVersion;
            if (compare > 0)
                result->compare = kResultOld;
            else if (compare < 0)
                result->compare = kResultFuture;
            else
                result->compare = kResultCurrent;

            ::PostMessage(hwnd, WM_UPDATE_CHECK_COMPLETED, 0, (LPARAM)result);
        }

    private:
        static std::string ExtractTagName(const std::string& json) {
            constexpr std::string_view key = "\"tag_name\"";
            size_t pos                     = json.find(key);
            if (pos == std::string_view::npos)
                return "";

            pos = json.find(':', pos + key.length());
            if (pos == std::string_view::npos)
                return "";

            size_t start = json.find('"', pos);
            if (start == std::string_view::npos)
                return "";
            start++;

            const size_t end = json.find('"', start);
            if (end == std::string_view::npos)
                return "";

            return std::string(json.substr(start, end - start));
        }

        static int CompareVersions(const std::string& v1, const std::string& v2) {
            auto parse = [](std::string_view v) {
                if (!v.empty() && v[0] == 'v')
                    v.remove_prefix(1);

                int mj = 0, mn = 0, p = 0;
                char dot;

                std::stringstream ss((std::string(v)));
                ss >> mj >> dot >> mn >> dot >> p;

                return std::vector<int> {mj, mn, p};
            };

            const std::vector<int> ver1 = parse(v1);
            const std::vector<int> ver2 = parse(v2);

            if (ver1 != ver2) {
                for (size_t i = 0; i < 3; ++i) {
                    if (ver1[i] != ver2[i])
                        return ver1[i] - ver2[i];
                }
            }

            return 0;
        }

        struct WinHttpHandleDeleter {
            void operator()(HINTERNET h) const {
                if (h)
                    ::WinHttpCloseHandle(h);
            }
        };

        using SafeHandle = std::unique_ptr<void, WinHttpHandleDeleter>;

        static std::string GetLatestVersion() {
            const SafeHandle hSession(::WinHttpOpen(kAgent,
                                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                    WINHTTP_NO_PROXY_NAME,
                                                    WINHTTP_NO_PROXY_BYPASS,
                                                    0));
            if (!hSession)
                return "";

            const SafeHandle hConn(::WinHttpConnect(hSession.get(), kApiEndpoint, INTERNET_DEFAULT_HTTPS_PORT, 0));
            if (!hConn)
                return "";

            const SafeHandle hReq(::WinHttpOpenRequest(hConn.get(),
                                                       L"GET",
                                                       kRepoStub,
                                                       NULL,
                                                       WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       WINHTTP_FLAG_SECURE));
            if (!hReq)
                return "";

            const auto headers = L"User-Agent: K8-LRT/1.0\r\n";
            ::WinHttpAddRequestHeaders(hReq.get(), headers, -1L, WINHTTP_ADDREQ_FLAG_ADD);

            if (!::WinHttpSendRequest(hReq.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !::WinHttpReceiveResponse(hReq.get(), NULL)) {
                return "";
            }

            DWORD statusCode     = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            ::WinHttpQueryHeaders(hReq.get(),
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  NULL,
                                  &statusCode,
                                  &statusCodeSize,
                                  NULL);

            if (statusCode != 200)
                return "";

            std::string respData;
            DWORD bytesAvail = 0;
            while (::WinHttpQueryDataAvailable(hReq.get(), &bytesAvail) && bytesAvail > 0) {
                std::vector<char> buffer(bytesAvail);
                DWORD bytes_read = 0;

                if (::WinHttpReadData(hReq.get(), buffer.data(), bytesAvail, &bytes_read)) {
                    respData.append(buffer.data(), bytes_read);
                } else {
                    break;
                }
            }

            return ExtractTagName(respData);
        }
    };

#pragma endregion

#pragma region Registry

    class Registry {
    public:
        static constexpr std::string_view kPrimaryRoot   = "SOFTWARE\\Native Instruments";
        static constexpr std::string_view kSecondaryRoot = "SOFTWARE\\WOW6432Node\\Native Instruments";

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
                _INFO("Enabled registry backup privileges.");

                return true;
            }

            _ERROR("Failed to enable registry backup privileges. Make sure you're running K8Tool as Admin.");
            return false;
        }

        static bool OpenKey(HKEY* key, HKEY base, const std::string& path, UINT sam) {
            const LONG result = ::RegOpenKeyExA(base, path.c_str(), 0, (REGSAM)sam, key);
            if (result != ERROR_SUCCESS) {
                _WARN("Failed to open registry key: %s", path.c_str());
                return false;
            }

            _INFO("Opened registry key: %s", path.c_str());
            return true;
        }

        static void CloseKey(HKEY key) {
            ::RegCloseKey(key);
        }

        static bool DeleteKey(HKEY base, const char* key) {
            const LONG result = ::RegDeleteKeyA(base, key);
            if (result != ERROR_SUCCESS) {
                _ERROR("Failed to delete registry key: %s", key);
                return false;
            }

            _INFO("Deleted registry key: %s", key);
            return true;
        }

        static std::vector<std::string> EnumerateKeys(HKEY key, const char* base) {
            std::vector<std::string> keys;

            char current[MAX_PATH];
            DWORD bufferSize = sizeof(current);
            FILETIME ftLast;
            DWORD index = 0;

            while (::RegEnumKeyExA(key, index, current, &bufferSize, nullptr, nullptr, nullptr, &ftLast) ==
                   ERROR_SUCCESS) {
                bufferSize = sizeof(current);
                keys.push_back(std::format("{}\\{}", base, current));
                index++;
            }

            return keys;
        }

        static bool GetStrValue(HKEY key, const char* valueName, std::string& strValue) {
            char value[MAX_PATH];
            DWORD bufferSize = sizeof(value);
            DWORD valueType;

            const LSTATUS status = ::RegQueryValueExA(key, valueName, nullptr, &valueType, (LPBYTE)value, &bufferSize);
            if (status == ERROR_SUCCESS && valueType == REG_SZ) {
                strValue = value;
                return true;
            }

            return false;
        }

        static bool SetStrValue(HKEY key, const char* valueName, const char* newValue) {
            const LSTATUS status =
              ::RegSetValueExA(key, valueName, 0, REG_SZ, (const BYTE*)newValue, (DWORD)(strlen(newValue) + 1));
            return (status == ERROR_SUCCESS);
        }
    };

#pragma endregion

#pragma region Libraries
    using Library = std::pair<std::string, std::string>;

    class LibraryManager {
        // Name : ContentDir
        using LibraryMap      = std::unordered_map<std::string, std::string>;
        LibraryMap _libraries = {};

        static constexpr std::array kExclusionList = {
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

    public:
        LibraryManager() = default;

        void Scan(HWND listView, bool showDlg = false) {
            Reset(listView);

            HKEY keyPrimary;
            HKEY keySecondary;

            std::vector<std::string> keys;
            std::vector<std::string> keysSecondary;

            bool opened = Registry::OpenKey(&keyPrimary, HKEY_LOCAL_MACHINE, Registry::kPrimaryRoot.data(), KEY_READ);
            if (opened) {
                keys = Registry::EnumerateKeys(keyPrimary, Registry::kPrimaryRoot.data());
                Registry::CloseKey(keyPrimary);
            }

            opened = Registry::OpenKey(&keySecondary, HKEY_LOCAL_MACHINE, Registry::kSecondaryRoot.data(), KEY_READ);
            if (opened) {
                keysSecondary = Registry::EnumerateKeys(keySecondary, Registry::kSecondaryRoot.data());
                Registry::CloseKey(keySecondary);
            }

            if (!keysSecondary.empty()) {
                std::ranges::move(keysSecondary, std::back_inserter(keys));
            }

            for (const auto& path : keys) {
                std::string name;
                const auto last_slash_pos = path.rfind('\\');
                if (last_slash_pos != std::string::npos) {
                    name = path.substr(last_slash_pos + 1);
                }

                const bool excluded = std::ranges::find(kExclusionList, name) != kExclusionList.end();
                if (excluded)
                    continue;

                HKEY subkey;
                opened = Registry::OpenKey(&subkey, HKEY_LOCAL_MACHINE, path.data(), KEY_READ);
                if (!opened)
                    continue;

                std::pair<std::string, std::string> library;
                library.first = name;
                Registry::GetStrValue(subkey, "ContentDir", library.second);
                if (library.second.empty()) {
                    _WARN("Failed to retrieve ContentDir value for registry key: %s", path.c_str());
                }
                Registry::CloseKey(subkey);

                _libraries.insert_or_assign(library.first, library.second);
                _INFO("Found library: %s\n  - (%s)", library.first.c_str(), library.second.c_str());

                // Insert into ListView
                LVITEM lvi  = {0};
                lvi.mask    = LVIF_TEXT;
                lvi.iItem   = 0;
                lvi.pszText = (char*)library.first.c_str();

                int index = ListView_InsertItem(listView, &lvi);
                ListView_SetItemText(listView, index, 1, (char*)library.second.c_str());
            }

            if (showDlg) {
                ::MessageBox(
                  nullptr,
                  std::format("Scan completed successfully.\n\nLibraries found: {}", _libraries.size()).c_str(),
                  "K8Tool",
                  MB_OK | MB_ICONINFORMATION);
            }
        }

        void Reset(HWND listView) {
            _libraries.clear();
            if (!ListView_DeleteAllItems(listView)) {
                _ERROR("Failed to clear ListView items.");
            }
        }

        bool Contains(const std::string& libraryName) const {
            return _libraries.contains(libraryName);
        }

        const std::string& GetContentDir(const std::string& libraryName) {
            return _libraries.at(libraryName);
        }

        const LibraryMap& GetLibraries() const {
            return _libraries;
        }

        Library GetLibrary(const std::string& libraryName) {
            return std::make_pair(libraryName, _libraries.at(libraryName));
        }
    };

#pragma endregion

#pragma region Dialogs

    namespace Dialog {
        struct RemoveSelectedDialogData {
            Library library;
            bool backupCacheFiles;
            bool removeContentDir;
        };

        namespace DialogProc {
            static INT_PTR CALLBACK About(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                switch (msg) {
                    case WM_NOTIFY: {
                        LPNMHDR pnmh = (LPNMHDR)lParam;
                        if (pnmh->idFrom == IDC_REPO_LINK && (pnmh->code == NM_CLICK || pnmh->code == NM_RETURN)) {
                            PNMLINK pnmLink = (PNMLINK)lParam;
                            ShellExecuteW(nullptr,
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
                            SetDlgItemTextA(hwnd, IDC_VER_LABEL, std::format("Version {}", latest_v).c_str());
                            SetDlgItemTextA(hwnd, IDC_BUILD_LABEL, std::format("Build {}", VER_BUILD).c_str());
                        }

                        return (INT_PTR)TRUE;
                    }

                    case WM_COMMAND:
                        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                            EndDialog(hwnd, LOWORD(wParam));
                            return (INT_PTR)TRUE;
                        }

                        if (LOWORD(wParam) == IDC_CHECK_UPDATES_BUTTON) {
                            std::thread(Update::CheckForUpdates, ::GetParent(hwnd)).detach();
                        }

                        break;
                }

                return (INT_PTR)FALSE;
            }

            static INT_PTR CALLBACK LogViewer(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                switch (msg) {
                    case WM_INITDIALOG: {
                        const auto contents = g_Logger.GetLogContents();
                        if (contents.empty()) {
                            ::MessageBox(hwnd,
                                         "Failed to retrieve contents of log file.",
                                         "Error",
                                         MB_OK | MB_ICONERROR);
                            return (INT_PTR)FALSE;
                        }

                        const auto logContents = Utils::LF_To_CRLF(contents);
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

            static INT_PTR CALLBACK Remove(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {}

            static INT_PTR CALLBACK RemoveSelected(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                static RemoveSelectedDialogData* data = nullptr;

                switch (msg) {
                    case WM_INITDIALOG: {
                        data = (RemoveSelectedDialogData*)lParam;

                        const auto hNameLabel = ::GetDlgItem(hwnd, IDC_REMOVE_SELECTED_NAME);
                        ::SetWindowText(hNameLabel, data->library.first.c_str());

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
                        ::SetWindowText(hContentDirLabel, data->library.second.c_str());

                        ::CheckDlgButton(hwnd,
                                         IDC_REMOVE_SELECTED_BACKUP_CHECK,
                                         data->backupCacheFiles ? BST_CHECKED : BST_UNCHECKED);
                        ::CheckDlgButton(hwnd,
                                         IDC_REMOVE_SELECTED_CONTENT_DIR_CHECK,
                                         data->removeContentDir ? BST_CHECKED : BST_UNCHECKED);

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
                                data->backupCacheFiles =
                                  (::IsDlgButtonChecked(hwnd, IDC_REMOVE_SELECTED_BACKUP_CHECK) == BST_CHECKED);
                                data->removeContentDir =
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

            static INT_PTR CALLBACK RelocateSelected(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {}
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

        static INT_PTR ShowRemoveSelected(HINSTANCE hInst, HWND hwnd, const RemoveSelectedDialogData* data) {
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

#pragma region Application

    class ApplicationError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class Application {
        static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

        HINSTANCE _hInstance;
        HWND _hwnd;
        LPCSTR _title;
        UINT _width, _height;
        int _nCmdShow;
        bool _console_attached {false};

        // UI Members
        HFONT _font;
        HWND _listView;
        HWND _removeButton;
        HWND _removeSelectedButton;
        HWND _relocateSelectedButton;

        // UI Element IDs
        static constexpr int kIDC_ListView               = 101;
        static constexpr int kIDC_RemoveButton           = 102;
        static constexpr int kIDC_RemoveSelectedButton   = 103;
        static constexpr int kIDC_RelocateSelectedButton = 104;

        // Business-logic members
        LibraryManager _libManager   = {};
        int _selectedIndex           = -1;
        std::string _selectedLibrary = "";

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
            std::thread(Update::CheckForUpdates, _hwnd).detach();

            MSG msg = {0};
            while (::GetMessage(&msg, nullptr, 0, 0)) {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
            }

            return 0;
        }

    private:
        void AttachConsole() {
            if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* fDummy;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                freopen_s(&fDummy, "CONOUT$", "w", stderr);
                _console_attached = true;
            } else {
                _console_attached = false;
            }
        }

        void ReleaseConsole() {
            if (!_console_attached)
                return;

            if (!FreeConsole()) {
                _ERROR("Unknown error occurred releasing console.");
                return;
            }

            _console_attached = false;
        }

        void Initialize() {
#ifndef NDEBUG
            AttachConsole();
#endif

            g_Logger.SetConsoleAttached(_console_attached);

            if (!Registry::EnableBackupPrivileges()) {
                throw ApplicationError("Failed to enable registry backup privileges.");
            }

            // Initialize COM
            auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
                _WARN("Failed to enable dark mode for title bar.");
            }
        }

        void Shutdown() {
#ifndef NDEBUG
            ReleaseConsole();
#endif
            CoUninitialize();
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
                _WARN("Failed to create default font. Falling back to system font.");
            }

            HMENU hMenubar = CreateMenu();
            HMENU hMenu    = CreateMenu();

            AppendMenu(hMenu, MF_STRING, ID_MENU_VIEW_LOG, "&View Log");
            AppendMenu(hMenu, MF_STRING, ID_MENU_RESCAN_LIBRARIES, "&Rescan Libraries");
            AppendMenu(hMenu, MF_STRING, ID_MENU_COLLECT_BACKUPS, "&Collect Backups and Zip");
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_MENU_ABOUT, "&About");
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_MENU_EXIT, "E&xit");

            AppendMenu(hMenubar, MF_POPUP, (UINT_PTR)hMenu, "&Menu");
            SetMenu(hwnd, hMenubar);

            const HWND label = ::CreateWindow("STATIC",
                                              "Select a library to remove:",
                                              WS_CHILD | WS_VISIBLE | SS_LEFT,
                                              10,
                                              10,
                                              300,
                                              16,
                                              hwnd,
                                              nullptr,
                                              _hInstance,
                                              nullptr);
            ::SendMessage(label, WM_SETFONT, (WPARAM)_font, TRUE);

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
            lvc.mask        = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            char name[]     = "Name";
            char location[] = "Location";
            // Column 1: Name
            lvc.iSubItem = 0;
            lvc.pszText  = name;
            lvc.cx       = 200;
            ListView_InsertColumn(_listView, 0, &lvc);
            // Column 2: ContentDir
            lvc.iSubItem = 1;
            lvc.pszText  = location;
            lvc.cx       = 340;
            ListView_InsertColumn(_listView, 1, &lvc);

            constexpr auto buttonWidth  = 184;
            constexpr auto buttonHeight = 30;
            _removeButton               = ::CreateWindowEx(0,
                                             "BUTTON",
                                             "Remove...",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
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

            _libManager.Scan(_listView);
        }

        void OnRemove() {
            const auto result = Dialog::ShowRemove(_hInstance, _hwnd);
            // if (result == ID_REMOVE_REMOVE) {}
        }

        void OnRemoveSelected() {
            if (_selectedLibrary.empty())
                return;

            Dialog::RemoveSelectedDialogData data;
            data.library          = _libManager.GetLibrary(_selectedLibrary);
            data.backupCacheFiles = false;
            data.removeContentDir = true;

            const auto result = Dialog::ShowRemoveSelected(_hInstance, _hwnd, &data);
            if (result == ID_REMOVE_SELECTED_REMOVE) {
                printf("Removing library:\n  - %s\n  - %s\n  - Backup: %s\n  - Remove Content: %s\n\n",
                       _selectedLibrary.c_str(),
                       data.library.second.c_str(),
                       data.backupCacheFiles ? "True" : "False",
                       data.removeContentDir ? "True" : "False");
            }
        }

        void OnRelocateSelected() {
            const auto result = Dialog::ShowRelocateSelected(_hInstance, _hwnd);
            // if (result == ID_RELOCATE_SELECTED_RELOCATE) {}
        }

        void OnRescanLibraries() {
            const auto response =
              ::MessageBox(_hwnd,
                           "Are you sure you want to clear the current library list and scan again?",
                           "K8Tool",
                           MB_YESNO | MB_ICONQUESTION);
            if (response == IDYES) {
                _libManager.Scan(_listView, true);
            }
        }

        void OnExit() {
            ::PostQuitMessage(0);
        }

        void OnUpdateCheckCompleted(const Update::CheckResult* result) const {
            switch (result->compare) {
                case Update::kResultCurrent: {
                    ::MessageBox(_hwnd, "You're running the latest version!", "Update", MB_OK | MB_ICONINFORMATION);
                    break;
                }

                case Update::kResultOld: {
                    const auto message = std::format("A new version of K8-LRT is available!\n\n"
                                                     "Current: {}\n"
                                                     "Latest: {}\n\n"
                                                     "Visit the GitHub releases page to download?",
                                                     VER_PRODUCTVERSION_STR,
                                                     &result->currentVersion[0] + 1);

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

                    ::MessageBox(_hwnd, message.c_str(), "Update", MB_OK | MB_ICONWARNING);
                    break;
                }
            }
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
                    SetBkMode(hdc, TRANSPARENT);
                    return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
                }

                case WM_NOTIFY: {
                    LPNMHDR lpnmh = (LPNMHDR)lParam;
                    if (lpnmh->idFrom == kIDC_ListView && lpnmh->code == LVN_ITEMCHANGED) {
                        LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
                        if ((pnmv->uChanged & LVIF_STATE) && (pnmv->uNewState & LVIS_SELECTED)) {
                            // Retrieve selected library name
                            _selectedIndex   = pnmv->iItem;
                            char buffer[256] = {'\0'};
                            ListView_GetItemText(lpnmh->hwndFrom, _selectedIndex, 0, buffer, 256);
                            _selectedLibrary = buffer;

                            // Enable Remove/Relocate buttons
                            ::EnableWindow(_removeSelectedButton, !_selectedLibrary.empty());
                            ::EnableWindow(_relocateSelectedButton, !_selectedLibrary.empty());
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
                        _ERROR("Failed to get result from update check.");
                        break;
                    }
                    OnUpdateCheckCompleted(result);
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
    _UNUSED(hPrevInstance);
    _UNUSED(lpCmdLine);

    using namespace K8;
    try {
        return Application {hInstance, "K8Tool - v" VER_FILEVERSION_STR, 600, 420, nCmdShow}.Run();
    } catch (const std::exception& ex) { _FATAL("A fatal error occurred during startup:\n\n%s", ex.what()); }
}