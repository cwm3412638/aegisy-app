# Aegisy Desktop 使用指南

Aegisy Desktop 用于管理 Aegisy 账号、API Key 和本机 AI 工具连接。当前支持 Claude Code、Codex CLI、Gemini CLI 与 OpenCode，并提供本地网关、系统体检、桌面客户端、AI 对话、Skills、MCP、用量和加密迁移等功能。

## 快速开始

1. 使用 Aegisy 邮箱和密码登录。启用“记住登录状态”后，登录 Token 会保存到系统凭据库。
2. 打开“API Keys”，确认账号中已有目标平台的可用 Key。
3. 点击“新建配置”，输入名称，选择一个工具、API Key 和模型。
4. 保存后点击配置卡片上的“激活”。Aegisy 会备份、合并写入并校验本地配置。
5. 点击“启动”，选择项目目录，应用会在系统终端中从该目录启动对应 CLI。

每个配置只绑定一个工具。Claude、Codex、Gemini 和 OpenCode 分别保留自己的当前配置，切换 Codex 不会取消 Claude 的激活状态。

## 配置与激活

配置卡片会显示工具、本地版本、脱敏 Key、模型、配置路径和当前状态。常用操作如下：

| 操作 | 结果 |
| --- | --- |
| 激活 | 备份当前文件，合并写入认证字段，回读校验；失败时自动回滚 |
| 修复 | 重新写入当前配置，用于文件被删除、损坏或被其他工具改写后的恢复 |
| 启动 | 选择工作目录，并在系统终端中启动对应 CLI |
| 测试 | 检查 API Key、模型、连接延迟和服务端响应 |
| 编辑 | 修改名称、Key、模型或工具类型 |
| 删除 | 删除保存的配置；不会静默删除其他工具的本地配置 |

“全部切换”可一次为 Claude、Codex 和 Gemini 选择 Key。未选择的工具会跳过。

### Aegisy 管理的文件

| 工具 | 主要配置文件 |
| --- | --- |
| Claude Code | `~/.claude/settings.json` |
| Codex CLI | `~/.codex/auth.json`、`~/.codex/config.toml` |
| Gemini CLI | `~/.gemini/.env` |
| OpenCode | `~/.config/opencode/config.json` |

Aegisy 只更新自己管理的认证、模型和服务地址字段。JSON 对象、Codex 项目信任设置、MCP、hooks 和其他未管理内容会尽量保留。

## 配置健康与自动修复

“已激活”表示你希望当前工具使用哪个配置；“配置健康”表示磁盘上的实际文件仍然可用。两者是不同状态。

Aegisy 会监控当前配置文件。出现以下情况时，卡片和顶部状态会立即显示“需修复”，不会继续假装可用：

- 手动删除 `auth.json`、`settings.json`、`.env` 或 `config.json`。
- JSON、TOML 或环境变量格式损坏。
- API Key、模型或服务地址缺失。
- 文件被 CC Switch 或其他配置工具切换到别的 Provider。
- Codex 根配置项被错误写入 `[tui.model_availability_nux]`、`[projects.*]` 等 TOML 表中。

点击卡片上的“修复”即可重新写入。点击“启动”时若发现异常，也会先询问是否修复；未修复前不会启动一个已知配置错误的 CLI。

Codex 激活和修复会把 `model_provider`、`model` 等根配置项放回 TOML 文件顶部，并写入 `model_context_window = 272000` 与 `model_auto_compact_token_limit = 272000`。这既避免 `invalid type: string "OpenAI", expected u32 in tui.model_availability_nux` 一类启动错误，也让长对话在达到配置阈值时及时自动压缩。

## 在指定目录启动

配置激活后点击“启动”，选择当前项目目录。Aegisy 会：

1. 记住该配置上次使用的目录。
2. 清理可能覆盖当前配置的旧 Provider 环境变量。
3. 使用 macOS Terminal/iTerm、Windows Terminal/PowerShell/cmd 或常见 Linux 终端启动工具。
4. 将所选目录作为 CLI 的工作目录。

取消目录选择不会启动终端，也不会改变当前配置。

## 与 CC Switch 共存

激活 Aegisy Codex 配置时，Aegisy 会把当前根级 `model_provider` 和认证信息切换到 Aegisy，但不会删除 CC Switch 已保存的其他 Provider 定义。

以下内容会保留：

- `[model_providers.ccswitch]` 等非 Aegisy Provider 表。
- Provider 自带的 `experimental_bearer_token` 等字段。
- `[projects.*]` 项目信任设置。
- `[tui.model_availability_nux]` 模型可用性状态。
- `[features]` 中除 Aegisy 管理项以外的功能开关。

如果随后在 CC Switch 中切换 Provider，磁盘配置会以 CC Switch 的选择为准。Aegisy 会把自己的当前配置标记为“需修复”；再次点击“修复/激活”才会切回 Aegisy。两个应用可以共存，但最后一次执行切换的应用决定当前 Provider。

## 桌面客户端与代理下载

“下载桌面端”用于检测并获取 Claude Desktop 或 ChatGPT Desktop。

在支持的平台上，客户端请求受认证的 Aegisy 下载接口：

```text
GET /api/v1/desktop-downloads/{product}/{platform}
```

安装包必须由 Aegisy 美国节点流式代理返回。客户端拒绝 3xx 重定向，并要求响应包含 `X-Aegisy-Download-Mode: proxy` 与明确的安装包格式，因此不会把登录 Token 转发给官方 CDN，也不会把网页 HTML 误存成安装包。下载内容会边接收边写入临时文件，完成后校验 DMG、PKG、EXE 或 MSIX 的签名特征，再移动到“下载”目录。

代理不可用时，对话框会保留错误状态并提供“重试”和“打开官网”；不会自动跳转浏览器。Linux 或没有对应安装包的平台只显示官方下载页。

“桌面增强”还提供 Codex 插件、Computer Use、历史会话同步、全量模型与 Claude Desktop 运行时中文界面等能力。具体可用项取决于本机工具和账号权限。

## API Keys、模型与用量

“API Keys”支持创建、编辑、切换分组、启用、禁用、删除和连通性测试。Key 列表会自动分页加载。

“模型”根据当前 Key 查询服务端可用模型，不使用客户端内置白名单。新建配置时只会列出与目标工具平台匹配的 Key。

点击右上角余额可打开用量中心，查看：

- 时间范围汇总与模型统计。
- Key 今日消费、累计消费和额度。
- 余额、使用率、并发与订阅权益。

余额会定时刷新，也可手动刷新。

## AI 对话、图片与 Skills

“AI 对话”支持切换 Key 和模型、流式回复、停止生成、上下文 Token、历史会话、复制、编辑重发与重新生成。历史保存在当前用户的本机应用数据目录，不包含 API Key。

“生图”使用 `gpt-image` 分组下的个人 Key，支持模型、尺寸、质量、格式、预览和保存。

“Skills”支持 HTTPS Skill 目录、`SKILL.md`、`INSTALL.md` 或本地目录导入。第三方脚本默认不会自动执行。内置图片和 PPT Skill 可在对话中自动路由，也可使用 `/image`、`/ppt` 明确调用。

PPT 运行时安装在应用数据目录的隔离 Python 环境中，不会修改系统 Python。

## MCP 配置

“MCP 配置”用于维护 Claude 的 MCP 服务器。支持 Stdio 命令和 SSE 地址。

激活配置时采用合并写入，`mcpServers`、hooks 和其他无关设置不会被覆盖。添加服务器时应使用可信命令和地址，并确认其文件、网络与凭据权限。

## 本地网关

本地网关监听 `127.0.0.1:43112`，可让工具通过本机代理访问 Aegisy。启用后：

- CLI 配置中写入独立本地令牌，而不是真实 API Key。
- 切换配置时网关在内存中更新真实 Key。
- 请求监控只记录方法、路径、状态和耗时，不记录 prompt、completion、工具参数或文件内容。

网关只监听本机地址。网关不可用时，请关闭网关模式并重新激活配置恢复直连，不要把本地端口暴露到局域网或公网。

## 备份、恢复与迁移

每次激活或修复前都会创建同批次备份，每个工具最多保留最近 10 次。恢复前还会备份当前状态，失败时自动回滚。

恢复磁盘文件后，Aegisy 会重新检查当前配置。如果恢复内容仍与保存的当前配置一致，状态保持正常；如果不一致，则显示“需修复”，而不是仅凭旧的激活标记判断。

“迁移”可导出或导入 `.aegisy` 加密档案。导出使用 PBKDF2-HMAC-SHA256 和 AES-256-GCM，密码至少 8 个字符。密码没有找回机制，错误密码或损坏文件不会写入任何配置。

## 系统体检与更新

“系统体检”集中检查 Node.js、npm、Git、pnpm、Bun、AI CLI、桌面应用、额外开发工具、系统凭据存储和应用数据目录。状态以正常、提醒、错误和可选未安装区分。

CLI 未安装、版本落后或 npm 安装损坏时，可在操作列安装、更新或修复。旧 Provider 环境变量会显示冲突提示；从 Aegisy 启动终端时会清理它们，外部终端仍需从 shell 配置中移除后重启。

macOS 使用 Sparkle、Windows 使用 WinSparkle 检查更新和验证签名。Linux 当前需要手动安装新版本。

## 安全与本地存储

- Windows 使用 DPAPI，macOS 使用 Keychain，Linux 使用 Secret Service 保存登录 Token 和配置 Key。
- API Key 不写入普通 `QSettings`，也不会出现在活动日志中。
- Linux 没有 `secret-tool` 或可用 Secret Service 时，应用拒绝持久化凭据。
- 配置文件使用原子写入，失败时自动回滚。
- 备份文件限制为当前用户读写。
- 下载请求的 Aegisy 登录 Token 只发送给 `https://aegisy.cc`，不会跟随重定向。

macOS 首次读取已保存凭据时可能请求钥匙串授权。正式发布包需要持续使用同一个 Apple Developer ID 签名，才能让系统跨版本稳定识别应用身份。

## 常见问题

### 删除认证文件后为什么还显示当前配置

“当前配置”记录你的目标选择，文件健康状态记录磁盘实际情况。文件被删除后，配置会显示“需修复”，不会被当作可启动状态。点击“修复”重新生成文件即可。

### 激活后 CLI 仍使用旧 Key

先检查卡片是否为正常状态，再关闭已经运行的 CLI 进程并重新启动。外部终端还应检查 `OPENAI_API_KEY`、`ANTHROPIC_API_KEY`、`GEMINI_API_KEY` 等旧环境变量。

### Codex 报 `expected u32 in tui.model_availability_nux`

这是根级模型配置被写进 TOML 表导致的类型错误。回到 Aegisy，找到 Codex 当前配置并点击“修复”，然后重新启动 Codex。

### CC Switch 的配置会被删除吗

不会删除非 Aegisy Provider 和项目设置，但激活 Aegisy 会把当前 Provider 切换到 Aegisy。之后使用 CC Switch 切换时，Aegisy 会检测到配置变化并显示“需修复”。

### 桌面安装包下载得到网页

客户端会拒绝 HTML 和没有代理标识的响应，不会保存该文件。当前线上接口若尚未部署，会显示下载服务错误；可重试或由用户主动打开官网。

### 检查更新提示无法读取更新源

macOS 更新源为 `https://aegisy.cc/desktop/macos/appcast.xml`，Windows 更新源为 `https://aegisy.cc/desktop/windows/appcast.xml`。地址必须直接返回 Appcast XML，不能返回网站 HTML。

### 无法保存配置

确认已选择匹配平台的 API Key。Linux 还需要安装并启动 Secret Service，例如：

```bash
sudo apt install libsecret-tools
```

官网：<https://aegisy.cc>
