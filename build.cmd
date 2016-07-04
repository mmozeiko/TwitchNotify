@echo off

call "%VS140COMNTOOLS%..\..\VC\vcvarsall.bat" amd64

set CL=/nologo /errorReport:none /Wall /WX /GS- /Gm- /GR- /fp:fast /EHa-
set LINK=/errorReport:none /INCREMENTAL:NO /NODEFAULTLIB /SUBSYSTEM:WINDOWS
set LINK=%LINK% kernel32.lib user32.lib shell32.lib wininet.lib

if 1 == 1 (
  rem release
  set CL=%CL% /Ox /GF /Gy
  set LINK=%LINK% /OPT:REF /OPT:ICF
) else (
  rem debug
  set CL=%CL% /Oi /Zi /D_DEBUG
  set LINK=%LINK% /DEBUG
)

ml64.exe /nologo /errorReport:none /c /Zi chkstk.asm
rc.exe /nologo TwitchNotify.rc
cl.exe TwitchNotify.c TwitchNotify.res chkstk.obj /FdTwitchNotify.pdb /FeTwitchNotify.exe
