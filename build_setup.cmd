@echo off
rem build_setup.cmd - build all three targets (GUI, CLI, installer).
rem
rem Order matters: setup.rc embeds the two exes from x64\Release as RCDATA,
rem so they must be built first or rc.exe will fail to find them.
rem
rem NOTE: keep this file ASCII-only. cmd.exe parses batch files with the OEM
rem code page (936 on zh-CN), so UTF-8 Chinese comments corrupt the script.
rem
rem Visual Studio is located with vswhere.exe (ships with every VS 2017+),
rem with fallbacks for hand-installed layouts. Do NOT hardcode a machine path
rem here: this script also runs on GitHub Actions runners.
setlocal
cd /d "%~dp0"

set "VSDIR="

rem --- 1) vswhere (most reliable) ---------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath 2^>nul`) do set "VSDIR=%%i"
)

rem --- 2) fallback: probe common install roots --------------------------
if not defined VSDIR call :probe "%ProgramFiles%\Microsoft Visual Studio"
if not defined VSDIR call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio"
if not defined VSDIR call :probe "D:\Microsoft Visual Studio"
if not defined VSDIR call :probe "C:\Microsoft Visual Studio"

if not defined VSDIR (
    echo ERROR: Visual Studio with the C++ toolset was not found.
    echo        Install "Desktop development with C++" or set VSDIR manually.
    exit /b 1
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat not found under "%VSDIR%".
    exit /b 1
)
set "MSBUILD=%VSDIR%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=%VSDIR%\MSBuild\15.0\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
    echo ERROR: MSBuild.exe not found under "%VSDIR%".
    exit /b 1
)
echo Using Visual Studio: %VSDIR%

echo === 0) set up the compiler environment ===
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

echo === 1) build the GUI (MSBuild) ===
taskkill /f /im FileEncryptor.exe >nul 2>&1
"%MSBUILD%" FileEncryptor.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal /nologo
if errorlevel 1 (
    echo GUI build failed
    exit /b 1
)

echo === 1b) build the CLI ===
pushd "%~dp0"
cl.exe /nologo /EHsc /std:c++17 /MT /O2 /GL /DNOMINMAX /utf-8 ^
    enc.cpp file_crypto.cpp update_check.cpp /Fe"x64\Release\enc.exe" ^
    /link /LTCG ws2_32.lib gdi32.lib crypt32.lib advapi32.lib user32.lib
popd
if errorlevel 1 (
    echo CLI build failed
    exit /b 1
)

if not exist "x64\Release\FileEncryptor.exe" (
    echo missing x64\Release\FileEncryptor.exe
    exit /b 1
)
if not exist "x64\Release\enc.exe" (
    echo missing x64\Release\enc.exe
    exit /b 1
)

echo === 2) compile resources ===
rc.exe /nologo /fo "x64\Release\setup.res" setup.rc
if errorlevel 1 (
    echo resource compile failed
    exit /b 1
)

echo === 3) compile the installer ===
cl.exe /nologo /EHsc /std:c++17 /MT /O2 /DNOMINMAX /utf-8 ^
    setup.cpp "x64\Release\setup.res" ^
    /Fe"x64\Release\FileEncryptorSetup.exe" ^
    /link /SUBSYSTEM:WINDOWS ^
    shell32.lib shlwapi.lib ole32.lib uuid.lib advapi32.lib user32.lib gdi32.lib
if errorlevel 1 (
    echo installer compile failed
    exit /b 1
)

del /q setup.obj enc.obj file_crypto.obj update_check.obj >nul 2>&1
del /q *.res *.exp >nul 2>&1
echo.
echo done: x64\Release\FileEncryptorSetup.exe
dir /b "x64\Release\FileEncryptorSetup.exe"
endlocal
exit /b 0

rem --- helper: find <root>\<VS version>\<edition> -----------------------
:probe
if not exist "%~1" exit /b 0
for /d %%v in ("%~1\*") do (
    for /d %%e in ("%%v\*") do (
        if exist "%%e\VC\Auxiliary\Build\vcvars64.bat" if not defined VSDIR set "VSDIR=%%e"
    )
)
exit /b 0
