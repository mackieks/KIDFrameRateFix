@echo off
rem Builds build\dinput8.dll (32-bit) with the MSVC toolchain.
rem Usage: build.cmd            - build only
rem        build.cmd install    - build and copy into ..\Binaries\Win32
setlocal
cd /d "%~dp0"

if defined VSCMD_ARG_TGT_ARCH goto :have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if defined VSINSTALL call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
:have_msvc
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
    echo An x86 MSVC environment is required.
    exit /b 1
)
where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC was not found. Install Visual Studio with the C++ x86 tools.
    exit /b 1
)

if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /W4 /WX /LD /DNDEBUG ^
   /Fo"build\\" src\KIDFrameRateFix.cpp src\DInput8Proxy.cpp src\TimingProbe.cpp src\CrashDump.cpp src\Resolution.cpp ^
   /link /DEF:src\dinput8.def /OUT:build\dinput8.dll /IMPLIB:build\dinput8.lib /PDB:build\dinput8.pdb /DEBUG:FULL /OPT:REF /OPT:ICF user32.lib
if errorlevel 1 exit /b %errorlevel%

if /i "%~1"=="install" (
    copy /y build\dinput8.dll ..\Binaries\Win32\dinput8.dll >nul || exit /b 1
    if not exist ..\Binaries\Win32\KIDFrameRateFix.ini copy /y KIDFrameRateFix.ini ..\Binaries\Win32\KIDFrameRateFix.ini >nul
    echo Installed to ..\Binaries\Win32
)
exit /b 0
