@echo off
setlocal enabledelayedexpansion

rem Installs the locally-built interceptor-driver as a LowerFilter on the
rem Keyboard and Mouse device setup classes (see interceptor-driver.inf).
rem
rem *** This is a class filter driver: once installed, it loads for every
rem *** keyboard and mouse Windows enumerates, at boot, before anyone logs
rem *** in. A bug in it can leave you without a working keyboard or mouse.
rem *** If that happens: boot into Safe Mode (third-party filters don't
rem *** load there) or use driver\uninstall-wdk.cmd / `pnputil
rem *** /delete-driver ... /uninstall` from a recovery command prompt, then
rem *** reboot again.
rem
rem Requires:
rem   - An elevated (Administrator) command prompt.
rem   - driver\build-wdk.cmd already run, producing
rem     driver\build\x64\Debug\interceptor-driver.{sys,inf,cat,cer}.
rem
rem A locally built driver is only test-signed (see interceptor-driver.vcxproj
rem Driversign settings), so this also enables test-signing mode
rem (bcdedit /set testsigning on) if it isn't already, and imports the
rem test-sign certificate into the Trusted Root and Trusted Publisher
rem stores. Both machine-wide changes; test-signing additionally needs a
rem reboot to take effect. This script does not reboot for you.

set "PKGDIR=%~dp0build\x64\Debug"
set "INF=%PKGDIR%\interceptor-driver.inf"
set "CER=%PKGDIR%\interceptor-driver.cer"

rem --- elevation check ---
net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: this must be run from an elevated ^(Administrator^) command
    echo prompt -- installing a driver and changing boot configuration both
    echo require it.
    exit /b 1
)

rem --- build output present? ---
if not exist "%INF%" (
    echo ERROR: "%INF%" not found.
    echo Run driver\build-wdk.cmd first.
    exit /b 1
)

echo.
echo === interceptor-driver install ===
echo INF: %INF%
echo.
echo This installs a system-wide keyboard/mouse class filter driver. A bad
echo build can leave keyboard/mouse input non-functional until you boot
echo into Safe Mode or a recovery prompt and remove it. Press Ctrl+C now to
echo abort, or
pause

rem --- test-signing mode ---
set "TESTSIGN_ON=0"
for /f "tokens=1,*" %%A in ('bcdedit /enum "{current}" ^| findstr /i "testsigning"') do (
    if /i "%%B"=="Yes" set "TESTSIGN_ON=1"
)

set "NEEDS_REBOOT=0"

if "%TESTSIGN_ON%"=="0" (
    echo Enabling test-signing mode ^(required to load a locally-signed driver^)...
    bcdedit /set testsigning on >nul
    if errorlevel 1 (
        echo ERROR: bcdedit /set testsigning on failed.
        exit /b 1
    )
    set "NEEDS_REBOOT=1"
) else (
    echo Test-signing mode already enabled.
)

rem --- trust the test certificate ---
if exist "%CER%" (
    echo Importing test certificate into Trusted Root and Trusted Publishers...
    certutil -addstore -f Root "%CER%" >nul
    if errorlevel 1 echo WARNING: failed to add certificate to the Trusted Root store.
    certutil -addstore -f TrustedPublisher "%CER%" >nul
    if errorlevel 1 echo WARNING: failed to add certificate to the Trusted Publisher store.
) else (
    echo WARNING: "%CER%" not found; skipping certificate trust step.
    echo If the driver fails to load with a signature error, re-run
    echo driver\build-wdk.cmd ^(which produces it^), or import the signing
    echo certificate manually.
)

rem --- publish + install ---
echo.
echo Installing driver package...
pnputil /add-driver "%INF%" /install
if errorlevel 1 (
    echo.
    echo INSTALL FAILED -- pnputil reported an error, see above.
    exit /b 1
)

echo.
echo Driver package installed.
if "%NEEDS_REBOOT%"=="1" (
    echo.
    echo A REBOOT IS REQUIRED for test-signing mode to take effect before the
    echo driver will actually load.
) else (
    echo.
    echo A reboot is recommended so the class filter attaches to your
    echo existing keyboard and mouse ^(LowerFilters only takes effect for
    echo devices enumerated after this point^).
)
echo.
echo To remove it later: driver\uninstall-wdk.cmd

exit /b 0
