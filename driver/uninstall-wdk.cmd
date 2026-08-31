@echo off
setlocal enabledelayedexpansion

rem Removes the interceptor-driver package installed by
rem driver\install-wdk.cmd: unpublishes it (which also clears its
rem LowerFilters registry entries off the Keyboard/Mouse classes and
rem stops/deletes the InterceptorDriver service), then reports whether
rem test-signing mode is still enabled so you can turn it off once you're
rem done developing against the driver.
rem
rem Requires: an elevated (Administrator) command prompt.

rem --- elevation check ---
net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: this must be run from an elevated ^(Administrator^) command prompt.
    exit /b 1
)

echo Looking for a published interceptor-driver package...

set "PUBNAME="
set "CANDIDATE="
for /f "tokens=1,* delims=:" %%A in ('pnputil /enum-drivers') do (
    set "FIELD=%%A"
    set "VALUE=%%B"
    for /f "tokens=* delims= " %%V in ("!VALUE!") do set "VALUE=%%V"
    if /i "!FIELD!"=="Published Name" set "CANDIDATE=!VALUE!"
    if /i "!FIELD!"=="Original Name" (
        if /i "!VALUE!"=="interceptor-driver.inf" set "PUBNAME=!CANDIDATE!"
    )
)

if not defined PUBNAME (
    echo No published interceptor-driver package found ^(pnputil /enum-drivers^);
    echo nothing to remove.
    goto :testsign_note
)

echo Removing %PUBNAME% ^(interceptor-driver.inf^)...
pnputil /delete-driver "%PUBNAME%" /uninstall
if errorlevel 1 (
    echo ERROR: pnputil /delete-driver failed, see above.
    exit /b 1
)

echo.
echo Driver package removed. Reboot to make sure it's unloaded from any
echo keyboard/mouse it's currently attached to.

:testsign_note
set "TESTSIGN_ON=0"
for /f "tokens=1,*" %%A in ('bcdedit /enum "{current}" ^| findstr /i "testsigning"') do (
    if /i "%%B"=="Yes" set "TESTSIGN_ON=1"
)
if "%TESTSIGN_ON%"=="1" (
    echo.
    echo Test-signing mode is still enabled ^(bcdedit /set testsigning on^).
    echo If you're done developing against this driver, turn it off and
    echo reboot: bcdedit /set testsigning off
)

exit /b 0
