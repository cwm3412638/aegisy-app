@echo off
REM ============================================================
REM  Aegisy Client - Windows 打包脚本
REM  产出：dist\AegisyClient\  可分发目录（含全部依赖）
REM  可选：用 Inno Setup 再打成单文件安装程序
REM ============================================================
setlocal enabledelayedexpansion

echo ==================================
echo  Aegisy Client Windows 打包
echo ==================================

REM ---- 1) 前置检查 ----
where cmake >nul 2>nul || (echo [错误] 未找到 cmake & pause & exit /b 1)
where windeployqt >nul 2>nul || (echo [错误] 未找到 windeployqt，请确认 Qt 的 bin 目录在 PATH 中 & pause & exit /b 1)

REM ---- 2) 编译 Release ----
echo.
echo [1/4] 编译 Release...
if not exist build mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release || (echo [错误] CMake 配置失败 & cd .. & pause & exit /b 1)
cmake --build . --config Release || (echo [错误] 编译失败 & cd .. & pause & exit /b 1)
cd ..

set EXE=build\Release\AegisyClient.exe
if not exist "%EXE%" (echo [错误] 未找到 %EXE% & pause & exit /b 1)

REM ---- 3) 组装 dist 目录 ----
echo.
echo [2/4] 组装分发目录 dist\AegisyClient ...
if exist dist\AegisyClient rmdir /s /q dist\AegisyClient
mkdir dist\AegisyClient
copy /y "%EXE%" dist\AegisyClient\ >nul

echo.
echo [3/4] windeployqt 收集 Qt 依赖...
windeployqt --release --no-translations --dir dist\AegisyClient dist\AegisyClient\AegisyClient.exe || (echo [错误] windeployqt 失败 & pause & exit /b 1)

REM ---- 4) 补 OpenSSL DLL（windeployqt 不含）----
echo.
echo [4/4] 拷贝 OpenSSL DLL...
if "%OPENSSL_DIR%"=="" (
    echo [警告] 环境变量 OPENSSL_DIR 未设置，跳过 OpenSSL DLL 拷贝。
    echo        请手动把 libssl-3-x64.dll 和 libcrypto-3-x64.dll 拷到 dist\AegisyClient\
) else (
    copy /y "%OPENSSL_DIR%\libssl-3-x64.dll" dist\AegisyClient\ >nul 2>nul
    copy /y "%OPENSSL_DIR%\libcrypto-3-x64.dll" dist\AegisyClient\ >nul 2>nul
    echo 已从 %OPENSSL_DIR% 拷贝 OpenSSL DLL
)

echo.
echo ==================================
echo  完成！可分发目录：dist\AegisyClient
echo ==================================
echo.
echo 下一步（生成单文件安装程序）：
echo   1. 安装 Inno Setup: https://jrsoftware.org/isdl.php
echo   2. 用 Inno Setup 打开项目根目录的 installer.iss 并编译（或命令行 iscc installer.iss）
echo   3. 产出：dist\AegisyClientSetup-1.0.0.exe
echo.
pause
