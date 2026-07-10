@echo off
setlocal EnableExtensions EnableDelayedExpansion

echo ==================================
echo  Aegisy Client Windows 打包
echo ==================================

where cmake >nul 2>nul || (echo [错误] 未找到 cmake & exit /b 1)
where windeployqt >nul 2>nul || (echo [错误] 未找到 windeployqt，请把 Qt bin 加入 PATH & exit /b 1)
where powershell >nul 2>nul || (echo [错误] 未找到 PowerShell & exit /b 1)

set "BUILD_DIR=build"
set "DIST_DIR=dist\AegisyClient"
set "UPDATE_DIR=dist\updates\windows"
set "WINDOWS_ARCH=x64"
set "UPDATE_OS=windows-x64"
set "UPDATE_BASE_URL=https://aegisy.cc/desktop/windows"
if not "%AEGISY_WINDOWS_UPDATE_BASE_URL%"=="" set "UPDATE_BASE_URL=%AEGISY_WINDOWS_UPDATE_BASE_URL%"

echo.
echo [1/6] 编译 Release (%WINDOWS_ARCH%)...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if "%OPENSSL_ROOT_DIR%"=="" (
    cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A %WINDOWS_ARCH% -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF || exit /b 1
) else (
    cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A %WINDOWS_ARCH% -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DOPENSSL_ROOT_DIR="%OPENSSL_ROOT_DIR%" || exit /b 1
)
cmake --build "%BUILD_DIR%" --config Release || exit /b 1

set "EXE=%BUILD_DIR%\Release\AegisyClient.exe"
if not exist "%EXE%" (echo [错误] 未找到 %EXE% & exit /b 1)
if not exist "%BUILD_DIR%\aegisy-version.txt" (echo [错误] 缺少版本文件 & exit /b 1)
set /p VERSION=<"%BUILD_DIR%\aegisy-version.txt"

echo.
echo [2/6] 组装分发目录...
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"
copy /y "%EXE%" "%DIST_DIR%\AegisyClient.exe" >nul || exit /b 1
copy /y "%BUILD_DIR%\Release\WinSparkle.dll" "%DIST_DIR%\WinSparkle.dll" >nul || exit /b 1
windeployqt --release --no-translations --dir "%DIST_DIR%" "%DIST_DIR%\AegisyClient.exe" || exit /b 1

echo.
echo [3/6] 补充 OpenSSL 运行库...
if "%OPENSSL_DIR%"=="" (
    echo [错误] OPENSSL_DIR 未设置，无法收集 OpenSSL 及 zlib 运行库
    exit /b 1
)
if not exist "%OPENSSL_DIR%\*.dll" (
    echo [错误] OPENSSL_DIR 中没有 DLL：%OPENSSL_DIR%
    exit /b 1
)
copy /y "%OPENSSL_DIR%\*.dll" "%DIST_DIR%\" >nul || exit /b 1

echo 正在验证分发目录可启动...
powershell -NoProfile -ExecutionPolicy Bypass -File release\smoke-test-windows-runtime.ps1 ^
    -Executable "%CD%\%DIST_DIR%\AegisyClient.exe" || exit /b 1

echo.
echo [4/6] 生成 Inno Setup 安装程序...
set "ISCC="
for /f "delims=" %%I in ('where iscc.exe 2^>nul') do if not defined ISCC set "ISCC=%%I"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC (echo [错误] 未找到 Inno Setup 6 的 ISCC.exe & exit /b 1)
"%ISCC%" /DMyAppVersion=%VERSION% installer.iss || exit /b 1

set "SETUP=dist\AegisyClientSetup-%VERSION%.exe"
if not exist "%SETUP%" (echo [错误] 未生成安装程序 %SETUP% & exit /b 1)

if not "%AEGISY_WINDOWS_CERT_SHA1%"=="" (
    echo.
    echo [5/6] Authenticode 签名...
    where signtool.exe >nul 2>nul || (echo [错误] 已配置证书但未找到 signtool.exe & exit /b 1)
    signtool sign /sha1 "%AEGISY_WINDOWS_CERT_SHA1%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 "%SETUP%" || exit /b 1
) else (
    echo.
    echo [5/6] 未配置 AEGISY_WINDOWS_CERT_SHA1，跳过 Authenticode 签名
)

echo.
echo [6/6] 生成 WinSparkle 更新源...
if exist "%UPDATE_DIR%" rmdir /s /q "%UPDATE_DIR%"
mkdir "%UPDATE_DIR%"
copy /y "%SETUP%" "%UPDATE_DIR%\" >nul || exit /b 1
set "NOTES=release\notes\%VERSION%-windows.md"
if not exist "%NOTES%" set "NOTES=release\notes\%VERSION%.md"
if not exist "%NOTES%" (echo [错误] 缺少发布说明 %NOTES% & exit /b 1)
copy /y "%NOTES%" "%UPDATE_DIR%\AegisyClient-%VERSION%-Windows-%WINDOWS_ARCH%.md" >nul || exit /b 1

set "PRIVATE_KEY=%AEGISY_SPARKLE_PRIVATE_KEY_FILE%"
if "%PRIVATE_KEY%"=="" set "PRIVATE_KEY=%USERPROFILE%\.aegisy\sparkle-private-key"
set "WINSPARKLE_TOOL=%BUILD_DIR%\_deps\WinSparkle-0.9.3\bin\winsparkle-tool.exe"
if not exist "%PRIVATE_KEY%" (
    echo [警告] 未找到 Sparkle 私钥：%PRIVATE_KEY%
    echo [警告] 安装程序已生成，但跳过 appcast 签名。发布更新前请设置 AEGISY_SPARKLE_PRIVATE_KEY_FILE。
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File release\generate-windows-appcast.ps1 ^
        -Version "%VERSION%" ^
        -Installer "%CD%\%UPDATE_DIR%\AegisyClientSetup-%VERSION%.exe" ^
        -ReleaseNotes "%CD%\%UPDATE_DIR%\AegisyClient-%VERSION%-Windows-%WINDOWS_ARCH%.md" ^
        -BaseUrl "%UPDATE_BASE_URL%" ^
        -PrivateKey "%PRIVATE_KEY%" ^
        -ToolPath "%CD%\%WINSPARKLE_TOOL%" ^
        -OutputPath "%CD%\%UPDATE_DIR%\appcast.xml" ^
        -Architecture "%UPDATE_OS%" || exit /b 1
    copy /y "%UPDATE_DIR%\appcast.xml" "dist\windows-appcast.xml" >nul
)

echo.
echo ==================================
echo  Windows 打包完成
echo  安装程序：%SETUP%
echo  更新目录：%UPDATE_DIR%
echo ==================================
exit /b 0
