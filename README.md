# Aegisy Desktop Client

Aegisy Desktop Client 是一个跨平台 Qt 桌面应用，用于把 Aegisy 账号中的 API Key 安全接入 Claude Code、Codex CLI 和 Gemini CLI。

## 当前功能

- 使用 Aegisy 账号登录，支持密码显隐、余额展示，并分页同步 API Keys
- 每个档案只绑定一个终端：Claude Code、Codex CLI 或 Gemini CLI
- 按终端筛选、编辑、删除和激活档案；三种终端分别保留一个当前配置
- 系统托盘快速切换档案，关闭主窗口后可继续运行
- 查询 API Key 可用模型并保存模型选择
- 检测 Node.js、CLI 安装状态、本地版本和冲突环境变量
- 缺少环境时通过 Homebrew、WinGet 或 Linux 系统包管理器一键安装 Node.js 和对应 CLI
- 原子写入本地配置，写入失败自动回滚
- 每个终端保留最近 10 次配置备份，并支持手动恢复
- 使用密码加密导入、导出档案（PBKDF2 + AES-256-GCM）
- Windows DPAPI、macOS Keychain、Linux Secret Service 安全保存凭据
- macOS 使用 Sparkle 2、Windows 使用 WinSparkle 检查、下载、验证并安装应用更新

## 支持的终端

| 终端 | 主要配置文件 |
| --- | --- |
| Claude Code | `~/.claude/settings.json` |
| Codex CLI | `~/.codex/auth.json`、`~/.codex/config.toml` |
| Gemini CLI | `~/.gemini/.env` |

配置写入采用读、合并、原子提交，不会直接覆盖无关字段。每次激活前都会建立同批次备份。

## 系统要求

- CMake 3.16+
- C++17 编译器
- Qt 5.15+ 或 Qt 6
- OpenSSL
- Linux：桌面 Secret Service 和 `secret-tool`，Ubuntu/Debian 可安装 `libsecret-tools`

### macOS

```bash
brew install cmake qt@6 openssl@3
./build.sh
open build/AegisyClient.app
```

首次配置会从 Sparkle 官方发布页下载固定版本的 Framework 到 `build/_deps`。

也可以直接运行：

```bash
./build/AegisyClient.app/Contents/MacOS/AegisyClient
```

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev libqt6network6 libssl-dev libsecret-tools
./build.sh
./build/AegisyClient
```

### Windows

在 Visual Studio 2022 Developer Command Prompt 中运行：

```bat
build.bat
build\Release\AegisyClient.exe
```

生成完整 Windows 安装程序：

```bat
set OPENSSL_DIR=C:\path\to\openssl\bin
set AEGISY_SPARKLE_PRIVATE_KEY_FILE=C:\Users\you\.aegisy\sparkle-private-key
package-windows.bat
```

需要预先安装 Qt、OpenSSL、CMake、Visual Studio 2022 和 Inno Setup 6。
`OPENSSL_DIR` 必须指向 OpenSSL 的运行库目录，其中应同时包含该发行版依赖的 zlib DLL；打包脚本会复制目录中的全部 DLL 并在生成安装包前执行启动测试。

## 使用流程

1. 登录 Aegisy 账号。
2. 点击“新建配置”，输入名称并选择唯一终端。
3. 选择与该终端平台匹配的 API Key，可选查询并指定模型。
4. 保存并激活档案。应用会先备份，再更新对应终端配置；其它终端的当前档案不会被清除。
5. 需要回退时，点击顶部“备份”并恢复历史版本。
6. macOS 和 Windows 可从顶部“更新”菜单检查新版本或调整自动检查开关。

“迁移”菜单提供加密档案导入和导出。导出密码无法找回，请单独妥善保存。

## 安全说明

- 登录 Token 和档案 API Key 不写入普通日志。
- 档案元数据保存在 `QSettings`，真实 API Key 只保存在系统凭据库。
- Linux 没有可用 Secret Service 时，应用拒绝持久化凭据，不会退回固定密钥 XOR。
- 加密导出文件使用 PBKDF2-HMAC-SHA256（200,000 次）和 AES-256-GCM。
- 备份文件权限限制为当前用户读写。

## 构建与测试

开发构建及测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

`build.sh` 和 `build.bat` 生成 Release 应用，不构建测试目标。

## 项目结构

```text
include/                 公共接口
src/api_client.cpp       Aegisy API、分页和超时处理
src/profile_manager.cpp  单终端档案和安全凭据引用
src/profile_archive.cpp  加密档案导入导出
src/tool_manager.cpp     CLI 检测、配置事务、备份恢复
src/update_manager_mac.mm  macOS Sparkle 应用内更新桥接
src/update_manager_win.cpp Windows WinSparkle 应用内更新桥接
src/main_window.cpp      主界面与档案操作
tests/                   自动化测试
```

macOS、Windows 发布流程和更新源说明见 `release/README.md`。

## 后续方向

- 导入已有本地 Aegisy 配置
- 开机启动设置
- 自定义服务地址与供应商预设
- 用量统计和连接健康检测
- MCP、Prompts、Skills 管理

网站：<https://www.aegisy.cc>
