#pragma warning (push, 0)
#include <windows.h>
#include <shlobj.h>
#include <wininet.h>
#include <strsafe.h>
#pragma warning (pop)

#pragma warning (disable : 4204 4711 4710)

#ifdef _DEBUG

#define Assert(cond) do \
{                       \
    if (!(cond))        \
    {                   \
        __debugbreak(); \
    }                   \
} while (0)

#else

#define Assert(cond) ((void)(cond))

#endif

#define WM_TWITCH_NOTIFY_COMMAND         (WM_USER + 1)
#define WM_TWITCH_NOTIFY_INTERNET_DATA   (WM_USER + 2)
#define WM_TWITCH_NOTIFY_ALREADY_RUNNING (WM_USER + 3)

#define TWITCH_NOTIFY_CONFIG L"TwitchNotify.txt"
#define TWITCH_NOTIFY_TITLE L"Twitch Notify"

#define MAX_USERS 128
#define MAX_NAMELEN 256

#define DEFAULT_INTERVAL_SECONDS 60
#define ONLINE_CHECK_TIMER_ID 1
#define RELOAD_CONFIG_TIMER_ID 2

struct User
{
    WCHAR name[MAX_NAMELEN];
    int online;
}
static gUsers[MAX_USERS];
static int gUserCount;

static int gReloadingConfig;

static int gUseLivestreamer;
static int gActive;
static int gLastPopupUserIndex = -1;

static HWND gWindow;
static WCHAR gExeFolder[MAX_PATH + 1];
static DWORD gExeFolderLength;

static char gInternetData[1024 * 1024];
static int gInternetDataLength;

static HINTERNET gInternet;
static HINTERNET gConnection;

static int gOnlineCheckIndex = -1;

#pragma function ("memset")
void* memset(void* dst, int value, size_t count)
{
    __stosb((BYTE*)dst, (BYTE)value, count);
    return dst;
}

static int StringsAreEqual(void* str1, size_t size1, void* str2, size_t size2)
{
    return size1 == size2 && RtlCompareMemory(str1, str2, size1) == size1;
}

static int StringBeginsWith(void* mem1, size_t size1, void* mem2, size_t size2)
{
    return size1 >= size2 && RtlCompareMemory(mem1, mem2, size2) == size2;
}

static void ShowNotification(LPWSTR message)
{
    NOTIFYICONDATAW data =
    {
        .cbSize = sizeof(data),
        .hWnd = gWindow,
        .uFlags = NIF_INFO,
        .dwInfoFlags = NIIF_INFO,
    };
    StringCchCopyW(data.szInfo, _countof(data.szInfo), message);
    StringCchCopyW(data.szInfoTitle, _countof(data.szInfoTitle), TWITCH_NOTIFY_TITLE);

    BOOL ret = Shell_NotifyIconW(NIM_MODIFY, &data);
    Assert(ret);
}

static void AddUser(char* name, int size)
{
    struct User* user = gUsers + gUserCount++;
    int count = MultiByteToWideChar(CP_UTF8, 0, name, size, user->name, _countof(user->name));
    Assert(count);

    user->name[min(count, _countof(user->name) - 1)] = 0;
    user->online = 0;
}

static void LoadUsers(char* data, int size)
{
    int begin = 0;
    while (begin < size)
    {
        int end = begin;
        while (end < size && data[end] != '\n' && data[end] != '\r')
        {
            ++end;
        }

        if (gUserCount == _countof(gUsers))
        {
            MessageBoxW(NULL, L"Config file contains too many users. Truncating!",
                TWITCH_NOTIFY_TITLE, MB_ICONINFORMATION);
            return;
        }
        AddUser(data + begin, end - begin);
        if (end + 1 < size && data[end] == '\r' && data[end + 1] == '\n')
        {
            ++end;
        }

        begin = end + 1;
    }
}

static int LoadConfig(WCHAR* folder, int folderLength)
{
    int result = 0;

    WCHAR config[MAX_PATH];
    StringCchCopyNW(config, _countof(config), folder, folderLength);
    StringCchCatW(config, _countof(config), L"\\" TWITCH_NOTIFY_CONFIG);

    HANDLE file = CreateFileW(config, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD size = GetFileSize(file, NULL);
        if (size)
        {
            HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, size, NULL);
            if (mapping)
            {
                char* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size);
                if (data)
                {
                    result = 1;
                    gUserCount = 0;
                    LoadUsers(data, size);
                    UnmapViewOfFile(data);
                }

                CloseHandle(mapping);
            }
        }

        CloseHandle(file);
    }

    return result;
}

static void OpenTwitchUser(int index)
{
    if (index < 0 || index >= gUserCount)
    {
        return;
    }

    WCHAR url[300] = L"https://www.twitch.tv/";
    StringCchCatW(url, _countof(url), gUsers[index].name);

    if (gUseLivestreamer && gUsers[index].online)
    {
        ShellExecuteW(NULL, L"open", L"livestreamer.exe", url, NULL, SW_HIDE);
    }
    else
    {
        ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    }
}

static int StartNextUserCheck(void)
{
    if (gOnlineCheckIndex >= gUserCount)
    {
        return 0;
    }

    WCHAR url[300] = L"https://api.twitch.tv/kraken/streams/";
    StringCchCatW(url, _countof(url), gUsers[gOnlineCheckIndex].name);

    InternetOpenUrlW(gInternet, url, NULL, 0,
        INTERNET_FLAG_EXISTING_CONNECT | INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_SECURE, (DWORD_PTR)gInternet);
    if (GetLastError() != ERROR_IO_PENDING)
    {
        gLastPopupUserIndex = -1;
        ShowNotification(L"Failed to connect to Twitch!");
        return 0;
    }

    return 1;
}

void CALLBACK InternetCallback(HINTERNET internet, DWORD_PTR context, DWORD status, LPVOID info, DWORD length)
{
    (void)internet;
    (void)length;
    (void)context;

    if (status == INTERNET_STATUS_HANDLE_CREATED)
    {
        HINTERNET* connection = info;
        gConnection = *connection;
        gInternetDataLength = 0;
    }
    else if (status == INTERNET_STATUS_REQUEST_COMPLETE)
    {
        INTERNET_ASYNC_RESULT* result = info;
        if (result->dwResult)
        {
            PostMessageW(gWindow, WM_TWITCH_NOTIFY_INTERNET_DATA, 0, 0);
        }
        else
        {
            ShowNotification(L"Failed to connect to Twitch!");
        }
    }
    else if (status == INTERNET_STATUS_HANDLE_CLOSING)
    {
        gConnection = NULL;
    }
}

static void StartInternetCheck(void)
{
    if (gConnection == NULL)
    {
        gOnlineCheckIndex = 0;
        StartNextUserCheck();
    }
}

static void ParseInternetData(void)
{
    static char offline[] = "{\"stream\":null,";
    static char online[] = "{\"stream\":{\"_id\":";

    if (StringBeginsWith(gInternetData, gInternetDataLength, offline, _countof(offline) - 1))
    {
        gUsers[gOnlineCheckIndex].online = 0;
    }
    else if (StringBeginsWith(gInternetData, gInternetDataLength, online, _countof(online) - 1))
    {
        if (gUsers[gOnlineCheckIndex].online == 0)
        {
            // TODO: load and show logo in notification, URL starts with "logo":"

            WCHAR message[1024];
            wsprintfW(message, L"'%s' just went live!", gUsers[gOnlineCheckIndex].name);
            gLastPopupUserIndex = gOnlineCheckIndex;
            ShowNotification(message);

            gUsers[gOnlineCheckIndex].online = 1;
        }
    }
}

static void ReceiveInternetData(void)
{
    INTERNET_BUFFERSW buffer =
    {
        .dwStructSize = sizeof(buffer),
        .lpvBuffer = gInternetData + gInternetDataLength,
        .dwBufferLength = _countof(gInternetData) - gInternetDataLength,
    };

    for (;;)
    {
        BOOL ok = InternetReadFileExW(gConnection, &buffer, IRF_ASYNC, 0);
        if (!ok)
        {
            if (GetLastError() == ERROR_IO_PENDING)
            {
                return;
            }
            InternetCloseHandle(gConnection);
            return;
        }

        gInternetDataLength += buffer.dwBufferLength;
        if (buffer.dwBufferLength == 0 || gInternetDataLength == _countof(gInternetData))
        {
            // too much data received, truncating
            break;
        }
    }

    ParseInternetData();

    InternetCloseHandle(gConnection);
    gOnlineCheckIndex++;
    StartNextUserCheck();
}

static void ToggleActive(HWND window)
{
    if (gActive)
    {
        KillTimer(window, ONLINE_CHECK_TIMER_ID);
    }
    else
    {
        StartInternetCheck();

        UINT_PTR timer = SetTimer(window, ONLINE_CHECK_TIMER_ID, DEFAULT_INTERVAL_SECONDS * 1000, NULL);
        Assert(timer);
    }
    gActive = !gActive;
}

static void FindLivestreamer(void)
{
    WCHAR livestreamer[MAX_PATH];
    if (FindExecutableW(L"livestreamer.exe", NULL, livestreamer) > (HINSTANCE)32)
    {
        gUseLivestreamer = 1;
    }
    else
    {
        gUseLivestreamer = 0;
    }
}

static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            NOTIFYICONDATAW data =
            {
                .cbSize = sizeof(data),
                .hWnd = window,
                .uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
                .uCallbackMessage = WM_TWITCH_NOTIFY_COMMAND,
                .hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)),
            };
            Assert(data.hIcon);
            StringCchCopyW(data.szTip, _countof(data.szTip), TWITCH_NOTIFY_TITLE);

            BOOL ret = Shell_NotifyIconW(NIM_ADD, &data);
            Assert(ret);

            ToggleActive(window);
            return 0;
        }

        case WM_DESTROY:
        {
            NOTIFYICONDATAW data =
            {
                .cbSize = sizeof(data),
                .hWnd = window,
            };
            BOOL ret = Shell_NotifyIconW(NIM_DELETE, &data);
            Assert(ret);

            PostQuitMessage(0);
            return 0;
        }

        case WM_TWITCH_NOTIFY_COMMAND:
        {
            if (lparam == WM_RBUTTONUP)
            {
                HMENU users = NULL;
                HMENU menu = CreatePopupMenu();
                Assert(menu);
                AppendMenuW(menu, gActive ? MF_CHECKED : MF_UNCHECKED, 1, L"Active");
                AppendMenuW(menu, gUseLivestreamer ? MF_CHECKED : MF_UNCHECKED, 2, L"Use livestreamer");
                AppendMenuW(menu, gUseLivestreamer ? MF_STRING : MF_GRAYED, 3, L"Edit livestreamerrc file");

                if (gUserCount == 0)
                {
                    AppendMenuW(menu, MF_GRAYED, 0, L"No users found");
                }
                else
                {
                    users = CreatePopupMenu();
                    Assert(users);
                    for (int i = 0; i < gUserCount; i++)
                    {
                        AppendMenuW(users, (gUsers[i].online ? MF_CHECKED : MF_UNCHECKED),(i + 1) << 8, gUsers[i].name);
                    }
                    AppendMenuW(menu, MF_POPUP, (UINT_PTR)users, L"Users");
                }
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, 255, L"Exit");

                POINT mouse;
                GetCursorPos(&mouse);

                SetForegroundWindow(window);
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, mouse.x, mouse.y, 0, window, NULL);
                if (cmd == 1)
                {
                    ToggleActive(window);
                }
                else if (cmd == 2)
                {
                    gUseLivestreamer = !gUseLivestreamer;
                    if (gUseLivestreamer)
                    {
                        FindLivestreamer();
                        if (!gUseLivestreamer)
                        {
                            MessageBoxW(NULL, L"Cannot find 'livestreamer.exe' in PATH!",
                                TWITCH_NOTIFY_TITLE, MB_ICONEXCLAMATION);
                        }
                    }
                }
                else if (cmd == 3)
                {
                    WCHAR path[MAX_PATH];

                    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path);
                    Assert(SUCCEEDED(hr));

                    StringCchCatW(path, _countof(path), L"\\livestreamer");

                    DWORD attrib = GetFileAttributesW(path);

                    if (attrib == INVALID_FILE_ATTRIBUTES
                        && !CreateDirectoryW(path, NULL))
                    {
                        MessageBoxW(NULL, L"Cannot create livestreamer configuration directory!",
                            TWITCH_NOTIFY_TITLE, MB_ICONERROR);
                    }

                    StringCchCatW(path, _countof(path), L"\\livestreamerrc");

                    ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
                }
                else if (cmd == 255)
                {
                    DestroyWindow(window);
                }
                else
                {
                    int idx = (cmd >> 8) - 1;
                    OpenTwitchUser(idx);
                }

                DestroyMenu(menu);
                if (users)
                {
                    DestroyMenu(users);
                }
                return 0;
            }
            else if (lparam == NIN_BALLOONUSERCLICK)
            {
                if (gLastPopupUserIndex != -1)
                {
                    OpenTwitchUser(gLastPopupUserIndex);
                    gLastPopupUserIndex = -1;
                }
                return 0;
            }
            else if (lparam == NIN_BALLOONTIMEOUT)
            {
                gLastPopupUserIndex = -1;
            }
            break;
        }

        case WM_TWITCH_NOTIFY_INTERNET_DATA:
        {
            ReceiveInternetData();
            return 0;
        }

        case WM_TWITCH_NOTIFY_ALREADY_RUNNING:
        {
            ShowNotification(TWITCH_NOTIFY_TITLE L" is already running!");
            return 0;
        }

        case WM_TIMER:
        {
            if (wparam == ONLINE_CHECK_TIMER_ID)
            {
                StartInternetCheck();
            }
            else if (wparam == RELOAD_CONFIG_TIMER_ID)
            {
                if (LoadConfig(gExeFolder, gExeFolderLength))
                {
                    KillTimer(window, RELOAD_CONFIG_TIMER_ID);
                    gReloadingConfig = 0;
                    StartInternetCheck();
                }
            }
            return 0;
        }
    }

    return DefWindowProcW(window, msg, wparam, lparam);
}

static DWORD ConfigNotifyThread(LPVOID arg)
{
    (void)arg;

    HANDLE handle = CreateFileW(gExeFolder, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    Assert(handle != INVALID_HANDLE_VALUE);

    for (;;)
    {
        char buffer[4096];
        DWORD read;
        BOOL ret = ReadDirectoryChangesW(handle, buffer, _countof(buffer), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, &read, NULL, NULL);
        Assert(ret);

        FILE_NOTIFY_INFORMATION* info = (void*)buffer;

        while ((char*)info + sizeof(*info) <= buffer + read)
        {
            if (StringsAreEqual(info->FileName, info->FileNameLength,
                TWITCH_NOTIFY_CONFIG, sizeof(TWITCH_NOTIFY_CONFIG) - sizeof(WCHAR)))
            {
                if (info->Action == FILE_ACTION_REMOVED)
                {
                    ShowNotification(L"Config file '" TWITCH_NOTIFY_CONFIG L"' deleted!");
                    gUserCount = 0;
                }
                else if (!gReloadingConfig)
                {
                    gReloadingConfig = 1;
                    SetTimer(gWindow, RELOAD_CONFIG_TIMER_ID, 100, NULL);
                }
            }

            if (info->NextEntryOffset == 0)
            {
                break;
            }

            info = (void*)((char*)info + info->NextEntryOffset);
        }
    }
}

static void FindExeFolder(HMODULE instance)
{
    gExeFolderLength = GetModuleFileNameW(instance, gExeFolder, _countof(gExeFolder));
    while (gExeFolderLength > 0 && gExeFolder[gExeFolderLength] != L'\\')
    {
        --gExeFolderLength;
    }
    gExeFolder[gExeFolderLength] = 0;

    if (!LoadConfig(gExeFolder, gExeFolderLength))
    {
        MessageBoxW(NULL, L"Cannot load '" TWITCH_NOTIFY_CONFIG L"' config file!", TWITCH_NOTIFY_TITLE, MB_ICONERROR);
    }
}

void WinMainCRTStartup(void)
{
    WNDCLASSEXW wc =
    {
        .cbSize = sizeof(wc),
        .lpfnWndProc = WindowProc,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"TwitchNotifyWindowClass",
    };

    HWND existing = FindWindowW(wc.lpszClassName, NULL);
    if (existing)
    {
        PostMessageW(existing, WM_TWITCH_NOTIFY_ALREADY_RUNNING, 0, 0);
        ExitProcess(0);
    }

    ATOM atom = RegisterClassExW(&wc);
    Assert(atom);

    FindExeFolder(wc.hInstance);
    FindLivestreamer();

    gInternet = InternetOpenW(L"TwitchNotify", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, INTERNET_FLAG_ASYNC);
    Assert(gInternet);

    InternetSetStatusCallback(gInternet, InternetCallback);

    gWindow = CreateWindowExW(0, wc.lpszClassName, TWITCH_NOTIFY_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, NULL, NULL, wc.hInstance, NULL);
    Assert(gWindow);

    HANDLE thread = CreateThread(NULL, 0, ConfigNotifyThread, NULL, 0, NULL);
    Assert(thread);

    for (;;)
    {
        MSG msg;
        BOOL ret = GetMessageW(&msg, NULL, 0, 0);
        if (ret == 0)
        {
            ExitProcess(0);
        }
        Assert(ret > 0);

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
