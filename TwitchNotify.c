#define INITGUID
#define COBJMACROS

#pragma warning (disable : 4204 4711 4710 4820)
#pragma warning (push, 0)

#include <windows.h>
#include <strsafe.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wininet.h>
#include <wincodec.h>

#include "jsmn.c" // https://github.com/zserge/jsmn
#include "jsmn_iterator.c" // https://github.com/zserge/jsmn/pull/69

#pragma warning (pop)

// TODO
// automatic check for updates https://api.github.com/repos/mmozeiko/TwitchNotify/git/refs/heads/master

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
#define WM_TWITCH_NOTIFY_START_UPDATE    (WM_USER + 5)
#define WM_TWITCH_NOTIFY_USER_ONLINE     (WM_USER + 6)
#define WM_TWITCH_NOTIFY_END_UPDATE      (WM_USER + 7)
#define WM_TWITCH_NOTIFY_UPDATE_ERROR    (WM_USER + 8)

#define CMD_OPEN_HOMEPAGE       1
#define CMD_TOGGLE_ACTIVE       2
#define CMD_USE_LIVESTREAMER    3
#define CMD_EDIT_LIVESTREAMERRC 4
#define CMD_EDIT_CONFIG_FILE    5
#define CMD_QUIT                255

#define TWITCH_NOTIFY_CONFIG L"TwitchNotify.txt"
#define TWITCH_NOTIFY_TITLE L"Twitch Notify"

#define MAX_USER_COUNT        100 // max user count that can be requested in one Twitch API call
#define MAX_USER_NAME_LENGTH  256
#define MAX_GAME_NAME_LENGTH  128
#define MAX_WINDOWS_ICON_SIZE 256

#define MAX_DOWNLOAD_SIZE (1024 * 1024) // 1 MiB

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

struct UserStatusOnline
{
    WCHAR user[MAX_GAME_NAME_LENGTH];
    WCHAR game[MAX_GAME_NAME_LENGTH];
    HICON icon;
};

static struct User gUsers[MAX_USER_COUNT];
static int gUserCount;

static int gUseLivestreamer;
static int gActive;
static int gLastPopupUserIndex = -1;

static HANDLE gHeap;
static HWND gWindow;
static HINTERNET gInternet;
static IWICImagingFactory* gWicFactory;

static HANDLE gUpdateEvent;
static HANDLE gConfigEvent;

static WCHAR gExeFolder[MAX_PATH + 1];

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

static void ShowUserOnlineNotification(struct UserStatusOnline* status)
{
    WCHAR title[1024];
    wnsprintfW(title, _countof(title), L"'%s' just went live!", status->user);

    WCHAR message[1024];

    if (status->game[0] == 0)
    {
        StrCpyNW(message, L"Playing unknown game", _countof(message));
    }
    else
    {
        wnsprintfW(message, _countof(message), L"Playing '%s'", status->game);
    }

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

static void OpenHomePage(void)
{
    ShellExecuteW(NULL, L"open", L"https://github.com/mmozeiko/TwitchNotify/", NULL, NULL, SW_SHOWNORMAL);
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
                    OpenHomePage();
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
            else if (lparam == WM_LBUTTONDBLCLK)
            {
                OpenHomePage();
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

        case WM_TWITCH_NOTIFY_START_UPDATE:
        {
            for (int i = 0; i < gUserCount; i++)
            {
                gUsers[i].online = -gUsers[i].online;
            }
            return 0;
        }

        case WM_TWITCH_NOTIFY_USER_ONLINE:
        {
            struct UserStatusOnline* status = (void*)wparam;

            for (int i = 0; i < gUserCount; i++)
            {
                struct User* user = gUsers + i;
                if (StrCmpW(user->name, status->user) == 0)
                {
                    if (user->online == 0)
                    {
                        gLastPopupUserIndex = i;
                        ShowUserOnlineNotification(status);
                    }
                    user->online = 1;
                    break;
                }
            }

            if (status->icon)
            {
                DestroyIcon(status->icon);
            }
            return 0;
        }

        case WM_TWITCH_NOTIFY_END_UPDATE:
        {
            for (int i = 0; i < gUserCount; i++)
            {
                // -1 means user was online, now is offline
                //  0 means user was and now is offline
                // +1 means user now is online
                gUsers[i].online = gUsers[i].online > 0;
            }
            return 0;
        }

        case WM_TWITCH_NOTIFY_UPDATE_ERROR:
        {
            gLastPopupUserIndex = -1;
            ShowNotification((LPWSTR)wparam, (LPWSTR)lparam, NIIF_ERROR, NULL);
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

static int DownloadURL(WCHAR* url, WCHAR* headers, char* data, DWORD* dataLength)
{
    HINTERNET connection = InternetOpenUrlW(gInternet, url, headers, (DWORD)-1,
        INTERNET_FLAG_EXISTING_CONNECT | INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_SECURE, 0);
    if (!connection)
    {
        return 0;
    }

    DWORD downloaded = 0;
    DWORD available = *dataLength;
    int result = 1;
    for (;;)
    {
        DWORD read;
        if (InternetReadFile(connection, data + downloaded, available, &read))
        {
            if (read == 0)
            {
                break;
            }
            downloaded += read;
        }
        else
        {
            result = read == 0;
            break;
        }
    }
    InternetCloseHandle(connection);
    *dataLength = downloaded;
    return result;
}

// http://www.isthe.com/chongo/tech/comp/fnv/
static UINT64 GetFnv1Hash(BYTE* bytes, size_t size)
{
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
                            Assert(w <= MAX_WINDOWS_ICON_SIZE && h <= MAX_WINDOWS_ICON_SIZE);

                            DWORD stride = w * 4;
                            DWORD size = w * h * 4;
                            BYTE pixels[MAX_WINDOWS_ICON_SIZE * MAX_WINDOWS_ICON_SIZE * 4];
                            if (SUCCEEDED(IWICBitmapFrameDecode_CopyPixels(source, NULL, stride, size, pixels)))
                            {
                                icon = CreateIcon(NULL, w, h, 1, 32, NULL, pixels);
                                SaveIconToCache(hash, w, h, pixels);
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

static HICON GetUserIcon(WCHAR* url, int urlLength, char* downloadBuffer)
{
    UINT64 hash = GetFnv1Hash((BYTE*)url, urlLength * sizeof(WCHAR));
    HICON icon = LoadUserIconFromCache(hash);
    if (icon)
    {
        return icon;
    }

    DWORD dataLength = MAX_DOWNLOAD_SIZE;
    if (DownloadURL(url, NULL, downloadBuffer, &dataLength))
    {
        icon = DecodeIconAndSaveToCache(hash, downloadBuffer, dataLength);
    }
    return icon;
}

static int jsoneq(const char* json, jsmntok_t* token, const char* str)
{
    if (token->type != JSMN_STRING)
    {
        return 0;
    }

    const char* ptr = json + token->start;
    int length = token->end - token->start;
    while (*str)
    {
        if (length == 0 || *ptr++ != *str++)
        {
            return 0;
        }
        length--;
    }
    return length == 0;
}

static int ConverJsonStringToW(char* src, int length, WCHAR* dst, int dstLength)
{
    char* read = src;
    char* write = src;
    while (length --> 0)
    {
        char ch = *read++;
        if (ch == '\\')
        {
            if (length == 0)
            {
                break;
            }
            ch = *read++;
            --length;
            if (ch == '"') *write++ = L'"';
            else if (ch == '\\') *write++ = L'\\';
            else if (ch == '/') *write++ = L'/';
            else if (ch == 'b') *write++ = L'\b';
            else if (ch == 'f') *write++ = L'\f';
            else if (ch == 'n') *write++ = L'\n';
            else if (ch == 'r') *write++ = L'\r';
            else if (ch == 't') *write++ = L'\t';
            else if (ch == 'u')
            {
                if (length < 4)
                {
                    break;
                }

                int value;
                char tmp = read[4];
                read[-2] = '0';
                read[-1] = 'x';
                read[4] = 0;
                BOOL ok = StrToIntEx(read - 2, STIF_SUPPORT_HEX, &value);
                Assert(ok);
                read[4] = tmp;
                read += 4;
                length -= 4;

                if (value >= 0xd800 && value <= 0xdfff)
                {
                    if (length < 6 || read[0] != '\\' || read[1] != 'u')
                    {
                        break;
                    }

                    int value2;
                    tmp = read[6];
                    read[0] = '0';
                    read[1] = 'x';
                    read[6] = 0;
                    ok = StrToIntEx(read, STIF_SUPPORT_HEX, &value2);
                    Assert(ok);
                    read[6] = tmp;
                    read += 6;
                    length -= 6;
                    if (value2 < 0xdc00 || value2 >= 0xdfff)
                    {
                        break;
                    }

                    value = ((value - 0xd800) << 10) + (value2 - 0xdc00) + 0x10000;
                }

                if (value < 0x80)
                {
                    *write++ = (char)value;
                }
                else if (value < 0x800)
                {
                    *write++ = (char)(0xc0 | (value >> 6));
                    *write++ = (char)(0x80 | (value & 0x3f));
                }
                else if (value < 0x10000)
                {
                    *write++ = (char)(0xe0 | (value >> 12));
                    *write++ = (char)(0x80 | ((value >> 6) & 0x3f));
                    *write++ = (char)(0x80 | (value & 0x3f));
                }
                else
                {
                    Assert(value <= 0x10ffff);
                    *write++ = (char)(0xf0 | (value >> 18));
                    *write++ = (char)(0x80 | ((value >> 12) & 0x3f));
                    *write++ = (char)(0x80 | ((value >> 6) & 0x3f));
                    *write++ = (char)(0x80 | ((value & 0x3f)));
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            *write++ = ch;
        }
    }

    dstLength = MultiByteToWideChar(CP_UTF8, 0, src, (int)(write - src), dst, dstLength);
    dst[dstLength] = 0;
    return dstLength;
}

static int UpdateUsers(void)
{
    if (gUserCount == 0)
    {
        return 1;
    }

    DWORD dataLength = MAX_DOWNLOAD_SIZE;
    char* data = HeapAlloc(gHeap, 0, 2 * MAX_DOWNLOAD_SIZE);
    if (!data)
    {
        return 0;
    }

    WCHAR url[4096] = L"https://api.twitch.tv/kraken/streams?channel";
    for (int i = 0; i < gUserCount; i++)
    {
        StringCbCatW(url, _countof(url), i == 0 ? L"=" : L",");
        StringCbCatW(url, _countof(url), gUsers[i].name);
    }

    SendMessageW(gWindow, WM_TWITCH_NOTIFY_START_UPDATE, 0, 0);

    int result = 1;

    WCHAR* headers = L"Client-ID: q35d4ta5iafud6yhnp8a23cj2etweq6\r\n\r\n";
    if (!DownloadURL(url, headers, data, &dataLength))
    {
        SendMessageW(gWindow, WM_TWITCH_NOTIFY_UPDATE_ERROR, (WPARAM)L"Failed to connect to Twitch!", 0);
        result = 0;
    }
    else
    {
        jsmn_parser parser;
        jsmn_init(&parser);

        jsmntok_t tokens[1024];
        int t = jsmn_parse(&parser, data, dataLength, tokens, _countof(tokens));
        if (t < 1 || tokens[0].type != JSMN_OBJECT)
        {
            SendMessage(gWindow, WM_TWITCH_NOTIFY_UPDATE_ERROR, (WPARAM)L"JSON parse error!", 0);
            result = 0;
        }
        else
        {
            enum
            {
                STATE_ROOT,
                STATE_ERROR,
                STATE_STREAMS,
                STATE_STREAM,
                STATE_CHANNEL,
            }
            state = STATE_ROOT;

            jsmn_iterator_t streams = { 0 };
            jsmn_iterator_t stream = { 0 };
            jsmn_iterator_t channel = { 0 };

            jsmn_iterator_t iter;
            int ok = jsmn_iterator_init(&iter, tokens, t, 0);
            Assert(ok >= 0);

            struct UserStatusOnline status;

            for (;;)
            {
                jsmntok_t* id;
                jsmntok_t* value;

                int next = jsmn_iterator_next(&iter, &id, &value, 0);
                if (next == 0)
                {
                    if (state == STATE_CHANNEL)
                    {
                        state = STATE_STREAM;
                        iter = channel;
                        continue;
                    }
                    if (state == STATE_STREAM)
                    {
                        SendMessageW(gWindow, WM_TWITCH_NOTIFY_USER_ONLINE, (WPARAM)&status, 0);
                        state = STATE_STREAMS;
                        iter = stream;
                        continue;
                    }
                    if (state == STATE_STREAM)
                    {
                        state = STATE_ROOT;
                        iter = streams;
                        continue;
                    }
                    break;
                }
                else if (next < 0)
                {
                    SendMessage(gWindow, WM_TWITCH_NOTIFY_UPDATE_ERROR, (WPARAM)L"JSON parse error!", 0);
                    result = 0;
                    break;
                }

                if (state == STATE_CHANNEL)
                {
                    if (jsoneq(data, id, "name") && value->type == JSMN_STRING)
                    {
                        char* str = data + value->start;
                        int length = value->end - value->start;
                        ConverJsonStringToW(str, length, status.user, _countof(status.user));
                    }
                    else if (jsoneq(data, id, "logo") && value->type == JSMN_STRING)
                    {
                        char* str = data + value->start;
                        int length = value->end - value->start;

                        int urlLength = ConverJsonStringToW(str, length, url, _countof(url));
                        status.icon = GetUserIcon(url, urlLength, data + MAX_DOWNLOAD_SIZE);
                    }
                }
                else if (state == STATE_STREAM)
                {
                    if (jsoneq(data, id, "game") && value->type == JSMN_STRING)
                    {
                        char* str = data + value->start;
                        int length = value->end - value->start;
                        ConverJsonStringToW(str, length, status.game, _countof(status.game));
                    }
                    else if (jsoneq(data, id, "channel") && value->type == JSMN_OBJECT)
                    {
                        channel = iter;
                        state = STATE_CHANNEL;
                        jsmn_iterator_init(&iter, tokens, t, (int)(value - tokens));
                    }
                }
                else if (state == STATE_STREAMS && value->type == JSMN_OBJECT)
                {
                    stream = iter;
                    state = STATE_STREAM;
                    jsmn_iterator_init(&iter, tokens, t, (int)(value - tokens));

                    status.user[0] = 0;
                    status.game[0] = 0;
                    status.icon = NULL;
                }
                else if (state == STATE_ERROR)
                {
                    if (jsoneq(data, id, "message") && value->type == JSMN_STRING)
                    {
                        WCHAR message[256];
                        char* str = data + value->start;
                        int length = value->end - value->start;
                        ConverJsonStringToW(str, length, message, _countof(message));
                        SendMessage(gWindow, WM_TWITCH_NOTIFY_UPDATE_ERROR, (WPARAM)message,
                            (LPARAM)L"Error from Twitch!");
                        result = 0;
                        break;
                    }
                }
                else
                {
                    if (jsoneq(data, id, "error"))
                    {
                        state = STATE_ERROR;
                    }
                    else if (jsoneq(data, id, "streams") && value->type == JSMN_ARRAY)
                    {
                        streams = iter;
                        state = STATE_STREAMS;
                        jsmn_iterator_init(&iter, tokens, t, (int)(value - tokens));
                    }
                }
            }
        }
    }

    SendMessageW(gWindow, WM_TWITCH_NOTIFY_END_UPDATE, 0, 0);

    HeapFree(gHeap, 0, data);
    return result;
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
            UpdateUsers();
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

    gHeap = GetProcessHeap();
    Assert(gHeap);

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
