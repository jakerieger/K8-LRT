/*
    K8-LRT - v2.0.0 - Library removal tool for Bobdule's Kontakt 8

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

        2.0.0  (2026-01-31)  bug fixes for registry querying, relocating libraries,
                             removed support for Windows 7, string pool
                             memory management, wide path support
        1.1.0  (2026-01-26)  additional directory checks and removals, UI additions and changes
        1.0.0  (2026-01-25)  UI redux, added functionality, updates
        0.3.1  (2026-01-23)  memory model improvements
        0.3.0  (2026-01-23)  sweeping code changes, bug fixes, and logging
        0.2.0  (2026-01-23)  tons of bug fixes and code improvements
        0.1.0  (2026-01-22)  initial release of K8-LRT
*/

#include "version.h"

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdarg.h>
#include <windows.h>   // Core Windows API
#include <winerror.h>  // Windows error API
#include <winnt.h>     // Additional core API stuff
#include <winreg.h>    // Registry API
#include <Shlwapi.h>   // Shell API
#include <shlobj.h>    // More Shell API
#include <winuser.h>   // Dialogs and display
#include <commctrl.h>  // For modern Windows styling (Common Controls)
#include <winhttp.h>   // For checking for updates
#include <pathcch.h>   // For long path support and newer file API (Windows 8+)
#include <strsafe.h>   // Window API safer string handling
#include <process.h>   // Threading

//===================================================================//
//                          -- LOGGING --                            //
//===================================================================//
#pragma region logging

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
    LOG_DEBUG,
} log_level;

static BOOL ATTACHED_TO_CONSOLE = FALSE;
static FILE* LOG_FILE           = NULL;

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

    va_list args;
    va_start(args, fmt);
    char body[1024] = {0};
    vsnprintf(body, 1024, fmt, args);
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
             level_str,
             body);

    fprintf(LOG_FILE, "%s", msg);
    fflush(LOG_FILE);

    if (level == LOG_FATAL) {
        char msgbox_msg[2048] = {0};
        snprintf(msgbox_msg, 2048, "A fatal error has occured and K8-LRT must shutdown:\n\n%s", body);
        MessageBoxA(NULL, msgbox_msg, "Fatal Error", MB_OK | MB_ICONERROR);
    }

#ifndef NDEBUG
    if (ATTACHED_TO_CONSOLE) {
        printf("%s", msg);
    }
#endif
}

void log_init(const char* filename) {
    const errno_t result = fopen_s(&LOG_FILE, filename, "a+");  // append (+ read) mode
    if (result == 0) {
        log_msg(LOG_INFO, "--- K8-LRT Started ---");
    } else {
        MessageBox(NULL, "Failed to initialize logger.", "Fatal", MB_OK | MB_ICONERROR);
        exit(1);
    }
}

void log_close(void) {
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
        quick_exit(1);                                                                                                 \
    } while (FALSE)
#define _LOG(fmt, ...) log_msg(LOG_DEBUG, fmt, ##__VA_ARGS__)

#pragma endregion
//===================================================================//
//                           -- MEMORY --                            //
//===================================================================//
#pragma region memory

#define INITIAL_STRPOOL_CAPACITY 16

typedef struct {
    char** strings;
    size_t count;
    size_t capacity;

    wchar_t** wide_strings;
    size_t wide_count;
    size_t wide_capacity;
} strpool;

// Global string pool instance
static strpool STRPOOL;

void strpool_init(void) {
    STRPOOL.strings = malloc(INITIAL_STRPOOL_CAPACITY * sizeof(char*));
    if (!STRPOOL.strings)
        _FATAL("Failed to allocate memory for string pool");
    STRPOOL.count    = 0;
    STRPOOL.capacity = INITIAL_STRPOOL_CAPACITY;

    STRPOOL.wide_strings = malloc(INITIAL_STRPOOL_CAPACITY * sizeof(wchar_t*));
    if (!STRPOOL.wide_strings)
        _FATAL("Failed to allocate memory for string pool");
    STRPOOL.wide_count    = 0;
    STRPOOL.wide_capacity = INITIAL_STRPOOL_CAPACITY;
}

char* strpool_strdup(const char* str) {
    if (!str)
        return NULL;

    char* copy = _strdup(str);
    if (!copy)
        return NULL;

    if (STRPOOL.count >= STRPOOL.capacity) {
        const size_t new_cap = STRPOOL.capacity * 2;
        char** new_strs      = realloc(STRPOOL.strings, new_cap * sizeof(char*));
        if (!new_strs) {
            free(copy);
            return NULL;
        }
        STRPOOL.strings  = new_strs;
        STRPOOL.capacity = new_cap;
    }

    STRPOOL.strings[STRPOOL.count++] = copy;
    return copy;
}

wchar_t* strpool_wstrdup(const wchar_t* str) {
    if (!str)
        return NULL;

    wchar_t* copy = _wcsdup(str);
    if (!copy)
        return NULL;

    if (STRPOOL.wide_count >= STRPOOL.wide_capacity) {
        const size_t new_cap = STRPOOL.wide_capacity * 2;
        wchar_t** new_strs   = realloc(STRPOOL.wide_strings, new_cap * sizeof(wchar_t*));
        if (!new_strs) {
            free(copy);
            return NULL;
        }
        STRPOOL.wide_strings  = new_strs;
        STRPOOL.wide_capacity = new_cap;
    }

    STRPOOL.wide_strings[STRPOOL.wide_count++] = copy;
    return copy;
}

char* strpool_sprintf(const char* fmt, ...) {
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    const int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (size < 0) {
        va_end(args_copy);
        return NULL;
    }

    char* str = malloc(size + 1);
    if (!str) {
        va_end(args_copy);
        return NULL;
    }

    vsnprintf(str, size + 1, fmt, args_copy);
    va_end(args_copy);

    if (STRPOOL.count >= STRPOOL.capacity) {
        const size_t new_capacity = STRPOOL.capacity * 2;
        char** new_strings        = realloc(STRPOOL.strings, new_capacity * sizeof(char*));
        if (!new_strings) {
            free(str);
            return NULL;
        }
        STRPOOL.strings  = new_strings;
        STRPOOL.capacity = new_capacity;
    }

    STRPOOL.strings[STRPOOL.count++] = str;
    return str;
}

wchar_t* strpool_wsprintf(const wchar_t* fmt, ...) {
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    const int size = _vscwprintf(fmt, args);
    va_end(args);

    if (size < 0) {
        va_end(args_copy);
        return NULL;
    }

    wchar_t* str = malloc((size + 1) * sizeof(wchar_t));
    if (!str) {
        va_end(args_copy);
        return NULL;
    }

    vswprintf(str, size + 1, fmt, args_copy);
    va_end(args_copy);

    if (STRPOOL.wide_count >= STRPOOL.wide_capacity) {
        const size_t new_capacity = STRPOOL.wide_capacity * 2;
        wchar_t** new_strings     = realloc(STRPOOL.wide_strings, new_capacity * sizeof(wchar_t*));
        if (!new_strings) {
            free(str);
            return NULL;
        }
        STRPOOL.wide_strings  = new_strings;
        STRPOOL.wide_capacity = new_capacity;
    }

    STRPOOL.wide_strings[STRPOOL.wide_count++] = str;
    return str;
}

char* strpool_alloc(size_t count) {
    char* buffer = malloc(count);
    if (!buffer)
        return NULL;

    if (STRPOOL.count >= STRPOOL.capacity) {
        const size_t new_capacity = STRPOOL.capacity * 2;
        char** new_strings        = realloc(STRPOOL.strings, new_capacity * sizeof(char*));
        if (!new_strings) {
            free(buffer);
            return NULL;
        }
        STRPOOL.strings  = new_strings;
        STRPOOL.capacity = new_capacity;
    }

    STRPOOL.strings[STRPOOL.count++] = buffer;
    return buffer;
}

wchar_t* strpool_walloc(size_t count) {
    wchar_t* buffer = malloc(count * sizeof(wchar_t));
    if (!buffer)
        return NULL;

    if (STRPOOL.wide_count >= STRPOOL.wide_capacity) {
        const size_t new_capacity = STRPOOL.wide_capacity * 2;
        wchar_t** new_strings     = realloc(STRPOOL.wide_strings, new_capacity * sizeof(wchar_t*));
        if (!new_strings) {
            free(buffer);
            return NULL;
        }
        STRPOOL.wide_strings  = new_strings;
        STRPOOL.wide_capacity = new_capacity;
    }

    STRPOOL.wide_strings[STRPOOL.wide_count++] = buffer;
    return buffer;
}

void strpool_destroy(void) {
    for (size_t i = 0; i < STRPOOL.count; i++)
        free(STRPOOL.strings[i]);
    free(STRPOOL.strings);

    for (size_t i = 0; i < STRPOOL.wide_count; i++)
        free(STRPOOL.wide_strings[i]);
    free(STRPOOL.wide_strings);
}

void strpool_reset(void) {
    strpool_destroy();
    strpool_init();
}

#pragma endregion
//===================================================================//
//                           -- STATE --                             //
//===================================================================//
#pragma region state

#define WM_IO_PROGRESS (WM_USER + 100)
#define WM_IO_COMPLETE (WM_USER + 101)
#define WM_IO_ERROR (WM_USER + 102)

typedef enum {
    IO_STATE_IDLE = 0,
    IO_STATE_REMOVING,
    IO_STATE_COPYING,
    IO_STATE_RELOCATING,
} io_operation_state;

typedef struct {
    io_operation_state state;
    CRITICAL_SECTION cs;
    HWND main_window;
    HANDLE thread_handle;
    volatile BOOL cancel_requested;
} io_state_manager;

static io_state_manager IO_STATE = {0};

void io_state_init(HWND hwnd) {
    IO_STATE.state         = IO_STATE_IDLE;
    IO_STATE.main_window   = hwnd;
    IO_STATE.thread_handle = NULL;
    InitializeCriticalSection(&IO_STATE.cs);
}

void io_state_cleanup(void) {
    DeleteCriticalSection(&IO_STATE.cs);
}

BOOL io_state_is_busy(void) {
    // ReSharper disable once CppJoinDeclarationAndAssignment
    BOOL busy;
    EnterCriticalSection(&IO_STATE.cs);
    busy = (IO_STATE.state != IO_STATE_IDLE);
    LeaveCriticalSection(&IO_STATE.cs);
    return busy;
}

BOOL io_state_try_begin(io_operation_state new_state) {
    BOOL success = FALSE;
    EnterCriticalSection(&IO_STATE.cs);
    if (IO_STATE.state == IO_STATE_IDLE) {
        IO_STATE.state            = new_state;
        IO_STATE.cancel_requested = FALSE;
        success                   = TRUE;
    }
    LeaveCriticalSection(&IO_STATE.cs);
    return success;
}

void io_state_end(void) {
    EnterCriticalSection(&IO_STATE.cs);
    IO_STATE.state = IO_STATE_IDLE;
    if (IO_STATE.thread_handle) {
        CloseHandle(IO_STATE.thread_handle);
        IO_STATE.thread_handle = NULL;
    }
    LeaveCriticalSection(&IO_STATE.cs);
}

void io_state_set_thread(HANDLE thread) {
    EnterCriticalSection(&IO_STATE.cs);
    IO_STATE.thread_handle = thread;
    LeaveCriticalSection(&IO_STATE.cs);
}

BOOL io_state_should_cancel(void) {
    BOOL cancel;
    EnterCriticalSection(&IO_STATE.cs);
    cancel = IO_STATE.cancel_requested;
    LeaveCriticalSection(&IO_STATE.cs);
    return cancel;
}

void io_state_request_cancel(void) {
    EnterCriticalSection(&IO_STATE.cs);
    IO_STATE.cancel_requested = TRUE;
    LeaveCriticalSection(&IO_STATE.cs);
}

#pragma endregion
//===================================================================//
//                   -- UI ELEMENT DEFINITIONS --                    //
//===================================================================//
#pragma region ui element definitions

#include "resource.h"

static HWND H_LISTBOX                    = NULL;
static HWND H_REMOVE_BUTTON              = NULL;
static HWND H_REMOVE_ALL_BUTTON          = NULL;
static HWND H_BACKUP_CHECKBOX            = NULL;
static HWND H_LOG_VIEWER                 = NULL;
static HWND H_REMOVE_LIB_FOLDER_CHECKBOX = NULL;
static HWND H_SELECT_LIB_LABEL           = NULL;
static HWND H_RELOCATE_BUTTON            = NULL;
static HWND H_PROGRESS_BAR               = NULL;
static HWND H_PROGRESS_TEXT              = NULL;
static HWND H_PROGRESS_PANEL             = NULL;
static HWND H_PROGRESS_CANCEL_BUTTON     = NULL;

static HFONT UI_FONT = NULL;

#pragma endregion
//===================================================================//
//                          -- GLOBALS --                            //
//===================================================================//
#pragma region globals

#define _WINDOW_W 300
#define _WINDOW_H 440
#define _WINDOW_CLASS "K8LRT_WindowClass\0"
#define _WINDOW_TITLE "K8-LRT - v" VER_PRODUCTVERSION_STR

#define _LOGVIEW_W 600
#define _LOGVIEW_H 400
#define _LOGVIEW_CLASS "K8LRT_LogView\0"
#define _LOGVIEW_TITLE "Log\0"

#define _STREQ(s1, s2) strcmp(s1, s2) == 0
#define _IS_CHECKED(checkbox) (IsDlgButtonChecked(hwnd, checkbox) == BST_CHECKED)
#define _MAX_LIB_COUNT 512
#define _MAX_KEY_LENGTH 255
#define _MAX_PATH_NFTS 32768

#define _LIB_CACHE_ROOT "Native Instruments\\Kontakt 8\\LibrariesCache\0"
#define _DB3_ROOT "Native Instruments\\Kontakt 8\\komplete.db3\0"
#define _RAS3_ROOT "C:\\Users\\Public\\Documents\\Native Instruments\\Native Access\\ras3\0"

typedef struct library_entry library_entry;

typedef struct {
    library_entry* library;
    BOOL backup_files;
    BOOL remove_content_dir;
} remove_lib_dialog_data;

typedef struct {
    library_entry* libraries;
    int lib_count;
    BOOL* selected;
    BOOL backup_files;
    BOOL remove_library_folder;
    int selected_count;
} batch_removal_dialog_data;

typedef struct {
    library_entry* library;
    char new_path[MAX_PATH];
} relocate_lib_dialog_data;

struct library_entry {
    // Library name in registry (this is always the general name used throughout NI's systems)
    const char* name;
    // Actual location of library on disk
    const char* content_dir;
};

static library_entry LIBRARIES[_MAX_LIB_COUNT];
static int LIB_COUNT      = 0;
static int SELECTED_INDEX = -1;

// Whether this is the first time querying for libraries
static BOOL INITIAL_SEARCH     = TRUE;
static BOOL BACKUP_FILES       = TRUE;
static BOOL REMOVE_CONTENT_DIR = TRUE;

// Exclusion list - NI products that aren't libraries
#define KEY_EXCLUSION_LIST_SIZE 42
static char* KEY_EXCLUSION_LIST[KEY_EXCLUSION_LIST_SIZE] = {
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

// TODO: Update these as more are found
#define KEY_EXCLUSION_PATTERNS_SIZE 3
static char* KEY_EXCLUSION_PATTERNS[KEY_EXCLUSION_PATTERNS_SIZE] = {
  "Universal Audio*",
  "u-he*",
  "Waves*",
};

#pragma endregion
//===================================================================//
//                     -- PROGRESS REPORTING --                      //
//===================================================================//
#pragma region progress reporting

typedef struct {
    DWORD last_update_time;
    int last_update_percent;
} progress_throttle_state;

void throttle_init(progress_throttle_state* state) {
    state->last_update_time    = GetTickCount();
    state->last_update_percent = -1;
}

BOOL throttle_should_update(progress_throttle_state* state, int current, int total) {
    const DWORD now        = GetTickCount();
    const DWORD elapsed_ms = now - state->last_update_time;
    const int percent      = total > 0 ? (int)((current * 100.0) / total) : 0;
    // Update if:
    // 1. At least 50ms has passed AND percentage changed, OR
    // 2. At least 200ms has passed (even if percentage didn't change)
    const BOOL time_threshold = (elapsed_ms >= 50 && percent != state->last_update_percent) || (elapsed_ms >= 200);
    if (time_threshold) {
        state->last_update_time    = now;
        state->last_update_percent = percent;
        return TRUE;
    }

    return FALSE;
}

typedef struct {
    int current;
    int total;
    wchar_t message[256];
} progress_data;

void report_progress(HWND hwnd, int current, int total, const wchar_t* msg) {
    progress_data* data = (progress_data*)malloc(sizeof(progress_data));
    if (data) {
        data->current = current;
        data->total   = total;
        if (msg) {
            wcsncpy(data->message, msg, 255);
            data->message[255] = L'\0';
        } else {
            data->message[0] = L'\0';
        }
        PostMessage(hwnd, WM_IO_PROGRESS, 0, (LPARAM)data);
    }
}

void report_complete(HWND hwnd, BOOL success, const wchar_t* msg) {
    wchar_t* message = NULL;
    if (msg) {
        message = _wcsdup(msg);
    }
    PostMessage(hwnd, WM_IO_COMPLETE, (WPARAM)success, (LPARAM)msg);
}

void report_error(HWND hwnd, const wchar_t* msg) {
    wchar_t* message = NULL;
    if (msg) {
        message = _wcsdup(msg);
    }
    PostMessage(hwnd, WM_IO_ERROR, 0, (LPARAM)msg);
}

#pragma endregion
//===================================================================//
//                      -- HELPER FUNCTIONS --                       //
//===================================================================//
#pragma region helper functions

#ifndef NDEBUG
    #define _ASSERT(condition)                                                                                         \
        if (!(condition)) {                                                                                            \
            _ERROR("Debug assertion failed: %s:%d", __FILE__, __LINE__);                                               \
            __debugbreak();                                                                                            \
        }

    #define _NOT_IMPLEMENTED()                                                                                         \
        do {                                                                                                           \
            fprintf(stderr, "NOT IMPLEMENTED");                                                                        \
            quick_exit(1);                                                                                             \
        } while (FALSE)
#else
    #define _ASSERT(condition)                                                                                         \
        if (!(condition)) {                                                                                            \
            _ERROR("Debug assertion failed: %s:%d", __FILE__, __LINE__);                                               \
        }

    #define _NOT_IMPLEMENTED()
#endif

wchar_t* make_long_path(const char* path) {
    if (!path)
        return NULL;

    const int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (needed == 0)
        return NULL;

    // Allocate buffer: "\\?\" + path + null
    const size_t buffer_size = needed + 10;  // Extra space for prefix and safety
    wchar_t* temp            = (wchar_t*)malloc(sizeof(wchar_t) * buffer_size);
    if (!temp)
        return NULL;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, temp, needed);

    // Check if already has \\?\ prefix
    if (wcsncmp(temp, L"\\\\?\\", 4) == 0) {
        return temp;
    }

    // Check if it's a UNC path (\\server\share)
    if (wcsncmp(temp, L"\\\\", 2) == 0) {
        // UNC path: convert to \\?\UNC\server\share
        wchar_t* result = strpool_walloc(buffer_size + 10);
        if (!result) {
            free(temp);
            return NULL;
        }
        StringCchPrintfW(result, buffer_size + 10, L"\\\\?\\UNC\\%s", temp + 2);
        free(temp);
        return result;
    }

    // Regular path: add \\?\ prefix
    wchar_t* result = strpool_walloc(buffer_size);
    if (!result) {
        free(temp);
        return NULL;
    }
    StringCchPrintfW(result, buffer_size, L"\\\\?\\%s", temp);
    free(temp);
    return result;
}

char* join_str(const char* prefix, const char* suffix) {
    return strpool_sprintf("%s%s", prefix, suffix);
}

char* join_paths(const char* base, const char* tail) {
    return strpool_sprintf("%s\\%s", base, tail);
}

wchar_t* join_paths_wide(const wchar_t* base, const wchar_t* tail) {
    if (!base || !tail)
        return NULL;

    const size_t base_len   = wcslen(base);
    const size_t append_len = wcslen(tail);
    const size_t total      = base_len + append_len + 2;  // +1 for backslash, +1 for null

    wchar_t* result = strpool_walloc(total);
    if (!result)
        return NULL;

    wcscpy_s(result, total, base);

    // Add backslash if needed
    if (base_len > 0 && result[base_len - 1] != L'\\') {
        wcscat_s(result, total, L"\\");
    }

    wcscat_s(result, total, tail);
    return result;
}

char* get_local_appdata_path(void) {
    PWSTR psz_path = NULL;

    const HRESULT hr = SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &psz_path);
    if (SUCCEEDED(hr)) {
        char path[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, psz_path, -1, path, MAX_PATH, NULL, NULL);
        CoTaskMemFree(psz_path);

        return strpool_strdup(path);
    } else {
        _ERROR("Failed to retrieve path. Error code: 0x%08X\n", (UINT)hr);
    }

    return NULL;
}

void attach_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f_dummy;
        freopen_s(&f_dummy, "CONOUT$", "w", stdout);
        freopen_s(&f_dummy, "CONOUT$", "w", stderr);
        ATTACHED_TO_CONSOLE = TRUE;
        _LOG("Attached debug console");
    } else {
        ATTACHED_TO_CONSOLE = FALSE;
        _WARN("Failed to attach debug console (process must be executed from within a shell)");
    }
}

BOOL file_exists(const char* path) {
    const DWORD dw_attrib = GetFileAttributesA(path);
    return (dw_attrib != INVALID_FILE_ATTRIBUTES && !(dw_attrib & FILE_ATTRIBUTE_DIRECTORY));
}

BOOL directory_exists(const char* path) {
    const DWORD dw_attrib = GetFileAttributesA(path);
    return (dw_attrib != INVALID_FILE_ATTRIBUTES && (dw_attrib & FILE_ATTRIBUTE_DIRECTORY));
}

BOOL has_extension(const char* filename, const char* ext) {
    const char* extension = strrchr(filename, '.');
    if (!extension)
        return FALSE;
    if (_STREQ(extension, ext))
        return TRUE;
    return FALSE;
}

BOOL rm_rf_recursive(const wchar_t* path) {
    wchar_t* search_path = join_paths_wide(path, L"*");
    if (!search_path)
        return FALSE;

    WIN32_FIND_DATAW find_data;
    const HANDLE h_find = FindFirstFileW(search_path, &find_data);

    if (h_find == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    BOOL success = TRUE;
    do {
        if (wcscmp(find_data.cFileName, L".") == 0 || wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }

        wchar_t* full_path = join_paths_wide(path, find_data.cFileName);
        if (!full_path) {
            success = FALSE;
            break;
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!rm_rf_recursive(full_path)) {
                success = FALSE;
            }
        } else {
            SetFileAttributesW(full_path, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full_path)) {
                _ERROR("Failed to delete file: %ls (Error: %lu)", full_path, GetLastError());
                success = FALSE;
            }
        }

        if (!success)
            break;

    } while (FindNextFileW(h_find, &find_data));

    FindClose(h_find);

    if (success) {
        if (!RemoveDirectoryW(path)) {
            _ERROR("Failed to remove directory: %ls (Error: %lu)", path, GetLastError());
            success = FALSE;
        }
    }

    return success;
}

BOOL rm_rf(const wchar_t* directory) {
    if (!directory)
        return FALSE;
    return rm_rf_recursive(directory);
}

BOOL copy_directory_recursive(const wchar_t* src, const wchar_t* dst) {
    if (!CreateDirectoryW(dst, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            _ERROR("Failed to create directory: %ls (Error: %lu)", dst, GetLastError());
            return FALSE;
        }
    }

    wchar_t* search_path = join_paths_wide(src, L"*");
    if (!search_path)
        return FALSE;

    WIN32_FIND_DATAW find_data;
    const HANDLE h_find = FindFirstFileW(search_path, &find_data);

    if (h_find == INVALID_HANDLE_VALUE) {
        _ERROR("FindFirstFileW failed for: %ls (Error: %lu)", src, GetLastError());
        return FALSE;
    }

    BOOL success = TRUE;
    do {
        // Skip . and ..
        if (wcscmp(find_data.cFileName, L".") == 0 || wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }

        wchar_t* src_path = join_paths_wide(src, find_data.cFileName);
        wchar_t* dst_path = join_paths_wide(dst, find_data.cFileName);

        if (!src_path || !dst_path) {
            success = FALSE;
            break;
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!copy_directory_recursive(src_path, dst_path)) {
                _ERROR("Failed to copy subdirectory: %ls", src_path);
                success = FALSE;
            }
        } else {
            COPYFILE2_EXTENDED_PARAMETERS params = {0};
            params.dwSize                        = sizeof(params);
            params.dwCopyFlags                   = COPY_FILE_NO_BUFFERING;

            const HRESULT hr = CopyFile2(src_path, dst_path, &params);
            if (FAILED(hr)) {
                _ERROR("Failed to copy file: %ls to %ls (HRESULT: 0x%08X)", src_path, dst_path, hr);
                success = FALSE;
            }
        }

        if (!success)
            break;

    } while (FindNextFileW(h_find, &find_data));

    FindClose(h_find);
    return success;
}

BOOL copy_directory(const char* src, const char* dst) {
    wchar_t* src_w = make_long_path(src);
    wchar_t* dst_w = make_long_path(dst);
    if (!src_w || !dst_w)
        return FALSE;
    return copy_directory_recursive(src_w, dst_w);
}

BOOL list_contains(char* haystack[], const int haystack_size, const char* needle) {
    _ASSERT(haystack_size > 0);

    for (int i = 0; i < haystack_size; i++) {
        const char* str = haystack[i];
        if (_STREQ(str, needle))
            return TRUE;
    }

    return FALSE;
}

BOOL matches_pattern_in_list(char* patterns[], const int patterns_count, const char* needle) {
    _ASSERT(patterns_count > 0);

    for (int i = 0; i < patterns_count; i++) {
        const char* str = patterns[i];
        if (PathMatchSpec(needle, str))
            return TRUE;
    }

    return FALSE;
}

// Enable registry key backups
void enable_backup_privilege(void) {
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

void clear_libraries(void) {
    strpool_reset();

    for (int i = 0; i < LIB_COUNT; i++) {
        LIBRARIES[i].name        = NULL;
        LIBRARIES[i].content_dir = NULL;
    }

    // Reset list box contents
    SendMessage(H_LISTBOX, LB_RESETCONTENT, 0, 0);

    SELECTED_INDEX = -1;
    LIB_COUNT      = 0;
}

BOOL open_registry_key(HKEY* key, HKEY base, const char* path, UINT sam) {
    const LONG result = RegOpenKeyExA(base, path, 0, (REGSAM)sam, key);
    if (result != ERROR_SUCCESS) {
        _ERROR("Failed to open registry key: '%s'", path);
        return FALSE;
    }

    return TRUE;
}

void close_registry_key(HKEY* key) {
    RegCloseKey(*key);
}

BOOL delete_registry_key(HKEY base_key, const char* key) {
    const LONG result = RegDeleteKeyA(base_key, key);
    if (result != ERROR_SUCCESS) {}

    return TRUE;
}

int enumerate_registry_keys(HKEY key, char* keys[]) {
    char current_key[_MAX_KEY_LENGTH];
    DWORD buffer_size = sizeof(current_key);
    FILETIME ft_last;
    DWORD index = 0;

    while (RegEnumKeyExA(key, index, current_key, &buffer_size, NULL, NULL, NULL, &ft_last) == ERROR_SUCCESS) {
        _INFO("Found registry entry: '%s'", current_key);
        buffer_size   = sizeof(current_key);
        keys[index++] = strpool_strdup(current_key);
    }

    return index;
}

const char* get_registry_value_str(HKEY key, const char* value_name) {
    char value[MAX_PATH];
    DWORD buffer_size = sizeof(value);
    DWORD value_type;

    const LSTATUS status = RegQueryValueExA(key, value_name, NULL, &value_type, (LPBYTE)value, &buffer_size);
    if (status == ERROR_SUCCESS && value_type == REG_SZ) {
        return strpool_strdup(value);
    }

    return NULL;
}

BOOL set_registry_value_str(HKEY key, const char* value_name, const char* new_value) {
    const LSTATUS status =
      RegSetValueExA(key, value_name, 0, REG_SZ, (const BYTE*)new_value, (DWORD)(strlen(new_value) + 1));
    return (status == ERROR_SUCCESS);
}

BOOL query_libraries(HWND hwnd) {
    clear_libraries();

    HKEY base_key;
    LPCSTR base_path = "SOFTWARE\\Native Instruments";
    BOOL open_result = open_registry_key(&base_key, HKEY_LOCAL_MACHINE, base_path, KEY_READ);
    if (!open_result)
        return FALSE;

    char* keys[_MAX_LIB_COUNT] = {0};
    const int key_count        = enumerate_registry_keys(base_key, keys);

    close_registry_key(&base_key);

    for (int i = 0; i < key_count; i++) {
        if (i >= _MAX_LIB_COUNT)
            break;

        const char* key_str = keys[i];

        const BOOL in_exclusion_list = list_contains(KEY_EXCLUSION_LIST, KEY_EXCLUSION_LIST_SIZE, key_str);
        const BOOL matches_exclusion_pattern =
          matches_pattern_in_list(KEY_EXCLUSION_PATTERNS, KEY_EXCLUSION_PATTERNS_SIZE, key_str);
        if (in_exclusion_list || matches_exclusion_pattern)
            continue;

        HKEY key;
        open_result = open_registry_key(&key, HKEY_LOCAL_MACHINE, join_paths(base_path, key_str), KEY_READ);
        if (!open_result)
            continue;

        library_entry entry     = {.name = key_str, NULL};
        const char* content_dir = get_registry_value_str(key, "ContentDir");

        if (content_dir != NULL) {
            entry.content_dir = content_dir;
        } else {
            _WARN("Failed to retrieve ContentDir value for registry key: 'HKEY_LOCAL_MACHINE\\%s\\%s'",
                  base_path,
                  key_str);
        }

        memcpy(&LIBRARIES[LIB_COUNT], &entry, sizeof(entry));
        SendMessage(H_LISTBOX, LB_INSERTSTRING, LIB_COUNT, (LPARAM)LIBRARIES[LIB_COUNT].name);

        _INFO("Found library entry: '%s (%s)'", LIBRARIES[LIB_COUNT].name, LIBRARIES[LIB_COUNT].content_dir);

        LIB_COUNT++;
        close_registry_key(&key);
    }

    _INFO("Finished querying registry entries (found %d library entries)", LIB_COUNT);

    if (INITIAL_SEARCH) {
        INITIAL_SEARCH = FALSE;
    } else {
        MessageBox(hwnd,
                   strpool_sprintf("Found %d installed libraries", LIB_COUNT),
                   "K8-LRT",
                   MB_OK | MB_ICONINFORMATION);
    }

    return TRUE;
}

BOOL remove_registry_keys(const char* key) {
    LPCSTR base_path = "SOFTWARE\\Native Instruments";
    HKEY h_key;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base_path, 0, KEY_ALL_ACCESS, &h_key) == ERROR_SUCCESS) {
        if (BACKUP_FILES) {
            HKEY h_subkey;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, join_paths(base_path, key), 0, KEY_READ, &h_subkey) ==
                ERROR_SUCCESS) {
                const char* reg_filename = join_str(key, ".reg");
                DeleteFileA(reg_filename);
                const LONG backup_res = RegSaveKeyExA(h_subkey, reg_filename, NULL, REG_LATEST_FORMAT);

                if (backup_res != ERROR_SUCCESS) {
                    _ERROR("Failed to backup registry entry for key: 'HKEY_LOCAL_MACHINE\\%s\\%s'", base_path, key);
                    RegCloseKey(h_subkey);
                    return FALSE;
                }

                RegCloseKey(h_subkey);
            }
        }

        const LONG res = RegDeleteKeyA(h_key, key);
        if (res != ERROR_SUCCESS) {
            _ERROR("Failed to delete registry key: 'HKEY_LOCAL_MACHINE\\%s\\%s'", base_path, key);
            RegCloseKey(h_key);
            return FALSE;
        }

        RegCloseKey(h_key);
        _INFO("Removed registry key: 'HKEY_LOCAL_MACHINE\\%s\\%s'", base_path, key);
    }

    return TRUE;
}

BOOL remove_xml_file(const char* name) {
    char* filename =
      strpool_sprintf("C:\\Program Files\\Common Files\\Native Instruments\\Service Center\\%s.xml", name);

    if (file_exists(filename)) {
        if (BACKUP_FILES) {
            const char* bak_filename = strpool_sprintf("%s.bak", filename);
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

    return TRUE;
}

BOOL remove_all_files_in_dir(const char* directory, BOOL backup) {
    WIN32_FIND_DATA find_data;
    HANDLE h_find = INVALID_HANDLE_VALUE;
    char search_path[MAX_PATH];
    char file_path[MAX_PATH];

    snprintf(search_path, MAX_PATH, "%s\\*", directory);
    h_find = FindFirstFile(search_path, &find_data);
    if (h_find != INVALID_HANDLE_VALUE) {
        do {
            // Skip the special directory entries '.' and '..'
            if (strcmp(find_data.cFileName, ".") != 0 && strcmp(find_data.cFileName, "..") != 0) {
                snprintf(file_path, MAX_PATH, "%s\\%s", directory, find_data.cFileName);

                if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (backup) {
                        char* bak_filename = join_str(file_path, ".bak");
                        if (file_exists(bak_filename)) {
                            const BOOL deleted = DeleteFileA(bak_filename);
                            if (!deleted) {
                                _ERROR("Failed to delete file: '%s'", bak_filename);
                                return FALSE;
                            }
                        }
                        const BOOL copied = CopyFileExA(file_path, bak_filename, NULL, NULL, NULL, 0);
                        if (!copied) {
                            _ERROR("Failed to backup file: '%s'", file_path);
                            return FALSE;
                        }
                    }

                    if (!DeleteFile(file_path)) {
                        _ERROR("Failed to delete file: '%s'", file_path);
                        return FALSE;
                    } else {
                        _INFO("Deleted file: '%s'", file_path);
                    }
                }
            }
        } while (FindNextFile(h_find, &find_data) != 0);
        FindClose(h_find);
    }

    return TRUE;
}

BOOL remove_cache_files(void) {
    const char* cache_path = join_paths(get_local_appdata_path(), _LIB_CACHE_ROOT);
    return remove_all_files_in_dir(cache_path, BACKUP_FILES);
}

BOOL remove_db3(void) {
    const char* appdata_local = get_local_appdata_path();
    const char* db3           = join_paths(appdata_local, _DB3_ROOT);
    if (file_exists(db3)) {
        if (BACKUP_FILES) {
            const char* db3_bak = join_str(db3, ".bak");
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

    return TRUE;
}

BOOL remove_ras3_jwt(void) {
    return remove_all_files_in_dir(_RAS3_ROOT, BACKUP_FILES);
}

BOOL remove_library(const library_entry* library, BOOL remove_content) {
    _ASSERT(library != NULL);
    _ASSERT(library->name != NULL);
    _ASSERT(library->content_dir != NULL);

    for (int i = 0; i < LIB_COUNT; i++) {
        const library_entry* entry = &LIBRARIES[i];
        if (_STREQ(entry->name, library->name)) {
            return FALSE;
        }
    }

    BOOL result = remove_registry_keys(library->name);
    if (!result)
        return FALSE;

    result = remove_xml_file(library->name);
    if (!result)
        return FALSE;

    result = remove_cache_files();
    if (!result)
        return FALSE;

    result = remove_db3();
    if (!result)
        return FALSE;

    result = remove_ras3_jwt();
    if (!result)
        return FALSE;

    if (remove_content && library->content_dir != NULL && directory_exists(library->content_dir)) {
        result = rm_rf(make_long_path(library->content_dir));
        if (!result)
            return FALSE;
    }

    _INFO("Finished removing library: '%s'", library->name);

    return TRUE;
}

BOOL remove_selected_library(void) {
    if (SELECTED_INDEX == -1)
        return FALSE;

    return remove_library(&LIBRARIES[SELECTED_INDEX], REMOVE_CONTENT_DIR);
}

// Extract tag name from GitHub json response
char* extract_tag_name(const char* json) {
    const char* tag_start = strstr(json, "\"tag_name\"");
    if (!tag_start)
        return NULL;

    const char* value_start = strchr(tag_start, ':');
    if (!value_start)
        return NULL;

    value_start = strchr(value_start, '"');
    if (!value_start)
        return NULL;
    value_start++;

    const char* value_end = strchr(value_start, '"');
    if (!value_end)
        return NULL;

    const size_t len = value_end - value_start;
    char* tag        = strpool_alloc(len + 1);
    strncpy_s(tag, len + 1, value_start, len);
    tag[len] = '\0';

    return tag;
}

// Compares version numbers (strips 'v' prefix if present).
// Returns 0 if versions match
int compare_versions(const char* v1, const char* v2) {
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    // Skip 'v' prefix if present
    if (v1[0] == 'v')
        v1++;
    if (v2[0] == 'v')
        v2++;

    sscanf_s(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf_s(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2)
        return major1 - major2;
    if (minor1 != minor2)
        return minor1 - minor2;
    return patch1 - patch2;
}

// Fetch the latest version of K8-LRT from the GitHub API
char* get_latest_version(void) {
    HINTERNET h_session   = NULL;
    HINTERNET h_connect   = NULL;
    HINTERNET h_request   = NULL;
    char* latest_version  = NULL;
    char* response_data   = NULL;
    DWORD bytes_available = 0;
    DWORD bytes_read      = 0;
    DWORD total_size      = 0;
    BOOL result           = FALSE;

    h_session =
      WinHttpOpen(L"K8-LRT/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h_session) {
        _ERROR("WinHttpOpen failed: %lu", GetLastError());
        goto cleanup;
    }

    h_connect = WinHttpConnect(h_session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!h_connect) {
        _ERROR("WinHttpConnect failed: %lu", GetLastError());
        goto cleanup;
    }

    h_request = WinHttpOpenRequest(h_connect,
                                   L"GET",
                                   L"/repos/jakerieger/K8-LRT/releases/latest",
                                   NULL,
                                   WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!h_request) {
        _ERROR("WinHttpOpenRequest failed: %lu", GetLastError());
        goto cleanup;
    }

    LPCWSTR headers = L"User-Agent: K8-LRT/1.0\r\n";
    WinHttpAddRequestHeaders(h_request, headers, -1L, WINHTTP_ADDREQ_FLAG_ADD);

    result = WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!result) {
        _ERROR("WinHttpReceiveResponse failed: %lu", GetLastError());
        goto cleanup;
    }

    result = WinHttpReceiveResponse(h_request, NULL);
    if (!result) {
        _ERROR("WinHttpReceiveResponse failed: %lu", GetLastError());
        goto cleanup;
    }

    DWORD status_code      = 0;
    DWORD status_code_size = sizeof(status_code);
    WinHttpQueryHeaders(h_request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL,
                        &status_code,
                        &status_code_size,
                        NULL);
    if (status_code != 200) {
        _ERROR("HTTP request failed with status code: %lu", status_code);
        goto cleanup;
    }

    response_data = (char*)malloc(1);
    if (!response_data)
        goto cleanup;
    response_data[0] = '\0';

    // Read the response data
    do {
        bytes_available = 0;
        if (!WinHttpQueryDataAvailable(h_request, &bytes_available)) {
            _ERROR("WinHttpQueryDataAvailable failed: %lu", GetLastError());
            break;
        }

        if (bytes_available == 0)
            break;

        char* tmp = (char*)realloc(response_data, total_size + bytes_available + 1);
        if (!tmp) {
            _ERROR("Memory allocation failed");
            break;
        }
        response_data = tmp;

        if (!WinHttpReadData(h_request, response_data + total_size, bytes_available, &bytes_read)) {
            _ERROR("WinHttpReadData failed: %lu", GetLastError());
            break;
        }

        total_size += bytes_read;
        response_data[total_size] = '\0';

    } while (bytes_available > 0);

    latest_version = extract_tag_name(response_data);

cleanup:
    if (response_data)
        free(response_data);
    if (h_request)
        WinHttpCloseHandle(h_request);
    if (h_connect)
        WinHttpCloseHandle(h_connect);
    if (h_session)
        WinHttpCloseHandle(h_session);

    return latest_version;
}

void check_for_updates(HWND hwnd, BOOL alert_up_to_date) {
    char* current_version = get_latest_version();
    const int compare     = compare_versions(current_version, VER_PRODUCTVERSION_STR);

    if (compare > 0) {
        const char* message = strpool_sprintf("A new version of K8-LRT is available!\n\n"
                                              "Current: %s\n"
                                              "Latest: %s\n\n"
                                              "Visit the GitHub releases page to download?",
                                              VER_PRODUCTVERSION_STR,
                                              current_version + 1);

        const int result = MessageBoxA(hwnd, message, "Update Available", MB_YESNO | MB_ICONINFORMATION);

        if (result == IDYES) {
            ShellExecuteA(NULL,
                          "open",
                          "https://github.com/jakerieger/K8-LRT/releases/latest",
                          NULL,
                          NULL,
                          SW_SHOWNORMAL);
        }
    } else if (compare < 0) {
        MessageBox(hwnd,
                   "You are running a development build. K8-LRT may not be stable.",
                   "Update Check",
                   MB_OK | MB_ICONWARNING);
    } else {
        if (alert_up_to_date)
            MessageBox(hwnd, "You're running the latest version!", "Up to Date", MB_OK | MB_ICONINFORMATION);
    }
}

void update_batch_count_label(HWND hwnd, int count) {
    char label_text[64] = {'\0'};
    if (count == 1) {
        sprintf_s(label_text, sizeof(label_text), "1 library selected");
    } else {
        sprintf_s(label_text, sizeof(label_text), "%d libraries selected", count);
    }
    SetDlgItemTextA(hwnd, IDC_BATCH_COUNT_LABEL, label_text);
}

BOOL open_folder_dialog(HWND owner, char* dst, int len) {
    IFileOpenDialog* pFileOpen = NULL;
    IShellItem* pItem          = NULL;
    PWSTR pszFilePath          = NULL;
    BOOL success               = FALSE;

    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_ALL, &IID_IFileOpenDialog, (void**)&pFileOpen);

    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        hr = pFileOpen->lpVtbl->GetOptions(pFileOpen, &dwOptions);
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->SetOptions(pFileOpen, dwOptions | FOS_PICKFOLDERS);
        }

        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->Show(pFileOpen, owner);
        }

        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->GetResult(pFileOpen, &pItem);

            if (SUCCEEDED(hr)) {
                hr = pItem->lpVtbl->GetDisplayName(pItem, SIGDN_FILESYSPATH, &pszFilePath);

                if (SUCCEEDED(hr)) {
                    WideCharToMultiByte(CP_ACP, 0, pszFilePath, -1, dst, len, NULL, NULL);
                    success = TRUE;
                    CoTaskMemFree(pszFilePath);
                }
                pItem->lpVtbl->Release(pItem);
            }
        }
        pFileOpen->lpVtbl->Release(pFileOpen);
    }

    return success;
}

#pragma endregion
//===================================================================//
//                     -- THREADED OPERATIONS --                     //
//===================================================================//
#pragma region threaded operations

BOOL thread_check_cancel_and_progress(
  HWND hwnd, progress_throttle_state* state, int current, int total, const wchar_t* msg) {
    if (io_state_should_cancel())
        return FALSE;
    if (throttle_should_update(state, current, total))
        report_progress(hwnd, current, total, msg);
    return TRUE;
}

typedef struct {
    HWND hwnd;
    library_entry* libraries;
    size_t lib_count;
    BOOL backup_cache;
    BOOL remove_content;
} remove_thread_params;

typedef struct {
    HWND hwnd;
    library_entry* library;
    const char* new_path;
} relocate_thread_params;

typedef struct {
    HWND hwnd;
} thread_params;

UINT __stdcall remove_worker_thread(void* param) {
    remove_thread_params* params = (remove_thread_params*)param;

    if (params->lib_count > 1) {
        // Remove multiple libraries
        for (int i = 0; i < params->lib_count; i++) {
            wchar_t msg[256];
            swprintf(msg, 256, L"Removing library %zu of %zu...", i + 1, params->lib_count);
            report_progress(params->hwnd, (int)i, (int)params->lib_count, msg);

            const BOOL removed = remove_library(params->libraries + i, params->remove_content);
            if (!removed) {
                report_error(params->hwnd, L"Failed to remove library");
            }
        }

        report_complete(params->hwnd, TRUE, L"Libraries removed successfully");
    } else {
        // Remove single library
        report_progress(params->hwnd, 0, 1, L"Removing selected library...");

        const BOOL removed = remove_library(params->libraries, params->remove_content);
        if (!removed) {
            report_error(params->hwnd, L"Failed to remove library");
        }

        report_complete(params->hwnd, TRUE, L"Library removed successfully");
    }

    free(params);
    io_state_end();

    return 0;
}

UINT __stdcall relocate_worker_thread(void* param) {
    relocate_thread_params* params = (relocate_thread_params*)param;

    report_progress(params->hwnd, 0, 1, L"Relocating library...");

    const char* new_path_full = join_paths(params->new_path, params->library->name);

    if (!copy_directory(params->library->content_dir, new_path_full)) {
        report_error(params->hwnd, L"Failed to copy original content directory to new location");
    }

    if (!rm_rf(make_long_path(params->library->content_dir))) {
        report_error(params->hwnd, L"Failed to delete original content directory");
    }

    HKEY regkey;
    const char* key_path = join_paths("SOFTWARE\\Native Instruments", params->library->name);
    const BOOL open      = open_registry_key(&regkey, HKEY_LOCAL_MACHINE, key_path, KEY_SET_VALUE);
    if (open) {
        if (!set_registry_value_str(regkey, "ContentDir", new_path_full)) {
            report_error(params->hwnd, L"Failed to update registry ContentDir value");
        }
    }
    close_registry_key(&regkey);

    report_complete(params->hwnd, TRUE, L"Library relocated successfully");
    free(params);

    io_state_end();
    return 0;
}

UINT __stdcall test_worker_thread(void* param) {
    thread_params* params = (thread_params*)param;

    progress_throttle_state throttle;
    throttle_init(&throttle);

    const int total = 1e6;
    for (int i = 0; i < total; i++) {
        if (!thread_check_cancel_and_progress(params->hwnd, &throttle, i, total, L"Processing...")) {
            report_complete(params->hwnd, FALSE, L"Operation cancelled");
            free(params);
            io_state_end();
            return 1;
        }

        printf("%d\n", i);
    }

    report_progress(params->hwnd, total, total, L"Complete!");
    report_complete(params->hwnd, TRUE, L"Test finished successfully!");

    free(params);
    io_state_end();

    return 0;
}

void start_remove_operation(HWND hwnd, library_entry* libs, size_t lib_count, BOOL backup, BOOL remove_content) {
    if (!io_state_try_begin(IO_STATE_REMOVING)) {
        MessageBox(hwnd, "Another operation is in progress. Please wait...", "Busy", MB_OK | MB_ICONWARNING);
        return;
    }

    remove_thread_params* params = malloc(sizeof(remove_thread_params));
    if (!params) {
        io_state_end();
        MessageBox(hwnd, "Failed to allocate memory for operation", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    params->hwnd           = hwnd;
    params->libraries      = libs;
    params->lib_count      = lib_count;
    params->remove_content = remove_content;
    params->backup_cache   = backup;

    const HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, remove_worker_thread, params, 0, NULL);
    if (thread) {
        io_state_set_thread(thread);
    } else {
        free(params);
        io_state_end();
        MessageBox(hwnd, "Failed to start operation thread", "Error", MB_OK | MB_ICONERROR);
    }
}

void start_relocate_operation(HWND hwnd, library_entry* lib, const char* new_path) {
    if (!io_state_try_begin(IO_STATE_RELOCATING)) {
        MessageBox(hwnd, "Another operation is in progress. Please wait...", "Busy", MB_OK | MB_ICONWARNING);
        return;
    }

    relocate_thread_params* params = malloc(sizeof(relocate_thread_params));
    if (!params) {
        io_state_end();
        MessageBox(hwnd, "Failed to allocate memory for operation", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    params->hwnd     = hwnd;
    params->library  = lib;
    params->new_path = new_path;

    const HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, relocate_worker_thread, params, 0, NULL);
    if (thread) {
        io_state_set_thread(thread);
    } else {
        free(params);
        io_state_end();
        MessageBox(hwnd, "Failed to start operation thread", "Error", MB_OK | MB_ICONERROR);
    }
}

void start_test_operation(HWND hwnd) {
    if (!io_state_try_begin(IO_STATE_RELOCATING)) {
        MessageBox(hwnd, "Another operation is in progress. Please wait...", "Busy", MB_OK | MB_ICONWARNING);
        return;
    }

    thread_params* params = malloc(sizeof(thread_params));
    if (!params) {
        io_state_end();
        MessageBox(hwnd, "Failed to allocate memory for operation", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    params->hwnd = hwnd;

    const HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, test_worker_thread, params, 0, NULL);
    if (thread) {
        io_state_set_thread(thread);
    } else {
        free(params);
        io_state_end();
        MessageBox(hwnd, "Failed to start operation thread", "Error", MB_OK | MB_ICONERROR);
    }
}

#pragma endregion
//===================================================================//
//                     -- UI HELPER FUNCTIONS --                     //
//===================================================================//
#pragma region ui helper functions

void create_button(HWND* button, const char* label, int x, int y, int w, int h, HWND hwnd, int menu, BOOL disabled) {
    _ASSERT(button != NULL);

    DWORD style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    if (disabled)
        style |= WS_DISABLED;

    *button = CreateWindowEx(0, "BUTTON", label, style, x, y, w, h, hwnd, (HMENU)menu, GetModuleHandle(NULL), NULL);
    SendMessage(*button, WM_SETFONT, (WPARAM)UI_FONT, TRUE);
}

void create_checkbox(HWND* checkbox,
                     const char* label,
                     int x,
                     int y,
                     int w,
                     int h,
                     HWND hwnd,
                     int menu,
                     BOOL begin_checked,
                     BOOL disabled) {
    _ASSERT(checkbox != NULL);

    DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX;
    if (disabled)
        style |= WS_DISABLED;

    *checkbox = CreateWindowEx(0, "BUTTON", label, style, x, y, w, h, hwnd, (HMENU)menu, GetModuleHandle(NULL), NULL);
    SendMessage(*checkbox, WM_SETFONT, (WPARAM)UI_FONT, TRUE);

    if (begin_checked) {
        SendMessage(*checkbox, BM_SETCHECK, (WPARAM)BST_CHECKED, 0);
    }
}

void create_label(HWND* label, const char* text, int x, int y, int w, int h, HWND hwnd) {
    _ASSERT(label != NULL);

    *label = CreateWindow("STATIC",
                          text,
                          WS_CHILD | WS_VISIBLE | SS_LEFT,
                          x,
                          y,
                          w,
                          h,
                          hwnd,
                          NULL,
                          GetModuleHandle(NULL),
                          NULL);
    SendMessage(*label, WM_SETFONT, (WPARAM)UI_FONT, TRUE);
}

void create_listbox(HWND* listbox, int x, int y, int w, int h, HWND hwnd, int menu) {
    _ASSERT(listbox != NULL);

    *listbox = CreateWindowEx(0,
                              "LISTBOX",
                              NULL,
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
                              x,
                              y,
                              w,
                              h,
                              hwnd,
                              (HMENU)menu,
                              GetModuleHandle(NULL),
                              NULL);
    SendMessage(*listbox, WM_SETFONT, (WPARAM)UI_FONT, TRUE);
}

void create_edit(HWND* edit, int x, int y, int w, int h, HWND hwnd, int menu, BOOL multiline, BOOL readonly) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_BORDER;
    if (multiline)
        style |= ES_MULTILINE;
    if (readonly)
        style |= ES_READONLY;

    *edit = CreateWindowEx(0, "EDIT", NULL, style, x, y, w, h, hwnd, (HMENU)menu, GetModuleHandle(NULL), NULL);
}

void create_menu_bar(HWND hwnd) {
    HMENU h_menubar = CreateMenu();
    HMENU h_menu    = CreateMenu();

    AppendMenu(h_menu, MF_STRING, ID_MENU_VIEW_LOG, "&View Log");
    AppendMenu(h_menu, MF_STRING, ID_MENU_RELOAD_LIBRARIES, "&Reload Libraries");
    AppendMenu(h_menu, MF_STRING, ID_MENU_COLLECT_BACKUPS, "&Collect Backups and Zip");
    AppendMenu(h_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(h_menu, MF_STRING, ID_MENU_CHECK_UPDATES, "&Check for Updates");
    AppendMenu(h_menu, MF_STRING, ID_MENU_ABOUT, "&About");
    AppendMenu(h_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(h_menu, MF_STRING, ID_MENU_EXIT, "E&xit");

    AppendMenu(h_menubar, MF_POPUP, (UINT_PTR)h_menu, "&Menu");
    SetMenu(hwnd, h_menubar);
}

void create_progress_panel(HWND hwnd) {
    const int panel_w = _WINDOW_W;
    const int panel_h = 80;
    const int panel_x = 0;
    const int panel_y = _WINDOW_H - ((panel_h * 2) - 20);

    H_PROGRESS_PANEL = CreateWindowEx(WS_EX_STATICEDGE,
                                      "STATIC",
                                      "",
                                      WS_CHILD | SS_NOTIFY,
                                      panel_x,
                                      panel_y,
                                      panel_w,
                                      panel_h,
                                      hwnd,
                                      (HMENU)IDC_PROGRESS_PANEL,
                                      GetModuleHandle(NULL),
                                      NULL);

    H_PROGRESS_TEXT = CreateWindowEx(0,
                                     "STATIC",
                                     "",
                                     WS_CHILD | SS_LEFT,
                                     10,
                                     15,
                                     panel_w / 2,
                                     20,
                                     H_PROGRESS_PANEL,
                                     (HMENU)IDC_PROGRESS_TEXT,
                                     GetModuleHandle(NULL),
                                     NULL);

    H_PROGRESS_CANCEL_BUTTON = CreateWindowEx(0,
                                              "BUTTON",
                                              "Cancel",
                                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                              panel_w - 90,
                                              10,
                                              60,
                                              25,
                                              H_PROGRESS_PANEL,
                                              (HMENU)IDC_PROGRESS_CANCEL_BUTTON,
                                              GetModuleHandle(NULL),
                                              NULL);

    H_PROGRESS_BAR = CreateWindowEx(0,
                                    PROGRESS_CLASS,
                                    NULL,
                                    WS_CHILD | PBS_SMOOTH,
                                    10,
                                    40,
                                    panel_w - 40,
                                    25,
                                    H_PROGRESS_PANEL,
                                    (HMENU)IDC_PROGRESS_BAR,
                                    GetModuleHandle(NULL),
                                    NULL);

    SendMessage(H_PROGRESS_TEXT, WM_SETFONT, (WPARAM)UI_FONT, TRUE);
    SendMessage(H_PROGRESS_CANCEL_BUTTON, WM_SETFONT, (WPARAM)UI_FONT, TRUE);
    ShowWindow(H_PROGRESS_PANEL, SW_HIDE);
}

void show_progress_panel(HWND hwnd) {
    ShowWindow(H_PROGRESS_PANEL, SW_SHOW);
    ShowWindow(H_PROGRESS_BAR, SW_SHOW);
    ShowWindow(H_PROGRESS_TEXT, SW_SHOW);

    ShowWindow(H_BACKUP_CHECKBOX, SW_HIDE);
    ShowWindow(H_REMOVE_LIB_FOLDER_CHECKBOX, SW_HIDE);
    ShowWindow(H_REMOVE_ALL_BUTTON, SW_HIDE);
    ShowWindow(H_REMOVE_BUTTON, SW_HIDE);
    ShowWindow(H_RELOCATE_BUTTON, SW_HIDE);
}

void hide_progress_panel(HWND hwnd) {
    ShowWindow(H_PROGRESS_PANEL, SW_HIDE);
    ShowWindow(H_PROGRESS_BAR, SW_HIDE);
    ShowWindow(H_PROGRESS_TEXT, SW_HIDE);

    ShowWindow(H_BACKUP_CHECKBOX, SW_SHOW);
    ShowWindow(H_REMOVE_LIB_FOLDER_CHECKBOX, SW_SHOW);
    ShowWindow(H_REMOVE_ALL_BUTTON, SW_SHOW);
    ShowWindow(H_REMOVE_BUTTON, SW_SHOW);
    ShowWindow(H_RELOCATE_BUTTON, SW_SHOW);
}

#pragma endregion
//===================================================================//
//                      -- DIALOG CALLBACKS --                       //
//===================================================================//
#pragma region dialog callbacks

LRESULT CALLBACK log_viewer_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch (umsg) {
        case WM_CREATE: {
            HWND h_edit;
            create_edit(&h_edit, 0, 0, 0, 0, hwnd, IDC_LOGVIEW_EDIT, TRUE, TRUE);

            const HFONT h_font = CreateFont(14,
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
                                            FIXED_PITCH | FF_MODERN,
                                            "Consolas");

            SendMessage(h_edit, WM_SETFONT, (WPARAM)h_font, TRUE);

            if (LOG_FILE) {
                const int current_pos = ftell(LOG_FILE);

                fseek(LOG_FILE, 0, SEEK_END);
                const int file_size = ftell(LOG_FILE);
                fseek(LOG_FILE, 0, SEEK_SET);

                char* buffer = (char*)malloc(file_size + 1);
                if (buffer) {
                    const size_t bytes_read = fread(buffer, 1, file_size, LOG_FILE);
                    buffer[bytes_read]      = '\0';

                    // Convert LF to CRLF for Windows Edit control
                    size_t lf_count = 0;
                    for (size_t i = 0; i < bytes_read; i++) {
                        if (buffer[i] == '\n' && (i == 0 || buffer[i - 1] != '\r')) {
                            lf_count++;
                        }
                    }

                    char* crlf_buffer = (char*)malloc(bytes_read + lf_count + 1);
                    if (crlf_buffer) {
                        size_t j = 0;
                        for (size_t i = 0; i < bytes_read; i++) {
                            if (buffer[i] == '\n' && (i == 0 || buffer[i - 1] != '\r')) {
                                crlf_buffer[j++] = '\r';
                            }
                            crlf_buffer[j++] = buffer[i];
                        }
                        crlf_buffer[j] = '\0';

                        SetWindowTextA(h_edit, crlf_buffer);

                        // Scroll to bottom
                        SendMessage(h_edit, EM_SETSEL, 0, -1);
                        SendMessage(h_edit, EM_SETSEL, -1, -1);
                        SendMessage(h_edit, EM_SCROLLCARET, 0, 0);

                        free(crlf_buffer);
                    }

                    free(buffer);
                }

                fseek(LOG_FILE, current_pos, SEEK_SET);  // Restore position for appending
            } else {
                SetWindowTextA(h_edit, "Log file not available.");
            }

            return 0;
        }

        case WM_SIZE: {
            const HWND h_edit = GetDlgItem(hwnd, IDC_LOGVIEW_EDIT);
            if (h_edit) {
                RECT rect;
                GetClientRect(hwnd, &rect);
                SetWindowPos(h_edit, NULL, 0, 0, rect.right, rect.bottom, SWP_NOZORDER);
            }
            return 0;
        }

        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            H_LOG_VIEWER = NULL;
            return 0;
        }
    }

    return DefWindowProc(hwnd, umsg, wparam, lparam);
}

INT_PTR CALLBACK about_dialog_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch (umsg) {
        case WM_NOTIFY: {
            LPNMHDR pnmh = (LPNMHDR)lparam;
            if (pnmh->idFrom == IDC_REPO_LINK && (pnmh->code == NM_CLICK || pnmh->code == NM_RETURN)) {
                PNMLINK pnmLink = (PNMLINK)lparam;
                ShellExecuteW(NULL, L"open", L"https://github.com/jakerieger/K8-LRT", NULL, NULL, SW_SHOWNORMAL);
                return (INT_PTR)TRUE;
            }
            break;
        }

        case WM_INITDIALOG: {
            char* latest_v = (char*)lparam;
            if (latest_v) {
                SetDlgItemTextA(hwnd, IDC_VER_LABEL, strpool_sprintf("Version %s", latest_v));
                SetDlgItemTextA(hwnd, IDC_BUILD_LABEL, strpool_sprintf("Build %d", VER_BUILD));
            }

            return (INT_PTR)TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
                EndDialog(hwnd, LOWORD(wparam));
                return (INT_PTR)TRUE;
            }

            break;
    }

    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK remove_library_dialog_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    static remove_lib_dialog_data* data = NULL;

    switch (umsg) {
        case WM_INITDIALOG: {
            data = (remove_lib_dialog_data*)lparam;

            HWND h_name_label = GetDlgItem(hwnd, IDC_REMOVE_LIBRARY_NAME);
            SetWindowTextA(h_name_label, data->library->name);

            HFONT h_font = CreateFont(16,
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
            SendMessage(h_name_label, WM_SETFONT, (WPARAM)h_font, TRUE);

            HWND h_content_dir_label = GetDlgItem(hwnd, IDC_REMOVE_LIBRARY_CONTENT_DIR);
            SetWindowTextA(h_content_dir_label, data->library->content_dir);

            CheckDlgButton(hwnd, IDC_REMOVE_BACKUP_CHECK, data->backup_files ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_REMOVE_FOLDER_CHECK, data->remove_content_dir ? BST_CHECKED : BST_UNCHECKED);

            RECT parent_rect, dlg_rect;
            HWND h_parent = GetParent(hwnd);
            GetWindowRect(h_parent, &parent_rect);
            GetWindowRect(hwnd, &dlg_rect);

            int dlg_w    = dlg_rect.right - dlg_rect.left;
            int dlg_h    = dlg_rect.bottom - dlg_rect.top;
            int parent_x = parent_rect.left;
            int parent_y = parent_rect.top;
            int parent_w = parent_rect.right - parent_rect.left;
            int parent_h = parent_rect.bottom - parent_rect.top;

            int x = parent_x + (parent_w - dlg_w) / 2;
            int y = parent_y + (parent_h - dlg_h) / 2;

            SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            return (INT_PTR)TRUE;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDREMOVE: {
                    data->backup_files       = (IsDlgButtonChecked(hwnd, IDC_REMOVE_BACKUP_CHECK) == BST_CHECKED);
                    data->remove_content_dir = (IsDlgButtonChecked(hwnd, IDC_REMOVE_FOLDER_CHECK) == BST_CHECKED);
                    EndDialog(hwnd, IDREMOVE);
                    return (INT_PTR)TRUE;
                }

                case IDCANCEL_REMOVE:
                case IDCANCEL: {
                    EndDialog(hwnd, IDCANCEL);
                    return (INT_PTR)TRUE;
                }
            }
            break;
        }

        case WM_CLOSE: {
            EndDialog(hwnd, IDCANCEL);
            return (INT_PTR)TRUE;
        }
    }

    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK batch_remove_dialog_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    static batch_removal_dialog_data* data = NULL;

    switch (umsg) {
        case WM_INITDIALOG: {
            data = (batch_removal_dialog_data*)lparam;

            HWND h_list = GetDlgItem(hwnd, IDC_BATCH_LIBRARY_LIST);
            ListView_SetExtendedListViewStyle(h_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

            LVCOLUMN lvc = {0};
            lvc.mask     = LVCF_TEXT | LVCF_WIDTH;
            lvc.cx       = 340;
            lvc.pszText  = "Library Name";
            ListView_InsertColumn(h_list, 0, &lvc);

            LVITEM lvi = {0};
            lvi.mask   = LVIF_TEXT;

            for (int i = 0; i < data->lib_count; i++) {
                lvi.iItem   = i;
                lvi.pszText = (char*)data->libraries[i].name;
                ListView_InsertItem(h_list, &lvi);

                ListView_SetCheckState(h_list, i, data->selected[i]);
            }

            CheckDlgButton(hwnd, IDC_BATCH_BACKUP_CHECK, data->backup_files ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_BATCH_FOLDER_CHECK, data->remove_library_folder ? BST_CHECKED : BST_UNCHECKED);

            update_batch_count_label(hwnd, data->selected_count);

            RECT parent_rect, dlg_rect;
            HWND h_parent = GetParent(hwnd);
            GetWindowRect(h_parent, &parent_rect);
            GetWindowRect(hwnd, &dlg_rect);

            int dlg_w    = dlg_rect.right - dlg_rect.left;
            int dlg_h    = dlg_rect.bottom - dlg_rect.top;
            int parent_x = parent_rect.left;
            int parent_y = parent_rect.top;
            int parent_w = parent_rect.right - parent_rect.left;
            int parent_h = parent_rect.bottom - parent_rect.top;

            int x = parent_x + (parent_w - dlg_w) / 2;
            int y = parent_y + (parent_h - dlg_h) / 2;

            SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            return (INT_PTR)TRUE;
        }

        case WM_NOTIFY: {
            LPNMHDR pnmh = (LPNMHDR)lparam;

            if (pnmh->idFrom == IDC_BATCH_LIBRARY_LIST) {
                HWND h_list = GetDlgItem(hwnd, IDC_BATCH_LIBRARY_LIST);

                if (pnmh->code == LVN_ITEMCHANGED) {
                    LPNMLISTVIEW pnmlv = (LPNMLISTVIEW)lparam;

                    if ((pnmlv->uChanged & LVIF_STATE) &&
                        ((pnmlv->uNewState & LVIS_STATEIMAGEMASK) != (pnmlv->uOldState & LVIS_STATEIMAGEMASK))) {
                        data->selected[pnmlv->iItem] = ListView_GetCheckState(h_list, pnmlv->iItem);

                        int count = 0;
                        for (int i = 0; i < data->lib_count; i++) {
                            if (data->selected[i]) {
                                count++;
                            }
                        }
                        data->selected_count = count;

                        update_batch_count_label(hwnd, count);
                        EnableWindow(GetDlgItem(hwnd, IDREMOVE_BATCH), count > 0);
                    }
                }
            }
            break;
        }

        case WM_COMMAND: {
            HWND h_list = GetDlgItem(hwnd, IDC_BATCH_LIBRARY_LIST);

            switch (LOWORD(wparam)) {
                case IDC_BATCH_SELECT_ALL: {
                    for (int i = 0; i < data->lib_count; i++) {
                        ListView_SetCheckState(h_list, i, TRUE);
                        data->selected[i] = TRUE;
                    }
                    data->selected_count = data->lib_count;
                    update_batch_count_label(hwnd, data->selected_count);
                    EnableWindow(GetDlgItem(hwnd, IDREMOVE_BATCH), TRUE);
                    return (INT_PTR)TRUE;
                }

                case IDC_BATCH_DESELECT_ALL: {
                    for (int i = 0; i < data->lib_count; i++) {
                        ListView_SetCheckState(h_list, i, FALSE);
                        data->selected[i] = FALSE;
                    }
                    data->selected_count = 0;
                    update_batch_count_label(hwnd, 0);
                    EnableWindow(GetDlgItem(hwnd, IDREMOVE_BATCH), FALSE);
                    return (INT_PTR)TRUE;
                }

                case IDREMOVE_BATCH: {
                    if (data->selected_count == 0) {
                        MessageBox(hwnd,
                                   "Please select at least one library to remove.",
                                   "No Selection",
                                   MB_OK | MB_ICONINFORMATION);
                        return (INT_PTR)TRUE;
                    }

                    data->backup_files          = (IsDlgButtonChecked(hwnd, IDC_BATCH_BACKUP_CHECK) == BST_CHECKED);
                    data->remove_library_folder = (IsDlgButtonChecked(hwnd, IDC_BATCH_FOLDER_CHECK) == BST_CHECKED);

                    for (int i = 0; i < data->lib_count; i++) {
                        data->selected[i] = ListView_GetCheckState(h_list, i);
                    }

                    EndDialog(hwnd, IDREMOVE_BATCH);
                    return (INT_PTR)TRUE;
                }

                case IDCANCEL_BATCH:
                case IDCANCEL: {
                    EndDialog(hwnd, IDCANCEL);
                    return (INT_PTR)TRUE;
                }
            }
            break;
        }

        case WM_CLOSE: {
            EndDialog(hwnd, IDCANCEL);
            return (INT_PTR)TRUE;
        }
    }

    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK relocate_dialog_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    static relocate_lib_dialog_data* data = NULL;

    switch (umsg) {
        case WM_INITDIALOG: {
            data = (relocate_lib_dialog_data*)lparam;

            HWND h_name_label = GetDlgItem(hwnd, IDC_LIB_NAME_LABEL);
            SetWindowTextA(h_name_label, data->library->name);

            HFONT h_font = CreateFont(16,
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
            SendMessage(h_name_label, WM_SETFONT, (WPARAM)h_font, TRUE);

            HWND h_content_dir_label = GetDlgItem(hwnd, IDC_CONTENT_DIR_LABEL);
            SetWindowTextA(h_content_dir_label, data->library->content_dir);

            RECT parent_rect, dlg_rect;
            HWND h_parent = GetParent(hwnd);
            GetWindowRect(h_parent, &parent_rect);
            GetWindowRect(hwnd, &dlg_rect);

            int dlg_w    = dlg_rect.right - dlg_rect.left;
            int dlg_h    = dlg_rect.bottom - dlg_rect.top;
            int parent_x = parent_rect.left;
            int parent_y = parent_rect.top;
            int parent_w = parent_rect.right - parent_rect.left;
            int parent_h = parent_rect.bottom - parent_rect.top;

            int x = parent_x + (parent_w - dlg_w) / 2;
            int y = parent_y + (parent_h - dlg_h) / 2;

            SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            return (INT_PTR)TRUE;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDC_RELOCATE_BROWSE_BTN: {
                    char selected_path[MAX_PATH] = {0};
                    if (open_folder_dialog(hwnd, selected_path, MAX_PATH)) {
                        SetDlgItemText(hwnd, IDC_RELOCATE_PATH_EDIT, selected_path);
                    }
                    return (INT_PTR)TRUE;
                }

                case IDRELOCATE_RELOCATE: {
                    GetDlgItemText(hwnd, IDC_RELOCATE_PATH_EDIT, data->new_path, MAX_PATH);

                    if (strlen(data->new_path) == 0) {
                        MessageBox(hwnd, "Please select a destination folder.", "Error", MB_ICONWARNING);
                        return (INT_PTR)TRUE;
                    }

                    EndDialog(hwnd, IDRELOCATE_RELOCATE);
                    return (INT_PTR)TRUE;
                }

                case IDCANCEL_RELOCATE:
                case IDCANCEL: {
                    EndDialog(hwnd, IDCANCEL);
                    return (INT_PTR)TRUE;
                }
            }
            break;
        }

        case WM_CLOSE: {
            EndDialog(hwnd, IDCANCEL);
            return (INT_PTR)TRUE;
        }
    }

    return (INT_PTR)FALSE;
}

#pragma endregion
//===================================================================//
//                     -- WNDPROC CALLBACKS --                       //
//===================================================================//
#pragma region wndproc callbacks

LRESULT on_create(HWND hwnd) {
    io_state_init(hwnd);

    UI_FONT = CreateFont(16,
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
    if (!UI_FONT) {
        _WARN("Failed to create UI font. Falling back to system default.");
    }

    create_menu_bar(hwnd);

    create_label(&H_SELECT_LIB_LABEL, "Select a library to remove:", 10, 10, 265, 36, hwnd);
    create_listbox(&H_LISTBOX, 10, 36, 265, 240, hwnd, IDC_LISTBOX);

    create_checkbox(&H_BACKUP_CHECKBOX,
                    "Backup cache files before deleting",
                    10,
                    260,
                    220,
                    20,
                    hwnd,
                    IDC_CHECKBOX_BACKUP,
                    TRUE,
                    FALSE);
    create_checkbox(&H_REMOVE_LIB_FOLDER_CHECKBOX,
                    "Delete library content directory",
                    10,
                    280,
                    220,
                    20,
                    hwnd,
                    IDC_CHECKBOX_REMOVE_LIB_FOLDER,
                    TRUE,
                    FALSE);

    create_button(&H_REMOVE_ALL_BUTTON, "Remove All...", 10, 312, 131, 30, hwnd, IDC_REMOVE_ALL_BUTTON, FALSE);
    create_button(&H_REMOVE_BUTTON, "Remove Selected", 145, 312, 131, 30, hwnd, IDC_REMOVE_BUTTON, TRUE);
    create_button(&H_RELOCATE_BUTTON, "Relocate Selected", 10, 346, 266, 26, hwnd, IDC_RELOCATE_BUTTON, TRUE);

    create_progress_panel(hwnd);

    return 0;
}

LRESULT on_show(HWND hwnd) {
    // Search registry for key entries in `HKEY_LOCAL_MACHINE/SOFTWARE/Native Instruments/..`
    const BOOL query_result = query_libraries(hwnd);
    if (!query_result) {
        MessageBox(hwnd,
                   "Failed to query libraries. Do you have any Kontakt libraries installed?\n\nCheck "
                   "'K8-LRT.log' for details.",
                   "Error",
                   MB_OK | MB_ICONERROR);
        PostQuitMessage(0);
    }

    check_for_updates(hwnd, FALSE);

    return 0;
}

void on_selection_changed(HWND hwnd) {
    const int sel = (int)SendMessage(H_LISTBOX, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) {
        SELECTED_INDEX = sel;
    }
    EnableWindow(H_REMOVE_BUTTON, (sel != LB_ERR));
    EnableWindow(H_RELOCATE_BUTTON, (sel != LB_ERR));
}

void on_remove_selected(HWND hwnd) {
    BACKUP_FILES       = _IS_CHECKED(IDC_CHECKBOX_BACKUP);
    REMOVE_CONTENT_DIR = _IS_CHECKED(IDC_CHECKBOX_REMOVE_LIB_FOLDER);

    remove_lib_dialog_data data = {
      .library            = &LIBRARIES[SELECTED_INDEX],
      .backup_files       = BACKUP_FILES,
      .remove_content_dir = REMOVE_CONTENT_DIR,
    };

    const INT_PTR result = DialogBoxParam(GetModuleHandle(NULL),
                                          MAKEINTRESOURCE(IDD_REMOVE_LIBRARYBOX),
                                          hwnd,
                                          remove_library_dialog_proc,
                                          (LPARAM)&data);

    if (result == IDREMOVE) {
        BACKUP_FILES       = data.backup_files;
        REMOVE_CONTENT_DIR = data.remove_content_dir;
        library_entry* lib = &LIBRARIES[SELECTED_INDEX];
        start_remove_operation(hwnd, lib, 1, BACKUP_FILES, REMOVE_CONTENT_DIR);
    }
}

void on_remove_all(HWND hwnd) {
    start_test_operation(hwnd);
    return;

    if (LIB_COUNT == 0) {
        MessageBox(hwnd, "No libraries found to remove.", "No Libraries", MB_OK | MB_ICONINFORMATION);
        return;
    }

    BACKUP_FILES       = _IS_CHECKED(IDC_CHECKBOX_BACKUP);
    REMOVE_CONTENT_DIR = _IS_CHECKED(IDC_CHECKBOX_REMOVE_LIB_FOLDER);

    BOOL* selected = (BOOL*)malloc(sizeof(BOOL) * LIB_COUNT);
    for (int i = 0; i < LIB_COUNT; i++) {
        selected[i] = TRUE;  // All selected by default
    }

    batch_removal_dialog_data dialog_data = {.libraries             = LIBRARIES,
                                             .lib_count             = LIB_COUNT,
                                             .selected              = selected,
                                             .backup_files          = BACKUP_FILES,
                                             .remove_library_folder = REMOVE_CONTENT_DIR,
                                             .selected_count        = LIB_COUNT};

    const INT_PTR result = DialogBoxParam(GetModuleHandle(NULL),
                                          MAKEINTRESOURCE(IDD_BATCH_REMOVEBOX),
                                          hwnd,
                                          batch_remove_dialog_proc,
                                          (LPARAM)&dialog_data);

    if (result == IDREMOVE_BATCH) {
        BACKUP_FILES       = dialog_data.backup_files;
        REMOVE_CONTENT_DIR = dialog_data.remove_library_folder;

        int selected_count = 0;
        library_entry libs[_MAX_LIB_COUNT];

        for (int i = 0; i < dialog_data.lib_count; i++) {
            if (dialog_data.selected[i]) {
                libs[selected_count++] = dialog_data.libraries[i];
            }
        }

        start_remove_operation(hwnd, libs, selected_count, BACKUP_FILES, REMOVE_CONTENT_DIR);
    }

    free(selected);
}

void on_relocate_selected(HWND hwnd) {
    relocate_lib_dialog_data data = {.library = &LIBRARIES[SELECTED_INDEX]};
    const INT_PTR result          = DialogBoxParam(GetModuleHandle(NULL),
                                          MAKEINTRESOURCE(IDD_RELOCATE_LIBRARYBOX),
                                          hwnd,
                                          relocate_dialog_proc,
                                          (LPARAM)&data);

    if (result == IDRELOCATE_RELOCATE) {
        _INFO("Relocating library '%s'", data.library->name);
        _INFO("Old Path: %s", data.library->content_dir);
        _INFO("New Path: %s", data.new_path);

        start_relocate_operation(hwnd, &LIBRARIES[SELECTED_INDEX], data.new_path);
    }
}

void on_view_log(HWND hwnd) {
    if (H_LOG_VIEWER && IsWindow(H_LOG_VIEWER)) {
        SetForegroundWindow(H_LOG_VIEWER);
        return;
    }

    WNDCLASS wc      = {0};
    wc.lpfnWndProc   = log_viewer_proc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = _LOGVIEW_CLASS;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon         = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON));

    if (!GetClassInfo(GetModuleHandle(NULL), _LOGVIEW_CLASS, &wc)) {
        RegisterClass(&wc);
    }

    RECT parent_rect;
    GetWindowRect(hwnd, &parent_rect);
    const int parent_x = parent_rect.left;
    const int parent_y = parent_rect.top;
    const int parent_w = parent_rect.right - parent_rect.left;
    const int parent_h = parent_rect.bottom - parent_rect.top;
    const int log_x    = parent_x + (parent_w - _LOGVIEW_W) / 2;
    const int log_y    = parent_y + (parent_h - _LOGVIEW_H) / 2;

    H_LOG_VIEWER = CreateWindowEx(WS_EX_TOOLWINDOW,
                                  _LOGVIEW_CLASS,
                                  _LOGVIEW_TITLE,
                                  WS_OVERLAPPEDWINDOW,
                                  log_x,
                                  log_y,
                                  _LOGVIEW_W,
                                  _LOGVIEW_H,
                                  hwnd,
                                  NULL,
                                  GetModuleHandle(NULL),
                                  NULL);

    if (H_LOG_VIEWER) {
        ShowWindow(H_LOG_VIEWER, SW_SHOW);
    } else {
        MessageBox(hwnd, "Failed to create log viewer window.", "Error", MB_OK | MB_ICONERROR);
    }
}

void on_reload_libraries(HWND hwnd) {
    const int response =
      MessageBox(hwnd, "Clear found libraries and search again?", "Confirm Reload", MB_YESNO | MB_ICONQUESTION);
    if (response == IDYES) {
        const BOOL query_result = query_libraries(hwnd);
        if (!query_result) {
            MessageBox(hwnd, "Failed to query libraries. Are you admin?", "Error querying", MB_OK | MB_ICONERROR);
        }
    }
}

void on_exit(HWND hwnd) {
    const int response = MessageBox(hwnd, "Are you sure you want exit?", "Confirm Exit", MB_YESNO | MB_ICONQUESTION);
    if (response == IDYES) {
        PostQuitMessage(0);
    }
}

void on_about(HWND hwnd) {
    const char* version = VER_PRODUCTVERSION_STR;
    DialogBoxParam(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, about_dialog_proc, (LPARAM)version);
}

#pragma endregion
//===================================================================//
//                      -- WINDOW CALLBACK --                        //
//===================================================================//
#pragma region window callbacks

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch (umsg) {
        case WM_CREATE:
            return on_create(hwnd);

        case WM_SHOWWINDOW:
            return on_show(hwnd);

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wparam;
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc      = (HDC)wparam;
            HWND control = (HWND)lparam;

            // Panel background
            if (control == H_PROGRESS_PANEL) {
                SetBkColor(hdc, RGB(245, 245, 245));  // Light gray
                static HBRUSH panel_brush = NULL;
                if (!panel_brush) {
                    panel_brush = CreateSolidBrush(RGB(245, 245, 245));
                }
                return (LRESULT)panel_brush;
            }

            // Text background (match panel)
            if (control == H_PROGRESS_TEXT) {
                SetBkColor(hdc, RGB(245, 245, 245));
                SetTextColor(hdc, RGB(50, 50, 50));  // Dark text
                static HBRUSH text_brush = NULL;
                if (!text_brush) {
                    text_brush = CreateSolidBrush(RGB(245, 245, 245));
                }
                return (LRESULT)text_brush;
            }

            // Default for other controls
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wparam;
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_IO_PROGRESS: {
            progress_data* data = (progress_data*)lparam;
            if (data) {
                int percent = 0;
                if (data->total > 0) {
                    percent = (int)((data->current * 100.0) / data->total);
                    if (percent > 100)
                        percent = 100;
                }

                // Set range to 0-100 (always safe for 16-bit)
                SendMessage(H_PROGRESS_BAR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
                SendMessage(H_PROGRESS_BAR, PBM_SETPOS, percent, 0);

                wchar_t status_text[512];
                if (data->message[0] != L'\0') {
                    swprintf(status_text, 512, L"%s (%d%%)", data->message, percent);
                } else {
                    swprintf(status_text, 512, L"Progress: %d%%", percent);
                }
                SetWindowTextW(H_PROGRESS_TEXT, status_text);

                show_progress_panel(hwnd);

                free(data);
            }
            return 0;
        }

        case WM_IO_COMPLETE: {
            BOOL success     = (BOOL)wparam;
            wchar_t* message = (wchar_t*)lparam;

            hide_progress_panel(hwnd);

            // Reset progress bar
            SendMessage(H_PROGRESS_BAR, PBM_SETPOS, 0, 0);
            SetWindowTextW(H_PROGRESS_TEXT, L"");

            // Show completion message
            if (message) {
                MessageBoxW(hwnd,
                            message,
                            success ? L"Success" : L"Operation Complete",
                            MB_OK | (success ? MB_ICONINFORMATION : MB_ICONWARNING));
            }

            SELECTED_INDEX          = -1;
            const BOOL query_result = query_libraries(hwnd);
            if (!query_result) {
                _FATAL("Failed to query libraries. See 'K8-LRT.log' for more details.");
            }

            return 0;
        }

        case WM_IO_ERROR: {
            wchar_t* error_message = (wchar_t*)lparam;

            hide_progress_panel(hwnd);

            SendMessage(H_PROGRESS_BAR, PBM_SETPOS, 0, 0);
            SetWindowTextW(H_PROGRESS_TEXT, L"");

            if (error_message) {
                MessageBoxW(hwnd, error_message, L"Error", MB_OK | MB_ICONERROR);
                free(error_message);
            }

            SELECTED_INDEX = -1;

            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case ID_MENU_VIEW_LOG: {
                    on_view_log(hwnd);
                    break;
                }

                case ID_MENU_RELOAD_LIBRARIES: {
                    on_reload_libraries(hwnd);
                    break;
                }

                case ID_MENU_EXIT: {
                    on_exit(hwnd);
                    break;
                }

                case ID_MENU_CHECK_UPDATES: {
                    check_for_updates(hwnd, TRUE);
                    break;
                }

                case ID_MENU_ABOUT: {
                    on_about(hwnd);
                    break;
                }

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

                case IDC_RELOCATE_BUTTON: {
                    on_relocate_selected(hwnd);
                    break;
                }
            }

            return 0;
        }

        case WM_CLOSE: {
            if (io_state_is_busy()) {
                io_state_request_cancel();

                if (IO_STATE.thread_handle) {
                    WaitForSingleObject(IO_STATE.thread_handle, 2000);
                }
            }

            DestroyWindow(hwnd);
            break;
        }

        case WM_DESTROY: {
            io_state_cleanup();
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hwnd, umsg, wparam, lparam);
}

#pragma endregion

//===================================================================//
//                         -- ENTRYPOINT --                          //
//===================================================================//

int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE h_prev_instance, LPSTR lp_cmd_line, int n_cmd_show) {
#ifndef NDEBUG
    attach_console();
#endif

    strpool_init();
    enable_backup_privilege();
    log_init("K8-LRT.log");

    const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
        return 1;

    // Initialize common controls
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASS wc      = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = h_instance;
    wc.lpszClassName = _WINDOW_CLASS;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(h_instance, MAKEINTRESOURCE(IDI_APPICON));

    RegisterClass(&wc);

    // Calculate screen center point
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    const int win_x    = (screen_w - _WINDOW_W) / 2;
    const int win_y    = (screen_h - _WINDOW_H) / 2;

    const HWND hwnd = CreateWindowEx(0,
                                     _WINDOW_CLASS,
                                     _WINDOW_TITLE,
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                     win_x,
                                     win_y,
                                     _WINDOW_W,
                                     _WINDOW_H,
                                     NULL,
                                     NULL,
                                     h_instance,
                                     NULL);

    if (hwnd == NULL)
        return 1;

    ShowWindow(hwnd, n_cmd_show);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

#ifndef NDEBUG
    if (FreeConsole()) {
        if (ATTACHED_TO_CONSOLE)
            _LOG("Detached debug console");
        ATTACHED_TO_CONSOLE = FALSE;
    }
#endif

    CoUninitialize();

    log_close();
    strpool_destroy();

    return 0;
}