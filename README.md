# Aegisy Desktop Client

Aegisy Desktop Client 是一个跨平台 Qt 桌面应用，用于把 Aegisy 账号中的 API Key 安全接入 Claude Code、Codex CLI、Gemini CLI 和 OpenCode，并统一管理配置健康、桌面工具与账号能力。

## 当前功能

- 使用 Aegisy 账号登录，支持密码显隐、余额展示，并分页同步 API Keys
- 点击右上角头像进入账号中心，可修改登录密码或使用密卡兑换余额、并发额度和订阅权益
- API Keys 管理与 Web 端保持一致，支持创建、编辑、切换分组、启用、禁用和删除
- 每个档案只绑定一个终端：Claude Code、Codex CLI、Gemini CLI 或 OpenCode
- 按终端筛选、编辑、删除和激活档案；四种终端分别保留一个当前配置
- 监控当前认证文件；文件被删除、损坏或被其他配置工具改写时显示“需修复”，启动前阻止错误配置继续运行
- 修复 Codex 根配置误入 `[tui.model_availability_nux]` 等 TOML 表导致的类型错误，并保留 CC Switch Provider、项目信任与其他功能设置
- 系统托盘快速切换档案，关闭主窗口后可继续运行
- 查询 API Key 可用模型并保存模型选择
- 新建或编辑档案时测试 Key、模型和连接延迟，并分类显示连接错误
- 使用 `gpt-image` 分组下的个人 API Key 生成 GPT 图片，支持模型、尺寸、质量、格式选择、预览和保存
- 内置类似 ChatGPT、Claude 的 AI 对话工作区，可切换 API Key 和该 Key 支持的模型，支持流式回复、上下文 Token、停止生成和持久历史
- 每条消息支持复制；用户消息支持编辑后重新发送，助手消息支持重新生成，也可按 Markdown 复制完整对话
- 用户与 Aegisy 助手消息使用独立头像，长回复和代码块滚动时保持身份位置稳定
- 新增 Skills 管理中心，支持从 HTTPS Skill 目录、`SKILL.md`、`INSTALL.md` 地址安装完整技能包，或导入本地目录
- Skill 安装会保留 `scripts/`、`references/`、`agents/` 等引用资源；第三方脚本默认不执行，内置执行器单独授权
- AI 对话可自动识别明确的生图和 PPT 请求，也可使用 `/image`、`/ppt` 强制调用对应 Skill
- 内置 GPT Image Skill 自动选择账号中的 `gpt-image` 分组 Key，并在对话中展示和保存图片
- 内置 PPT Skill使用当前会话模型生成大纲，再通过隔离的 `python-pptx` 环境生成可打开的 PPTX 文件
- 用量中心展示时间范围汇总、模型统计、Key 今日消费、累计消费、额度和使用率
- 检测 CLI 本地版本和 npm 最新版本，并提示可更新状态
- 激活前预览目标文件、字段变化、模型、冲突和备份策略
- 系统体检统一检查 Node.js、npm、Git、pnpm、Bun、AI CLI、额外开发工具、桌面客户端、Aegisy 配置和系统安全存储
- 缺少环境时通过 Homebrew、WinGet 或 Linux 系统包管理器一键安装 Node.js 和对应 CLI
- 支持确认后安装或更新三个 AI CLI，并从当前激活档案使用系统原生终端启动
- 启动终端时清除可能覆盖档案的旧 Provider 环境变量，确保新进程使用当前激活 Key
- 启动时选择并记住每个档案的工作目录，自动适配 macOS Terminal/iTerm、Windows Terminal/PowerShell/cmd 和常见 Linux 终端
- 可选本地网关模式支持快速切换档案和元数据级请求监控
- “桌面增强”展示 Codex 官方市场中的已安装和可安装插件，读取插件清单并提供中文功能说明
- Claude Desktop 与 ChatGPT Desktop 使用受认证的 Aegisy 服务端流式代理下载；客户端拒绝重定向、验证代理标识与安装包格式，失败后由用户选择重试或打开官网
- 插件支持复选框多选、全选可安装项和批量安装，逐项展示进度与成功/失败结果
- 支持一键安装 Codex Computer Use；电脑控制仍遵循 Codex 的授权、沙箱和确认策略
- 支持把 Codex JSONL 历史会话与 SQLite 桌面索引同步到当前 Provider，写入前自动建立可恢复备份
- 支持 Windows、macOS 以调试端口运行时注入 Claude Desktop 中文词典，不修改 Claude 安装文件
- 全量模型列表直接展示当前 Aegisy API Key 返回的模型，不使用桌面客户端内置白名单过滤
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
| OpenCode | `~/.config/opencode/config.json` |

配置写入采用读、合并、原子提交，不会直接覆盖无关字段。每次激活前都会建立同批次备份。

## Aegisy Agent Workbench 开发进度

仓库正在按 `openspec/changes/build-aegisy-agent-workbench/` 推进原生 AI 编程工作台。

### 当前进度：101/239 任务完成 (42%)

**已完成的核心功能** (100%):
- ✅ **Section 5**: Event Store, Database, and Recovery - 事件存储与恢复
- ✅ **Section 11**: Workbench Host and Navigation - 工作台界面与导航
- ✅ **Section 12**: Agent Timeline and Composer - 对话时间线与输入
- ✅ **Section 13**: Files, Editor, Search, and Diagnostics - 文件、编辑器、搜索与诊断
- ✅ **Section 15**: Structured Edits, Diffs, and Checkpoints - 结构化编辑、差异与检查点

**高完成度功能** (67-89%):
- 🔄 **Section 2** (89%): UI Technology Spike - Monaco 编辑器、终端集成
- 🔄 **Section 3** (75%): AAP Protocol Foundation - 协议基础
- 🔄 **Section 14** (67%): Terminal and Process Execution - 终端与进程执行

**完整文档** (70%):
- 📚 **Section 23** (70%): Documentation and Operational Readiness
  - 用户入门指南 ([FIRST-RUN-ONBOARDING.md](docs/FIRST-RUN-ONBOARDING.md))
  - 扩展开发指南 ([EXTENSION-AUTHOR-GUIDE.md](docs/EXTENSION-AUTHOR-GUIDE.md))
  - 安全文档 ([SECURITY-DOCUMENTATION.md](docs/SECURITY-DOCUMENTATION.md))
  - 模型切换指南 ([MODEL-SWITCHING-GUIDE.md](docs/MODEL-SWITCHING-GUIDE.md))
  - 架构文档 ([ARCHITECTURE.md](ARCHITECTURE.md))
  - AAP 协议指南 ([AAP-PROTOCOL-GUIDE.md](docs/AAP-PROTOCOL-GUIDE.md))
  - AAP API 参考 ([AAP-API-REFERENCE.md](docs/AAP-API-REFERENCE.md))
  - 故障排查手册 ([Aegisy-TROUBLESHOOTING-RUNBOOK.md](docs/Aegisy-TROUBLESHOOTING-RUNBOOK.md))

### 技术架构

**多进程架构**:
- **Desktop Host (Qt/C++)**: 用户界面与应用生命周期管理
- **Agent Runtime Sidecar (Rust)**: 业务逻辑、存储、安全策略
- **Runtime Adapters**: Codex App Server 和 ACP 协议适配

**核心特性**:
- 🔒 **安全优先**: 进程隔离、权限模型、秘密保护、沙箱边界
- 📝 **事件溯源**: 完整的操作历史与状态重放
- ✅ **审批工作流**: 所有变更需要明确批准
- 🔄 **Git 集成**: Git 感知的检查点与恢复
- 🎯 **多运行时**: 支持 Codex 和 ACP 协议

**当前状态**:
- ✅ 完整的 UI 框架（产品导航、三栏布局、命令面板）
- ✅ 项目与会话管理（持久化、恢复、归档）
- ✅ Monaco 编辑器集成（语法高亮、差异视图、多文件）
- ✅ 终端支持（macOS PTY、Windows ConPTY 待测试）
- ✅ 只读 Git 操作（状态、日志、差异）
- ✅ 结构化编辑预览（创建、更新、删除、重命名）
- ✅ 检查点与回滚（Git 感知、用户变更分离）
- ⏳ Codex Agent 保持只读（审批流程已实现但未启用）

**安全基础**:
- 后台作业队列、调度租约、恢复快照和进程观测已实现内部基础
- 当前仅开放 metadata-only 的 AAP/Qt 检查入口
- 不会派发后台工作、自动接管进程、自动重试、自动审批或执行无人值守写入
- 恢复日志只保存内容无关的复核证据，不会改变作业状态
- 进程退出不会被当成作业成功，缺少真实进程句柄或完整终止事件时必须人工核对

**后台通知** (`background-job-notification-intent/0.1`):
- 只保存作业/预算身份、状态、耗尽维度和稳定去重键
- 不保存通知标题、正文或任务内容
- 固定声明平台投递不可用、未尝试且无投递权限
- Workbench schema v12 已将 intent 与事件原子写入持久 outbox
- 当前仍无 AAP/Qt 投递控制、调度器自动生产、macOS/Windows 通知调用

**后台恢复** (`session/background-recovery`):
- 提供会话级只读分页，可查看恢复结论、租约/进程归属、阻塞原因和已记录审查
- 固定声明 dispatch、自动重试、自动审批、自动接管和 mutation authority 均不可用
- 不会改变作业、租约或进程状态

### 开发路线图

**近期目标** (完成度 > 80%):
- 完成 Section 2 (Windows UI 测试)
- 完成 Section 3 (协议规范)
- 完成 Section 14 (Windows 终端测试)
- 完成 Section 23 (可见状态审计)

**中期目标** (0-20% 完成):
- Section 10: Model Profiles, Routing, and Switching (模型配置与路由)
- Section 17: Context Engine and Compaction (上下文引擎与压缩)
- Section 18: Permission, Sandbox, and Secret Enforcement (权限与沙箱)
- Section 19: Skills, Plugins, MCP, Hooks (技能与插件系统)

详细进度见 [OpenSpec 任务列表](openspec/changes/build-aegisy-agent-workbench/tasks.md)。

## 系统要求

- CMake 3.16+
- C++17 编译器
- Qt 5.15+ 或 Qt 6，包含 SQL 与 WebSockets 模块
- OpenSSL
- Node.js：本地网关和 CLI 管理需要
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
sudo apt install build-essential cmake qt6-base-dev qt6-websockets-dev libqt6sql6-sqlite libqt6network6 libssl-dev libsecret-tools
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
set OPENSSL_ROOT_DIR=C:\path\to\openssl
set OPENSSL_DIR=%OPENSSL_ROOT_DIR%\bin
set AEGISY_SPARKLE_PRIVATE_KEY_FILE=C:\Users\you\.aegisy\sparkle-private-key
package-windows.bat
```

需要预先安装 Qt、OpenSSL、CMake、Visual Studio 2022 和 Inno Setup 6。
`OPENSSL_DIR` 必须位于同一个 `OPENSSL_ROOT_DIR` 下。打包脚本会校验 Qt Network
导入、SSL/crypto DLL 架构与版本，并从暂存目录执行真实 HTTPS TLS 探测；任一检查
失败都会在生成安装包前停止。发布前仍必须在没有开发版 Qt/OpenSSL `PATH` 的干净
Windows x64 环境安装并运行完整安装包。

## 使用流程

1. 登录 Aegisy 账号。
2. 点击“新建配置”，输入名称并选择唯一终端。
3. 选择与该终端平台匹配的 API Key，可选查询并指定模型。
4. 保存并激活档案。应用会先备份，再更新对应终端配置；其它终端的当前档案不会被清除。
5. 需要回退时，点击顶部“备份”并恢复历史版本。
6. macOS 和 Windows 可从顶部“更新”菜单检查新版本或调整自动检查开关。
7. 点击顶部“生图”，选择 `gpt-image` 分组及其 Key，设置参数并生成、预览或保存图片。
8. 点击侧栏“系统体检”检查依赖与配置；当前档案可用“启动”按钮选择并记住项目目录后运行 CLI。
9. 点击右上角余额查看用量中心；点击侧栏“本地网关”可选择启用本机代理与请求监控。
10. 点击侧栏“桌面增强”查看全量模型与 Codex 插件、同步历史会话、安装 Computer Use 或以运行时方式启动 Claude 中文界面。
11. 点击右上角头像修改密码或兑换密卡；点击“API Keys”创建和维护账号 Key。
12. 点击侧栏“AI 对话”，选择 API Key 和模型后开始流式对话；历史保存在本机应用数据目录，不保存 API Key。
13. 点击侧栏“Skills”安装、导入、启用或禁用技能；首次使用 PPT Skill 时在该页面安装隔离运行环境。

“迁移”菜单提供加密档案导入和导出。导出密码无法找回，请单独妥善保存。

## 安全说明

- 登录 Token 和档案 API Key 不写入普通日志。
- 档案元数据保存在 `QSettings`，真实 API Key 只保存在系统凭据库。
- 本地网关只监听 `127.0.0.1`，使用独立本地令牌；真实 Key 只在客户端和网关进程内存之间传递。
- 网关请求日志不保存 prompt、completion、工具参数或文件内容。
- Linux 没有可用 Secret Service 时，应用拒绝持久化凭据，不会退回固定密钥 XOR。
- 加密导出文件使用 PBKDF2-HMAC-SHA256（200,000 次）和 AES-256-GCM。
- 备份文件权限限制为当前用户读写。
- Claude 汉化只驻留在 Claude 进程内存中，关闭 Claude 后失效；Aegisy 会先验证调试目标确实属于 Claude。
- 插件安装调用 Codex 官方 `plugin add` 命令，不直接修改 Codex 插件缓存或绕过账号权限。
- 修改密码、密卡兑换和 Key 增删改均使用登录 JWT 调用 Aegisy 官方接口，密码与兑换码不写入日志或本地配置。
- AI 对话直接使用所选 API Key 调用兼容接口，提示词和回复不写入日志；历史记录只写入当前用户的本机应用数据目录，且不包含 API Key。
- 桌面安装包下载 Token 只发送给 Aegisy HTTPS 地址；客户端不跟随重定向，并在保存前验证服务端代理标识和安装包格式。
- 通过 URL 或目录安装的第三方 Skill 默认是“仅指令”模式，脚本不会被自动执行；安装器仅接受 HTTPS、限制包大小并拒绝目录穿越路径。
- PPT 运行时安装在应用数据目录的独立 Python 虚拟环境，不修改系统 Python；生成文件保存在当前用户的 Skill 产物目录。

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
src/system_doctor_dialog.cpp 系统依赖、CLI、配置和安全存储体检
src/usage_dialog.cpp      账号、模型和 API Key 用量中心
src/skill_manager.cpp     Skills 安装、扫描、权限、路由与本地执行器
src/skills_dialog.cpp      Skills 管理页面与 PPT 运行环境安装
src/gateway_manager.cpp   本地网关生命周期、凭据管道和请求元数据
src/desktop_downloader.cpp 受认证的桌面安装包流式代理下载与校验
src/status_badge.cpp       通用语义状态组件
src/update_manager_mac.mm  macOS Sparkle 应用内更新桥接
src/update_manager_win.cpp Windows WinSparkle 应用内更新桥接
src/main_window.cpp      主界面与档案操作
tests/                   自动化测试
```

macOS、Windows 发布流程和更新源说明见 `release/README.md`。

完整操作、配置修复、CC Switch 共存和故障排查见 [Aegisy Desktop 使用指南](USER-GUIDE.md)。
生产端桌面安装包流式代理要求见 [桌面安装包代理接口契约](docs/DESKTOP-DOWNLOAD-PROXY.md)。

网站：<https://www.aegisy.cc>
