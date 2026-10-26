@echo off
setlocal
set "CONFIG=%~1"
if not defined CONFIG set "CONFIG=Debug"
if /i not "%CONFIG%"=="Debug" if /i not "%CONFIG%"=="Release" (
    echo Usage: build.cmd [Debug^|Release] [PlatformToolset] 1>&2
    exit /b 1
)
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBUILD set "MSBUILD=%%I"
)
if not defined MSBUILD (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe"
        if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%E\MSBuild\Current\Bin\MSBuild.exe" if not defined MSBUILD set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%E\MSBuild\Current\Bin\MSBuild.exe"
    )
)
if not defined MSBUILD (
    echo MSBuild not found. Install Visual Studio Desktop development with C++ and a Windows SDK. 1>&2
    exit /b 1
)
set "TOOLSET="
if not "%~2"=="" set "TOOLSET=/p:PlatformToolset=%~2"
"%MSBUILD%" "%~dp0minimal-directx12.vcxproj" /nologo /m /p:Configuration=%CONFIG% /p:Platform=x64 %TOOLSET%
exit /b %errorlevel%
