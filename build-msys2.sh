#!/usr/bin/env bash
# 在 Windows 10 的 MSYS2 环境下编译本启动器为原生 Windows 可执行文件。
#
# 用法：在 MSYS2 的 MINGW64 或 UCRT64 终端中执行
#   ./build-msys2.sh
#
# 说明：
#   - 自动根据当前 MSYSTEM（MINGW64/UCRT64/CLANG64）选择包名前缀
#   - 编译依赖：gcc / pkg-config / gtk3 / openssl（必须）
#   - 运行依赖：freerdp(xfreerdp) / virt-viewer(remote-viewer)，如需内置连接请一并安装
set -euo pipefail

case "${MSYSTEM:-}" in
	UCRT64)  P=mingw-w64-ucrt-x86_64    ;;
	CLANG64) P=mingw-w64-clang-x86_64   ;;
	CLANG32) P=mingw-w64-clang-i686     ;;
	MINGW32) P=mingw-w64-i686           ;;
	*)       P=mingw-w64-x86_64         ;; # MINGW64 默认
esac

echo "==> MSYSTEM=${MSYSTEM:-<未设置>}，使用前缀 ${P}"

# 编译依赖
pacman -S --needed --noconfirm make "$P-gcc" "$P-pkg-config" "$P-gtk3" "$P-openssl"

# 运行依赖（可选：不装则只做启动器，连接时需自行配置 xfreerdp / remote-viewer 的 PATH）
pacman -S --needed --noconfirm "$P-freerdp" "$P-virt-viewer"

echo "==> 开始编译"
make clean
make

echo
echo "==> 完成，产物：./xfreerdp-launcher.exe"
echo "    可直接双击运行，或复制其依赖 DLL 后分发。"