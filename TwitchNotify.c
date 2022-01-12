#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>

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

#define WM_TWITCH_NOTIFY_COMMAND         (WM_USER + 1)
#define WM_TWITCH_NOTIFY_ALREADY_RUNNING (WM_USER + 2)
#define WM_TWITCH_NOTIFY_WEBSOCKET       (WM_USER + 3) // wparam=bool for connection status
#define WM_TWITCH_NOTIFY_USER_STATUS     (WM_USER + 4) // wparam=UserId, lparam=true if connected
#define WM_TWITCH_NOTIFY_USER_VIEW_COUNT (WM_USER + 5) // wparam=UserId, lparam=viewer count
#define WM_TWITCH_NOTIFY_USER_STREAM     (WM_USER + 6) // wparam=UserId, lparam=JsonObject for game & stream name
#define WM_TWITCH_NOTIFY_USER_INFO       (WM_USER + 7) // wparam=JsonObject for user

#define CMD_OPEN_HOMEPAGE 10
#define CMD_USE_MPV       20
#define CMD_QUALITY       30
#define CMD_EDIT_USERS    40
#define CMD_EXIT          50
#define CMD_USER          60 // should be last

#define TWITCH_NOTIFY_NAME     L"Twitch Notify"
#define TWITCH_NOTIFY_INI      L"TwitchNotify.ini"
#define TWITCH_NOTIFY_APPID    L"TwitchNotify.TwitchNotify" // CompanyName.ProductName
#define TWITCH_NOTIFY_HOMEPAGE L"https://github.com/mmozeiko/TwitchNotify/"

#define MAX_USER_COUNT    50 // oficially documented user count limit for Twitch websocket
#define MAX_STRING_LENGTH 256

#define MAX_IMAGE_CACHE_AGE (60*60*24*7)  // 1 week in seconds

#define TIMER_RELOAD_USERS 1
#define TIMER_RELOAD_USERS_DELAY 100 // msec

#define TIMER_WEBSOCKET_PING 2
#define TIMER_WEBSOCKET_PING_INTERVAL (2*60*1000) // 2 minutes in msec, Twitch requires at least one ping per 5 minutes

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

// message sent when explorer.exe restarts, to restore tray icons
static UINT WM_TASKBARCREATED;

typedef struct
{
	WCHAR Name[MAX_STRING_LENGTH];
	WCHAR DisplayName[MAX_STRING_LENGTH];
	WCHAR ImagePath[MAX_STRING_LENGTH];
	int UserId;
	int ViewerCount;
	BOOL IsOnline;
	void* Notification;
} User;

struct
{
	HICON Icon;
	HICON IconDisconnected;
	HWND Window;
	WindowsToast Toast;
	HINTERNET Session;
	HINTERNET Websocket;
	PTP_POOL ThreadPool;

	WCHAR IniPath[MAX_PATH];
	FILETIME LastIniWriteTime;

	BYTE ExeFolderChanges[4096];
	OVERLAPPED ExeFolderOverlapped;
	HANDLE ExeFolderHandle;

	UINT Quality;
	BOOL UseMpv;

	User Users[MAX_USER_COUNT];
	int UserCount;
}
static State;

// http://www.isthe.com/chongo/tech/comp/fnv/
static UINT64 GetFnv1Hash(const void* Ptr, int Size)
{
	const BYTE* Bytes = Ptr;
	UINT64 Hash = 14695981039346656037ULL;
	for (int Index = 0; Index < Size; Index++)
	{
		Hash *= 1099511628211ULL;
		Hash ^= Bytes[Index];
	}
	return Hash;
}

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

static void ShowTrayMessage(HWND Window, DWORD InfoType, LPCWSTR Message)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_INFO,
		.dwInfoFlags = InfoType,
		.hIcon = State.Websocket ? State.Icon : State.IconDisconnected,
	};
	StrCpyNW(Data.szInfo, Message, ARRAYSIZE(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, TWITCH_NOTIFY_NAME, ARRAYSIZE(Data.szInfoTitle));
	Assert(Shell_NotifyIconW(NIM_MODIFY, &Data));
}

static void WINAPI OnMonitorIniChanges(DWORD ErrorCode, DWORD NumberOfBytesTransfered, LPOVERLAPPED Overlapped)
{
	if (ErrorCode == ERROR_SUCCESS)
	{
		FILE_NOTIFY_INFORMATION* Info = (void*)State.ExeFolderChanges;

		while ((char*)Info + sizeof(*Info) <= State.ExeFolderChanges + NumberOfBytesTransfered)
		{
			if (StrCmpNW(TWITCH_NOTIFY_INI, Info->FileName, Info->FileNameLength) == 0)
			{
				if (Info->Action == FILE_ACTION_REMOVED)
				{
					ShowTrayMessage(State.Window, NIIF_WARNING, L"Config file '" TWITCH_NOTIFY_NAME L"' deleted");
				}
				SetTimer(State.Window, TIMER_RELOAD_USERS, TIMER_RELOAD_USERS_DELAY, NULL);
			}
			if (Info->NextEntryOffset == 0)
			{
				break;
			}
			Info = (void*)((char*)Info + Info->NextEntryOffset);
		}
	}

	BOOL Ok = ReadDirectoryChangesW(
		State.ExeFolderHandle,
		State.ExeFolderChanges,
		ARRAYSIZE(State.ExeFolderChanges),
		FALSE,
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
		NULL,
		&State.ExeFolderOverlapped,
		NULL);
	Assert(Ok);
}

static BOOL IsMpvInPath(void)
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

static void ShowUserNotification(User* User)
{
	WCHAR ImagePath[MAX_PATH];

	if (GetFileAttributesW(User->ImagePath) == INVALID_FILE_ATTRIBUTES)
	{
		// in case image file is missing, use generic twitch image from our icon
		DWORD TempLength = GetTempPathW(MAX_PATH, ImagePath);
		Assert(TempLength);
		wsprintfW(ImagePath + TempLength, L"twitch.png");

		if (GetFileAttributesW(ImagePath) == INVALID_FILE_ATTRIBUTES)
		{
			// extract first icon image data from resources, it is a png file
			HRSRC Resource = FindResourceW(NULL, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(3)); // 3=RT_ICON
			if (Resource)
			{
				HGLOBAL Global = LoadResource(NULL, Resource);
				if (Global)
				{
					DWORD ResourceSize = SizeofResource(NULL, Resource);
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
		{ L"game",   L"..."            },
		{ L"stream", L""               },
	};
	User->Notification = WindowsToast_Create(&State.Toast, Xml, XmlLength, Data, ARRAYSIZE(Data));
	WindowsToast_Show(&State.Toast, User->Notification);
}

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

static void WebsocketPing(void)
{
	char Data[] = "{\"type\":\"PING\"}";
	WinHttpWebSocketSend(State.Websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, Data, ARRAYSIZE(Data) - 1);
}

static void WesocketListenUser(int UserId, BOOL Listen)
{
	if (State.Websocket)
	{
		char Data[1024];
		int DataSize = wsprintfA(Data, "{\"type\":\"%s\",\"data\":{\"topics\":[\"video-playback-by-id.%d\"]}}", Listen ? "LISTEN" : "UNLISTEN", UserId);
		WinHttpWebSocketSend(State.Websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, Data, DataSize);
	}
}

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
			if (WinHttpSendRequest(Request, Headers, ARRAYSIZE(Headers) - 1, Query, QuerySize, QuerySize, 0) && WinHttpReceiveResponse(Request, NULL))
			{
				DWORD Status = 0;
				DWORD StatusSize = sizeof(Status);
				WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &StatusSize, WINHTTP_NO_HEADER_INDEX);

				if (Status == HTTP_STATUS_OK)
				{
					while (ReadSize < BufferSize)
					{
						DWORD Read;
						if (!WinHttpReadData(Request, Buffer + ReadSize, BufferSize - ReadSize, &Read))
						{
							break;
						}
						if (Read == 0)
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

static void GetImagePath(LPWSTR ImagePath, LPCWSTR ImageUrl)
{
	DWORD ImageUrlLength = lstrlenW(ImageUrl);
	UINT64 Hash = GetFnv1Hash(ImageUrl, ImageUrlLength * sizeof(WCHAR));

	DWORD TempLength = GetTempPathW(MAX_PATH, ImagePath);
	Assert(TempLength);

	LPWSTR Extension = StrRChrW(ImageUrl, ImageUrl + ImageUrlLength, L'.');
	wsprintfW(ImagePath + TempLength, L"%016I64x%s", Hash, Extension);
}

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
			HINTERNET Request = WinHttpOpenRequest(Connection, L"GET", UrlPath, NULL, NULL, NULL, Url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
			if (Request)
			{
				if (WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(Request, NULL))
				{
					DWORD Status = 0;
					DWORD StatusSize = sizeof(Status);
					WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &StatusSize, WINHTTP_NO_HEADER_INDEX);

					if (Status == HTTP_STATUS_OK)
					{
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

static void DownloadUserImage(LPWSTR ImagePath, LPCWSTR ImageUrl)
{
	GetImagePath(ImagePath, ImageUrl);

	WIN32_FILE_ATTRIBUTE_DATA Data;
	if (GetFileAttributesExW(ImagePath, GetFileExInfoStandard, &Data))
	{
		UINT64 LastWrite = (((UINT64)Data.ftLastWriteTime.dwHighDateTime) << 32) + Data.ftLastWriteTime.dwLowDateTime;

		FILETIME NowTime;
		GetSystemTimeAsFileTime(&NowTime);

		UINT64 Now = (((UINT64)NowTime.dwHighDateTime) << 32) + NowTime.dwLowDateTime;
		UINT64 Expires = LastWrite + (UINT64)MAX_IMAGE_CACHE_AGE * 10 * 1000 * 1000;
		if (Now < Expires)
		{
			// ok image up to date
			return;
		}
	}

	// queue downloading to background

	TP_CALLBACK_ENVIRON Environ;
	InitializeThreadpoolEnvironment(&Environ);
	SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

	PTP_WORK Work = CreateThreadpoolWork(&DownloadUserImageWork, StrDupW(ImageUrl), &Environ);
	Assert(Work);

	SubmitThreadpoolWork(Work);
}

static void CALLBACK DownloadUserInfoWork(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work)
{
	char* Query = (char*)Context;

	char Buffer[65536];
	int BufferSize = DoGqlQuery(Query, lstrlenA(Query), Buffer, sizeof(Buffer));
	LocalFree(Query);

	JsonObject* Json = JsonObject_Parse(Buffer, BufferSize);
	JsonArray* Errors = JsonObject_GetArray(Json, JsonCSTR("errors"));
	if (Errors)
	{
		JsonObject* ErrorMessage = JsonArray_GetObject(Errors, 0);
		LPCWSTR Message = JsonObject_GetString(ErrorMessage, JsonCSTR("message"));
		ShowTrayMessage(State.Window, NIIF_ERROR, Message);
		JsonRelease(ErrorMessage);
		JsonRelease(Errors);
	}
	else
	{
		JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
		JsonArray* Users = JsonObject_GetArray(Data, JsonCSTR("users"));
		JsonRelease(Data);

		PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_INFO, (WPARAM)Users, 0);
	}
	JsonRelease(Json);

	CloseThreadpoolWork(Work);
}

static void LoadUsers(void)
{
	WIN32_FILE_ATTRIBUTE_DATA Data;
	if (!GetFileAttributesExW(State.IniPath, GetFileExInfoStandard, &Data))
	{
		// ini file deleted?
		return;
	}

	if (CompareFileTime(&State.LastIniWriteTime, &Data.ftLastWriteTime) >= 0)
	{
		// ini file is up to date
		return;
	}

	State.LastIniWriteTime = Data.ftLastWriteTime;

	WCHAR Users[32768]; // max size GetPrivateProfileSection() can return
	Users[0] = 0;
	GetPrivateProfileSectionW(L"users", Users, ARRAYSIZE(Users), State.IniPath);

	WCHAR* Ptr = Users;

	LPCWSTR NewUsers[MAX_USER_COUNT];

	int NewCount = 0;
	while (*Ptr != 0)
	{
		if (NewCount == MAX_USER_COUNT)
		{
			ShowTrayMessage(State.Window, NIIF_WARNING, L"More than 50 users is not supported");
			break;
		}

		int PtrLength = lstrlenW(Ptr);

		WCHAR* Name = Ptr;
		StrTrimW(Name, L" \t");
		if (Name[0])
		{
			NewUsers[NewCount++] = Name;
		}
		Ptr += PtrLength + 1;
	}

	int OldCount = State.UserCount;

	User OldUsers[MAX_USER_COUNT];
	CopyMemory(OldUsers, State.Users, sizeof(User) * OldCount);

	// copy over existing users
	int UserCount = 0;
	for (int OldIndex = 0; OldIndex < OldCount; OldIndex++)
	{
		for (int NewIndex = 0; NewIndex < NewCount; NewIndex++)
		{
			if (StrCmpW(OldUsers[OldIndex].Name, NewUsers[NewIndex]) == 0)
			{
				State.Users[UserCount++] = OldUsers[OldIndex];
				NewUsers[NewIndex] = NULL;
				OldUsers[OldIndex].Name[0] = 0;
				break;
			}
		}
	}

	// unsubscribe from removed ones
	for (int OldIndex = 0; OldIndex < OldCount; OldIndex++)
	{
		User* OldUser = &OldUsers[OldIndex];
		if (OldUser->Name[0])
		{
			if (OldUser->UserId > 0)
			{
				WesocketListenUser(OldUser->UserId, FALSE);
			}
			UpdateUserNotification(OldUser, L"", L"");
		}
	}

	WCHAR Query[4096];
	int QuerySize = 0;

	QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"{\"query\":\"{users(logins:[");

	LPCWSTR Delim = L"";

	// add new users & prepare query to download their info - user id, display name, profile image, stream viewer count
	for (int NewIndex = 0; NewIndex < NewCount; NewIndex++)
	{
		if (NewUsers[NewIndex])
		{
			User* User = &State.Users[UserCount++];
			StrCpyNW(User->Name, NewUsers[NewIndex], MAX_STRING_LENGTH);
			User->UserId = 0;
			User->DisplayName[0] = 0;
			User->ViewerCount = 0;
			User->IsOnline = FALSE;
			User->Notification = NULL;
			
			QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, Delim);
			QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"\\\"");
			QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, NewUsers[NewIndex]);
			QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"\\\"");
			Delim = L",";
		}
	}
	State.UserCount = UserCount;

	// allowed image widths: 28, 50, 70, 150, 300, 600
	// use 70 because for 100% dpi scale the toast size is 48 pixels, for 200% it is 96
	QuerySize = StrCatChainW(Query, ARRAYSIZE(Query), QuerySize, L"]){id,displayName,profileImageURL(width:70),stream{viewersCount}}}\"}");

	char QueryBytes[4096];
	QuerySize = WideCharToMultiByte(CP_UTF8, 0, Query, QuerySize, QueryBytes, ARRAYSIZE(QueryBytes), NULL, NULL);
	QueryBytes[QuerySize] = 0;

	// queue downloading to background

	TP_CALLBACK_ENVIRON Environ;
	InitializeThreadpoolEnvironment(&Environ);
	SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

	PTP_WORK Work = CreateThreadpoolWork(&DownloadUserInfoWork, StrDupA(QueryBytes), &Environ);
	Assert(Work);

	SubmitThreadpoolWork(Work);
}

static void DownloadUserStreamCommon(int UserId)
{
	char Query[1024];
	int QuerySize = wsprintfA(Query, "{\"query\":\"{users(ids:[%d]){stream{title,game{displayName}}}}\"}", UserId);

	char Buffer[4096];
	int BufferSize = DoGqlQuery(Query, QuerySize, Buffer, sizeof(Buffer));

	JsonObject* Json = BufferSize ? JsonObject_Parse(Buffer, BufferSize) : NULL;
	JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
	JsonArray* Users = JsonObject_GetArray(Data, JsonCSTR("users"));
	JsonObject* UserData = JsonArray_GetObject(Users, 0);
	JsonObject* Stream = JsonObject_GetObject(UserData, JsonCSTR("stream"));

	PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_STREAM, UserId, (LPARAM)Stream);

	JsonRelease(UserData);
	JsonRelease(Users);
	JsonRelease(Data);
	JsonRelease(Json);
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

static void DownloadUserStream(int UserId, int Delay)
{
	if (Delay == 0)
	{
		TP_CALLBACK_ENVIRON Environ;
		InitializeThreadpoolEnvironment(&Environ);
		SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

		PTP_WORK Work = CreateThreadpoolWork(&DownloadUserStreamWork, IntToPtr(UserId), &Environ);
		Assert(Work);

		SubmitThreadpoolWork(Work);
	}
	else
	{
		TP_CALLBACK_ENVIRON Environ;
		InitializeThreadpoolEnvironment(&Environ);
		SetThreadpoolCallbackPool(&Environ, State.ThreadPool);

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

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		AddTrayIcon(Window, State.Icon); // initial icon will show up as connected, just to look prettier
		OnMonitorIniChanges(-1, 0, NULL);
		State.Quality = GetPrivateProfileIntW(L"player", L"quality", 0, State.IniPath);
		State.UseMpv = GetPrivateProfileIntW(L"player", L"mpv", 1, State.IniPath);
		return 0;
	}
	else if (Message == WM_DESTROY)
	{
		WCHAR Str[2] = { L'0' + State.Quality, 0 };
		WritePrivateProfileStringW(L"player", L"quality", Str, State.IniPath);
		WritePrivateProfileStringW(L"player", L"mpv", State.UseMpv ? L"1" : L"0", State.IniPath);
		RemoveTrayIcon(Window);
		if (State.Websocket)
		{
			WinHttpWebSocketClose(State.Websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
			WinHttpCloseHandle(State.Websocket);
		}
		PostQuitMessage(0);
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_COMMAND)
	{
		if (LParam == WM_RBUTTONUP)
		{
			HMENU QualityMenu = CreatePopupMenu();
			Assert(QualityMenu);

			for (int Index = 0; Index < ARRAYSIZE(Quality); Index++)
			{
				AppendMenuW(QualityMenu, State.Quality == Index ? MF_CHECKED : MF_UNCHECKED, CMD_QUALITY + Index, Quality[Index].Name);
			}

			HMENU Users = CreatePopupMenu();
			Assert(Users);

			for (int Index = 0; Index < State.UserCount; Index++)
			{
				User* User = &State.Users[Index];
				if (User->UserId > 0)
				{
					LPCWSTR Name = User->DisplayName[0] ? User->DisplayName : User->Name;
					if (User->IsOnline)
					{
						WCHAR Caption[1024];
						wsprintfW(Caption, L"%s\t%d", Name, User->ViewerCount);
						AppendMenuW(Users, MF_CHECKED, CMD_USER + Index, Caption);
					}
					else
					{
						AppendMenuW(Users, MF_STRING, CMD_USER + Index, Name);
					}
				}
				else // unknown user
				{
					AppendMenuW(Users, MF_GRAYED, 0, User->Name);
				}
			}
			if (State.UserCount == 0)
			{
				AppendMenuW(Users, MF_GRAYED, 0, L"No users");
			}
			AppendMenuW(Users, MF_SEPARATOR, 0, NULL);
			AppendMenuW(Users, MF_STRING, CMD_EDIT_USERS, L"Edit List");

			HMENU Menu = CreatePopupMenu();
			Assert(Menu);

			AppendMenuW(Menu, MF_STRING, CMD_OPEN_HOMEPAGE, L"Twitch Notify");
			AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(Menu, State.UseMpv ? MF_CHECKED : MF_STRING, CMD_USE_MPV, L"Use mpv");
			AppendMenuW(Menu, MF_POPUP, (UINT_PTR)QualityMenu, L"Quality");
			AppendMenuW(Menu, MF_POPUP, (UINT_PTR)Users, L"Users");

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
			else if (Command == CMD_EXIT)
			{
				DestroyWindow(Window);
			}
			else if (Command >= CMD_USER && Command < CMD_USER + State.UserCount)
			{
				User* User = &State.Users[Command - CMD_USER];

				WCHAR Url[1024];
				wsprintfW(Url, L"https://www.twitch.tv/%s", User->Name);

				if (State.UseMpv && IsMpvInPath() && User->IsOnline)
				{
					OpenMpvUrl(Url);
				}
				else
				{
					ShellExecuteW(NULL, L"open", Url, NULL, NULL, SW_SHOWNORMAL);
				}
			}

			DestroyMenu(Menu);
			DestroyMenu(Users);
			DestroyMenu(QualityMenu);
		}
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_ALREADY_RUNNING)
	{
		ShowTrayMessage(Window, NIIF_INFO, TWITCH_NOTIFY_NAME" is already running");
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_WEBSOCKET)
	{
		BOOL Connected = (BOOL)WParam;
		UpdateTrayIcon(Window, Connected ? State.Icon : State.IconDisconnected);
		if (Connected)
		{
			SetTimer(State.Window, TIMER_WEBSOCKET_PING, TIMER_WEBSOCKET_PING_INTERVAL, NULL);

			for (int Index = 0; Index < State.UserCount; Index++)
			{
				User* User = &State.Users[Index];
				if (User->UserId > 0)
				{
					WesocketListenUser(User->UserId, TRUE);
				}
			}
		}
		else
		{
			KillTimer(State.Window, TIMER_WEBSOCKET_PING);
		}
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_USER_STATUS)
	{
		int UserId = (int)WParam;
		BOOL Connected = (BOOL)LParam;
		for (int Index = 0; Index < State.UserCount; Index++)
		{
			User* User = &State.Users[Index];
			if (User->UserId == UserId)
			{
				if (Connected)
				{
					User->IsOnline = TRUE;
					User->ViewerCount = 0;
					ShowUserNotification(User);
					DownloadUserStream(UserId, 0);
				}
				else
				{
					User->IsOnline = FALSE;
				}
				break;
			}
		}
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_USER_VIEW_COUNT)
	{
		int UserId = (int)WParam;
		int ViewerCount = (int)LParam;
		for (int Index = 0; Index < State.UserCount; Index++)
		{
			User* User = &State.Users[Index];
			if (User->UserId == UserId)
			{
				User->ViewerCount = ViewerCount;
				break;
			}
		}
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_USER_STREAM)
	{
		int UserId = (int)WParam;
		JsonObject* Stream = (JsonObject*)LParam;
		for (int Index = 0; Index < State.UserCount; Index++)
		{
			User* User = &State.Users[Index];
			if (User->UserId == UserId)
			{
				JsonObject* Game = JsonObject_GetObject(Stream, JsonCSTR("game"));
				LPCWSTR GameName = JsonObject_GetString(Game, JsonCSTR("displayName"));
				LPCWSTR StreamName = JsonObject_GetString(Stream, JsonCSTR("title"));

				if (StreamName)
				{
					UpdateUserNotification(User, GameName, StreamName);
				}
				else if (User->Notification != NULL) // if notification is still up
				{
					// cannot get user game & stream name
					// retry a bit later - after 1 second
					DownloadUserStream(UserId, 1000);
				}
				else
				{
					// notification was removed, nothing to do
				}

				JsonRelease(Game);
				break;
			}
		}
		JsonRelease(Stream);
		return 0;
	}
	else if (Message == WM_TWITCH_NOTIFY_USER_INFO)
	{
		JsonArray* Users = (JsonArray*)WParam;

		int UserIndex = 0;
		int UsersCount = JsonArray_GetCount(Users);
		for (int Index = 0; Index < UsersCount; Index++)
		{
			User* User = &State.Users[UserIndex];
			while (User->UserId != 0)
			{
				User = &State.Users[++UserIndex];
			}

			JsonObject* UserData = JsonArray_GetObject(Users, Index);
			if (UserData == NULL)
			{
				// user does not exist
				User->UserId = -1;
			}
			else
			{
				LPCWSTR Id = JsonObject_GetString(UserData, JsonCSTR("id"));
				LPCWSTR ProfileImageUrl = JsonObject_GetString(UserData, JsonCSTR("profileImageURL"));
				LPCWSTR DisplayName = JsonObject_GetString(UserData, JsonCSTR("displayName"));

				StrCpyNW(User->DisplayName, DisplayName, MAX_STRING_LENGTH);
				DownloadUserImage(User->ImagePath, ProfileImageUrl);
				User->UserId = StrToIntW(Id);

				JsonObject* Stream = JsonObject_GetObject(UserData, JsonCSTR("stream"));
				User->ViewerCount = (int)JsonObject_GetNumber(Stream, JsonCSTR("viewersCount"));
				JsonRelease(Stream);

				User->IsOnline = Stream != NULL;
				WesocketListenUser(User->UserId, TRUE);

				JsonRelease(UserData);
			}
		}
		JsonRelease(Users);
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
		else if (WParam == TIMER_WEBSOCKET_PING)
		{
			WebsocketPing();
			return 0;
		}
	}
	else if (Message == WM_TASKBARCREATED)
	{
		AddTrayIcon(Window, State.Websocket ? State.Icon : State.IconDisconnected);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

static void WebsocketLoop(HINTERNET Websocket)
{
	// we don't expect to receive large messages over websocket
	char Buffer[4096];
	int BufferSize = 0;

	for (;;)
	{
		int BufferAvailable = sizeof(Buffer) - BufferSize;
		if (BufferAvailable == 0)
		{
			// in case server is sending too much garbage data just disconnect
			break;
		}

		DWORD Read;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE Type;
		if (WinHttpWebSocketReceive(Websocket, Buffer + BufferSize, BufferAvailable, &Read, &Type) != NO_ERROR)
		{
			// error reading from server or disconnected
			break;
		}
		BufferSize += Read;

		if (Type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
		{
			JsonObject* Json = JsonObject_Parse(Buffer, BufferSize);
			if (Json)
			{
				LPCWSTR JsonType = JsonObject_GetString(Json, JsonCSTR("type"));
				if (StrCmpW(JsonType, L"MESSAGE") == 0)
				{
					JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));

					LPCWSTR Topic = JsonObject_GetString(Data, JsonCSTR("topic"));
					LPCWSTR TopicLast = Topic ? StrRChrW(Topic, NULL, L'.') : NULL;
					int UserId = TopicLast ? StrToIntW(TopicLast + 1) : 0;

					LPCWSTR Message = JsonObject_GetString(Data, JsonCSTR("message"));
					if (Message && UserId)
					{
						JsonObject* Msg = JsonObject_ParseW(Message, -1);
						LPCWSTR Type = JsonObject_GetString(Msg, JsonCSTR("type"));
						if (Type)
						{
							if (StrCmpW(Type, L"stream-up") == 0)
							{
								PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_STATUS, UserId, TRUE);
							}
							else if (StrCmpW(Type, L"stream-down") == 0)
							{
								PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_STATUS, UserId, FALSE);
							}
							else if (StrCmpW(Type, L"viewcount") == 0)
							{
								int ViewerCount = (int)JsonObject_GetNumber(Msg, JsonCSTR("viewers"));
								PostMessageW(State.Window, WM_TWITCH_NOTIFY_USER_VIEW_COUNT, UserId, ViewerCount);
							}
						}
						JsonRelease(Msg);
					}
					JsonRelease(Data);
				}
				JsonRelease(Json);
			}
			BufferSize = 0;
		}
		else if (Type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
		{
			// binary message type is not expected, disconnect
			break;
		}
	}
}

static DWORD WINAPI WebsocketThread(LPVOID Arg)
{
	// how much to delay between reconnection attempts
	const DWORD DefaultDelay = 1000;  // 1 second default delay
	const DWORD MaxDelay = 60 * 1000; // max delay = 1 minute
	DWORD Delay = DefaultDelay;
	for (;;)
	{
		HINTERNET Connection = WinHttpConnect(State.Session, L"pubsub-edge.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (Connection)
		{
			DWORD ConnectTimeout = 10; // seconds
			WinHttpSetOption(Connection, WINHTTP_OPTION_CONNECT_TIMEOUT, &ConnectTimeout, sizeof(ConnectTimeout));

			HINTERNET Request = WinHttpOpenRequest(Connection, L"GET", L"/v1", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
			if (Request)
			{
				WinHttpSetOption(Request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
				if (WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(Request, 0))
				{
					HINTERNET Websocket = WinHttpWebSocketCompleteUpgrade(Request, 0);
					if (Websocket)
					{
						WinHttpCloseHandle(Request);
						Request = NULL;
						Delay = DefaultDelay;

						State.Websocket = Websocket;
						PostMessageW(State.Window, WM_TWITCH_NOTIFY_WEBSOCKET, TRUE, 0);

						WebsocketLoop(Websocket);

						State.Websocket = NULL;
						PostMessageW(State.Window, WM_TWITCH_NOTIFY_WEBSOCKET, FALSE, 0);

						WinHttpWebSocketClose(Websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
						WinHttpCloseHandle(Websocket);
					}
				}

				if (Request)
				{
					WinHttpCloseHandle(Request);
				}
			}
			WinHttpCloseHandle(Connection);
		}

		Sleep(Delay);
		Delay = min(Delay * 2, MaxDelay);
	}
}

static void TwitchNotify_OnActivated(WindowsToast* Toast, void* Item, LPCWSTR Action)
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
}

void WinMainCRTStartup(void)
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

	// initialize Windows Toast notifications
	WindowsToast_Init(&State.Toast, TWITCH_NOTIFY_NAME, TWITCH_NOTIFY_APPID);
	WindowsToast_HideAll(&State.Toast, TWITCH_NOTIFY_APPID);
	State.Toast.OnActivatedCallback = TwitchNotify_OnActivated;

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
	State.ExeFolderHandle = CreateFileW(ExeFolder, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	Assert(State.ExeFolderHandle != INVALID_HANDLE_VALUE);
	BOOL Ok = BindIoCompletionCallback(State.ExeFolderHandle, &OnMonitorIniChanges, 0);
	Assert(Ok);

	// load tray icons
	State.Icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
	State.IconDisconnected = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(2));
	Assert(State.Icon && State.IconDisconnected);

	// create window
	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	State.Window = CreateWindowExW(0, WindowClass.lpszClassName, WindowClass.lpszClassName,
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		CW_USEDEFAULT, NULL, NULL, WindowClass.hInstance, NULL);
	Assert(State.Window);

	// start background websocket thread
	HANDLE Thread = CreateThread(NULL, 0, &WebsocketThread, NULL, 0, NULL);
	Assert(Thread);

	// load initial user list
	LoadUsers();

	// do the window message loop
	for (;;)
	{
		MSG Message;
		if (GetMessageW(&Message, NULL, 0, 0) <= 0)
		{
			ExitProcess(0);
		}
		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
}
