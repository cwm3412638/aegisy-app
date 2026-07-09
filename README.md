# Aegisy Desktop Client

跨平台桌面客户端，用于管理 Aegisy API 配置和环境。

## 功能特性（MVP 版本）

- ✅ **用户登录认证**：通过 Aegisy 账号登录
- ✅ **环境检测**：自动检测 Claude Desktop、Cursor、Continue.dev 的配置状态
- ✅ **安全存储**：使用系统密钥链（Windows DPAPI / macOS Keychain）加密存储凭证
- ✅ **跨平台支持**：Windows、macOS、Linux

## 系统要求

### Windows
- Windows 10/11
- Visual Studio 2019 或更新版本
- CMake 3.16+
- Qt 5.15+ 或 Qt 6.x
- OpenSSL

### macOS
- macOS 10.15+
- Xcode Command Line Tools
- CMake 3.16+
- Qt 5.15+ 或 Qt 6.x (通过 Homebrew)

### Linux
- Ubuntu 20.04+ / Debian 11+ / Fedora 35+
- GCC 7+ 或 Clang 5+
- CMake 3.16+
- Qt 5.15+ 或 Qt 6.x
- OpenSSL

## 快速开始

### 1. 安装依赖

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev libqt6network6 libssl-dev
```

**macOS:**
```bash
brew install cmake qt@6
```

**Windows:**
1. 安装 [Visual Studio 2022 Community](https://visualstudio.microsoft.com/)
2. 安装 [CMake](https://cmake.org/download/)
3. 安装 [Qt](https://www.qt.io/download-open-source-installer)

### 2. 克隆并构建

```bash
cd /root/aegisy-app

# Linux/macOS
chmod +x build.sh
./build.sh

# Windows (从 Developer Command Prompt 运行)
build.bat
```

### 3. 运行

```bash
# Linux/macOS
./build/AegisyClient

# Windows
build\Release\AegisyClient.exe
```

## API 端点

客户端连接到 `https://www.aegisy.cc`，使用以下 API：

- `POST /api/v1/auth/login` - 用户登录
- `GET /api/v1/keys` - 获取 API Keys
- `GET /api/v1/user` - 获取用户信息

## 配置文件检测

客户端会自动检测以下应用的配置：

### Claude Desktop
- **Windows**: `%APPDATA%\Claude\claude_desktop_config.json`
- **macOS**: `~/Library/Application Support/Claude/claude_desktop_config.json`
- **Linux**: `~/.config/Claude/claude_desktop_config.json`

### Cursor
- **Windows**: `%APPDATA%\Cursor\User\settings.json`
- **macOS**: `~/Library/Application Support/Cursor/User/settings.json`
- **Linux**: `~/.config/Cursor/User/settings.json`

### Continue.dev
- **所有平台**: `~/.continue/config.json`

### 环境变量
- `OPENAI_API_KEY`
- `OPENAI_BASE_URL`
- `ANTHROPIC_API_KEY`
- `ANTHROPIC_BASE_URL`

## 项目结构

```
aegisy-app/
├── CMakeLists.txt           # CMake 构建配置
├── build.sh                 # Linux/macOS 构建脚本
├── build.bat                # Windows 构建脚本
├── include/                 # 头文件
│   ├── api_client.h         # API 客户端
│   ├── secure_storage.h     # 安全存储
│   ├── env_detector.h       # 环境检测
│   ├── config_manager.h     # 配置管理
│   ├── login_dialog.h       # 登录对话框
│   └── main_window.h        # 主窗口
├── src/                     # 源文件
│   ├── main.cpp
│   ├── api_client.cpp
│   ├── secure_storage.cpp
│   ├── env_detector.cpp
│   ├── config_manager.cpp
│   ├── login_dialog.cpp
│   └── main_window.cpp
└── resources/               # 资源文件（图标、图片等）
```

## 安全特性

### 凭证存储
- **Windows**: 使用 DPAPI (Data Protection API) 加密
- **macOS**: 使用 Keychain Services
- **Linux**: 使用 XOR 加密 + 文件权限保护（后续可增强为 libsecret）

### 网络通信
- HTTPS/TLS 1.2+ 强制加密
- 支持证书锁定（Certificate Pinning）
- JWT Token 认证

### 代码保护
- 编译时字符串混淆（计划中）
- VMProtect 集成（计划中）
- 反调试机制（计划中）

## 开发路线图

### ✅ MVP (当前版本)
- [x] 用户登录
- [x] 环境检测
- [x] 安全存储
- [x] 基础 UI

### 🚧 v1.1 (下一版本)
- [ ] 多环境管理
- [ ] API Key 切换
- [ ] 自动配置写入
- [ ] 配置备份/恢复

### 📋 v1.2 (未来计划)
- [ ] 自动更新
- [ ] 使用统计
- [ ] 通知系统
- [ ] 暗色主题

### 🔒 v2.0 (安全加固)
- [ ] VMProtect 集成
- [ ] 证书锁定
- [ ] 反调试机制
- [ ] 代码签名

## 故障排除

### Qt 未找到
```bash
# Linux
export Qt6_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6

# macOS
export Qt6_DIR=/opt/homebrew/opt/qt@6/lib/cmake/Qt6
```

### OpenSSL 错误
```bash
# Ubuntu/Debian
sudo apt install libssl-dev

# macOS (通常已安装)
brew install openssl@3
```

### 编译错误
1. 确保使用 C++17 或更新版本
2. 清理构建目录：`rm -rf build && mkdir build`
3. 检查 CMake 输出中的错误信息

## 许可证

本项目使用 LGPLv3 许可证（因使用开源 Qt）。

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

- 网站：https://www.aegisy.cc
- 问题反馈：通过 GitHub Issues

## 致谢

- Qt Framework
- OpenSSL
- nlohmann/json (计划集成)
