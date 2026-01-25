/*
    K8-LRT - v1.0.0 - Library removal tool for Bobdule's Kontakt 8

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

        1.0.0  (TBD)        UI redux, added functionality, updates
        0.3.1  (2026-01-23) memory model improvements
        0.3.0  (2026-01-23) sweeping code changes, bug fixes, and logging
        0.2.0  (2026-01-23) tons of bug fixes and code improvements
        0.1.0  (2026-01-22) initial release of K8-LRT
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
    errno_t result = fopen_s(&LOG_FILE, filename, "a+");  // append (+ read) mode
    if (result == 0) {
        log_msg(LOG_INFO, "--- K8-LRT Started ---");
    } else {
        MessageBox(hwnd, "Failed to initialize logger.", "Fatal", MB_OK | MB_ICONERROR);
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

static HWND H_LISTBOX           = NULL;
static HWND H_REMOVE_BUTTON     = NULL;
static HWND H_REMOVE_ALL_BUTTON = NULL;
static HWND H_BACKUP_CHECKBOX   = NULL;  // Whether or not we should backup delete filesa
static HWND H_LOG_VIEWER        = NULL;

//====================================================================//
//                          -- GLOBALS --                             //
//====================================================================//

#define _WINDOW_W 300
#define _WINDOW_H 390
#define _WINDOW_CLASS "K8LRT_WindowClass\0"
#define _WINDOW_TITLE "K8-LRT - v" VER_PRODUCTVERSION_STR

#define _LOGVIEW_W 600
#define _LOGVIEW_H 400
#define _LOGVIEW_CLASS "K8LRT_LogView\0"
#define _LOGVIEW_TITLE "Log\0"

#define _STREQ(s1, s2) strcmp(s1, s2) == 0
#define _IS_CHECKED(checkbox) (SendMessage(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED)
#define _MAX_LIB_COUNT 512
#define _LIB_CACHE_ROOT "Native Instruments\\Kontakt 8\\LibrariesCache\0"
#define _DB3_ROOT "Native Instruments\\Kontakt 8\\komplete.db3\0"

static char** LIBRARIES    = {NULL};
static int LIB_COUNT       = 0;
static int SELECTED_INDEX  = -1;
static BOOL BACKUP_FILES   = TRUE;
static BOOL INITIAL_SEARCH = TRUE;

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

// TODO: Update these as more are found
static char* EXCLUSION_PATTERNS[] = {
  "Universal Audio*",
  "u-he*",
  "Waves*",
  NULL,
};

#define _GITHUB_OWNER "jakerieger\0"
#define _GITHUB_REPO "K8-LRT\0"

//====================================================================//
//                      -- HELPER FUNCTIONS --                        //
//====================================================================//

#define _NOT_IMPLEMENTED()                                                                                             \
    do {                                                                                                               \
        fprintf(stderr, "NOT IMPLEMENTED");                                                                            \
        quick_exit(1);                                                                                                 \
    } while (FALSE)

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

BOOL matches_pattern_in_list(char* patterns[], const char* needle) {
    for (char** p = patterns; *p != NULL; p++) {
        if (PathMatchSpec(needle, *p)) {
            return TRUE;
        }
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

BOOL query_libraries(HWND hwnd) {
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

        if (!list_contains(EXCLUSION_LIST, key) && !matches_pattern_in_list(EXCLUSION_PATTERNS, key)) {
            LIBRARIES[LIB_COUNT] = _strdup(key);
            SendMessage(H_LISTBOX, LB_INSERTSTRING, LIB_COUNT, (LPARAM)LIBRARIES[LIB_COUNT]);
            LIB_COUNT++;
        }

        free(key);
        found_keys[i] = NULL;
    }
    LIBRARIES[LIB_COUNT] = NULL;

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

BOOL remove_selected_library(void) {
    if (SELECTED_INDEX == -1)
        return FALSE;

    return remove_library(LIBRARIES[SELECTED_INDEX]);
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

//====================================================================//
//                      -- INPUT CALLBACKS --                         //
//====================================================================//

LRESULT on_create(HWND hwnd) {
    HMENU h_menubar   = CreateMenu();
    HMENU h_file_menu = CreateMenu();
    HMENU h_help_menu = CreateMenu();

    AppendMenu(h_file_menu, MF_STRING, ID_FILE_VIEW_LOG, "&View Log");
    AppendMenu(h_file_menu, MF_STRING, ID_FILE_RELOAD_LIBRARIES, "&Reload Libraries");
    AppendMenu(h_file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(h_file_menu, MF_STRING, ID_FILE_EXIT, "E&xit");

    AppendMenu(h_help_menu, MF_STRING, ID_HELP_CHECK_UPDATES, "&Check for Updates");
    AppendMenu(h_help_menu, MF_STRING, ID_HELP_ABOUT, "&About");

    AppendMenu(h_menubar, MF_POPUP, (UINT_PTR)h_file_menu, "&File");
    AppendMenu(h_menubar, MF_POPUP, (UINT_PTR)h_help_menu, "&Help");

    SetMenu(hwnd, h_menubar);

    H_LISTBOX = CreateWindowEx(WS_EX_CLIENTEDGE,
                               "LISTBOX",
                               NULL,
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                               10,
                               36,
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
                                       260,
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
                                         290,
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
                                     290,
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

    SendMessage(H_LISTBOX, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_REMOVE_ALL_BUTTON, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_REMOVE_BUTTON, WM_SETFONT, (WPARAM)h_font, TRUE);

    SendMessage(H_BACKUP_CHECKBOX, WM_SETFONT, (WPARAM)h_font, TRUE);
    SendMessage(H_BACKUP_CHECKBOX, BM_SETCHECK, (WPARAM)TRUE, TRUE);

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

    char label_text[256] = {'\0'};
    snprintf(label_text, 256, "Select a library to remove (found %d):", LIB_COUNT);

    HWND h_label = CreateWindow("STATIC",
                                label_text,
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                10,
                                10,
                                265,
                                20,
                                hwnd,
                                NULL,
                                GetModuleHandle(NULL),
                                NULL);
    SendMessage(h_label, WM_SETFONT, (WPARAM)h_font, TRUE);

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
            BOOL query_result = query_libraries(hwnd);
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

        BOOL query_result = query_libraries(hwnd);
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

void on_check_for_updates(HWND hwnd) {
    char* latest_version = get_latest_version();
    if (!latest_version) {
        MessageBox(hwnd, "Failed to check for updates.", "Update Check", MB_OK | MB_ICONERROR);
        return;
    }

    int comparison = compare_versions(VER_PRODUCTVERSION_STR, latest_version);
    if (comparison != 0) {
        char message[512];
        sprintf_s(message,
                  512,
                  "A new version of K8-LRT is available!\n\n"
                  "Current: %s\n"
                  "Latest: %s\n\n"
                  "Visit the GitHub releases page to download?",
                  VER_PRODUCTVERSION_STR,
                  latest_version + 1);

        int result = MessageBoxA(NULL, message, "Update Available", MB_YESNO | MB_ICONINFORMATION);

        if (result == IDYES) {
            ShellExecuteA(NULL,
                          "open",
                          "https://github.com/jakerieger/K8-LRT/releases/latest",
                          NULL,
                          NULL,
                          SW_SHOWNORMAL);
        }
    } else {
        MessageBoxA(NULL, "You're running the latest version!", "Up to Date", MB_OK | MB_ICONINFORMATION);
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
                case ID_FILE_VIEW_LOG: {
                    on_view_log(hwnd);
                    break;
                }

                case ID_FILE_RELOAD_LIBRARIES: {
                    on_reload_libraries(hwnd);
                    break;
                }

                case ID_FILE_EXIT: {
                    on_exit(hwnd);
                    break;
                }

                case ID_HELP_CHECK_UPDATES: {
                    on_check_for_updates(hwnd);
                    break;
                }

                case ID_HELP_ABOUT: {
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