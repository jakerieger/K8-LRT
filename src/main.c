#include <stdio.h>
#include <windows.h>
#include <winreg.h>

#define IDC_LISTBOX 101
#define IDC_REMOVE_BUTTON 102
#define IDC_QUIT_BUTTON 103

HWND h_listbox;
HWND h_remove_button;
HWND h_quit_button;

#define MAX_LIB_COUNT 256

char* g_libraries[MAX_LIB_COUNT];
int g_lib_count      = 0;
int g_selected_index = -1;

void reset_libraries() {
    for (int i = 0; i < g_lib_count; i++) {
        SendMessage(h_listbox, LB_DELETESTRING, i, 0);
        free(g_libraries[i]);
    }

    g_selected_index = -1;
    g_lib_count      = 0;
}

void query_ni_registry_keys(HWND hwnd) {
    reset_libraries();

    HKEY h_key;
    LPCSTR subkey = "SOFTWARE\\Native Instruments";

    LONG result = RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h_key);
    if (result != ERROR_SUCCESS) {
        if (result == ERROR_FILE_NOT_FOUND) {
            MessageBox(hwnd, "Registry key not found", "Error", MB_ICONERROR);
        } else {
            MessageBox(hwnd, "Error opening registry key", "Error", MB_ICONERROR);
        }
        return;
    }

    char ach_key[255];
    DWORD cb_name = sizeof(ach_key);
    FILETIME ft_last_write_time;
    g_lib_count = 0;

    while (RegEnumKeyExA(h_key, (DWORD)g_lib_count, ach_key, &cb_name, NULL, NULL, NULL, &ft_last_write_time) ==
           ERROR_SUCCESS) {
        if (g_lib_count > MAX_LIB_COUNT) {
            return;
        }
        g_libraries[g_lib_count] = _strdup(ach_key);
        cb_name                  = sizeof(ach_key);
        SendMessage(h_listbox, LB_INSERTSTRING, g_lib_count, (LPARAM)g_libraries[g_lib_count]);

        g_lib_count++;
    }

    RegCloseKey(h_key);
}

void remove_selected_library(HWND hwnd) {
    if (g_selected_index == -1)
        return;

    LPCSTR base_path = "SOFTWARE\\Native Instruments";
    HKEY h_key;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base_path, 0, KEY_WRITE, &h_key) == ERROR_SUCCESS) {
        LONG res = RegDeleteKeyA(h_key, g_libraries[g_selected_index]);
        if (res == ERROR_SUCCESS) {
            MessageBox(hwnd, "Library removed from registry.", "Success", MB_OK);
            reset_libraries();  // Reset UI
        } else {
            MessageBox(hwnd, "Failed to delete key. Are you Admin?", "Error", MB_ICONERROR);
        }

        RegCloseKey(h_key);
    }
}

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
                                       250,
                                       hwnd,
                                       (HMENU)IDC_LISTBOX,
                                       GetModuleHandle(NULL),
                                       NULL);

            h_quit_button = CreateWindowEx(WS_EX_CLIENTEDGE,
                                           "BUTTON",
                                           "Quit",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           10,
                                           280,
                                           130,
                                           30,
                                           hwnd,
                                           (HMENU)IDC_QUIT_BUTTON,
                                           GetModuleHandle(NULL),
                                           NULL);

            h_remove_button = CreateWindowEx(WS_EX_CLIENTEDGE,
                                             "BUTTON",
                                             "Remove Library",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
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

            SendMessage(h_listbox, WM_SETFONT, (WPARAM)h_font, TRUE);
            SendMessage(h_quit_button, WM_SETFONT, (WPARAM)h_font, TRUE);
            SendMessage(h_remove_button, WM_SETFONT, (WPARAM)h_font, TRUE);

            // Search registry for key entries in `HKEY_LOCAL_MACHINE/SOFTWARE/Native Instruments/..`
            query_ni_registry_keys(hwnd);

            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDC_QUIT_BUTTON:
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    break;

                case IDC_REMOVE_BUTTON: {
                    if (g_selected_index != -1) {
                        char message[512];
                        sprintf_s(message,
                                  sizeof(message),
                                  "Are you sure you want to permanently remove '%s' from the registry?",
                                  g_libraries[g_selected_index]);

                        int response =
                          MessageBox(hwnd, message, "Confirm Removal", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

                        if (response == IDYES) {
                            remove_selected_library(hwnd);
                        }
                    } else {
                        MessageBox(hwnd, "Please select a library first.", "No Selection", MB_OK);
                    }

                    break;
                }

                case IDC_LISTBOX: {
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        int sel = (int)SendMessage(h_listbox, LB_GETCURSEL, 0, 0);
                        if (sel != LB_ERR) {
                            g_selected_index = sel;
                        }
                    }

                    break;
                }
            }

            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hwnd, umsg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE h_prev_instance, LPSTR lp_cmd_line, int n_cmd_show) {
    // create_and_attach_console();

    const char CLASS_NAME[] = "K8RemovalWindowClass";

    WNDCLASS wc      = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = h_instance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

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

    return 0;
}