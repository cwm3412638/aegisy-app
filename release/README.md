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

不要用 ad-hoc 签名发布给长期使用的用户。ad-hoc 签名的代码身份会随每次构建变化，macOS 钥匙串会把更新后的程序视为新的访问方，导致已保存的登录 Token 或 API Key 再次出现授权提示。跨版本保持同一个 Developer ID 签名后，用户选择一次“始终允许”即可持续授权。

## Windows 发布

在 Visual Studio 2022 Developer Command Prompt 中执行：

```bat
set OPENSSL_DIR=C:\path\to\openssl\bin
set OPENSSL_ROOT_DIR=C:\path\to\openssl
set AEGISY_SPARKLE_PRIVATE_KEY_FILE=%USERPROFILE%\.aegisy\sparkle-private-key
package-windows.bat
```

需要可用的 Rust stable 工具链。打包前应在 Windows 上执行
`cargo test --workspace --manifest-path agent-runtime\Cargo.toml` 和严格 Clippy；
GitHub `windows-package` 工作流已将其作为安装包生成前的硬门禁。

`OPENSSL_ROOT_DIR` 是 CMake 链接和运行时 DLL 收集的唯一 OpenSSL 根目录。
`OPENSSL_DIR` 默认使用 `%OPENSSL_ROOT_DIR%\bin`，只能指向该根目录内部，不能混用另一套 OpenSSL。
脚本会拒绝重复或版本不一致的 SSL/Crypto DLL、非 x64 DLL 和 EXE 导入不匹配，随后在分发目录中对 `https://www.aegisy.cc/` 执行真实 Qt TLS 握手。可通过 `AEGISY_WINDOWS_TLS_PROBE_URL` 覆盖探针地址。只有 TLS 探针和 GUI 启动冒烟都成功，才会生成 Inno Setup 安装包。

脚本会构建 x64 Release、将 `aegisy-agentd.exe` 连同 Qt/WinSparkle/OpenSSL
一起收集、调用 Inno Setup，并生成：

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
