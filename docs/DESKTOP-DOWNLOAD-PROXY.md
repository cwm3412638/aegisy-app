# 桌面安装包代理接口契约

本文定义 Aegisy Desktop 使用的生产下载接口。它属于当前 `https://www.aegisy.cc/api/v1` 登录体系，不是新 Web 控制面的 `/api/me` 路由。

## 接口

```http
GET /api/v1/desktop-downloads/{product}/{platform}
Authorization: Bearer <Aegisy login JWT>
Accept: application/octet-stream
X-Aegisy-Client: desktop
```

支持矩阵：

| product | platform | 上游 |
| --- | --- | --- |
| `claude` | `mac-arm64` | Claude macOS universal PKG |
| `claude` | `mac-x64` | Claude macOS universal PKG |
| `claude` | `win-x64` | Claude Windows x64 MSIX |
| `chatgpt` | `mac-arm64` | ChatGPT macOS DMG |
| `chatgpt` | `win-x64` | ChatGPT Windows x64 signed EXE/MSIX |

其他组合返回 `400 unsupported_download`。ChatGPT macOS 当前要求 Apple Silicon；Windows 安装包必须由服务端固定映射到经过验证的官方签名 EXE/MSIX，不允许把 Microsoft Store 页面或任意客户端 URL 当作上游。

## 官方上游

上游必须由服务端固定映射，禁止从 query、body 或 header 接收任意 URL。

Claude macOS 与 Windows 使用 `downloads.claude.ai` 的版本化官方 CDN 地址。版本更新必须先在隔离环境验证文件大小、格式签名和平台，再更新服务端固定配置；不要在用户请求中访问容易触发挑战页的 `claude.ai/api/desktop/.../redirect`。

ChatGPT macOS 使用 `https://persistent.oaistatic.com/codex-app-prod/ChatGPT.dmg`。ChatGPT Windows 上游由服务端固定配置维护；发布前必须验证 Authenticode/包签名、x64 架构和 EXE/MSIX 格式，再允许代理路由返回。

如固定入口发生重定向，每一跳都必须重新校验 HTTPS 和主机白名单，最多 3 跳。白名单仅包含 `downloads.claude.ai` 与 `persistent.oaistatic.com`；不得把上游 `Location` 返回给客户端。

官方入口依据：[Claude Download](https://claude.com/download)、[ChatGPT Download](https://chatgpt.com/download/)。固定 URL 应由定时健康检查验证，页面结构变化不应在用户请求时临时抓取解析。

## 成功响应

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Disposition: attachment; filename="Claude.pkg"
Content-Length: <when known>
X-Aegisy-Download-Mode: proxy
X-Aegisy-Installer-Format: pkg
X-Content-Type-Options: nosniff
Cache-Control: private, no-store
```

响应体必须从上游流式复制到客户端：不允许 `ReadAll`、`arrayBuffer()` 或先写完整临时文件。客户端断开时取消上游请求；写入变慢时自然传递 backpressure。

`Authorization`、Cookie 和 Aegisy 内部 header 绝不能转发给上游。只向上游发送必要的 `User-Agent`、`Accept` 与 Range 相关 header。

## 鉴权与限流

- 使用与 `/api/v1/auth/me` 相同的登录 JWT 校验逻辑。
- 路由不接受 API Key 代替登录 JWT。
- 每用户最多 1 个并发下载、每小时最多启动 3 次；服务全局最多 3 个并发下载。
- 超限返回 `429` 和 `Retry-After`，不要在错误正文或日志中记录 Token。
- 该下载不进入模型用量、余额或计费流水，但应有独立的下载次数、字节数、失败阶段和上游耗时指标。

## 网络与资源边界

- 复用进程级 HTTP client 和连接池；禁止每次请求新建 transport。
- DNS、连接、TLS 和每次重定向都只能访问显式允许的 HTTPS 主机。
- 建议连接超时 10 秒、响应头超时 30 秒、读取空闲超时 60 秒、总时长上限 30 分钟。
- 最大响应体 2 GB；上游 `Content-Length` 超限时在发送响应头前拒绝，无长度时在流式计数到上限后取消。
- 在返回 200 前检查上游状态、Content-Type 和前 64 字节，拒绝 HTML、JSON 和错误页。

## 错误响应

错误在尚未开始流式响应时使用 JSON：

| HTTP | code | 场景 |
| --- | --- | --- |
| 400 | `unsupported_download` | product/platform 不在固定矩阵 |
| 401 | `unauthorized` | JWT 缺失、无效或过期 |
| 429 | `download_rate_limited` | 用户、IP 或并发限制 |
| 502 | `download_upstream_invalid` | 上游状态、类型、主机或前缀不符合预期 |
| 504 | `download_upstream_timeout` | 连接、响应头、空闲或总时长超时 |

流已经开始后发生错误时只能取消连接，并记录不含敏感信息的 request ID、product、platform、已传输字节数和失败阶段。

## 验收

1. 未登录请求返回 401；合法登录 JWT 返回安装包。
2. 客户端看到的最终 URL 始终是 `www.aegisy.cc`，响应没有 `Location`。
3. 响应包含 `X-Aegisy-Download-Mode: proxy`，首字节来自服务端连接。
4. 代理进程常驻内存在下载 500 MB 文件时不随文件大小线性增长。
5. 客户端中断后，上游连接在 2 秒内取消。
6. HTML、JSON、非白名单重定向、超大文件和超时均被拒绝。
7. 使用真实 Claude/ChatGPT 上游至少执行一次端到端下载，并由客户端完成 DMG/PKG/EXE/MSIX 校验。
