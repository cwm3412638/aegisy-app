# Aegisy 桌面客户端 - 技术方案速查表

## 一、核心技术选型

### 推荐方案 A（成熟稳定）
```
语言：       C++17/20
GUI：        Qt 6.x (商业许可 $5000/年 或 LGPLv3)
网络：       Qt Network 或 libcurl
JSON：       nlohmann/json
加密：       OpenSSL + DPAPI(Win) / Keychain(macOS)
防护：       VMProtect ($200-500)
构建：       CMake + vcpkg/conan
```

### 替代方案 B（开源免费）
```
语言：       C++17/20
GUI：        wxWidgets (开源)
网络：       libcurl
JSON：       nlohmann/json
加密：       Crypto++
防护：       LLVM Obfuscator + 手动反调试
构建：       CMake
```

### 替代方案 C（快速开发）
```
语言：       TypeScript
GUI：        Electron
网络：       axios
加密：       node-crypto
防护：       webpack-obfuscator + asar 加密
构建：       electron-builder
```

## 二、关键文件路径

### Claude Desktop
```
Windows:  %APPDATA%\Claude\claude_desktop_config.json
macOS:    ~/Library/Application Support/Claude/claude_desktop_config.json

配置格式：
{
  "anthropic": {
    "api_key": "sk-xxx",
    "base_url": "https://www.aegisy.cc/v1"
  }
}
```

### Cursor
```
Windows:  %APPDATA%\Cursor\User\settings.json
macOS:    ~/Library/Application Support/Cursor/User/settings.json

配置字段：
"cursor.general.apiKey": "sk-xxx"
"cursor.general.baseUrl": "https://www.aegisy.cc/v1"
```

### Continue.dev
```
Windows:  %USERPROFILE%\.continue\config.json
macOS:    ~/.continue/config.json

配置格式：
{
  "models": [{
    "provider": "openai",
    "apiKey": "sk-xxx",
    "apiBase": "https://www.aegisy.cc/v1"
  }]
}
```

### 系统环境变量
```
Windows:  HKEY_CURRENT_USER\Environment (注册表)
macOS:    ~/.zshrc 或 ~/.bash_profile

变量名：
OPENAI_API_KEY
OPENAI_BASE_URL
ANTHROPIC_API_KEY
ANTHROPIC_BASE_URL
```

## 三、防护技术清单

### 基础防护（必须）
- [x] DPAPI/Keychain 存储敏感数据
- [x] HTTPS + TLS 1.2+
- [x] 证书锁定（Certificate Pinning）
- [x] 基础反调试（IsDebuggerPresent）
- [x] 配置文件权限限制（600）

### 标准防护（推荐）
- [x] VMProtect/Themida 代码保护
- [x] 编译时字符串加密（ADVobfuscator）
- [x] 控制流混淆（LLVM o-llvm）
- [x] 完整性校验（PE/Mach-O 哈希）
- [x] 设备指纹绑定

### 高级防护（可选）
- [x] 请求签名（HMAC-SHA256）
- [x] 代码虚拟化
- [x] 内存加密
- [x] 反注入检测
- [x] 定期更新（每 3-6 个月）

## 四、开发优先级

### MVP 阶段（4 周）
```
Week 1: 
  - [x] Qt 项目搭建
  - [x] 登录 UI
  - [x] 网络层封装（HTTP Client）

Week 2:
  - [x] 登录逻辑实现
  - [x] JWT Token 管理
  - [x] 数据加密存储（DPAPI/Keychain）

Week 3:
  - [x] 环境检测模块
  - [x] 配置文件读取（Claude/Cursor/Continue）
  - [x] 环境变量检测

Week 4:
  - [x] 基础配置功能
  - [x] 单环境写入
  - [x] 基础测试
```

### 功能完善（3 周）
```
Week 5-6:
  - [x] API Key 管理界面
  - [x] 多环境管理
  - [x] 环境切换功能

Week 7:
  - [x] 自动更新检测
  - [x] 日志系统
  - [x] 错误处理优化
```

### 防护加固（2 周）
```
Week 8:
  - [x] VMProtect 集成
  - [x] 字符串加密
  - [x] 反调试机制

Week 9:
  - [x] 证书锁定
  - [x] 代码签名
  - [x] 安全测试
```

## 五、API 端点（待确认）

### 认证
```http
POST /api/v1/auth/login
Content-Type: application/json

{
  "username": "user@example.com",
  "password": "password123"
}

Response:
{
  "token": "eyJhbGc...",
  "expires_at": 1234567890
}
```

### 获取 API Keys
```http
GET /api/v1/keys
Authorization: Bearer eyJhbGc...

Response:
{
  "keys": [
    {
      "id": "key_123",
      "name": "Production Key",
      "key": "sk-prod-xxx",
      "status": "active",
      "quota": 1000000,
      "used": 50000,
      "created_at": "2024-01-01T00:00:00Z"
    }
  ]
}
```

### 用户信息
```http
GET /api/v1/user
Authorization: Bearer eyJhbGc...

Response:
{
  "id": "user_123",
  "username": "user@example.com",
  "balance": 100.00,
  "subscription": "pro"
}
```

## 六、依赖库版本

### Qt 方案
```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(AegisyClient VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)
find_package(OpenSSL REQUIRED)

# vcpkg dependencies
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cryptopp CONFIG REQUIRED)
```

### 依赖列表
```json
{
  "dependencies": {
    "qt6": "6.5+",
    "openssl": "3.0+",
    "nlohmann-json": "3.11+",
    "libcurl": "7.88+" // 如果不用 Qt Network
  }
}
```

## 七、构建脚本模板

### Windows (MSVC)
```batch
@echo off
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### macOS (Clang)
```bash
#!/bin/bash
mkdir -p build && cd build
cmake .. -G "Xcode" \
    -DCMAKE_BUILD_TYPE=Release \
    -DQt6_DIR=/opt/homebrew/opt/qt@6/lib/cmake/Qt6
cmake --build . --config Release
```

## 八、待办事项

### 在开始开发前
- [ ] 运行 `api-probe.py` 确认 API 端点
- [ ] 决定 Qt 商业许可 vs 开源许可
- [ ] 购买代码签名证书
- [ ] 准备测试账号和 API Keys

### 安全加固前
- [ ] 购买 VMProtect 许可证
- [ ] 配置 SSL 证书锁定参数
- [ ] 实现服务端设备指纹验证

### 发布前
- [ ] 代码签名（Windows: Authenticode, macOS: notarization）
- [ ] 安全审计
- [ ] 准备用户协议和隐私政策

## 九、成本预算

| 项目 | 一次性 | 年付 |
|------|--------|------|
| 开发人力（3-5月） | - | - |
| Qt 商业许可 | - | $5,000 |
| VMProtect | $300 | - |
| 代码签名证书（Win+Mac） | $400 | $400 |
| **总计（首年）** | **$700** | **$5,400** |
| **总计（次年起）** | **-** | **$5,400** |

**开源方案成本**：仅代码签名 $400/年

## 十、联系方式

- 报告位置：`/root/aegisy-app/feasibility-report.md`
- API 探测：`python3 /root/aegisy-app/api-probe.py`
