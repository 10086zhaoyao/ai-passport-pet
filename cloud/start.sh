#!/usr/bin/env bash
# 云端终端启动脚本：激活 ESP-IDF 环境并把工程目标芯片设为 esp32c3。
set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_PATH
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "[start] 未找到 $IDF_PATH/export.sh，请先执行 bash cloud/install.sh" >&2
  exit 1
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh"

cd "$PROJECT_DIR"

# 每次启动都显式指定目标芯片，避免继承到其他芯片的 build 目录。
if [ "${IDF_SKIP_SET_TARGET:-0}" != "1" ]; then
  idf.py set-target esp32c3
fi

echo "[start] 环境就绪：$(idf.py --version) 目录=$PROJECT_DIR"
echo "[start] 常用命令: idf.py build | idf.py -p <PORT> flash monitor"
