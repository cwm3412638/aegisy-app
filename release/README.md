# 桌面客户端更新发布

macOS 和 Windows 使用独立 Appcast，避免不同平台误下载对方安装包：

- macOS：`https://aegisy.cc/desktop/macos/appcast.xml`
- Windows：`https://aegisy.cc/desktop/windows/appcast.xml`

两个平台使用同一对 Ed25519 密钥。公钥已经编译进客户端，私钥不能提交到 Git。

## 密钥备份

开发机的 Sparkle `aegisy` 账户已在 macOS 登录钥匙串中创建私钥。执行以下命令导出，并把文件安全传输到 Windows 打包机：

```bash
mkdir -p ~/.aegisy
build/_deps/sparkle-2.9.4/bin/generate_keys \
  --account aegisy -x ~/.aegisy/sparkle-private-key
chmod 600 ~/.aegisy/sparkle-private-key
```

Windows 默认从 `%USERPROFILE%\.aegisy\sparkle-private-key` 读取，也可以通过 `AEGISY_SPARKLE_PRIVATE_KEY_FILE` 指定其它路径。

## macOS 发布

`package-macos.sh` 生成：

- `dist/AegisyClient-<version>-macOS-<arch>.dmg`
- `dist/updates/macos/AegisyClient-<version>-macOS-<arch>.zip`
- `dist/updates/macos/AegisyClient-<version>-macOS-<arch>.md`
- `dist/updates/macos/appcast.xml`

正式公开发布需要 Apple Developer ID 和公证凭据：

```bash
export AEGISY_CODESIGN_IDENTITY="Developer ID Application: Company Name (TEAMID)"
export AEGISY_NOTARY_PROFILE="aegisy-notary"
./package-macos.sh
```

## Windows 发布

在 Visual Studio 2022 Developer Command Prompt 中执行：

```bat
set OPENSSL_DIR=C:\path\to\openssl\bin
set AEGISY_SPARKLE_PRIVATE_KEY_FILE=%USERPROFILE%\.aegisy\sparkle-private-key
package-windows.bat
```

脚本会构建 x64 Release、收集 Qt/WinSparkle/OpenSSL、调用 Inno Setup，并生成：

- `dist/AegisyClientSetup-<version>.exe`
- `dist/updates/windows/AegisyClientSetup-<version>.exe`
- `dist/updates/windows/AegisyClient-<version>-Windows-x64.md`
- `dist/updates/windows/appcast.xml`

如有 Windows 代码签名证书，可设置 `AEGISY_WINDOWS_CERT_SHA1`。没有 Authenticode 签名时 Ed25519 更新校验仍然有效，但 Windows SmartScreen 可能显示未知发布者。

## 服务器发布

将 `dist/updates/macos/` 上传到站点 `/desktop/macos/`，将 `dist/updates/windows/` 上传到 `/desktop/windows/`。Web 服务器必须直接返回 XML、ZIP、EXE 和 Markdown 文件，不能回退到 SPA 的 `index.html`。

OpenResty/Nginx 路由模板见 `release/nginx-desktop-updates.conf`。

发布后至少验证：

```bash
curl -fsSI https://aegisy.cc/desktop/macos/appcast.xml
curl -fsSI https://aegisy.cc/desktop/windows/appcast.xml
```

CI 可通过 `AEGISY_SPARKLE_PRIVATE_KEY` 向 macOS 脚本传入私钥内容；Windows 使用私钥文件路径。下载地址可分别通过 `AEGISY_UPDATE_BASE_URL` 和 `AEGISY_WINDOWS_UPDATE_BASE_URL` 覆盖。
