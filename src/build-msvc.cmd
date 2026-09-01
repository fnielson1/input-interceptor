@echo off
setlocal

rem Standalone x64 build using a modern Visual Studio toolchain, as an
rem alternative to buildit.cmd, which needs the legacy WDK build environment
rem (%WDK%\bin\setenv) that no longer ships with current kits.
rem
rem interceptor.c calls no CRT function, so this links /NODEFAULTLIB against
rem kernel32 and user32 alone (the latter for SetWindowsHookEx/RegisterRaw-
rem InputDevices/SendInput): no msvcrt import, no VC++ redistributable
rem requirement.
rem
rem   /GS-          no stack cookie (would pull __security_check_cookie from the CRT)
rem   /Gs1000000    no stack probe (__chkstk likewise); frames here are well under 4K
rem
rem Output: build\x64\interceptor.dll
rem
rem Note on style: the toolchain lookup below deliberately uses goto rather than
rem parenthesised if-blocks. The path to vswhere.exe contains "(x86)", and a
rem closing paren inside a block closes the block at parse time, mangling it.

set "OUTDIR=%~dp0build\x64"

if defined VSINSTALLDIR goto :have_toolchain

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :run_vswhere
set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :run_vswhere

echo ERROR: vswhere.exe not found; is Visual Studio installed?
exit /b 1

:run_vswhere
set "VSPATH="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%I"

if not defined VSPATH goto :no_toolset
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto :no_toolset

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_toolset
goto :have_toolchain

:no_toolset
echo ERROR: no Visual Studio install with the C++ x64 toolset was found.
exit /b 1

:have_toolchain
where cl >nul 2>&1
if errorlevel 1 goto :no_toolset

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

pushd "%~dp0"

echo Compiling...
cl /nologo /c /O2 /W4 /WX /GS- /Gs1000000 /DINTERCEPTOR_EXPORT /Fo"%OUTDIR%\\" interceptor.c dllmain.c
if errorlevel 1 goto :failed

echo Compiling resources...
rc /nologo /fo "%OUTDIR%\interceptor.res" interceptor-standalone.rc
if errorlevel 1 goto :failed

echo Linking...
link /nologo /DLL /NODEFAULTLIB /ENTRY:DllMain /OPT:REF /OPT:ICF /OUT:"%OUTDIR%\interceptor.dll" /IMPLIB:"%OUTDIR%\interceptor.lib" "%OUTDIR%\interceptor.obj" "%OUTDIR%\dllmain.obj" "%OUTDIR%\interceptor.res" kernel32.lib user32.lib
if errorlevel 1 goto :failed

popd
echo.
echo Built %OUTDIR%\interceptor.dll
exit /b 0

:failed
popd
echo.
echo BUILD FAILED
exit /b 1
