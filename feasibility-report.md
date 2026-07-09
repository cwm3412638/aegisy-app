# Aegisy 桌面客户端可行性调研报告

## 一、项目背景

基于现有的 aegisy-stack（sub2api 网关系统），开发 Windows/macOS 桌面客户端，提供：
- 用户登录认证
- 本地 Codex/Claude 环境检测与配置
- 多环境管理与切换
- API Key 管理与切换
- 防逆向保护

**技术偏好**：C++

---

## 二、后端架构分析

### 2.1 现有系统架构

```
客户端 → nginx(OpenResty, TLS) → sub2api:8080 → postgres/redis
```

**sub2api 核心组件**：
- **数据库**：PostgreSQL（用户、订阅、API密钥）
- **缓存**：Redis（会话、限流）
- **认证**：JWT（配置在 config.yaml 中的 jwt.secret）
- **API端口**：8080（内部），通过 nginx 443 对外

### 2.2 API 端点推测

根据配置文件和 nginx 反向代理配置，sub2api 应该提供标准的 REST API：

**认证相关**：
- `POST /api/v1/auth/login` - 用户登录（用户名/密码）
- `POST /api/v1/auth/register` - 用户注册
- `GET /api/v1/auth/user` - 获取当前用户信息

**API Key 管理**：
- `GET /api/v1/keys` - 获取用户的 API Keys 列表
- `POST /api/v1/keys` - 创建新的 API Key
- `DELETE /api/v1/keys/{id}` - 删除 API Key
- `PUT /api/v1/keys/{id}` - 更新 Key 状态（激活/停用）

**订阅/账户**：
- `GET /api/v1/account` - 获取账户余额、配额
- `GET /api/v1/usage` - 获取使用统计

---

## 三、客户端功能可行性分析

### 3.1 用户登录认证 ✅ **完全可行**

**实现方案**：
1. 通过 HTTPS 调用 `POST https://www.aegisy.cc/api/v1/auth/login`
2. 传递 `username` 和 `password`
3. 获取 JWT token（有效期 24小时，配置在 `JWT_EXPIRE_HOUR`）
4. 本地加密存储 token 用于后续 API 调用

**安全考虑**：
- 使用系统密钥链存储凭证（Windows: DPAPI, macOS: Keychain）
- JWT token 加密存储，避免明文
- 支持自动刷新 token 机制

### 3.2 Codex/Claude 环境检测 ✅ **完全可行**

**检测内容**：

#### 3.2.1 环境变量检测
```cpp
// 检测项：
- OPENAI_API_KEY
- OPENAI_BASE_URL
- ANTHROPIC_API_KEY  
- ANTHROPIC_BASE_URL
```

#### 3.2.2 配置文件检测
**Claude Desktop 配置**：
- Windows: `%APPDATA%\Claude\claude_desktop_config.json`
- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`

检测内容示例：
```json
{
  "anthropic": {
    "api_key": "sk-xxx",
    "base_url": "https://www.aegisy.cc/v1"
  }
}
```

**Cursor 配置**：
- Windows: `%APPDATA%\Cursor\User\settings.json`
- macOS: `~/Library/Application Support/Cursor/User/settings.json`

检测内容：
```json
{
  "cursor.general.apiKey": "sk-xxx",
  "cursor.general.baseUrl": "https://www.aegisy.cc/v1"
}
```

**Continue.dev 配置**：
- Windows: `%USERPROFILE%\.continue\config.json`
- macOS: `~/.continue/config.json`

### 3.3 环境配置功能 ✅ **完全可行**

**多环境管理方案**：

#### 3.3.1 配置存储结构
```json
{
  "environments": [
    {
      "id": "env-1",
      "name": "生产环境",
      "active": true,
      "api_keys": ["sk-prod-1", "sk-prod-2"],
      "active_key": "sk-prod-1",
      "base_url": "https://www.aegisy.cc/v1",
      "targets": ["claude", "cursor", "continue"]
    },
    {
      "id": "env-2", 
      "name": "测试环境",
      "active": false,
      "api_keys": ["sk-test-1"],
      "active_key": "sk-test-1",
      "base_url": "https://test.aegisy.cc/v1",
      "targets": ["claude"]
    }
  ]
}
```

#### 3.3.2 自动配置功能
客户端可以自动修改以下文件：

1. **系统环境变量**（需要管理员权限）
   - Windows: 通过注册表 `HKEY_CURRENT_USER\Environment`
   - macOS: 修改 `~/.zshrc` 或 `~/.bash_profile`

2. **应用配置文件**
   - 备份原配置
   - 根据激活的环境写入新配置
   - 支持一键回滚

3. **实时切换机制**
   ```cpp
   // 切换环境流程
   1. 保存当前环境配置快照
   2. 备份目标应用配置文件
   3. 写入新环境的 API Key 和 Base URL
   4. 通知用户重启相关应用（Claude/Cursor）
   ```

### 3.4 API Key 管理 ✅ **完全可行**

**功能设计**：

1. **从服务器获取 Key 列表**
   ```http
   GET https://www.aegisy.cc/api/v1/keys
   Authorization: Bearer {jwt_token}
   ```

2. **本地展示与管理**
   - 显示 Key 列表（名称、状态、配额、创建时间）
   - 标记激活的 Key
   - 一键切换激活 Key

3. **切换激活 Key**
   - 更新本地环境配置中的 `active_key`
   - 自动写入到已配置的应用（Claude/Cursor/Continue）
   - 无需调用服务器 API（服务器端不维护"激活状态"）

---

## 四、防逆向保护方案

### 4.1 威胁分析

**敏感信息**：
- 用户的 JWT token
- API Keys
- 服务器端点配置
- 客户端逻辑（防止仿冒）

**常见逆向手段**：
- 静态分析（IDA Pro, Ghidra, x64dbg）
- 动态调试（OllyDbg, WinDbg）
- 内存 dump（Cheat Engine）
- 网络抓包（Wireshark, Fiddler）
- 字符串提取（strings 工具）

### 4.2 C++ 防护技术栈 ⚠️ **可行但需多层防御**

#### 4.2.1 代码混淆与保护

**1. 商业级保护方案**（推荐）

| 方案 | 平台 | 特点 | 成本 |
|------|------|------|------|
| **VMProtect** | Win/Mac | 虚拟化保护、代码变形、反调试 | $200-$500 |
| **Themida** | Windows | 强度最高、虚拟机保护 | $300-$600 |
| **Code Virtualizer** | Windows | 代码虚拟化、轻量级 | $150-$400 |
| **Enigma Protector** | Windows | 全面保护、许可系统 | $180-$450 |

**2. 开源/编译器级混淆**

```cpp
// LLVM Obfuscator-LLVM (o-llvm)
// 编译时混淆：控制流平坦化、指令替换、虚假控制流
-mllvm -fla          // 控制流平坦化
-mllvm -sub          // 指令替换
-mllvm -bcf          // 虚假控制流
```

#### 4.2.2 字符串加密

**问题**：硬编码的 API 端点、密钥会被 `strings` 工具直接提取

**解决方案**：
```cpp
// 编译时 XOR 加密字符串
constexpr char enc_api_url[] = "\x3a\x7f\x7f\x71\x68..."; // 加密的 URL
constexpr uint8_t xor_key = 0x42;

std::string decrypt_string(const char* enc, size_t len) {
    std::string result;
    for (size_t i = 0; i < len; ++i) {
        result += enc[i] ^ xor_key ^ (i & 0xFF);
    }
    return result;
}
```

**推荐库**：
- **ADVobfuscator**：编译时字符串混淆
- **obfuscate.h**：单头文件，C++14 constexpr

#### 4.2.3 敏感数据保护

**JWT Token 和 API Key 存储**：

**Windows**：
```cpp
#include <windows.h>
#include <wincrypt.h>

// 使用 DPAPI (Data Protection API)
BOOL EncryptData(const BYTE* plaintext, DWORD len, 
                 BYTE** ciphertext, DWORD* cipher_len) {
    DATA_BLOB input = {len, (BYTE*)plaintext};
    DATA_BLOB output;
    
    if (CryptProtectData(&input, L"AegisyClient", NULL,
                         NULL, NULL, 0, &output)) {
        *ciphertext = output.pbData;
        *cipher_len = output.cbData;
        return TRUE;
    }
    return FALSE;
}
```

**macOS**：
```cpp
#include <Security/Security.h>

// 使用 Keychain Services
OSStatus StoreToken(const std::string& service,
                    const std::string& account,
                    const std::string& token) {
    return SecKeychainAddGenericPassword(
        NULL,
        service.length(), service.c_str(),
        account.length(), account.c_str(),
        token.length(), token.c_str(),
        NULL
    );
}
```

#### 4.2.4 反调试与反注入

```cpp
// 1. 反调试检测
#ifdef _WIN32
bool IsDebuggerPresent() {
    return ::IsDebuggerPresent();
}

bool CheckRemoteDebugger() {
    BOOL isDebuggerPresent = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &isDebuggerPresent);
    return isDebuggerPresent;
}

// 2. 时间检测（调试器会减慢执行）
bool DetectTimingAttack() {
    DWORD start = GetTickCount();
    // 简单操作
    Sleep(1);
    DWORD end = GetTickCount();
    return (end - start) > 100; // 阈值
}
#endif

// 3. 完整性检查（防篡改）
bool VerifyCodeIntegrity() {
    // 计算自身 PE 文件的哈希值
    // 与编译时嵌入的哈希比对
}
```

#### 4.2.5 网络通信安全

**问题**：Fiddler/Burp Suite 可以抓包获取 API 请求

**防护措施**：

1. **证书锁定（Certificate Pinning）**
```cpp
// 使用 libcurl 示例
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, 
    "sha256//公钥哈希值"); // 锁定 aegisy.cc 的证书
```

2. **请求签名**
```cpp
// 每个请求附加签名，防止重放攻击
std::string sign_request(const std::string& payload) {
    std::string timestamp = get_timestamp();
    std::string nonce = generate_nonce();
    std::string data = payload + timestamp + nonce + device_id;
    return hmac_sha256(data, client_secret);
}
```

3. **加密 Payload**（可选）
```cpp
// 在 HTTPS 基础上，对敏感字段再次加密
// 使用 AES-256-GCM
```

### 4.3 防护等级建议

根据不同安全需求，提供三个等级：

| 等级 | 防护措施 | 抵御能力 | 成本 |
|------|---------|---------|------|
| **基础** | DPAPI/Keychain + HTTPS + 基础反调试 | 抵御脚本小子、字符串提取 | 开发时间 |
| **标准** | + VMProtect/Themida + 字符串加密 + 证书锁定 | 抵御一般逆向工程师 | ~$500 |
| **高级** | + 代码虚拟化 + 请求签名 + 服务端校验 + 定期更新 | 抵御专业逆向团队（延缓数周-数月） | ~$1000 + 持续维护 |

**现实评估**：
- ⚠️ **没有绝对安全**：所有客户端保护都能被破解，只是时间问题
- ✅ **提高破解成本**：让破解者付出不成比例的精力
- ✅ **分层防御**：多层保护提高破解难度
- ✅ **服务端验证**：关键逻辑放在服务端（如 token 验证、配额检查）

---

## 五、C++ 技术栈推荐

### 5.1 跨平台 GUI 框架

| 框架 | 优点 | 缺点 | 适用场景 |
|------|------|------|---------|
| **Qt (推荐)** | 成熟稳定、原生外观、丰富组件、良好文档 | 许可证成本（商业需购买）、体积较大 | 功能完整的桌面应用 |
| **Dear ImGui** | 轻量、灵活、无许可证问题 | 需要手动集成渲染、外观不够现代 | 工具类应用、调试工具 |
| **wxWidgets** | 开源免费、原生控件 | API 老旧、文档欠佳 | 预算有限的项目 |
| **Electron (非C++)** | 易开发、跨平台 | 体积大（>100MB）、资源占用高 | 不适合需要防护的场景 |

**推荐选择**：**Qt 6.x**
- 商业许可：~$5000/年/开发者（如需商用）
- 开源许可：LGPLv3（需动态链接，可能被逆向但影响有限）

### 5.2 网络库

| 库 | 特点 | 难度 |
|------|------|------|
| **libcurl** | 支持 HTTPS、证书锁定、成熟稳定 | 中等 |
| **cpp-httplib** | 单头文件、易用、支持 HTTPS | 简单 |
| **Boost.Beast** | 现代 C++、异步支持 | 较难 |
| **Qt Network** | 与 Qt 集成、支持 SSL | 简单（使用 Qt 时） |

**推荐**：**libcurl**（单独使用）或 **Qt Network**（使用 Qt 框架时）

### 5.3 JSON 处理

| 库 | 特点 |
|------|------|
| **nlohmann/json** | 单头文件、现代 C++、易用 |
| **RapidJSON** | 高性能、低内存占用 |
| **Qt JSON** | Qt 内置，API 友好 |

**推荐**：**nlohmann/json**

### 5.4 加密库

| 库 | 特点 |
|------|------|
| **OpenSSL** | 功能全面、工业标准 |
| **Crypto++** | C++ 原生、易用 |
| **Botan** | 现代 C++、易于集成 |

**推荐**：**OpenSSL**（成熟度高）或 **Crypto++**（纯 C++）

### 5.5 完整技术栈示例

```
┌─────────────────────────────────────┐
│         GUI 层：Qt 6.x              │
├─────────────────────────────────────┤
│   业务逻辑：C++17/20                │
│   - 登录模块                        │
│   - 环境检测模块                    │
│   - 配置管理模块                    │
│   - API Key 管理模块                │
├─────────────────────────────────────┤
│   网络层：libcurl/Qt Network        │
│   JSON：nlohmann/json               │
│   加密：OpenSSL + DPAPI/Keychain    │
├─────────────────────────────────────┤
│   防护层：VMProtect/Themida         │
│   - 代码混淆                        │
│   - 反调试                          │
│   - 字符串加密                      │
└─────────────────────────────────────┘
```

---

## 六、开发工作量估算

### 6.1 功能模块拆解

| 模块 | 工作量（人天） | 复杂度 |
|------|---------------|--------|
| **GUI 框架搭建** | 3-5 | 中 |
| **登录模块** | 5-7 | 中 |
| **环境检测模块** | 8-12 | 高 |
| **环境配置模块** | 10-15 | 高 |
| **API Key 管理** | 5-8 | 中 |
| **多环境切换** | 8-10 | 高 |
| **数据加密存储** | 5-7 | 中 |
| **网络通信层** | 5-7 | 中 |
| **防逆向集成** | 7-10 | 高 |
| **Windows 适配** | 3-5 | 低 |
| **macOS 适配** | 3-5 | 低 |
| **测试与优化** | 10-15 | 中 |

**总计**：**72-106 人天**（约 3-5 个月，1 人开发）

### 6.2 成本估算

| 项目 | 成本 |
|------|------|
| **开发人力** | 3-5 个月（根据薪资计算） |
| **Qt 商业许可**（如需） | $5,000/年/开发者 |
| **VMProtect** | $200-$500 一次性 |
| **代码签名证书** | $300-$500/年（Windows + macOS） |
| **测试设备** | Windows VM + macOS 实体机 |

---

## 七、风险与挑战

### 7.1 技术风险

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| **sub2api API 未公开** | 需要逆向分析 | 通过网络抓包分析前端请求 |
| **API 变更** | 客户端失效 | 实现版本检测和自动更新 |
| **配置文件格式变化** | 写入失败 | 多版本兼容 + 错误恢复 |
| **系统权限不足** | 无法修改环境变量 | 提示用户授权或手动操作 |
| **防护被破解** | 敏感信息泄露 | 服务端二次验证 + 定期更新 |

### 7.2 法律与合规风险

⚠️ **重要提示**：
1. **API 滥用防护**：需要服务端限流和异常检测
2. **用户协议**：明确客户端使用条款
3. **开源许可证**：如使用 Qt LGPLv3，需遵守相关条款
4. **逆向工程**：某些应用（如 Cursor）可能禁止自动化配置

---

## 八、替代方案

### 8.1 Electron + 混淆

**优点**：
- 开发速度快（TypeScript/JavaScript）
- 跨平台容易
- 生态丰富（npm 包）

**缺点**：
- 体积大（~150MB）
- 防护较弱（JavaScript 易被反混淆）
- 资源占用高

**适用场景**：快速 MVP，防护要求不高

### 8.2 Rust

**优点**：
- 内存安全
- 性能接近 C++
- 静态链接，逆向难度中等

**缺点**：
- GUI 生态不如 C++/Qt 成熟
- 学习曲线陡峭
- 开发周期较长

**适用场景**：长期项目，重视安全性

### 8.3 Web 面板 + 浏览器插件

**方案**：
- 服务端提供 Web 控制面板
- 浏览器插件负责本地环境检测和配置

**优点**：
- 无需安装桌面应用
- 跨平台天然支持
- 更新方便

**缺点**：
- 浏览器插件权限有限
- 无法访问系统环境变量
- 防护能力极弱

---

## 九、结论与建议

### 9.1 可行性结论

✅ **功能层面：完全可行**
- 所有需求功能都有成熟的技术方案
- C++ 生态支持良好

⚠️ **防护层面：可行但有限制**
- 可以提高逆向门槛，但无法完全防止
- 需要服务端配合验证
- 需要持续更新对抗

### 9.2 推荐方案

**技术栈**：
```
- 语言：C++17/20
- GUI：Qt 6.x（商业许可）或 wxWidgets（开源）
- 网络：libcurl + 证书锁定
- JSON：nlohmann/json
- 加密：OpenSSL + DPAPI/Keychain
- 防护：VMProtect（Windows）+ 编译时混淆
- 构建：CMake
```

**防护策略**（推荐"标准"级别）：
1. 敏感数据使用系统密钥链
2. 字符串编译时加密
3. VMProtect 保护核心模块
4. HTTPS + 证书锁定
5. 服务端二次验证（device_id + JWT）

### 9.3 开发路线图

**第一阶段（4-6 周）**：核心功能
- [ ] GUI 框架搭建
- [ ] 登录模块
- [ ] 环境检测（Claude/Cursor/Continue）
- [ ] 基础配置功能

**第二阶段（3-4 周）**：高级功能
- [ ] 多环境管理
- [ ] API Key 管理与切换
- [ ] 自动配置写入

**第三阶段（2-3 周）**：防护与优化
- [ ] 集成 VMProtect/Themida
- [ ] 字符串加密
- [ ] 证书锁定
- [ ] 反调试机制

**第四阶段（2-3 周）**：测试与发布
- [ ] 多系统测试
- [ ] 代码签名
- [ ] 自动更新机制
- [ ] 文档编写

### 9.4 关键建议

1. **先实现 MVP**：
   - 第一版只做登录 + 环境检测 + 单 Key 配置
   - 验证用户需求和 API 可用性
   - 延迟防护功能到后期

2. **服务端配合**：
   - 实现设备指纹绑定
   - 异常登录检测
   - API 调用频率限制

3. **法律合规**：
   - 明确用户协议
   - 遵守第三方应用（Cursor 等）的 ToS

4. **持续更新**：
   - 防护是持续对抗的过程
   - 计划每 3-6 个月更新防护机制

5. **备选方案**：
   - 如果开发周期受限，考虑 Electron + 基础混淆
   - 如果防护要求极高，考虑 Rust + Tauri

---

## 十、附录：需要的额外信息

为了更精确地实施开发，还需要获取以下信息：

### 10.1 API 端点确认

需要通过抓包或后端文档确认：
- [ ] 登录接口的准确路径和参数格式
- [ ] API Key 列表接口
- [ ] 用户信息接口
- [ ] 是否有 refresh token 机制

**建议操作**：
```bash
# 在浏览器中登录 www.aegisy.cc
# 打开开发者工具 → Network 标签
# 记录所有 /api/ 请求的完整信息
```

### 10.2 预算确认

- 是否购买 Qt 商业许可？
- 是否购买 VMProtect/Themida？
- 是否需要代码签名证书？

### 10.3 优先级确认

功能优先级排序（影响开发顺序）：
1. 登录
2. 环境检测
3. 单环境配置
4. 多环境管理
5. API Key 切换
6. 防护加固

---

## 总结

**项目可行性：✅ 高度可行**

所有功能都有成熟的技术方案，C++ 是合适的选择。防护方面虽然无法做到绝对安全，但通过多层防御可以大幅提高破解成本。建议采用渐进式开发策略，先实现 MVP 验证需求，再逐步加固防护。

预计 **3-5 个月**可以完成一个功能完整、防护到位的桌面客户端。
