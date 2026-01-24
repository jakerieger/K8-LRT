/*
    K8-LRT - v0.3.1 - Library removal tool for Bobdule's Kontakt 8

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

    REVISION HISTORY

        0.3.0  (2026-01-23) sweeping code changes, bug fixes, and logging
        0.2.0  (2026-01-23) tons of bug fixes and code improvements
        0.1.0  (2026-01-22) initial release of K8-LRT
*/

#include <stdio.h>
#include <stdarg.h>
#include <windows.h>
#include <winerror.h>
#include <winnt.h>
#include <winreg.h>
#include <shlobj.h>
#include <Shlwapi.h>
#include <winuser.h>
#include <CommCtrl.h>

//====================================================================//
//                          -- LOGGING --                             //
//====================================================================//

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
    LOG_DEBUG,
} log_level;

static FILE* LOG_FILE = NULL;

void log_msg(log_level level, const char* fmt, ...) {
    if (!LOG_FILE)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    const char* level_str;
    switch (level) {
        case LOG_INFO:
            level_str = "INFO";
            break;
        case LOG_WARN:
            level_str = "WARN";
            break;
        case LOG_ERROR:
            level_str = "ERROR";
            break;
        case LOG_FATAL:
            level_str = "FATAL";
            break;
        case LOG_DEBUG:
        default:
            level_str = "DEBUG";
            break;
    }

    fprintf(LOG_FILE,
            "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond,
            st.wMilliseconds,
            level_str);

    va_list args;
    va_start(args, fmt);
    vfprintf(LOG_FILE, fmt, args);
    va_end(args);

    fprintf(LOG_FILE, "\n");
    fflush(LOG_FILE);
}

void log_init(HWND hwnd, const char* filename) {
    errno_t result = fopen_s(&LOG_FILE, filename, "a");  // append mode
    if (result == 0) {
        log_msg(LOG_INFO, "--- K8-LRT Started ---");
    } else {
        MessageBox(hwnd, "Failed to initialize logger.", "Fatal", MB_OK | MB_ICONERROR);
        exit(1);
    }
}

void log_close() {
    if (LOG_FILE) {
        log_msg(LOG_INFO, "--- K8-LRT Stopped ---");
        fclose(LOG_FILE);
    }
}

#define _INFO(fmt, ...) log_msg(LOG_INFO, fmt, ##__VA_ARGS__)
#define _WARN(fmt, ...) log_msg(LOG_WARN, fmt, ##__VA_ARGS__)
#define _ERROR(fmt, ...) log_msg(LOG_ERROR, fmt, ##__VA_ARGS__)
#define _FATAL(fmt, ...)                                                                                               \
    do {                                                                                                               \
        log_msg(LOG_FATAL, fmt, ##__VA_ARGS__);                                                                        \
        exit(1);                                                                                                       \
    } while (FALSE)
#define _LOG(fmt, ...) log_msg(LOG_DEBUG, fmt, ##__VA_ARGS__)

//====================================================================//
//                           -- MEMORY --                             //
//====================================================================//

#define _ARENA_BASE (sizeof(mem_arena))
#define _PAGESIZE (sizeof(void*))
#define _ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define _KB(n) ((UINT64)(n) << 10)
#define _MB(n) ((UINT64)(n) << 20)
#define _GB(n) ((UINT64)(n) << 30)

typedef struct {
    UINT64 capacity;
    UINT64 position;
} mem_arena;

static mem_arena* ARENA = NULL;

void arena_init(UINT64 capacity) {
    ARENA = (mem_arena*)malloc(capacity);
    if (!ARENA) {
        _FATAL("Failed to allocate memory");
    }

    ARENA->position = _ARENA_BASE;
    ARENA->capacity = capacity;
}

void arena_destroy() {
    free(ARENA);
}

void* arena_push(UINT64 size, BOOL non_zero) {
    UINT64 pos_aligned = _ALIGN_UP(ARENA->position, _PAGESIZE);
    UINT64 new_pos     = pos_aligned + size;

    if (new_pos > ARENA->capacity) {
        _FATAL("Arena capacity is full");
    }

    ARENA->position = new_pos;
    UINT8* new_mem  = (UINT8*)ARENA + pos_aligned;

    if (!non_zero) {
        memset(new_mem, 0, size);
    }

    return new_mem;
}

void arena_pop(UINT64 size) {
    size = min(size, ARENA->position - _ARENA_BASE);
    ARENA->position -= size;
}

void arena_pop_to(UINT64 position) {
    UINT64 size = position < ARENA->position ? ARENA->position - 1 : 0;
    arena_pop(size);
}

void arena_clear() {
    arena_pop_to(_ARENA_BASE);
}

#define _ALLOC_ARRAY(type, count) (type*)arena_push(sizeof(type) * (count), FALSE)
#define _ALLOC_STR(length) (char*)arena_push(sizeof(char) * (length), FALSE)
#define _ALLOC(type) (type*)arena_push(sizeof(type), FALSE)

//====================================================================//
//                   -- UI ELEMENT DEFINITIONS --                     //
//====================================================================//

#define IDC_LISTBOX 101
#define IDC_REMOVE_BUTTON 102
#define IDC_REMOVE_ALL_BUTTON 103
#define IDC_CHECKBOX_BACKUP 104

#define IDI_APPICON 501

static HWND H_LISTBOX;
static HWND H_REMOVE_BUTTON;
static HWND H_REMOVE_ALL_BUTTON;
static HWND H_BACKUP_CHECKBOX;  // Whether or not we should backup delete files

//====================================================================//
//                          -- GLOBALS --                             //
//====================================================================//

#define _STREQ(s1, s2) strcmp(s1, s2) == 0
#define _IS_CHECKED(checkbox) (SendMessage(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED)
#define _MAX_LIB_COUNT 512
#define _LIB_CACHE_ROOT "Native Instruments\\Kontakt 8\\LibrariesCache"
#define _DB3_ROOT "Native Instruments\\Kontakt 8\\komplete.db3"

static char** LIBRARIES   = {NULL};
static int LIB_COUNT      = 0;
static int SELECTED_INDEX = -1;
static BOOL BACKUP_FILES  = TRUE;

// Exclusion list - NI products that aren't libraries
static char* EXCLUSION_LIST[] = {
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
  NULL,
};

//====================================================================//
//                      -- HELPER FUNCTIONS --                        //
//====================================================================//

char* get_local_appdata_path() {
    PWSTR psz_path = NULL;

    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &psz_path);
    if (SUCCEEDED(hr)) {
        // convert to ansi
        char path[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, psz_path, -1, path, MAX_PATH, NULL, NULL);
        CoTaskMemFree(psz_path);

        char* path_mem = _ALLOC_STR(strlen(path) + 1);
        memcpy(path_mem, path, strlen(path));
        path_mem[strlen(path)] = '\0';

        return path_mem;
    } else {
        _ERROR("Failed to retrieve path. Error code: 0x%08X\n", (UINT)hr);
    }

    return NULL;
}

char* join_str(const char* prefix, const char* suffix) {
    const size_t prefix_len = strlen(prefix);
    const size_t suffix_len = strlen(suffix);
    const size_t needed     = prefix_len + suffix_len + 1;

    char* buffer = _ALLOC_STR(needed);
    int offset   = 0;

    memcpy(buffer, prefix, prefix_len);
    offset += prefix_len;

    memcpy(buffer + offset, suffix, suffix_len);
    offset += suffix_len;

    buffer[offset] = '\0';

    return buffer;
}

char* join_paths(const char* path_prefix, const char* path_suffix) {
    char* prefix = join_str(path_prefix, "\\");
    char* full   = join_str(prefix, path_suffix);
    return full;
}

void attach_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f_dummy;
        freopen_s(&f_dummy, "CONOUT$", "w", stdout);
        freopen_s(&f_dummy, "CONOUT$", "w", stderr);
        _LOG("Attached debug console");
    } else {
        _ERROR("Failed to attach debug console");
    }
}

BOOL file_exists(const char* path) {
    DWORD dw_attrib = GetFileAttributesA(path);
    return (dw_attrib != INVALID_FILE_ATTRIBUTES && !(dw_attrib & FILE_ATTRIBUTE_DIRECTORY));
}

BOOL list_contains(char* haystack[], const char* needle) {
    for (char** p = haystack; *p != NULL; p++) {
        if (_STREQ(*p, needle))
            return TRUE;
    }
    return FALSE;
}

// Enable registry key backups
void enable_backup_privilege() {
    HANDLE h_token;
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &h_token)) {
        if (LookupPrivilegeValue(NULL, SE_BACKUP_NAME, &luid)) {
            tp.PrivilegeCount           = 1;
            tp.Privileges[0].Luid       = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(h_token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
        }
        CloseHandle(h_token);
        _INFO("Enabled registry backup privileges");
    } else {
        _FATAL("Failed to enable registry backup privileges. Are you admin?");
    }
}

void clear_libraries() {
    if (ARENA->position > _ARENA_BASE) {
        arena_clear();
    }

    LIBRARIES = _ALLOC_ARRAY(char*, _MAX_LIB_COUNT);

    for (int i = 0; i < _MAX_LIB_COUNT; i++) {
        if (LIBRARIES[i] != NULL) {
            free(LIBRARIES[i]);
            LIBRARIES[i] = NULL;
        }
    }

    // Reset list box contents
    SendMessage(H_LISTBOX, LB_RESETCONTENT, 0, 0);

    SELECTED_INDEX = -1;
    LIB_COUNT      = 0;
}

BOOL query_libraries() {
    clear_libraries();

    HKEY h_key;
    LPCSTR subkey = "SOFTWARE\\Native Instruments";

    LONG result = RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h_key);
    if (result != ERROR_SUCCESS) {
        _ERROR("Failed to open registry key: '%s'", subkey);
        return FALSE;
    }

    char ach_key[255];
    DWORD cb_name = sizeof(ach_key);
    FILETIME ft_last_write_time;

    DWORD found = 0;
    char* found_keys[_MAX_LIB_COUNT];

    while (RegEnumKeyExA(h_key, found, ach_key, &cb_name, NULL, NULL, NULL, &ft_last_write_time) == ERROR_SUCCESS) {
        if (found > _MAX_LIB_COUNT) {
            RegCloseKey(h_key);
            _ERROR("Number of libraries found exceeds current program limit of %d", _MAX_LIB_COUNT);
            return FALSE;
        }

        found_keys[found] = _strdup(ach_key);
        cb_name           = sizeof(ach_key);
        found++;
    }
    RegCloseKey(h_key);
    found_keys[found] = NULL;

    // Filter found keys
    LIB_COUNT = 0;
    for (DWORD i = 0; i < found; i++) {
        char* key = found_keys[i];

        if (!list_contains(EXCLUSION_LIST, key)) {
            LIBRARIES[LIB_COUNT] = _strdup(key);
            SendMessage(H_LISTBOX, LB_INSERTSTRING, LIB_COUNT, (LPARAM)LIBRARIES[LIB_COUNT]);
            LIB_COUNT++;
        }

        free(key);
        found_keys[i] = NULL;
    }
    LIBRARIES[LIB_COUNT] = NULL;

    _INFO("Finished querying registry entries (found %d library entries)", LIB_COUNT);

    return TRUE;
}

BOOL remove_library(const char* name) {
    if (!list_contains(LIBRARIES, name)) {
        _ERROR("No library named '%s' found", name);
        return FALSE;
    }

    LPCSTR base_path = "SOFTWARE\\Native Instruments";
    HKEY h_key;

    // 1. Remove registry key
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base_path, 0, KEY_ALL_ACCESS, &h_key) == ERROR_SUCCESS) {
        if (BACKUP_FILES) {
            HKEY h_subkey;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, join_paths(base_path, name), 0, KEY_READ, &h_subkey) ==
                ERROR_SUCCESS) {
                char* reg_filename = join_str(name, ".reg");
                DeleteFileA(reg_filename);
                LONG backup_res = RegSaveKeyExA(h_subkey, reg_filename, NULL, REG_LATEST_FORMAT);

                if (backup_res != ERROR_SUCCESS) {
                    _ERROR("Failed to backup registry entry for key: '%s\\%s'", base_path, name);
                    RegCloseKey(h_subkey);
                    return FALSE;
                }

                RegCloseKey(h_subkey);
            }
        }

        LONG res = RegDeleteKeyA(h_key, name);
        if (res != ERROR_SUCCESS) {
            _ERROR("Failed to delete registry key: '%s\\%s'", base_path, name);
            RegCloseKey(h_key);
            return FALSE;
        }

        RegCloseKey(h_key);
        _INFO("Removed registry key: '%s\\%s'", base_path, name);
    }

    // 2. Check for .xml files in `C:\Program Files\Common Files\Native Instruments\Service Center`
    char* prefix   = join_paths("C:\\Program Files\\Common Files\\Native Instruments\\Service Center", name);
    char* filename = join_str(prefix, ".xml");

    if (file_exists(filename)) {
        if (BACKUP_FILES) {
            char* bak_filename = join_str(prefix, ".xml.bak");
            if (!CopyFileExA(filename, bak_filename, NULL, NULL, NULL, 0)) {
                _ERROR("Failed to backup XML file: '%s'", filename);
                return FALSE;
            }
        }

        if (!DeleteFileA(filename)) {
            _ERROR("Failed to delete XML file: '%s'", filename);
            return FALSE;
        }

        _INFO("Deleted XML file: '%s'", filename);
    }

    // 3. Check for cache file in `~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache`
    // This one is tough. Cache files are binary formats and filenames appear to be hashes of some kind.
    // For now we'll just delete all of the cache files because they don't seem to play a significant role
    char* appdata_local = get_local_appdata_path();
    WIN32_FIND_DATA find_data;
    HANDLE h_find = INVALID_HANDLE_VALUE;
    char search_path[MAX_PATH];
    char file_path[MAX_PATH];

    char* cache_path = join_paths(appdata_local, _LIB_CACHE_ROOT);
    snprintf(search_path, MAX_PATH, "%s\\*", cache_path);
    h_find = FindFirstFile(search_path, &find_data);
    if (h_find != INVALID_HANDLE_VALUE) {
        do {
            // Skip the special directory entries '.' and '..'
            if (strcmp(find_data.cFileName, ".") != 0 && strcmp(find_data.cFileName, "..") != 0) {
                snprintf(file_path, MAX_PATH, "%s\\%s", cache_path, find_data.cFileName);

                if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (BACKUP_FILES) {
                        char* bak_filename = join_str(file_path, ".bak");
                        if (file_exists(bak_filename)) {
                            BOOL deleted = DeleteFileA(bak_filename);
                            if (!deleted) {
                                _ERROR("Failed to delete backup file: '%s'", bak_filename);
                            }
                        }
                        BOOL copied = CopyFileExA(file_path, bak_filename, NULL, NULL, NULL, 0);
                        if (!copied) {
                            _ERROR("Failed to backup cache file: '%s'", file_path);
                        }
                    }

                    if (!DeleteFile(file_path)) {
                        _ERROR("Failed to delete cache file: '%s'", file_path);
                    } else {
                        _INFO("Deleted cache file: '%s'", file_path);
                    }
                }
            }
        } while (FindNextFile(h_find, &find_data) != 0);
        FindClose(h_find);
    }

    // 4. Create backup of `~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3` to force DB rebuild (if enabled)
    char* db3 = join_paths(appdata_local, _DB3_ROOT);
    if (file_exists(db3)) {
        if (BACKUP_FILES) {
            char* db3_bak = join_str(db3, ".bak");
            if (!CopyFileExA(db3, db3_bak, NULL, NULL, NULL, 0)) {
                _ERROR("Failed to backup komplete.db3");
                return FALSE;
            }
            _INFO("Backed up komplete.db3");
        }

        if (!DeleteFileA(db3)) {
            _ERROR("Failed to delete komplete.db3");
            return FALSE;
        } else {
            _INFO("Deleted komplete.db3");
        }
    }

    _INFO("Finished removing library: '%s'", name);

    return TRUE;
}

BOOL remove_selected_library() {
    if (SELECTED_INDEX == -1)
        return FALSE;

    return remove_library(LIBRARIES[SELECTED_INDEX]);
}

//====================================================================//
//                      -- INPUT CALLBACKS --                         //
//====================================================================//

LRESULT on_create(HWND hwnd) {
    HWND h_label = CreateWindow("STATIC",
                                "Select a library to remove below:",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                10,
                                5,
                                265,
                                20,
                                hwnd,
                                NULL,
                                GetModuleHandle(NULL),
                                NULL);

    H_LISTBOX = CreateWindowEx(WS_EX_CLIENTEDGE,
                               "LISTBOX",
                               NULL,
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                               10,
                               26,
                               265,
                               240,
                               hwnd,
                               (HMENU)IDC_LISTBOX,
                               GetModuleHandle(NULL),
                               NULL);

    H_BACKUP_CHECKBOX = CreateWindowEx(0,
                                       "BUTTON",
                                       "Backup files before deleting",
                                       WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                       10,
                                       250,
                                       220,
                                       20,
                                       hwnd,
                                       (HMENU)IDC_CHECKBOX_BACKUP,
                                       GetModuleHandle(NULL),
                                       NULL);

    H_REMOVE_ALL_BUTTON = CreateWindowEx(0,
                                         "BUTTON",
                                         "Remove All",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         10,
                                         280,
                                         130,
                                         30,
                                         hwnd,
                                         (HMENU)IDC_REMOVE_ALL_BUTTON,
                                         GetModuleHandle(NULL),
                                         NULL);

    H_REMOVE_BUTTON = CreateWindowEx(0,
                                     "BUTTON",
                                     "Remove Selected",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                     147,
                                     280,
                                     130,
                                     30,
                                     hwnd,
                                     (HMENU)IDC_REMOVE_BUTTON,
                                     GetModuleHandle(NULL),
                                     NULL);

    HFONT h_font = CreateFont(16,
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

    SendMessage(h_label, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_LISTBOX, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_REMOVE_ALL_BUTTON, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_REMOVE_BUTTON, WM_SETFONT, (WPARAM)h_font, TRUE);

    SendMessage(H_BACKUP_CHECKBOX, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_BACKUP_CHECKBOX, BM_SETCHECK, (WPARAM)TRUE, TRUE);

    // Search registry for key entries in `HKEY_LOCAL_MACHINE/SOFTWARE/Native Instruments/..`
    BOOL query_result = query_libraries();
    if (!query_result) {
        MessageBox(hwnd,
                   "Failed to query libraries. Do you have any Kontakt libraries installed?\n\nCheck "
                   "'K8-LRT.log' for details.",
                   "Error",
                   MB_OK | MB_ICONERROR);
        PostQuitMessage(0);
    }

    return 0;
}

void on_selection_changed(HWND hwnd) {
    int sel = (int)SendMessage(H_LISTBOX, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) {
        SELECTED_INDEX = sel;
    }
    EnableWindow(H_REMOVE_BUTTON, (sel != LB_ERR));
}

void on_remove_selected(HWND hwnd) {
    char msg[512];
    sprintf_s(msg, sizeof(msg), "Are you sure you want to permanently remove '%s'?", LIBRARIES[SELECTED_INDEX]);

    int response = MessageBox(hwnd, msg, "Confirm Removal", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

    if (response == IDYES) {
        BACKUP_FILES = _IS_CHECKED(H_BACKUP_CHECKBOX);
        BOOL removed = remove_selected_library();

        if (!removed) {
            MessageBox(hwnd, "Failed to remove library", "Error", MB_OK | MB_ICONERROR);
            return;
        } else {
            BOOL query_result = query_libraries();
            if (query_result) {
                EnableWindow(H_REMOVE_BUTTON, FALSE);
                MessageBox(hwnd, "Successfully removed library.", "Success", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBox(hwnd,
                           "Failed to query libraries. Do you have any Kontakt libraries "
                           "installed?\n\nCheck "
                           "'K8-LRT.log' for details.",
                           "Error",
                           MB_OK | MB_ICONERROR);
                PostQuitMessage(0);
            }
        }
    }
}

void on_remove_all(HWND hwnd) {
    int response = MessageBox(hwnd,
                              "Are you sure you want to permanently remove all libraries?",
                              "Confirm Removal",
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

    if (response == IDYES) {
        BACKUP_FILES   = _IS_CHECKED(H_BACKUP_CHECKBOX);
        SELECTED_INDEX = -1;

        for (int i = 0; i < LIB_COUNT; i++) {
            BOOL removed = remove_library(LIBRARIES[i]);
            if (!removed) {
                _ERROR("Failed to remove library: '%s'", LIBRARIES[SELECTED_INDEX]);
            }
            i++;
        }

        BOOL query_result = query_libraries();
        if (query_result) {
            EnableWindow(H_REMOVE_BUTTON, FALSE);
            MessageBox(hwnd, "Successfully removed all libraries.", "Success", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBox(hwnd,
                       "Failed to query libraries. Do you have any Kontakt libraries "
                       "installed?\n\nCheck "
                       "'K8-LRT.log' for details.",
                       "Error",
                       MB_OK | MB_ICONERROR);
            PostQuitMessage(0);
        }
    }
}

//====================================================================//
//                      -- WINDOW CALLBACK --                         //
//====================================================================//

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch (umsg) {
        case WM_CREATE:
            return on_create(hwnd);

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wparam;
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wparam;
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wparam;
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDC_REMOVE_ALL_BUTTON: {
                    on_remove_all(hwnd);
                    break;
                }

                case IDC_REMOVE_BUTTON: {
                    on_remove_selected(hwnd);
                    break;
                }

                case IDC_LISTBOX: {
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        on_selection_changed(hwnd);
                    }

                    break;
                }
            }

            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hwnd, umsg, wparam, lparam);
}

//====================================================================//
//                         -- ENTRYPOINT --                           //
//====================================================================//

int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE h_prev_instance, LPSTR lp_cmd_line, int n_cmd_show) {
#ifndef NDEBUG
    attach_console();
#endif

    // Initialize common controls
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    arena_init(_KB(32));
    enable_backup_privilege();

    const char CLASS_NAME[] = "K8RemovalWindowClass";

    WNDCLASS wc      = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = h_instance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(h_instance, MAKEINTRESOURCE(IDI_APPICON));

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0,
                               CLASS_NAME,
                               "K8-LRT",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT,
                               CW_USEDEFAULT,
                               300,
                               360,
                               NULL,
                               NULL,
                               h_instance,
                               NULL);

    if (hwnd == NULL) {
        return 1;
    }

    ShowWindow(hwnd, n_cmd_show);
    log_init(hwnd, "K8-LRT.log");

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

#ifndef NDEBUG
    if (FreeConsole()) {
        _LOG("Detached debug console");
    }
#endif

    log_close();
    arena_destroy();

    return 0;
}