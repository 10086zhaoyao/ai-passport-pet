# 云端环境配置（TraeWork）

面向 ESP32-C3 的 AI Passport 灵宠身份卡玩法。本目录只放云端镜像/沙箱的启动脚本，不参与固件构建。

- 目标芯片：`esp32c3`
- ESP-IDF：`v5.5.3`
- Flash：8 MB，无 PSRAM
- 构建命令：`idf.py build`（产物在 `build/`，已被 `.gitignore` 排除）

## TraeWork UI 需要填写的字段

| 字段 | 建议值 | 说明 |
| --- | --- | --- |
| 名称 | `ai-passport-pet` | 与 GitHub 仓库名保持一致 |
| Python 版本 | `3.11` | ESP-IDF 5.5 要求 Python >= 3.9；3.11/3.12 均可，镜像若默认 3.13 需显式降级 |
| 环境变量 | `IDF_PATH=/home/user/esp/esp-idf`<br>`IDF_TOOLS_PATH=/home/user/.espressif` | 路径需与镜像中 `$HOME` 一致；镜像用户不是 `user` 时改成实际家目录 |
| install 命令 | `bash cloud/install.sh` | 幂等，重复执行不会重复克隆 |
| start 命令 | `bash cloud/start.sh` | 激活 `export.sh` 并执行 `idf.py set-target esp32c3` |
| 默认终端 | `bash` | 脚本是 bash；默认 shell 为 sh/zsh 时用 `bash <script>` 显式调用 |

可选环境变量：

- `IDF_VERSION`：默认 `v5.5.3`，一般不需要改。
- `IDF_SKIP_SET_TARGET=1`：跳过 `start.sh` 里的 `set-target`，用于复用已有构建目录。

## 脚本行为

- `cloud/install.sh`：若 `$IDF_PATH/export.sh` 不存在，则浅克隆 ESP-IDF `v5.5.3`（含递归子模块）到 `$IDF_PATH`，再执行 `./install.sh esp32c3` 安装工具链。
- `cloud/start.sh`：`source $IDF_PATH/export.sh`，切到仓库根目录，执行 `idf.py set-target esp32c3`，最后打印 `idf.py --version`。

两个脚本在 git 索引中已带可执行位（`100755`），直接用 `./cloud/install.sh` 也可以。

## 网络白名单风险

ESP-IDF 安装与构建依赖以下外网域名，任一不可达都会导致环境准备失败：

1. `github.com` / `codeload.github.com` / `objects.githubusercontent.com`：克隆 ESP-IDF 与子模块。
2. `dl.espressif.com`：**关键风险点**。工具链、`esptool`、`openocd` 等二进制默认从这里下载；若白名单未放开，`install.sh` 会在下载阶段失败。
3. `pypi.org` / `files.pythonhosted.org`：安装 `requirements.txt` 里的 Python 依赖。

应对方式：

- 优先让云端放开 `dl.espressif.com` 与 `github.com`；国内镜像可设置 `IDF_GITHUB_MIRROR` 指向内部镜像。
- 若 `dl.espressif.com` 无法放开，需要预先把工具链打进镜像（离线包），或在白名单里加入 Espressif 的下载 CDN 域名后在 install 阶段缓存。
- 判断是否命中该风险：`install.sh` 日志里出现 `Failed to download`、`Could not resolve host: dl.espressif.com` 或超时，即为白名单/出网限制，而不是脚本问题。

## 未验证项

云端镜像尚未实际跑通，`install.sh` / `start.sh` 仅做了语法检查（见提交说明）。首次在 TraeWork 里启动时，请确认：

- `idf.py --version` 输出含 `v5.5.3`
- `idf.py set-target esp32c3` 成功生成 `build/`
- `idf.py build` 能通过（需要联网下载 `managed_components` 依赖）
