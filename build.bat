@echo off
REM Aegisy Client - Build Script for Windows

echo ==================================
echo Aegisy Client Build Script
echo ==================================

REM 检查 CMake
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: CMake is not installed
    echo Please install CMake from https://cmake.org/download/
    pause
    exit /b 1
)

REM 检查 Visual Studio
where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: Visual Studio compiler not found
    echo Please run this from "Developer Command Prompt for VS"
    pause
    exit /b 1
)

echo.
echo All dependencies found!

REM 创建构建目录
echo.
echo Creating build directory...
if not exist build mkdir build
cd build

REM 配置 CMake
echo.
echo Configuring CMake...
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF

if %ERRORLEVEL% NEQ 0 (
    echo Error: CMake configuration failed
    pause
    exit /b 1
)

REM 编译
echo.
echo Building...
cmake --build . --config Release

if %ERRORLEVEL% NEQ 0 (
    echo Error: Build failed
    pause
    exit /b 1
)

echo.
echo ==================================
echo Build completed successfully!
echo ==================================
echo Executable: build\Release\AegisyClient.exe
echo.
pause
