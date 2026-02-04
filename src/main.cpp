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

#include <algorithm>
#include <windows.h>
#include <winreg.h>
#include <commctrl.h>
#include <exception>
#include <format>
#include <cassert>
#include <unordered_map>
#include <ranges>
#include <array>

#define _UNUSED(x) (void)(x)

namespace K8 {
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

        void Query(HWND listView) {
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
    };

#pragma endregion

#pragma region Dialogs

    namespace Dialog {
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

                        break;
                }

                return (INT_PTR)FALSE;
            }
        }  // namespace DialogProc

        static void ShowAbout(HINSTANCE hInst, HWND hwnd, int id, const char* version) {
            ::DialogBoxParam(hInst, MAKEINTRESOURCE(id), hwnd, DialogProc::About, (LPARAM)version);
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
        HWND _removeAllButton;
        HWND _removeButton;
        HWND _relocateButton;

        // UI Element IDs
        static constexpr int kIDC_ListView        = 101;
        static constexpr int kIDC_RemoveAllButton = 102;
        static constexpr int kIDC_RemoveButton    = 103;
        static constexpr int kIDC_RelocateButton  = 104;

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

        void OnRemoveAll() {}

        void OnRemove() {}

        void OnRelocate() {}

        LRESULT CALLBACK HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            assert(this);

            switch (msg) {
                case WM_CREATE: {
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
                    AppendMenu(hMenu, MF_STRING, ID_MENU_RELOAD_LIBRARIES, "&Reload Libraries");
                    AppendMenu(hMenu, MF_STRING, ID_MENU_COLLECT_BACKUPS, "&Collect Backups and Zip");
                    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenu(hMenu, MF_STRING, ID_MENU_CHECK_UPDATES, "&Check for Updates");
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
                    ListView_SetExtendedListViewStyle(_listView,
                                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

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
                    _removeAllButton            = ::CreateWindowEx(0,
                                                        "BUTTON",
                                                        "Remove All",
                                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                        10,
                                                        320,
                                                        buttonWidth,
                                                        buttonHeight,
                                                        hwnd,
                                                        (HMENU)kIDC_RemoveAllButton,
                                                        _hInstance,
                                                        nullptr);
                    ::SendMessage(_removeAllButton, WM_SETFONT, (WPARAM)_font, TRUE);

                    _removeButton = ::CreateWindowEx(0,
                                                     "BUTTON",
                                                     "Remove Selected",
                                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                                     (_width / 2 - (buttonWidth / 2)) - 8,
                                                     320,
                                                     buttonWidth,
                                                     buttonHeight,
                                                     hwnd,
                                                     (HMENU)kIDC_RemoveButton,
                                                     _hInstance,
                                                     nullptr);
                    ::SendMessage(_removeButton, WM_SETFONT, (WPARAM)_font, TRUE);

                    _relocateButton = ::CreateWindowEx(0,
                                                       "BUTTON",
                                                       "Relocate Selected",
                                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                                       _width - buttonWidth - 26,
                                                       320,
                                                       buttonWidth,
                                                       buttonHeight,
                                                       hwnd,
                                                       (HMENU)kIDC_RelocateButton,
                                                       _hInstance,
                                                       nullptr);
                    ::SendMessage(_relocateButton, WM_SETFONT, (WPARAM)_font, TRUE);

                    _libManager.Query(_listView);
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
                            ::EnableWindow(_removeButton, !_selectedLibrary.empty());
                            ::EnableWindow(_relocateButton, !_selectedLibrary.empty());
                        }
                    }

                    break;
                }

                case WM_COMMAND: {
                    switch (LOWORD(wParam)) {
                        case kIDC_RemoveAllButton: {
                            OnRemoveAll();
                            break;
                        }

                        case kIDC_RemoveButton: {
                            OnRemove();
                            break;
                        }

                        case kIDC_RelocateButton: {
                            OnRelocate();
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