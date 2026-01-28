/*
    K8-LRT - v1.2.0 - Library removal tool for Bobdule's Kontakt 8

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

        1.2.0  (TBD)         bug fixes for registry querying
        1.1.0  (2026-01-26)  additional directory checks and removals, UI additions and changes
        1.0.0  (2026-01-25)  UI redux, added functionality, updates
        0.3.1  (2026-01-23)  memory model improvements
        0.3.0  (2026-01-23)  sweeping code changes, bug fixes, and logging
        0.2.0  (2026-01-23)  tons of bug fixes and code improvements
        0.1.0  (2026-01-22)  initial release of K8-LRT
*/

#include "version.h"

#include <stdio.h>
#include <stdarg.h>
#include <windows.h>   // Core Windows API
#include <winerror.h>  // Windows error API
#include <winreg.h>    // Registry API
#include <Shlwapi.h>   // Shell API
#include <shlobj.h>    // More Shell API
#include <winuser.h>   // Dialogs and display
#include <CommCtrl.h>  // For modern Windows styling (Common Controls)
#include <winhttp.h>   // For checking for updates

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
#ifndef NDEBUG
    if (ATTACHED_TO_CONSOLE) {
        printf("[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
               st.wYear,
               st.wMonth,
               st.wDay,
               st.wHour,
               st.wMinute,
               st.wSecond,
               st.wMilliseconds,
               level_str);
        vprintf(fmt, args);
        printf("\n");
    }
#endif
    va_end(args);

    fprintf(LOG_FILE, "\n");
    fflush(LOG_FILE);
}

void log_init(const char* filename) {
    errno_t result = fopen_s(&LOG_FILE, filename, "a+");  // append (+ read) mode
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

//====================================================================//
//                           -- MEMORY --                             //
//====================================================================//

#define _ARENA_BASE (sizeof(arena))
#define _PAGESIZE (sizeof(void*))
#define _ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define _KB(n) ((UINT64)(n) << 10)
#define _MB(n) ((UINT64)(n) << 20)
#define _GB(n) ((UINT64)(n) << 30)
#define _PROGRAM_MEMORY (_KB(128))  // Define how much memory to give K8-LRT (128 kilobytes)

typedef struct {
    UINT64 capacity;
    UINT64 position;
} arena;

static arena* ARENA = NULL;

void arena_init(UINT64 capacity) {
    ARENA = (arena*)malloc(capacity);
    if (!ARENA) {
        _FATAL("Failed to allocate memory");
    }

    ARENA->position = _ARENA_BASE;
    ARENA->capacity = capacity;
}

void arena_destroy(void) {
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

void arena_clear(void) {
    arena_pop_to(_ARENA_BASE);
}

#define _ALLOC_ARRAY(type, count) (type*)arena_push(sizeof(type) * (count), FALSE)
#define _ALLOC_STR(length) (char*)arena_push(sizeof(char) * (length), FALSE)
#define _ALLOC(type) (type*)arena_push(sizeof(type), FALSE)

//====================================================================//
//                   -- UI ELEMENT DEFINITIONS --                     //
//====================================================================//

#include "resource.h"

static HWND H_LISTBOX                    = NULL;
static HWND H_REMOVE_BUTTON              = NULL;
static HWND H_REMOVE_ALL_BUTTON          = NULL;
static HWND H_BACKUP_CHECKBOX            = NULL;  // Whether or not we should backup delete filesa
static HWND H_LOG_VIEWER                 = NULL;
static HWND H_REMOVE_LIB_FOLDER_CHECKBOX = NULL;
static HWND H_SELECT_LIB_LABEL           = NULL;
static HWND H_RELOCATE_BUTTON            = NULL;

static HFONT UI_FONT = NULL;

//====================================================================//
//                          -- GLOBALS --                             //
//====================================================================//

#define _WINDOW_W 300
#define _WINDOW_H 436
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

struct library_entry {
    // Library name in registry (this is always the general name used throughout NI's systems)
    const char* name;
    // Actual location of library on disk
    const char* content_dir;
};

static library_entry* LIBRARIES = {NULL};
static int LIB_COUNT            = 0;
static int SELECTED_INDEX       = -1;

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

//====================================================================//
//                      -- HELPER FUNCTIONS --                        //
//====================================================================//
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

char* get_local_appdata_path(void) {
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
        ATTACHED_TO_CONSOLE = TRUE;
        _LOG("Attached debug console");
    } else {
        ATTACHED_TO_CONSOLE = FALSE;
        _WARN("Failed to attach debug console (process must be executed from within a shell)");
    }
}

BOOL file_exists(const char* path) {
    DWORD dw_attrib = GetFileAttributesA(path);
    return (dw_attrib != INVALID_FILE_ATTRIBUTES && !(dw_attrib & FILE_ATTRIBUTE_DIRECTORY));
}

BOOL directory_exists(const char* path) {
    DWORD dw_attrib = GetFileAttributesA(path);
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

BOOL rm_rf(const char* directory) {
    if (directory == NULL)
        return FALSE;

    SHFILEOPSTRUCTA file_op = {
      .hwnd                  = NULL,
      .wFunc                 = FO_DELETE,
      .pFrom                 = directory,
      .pTo                   = NULL,
      .fFlags                = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT,
      .fAnyOperationsAborted = FALSE,
      .hNameMappings         = NULL,
      .lpszProgressTitle     = NULL,
    };

    // SHFileOperation expects a double-null terminated string
    char path_buffer[MAX_PATH + 1] = {0};
    strncpy_s(path_buffer, strlen(directory), directory, MAX_PATH);

    file_op.pFrom = path_buffer;
    return SHFileOperationA(&file_op) == 0;
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
    if (ARENA->position > _ARENA_BASE) {
        arena_clear();
    }

    // Reset libraries array
    LIBRARIES = _ALLOC_ARRAY(library_entry, _MAX_LIB_COUNT);

    // Reset list box contents
    SendMessage(H_LISTBOX, LB_RESETCONTENT, 0, 0);

    SELECTED_INDEX = -1;
    LIB_COUNT      = 0;
}

BOOL open_regisry_key(HKEY* key, HKEY base, const char* path, UINT sam) {
    LONG result = RegOpenKeyExA(base, path, 0, (REGSAM)sam, key);
    if (result != ERROR_SUCCESS) {
        _ERROR("Failed to open registry key: '%s'", path);
        return FALSE;
    }

    return TRUE;
}

void close_registry_key(HKEY* key) {
    RegCloseKey(*key);
}

int enumerate_registry_keys(HKEY key, char* keys[]) {
    char current_key[_MAX_KEY_LENGTH];
    DWORD buffer_size = sizeof(current_key);
    FILETIME ft_last;
    DWORD index = 0;

    while (RegEnumKeyExA(key, index, current_key, &buffer_size, NULL, NULL, NULL, &ft_last) == ERROR_SUCCESS) {
        _INFO("Found registry entry: '%s'", current_key);
        const size_t key_len = strlen(current_key);
        char* key_memory     = _ALLOC_STR(key_len + 1);
        memcpy(key_memory, current_key, key_len);
        key_memory[key_len] = '\0';
        keys[index]         = key_memory;

        buffer_size = sizeof(current_key);
        index++;
    }

    return index;
}

const char* get_registry_value_str(HKEY key, const char* value_name) {
    char value[MAX_PATH];
    DWORD buffer_size = sizeof(value);
    DWORD value_type;

    LSTATUS status = RegQueryValueExA(key, value_name, NULL, &value_type, (LPBYTE)value, &buffer_size);
    if (status == ERROR_SUCCESS && value_type == REG_SZ) {
        const size_t value_len = strlen(value);
        char* value_memory     = _ALLOC_STR(value_len + 1);
        memcpy(value_memory, value, value_len);
        value_memory[value_len] = '\0';

        return value_memory;
    }

    return NULL;
}

BOOL query_libraries(HWND hwnd) {
    clear_libraries();

    HKEY base_key;
    LPCSTR base_path = "SOFTWARE\\Native Instruments";
    BOOL result      = open_regisry_key(&base_key, HKEY_LOCAL_MACHINE, base_path, KEY_READ);
    if (!result)
        return FALSE;

    char** keys   = _ALLOC_ARRAY(char*, _MAX_LIB_COUNT);
    int key_count = enumerate_registry_keys(base_key, keys);

    close_registry_key(&base_key);

    for (int i = 0; i < key_count; i++) {
        if ((ARENA->position - _ARENA_BASE) + sizeof(library_entry) > ARENA->capacity) {
            _FATAL("Memory capacity reached (%d > %d)",
                   (ARENA->position - _ARENA_BASE) + sizeof(library_entry),
                   ARENA->capacity);
        }

        const char* key_str = keys[i];

        BOOL in_exclusion_list = list_contains(KEY_EXCLUSION_LIST, KEY_EXCLUSION_LIST_SIZE, key_str);
        BOOL matches_exclusion_pattern =
          matches_pattern_in_list(KEY_EXCLUSION_PATTERNS, KEY_EXCLUSION_PATTERNS_SIZE, key_str);
        if (in_exclusion_list || matches_exclusion_pattern)
            continue;

        HKEY key;
        BOOL result = open_regisry_key(&key, HKEY_LOCAL_MACHINE, join_paths(base_path, key_str), KEY_READ);
        if (!result)
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
        char msg[256] = {'\0'};
        snprintf(msg, 256, "Found %d installed libraries", LIB_COUNT);
        MessageBox(hwnd, msg, "K8-LRT", MB_OK | MB_ICONINFORMATION);
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
                char* reg_filename = join_str(key, ".reg");
                DeleteFileA(reg_filename);
                LONG backup_res = RegSaveKeyExA(h_subkey, reg_filename, NULL, REG_LATEST_FORMAT);

                if (backup_res != ERROR_SUCCESS) {
                    _ERROR("Failed to backup registry entry for key: 'HKEY_LOCAL_MACHINE\\%s\\%s'", base_path, key);
                    RegCloseKey(h_subkey);
                    return FALSE;
                }

                RegCloseKey(h_subkey);
            }
        }

        LONG res = RegDeleteKeyA(h_key, key);
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
                            BOOL deleted = DeleteFileA(bak_filename);
                            if (!deleted) {
                                _ERROR("Failed to delete file: '%s'", bak_filename);
                                return FALSE;
                            }
                        }
                        BOOL copied = CopyFileExA(file_path, bak_filename, NULL, NULL, NULL, 0);
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
    char* cache_path = join_paths(get_local_appdata_path(), _LIB_CACHE_ROOT);
    return remove_all_files_in_dir(cache_path, BACKUP_FILES);
}

BOOL remove_db3(void) {
    char* appdata_local = get_local_appdata_path();
    char* db3           = join_paths(appdata_local, _DB3_ROOT);
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
        result = rm_rf(library->content_dir);
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

    size_t len = value_end - value_start;
    char* tag  = _ALLOC_STR(len + 1);
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
    int compare           = compare_versions(current_version, VER_PRODUCTVERSION_STR);

    if (compare > 0) {
        char message[512];
        sprintf_s(message,
                  512,
                  "A new version of K8-LRT is available!\n\n"
                  "Current: %s\n"
                  "Latest: %s\n\n"
                  "Visit the GitHub releases page to download?",
                  VER_PRODUCTVERSION_STR,
                  current_version + 1);

        int result = MessageBoxA(hwnd, message, "Update Available", MB_YESNO | MB_ICONINFORMATION);

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

//====================================================================//
//                     -- UI HELPER FUNCTIONS --                      //
//====================================================================//

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

    if (begin_checked)
        SendMessage(*checkbox, BM_SETCHECK, (WPARAM)TRUE, TRUE);
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

    *listbox = CreateWindowEx(WS_EX_CLIENTEDGE,
                              "LISTBOX",
                              NULL,
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
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

//====================================================================//
//                      -- DIALOG CALLBACKS --                        //
//====================================================================//

LRESULT CALLBACK log_viewer_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch (umsg) {
        case WM_CREATE: {
            HWND h_edit = CreateWindowEx(WS_EX_CLIENTEDGE,
                                         "EDIT",
                                         NULL,
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                           ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                         0,
                                         0,
                                         0,
                                         0,
                                         hwnd,
                                         (HMENU)IDC_LOGVIEW_EDIT,
                                         GetModuleHandle(NULL),
                                         NULL);

            HFONT h_font = CreateFont(14,
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
                int current_pos = ftell(LOG_FILE);

                fseek(LOG_FILE, 0, SEEK_END);
                int file_size = ftell(LOG_FILE);
                fseek(LOG_FILE, 0, SEEK_SET);

                char* buffer = (char*)malloc(file_size + 1);
                if (buffer) {
                    size_t bytes_read  = fread(buffer, 1, file_size, LOG_FILE);
                    buffer[bytes_read] = '\0';

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
            HWND h_edit = GetDlgItem(hwnd, IDC_LOGVIEW_EDIT);
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
                char buffer[64] = {'\0'};
                wsprintfA(buffer, "Version %s", latest_v);
                SetDlgItemTextA(hwnd, IDC_VER_LABEL, buffer);

                memset(buffer, 0, 64);
                wsprintfA(buffer, "Build %d", VER_BUILD);
                SetDlgItemTextA(hwnd, IDC_BUILD_LABEL, buffer);
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

//====================================================================//
//                     -- WNDPROC CALLBACKS --                        //
//====================================================================//

LRESULT on_create(HWND hwnd) {
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

    create_button(&H_REMOVE_ALL_BUTTON, "Remove All...", 10, 306, 130, 30, hwnd, IDC_REMOVE_ALL_BUTTON, FALSE);
    create_button(&H_REMOVE_BUTTON, "Remove Selected", 147, 306, 130, 30, hwnd, IDC_REMOVE_BUTTON, TRUE);
    create_button(&H_RELOCATE_BUTTON, "Relocate Selected", 10, 340, 266, 26, hwnd, IDC_RELOCATE_BUTTON, TRUE);

    return 0;
}

LRESULT on_show(HWND hwnd) {
    // Search registry for key entries in `HKEY_LOCAL_MACHINE/SOFTWARE/Native Instruments/..`
    BOOL query_result = query_libraries(hwnd);
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
    int sel = (int)SendMessage(H_LISTBOX, LB_GETCURSEL, 0, 0);
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

    INT_PTR result = DialogBoxParam(GetModuleHandle(NULL),
                                    MAKEINTRESOURCE(IDD_REMOVE_LIBRARYBOX),
                                    hwnd,
                                    remove_library_dialog_proc,
                                    (LPARAM)&data);

    if (result == IDREMOVE) {
        BACKUP_FILES       = data.backup_files;
        REMOVE_CONTENT_DIR = data.remove_content_dir;

        BOOL removed = remove_selected_library();
        if (!removed) {
            MessageBox(hwnd, "Failed to remove library. Check K8-LRT.log for details.", "Error", MB_OK | MB_ICONERROR);
            return;
        } else {
            BOOL query_result = query_libraries(hwnd);
            if (query_result) {
                EnableWindow(H_REMOVE_BUTTON, FALSE);
                MessageBox(hwnd, "Successfully removed library.", "Success", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBox(hwnd,
                           "Failed to query libraries. Do you have any Kontakt libraries "
                           "installed?\n\nCheck 'K8-LRT.log' for details.",
                           "Error",
                           MB_OK | MB_ICONERROR);
                PostQuitMessage(0);
            }
        }
    }
}

void on_remove_all(HWND hwnd) {
    if (LIB_COUNT == 0) {
        MessageBox(hwnd, "No libraries found to remove.", "No Libraries", MB_OK | MB_ICONINFORMATION);
        return;
    }

    BACKUP_FILES       = _IS_CHECKED(IDC_CHECKBOX_BACKUP);
    REMOVE_CONTENT_DIR = _IS_CHECKED(IDC_CHECKBOX_REMOVE_LIB_FOLDER);

    BOOL* selected = _ALLOC_ARRAY(BOOL, LIB_COUNT);
    for (int i = 0; i < LIB_COUNT; i++) {
        selected[i] = TRUE;  // All selected by default
    }

    batch_removal_dialog_data dialog_data = {.libraries             = LIBRARIES,
                                             .lib_count             = LIB_COUNT,
                                             .selected              = selected,
                                             .backup_files          = BACKUP_FILES,
                                             .remove_library_folder = REMOVE_CONTENT_DIR,
                                             .selected_count        = LIB_COUNT};

    INT_PTR result = DialogBoxParam(GetModuleHandle(NULL),
                                    MAKEINTRESOURCE(IDD_BATCH_REMOVEBOX),
                                    hwnd,
                                    batch_remove_dialog_proc,
                                    (LPARAM)&dialog_data);

    if (result == IDREMOVE_BATCH) {
        BACKUP_FILES       = dialog_data.backup_files;
        REMOVE_CONTENT_DIR = dialog_data.remove_library_folder;

        int removed_count = 0;
        int failed_count  = 0;

        for (int i = 0; i < dialog_data.lib_count; i++) {
            if (dialog_data.selected[i]) {
                BOOL removed = remove_library(&LIBRARIES[i], REMOVE_CONTENT_DIR);
                if (removed) {
                    removed_count++;
                } else {
                    failed_count++;
                    _ERROR("Failed to remove library: '%s'", LIBRARIES[i]);
                }
            }
        }

        BOOL query_result = query_libraries(hwnd);

        char result_msg[256];
        if (failed_count == 0) {
            sprintf_s(result_msg, sizeof(result_msg), "Successfully removed %d library(ies).", removed_count);
            MessageBox(hwnd, result_msg, "Success", MB_OK | MB_ICONINFORMATION);
        } else {
            sprintf_s(result_msg,
                      sizeof(result_msg),
                      "Removed %d library(ies).\n%d failed.\n\nCheck K8-LRT.log for details.",
                      removed_count,
                      failed_count);
            MessageBox(hwnd, result_msg, "Partial Success", MB_OK | MB_ICONWARNING);
        }

        if (!query_result) {
            MessageBox(hwnd,
                       "Failed to query libraries.\n\nCheck 'K8-LRT.log' for details.",
                       "Error",
                       MB_OK | MB_ICONERROR);
        }

        EnableWindow(H_REMOVE_BUTTON, FALSE);
        SELECTED_INDEX = -1;
    }
}

void on_relocate_selected(HWND hwnd) {
    const char* content_dir = LIBRARIES[SELECTED_INDEX].content_dir;

    // 1. Copy library to new directory
    // 2. Delete original library directory
    // 3. Update registry key
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
    int response =
      MessageBox(hwnd, "Clear found libraries and search again?", "Confirm Reload", MB_YESNO | MB_ICONQUESTION);
    if (response == IDYES) {
        BOOL query_result = query_libraries(hwnd);
        if (!query_result) {
            MessageBox(hwnd, "Failed to query libraries. Are you admin?", "Error querying", MB_OK | MB_ICONERROR);
        }
    }
}

void on_exit(HWND hwnd) {
    int response = MessageBox(hwnd, "Are you sure you want exit?", "Confirm Exit", MB_YESNO | MB_ICONQUESTION);
    if (response == IDYES) {
        PostQuitMessage(0);
    }
}

void on_about(HWND hwnd) {
    const char* version = VER_PRODUCTVERSION_STR;
    DialogBoxParam(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, about_dialog_proc, (LPARAM)version);
}

//====================================================================//
//                      -- WINDOW CALLBACK --                         //
//====================================================================//

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

    log_init("K8-LRT.log");

    // Initialize common controls
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    arena_init(_PROGRAM_MEMORY);
    enable_backup_privilege();

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

    HWND hwnd = CreateWindowEx(0,
                               _WINDOW_CLASS,
                               _WINDOW_TITLE,
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               win_x,
                               win_y,
                               _WINDOW_W,
                               _WINDOW_H,
                               NULL,
                               NULL,
                               h_instance,
                               NULL);

    if (hwnd == NULL) {
        return 1;
    }

    ShowWindow(hwnd, n_cmd_show);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

#ifndef NDEBUG
    if (FreeConsole()) {
        ATTACHED_TO_CONSOLE = FALSE;
        _LOG("Detached debug console");
    }
#endif

    log_close();
    arena_destroy();

    return 0;
}