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
// automatic check for updates https://api.github.com/repos/mmozeiko/TwitchNotify/git/refs/heads/master
// parsing json with proper parser http://zserge.com/jsmn.html

#pragma warning (disable : 4204 4711 4710 4820)

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
#define WM_TWITCH_NOTIFY_ALREADY_RUNNING (WM_USER + 2)
#define WM_TWITCH_NOTIFY_REMOVE_USERS    (WM_USER + 3)
#define WM_TWITCH_NOTIFY_ADD_USER        (WM_USER + 4)
#define WM_TWITCH_NOTIFY_UPDATE_USER     (WM_USER + 5)

#define CMD_OPEN_HOMEPAGE       1
#define CMD_TOGGLE_ACTIVE       2
#define CMD_USE_LIVESTREAMER    3
#define CMD_EDIT_LIVESTREAMERRC 4
#define CMD_EDIT_CONFIG_FILE    5
#define CMD_QUIT                255

#define TWITCH_NOTIFY_CONFIG L"TwitchNotify.txt"
#define TWITCH_NOTIFY_TITLE L"Twitch Notify"

#define MAX_USER_COUNT 128
#define MAX_USER_NAME_LENGTH 256
#define MAX_GAME_NAME_LENGTH 128
#define MAX_WINDOWS_ICON_SIZE 256

#define CHECK_USERS_TIMER_INTERVAL 60 // seconds
#define RELOAD_CONFIG_TIMER_DELAY 100 // msec

#define UPDATE_USERS_TIMER_ID  1
#define RELOAD_CONFIG_TIMER_ID 2

#define MAX_ICON_CACHE_AGE (60*60*24*7) // 1 week

struct User
{
    WCHAR name[MAX_USER_NAME_LENGTH];
    int online;
};

struct UserStatusChange
{
    WCHAR game[MAX_GAME_NAME_LENGTH];
    HICON icon;
    int online;
};

static struct User gUsers[MAX_USER_COUNT];
static int gUserCount;

static int gUseLivestreamer;
static int gActive;
static int gLastPopupUserIndex = -1;

static HWND gWindow;
static HINTERNET gInternet;
static IWICImagingFactory* gWicFactory;

static HANDLE gUpdateEvent;
static HANDLE gConfigEvent;

static WCHAR gExeFolder[MAX_PATH + 1];

static char gDownloadData[1024 * 1024];
static DWORD gDownloadLength;

static UINT WM_TASKBARCREATED;

int _fltused;

#pragma function ("memset")
void* memset(void* dst, int value, size_t count)
{
    __stosb((BYTE*)dst, (BYTE)value, count);
    return dst;
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

static void ShowUserOnlineNotification(int index, struct UserStatusChange* status)
{
    WCHAR title[1024];
    wnsprintfW(title, _countof(title), L"'%s' just went live!", gUsers[index].name);

    WCHAR message[1024];

    if (status->game[0] == 0)
    {
        StrCpyNW(message, L"Playing unknown game", _countof(message));
    }
    else
    {
        wsprintfW(message, L"Playing '%s'", status->game);
    }

    gLastPopupUserIndex = index;
    ShowNotification(message, title, NIIF_USER, status->icon);
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

static void ToggleActive(HWND window)
{
    if (gActive)
    {
        KillTimer(window, UPDATE_USERS_TIMER_ID);
        for (int i = 0; i < gUserCount; i++)
        {
            gUsers[i].online = 0;
        }
    }
    else
    {
        UINT_PTR timer = SetTimer(window, UPDATE_USERS_TIMER_ID, CHECK_USERS_TIMER_INTERVAL * 1000, NULL);
        Assert(timer);
        SetEvent(gUpdateEvent);
    }
    gActive = !gActive;
}

static int IsLivestreamerInPath(void)
{
    WCHAR livestreamer[MAX_PATH];
    return FindExecutableW(L"livestreamer.exe", NULL, livestreamer) > (HINSTANCE)32;
}

static void ToggleLivestreamer(void)
{
    gUseLivestreamer = !gUseLivestreamer;
    if (gUseLivestreamer)
    {
        if (!IsLivestreamerInPath())
        {
            MessageBoxW(NULL, L"Cannot find 'livestreamer.exe' in PATH!",
                TWITCH_NOTIFY_TITLE, MB_ICONEXCLAMATION);

            gUseLivestreamer = 0;
        }
    }
}

static void EditLivestreamerConfig(void)
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

static void EditConfigFile(void)
{
    WCHAR path[MAX_PATH];
    StrCpyNW(path, gExeFolder, _countof(path));
    PathAppendW(path, TWITCH_NOTIFY_CONFIG);

    ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
}

static void AddTrayIcon(HWND window)
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
}

static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
            Assert(WM_TASKBARCREATED);

            AddTrayIcon(window);
            ToggleActive(window);
            SetEvent(gConfigEvent);
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

        case WM_POWERBROADCAST:
        {
            if (wparam == PBT_APMRESUMEAUTOMATIC)
            {
                if (gActive)
                {
                    ToggleActive(window);
                    ToggleActive(window);
                }
            }
            return 0;
        }

        case WM_TWITCH_NOTIFY_COMMAND:
        {
            if (lparam == WM_RBUTTONUP)
            {
                HMENU users = CreatePopupMenu();
                Assert(users);

                for (int i = 0; i < gUserCount; i++)
                {
                    AppendMenuW(users, gUsers[i].online ? MF_CHECKED : MF_STRING, (i + 1) << 8, gUsers[i].name);
                }

                HMENU menu = CreatePopupMenu();
                Assert(menu);

#ifdef TWITCH_NOTIFY_VERSION
                AppendMenuW(menu, MF_STRING, CMD_OPEN_HOMEPAGE, L"Twitch Notify (" TWITCH_NOTIFY_VERSION ")");
#else
                AppendMenuW(menu, MF_STRING, CMD_OPEN_HOMEPAGE, L"Twitch Notify");
#endif
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, gActive ? MF_CHECKED : MF_UNCHECKED, CMD_TOGGLE_ACTIVE, L"Active");
                AppendMenuW(menu, gUseLivestreamer ? MF_CHECKED : MF_UNCHECKED, CMD_USE_LIVESTREAMER, L"Use livestreamer");
                AppendMenuW(menu, gUseLivestreamer ? MF_STRING : MF_GRAYED, CMD_EDIT_LIVESTREAMERRC, L"Edit livestreamerrc file");
                AppendMenuW(menu, MF_STRING, CMD_EDIT_CONFIG_FILE, L"Modify user list");

                if (gUserCount == 0)
                {
                    AppendMenuW(menu, MF_GRAYED, 0, L"No users found");
                }
                else
                {
                    AppendMenuW(menu, MF_POPUP, (UINT_PTR)users, L"Users");
                }

                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, CMD_QUIT, L"Exit");

                POINT mouse;
                GetCursorPos(&mouse);

                SetForegroundWindow(window);
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, mouse.x, mouse.y, 0, window, NULL);
                switch (cmd)
                {
                case CMD_OPEN_HOMEPAGE:
                    ShellExecuteW(NULL, L"open", L"https://github.com/mmozeiko/TwitchNotify/", NULL, NULL, SW_SHOWNORMAL);
                    break;
                case CMD_TOGGLE_ACTIVE:
                    ToggleActive(window);
                    break;
                case CMD_USE_LIVESTREAMER:
                    ToggleLivestreamer();
                    break;
                case CMD_EDIT_LIVESTREAMERRC:
                    EditLivestreamerConfig();
                    break;
                case CMD_EDIT_CONFIG_FILE:
                    EditConfigFile();
                    break;
                case CMD_QUIT:
                    DestroyWindow(window);
                    break;
                default:
                    OpenTwitchUser((cmd >> 8) - 1);
                    break;
                }

                DestroyMenu(menu);
                DestroyMenu(users);
            }
            else if (lparam == NIN_BALLOONUSERCLICK)
            {
                if (gLastPopupUserIndex != -1)
                {
                    OpenTwitchUser(gLastPopupUserIndex);
                    gLastPopupUserIndex = -1;
                }
            }
            break;
        }

        case WM_TWITCH_NOTIFY_REMOVE_USERS:
        {
            gUserCount = 0;
            return 0;
        }
        
        case WM_TWITCH_NOTIFY_ADD_USER:
        {
            if (gUserCount < _countof(gUsers))
            {
                struct User* user = gUsers + gUserCount++;
                StrCpyNW(user->name, (PCWSTR)wparam, _countof(user->name));
                user->online = 0;
            }
            return 0;
        }

        case WM_TWITCH_NOTIFY_UPDATE_USER:
        {
            int index = (int)wparam;
            struct UserStatusChange* status = (void*)lparam;

            if (index < gUserCount)
            {
                struct User* user = gUsers + index;
                user->online = status->online;

                if (user->online)
                {
                    ShowUserOnlineNotification(index, status);
                }
            }

            if (status->icon)
            {
                DestroyIcon(status->icon);
            }

            return 0;
        }

        case WM_TWITCH_NOTIFY_ALREADY_RUNNING:
        {
            gLastPopupUserIndex = -1;
            ShowNotification(TWITCH_NOTIFY_TITLE L" is already running!", NULL, NIIF_INFO, NULL);
            return 0;
        }

        case WM_TIMER:
        {
            if (wparam == UPDATE_USERS_TIMER_ID)
            {
                SetEvent(gUpdateEvent);
            }
            else if (wparam == RELOAD_CONFIG_TIMER_ID)
            {
                SetEvent(gConfigEvent);
                KillTimer(window, RELOAD_CONFIG_TIMER_ID);
            }
            return 0;
        }

        default:
            if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED)
            {
                AddTrayIcon(window);
                return 0;
            }
            break;
    }

    return DefWindowProcW(window, msg, wparam, lparam);
}

static int DownloadURL(WCHAR* url, WCHAR* headers, DWORD headersLength)
{
    gDownloadLength = 0;

    HINTERNET connection = InternetOpenUrlW(gInternet, url, headers, headersLength,
        INTERNET_FLAG_EXISTING_CONNECT | INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_SECURE, 0);
    if (!connection)
    {
        return 0;
    }

    int result = 1;
    for (;;)
    {
        DWORD available = _countof(gDownloadData) - gDownloadLength;
        DWORD read;
        if (InternetReadFile(connection, gDownloadData + gDownloadLength, available, &read))
        {
            if (read == 0)
            {
                break;
            }
            gDownloadLength += read;
        }
        else
        {
            result = 0;
            break;
        }
    }
    InternetCloseHandle(connection);
    return result;
}

// http://www.isthe.com/chongo/tech/comp/fnv/
static UINT64 GetFnv1Hash(void* ptr, size_t size)
{
    BYTE* bytes = ptr;
    UINT64 hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; i++)
    {
        hash *= 1099511628211ULL;
        hash ^= bytes[i];
    }
    return hash;
}

static int GetUserIconCachePath(WCHAR* path, DWORD size, UINT64 hash)
{
    DWORD length = GetTempPathW(size, path);
    if (length)
    {
        wnsprintfW(path + length, size - length, L"%016I64x.ico", hash);
        return 1;
    }
    return 0;
}

static void SaveIconToCache(UINT64 hash, UINT width, UINT height, BYTE* pixels)
{
    WCHAR path[MAX_PATH];
    if (!GetUserIconCachePath(path, _countof(path), hash))
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
        .bWidth = (BYTE)(width == MAX_WINDOWS_ICON_SIZE ? 0 : width),
        .bHeight = (BYTE)(height == MAX_WINDOWS_ICON_SIZE ? 0 : height),
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

    for (UINT y = 0; y < height / 2; y++)
    {
        DWORD* row0 = (DWORD*)(pixels + y * width * 4);
        DWORD* row1 = (DWORD*)(pixels + (height - 1 - y) * width * 4);
        for (UINT x = 0; x < width; x++)
        {
            DWORD tmp = *row0;
            *row0++ = *row1;
            *row1++ = tmp;
        }
    }

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

static HICON DecodeIconAndSaveToCache(UINT64 hash, void* data, DWORD length)
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

                        if (w > MAX_WINDOWS_ICON_SIZE || h > MAX_WINDOWS_ICON_SIZE)
                        {
                            if (w > MAX_WINDOWS_ICON_SIZE)
                            {
                                h = h * MAX_WINDOWS_ICON_SIZE / w;
                                w = MAX_WINDOWS_ICON_SIZE;
                            }
                            if (h > MAX_WINDOWS_ICON_SIZE)
                            {
                                h = MAX_WINDOWS_ICON_SIZE;
                                w = w * MAX_WINDOWS_ICON_SIZE / h;
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
                                    icon = CreateIcon(NULL, w, h, 1, 32, NULL, pixels);
                                    SaveIconToCache(hash, w, h, pixels);
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

static HICON LoadUserIconFromCache(UINT64 hash)
{
    WCHAR path[MAX_PATH];
    if (!GetUserIconCachePath(path, _countof(path), hash))
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

static HICON GetUserIcon(char* url, size_t urlLength)
{
    UINT64 hash = GetFnv1Hash(url, urlLength);
    HICON icon = LoadUserIconFromCache(hash);
    if (icon)
    {
        return icon;
    }

    WCHAR* wurl = _alloca((urlLength + 1) * sizeof(WCHAR));
    int wurlLength = MultiByteToWideChar(CP_UTF8, 0, url, (int)urlLength, wurl, (int)urlLength + 1);
    wurl[wurlLength] = 0;

    if (!DownloadURL(wurl, NULL, 0))
    {
        return NULL;
    }

    return DecodeIconAndSaveToCache(hash, gDownloadData, gDownloadLength);
}

static int FindSubstring(char* data, size_t dataLength, char* strstart, char chend, char** resultbegin, char** resultend)
{
    int strstartLength = (int)strlen(strstart);
    char* start = data;
    while (start + strstartLength < data + dataLength)
    {
        if (StrCmpNA(start, strstart, strstartLength) == 0)
        {
            break;
        }
        start++;
    }

    if (start + strstartLength < data + dataLength)
    {
        start += strstartLength;

        char* end = start;
        while (end < data + dataLength)
        {
            if (*end == chend)
            {
                break;
            }
            end++;
        }

        if (end < data + dataLength && *end == chend)
        {
            *resultbegin = start;
            *resultend = end;
            return 1;
        }
    }
    return 0;
}

static int StringBeginsWith(void* mem1, size_t size1, void* mem2, size_t size2)
{
    return size1 >= size2 && RtlCompareMemory(mem1, mem2, size2) == size2;
}

static int ParseUserData(int index, char* data, size_t dataLength)
{
    static char offline[] = "{\"stream\":null,";
    static char online[] = "{\"stream\":{\"_id\":";
    static char error[] = "{\"error\":\"";

    const struct User* user = gUsers + index;

    if (StringBeginsWith(data, dataLength, offline, _countof(offline) - 1))
    {
        if (user->online == 1)
        {
            struct UserStatusChange status =
            {
                .online = 0,
            };
            SendMessageW(gWindow, WM_TWITCH_NOTIFY_UPDATE_USER, (WPARAM)index, (LPARAM)&status);
        }
    }
    else if (StringBeginsWith(data, dataLength, online, _countof(online) - 1))
    {
        if (user->online == 0)
        {
            struct UserStatusChange status =
            {
                .online = 1,
            };

            char* gameStart;
            char* gameEnd;
            if (FindSubstring(data, dataLength, ",\"game\":\"", '"', &gameStart, &gameEnd))
            {
                int wlength = MultiByteToWideChar(CP_UTF8, 0, gameStart, (int)(gameEnd - gameStart),
                    status.game, _countof(status.game) - 1);
                status.game[wlength] = 0;

                // convert \uXXXX to unicode chars
                {
                    WCHAR* write = status.game;
                    WCHAR* read = status.game;
                    while (*read && read - status.game + 6 < _countof(status.game))
                    {
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
                }
            }

            char* logoStart;
            char* logoEnd;
            if (FindSubstring(data, dataLength, ",\"logo\":\"", '"', &logoStart, &logoEnd))
            {
                status.icon = GetUserIcon(logoStart, logoEnd - logoStart);
            }

            SendMessageW(gWindow, WM_TWITCH_NOTIFY_UPDATE_USER, (WPARAM)index, (LPARAM)&status);
        }
    }
    else if (StringBeginsWith(data, dataLength, error, _countof(error) - 1))
    {
        // user doesn't exist?
    }
    else
    {
        return 0;
    }

    return 1;
}

static int UpdateUsers(void)
{
    for (int index = 0; index < gUserCount; index++)
    {
        WCHAR url[300];
        wnsprintfW(url, _countof(url), L"https://api.twitch.tv/kraken/streams/%s", gUsers[index].name);

        static WCHAR headers[] = L"Client-ID: q35d4ta5iafud6yhnp8a23cj2etweq6\r\n\r\n";

        if (DownloadURL(url, headers, _countof(headers) - 1))
        {
            if (!ParseUserData(index, gDownloadData, gDownloadLength))
            {
                return 0;
            }
        }
        else
        {
            return 0;
        }
    }
    return 1;
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

        char* name = data + begin;
        int nameLength = end - begin;
        if (nameLength)
        {
            WCHAR wname[MAX_USER_NAME_LENGTH];
            int wlength = MultiByteToWideChar(CP_UTF8, 0, name, nameLength, wname, _countof(wname));
            wname[wlength] = 0;

            SendMessageW(gWindow, WM_TWITCH_NOTIFY_ADD_USER, (WPARAM)wname, 0);
        }

        if (end + 1 < size && data[end] == '\r' && data[end + 1] == '\n')
        {
            ++end;
        }

        begin = end + 1;
    }
}

static void ReloadUsers(void)
{
    SendMessageW(gWindow, WM_TWITCH_NOTIFY_REMOVE_USERS, 0, 0);

    WCHAR config[MAX_PATH];
    wnsprintfW(config, MAX_PATH, L"%s\\" TWITCH_NOTIFY_CONFIG, gExeFolder);

    HANDLE file = CreateFileW(config, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
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
                    LoadUsers(data, size);
                    UnmapViewOfFile(data);
                }
                CloseHandle(mapping);
            }
        }
        CloseHandle(file);
    }
}

static DWORD UpdateThread(LPVOID arg)
{
    (void)arg;

    for (;;)
    {
        HANDLE handles[] = { gUpdateEvent, gConfigEvent };
        DWORD wait = WaitForMultipleObjects(_countof(handles), handles, FALSE, INFINITE);

        if (wait == WAIT_OBJECT_0)
        {
            if (!UpdateUsers())
            {
                gLastPopupUserIndex = -1;
                ShowNotification(L"Failed to connect to Twitch!", NULL, NIIF_ERROR, NULL);
            }
        }
        else if (wait == WAIT_OBJECT_0 + 1)
        {
            ReloadUsers();
            SetEvent(gUpdateEvent);
        }
    }
}

static DWORD ConfigThread(LPVOID arg)
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
                }
                SetTimer(gWindow, RELOAD_CONFIG_TIMER_ID, RELOAD_CONFIG_TIMER_DELAY, NULL);
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
}

static void SetupWIC(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
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
    if (IsLivestreamerInPath())
    {
        gUseLivestreamer = 1;
    }

    gInternet = InternetOpenW(L"TwitchNotify", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    Assert(gInternet);

    gUpdateEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    Assert(gUpdateEvent);

    gConfigEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    Assert(gConfigEvent);

    gWindow = CreateWindowExW(0, wc.lpszClassName, TWITCH_NOTIFY_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, NULL, NULL, wc.hInstance, NULL);
    Assert(gWindow);

    HANDLE update = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    Assert(update);

    HANDLE config = CreateThread(NULL, 0, ConfigThread, NULL, 0, NULL);
    Assert(config);

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
