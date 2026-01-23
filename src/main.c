#include <stdio.h>
#include <windows.h>
#include <winerror.h>
#include <winnt.h>
#include <winreg.h>
#include <shlobj.h>
#include <Shlwapi.h>
#include <winuser.h>

//====================================================================//
//                   -- UI ELEMENT DEFINITIONS --                     //
//====================================================================//

#define IDC_LISTBOX 101
#define IDC_REMOVE_BUTTON 102
#define IDC_REMOVE_ALL_BUTTON 103
#define IDC_CHECKBOX_BACKUP 104

#define IDI_APPICON 501

HWND h_listbox;
HWND h_remove_button;
HWND h_remove_all_button;
HWND h_backup_checkbox;  // Whether or not we should backup delete files

//====================================================================//
//                          -- GLOBALS --                             //
//====================================================================//

#define STREQ(s1, s2) strcmp(s1, s2) == 0
#define MAX_LIB_COUNT 256

char* g_libraries[MAX_LIB_COUNT];
int g_lib_count      = 0;
int g_selected_index = -1;
BOOL g_backup_files  = TRUE;

#define LIB_CACHE_ROOT "Native Instruments\\Kontakt 8\\LibrariesCache"
#define DB3_ROOT "Native Instruments\\Kontakt 8\\komplete.db3"

// Exclusion list - NI products that aren't libraries
const char* EXCLUSION_LIST[] = {
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
        return _strdup(path);
    } else {
        fprintf(stderr, "Failed to retrieve path. Error code: 0x%08X\n", (UINT)hr);
    }

    return NULL;
}

char* join_str(const char* prefix, const char* suffix) {
    const size_t prefix_len = strlen(prefix);
    const size_t suffix_len = strlen(suffix);
    const size_t needed     = prefix_len + suffix_len + 1;

    char* buffer = (char*)malloc(needed);
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
    free(prefix);
    return full;
}

void attach_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f_dummy;
        freopen_s(&f_dummy, "CONOUT$", "w", stdout);
        freopen_s(&f_dummy, "CONOUT$", "w", stderr);
    }
}

BOOL file_exists(const char* path) {
    DWORD dw_attrib = GetFileAttributesA(path);
    return (dw_attrib != INVALID_FILE_ATTRIBUTES && !(dw_attrib & FILE_ATTRIBUTE_DIRECTORY));
}

BOOL list_contains(const char* haystack[], const char* needle) {
    for (const char** p = haystack; *p != NULL; p++) {
        if (STREQ(*p, needle))
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
    }
}

void reset_libraries() {
    for (int i = 0; i < MAX_LIB_COUNT; i++) {
        if (g_libraries[i] != NULL) {
            SendMessage(h_listbox, LB_DELETESTRING, i, 0);
            free(g_libraries[i]);
            g_libraries[i] = NULL;
        }
    }

    g_selected_index = -1;
    g_lib_count      = 0;
}

BOOL query_libraries() {
    reset_libraries();

    HKEY h_key;
    LPCSTR subkey = "SOFTWARE\\Native Instruments";

    LONG result = RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h_key);
    if (result != ERROR_SUCCESS) {
        return FALSE;
    }

    char ach_key[255];
    DWORD cb_name = sizeof(ach_key);
    FILETIME ft_last_write_time;

    DWORD found = 0;
    char* found_keys[MAX_LIB_COUNT];

    while (RegEnumKeyExA(h_key, found, ach_key, &cb_name, NULL, NULL, NULL, &ft_last_write_time) == ERROR_SUCCESS) {
        if (found > MAX_LIB_COUNT) {
            RegCloseKey(h_key);
            return FALSE;
        }

        found_keys[found] = _strdup(ach_key);
        cb_name           = sizeof(ach_key);
        found++;
    }
    RegCloseKey(h_key);
    found_keys[found] = NULL;

    // Filter found keys
    g_lib_count = 0;
    for (DWORD i = 0; i < found; i++) {
        char* key = found_keys[i];

        if (!list_contains(EXCLUSION_LIST, key)) {
            // add key to libs
            g_libraries[g_lib_count] = _strdup(key);
            SendMessage(h_listbox, LB_INSERTSTRING, g_lib_count, (LPARAM)g_libraries[g_lib_count]);
            g_lib_count++;
        }

        free(key);
        found_keys[i] = NULL;
    }
    g_libraries[g_lib_count] = NULL;

    return TRUE;
}

BOOL remove_selected_library() {
    if (g_selected_index == -1)
        return FALSE;

    LPCSTR base_path = "SOFTWARE\\Native Instruments";
    HKEY h_key;

    // 1. Remove registry key
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base_path, 0, KEY_ALL_ACCESS, &h_key) == ERROR_SUCCESS) {
        if (g_backup_files) {
            HKEY h_subkey;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                              join_paths(base_path, g_libraries[g_selected_index]),
                              0,
                              KEY_READ,
                              &h_subkey) == ERROR_SUCCESS) {
                char* reg_filename = join_str(g_libraries[g_selected_index], ".reg");
                DeleteFileA(reg_filename);
                LONG backup_res = RegSaveKeyExA(h_subkey, reg_filename, NULL, REG_LATEST_FORMAT);

                if (backup_res != ERROR_SUCCESS) {
                    return FALSE;
                }

                RegCloseKey(h_subkey);
                free(reg_filename);
            }
        }

        LONG res = RegDeleteKeyA(h_key, g_libraries[g_selected_index]);
        if (res != ERROR_SUCCESS) {
            RegCloseKey(h_key);
            return FALSE;
        }

        RegCloseKey(h_key);
    }

    // 2. Check for .xml files in `C:\Program Files\Common Files\Native Instruments\Service Center`
    char* prefix =
      join_paths("C:\\Program Files\\Common Files\\Native Instruments\\Service Center", g_libraries[g_selected_index]);
    char* filename = join_str(prefix, ".xml");

    if (file_exists(filename)) {
        if (g_backup_files) {
            char* bak_filename = join_str(prefix, ".xml.bak");
            if (!CopyFileExA(filename, bak_filename, NULL, NULL, NULL, 0)) {
                free(bak_filename);
                free(prefix);
                free(filename);
                return FALSE;
            }
            free(bak_filename);
        }

        if (!DeleteFileA(filename)) {
            free(prefix);
            free(filename);
            return FALSE;
        }
    }
    free(prefix);
    free(filename);

    // 3. Check for cache file in `~\AppData\Local\Native Instruments\Kontakt 8\LibrariesCache`
    // TODO: This one is tough. Cache files are binary formats and filenames appear to be hashes of some kind.

    // 4. Create backup of `~\AppData\Local\Native Instruments\Kontakt 8\komplete.db3` to force DB rebuild (if enabled)
    char* appdata_local = get_local_appdata_path();
    char* db3           = join_paths(appdata_local, DB3_ROOT);
    free(appdata_local);

    if (file_exists(db3)) {
        if (g_backup_files) {
            char* db3_bak = join_str(db3, ".bak");
            if (!CopyFileExA(db3, db3_bak, NULL, NULL, NULL, 0)) {
                free(db3_bak);
                free(appdata_local);
                return FALSE;
            }
            free(db3_bak);
        }

        if (!DeleteFileA(db3)) {
            free(appdata_local);
            return FALSE;
        }

        free(appdata_local);
    }

    return TRUE;
}

//====================================================================//
//                      -- WINDOW CALLBACK --                         //
//====================================================================//

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam) {
    switch (umsg) {
        case WM_CREATE: {
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

            h_listbox = CreateWindowEx(WS_EX_CLIENTEDGE,
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

            h_backup_checkbox = CreateWindowEx(0,
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

            h_remove_all_button = CreateWindowEx(WS_EX_CLIENTEDGE,
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

            h_remove_button = CreateWindowEx(WS_EX_CLIENTEDGE,
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
            SendMessage(h_listbox, WM_SETFONT, (WPARAM)h_font, TRUE);
            SendMessage(h_remove_all_button, WM_SETFONT, (WPARAM)h_font, TRUE);
            SendMessage(h_remove_button, WM_SETFONT, (WPARAM)h_font, TRUE);

            SendMessage(h_backup_checkbox, WM_SETFONT, (WPARAM)h_font, TRUE);
            SendMessage(h_backup_checkbox, BM_SETCHECK, (WPARAM)TRUE, TRUE);

            // Search registry for key entries in `HKEY_LOCAL_MACHINE/SOFTWARE/Native Instruments/..`
            if (!query_libraries()) {
                MessageBox(hwnd,
                           "Failed to query libraries. Do you have any Kontakt libraries installed?",
                           "Error",
                           MB_OK | MB_ICONERROR);
                SendMessage(hwnd, WM_CLOSE, 0, 0);
            }

            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDC_REMOVE_ALL_BUTTON: {
                    int response = MessageBox(hwnd,
                                              "Are you sure you want to permanently remove all libraries?",
                                              "Confirm Removal",
                                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

                    if (response == IDYES) {
                        g_backup_files = (SendMessage(h_backup_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        while (g_lib_count > 0) {
                            g_selected_index = 0;
                            if (remove_selected_library()) {
                                if (!query_libraries()) {
                                    MessageBox(
                                      hwnd,
                                      "Failed to query libraries. Do you have any Kontakt libraries installed?",
                                      "Error",
                                      MB_OK | MB_ICONERROR);
                                    SendMessage(hwnd, WM_CLOSE, 0, 0);
                                }
                            }
                        }
                        g_selected_index = -1;
                        EnableWindow(h_remove_button, FALSE);

                        MessageBox(hwnd, "Successfully removed all libraries.", "Success", MB_OK | MB_ICONINFORMATION);
                    }

                    break;
                }

                case IDC_REMOVE_BUTTON: {
                    if (g_selected_index != -1) {
                        char message[512];
                        sprintf_s(message,
                                  sizeof(message),
                                  "Are you sure you want to permanently remove '%s'?",
                                  g_libraries[g_selected_index]);

                        int response =
                          MessageBox(hwnd, message, "Confirm Removal", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

                        if (response == IDYES) {
                            g_backup_files = (SendMessage(h_backup_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                            if (remove_selected_library()) {
                                if (!query_libraries()) {
                                    MessageBox(
                                      hwnd,
                                      "Failed to query libraries. Do you have any Kontakt libraries installed?",
                                      "Error",
                                      MB_OK | MB_ICONERROR);
                                    SendMessage(hwnd, WM_CLOSE, 0, 0);
                                }
                                EnableWindow(h_remove_button, FALSE);

                                MessageBox(hwnd,
                                           "Successfully removed library.",
                                           "Success",
                                           MB_OK | MB_ICONINFORMATION);
                            }
                        }
                    }

                    break;
                }

                case IDC_LISTBOX: {
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        int sel = (int)SendMessage(h_listbox, LB_GETCURSEL, 0, 0);
                        if (sel != LB_ERR) {
                            g_selected_index = sel;
                        }
                        EnableWindow(h_remove_button, (sel != LB_ERR));
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

    enable_backup_privilege();

    const char CLASS_NAME[] = "K8RemovalWindowClass";

    WNDCLASS wc      = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = h_instance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(h_instance, MAKEINTRESOURCE(IDI_APPICON));

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(WS_EX_TOOLWINDOW,
                               CLASS_NAME,
                               "Kontakt 8 Library Removal Tool",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
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

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

#ifndef NDEBUG
    FreeConsole();
#endif

    return 0;
}