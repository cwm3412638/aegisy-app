#include "help_dialog.h"
#include "app_theme.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// ── 每个章节的 Markdown 文档 ─────────────────────────────────────

const char *kSections[][2] = {
    {
        "快速开始",
        R"(
## 什么是 Aegisy？

**Aegisy** 是一个 AI 工具连接管理平台，帮助你在本地电脑上使用
Claude Code、Codex CLI、Gemini CLI 等 AI 编程助手。

通过 Aegisy，你只需在网页端管理 API Key，桌面客户端会自动
把认证信息写入各 CLI 工具的配置文件，无需手动修改任何文件。

---

## 第一步：登录账号

在 https://aegisy.cc 注册账号后，使用邮箱和密码登录桌面端。

---

## 第二步：创建配置

1. 点击顶栏「**新建配置**」按钮
2. 填写配置名称（如「工作账号 Claude」）
3. 选择工具类型：Claude Code / Codex CLI / Gemini CLI / OpenCode
4. 选择对应的 API Key（从账号 Key 列表中选择）
5. 选择使用的模型（可选）
6. 点击「**保存配置**」完成创建

---

## 第三步：激活配置

- 在配置卡片上点击「**激活**」按钮
- 系统会自动将 API Key 写入对应 CLI 的本地配置文件
- 激活后在终端运行 `claude` / `codex` / `gemini` 命令即可使用

---

## 第四步：启动终端

激活配置后，点击卡片上的「**启动**」按钮，系统会自动检测
macOS 上的 iTerm2 / Terminal、Windows 上的 Windows Terminal
并在其中运行 CLI 工具。
)"
    },
    {
        "连接配置管理",
        R"(
## 配置卡片说明

每张配置卡片显示：
- **配置名称**（顶部大字）
- **工具类型** badge（Claude / Codex / Gemini / OpenCode）
- **当前使用** badge（绿色，当前激活的配置）
- **工具信息行**：工具名称 · 版本号 · 配置文件路径
- **Key 信息**：是否已绑定，末尾数位用于区分不同 Key
- **模型**：当前配置使用的模型

---

## 筛选配置

左侧侧边栏可按工具类型筛选配置：
- **全部配置**：显示所有工具的配置
- **Claude Code**：仅显示 Claude 配置
- **Codex CLI**：仅显示 Codex 配置
- **Gemini CLI**：仅显示 Gemini 配置
- **OpenCode**：仅显示 OpenCode 配置

---

## 快速切换（不弹确认框）

首次激活时，在确认对话框底部勾选「**以后直接切换**」，
之后激活任何配置都无需确认，和 cc-switch 一样一键切换。

如需重新开启确认框，在终端执行：
```
defaults delete cc.aegisy.client ui/skipActivateConfirm
```

---

## 全工具一键切换

点击内容区「**全部切换**」按钮，可以同时为 Claude / Codex /
Gemini 三个工具选择 Key，一次性全部写入配置。
)"
    },
    {
        "备份与恢复",
        R"(
## 自动备份机制

每次激活配置前，Aegisy 会**自动备份**当前 CLI 工具的配置文件：
- Claude Code：`~/.claude/settings.json`
- Codex CLI：`~/.codex/auth.json` + `~/.codex/config.toml`
- Gemini CLI：`~/.gemini/.env`
- OpenCode：`~/.config/opencode/config.json`

每个工具最多保留最近 **10 次**备份，超出时自动删除最旧的。

---

## 查看备份历史

点击顶栏「**备份**」按钮，可以：
1. 按工具类型切换查看备份列表
2. 选择某个备份点后点击「**恢复所选备份**」
3. 恢复前会先备份当前配置，失败时自动回滚

---

## 配置写入后的校验

Aegisy 写入配置文件后会**回读验证**，确保 Key 确实写入成功。
如果写入或校验失败，会自动回滚到备份状态并报错。

---

## 导出/导入加密档案

点击顶栏「**迁移**」按钮可以：
- **导出**：将所有配置用 AES-256-GCM 加密打包为 `.aegisy` 文件
- **导入**：使用密码解密并导入其他设备的配置档案

适合换电脑时快速迁移所有配置。
)"
    },
    {
        "AI 对话",
        R"(
## 打开 AI 对话

点击左侧侧边栏「**AI 对话**」按钮，即可打开 AI 聊天窗口。

---

## 选择 API Key 和模型

对话窗口顶部有两个下拉框：
1. **API Key**：选择要使用的账号 Key
2. **模型**：从当前 Key 支持的模型中选择

选择 Key 后模型列表会自动加载。

---

## 发送消息

- **Enter** 发送，**Shift+Enter** 换行
- 点击「发送」按钮发送

---

## 消息操作

鼠标悬停在消息上会出现操作按钮：
- **复制**：复制消息内容
- **编辑**（用户消息）：修改并重新发送
- **重新发送**（用户消息）：不修改内容重新发送
- **重新生成**（AI 回复）：重新生成该条回复

---

## 快速 Skills

对话框底部有两个 Skill 快捷按钮：
- **生图**：使用 GPT Image 生成图片
- **PPT**：根据描述自动生成演示文稿

---

## 历史对话

左侧「历史对话」列表保存所有会话。
点击「新对话」开始全新对话，底部显示本次会话的 Token 用量。
)"
    },
    {
        "Skills 管理",
        R"(
## 什么是 Skills？

Skills 是可以在 AI 对话中**自动调用**的扩展功能模块。
当你的消息匹配某个 Skill 的触发条件时，系统会自动调用对应功能，
而不是普通的 AI 对话。

---

## 安装 Skill

点击侧边栏「**Skills**」打开管理界面：

1. 切换到「**可安装**」标签
2. 可使用搜索框过滤
3. 选中 Skill 后在底部点击「**安装**」

---

## 管理已安装的 Skills

「**已安装**」标签显示所有已安装的 Skills：
- **启用/禁用**：最左侧勾选框控制是否启用
- **执行器**：instruction（纯文本引导）/ image（图片生成）/ presentation（PPT）
- **权限**：该 Skill 需要的权限级别
- **状态**：兼容（可用）/ 不兼容（环境缺失）

---

## 内置 Skills

Aegisy 内置了两个 Skills：
- **GPT Image 生图**：使用 DALL-E / GPT Image 生成图片
- **PPT 制作**：AI 规划大纲 + Python 自动生成 PPTX

PPT Skill 需要安装 Python 运行环境，点击「**安装 PPT 运行环境**」一键安装。

---

## 使用斜杠命令强制调用

在对话输入框输入：
- `/image 一只可爱的猫`：强制使用生图 Skill
- `/ppt 介绍量子计算的演示文稿，6页`：强制使用 PPT Skill
)"
    },
    {
        "MCP 配置",
        R"(
## 什么是 MCP？

MCP（Model Context Protocol）是 Anthropic 推出的协议，
允许 AI 工具调用外部工具服务器（如文件系统、数据库、API 等）。

---

## 打开 MCP 配置面板

点击侧边栏「**MCP 配置**」按钮。

---

## 添加 MCP 服务器

1. 点击「**＋ 添加**」
2. 输入服务器名称（唯一标识，如 `filesystem`）
3. 输入命令路径（Stdio 模式）或 URL（SSE 模式）：
   - **Stdio**：`/usr/bin/npx @modelcontextprotocol/server-filesystem /path`
   - **SSE**：`http://localhost:3000/sse`
4. 点击「**保存**」

---

## 切换档案时自动保留

Aegisy 使用**合并写入**模式：激活配置时只更新认证字段，
`mcpServers` 等其他设置**不会被覆盖**，你无需担心切换档案
会删除已配置的 MCP 服务器。

---

## 常用 MCP 服务器

| 服务器 | 命令 |
|--------|------|
| 文件系统 | `npx @modelcontextprotocol/server-filesystem ~/` |
| GitHub | `npx @modelcontextprotocol/server-github` |
| 数据库 | `npx @modelcontextprotocol/server-sqlite database.db` |
)"
    },
    {
        "本地网关",
        R"(
## 什么是本地网关？

本地网关是一个运行在 `127.0.0.1:43112` 的代理服务。

开启后，AI 工具的请求会先经过本地网关，再由网关转发到
Aegisy 服务器，实现：
- **快速档案切换**：切换时无需重启 CLI（网关动态替换 Key）
- **请求监控**：查看实际发出的 API 请求
- **安全隔离**：真实 API Key 不写入 CLI 配置文件，只存在内存中

---

## 开启本地网关

点击侧边栏「**本地网关**」，打开网关管理界面并启用。

启用后，激活任意配置时都会自动通过网关路由。

---

## 注意事项

- 本地网关仅监听本机 127.0.0.1，不向外网暴露
- 网关意外停止时会自动回退到直连模式
- 重启应用会根据上次设置自动启动网关
)"
    },
    {
        "系统体检",
        R"(
## 系统体检功能

点击侧边栏「**系统体检**」，可以检查所有依赖项的状态：

---

## 检查项目

| 类别 | 检查内容 |
|------|---------|
| **系统依赖** | Node.js、npm、Git 是否安装及版本 |
| **AI CLI 工具** | Claude Code、Codex、Gemini、OpenCode 安装状态及版本 |
| **桌面应用** | Claude Desktop、ChatGPT 是否安装 |
| **其它工具** | OpenCode、OpenClaw、VSCode 等 |

---

## 一键更新 CLI

体检界面发现 CLI 有新版本时，可以直接点击「**更新**」按钮
运行 `npm install -g` 升级到最新版本。

---

## 冲突检测

如果系统环境变量（`ANTHROPIC_API_KEY`、`OPENAI_API_KEY` 等）
覆盖了配置文件中的值，Aegisy 会显示⚠️ 警告提示。

Aegisy 启动终端时会自动清除这些环境变量，但外部终端需要
手动删除（通常在 `~/.bashrc` 或 `~/.zshrc` 中）。
)"
    },
    {
        "常见问题",
        R"(
## ❓ 激活后 CLI 还是使用旧 Key？

已运行的终端进程会把 Key 读入内存，不会自动重新读取配置文件。
**关闭并重新启动终端**后，新 Key 才会生效。

---

## ❓ 切换配置会删除我的 MCP 设置吗？

不会。Aegisy 使用**合并写入**模式，只更新认证相关字段，
`mcpServers`、`hooks` 等其他字段保持不变。

---

## ❓ 账号 API Key 和档案 Key 是两套体系？

是的：
- **账号 API Key**：在 aegisy.cc 网页端管理，用于 AI 对话功能
- **档案 Key**：同一个 Key，通过档案配置写入 CLI 工具的本地配置

两者的 Key 字符串是同一个，但用途和管理入口不同。

---

## ❓ 如何让两台电脑使用同一配置？

点击顶栏「**迁移**」→「导出加密档案」，将 `.aegisy` 文件
发送到另一台电脑后，使用「导入加密档案」恢复。

---

## ❓ 密卡是什么？

密卡是 Aegisy 的充值卡，购买后输入卡号即可为账号充值余额。
在账号中心「密卡充值」标签页输入兑换码即可。

点击「**前往购买密卡**」链接或访问：
https://pay.ldxp.cn/shop/6W0YTHLS

---

## ❓ 如何联系支持？

官网：https://aegisy.cc
如遇问题，可在官网联系客服或提交工单。
)"
    },
};

constexpr int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

} // namespace

HelpDialog::HelpDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("使用说明文档"));
    resize(860, 620);
    setMinimumSize(720, 500);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 左侧章节列表 ──────────────────────────────────────────────
    auto *sidebar = new QWidget(this);
    sidebar->setFixedWidth(180);
    sidebar->setStyleSheet(QStringLiteral(
        "QWidget { background: #f8fafc; border-right: 1px solid #e4e7ec; }"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 12, 0, 12);
    sideLayout->setSpacing(2);

    auto *docTitle = new QLabel(QStringLiteral("  使用文档"), sidebar);
    docTitle->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: #98a2b3; padding: 4px 12px 8px 12px;"
        "letter-spacing: 0.5px; background: transparent;"));
    sideLayout->addWidget(docTitle);

    auto *navList = new QListWidget(sidebar);
    navList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { padding: 9px 14px; border-radius: 0; font-size: 13px; color: #475467; }"
        "QListWidget::item:selected { background: #e7f5f2; color: #0f5f59; font-weight: 600;"
        "  border-left: 3px solid #0f766e; padding-left: 11px; }"
        "QListWidget::item:hover:!selected { background: #f0f2f5; }"));

    for (int i = 0; i < kSectionCount; ++i) {
        navList->addItem(QString::fromUtf8(kSections[i][0]));
    }
    sideLayout->addWidget(navList, 1);
    root->addWidget(sidebar);

    // ── 右侧文档内容 ──────────────────────────────────────────────
    auto *content = new QWidget(this);
    content->setStyleSheet(QStringLiteral("QWidget { background: #ffffff; }"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    auto *stack = new QStackedWidget(content);
    for (int i = 0; i < kSectionCount; ++i) {
        auto *browser = new QTextBrowser(stack);
        browser->setOpenExternalLinks(true);
        browser->setFrameShape(QFrame::NoFrame);
        browser->setStyleSheet(QStringLiteral(
            "QTextBrowser { background: #ffffff; border: none; padding: 0; font-size: 13px; }"));
        browser->document()->setDefaultStyleSheet(QStringLiteral(
            "body { color: #17212b; font-family: system-ui; line-height: 1.65; margin: 0; }"
            "h2 { color: #101828; font-size: 16px; font-weight: 700; margin: 16px 0 8px 0; }"
            "p { margin: 6px 0 10px 0; color: #344054; }"
            "code, pre {"
            "  font-family: 'Consolas','Menlo',monospace; font-size: 12px;"
            "  background: #f2f4f7; border: 1px solid #e4e7ec; border-radius: 4px;"
            "  padding: 2px 6px;"
            "}"
            "pre { padding: 10px 14px; display: block; margin: 8px 0; }"
            "hr { border: none; border-top: 1px solid #f0f2f5; margin: 16px 0; }"
            "table { border-collapse: collapse; width: 100%; margin: 10px 0; }"
            "th { background: #f7f9fb; color: #667085; font-size: 12px; font-weight: 600;"
            "     padding: 8px 12px; border-bottom: 1px solid #e4e7ec; text-align: left; }"
            "td { padding: 8px 12px; color: #344054; border-bottom: 1px solid #f0f2f5; }"
            "ul, ol { margin: 6px 0 10px 0; padding-left: 20px; color: #344054; }"
            "li { margin: 4px 0; }"
            "a { color: #0f766e; }"
            "strong { color: #101828; font-weight: 600; }"
        ));
        browser->setContentsMargins(28, 24, 28, 24);
        browser->setMarkdown(QString::fromUtf8(kSections[i][1]));
        stack->addWidget(browser);
    }
    contentLayout->addWidget(stack, 1);

    // 底部工具栏
    auto *footer = new QWidget(content);
    footer->setFixedHeight(52);
    footer->setStyleSheet(QStringLiteral(
        "QWidget { background: #f8fafc; border-top: 1px solid #e4e7ec; }"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 0, 18, 0);
    auto *officialLink = new QPushButton(QStringLiteral("打开官网 aegisy.cc"), footer);
    officialLink->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #0f766e; border: none;"
        " font-size: 12px; font-weight: 600; padding: 0; }"
        "QPushButton:hover { color: #0b625c; text-decoration: underline; }"));
    officialLink->setCursor(Qt::PointingHandCursor);
    connect(officialLink, &QPushButton::clicked, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://aegisy.cc")));
    });
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), footer);
    closeBtn->setFixedHeight(34);
    closeBtn->setMinimumWidth(72);
    closeBtn->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    footerLayout->addWidget(officialLink);
    footerLayout->addStretch();
    footerLayout->addWidget(closeBtn);
    contentLayout->addWidget(footer);

    root->addWidget(content, 1);

    // 默认选中第一章
    navList->setCurrentRow(0);
    connect(navList, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
}
