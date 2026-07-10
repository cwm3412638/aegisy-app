# Aegisy Desktop Client - MVP 开发说明

> 注意：本文后半部分保留了早期 MVP 设计记录。当前支持 Claude Code、Codex CLI
> 和 Gemini CLI，架构与功能清单以 `README.md`、`include/` 和 `src/` 为准。

## 当前状态

✅ **MVP 核心功能已完成**

### 已实现的功能

1. **API 客户端** (`api_client.cpp`)
   - HTTPS 通信
   - JWT Token 认证
   - 登录接口（已验证格式：`email + password`）
   - API Keys 获取接口
   - 错误处理

2. **安全存储** (`secure_storage.cpp`)
   - Windows: DPAPI 加密
   - macOS: Keychain Services
   - Linux: XOR 加密（回退方案）
   - Token 自动保存/加载

3. **环境检测** (`env_detector.cpp`)
   - Claude Desktop 配置检测
   - Cursor 配置检测
   - Continue.dev 配置检测
   - 系统环境变量检测
   - 跨平台路径支持

4. **配置管理** (`config_manager.cpp`)
   - 多环境配置存储
   - JSON 配置读写
   - 配置应用到目标应用
   - 环境切换

5. **用户界面**
   - 登录对话框（现代化设计）
   - 主窗口（环境状态展示）
   - 日志输出
   - 响应式布局

## 下一步开发任务

### 立即可做

1. **测试 MVP**
   ```bash
   # 需要在有 GUI 的环境中测试
   # 当前在 Linux 服务器环境，无法运行 GUI
   ```

2. **添加 API Key 管理界面**
   - 创建 `api_keys_dialog.h/cpp`
   - 展示用户的所有 API Keys
   - 支持切换激活的 Key
   - 复制 Key 到剪贴板

3. **完善配置写入功能**
   - 添加配置前的备份
   - 支持一键应用配置
   - 配置回滚功能

### 中期任务

1. **环境管理界面**
   - 创建 `env_manager_dialog.h/cpp`
   - 添加/编辑/删除环境
   - 环境模板功能
   - 批量配置多个应用

2. **自动更新**
   - 版本检查
   - 下载更新包
   - 自动安装（需要权限）

3. **设置界面**
   - 主题选择
   - 代理设置
   - 日志级别
   - 启动选项

### 长期任务

1. **防护加固**
   - 集成 VMProtect
   - 字符串加密
   - 证书锁定增强
   - 反调试机制

2. **高级功能**
   - 使用统计
   - 配额监控
   - 通知系统
   - 插件系统

## 已验证的 API 信息

### 登录接口
```http
POST https://www.aegisy.cc/api/v1/auth/login
Content-Type: application/json

{
  "email": "user@example.com",
  "password": "password123"
}

成功响应：
{
  "code": 0,
  "message": "success",
  "data": {
    "token": "eyJhbGc...",
    "user": { ... }
  }
}

失败响应：
{
  "code": 401,
  "message": "invalid email or password",
  "reason": "INVALID_CREDENTIALS"
}
```

### 其他端点
- `/api/v1/keys` - 需要认证 (401)
- `/api/v1/usage` - 需要认证 (401)
- `/health` - 公开端点，返回 `{"status":"ok"}`

## 构建与测试

### 本地开发环境搭建

**Windows:**
```cmd
# 1. 安装 Visual Studio 2022
# 2. 安装 Qt 6.5+ (https://www.qt.io/download)
# 3. 克隆代码

git clone <repo>
cd aegisy-app
build.bat
```

**macOS:**
```bash
# 1. 安装 Xcode Command Line Tools
xcode-select --install

# 2. 安装 Homebrew
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 3. 安装依赖
brew install cmake qt@6

# 4. 构建
cd aegisy-app
chmod +x build.sh
./build.sh

# 5. 运行
open build/AegisyClient.app
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt update
sudo apt install -y build-essential cmake qt6-base-dev libqt6network6 libssl-dev

cd aegisy-app
chmod +x build.sh
./build.sh

./build/AegisyClient
```

## 已知问题

1. **macOS 编译问题**
   - 需要修正 `#ifdef Q_OS_MACOS` 为 `#ifdef Q_OS_MAC`
   - Keychain API 需要链接 Security framework

2. **Linux XOR 加密**
   - 当前是简单的 XOR，安全性不足
   - 建议后续集成 libsecret 或 KWallet

3. **配置写入权限**
   - 某些系统可能需要管理员权限
   - 需要添加权限检查和提示

4. **GUI 在服务器环境**
   - 当前代码在 Linux 服务器上无法测试 GUI
   - 需要在桌面环境中测试

## 调试技巧

### 启用详细日志
```cpp
// 在 main.cpp 中添加
qSetMessagePattern("[%{type}] %{file}:%{line} - %{message}");
```

### 测试 API 连接
```bash
# 测试登录
curl -X POST https://www.aegisy.cc/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"email":"test@test.com","password":"test123"}'
```

### 查看存储的 Token
```bash
# Windows (注册表)
reg query "HKCU\Software\Aegisy\AegisyClient"

# macOS (Keychain)
security find-generic-password -s "AegisyClient" -a "auth_token"

# Linux (配置文件)
cat ~/.config/Aegisy/AegisyClient.conf
```

## 性能优化建议

1. **异步网络请求**
   - 使用 `QtConcurrent` 避免阻塞 UI
   - 添加加载动画

2. **配置缓存**
   - 缓存环境检测结果
   - 避免频繁读取文件

3. **延迟加载**
   - 只在需要时加载配置
   - 减少启动时间

## 打包发布

### Windows (NSIS Installer)
```bash
# 使用 Qt Installer Framework 或 NSIS
# 需要包含 Qt DLLs 和 OpenSSL DLLs
```

### macOS (DMG)
```bash
# 使用 macdeployqt
macdeployqt build/AegisyClient.app -dmg
```

### Linux (AppImage)
```bash
# 使用 linuxdeployqt
linuxdeployqt AegisyClient -appimage
```

## 代码风格

- 使用 Qt 命名约定
- 成员变量使用 `m_` 前缀
- 信号/槽使用 `on` 前缀
- 使用 Qt 容器类（`QString`, `QList` 等）
- 4 空格缩进

## 贡献指南

1. Fork 项目
2. 创建功能分支 (`git checkout -b feature/amazing-feature`)
3. 提交更改 (`git commit -m 'Add amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 创建 Pull Request

## 参考资源

- [Qt Documentation](https://doc.qt.io/)
- [sub2api GitHub](https://github.com/weiShaw/sub2api)
- [OpenSSL Documentation](https://www.openssl.org/docs/)
- [VMProtect Manual](https://vmpsoft.com/support/user-manual/)

## 常见问题

**Q: 为什么选择 Qt 而不是 Electron？**
A: Qt 提供更好的性能、更小的体积和更强的防护能力。

**Q: 支持哪些 Qt 版本？**
A: Qt 5.15+ 和 Qt 6.x，推荐使用 Qt 6.5+。

**Q: 如何贡献代码？**
A: 请查看上面的贡献指南。

**Q: 遇到编译错误怎么办？**
A: 查看 README 的故障排除章节，或提交 Issue。
