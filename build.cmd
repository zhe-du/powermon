@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem Build both variants with MinGW-w64.
rem Usage:  build.cmd [path\to\g++.exe]
rem   default: g++ from PATH

set "CXX=g++"
if not "%~1"=="" set "CXX=%~1"

set "FLAGS=-std=c++17 -O2 -municode -mwindows -static"
set "LIBS=-lgdi32 -luser32 -lole32 -loleaut32 -ladvapi32 -luuid -lshell32 -lpdh -lshlwapi"

echo [1/3] compiling resources
windres src\app.rc -O coff -o src\app.res.o
if errorlevel 1 goto :err

echo [2/3] full build   -> dist\PowerMon.exe
"%CXX%" %FLAGS% -o dist\PowerMon.exe src\main.cpp src\app.res.o %LIBS%
if errorlevel 1 goto :err

echo [3/3] lite build   -> dist\PowerMon-Lite.exe
"%CXX%" %FLAGS% -DPOWERMON_LITE -o dist\PowerMon-Lite.exe src\main.cpp src\app.res.o %LIBS%
if errorlevel 1 goto :err

echo.
echo done:
dir /b dist
exit /b 0

:err
echo.
echo BUILD FAILED
exit /b 1
