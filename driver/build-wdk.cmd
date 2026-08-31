@echo off
setlocal enabledelayedexpansion

rem Builds interceptor-driver.sys with the modern, VS-integrated WDK
rem (MSBuild-based), as the driver-side counterpart to src\build-msvc.cmd.
rem
rem Unlike the DLL in src\, there is no WDK-free path here: kernel code
rem always needs real WDK headers/import libraries (ntddk.h, ntoskrnl.lib,
rem the km\ headers), so this requires the "Windows Driver Kit" Visual
rem Studio component in addition to the C++ x64 toolset -- installable from
rem the Visual Studio Installer (Individual Components) or the standalone
rem WDK installer, matching the installed Windows SDK version.
rem
rem 64-bit only: this always builds with the 64-bit MSBuild.exe
rem (MSBuild\Current\Bin\amd64) and targets Platform=x64. Some WDK releases
rem only ship InfVerif.dll for the x64/arm64 host tool directories, not x86,
rem so the 32-bit MSBuild.exe fails the InfVerif build step outright; there
rem is no x86 driver configuration in interceptor-driver.vcxproj either.
rem
rem Output: build\x64\Debug\interceptor-driver.{sys,inf,cat}

set "OUTDIR=%~dp0build\x64"
set "CONFIG=Debug"
if not "%~1"=="" set "CONFIG=%~1"

if defined VSINSTALLDIR (
    set "VSPATH=%VSINSTALLDIR%"
    goto :have_vspath
)

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

:have_vspath
if "%VSPATH:~-1%"=="\" set "VSPATH=%VSPATH:~0,-1%"

set "MSBUILD=%VSPATH%\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%MSBUILD%" goto :no_msbuild

rem A missing WDK shows up as MSBuild failing to resolve the Driver
rem ConfigurationType / WindowsKernelModeDriver10.0 toolset -- check for that
rem up front so the failure message is actionable instead of a raw MSBuild
rem property-sheet error.
if not exist "%ProgramFiles(x86)%\Windows Kits\10\Include\wdf" if not exist "%ProgramFiles(x86)%\Windows Kits\10\build\WindowsDriver.KernelMode.props" goto :maybe_no_wdk

call :find_wdk_version
if defined WDKVER (
    "%MSBUILD%" "%~dp0interceptor-driver.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:WindowsTargetPlatformVersion=%WDKVER% /nologo
) else (
    "%MSBUILD%" "%~dp0interceptor-driver.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /nologo
)
if errorlevel 1 goto :failed

echo.
echo Built %OUTDIR%\%CONFIG%\interceptor-driver.sys
exit /b 0

:maybe_no_wdk
call :find_wdk_version
if defined WDKVER (
    "%MSBUILD%" "%~dp0interceptor-driver.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /p:WindowsTargetPlatformVersion=%WDKVER% /nologo
) else (
    "%MSBUILD%" "%~dp0interceptor-driver.vcxproj" /p:Configuration=%CONFIG% /p:Platform=x64 /nologo
)
if errorlevel 1 goto :no_wdk
echo.
echo Built %OUTDIR%\%CONFIG%\interceptor-driver.sys
exit /b 0

:no_wdk
echo.
echo BUILD FAILED -- this looks like a missing WDK rather than a code error.
echo Install the "Windows Driver Kit" Visual Studio component (Visual Studio
echo Installer ^> Individual components), matching your installed Windows
echo SDK version, or the standalone WDK installer from Microsoft, then retry.
exit /b 1

:no_toolset
echo ERROR: no Visual Studio install with the C++ x64 toolset was found.
exit /b 1

:no_msbuild
echo ERROR: no 64-bit MSBuild.exe found under "%VSPATH%\MSBuild\Current\Bin\amd64".
echo This requires a Visual Studio install with MSBuild's 64-bit toolset;
echo the 32-bit MSBuild.exe cannot load this WDK's InfVerif.dll.
exit /b 1

:failed
echo.
echo BUILD FAILED
exit /b 1

rem Picks the newest installed Windows SDK version that actually has WDK
rem kernel headers (Include\<ver>\km). A machine can have more than one
rem SDK version under Windows Kits\10 -- e.g. one matching the running OS
rem build, installed with Visual Studio, plus a newer one pulled in by the
rem standalone WDK installer -- and the km\ subtree (ntddk.h etc.) only
rem exists under whichever version the WDK installer actually targeted.
rem Leaving WindowsTargetPlatformVersion=10.0 to auto-resolve can silently
rem pick the km-less SDK version and fail with "Cannot open include file:
rem 'ntddk.h'". Sets WDKVER, or leaves it undefined if none is found (in
rem which case the caller falls back to plain auto-resolution).
:find_wdk_version
set "WDKVER="
for /f "delims=" %%D in ('dir /b /ad-h /o-n "%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.*" 2^>nul') do (
    if not defined WDKVER if exist "%ProgramFiles(x86)%\Windows Kits\10\Include\%%D\km" set "WDKVER=%%D"
)
exit /b 0
