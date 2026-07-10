#!/bin/bash
# Aegisy Client - macOS release packaging
# Output: DMG installer plus signed Sparkle update feed under dist/updates

set -euo pipefail

echo "=================================="
echo " Aegisy Client macOS 打包"
echo "=================================="

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[错误] package-macos.sh 只能在 macOS 上运行"
    exit 1
fi

for command_name in cmake hdiutil codesign otool ditto file install_name_tool lipo shasum xmllint; do
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
SPARKLE_VERSION="2.9.4"
SPARKLE_ROOT="$BUILD_DIR/_deps/sparkle-$SPARKLE_VERSION"
SPARKLE_ACCOUNT="${AEGISY_SPARKLE_ACCOUNT:-aegisy}"
UPDATE_BASE_URL="${AEGISY_UPDATE_BASE_URL:-https://aegisy.cc/desktop/macos}"
MACOS_DEPLOYMENT_TARGET="${AEGISY_MACOS_DEPLOYMENT_TARGET:-26.0}"
SIGNING_IDENTITY="${AEGISY_CODESIGN_IDENTITY:--}"

echo ""
echo "[1/6] 编译 Release..."
rm -rf "$APP" "$STAGE" "$MOUNT_POINT"
mkdir -p "$BUILD_DIR"

CMAKE_ARGS=(
    -S .
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET"
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
echo "[2/6] 部署 Qt 和运行库..."
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
echo "[3/6] 检查依赖并签名..."

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

# 移除构建机路径，确保 @rpath 只从应用包内解析。
while IFS= read -r -d '' binary; do
    if ! file "$binary" | grep -q "Mach-O"; then
        continue
    fi
    while IFS= read -r rpath; do
        case "$rpath" in
            /opt/homebrew/*|/usr/local/*|"$PWD"/*)
                install_name_tool -delete_rpath "$rpath" "$binary"
                ;;
        esac
    done < <(otool -l "$binary" | awk '/cmd LC_RPATH/{getline; getline; print $2}')
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

# 没有 Developer ID 时使用 ad-hoc 签名。正式发布可传 AEGISY_CODESIGN_IDENTITY。
SIGN_ARGS=(--force --sign "$SIGNING_IDENTITY")
if [ "$SIGNING_IDENTITY" = "-" ]; then
    SIGN_ARGS+=(--timestamp=none)
else
    SIGN_ARGS+=(--options runtime --timestamp)
fi
while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q "Mach-O"; then
        codesign "${SIGN_ARGS[@]}" "$binary"
    fi
done < <(find "$APP/Contents" -type f -print0)
codesign --deep "${SIGN_ARGS[@]}" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
ARCH="$(lipo -archs "$APP/Contents/MacOS/AegisyClient" | tr ' ' '-')"
DMG_NAME="AegisyClient-${VERSION}-macOS-${ARCH}.dmg"
DMG_PATH="dist/$DMG_NAME"
UPDATE_DIR="dist/updates/macos"
UPDATE_BASENAME="AegisyClient-${VERSION}-macOS-${ARCH}"
UPDATE_ZIP="$UPDATE_DIR/$UPDATE_BASENAME.zip"

if [ -n "${AEGISY_NOTARY_PROFILE:-}" ]; then
    if [ "$SIGNING_IDENTITY" = "-" ]; then
        echo "[错误] 公证需要通过 AEGISY_CODESIGN_IDENTITY 指定 Developer ID"
        exit 1
    fi
    command -v xcrun >/dev/null 2>&1 || {
        echo "[错误] 未找到 xcrun，无法执行 Apple 公证"
        exit 1
    }
    NOTARY_ARCHIVE="$BUILD_DIR/AegisyClient-notary.zip"
    rm -f "$NOTARY_ARCHIVE"
    ditto -c -k --sequesterRsrc --keepParent "$APP" "$NOTARY_ARCHIVE"
    xcrun notarytool submit "$NOTARY_ARCHIVE" \
        --keychain-profile "$AEGISY_NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    rm -f "$NOTARY_ARCHIVE"
fi

echo ""
echo "[4/6] 生成 Sparkle 更新源..."
[ -x "$SPARKLE_ROOT/bin/generate_appcast" ] || {
    echo "[错误] 未找到 Sparkle 发布工具：$SPARKLE_ROOT/bin/generate_appcast"
    exit 1
}
mkdir -p "$UPDATE_DIR"
rm -f "$UPDATE_ZIP"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$UPDATE_ZIP"

RELEASE_NOTES="release/notes/$VERSION.md"
if [ -f "$RELEASE_NOTES" ]; then
    cp "$RELEASE_NOTES" "$UPDATE_DIR/$UPDATE_BASENAME.md"
fi

APPCAST_ARGS=(
    --download-url-prefix "$UPDATE_BASE_URL/"
    --release-notes-url-prefix "$UPDATE_BASE_URL/"
    --link "https://aegisy.cc"
    --maximum-versions 5
    --maximum-deltas 5
    "$UPDATE_DIR"
)
if [ -n "${AEGISY_SPARKLE_PRIVATE_KEY:-}" ]; then
    printf '%s' "$AEGISY_SPARKLE_PRIVATE_KEY" | \
        "$SPARKLE_ROOT/bin/generate_appcast" --ed-key-file - "${APPCAST_ARGS[@]}"
else
    "$SPARKLE_ROOT/bin/generate_appcast" \
        --account "$SPARKLE_ACCOUNT" "${APPCAST_ARGS[@]}"
fi
cp "$UPDATE_DIR/appcast.xml" dist/appcast.xml
cp "$UPDATE_DIR/appcast.xml" dist/macos-appcast.xml
xmllint --noout "$UPDATE_DIR/appcast.xml"
UPDATE_SIGNATURE="$(xmllint --xpath \
    'string(//*[local-name()="enclosure"]/@*[local-name()="edSignature"])' \
    "$UPDATE_DIR/appcast.xml")"
[ -n "$UPDATE_SIGNATURE" ] || {
    echo "[错误] appcast 中没有更新包签名"
    exit 1
}
if [ -n "${AEGISY_SPARKLE_PRIVATE_KEY:-}" ]; then
    printf '%s' "$AEGISY_SPARKLE_PRIVATE_KEY" | \
        "$SPARKLE_ROOT/bin/sign_update" --ed-key-file - --verify \
        "$UPDATE_ZIP" "$UPDATE_SIGNATURE"
else
    "$SPARKLE_ROOT/bin/sign_update" --account "$SPARKLE_ACCOUNT" --verify \
        "$UPDATE_ZIP" "$UPDATE_SIGNATURE"
fi

echo ""
echo "[5/6] 生成 DMG..."
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
echo "[6/6] 挂载回验..."
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
UPDATE_SHA256="$(shasum -a 256 "$UPDATE_ZIP" | awk '{print $1}')"
UPDATE_SIZE="$(du -h "$UPDATE_ZIP" | awk '{print $1}')"

echo ""
echo "=================================="
echo " 打包完成"
echo "=================================="
echo "安装包：$DMG_PATH"
echo "兼容架构：$ARCH"
echo "文件大小：$SIZE"
echo "SHA-256：$SHA256"
echo "更新包：$UPDATE_ZIP"
echo "更新包大小：$UPDATE_SIZE"
echo "更新包 SHA-256：$UPDATE_SHA256"
echo "更新源：$UPDATE_DIR/appcast.xml"
echo ""
if [ "$SIGNING_IDENTITY" = "-" ]; then
    echo "当前为 ad-hoc 签名、未公证版本。正式自动更新发布前请配置"
    echo "AEGISY_CODESIGN_IDENTITY 和 AEGISY_NOTARY_PROFILE。"
fi
