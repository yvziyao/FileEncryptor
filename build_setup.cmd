@echo off
rem build_setup.cmd - build the "standard" (installed) edition: FileEncryptorSetup.exe
rem
rem Order matters: setup.rc embeds the two exes from x64\Release as RCDATA,
rem so they must be built first or rc.exe will fail to find them.
rem
rem NOTE: keep this file ASCII-only. cmd.exe parses batch files with the OEM
rem code page (936 on zh-CN), so UTF-8 Chinese comments corrupt the script.
setlocal
cd /d "%~dp0"

echo === 1) build GUI and CLI ===
call "D:\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo vcvars64.bat not found - check the Visual Studio path
    exit /b 1
)

taskkill /f /im FileEncryptor.exe >nul 2>&1
"D:\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" ^
    FileEncryptor.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal /nologo
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
