#!/usr/bin/env bash
set -euo pipefail

# ── Nautilus Toolkit 安装脚本 ──

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Nautilus 扩展目录 (自动检测)
NAUTILUS_EXT_DIR=""
if pkg-config --exists libnautilus-extension-4 2>/dev/null; then
    NAUTILUS_EXT_DIR="$(pkg-config --variable=extensiondir libnautilus-extension-4)"
fi

if [ -z "$NAUTILUS_EXT_DIR" ]; then
    echo "错误: 未检测到 libnautilus-extension-4"
    exit 1
fi

# ── LLVM 工具链 / 最大优化参数 ──
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"
RANLIB="${RANLIB:-llvm-ranlib}"
STRIP="${STRIP:-llvm-strip}"
LD="${LD:-ld.lld}"
LTO_MODE="${LTO_MODE:-full}"
RELEASE_CFLAGS="${RELEASE_CFLAGS:--O3 -DNDEBUG -march=native -mtune=native -flto=${LTO_MODE} -fno-semantic-interposition}"
RELEASE_LDFLAGS="${RELEASE_LDFLAGS:--fuse-ld=lld -flto=${LTO_MODE} -Wl,-O3 -Wl,--gc-sections -Wl,--as-needed -Wl,--icf=all}"

# ── 检查依赖 ──
missing=()
for cmd in cmake ninja pkg-config "$CC" "$AR" "$RANLIB" "$STRIP" "$LD"; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done
if ! pkg-config --exists libadwaita-1 2>/dev/null; then
    missing+=("libadwaita-1 (dev)")
fi
if ! pkg-config --exists json-glib-1.0 2>/dev/null; then
    missing+=("json-glib-1.0 (dev)")
fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "错误: 缺少构建依赖: ${missing[*]}"
    echo "Arch Linux:   sudo pacman -S cmake ninja libadwaita json-glib"
    echo "Ubuntu/Debian: sudo apt install cmake ninja-build libadwaita-1-dev libjson-glib-dev"
    echo "Fedora:       sudo dnf install cmake ninja-build libadwaita-devel json-glib-devel"
    exit 1
fi

# ── 构建 ──
echo "==> 构建 nautilus-toolkit ..."
echo "==> 优化: LLVM full LTO, lld, -O3, native CPU, section GC, ICF"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_RANLIB="$RANLIB" \
    -DCMAKE_STRIP="$STRIP" \
    -DCMAKE_C_COMPILER_AR="$AR" \
    -DCMAKE_C_COMPILER_RANLIB="$RANLIB" \
    -DNTK_ENABLE_IPO=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF \
    -DCMAKE_C_FLAGS_RELEASE="$RELEASE_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$RELEASE_LDFLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="$RELEASE_LDFLAGS" \
    -DCMAKE_MODULE_LINKER_FLAGS_RELEASE="$RELEASE_LDFLAGS"
cmake --build "$BUILD_DIR"

# ── 安装扩展 ──
if [ -f "${BUILD_DIR}/nautilus-toolkit.so" ]; then
    echo "==> 安装 Nautilus 扩展到 ${NAUTILUS_EXT_DIR} ..."
    sudo install -Dm755 -s --strip-program="$STRIP" \
        "${BUILD_DIR}/nautilus-toolkit.so" \
        "${NAUTILUS_EXT_DIR}/nautilus-toolkit.so"

    if command -v nautilus >/dev/null 2>&1; then
        echo "==> 重启 Nautilus ..."
        nautilus -q 2>/dev/null || true
    fi
fi

echo ""
echo "安装完成!"
echo "  Nautilus 扩展: ${NAUTILUS_EXT_DIR}/nautilus-toolkit.so"
echo ""
echo "运行时依赖: 7z, bsdtar(libarchive), file, tar, zstd"
echo "右键菜单将显示「解压/压缩」菜单项。"
