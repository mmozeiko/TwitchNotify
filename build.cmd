@echo off
setlocal enabledelayedexpansion

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )  
  call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" amd64 || exit /b 1
)

if "%VSCMD_ARG_TGT_ARCH%" neq "x64" (
  echo ERROR: please run this from MSVC x64 native tools command prompt, 32-bit target is not supported!
  exit /b 1
)

if "%1" equ "debug" (
  set CL=/MTd /Od /Zi /D_DEBUG /RTC1 /FdTwitchNotify.pdb /fsanitize=address
  set LINK=/DEBUG libucrtd.lib libvcruntimed.lib
) else (
  set CL=/O1 /DNDEBUG /GS-
  set LINK=/OPT:REF /OPT:ICF libvcruntime.lib
)
rc.exe /nologo TwitchNotify.rc
cl.exe /nologo TwitchNotify.c TwitchNotify.res /W3 /WX /FC /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:TwitchNotify.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata
del *.obj *.res >nul
