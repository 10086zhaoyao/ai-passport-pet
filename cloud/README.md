<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# Cloud environment setup (TraeWork)

Pet identity card firmware for the ESP32-C3. This directory only holds the cloud
image/sandbox bootstrap scripts; it is not part of the firmware build.

- Target chip: `esp32c3`
- ESP-IDF: `v5.5.3`
- Flash: 8 MB, no PSRAM
- Build command: `idf.py build` (output goes to `build/`, which is gitignored)

## Fields to fill in the TraeWork UI

| Field | Suggested value | Notes |
| --- | --- | --- |
| Name | `ai-passport-pet` | Keep it identical to the GitHub repository name |
| Python version | `3.11` | ESP-IDF 5.5 needs Python >= 3.9; 3.11 and 3.12 both work. Downgrade explicitly if the image defaults to 3.13. |
| Environment | `IDF_PATH=/home/user/esp/esp-idf`<br>`IDF_TOOLS_PATH=/home/user/.espressif` | Paths must match `$HOME` in the image; change them if the image user is not `user`. |
| install command | `bash cloud/install.sh` | Idempotent; repeated runs do not clone twice |
| start command | `bash cloud/start.sh` | Sources `export.sh` and runs `idf.py set-target esp32c3` |
| Default terminal | `bash` | The scripts are bash; invoke them explicitly with `bash <script>` if the default shell is sh or zsh |

Optional environment variables:

- `IDF_VERSION`: defaults to `v5.5.3`; usually no need to change.
- `IDF_SKIP_SET_TARGET=1`: skips `set-target` in `start.sh` so an existing build directory can be reused.

## Script behavior

- `cloud/install.sh`: if `$IDF_PATH/export.sh` does not exist, shallow-clones ESP-IDF `v5.5.3` (with recursive submodules) into `$IDF_PATH`, then runs `./install.sh esp32c3` to install the toolchain.
- `cloud/start.sh`: sources `$IDF_PATH/export.sh`, switches to the repository root, runs `idf.py set-target esp32c3`, and finally prints `idf.py --version`.

Both scripts already carry the executable bit (`100755`) in the git index, so `./cloud/install.sh` works as well.

## Network allowlist risk

ESP-IDF installation and builds depend on the following external domains; if any
is unreachable, environment setup fails:

1. `github.com` / `codeload.github.com` / `objects.githubusercontent.com`: cloning ESP-IDF and its submodules.
2. `dl.espressif.com`: **the main risk**. Toolchains, `esptool`, `openocd`, and other binaries are downloaded from here by default; without allowlisting, `install.sh` fails during download.
3. `pypi.org` / `files.pythonhosted.org`: installing the Python dependencies listed in `requirements.txt`.

Mitigations:

- Prefer allowlisting `dl.espressif.com` and `github.com`. For mainland China mirrors, point `IDF_GITHUB_MIRROR` at an internal mirror.
- If `dl.espressif.com` cannot be allowlisted, pre-bake the toolchain into the image (offline package), or cache it during install after allowlisting Espressif's download CDN.
- To confirm this is the cause: `Failed to download`, `Could not resolve host: dl.espressif.com`, or a timeout in `install.sh` logs means a network/allowlist restriction, not a script bug.

## Unverified items

The cloud image has not been run end to end; `install.sh` and `start.sh` were only
syntax-checked (see the commit message). On the first TraeWork start, confirm:

- `idf.py --version` reports `v5.5.3`
- `idf.py set-target esp32c3` succeeds and creates `build/`
- `idf.py build` passes (it downloads `managed_components`, so network access is required)
