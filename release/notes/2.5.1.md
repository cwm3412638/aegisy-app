# Aegisy Client 2.5.1

本版本重点增强配置可靠性、桌面安装包下载、使用文档和日常操作体验。

## 主要变化

- 配置激活状态与磁盘健康状态分离；认证文件被删除、损坏或被其他工具改写时会立即提示修复。
- Codex `config.toml` 根配置会自动迁移到首个 TOML 表之前，避免字段误入 `[tui.model_availability_nux]` 导致启动错误。
- Codex 激活和修复写入 `model_context_window = 272000` 与 `model_auto_compact_token_limit = 272000`，旧值或错误位置会被检测并修复。
- 激活配置前自动备份，写入失败或校验失败时恢复原文件。
- Claude、Codex、Gemini 和 OpenCode 分别维护当前配置，切换一个工具不会清除其他工具的激活状态。
- 保留 CC Switch 的 Provider、项目权限、MCP、hooks 和其他未管理配置；最后执行切换的工具决定当前 Provider。
- 启动 CLI 时可选择项目目录，并记住每个配置最近使用的目录。
- 清理启动进程中的旧 Provider 环境变量，减少外部环境覆盖当前配置的问题。
- OpenCode 使用独立本地网关路由，配置预览、健康检查和实际写入保持一致。
- Claude Desktop 与 ChatGPT Desktop 改为经过 Aegisy 登录认证的服务器流式代理下载，不向官方 CDN 转发 Aegisy Token。
- 下载器拒绝重定向、HTML、JSON、超大文件及无代理标识响应，并校验 DMG、PKG、EXE 和 MSIX 安装包特征。
- 主界面、连接向导、MCP 和系统体检使用更清晰的状态组件、图标和操作布局。
- 使用指南改为 Markdown 单一来源并在应用内渲染，补充配置修复、CC Switch 共存、代理下载和安全说明。

macOS 发布包为 Apple Silicon 版本。当前构建使用 ad-hoc 代码签名和 Sparkle Ed25519 更新签名，未进行 Apple 公证。
