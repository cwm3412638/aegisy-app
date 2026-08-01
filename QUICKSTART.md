# Quick Start Guide

Get started with Aegisy Agent Workbench development in 5 minutes.

## Prerequisites

**macOS:**
```bash
brew install cmake qt@6 openssl@3 rust
```

**Ubuntu/Debian:**
```bash
sudo apt install build-essential cmake qt6-base-dev qt6-websockets-dev \
  libqt6sql6-sqlite libssl-dev curl
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

**Windows:**
- Visual Studio 2022, Qt 6.5+, OpenSSL, Rust

## Build & Run

```bash
./build.sh                    # macOS/Linux
open build/AegisyClient.app   # macOS
./build/AegisyClient          # Linux
```

## Enable Agent Workbench

Feature flag (disabled by default):
- Settings → Advanced → Feature Flags → Enable “Agent Workbench”
- Or: `./build/AegisyClient --enable-workbench`

## Development

```bash
# Debug build with tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure

# Useful scripts
./scripts/project-status.sh      # Project status
./scripts/openspec-find.sh -i    # Find tasks
```

## Project Structure

```
src/              # Qt C++ source
include/          # Qt C++ headers
agent-runtime/    # Rust sidecar
tests/            # Tests
docs/             # Documentation
```

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - System design
- [CONTRIBUTING.md](CONTRIBUTING.md) - Guidelines
- [AAP-API-REFERENCE.md](docs/AAP-API-REFERENCE.md) - API reference
- [README.md](README.md) - Full documentation

## Status

Progress: 101/239 tasks (42%)

Completed: Event Store, Workbench UI, Timeline, Files/Editor, Structured Edits

---

## 中文快速启动

### macOS
```bash
brew install cmake qt@6 openssl@3
./build.sh
open build/AegisyClient.app
```

### Ubuntu/Debian
```bash
sudo apt install build-essential cmake qt6-base-dev libqt6network6 libssl-dev
./build.sh
./build/AegisyClient
```

### Windows
```bat
build.bat
build\Release\AegisyClient.exe
```

### 第一次使用
1. 登录 Aegisy 账号
2. 新建配置，选择终端和 API Key
3. 保存并激活

详见 [USER-GUIDE.md](USER-GUIDE.md)
