# Aegisy Client 2.5.1 for Windows

本版本重点增强配置可靠性、桌面安装包下载、使用文档和 Windows CLI 安装体验。

## 主要变化

- 修复 Node.js 安装在 `C:\Program Files\nodejs` 时，Codex、Gemini、Claude 和 OpenCode CLI 无法通过 npm 安装的问题。
- 修复 Windows 系统命令输出中的中文乱码。
- 配置激活状态与磁盘健康状态分离；认证文件被删除、损坏或被其他工具改写时会立即提示修复。
- Codex `config.toml` 根配置会自动迁移到首个 TOML 表之前，避免字段误入 `[tui.model_availability_nux]` 导致启动错误。
- Codex 激活和修复写入 `model_context_window = 272000` 与 `model_auto_compact_token_limit = 272000`。
- 激活配置前自动备份，写入失败或校验失败时恢复原文件。
- Claude、Codex、Gemini 和 OpenCode 分别维护当前配置，切换一个工具不会清除其他工具的激活状态。
- 保留 CC Switch 的 Provider、项目权限、MCP、hooks 和其他未管理配置。
- 启动 CLI 时可选择项目目录，并记住每个配置最近使用的目录。
- Claude Desktop 与 ChatGPT Desktop 改为经过 Aegisy 登录认证的服务器流式代理下载。
- 主界面、连接向导、MCP 和系统体检使用更清晰的状态组件、图标和操作布局。
- 使用指南改为 Markdown 单一来源并在应用内渲染。
