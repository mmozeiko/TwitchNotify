#define INITGUID
#define COBJMACROS
#pragma warning (push, 0)
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wininet.h>
#include <wincodec.h>
#pragma warning (pop)

// TODO
// check for updates https://api.github.com/repos/mmozeiko/TwitchNotify/commits/master
// parsing json with proper parser http://zserge.com/jsmn.html

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

#define CMD_OPEN_HOMEPAGE       1
#define CMD_TOGGLE_ACTIVE       2
#define CMD_USE_LIVESTREAMER    3
#define CMD_EDIT_LIVESTREAMERRC 4
#define CMD_EDIT_CONFIG_FILE    5

#define TWITCH_NOTIFY_CONFIG L"TwitchNotify.txt"
#define TWITCH_NOTIFY_TITLE L"Twitch Notify"

#define MAX_USERS 128
#define MAX_NAMELEN 256
#define MAX_ICON_SIZE 256

#define DEFAULT_INTERVAL_SECONDS 60
#define ONLINE_CHECK_TIMER_ID 1
#define RELOAD_CONFIG_TIMER_ID 2

#define MAX_ICON_CACHE_AGE (60*60*24*7) // 1 week

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

static char gInternetData[1024 * 1024];
static int gInternetDataLength;

static HINTERNET gInternet;
static HINTERNET gConnection;

static int gOnlineCheckIndex = -1;
static int gDownloadingImage;
static UINT64 gImageHash;
static WCHAR gGameName[128];

static IWICImagingFactory* gWicFactory;

int _fltused;

#pragma function ("memset")
void* memset(void* dst, int value, size_t count)
{
    __stosb((BYTE*)dst, (BYTE)value, count);
    return dst;
}

static int StringBeginsWith(void* mem1, size_t size1, void* mem2, size_t size2)
{
    return size1 >= size2 && RtlCompareMemory(mem1, mem2, size2) == size2;
}

static void ShowNotification(LPWSTR message, LPWSTR title, DWORD flags, HICON icon)
{
    NOTIFYICONDATAW data =
    {
        .cbSize = sizeof(data),
        .hWnd = gWindow,
        .uFlags = NIF_INFO,
        .dwInfoFlags = flags | (icon ? NIIF_LARGE_ICON : 0),
        .hBalloonIcon = icon,

    };
    StrCpyNW(data.szInfo, message, _countof(data.szInfo));
    StrCpyNW(data.szInfoTitle, title ? title : TWITCH_NOTIFY_TITLE, _countof(data.szInfoTitle));

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

static int LoadConfig(WCHAR* folder)
{
    int result = 0;

    WCHAR config[MAX_PATH];
    wnsprintfW(config, MAX_PATH, L"%s\\" TWITCH_NOTIFY_CONFIG, folder);

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

    WCHAR url[300];
    wnsprintfW(url, _countof(url), L"https://www.twitch.tv/%s", gUsers[index].name);

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
    if (gConnection != NULL)
    {
        gConnection = NULL;
        InternetCloseHandle(gConnection);
    }
    if (++gOnlineCheckIndex >= gUserCount)
    {
        return 0;
    }
    gDownloadingImage = 0;

    WCHAR url[300];
    wnsprintfW(url, _countof(url), L"https://api.twitch.tv/kraken/streams/%s", gUsers[gOnlineCheckIndex].name);

    WCHAR headers[] = L"Client-ID: q35d4ta5iafud6yhnp8a23cj2etweq6\r\n\r\n";

    InternetOpenUrlW(gInternet, url, headers, _countof(headers) - 1,
        INTERNET_FLAG_EXISTING_CONNECT | INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_SECURE, (DWORD_PTR)gInternet);
    if (GetLastError() != ERROR_IO_PENDING)
    {
        ShowNotification(L"Failed to connect to Twitch!", NULL, NIIF_ERROR, NULL);
        return 0;
    }

    return 1;
}

static void StartInternetCheck(void)
{
    if (gConnection == NULL)
    {
        gOnlineCheckIndex = -1;
        StartNextUserCheck();
    }
}

static void ShowTwitchUserIsLiveNotification(HICON icon)
{
    gLastPopupUserIndex = gOnlineCheckIndex;

    WCHAR title[1024];
    wnsprintfW(title, _countof(title), L"'%s' just went live!", gUsers[gOnlineCheckIndex].name);

    WCHAR message[1024];

    if (gGameName[0] == 0)
    {
        StrCpyNW(message, L"Playing unknown game", _countof(message));
    }
    else
    {
        WCHAR* write = gGameName;
        WCHAR* read = gGameName;
        while (*read && read - gGameName + 6 < _countof(gGameName))
        {
            // convert \uXXXX to unicode char
            if (read[0] == '\\' && read[1] == 'u')
            {
                int value;
                WCHAR temp = read[6];
                read[0] = L'0';
                read[1] = L'x';
                read[6] = 0;
                if (StrToIntExW(read, STIF_SUPPORT_HEX, &value))
                {
                    *write++ = (WCHAR)value;
                }
                else
                {
                    *write++ = L'?';
                }
                read[6] = temp;
                read += 6;
            }
            else
            {
                *write++ = *read++;
            }
        }
        *write++ = 0;

        wsprintfW(message, L"Playing '%s'", gGameName);
    }
    ShowNotification(message, title, NIIF_USER, icon);
}

static void StartImageDownload(char* url, int length)
{
    gDownloadingImage = 1;

    WCHAR* wurl = _alloca((length + 1) * sizeof(WCHAR));
    int wlength = MultiByteToWideChar(CP_UTF8, 0, url, length, wurl, length + 1);
    wurl[wlength] = 0;

    InternetOpenUrlW(gInternet, wurl, NULL, 0,
        INTERNET_FLAG_EXISTING_CONNECT | INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_SECURE, (DWORD_PTR)gInternet);
    if (GetLastError() != ERROR_IO_PENDING)
    {
        ShowTwitchUserIsLiveNotification(NULL);
        StartNextUserCheck();
    }
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
            ShowNotification(L"Failed to connect to Twitch!", NULL, NIIF_ERROR, NULL);
        }
    }
    else if (status == INTERNET_STATUS_HANDLE_CLOSING)
    {
        gConnection = NULL;
    }
}

static int GetCachePath(WCHAR* path, DWORD size, UINT64 hash)
{
    DWORD length = GetTempPathW(size, path);
    if (length)
    {
        wnsprintfW(path + length, size - length, L"%016I64x.ico", hash);
        return 1;
    }
    return 0;
}

static void SaveToCache(UINT64 hash, UINT width, UINT height, BYTE* pixels)
{
    WCHAR path[MAX_PATH];
    if (!GetCachePath(path, _countof(path), hash))
    {
        return;
    }

    DWORD size = width * height * 4;

    struct ICONDIR
    {
        WORD idReserved;
        WORD  idType;
        WORD idCount;
    } dir = {
        .idType = 1,
        .idCount = 1,
    };

    struct ICONDIRENTRY
    {
        BYTE bWidth;
        BYTE bHeight;
        BYTE bColorCount;
        BYTE bReserved;
        WORD wPlanes;
        WORD wBitCount;
        DWORD dwBytesInRes;
        DWORD dwImageOffset;
    } entry = {
        .bWidth = (BYTE)(width == MAX_ICON_SIZE ? 0 : width),
        .bHeight = (BYTE)(height == MAX_ICON_SIZE ? 0 : height),
        .wPlanes = 1,
        .wBitCount = 32,
        .dwBytesInRes = sizeof(BITMAPINFOHEADER) + size + size/32,
        .dwImageOffset = sizeof(dir) + sizeof(entry),
    };

    BITMAPINFOHEADER bmp =
    {
        .biSize = sizeof(bmp),
        .biWidth = width,
        .biHeight = 2 * height,
        .biPlanes = 1,
        .biBitCount = 32,
        .biSizeImage = size + size / 32,
    };

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, &dir, sizeof(dir), &written, NULL);
        WriteFile(file, &entry, sizeof(entry), &written, NULL);
        WriteFile(file, &bmp, sizeof(bmp), &written, NULL);
        WriteFile(file, pixels, size, &written, NULL);
        SetFilePointer(file, size / 32, NULL, FILE_CURRENT); // and-mask, 1bpp
        SetEndOfFile(file);
        CloseHandle(file);
    }
}

static HICON LoadFromCache(UINT64 hash)
{
    WCHAR path[MAX_PATH];
    if (!GetCachePath(path, _countof(path), hash))
    {
        return NULL;
    }

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data))
    {
        UINT64 lastWrite = (((UINT64)data.ftLastWriteTime.dwHighDateTime) << 32) +
            data.ftLastWriteTime.dwLowDateTime;

        FILETIME nowTime;
        GetSystemTimeAsFileTime(&nowTime);

        UINT64 now = (((UINT64)nowTime.dwHighDateTime) << 32) + nowTime.dwLowDateTime;
        UINT64 expires = lastWrite + (UINT64)MAX_ICON_CACHE_AGE * 10 * 1000 * 1000;
        if (expires < now)
        {
            return NULL;
        }
        return LoadImageW(NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    }

    return NULL;
}

static HICON CreateNotificationIcon(UINT64 hash, void* data, DWORD length)
{
    if (!gWicFactory)
    {
        return NULL;
    }

    IStream* stream = SHCreateMemStream(data, length);
    if (!stream)
    {
        return NULL;
    }

    HICON icon = NULL;
    HRESULT hr;

    IWICBitmapDecoder* decoder;
    hr = IWICImagingFactory_CreateDecoderFromStream(gWicFactory, stream, NULL, WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (SUCCEEDED(hr))
    {
        IWICBitmapFrameDecode* frame;
        hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
        if (SUCCEEDED(hr))
        {
            IWICFormatConverter* bitmap;
            hr = IWICImagingFactory_CreateFormatConverter(gWicFactory, &bitmap);
            if (SUCCEEDED(hr))
            {
                hr = IWICFormatConverter_Initialize(bitmap, (IWICBitmapSource*)frame,
                    &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0., WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    UINT w, h;
                    if (SUCCEEDED(IWICBitmapFrameDecode_GetSize(bitmap, &w, &h)))
                    {
                        IWICBitmapSource* source = NULL;

                        if (w > MAX_ICON_SIZE || h > MAX_ICON_SIZE)
                        {
                            if (w > MAX_ICON_SIZE)
                            {
                                h = h * MAX_ICON_SIZE / w;
                                w = MAX_ICON_SIZE;
                            }
                            if (h > MAX_ICON_SIZE)
                            {
                                h = MAX_ICON_SIZE;
                                w = w * MAX_ICON_SIZE / h;
                            }

                            IWICBitmapScaler* scaler;
                            hr = IWICImagingFactory_CreateBitmapScaler(gWicFactory, &scaler);
                            if (SUCCEEDED(hr))
                            {
                                hr = IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource*)bitmap, w, h,
                                    WICBitmapInterpolationModeCubic);
                                if (SUCCEEDED(hr))
                                {
                                    hr = IWICBitmapScaler_QueryInterface(scaler, &IID_IWICBitmapSource, &source);
                                }
                                IWICBitmapScaler_Release(scaler);
                            }
                        }
                        else
                        {
                            hr = IWICBitmapScaler_QueryInterface(bitmap, &IID_IWICBitmapSource, &source);
                        }

                        if (SUCCEEDED(hr))
                        {
                            Assert(source);

                            DWORD stride = w * 4;
                            DWORD size = w * h * 4;
                            BYTE* pixels = LocalAlloc(LMEM_FIXED, size);
                            if (pixels)
                            {
                                if (SUCCEEDED(IWICBitmapFrameDecode_CopyPixels(source, NULL, stride, size, pixels)))
                                {
                                    SaveToCache(hash, w, h, pixels);
                                    icon = CreateIcon(NULL, w, h, 1, 32, NULL, pixels);
                                }
                                LocalFree(pixels);
                            }
                            IWICBitmapSource_Release(source);
                        }
                    }
                }
                IWICFormatConverter_Release(bitmap);
            }
            IWICBitmapFrameDecode_Release(frame);
        }
        IWICBitmapFrameDecode_Release(decoder);
    }
    IStream_Release(stream);

    return icon;
}

static int FindSubstring(char* strStart, char chEnd, char** resultBegin, char** resultEnd)
{
    int strStartLength = (int)strlen(strStart);
    char* start = gInternetData;
    while (start + strStartLength < gInternetData + gInternetDataLength)
    {
        if (StrCmpNA(start, strStart, strStartLength) == 0)
        {
            break;
        }
        start++;
    }

    if (start + strStartLength < gInternetData + gInternetDataLength)
    {
        start += strStartLength;

        char* end = start;
        while (end < gInternetData + gInternetDataLength)
        {
            if (*end == chEnd)
            {
                break;
            }
            end++;
        }

        if (end < gInternetData + gInternetDataLength && *end == chEnd)
        {
            *resultBegin = start;
            *resultEnd = end;
            return 1;
        }
    }
    return 0;
}

static UINT64 GetFnv1Hash(BYTE* bytes, int length)
{
    UINT64 hash = 14695981039346656037ULL;
    for (int i = 0; i < length; i++)
    {
        hash *= 1099511628211ULL;
        hash ^= bytes[i];
    }
    return hash;
}

static void ParseInternetDataAndNotify(void)
{
    static char offline[] = "{\"stream\":null,";
    static char online[] = "{\"stream\":{\"_id\":";
    static char error[] = "{\"error\":\"";

    if (StringBeginsWith(gInternetData, gInternetDataLength, offline, _countof(offline) - 1))
    {
        gUsers[gOnlineCheckIndex].online = 0;
    }
    else if (StringBeginsWith(gInternetData, gInternetDataLength, online, _countof(online) - 1))
    {
        if (gUsers[gOnlineCheckIndex].online == 0)
        {
            gUsers[gOnlineCheckIndex].online = 1;

            char* gameStart;
            char* gameEnd;
            if (FindSubstring(",\"game\":\"", '"', &gameStart, &gameEnd))
            {
                int wlength = MultiByteToWideChar(CP_UTF8, 0, gameStart, (int)(gameEnd - gameStart),
                    gGameName, _countof(gGameName) - 1);
                gGameName[wlength] = 0;
            }
            else
            {
                gGameName[0] = 0;
            }

            char* logoStart;
            char* logoEnd;
            if (FindSubstring(",\"logo\":\"", '"', &logoStart, &logoEnd))
            {
                int logoBytes = (int)(logoEnd - logoStart);
                UINT64 hash = GetFnv1Hash((BYTE*)logoStart, logoBytes);

                HICON icon = LoadFromCache(hash);
                if (icon)
                {
                    ShowTwitchUserIsLiveNotification(icon);
                    StartNextUserCheck();
                    DestroyIcon(icon);
                    return;
                }

                gImageHash = hash;
                StartImageDownload(logoStart, logoBytes);
                return;
            }

            ShowTwitchUserIsLiveNotification(NULL);
        }
    }
    else if (StringBeginsWith(gInternetData, gInternetDataLength, error, _countof(error) - 1))
    {
        // user doesn't exist?
    }

    StartNextUserCheck();
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

    if (gDownloadingImage)
    {
        HICON icon = CreateNotificationIcon(gImageHash, gInternetData, gInternetDataLength);
        ShowTwitchUserIsLiveNotification(icon);
        StartNextUserCheck();
        if (icon)
        {
            DestroyIcon(icon);
        }
    }
    else
    {
        ParseInternetDataAndNotify();
    }
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
            StrCpyNW(data.szTip, TWITCH_NOTIFY_TITLE, _countof(data.szTip));

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
                
#ifdef TWITCH_NOTIFY_VERSION
                AppendMenuW(menu, MF_GRAYED, 0, L"Twitch Notify - v" TWITCH_NOTIFY_VERSION);
#else
                AppendMenuW(menu, MF_GRAYED, 0, L"Twitch Notify");
#endif
                AppendMenuW(menu, MF_STRING, CMD_OPEN_HOMEPAGE, L"Open homepage");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, gActive ? MF_CHECKED : MF_UNCHECKED, CMD_TOGGLE_ACTIVE, L"Active");
                AppendMenuW(menu, gUseLivestreamer ? MF_CHECKED : MF_UNCHECKED, CMD_USE_LIVESTREAMER, L"Use livestreamer");
                AppendMenuW(menu, gUseLivestreamer ? MF_STRING : MF_GRAYED, CMD_EDIT_LIVESTREAMERRC, L"Edit livestreamerrc file");
                AppendMenuW(menu, gUseLivestreamer ? MF_STRING : MF_GRAYED, CMD_EDIT_CONFIG_FILE, L"Modify user list");

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
                if (cmd == CMD_OPEN_HOMEPAGE)
                {
                    ShellExecuteW(NULL, L"open", L"https://github.com/mmozeiko/TwitchNotify/", NULL, NULL, SW_SHOWNORMAL);
                }
                else if (cmd == CMD_TOGGLE_ACTIVE)
                {
                    ToggleActive(window);
                }
                else if (cmd == CMD_USE_LIVESTREAMER)
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
                else if (cmd == CMD_EDIT_LIVESTREAMERRC)
                {
                    WCHAR path[MAX_PATH];
                    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path);
                    Assert(SUCCEEDED(hr));

                    PathAppendW(path, L"livestreamer");
                    if (!PathIsDirectoryW(path) && !CreateDirectoryW(path, NULL))
                    {
                        MessageBoxW(NULL, L"Cannot create livestreamer configuration directory!",
                            TWITCH_NOTIFY_TITLE, MB_ICONERROR);
                    }
                    PathAppendW(path, L"livestreamerrc");

                    ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
                }
                else if (cmd == 5)
                {
                    WCHAR path[MAX_PATH];
                    StrCpyNW(path, gExeFolder, _countof(path));
                    PathAppendW(path, TWITCH_NOTIFY_CONFIG);

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
            }
            else if (lparam == NIN_BALLOONUSERCLICK)
            {
                if (gLastPopupUserIndex != -1)
                {
                    OpenTwitchUser(gLastPopupUserIndex);
                    gLastPopupUserIndex = -1;
                }
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
            ShowNotification(TWITCH_NOTIFY_TITLE L" is already running!", NULL, NIIF_INFO, NULL);
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
                if (LoadConfig(gExeFolder))
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
            if (StrCmpNW(TWITCH_NOTIFY_CONFIG, info->FileName, info->FileNameLength) == 0)
            {
                if (info->Action == FILE_ACTION_REMOVED)
                {
                    ShowNotification(L"Config file '" TWITCH_NOTIFY_CONFIG L"' deleted!", NULL, NIIF_WARNING, NULL);
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
    DWORD length = GetModuleFileNameW(instance, gExeFolder, _countof(gExeFolder));
    while (length > 0 && gExeFolder[length] != L'\\')
    {
        --length;
    }
    gExeFolder[length] = 0;

    if (!LoadConfig(gExeFolder))
    {
        MessageBoxW(NULL, L"Cannot load '" TWITCH_NOTIFY_CONFIG L"' config file!", TWITCH_NOTIFY_TITLE, MB_ICONERROR);
    }
}

static void SetupWIC(void)
{
    HRESULT hr = CoInitializeEx(NULL, 0);
    Assert(SUCCEEDED(hr));

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, &gWicFactory);
    Assert(SUCCEEDED(hr));
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

    SetupWIC();

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
