# 🚀 Aegisy 桌面客户端 - 快速启动指南

## 5分钟快速开始

### Windows 用户

1. **安装依赖**（首次使用）
   ```powershell
   # 下载并安装（按顺序）：
   # 1. Visual Studio 2022 Community: https://visualstudio.microsoft.com/
   # 2. CMake: https://cmake.org/download/
   # 3. Qt 6: https://www.qt.io/download-open-source-installer
   ```

2. **构建项目**
   ```cmd
   # 打开 "Developer Command Prompt for VS 2022"
   cd C:\path\to\aegisy-app
   build.bat
   ```

3. **运行程序**
   ```cmd
   build\Release\AegisyClient.exe
   ```

---

### macOS 用户

1. **安装依赖**（一行命令）
   ```bash
   # 安装 Homebrew（如果未安装）
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   
   # 安装开发工具
   xcode-select --install
   brew install cmake qt@6
   ```

2. **构建并运行**
   ```bash
   cd /path/to/aegisy-app
   ./build.sh
   ./build/AegisyClient
   ```

---

### Linux 用户（Ubuntu/Debian）

1. **一键安装依赖并构建**
   ```bash
   # 安装依赖
   sudo apt update
   sudo apt install -y build-essential cmake qt6-base-dev libqt6network6 libssl-dev
   
   # 构建项目
   cd /root/aegisy-app
   ./build.sh
   
   # 运行（需要 GUI 环境）
   ./build/AegisyClient
   ```

---

## 功能演示

### 1. 登录界面
启动后首先看到登录界面：
- 输入您的 Aegisy 邮箱和密码
- 勾选"记住我"可保存登录状态
- 点击"登录"

### 2. 环境检测
登录成功后自动检测以下环境：
- ✅ Claude Desktop 配置
- ✅ Cursor 编辑器配置
- ✅ Continue.dev 配置
- ✅ 系统环境变量

### 3. 查看状态
主界面显示：
- 每个应用的配置状态（已配置/未配置）
- 当前使用的 API Key（部分遮罩）
- Base URL 地址
- 详细日志输出

---

## 测试账号

请使用您自己的 Aegisy 账号：
- 网站：https://www.aegisy.cc
- 如果还没有账号，请先注册

---

## 常见问题

### Q: 提示 "Qt not found"
**A:** 确保 Qt 已正确安装并设置环境变量：
```bash
# macOS
export Qt6_DIR=/opt/homebrew/opt/qt@6/lib/cmake/Qt6

# Linux
export Qt6_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6
```

### Q: Windows 编译失败
**A:** 确保使用 "Developer Command Prompt for VS" 而不是普通命令提示符

### Q: macOS 提示"无法打开"
**A:** 右键点击程序 → 选择"打开"以绕过 Gatekeeper

### Q: Linux 无法运行 GUI
**A:** 确保在桌面环境中运行，而不是 SSH 终端

---

## 配置文件位置

客户端会检测以下位置的配置：

### Claude Desktop
- Windows: `%APPDATA%\Claude\claude_desktop_config.json`
- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
- Linux: `~/.config/Claude/claude_desktop_config.json`

### Cursor
- Windows: `%APPDATA%\Cursor\User\settings.json`
- macOS: `~/Library/Application Support/Cursor/User/settings.json`
- Linux: `~/.config/Cursor/User/settings.json`

### Continue.dev
- 所有平台: `~/.continue/config.json`

---

## 下一步

1. ✅ 登录并查看环境检测结果
2. 📝 记录需要改进的地方
3. 🔧 等待 v1.1 版本（自动配置功能）
4. 💡 提出您的功能建议

---

## 获取帮助

- 📖 详细文档：`README.md`
- 🔧 开发文档：`DEVELOPMENT.md`
- 📊 调研报告：`feasibility-report.md`
- 🎯 项目总结：`PROJECT-SUMMARY.md`

---

## 版本信息

- **当前版本**: v1.0.0 (MVP)
- **发布日期**: 2024
- **许可证**: LGPLv3（使用开源 Qt）

---

**祝使用愉快！** 🎉

如有问题或建议，欢迎反馈！
