@echo off
REM ============================================================================
REM  MSAT Sync launcher (Windows) - menu front-end for msatsync.sh (Git Bash).
REM  Sync data from the MSAT device, clean up old files, or protect the output
REM  folder. Copyright (c) 2026 Burapha University. All rights reserved.
REM  Inventor/developer: Teeranan Nongnual <teeranan.no@buu.ac.th>, Burapha Univ.
REM  Petty Patent pending: Application No. 2603001145 (filed 2026-05-12).
REM  License: PolyForm Noncommercial 1.0.0 (see \LICENSE) - noncommercial use,
REM  modification allowed, selling prohibited.
REM  SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
REM ============================================================================
color F0
setlocal
set "LAUNCHER_VERSION=V.Y2026.88.20"

:: Relative destination folder: in MSAT-SYNC\MSAT-Output
set "SCRIPT_DIR=%~dp0"
set "WIN_DEST=%SCRIPT_DIR%MSAT-Output"

:: Check if folder exists, create if not
if not exist "%WIN_DEST%" mkdir "%WIN_DEST%"

:: Convert Path from Windows to Bash format (e.g. /d/...)
set "SH_DEST=/%WIN_DEST::=%"
set "SH_DEST=%SH_DEST:\=/%"

echo.
echo MSAT Sync Launcher %LAUNCHER_VERSION%
echo Destination: %WIN_DEST%
echo.

:: Find bash.exe (Git Bash) from common install locations or PATH
call :find_bash

if not defined BASH_EXE (
	echo [X] Git Bash not found
	echo     Checked PATH and common Git for Windows install folders.
	echo     Install Git for Windows or edit PATH in runmsatsync.bat
	pause
	exit /b 1
)

echo Using Bash: %BASH_EXE%

:: Detect current folder protection state (no admin needed, just reads ACL).
:: The protect script applies "(RX,W)" for the current user; if we see it
:: in the ACL listing the folder is currently protected.
set "PROTECT_STATE=UNPROTECTED"
icacls "%WIN_DEST%" 2>nul | findstr /C:"(RX,W)" >nul && set "PROTECT_STATE=PROTECTED"

echo.
echo ==== Choose run mode ====
echo   Folder status: %PROTECT_STATE%
echo.
echo   [1] Sync files MSAT -^> PC          (default)
echo   [2] Clean up space on MSAT          (delete files older than N days)
echo   --
echo   [4] PROTECT MSAT-Output folder      (block accidental delete/rename)
echo   [5] UNPROTECT MSAT-Output folder    (restore normal permissions)
echo   [Q] Quit
echo.
set "CHOICE="
set /p "CHOICE=Choose [1/2/4/5/Q]: "
if /i "%CHOICE%"=="Q" exit /b 0
if "%CHOICE%"=="" set "CHOICE=1"

if "%CHOICE%"=="4" goto :do_protect
if "%CHOICE%"=="5" goto :do_unprotect

set "MODE_ARG="
set "DAYS_ARG="
if "%CHOICE%"=="1" set "MODE_ARG=--mode=sync"
if "%CHOICE%"=="2" set "MODE_ARG=--mode=cleanup"
if not defined MODE_ARG (
    echo Invalid choice "%CHOICE%". Defaulting to sync.
    set "MODE_ARG=--mode=sync"
    set "CHOICE=1"
)

if "%CHOICE%"=="2" goto :ask_days
goto :launch

:ask_days
set "DAYS="
set /p "DAYS=Days threshold [30]: "
if "%DAYS%"=="" set "DAYS=30"
set "DAYS_ARG=--cleanup-days=%DAYS%"
goto :launch

:: ---- PROTECT: lock MSAT-Output (no delete/rename for current user) ----
:: No admin elevation needed - the user owns the folder (it's in their
:: OneDrive profile) and so holds WriteDAC implicitly, which is all that
:: icacls needs to change the ACL.
:do_protect
where icacls >nul 2>&1 || (echo [X] icacls.exe not found in PATH. & pause & exit /b 1)
echo.
echo Target : "%WIN_DEST%"
echo User   : %USERNAME%
echo.
echo This will allow %USERNAME% to READ / COPY OUT / CREATE NEW files,
echo but BLOCK DELETE and RENAME of existing files.
echo Administrators + SYSTEM keep full control as a safety net.
echo.
choice /C YN /M "Proceed"
if errorlevel 2 exit /b 0
echo.
echo [1/4] Reset child ACLs to inherit from folder...
icacls "%WIN_DEST%" /reset /T /C >nul
echo [2/4] Stop inheriting from parent (lock down)...
icacls "%WIN_DEST%" /inheritance:r >nul
echo [3/4] Grant Administrators + SYSTEM full control...
icacls "%WIN_DEST%" /grant *S-1-5-32-544:(OI)(CI)F >nul
icacls "%WIN_DEST%" /grant *S-1-5-18:(OI)(CI)F >nul
echo [4/4] Grant %USERNAME%: Read + Write, no Delete/Rename...
icacls "%WIN_DEST%" /grant "%USERNAME%":(OI)(CI)(RX,W) >nul
echo.
echo [OK] Folder protected. Try deleting a file in Explorer to confirm.
echo.
pause
exit /b 0

:: ---- UNPROTECT: restore normal permissions (delete/rename allowed) ----
:do_unprotect
where icacls >nul 2>&1 || (echo [X] icacls.exe not found in PATH. & pause & exit /b 1)
echo.
echo Target : "%WIN_DEST%"
echo This restores normal Modify access (delete + rename allowed).
echo.
choice /C YN /M "Proceed"
if errorlevel 2 exit /b 0
echo.
echo [1/2] Reset ACL on folder and children to defaults...
icacls "%WIN_DEST%" /reset /T /C >nul
echo [2/2] Re-enable inheritance from parent...
icacls "%WIN_DEST%" /inheritance:e >nul
echo.
echo [OK] Folder unlocked. Delete/rename allowed again.
echo.
pause
exit /b 0

:launch
echo.

:: --no-manifest: device's cached _index.txt/sync_manifest goes stale and
:: misses freshly recorded files. /listfiles?limit=all does a live SD scan.
"%BASH_EXE%" "%SCRIPT_DIR%msatsync.sh" "%SH_DEST%" --no-manifest %MODE_ARG% %DAYS_ARG% %*

pause
exit /b 0

:find_bash
set "BASH_EXE="

for %%P in (
	"%ProgramFiles%\Git\bin\bash.exe"
	"%ProgramFiles%\Git\usr\bin\bash.exe"
	"%ProgramFiles(x86)%\Git\bin\bash.exe"
	"%ProgramFiles(x86)%\Git\usr\bin\bash.exe"
	"%LocalAppData%\Programs\Git\bin\bash.exe"
	"%LocalAppData%\Programs\Git\usr\bin\bash.exe"
) do (
	if exist "%%~P" (
		set "BASH_EXE=%%~P"
		goto :eof
	)
)

for /f "delims=" %%P in ('where bash.exe 2^>nul') do (
	set "BASH_EXE=%%~fP"
	goto :eof
)

for /f "delims=" %%P in ('where git.exe 2^>nul') do (
	if exist "%%~dpP..\bin\bash.exe" (
		set "BASH_EXE=%%~dpP..\bin\bash.exe"
		goto :eof
	)
	if exist "%%~dpP..\usr\bin\bash.exe" (
		set "BASH_EXE=%%~dpP..\usr\bin\bash.exe"
		goto :eof
	)
)

goto :eof