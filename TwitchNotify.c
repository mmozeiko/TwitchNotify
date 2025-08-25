#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <stdbool.h>
#include <stdint.h>

#pragma comment (lib, "kernel32.lib")
#pragma comment (lib, "user32.lib")
#pragma comment (lib, "shell32.lib")
#pragma comment (lib, "shlwapi.lib")
#pragma comment (lib, "ole32.lib")
#pragma comment (lib, "winhttp.lib")

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) ((void)(Cond))
#endif

#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

#include "WindowsToast.h"
#include "WindowsJson.h"

// sent when tray icon is clicked
#define WM_TWITCH_NOTIFY_COMMAND (WM_USER + 1)

// sent from duplicate to original instance of application to show notification
#define WM_TWITCH_NOTIFY_ALREADY_RUNNING (WM_USER + 2)

// sent when game/stream name is downloaded
// WParam is UserId, LParam is JsonObject for response (must release it)
#define WM_TWITCH_NOTIFY_USER_STREAM (WM_USER + 6)

// sent when user info is downloaded
// WParam is JsonObject for response (must release it)
#define WM_TWITCH_NOTIFY_USER_INFO (WM_USER + 7)

// sent when user stream status is downloaded
// sent when list of followed users is downloaded
// WParam is JsonObject for response (must release it)
#define WM_TWITCH_NOTIFY_FOLLOWED_USERS (WM_USER + 9)

// command id's for popup menu items
#define CMD_OPEN_HOMEPAGE     10 // "Twitch Notify" item selected
#define CMD_USE_MPV           20 // "Use mpv" checkbox
#define CMD_QUALITY           30 // +N for Quality submenu items
#define CMD_EDIT_USERS        40 // "Edit List" in user submenu
#define CMD_DOWNLOAD_USERS    50 // "Download List" in user submenu
#define CMD_NOTIFY_ON_STARTUP 60 // Notify on startup
#define CMD_EXIT              70 // "Exit"
#define CMD_USER              80 // +N for one of users (indexed into State.Users[] array)

#define TWITCH_NOTIFY_NAME     L"Twitch Notify"
#define TWITCH_NOTIFY_INI      L"TwitchNotify.ini"
#define TWITCH_NOTIFY_APPID    L"TwitchNotify.TwitchNotify" // CompanyName.ProductName
#define TWITCH_NOTIFY_HOMEPAGE L"https://github.com/mmozeiko/TwitchNotify/"

#define MAX_USER_COUNT    100

// just arbitrary limit for strings (most of them will be shorter anyway)
#define MAX_STRING_LENGTH 256

// downloaded user profile image files will be cached
// length in seconds, 1 week
#define MAX_IMAGE_CACHE_AGE (60*60*24*7)

// WM_TIMER id's
#define TIMER_RELOAD_USERS 1
#define TIMER_STREAM_CHECK 2

// how much to delay after .ini file notification is detected
#define TIMER_RELOAD_USERS_DELAY 100 // msec

// how often to check if users are streaming
#define TIMER_STREAM_CHECK_INTERVAL (60 * 1000) // 1 minute

// youtube-dl quality names to use with mpv player
struct
{
	LPCWSTR Name;
	LPCSTR Format;
}
static const Quality[] =
{
	{ L"Source",     "best"                              },
	{ L"1080p @ 60", "best[height<=?1080]/best"          },
	{ L"1080p",      "best[height<=?1080][fps<=30]/best" },
	{ L"720p @ 60",  "best[height<=?720]/best"           },
	{ L"720p",       "best[height<=?720][fps<=?30]/best" },
	{ L"480p",       "best[height<=?480]/best"           },
	{ L"360p",       "best[height<=?360]/best"           },
	{ L"160p",       "best[height<=?160]/best"           },
};

// message sent when explorer.exe restarts, to restore tray icon
static uint32_t WM_TASKBARCREATED;

typedef struct
{
	WCHAR Name[MAX_STRING_LENGTH];        // name from .ini file, used for Twitch URL
	WCHAR DisplayName[MAX_STRING_LENGTH]; // display name user can customize
	WCHAR ImagePath[MAX_PATH];   // path to downloaded profile image

	//  0 when user id is not yet known (gql query executing)
	// -1 when gql query did not return any user (invalid username)
	// >0 for actual users
	int UserId;
	int ViewerCount;
	bool IsLive;

	void* Notification;
} User;

// global state of application
// members are modified & accessed only from main thread
// only exception is HWND Window - to post messages from background threads
struct
{
	HICON Icon;
	HWND Window;
	WindowsToast Toast;
	HINTERNET Session;
	PTP_POOL ThreadPool;
	bool Initialized; // to not show notifications on startup

	// for loading & saving .ini file
	WCHAR IniPath[MAX_PATH];
	FILETIME LastIniWriteTime;

	// global settings
	int Quality;
	bool UseMpv;
	bool NotifyOnStartup;

	// user list
	User Users[MAX_USER_COUNT];
	int UserCount;
}
static State;

// http://www.isthe.com/chongo/tech/comp/fnv/
static uint64_t GetFnv1Hash(const void* Ptr, int Size)
{
	const uint8_t* Bytes = Ptr;
	uint64_t Hash = 14695981039346656037ULL;
	for (int Index = 0; Index < Size; Index++)
	{
		Hash *= 1099511628211ULL;
		Hash ^= Bytes[Index];
	}
	return Hash;
}

// system tray icon stuff

static void AddTrayIcon(HWND Window, HICON Icon)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_TWITCH_NOTIFY_COMMAND,
		.hIcon = Icon,
	};
	StrCpyNW(Data.szInfoTitle, TWITCH_NOTIFY_NAME, ARRAYSIZE(Data.szInfoTitle));
	Assert(Shell_NotifyIconW(NIM_ADD, &Data));
}

static void RemoveTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
	};
	Assert(Shell_NotifyIconW(NIM_DELETE, &Data));
}

static void UpdateTrayIcon(HWND Window, HICON Icon)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_ICON,
		.hIcon = Icon,
	};
	Assert(Shell_NotifyIconW(NIM_MODIFY, &Data));
}

// InfoType can be: NIIF_NONE, NIIF_INFO, NIIF_WARNING, NIIF_ERROR
static void ShowTrayMessage(HWND Window, DWORD InfoType, LPCWSTR Message)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_INFO,
		.dwInfoFlags = InfoType,
		.hIcon = State.Icon,
	};
	StrCpyNW(Data.szInfo, Message, ARRAYSIZE(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, TWITCH_NOTIFY_NAME, ARRAYSIZE(Data.szInfoTitle));
	Assert(Shell_NotifyIconW(NIM_MODIFY, &Data));
}

static bool IsMpvInPath(void)
{
	WCHAR mpv[MAX_PATH];
	return FindExecutableW(L"mpv.exe", NULL, mpv) > (HINSTANCE)32;
}

static void OpenMpvUrl(LPCWSTR Url)
{
	WCHAR Args[1024];
	wsprintfW(Args, L"--profile=low-latency --ytdl-format=\"%S\" %s", Quality[State.Quality].Format, Url);

	ShellExecuteW(NULL, L"open", L"mpv.exe", Args, NULL, SW_SHOWNORMAL);
}

static void GetTwitchIcon(LPWSTR ImagePath)
{
	int TempLength = GetTempPathW(MAX_PATH, ImagePath);
	Assert(TempLength);
	wsprintfW(ImagePath + TempLength, L"twitch.png");

	// if file does not exist
	if (GetFileAttributesW(ImagePath) == INVALID_FILE_ATTRIBUTES)
	{
		// extract first icon image data from resources, it is a png file
		HRSRC Resource = FindResourceW(NULL, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(3)); // 3=RT_ICON
		if (Resource)
		{
			HGLOBAL Global = LoadResource(NULL, Resource);
			if (Global)
			{
				int ResourceSize = SizeofResource(NULL, Resource);
				LPVOID ResourceData = LockResource(Global);
				if (ResourceData && ResourceSize)
				{
					HANDLE File = CreateFileW(ImagePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
					if (File != INVALID_HANDLE_VALUE)
					{
						DWORD Written;
						WriteFile(File, ResourceData, ResourceSize, &Written, NULL);
						CloseHandle(File);
					}
				}
			}
		}
	}
}

// gql query stuff, will be called from background threads

static int DoGqlQuery(char* Query, int QuerySize, char* Buffer, int BufferSize)
{
	int ReadSize = 0;

	HINTERNET Connection = WinHttpConnect(State.Session, L"gql.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (Connection)
	{
		HINTERNET Request = WinHttpOpenRequest(Connection, L"POST", L"/gql", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
		if (Request)
		{
			WCHAR Headers[] = L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko";
			if (WinHttpSendRequest(Request, Headers, ARRAYSIZE(Headers) - 1, Query, QuerySize, QuerySize, 0) &&
				WinHttpReceiveResponse(Request, NULL))
			{
				DWORD Status = 0;
				DWORD StatusSize = sizeof(Status);
				WinHttpQueryHeaders(
					Request,
					WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX,
					&Status,
					&StatusSize,
					WINHTTP_NO_HEADER_INDEX);

				if (Status == HTTP_STATUS_OK)
				{
					while (ReadSize < BufferSize)
					{
						DWORD Read;
						if (!WinHttpReadData(Request, Buffer + ReadSize, BufferSize - ReadSize, &Read) || Read == 0)
						{
							break;
						}
						ReadSize += Read;
					}
				}
			}
			WinHttpCloseHandle(Request);
		}
		WinHttpCloseHandle(Connection);
	}

	return ReadSize;
}

// gets "unique" path on disk to image - do this by hashing image URL
// image will be placed in %TEMP% folder

static void GetImagePath(LPWSTR ImagePath, LPCWSTR ImageUrl)
{
	int  ImageUrlLength = lstrlenW(ImageUrl);
	uint64_t Hash = GetFnv1Hash(ImageUrl, ImageUrlLength * sizeof(WCHAR));

	int TempLength = GetTempPathW(MAX_PATH, ImagePath);
	Assert(TempLength);

	LPWSTR Extension = StrRChrW(ImageUrl, ImageUrl + ImageUrlLength, L'.');
	wsprintfW(ImagePath + TempLength, L"%016I64x%s", Hash, Extension);
}

// image downloading, happens in background thread

static void CALLBACK DownloadUserImageWork(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work)
{
	LPWSTR ImageUrl = (LPWSTR)Context;

	WCHAR ImagePath[MAX_PATH];
	GetImagePath(ImagePath, ImageUrl);

	WCHAR HostName[MAX_PATH];
	WCHAR UrlPath[MAX_PATH];
	URL_COMPONENTSW Url =
	{
		.dwStructSize = sizeof(Url),
		.lpszHostName = HostName,
		.lpszUrlPath = UrlPath,
		.dwHostNameLength = ARRAYSIZE(HostName),
		.dwUrlPathLength = ARRAYSIZE(UrlPath),
	};

	if (WinHttpCrackUrl(ImageUrl, 0, 0, &Url))
	{
		HINTERNET Connection = WinHttpConnect(State.Session, HostName, Url.nPort, 0);
		if (Connection)
		{
			HINTERNET Request = WinHttpOpenRequest(
				Connection, L"GET", UrlPath, NULL,
				NULL, NULL, Url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
			if (Request)
			{
				if (WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
					WinHttpReceiveResponse(Request, NULL))
				{
					DWORD Status = 0;
					DWORD StatusSize = sizeof(Status);
					WinHttpQueryHeaders(
						Request,
						WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX,
						&Status,
						&StatusSize,
						WINHTTP_NO_HEADER_INDEX);

					if (Status == HTTP_STATUS_OK)
					{
						// there's a minor race condition when user notification can show up
						// before image file has finished downloading, but this will almost
						// never happen, and it is such a minor issue, so... meh
						HANDLE File = CreateFileW(ImagePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
						if (File != INVALID_HANDLE_VALUE)
						{
							char Buffer[65536];
							for (;;)
							{
								DWORD Read;
								if (!WinHttpReadData(Request, Buffer, sizeof(Buffer), &Read) || Read == 0)
								{
									break;
								}
								DWORD Written;
								WriteFile(File, Buffer, Read, &Written, NULL);
							}
							CloseHandle(File);
						}
					}
					WinHttpCloseHandle(Request);
				}
				WinHttpCloseHandle(Connection);
			}
		}
	}

	LocalFree(ImageUrl);

	CloseThreadpoolWork(Work);
}

// called on main thread, gets path to profile image, and downloads it when necessary
static void DownloadUserImage(LPWSTR ImagePath, LPCWSTR ImageUrl)
{
	GetImagePath(ImagePath, ImageUrl);

	WIN32_FILE_ATTRIBUTE_DATA Data;
	if (GetFileAttributesExW(ImagePath, GetFileExInfoStandard, &Data))
	{
		// FILETIME is in 100nsec units
		uint64_t LastWrite =
			(((uint64_t)Data.ftLastWriteTime.dwHighDateTime) << 32) + Data.ftLastWriteTime.dwLowDateTime;

		FILETIME NowTime;
		GetSystemTimeAsFileTime(&NowTime);

		uint64_t Now = (((uint64_t)NowTime.dwHighDateTime) << 32) + NowTime.dwLowDateTime;
		uint64_t Expires = LastWrite + (uint64_t)MAX_IMAGE_CACHE_AGE * 10 * 1000 * 1000;
		if (Now < Expires)
		{
			// ok image up to date
			return;
		}
	}

	// when image is not present or out of date, queue its download

	TP_CALLBACK_ENVIRON Environ;
	InitializeThreadpoolEnvironment(&Environ);
	SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

	// NOTE: callback must LocalFree() passed context pointer
	PTP_WORK Work = CreateThreadpoolWork(&DownloadUserImageWork, StrDupW(ImageUrl), &Environ);
	Assert(Work);

	SubmitThreadpoolWork(Work);
}

// gql query for getting user info, happens in background thread

static void CALLBACK DownloadUserInfoWork(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work)
{
	char* Query = (char*)Context;

	char Buffer[65536];
	int BufferSize = DoGqlQuery(Query, lstrlenA(Query), Buffer, sizeof(Buffer));
	LocalFree(Query);

	JsonObject* Json = JsonObject_Parse(Buffer, BufferSize);
	PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_INFO, (WPARAM)Json, 0);

	CloseThreadpoolWork(Work);
}

// requests user status
static void UpdateUserInfo(void)
{
	WCHAR Query[4096];
	int QuerySize = 0;

	QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"{\"query\":\"{users(logins:[");

	LPCWSTR Delim = L"";

	// prepare query to download user info - user id, display name, profile image, stream viewer count
	for (int Index = 0; Index < State.UserCount; Index++)
	{
		User* User = &State.Users[Index];
		
		QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, Delim);
		QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"\\\"");
		QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, User->Name);
		QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"\\\"");
		Delim = L",";
	}

	// allowed image widths: 28, 50, 70, 150, 300, 600
	// use 70 because for 100% dpi scale the toast size is 48 pixels, for 200% it is 96
	QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"]){id,displayName,profileImageURL(width:70),stream{title,viewersCount,game{displayName}}}}\"}");

	char QueryBytes[4096];
	QuerySize = WideCharToMultiByte(CP_UTF8, 0, Query, QuerySize, QueryBytes, ARRAYSIZE(QueryBytes), NULL, NULL);
	QueryBytes[QuerySize] = 0;

	// queue gql query to background

	TP_CALLBACK_ENVIRON Environ;
	InitializeThreadpoolEnvironment(&Environ);
	SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

	// NOTE: callback must LocalFree() passed context pointer
	PTP_WORK Work = CreateThreadpoolWork(&DownloadUserInfoWork, StrDupA(QueryBytes), &Environ);
	Assert(Work);

	SubmitThreadpoolWork(Work);
}

// reloads .ini file if need & updates State.User[] array, must be called from main thread
static void LoadUsers(void)
{
	WIN32_FILE_ATTRIBUTE_DATA Data;
	if (!GetFileAttributesExW(State.IniPath, GetFileExInfoStandard, &Data))
	{
		// .ini file deleted?
		return;
	}

	if (CompareFileTime(&State.LastIniWriteTime, &Data.ftLastWriteTime) >= 0)
	{
		// .ini file is up to date
		return;
	}
	State.LastIniWriteTime = Data.ftLastWriteTime;

	WCHAR Users[32768]; // max size GetPrivateProfileSection() can return
	Users[0] = 0;
	GetPrivateProfileSectionW(L"users", Users, ARRAYSIZE(Users), State.IniPath);

	WCHAR* Ptr = Users;

	// remember current user count
	int OldCount = State.UserCount;
	User OldUsers[MAX_USER_COUNT];
	CopyMemory(OldUsers, State.Users, OldCount * sizeof(User));
	State.UserCount = 0;

	while (*Ptr != 0)
	{
		if (State.UserCount == MAX_USER_COUNT)
		{
			ShowTrayMessage(State.Window, NIIF_WARNING, L"Too many users in .ini file!");
			break;
		}

		int PtrLength = lstrlenW(Ptr);

		WCHAR* Name = Ptr;
		StrTrimW(Name, L" \t");
		if (Name[0])
		{
			for (int OldIndex = 0; OldIndex < OldCount; OldIndex++)
			{
				if (StrCmpW(OldUsers[OldIndex].Name, Name) == 0)
				{
					State.Users[State.UserCount++] = OldUsers[OldIndex];
					Name = NULL;
					break;
				}
			}
			if (Name)
			{
				User* User = &State.Users[State.UserCount++];
				StrCpyNW(User->Name, Name, MAX_STRING_LENGTH);

				// this info is not known yet, will be downloaded in DownloadUserInfoWork callback
				User->UserId = 0;
				User->ViewerCount = 0;
				User->IsLive = false;
				User->DisplayName[0] = 0;
				User->ImagePath[0] = 0;
				User->Notification = NULL;
			}
		}
		Ptr += PtrLength + 1;
	}

	State.Initialized = false;
	UpdateUserInfo();
}

// gql query for getting list of followed usernames, happens in background thread

static void CALLBACK DownloadFollowedUsersWork(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work)
{
	char* Query = (char*)Context;

	char Buffer[65536];
	int BufferSize = DoGqlQuery(Query, lstrlenA(Query), Buffer, sizeof(Buffer));
	LocalFree(Query);

	JsonObject* Json = JsonObject_Parse(Buffer, BufferSize);
	PostMessageW(State.Window, WM_TWITCH_NOTIFY_FOLLOWED_USERS, (WPARAM)Json, 0);

	CloseThreadpoolWork(Work);
}

// starts download of list of followed users
static void DownloadFollowedUsers(void)
{
	WCHAR username[MAX_STRING_LENGTH];
	if (!GetPrivateProfileStringW(L"twitch", L"username", L"", username, ARRAYSIZE(username), State.IniPath) || username[0] == 0)
	{
		WCHAR ImagePath[MAX_PATH];
		GetTwitchIcon(ImagePath);

		WCHAR Xml[4096];
		int XmlLength = 0;

		XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength,
			L"<toast><visual><binding template=\"ToastGeneric\">"
			L"<image placement=\"appLogoOverride\" src=\"file:///");
		XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, ImagePath);
		XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, L"\"/>"
			L"<text>Username not set</text>"
			L"<text>Cannot download followed users!</text>"
			L"<text>Edit .ini file to set username?</text>"
			L"</binding></visual><actions>"
			L"<action content=\"Yes\" arguments=\"E\"/>"
			L"<action content=\"No\" arguments=\"N\"/>"
			L"</actions></toast>");

		WindowsToast_ShowSimple(&State.Toast, Xml, XmlLength, NULL, 0);
		return;
	}

	char QueryBytes[1024];
	wsprintfA(QueryBytes, "{\"query\":\"# query:\\n{user(login:\\\"%S\\\"){follows(first:%u){edges{node{login}}}}}\"}", username, MAX_USER_COUNT);

	// queue gql query to background

	TP_CALLBACK_ENVIRON Environ;
	InitializeThreadpoolEnvironment(&Environ);
	SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

	// NOTE: callback must LocalFree() passed context pointer
	PTP_WORK Work = CreateThreadpoolWork(&DownloadFollowedUsersWork, StrDupA(QueryBytes), &Environ);
	Assert(Work);

	SubmitThreadpoolWork(Work);
}

// gql query for getting user game/stream title, happens in background thread

static void DownloadUserStreamCommon(int UserId)
{
	char Query[1024];
	int QuerySize = wsprintfA(Query, "{\"query\":\"{users(ids:[%d]){stream{title,game{displayName}}}}\"}", UserId);

	char Buffer[4096];
	int BufferSize = DoGqlQuery(Query, QuerySize, Buffer, sizeof(Buffer));

	JsonObject* Json = BufferSize ? JsonObject_Parse(Buffer, BufferSize) : NULL;
	PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_STREAM, UserId, (LPARAM)Json);
}

static void CALLBACK DownloadUserStreamWork(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work)
{
	int UserId = PtrToInt(Context);
	DownloadUserStreamCommon(UserId);
	CloseThreadpoolWork(Work);
}

static void CALLBACK DownloadUserStreamTimer(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer)
{
	int UserId = PtrToInt(Context);
	DownloadUserStreamCommon(UserId);
	CloseThreadpoolTimer(Timer);
}

// issues gql query for getting user game/stream title, must be called from main thread
// Delay is extra time to delay before actual download, in msec
static void DownloadUserStream(int UserId, int Delay)
{
	TP_CALLBACK_ENVIRON Environ;
	InitializeThreadpoolEnvironment(&Environ);
	SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

	if (Delay == 0)
	{
		PTP_WORK Work = CreateThreadpoolWork(&DownloadUserStreamWork, IntToPtr(UserId), &Environ);
		Assert(Work);
		SubmitThreadpoolWork(Work);
	}
	else
	{
		PTP_TIMER Timer = CreateThreadpoolTimer(&DownloadUserStreamTimer, IntToPtr(UserId), &Environ);
		Assert(Timer);

		// negative timer value means relative time
		LARGE_INTEGER Time = { .QuadPart = -Delay * 10000LL }; // in 100 nsec units
		FILETIME DueTime =
		{
			.dwLowDateTime = Time.LowPart,
			.dwHighDateTime = Time.HighPart,
		};
		SetThreadpoolTimer(Timer, &DueTime, 0, 0);
	}
}

// show Windows Toast notification

static void ShowUserNotification(User* User, JsonObject* Stream)
{
	JsonObject* Game = JsonObject_GetObject(Stream, JsonCSTR("game"));
	HSTRING GameName = JsonObject_GetString(Game, JsonCSTR("displayName"));
	HSTRING StreamName = JsonObject_GetString(Stream, JsonCSTR("title"));

	WCHAR ImagePath[MAX_PATH];

	if (GetFileAttributesW(User->ImagePath) == INVALID_FILE_ATTRIBUTES)
	{
		// unlikely, but in case image file is missing on disk, use generic Twitch image from our icon
		GetTwitchIcon(ImagePath);
	}
	else
	{
		StrCpyW(ImagePath, User->ImagePath);
	}
	for (WCHAR* P = ImagePath; *P; P++)
	{
		if (*P == '\\') *P = '/';
	}

	WCHAR Xml[4096];
	int XmlLength = 0;

	// use long duration, because getting game & stream name often takes a while
	XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength,
		L"<toast duration=\"long\"><visual><binding template=\"ToastGeneric\">"
		L"<image placement=\"appLogoOverride\" src=\"file:///");
	XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, ImagePath);
	XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength,
		L"\"/>"
		L"<text hint-maxLines=\"3\">{user}</text>"
		L"<text>{game}</text>"
		L"<text>{stream}</text>"
		L"</binding></visual><actions>");

	if (IsMpvInPath())
	{
		XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, L"<action content=\"Play\" arguments=\"Phttps://www.twitch.tv/");
		XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, User->Name);
		XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, L"\"/>");
	}

	XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, L"<action content=\"Open Browser\" arguments=\"Ohttps://www.twitch.tv/");
	XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength, User->Name);
	XmlLength = StrCatChainW(Xml, ARRAYSIZE(Xml), XmlLength,
		L"\"/>"
		L"</actions></toast>");

	LPCWSTR Data[][2] =
	{
		{ L"user",   User->DisplayName },
		{ L"game",   StreamName ? WindowsGetStringRawBuffer(GameName, NULL) : L"..." },
		{ L"stream", StreamName ? WindowsGetStringRawBuffer(StreamName, NULL) : L""  },
	};
	void* Notification = WindowsToast_Create(&State.Toast, Xml, XmlLength, Data, ARRAYSIZE(Data));
	WindowsToast_Show(&State.Toast, Notification);

	if (StreamName)
	{
		WindowsToast_Release(&State.Toast, Notification);
	}
	else
	{
		User->Notification = Notification;

		// game/stream name was not available, download it to update in notification later
		DownloadUserStream(User->UserId, 0);
	}

	WindowsDeleteString(StreamName);
	WindowsDeleteString(GameName);
	JsonRelease(Game);
}

// updates & releases user notification
static void UpdateUserNotification(User* User, LPCWSTR GameName, LPCWSTR StreamName)
{
	if (User->Notification)
	{
		LPCWSTR Data[][2] =
		{
			{ L"game",   GameName   },
			{ L"stream", StreamName },
		};
		WindowsToast_Update(&State.Toast, User->Notification, Data, ARRAYSIZE(Data));
		WindowsToast_Release(&State.Toast, User->Notification);
		User->Notification = NULL;
	}
}

static void ShowTrayMenu(HWND Window)
{
	bool MpvFound = IsMpvInPath();
	WCHAR username[MAX_STRING_LENGTH];

	bool CanUpdateUsers = GetPrivateProfileStringW(L"twitch", L"username", L"", username, ARRAYSIZE(username), State.IniPath) && username[0];

	HMENU QualityMenu = CreatePopupMenu();
	Assert(QualityMenu);

	for (int Index = 0; Index < ARRAYSIZE(Quality); Index++)
	{
		UINT Flags = State.Quality == Index ? MF_CHECKED : MF_UNCHECKED;
		AppendMenuW(QualityMenu, Flags, CMD_QUALITY + Index, Quality[Index].Name);
	}

	HMENU Menu = CreatePopupMenu();
	Assert(Menu);

	AppendMenuW(Menu, MF_STRING, CMD_OPEN_HOMEPAGE, L"Twitch Notify");
	AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);


	for (int Index = 0; Index < State.UserCount; Index++)
	{
		User* User = &State.Users[Index];
		if (User->UserId > 0)
		{
			LPCWSTR Name = User->DisplayName[0] ? User->DisplayName : User->Name;
			if (User->IsLive)
			{
				WCHAR Caption[1024];
				wsprintfW(Caption, L"%s\t%d", Name, User->ViewerCount);
				AppendMenuW(Menu, MF_CHECKED, CMD_USER + Index, Caption);
			}
			else
			{
				AppendMenuW(Menu, MF_STRING, CMD_USER + Index, Name);
			}
		}
		else // unknown user
		{
			AppendMenuW(Menu, MF_GRAYED, 0, User->Name);
		}
	}
	if (State.UserCount == 0)
	{
		AppendMenuW(Menu, MF_GRAYED, 0, L"No users");
	}

	AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);

	AppendMenuW(Menu, MF_STRING | (CanUpdateUsers ? 0 : MF_GRAYED), CMD_DOWNLOAD_USERS, L"Download User List");
	AppendMenuW(Menu, MF_STRING, CMD_EDIT_USERS, L"Edit User List");

	AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);

	AppendMenuW(Menu, (State.UseMpv ? MF_CHECKED : MF_STRING) | (MpvFound ? 0 : MF_GRAYED), CMD_USE_MPV, L"mpv Playback");
	AppendMenuW(Menu, MF_POPUP | (MpvFound ? 0 : MF_GRAYED), (UINT_PTR)QualityMenu, L"mpv Quality");
	AppendMenuW(Menu, (State.NotifyOnStartup ? MF_CHECKED : MF_STRING), CMD_NOTIFY_ON_STARTUP, L"Notify on Startup");

	AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(Menu, MF_STRING, CMD_EXIT, L"Exit");

	POINT Mouse;
	GetCursorPos(&Mouse);

	SetForegroundWindow(Window);
	int Command = TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_NONOTIFY, Mouse.x, Mouse.y, 0, Window, NULL);
	if (Command == CMD_OPEN_HOMEPAGE)
	{
		ShellExecuteW(NULL, L"open", TWITCH_NOTIFY_HOMEPAGE, NULL, NULL, SW_SHOWNORMAL);
	}
	else if (Command == CMD_USE_MPV)
	{
		State.UseMpv = !State.UseMpv;
	}
	else if (Command >= CMD_QUALITY && Command < CMD_QUALITY + ARRAYSIZE(Quality))
	{
		State.Quality = Command - CMD_QUALITY;
	}
	else if (Command == CMD_EDIT_USERS)
	{
		ShellExecuteW(NULL, L"edit", State.IniPath, NULL, NULL, SW_SHOWNORMAL);
	}
	else if (Command == CMD_DOWNLOAD_USERS)
	{
		DownloadFollowedUsers();
	}
	else if (Command == CMD_NOTIFY_ON_STARTUP)
	{
		State.NotifyOnStartup = !State.NotifyOnStartup;
	}
	else if (Command == CMD_EXIT)
	{
		DestroyWindow(Window);
	}
	else if (Command >= CMD_USER && Command < CMD_USER + State.UserCount)
	{
		User* User = &State.Users[Command - CMD_USER];

		WCHAR Url[1024];
		wsprintfW(Url, L"https://www.twitch.tv/%s", User->Name);

		if (State.UseMpv && IsMpvInPath() && User->IsLive)
		{
			// use mpv only if mpv is selected, it is available in path, and user is live
			OpenMpvUrl(Url);
		}
		else
		{
			// otherwise open browser url
			ShellExecuteW(NULL, L"open", Url, NULL, NULL, SW_SHOWNORMAL);
		}
	}

	DestroyMenu(Menu);
	DestroyMenu(QualityMenu);
}

static User* GetUser(int UserId)
{
	for (int Index = 0; Index < State.UserCount; Index++)
	{
		User* User = &State.Users[Index];
		if (User->UserId == UserId)
		{
			return User;
		}
	}
	return NULL;
}

static void OnUserStream(int UserId, JsonObject* Json)
{
	User* User = GetUser(UserId);
	if (!User)
	{
		// no problem if user is not found
		// probably user list was edited right before gql download finished
		return;
	}

	JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
	JsonArray* Users = JsonObject_GetArray(Data, JsonCSTR("users"));
	JsonObject* UserData = JsonArray_GetObject(Users, 0);
	JsonObject* Stream = JsonObject_GetObject(UserData, JsonCSTR("stream"));
	JsonObject* Game = JsonObject_GetObject(Stream, JsonCSTR("game"));
	HSTRING GameName = JsonObject_GetString(Game, JsonCSTR("displayName"));
	HSTRING StreamName = JsonObject_GetString(Stream, JsonCSTR("title"));

	if (StreamName)
	{
		// if stream name is available in json response
		// update notification with actual game/stream title
		LPCWSTR GameNameStr = WindowsGetStringRawBuffer(GameName, NULL);
		LPCWSTR StreamNameStr = WindowsGetStringRawBuffer(StreamName, NULL);
		UpdateUserNotification(User, GameNameStr, StreamNameStr);
	}
	else
	{
		// cannot get user game/stream title
		// if notification is still up then retry a bit later - after 1 second
		if (User->Notification != NULL)
		{
			DownloadUserStream(UserId, 1000);
		}
	}

	WindowsDeleteString(StreamName);
	WindowsDeleteString(GameName);
	JsonRelease(Game);
	JsonRelease(Stream);
	JsonRelease(UserData);
	JsonRelease(Users);
	JsonRelease(Data);
}

static void OnUserInfo(JsonObject* Json)
{
	JsonArray* Errors = JsonObject_GetArray(Json, JsonCSTR("errors"));
	if (Errors)
	{
		//JsonObject* ErrorMessage = JsonArray_GetObject(Errors, 0);
		//HSTRING Message = JsonObject_GetString(ErrorMessage, JsonCSTR("message"));
		//ShowTrayMessage(State.Window, NIIF_ERROR, WindowsGetStringRawBuffer(Message, NULL));
		//WindowsDeleteString(Message);
		//JsonRelease(ErrorMessage);
		JsonRelease(Errors);
		return;
	}

	JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
	JsonArray* Users = JsonObject_GetArray(Data, JsonCSTR("users"));

	// update all users that have UserId set to 0 - those are the ones
	// for which gql query was downloaded

	int Count = JsonArray_GetCount(Users);
	for (int Index = 0; Index < Count; Index++)
	{
		User* User = &State.Users[Index];

		JsonObject* UserData = JsonArray_GetObject(Users, Index);
		if (UserData == NULL)
		{
			// user does not exist
			User->UserId = -1;
		}
		else
		{
			HSTRING UserId = JsonObject_GetString(UserData, JsonCSTR("id"));
			User->UserId = StrToIntW(WindowsGetStringRawBuffer(UserId, NULL));

			HSTRING DisplayName = JsonObject_GetString(UserData, JsonCSTR("displayName"));
			StrCpyNW(User->DisplayName, WindowsGetStringRawBuffer(DisplayName, NULL), MAX_STRING_LENGTH);

			HSTRING ProfileImageUrl = JsonObject_GetString(UserData, JsonCSTR("profileImageURL"));
			DownloadUserImage(User->ImagePath, WindowsGetStringRawBuffer(ProfileImageUrl, NULL));

			JsonObject* Stream = JsonObject_GetObject(UserData, JsonCSTR("stream"));
			User->ViewerCount = (int)JsonObject_GetNumber(Stream, JsonCSTR("viewersCount"));

			if (State.Initialized || State.NotifyOnStartup)
			{
				if (Stream && !User->IsLive)
				{
					ShowUserNotification(User, Stream);
				}
				else if (User->IsLive && !Stream)
				{
					// in case notification game/stream title update was pending
					// update it and release notification handle
					UpdateUserNotification(User, L"", L"");
				}
			}
			User->IsLive = Stream != NULL;

			WindowsDeleteString(UserId);
			WindowsDeleteString(DisplayName);
			WindowsDeleteString(ProfileImageUrl);
			JsonRelease(Stream);
			JsonRelease(UserData);
		}
	}
	JsonRelease(Users);
	JsonRelease(Data);
}

static void OnFollowedUsers(JsonObject* Json)
{
	JsonArray* Errors = JsonObject_GetArray(Json, JsonCSTR("errors"));
	if (Errors)
	{
		JsonObject* ErrorMessage = JsonArray_GetObject(Errors, 0);
		HSTRING Message = JsonObject_GetString(ErrorMessage, JsonCSTR("message"));
		ShowTrayMessage(State.Window, NIIF_ERROR, WindowsGetStringRawBuffer(Message, NULL));
		WindowsDeleteString(Message);
		JsonRelease(ErrorMessage);
		JsonRelease(Errors);
		return;
	}

	JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
	JsonObject* User = JsonObject_GetObject(Data, JsonCSTR("user"));
	JsonObject* Follows = JsonObject_GetObject(User, JsonCSTR("follows"));
	JsonArray* Edges = JsonObject_GetArray(Follows, JsonCSTR("edges"));

	WCHAR Users[32768];
	int UsersLength = 0;
	
	int Count = JsonArray_GetCount(Edges);
	for (int Index = 0; Index < Count; Index++)
	{
		JsonObject* Edge = JsonArray_GetObject(Edges, Index);
		JsonObject* Node = JsonObject_GetObject(Edge, JsonCSTR("node"));
		HSTRING Login = JsonObject_GetString(Node, JsonCSTR("login"));
		if (Login)
		{
			if (Index != 0)
			{
				UsersLength = StrCatChainW(Users, ARRAYSIZE(Users), UsersLength, L"\r\n");
			}
			UsersLength = StrCatChainW(Users, ARRAYSIZE(Users), UsersLength, WindowsGetStringRawBuffer(Login, NULL));
		}

		WindowsDeleteString(Login);
		JsonRelease(Node);
		JsonRelease(Edge);
	}

	if (UsersLength)
	{
		// delete contents of current "users" section
		WritePrivateProfileStringW(L"users", NULL, NULL, State.IniPath);
		// write new "users" section, it will trigger modification notification that will reload .ini file later
		WritePrivateProfileSectionW(L"users", Users, State.IniPath);
	}

	JsonRelease(Edges);
	JsonRelease(Follows);
	JsonRelease(User);
	JsonRelease(Data);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		AddTrayIcon(Window, State.Icon); // initial icon will show up as connected, just to look prettier
		State.Quality = GetPrivateProfileIntW(L"player", L"quality", 0, State.IniPath);
		State.UseMpv = GetPrivateProfileIntW(L"player", L"mpv", 1, State.IniPath);
		State.NotifyOnStartup = GetPrivateProfileIntW(L"player", L"notifyOnStartup", 0, State.IniPath);

		if (GetPrivateProfileIntW(L"twitch", L"autoupdate", 0, State.IniPath) == 0)
		{
			// load initial user list
			LoadUsers();
		}
		else
		{
			// download followed user list
			DownloadFollowedUsers();
		}
		return 0;
	}
	else if (Message == WM_DESTROY)
	{
		WCHAR Str[2] = { L'0' + State.Quality, 0 };
		WritePrivateProfileStringW(L"player", L"quality", Str, State.IniPath);
		WritePrivateProfileStringW(L"player", L"mpv", State.UseMpv ? L"1" : L"0", State.IniPath);
		WritePrivateProfileStringW(L"player", L"notifyOnStartup", State.NotifyOnStartup ? L"1" : L"0", State.IniPath);
		RemoveTrayIcon(Window);
		PostQuitMessage(0);
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_COMMAND && LParam == WM_RBUTTONUP)
	{
		ShowTrayMenu(Window);
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_ALREADY_RUNNING)
	{
		ShowTrayMessage(Window, NIIF_INFO, TWITCH_NOTIFY_NAME" is already running");
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_USER_STREAM)
	{
		// user game/stream title received from gql query
		int UserId = (int)WParam;
		JsonObject* Json = (JsonObject*)LParam;
		OnUserStream(UserId, Json);
		JsonRelease(Json);
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_USER_INFO)
	{
		// user info received from gql query
		JsonObject* Json = (JsonObject*)WParam;
		OnUserInfo(Json);
		if (!State.Initialized)
		{
			State.Initialized = true;
			SetTimer(Window, TIMER_STREAM_CHECK, TIMER_STREAM_CHECK_INTERVAL, NULL);
		}
		JsonRelease(Json);
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_FOLLOWED_USERS)
	{
		// process list of followed users
		JsonObject* Json = (JsonObject*)WParam;
		OnFollowedUsers(Json);
		JsonRelease(Json);
		return 0;
	}
 	else if (Message == WM_TIMER)
	{
		if (WParam == TIMER_RELOAD_USERS)
		{
			KillTimer(Window, TIMER_RELOAD_USERS);
			LoadUsers();
			return 0;
		}
		else if (WParam == TIMER_STREAM_CHECK)
		{
			UpdateUserInfo();
			return 0;
		}
	}
	else if (Message == WM_POWERBROADCAST)
	{
		// resuming from sleep/hibernate
		if (WParam == PBT_APMRESUMEAUTOMATIC)
		{
			// set all users to be offline
			for (int Index = 0; Index < State.UserCount; Index++)
			{
				State.Users[Index].IsLive = false;
			}

			State.Initialized = false;
			SetTimer(Window, TIMER_STREAM_CHECK, TIMER_STREAM_CHECK_INTERVAL, NULL);
		}
		else if (WParam == PBT_APMSUSPEND)
		{
			KillTimer(Window, TIMER_STREAM_CHECK);
		}

		return TRUE;
	}
	else if (Message == WM_TASKBARCREATED)
	{
		AddTrayIcon(Window, State.Icon);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

static void OnToastActivated(WindowsToast* Toast, void* Item, LPCWSTR Action)
{
	if (Action[0] == L'P')
	{
		LPCWSTR Url = Action + 1;
		OpenMpvUrl(Url);
	}
	else if (Action[0] == L'O')
	{
		LPCWSTR Url = Action + 1;
		ShellExecuteW(NULL, L"open", Url, NULL, NULL, SW_SHOWNORMAL);
	}
	else if (Action[0] == L'E')
	{
		ShellExecuteW(NULL, L"edit", State.IniPath, NULL, NULL, SW_SHOWNORMAL);
	}
}

#ifdef _DEBUG
int wWinMain(HINSTANCE Instance, HINSTANCE PrevInstance, LPWSTR CmdLine, int ShowCmd)
#else
void WinMainCRTStartup(void)
#endif
{
	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.lpfnWndProc = &WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = TWITCH_NOTIFY_NAME,
	};

	// check if TwitchNotify is already running
	HWND Existing = FindWindowW(WindowClass.lpszClassName, NULL);
	if (Existing)
	{
		PostMessageW(Existing, WM_TWITCH_NOTIFY_ALREADY_RUNNING, 0, 0);
		ExitProcess(0);
	}

	// initialize Windows Runtime
	HR(RoInitialize(RO_INIT_SINGLETHREADED));

	// initialize Windows Toast notifications
	WindowsToast_Init(&State.Toast, TWITCH_NOTIFY_NAME, TWITCH_NOTIFY_APPID);
	WindowsToast_HideAll(&State.Toast, TWITCH_NOTIFY_APPID);
	State.Toast.OnActivatedCallback = &OnToastActivated;

	// initialize Windows HTTP Services
	State.Session = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	Assert(State.Session);

	// create thread pool with 2 threads for background downloads
	State.ThreadPool = CreateThreadpool(NULL);
	Assert(State.ThreadPool);
	SetThreadpoolThreadMinimum(State.ThreadPool, 2);
	SetThreadpoolThreadMaximum(State.ThreadPool, 2);

	// setup paths
	WCHAR ExeFolder[MAX_PATH];
	GetModuleFileNameW(NULL, ExeFolder, ARRAYSIZE(ExeFolder));
	PathRemoveFileSpecW(ExeFolder);
	PathCombineW(State.IniPath, ExeFolder, TWITCH_NOTIFY_INI);

	// notifications on folder to monitor when ini file changes
	HANDLE ExeFolderHandle = FindFirstChangeNotificationW(ExeFolder, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
	Assert(ExeFolderHandle != INVALID_HANDLE_VALUE);

	// load tray icon
	State.Icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
	Assert(State.Icon);

	// create window
	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	State.Window = CreateWindowExW(0, WindowClass.lpszClassName, WindowClass.lpszClassName,
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		CW_USEDEFAULT, NULL, NULL, WindowClass.hInstance, NULL);
	Assert(State.Window);

	// do the window message loop
	for (;;)
	{
		DWORD Wait = MsgWaitForMultipleObjects(1, &ExeFolderHandle, FALSE, INFINITE, QS_ALLINPUT);
		if (Wait == WAIT_OBJECT_0)
		{
			// start timer to reload .ini file, if file will be
			// modified again (Ctrl+S pressed 2x fast) then timer will reset
			SetTimer(State.Window, TIMER_RELOAD_USERS, TIMER_RELOAD_USERS_DELAY, NULL);

			Assert(FindNextChangeNotification(ExeFolderHandle));
		}
		else if (Wait == WAIT_OBJECT_0 + 1)
		{
			MSG Message;
			while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
			{
				if (Message.message == WM_QUIT)
				{
					ExitProcess(0);
				}
				TranslateMessage(&Message);
				DispatchMessageW(&Message);
			}
		}
		else
		{
			Assert(false);
		}
	}
}
