#!/bin/bash
# 设备端界面预览：格式化 → 构建 → 出图。用法：bash device/tools/ui-preview/run.sh
# 时钟字体与角色图片不在仓库里（来自设备），放在 device/build/ui-preview/ 下。
set -e
cd "$(dirname "$0")/../../.."
VS="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
CM="$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
B=device/build/ui-preview
[ -f "$B/CMakeCache.txt" ] || "$CM" -S device/tools/ui-preview -B "$B" -G "Visual Studio 17 2022" -A x64 >/dev/null
"$CM" --build "$B" --config Release --parallel 4 2>&1 | grep -E " error |warning C4" || true
mkdir -p "$B/out"
"$B/Release/ui_preview.exe" frontend/assets/device/font-noto-sc.otf "$B/font-clock.ttf" "$B/out" "$B/char.gif" | grep -v "^wrote\|font loaded" || true
echo "previews in $B/out"
