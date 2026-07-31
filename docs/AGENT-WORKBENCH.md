# Agent Workbench

Agent Workbench 是 Aegisy 的下一代代理工作台界面，提供完整的对话管理、时间线可视化和上下文控制。

## 状态

**预览版** - 功能标志控制，默认禁用

## 功能特性

### 核心界面
- **三栏布局**: 会话列表、时间线、上下文面板
- **产品导航栏**: Chat、Work、Projects、Sessions、Extensions、Settings
- **可调整窗格**: 拖拽调整大小，支持隐藏/显示
- **命令面板**: Cmd+K (macOS) / Ctrl+K (Windows) 快速访问
- **原生菜单**: 完整的菜单栏和键盘快捷键

### Timeline 和 Composer
- **6 种项目类型**: 用户、代理、命令、错误、批准、使用情况
- **状态渲染**: 流式（进行中）和完成状态
- **内联批准**: 风险等级指示器（高/中/低）
- **结构化问题**: 选项卡片，支持选择和提交
- **Composer**: 执行上下文显示，消息输入
- **附件系统**: 文件、图像、诊断预览

### 安全特性
- **CSP 强制执行**: 阻止所有外部资源
- **导航阻止**: 仅允许本地 qrc:// 资源
- **JavaScript 沙箱**: 无法访问本地文件或远程 URL
- **崩溃恢复**: 渲染器崩溃自动恢复
- **隔离 Profile**: 无缓存和 Cookie

### 无障碍支持
- **ARIA 标签**: 完整的屏幕阅读器支持
- **键盘导航**: 所有功能可通过键盘访问
- **焦点指示器**: 清晰的焦点状态
- **减少动画**: 支持 prefers-reduced-motion
- **高对比度**: 支持 prefers-contrast

### 主题系统
- **深色主题**: 默认主题
- **浅色主题**: 自动检测 prefers-color-scheme
- **高对比度**: 自动检测 prefers-contrast
- **系统字体**: 使用系统原生字体栈

## 启用方法

### 方法 1: 通过 QSettings（推荐）

在 macOS 上：
```bash
defaults write cc.aegisy.AegisyClient features/agentWorkbench -bool true
```

在 Windows 上（注册表）：
```
HKEY_CURRENT_USER\Software\Aegisy\AegisyClient
features/agentWorkbench = 1 (DWORD)
```

### 方法 2: 通过代码

```cpp
#include "feature_flags.h"

FeatureFlags::setAgentWorkbenchEnabled(true);
```

### 禁用

```bash
# macOS
defaults delete cc.aegisy.AegisyClient features/agentWorkbench

# 或设置为 false
defaults write cc.aegisy.AegisyClient features/agentWorkbench -bool false
```

## 键盘快捷键

### 全局
- `Cmd+K` / `Ctrl+K` - 打开命令面板

### 视图
- `Ctrl+B` - 切换左侧面板
- `Ctrl+Shift+B` - 切换右侧面板
- `Ctrl+Shift+R` - 重置布局

### Composer
- `Cmd+Enter` / `Ctrl+Enter` - 发送消息

## 架构

### 技术栈
- **Qt WebEngine**: 渲染引擎
- **QWebChannel**: Qt-JavaScript 桥接
- **localStorage**: 布局持久化
- **CSS Variables**: 主题系统

### 安全架构
```
┌─────────────────────────────────────┐
│  Qt Application (Native)            │
│  ┌───────────────────────────────┐  │
│  │  QWebEngineView                │  │
│  │  ┌─────────────────────────┐  │  │
│  │  │  Isolated Profile       │  │  │
│  │  │  - No Cache             │  │  │
│  │  │  - No Cookies           │  │  │
│  │  │  - CSP Enforced         │  │  │
│  │  │  - Navigation Blocked   │  │  │
│  │  └─────────────────────────┘  │  │
│  │  ┌─────────────────────────┐  │  │
│  │  │  QWebChannel Bridge     │  │  │
│  │  │  - Type-safe            │  │  │
│  │  │  - Size limits          │  │  │
│  │  └─────────────────────────┘  │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

## 开发

### 构建
```bash
cmake -B build -S .
cmake --build build --target AegisyClient
```

### 测试
```bash
# 运行所有测试
ctest --test-dir build

# 运行特定测试
./build/test_feature_flags
./build/test_workbench_security
./build/test_workbench_accessibility
./build/test_workbench_window
```

### 代码原则
- **最小化代码**: 仅实现必要功能
- **无冗余**: 每行代码都有明确目的
- **安全优先**: 多层安全防护
- **无障碍优先**: ARIA 和语义 HTML

## 限制

### 当前版本
- ✅ 完整的 UI 基础
- ✅ 所有主要组件
- ⏳ 后端集成（待完成）
- ⏳ 实时更新（待完成）
- ⏳ 数据持久化（待完成）

### 已知问题
- 批准提交功能需要后端集成
- 问题回答功能需要后端集成
- Turn 提交功能需要后端集成
- 附件选择功能需要后端集成

## OpenSpec 进度

- **Section 11**: 100% (9/9 任务)
- **Section 12**: UI 基础完成 (6 个部分任务)
- **总体**: 31% (74/235 任务)

## 贡献

参见 [CONTRIBUTING.md](../CONTRIBUTING.md)

## 许可

参见项目根目录的 LICENSE 文件
