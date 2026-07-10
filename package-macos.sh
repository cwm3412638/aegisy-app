#!/bin/bash
# Aegisy Client - macOS release packaging
# Output: dist/AegisyClient-<version>-macOS-<arch>.dmg

set -euo pipefail

echo "=================================="
echo " Aegisy Client macOS 打包"
echo "=================================="

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[错误] package-macos.sh 只能在 macOS 上运行"
    exit 1
fi

for command_name in cmake hdiutil codesign otool ditto file install_name_tool lipo shasum; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "[错误] 未找到 $command_name"
        exit 1
    }
done

QT_BIN=""
if command -v macdeployqt >/dev/null 2>&1; then
    QT_BIN="$(dirname "$(command -v macdeployqt)")"
elif [ -x "/opt/homebrew/opt/qt@6/bin/macdeployqt" ]; then
    QT_BIN="/opt/homebrew/opt/qt@6/bin"
elif [ -x "/usr/local/opt/qt@6/bin/macdeployqt" ]; then
    QT_BIN="/usr/local/opt/qt@6/bin"
fi
[ -n "$QT_BIN" ] || {
    echo "[错误] 未找到 macdeployqt，请安装 Qt6"
    exit 1
}

QMAKE="$QT_BIN/qmake"
if [ ! -x "$QMAKE" ]; then
    QMAKE="$(command -v qmake6 || command -v qmake || true)"
fi
[ -x "$QMAKE" ] || {
    echo "[错误] 未找到 qmake，无法定位 Qt 插件"
    exit 1
}

QT_LIBS="$($QMAKE -query QT_INSTALL_LIBS)"
QT_PLUGINS="$($QMAKE -query QT_INSTALL_PLUGINS)"
echo "Qt 库目录：$QT_LIBS"
echo "Qt 插件目录：$QT_PLUGINS"

BUILD_DIR="build"
APP="$BUILD_DIR/AegisyClient.app"
STAGE="$BUILD_DIR/dmg-root"
MOUNT_POINT="$BUILD_DIR/dmg-mount"

echo ""
echo "[1/5] 编译 Release..."
rm -rf "$APP" "$STAGE" "$MOUNT_POINT"
mkdir -p "$BUILD_DIR"

CMAKE_ARGS=(
    -S .
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
)
if [ -d "/opt/homebrew/opt/qt@6/lib/cmake/Qt6" ]; then
    CMAKE_ARGS+=("-DQt6_DIR=/opt/homebrew/opt/qt@6/lib/cmake/Qt6")
fi
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --config Release -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

[ -x "$APP/Contents/MacOS/AegisyClient" ] || {
    echo "[错误] 未生成可执行应用：$APP"
    exit 1
}

echo ""
echo "[2/5] 部署 Qt 和运行库..."
mkdir -p "$APP/Contents/PlugIns/platforms" "$APP/Contents/PlugIns/tls"
cp "$QT_PLUGINS/platforms/libqcocoa.dylib" \
   "$APP/Contents/PlugIns/platforms/"
cp "$QT_PLUGINS/tls/libqsecuretransportbackend.dylib" \
   "$APP/Contents/PlugIns/tls/"

DEPLOY_ARGS=(
    "$APP"
    -no-plugins
    -no-codesign
    -always-overwrite
    "-libpath=$QT_LIBS"
    "-executable=$APP/Contents/PlugIns/platforms/libqcocoa.dylib"
    "-executable=$APP/Contents/PlugIns/tls/libqsecuretransportbackend.dylib"
)
if [ -d "/opt/homebrew/lib" ]; then
    DEPLOY_ARGS+=("-libpath=/opt/homebrew/lib")
fi
if [ -d "/usr/local/lib" ]; then
    DEPLOY_ARGS+=("-libpath=/usr/local/lib")
fi
"$QT_BIN/macdeployqt" "${DEPLOY_ARGS[@]}" -verbose=2

echo ""
echo "[3/5] 检查依赖并签名..."

# Homebrew 的部分库会保留绝对安装名。它们作为 bundle 内文件的 ID
# 没有运行时价值，还会让可移植性检查和后续签名变得不可靠。
while IFS= read -r -d '' binary; do
    if ! file "$binary" | grep -q "Mach-O"; then
        continue
    fi

    install_id="$(otool -D "$binary" 2>/dev/null | tail -n +2 | head -n 1 || true)"
    case "$install_id" in
        /opt/homebrew/*|/usr/local/*)
            relative_path="${binary#"$APP/Contents/Frameworks/"}"
            if [[ "$relative_path" == *.framework/Versions/* ]]; then
                framework_name="${relative_path%%/*}"
                framework_binary="${relative_path#"$framework_name/"}"
                portable_id="@rpath/$framework_name/$framework_binary"
            else
                portable_id="@rpath/$(basename "$binary")"
            fi
            install_name_tool -id "$portable_id" "$binary"
            ;;
    esac
done < <(find "$APP/Contents" -type f -print0)

dependency_error=0
while IFS= read -r -d '' binary; do
    if ! file "$binary" | grep -q "Mach-O"; then
        continue
    fi
    while IFS= read -r dependency; do
        case "$dependency" in
            /opt/homebrew/*|/usr/local/*)
                echo "[错误] 仍引用本机依赖：$binary -> $dependency"
                dependency_error=1
                ;;
        esac
    done < <(otool -L "$binary" | tail -n +2 | awk '{print $1}')
done < <(find "$APP/Contents" -type f -print0)

if [ "$dependency_error" -ne 0 ]; then
    exit 1
fi

# 当前机器没有 Developer ID 时使用 ad-hoc 签名。先签内部二进制，再签 bundle。
while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q "Mach-O"; then
        codesign --force --sign - --timestamp=none "$binary"
    fi
done < <(find "$APP/Contents" -type f -print0)
codesign --force --deep --sign - --timestamp=none "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
ARCH="$(lipo -archs "$APP/Contents/MacOS/AegisyClient" | tr ' ' '-')"
DMG_NAME="AegisyClient-${VERSION}-macOS-${ARCH}.dmg"
DMG_PATH="dist/$DMG_NAME"

echo ""
echo "[4/5] 生成 DMG..."
rm -f "$DMG_PATH" dist/AegisyClient.dmg
mkdir -p dist "$STAGE"
ditto "$APP" "$STAGE/AegisyClient.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create \
    -volname "Aegisy Client" \
    -srcfolder "$STAGE" \
    -ov \
    -format UDZO \
    "$DMG_PATH"
cp "$DMG_PATH" dist/AegisyClient.dmg

echo ""
echo "[5/5] 挂载回验..."
mkdir -p "$MOUNT_POINT"
hdiutil attach "$DMG_PATH" -nobrowse -readonly -mountpoint "$MOUNT_POINT" >/dev/null
trap 'hdiutil detach "$MOUNT_POINT" >/dev/null 2>&1 || true' EXIT
test -x "$MOUNT_POINT/AegisyClient.app/Contents/MacOS/AegisyClient"
test -L "$MOUNT_POINT/Applications"
codesign --verify --deep --strict --verbose=2 "$MOUNT_POINT/AegisyClient.app"
hdiutil detach "$MOUNT_POINT" >/dev/null
trap - EXIT

SHA256="$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')"
SIZE="$(du -h "$DMG_PATH" | awk '{print $1}')"

echo ""
echo "=================================="
echo " 打包完成"
echo "=================================="
echo "安装包：$DMG_PATH"
echo "兼容架构：$ARCH"
echo "文件大小：$SIZE"
echo "SHA-256：$SHA256"
echo ""
echo "当前为 ad-hoc 签名、未公证版本。其他 Mac 首次打开时可能需要"
echo "右键选择“打开”，或在“系统设置 → 隐私与安全性”中允许打开。"
