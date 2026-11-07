#!/usr/bin/env bash
# 云端环境安装脚本：克隆并安装 ESP-IDF v5.5.3（仅 esp32c3 工具链）。
# 幂等：已存在 $IDF_PATH 时跳过克隆，只重跑 install.sh。
set -euo pipefail

IDF_VERSION="${IDF_VERSION:-v5.5.3}"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_PATH
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"

log() { printf '\n[install] %s\n' "$*"; }

if ! command -v git >/dev/null 2>&1; then
  log "缺少 git，请先在镜像里安装（Debian/Ubuntu: apt-get install -y git python3 python3-pip）"
  exit 1
fi

if [ -f "$IDF_PATH/export.sh" ]; then
  log "已检测到 $IDF_PATH，跳过克隆"
else
  log "克隆 ESP-IDF $IDF_VERSION 到 $IDF_PATH"
  mkdir -p "$(dirname "$IDF_PATH")"
  # 浅克隆 + 递归子模块，减少云端首次拉取体积。
  git clone -b "$IDF_VERSION" --depth 1 --recursive \
    https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi

log "安装 esp32c3 工具链到 $IDF_TOOLS_PATH"
cd "$IDF_PATH"
./install.sh esp32c3

log "完成。后续使用: source $IDF_PATH/export.sh"
