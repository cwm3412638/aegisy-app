#!/bin/bash
# ============================================================
#  Aegisy Client - macOS 打包脚本
#  产出：dist/AegisyClient.dmg  可分发磁盘映像
# ============================================================
set -e

echo "=================================="
echo " Aegisy Client macOS 打包"
echo "=================================="

# ---- 1) 前置检查 ----
command -v cmake >/dev/null 2>&1 || { echo "[错误] 未找到 cmake"; exit 1; }

# 定位 Qt 的 bin（macdeployqt 所在）
QT_BIN=""
if command -v macdeployqt >/dev/null 2>&1; then
    QT_BIN="$(dirname "$(command -v macdeployqt)")"
elif [ -d "/opt/homebrew/opt/qt@6/bin" ]; then
    QT_BIN="/opt/homebrew/opt/qt@6/bin"
elif [ -d "/usr/local/opt/qt@6/bin" ]; then
    QT_BIN="/usr/local/opt/qt@6/bin"
fi
[ -z "$QT_BIN" ] && { echo "[错误] 未找到 macdeployqt，请确认已安装 Qt6（brew install qt@6）"; exit 1; }
echo "使用 Qt 工具目录：$QT_BIN"

# ---- 2) 编译 Release ----
echo ""
echo "[1/3] 编译 Release..."
mkdir -p build
cd build
if [ -d "/opt/homebrew/opt/qt@6" ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release -DQt6_DIR=/opt/homebrew/opt/qt@6/lib/cmake/Qt6
else
    cmake .. -DCMAKE_BUILD_TYPE=Release
fi
make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cd ..

APP="build/AegisyClient.app"
[ -d "$APP" ] || { echo "[错误] 未生成 $APP，请确认 CMakeLists 已设 MACOSX_BUNDLE"; exit 1; }

# ---- 3) macdeployqt 打包 + 生成 dmg ----
echo ""
echo "[2/3] macdeployqt 打包依赖并生成 dmg..."
# 先清理旧 dmg，macdeployqt 不会覆盖
rm -f build/AegisyClient.dmg
"$QT_BIN/macdeployqt" "$APP" -dmg

echo ""
echo "[3/3] 移动到 dist/ ..."
mkdir -p dist
mv build/AegisyClient.dmg dist/AegisyClient.dmg

echo ""
echo "=================================="
echo " 完成！可分发映像：dist/AegisyClient.dmg"
echo "=================================="
echo ""
echo "提示：未做代码签名 / 公证，用户首次打开需在"
echo "「系统设置 → 隐私与安全性」里点「仍要打开」。"
echo "如需正式分发，建议用 Apple Developer 证书签名并公证。"
