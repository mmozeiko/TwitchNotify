@echo off
setlocal enabledelayedexpansion

if not "%VS140COMNTOOLS%" == "" goto vs2015
if not "%VS120COMNTOOLS%" == "" goto vs2013

echo Could not find VS2013 or VS2015.
goto :eof

:vs2015
    call "%VS140COMNTOOLS%..\..\VC\vcvarsall.bat" amd64
    goto vssetupdone

:vs2013
    call "%VS120COMNTOOLS%..\..\VC\vcvarsall.bat" amd64
    goto vssetupdone

:vssetupdone

set CL=/nologo /errorReport:none /Wall /WX /GS- /Gm- /GR- /fp:fast /EHa-
set LINK=/errorReport:none /INCREMENTAL:NO /NODEFAULTLIB /SUBSYSTEM:WINDOWS
set LINK=%LINK% kernel32.lib user32.lib shell32.lib shlwapi.lib ole32.lib wininet.lib windowscodecs.lib

if 1 == 1 (
    rem release
    where /q git.exe
    if "%ERRORLEVEL%" equ "0" (
        for /f "tokens=1" %%t in ('git.exe rev-list --count master') do (
            set REV=%%t
        )
        for /f "tokens=1" %%t in ('git.exe log --oneline -n 1') do (
            set COMMIT=%%t
        )
        set CL=%CL% /DTWITCH_NOTIFY_VERSION=\"r!REV!-!COMMIT!\"
    )
    set CL=!CL! /Ox /Os /GF /Gy
    set LINK=%LINK% /OPT:REF /OPT:ICF
) else (
    rem debug
    set CL=!CL! /Oi /Zi /D_DEBUG
    set LINK=%LINK% /DEBUG
)

ml64.exe /nologo /errorReport:none /c /Zi chkstk.asm
rc.exe /nologo TwitchNotify.rc
cl.exe TwitchNotify.c TwitchNotify.res chkstk.obj /FdTwitchNotify.pdb /FeTwitchNotify.exe
