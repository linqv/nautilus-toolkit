#!/usr/bin/env bash
set -euo pipefail

# ── Nautilus Toolkit 卸载脚本 ──

PREFIX="${PREFIX:-/usr/local}"
BINDIR="${PREFIX}/bin"

# Nautilus 扩展目录 (自动检测)
NAUTILUS_EXT_DIR=""
if pkg-config --exists libnautilus-extension-4 2>/dev/null; then
    NAUTILUS_EXT_DIR="$(pkg-config --variable=extensiondir libnautilus-extension-4)"
fi

removed=0

# ── 删除 CLI ──
if [ -f "${BINDIR}/smart-unzip" ]; then
    echo "==> 删除 ${BINDIR}/smart-unzip"
    sudo rm -f "${BINDIR}/smart-unzip"
    removed=1
fi

# ── 删除 Nautilus 扩展 ──
if [ -n "$NAUTILUS_EXT_DIR" ] && [ -f "${NAUTILUS_EXT_DIR}/nautilus-toolkit.so" ]; then
    echo "==> 删除 ${NAUTILUS_EXT_DIR}/nautilus-toolkit.so"
    sudo rm -f "${NAUTILUS_EXT_DIR}/nautilus-toolkit.so"
    removed=1

    if command -v nautilus >/dev/null 2>&1; then
        echo "==> 重启 Nautilus ..."
        nautilus -q 2>/dev/null || true
    fi
fi

if [ "$removed" -eq 0 ]; then
    echo "未找到已安装的 nautilus-toolkit 组件"
    echo "  检查路径: ${BINDIR}/smart-unzip"
    [ -n "$NAUTILUS_EXT_DIR" ] && echo "  检查路径: ${NAUTILUS_EXT_DIR}/nautilus-toolkit.so"
    exit 1
fi

echo ""
echo "卸载完成!"
