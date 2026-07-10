# Aegisy 快速启动

## macOS

```bash
brew install cmake qt@6 openssl@3
./build.sh
open build/AegisyClient.app
```

## Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev libqt6network6 libssl-dev libsecret-tools
./build.sh
./build/AegisyClient
```

## Windows

在 Visual Studio 2022 Developer Command Prompt 中执行：

```bat
build.bat
build\Release\AegisyClient.exe
```

## 第一次使用

1. 登录 Aegisy 账号。
2. 点击“新建配置”。
3. 选择 Claude Code、Codex CLI 或 Gemini CLI 中的一个终端。
4. 选择 API Key 和模型。
5. 保存并激活。

应用会在写入配置前自动备份。顶部“备份”可以恢复历史版本，“迁移”可以加密导入或导出档案。

更多说明见 [USER-GUIDE.md](USER-GUIDE.md)。
