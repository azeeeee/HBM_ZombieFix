@echo off
setlocal
rem ------------------------------------------------------------------
rem  Build hbm_poolfix.asi and the dinput8.dll proxy (32-bit).
rem  Requires the MSVC x86 build tools (Visual Studio Build Tools).
rem ------------------------------------------------------------------

cd /d "%~dp0"

set "VCVARS="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" (
    set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" (
    set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" (
    set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
)

if not defined VCVARS (
    echo [!] Could not find vcvars32.bat. Edit this script with your VS path.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [!] vcvars32.bat failed.
    exit /b 1
)

if not exist out mkdir out

echo Building hbm_poolfix.asi ...
cl /nologo /LD /MT /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS ^
   src\hbm_poolfix.c /Fo:out\ /Fd:out\ ^
   /link /MACHINE:X86 /SUBSYSTEM:WINDOWS /OPT:NOREF /MAP:out\hbm_poolfix.map ^
   /OUT:out\hbm_poolfix.asi
if errorlevel 1 goto fail

echo Building dinput8.dll ...
cl /nologo /LD /MT /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS ^
   src\dinput8_proxy.c src\dinput8.def /Fo:out\ /Fd:out\ ^
   /link /MACHINE:X86 /SUBSYSTEM:WINDOWS /OPT:NOREF ^
   /OUT:out\dinput8.dll
if errorlevel 1 goto fail

echo.
echo Build OK:
dir /b out\*.asi out\*.dll
exit /b 0

:fail
echo.
echo [!] Build failed.
exit /b 1
