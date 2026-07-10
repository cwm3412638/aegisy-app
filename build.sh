#!/bin/bash
# Aegisy Client - Build Script for Linux/macOS

set -e

echo "=================================="
echo "Aegisy Client Build Script"
echo "=================================="

# 检测操作系统
OS=$(uname -s)
echo "Operating System: $OS"

# 检查依赖
check_dependency() {
    if ! command -v $1 &> /dev/null; then
        echo "Error: $1 is not installed"
        exit 1
    fi
}

echo ""
echo "Checking dependencies..."
check_dependency cmake
check_dependency make

# 检查 Qt
if ! command -v qmake &> /dev/null && ! command -v qmake6 &> /dev/null; then
    echo "Error: Qt is not installed"
    echo "Please install Qt6 or Qt5:"
    echo "  Ubuntu/Debian: sudo apt install qt6-base-dev libqt6network6 libssl-dev"
    echo "  macOS: brew install qt@6"
    exit 1
fi

# 检查 OpenSSL
if [ "$OS" = "Linux" ]; then
    if ! ldconfig -p | grep -q libssl; then
        echo "Error: OpenSSL is not installed"
        echo "Install with: sudo apt install libssl-dev"
        exit 1
    fi
fi

echo "All dependencies found!"

# 创建构建目录
echo ""
echo "Creating build directory..."
mkdir -p build
cd build

# 配置 CMake
echo ""
echo "Configuring CMake..."

if [ "$OS" = "Darwin" ]; then
    # macOS
    if [ -d "/opt/homebrew/opt/qt@6" ]; then
        cmake .. -DCMAKE_BUILD_TYPE=Release \
                 -DBUILD_TESTING=OFF \
                 -DQt6_DIR=/opt/homebrew/opt/qt@6/lib/cmake/Qt6
    else
        cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    fi
else
    # Linux
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
fi

# 编译
echo ""
echo "Building..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cd ..

echo ""
echo "=================================="
echo "Build completed successfully!"
echo "=================================="
if [ "$OS" = "Darwin" ]; then
    APP_PATH="build/AegisyClient.app"
    EXECUTABLE_PATH="$APP_PATH/Contents/MacOS/AegisyClient"
    if [ ! -x "$EXECUTABLE_PATH" ]; then
        echo "Error: macOS executable was not generated: $EXECUTABLE_PATH"
        exit 1
    fi
    echo "Application: $APP_PATH"
    echo "Executable:  $EXECUTABLE_PATH"
    echo ""
    echo "Run with: open $APP_PATH"
    echo "CLI run:  $EXECUTABLE_PATH"
else
    EXECUTABLE_PATH="build/AegisyClient"
    if [ ! -x "$EXECUTABLE_PATH" ]; then
        echo "Error: executable was not generated: $EXECUTABLE_PATH"
        exit 1
    fi
    echo "Executable: $EXECUTABLE_PATH"
    echo ""
    echo "Run with: ./$EXECUTABLE_PATH"
fi
