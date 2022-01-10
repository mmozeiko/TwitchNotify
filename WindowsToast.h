#pragma once

#include <windows.h>
#include <windows.ui.notifications.h>

// interface

typedef struct WindowsToast WindowsToast;
typedef __x_ABI_CWindows_CUI_CNotifications_CToastDismissalReason WindowsToast_DismissReason;

typedef void WindowsToast_OnActivated(WindowsToast* Toast, LPCWSTR Action);
typedef void WindowsToast_OnDismissed(WindowsToast* Toast, WindowsToast_DismissReason Reason);

struct WindowsToast
{
	// public
	WindowsToast_OnActivated* OnActivatedCallback;
	WindowsToast_OnDismissed* OnDismissedCallback;

	// private
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory* NotificationFactory;
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics* ManagerStatics;
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotifier* Notifier;

	__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable OnActivated;
	__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs OnDismissed;
};

// AppId is in form of "CompanyName.ProductName"
static void WindowsToast_Init(WindowsToast* Toast, LPCWSTR AppName, LPCWSTR AppId);
static void WindowsToast_Done(WindowsToast* Toast);
static void WindowsToast_HideAll(WindowsToast* Toast, LPCWSTR AppId);

// xml examples & schema
// https://docs.microsoft.com/en-us/windows/apps/design/shell/tiles-and-notifications/adaptive-interactive-toasts
// https://docs.microsoft.com/en-us/windows/apps/design/shell/tiles-and-notifications/toast-schema
// pass -1 for XmlLength if XML is 0 terminated
static void* WindowsToast_Show(WindowsToast* Toast, LPCWSTR Xml, int XmlLength);

// release memory for returned value of Show()
static void WindowsToast_Release(WindowsToast* Toast, void* Item);

// hide notification
static void WindowsToast_Hide(WindowsToast* Toast, void* Item);

// implementation

#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <roapi.h>

#pragma comment (lib, "shlwapi.lib")
#pragma comment (lib, "ole32.lib")
#pragma comment (lib, "runtimeobject.lib")

#ifndef Assert
#define Assert(Cond) ((void)(Cond))
#endif

#ifndef HR
#define HR(hr) ((void)(hr))
#endif

typedef struct {
	DWORD Flags;
	DWORD Length;
	DWORD Padding1;
	DWORD Padding2;
	LPCWSTR Ptr;
} HSTRING_IMPL;

#define STATIC_HSTRING(name, str) static HSTRING_IMPL _##name = { 1, sizeof(str)/sizeof(WCHAR) - 1, 0, 0, str }; static HSTRING name = (void*)&_##name
STATIC_HSTRING(ToastNotificationManagerName, L"Windows.UI.Notifications.ToastNotificationManager");
STATIC_HSTRING(ToastNotificationName,        L"Windows.UI.Notifications.ToastNotification");
STATIC_HSTRING(XmlDocumentName,              L"Windows.Data.Xml.Dom.XmlDocument");
#undef STATIC_HSTRING

DEFINE_GUID(IID_IToastNotificationManagerStatics,  0x50ac103f, 0xd235, 0x4598, 0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4);
DEFINE_GUID(IID_IToastNotificationManagerStatics2, 0x7ab93c52, 0x0e48, 0x4750, 0xba, 0x9d, 0x1a, 0x41, 0x13, 0x98, 0x18, 0x47);
DEFINE_GUID(IID_IToastNotificationFactory,         0x04124b20, 0x82c6, 0x4229, 0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53);
DEFINE_GUID(IID_IToastActivatedEventArgs,          0xe3bf92f3, 0xc197, 0x436f, 0x82, 0x65, 0x06, 0x25, 0x82, 0x4f, 0x8d, 0xac);
DEFINE_GUID(IID_IToastActivatedEventHandler,       0xab54de2d, 0x97d9, 0x5528, 0xb6, 0xad, 0x10, 0x5a, 0xfe, 0x15, 0x65, 0x30);
DEFINE_GUID(IID_IToastDismissedEventHandler,       0x61c2402f, 0x0ed0, 0x5a18, 0xab, 0x69, 0x59, 0xf4, 0xaa, 0x99, 0xa3, 0x68);
DEFINE_GUID(IID_IXmlDocumentIO,                    0x6cd0e74e, 0xee65, 0x4489, 0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37);

static HRESULT STDMETHODCALLTYPE WindowsToast__OnActivated_QueryInterface(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(Riid, &IID_IToastActivatedEventHandler) ||
		IsEqualGUID(Riid, &IID_IAgileObject) ||
		IsEqualGUID(Riid, &IID_IUnknown))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI STDMETHODCALLTYPE WindowsToast__OnActivated_AddRef(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable* This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE WindowsToast__OnActivated_Release(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable* This)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE WindowsToast__OnActivated_Invoke(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable* This, __x_ABI_CWindows_CUI_CNotifications_CIToastNotification* Sender, IInspectable* Args)
{
	WindowsToast* Toast = CONTAINING_RECORD(This, WindowsToast, OnActivated);
	if (Toast->OnActivatedCallback)
	{
		__x_ABI_CWindows_CUI_CNotifications_CIToastActivatedEventArgs* EventArgs;
		HR(IInspectable_QueryInterface(Args, &IID_IToastActivatedEventArgs, &EventArgs));

		HSTRING ArgString;
		HR(__x_ABI_CWindows_CUI_CNotifications_CIToastActivatedEventArgs_get_Arguments(EventArgs, &ArgString));

		Toast->OnActivatedCallback(Toast, WindowsGetStringRawBuffer(ArgString, NULL));
	}
	return S_OK;
}

static __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectableVtbl WindowsToast__OnActivatedVtbl =
{
	.QueryInterface = &WindowsToast__OnActivated_QueryInterface,
	.AddRef         = &WindowsToast__OnActivated_AddRef,
	.Release        = &WindowsToast__OnActivated_Release,
	.Invoke         = &WindowsToast__OnActivated_Invoke,
};

static HRESULT STDMETHODCALLTYPE WindowsToast__OnDismissed_QueryInterface(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(Riid, &IID_IToastDismissedEventHandler) ||
		IsEqualGUID(Riid, &IID_IAgileObject) ||
		IsEqualGUID(Riid, &IID_IUnknown))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI STDMETHODCALLTYPE WindowsToast__OnDismissed_AddRef(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs* This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE WindowsToast__OnDismissed_Release(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs* This)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE WindowsToast__OnDismissed_Invoke(__FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs* This, __x_ABI_CWindows_CUI_CNotifications_CIToastNotification* Sender, __x_ABI_CWindows_CUI_CNotifications_CIToastDismissedEventArgs* Args)
{
	WindowsToast* Toast = CONTAINING_RECORD(This, WindowsToast, OnDismissed);
	if (Toast->OnDismissedCallback)
	{
		__x_ABI_CWindows_CUI_CNotifications_CToastDismissalReason Reason;
		HR(__x_ABI_CWindows_CUI_CNotifications_CIToastDismissedEventArgs_get_Reason(Args, &Reason));
		Toast->OnDismissedCallback(Toast, Reason);
	}
	return S_OK;
}

static __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgsVtbl WindowsToast__OnDismissedVtbl =
{
	.QueryInterface = &WindowsToast__OnDismissed_QueryInterface,
	.AddRef         = &WindowsToast__OnDismissed_AddRef,
	.Release        = &WindowsToast__OnDismissed_Release,
	.Invoke         = &WindowsToast__OnDismissed_Invoke,
};

static void WindowsToast_Init(WindowsToast* Toast, LPCWSTR AppName, LPCWSTR AppId)
{
	// initialize Windows Runtime
	HR(RoInitialize(RO_INIT_MULTITHREADED));
	HR(SetCurrentProcessExplicitAppUserModelID(AppId));

	HSTRING_HEADER AppIdHeader;
	HSTRING AppIdString;
	HR(WindowsCreateStringReference(AppId, lstrlenW(AppId), &AppIdHeader, &AppIdString));

	// create Toast objects
	{
		HR(RoGetActivationFactory(ToastNotificationName, &IID_IToastNotificationFactory, &Toast->NotificationFactory));
		HR(SUCCEEDED(RoGetActivationFactory(ToastNotificationManagerName, &IID_IToastNotificationManagerStatics, &Toast->ManagerStatics)));
		HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics_CreateToastNotifierWithId(Toast->ManagerStatics, AppIdString, &Toast->Notifier));
	}

	// setup shortcut with proper properties
	{
		WCHAR LinkPath[MAX_PATH];
		Assert(0 != GetEnvironmentVariableW(L"APPDATA", LinkPath, ARRAYSIZE(LinkPath)));
		PathAppendW(LinkPath, L"Microsoft\\Windows\\Start Menu\\Programs");
		PathAppendW(LinkPath, AppName);
		StrCatW(LinkPath, L".lnk");

		IShellLinkW* ShellLink;
		HR(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, &ShellLink));

		IPersistFile* PersistFile;
		HR(IShellLinkW_QueryInterface(ShellLink, &IID_IPersistFile, &PersistFile));

		IPropertyStore* PropertyStore;
		HR(IShellLinkW_QueryInterface(ShellLink, &IID_IPropertyStore, &PropertyStore));

		if (GetFileAttributesW(LinkPath) != INVALID_FILE_ATTRIBUTES)
		{
			HR(IPersistFile_Load(PersistFile, LinkPath, STGM_READWRITE));

			PROPVARIANT Var;
			if (FAILED(IPropertyStore_GetValue(PropertyStore, &PKEY_AppUserModel_ID, &Var)))
			{
				Var.vt = VT_LPWSTR;
				Var.pwszVal = (LPWSTR)AppId;

				HR(IPropertyStore_SetValue(PropertyStore, &PKEY_AppUserModel_ID, &Var));
				HR(IPropertyStore_Commit(PropertyStore));
				if (IPersistFile_IsDirty(PersistFile) == S_OK)
				{
					HR(IPersistFile_Save(PersistFile, LinkPath, TRUE));
				}
			}
			else
			{
				PropVariantClear(&Var);
			}
		}
		else
		{
			WCHAR ExePath[MAX_PATH];
			GetModuleFileNameW(NULL, ExePath, ARRAYSIZE(ExePath));
			HR(IShellLinkW_SetPath(ShellLink, ExePath));

			HR(IShellLinkW_SetArguments(ShellLink, L""));

			PathRemoveFileSpecW(ExePath);
			HR(IShellLinkW_SetWorkingDirectory(ShellLink, ExePath));

			PROPVARIANT Var =
			{
				.vt = VT_LPWSTR,
				.pwszVal = (LPWSTR)AppId,
			};

			HR(IPropertyStore_SetValue(PropertyStore, &PKEY_AppUserModel_ID, &Var));
			HR(IPropertyStore_Commit(PropertyStore));
			HR(IPersistFile_Save(PersistFile, LinkPath, TRUE));
		}

		IPropertyStore_Release(PropertyStore);
		IPersistFile_Release(PersistFile);
		IShellLinkW_Release(ShellLink);
	}

	HR(WindowsDeleteString(AppIdString));

	Toast->OnActivated.lpVtbl = &WindowsToast__OnActivatedVtbl;
	Toast->OnDismissed.lpVtbl = &WindowsToast__OnDismissedVtbl;
}

static void WindowsToast_HideAll(WindowsToast* Toast, LPCWSTR AppId)
{
	HSTRING_HEADER AppIdHeader;
	HSTRING AppIdString;
	HR(WindowsCreateStringReference(AppId, lstrlenW(AppId), &AppIdHeader, &AppIdString));

	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2* ManagerStatics2;
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory* NotificationHistory;
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics_QueryInterface(Toast->ManagerStatics, &IID_IToastNotificationManagerStatics2, &ManagerStatics2));
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2_get_History(ManagerStatics2, &NotificationHistory));
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory_ClearWithId(NotificationHistory, AppIdString));
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory_Release(NotificationHistory);
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2_Release(ManagerStatics2);

	HR(WindowsDeleteString(AppIdString));
}

static void WindowsToast_Done(WindowsToast* Toast)
{
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotifier_Release(Toast->Notifier);
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics_Release(Toast->ManagerStatics);
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory_Release(Toast->NotificationFactory);

	RoUninitialize();
}

static void* WindowsToast_Show(WindowsToast* Toast, LPCWSTR Xml, int XmlLength)
{
	__x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument* XmlDocument;
	HR(RoActivateInstance(XmlDocumentName, (IInspectable**)&XmlDocument));
	{
		__x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO* XmlIO;
		HR(__x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument_QueryInterface(XmlDocument, &IID_IXmlDocumentIO, &XmlIO));
		{
			HSTRING_HEADER XmlStringHeader;
			HSTRING XmlString;
			HR(WindowsCreateStringReference(Xml, XmlLength >= 0 ? XmlLength : lstrlenW(Xml), &XmlStringHeader, &XmlString));
			HR(__x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO_LoadXml(XmlIO, XmlString));
			WindowsDeleteString(XmlString);
		}
		__x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO_Release(XmlIO);
	}

	__x_ABI_CWindows_CUI_CNotifications_CIToastNotification* Notification;
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory_CreateToastNotification(Toast->NotificationFactory, XmlDocument, &Notification));

	EventRegistrationToken ActivatedToken;
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotification_add_Activated(Notification, &Toast->OnActivated, &ActivatedToken));

	EventRegistrationToken DismissedToken;
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotification_add_Dismissed(Notification, &Toast->OnDismissed, &DismissedToken));

	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotifier_Show(Toast->Notifier, Notification));

	__x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument_Release(XmlDocument);

	return Notification;
}

static void WindowsToast_Release(WindowsToast* Toast, void* Item)
{
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotification* Notification = Item;
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotification_Release(Notification);
}

static void WindowsToast_Hide(WindowsToast* Toast, void* Item)
{
	__x_ABI_CWindows_CUI_CNotifications_CIToastNotification* Notification = Item;
	HR(__x_ABI_CWindows_CUI_CNotifications_CIToastNotifier_Hide(Toast->Notifier, Notification));
}
