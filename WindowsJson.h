#pragma once

#include <windows.h>
#include <windows.data.json.h>

// interface

typedef __x_ABI_CWindows_CData_CJson_CIJsonObject JsonObject;
typedef __x_ABI_CWindows_CData_CJson_CIJsonArray  JsonArray;

#define JsonCSTR(Name) (HSTRING)&(JsonHSTRING){ 1, sizeof(Name) - 1, 0, 0, L ## Name }
#define JsonRelease(Obj) do { if (Obj) IUnknown_Release((IUnknown*)Obj); } while (0)

// NOTE: make sure you have called RoInitialize() before using any of functions

// if zero terminated, can set Length to -1
static JsonObject* JsonObject_Parse(LPCSTR String, int Length);
static JsonObject* JsonObject_ParseW(LPCWSTR String, int Length);
static JsonObject* JsonObject_ParseStr(HSTRING String);

static JsonObject* JsonObject_GetObject (JsonObject* Object, HSTRING Name);
static JsonArray*  JsonObject_GetArray  (JsonObject* Object, HSTRING Name);
static HSTRING     JsonObject_GetString (JsonObject* Object, HSTRING Name);
static double      JsonObject_GetNumber (JsonObject* Object, HSTRING Name);
static boolean     JsonObject_GetBoolean(JsonObject* Object, HSTRING Name);

static UINT32      JsonArray_GetCount  (JsonArray* Array);
static JsonObject* JsonArray_GetObject (JsonArray* Array, UINT32 Index);
static JsonArray*  JsonArray_GetArray  (JsonArray* Array, UINT32 Index);
static HSTRING     JsonArray_GetString (JsonArray* Array, UINT32 Index);
static double      JsonArray_GetNumber (JsonArray* Array, UINT32 Index);
static boolean     JsonArray_GetBoolean(JsonArray* Array, UINT32 Index);

// implementation

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
} JsonHSTRING;

DEFINE_GUID(IID_IJsonObject,        0x2289f159, 0x54de, 0x45d8, 0xab, 0xcc, 0x22, 0x60, 0x3f, 0xa0, 0x66, 0xa0);
DEFINE_GUID(IID_IVector_IJsonValue, 0xd44662bc, 0xdce3, 0x59a8, 0x92, 0x72, 0x4b, 0x21, 0x0f, 0x33, 0x90, 0x8b);

JsonObject* JsonObject_Parse(LPCSTR String, int Length)
{
	if (Length < 0) Length = lstrlenA(String);
	int WideLength = MultiByteToWideChar(CP_UTF8, 0, String, Length, NULL, 0);
	Assert(WideLength != 0);

	WCHAR* Wide = LocalAlloc(0, WideLength * sizeof(WCHAR));
	MultiByteToWideChar(CP_UTF8, 0, String, Length, Wide, WideLength);

	JsonObject* Result = JsonObject_ParseW(Wide, WideLength);

	LocalFree(Wide);

	return Result;
}

JsonObject* JsonObject_ParseW(LPCWSTR String, int Length)
{
	JsonHSTRING WideString = { 1, Length < 0 ? lstrlenW(String) : Length, 0, 0, String };
	return JsonObject_ParseStr((HSTRING)&WideString);
}

JsonObject* JsonObject_ParseStr(HSTRING String)
{
	__x_ABI_CWindows_CData_CJson_CIJsonObjectStatics* ObjectStatics;
	HR(RoGetActivationFactory(JsonCSTR("Windows.Data.Json.JsonObject"), &IID_IJsonObject, &ObjectStatics));

	JsonObject* Object;
	HRESULT hr;
	if (FAILED(hr = __x_ABI_CWindows_CData_CJson_CIJsonObjectStatics_Parse(ObjectStatics, String, &Object)))
	{
		Object = NULL;
	}
	__x_ABI_CWindows_CData_CJson_CIJsonObjectStatics_Release(ObjectStatics);

	return Object;
}

JsonObject* JsonObject_GetObject(JsonObject* Object, HSTRING Name)
{
	JsonObject* Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedObject(Object, Name, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

JsonArray* JsonObject_GetArray(JsonObject* Object, HSTRING Name)
{
	JsonArray* Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedArray(Object, Name, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

HSTRING JsonObject_GetString(JsonObject* Object, HSTRING Name)
{
	HSTRING Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedString(Object, Name, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

double JsonObject_GetNumber(JsonObject* Object, HSTRING Name)
{
	double Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedNumber(Object, Name, &Result)))
	{
		Result = 0;
	}
	return Result;
}

boolean JsonObject_GetBoolean(JsonObject* Object, HSTRING Name)
{
	boolean Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedBoolean(Object, Name, &Result)))
	{
		Result = 0;
	}
	return Result;
}

UINT32 JsonArray_GetCount(JsonArray* Array)
{
	if (!Array)
	{
		return 0;
	}

	UINT32 Count;
	
	__FIVector_1_Windows__CData__CJson__CIJsonValue* Vector;
	HR(__x_ABI_CWindows_CData_CJson_CIJsonArray_QueryInterface(Array, &IID_IVector_IJsonValue, &Vector));
	if (FAILED(__FIVector_1_Windows__CData__CJson__CIJsonValue_get_Size(Vector, &Count)))
	{
		Count = 0;
	}
	__FIVector_1_Windows__CData__CJson__CIJsonValue_Release(Vector);

	return Count;
}

JsonObject* JsonArray_GetObject(JsonArray* Array, UINT32 Index)
{
	JsonObject* Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetObjectAt(Array, Index, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

JsonArray* JsonArray_GetArray(JsonArray* Array, UINT32 Index)
{
	JsonArray* Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetArrayAt(Array, Index, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

HSTRING JsonArray_GetString(JsonArray* Array, UINT32 Index)
{
	HSTRING Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetStringAt(Array, Index, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

double JsonArray_GetNumber(JsonArray* Array, UINT32 Index)
{
	double Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetNumberAt(Array, Index, &Result)))
	{
		Result = 0;
	}
	return Result;
}

boolean JsonArray_GetBoolean(JsonArray* Array, UINT32 Index)
{
	boolean Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetBooleanAt(Array, Index, &Result)))
	{
		Result = FALSE;
	}
	return Result;
}
