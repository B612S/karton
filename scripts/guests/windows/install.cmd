@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
rem
rem Karton guest tools installer for Windows guests.
rem
rem Run this file inside the guest (double-click it or start it from an
rem elevated prompt). It downloads and installs the virtio-win guest tools
rem bundle (VirtIO drivers, QEMU guest agent, SPICE agent), falls back to the
rem driver MSI, installs the maintained SPICE agent (side/forward mouse buttons
rem need it), installs WinFsp for VirtIO FS shares and offers to restart
rem Windows. The guest needs internet access: Karton does not download or
rem execute anything in the guest.
rem
rem Usage: install.cmd [/quiet]

setlocal EnableExtensions

set "QUIET="
if /i "%~1"=="/quiet" set "QUIET=1"

rem Writes require administrator rights: restart elevated when needed.
net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Administrator rights are required. Asking for elevation...
    if defined QUIET (
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '/quiet' -Verb RunAs"
    ) else (
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    )
    exit /b 0
)

set "LOG=%TEMP%\karton-guest-tools.log"
set "WORK=%TEMP%\karton-guest-tools"
if not exist "%WORK%" mkdir "%WORK%" >nul 2>&1

>"%LOG%" echo Karton guest tools, %DATE% %TIME%
echo Karton guest tools installer
echo Log: %LOG%

set "TOOLS=%WORK%\virtio-win-guest-tools.exe"
set "DRIVER_MSI=%WORK%\virtio-win-gt-x64.msi"
set "WINFSP_MSI=%WORK%\winfsp.msi"
set "VDAGENT_MSI=%WORK%\spice-vdagent-x64.msi"
set "VDAGENT_URL=https://github.com/nefarius/vd_agent/releases/download/v0.13.0/spice-vdagent-x64-0.13.0.msi"
set "RC=0"
set "DRIVERS_OK="

echo.
echo Downloading the virtio-win guest tools...
call :download "https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/latest-virtio/virtio-win-guest-tools.exe" "%TOOLS%"
if errorlevel 1 goto :driver_msi
call :at_least "%TOOLS%" 10000000
if errorlevel 1 goto :driver_msi

echo Installing VirtIO drivers and the QEMU/SPICE guest agents...
start /wait "" "%TOOLS%" /install /quiet /norestart ACCEPTEULA=1 >>"%LOG%" 2>&1
set "RC=%errorlevel%"
echo Guest tools exit code: %RC% >>"%LOG%"
call :accepted %RC%
if not errorlevel 1 set "DRIVERS_OK=1"
if defined DRIVERS_OK goto :winfsp

:driver_msi
echo Downloading the VirtIO driver package instead...
call :download "https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/latest-virtio/virtio-win-gt-x64.msi" "%DRIVER_MSI%"
if errorlevel 1 goto :drivers_failed
call :at_least "%DRIVER_MSI%" 1000000
if errorlevel 1 goto :drivers_failed
echo Installing the VirtIO driver package...
start /wait "" msiexec /i "%DRIVER_MSI%" /qn /norestart /l*v "%WORK%\karton-virtio-drivers-msi.log" >>"%LOG%" 2>&1
set "RC=%errorlevel%"
echo Driver MSI exit code: %RC% >>"%LOG%"
call :accepted %RC%
if not errorlevel 1 set "DRIVERS_OK=1"

:winfsp
if not defined DRIVERS_OK goto :drivers_failed
echo.
echo Downloading WinFsp for VirtIO FS file sharing...
call :download "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi" "%WINFSP_MSI%"
if errorlevel 1 (
    echo WinFsp download failed; VirtIO FS shares will not work. >>"%LOG%"
    goto :vdagent
)
echo Installing WinFsp...
start /wait "" msiexec /i "%WINFSP_MSI%" /qn /norestart /l*v "%WORK%\karton-winfsp-msi.log" >>"%LOG%" 2>&1
set "RC=%errorlevel%"
echo WinFsp MSI exit code: %RC% >>"%LOG%"
call :accepted %RC%
if errorlevel 1 echo WinFsp installation failed with exit code %RC%. >>"%LOG%"

:vdagent
echo.
echo Updating the SPICE agent for side/forward mouse buttons...
call :download "%VDAGENT_URL%" "%VDAGENT_MSI%"
if errorlevel 1 (
    echo SPICE agent download failed; side mouse buttons may not work. >>"%LOG%"
    goto :finished
)
call :at_least "%VDAGENT_MSI%" 100000
if errorlevel 1 goto :finished
echo Installing the updated SPICE agent...
start /wait "" msiexec /i "%VDAGENT_MSI%" /qn /norestart /l*v "%WORK%\karton-spice-vdagent-msi.log" >>"%LOG%" 2>&1
set "RC=%errorlevel%"
echo SPICE agent MSI exit code: %RC% >>"%LOG%"
call :accepted %RC%
if errorlevel 1 echo SPICE agent update failed with exit code %RC%. >>"%LOG%"

:finished
echo.
echo Installation finished.
if defined QUIET goto :cleanup
choice /c YN /t 30 /d N /m "Restart Windows now"
if errorlevel 2 goto :cleanup
echo Restarting Windows...
shutdown /r /t 10
goto :cleanup

:drivers_failed
echo.
echo The VirtIO drivers could not be installed.
echo See the log for details: %LOG%
if not defined QUIET pause
endlocal
exit /b 1

:cleanup
rmdir /s /q "%WORK%" >nul 2>&1
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem Subroutines
rem ---------------------------------------------------------------------------

:download
rem curl.exe ships with Windows 10 1803 and newer; fall back to PowerShell
rem (available everywhere) for older guests.
where curl.exe >nul 2>&1
if not errorlevel 1 (
    curl.exe -L --fail --retry 3 --connect-timeout 20 -o "%~2" "%~1" >>"%LOG%" 2>&1
    if not errorlevel 1 exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; try { (New-Object Net.WebClient).DownloadFile('%~1','%~2'); exit 0 } catch { Write-Host $_.Exception.Message; exit 1 }" >>"%LOG%" 2>&1
if errorlevel 1 (
    echo Download failed: %~1 >>"%LOG%"
    exit /b 1
)
exit /b 0

rem Fail when the file is missing or smaller than the expected minimum size.
:at_least
if not exist "%~1" exit /b 1
for %%A in ("%~1") do if %%~zA LSS %~2 exit /b 1
exit /b 0

rem Exit codes 0, 3010 and 1641 mean success (the last two require a reboot),
rem 1638 means another version of the product is already installed.
:accepted
if "%~1"=="0" exit /b 0
if "%~1"=="3010" exit /b 0
if "%~1"=="1641" exit /b 0
if "%~1"=="1638" exit /b 0
exit /b 1
