# 版本更新日志

## v2.5.0 - 本地网关、用量中心与终端工作台

### 新增功能

- 新建和编辑档案时支持连接测试，同时验证 API Key、模型可用性和响应耗时，并分类显示鉴权、余额、模型和渠道错误。
- CLI 本地版本检测后异步查询 npm 最新版本，在侧栏标记可更新状态。
- 激活档案前展示目标文件、计划字段、模型、环境冲突与备份策略，确认后才执行写入。
- 点击右上角余额进入用量中心，支持今日、7 天、30 天汇总、模型统计和每个 API Key 的今日与累计消费。
- 新增默认关闭的本地网关模式，仅监听 `127.0.0.1:43112`，支持 Claude、Codex、Gemini 独立路由和快速档案切换。
- 本地网关请求监控只记录时间、工具、模型、路径、状态码和耗时，不保存提示词、回复或工具参数。
- “启动”会自动探测系统原生终端并直接运行，Windows 优先使用 Windows Terminal，其次 PowerShell 7、Windows PowerShell 和 cmd。
- 启动 CLI 时清除继承的旧 Provider 环境变量，避免它们覆盖刚切换的档案 Key；已运行进程会明确提示需要重启。
- 系统体检比较本地版本与 npm 最新版本，仅在确有新版本时显示“更新”，最新版显示“已是最新”。
- 移除无法完整支持交互式 TUI 的内置文本终端入口，避免 Claude 输入无响应和 Codex `TERM=dumb`。
- 系统体检增加 OpenCode、OpenClaw、Hermes、VS Code、Claude Desktop 和 ChatGPT Desktop 检测。
- 本地网关令牌保存在系统安全存储，真实 Aegisy Key 只通过进程管道传入网关内存。
- 新增“桌面增强”中心，通过 Codex 官方 CLI 展示完整插件市场列表并安装插件。
- 支持一键安装 Computer Use，保留 Codex 原生授权、沙箱和操作确认。
- 支持同步 Codex JSONL 历史会话和 SQLite 桌面索引，写入前创建独立备份清单。
- 全量模型列表以 Aegisy API 返回结果为准，不套用 Codex Desktop 内置模型白名单。
- Windows 与 macOS 支持 Claude Desktop 运行时中文注入，不修改官方安装文件，关闭 Claude 后自动失效。
- Windows 打包新增 Qt WebSockets 与 SQLite 驱动完整性检查，缺少运行库时停止生成安装包。
- 顶栏账号状态升级为可点击头像，账号中心支持修改密码和密卡兑换，兑换后立即刷新余额。
- API Keys 页面新增创建、编辑、切换分组、启用/禁用和删除，接口与 Aegisy Web 端保持一致。
- Codex 插件列表新增中文功能说明、官方描述详情和说明文字搜索。
- 插件安装支持复选框多选、全选可安装项、顺序批量安装及逐项结果汇总。

## v2.4.0 - GPT Image 生图

### 新增功能

- 主窗口新增“生图”入口。
- 自动读取账号中的生图分组，并仅展示 `gpt-image` 分组下的可用 API Key。
- 支持选择 GPT Image 模型、尺寸、质量和 PNG、JPEG、WebP 输出格式。
- 支持客户端内图片预览和另存，生成请求最长等待 20 分钟。
- 关闭生图窗口时自动取消未完成的请求，API Key 不写入日志或普通配置文件。

## v1.1.0 (开发中) - 完整环境管理

### 新增功能 ✨

#### 🔑 API Key 管理界面
- **位置**: 主窗口 → "Manage API Keys" 按钮
- **功能**:
  - ✅ 查看所有 API Keys 列表
  - ✅ 显示 Key 详细信息（名称、状态、配额、使用量）
  - ✅ 复制 Key 到剪贴板
  - ✅ 切换激活的 Key（标记为 ★ 活跃）
  - ✅ 实时刷新 Keys
  - ✅ 使用率百分比显示（带颜色警告）
  - ✅ 现代化 UI 设计

#### 🔧 环境配置自动写入
- **位置**: 主窗口 → "Configure Environment" 按钮
- **功能**:
  - ✅ 一键应用配置到多个应用
  - ✅ 自动备份现有配置
  - ✅ 选择性配置目标应用（Claude/Cursor/Continue）
  - ✅ 实时进度显示
  - ✅ 详细日志输出
  - ✅ 配置验证和错误处理
  - ✅ 配置恢复功能（NEW！）
  - ✅ 备份历史选择

#### 🎯 多环境管理（NEW！）
- **位置**: 主窗口 → "Manage Environments" 按钮
- **功能**:
  - ✅ 创建多个环境配置（生产/测试/开发等）
  - ✅ 编辑环境配置
  - ✅ 删除环境
  - ✅ 一键切换环境
  - ✅ 查看环境详情
  - ✅ 环境激活状态标记（★）
  - ✅ 双击快速编辑
  - ✅ 活跃环境保护（防止误删）

#### 配置界面预览
```
┌────────────────────────────────────────────────────────┐
│ Configure AI Applications                              │
├────────────────────────────────────────────────────────┤
│ Configuration:                                         │
│   API Key:   [sk-your-api-key________________]        │
│   Base URL:  [https://www.aegisy.cc/v1_______]        │
│                                                        │
│ Target Applications:                                   │
│   ☑ Claude Desktop                                    │
│   ☑ Cursor Editor                                     │
│   ☑ Continue.dev                                      │
│                                                        │
│ [Progress Bar ████████████░░░░░░░░░░░] 60%          │
│ Status: Configuring Cursor...                         │
│                                                        │
│ Log:                                                   │
│ ✓ Backup created successfully                         │
│ ✓ Claude Desktop configured                           │
│ 🔧 Configuring Cursor...                             │
└────────────────────────────────────────────────────────┘
```

### 技术实现 🔧

#### API Key 管理
**新增文件**:
- `include/api_keys_dialog.h`
- `src/api_keys_dialog.cpp`

**功能**:
- Qt Table Widget 展示 Keys
- QClipboard 复制功能
- 信号槽处理 Key 切换
- 现代化 UI 设计

#### 环境配置
**新增文件**:
- `include/env_config_dialog.h`
- `src/env_config_dialog.cpp`

**功能**:
- 自动备份到 `AppData/backups/`
- 进度条显示配置过程
- 详细日志输出
- 错误处理和验证

#### 环境管理（NEW！）
**新增文件**:
- `include/env_manager_dialog.h`
- `src/env_manager_dialog.cpp`

**功能**:
- 多环境配置存储（JSON）
- 环境列表展示（QListWidget）
- 环境编辑对话框
- 一键切换环境
- 活跃环境标记
- UUID 生成环境ID

**修改文件**:
- `CMakeLists.txt` - 添加新源文件
- `include/main_window.h` - 添加按钮
- `src/main_window.cpp` - 实现对话框调用
- `CHANGELOG.md` - 更新日志

### 使用说明 📖

#### API Keys 管理

1. **打开 API Keys 管理**
   - 登录后，点击主窗口的 "Manage API Keys" 按钮

2. **查看 Keys 列表**
   - 表格显示所有 API Keys 及其详细信息
   - 当前激活的 Key 会显示 ★ 标记

3. **复制 Key**
   - 选择一个 Key
   - 点击 "📋 Copy Key" 按钮
   - Key 会被复制到剪贴板

4. **切换激活 Key**
   - 选择要激活的 Key
   - 点击 "✓ Set as Active" 按钮
   - 该 Key 会被标记为活跃状态

5. **刷新列表**
   - 点击 "🔄 Refresh" 重新从服务器加载

#### 环境配置（NEW！）

1. **打开配置对话框**
   - 点击主窗口的 "Configure Environment" 按钮

2. **输入配置信息**
   - 输入或粘贴你的 API Key
   - 确认 Base URL（默认：https://www.aegisy.cc/v1）

3. **选择目标应用**
   - 勾选要配置的应用（Claude/Cursor/Continue）
   - 可以全选或只选某些应用

4. **应用配置**
   - 点击 "✓ Apply Configuration" 按钮
   - 确认对话框后，自动开始配置
   - 查看进度条和日志输出

5. **手动备份**
   - 点击 "📦 Backup Current" 创建手动备份
   - 备份保存在 `AppData/Aegisy/AegisyClient/backups/`

6. **恢复配置（NEW！）**
   - 点击 "♻️ Restore Backup" 按钮
   - 从备份历史列表中选择要恢复的备份
   - 确认后自动恢复配置文件
   - 支持恢复 Claude/Cursor/Continue 所有配置

**重要提示**:
- ⚠️ 配置后需要重启相关应用才能生效
- ✅ 每次应用配置前会自动创建备份
- 📁 备份位置会在日志中显示

#### 环境管理（NEW！）

1. **打开环境管理器**
   - 点击主窗口的 "Manage Environments" 按钮

2. **创建新环境**
   - 点击 "➕ Add" 按钮
   - 输入环境名称（如：Production, Development, Testing）
   - 输入 API Key 和 Base URL
   - 选择目标应用
   - 点击 "Create" 保存

3. **编辑环境**
   - 选择要编辑的环境
   - 点击 "✏️ Edit" 按钮或双击环境名称
   - 修改配置信息
   - 点击 "Save" 保存更改

4. **切换环境**
   - 选择要激活的环境
   - 查看右侧的环境详情
   - 点击 "✓ Activate Environment" 按钮
   - 确认后自动应用配置

5. **删除环境**
   - 选择要删除的环境（不能删除当前激活的环境）
   - 点击 "🗑️ Delete" 按钮
   - 确认删除操作

**使用场景**:
- 🏢 **生产环境**：配置生产 API Key，应用到所有工具
- 🧪 **测试环境**：使用测试 API Key 进行开发测试
- 💻 **开发环境**：使用开发 API Key，可能只配置部分工具
- 🔄 **快速切换**：在不同环境间一键切换，无需手动修改配置


### 注意事项 ⚠️

- **服务器端支持**: 需要 `/api/v1/keys` 端点返回 Keys 列表
- **JSON 格式**: 期望的响应格式：
  ```json
  {
    "code": 0,
    "data": [
      {
        "id": "key_123",
        "name": "Production Key",
        "key": "sk-prod-xxx",
        "status": "active",
        "quota": 10000,
        "used": 5000,
        "created_at": "2024-01-01T00:00:00Z",
        "expires_at": "",
        "is_active": true
      }
    ]
  }
  ```

### 已知限制 📝

- ⏳ 切换 Key 后不会自动应用到环境配置（需 v1.2）
- ⏳ 不支持创建/删除 Keys（需服务端支持）
- ⏳ 不显示 Key 的详细使用历史

---

## v1.0.0 (2024) - MVP 发布

### 核心功能
- ✅ 用户登录认证
- ✅ 安全凭证存储（DPAPI/Keychain）
- ✅ 环境自动检测（Claude/Cursor/Continue）
- ✅ 配置管理基础架构
- ✅ 跨平台支持（Windows/macOS/Linux）

### 详细信息
参见 `PROJECT-SUMMARY.md`

---

## 即将推出 🚀

### v1.2 计划
- [ ] 一键配置写入功能
- [ ] 环境切换界面
- [ ] 配置备份/恢复
- [ ] Key 切换后自动应用到配置

### v2.0 计划
- [ ] 防护加固（VMProtect）
- [ ] 自动更新
- [ ] 使用统计图表
- [ ] 暗色主题

---

**最后更新**: 2024  
**当前版本**: v1.1.0-dev
