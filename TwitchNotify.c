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
#define WM_TWITCH_NOTIFY_TRAY_ICON       (WM_USER + 3)

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

#define MAX_BUFFER_SIZE     (1024*1024)   // 1 MiB
#define MAX_IMAGE_CACHE_AGE (60*60*24*7)  // 1 week in seconds

#define TIMER_RELOAD_USERS 1
#define TIMER_RELOAD_USERS_DELAY 100 // msec

#define TIMER_WEBSOCKET_PING 2
#define TIMER_WEBSOCKET_PING_INTERVAL (2*60*1000) // 2 minutes in msec

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
	WCHAR GameName[MAX_STRING_LENGTH];
	WCHAR StreamName[MAX_STRING_LENGTH];
	WCHAR ImagePath[MAX_PATH];
	UINT UserId;
	BOOL IsOnline;
} User;

struct
{
	HICON Icon[2];
	HWND Window;
	WindowsToast Toast;
	HINTERNET Session;

	WCHAR IniPath[MAX_PATH];
	FILETIME LastIniWriteTime;

	HANDLE ExeFolderHandle;
	BYTE ExeFolderChanges[4096];
	OVERLAPPED ExeFolderOverlapped;

	UINT Quality;
	BOOL UseMpv;

	User Users[MAX_USER_COUNT];
	int UserCount;

	HANDLE ReloadEvent;
	HINTERNET Websocket;
	char Buffer[MAX_BUFFER_SIZE];
}
static State;

static void AddTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_TWITCH_NOTIFY_COMMAND,
		.hIcon = State.Icon[0],
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
		.hIcon = State.Icon[0],
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

static int XmlEscape(WCHAR* Dst, LPCWSTR String)
{
	WCHAR* Ptr = Dst;
	for (const WCHAR* C = String; *C; C++)
	{
		if (*C == L'"')       Ptr += wsprintfW(Ptr, L"&quot;");
		else if (*C == L'\'') Ptr += wsprintfW(Ptr, L"&apos;");
		else if (*C == L'<')  Ptr += wsprintfW(Ptr, L"&lt;");
		else if (*C == L'>')  Ptr += wsprintfW(Ptr, L"&gt;");
		else if (*C == L'&')  Ptr += wsprintfW(Ptr, L"&amp;");
		else *Ptr++ = *C;
	}
	return (int)(Ptr - Dst);
}

static void ShowUserNotification(User* User)
{
	WCHAR* Xml = (WCHAR*)State.Buffer;
	WCHAR* Ptr = Xml;

	DWORD ToastType = 1;
	if (User->StreamName[0] && User->GameName[0])      ToastType = 4;
	else if (User->StreamName[0] || User->GameName[0]) ToastType = 2;

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
	Ptr += wsprintfW(Ptr, L"<toast><visual><binding template=\"ToastImageAndText0%u\"><image id=\"1\" src=\"file:///%s\"/>", ToastType, ImagePath);
	Ptr += wsprintfW(Ptr, L"<text id=\"1\">");
	Ptr += XmlEscape(Ptr, User->DisplayName);
	Ptr += wsprintfW(Ptr, L" is live!</text>");

	DWORD Line = 2;
	if (User->GameName[0])
	{
		Ptr += wsprintfW(Ptr, L"<text id=\"%u\">", Line++);
		Ptr += XmlEscape(Ptr, User->GameName);
		Ptr += wsprintfW(Ptr, L"</text>");
	}
	if (User->StreamName[0])
	{
		Ptr += wsprintfW(Ptr, L"<text id=\"%u\">", Line++);
		Ptr += XmlEscape(Ptr, User->StreamName);
		Ptr += wsprintfW(Ptr, L"</text>");
	}
	Ptr += wsprintfW(Ptr, L"</binding></visual><actions>");

	if (IsMpvInPath())
	{
		Ptr += wsprintfW(Ptr, L"<action content=\"Play\" arguments=\"Phttps://www.twitch.tv/%s\"/>", User->Name);
	}
	Ptr += wsprintfW(Ptr, L"<action content=\"Open Browser\" arguments=\"Ohttps://www.twitch.tv/%s\"/>", User->Name);
	Ptr += wsprintfW(Ptr, L"</actions></toast>");

	int XmlLength = (int)(Ptr - Xml);
	void* Item = WindowsToast_Show(&State.Toast, Xml, XmlLength);
	WindowsToast_Release(&State.Toast, Item);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		AddTrayIcon(Window);
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
				if (User->UserId)
				{
					AppendMenuW(Users, User->IsOnline ? MF_CHECKED : MF_STRING, CMD_USER + Index, User->DisplayName);
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
	else if (Message == WM_TWITCH_NOTIFY_TRAY_ICON)
	{
		UpdateTrayIcon(Window, State.Icon[WParam]);
		return 0;
	}
 	else if (Message == WM_TIMER)
	{
		if (WParam == TIMER_RELOAD_USERS)
		{
			SetEvent(State.ReloadEvent);
			KillTimer(Window, TIMER_RELOAD_USERS);
			if (State.Websocket)
			{
				WinHttpWebSocketClose(State.Websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
			}
			return 0;
		}
		else if (WParam == TIMER_WEBSOCKET_PING)
		{
			char Data[] = "{\"type\":\"PING\"}";
			WinHttpWebSocketSend(State.Websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, Data, sizeof(Data) - 1);
			return 0;
		}
	}
	else if (WM_TASKBARCREATED && Message == WM_TASKBARCREATED)
	{
		AddTrayIcon(Window);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
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

	WCHAR Users[32768];
	Users[0] = 0;
	GetPrivateProfileSectionW(L"users", Users, ARRAYSIZE(Users), State.IniPath);

	WCHAR* Ptr = Users;

	int Count = 0;
	while (*Ptr != 0)
	{
		if (Count == MAX_USER_COUNT)
		{
			ShowTrayMessage(State.Window, NIIF_WARNING, L"More than 50 users is not supported");
			break;
		}
		User* User = &State.Users[Count];
		User->UserId = 0;

		WCHAR* Name = Ptr;
		StrTrimW(Name, L" \t");
		if (Name[0])
		{
			StrCpyNW(User->Name, Name, MAX_STRING_LENGTH);
			Count++;
		}
		Ptr += lstrlenW(Ptr) + 1;
	}
	State.UserCount = Count;
}

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

static void DownloadUserImage(LPWSTR ImagePath, LPCWSTR ImageUrl)
{
	DWORD ImageUrlLength = lstrlenW(ImageUrl);
	UINT64 Hash = GetFnv1Hash(ImageUrl, ImageUrlLength * sizeof(WCHAR));

	DWORD TempLength = GetTempPathW(MAX_PATH, ImagePath);
	Assert(TempLength);

	LPWSTR Extension = StrRChrW(ImageUrl, ImageUrl + ImageUrlLength, L'.');
	wsprintfW(ImagePath + TempLength, L"%08x%08x%s", (DWORD)(Hash >> 32), (DWORD)Hash, Extension);

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

	// download image

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

	if (!WinHttpCrackUrl(ImageUrl, ImageUrlLength, 0, &Url))
	{
		// bad image url
		return;
	}

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
						for (;;)
						{
							DWORD Read;
							if (!WinHttpReadData(Request, State.Buffer, MAX_BUFFER_SIZE, &Read))
							{
								break;
							}
							if (Read == 0)
							{
								break;
							}
							DWORD Written;
							WriteFile(File, State.Buffer, Read, &Written, NULL);
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

static DWORD SendGqlQuery(char* Query, DWORD QuerySize)
{
	DWORD ReadSize = 0;

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
					for (;;)
					{
						DWORD Read;
						if (!WinHttpReadData(Request, State.Buffer + ReadSize, MAX_BUFFER_SIZE - ReadSize, &Read))
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

static void DownloadUserList(void)
{
	char* Query = State.Buffer;

	char* Ptr = Query;
	{
		Ptr += wsprintfA(Ptr, "{\"query\":\"{users(logins:[");
		for (int UserIndex = 0; UserIndex < State.UserCount; UserIndex++)
		{
			Ptr += wsprintfA(Ptr, "%s\\\"%S\\\"", UserIndex ? "," : "", State.Users[UserIndex].Name);
		}
		// allowed image widths: 28, 50, 70, 150, 300, 600
		Ptr += wsprintfA(Ptr, "]){id,displayName,profileImageURL(width:300),stream{title,game{displayName}}}}\"}");
	}
	DWORD QuerySize = (DWORD)(Ptr - Query);

	DWORD ReadSize = SendGqlQuery(Query, QuerySize);
	if (ReadSize == 0)
	{
		ShowTrayMessage(State.Window, NIIF_ERROR, L"Failed to download user list from Twitch");
		return;
	}

	JsonObject* Json = JsonObject_Parse(State.Buffer, ReadSize);
	JsonArray* Errors = JsonObject_GetArray(Json, JsonCSTR("errors"));
	if (Errors)
	{
		JsonObject* ErrorMessage = JsonArray_GetObject(Errors, 0);
		LPCWSTR Message = JsonObject_GetString(ErrorMessage, JsonCSTR("message"));
		ShowTrayMessage(State.Window, NIIF_ERROR, Message);
		JsonRelease(ErrorMessage);
		JsonRelease(Errors);
		JsonRelease(Json);
		return;
	}

	JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
	JsonArray* Users = JsonObject_GetArray(Data, JsonCSTR("users"));

	UINT32 UsersCount = JsonArray_GetCount(Users);
	for (UINT32 Index = 0; Index < UsersCount; Index++)
	{
		User* User = &State.Users[Index];

		JsonObject* UserData = JsonArray_GetObject(Users, Index);
		if (UserData == NULL)
		{
			// user does not exist
			User->UserId = 0;
		}
		else
		{
			LPCWSTR Id = JsonObject_GetString(UserData, JsonCSTR("id"));
			LPCWSTR ProfileImageUrl = JsonObject_GetString(UserData, JsonCSTR("profileImageURL"));
			LPCWSTR DisplayName = JsonObject_GetString(UserData, JsonCSTR("displayName"));

			StrCpyNW(User->DisplayName, DisplayName ? DisplayName : User->Name, MAX_STRING_LENGTH);
			DownloadUserImage(User->ImagePath, ProfileImageUrl);
			User->UserId = StrToIntW(Id);

			JsonObject* Stream = JsonObject_GetObject(UserData, JsonCSTR("stream"));
			if (Stream)
			{
				LPCWSTR StreamName = JsonObject_GetString(Stream, JsonCSTR("title"));
				StrCpyNW(User->StreamName, StreamName ? StreamName : L"", MAX_STRING_LENGTH);

				JsonObject* Game = JsonObject_GetObject(Stream, JsonCSTR("game"));
				LPCWSTR GameName = JsonObject_GetString(Game, JsonCSTR("displayName"));
				StrCpyNW(User->GameName, GameName ? GameName : L"", MAX_STRING_LENGTH);

				User->IsOnline = TRUE;

				JsonRelease(Game);
				JsonRelease(Stream);
			}
			else
			{
				User->IsOnline = FALSE;
			}

			JsonRelease(UserData);
		}
	}
	JsonRelease(Users);
	JsonRelease(Data);
	JsonRelease(Json);
}

static void ProcessUserOnline(UINT UserId)
{
	for (int Index = 0; Index < State.UserCount; Index++)
	{
		User* User = &State.Users[Index];
		if (User->UserId == UserId)
		{
			char* Query = State.Buffer;
			int QuerySize = wsprintfA(Query, "{\"query\":\"{users(ids:[%u]){stream{title,game{displayName}}}}\"}", UserId);

			DWORD ReadSize;

			// try to get info 5 times with 1second delays
			// TODO: ideally this should be handle by timer on window to not block this thread
			int Retries = 5;
			for (int i = 0; i < 5; i++)
			{
				if (i != 0) Sleep(1000);
				ReadSize = SendGqlQuery(Query, QuerySize);
				if (ReadSize != 0)
				{
					break;
				}
			}

			User->StreamName[0] = 0;
			User->GameName[0] = 0;

			if (ReadSize != 0)
			{
				JsonObject* Json = JsonObject_Parse(State.Buffer, ReadSize);
				JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
				JsonArray* Users = JsonObject_GetArray(Data, JsonCSTR("users"));
				JsonObject* UserData = JsonArray_GetObject(Users, 0);
				JsonObject* Stream = JsonObject_GetObject(UserData, JsonCSTR("stream"));
				if (Stream)
				{
					LPCWSTR StreamName = JsonObject_GetString(Stream, JsonCSTR("title"));
					StrCpyNW(User->StreamName, StreamName ? StreamName : L"", MAX_STRING_LENGTH);

					JsonObject* Game = JsonObject_GetObject(Stream, JsonCSTR("game"));
					LPCWSTR GameName = JsonObject_GetString(Game, JsonCSTR("displayName"));
					StrCpyNW(User->GameName, GameName ? GameName : L"", MAX_STRING_LENGTH);

					JsonRelease(Game);
					JsonRelease(Stream);
				}

				JsonRelease(UserData);
				JsonRelease(Users);
				JsonRelease(Data);
				JsonRelease(Json);
			}

			User->IsOnline = TRUE;
			ShowUserNotification(User);

			return;
		}
	}
}

static void ProcessUserOffline(UINT UserId)
{
	for (int Index = 0; Index < State.UserCount; Index++)
	{
		if (State.Users[Index].UserId == UserId)
		{
			State.Users[Index].IsOnline = FALSE;
			return;
		}
	}
}

static void ConnectWebsocket(void)
{
	HINTERNET Connection = WinHttpConnect(State.Session, L"pubsub-edge.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!Connection)
	{
		return;
	}

	HINTERNET Request = WinHttpOpenRequest(Connection, L"GET", L"/v1", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
	if (!Request)
	{
		WinHttpCloseHandle(Connection);
		return;
	}

	WinHttpSetOption(Request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
	if (!WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(Request, 0))
	{
		WinHttpCloseHandle(Request);
		WinHttpCloseHandle(Connection);
		return;
	}

	HINTERNET Websocket = WinHttpWebSocketCompleteUpgrade(Request, 0);
	WinHttpCloseHandle(Request);
	if (!Websocket)
	{
		WinHttpCloseHandle(Connection);
		return;
	}

	State.Websocket = Websocket;

	DWORD Error;

	for (int Index = 0; Index < State.UserCount; Index++)
	{
		UINT UserId = State.Users[Index].UserId;

		if (UserId)
		{
			char Data[1024];
			int DataLength = wsprintfA(Data, "{\"type\":\"LISTEN\",\"data\":{\"topics\":[\"video-playback-by-id.%u\"]}}", UserId);

			Error = WinHttpWebSocketSend(Websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, Data, DataLength);
			if (Error != NO_ERROR)
			{
				break;
			}
		}
	}

	if (Error == NO_ERROR)
	{
		DWORD BufferSize = 0;

		PostMessageW(State.Window, WM_TWITCH_NOTIFY_TRAY_ICON, 0, 0);
		SetTimer(State.Window, TIMER_WEBSOCKET_PING, TIMER_WEBSOCKET_PING_INTERVAL, NULL);

		for (;;)
		{
			DWORD Read;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE Type;
			Error = WinHttpWebSocketReceive(Websocket, State.Buffer + BufferSize, MAX_BUFFER_SIZE - BufferSize, &Read, &Type);
			if (Error != NO_ERROR)
			{
				break;
			}
			BufferSize += Read;

			if (Type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
			{
				JsonObject* Json = JsonObject_Parse(State.Buffer, BufferSize);
				if (Json)
				{
					LPCWSTR JsonType = JsonObject_GetString(Json, JsonCSTR("type"));
					if (StrCmpW(JsonType, L"MESSAGE") == 0)
					{
						JsonObject* Data = JsonObject_GetObject(Json, JsonCSTR("data"));
						LPCWSTR Message = JsonObject_GetString(Data, JsonCSTR("message"));
						if (Message)
						{
							JsonObject* Msg = JsonObject_ParseW(Message, -1);
							LPCWSTR Type = JsonObject_GetString(Msg, JsonCSTR("type"));
							if (Type)
							{
								if (StrCmpW(Type, L"stream-up") == 0)
								{
									LPCWSTR Topic = JsonObject_GetString(Data, JsonCSTR("topic"));
									LPCWSTR Last = StrRChrW(Topic, NULL, L'.');
									if (Last != NULL)
									{
										int UserId = StrToIntW(Last + 1);
										ProcessUserOnline(UserId);
									}
								}
								else if (StrCmpW(Type, L"stream-down") == 0)
								{
									LPCWSTR Topic = JsonObject_GetString(Data, JsonCSTR("topic"));
									LPCWSTR Last = StrRChrW(Topic, NULL, L'.');
									if (Last != NULL)
									{
										int UserId = StrToIntW(Last + 1);
										ProcessUserOffline(UserId);
									}
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
				// binary message type is not expected
				break;
			}
		}

		KillTimer(State.Window, TIMER_WEBSOCKET_PING);
		PostMessageW(State.Window, WM_TWITCH_NOTIFY_TRAY_ICON, 1, 0);
	}

	WinHttpWebSocketClose(Websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
	WinHttpCloseHandle(Websocket);
	WinHttpCloseHandle(Connection);
}

static DWORD WINAPI UpdateThread(LPVOID Arg)
{
	for (;;)
	{
		LoadUsers();
		DownloadUserList();

		if (State.UserCount != 0)
		{
			DWORD Delay = 1000;
			for (;;)
			{
				ConnectWebsocket();
				if (WaitForSingleObject(State.ReloadEvent, Delay) == WAIT_OBJECT_0)
				{
					break;
				}
				if (Delay < 60 * 1000)
				{
					Delay *= 2;
				}
			}
		}
	}
}

static void TwitchNotify_OnActivated(WindowsToast* Toast, LPCWSTR Action)
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

	// initialize Windows Toasts
	WindowsToast_Init(&State.Toast, TWITCH_NOTIFY_NAME, TWITCH_NOTIFY_APPID);
	WindowsToast_HideAll(&State.Toast, TWITCH_NOTIFY_APPID);
	State.Toast.OnActivatedCallback = TwitchNotify_OnActivated;

	// initialize Windows HTTP Services
	State.Session = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	Assert(State.Session);

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
	State.Icon[0] = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
	State.Icon[1] = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(2));
	Assert(State.Icon[0] && State.Icon[1]);

	// create window & do the message loop
	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	State.Window = CreateWindowExW(0, WindowClass.lpszClassName, WindowClass.lpszClassName,
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		CW_USEDEFAULT, NULL, NULL, WindowClass.hInstance, NULL);
	Assert(State.Window);

	// start background thread for user list loading & websocket
	State.ReloadEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	Assert(State.ReloadEvent);
	HANDLE Thread = CreateThread(NULL, 0, &UpdateThread, NULL, 0, NULL);
	Assert(Thread);

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
